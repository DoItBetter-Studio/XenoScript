/*
 * xar.c — XenoScript Archive (.xar) implementation
 *
 * Binary layout (all multi-byte integers big-endian):
 *
 *   [4]  magic        "XAR1"
 *   [4]  manifest_len
 *   [N]  manifest JSON
 *   [4]  chunk_count
 *   Per chunk:
 *     [4]  name_len
 *     [N]  name
 *     [4]  data_len
 *     [N]  .xbc bytes
 *
 * JSON is hand-generated and hand-parsed — no external dependencies.
 * The format is intentionally simple: string values only, no nesting
 * beyond the top-level arrays for exports and dependencies.
 */

#include "xar.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Utility ──────────────────────────────────────────────────────────── */

static uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

const char *xar_result_str(XarResult r)
{
    switch (r)
    {
    case XAR_OK:
        return "ok";
    case XAR_ERR_IO:
        return "I/O error";
    case XAR_ERR_BAD_MAGIC:
        return "bad magic (not a .xar file)";
    case XAR_ERR_CORRUPT:
        return "corrupt or truncated data";
    case XAR_ERR_OOM:
        return "out of memory";
    case XAR_ERR_TOO_MANY:
        return "too many chunks or exports";
    default:
        return "unknown error";
    }
}

void xar_archive_free(XarArchive *ar)
{
    for (int i = 0; i < ar->chunk_count; i++)
        free(ar->chunks[i].data);
    ar->chunk_count = 0;
}

/* ── Growable write buffer ────────────────────────────────────────────── */

typedef struct
{
    uint8_t *data;
    size_t size, cap;
} WBuf;

static bool wb_init(WBuf *w)
{
    w->data = malloc(512);
    w->size = 0;
    w->cap = 512;
    return w->data != NULL;
}
static void wb_free(WBuf *w) { free(w->data); }
static bool wb_ensure(WBuf *w, size_t n)
{
    if (w->size + n <= w->cap)
        return true;
    size_t nc = w->cap * 2;
    while (nc < w->size + n)
        nc *= 2;
    uint8_t *p = realloc(w->data, nc);
    if (!p)
        return false;
    w->data = p;
    w->cap = nc;
    return true;
}
static bool wb_bytes(WBuf *w, const void *d, size_t n)
{
    if (!wb_ensure(w, n))
        return false;
    memcpy(w->data + w->size, d, n);
    w->size += n;
    return true;
}
static bool wb_u32(WBuf *w, uint32_t v)
{
    uint8_t b[4];
    write_u32_be(b, v);
    return wb_bytes(w, b, 4);
}
static bool wb_str(WBuf *w, const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    return wb_u32(w, len) && wb_bytes(w, s, len);
}

/* ── Manifest JSON ────────────────────────────────────────────────────── */

/* Append a JSON-escaped string to buf (caller manages buf/pos/cap). */
static void json_str(char *buf, size_t *pos, size_t cap, const char *s)
{
    /* Opening quote */
    if (*pos < cap - 1)
        buf[(*pos)++] = '"';
    for (const char *p = s; *p && *pos < cap - 2; p++)
    {
        if (*p == '"' || *p == '\\')
        {
            if (*pos < cap - 2)
                buf[(*pos)++] = '\\';
        }
        buf[(*pos)++] = *p;
    }
    if (*pos < cap - 1)
        buf[(*pos)++] = '"';
}

#define J(lit)                            \
    do                                    \
    {                                     \
        size_t _l = strlen(lit);          \
        if (pos + _l < cap)               \
        {                                 \
            memcpy(out + pos, (lit), _l); \
            pos += _l;                    \
        }                                 \
    } while (0)

char *xar_manifest_to_json(const XarManifest *m)
{
    size_t cap = 8192;
    char *out = malloc(cap);
    if (!out)
        return NULL;
    size_t pos = 0;

    J("{");
    J("\"name\":");
    json_str(out, &pos, cap, m->name);
    J(",");
    J("\"version\":");
    json_str(out, &pos, cap, m->version);
    J(",");
    J("\"author\":");
    json_str(out, &pos, cap, m->author);
    J(",");
    J("\"description\":");
    json_str(out, &pos, cap, m->description);
    J(",");

    J("\"exports\":[");
    for (int i = 0; i < m->export_count; i++)
    {
        if (i)
            J(",");
        json_str(out, &pos, cap, m->exports[i]);
    }
    J("],");

    /* Dependencies stored as {"name":"id","version":"ver"} objects */
    J("\"dependencies\":[");
    for (int i = 0; i < m->dep_count; i++)
    {
        if (i)
            J(",");
        J("{\"name\":");
        json_str(out, &pos, cap, m->dependencies[i]);
        J(",\"version\":");
        json_str(out, &pos, cap, m->dep_versions[i]);
        J("}");
    }
    J("]");

    J("}");
    if (pos < cap)
        out[pos] = '\0';
    else
        out[cap - 1] = '\0';
    return out;
}
#undef J

/* Minimal JSON parser — only handles our own manifest format. */
static const char *json_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* Parse a JSON string into buf[bufsize]. Returns pointer past closing quote,
 * or NULL on error. */
static const char *json_parse_str(const char *p, char *buf, size_t bufsize)
{
    p = json_skip_ws(p);
    if (*p != '"')
        return NULL;
    p++;
    size_t i = 0;
    while (*p && *p != '"')
    {
        if (*p == '\\')
        {
            p++;
            if (!*p)
                return NULL;
        }
        if (i < bufsize - 1)
            buf[i++] = *p;
        p++;
    }
    if (*p != '"')
        return NULL;
    buf[i] = '\0';
    return p + 1;
}

bool xar_manifest_from_json(XarManifest *m, const char *json, size_t len)
{
    memset(m, 0, sizeof(*m));
    const char *p = json;
    const char *end = json + len;
    (void)end;

    p = json_skip_ws(p);
    if (*p != '{')
        return false;
    p++;

    while (*p)
    {
        p = json_skip_ws(p);
        if (*p == '}')
            break;
        if (*p == ',')
        {
            p++;
            continue;
        }

        /* Parse key */
        char key[64] = {0};
        p = json_parse_str(p, key, sizeof(key));
        if (!p)
            return false;
        p = json_skip_ws(p);
        if (*p != ':')
            return false;
        p++;
        p = json_skip_ws(p);

        if (strcmp(key, "name") == 0)
        {
            p = json_parse_str(p, m->name, sizeof(m->name));
        }
        else if (strcmp(key, "version") == 0)
        {
            p = json_parse_str(p, m->version, sizeof(m->version));
        }
        else if (strcmp(key, "author") == 0)
        {
            p = json_parse_str(p, m->author, sizeof(m->author));
        }
        else if (strcmp(key, "description") == 0)
        {
            p = json_parse_str(p, m->description, sizeof(m->description));
        }
        else if (strcmp(key, "exports") == 0 ||
                 strcmp(key, "dependencies") == 0)
        {
            bool is_exports = (strcmp(key, "exports") == 0);
            p = json_skip_ws(p);
            if (*p != '[')
                return false;
            p++;
            while (*p)
            {
                p = json_skip_ws(p);
                if (*p == ']')
                {
                    p++;
                    break;
                }
                if (*p == ',')
                {
                    p++;
                    continue;
                }

                if (is_exports)
                {
                    /* exports: array of strings */
                    char val[XAR_MAX_NAME] = {0};
                    p = json_parse_str(p, val, sizeof(val));
                    if (!p)
                        return false;
                    if (m->export_count < XAR_MAX_EXPORTS)
                        snprintf(m->exports[m->export_count++],
                                 XAR_MAX_NAME, "%s", val);
                }
                else
                {
                    /* dependencies: array of {name, version} objects */
                    p = json_skip_ws(p);
                    if (*p == '{')
                    {
                        p++;
                        char dep_name[XAR_MAX_NAME] = {0};
                        char dep_ver[64] = {0};
                        while (*p)
                        {
                            p = json_skip_ws(p);
                            if (*p == '}')
                            {
                                p++;
                                break;
                            }
                            if (*p == ',')
                            {
                                p++;
                                continue;
                            }
                            char dkey[32] = {0};
                            p = json_parse_str(p, dkey, sizeof(dkey));
                            if (!p)
                                return false;
                            p = json_skip_ws(p);
                            if (*p != ':')
                                return false;
                            p++;
                            p = json_skip_ws(p);
                            if (strcmp(dkey, "name") == 0)
                                p = json_parse_str(p, dep_name, sizeof(dep_name));
                            else if (strcmp(dkey, "version") == 0)
                                p = json_parse_str(p, dep_ver, sizeof(dep_ver));
                            else
                            {
                                /* unknown key — skip value */
                                char tmp[256];
                                p = json_parse_str(p, tmp, sizeof(tmp));
                            }
                            if (!p)
                                return false;
                        }
                        if (m->dep_count < XAR_MAX_DEPS)
                        {
                            snprintf(m->dependencies[m->dep_count], XAR_MAX_NAME,
                                     "%s", dep_name);
                            snprintf(m->dep_versions[m->dep_count], 64,
                                     "%s", dep_ver);
                            m->dep_count++;
                        }
                    }
                    else
                    {
                        /* Fallback: plain string dep (old format) */
                        char val[XAR_MAX_NAME] = {0};
                        p = json_parse_str(p, val, sizeof(val));
                        if (!p)
                            return false;
                        if (m->dep_count < XAR_MAX_DEPS)
                        {
                            snprintf(m->dependencies[m->dep_count],
                                     XAR_MAX_NAME, "%s", val);
                            m->dep_versions[m->dep_count][0] = '\0';
                            m->dep_count++;
                        }
                    }
                }
            }
        }
        else
        {
            /* Unknown key — skip its value */
            if (*p == '"')
            {
                char tmp[512];
                p = json_parse_str(p, tmp, sizeof(tmp));
            }
            else
            {
                while (*p && *p != ',' && *p != '}')
                    p++;
            }
        }
        if (!p)
            return false;
    }
    return true;
}

/* ── Core serialisation ───────────────────────────────────────────────── */

static XarResult serialise(WBuf *w,
                           const XarManifest *manifest,
                           const XarChunk *chunks, int n_chunks)
{
    /* Magic */
    if (!wb_bytes(w, XAR_MAGIC, XAR_MAGIC_LEN))
        return XAR_ERR_OOM;

    /* Manifest JSON */
    char *json = xar_manifest_to_json(manifest);
    if (!json)
        return XAR_ERR_OOM;
    uint32_t jlen = (uint32_t)strlen(json);
    bool ok = wb_u32(w, jlen) && wb_bytes(w, json, jlen);
    free(json);
    if (!ok)
        return XAR_ERR_OOM;

    /* Chunks */
    if (!wb_u32(w, (uint32_t)n_chunks))
        return XAR_ERR_OOM;
    for (int i = 0; i < n_chunks; i++)
    {
        const XarChunk *c = &chunks[i];
        if (!wb_str(w, c->name))
            return XAR_ERR_OOM;
        if (!wb_u32(w, (uint32_t)c->size))
            return XAR_ERR_OOM;
        if (!wb_bytes(w, c->data, c->size))
            return XAR_ERR_OOM;
    }
    return XAR_OK;
}

XarResult xar_write_mem(uint8_t **out, size_t *out_size,
                        const XarManifest *manifest,
                        const XarChunk *chunks, int n_chunks)
{
    WBuf w;
    if (!wb_init(&w))
        return XAR_ERR_OOM;
    XarResult r = serialise(&w, manifest, chunks, n_chunks);
    if (r != XAR_OK)
    {
        wb_free(&w);
        return r;
    }
    *out = w.data;
    *out_size = w.size;
    return XAR_OK;
}

XarResult xar_write(const char *path,
                    const XarManifest *manifest,
                    const XarChunk *chunks, int n_chunks)
{
    uint8_t *buf = NULL;
    size_t size = 0;
    XarResult r = xar_write_mem(&buf, &size, manifest, chunks, n_chunks);
    if (r != XAR_OK)
        return r;

    FILE *f = fopen(path, "wb");
    if (!f)
    {
        free(buf);
        return XAR_ERR_IO;
    }
    size_t written = fwrite(buf, 1, size, f);
    fclose(f);
    free(buf);
    return (written == size) ? XAR_OK : XAR_ERR_IO;
}

/* ── Deserialisation ──────────────────────────────────────────────────── */

typedef struct
{
    const uint8_t *p;
    const uint8_t *end;
} RBuf;

static bool rb_bytes(RBuf *r, void *out, size_t n)
{
    if ((size_t)(r->end - r->p) < n)
        return false;
    memcpy(out, r->p, n);
    r->p += n;
    return true;
}
static bool rb_u32(RBuf *r, uint32_t *out)
{
    uint8_t b[4];
    if (!rb_bytes(r, b, 4))
        return false;
    *out = read_u32_be(b);
    return true;
}

XarResult xar_read_mem(XarArchive *ar, const uint8_t *buf, size_t size)
{
    memset(ar, 0, sizeof(*ar));
    RBuf r = {buf, buf + size};

    /* Magic */
    char magic[4];
    if (!rb_bytes(&r, magic, 4))
        return XAR_ERR_CORRUPT;
    if (memcmp(magic, XAR_MAGIC, 4) != 0)
        return XAR_ERR_BAD_MAGIC;

    /* Manifest JSON */
    uint32_t jlen;
    if (!rb_u32(&r, &jlen))
        return XAR_ERR_CORRUPT;
    if ((size_t)(r.end - r.p) < jlen)
        return XAR_ERR_CORRUPT;
    if (!xar_manifest_from_json(&ar->manifest, (const char *)r.p, jlen))
        return XAR_ERR_CORRUPT;
    r.p += jlen;

    /* Chunks */
    uint32_t n_chunks;
    if (!rb_u32(&r, &n_chunks))
        return XAR_ERR_CORRUPT;
    if (n_chunks > XAR_MAX_CHUNKS)
        return XAR_ERR_TOO_MANY;

    for (uint32_t i = 0; i < n_chunks; i++)
    {
        XarChunk *c = &ar->chunks[i];

        uint32_t name_len;
        if (!rb_u32(&r, &name_len))
            return XAR_ERR_CORRUPT;
        if (name_len >= XAR_MAX_NAME)
            return XAR_ERR_CORRUPT;
        if (!rb_bytes(&r, c->name, name_len))
            return XAR_ERR_CORRUPT;
        c->name[name_len] = '\0';

        uint32_t data_len;
        if (!rb_u32(&r, &data_len))
            return XAR_ERR_CORRUPT;
        if ((size_t)(r.end - r.p) < data_len)
            return XAR_ERR_CORRUPT;

        c->data = malloc(data_len);
        if (!c->data)
        {
            ar->chunk_count = (int)i;
            xar_archive_free(ar);
            return XAR_ERR_OOM;
        }
        memcpy(c->data, r.p, data_len);
        c->size = data_len;
        r.p += data_len;

        ar->chunk_count++;
    }
    return XAR_OK;
}

XarResult xar_read(XarArchive *ar, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return XAR_ERR_IO;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc(sz);
    if (!buf)
    {
        fclose(f);
        return XAR_ERR_OOM;
    }

    size_t nr = fread(buf, 1, sz, f);
    fclose(f);
    if ((long)nr != sz)
    {
        free(buf);
        return XAR_ERR_IO;
    }

    XarResult r = xar_read_mem(ar, buf, sz);
    free(buf);
    return r;
}

/* ── xeno.project TOML → manifest ────────────────────────────────────── */

#include "toml.h"

bool xar_manifest_from_toml(XarManifest *m, const char *toml_source,
                            char *error_out, size_t error_cap)
{
    memset(m, 0, sizeof(*m));

    TomlDoc doc;
    if (!toml_parse(&doc, toml_source))
    {
        if (error_out)
            snprintf(error_out, error_cap, "TOML parse error: %s", doc.error);
        return false;
    }

    /* [mod] section */
    const char *id = toml_get(&doc, "mod", "id");
    const char *ver = toml_get(&doc, "mod", "version");
    const char *auth = toml_get(&doc, "mod", "author");
    const char *desc = toml_get(&doc, "mod", "description");

    if (!id || !*id)
    {
        if (error_out)
            snprintf(error_out, error_cap,
                     "xeno.project: [mod] id is required");
        toml_free(&doc);
        return false;
    }
    if (!ver || !*ver)
    {
        if (error_out)
            snprintf(error_out, error_cap,
                     "xeno.project: [mod] version is required");
        toml_free(&doc);
        return false;
    }

    snprintf(m->name, sizeof(m->name), "%s", id);
    snprintf(m->version, sizeof(m->version), "%s", ver);
    snprintf(m->author, sizeof(m->author), "%s", auth ? auth : "");
    snprintf(m->description, sizeof(m->description), "%s", desc ? desc : "");

    /* [dependencies] section — each entry is:  dep-id = "version" */
    for (int i = 0; i < doc.count; i++)
    {
        const TomlEntry *e = &doc.entries[i];
        if (strcmp(e->section, "dependencies") != 0)
            continue;
        if (m->dep_count >= XAR_MAX_DEPS)
        {
            if (error_out)
                snprintf(error_out, error_cap,
                         "xeno.project: too many dependencies (max %d)",
                         XAR_MAX_DEPS);
            toml_free(&doc);
            return false;
        }
        snprintf(m->dependencies[m->dep_count], XAR_MAX_NAME, "%s", e->key);
        snprintf(m->dep_versions[m->dep_count], 64, "%s", e->value);
        m->dep_count++;
    }

    toml_free(&doc);
    return true;
}
