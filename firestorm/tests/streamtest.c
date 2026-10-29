/* The shim's MFD stream encoder on its own, for tools/test_mfdstream.py.
 *
 *   streamtest W H TILE DISPLAY STREAM < commands > packets
 *
 * Commands: 'F' and W*H bytes of grey - a frame; 'K' - a key frame of the
 * last one. Each part comes out as a FRAME message in the module protocol's
 * framing (COBS, zero-ended), its time_us the command's index. */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <io.h>
#include "../src/mfdstream.h"

static uint8_t seq;

static void emit(void *ctx, const uint8_t *p, int len)
{
	static uint8_t out[2 * (MS_HEADER + MS_MAX_BODY) + 16];
	int n = mp_pack(MP_FRAME, ++seq, p, len, out);
	fwrite(out, 1, n, stdout);
}

int main(int argc, char **argv)
{
	struct ms_enc e;
	int w, h, c;
	uint32_t i = 0;
	uint8_t *frame;

	if (argc < 6) {
		fprintf(stderr, "usage: streamtest W H TILE DISPLAY STREAM\n");
		return 2;
	}
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	w = atoi(argv[1]);
	h = atoi(argv[2]);
	ms_init(&e, atoi(argv[4]), w, h, atoi(argv[3]), (uint32_t)strtoul(argv[5], NULL, 0));
	frame = malloc((size_t)w * h);
	while ((c = getchar()) != EOF) {
		if (c == 'F') {
			if (fread(frame, 1, (size_t)w * h, stdin) != (size_t)w * h) return 1;
			ms_update(&e, frame, i, emit, NULL);
		} else if (c == 'K')
			ms_key(&e, i, emit, NULL);
		i++;
	}
	ms_free(&e);
	return 0;
}
