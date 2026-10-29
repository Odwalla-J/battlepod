/* MFD display streaming: the encoder (docs/MFD_STREAMING_PROTOCOL.md).
 *
 * Plain C, no Windows: the shim (ddraw.c) uses it from its streaming thread,
 * and tests/streamtest.c drives it alone against the Python reference
 * (tools/mfdstream.py), whose algorithm it follows byte for byte. */
#ifndef MFDSTREAM_H
#define MFDSTREAM_H
#include <stdint.h>

#define MS_VERSION	1
#define MS_F_KEY	0x01
#define MS_F_END	0x02
#define MS_F_DEFLATE	0x04
#define MS_C_RAW	0
#define MS_C_RLA	1
#define MS_HEADER	22	/* ver display flags 0, stream, frame, part, w, h, time_us */
#define MS_RECT		11	/* x y w h (u16), codec (u8), length (u16) */
#define MS_MAX_BODY	4096	/* a part's rectangles */
#define MS_MAX_RUN	8192

/* module protocol message types (docs/MODULE_PROTOCOL.md) */
#define MP_HELLO_Q	0x01
#define MP_HELLO	0x02
#define MP_FRAME	0x16
#define MP_SYNC		0x23
#define MP_FRAME_ACK	0x24

/* One finished FRAME payload (header and rectangles). */
typedef void (*ms_emit_fn)(void *ctx, const uint8_t *payload, int len);

struct ms_enc {
	int display, w, h, tile;
	uint32_t stream, frame;
	uint8_t *prev;			/* the frame last sent; NULL before the first */
	/* the part being filled */
	uint8_t part[MS_HEADER + MS_MAX_BODY];
	int body, nparts, flags;
	uint32_t time_us;
	ms_emit_fn emit;
	void *ctx;
	uint8_t scratch[MS_MAX_BODY];
	/* what it has sent, for the statistics */
	uint32_t updates, keys, parts, bytes, rects;
};

int ms_init(struct ms_enc *e, int display, int w, int h, int tile, uint32_t stream);
void ms_free(struct ms_enc *e);
/* Parts for a new frame (w*h bytes of grey): a key frame the first time,
 * then the changed regions; nothing when nothing changed. Returns how many
 * parts were emitted. */
int ms_update(struct ms_enc *e, const uint8_t *cur, uint32_t time_us, ms_emit_fn emit, void *ctx);
/* The frame last sent, whole, under its own number. */
int ms_key(struct ms_enc *e, uint32_t time_us, ms_emit_fn emit, void *ctx);

/* RLA-code a rectangle of fb into out; -1 if it needs more than cap bytes. */
int ms_rla(const uint8_t *fb, int stride, int x, int y, int w, int h, uint8_t *out, int cap);

/* The module protocol's framing: type, seq, payload, CRC-16/CCITT-FALSE,
 * COBS, a zero. out needs len + len/254 + 7 bytes. Returns its length. */
int mp_pack(uint8_t type, uint8_t seq, const uint8_t *payload, int len, uint8_t *out);
uint16_t mp_crc16(const uint8_t *p, int n, uint16_t crc);
/* A packet without its zero, decoded and checked: the message (type, seq,
 * payload) in out, its length, or -1. */
int mp_unpack(const uint8_t *pkt, int n, uint8_t *out, int cap);

#endif
