/* The radar screen: the pod's secondary display, stood in for.
 *
 * On the pod it was an Amiga board under the main monitor, fed by the
 * 68020 through a shared block (SecCom) and a message protocol that is not
 * decoded (RENDERING.md, *The auxiliary displays*). What it showed is known
 * from the firmware's own names - SECTOR, TARGET and DAMAGE, MAPDISP and
 * NAVSECT - and from footage: a green radar grid.
 *
 * This draws it from what the cockpit already receives over its network
 * link, which is everything a radar needs:
 *
 *   0xEC   every Mech's position and heading, ten times a second (src/
 *          battlepod.c's radar_note keeps them; its own goes out the same way)
 *   0xE4   MECH_CLASS (class 1): each Mech's name, vehicle record and paint,
 *          its node at +0x09 - the byte that also names it in its 0xEC
 *   0xE4   classes 2, 3 and 6: the map, object by object - a model id, a
 *          place, a heading and a scale - drawn as each model's footprint,
 *          from the bounding box it states for itself
 *
 * Three pages, chosen as the pod's own display keys choose (F1, F2, F3 -
 * buttons 00, 04 and 08), or with Tab: SECTOR, the radar round this Mech,
 * heading up; MAP, the whole battlefield, north up; and CONTACTS, every
 * Mech in a table. PgUp/PgDn change the radar's range.
 *
 * Its window is [radar] in battletech.ini: show, x, y, w, h, fullscreen and
 * monitor, as [view]'s; layout (portrait, landscape, or the window's own
 * shape, the default) (monitor 2 by default: a second screen is where the
 * pod had it), title, and the page (0-2) and range (0-4: 250 m to 4 km) it
 * opens on.
 */
#ifndef BATTLEPOD_RADAR_H
#define BATTLEPOD_RADAR_H

#define RADAR_W 640
#define RADAR_H 400
#define RMAP_MAX 2048
#define RMECH_MAX 32

/* A Mech by its node: what MECH_CLASS said about it. */
static struct rmech { int set; char name[9]; int vehicle, paint; } g_rmech[RMECH_MAX];

/* The map, as the console built it. */
static struct robj { uint32_t shape; int cls; float x, y, heading, scale; } g_rmap[RMAP_MAX];
static int g_nrmap;

static SDL_Window *g_rw;
static SDL_Renderer *g_rr;
static int g_rpage;			/* 0 sector, 1 map, 2 contacts */
static int g_rrange = 1;		/* index into RRANGES */
static const int RRANGES[] = { 250, 500, 1000, 2000, 4000 };

static void rmap_note(const uint8_t *p, uint32_t n)
{
	int32_t cls;

	if (n < 0x40 || p[0] != 0xE4) return;
	cls = (int32_t)(((uint32_t)p[0x0E] << 24) | ((uint32_t)p[0x0F] << 16) | ((uint32_t)p[0x10] << 8) | p[0x11]);
	if (cls == -1) {			/* "Reset world" */
		g_nrmap = 0;
		memset(g_rmech, 0, sizeof g_rmech);
		return;
	}
	if (cls == 1 && n >= 0x54) {
		struct rmech *m = &g_rmech[p[0x09] % RMECH_MAX];
		int i;
		m->set = 1;
		for (i = 0; i < 8; i++) {
			char c = (char)p[0x22 + i];
			m->name[i] = c >= ' ' && c < 127 ? c : 0;
			if (!c) break;
		}
		m->name[8] = 0;
		m->vehicle = p[0x4A] << 8 | p[0x4B];
		m->paint = p[0x4E] << 8 | p[0x4F];
		return;
	}
	if ((cls == 2 || cls == 3 || cls == 6) && g_nrmap < RMAP_MAX) {
		struct robj *o = &g_rmap[g_nrmap++];
		o->cls = cls;
		o->shape = ((uint32_t)p[0x22] << 24) | ((uint32_t)p[0x23] << 16) | ((uint32_t)p[0x24] << 8) | p[0x25];
		o->x = be_float(p + 0x16);
		o->y = be_float(p + 0x1A);
		o->scale = cls == 2 ? be_float(p + 0x2A) : cls == 3 ? be_float(p + 0x26) : 1.0f;
		o->heading = cls == 2 ? be_float(p + 0x2E) : be_float(p + 0x2A);
		if (!(o->scale > 0.0f && o->scale < 100.0f)) o->scale = 1.0f;
	}
}

/* A model's footprint, from the box its header states: x and z extents,
 * and how tall it stands. */
static int rfoot(uint32_t shape, float *x0, float *x1, float *z0, float *z1, float *top)
{
	static struct { uint32_t id; int ok; float b[6]; } cache[256];
	static int ncache;
	const uint8_t *d;
	uint32_t len;
	int i, k;

	for (i = 0; i < ncache; i++)
		if (cache[i].id == shape) break;
	if (i == ncache) {
		if (ncache >= 256) return 0;
		cache[i].id = shape;
		cache[i].ok = 0;
		ncache++;
		d = res_bytes(shape, &len);
		if (d && len >= 64) {
			for (k = 0; k < 6; k++)
				cache[i].b[k] = be_float(d + 36 + k * 4);
			cache[i].ok = cache[i].b[1] > cache[i].b[0] && cache[i].b[5] > cache[i].b[4];
		}
	}
	if (!cache[i].ok) return 0;
	*x0 = cache[i].b[0]; *x1 = cache[i].b[1];
	*top = cache[i].b[3];
	*z0 = cache[i].b[4]; *z1 = cache[i].b[5];
	return 1;
}

/* A vehicle record's name out of the ROM in memory: 38 records of 958
 * bytes at file offset 0x70582 (tools/vehicles.py), the image loaded at
 * 0x020FFFE4. "MP-MadCat Prime" comes back as "MADCAT PRIME". */
static void rvehicle(int v, char *out, int n)
{
	char s[41];
	int i, j = 0, start = 0;

	out[0] = 0;
	if (v < 0 || v >= 38) return;
	for (i = 0; i < 40; i++) {
		s[i] = (char)m68k_read_memory_8(0x020FFFE4u + 0x70582u + (uint32_t)v * 958u + (uint32_t)i);
		if (!s[i]) break;
	}
	s[i] = 0;
	if (i > 3 && s[2] == '-') start = 3;
	for (i = start; s[i] && j < n - 1; i++)
		out[j++] = (char)(s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i]);
	out[j] = 0;
}

/* The Mech trims' colours (the units of its paint), as the radar's blips. */
static void raccent(SDL_Renderer *r, int paint, int dim)
{
	static const uint8_t C[8][3] = {
		{ 255, 70, 50 }, { 80, 255, 90 }, { 90, 150, 255 }, { 255, 230, 60 },
		{ 240, 240, 240 }, { 150, 150, 150 }, { 190, 190, 190 }, { 210, 110, 255 },
	};
	const uint8_t *c = C[(paint % 10) & 7];
	if (dim) SDL_SetRenderDrawColor(r, c[0] / 3, c[1] / 3, c[2] / 3, 255);
	else SDL_SetRenderDrawColor(r, c[0], c[1], c[2], 255);
}

#define RG_BACK   0, 8, 3
#define RG_GRID   0, 70, 28
#define RG_MAP    0, 110, 45
#define RG_TALL   30, 170, 70
#define RG_TEXT   90, 255, 130
#define RG_DIM    40, 140, 70

static void rcol(SDL_Renderer *r, int a, int b, int c) { SDL_SetRenderDrawColor(r, (Uint8)a, (Uint8)b, (Uint8)c, 255); }

/* World (x, y) to screen through a view: centre, scale (pixels a metre)
 * and a rotation - heading up (sin h, -cos h is forward) or north up. */
struct rview { float cx, cy, wx, wy, ppm, s, c; };

static void rproj(const struct rview *v, float x, float y, float *sx, float *sy)
{
	float dx = x - v->wx, dy = y - v->wy;
	/* forward = (sin h, -cos h), right = (cos h, sin h) */
	float right = dx * v->c + dy * v->s, fwd = dx * v->s - dy * v->c;
	*sx = v->cx + right * v->ppm;
	*sy = v->cy - fwd * v->ppm;
}

static void rline(SDL_Renderer *r, const struct rview *v, float ax, float ay, float bx, float by)
{
	float x0, y0, x1, y1;
	rproj(v, ax, ay, &x0, &y0);
	rproj(v, bx, by, &x1, &y1);
	SDL_RenderDrawLine(r, (int)x0, (int)y0, (int)x1, (int)y1);
}

/* A flat model's own polygons, decoded once: the drop pads, roads and
 * plazas (101, 104, 105) are L-shapes and strips that a bounding box turns
 * into a 200 m square. */
static const struct mesh *rflat(uint32_t shape)
{
	static struct { uint32_t id; struct mesh *m; } cache[32];
	static int n;
	const uint8_t *d;
	uint32_t len;
	int i;

	for (i = 0; i < n; i++)
		if (cache[i].id == shape) return cache[i].m;
	if (n >= 32) return NULL;
	cache[n].id = shape;
	cache[n].m = NULL;
	if ((d = res_bytes(shape, &len)) != NULL && (cache[n].m = malloc(sizeof *cache[n].m)) != NULL) {
		mesh_best(cache[n].m, d, len);
		if (!cache[n].m->npoly) { free(cache[n].m); cache[n].m = NULL; }
	}
	return cache[n++].m;
}

static void rmap_draw(SDL_Renderer *r, const struct rview *v)
{
	int i, pass;

	/* What lies flat first, filled dimly; then what stands, outlined. The
	 * terrain patches (41, 750 m across) are only the ground's own grain
	 * and are left out: drawn, they crossed the whole scope. */
	for (pass = 0; pass < 2; pass++)
	for (i = 0; i < g_nrmap; i++) {
		const struct robj *o = &g_rmap[i];
		float x0, x1, z0, z1, top, h = o->heading * 3.14159265f / 180.0f;
		float s = sinf(h), c = cosf(h), px[4], py[4], sx, sy;
		int k, flat;
		if (!rfoot(o->shape, &x0, &x1, &z0, &z1, &top)) continue;
		rproj(v, o->x, o->y, &sx, &sy);
		if (sx < -900 || sy < -900 || sx > RADAR_W + 900 || sy > RADAR_H + 900) continue;
		flat = top * o->scale < 1.0f;
		if (flat != (pass == 0)) continue;
		if (flat) {
			const struct mesh *m;
			int p;
			if ((x1 - x0) * o->scale > 400.0f || !(m = rflat(o->shape))) continue;
			for (p = 0; p < m->npoly; p++) {
				const struct mesh_poly *q = &m->poly[p];
				SDL_Vertex vx[MESH_PIDX];
				int j, ok = 1, mi = q->mat < MESH_MATS ? q->mat : 0;
				/* dark ground (pads, roads) green, light ground left out */
				float lum = m->mset[mi] ? (m->mr[mi] + m->mg[mi] + m->mb[mi]) / 3 : 0.5f;
				if (lum > 0.3f || q->n < 3) continue;
				for (j = 0; j < q->n; j++) {
					int vi = q->v[j];
					float mx, mz, wx, wy;
					if (vi >= MESH_VERTS || !m->vset[vi]) { ok = 0; break; }
					mx = m->vx[vi] * o->scale; mz = m->vz[vi] * o->scale;
					wx = o->x + mx * c + mz * s;
					wy = o->y + mx * s - mz * c;
					rproj(v, wx, wy, &vx[j].position.x, &vx[j].position.y);
					vx[j].color.r = 0; vx[j].color.g = 38; vx[j].color.b = 16; vx[j].color.a = 255;
					vx[j].tex_coord.x = vx[j].tex_coord.y = 0;
				}
				if (!ok) continue;
				for (j = 1; j + 1 < q->n; j++) {
					SDL_Vertex tri[3];
					tri[0] = vx[0]; tri[1] = vx[j]; tri[2] = vx[j + 1];
					SDL_RenderGeometry(r, NULL, tri, 3, NULL, 0);
				}
			}
			continue;
		}
		for (k = 0; k < 4; k++) {
			float mx = (k == 1 || k == 2 ? x1 : x0) * o->scale;
			float mz = (k >= 2 ? z1 : z0) * o->scale;
			px[k] = o->x + mx * c + mz * s;
			py[k] = o->y + mx * s - mz * c;
		}
		/* low walls and rocks in the grid's green, what stands tall brighter */
		if (top * o->scale > 4.0f) rcol(r, RG_TALL); else rcol(r, RG_MAP);
		for (k = 0; k < 4; k++) rline(r, v, px[k], py[k], px[(k + 1) & 3], py[(k + 1) & 3]);
	}
}

/* A Mech: a triangle along its heading, in its trim's colour. */
static void rmech_draw(SDL_Renderer *r, const struct rview *v, const struct blip *b, int paint,
		       int me, const char *label)
{
	float h = b->h * 3.14159265f / 180.0f, fx = sinf(h), fy = -cosf(h), rx = cosf(h), ry = sinf(h);
	float k = 7.0f / v->ppm, x[3], y[3];
	int i;

	rproj(v, b->x + fx * k * 1.4f, b->y + fy * k * 1.4f, &x[0], &y[0]);
	rproj(v, b->x - fx * k + rx * k * 0.8f, b->y - fy * k + ry * k * 0.8f, &x[1], &y[1]);
	rproj(v, b->x - fx * k - rx * k * 0.8f, b->y - fy * k - ry * k * 0.8f, &x[2], &y[2]);
	if (me) rcol(r, RG_TEXT); else raccent(r, paint, 0);
	for (i = 0; i < 3; i++)
		SDL_RenderDrawLine(r, (int)x[i], (int)y[i], (int)x[(i + 1) % 3], (int)y[(i + 1) % 3]);
	if (label && *label) {
		float sx, sy;
		rproj(v, b->x, b->y, &sx, &sy);
		text(r, (int)sx + 9, (int)sy - 3, 1, label, 8);
	}
}

static void rcircle(SDL_Renderer *r, int cx, int cy, int rad)
{
	SDL_Point pts[73];
	int i;
	for (i = 0; i <= 72; i++) {
		float a = i * 6.2831853f / 72;
		pts[i].x = cx + (int)(rad * cosf(a));
		pts[i].y = cy + (int)(rad * sinf(a));
	}
	SDL_RenderDrawLines(r, pts, 73);
}

/* The name a blip goes by: its MECH_CLASS name, else its node. */
static const char *rname(const struct blip *b, char *buf)
{
	const struct rmech *m = &g_rmech[(b->thing & 0xFF) % RMECH_MAX];
	if (m->set && m->name[0]) return m->name;
	snprintf(buf, 12, "POD %u", (unsigned)(b->thing & 0xFF));
	return buf;
}

static int rpaint(const struct blip *b)
{
	const struct rmech *m = &g_rmech[(b->thing & 0xFF) % RMECH_MAX];
	return m->set ? m->paint : 0;
}

static int rlive(const struct blip *b, Uint32 now) { return b->seen && now - b->seen < 3000; }

static float rdist(const struct blip *b, float *brg)
{
	float dx = b->x - g_me.x, dy = b->y - g_me.y;
	if (brg) {
		/* compass bearing: 0 toward -Y, as a heading is */
		float a = atan2f(dx, -dy) * 57.29578f;
		*brg = a < 0 ? a + 360.0f : a;
	}
	return sqrtf(dx * dx + dy * dy);
}

/* Where things go on the screen. Landscape is 640 x 400, the scope at the
 * left and the panel down the right; portrait is 480 x 640, for a screen
 * stood on end as the cockpit's is - the scope across the top and the panel
 * in a band beneath it, readouts left and contacts right. The window's own
 * shape picks one. */
struct rlay {
	int portrait, w, h;
	int cx, cy, rad;		/* the scope */
	SDL_Rect box;			/* the map page's square */
	int tx, ty;			/* the page's title and this Mech's readouts */
	int lx, ly, rows;		/* the contact list */
	int hx, hy;			/* the key hints */
};

static char g_rlayout;			/* [radar] layout: 'p', 'l', or 0 for the window's shape */

static void rlayout(struct rlay *L, int portrait)
{
	memset(L, 0, sizeof *L);
	L->portrait = portrait;
	if (portrait) {
		L->w = 480; L->h = 640;
		L->cx = 240; L->cy = 236; L->rad = 220;
		L->box.x = 6; L->box.y = 6; L->box.w = 468; L->box.h = 452;
		L->tx = 10; L->ty = 476;
		L->lx = 178; L->ly = 476; L->rows = 9;
		L->hx = 480 - 6 - 6 * 20; L->hy = 640 - 34;
	} else {
		L->w = RADAR_W; L->h = RADAR_H;
		L->cx = 200; L->cy = 200; L->rad = 186;
		L->box.x = 6; L->box.y = 6; L->box.w = 390; L->box.h = 390;
		L->tx = 414; L->ty = 12;
		L->lx = 414; L->ly = 100; L->rows = 12;
		L->hx = 414; L->hy = RADAR_H - 34;
	}
}

static void rsidebar(SDL_Renderer *r, Uint32 now, const struct rlay *L)
{
	char s[48], nm[12];
	int i, row = 0;
	static float lx, ly, spd;
	static Uint32 lt;

	/* speed from where this Mech was half a second ago */
	if (g_me.seen && now - lt >= 500) {
		if (lt)
			spd = sqrtf((g_me.x - lx) * (g_me.x - lx) + (g_me.y - ly) * (g_me.y - ly)) /
			      ((now - lt) / 1000.0f) * 3.6f;
		lx = g_me.x; ly = g_me.y; lt = now;
	}
	rcol(r, RG_GRID);
	if (L->portrait) {
		SDL_RenderDrawLine(r, 8, 464, L->w - 8, 464);
		SDL_RenderDrawLine(r, L->lx - 10, 470, L->lx - 10, L->h - 8);
	} else {
		SDL_RenderDrawLine(r, 404, 8, 404, L->h - 8);
		SDL_RenderDrawLine(r, L->lx, L->ly - 8, L->w - 10, L->ly - 8);
	}
	rcol(r, RG_TEXT);
	text(r, L->tx, L->ty, 2, g_rpage == 0 ? "SECTOR" : g_rpage == 1 ? "MAP" : "CONTACTS", 8);
	rcol(r, RG_DIM);
	snprintf(s, sizeof s, "RANGE %d M", RRANGES[g_rrange]);
	text(r, L->tx, L->ty + 28, 1, s, 20);
	if (g_me.seen) {
		snprintf(s, sizeof s, "HEADING %03d", ((int)(g_me.h + 360.5f)) % 360);
		text(r, L->tx, L->ty + 40, 1, s, 20);
		snprintf(s, sizeof s, "SPEED %3d KPH", (int)(spd + 0.5f));
		text(r, L->tx, L->ty + 52, 1, s, 20);
		snprintf(s, sizeof s, "AT %5d %5d", (int)g_me.x, (int)g_me.y);
		text(r, L->tx, L->ty + 64, 1, s, 20);
	}
	for (i = 0; i < RADAR_MAX && row < L->rows; i++) {
		const struct blip *b = &g_blips[i];
		float brg, d;
		if (!b->seen || b->thing == g_me.thing) continue;
		d = rdist(b, &brg);
		if (rlive(b, now)) raccent(r, rpaint(b), 0); else rcol(r, RG_DIM);
		snprintf(s, sizeof s, "%-8.8s %5d %03d", rname(b, nm), (int)d, (int)brg % 360);
		text(r, L->lx, L->ly + row * 12, 1, s, 20);
		if (!rlive(b, now)) text(r, L->lx + 6 * 19, L->ly + row * 12, 1, "-", 1);
		row++;
	}
	if (!row) { rcol(r, RG_DIM); text(r, L->lx, L->ly, 1, "NO CONTACTS", 20); }
	rcol(r, RG_DIM);
	text(r, L->hx, L->hy, 1, "F1 SECTOR  F2 MAP", 20);
	text(r, L->hx, L->hy + 12, 1, "F3 CONTACTS  PGUP/DN", 20);
}

static void rsector(SDL_Renderer *r, Uint32 now, const struct rlay *L)
{
	const int cx = L->cx, cy = L->cy, rad = L->rad;
	float range = (float)RRANGES[g_rrange], h = g_me.h * 3.14159265f / 180.0f;
	struct rview v = { (float)cx, (float)cy, g_me.x, g_me.y, rad / range, sinf(h), cosf(h) };
	SDL_Rect clip = { cx - rad, cy - rad, rad * 2 + 1, rad * 2 + 1 };
	int i, k;
	char s[16], nm[12];

	SDL_RenderSetClipRect(r, &clip);
	rmap_draw(r, &v);
	/* the sweep, a turn every four seconds, fading behind */
	for (k = 0; k < 24; k++) {
		float a = (float)((now % 4000) / 4000.0) * 6.2831853f - k * 0.02f;
		rcol(r, 0, 120 - k * 5, 40 - k);
		SDL_RenderDrawLine(r, cx, cy, cx + (int)(rad * sinf(a)), cy - (int)(rad * cosf(a)));
	}
	for (i = 0; i < RADAR_MAX; i++) {
		const struct blip *b = &g_blips[i];
		if (!rlive(b, now) || b->thing == g_me.thing) continue;
		rmech_draw(r, &v, b, rpaint(b), 0, rname(b, nm));
	}
	SDL_RenderSetClipRect(r, NULL);
	/* black out the square's corners: the scope is round */
	rcol(r, RG_BACK);
	for (k = -rad; k <= rad; k++) {
		int w = (int)sqrtf((float)(rad * rad - k * k));
		SDL_RenderDrawLine(r, cx - rad - 2, cy + k, cx - w - 1, cy + k);
		SDL_RenderDrawLine(r, cx + w + 1, cy + k, cx + rad + 2, cy + k);
	}
	rcol(r, RG_GRID);
	for (k = 1; k <= 4; k++) rcircle(r, cx, cy, rad * k / 4);
	SDL_RenderDrawLine(r, cx - rad, cy, cx + rad, cy);
	SDL_RenderDrawLine(r, cx, cy - rad, cx, cy + rad);
	/* the compass, turning with the heading */
	rcol(r, RG_TEXT);
	for (k = 0; k < 4; k++) {
		static const char *const N[4] = { "N", "E", "S", "W" };
		float a = (k * 90.0f - g_me.h) * 3.14159265f / 180.0f;
		int px = cx + (int)((rad + 8) * sinf(a)) - 3, py = cy - (int)((rad + 8) * cosf(a)) - 3;
		if (px < 1) px = 1;
		if (px > L->w - 7) px = L->w - 7;
		if (py < 1) py = 1;
		text(r, px, py, 1, N[k], 1);
	}
	rcol(r, RG_DIM);
	for (k = 1; k <= 4; k++) {
		snprintf(s, sizeof s, "%d", RRANGES[g_rrange] * k / 4);
		text(r, cx + 3, cy - rad * k / 4 + 2, 1, s, 6);
	}
	rcol(r, RG_TEXT);			/* this Mech */
	SDL_RenderDrawLine(r, cx, cy - 7, cx - 5, cy + 5);
	SDL_RenderDrawLine(r, cx, cy - 7, cx + 5, cy + 5);
	SDL_RenderDrawLine(r, cx - 5, cy + 5, cx + 5, cy + 5);
}

static void rwhole(SDL_Renderer *r, Uint32 now, const struct rlay *L)
{
	float lo[2] = { 1e30f, 1e30f }, hi[2] = { -1e30f, -1e30f }, span, ppm;
	struct rview v;
	int i;
	char nm[12];

	for (i = 0; i < g_nrmap; i++) {
		if (g_rmap[i].x < lo[0]) lo[0] = g_rmap[i].x;
		if (g_rmap[i].x > hi[0]) hi[0] = g_rmap[i].x;
		if (g_rmap[i].y < lo[1]) lo[1] = g_rmap[i].y;
		if (g_rmap[i].y > hi[1]) hi[1] = g_rmap[i].y;
	}
	for (i = 0; i < RADAR_MAX; i++) {
		const struct blip *b = &g_blips[i];
		if (!rlive(b, now)) continue;
		if (b->x < lo[0]) lo[0] = b->x;
		if (b->x > hi[0]) hi[0] = b->x;
		if (b->y < lo[1]) lo[1] = b->y;
		if (b->y > hi[1]) hi[1] = b->y;
	}
	if (g_me.seen) {
		if (g_me.x < lo[0]) lo[0] = g_me.x;
		if (g_me.x > hi[0]) hi[0] = g_me.x;
		if (g_me.y < lo[1]) lo[1] = g_me.y;
		if (g_me.y > hi[1]) hi[1] = g_me.y;
	}
	if (lo[0] > hi[0]) return;
	span = fmaxf(hi[0] - lo[0], hi[1] - lo[1]) + 200.0f;
	ppm = (float)(L->box.w < L->box.h ? L->box.w : L->box.h) / span;
	/* north up: -Y at the top */
	v.cx = L->box.x + L->box.w / 2.0f; v.cy = L->box.y + L->box.h / 2.0f;
	v.wx = (lo[0] + hi[0]) / 2; v.wy = (lo[1] + hi[1]) / 2;
	v.ppm = ppm; v.s = 0; v.c = 1;
	SDL_RenderSetClipRect(r, &L->box);
	rcol(r, RG_GRID);
	SDL_RenderDrawRect(r, &L->box);
	rmap_draw(r, &v);
	if (g_me.seen) {
		float sx, sy;
		rproj(&v, g_me.x, g_me.y, &sx, &sy);
		rcol(r, RG_GRID);
		rcircle(r, (int)sx, (int)sy, (int)(RRANGES[g_rrange] * ppm));
	}
	for (i = 0; i < RADAR_MAX; i++) {
		const struct blip *b = &g_blips[i];
		if (!rlive(b, now) || b->thing == g_me.thing) continue;
		rmech_draw(r, &v, b, rpaint(b), 0, rname(b, nm));
	}
	if (g_me.seen) rmech_draw(r, &v, &g_me, 0, 1, "YOU");
	SDL_RenderSetClipRect(r, NULL);
	rcol(r, RG_TEXT);
	text(r, (int)v.cx - 3, L->box.y + 4, 1, "N", 1);
}

static void rcontacts(SDL_Renderer *r, Uint32 now, const struct rlay *L)
{
	char s[64], nm[12], veh[24];
	int i, row = 0;

	rcol(r, RG_TEXT);
	text(r, 14, 14, 1, "PILOT    MECH            RANGE BRG  ", 40);
	rcol(r, RG_GRID);
	SDL_RenderDrawLine(r, 14, 26, L->box.x + L->box.w - 6, 26);
	for (i = 0; i < RADAR_MAX && row < 26; i++) {
		const struct blip *b = &g_blips[i];
		const struct rmech *m = &g_rmech[(b->thing & 0xFF) % RMECH_MAX];
		float brg, d;
		if (!b->seen || b->thing == g_me.thing) continue;
		d = rdist(b, &brg);
		rvehicle(m->set ? m->vehicle : -1, veh, sizeof veh);
		if (rlive(b, now)) raccent(r, rpaint(b), 0); else rcol(r, RG_DIM);
		snprintf(s, sizeof s, "%-8.8s %-15.15s %5d %03d %s", rname(b, nm), veh, (int)d, (int)brg % 360,
			 rlive(b, now) ? "" : "LOST");
		text(r, 14, 34 + row * 13, 1, s, 44);
		row++;
	}
	if (!row) { rcol(r, RG_DIM); text(r, 14, 34, 1, "NO CONTACTS", 20); }
}

static void radar_open(void)
{
	int x = SDL_WINDOWPOS_UNDEFINED, y = SDL_WINDOWPOS_UNDEFINED, w = RADAR_W * 3 / 2, h = RADAR_H * 3 / 2;
	Uint32 fl = SDL_WINDOW_RESIZABLE;
	char title[96];

	if (!ini_int("radar", "show", 0)) return;
	ini_rect("radar", &x, &y, &w, &h);
	ini_str("radar", "title", "BattleTech 3.0 - radar", title, sizeof title);
	if (ini_int("radar", "fullscreen", 0)) {
		if (ini_int("radar", "x", -99999) == -99999) {
			int m = ini_int("radar", "monitor", 2) - 1;
			if (m < 0 || m >= SDL_GetNumVideoDisplays()) m = 0;
			x = y = SDL_WINDOWPOS_CENTERED_DISPLAY(m);
		}
		fl = SDL_WINDOW_FULLSCREEN_DESKTOP;
	}
	{
		char lay[16];
		ini_str("radar", "layout", "", lay, sizeof lay);
		g_rlayout = lay[0] == 'p' || lay[0] == 'P' ? 'p' : lay[0] == 'l' || lay[0] == 'L' ? 'l' : 0;
	}
	g_rpage = ini_int("radar", "page", 0) % 3;
	g_rrange = ini_int("radar", "range", 1);
	if (g_rrange < 0 || g_rrange >= (int)(sizeof RRANGES / sizeof *RRANGES)) g_rrange = 1;
	g_rw = SDL_CreateWindow(title, x, y, w, h, fl);
	if (!g_rw) return;
	g_rr = SDL_CreateRenderer(g_rw, -1, SDL_RENDERER_ACCELERATED);
	if (!g_rr) g_rr = SDL_CreateRenderer(g_rw, -1, 0);
}

/* Its keys: the pod's display buttons pick the page as they do on the pod;
 * Tab steps through them; PgUp/PgDn the range. */
static void radar_key(SDL_Keycode k, int down)
{
	int n = (int)(sizeof RRANGES / sizeof *RRANGES);
	if (!down) return;
	if (k == SDLK_F1) g_rpage = 0;
	else if (k == SDLK_F2) g_rpage = 1;
	else if (k == SDLK_F3) g_rpage = 2;
	else if (k == SDLK_TAB) g_rpage = (g_rpage + 1) % 3;
	else if (k == SDLK_PAGEUP && g_rrange > 0) g_rrange--;
	else if (k == SDLK_PAGEDOWN && g_rrange < n - 1) g_rrange++;
}

static void radar_draw(void)
{
	static Uint32 at;
	static int shape = -1;
	Uint32 now = SDL_GetTicks();
	struct rlay L;
	int w, h, portrait;

	if (!g_rr || now - at < 50) return;	/* twenty times a second */
	at = now;
	/* [radar] layout = portrait or landscape; else the window's shape. */
	SDL_GetWindowSize(g_rw, &w, &h);
	portrait = g_rlayout == 'p' ? 1 : g_rlayout == 'l' ? 0 : h > w;
	rlayout(&L, portrait);
	if (portrait != shape) {
		SDL_RenderSetLogicalSize(g_rr, L.w, L.h);
		shape = portrait;
	}
	rcol(g_rr, RG_BACK);
	SDL_RenderClear(g_rr);
	if (g_rpage == 2) rcontacts(g_rr, now, &L);
	else if (!g_me.seen) {
		rcol(g_rr, RG_DIM);
		text(g_rr, L.cx - 66, L.cy - 4, 2, "NO POSITION", 12);
	} else if (g_rpage == 1) rwhole(g_rr, now, &L);
	else rsector(g_rr, now, &L);
	rsidebar(g_rr, now, &L);
	SDL_RenderPresent(g_rr);
}

static void radar_close(void)
{
	if (g_rr) SDL_DestroyRenderer(g_rr);
	if (g_rw) SDL_DestroyWindow(g_rw);
	g_rr = NULL;
	g_rw = NULL;
}

#endif /* BATTLEPOD_RADAR_H */
