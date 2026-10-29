/* MFD display streaming: the encoder. See mfdstream.h and
 * docs/MFD_STREAMING_PROTOCOL.md; tools/mfdstream.py is the reference. */
#include <stdlib.h>
#include <string.h>
#include "mfdstream.h"

static void be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v & 0xFFFF); }

/* ------------------------------------------------ RLA
 *
 * Tokens, kind in the top two bits: 0 literal (that many bytes follow),
 * 1 repeat the last pixel, 2 copy the pixel above. A count n is 1..32 in
 * the low five bits as n-1, or with bit 5 set 1..8192 as 13 bits over two
 * bytes. Pixels run left to right, top to bottom, across row ends. */

struct rla { uint8_t *out; int n, cap; };

static int put(struct rla *r, int kind, int n)
{
	n--;
	if (n < 32) {
		if (r->n + 1 > r->cap) return -1;
		r->out[r->n++] = (uint8_t)(kind << 6 | n);
	} else {
		if (r->n + 2 > r->cap) return -1;
		r->out[r->n++] = (uint8_t)(kind << 6 | 0x20 | n >> 8);
		r->out[r->n++] = (uint8_t)n;
	}
	return 0;
}

int ms_rla(const uint8_t *fb, int stride, int x, int y, int w, int h, uint8_t *out, int cap)
{
	struct rla r = { out, 0, cap };
	int n = w * h, i = 0, lit = 0, prev = 0;
#define PX(j) fb[(y + (j) / w) * stride + x + (j) % w]
#define UP(j) fb[(y + (j) / w - 1) * stride + x + (j) % w]

	while (i < n) {
		int run = 0, a = 0;
		while (i + run < n && PX(i + run) == prev && run < MS_MAX_RUN) run++;
		while (i + a < n && a < MS_MAX_RUN) {
			int j = i + a;
			if (j < w && y == 0) break;		/* the screen's top row */
			if (PX(j) != UP(j)) break;
			a++;
		}
		if ((a >= 2 && a >= run) || run >= 2 || (run == 1 && !lit)) {
			int k, s = i - lit;			/* the literals before it */
			while (lit) {
				k = lit < MS_MAX_RUN ? lit : MS_MAX_RUN;
				if (put(&r, 0, k) || r.n + k > r.cap) return -1;
				for (; k; k--, lit--, s++) r.out[r.n++] = PX(s);
			}
			if (a >= 2 && a >= run) {
				if (put(&r, 2, a)) return -1;
				i += a;
				prev = PX(i - 1);
			} else {
				if (put(&r, 1, run)) return -1;
				i += run;
			}
		} else {
			prev = PX(i);
			lit++;
			i++;
		}
	}
	{
		int k, s = i - lit;
		while (lit) {
			k = lit < MS_MAX_RUN ? lit : MS_MAX_RUN;
			if (put(&r, 0, k) || r.n + k > r.cap) return -1;
			for (; k; k--, lit--, s++) r.out[r.n++] = PX(s);
		}
	}
#undef PX
#undef UP
	return r.n;
}

/* ------------------------------------------------ parts */

static void part_start(struct ms_enc *e)
{
	e->body = 0;
}

static void part_send(struct ms_enc *e, int end)
{
	uint8_t *p = e->part;
	p[0] = MS_VERSION;
	p[1] = (uint8_t)e->display;
	p[2] = (uint8_t)(e->flags | (end ? MS_F_END : 0));
	p[3] = 0;
	be32(p + 4, e->stream);
	be32(p + 8, e->frame);
	be16(p + 12, (unsigned)e->nparts);
	be16(p + 14, (unsigned)e->w);
	be16(p + 16, (unsigned)e->h);
	be32(p + 18, e->time_us);
	e->emit(e->ctx, p, MS_HEADER + e->body);
	e->nparts++;
	e->parts++;
	e->bytes += MS_HEADER + e->body;
	part_start(e);
}

static void add_rect(struct ms_enc *e, int x, int y, int w, int h, int codec, const uint8_t *data, int n,
		     const uint8_t *fb)
{
	uint8_t *o;
	int r;

	if (e->body && e->body + MS_RECT + n > MS_MAX_BODY) part_send(e, 0);
	o = e->part + MS_HEADER + e->body;
	be16(o, x); be16(o + 2, y); be16(o + 4, w); be16(o + 6, h);
	o[8] = (uint8_t)codec;
	be16(o + 9, n);
	o += MS_RECT;
	if (codec == MS_C_RAW)
		for (r = 0; r < h; r++) memcpy(o + r * w, fb + (y + r) * e->w + x, w);
	else
		memcpy(o, data, n);
	e->body += MS_RECT + n;
	e->rects++;
}

/* A span, coded; split in two (on a tile edge, else by rows) until it fits. */
static void code_rect(struct ms_enc *e, const uint8_t *fb, int x, int y, int w, int h)
{
	int limit = MS_MAX_BODY - MS_RECT;
	int n = ms_rla(fb, e->w, x, y, w, h, e->scratch, limit);

	if ((n < 0 || n >= w * h) && w * h <= limit) {
		add_rect(e, x, y, w, h, MS_C_RAW, NULL, w * h, fb);	/* noise */
		return;
	}
	if (n >= 0 && n < w * h) {
		add_rect(e, x, y, w, h, MS_C_RLA, e->scratch, n, fb);
		return;
	}
	if (w > e->tile) {
		int half = (w / e->tile / 2) * e->tile;
		code_rect(e, fb, x, y, half, h);
		code_rect(e, fb, x + half, y, w - half, h);
	} else {
		int half = h / 2;
		code_rect(e, fb, x, y, w, half);
		code_rect(e, fb, x, y + half, w, h - half);
	}
}

static void begin(struct ms_enc *e, int flags, uint32_t time_us, ms_emit_fn emit, void *ctx)
{
	e->flags = flags;
	e->time_us = time_us;
	e->emit = emit;
	e->ctx = ctx;
	e->nparts = 0;
	part_start(e);
}

int ms_init(struct ms_enc *e, int display, int w, int h, int tile, uint32_t stream)
{
	memset(e, 0, sizeof *e);
	e->display = display;
	e->w = w;
	e->h = h;
	e->tile = tile >= 4 ? tile : 16;
	e->stream = stream;
	return 0;
}

void ms_free(struct ms_enc *e)
{
	free(e->prev);
	e->prev = NULL;
}

int ms_key(struct ms_enc *e, uint32_t time_us, ms_emit_fn emit, void *ctx)
{
	int y;

	if (!e->prev) return 0;
	begin(e, MS_F_KEY, time_us, emit, ctx);
	for (y = 0; y < e->h; y += e->tile)
		code_rect(e, e->prev, 0, y, e->w, e->h - y < e->tile ? e->h - y : e->tile);
	part_send(e, 1);
	e->keys++;
	return e->nparts;
}

int ms_update(struct ms_enc *e, const uint8_t *cur, uint32_t time_us, ms_emit_fn emit, void *ctx)
{
	int k, ty, t = e->tile, w = e->w, h = e->h, any = 0, ntx = (w + t - 1) / t;

	if (!e->prev) {
		e->prev = malloc((size_t)w * h);
		if (!e->prev) return 0;
		memcpy(e->prev, cur, (size_t)w * h);
		e->frame++;
		return ms_key(e, time_us, emit, ctx);
	}
	begin(e, 0, time_us, emit, ctx);
	for (ty = 0; ty < h; ty += t) {
		int th = h - ty < t ? h - ty : t, start = -1;
		for (k = 0; k <= ntx; k++) {
			int tx = k * t, changed = 0, r, tw = w - tx < t ? w - tx : t;
			if (k < ntx)
				for (r = 0; r < th && !changed; r++)
					changed = memcmp(cur + (ty + r) * w + tx, e->prev + (ty + r) * w + tx, tw) != 0;
			if (changed && start < 0) start = tx;
			if (!changed && start >= 0) {
				if (!any) e->frame++;
				any = 1;
				/* Coded from the new frame: copies from above read rows
				 * that are new already, here or at the receiver. */
				code_rect(e, cur, start, ty, (tx < w ? tx : w) - start, th);
				start = -1;
			}
		}
	}
	memcpy(e->prev, cur, (size_t)w * h);
	if (!any) return 0;
	part_send(e, 1);
	e->updates++;
	return e->nparts;
}

/* ------------------------------------------------ framing */

uint16_t mp_crc16(const uint8_t *p, int n, uint16_t crc)
{
	int i, b;
	for (i = 0; i < n; i++) {
		crc ^= (uint16_t)(p[i] << 8);
		for (b = 0; b < 8; b++) crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
	}
	return crc;
}

/* COBS over type, seq, payload and CRC, without building them in one place. */
int mp_pack(uint8_t type, uint8_t seq, const uint8_t *payload, int len, uint8_t *out)
{
	uint8_t hd[2], crc[2];
	uint16_t c;
	int o = 1, code_at = 0, code = 1, i, part;
	const uint8_t *src[3];
	int srcn[3];

	hd[0] = type;
	hd[1] = seq;
	c = mp_crc16(hd, 2, 0xFFFF);
	c = mp_crc16(payload, len, c);
	crc[0] = (uint8_t)(c >> 8);
	crc[1] = (uint8_t)c;
	src[0] = hd; srcn[0] = 2;
	src[1] = payload; srcn[1] = len;
	src[2] = crc; srcn[2] = 2;
	for (part = 0; part < 3; part++)
		for (i = 0; i < srcn[part]; i++) {
			uint8_t b = src[part][i];
			if (b == 0) {
				out[code_at] = (uint8_t)code;
				code_at = o++;
				code = 1;
			} else {
				out[o++] = b;
				if (++code == 255) {
					out[code_at] = (uint8_t)code;
					code_at = o++;
					code = 1;
				}
			}
		}
	out[code_at] = (uint8_t)code;
	out[o++] = 0;
	return o;
}

int mp_unpack(const uint8_t *pkt, int n, uint8_t *out, int cap)
{
	int i = 0, o = 0;
	uint16_t c;

	while (i < n) {
		int code = pkt[i++], k;
		if (!code || i + code - 1 > n) return -1;
		for (k = 1; k < code; k++) {
			if (o >= cap) return -1;
			out[o++] = pkt[i++];
		}
		if (code < 255 && i < n) {
			if (o >= cap) return -1;
			out[o++] = 0;
		}
	}
	if (o < 4) return -1;
	c = mp_crc16(out, o - 2, 0xFFFF);
	if (out[o - 2] != (uint8_t)(c >> 8) || out[o - 1] != (uint8_t)c) return -1;
	return o - 2;
}
