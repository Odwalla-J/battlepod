/* Weapon effects: a shot you can see, for every weapon fired.
 *
 * NOT the pod's. A 3.0 display list carries no beam, tracer or bolt: a shot
 * is the firing arm's muzzle flash (its model draws it while var 3's firing
 * bits are set), the missile in flight (model 113), the explosion where it
 * lands (80) and the victim's one-frame flash. Played today, that reads as
 * weapons doing nothing, so this layer adds what a pilot needs to see, from
 * what the frame does carry:
 *
 *   when    a Mech's var 3 firing bits rising (0x30 its right arm, 0xC0 its
 *           left), or - for this cockpit's own Mech - a weapon's recycle bar
 *           on the panel dropping, which also names the weapon
 *   colour  the arm's own flash colours, as its model picks them: blue and
 *           cyan for one bit, orange and white for the other
 *   from    the arm's muzzle, placed by the Mech's own 3x3
 *   to      the Mech nearest its line of fire, else straight on to 500 m
 *
 * Lasers are beams, held a moment; PPCs bolts; cannon, machine guns and
 * Gauss tracers. Missiles leave a fading smoke trail. battletech.ini [view]
 * effects = 0 turns the layer off.
 */
#ifndef BATTLEPOD_FX_H
#define BATTLEPOD_FX_H

#define FX_MAX 96
#define FX_BEAM   0
#define FX_BOLT   1
#define FX_TRACER 2
#define FX_SMOKE  3

static int g_fx_on = 1;
static int g_fx_panel;			/* the cockpit reports its own shots from the panel */

static struct fx {
	int kind, live;
	uint32_t t0;			/* pod clock, hundredths */
	int life;			/* hundredths */
	float a[3], b[3];		/* world (X, height, Z) */
	uint8_t core[3], glow[3];
} g_fx[FX_MAX];

/* Own shots the panel saw, waiting for a frame to aim them. */
static struct { int kind, side; uint32_t t; } g_fx_own[16];
static int g_fx_nown;

static void fx_add(int kind, uint32_t t0, int life, const float *a, const float *b,
		   const uint8_t *core, const uint8_t *glow)
{
	int i, at = 0;
	uint32_t oldest = 0xFFFFFFFFu;

	for (i = 0; i < FX_MAX; i++) {
		if (!g_fx[i].live) { at = i; break; }
		if (g_fx[i].t0 < oldest) { oldest = g_fx[i].t0; at = i; }
	}
	g_fx[at].kind = kind; g_fx[at].live = 1; g_fx[at].t0 = t0; g_fx[at].life = life;
	memcpy(g_fx[at].a, a, sizeof g_fx[at].a);
	memcpy(g_fx[at].b, b, sizeof g_fx[at].b);
	memcpy(g_fx[at].core, core, 3);
	memcpy(g_fx[at].glow, glow, 3);
}

/* A weapon's kind from its name on the panel: "E LAS MD", "PPC", "AFC 2520",
 * "LRM 1524", "GAUSS", "MG". Missiles are left to their own model. */
static int fx_kind_of(const char *name)
{
	if (strstr(name, "LAS")) return FX_BEAM;
	if (strstr(name, "PPC")) return FX_BOLT;
	if (strstr(name, "LRM") || strstr(name, "SRM")) return -1;
	return FX_TRACER;
}

/* The panel saw weapon slot k fire. */
static void fx_own_fire(int k, const char *name, uint32_t now)
{
	int kind = fx_kind_of(name);
	if (kind < 0 || g_fx_nown >= 16) return;
	g_fx_own[g_fx_nown].kind = kind;
	g_fx_own[g_fx_nown].side = k & 1;
	g_fx_own[g_fx_nown].t = now;
	g_fx_nown++;
}

/* Model space to world through a record's 3x3 (rows: its axes) and place. */
static void fx_place(const float *rot, const float *at, float mx, float my, float mz, float *o)
{
	int c;
	for (c = 0; c < 3; c++)
		o[c] = at[c] + mx * rot[c] + my * rot[3 + c] + mz * rot[6 + c];
}

/* Where a shot from `from` along `dir` ends: the centre of the Mech nearest
 * its line within `cone` (a cosine), else 500 m on. */
static void fx_aim(const struct pod_frame *fr, int self, const float *from, const float *dir,
		   float cone, float *to)
{
	float best = cone;
	int i, c;

	for (c = 0; c < 3; c++) to[c] = from[c] + dir[c] * 500.0f;
	/* The viewer's own Mech is not in its own list: a shot at the eye's
	 * Mech aims at the eye, a little under it, unless it is the eye's own. */
	{
		float v[3] = { fr->cam[9] - from[0], fr->cam[10] - 1.5f - from[1], fr->cam[11] - from[2] }, d, cs;
		d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (d > 5.0f && d < 1200.0f) {
			cs = (v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2]) / d;
			if (cs > best) {
				best = cs;
				to[0] = fr->cam[9]; to[1] = fr->cam[10] - 1.5f; to[2] = fr->cam[11];
			}
		}
	}
	for (i = 0; i < fr->n; i++) {
		float v[3], d, cs;
		uint32_t id = fr->obj[i].model;
		if (i == self || id < 451 || id > 456) continue;
		v[0] = fr->obj[i].at[0] - from[0];
		v[1] = fr->obj[i].at[1] + 1.5f - from[1];
		v[2] = fr->obj[i].at[2] - from[2];
		d = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (d < 5.0f || d > 1200.0f) continue;
		cs = (v[0] * dir[0] + v[1] * dir[1] + v[2] * dir[2]) / d;
		if (cs > best) {
			best = cs;
			to[0] = fr->obj[i].at[0]; to[1] = fr->obj[i].at[1] + 1.5f; to[2] = fr->obj[i].at[2];
		}
	}
}

static const uint8_t FX_BLUE[3] = { 0, 40, 255 }, FX_CYAN[3] = { 80, 232, 255 };
static const uint8_t FX_ORANGE[3] = { 255, 204, 102 }, FX_WHITE[3] = { 255, 255, 255 };
static const uint8_t FX_PPC[3] = { 120, 160, 255 }, FX_RED[3] = { 255, 40, 30 }, FX_PINK[3] = { 255, 190, 170 };

static void fx_shot(int kind, uint32_t t, const float *from, const float *to, int lasercolour)
{
	if (kind == FX_BEAM) {
		/* the pod's blue/cyan for a beam from an arm, red for this
		 * cockpit's own lasers, which the panel names */
		if (lasercolour) fx_add(FX_BEAM, t, 30, from, to, FX_CYAN, FX_BLUE);
		else fx_add(FX_BEAM, t, 30, from, to, FX_PINK, FX_RED);
	} else if (kind == FX_BOLT) {
		fx_add(FX_BOLT, t, 250, from, to, FX_WHITE, FX_PPC);
	} else {
		int k;
		for (k = 0; k < 3; k++)		/* a short burst */
			fx_add(FX_TRACER, t + (uint32_t)(k * 6), 150, from, to, FX_WHITE, FX_ORANGE);
	}
}

/* What this frame says was fired: var 3 rising on any Mech, the panel's own
 * shots aimed from this cockpit's Mech, and every missile's position for
 * its trail. Once per list. */
static void fx_note(const struct pod_frame *fr)
{
	static uint32_t last_v3[256];
	static uint8_t seen[256];
	static struct { uint32_t ent; float p[3]; uint32_t t; } trail[64];
	uint32_t now = fr->clock;
	float eye[3] = { fr->cam[9], fr->cam[10], fr->cam[11] };
	float fwd[3] = { fr->cam[6], fr->cam[7], fr->cam[8] };
	int i, k, own = -1;

	for (i = 0; i < fr->n; i++) {
		uint32_t id = fr->obj[i].model, ent = fr->obj[i].entity & 255, v3, rise;
		float dx = fr->obj[i].at[0] - eye[0], dz = fr->obj[i].at[2] - eye[2];
		if (id == 113) {
			/* a missile: smoke from where it was a list ago */
			int slot = -1, free_at = -1;
			for (k = 0; k < 64; k++) {
				if (trail[k].ent == fr->obj[i].entity && trail[k].t) { slot = k; break; }
				if (free_at < 0 && (!trail[k].t || now - trail[k].t > 100)) free_at = k;
			}
			if (slot >= 0) {
				/* an id the pod has handed a new missile joins nothing */
				static const uint8_t SMOKE[3] = { 200, 196, 186 }, SMOKE2[3] = { 120, 118, 112 };
				float mx = fr->obj[i].at[0] - trail[slot].p[0], my = fr->obj[i].at[1] - trail[slot].p[1];
				float mz = fr->obj[i].at[2] - trail[slot].p[2];
				/* nor does one still at the eye: cut at the camera, it
				 * would cross the whole screen */
				float ex = trail[slot].p[0] - eye[0], ey = trail[slot].p[1] - eye[1], ez = trail[slot].p[2] - eye[2];
				if (now - trail[slot].t <= 10 && mx * mx + my * my + mz * mz < 80.0f * 80.0f &&
				    ex * ex + ey * ey + ez * ez > 15.0f * 15.0f)
					fx_add(FX_SMOKE, now, 90, trail[slot].p, fr->obj[i].at, SMOKE, SMOKE2);
			} else slot = free_at;
			if (slot >= 0) {
				trail[slot].ent = fr->obj[i].entity;
				memcpy(trail[slot].p, fr->obj[i].at, sizeof trail[slot].p);
				trail[slot].t = now;
			}
			continue;
		}
		if (id < 451 || id > 456 || fr->obj[i].nvar < 4) continue;
		if (dx * dx + dz * dz < 1.0f) own = i;
		v3 = fr->obj[i].var[3];
		rise = seen[ent] ? v3 & ~last_v3[ent] : 0;
		last_v3[ent] = v3;
		seen[ent] = 1;
		if (i == own && g_fx_panel) continue;	/* the panel says it better */
		for (k = 0; k < 2; k++) {
			uint32_t any = k ? 0xC0 : 0x30, orange = k ? 0x40 : 0x10;
			float from[3], dir[3], to[3];
			if (!(rise & any)) continue;
			fx_place(fr->obj[i].rot, fr->obj[i].at, k ? -1.4f : 1.4f, 1.2f, 4.5f, from);
			if (i == own) memcpy(dir, fwd, sizeof dir);
			else memcpy(dir, fr->obj[i].rot + 6, sizeof dir);
			fx_aim(fr, i, from, dir, 0.94f, to);
			fx_shot((v3 & orange) ? FX_TRACER : FX_BEAM, now, from, to, 1);
		}
	}
	/* this cockpit's own, from the panel: from beside and below the eye
	 * toward what the reticle is on */
	for (k = 0; k < g_fx_nown; k++) {
		float from[3], to[3], rgt[3] = { fr->cam[0], fr->cam[1], fr->cam[2] };
		float side = g_fx_own[k].side ? -1.4f : 1.4f;
		int c;
		for (c = 0; c < 3; c++) from[c] = eye[c] + rgt[c] * side - (c == 1 ? 1.6f : 0.0f) + fwd[c] * 2.0f;
		fx_aim(fr, own, eye, fwd, 0.995f, to);
		fx_shot(g_fx_own[k].kind, now, from, to, 0);
	}
	g_fx_nown = 0;
}

/* A line in the world, `w` pixels wide, through the depth buffer; `dither`
 * draws every other pixel - the fading half of a trail. */
static void fx_line(struct raster *f, const struct ras_cam *cam, const float *a, const float *b,
		    int w, const uint8_t *c, int dither)
{
	float pa[3], pb[3], sa[3], sb[3];
	int n, s, ox, oy;

	ras_view(cam, a[0], a[1], a[2], pa);
	ras_view(cam, b[0], b[1], b[2], pb);
	if (pa[2] < RAS_NEAR && pb[2] < RAS_NEAR) return;
	if (pa[2] < RAS_NEAR || pb[2] < RAS_NEAR) {
		float *p = pa[2] < RAS_NEAR ? pa : pb, *q = p == pa ? pb : pa;
		float t = (RAS_NEAR - p[2]) / (q[2] - p[2]);
		p[0] += (q[0] - p[0]) * t; p[1] += (q[1] - p[1]) * t; p[2] = RAS_NEAR;
	}
	if (!ras_project(pa, sa) || !ras_project(pb, sb)) return;
	n = (int)fmaxf(fabsf(sb[0] - sa[0]), fabsf(sb[1] - sa[1]));
	if (n > 2 * RAS_W) n = 2 * RAS_W;
	for (s = 0; s <= n; s++) {
		float t = n ? (float)s / n : 0;
		int x = (int)(sa[0] + (sb[0] - sa[0]) * t), y = (int)(sa[1] + (sb[1] - sa[1]) * t);
		float z = sa[2] + (sb[2] - sa[2]) * t - 0.5f;
		for (oy = -(w / 2); oy <= (w - 1) / 2; oy++)
			for (ox = -(w / 2); ox <= (w - 1) / 2; ox++)
				if (!dither || ((x + ox + y + oy) & 1))
					ras_dot(f, x + ox, y + oy, z, c);
	}
}

static void shots_draw(struct raster *f, const struct ras_cam *cam, uint32_t now)
{
	int i, c;

	for (i = 0; i < FX_MAX; i++) {
		struct fx *e = &g_fx[i];
		int age;
		if (!e->live) continue;
		if (now < e->t0) continue;		/* a burst's later rounds */
		age = (int)(now - e->t0);
		if (age > e->life) { e->live = 0; continue; }
		if (e->kind == FX_BEAM) {
			/* full for a fifth of a second, then a thin core */
			if (age < 20) fx_line(f, cam, e->a, e->b, 4, e->glow, 0);
			fx_line(f, cam, e->a, e->b, age < 20 ? 2 : 1, age < 20 ? e->core : e->glow, 0);
		} else if (e->kind == FX_SMOKE) {
			fx_line(f, cam, e->a, e->b, age < 40 ? 3 : 2, age < 40 ? e->core : e->glow, age >= 40);
		} else {
			/* a bolt or a tracer: a bright stretch flying down the line */
			float len, d[3], speed = e->kind == FX_BOLT ? 250.0f : 450.0f;	/* slow enough to see */
			float seg = e->kind == FX_BOLT ? 18.0f : 7.0f, s0, s1, p0[3], p1[3];
			for (c = 0; c < 3; c++) d[c] = e->b[c] - e->a[c];
			len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			if (len < 1.0f) { e->live = 0; continue; }
			s1 = speed * age / 100.0f;
			s0 = s1 - seg;
			if (s0 > len) { e->live = 0; continue; }
			if (s0 < 0) s0 = 0;
			if (s1 > len) s1 = len;
			for (c = 0; c < 3; c++) {
				p0[c] = e->a[c] + d[c] * s0 / len;
				p1[c] = e->a[c] + d[c] * s1 / len;
			}
			fx_line(f, cam, p0, p1, e->kind == FX_BOLT ? 4 : 2, e->glow, 0);
			fx_line(f, cam, p0, p1, e->kind == FX_BOLT ? 2 : 1, e->core, 0);
		}
	}
}

#endif /* BATTLEPOD_FX_H */
