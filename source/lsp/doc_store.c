/*
 * doc_store.c — Open document management for xenolsp
 */

#define _POSIX_C_SOURCE 200809L

/* Include the pipeline header (defines static helpers) */
#include "../compiler/compile_pipeline.h"

#include "doc_store.h"
#include "xdoc.h"
#include "toml.h" /* xeno.project parsing — adjust path to wherever toml.h lives */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Name of the project marker file. Its presence marks a project root. */
#define XENO_PROJECT_FILE "xeno.project"

/* Convert a file:// URI to a real, native filesystem path:
 *   - percent-decodes  (file:///d%3A/My%20Mod/a.xeno)
 *   - strips the leading '/' before a Windows drive letter ("/d:/x" -> "d:/x")
 * Used ONLY for touching the filesystem. Symbol def_file paths keep using
 * uri_to_path() so they round-trip back into URIs unchanged. */
static void uri_to_fs_path(const char *uri, char *out, size_t sz)
{
	const char *p = uri;
	if (strncmp(p, "file://", 7) == 0)
		p += 7;
	size_t n = 0;
	while (*p && n + 1 < sz)
	{
		if (p[0] == '%' && isxdigit((unsigned char)p[1]) &&
			isxdigit((unsigned char)p[2]))
		{
			char hex[3] = {p[1], p[2], '\0'};
			out[n++] = (char)strtol(hex, NULL, 16);
			p += 3;
		}
		else
		{
			out[n++] = *p++;
		}
	}
	out[n] = '\0';
	if (out[0] == '/' && isalpha((unsigned char)out[1]) && out[2] == ':')
		memmove(out, out + 1, strlen(out));
}

/* Walk up from `fs_path` looking for XENO_PROJECT_FILE. On success writes the
 * project root directory into root_out and returns true. */
static bool find_project_root(const char *fs_path, char *root_out, size_t sz)
{
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", fs_path);
	for (;;)
	{
		char *sl = strrchr(dir, '/');
#ifdef _WIN32
		char *bs = strrchr(dir, '\\');
		if (bs && (!sl || bs > sl))
			sl = bs;
#endif
		if (!sl || sl == dir)
			return false;
		*sl = '\0'; /* drop last path component */

		char probe[1100];
		snprintf(probe, sizeof(probe), "%s/" XENO_PROJECT_FILE, dir);
		FILE *f = fopen(probe, "rb");
		if (f)
		{
			fclose(f);
			snprintf(root_out, sz, "%s", dir);
			return true;
		}
	}
}

/* Read <root>/xeno.project and list the [dependencies] whose
 * <deps_dir>/<name>.xar file does not exist, as "a.xar, b.xar".
 * Returns how many are missing (out is empty when 0 or on parse trouble). */
static int missing_declared_deps(const char *root, const char *deps_dir,
								 char *out, size_t sz)
{
	out[0] = '\0';
	char path[1100];
	snprintf(path, sizeof(path), "%s/" XENO_PROJECT_FILE, root);
	char *text = pipeline_read_file(path);
	if (!text)
		return 0;

	TomlDoc *doc = malloc(sizeof(TomlDoc)); /* fixed-size tables — keep off the stack */
	if (!doc)
	{
		free(text);
		return 0;
	}

	int missing = 0;
	if (toml_parse(doc, text))
	{
		for (int i = 0; i < doc->count; i++)
		{
			if (strcmp(doc->entries[i].section, "dependencies") != 0)
				continue;
			char xar[1200];
			snprintf(xar, sizeof(xar), "%s/%s.xar", deps_dir, doc->entries[i].key);
			FILE *f = fopen(xar, "rb");
			if (f)
			{
				fclose(f);
				continue;
			}
			size_t n = strlen(out);
			snprintf(out + n, sz - n, "%s%s.xar", missing ? ", " : "", doc->entries[i].key);
			missing++;
		}
	}
	toml_free(doc);
	free(doc);
	free(text);
	return missing;
}

/* Locate the import of `name` (`<name>` or `"name"`).
 * Returns 0-based line. On success also fills 0-based [col, end_col) covering
 * the module specifier (including <> or quotes). Falls back to underlining
 * the `import` keyword if the name isn't on the line. */
static int find_import_range(const char *text, const char *name,
							 int *out_col, int *out_end_col)
{
	if (out_col)
		*out_col = 0;
	if (out_end_col)
		*out_end_col = 6; /* "import" */

	if (!text || !name || !*name)
		return 0;

	/* Prefer basename if a full path was recorded */
	const char *base = name;
	{
		const char *sl = strrchr(name, '/');
#ifdef _WIN32
		const char *bs = strrchr(name, '\\');
		if (bs && (!sl || bs > sl))
			sl = bs;
#endif
		if (sl && sl[1])
			base = sl + 1;
	}

	char needle_a[520], needle_b[520];
	snprintf(needle_a, sizeof(needle_a), "<%s>", base);
	snprintf(needle_b, sizeof(needle_b), "\"%s\"", base);

	int line = 0;
	const char *p = text;
	while (*p)
	{
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);

		char buf[1024];
		size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
		memcpy(buf, p, n);
		buf[n] = '\0';

		const char *q = buf;
		while (*q == ' ' || *q == '\t')
			q++;
		if (strncmp(q, "import", 6) == 0)
		{
			const char *hit = strstr(buf, needle_a);
			size_t hit_len = strlen(needle_a);
			if (!hit)
			{
				hit = strstr(buf, needle_b);
				hit_len = strlen(needle_b);
			}
			if (hit)
			{
				if (out_col)
					*out_col = (int)(hit - buf);
				if (out_end_col)
					*out_end_col = (int)(hit - buf) + (int)hit_len;
				return line;
			}
			if (out_col)
				*out_col = (int)(q - buf);
			if (out_end_col)
				*out_end_col = (int)(q - buf) + 6;
			return line;
		}

		if (!eol)
			break;
		p = eol + 1;
		line++;
	}
	return 0;
}

static void free_checker_state(DocEntry *e)
{
	if (e->checker)
	{
		/* source_file is strdup'd in doc_store_run — free it here */
		if (e->checker->source_file)
			free((char *)e->checker->source_file);
		free(e->checker);
		e->checker = NULL;
	}
	if (e->arena)
	{
		arena_free(e->arena);
		free(e->arena);
		e->arena = NULL;
	}
	if (e->program)
	{
		free(e->program);
		e->program = NULL;
	}
	if (e->merged_source)
	{
		free(e->merged_source);
		e->merged_source = NULL;
	}
	if (e->checker_staging)
	{
		/* Must not be freed before `checker` — its class symbols hold raw
		 * pointers into this module's classes[] array. */
		module_free(e->checker_staging);
		free(e->checker_staging);
		e->checker_staging = NULL;
	}
}

/* ── Public API ──────────────────────────────────────────────────────── */

void doc_store_init(DocStore *store)
{
	memset(store, 0, sizeof(*store));

	/* Load global staging (stdlib) once */
	store->global_staging = calloc(1, sizeof(Module));
	module_init(store->global_staging);

	PipelineState ps;
	pipeline_state_init(&ps);
	pipeline_load_sys_module(&ps, "core", store->global_staging);
	pipeline_load_sys_module(&ps, "math", store->global_staging);
	pipeline_load_sys_module(&ps, "collections", store->global_staging);
}

void doc_store_free(DocStore *store)
{
	for (int i = 0; i < DOC_STORE_MAX; i++)
	{
		DocEntry *e = &store->entries[i];
		if (!e->in_use)
			continue;
		free(e->text);
		free_checker_state(e);
		if (e->staging)
		{
			module_free(e->staging);
			free(e->staging);
		}
	}
	if (store->global_staging)
	{
		module_free(store->global_staging);
		free(store->global_staging);
	}
	memset(store, 0, sizeof(*store));
}

DocEntry *doc_store_get(DocStore *store, const char *uri)
{
	/* Find existing */
	for (int i = 0; i < DOC_STORE_MAX; i++)
	{
		if (store->entries[i].in_use &&
			strcmp(store->entries[i].uri, uri) == 0)
			return &store->entries[i];
	}
	/* Allocate new slot */
	for (int i = 0; i < DOC_STORE_MAX; i++)
	{
		DocEntry *e = &store->entries[i];
		if (!e->in_use)
		{
			memset(e, 0, sizeof(*e));
			e->in_use = true;
			strncpy(e->uri, uri, DOC_URI_MAX - 1);

			/* Give this doc its own staging module (copy of global) */
			e->staging = calloc(1, sizeof(Module));
			module_init(e->staging);
			module_merge(e->staging, store->global_staging);

			store->count++;
			return e;
		}
	}
	return NULL; /* store full */
}

void doc_store_close(DocStore *store, const char *uri)
{
	for (int i = 0; i < DOC_STORE_MAX; i++)
	{
		DocEntry *e = &store->entries[i];
		if (!e->in_use || strcmp(e->uri, uri) != 0)
			continue;
		free(e->text);
		free_checker_state(e);
		if (e->staging)
		{
			module_free(e->staging);
			free(e->staging);
		}
		memset(e, 0, sizeof(*e));
		store->count--;
		return;
	}
}

bool doc_store_run(DocStore *store, DocEntry *entry,
				   const char *text,
				   Diagnostic *diags_out, int *diag_count_out)
{
	(void)store;

	*diag_count_out = 0;

	/* Update stored text */
	free(entry->text);
	entry->text = strdup(text);

	char fs_path[1024];
	uri_to_fs_path(entry->uri, fs_path, sizeof(fs_path));

	/* Real filesystem path + project dependency dir (if inside a project) */
	char project_root[1024] = "";
	bool in_project = find_project_root(fs_path, project_root, sizeof(project_root));

	PipelineState ps;
	pipeline_state_init(&ps);
	pipeline_clear_error();

	if (in_project)
	{
		/* Reads xeno.project → fills ps.deps_dir and ps.deps[] */
		pipeline_load_project(&ps, project_root);
	}

	/* ── 1. Prepare ── */
	Module *staging = calloc(1, sizeof(Module));
	module_init(staging);
	module_merge(staging, entry->staging); /* global stdlib copy */

	bool import_err = false;

	/* New entry point that takes the full PipelineState */
	char *merged = pipeline_prepare_with_state(text, fs_path, &ps, staging, &import_err);

	if (import_err || !merged)
	{
		/* Emit a single diagnostic for import failure, on the failing import's
		 * line when we can find it, using the pipeline's own reason if it gave one. */
		if (diags_out && *diag_count_out < DOC_MAX_DIAGNOSTICS)
		{
			Diagnostic *d = &diags_out[(*diag_count_out)++];
			d->line = find_import_range(text, pipeline_last_error_import,
										&d->col, &d->end_col);
			d->is_warning = false;

			char missing[200];
			if (pipeline_last_error[0])
			{
				snprintf(d->message, sizeof(d->message), "%s%s", pipeline_last_error,
						 in_project ? ""
									: " (no " XENO_PROJECT_FILE " found above this file, "
									  "so only the standard library is available)");
			}
			else if (in_project &&
					 missing_declared_deps(project_root, ps.deps_dir, missing, sizeof(missing)) > 0)
			{
				snprintf(d->message, sizeof(d->message),
						 "Failed to resolve imports: declared dependencies missing from %s: %s",
						 ps.deps_dir, missing);
			}
			else
			{
				snprintf(d->message, sizeof(d->message), "Failed to resolve imports");
			}
		}
		free(merged);
		module_free(staging);
		free(staging);
		entry->checker_ok = false;
		return false;
	}

	/* ── 2. Lex + Parse ── */
	Lexer *lexer = malloc(sizeof(Lexer));
	Parser *parser = malloc(sizeof(Parser));
	lexer_init(lexer, merged);
	parser_init(parser, lexer);

	Program *program = malloc(sizeof(Program));
	*program = parser_parse(parser);

	if (parser->had_error)
	{
		/* Collect real parse errors with line/column ranges (1-based → 0-based). */
		for (int i = 0; i < parser->error_count && diags_out &&
			 *diag_count_out < DOC_MAX_DIAGNOSTICS; i++)
		{
			ParseError *pe = &parser->errors[i];
			Diagnostic *d = &diags_out[(*diag_count_out)++];
			d->line = pe->line > 0 ? pe->line - 1 : 0;
			d->col = pe->col > 0 ? pe->col - 1 : 0;
			d->end_col = pe->end_col > 0 ? pe->end_col - 1 : 0;
			if (d->end_col <= d->col)
				d->end_col = d->col + 1;
			d->is_warning = false;
			strncpy(d->message, pe->message, sizeof(d->message) - 1);
			d->message[sizeof(d->message) - 1] = '\0';
		}
		if (*diag_count_out == 0 && diags_out)
		{
			Diagnostic *d = &diags_out[(*diag_count_out)++];
			d->line = 0;
			d->col = 0;
			d->end_col = 1;
			d->is_warning = false;
			snprintf(d->message, sizeof(d->message), "Parse error");
		}
		/* Don't update checker state — keep last-good */
		free(merged);
		free(program);
		arena_free(&parser->arena);
		free(parser);
		free(lexer);
		module_free(staging);
		free(staging);
		entry->checker_ok = false;
		return false;
	}

	/* ── 3. Type-check ── */
	Checker *checker = malloc(sizeof(Checker));
	Arena *arena = malloc(sizeof(Arena));
	*arena = parser->arena; /* take ownership of parser's arena */

	checker_init(checker, arena);
	checker->source_file = strdup(fs_path); /* LSP needs this */

	/* Declare stdlib host functions */
	Type void_t = type_void(), any_t = type_any();
	checker_declare_host(checker, "print", void_t, &any_t, 1);

	CompilerHostTable host_table;
	compiler_host_table_init(&host_table);
	compiler_host_table_add_any(&host_table, "print", 0, 1);
	stdlib_declare_host_fns(checker, &host_table);
	pipeline_declare_staging(checker, staging);

	bool check_ok = checker_check(checker, program);

	/* Collect checker diagnostics */
	int max_diags = DOC_MAX_DIAGNOSTICS;
	for (int i = 0; i < checker->error_count && *diag_count_out < max_diags; i++)
	{
		Diagnostic *d = &diags_out[(*diag_count_out)++];
		/* checker errors are 1-based lines — convert to 0-based for LSP */
		d->line = checker->errors[i].line > 0 ? checker->errors[i].line - 1 : 0;
		d->col = checker->errors[i].col > 0 ? checker->errors[i].col - 1 : 0;
		d->end_col = checker->errors[i].end_col > 0 ? checker->errors[i].end_col - 1 : 0;
		d->is_warning = checker->errors[i].is_warning;
		strncpy(d->message, checker->errors[i].message, sizeof(d->message) - 1);
		d->message[sizeof(d->message) - 1] = '\0';
	}

	if (check_ok)
	{
		/* Replace stored good state */
		free_checker_state(entry); /* also frees the PREVIOUS checker_staging */
		entry->checker = checker;
		entry->program = program;
		entry->arena = arena;
		entry->merged_source = merged;
		entry->checker_staging = staging; /* transfer ownership — checker's class
										   * symbols point into it; must outlive
										   * checker, so do NOT free it below */
		entry->checker_ok = true;
	}
	else
	{
		/* Keep previous good state; discard this run's data */
		free(merged);
		if (checker->source_file)
			free((char *)checker->source_file);
		free(checker);
		arena_free(arena);
		free(arena);
		free(program);
		entry->checker_ok = false;
		/* `checker` (and its references into `staging`) was just discarded,
		 * so nothing keeps `staging` alive — safe to free here. */
		module_free(staging);
		free(staging);
	}

	free(parser);
	free(lexer);

	return check_ok;
}

void doc_store_load_project_xdocs(const char *uri, XdocArchive *out)
{
	if (!uri || !out)
		return;

	char fs_path[1024];
	uri_to_fs_path(uri, fs_path, sizeof(fs_path));

	char project_root[1024] = "";
	if (!find_project_root(fs_path, project_root, sizeof(project_root)))
		return;

	PipelineState ps;
	pipeline_state_init(&ps);
	if (!pipeline_load_project(&ps, project_root))
		return;
	if (!ps.deps_dir[0])
		return;

	/* Declared deps first */
	for (int i = 0; i < ps.dep_count; i++)
	{
		char path[1100];
		snprintf(path, sizeof(path), "%s/%s.xdoc", ps.deps_dir, ps.deps[i].name);
		XdocArchive tmp;
		memset(&tmp, 0, sizeof(tmp));
		if (xdoc_load(&tmp, path))
		{
			xdoc_merge(out, &tmp);
			xdoc_free(&tmp);
		}
	}
}