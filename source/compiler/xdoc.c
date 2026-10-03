/*
 * xdoc.c — XenoScript Documentation Archive implementation
 */

#include "xdoc.h"
#include "strutil.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Archive helpers ─────────────────────────────────────────────────── */

static bool xdoc_grow(XdocArchive *ar)
{
	int nc = ar->capacity ? ar->capacity * 2 : 32;
	XdocEntry *n = realloc(ar->entries, (size_t)nc * sizeof(XdocEntry));
	if (!n)
		return false;
	ar->entries = n;
	ar->capacity = nc;
	return true;
}

static XdocEntry *xdoc_push(XdocArchive *ar)
{
	if (ar->count >= ar->capacity && !xdoc_grow(ar))
		return NULL;
	XdocEntry *e = &ar->entries[ar->count++];
	memset(e, 0, sizeof(*e));
	return e;
}

void xdoc_free(XdocArchive *ar)
{
	free(ar->entries);
	ar->entries = NULL;
	ar->count = ar->capacity = 0;
}

const XdocEntry *xdoc_find(const XdocArchive *ar, const char *symbol)
{
	if (!ar || !symbol)
		return NULL;
	for (int i = 0; i < ar->count; i++)
	{
		if (strcmp(ar->entries[i].symbol, symbol) == 0)
			return &ar->entries[i];
	}
	return NULL;
}

/* ── Tag parsing ─────────────────────────────────────────────────────── */

static void trim_copy(char *dst, size_t dstsz, const char *src, size_t len)
{
	while (len > 0 && isspace((unsigned char)*src))
	{
		src++;
		len--;
	}
	while (len > 0 && isspace((unsigned char)src[len - 1]))
		len--;
	/* Strip surrounding quotes */
	if (len >= 2 && src[0] == '"' && src[len - 1] == '"')
	{
		src++;
		len -= 2;
	}
	if (len >= dstsz)
		len = dstsz - 1;
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static void apply_tag(XdocEntry *e, const char *tag, const char *rest, size_t rest_len)
{
	char buf[XDOC_TEXT_MAX];
	trim_copy(buf, sizeof(buf), rest, rest_len);

	if (strcmp(tag, "title") == 0)
		xeno_copy_str(e->title, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "summary") == 0)
		xeno_copy_str(e->summary, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "description") == 0)
		xeno_copy_str(e->description, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "returns") == 0)
		xeno_copy_str(e->returns_doc, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "example") == 0)
		xeno_copy_str(e->example, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "see") == 0)
		xeno_copy_str(e->see, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "link") == 0)
		xeno_copy_str(e->link, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "since") == 0)
		xeno_copy_str(e->since, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "deprecated") == 0)
		xeno_copy_str(e->deprecated, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "note") == 0)
		xeno_copy_str(e->note, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "warning") == 0)
		xeno_copy_str(e->warning, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "important") == 0)
		xeno_copy_str(e->important, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "remarks") == 0)
		xeno_copy_str(e->remarks, XDOC_TEXT_MAX, buf);
	else if (strcmp(tag, "param") == 0 && e->param_count < XDOC_MAX_PARAMS)
	{
		/* $name rest...  or  name rest... */
		const char *p = rest;
		size_t plen = rest_len;
		while (plen > 0 && isspace((unsigned char)*p))
		{
			p++;
			plen--;
		}
		if (plen > 0 && *p == '$')
		{
			p++;
			plen--;
		}
		char name[XDOC_NAME_MAX];
		size_t ni = 0;
		while (plen > 0 && (isalnum((unsigned char)*p) || *p == '_') && ni + 1 < sizeof(name))
		{
			name[ni++] = *p++;
			plen--;
		}
		name[ni] = '\0';
		while (plen > 0 && isspace((unsigned char)*p))
		{
			p++;
			plen--;
		}
		XdocParam *pm = &e->params[e->param_count++];
		xeno_copy_str(pm->name, XDOC_NAME_MAX, name);
		trim_copy(pm->doc, sizeof(pm->doc), p, plen);
	}
	else if (strcmp(tag, "throws") == 0 && e->throw_count < XDOC_MAX_THROWS)
	{
		const char *p = rest;
		size_t plen = rest_len;
		while (plen > 0 && isspace((unsigned char)*p))
		{
			p++;
			plen--;
		}
		if (plen > 0 && *p == '$')
		{
			p++;
			plen--;
		}
		char tname[XDOC_NAME_MAX];
		size_t ni = 0;
		while (plen > 0 && (isalnum((unsigned char)*p) || *p == '_') && ni + 1 < sizeof(tname))
		{
			tname[ni++] = *p++;
			plen--;
		}
		tname[ni] = '\0';
		while (plen > 0 && isspace((unsigned char)*p))
		{
			p++;
			plen--;
		}
		XdocThrow *th = &e->throws[e->throw_count++];
		xeno_copy_str(th->type_name, XDOC_NAME_MAX, tname);
		trim_copy(th->doc, sizeof(th->doc), p, plen);
	}
}

/* Parse body lines between #Docs and #!Docs (may include leading " * "). */
static void parse_docs_body(XdocEntry *e, const char *body, size_t body_len)
{
	const char *p = body;
	const char *end = body + body_len;
	while (p < end)
	{
		const char *line = p;
		while (p < end && *p != '\n')
			p++;
		size_t llen = (size_t)(p - line);
		if (p < end && *p == '\n')
			p++;

		/* Strip leading spaces, optional '*', more spaces */
		const char *q = line;
		const char *qend = line + llen;
		while (q < qend && isspace((unsigned char)*q))
			q++;
		if (q < qend && *q == '*')
		{
			q++;
			while (q < qend && isspace((unsigned char)*q))
				q++;
		}
		if (q >= qend || *q != '#')
			continue;
		q++; /* skip '#' */
		while (q < qend && isspace((unsigned char)*q))
			q++;

		/* tag name */
		char tag[32];
		size_t ti = 0;
		while (q < qend && (isalnum((unsigned char)*q) || *q == '_') && ti + 1 < sizeof(tag))
			tag[ti++] = *q++;
		tag[ti] = '\0';
		if (tag[0] == '\0')
			continue;
		while (q < qend && (*q == ':' || isspace((unsigned char)*q)))
			q++;
		apply_tag(e, tag, q, (size_t)(qend - q));
	}
}

/* Skip whitespace and find next declaration name after docs block.
 * Returns kind: 'c' class, 'f' function, 'v' field/other ident, 0 if none.
 * Writes name into out_name. Updates *class_ctx if class/interface/enum seen. */
static int find_next_decl(const char *src, size_t start, size_t len,
						  char *out_name, size_t out_sz,
						  char *class_ctx, size_t class_sz)
{
	const char *p = src + start;
	const char *end = src + len;
	out_name[0] = '\0';

	while (p < end)
	{
		while (p < end && isspace((unsigned char)*p))
			p++;
		if (p >= end)
			break;

		/* Skip line comments */
		if (p + 1 < end && p[0] == '/' && p[1] == '/')
		{
			while (p < end && *p != '\n')
				p++;
			continue;
		}
		/* Skip block comments */
		if (p + 1 < end && p[0] == '/' && p[1] == '*')
		{
			p += 2;
			while (p + 1 < end && !(p[0] == '*' && p[1] == '/'))
				p++;
			if (p + 1 < end)
				p += 2;
			continue;
		}

		/* Keywords that introduce declarations */
		if (strncmp(p, "class", 5) == 0 && !isalnum((unsigned char)p[5]) && p[5] != '_')
		{
			p += 5;
			while (p < end && isspace((unsigned char)*p))
				p++;
			size_t ni = 0;
			while (p < end && (isalnum((unsigned char)*p) || *p == '_') && ni + 1 < out_sz)
				out_name[ni++] = *p++;
			out_name[ni] = '\0';
			if (ni > 0)
			{
				xeno_copy_str(class_ctx, class_sz, out_name);
				class_ctx[class_sz - 1] = '\0';
				return 'c';
			}
			return 0;
		}
		if (strncmp(p, "interface", 9) == 0 && !isalnum((unsigned char)p[9]))
		{
			p += 9;
			while (p < end && isspace((unsigned char)*p))
				p++;
			size_t ni = 0;
			while (p < end && (isalnum((unsigned char)*p) || *p == '_') && ni + 1 < out_sz)
				out_name[ni++] = *p++;
			out_name[ni] = '\0';
			if (ni > 0)
			{
				xeno_copy_str(class_ctx, class_sz, out_name);
				class_ctx[class_sz - 1] = '\0';
				return 'c';
			}
			return 0;
		}
		if (strncmp(p, "enum", 4) == 0 && !isalnum((unsigned char)p[4]))
		{
			p += 4;
			while (p < end && isspace((unsigned char)*p))
				p++;
			size_t ni = 0;
			while (p < end && (isalnum((unsigned char)*p) || *p == '_') && ni + 1 < out_sz)
				out_name[ni++] = *p++;
			out_name[ni] = '\0';
			if (ni > 0)
			{
				xeno_copy_str(class_ctx, class_sz, out_name);
				class_ctx[class_sz - 1] = '\0';
				return 'c';
			}
			return 0;
		}
		if (strncmp(p, "function", 8) == 0 && !isalnum((unsigned char)p[8]))
		{
			p += 8;
			while (p < end && isspace((unsigned char)*p))
				p++;
			size_t ni = 0;
			while (p < end && (isalnum((unsigned char)*p) || *p == '_') && ni + 1 < out_sz)
				out_name[ni++] = *p++;
			out_name[ni] = '\0';
			return ni > 0 ? 'f' : 0;
		}
		/* Constructor: ClassName( */
		if (class_ctx[0] && isalpha((unsigned char)*p))
		{
			const char *save = p;
			size_t ni = 0;
			char tmp[XDOC_NAME_MAX];
			while (p < end && (isalnum((unsigned char)*p) || *p == '_') && ni + 1 < sizeof(tmp))
				tmp[ni++] = *p++;
			tmp[ni] = '\0';
			while (p < end && isspace((unsigned char)*p))
				p++;
			if (p < end && *p == '(' && strcmp(tmp, class_ctx) == 0)
			{
				xeno_copy_str(out_name, out_sz, tmp);
				out_name[out_sz - 1] = '\0';
				return 'f'; /* constructor treated as method */
			}
			/* field or method without function keyword — Xeno uses `function name` */
			/* Access / type keywords — skip one token and continue */
			if (strcmp(tmp, "public") == 0 || strcmp(tmp, "private") == 0 ||
				strcmp(tmp, "protected") == 0 || strcmp(tmp, "static") == 0 ||
				strcmp(tmp, "final") == 0 || strcmp(tmp, "virtual") == 0 ||
				strcmp(tmp, "override") == 0 || strcmp(tmp, "event") == 0)
			{
				continue;
			}
			/* Possible field: Type name;  — take next ident after type */
			if (ni > 0)
			{
				while (p < end && isspace((unsigned char)*p))
					p++;
				/* generics skip */
				if (p < end && *p == '<')
				{
					int depth = 1;
					p++;
					while (p < end && depth > 0)
					{
						if (*p == '<')
							depth++;
						else if (*p == '>')
							depth--;
						p++;
					}
					while (p < end && isspace((unsigned char)*p))
						p++;
				}
				if (p < end && (isalpha((unsigned char)*p) || *p == '_'))
				{
					ni = 0;
					while (p < end && (isalnum((unsigned char)*p) || *p == '_') && ni + 1 < out_sz)
						out_name[ni++] = *p++;
					out_name[ni] = '\0';
					return ni > 0 ? 'v' : 0;
				}
			}
			p = save + 1;
			continue;
		}
		/* No recognisable declaration */
		break;
	}
	return 0;
}

void xdoc_merge(XdocArchive *dst, const XdocArchive *src)
{
	if (!dst || !src)
		return;
	for (int i = 0; i < src->count; i++)
	{
		int found = -1;
		for (int j = 0; j < dst->count; j++)
		{
			if (strcmp(dst->entries[j].symbol, src->entries[i].symbol) == 0)
			{
				found = j;
				break;
			}
		}
		if (found >= 0)
		{
			dst->entries[found] = src->entries[i];
		}
		else
		{
			XdocEntry *e = xdoc_push(dst);
			if (!e)
				return;
			*e = src->entries[i];
		}
	}
}


int xdoc_extract_from_source(const char *source, const char *filename,
							XdocArchive *out)
{
	if (!source || !out)
		return 0;
	int added = 0;
	char class_ctx[XDOC_NAME_MAX] = "";
	const char *p = source;
	size_t len = strlen(source);

	while (*p)
	{
		/* Find block comment start */
		if (!(p[0] == '/' && p[1] == '*'))
		{
			/* Track class context outside docs */
			if (strncmp(p, "class", 5) == 0 && !isalnum((unsigned char)p[5]))
			{
				const char *q = p + 5;
				while (*q && isspace((unsigned char)*q))
					q++;
				size_t ni = 0;
				while (q[ni] && (isalnum((unsigned char)q[ni]) || q[ni] == '_') &&
					   ni + 1 < sizeof(class_ctx))
					ni++;
				if (ni > 0)
				{
					memcpy(class_ctx, q, ni);
					class_ctx[ni] = '\0';
				}
			}
			p++;
			continue;
		}

		const char *block_start = p;
		p += 2;
		const char *body = p;
		while (p[0] && !(p[0] == '*' && p[1] == '/'))
			p++;
		const char *body_end = p;
		if (p[0] == '*' && p[1] == '/')
			p += 2;
		else
			break;

		/* Does this block contain #Docs ... #!Docs? */
		const char *docs = NULL;
		const char *docs_end = NULL;
		for (const char *s = body; s + 5 < body_end; s++)
		{
			if (s[0] == '#' && strncmp(s, "#Docs", 5) == 0)
			{
				docs = s + 5;
				break;
			}
		}
		if (!docs)
			continue;
		for (const char *s = docs; s + 6 < body_end; s++)
		{
			if (s[0] == '#' && strncmp(s, "#!Docs", 6) == 0)
			{
				docs_end = s;
				break;
			}
		}
		if (!docs_end)
			continue;

		XdocEntry tmp;
		memset(&tmp, 0, sizeof(tmp));
		parse_docs_body(&tmp, docs, (size_t)(docs_end - docs));

		/* Bind to next declaration */
		char decl_name[XDOC_NAME_MAX] = "";
		size_t after = (size_t)(p - source);
		int kind = find_next_decl(source, after, len, decl_name, sizeof(decl_name),
								  class_ctx, sizeof(class_ctx));
		if (kind == 0 || !decl_name[0])
		{
			/* No following declaration — drop entirely */
			(void)block_start;
			continue;
		}

		if (tmp.title[0] && strcmp(tmp.title, decl_name) != 0)
		{
			fprintf(stderr,
					"xdoc: warning: docs title '%s' does not match following "
					"declaration '%s'%s%s — binding to declaration\n",
					tmp.title, decl_name,
					filename ? " in " : "", filename ? filename : "");
		}

		/* Build symbol key */
		if (kind == 'c')
			snprintf(tmp.symbol, sizeof(tmp.symbol), "%s", decl_name);
		else if (class_ctx[0])
			snprintf(tmp.symbol, sizeof(tmp.symbol), "%s.%s", class_ctx, decl_name);
		else
			snprintf(tmp.symbol, sizeof(tmp.symbol), "%s", decl_name);

		XdocEntry *dst = xdoc_push(out);
		if (!dst)
			return added;
		*dst = tmp;
		added++;
	}
	return added;
}

/* ── Load / save ─────────────────────────────────────────────────────── */

bool xdoc_save(const XdocArchive *ar, const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	fprintf(f, "XDOC1\n");
	for (int i = 0; i < ar->count; i++)
	{
		const XdocEntry *e = &ar->entries[i];
		fprintf(f, "ENTRY %s\n", e->symbol);
#define W(field) \
		if (e->field[0]) fprintf(f, #field " %s\n", e->field)
		W(title);
		W(summary);
		W(description);
		W(returns_doc);
		/* map returns_doc -> returns in file */
#undef W
		if (e->returns_doc[0])
			fprintf(f, "returns %s\n", e->returns_doc);
		if (e->example[0])
			fprintf(f, "example %s\n", e->example);
		if (e->see[0])
			fprintf(f, "see %s\n", e->see);
		if (e->link[0])
			fprintf(f, "link %s\n", e->link);
		if (e->since[0])
			fprintf(f, "since %s\n", e->since);
		if (e->deprecated[0])
			fprintf(f, "deprecated %s\n", e->deprecated);
		if (e->note[0])
			fprintf(f, "note %s\n", e->note);
		if (e->warning[0])
			fprintf(f, "warning %s\n", e->warning);
		if (e->important[0])
			fprintf(f, "important %s\n", e->important);
		if (e->remarks[0])
			fprintf(f, "remarks %s\n", e->remarks);
		for (int pi = 0; pi < e->param_count; pi++)
			fprintf(f, "param %s %s\n", e->params[pi].name, e->params[pi].doc);
		for (int ti = 0; ti < e->throw_count; ti++)
			fprintf(f, "throws %s %s\n", e->throws[ti].type_name, e->throws[ti].doc);
		fprintf(f, "END\n");
	}
	fclose(f);
	return true;
}

bool xdoc_load_mem(XdocArchive *out, const char *data, size_t size)
{
	memset(out, 0, sizeof(*out));
	if (!data || size < 5)
		return false;
	/* Require XDOC1 header on first line */
	if (strncmp(data, "XDOC1", 5) != 0)
		return false;

	const char *p = data;
	const char *end = data + size;
	/* skip first line */
	while (p < end && *p != '\n')
		p++;
	if (p < end)
		p++;

	XdocEntry *cur = NULL;
	while (p < end)
	{
		const char *line = p;
		while (p < end && *p != '\n')
			p++;
		size_t n = (size_t)(p - line);
		if (p < end)
			p++;
		while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n'))
			n--;
		if (n == 0)
			continue;

		char buf[1024];
		if (n >= sizeof(buf))
			n = sizeof(buf) - 1;
		memcpy(buf, line, n);
		buf[n] = '\0';

		if (strncmp(buf, "ENTRY ", 6) == 0)
		{
			cur = xdoc_push(out);
			if (!cur)
				return false;
			xeno_copy_str(cur->symbol, XDOC_SYM_MAX, buf + 6);
			continue;
		}
		if (strcmp(buf, "END") == 0)
		{
			cur = NULL;
			continue;
		}
		if (!cur)
			continue;
		char *sp = strchr(buf, ' ');
		if (!sp)
			continue;
		*sp = '\0';
		apply_tag(cur, buf, sp + 1, strlen(sp + 1));
	}
	return out->count > 0 || (size >= 5);
}

bool xdoc_load(XdocArchive *out, const char *path)
{
	memset(out, 0, sizeof(*out));
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0)
	{
		fclose(f);
		return false;
	}
	char *data = malloc((size_t)sz + 1);
	if (!data)
	{
		fclose(f);
		return false;
	}
	size_t nr = fread(data, 1, (size_t)sz, f);
	fclose(f);
	data[nr] = '\0';
	bool ok = xdoc_load_mem(out, data, nr);
	free(data);
	return ok;
}


char *xdoc_format_markdown(const XdocEntry *e)
{
	if (!e)
		return NULL;
	size_t cap = 2048;
	char *buf = malloc(cap);
	if (!buf)
		return NULL;
	size_t len = 0;
	#define APP(fmt, ...) \
		do { \
			int _n = snprintf(buf + len, cap > len ? cap - len : 0, fmt, ##__VA_ARGS__); \
			if (_n > 0) len += (size_t)_n; \
			if (len + 256 > cap) { \
				cap *= 2; \
				char *_b = realloc(buf, cap); \
				if (!_b) { free(buf); return NULL; } \
				buf = _b; \
			} \
		} while (0)

	if (e->title[0])
		APP("**%s**\n\n", e->title);
	if (e->summary[0])
		APP("%s\n\n", e->summary);
	if (e->description[0])
		APP("%s\n\n", e->description);
	if (e->param_count > 0)
	{
		APP("**Parameters**\n\n");
		for (int i = 0; i < e->param_count; i++)
			APP("- `%s` — %s\n", e->params[i].name, e->params[i].doc);
		APP("\n");
	}
	if (e->returns_doc[0])
		APP("**Returns** — %s\n\n", e->returns_doc);
	if (e->throw_count > 0)
	{
		APP("**Throws**\n\n");
		for (int i = 0; i < e->throw_count; i++)
			APP("- `%s` — %s\n", e->throws[i].type_name, e->throws[i].doc);
		APP("\n");
	}
	if (e->example[0])
		APP("**Example**\n\n```xenoscript\n%s\n```\n\n", e->example);
	if (e->deprecated[0])
		APP("**Deprecated** — %s\n\n", e->deprecated);
	if (e->since[0])
		APP("*Since %s*\n\n", e->since);
	if (e->note[0])
		APP("> **Note:** %s\n\n", e->note);
	if (e->warning[0])
		APP("> **Warning:** %s\n\n", e->warning);
	if (e->important[0])
		APP("> **Important:** %s\n\n", e->important);
	if (e->remarks[0])
		APP("%s\n\n", e->remarks);
	if (e->see[0])
		APP("*See also: %s*\n", e->see);
	if (e->link[0])
		APP("*<%s>*\n", e->link);
	#undef APP
	buf[len] = '\0';
	return buf;
}