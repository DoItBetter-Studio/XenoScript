/*
 * lsp_main.c — XenoScript Language Server (xenolsp)
 *
 * Single-threaded JSON-RPC server over stdio.
 * Protocol: LSP 3.17 (subset)
 *
 * Supported methods:
 *   initialize / initialized / shutdown / exit
 *   textDocument/didOpen
 *   textDocument/didChange
 *   textDocument/didClose
 *   textDocument/hover
 *   textDocument/definition
 *   textDocument/references
 *   textDocument/completion
 *   $/cancelRequest   (no-op)
 */

#define _POSIX_C_SOURCE 200809L

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "jsonrpc.h"
#include "json.h"
#include "doc_store.h"
#include "stub_gen.h"
#include "xdoc.h"
#include "stdlib_xdoc.h"

/* Merged documentation from open buffers + loaded .xdoc side-cars */
static XdocArchive g_docs;


#include "../../includes/checker.h"
#include "../../includes/bytecode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── Global state ─────────────────────────────────────────────────────── */

static DocStore g_store;
static bool g_shutdown = false;

/* ── Response helpers ─────────────────────────────────────────────────── */

/* Send a success response with a JSON result value */
static void respond_result(long long id, const char *result_json)
{
	JsonBuf b;
	json_buf_init(&b);
	json_buf_raw(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
	json_buf_int(&b, id);
	json_buf_raw(&b, ",\"result\":");
	json_buf_raw(&b, result_json);
	json_buf_raw(&b, "}");
	char *msg = json_buf_take(&b);
	json_buf_free(&b);
	if (msg)
	{
		jsonrpc_write_str(msg);
		free(msg);
	}
}

/* Send a null result (for methods with no meaningful return) */
static void respond_null(long long id)
{
	respond_result(id, "null");
}

/* Send an error response */
static void respond_error(long long id, int code, const char *message)
{
	JsonBuf b;
	json_buf_init(&b);
	json_buf_rawf(&b, "{\"jsonrpc\":\"2.0\",\"id\":%lld,"
					  "\"error\":{\"code\":%d,\"message\":",
				  id, code);
	json_buf_str(&b, message);
	json_buf_raw(&b, "}}");
	char *msg = json_buf_take(&b);
	json_buf_free(&b);
	if (msg)
	{
		jsonrpc_write_str(msg);
		free(msg);
	}
}

/* Send a notification (no id) */
static void notify(const char *method, const char *params_json)
{
	JsonBuf b;
	json_buf_init(&b);
	json_buf_raw(&b, "{\"jsonrpc\":\"2.0\",\"method\":");
	json_buf_str(&b, method);
	json_buf_raw(&b, ",\"params\":");
	json_buf_raw(&b, params_json);
	json_buf_raw(&b, "}");
	char *msg = json_buf_take(&b);
	json_buf_free(&b);
	if (msg)
	{
		jsonrpc_write_str(msg);
		free(msg);
	}
}

/* ── Diagnostics publisher ────────────────────────────────────────────── */

static void publish_diagnostics(const char *uri,
								Diagnostic *diags, int count)
{
	JsonBuf b;
	json_buf_init(&b);
	json_buf_raw(&b, "{\"uri\":");
	json_buf_str(&b, uri);
	json_buf_raw(&b, ",\"diagnostics\":[");

	for (int i = 0; i < count; i++)
	{
		if (i > 0)
			json_buf_raw(&b, ",");
		/* severity: 1=Error, 2=Warning */
		int sev = diags[i].is_warning ? 2 : 1;

		int start_col = diags[i].col;
		int end_col = diags[i].end_col;

		/* No end from checker → underline a short span so it’s visible */
		if (end_col <= start_col)
			end_col = start_col + 1;

		/* Optional: if col is 0 (unknown), still show something on the line */
		if (start_col < 0)
			start_col = 0;
		if (end_col < start_col + 1)
			end_col = start_col + 1;

		json_buf_rawf(&b,
					  "{\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
					  "\"end\":{\"line\":%d,\"character\":%d}},"
					  "\"severity\":%d,\"message\":",
					  diags[i].line, start_col,
					  diags[i].line, end_col,
					  sev);
		json_buf_str(&b, diags[i].message);
		json_buf_raw(&b, ",\"source\":\"xenolsp\"}");
	}

	json_buf_raw(&b, "]}");
	char *params = json_buf_take(&b);
	json_buf_free(&b);
	if (params)
	{
		notify("textDocument/publishDiagnostics", params);
		free(params);
	}
}

/* ── initialize ──────────────────────────────────────────────────────── */

static void handle_initialize(long long id)
{
	/* Return server capabilities */
	const char *caps =
		"{"
		"  \"capabilities\": {"
		"    \"textDocumentSync\": {"
		"      \"openClose\": true,"
		"      \"change\": 1" /* Full sync */
		"    },"
		"    \"hoverProvider\": true,"
		"    \"definitionProvider\": true,"
		"    \"referencesProvider\": true,"
		"    \"completionProvider\": {"
		"      \"triggerCharacters\": [\".\"]"
		"    }"
		"  },"
		"  \"serverInfo\": {"
		"    \"name\": \"xenolsp\","
		"    \"version\": \"0.1.0\""
		"  }"
		"}";
	respond_result(id, caps);
}

/* ── textDocument/didOpen ─────────────────────────────────────────────── */


/* Load embedded stdlib .xdoc blobs (and optional disk fallback). */
static void load_stdlib_xdocs(void)
{
#if STDLIB_XDOC_COUNT > 0
	for (int i = 0; i < STDLIB_XDOC_COUNT; i++)
	{
		const StdlibXdocEntry *e = &STDLIB_XDOC_TABLE[i];
		if (!e->start || !e->end || e->end <= e->start)
			continue;
		XdocArchive tmp;
		memset(&tmp, 0, sizeof(tmp));
		if (xdoc_load_mem(&tmp, (const char *)e->start, (size_t)(e->end - e->start)))
		{
			xdoc_merge(&g_docs, &tmp);
			xdoc_free(&tmp);
		}
	}
#endif
	/* Disk fallback (dev tree / pack output next to cwd) */
	{
		static const char *mods[] = { "core", "math", "collections", NULL };
		static const char *dirs[] = { "build/xar", NULL };
		for (int d = 0; dirs[d]; d++)
		{
			for (int m = 0; mods[m]; m++)
			{
				char path[512];
				snprintf(path, sizeof(path), "%s/%s.xdoc", dirs[d], mods[m]);
				XdocArchive tmp;
				memset(&tmp, 0, sizeof(tmp));
				if (xdoc_load(&tmp, path))
				{
					xdoc_merge(&g_docs, &tmp);
					xdoc_free(&tmp);
				}
			}
		}
	}
}

static void refresh_live_docs(const char *text, const char *uri);

static void handle_did_open(const char *params)
{
	char *uri = json_get_str(params, "textDocument.uri");
	char *text = json_get_str(params, "textDocument.text");
	if (!uri || !text)
	{
		free(uri);
		free(text);
		return;
	}

	DocEntry *entry = doc_store_get(&g_store, uri);
	if (entry)
	{
		Diagnostic diags[DOC_MAX_DIAGNOSTICS];
		int diag_count = 0;
		doc_store_run(&g_store, entry, text, diags, &diag_count);
		refresh_live_docs(text, uri);
		publish_diagnostics(uri, diags, diag_count);
	}

	free(uri);
	free(text);
}

/* ── textDocument/didChange ───────────────────────────────────────────── */

static void handle_did_change(const char *params)
{
	char *uri = json_get_str(params, "textDocument.uri");
	char *text = json_get_str(params, "contentChanges.0.text");

	if (!uri || !text)
	{
		free(uri);
		free(text);
		return;
	}

	DocEntry *entry = doc_store_get(&g_store, uri);
	if (entry)
	{
		Diagnostic diags[DOC_MAX_DIAGNOSTICS];
		int diag_count = 0;
		doc_store_run(&g_store, entry, text, diags, &diag_count);
		refresh_live_docs(text, uri);
		publish_diagnostics(uri, diags, diag_count);
	}

	free(uri);
	free(text);
}

/* ── textDocument/didClose ───────────────────────────────────────────── */

static void handle_did_close(const char *params)
{
	char *uri = json_get_str(params, "textDocument.uri");
	if (!uri)
		return;
	doc_store_close(&g_store, uri);
	/* Publish empty diagnostics to clear squiggles */
	publish_diagnostics(uri, NULL, 0);
	free(uri);
}

/* ── Hover ────────────────────────────────────────────────────────────── */

/*
 * Format a Symbol into a markdown hover string.
 * Returns a heap-allocated string; caller frees.
 */
static char *format_hover(const Symbol *sym)
{
	if (!sym)
		return NULL;

	JsonBuf b;
	json_buf_init(&b);

	/* Build a short type annotation string */
	char type_str[128] = "unknown";
	switch (sym->kind)
	{
	case SYM_VAR:
	{
		/* Format type.kind as a keyword */
		const char *tk = "unknown";
		switch (sym->type.kind)
		{
		case TYPE_INT:
			tk = "int";
			break;
		case TYPE_FLOAT:
			tk = "float";
			break;
		case TYPE_BOOL:
			tk = "bool";
			break;
		case TYPE_STRING:
			tk = "string";
			break;
		case TYPE_VOID:
			tk = "void";
			break;
		case TYPE_OBJECT:
			tk = sym->type.class_name[0]
					 ? sym->type.class_name
					 : "object";
			break;
		case TYPE_ENUM:
			tk = sym->type.class_name[0]
					 ? sym->type.class_name
					 : "enum";
			break;
		case TYPE_ANY:
			tk = "any";
			break;
		default:
			tk = "object";
			break;
		}
		snprintf(type_str, sizeof(type_str), "%s%s",
				 tk, sym->type.is_nullable ? "?" : "");
		snprintf(type_str, sizeof(type_str), "%.*s: %s%s",
				 sym->length, sym->name, tk,
				 sym->type.is_nullable ? "?" : "");
		break;
	}
	case SYM_FN:
	{
		const char *rt = "void";
		switch (sym->type.kind)
		{
		case TYPE_INT:    rt = "int"; break;
		case TYPE_FLOAT:  rt = "float"; break;
		case TYPE_BOOL:   rt = "bool"; break;
		case TYPE_STRING: rt = "string"; break;
		case TYPE_VOID:   rt = "void"; break;
		case TYPE_OBJECT:
			rt = sym->type.class_name[0] ? sym->type.class_name : "object";
			break;
		default: rt = "object"; break;
		}
		/* No (function) prefix — keep signature native */
		snprintf(type_str, sizeof(type_str), "%.*s(): %s",
				 sym->length, sym->name, rt);
		break;
	}
	case SYM_CLASS:
		/* Prefer docs-only hover for types; type_str kept as bare name fallback */
		snprintf(type_str, sizeof(type_str), "%.*s", sym->length, sym->name);
		break;
	case SYM_ENUM:
		snprintf(type_str, sizeof(type_str), "%.*s", sym->length, sym->name);
		break;
	case SYM_INTERFACE:
		snprintf(type_str, sizeof(type_str), "%.*s", sym->length, sym->name);
		break;
	case SYM_EVENT:
		snprintf(type_str, sizeof(type_str), "%.*s", sym->length, sym->name);
		break;
	}

	/* Location annotation */
	char loc[256] = "";
	if (sym->def_file && sym->def_line > 0)
		snprintf(loc, sizeof(loc), "\n\n*Defined at %s:%d*",
				 sym->def_file, sym->def_line); /* already 1-based */

	/* Optional docs from .xdoc / open-buffer extract */
	char *doc_md = NULL;
	{
		char key[128];
		key[0] = '\0';
		if (sym->kind == SYM_CLASS || sym->kind == SYM_INTERFACE || sym->kind == SYM_ENUM)
			snprintf(key, sizeof(key), "%.*s", sym->length, sym->name);
		else if (sym->kind == SYM_FN)
		{
			/* Method symbols stash owner class in class_name_buf */
			if (sym->class_name_buf[0])
				snprintf(key, sizeof(key), "%s.%.*s",
						 sym->class_name_buf, sym->length, sym->name);
			else
				snprintf(key, sizeof(key), "%.*s", sym->length, sym->name);
		}
		/* Prefer Class.member when we have a class prefix in the name (rare);
		 * also try bare name. */
		const XdocEntry *de = NULL;
		if (key[0])
			de = xdoc_find(&g_docs, key);
		if (de)
			doc_md = xdoc_format_markdown(de);
	}

	/* Wrap in LSP Hover markdown format.
	 * With docs: show documentation only (no synthetic "(class) Name" line).
	 * Without docs: show a short signature fence as fallback. */
	json_buf_raw(&b, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
	char full[2048];
	if (doc_md && doc_md[0])
		snprintf(full, sizeof(full), "%s%s", doc_md, loc);
	else
		snprintf(full, sizeof(full), "```xenoscript\n%s\n```%s", type_str, loc);
	json_buf_str(&b, full);
	json_buf_raw(&b, "}}");
	free(doc_md);

	char *out = json_buf_take(&b);
	json_buf_free(&b);
	return out;
}


/* Rebuild g_docs from an open document's source (live #Docs blocks). */
static void refresh_live_docs(const char *text, const char *uri)
{
	XdocArchive live;
	memset(&live, 0, sizeof(live));
	xdoc_extract_from_source(text, uri ? uri : "<buffer>", &live);
	xdoc_merge(&g_docs, &live);
	xdoc_free(&live);

	/* Side-car docs for project dependencies (deps/<name>.xdoc). */
	doc_store_load_project_xdocs(uri, &g_docs);
}

static void handle_hover(long long id, const char *params)
{
	char *uri = json_get_str(params, "textDocument.uri");
	long long line = 0, col = 0;
	json_get_int(params, "position.line", &line);
	json_get_int(params, "position.character", &col);

	if (!uri)
	{
		respond_null(id);
		return;
	}

	DocEntry *entry = doc_store_get(&g_store, uri);
	free(uri);

	if (!entry || !entry->checker)
	{
		respond_null(id);
		return;
	}

	/* LSP positions are 0-based; checker also uses 0-based lines (after
	 * pipeline_prepare's blank-line preservation aligns them). Columns are
	 * 0-based in LSP, 1-based in checker — add 1 for col only. */
	const Symbol *sym = checker_find_symbol_at(entry->checker,
											   (int)line + 1, (int)col + 1);
	if (!sym)
	{
		respond_null(id);
		return;
	}

	char *hover_json = format_hover(sym);
	if (hover_json)
	{
		respond_result(id, hover_json);
		free(hover_json);
	}
	else
	{
		respond_null(id);
	}
}

/* ── Definition ───────────────────────────────────────────────────────── */

static void handle_definition(long long id, const char *params)
{
	char *uri = json_get_str(params, "textDocument.uri");
	long long line = 0, col = 0;
	json_get_int(params, "position.line", &line);
	json_get_int(params, "position.character", &col);

	if (!uri)
	{
		respond_null(id);
		return;
	}

	DocEntry *entry = doc_store_get(&g_store, uri);
	free(uri);

	if (!entry || !entry->checker)
	{
		respond_null(id);
		return;
	}

	const Symbol *sym = checker_find_definition(entry->checker,
												(int)line + 1, (int)col + 1);
	if (!sym)
	{
		respond_null(id);
		return;
	}

	JsonBuf b;
	json_buf_init(&b);

	const ClassDef *cdef = NULL;
	if ((sym->kind == SYM_CLASS || sym->kind == SYM_ENUM ||
		 sym->kind == SYM_INTERFACE) &&
		sym->class_def)
	{
		cdef = (const ClassDef *)sym->class_def;
	}

	if (cdef)
	{
		/* Came from a binary / stdlib ClassDef → always use stub */
		char stub_uri[256];
		snprintf(stub_uri, sizeof(stub_uri), "xeno-stub:///%s.xeno", cdef->name);

		json_buf_raw(&b, "{\"uri\":");
		json_buf_str(&b, stub_uri);
		json_buf_raw(&b,
					 ",\"range\":{\"start\":{\"line\":0,\"character\":0},"
					 "\"end\":{\"line\":0,\"character\":0}}}");
	}
	else if (sym->def_file && sym->def_line > 0)
	{
		/* Normal source file */
		char def_uri[DOC_URI_MAX];
		if (strncmp(sym->def_file, "file://", 7) == 0)
		{
			snprintf(def_uri, sizeof(def_uri), "%s", sym->def_file);
		}
		else
		{
			snprintf(def_uri, sizeof(def_uri), "file://%s", sym->def_file);
		}
		int def_line = sym->def_line > 0 ? sym->def_line - 1 : 0;
		int def_col = sym->def_col > 0 ? sym->def_col - 1 : 0;

		json_buf_raw(&b, "{\"uri\":");
		json_buf_str(&b, def_uri);
		json_buf_rawf(&b, ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
						  "\"end\":{\"line\":%d,\"character\":%d}}}",
					  def_line, def_col, def_line, def_col + sym->length);
	}
	else
	{
		json_buf_free(&b);
		respond_null(id);
		return;
	}

	char *result = json_buf_take(&b);
	json_buf_free(&b);
	if (result)
	{
		respond_result(id, result);
		free(result);
	}
	else
		respond_null(id);
}

/* ── References ───────────────────────────────────────────────────────── */

static void handle_references(long long id, const char *params)
{
	char *uri = json_get_str(params, "textDocument.uri");
	long long line = 0, col = 0;
	json_get_int(params, "position.line", &line);
	json_get_int(params, "position.character", &col);

	if (!uri)
	{
		respond_null(id);
		return;
	}

	DocEntry *entry = doc_store_get(&g_store, uri);

	if (!entry || !entry->checker)
	{
		free(uri);
		respond_null(id);
		return;
	}

	UsageRecord usages[512];
	int count = checker_usages_of(entry->checker,
								  (int)line + 1, (int)col + 1,
								  usages, 512);
	free(uri);

	JsonBuf b;
	json_buf_init(&b);
	json_buf_raw(&b, "[");

	for (int i = 0; i < count; i++)
	{
		if (i > 0)
			json_buf_raw(&b, ",");

		const char *ref_file = usages[i].file ? usages[i].file : "";
		char ref_uri[DOC_URI_MAX];
		if (strncmp(ref_file, "file://", 7) == 0)
		{
			snprintf(ref_uri, sizeof(ref_uri), "%s", ref_file);
		}
		else if (ref_file[0])
		{
			snprintf(ref_uri, sizeof(ref_uri), "file://%s", ref_file);
		}
		else
		{
			ref_uri[0] = '\0';
		}

		int ref_line = usages[i].line; /* already 0-based */
		int ref_col = usages[i].col > 0 ? usages[i].col - 1 : 0;
		int ref_end = ref_col + usages[i].length;

		json_buf_raw(&b, "{\"uri\":");
		json_buf_str(&b, ref_uri);
		json_buf_rawf(&b,
					  ",\"range\":{\"start\":{\"line\":%d,\"character\":%d},"
					  "\"end\":{\"line\":%d,\"character\":%d}}}",
					  ref_line, ref_col, ref_line, ref_end);
	}

	json_buf_raw(&b, "]");
	char *result = json_buf_take(&b);
	json_buf_free(&b);
	if (result)
	{
		respond_result(id, result);
		free(result);
	}
	else
		respond_null(id);
}

/* ── Go-To-Definition ─────────────────────────────────────────────────── */
static void handle_get_stub(long long id, const char *params)
{
	char *name = json_get_str(params, "name");
	if (!name)
	{
		respond_null(id);
		return;
	}

	/* Stub URIs carry a ".xeno" suffix (so the editor picks the XenoScript
	 * language by extension) — strip it if the client passed it through. */
	size_t nlen = strlen(name);
	if (nlen > 5 && strcmp(name + nlen - 5, ".xeno") == 0)
		name[nlen - 5] = '\0';

	/* Find the class: stdlib first, then the dependencies of any open document
	 * (each document's checker_staging holds its project's .xar classes).
	 * `cmod` must be the module the ClassDef was found in — the constructor
	 * index inside a ClassDef is relative to that module. */
	const ClassDef *cdef = NULL;
	const Module *cmod = NULL;
	if (g_store.global_staging)
	{
		int idx = module_find_class(g_store.global_staging, name);
		if (idx >= 0)
		{
			cdef = &g_store.global_staging->classes[idx];
			cmod = g_store.global_staging;
		}
	}
	for (int i = 0; !cdef && i < DOC_STORE_MAX; i++)
	{
		const DocEntry *e = &g_store.entries[i];
		if (!e->in_use || !e->checker_staging)
			continue;
		int idx = module_find_class(e->checker_staging, name);
		if (idx >= 0)
		{
			cdef = &e->checker_staging->classes[idx];
			cmod = e->checker_staging;
		}
	}

	if (!cdef)
	{
		free(name);
		respond_null(id);
		return;
	}

	char *stub = stub_gen_class(cdef, cmod, &g_docs);
	free(name);

	if (!stub)
	{
		respond_null(id);
		return;
	}

	JsonBuf b;
	json_buf_init(&b);
	json_buf_str(&b, stub); /* properly escaped JSON string */
	free(stub);

	char *result = json_buf_take(&b);
	json_buf_free(&b);

	if (result)
	{
		respond_result(id, result);
		free(result);
	}
	else
	{
		respond_null(id);
	}
}

/* ── Completion ───────────────────────────────────────────────────────── */

/* LSP CompletionItemKind */
#define CIK_METHOD    2
#define CIK_FUNCTION  3
#define CIK_FIELD     5
#define CIK_VARIABLE  6
#define CIK_CLASS     7
#define CIK_INTERFACE 8
#define CIK_KEYWORD  14
#define CIK_ENUM     13
#define CIK_ENUM_MEMBER 20

static bool prefix_match(const char *name, int nlen, const char *prefix, int plen)
{
	if (plen <= 0)
		return true;
	if (nlen < plen)
		return false;
	return memcmp(name, prefix, (size_t)plen) == 0;
}

static void completion_add(JsonBuf *b, int *count, int *first,
						   const char *label, int kind, const char *detail)
{
	if (*count >= 200)
		return;
	if (!*first)
		json_buf_raw(b, ",");
	*first = 0;
	json_buf_raw(b, "{\"label\":");
	json_buf_str(b, label);
	json_buf_rawf(b, ",\"kind\":%d", kind);
	if (detail && detail[0])
	{
		json_buf_raw(b, ",\"detail\":");
		json_buf_str(b, detail);
	}
	json_buf_raw(b, "}");
	(*count)++;
}

static void complete_from_class_def(JsonBuf *b, int *count, int *first,
									const ClassDef *def,
									const char *prefix, int plen)
{
	if (!def)
		return;
	for (int i = 0; i < def->field_count; i++)
	{
		const FieldDef *f = &def->fields[i];
		if (!prefix_match(f->name, (int)strlen(f->name), prefix, plen))
			continue;
		char detail[128];
		snprintf(detail, sizeof(detail), "%s%s",
				 f->is_static ? "static " : "",
				 type_kind_name((TypeKind)f->type_kind));
		completion_add(b, count, first, f->name, CIK_FIELD, detail);
	}
	for (int i = 0; i < def->method_count; i++)
	{
		const MethodDef *m = &def->methods[i];
		if (!prefix_match(m->name, (int)strlen(m->name), prefix, plen))
			continue;
		char detail[128];
		snprintf(detail, sizeof(detail), "%sfunction(): %s",
				 m->is_static ? "static " : "",
				 type_kind_name((TypeKind)m->return_type_kind));
		completion_add(b, count, first, m->name, CIK_METHOD, detail);
	}
}

static void complete_from_class_ast(JsonBuf *b, int *count, int *first,
									Stmt *cls,
									const char *prefix, int plen)
{
	if (!cls || cls->kind != STMT_CLASS_DECL)
		return;
	typedef struct ClassFieldNode CFNode;
	typedef struct ClassMethodNode CMNode;
	for (CFNode *f = cls->class_decl.fields; f; f = f->next)
	{
		if (!prefix_match(f->name, f->length, prefix, plen))
			continue;
		char label[64];
		int n = f->length < 63 ? f->length : 63;
		memcpy(label, f->name, (size_t)n);
		label[n] = '\0';
		completion_add(b, count, first, label, CIK_FIELD,
					   f->is_static ? "static field" : "field");
	}
	for (CMNode *m = cls->class_decl.methods; m; m = m->next)
	{
		if (!m->fn || m->is_constructor)
			continue;
		const char *nm = m->fn->fn_decl.name;
		int nlen = m->fn->fn_decl.length;
		if (!prefix_match(nm, nlen, prefix, plen))
			continue;
		char label[64];
		int n = nlen < 63 ? nlen : 63;
		memcpy(label, nm, (size_t)n);
		label[n] = '\0';
		completion_add(b, count, first, label, CIK_METHOD,
					   m->is_static ? "static method" : "method");
	}
}

static void complete_from_enum_ast(JsonBuf *b, int *count, int *first,
								   Stmt *en,
								   const char *prefix, int plen)
{
	if (!en || en->kind != STMT_ENUM_DECL)
		return;
	typedef struct EnumMemberNode EMNode;
	for (EMNode *m = en->enum_decl.members; m; m = m->next)
	{
		if (!prefix_match(m->name, m->length, prefix, plen))
			continue;
		char label[64];
		int n = m->length < 63 ? m->length : 63;
		memcpy(label, m->name, (size_t)n);
		label[n] = '\0';
		completion_add(b, count, first, label, CIK_ENUM_MEMBER, "enum member");
	}
}

/* Find identifier immediately before cursor; detect member access. */
static void completion_scan_prefix(const char *text, int line0, int col0,
								   char *prefix, int prefix_cap,
								   int *out_plen,
								   bool *out_member,
								   char *receiver, int recv_cap,
								   int *out_rlen)
{
	prefix[0] = '\0';
	receiver[0] = '\0';
	*out_plen = 0;
	*out_rlen = 0;
	*out_member = false;
	if (!text)
		return;

	/* Seek to start of line line0 (0-based) */
	const char *p = text;
	int ln = 0;
	while (*p && ln < line0)
	{
		if (*p == '\n')
			ln++;
		p++;
	}
	if (ln != line0)
		return;

	/* Cursor column (0-based), clamp to line length */
	int len = 0;
	while (p[len] && p[len] != '\n' && p[len] != '\r')
		len++;
	int col = col0;
	if (col > len)
		col = len;
	if (col < 0)
		col = 0;

	/* Walk back over identifier characters for prefix */
	int end = col;
	int start = end;
	while (start > 0)
	{
		char c = p[start - 1];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_')
			start--;
		else
			break;
	}
	int plen = end - start;
	if (plen >= prefix_cap)
		plen = prefix_cap - 1;
	if (plen > 0)
		memcpy(prefix, p + start, (size_t)plen);
	prefix[plen] = '\0';
	*out_plen = plen;

	/* Member access: optional whitespace then '.' then prefix */
	int dot = start - 1;
	while (dot >= 0 && (p[dot] == ' ' || p[dot] == '\t'))
		dot--;
	if (dot < 0 || p[dot] != '.')
		return;
	*out_member = true;

	/* Receiver identifier before '.' */
	int r_end = dot;
	while (r_end > 0 && (p[r_end - 1] == ' ' || p[r_end - 1] == '\t'))
		r_end--;
	int r_start = r_end;
	while (r_start > 0)
	{
		char c = p[r_start - 1];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			(c >= '0' && c <= '9') || c == '_')
			r_start--;
		else
			break;
	}
	int rlen = r_end - r_start;
	if (rlen >= recv_cap)
		rlen = recv_cap - 1;
	if (rlen > 0)
		memcpy(receiver, p + r_start, (size_t)rlen);
	receiver[rlen] = '\0';
	*out_rlen = rlen;
}

static Symbol *lookup_any(Checker *c, const char *name, int len)
{
	if (!c || !name || len <= 0)
		return NULL;
	for (int d = c->scope_depth; d >= 0; d--)
	{
		Scope *scope = &c->scopes[d];
		for (int i = 0; i < scope->count; i++)
		{
			Symbol *s = &scope->symbols[i];
			if (s->length == len && memcmp(s->name, name, (size_t)len) == 0)
				return s;
		}
	}
	return NULL;
}

static void handle_completion(long long id, const char *params)
{
	char *uri = json_get_str(params, "textDocument.uri");
	long long line = 0, col = 0;
	json_get_int(params, "position.line", &line);
	json_get_int(params, "position.character", &col);

	if (!uri)
	{
		respond_result(id, "[]");
		return;
	}

	DocEntry *entry = doc_store_get(&g_store, uri);
	free(uri);

	char prefix[64];
	char receiver[64];
	int plen = 0, rlen = 0;
	bool is_member = false;
	const char *text = entry ? entry->text : NULL;
	completion_scan_prefix(text, (int)line, (int)col,
						   prefix, sizeof(prefix), &plen,
						   &is_member,
						   receiver, sizeof(receiver), &rlen);

	JsonBuf b;
	json_buf_init(&b);
	json_buf_raw(&b, "[");
	int count = 0;
	int first = 1;

	Checker *c = entry ? entry->checker : NULL;

	if (is_member && c && rlen > 0)
	{
		Symbol *recv = lookup_any(c, receiver, rlen);
		if (recv)
		{
			if (recv->kind == SYM_CLASS)
			{
				if (recv->class_def)
					complete_from_class_def(&b, &count, &first,
											(const ClassDef *)recv->class_def,
											prefix, plen);
				if (recv->class_decl)
					complete_from_class_ast(&b, &count, &first,
											recv->class_decl, prefix, plen);
			}
			else if (recv->kind == SYM_ENUM)
			{
				complete_from_enum_ast(&b, &count, &first,
									   recv->enum_decl, prefix, plen);
			}
			else if (recv->kind == SYM_INTERFACE && recv->interface_decl)
			{
				typedef struct IfaceMethodNode IMNode;
				for (IMNode *m = recv->interface_decl->interface_decl.methods;
					 m; m = m->next)
				{
					if (!prefix_match(m->name, m->length, prefix, plen))
						continue;
					char label[64];
					int n = m->length < 63 ? m->length : 63;
					memcpy(label, m->name, (size_t)n);
					label[n] = '\0';
					completion_add(&b, &count, &first, label, CIK_METHOD,
								   "interface method");
				}
			}
			else if (recv->kind == SYM_VAR || recv->kind == SYM_FN)
			{
				/* Instance / type of variable — resolve class name */
				const char *cname = NULL;
				if (recv->type.kind == TYPE_OBJECT && recv->type.class_name &&
					recv->type.class_name[0])
					cname = recv->type.class_name;
				if (cname)
				{
					/* Strip generic suffix: List<int> → List */
					char base[64];
					int bi = 0;
					for (const char *q = cname; *q && *q != '<' && bi < 63; q++)
						base[bi++] = *q;
					base[bi] = '\0';
					Symbol *cls = lookup_any(c, base, bi);
					if (cls && cls->kind == SYM_CLASS)
					{
						if (cls->class_def)
							complete_from_class_def(&b, &count, &first,
													(const ClassDef *)cls->class_def,
													prefix, plen);
						if (cls->class_decl)
							complete_from_class_ast(&b, &count, &first,
													cls->class_decl, prefix, plen);
					}
				}
			}
		}
	}
	else
	{
		/* Keywords */
		static const char *keywords[] = {
			"class", "interface", "enum", "function", "return",
			"if", "else", "for", "foreach", "while", "break", "continue",
			"match", "case", "try", "catch", "finally", "throw",
			"new", "this", "true", "false", "null",
			"public", "private", "protected", "static", "final",
			"virtual", "override", "import", "as", "is", "typeof",
			"int", "float", "double", "bool", "string", "void", "char",
			"byte", "sbyte", "short", "ushort", "uint", "long", "ulong",
			NULL
		};
		for (int i = 0; keywords[i]; i++)
		{
			if (!prefix_match(keywords[i], (int)strlen(keywords[i]), prefix, plen))
				continue;
			completion_add(&b, &count, &first, keywords[i], CIK_KEYWORD, "keyword");
		}

		/* Top-level (and any remaining) symbols from checker scopes */
		if (c)
		{
			for (int d = 0; d <= c->scope_depth; d++)
			{
				Scope *scope = &c->scopes[d];
				for (int i = 0; i < scope->count; i++)
				{
					Symbol *s = &scope->symbols[i];
					if (!prefix_match(s->name, s->length, prefix, plen))
						continue;
					char label[64];
					int n = s->length < 63 ? s->length : 63;
					memcpy(label, s->name, (size_t)n);
					label[n] = '\0';
					int kind = CIK_VARIABLE;
					const char *detail = NULL;
					switch (s->kind)
					{
					case SYM_FN:        kind = CIK_FUNCTION;  detail = "function"; break;
					case SYM_CLASS:     kind = CIK_CLASS;     detail = "class"; break;
					case SYM_ENUM:      kind = CIK_ENUM;      detail = "enum"; break;
					case SYM_INTERFACE: kind = CIK_INTERFACE; detail = "interface"; break;
					case SYM_EVENT:     kind = CIK_FUNCTION;  detail = "event"; break;
					case SYM_VAR:       kind = CIK_VARIABLE;  detail = "variable"; break;
					}
					completion_add(&b, &count, &first, label, kind, detail);
				}
			}
		}
	}

	json_buf_raw(&b, "]");
	char *result = json_buf_take(&b);
	json_buf_free(&b);
	if (result)
	{
		respond_result(id, result);
		free(result);
	}
	else
	{
		respond_result(id, "[]");
	}
}

/* ── Main dispatch loop ───────────────────────────────────────────────── */

static void dispatch(const char *json)
{
	char *method = json_get_str(json, "method");
	long long id = -1;
	json_get_int(json, "id", &id);

	char *params_raw = json_get_raw(json, "params");
	const char *params = params_raw ? params_raw : "{}";

	if (!method)
	{
		free(params_raw);
		return;
	}

	/* ── Lifecycle ── */
	if (strcmp(method, "initialize") == 0)
	{
		handle_initialize(id);
	}
	else if (strcmp(method, "initialized") == 0)
	{
		/* No-op notification */
	}
	else if (strcmp(method, "shutdown") == 0)
	{
		g_shutdown = true;
		respond_null(id);
	}
	else if (strcmp(method, "exit") == 0)
	{
		/* exit without prior shutdown is an error per spec */
		exit(g_shutdown ? 0 : 1);

		/* ── Document sync ── */
	}
	else if (strcmp(method, "textDocument/didOpen") == 0)
	{
		handle_did_open(params);
	}
	else if (strcmp(method, "textDocument/didChange") == 0)
	{
		handle_did_change(params);
	}
	else if (strcmp(method, "textDocument/didClose") == 0)
	{
		handle_did_close(params);

		/* ── LSP features ── */
	}
	else if (strcmp(method, "textDocument/hover") == 0)
	{
		handle_hover(id, params);
	}
	else if (strcmp(method, "textDocument/definition") == 0)
	{
		handle_definition(id, params);
	}
	else if (strcmp(method, "textDocument/references") == 0)
	{
		handle_references(id, params);
	}
	else if (strcmp(method, "textDocument/completion") == 0)
	{
		handle_completion(id, params);

		/* ── Cancellation / unknown ── */
	}
	else if (strcmp(method, "$/cancelRequest") == 0)
	{
		/* No-op — single-threaded, requests complete immediately */
	}
	else if (strcmp(method, "xeno/getStub") == 0)
	{
		handle_get_stub(id, params);
	}
	else
	{
		/* Unknown method with an id → MethodNotFound */
		if (id >= 0)
			respond_error(id, -32601, "Method not found");
	}

	free(method);
	free(params_raw);
}

/* ── Entry point ──────────────────────────────────────────────────────── */

int main(void)
{
	/* Set stdio to binary mode so \r\n is not mangled on Windows */
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	/* Disable buffering on stdout so responses go out immediately */
	setvbuf(stdout, NULL, _IONBF, 0);

	doc_store_init(&g_store);
	memset(&g_docs, 0, sizeof(g_docs));
	load_stdlib_xdocs();

	/* Main read loop */
	for (;;)
	{
		char *msg = jsonrpc_read();
		if (!msg)
			break; /* EOF */
		dispatch(msg);
		free(msg);
	}

	doc_store_free(&g_store);
	xdoc_free(&g_docs);
	return 0;
}