/*
 * stub_gen.c — XenoScript stub source generator
 *
 * Converts a binary ClassDef back into readable .xeno source code.
 * Used by the LSP to present go-to-definition targets for XAR-loaded classes.
 *
 * Param names are synthesised as arg0, arg1, etc.
 * Type parameter names (e.g. K, V for Dictionary<K,V>) are preserved since
 * they ARE stored in ClassDef.type_param_names[].
 */

#include "stub_gen.h"
#include "../../includes/compiler.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Type-kind → keyword string ────────────────────────────────────────── */

static const char *kind_to_str(int kind) {
    switch (kind) {
        case  1: return "void";
        case  2: return "bool";
        case  3: return "int";
        case  4: return "float";
        case  5: return "string";
        case  6: return "object";
        case  7: return "sbyte";
        case  8: return "byte";
        case  9: return "short";
        case 10: return "ushort";
        case 11: return "uint";
        case 12: return "long";
        case 13: return "ulong";
        case 14: return "double";
        case 15: return "char";
        default: return "object";
    }
}

/* Write a type string into buf (up to bufsz-1 chars).
 * Handles primitives, object types (class_name), and nullable suffix. */
static void fmt_type(char *buf, size_t bufsz,
                     int kind, const char *class_name, bool nullable) {
    const char *base;
    char tmp[CLASS_NAME_MAX + 4];

    /* TYPE_OBJECT (6) or TYPE_CLASS_REF (16+) — use class_name */
    if ((kind == 6 || kind >= 16) && class_name && class_name[0]) {
        base = class_name;
    } else {
        base = kind_to_str(kind);
    }

    if (nullable)
        snprintf(tmp, sizeof(tmp), "%s?", base);
    else
        snprintf(tmp, sizeof(tmp), "%s", base);

    strncpy(buf, tmp, bufsz - 1);
    buf[bufsz - 1] = '\0';
}

/* ── Dynamic string buffer ──────────────────────────────────────────────── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} SBuf;

static bool sbuf_init(SBuf *b) {
    b->data = malloc(4096);
    if (!b->data) return false;
    b->data[0] = '\0';
    b->len = 0;
    b->cap = 4096;
    return true;
}

static bool sbuf_grow(SBuf *b, size_t needed) {
    if (b->len + needed + 1 <= b->cap) return true;
    size_t newcap = b->cap * 2;
    while (newcap < b->len + needed + 1) newcap *= 2;
    char *p = realloc(b->data, newcap);
    if (!p) return false;
    b->data = p;
    b->cap  = newcap;
    return true;
}

static bool sbuf_append(SBuf *b, const char *s) {
    size_t n = strlen(s);
    if (!sbuf_grow(b, n)) return false;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return true;
}

static bool sbuf_appendf(SBuf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    return sbuf_append(b, tmp);
}

/* ── Main generator ─────────────────────────────────────────────────────── */

char *stub_gen_class(const ClassDef *def, const Module *module) {
    (void)module; /* param info now stored in MethodDef directly */
    if (!def) return NULL;

    SBuf b;
    if (!sbuf_init(&b)) return NULL;

    /* File header comment */
    sbuf_appendf(&b,
        "// XenoScript generated stub — read only\n"
        "// Source: %s (compiled binary)\n\n",
        def->name);

    /* Attributes */
    for (int ai = 0; ai < def->attribute_count; ai++) {
        const AttributeInstance *attr = &def->attributes[ai];
        sbuf_appendf(&b, "@%s", attr->class_name);
        if (attr->arg_count > 0) {
            sbuf_append(&b, "(");
            for (int j = 0; j < attr->arg_count; j++) {
                if (j > 0) sbuf_append(&b, ", ");
                const AttrArg *arg = &attr->args[j];
                switch (arg->kind) {
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
    if (def->is_enum) {
        sbuf_appendf(&b, "enum %s {\n", def->name);
        for (int i = 0; i < def->enum_member_count; i++) {
            if (def->enum_member_names[i]) {
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
    sbuf_appendf(&b, "%s %s", kw, def->name);
    if (def->type_param_count > 0) {
        sbuf_append(&b, "<");
        for (int i = 0; i < def->type_param_count; i++) {
            if (i > 0) sbuf_append(&b, ", ");
            sbuf_append(&b, def->type_param_names[i]);
        }
        sbuf_append(&b, ">");
    }

    /* Interface implementations */
    if (def->interface_count > 0 && !def->is_interface) {
        sbuf_append(&b, " implements ");
        for (int i = 0; i < def->interface_count; i++) {
            if (i > 0) sbuf_append(&b, ", ");
            sbuf_append(&b, def->interface_names[i]);
        }
    }

    sbuf_append(&b, " {\n");

    /* ── Fields ── */
    if (def->field_count > 0) {
        sbuf_append(&b, "    // Fields\n");
        for (int i = 0; i < def->field_count; i++) {
            const FieldDef *f = &def->fields[i];
            char type_str[CLASS_NAME_MAX + 4];
            fmt_type(type_str, sizeof(type_str),
                     f->type_kind, f->class_name, f->is_nullable);

            sbuf_appendf(&b, "    public %s%s%s %s;\n",
                f->is_static ? "static " : "",
                f->is_final  ? "final "  : "",
                type_str,
                f->name);
        }
        sbuf_append(&b, "\n");
    }

    /* ── Methods ── */
    if (def->method_count > 0) {
        sbuf_append(&b, "    // Methods\n");
        for (int i = 0; i < def->method_count; i++) {
            const MethodDef *m = &def->methods[i];

            /* Method attributes */
            for (int ai = 0; ai < m->attribute_count; ai++) {
                const AttributeInstance *attr = &m->attributes[ai];
                sbuf_appendf(&b, "    @%s", attr->class_name);
                if (attr->arg_count > 0) {
                    sbuf_append(&b, "(");
                    for (int j = 0; j < attr->arg_count; j++) {
                        if (j > 0) sbuf_append(&b, ", ");
                        const AttrArg *arg = &attr->args[j];
                        switch (arg->kind) {
                            case ATTR_ARG_STRING:
                                sbuf_appendf(&b, "\"%s\"", arg->s ? arg->s : "");
                                break;
                            case ATTR_ARG_INT:
                                sbuf_appendf(&b, "%lld", (long long)arg->i);
                                break;
                            default: sbuf_append(&b, "..."); break;
                        }
                    }
                    sbuf_append(&b, ")");
                }
                sbuf_append(&b, "\n");
            }

            /* Return type */
            char ret_str[CLASS_NAME_MAX + 4];
            fmt_type(ret_str, sizeof(ret_str),
                     m->return_type_kind, m->return_class_name,
                     m->return_is_nullable);

            sbuf_appendf(&b, "    public %s%sfunction %s(",
                m->is_static  ? "static "  : "",
                m->is_virtual ? "virtual " : "",
                m->name);

            for (int pi = 0; pi < m->param_count; pi++) {
                if (pi > 0) sbuf_append(&b, ", ");
                char p_type[CLASS_NAME_MAX + 4];
                fmt_type(p_type, sizeof(p_type),
                         m->param_type_kinds[pi],
                         m->param_class_names[pi],
                         m->param_is_nullable[pi]);
                sbuf_appendf(&b, "arg%d: %s", pi, p_type);
            }

            sbuf_appendf(&b, "): %s;\n", ret_str);
        }
        sbuf_append(&b, "\n");
    }

    /* ── Events ── */
    if (def->event_count > 0) {
        sbuf_append(&b, "    // Events\n");
        for (int i = 0; i < def->event_count; i++) {
            const EventDef *ev = &def->events[i];
            sbuf_appendf(&b, "    event %s(", ev->name);
            for (int pi = 0; pi < ev->param_count; pi++) {
                if (pi > 0) sbuf_append(&b, ", ");
                char p_type[CLASS_NAME_MAX + 4];
                fmt_type(p_type, sizeof(p_type),
                         ev->param_type_kinds[pi],
                         ev->param_class_names[pi],
                         ev->param_is_nullable[pi]);
                sbuf_appendf(&b, "arg%d: %s", pi, p_type);
            }
            sbuf_append(&b, ");\n");
        }
    }

    sbuf_append(&b, "}\n");
    return b.data;
}
