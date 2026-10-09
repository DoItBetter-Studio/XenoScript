/*
 * xdbg.c — write / load .xdbg debug symbol sidecars
 */

#include "xdbg.h"
#include "strutil.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Chunk local-name helpers ─────────────────────────────────────────── */

bool chunk_ensure_local_names(Chunk *chunk, int slot_count)
{
	if (!chunk || slot_count <= 0)
		return true;
	if (slot_count > 64)
		slot_count = 64;
	if (chunk->local_names)
	{
		if (chunk->local_name_count >= slot_count)
			return true;
		char (*nn)[64] = realloc(chunk->local_names,
								 (size_t)slot_count * sizeof(*nn));
		if (!nn)
			return false;
		for (int i = chunk->local_name_count; i < slot_count; i++)
			nn[i][0] = '\0';
		chunk->local_names = nn;
		int *tk = realloc(chunk->local_type_kinds,
						  (size_t)slot_count * sizeof(int));
		if (tk)
		{
			for (int i = chunk->local_name_count; i < slot_count; i++)
				tk[i] = 0;
			chunk->local_type_kinds = tk;
		}
		chunk->local_name_count = slot_count;
		return true;
	}
	chunk->local_names = calloc((size_t)slot_count, sizeof(*chunk->local_names));
	chunk->local_type_kinds = calloc((size_t)slot_count, sizeof(int));
	if (!chunk->local_names)
		return false;
	chunk->local_name_count = slot_count;
	return true;
}

void chunk_set_local_name(Chunk *chunk, int slot, const char *name, int length)
{
	chunk_set_local_info(chunk, slot, name, length, 0);
}

void chunk_set_local_info(Chunk *chunk, int slot, const char *name, int length,
						  int type_kind)
{
	if (!chunk || slot < 0 || slot >= 64)
		return;
	if (!chunk_ensure_local_names(chunk, slot + 1))
		return;
	if (!name || length <= 0)
		chunk->local_names[slot][0] = '\0';
	else
	{
		int n = length < 63 ? length : 63;
		memcpy(chunk->local_names[slot], name, (size_t)n);
		chunk->local_names[slot][n] = '\0';
	}
	if (chunk->local_type_kinds)
		chunk->local_type_kinds[slot] = type_kind;
}

/* ── Binary helpers ───────────────────────────────────────────────────── */

static bool wb_u8(FILE *f, uint8_t v)  { return fwrite(&v, 1, 1, f) == 1; }
static bool wb_u16(FILE *f, uint16_t v)
{
	uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
	return fwrite(b, 1, 2, f) == 2;
}
static bool wb_u32(FILE *f, uint32_t v)
{
	uint8_t b[4] = {
		(uint8_t)(v & 0xFF),
		(uint8_t)((v >> 8) & 0xFF),
		(uint8_t)((v >> 16) & 0xFF),
		(uint8_t)((v >> 24) & 0xFF)
	};
	return fwrite(b, 1, 4, f) == 4;
}
static bool wb_str(FILE *f, const char *s)
{
	if (!s) s = "";
	size_t n = strlen(s);
	if (n > 65535) n = 65535;
	if (!wb_u16(f, (uint16_t)n)) return false;
	return n == 0 || fwrite(s, 1, n, f) == n;
}

/* ── Public API ───────────────────────────────────────────────────────── */

bool xdbg_write(const Module *module, const char *path)
{
	if (!module || !path) return false;
	FILE *f = fopen(path, "wb");
	if (!f) return false;

	bool ok = true;
	if (fwrite(XDBG_MAGIC, 1, 4, f) != 4) ok = false;
	if (ok && !wb_u8(f, XDBG_VERSION)) ok = false;
	if (ok && !wb_u32(f, (uint32_t)module->count)) ok = false;

	for (int i = 0; ok && i < module->count; i++)
	{
		const Chunk *ch = &module->chunks[i];
		const char *name = module->names[i][0] ? module->names[i] : "";
		if (!wb_str(f, name)) { ok = false; break; }
		if (!wb_str(f, ch->source_file)) { ok = false; break; }

		int lc = ch->local_count;
		if (lc < 0) lc = 0;
		if (lc > 64) lc = 64;
		if (!wb_u16(f, (uint16_t)lc)) { ok = false; break; }

		for (int s = 0; s < lc; s++)
		{
			const char *ln = "";
			if (ch->local_names && s < ch->local_name_count &&
				ch->local_names[s][0])
				ln = ch->local_names[s];
			if (!wb_str(f, ln)) { ok = false; break; }
			/* v2: TypeKind byte after each name */
			uint8_t tk = 0;
			if (ch->local_type_kinds && s < ch->local_name_count)
				tk = (uint8_t)ch->local_type_kinds[s];
			if (!wb_u8(f, tk)) { ok = false; break; }
		}
	}

	fclose(f);
	if (!ok)
		remove(path);
	return ok;
}

/* Cursor-based readers for in-memory load */
typedef struct {
	const uint8_t *p;
	const uint8_t *end;
} XdbgR;

static bool xr_u8(XdbgR *r, uint8_t *v)
{
	if (r->p >= r->end) return false;
	*v = *r->p++;
	return true;
}
static bool xr_u16(XdbgR *r, uint16_t *v)
{
	if (r->p + 2 > r->end) return false;
	*v = (uint16_t)r->p[0] | ((uint16_t)r->p[1] << 8);
	r->p += 2;
	return true;
}
static bool xr_u32(XdbgR *r, uint32_t *v)
{
	if (r->p + 4 > r->end) return false;
	*v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) |
		 ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
	r->p += 4;
	return true;
}
static bool xr_str(XdbgR *r, char *buf, size_t cap)
{
	uint16_t n = 0;
	if (!xr_u16(r, &n)) return false;
	if (r->p + n > r->end) return false;
	if (n >= cap)
	{
		if (cap > 0) buf[0] = '\0';
		r->p += n;
		return true;
	}
	if (n > 0) memcpy(buf, r->p, n);
	buf[n] = '\0';
	r->p += n;
	return true;
}

bool xdbg_load_mem(Module *module, const void *data, size_t size)
{
	if (!module || !data || size < 9) return false;

	XdbgR r;
	r.p = (const uint8_t *)data;
	r.end = r.p + size;

	if (r.p + 4 > r.end || memcmp(r.p, XDBG_MAGIC, 4) != 0)
		return false;
	r.p += 4;

	uint8_t ver = 0;
	if (!xr_u8(&r, &ver) || (ver != 1 && ver != 2))
		return false;
	uint32_t nchunks = 0;
	if (!xr_u32(&r, &nchunks))
		return false;

	for (uint32_t i = 0; i < nchunks; i++)
	{
		char cname[128];
		char sfile[128];
		if (!xr_str(&r, cname, sizeof(cname))) break;
		if (!xr_str(&r, sfile, sizeof(sfile))) break;

		uint16_t lc = 0;
		if (!xr_u16(&r, &lc)) break;

		int ci = -1;
		for (int j = 0; j < module->count; j++)
		{
			if (strcmp(module->names[j], cname) == 0)
			{
				ci = j;
				break;
			}
		}

		if (ci >= 0)
		{
			Chunk *ch = &module->chunks[ci];
			if (sfile[0] && !ch->source_file[0])
				xeno_copy_str(ch->source_file, sizeof(ch->source_file), sfile);

			if ((int)lc > 0 && chunk_ensure_local_names(ch, ch->local_count > 0
															? ch->local_count
															: (int)lc))
			{
				for (int s = 0; s < (int)lc; s++)
				{
					char nbuf[64];
					if (!xr_str(&r, nbuf, sizeof(nbuf)))
						return true;
					uint8_t tk = 0;
					if (ver >= 2 && !xr_u8(&r, &tk))
						return true;
					if (s < ch->local_count)
					{
						if (nbuf[0])
							xeno_copy_str(ch->local_names[s], 64, nbuf);
						if (ch->local_type_kinds)
							ch->local_type_kinds[s] = (int)tk;
					}
				}
			}
			else
			{
				for (int s = 0; s < (int)lc; s++)
				{
					char nbuf[64];
					if (!xr_str(&r, nbuf, sizeof(nbuf)))
						return true;
					uint8_t tk = 0;
					if (ver >= 2 && !xr_u8(&r, &tk))
						return true;
					(void)tk;
				}
			}
		}
		else
		{
			for (int s = 0; s < (int)lc; s++)
			{
				char nbuf[64];
				if (!xr_str(&r, nbuf, sizeof(nbuf)))
					return true;
				uint8_t tk = 0;
				if (ver >= 2 && !xr_u8(&r, &tk))
					return true;
				(void)tk;
			}
		}
	}
	return true;
}

bool xdbg_load(Module *module, const char *path)
{
	if (!module || !path) return false;
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0)
	{
		fclose(f);
		return false;
	}
	uint8_t *buf = malloc((size_t)sz);
	if (!buf)
	{
		fclose(f);
		return false;
	}
	size_t nr = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	bool ok = (nr == (size_t)sz) && xdbg_load_mem(module, buf, nr);
	free(buf);
	return ok;
}

bool xdbg_path_from_artifact(const char *artifact_path, char *out, size_t out_sz)
{
	if (!artifact_path || !out || out_sz < 8)
		return false;
	xeno_copy_str(out, out_sz, artifact_path);
	char *dot = strrchr(out, '.');
	if (dot && (strcmp(dot, ".xbc") == 0 || strcmp(dot, ".xar") == 0 ||
				strcmp(dot, ".XBC") == 0 || strcmp(dot, ".XAR") == 0))
	{
		xeno_copy_str(dot, out_sz - (size_t)(dot - out), ".xdbg");
	}
	else
	{
		size_t len = strlen(out);
		if (len + 6 >= out_sz)
			return false;
		memcpy(out + len, ".xdbg", 6);
	}
	return true;
}
