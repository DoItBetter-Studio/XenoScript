/*
 * stub_gen.c — XenoScript stub source generator
 *
 * Converts a binary ClassDef back into readable .xeno source code.
 * Used by the LSP to present go-to-definition targets for XAR-loaded classes.
 *
 * Param names default to arg0, arg1, ... unless docs supply $names.
 * Type parameter names (e.g. K, V for Dictionary<K,V>) are preserved since
 * they ARE stored in ClassDef.type_param_names[].
 */

#include "stub_gen.h"
#include "strutil.h"
#include "../../includes/compiler.h"
#include "../../includes/ast.h"
#include "../../includes/xdoc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Type-kind → keyword string ────────────────────────────────────────── */

static const char *kind_to_str(int kind)
{
	switch ((TypeKind)kind)
	{
	case TYPE_VOID:   return "void";
	case TYPE_BOOL:   return "bool";
	case TYPE_INT:    return "int";
	case TYPE_FLOAT:  return "float";
	case TYPE_STRING: return "string";
	case TYPE_SBYTE:  return "sbyte";
	case TYPE_BYTE:   return "byte";
	case TYPE_SHORT:  return "short";
	case TYPE_USHORT: return "ushort";
	case TYPE_UINT:   return "uint";
	case TYPE_LONG:   return "long";
	case TYPE_ULONG:  return "ulong";
	case TYPE_DOUBLE: return "double";
	case TYPE_CHAR:   return "char";
	default:          return "object";
	}
}

/* NOTE: for TYPE_ARRAY this assumes class_name carries the ELEMENT type name.
 * If ClassDef stores element info elsewhere, wire it in here. */
static void fmt_type(char *buf, size_t bufsz,
					 int kind, const char *class_name, bool nullable)
{
	char tmp[CLASS_NAME_MAX + 8];
	bool has_name = class_name && class_name[0];

	switch ((TypeKind)kind)
	{
	case TYPE_OBJECT:
	case TYPE_ENUM:
	case TYPE_CLASS_REF:
	case TYPE_PARAM:
		snprintf(tmp, sizeof(tmp), "%s", has_name ? class_name : kind_to_str(kind));
		break;
	case TYPE_ARRAY:
		snprintf(tmp, sizeof(tmp), "%s[]", has_name ? class_name : "object");
		break;
	default:
		snprintf(tmp, sizeof(tmp), "%s", kind_to_str(kind));
		break;
	}

	if (nullable)
	{
		size_t n = strlen(tmp);
		if (n + 1 < sizeof(tmp)) { tmp[n] = '?'; tmp[n + 1] = '\0'; }
	}

	xeno_copy_str(buf, bufsz, tmp);
}

/* ── Dynamic string buffer ──────────────────────────────────────────────── */

typedef struct
{
	char *data;
	size_t len;
	size_t cap;
} SBuf;

static bool sbuf_init(SBuf *b)
{
	b->data = malloc(4096);
	if (!b->data)
		return false;
	b->data[0] = '\0';
	b->len = 0;
	b->cap = 4096;
	return true;
}

static bool sbuf_grow(SBuf *b, size_t needed)
{
	if (b->len + needed + 1 <= b->cap)
		return true;
	size_t newcap = b->cap * 2;
	while (newcap < b->len + needed + 1)
		newcap *= 2;
	char *p = realloc(b->data, newcap);
	if (!p)
		return false;
	b->data = p;
	b->cap = newcap;
	return true;
}

static bool sbuf_append(SBuf *b, const char *s)
{
	size_t n = strlen(s);
	if (!sbuf_grow(b, n))
		return false;
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
	return true;
}

static bool sbuf_appendf(SBuf *b, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	return sbuf_append(b, tmp);
}

/* Helper to emit a visibility section header only when it changes */
static const char *vis_name(uint8_t flags)
{
	switch (flags)
	{
	case 1:
		return "private";
	case 2:
		return "protected";
	default:
		return "public";
	}
}

/* ── Main generator ─────────────────────────────────────────────────────── */


/* Emit readable // documentation lines into a stub from an Xdoc entry. */
static void emit_doc_comments(SBuf *b, const XdocEntry *e, const char *indent)
{
	if (!e || !b)
		return;
	if (e->summary[0])
		sbuf_appendf(b, "%s// %s\n", indent, e->summary);
	if (e->description[0])
		sbuf_appendf(b, "%s// %s\n", indent, e->description);
	for (int i = 0; i < e->param_count; i++)
	{
		if (e->params[i].name[0] && e->params[i].doc[0])
			sbuf_appendf(b, "%s// @param %s — %s\n", indent,
						 e->params[i].name, e->params[i].doc);
		else if (e->params[i].name[0])
			sbuf_appendf(b, "%s// @param %s\n", indent, e->params[i].name);
	}
	if (e->returns_doc[0])
		sbuf_appendf(b, "%s// @returns %s\n", indent, e->returns_doc);
	for (int i = 0; i < e->throw_count; i++)
	{
		if (e->throws[i].type_name[0] && e->throws[i].doc[0])
			sbuf_appendf(b, "%s// @throws %s — %s\n", indent,
						 e->throws[i].type_name, e->throws[i].doc);
	}
	if (e->deprecated[0])
		sbuf_appendf(b, "%s// @deprecated %s\n", indent, e->deprecated);
	if (e->since[0])
		sbuf_appendf(b, "%s// @since %s\n", indent, e->since);
	if (e->note[0])
		sbuf_appendf(b, "%s// Note: %s\n", indent, e->note);
	if (e->warning[0])
		sbuf_appendf(b, "%s// Warning: %s\n", indent, e->warning);
}

char *stub_gen_class(const ClassDef *def, const Module *module, const XdocArchive *docs)
{
	if (!def)
		return NULL;

	SBuf b;
	if (!sbuf_init(&b))
		return NULL;

	/* File header comment */
	sbuf_appendf(&b,
				 "// XenoScript generated stub — read only\n"
				 "// Source: %s (compiled binary)\n\n",
				 def->name);

	/* Attributes */
	for (int ai = 0; ai < def->attribute_count; ai++)
	{
		const AttributeInstance *attr = &def->attributes[ai];
		sbuf_appendf(&b, "@%s", attr->class_name);
		if (attr->arg_count > 0)
		{
			sbuf_append(&b, "(");
			for (int j = 0; j < attr->arg_count; j++)
			{
				if (j > 0)
					sbuf_append(&b, ", ");
				const AttrArg *arg = &attr->args[j];
				switch (arg->kind)
				{
				case ATTR_ARG_STRING:
					sbuf_appendf(&b, "\"%s\"", arg->s ? arg->s : "");
					break;
				case ATTR_ARG_INT:
					sbuf_appendf(&b, "%lld", (long long)arg->i);
					break;
				case ATTR_ARG_FLOAT:
					sbuf_appendf(&b, "%g", arg->f);
					break;
				case ATTR_ARG_BOOL:
					sbuf_append(&b, arg->b ? "true" : "false");
					break;
				default:
					sbuf_append(&b, "...");
					break;
				}
			}
			sbuf_append(&b, ")");
		}
		sbuf_append(&b, "\n");
	}

	/* Class / interface / enum declaration line */
	if (def->is_enum)
	{
		sbuf_appendf(&b, "enum %s {\n", def->name);
		for (int i = 0; i < def->enum_member_count; i++)
		{
			if (def->enum_member_names[i])
			{
				if (i < def->enum_member_count - 1)
					sbuf_appendf(&b, "    %s,\n", def->enum_member_names[i]);
				else
					sbuf_appendf(&b, "    %s\n", def->enum_member_names[i]);
			}
		}
		sbuf_append(&b, "}\n");
		return b.data;
	}

	/* Generic type params */
	const char *kw = def->is_interface ? "interface" : "class";
	/* Class-level docs from .xdoc */
	if (docs)
	{
		const XdocEntry *cdoc = xdoc_find(docs, def->name);
		emit_doc_comments(&b, cdoc, "");
	}

	sbuf_appendf(&b, "%s %s", kw, def->name);
	if (def->type_param_count > 0)
	{
		sbuf_append(&b, "<");
		for (int i = 0; i < def->type_param_count; i++)
		{
			if (i > 0)
				sbuf_append(&b, ", ");
			sbuf_append(&b, def->type_param_names[i]);
		}
		sbuf_append(&b, ">");
	}

	/* Interface implementations */
	if (def->interface_count > 0 && !def->is_interface)
	{
		sbuf_append(&b, " implements ");
		for (int i = 0; i < def->interface_count; i++)
		{
			if (i > 0)
				sbuf_append(&b, ", ");
			sbuf_append(&b, def->interface_names[i]);
		}
	}

	sbuf_append(&b, " {\n");

	/* ── Fields ── */
	if (def->field_count > 0)
	{
		sbuf_append(&b, "    // Fields\n");
		int last_vis = -1;
		for (int i = 0; i < def->field_count; i++)
		{
			const FieldDef *f = &def->fields[i];
			char type_str[CLASS_NAME_MAX + 4];
			fmt_type(type_str, sizeof(type_str),
					 f->type_kind, f->class_name, f->is_nullable);

			if ((int)f->access_flags != last_vis)
			{
				sbuf_appendf(&b, "	%s:\n", vis_name(f->access_flags));
				last_vis = f->access_flags;
			}

			sbuf_appendf(&b, "		%s%s%s %s;\n",
						 f->is_static ? "static " : "",
						 f->is_final ? "final " : "",
						 type_str,
						 f->name);
		}
		sbuf_append(&b, "\n");
	}

	/* ── Constructor ── (stored as a function-table index, not in methods[]) */
	if (module && def->constructor_index >= 0 && def->constructor_index < module->count)
	{
		const Chunk *ctor = &module->chunks[def->constructor_index];
		int pc = ctor->param_count < 16 ? ctor->param_count : 16;

		sbuf_append(&b, "    // Constructor\n");
		sbuf_append(&b, "    public:\n");
		sbuf_appendf(&b, "        %s(", def->name);
		for (int pi = 0; pi < pc; pi++)
		{
			if (pi > 0)
				sbuf_append(&b, ", ");
			char p_type[CLASS_NAME_MAX + 8];
			fmt_type(p_type, sizeof(p_type), ctor->param_type_kinds[pi], NULL, false);
			sbuf_appendf(&b, "%s arg%d", p_type, pi);
		}
		sbuf_append(&b, ");\n\n");
	}

	/* ── Methods ── */
	if (def->method_count > 0)
	{
		sbuf_append(&b, "    // Methods\n");
		int last_vis = -1;
		for (int i = 0; i < def->method_count; i++)
		{
			const MethodDef *m = &def->methods[i];

			/* Method attributes */
			for (int ai = 0; ai < m->attribute_count; ai++)
			{
				const AttributeInstance *attr = &m->attributes[ai];
				sbuf_appendf(&b, "    @%s", attr->class_name);
				if (attr->arg_count > 0)
				{
					sbuf_append(&b, "(");
					for (int j = 0; j < attr->arg_count; j++)
					{
						if (j > 0)
							sbuf_append(&b, ", ");
						const AttrArg *arg = &attr->args[j];
						switch (arg->kind)
						{
						case ATTR_ARG_STRING:
							sbuf_appendf(&b, "\"%s\"", arg->s ? arg->s : "");
							break;
						case ATTR_ARG_INT:
							sbuf_appendf(&b, "%lld", (long long)arg->i);
							break;
						default:
							sbuf_append(&b, "...");
							break;
						}
					}
					sbuf_append(&b, ")");
				}
				sbuf_append(&b, "\n");
			}

			if ((int)m->access_flags != last_vis)
			{
				sbuf_appendf(&b, "    %s:\n", vis_name(m->access_flags));
				last_vis = m->access_flags;
			}

			char ret_str[CLASS_NAME_MAX + 4];
			fmt_type(ret_str, sizeof(ret_str),
					 m->return_type_kind, m->return_class_name,
					 m->return_is_nullable);

			/* Docs key: Class.method — param names + comments from .xdoc */
			const XdocEntry *mdoc = NULL;
			if (docs)
			{
				char key[128];
				snprintf(key, sizeof(key), "%s.%s", def->name, m->name);
				mdoc = xdoc_find(docs, key);
			}
			emit_doc_comments(&b, mdoc, "        ");

			sbuf_appendf(&b, "        %s%sfunction %s(",
						 m->is_static ? "static " : "",
						 m->is_virtual ? "virtual " : "",
						 m->name);
			for (int pi = 0; pi < m->param_count; pi++)
			{
				if (pi > 0)
					sbuf_append(&b, ", ");
				char p_type[CLASS_NAME_MAX + 4];
				fmt_type(p_type, sizeof(p_type),
						 m->param_type_kinds[pi],
						 m->param_class_names[pi],
						 m->param_is_nullable[pi]);
				const char *pname = NULL;
				if (mdoc && pi < mdoc->param_count && mdoc->params[pi].name[0])
					pname = mdoc->params[pi].name;
				if (pname)
					sbuf_appendf(&b, "%s %s", p_type, pname);
				else
					sbuf_appendf(&b, "%s arg%d", p_type, pi);
			}

			sbuf_appendf(&b, "): %s;\n", ret_str);
		}
		sbuf_append(&b, "\n");
	}

	/* ── Events ── */
	if (def->event_count > 0)
	{
		sbuf_append(&b, "    // Events\n");
		for (int i = 0; i < def->event_count; i++)
		{
			const EventDef *ev = &def->events[i];
			sbuf_appendf(&b, "    event %s(", ev->name);
			for (int pi = 0; pi < ev->param_count; pi++)
			{
				if (pi > 0)
					sbuf_append(&b, ", ");
				char p_type[CLASS_NAME_MAX + 4];
				fmt_type(p_type, sizeof(p_type),
						 ev->param_type_kinds[pi],
						 ev->param_class_names[pi],
						 ev->param_is_nullable[pi]);
				sbuf_appendf(&b, "%s arg%d", p_type, pi);
			}
			sbuf_append(&b, ");\n");
		}
	}

	sbuf_append(&b, "}\n");
	return b.data;
}