/* Flat-shaded, z-buffered triangles at the pod's own 480x360.
 *
 * `tools/render.py` ported to C so the picture can be made inside the
 * emulator's process. Same camera, same shading, same sky and ground and hard
 * cast shadow that the period footage shows - and the same 480x360 the display
 * list says the pod ran.
 */
#ifndef BATTLEPOD_RASTER_H
#define BATTLEPOD_RASTER_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "mesh.h"

#define RAS_W 480
#define RAS_H 360

struct raster {
	uint8_t px[RAS_W * RAS_H * 3];
	float z[RAS_W * RAS_H];
	/* Paint without the depth buffer, in order: what lies flat on the
	 * ground (shadows, pads, line-work) cannot hide anything standing on
	 * it, and drawn first it is covered by whatever stands in front. */
	int flat;
	/* Polygon colours to the screen through 1/gamma (0: straight). The
	 * archive's ground models give black two materials, 0.0 and 0.0125, which
	 * a straight scale to 5 bits a channel would make the same colour; a
	 * display curve keeps them apart and lifts roads and arena floors from
	 * black to the dark greys they read as. An inference (UNRESOLVED.md). */
	float gamma;
	/* The scene's light on lit surfaces, per channel (tinted, else 1), and
	 * its ground colour (have_ground, else RAS_GROUND) - both the pod's,
	 * from its display list. */
	int tinted, have_ground;
	float tint[3];
	uint8_t ground[3];
	/* Light faces by where they face in the world, the sun fixed and the
	 * spotlights where the pod put them, rather than relative to the eye
	 * as the model previews do. */
	int world_light;
	const struct ras_spot *spot;
	int nspot;
};

/* A spotlight, the pod's type 5 record: a Mech's searchlight. Full within
 * `r_in` metres and `cos_in` of its axis, gone by `r_out` and `cos_out`. */
struct ras_spot {
	float pos[3], dir[3], col[3];
	float r_in, r_out, cos_in, cos_out;
};

/* The shade a colour reaches the screen as, 0-255. */
static uint8_t ras_level(const struct raster *f, float v)
{
	if (v <= 0) return 0;
	if (v >= 1) return 255;
	if (f && f->gamma > 0) v = powf(v, 1.0f / f->gamma);
	return (uint8_t)(v * 255.0f);
}

/* Nothing nearer than this to the eye is drawn; a polygon crossing it is cut
 * there rather than dropped, or the ground under a Mech vanishes whenever one
 * of its corners is behind the camera. */
#define RAS_NEAR 2.0f

/* The palette the footage shows: a graded sky, sand, and a hard dark shadow. */
static const uint8_t RAS_SKY_TOP[3] = { 0x2E, 0x5C, 0xA8 };
static const uint8_t RAS_SKY_LOW[3] = { 0xBF, 0xCF, 0xE2 };
static const uint8_t RAS_GROUND[3]  = { 0xC9, 0xA9, 0x6E };
static const uint8_t RAS_SHADOW[3]  = { 0x6B, 0x55, 0x36 };
static const float RAS_LIGHT[3] = { -0.35f, 0.80f, -0.49f };

static void ras_background(struct raster *f, int horizon)
{
	const uint8_t *ground = f->have_ground ? f->ground : RAS_GROUND;
	int y, x, i;

	for (y = 0; y < RAS_H; y++) {
		uint8_t c[3];
		if (y < horizon) {
			float t = (float)y / (horizon > 0 ? horizon : 1);
			for (i = 0; i < 3; i++)
				c[i] = (uint8_t)(RAS_SKY_TOP[i] + (RAS_SKY_LOW[i] - RAS_SKY_TOP[i]) * t);
		} else {
			/* the ground fades toward the horizon - distance haze */
			float t = (float)(y - horizon) / (RAS_H - horizon > 0 ? RAS_H - horizon : 1);
			if (t * 3.0f < 1.0f) t = t * 3.0f; else t = 1.0f;
			for (i = 0; i < 3; i++)
				c[i] = (uint8_t)(RAS_SKY_LOW[i] + (ground[i] - RAS_SKY_LOW[i]) * t);
		}
		for (x = 0; x < RAS_W; x++)
			memcpy(&f->px[(y * RAS_W + x) * 3], c, 3);
	}
	for (i = 0; i < RAS_W * RAS_H; i++) f->z[i] = 1e30f;
}

static void ras_triangle(struct raster *f, const float *p0, const float *p1,
			 const float *p2, const uint8_t *c)
{
	float area;
	int lo_x, hi_x, lo_y, hi_y, x, y;

	lo_x = (int)floorf(fminf(p0[0], fminf(p1[0], p2[0])));
	hi_x = (int)ceilf(fmaxf(p0[0], fmaxf(p1[0], p2[0])));
	lo_y = (int)floorf(fminf(p0[1], fminf(p1[1], p2[1])));
	hi_y = (int)ceilf(fmaxf(p0[1], fmaxf(p1[1], p2[1])));
	if (lo_x < 0) lo_x = 0;
	if (lo_y < 0) lo_y = 0;
	if (hi_x > RAS_W - 1) hi_x = RAS_W - 1;
	if (hi_y > RAS_H - 1) hi_y = RAS_H - 1;
	if (lo_x > hi_x || lo_y > hi_y) return;

	area = (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (p1[1] - p0[1]);
	if (fabsf(area) < 1e-9f) return;

	for (y = lo_y; y <= hi_y; y++) {
		for (x = lo_x; x <= hi_x; x++) {
			float px = x + 0.5f, py = y + 0.5f, w0, w1, z;
			int i;

			w0 = ((p1[0] - p0[0]) * (py - p0[1]) - (px - p0[0]) * (p1[1] - p0[1])) / area;
			w1 = ((px - p0[0]) * (p2[1] - p0[1]) - (p2[0] - p0[0]) * (py - p0[1])) / area;
			if (w0 < 0 || w1 < 0 || w0 + w1 > 1) continue;
			z = p0[2] + w1 * (p1[2] - p0[2]) + w0 * (p2[2] - p0[2]);
			i = y * RAS_W + x;
			if (f->flat) {
				memcpy(&f->px[i * 3], c, 3);
			} else if (z < f->z[i]) {
				f->z[i] = z;
				memcpy(&f->px[i * 3], c, 3);
			}
		}
	}
}

static void ras_shade_lit(uint8_t *out, float r, float g, float b, const float *nrm, int kind, float amb,
			  const struct raster *f, const float *extra);

/* One flat colour per face - lit, unless the material is kind 0, which the
 * archive settles as emissive: every light and marker in it points at a kind 0
 * and not one points at a kind 1. */
/* `amb` is the material's fourth value. Read as how much of its colour a face
 * keeps when it is turned from the light - an inference, not a reading of the
 * TMS34082's code: 0.5 on most lit materials, 1.0 on most emissive ones, and
 * 0 only where the colour is black or a line (UNRESOLVED.md). */
static void ras_shade_amb(uint8_t *out, float r, float g, float b, const float *nrm, int kind, float amb,
			  const struct raster *f)
{
	static const float none[3] = { 0, 0, 0 };
	ras_shade_lit(out, r, g, b, nrm, kind, amb, f, none);
}

static void ras_shade_lit(uint8_t *out, float r, float g, float b, const float *nrm, int kind, float amb,
			  const struct raster *f, const float *extra)
{
	float k = 1.0f, c[3];
	int i;

	if (kind != 0) {
		float d = nrm[0] * RAS_LIGHT[0] + nrm[1] * RAS_LIGHT[1] + nrm[2] * RAS_LIGHT[2];
		if (d < 0) d = 0;
		if (amb <= 0.0f || amb > 1.0f) amb = 0.35f;
		/* Buildings say 0.1, which leaves their shaded sides black - a
		 * face the eye takes for a hole. Keep at least 45%. */
		if (f && f->world_light && amb < 0.45f) amb = 0.45f;
		k = amb + (1.0f - amb) * d;
	}
	c[0] = r; c[1] = g; c[2] = b;
	for (i = 0; i < 3; i++)
		out[i] = ras_level(f, c[i] * (k * (kind != 0 && f && f->tinted ? f->tint[i] : 1.0f) +
					      (kind != 0 ? extra[i] : 0.0f)));
}

struct ras_cam {
	float turn, pitch, dist;
	float centre[3];
};

static void ras_view(const struct ras_cam *cam, float x, float y, float z, float *o)
{
	float ct = cosf(cam->turn), st = sinf(cam->turn);
	float cp = cosf(cam->pitch), sp = sinf(cam->pitch);
	float nx, ny, nz;

	x -= cam->centre[0]; y -= cam->centre[1]; z -= cam->centre[2];
	nx = x * ct + z * st;
	nz = -x * st + z * ct;
	ny = y * cp - nz * sp;
	nz = y * sp + nz * cp;
	o[0] = nx; o[1] = ny; o[2] = nz + cam->dist;
}

/* Perspective. Y is up in the model and down on the screen. */
static int ras_project(const float *p, float *o)
{
	float f;

	if (p[2] <= 0.01f) return 0;
	f = (RAS_W / 2.0f) / tanf(0.95f / 2.0f);
	o[0] = RAS_W / 2.0f + p[0] * f / p[2];
	o[1] = RAS_H / 2.0f - p[1] * f / p[2];
	o[2] = p[2];
	return 1;
}

/* Frame on the vertices actually decoded, not the stated bounding sphere:
 * the sphere has to contain the origin too, so framing on it leaves the model
 * small in the middle of the picture. */
static void ras_frame_on(const struct mesh *m, struct ras_cam *cam, float zoom, float *floor)
{
	float lo[3], hi[3], radius = 1e-3f;
	int i, any = 0, a;

	for (a = 0; a < 3; a++) { lo[a] = 1e30f; hi[a] = -1e30f; }
	for (i = 0; i < m->top; i++) {
		if (!m->vset[i]) continue;
		any = 1;
		if (m->vx[i] < lo[0]) lo[0] = m->vx[i];
		if (m->vx[i] > hi[0]) hi[0] = m->vx[i];
		if (m->vy[i] < lo[1]) lo[1] = m->vy[i];
		if (m->vy[i] > hi[1]) hi[1] = m->vy[i];
		if (m->vz[i] < lo[2]) lo[2] = m->vz[i];
		if (m->vz[i] > hi[2]) hi[2] = m->vz[i];
	}
	if (!any) { lo[0] = lo[1] = lo[2] = -1; hi[0] = hi[1] = hi[2] = 1; }
	for (a = 0; a < 3; a++) {
		cam->centre[a] = (lo[a] + hi[a]) / 2;
		if ((hi[a] - lo[a]) / 2 > radius) radius = (hi[a] - lo[a]) / 2;
	}
	cam->dist = radius * zoom;
	*floor = lo[1];
}

/* Where an object stands in the world: a position, a heading about the
 * vertical, and a scale. A scenario file gives all three per object, so one
 * model resource is drawn many times in one scene. */
struct ras_place {
	float x, y, z;			/* world position; z is height */
	float heading;			/* radians about the vertical */
	float scale;
	float stretch;			/* along the model's z as well; 0 is none */
	/* Or the whole orientation: three rows, the model's x, y and z axes
	 * in the world, as a display list record carries them. Overrides
	 * heading. */
	const float *rot;
};

static const struct ras_place RAS_HERE = { 0, 0, 0, 0, 1.0f };

/* Model space has Y up; the world puts its ground plane in x/y and height in
 * z, which is how the scenario files are written. This is the one place the
 * two conventions meet. */
static void ras_to_world(const struct ras_place *p, float mx, float my, float mz,
			 float *o)
{
	float c = cosf(p->heading), s = sinf(p->heading);

	if (p->stretch != 0.0f) mz *= p->stretch;

	if (p->rot) {
		const float *r = p->rot;
		o[0] = p->x + p->scale * (mx * r[0] + my * r[3] + mz * r[6]);
		o[1] = p->z + p->scale * (mx * r[1] + my * r[4] + mz * r[7]);
		o[2] = p->y + p->scale * (mx * r[2] + my * r[5] + mz * r[8]);
		return;
	}
	o[0] = p->x + p->scale * (mx * c - mz * s);
	o[1] = p->z + p->scale * my;
	o[2] = p->y + p->scale * (mx * s + mz * c);
}

static void ras_dot(struct raster *f, int x, int y, float z, const uint8_t *c)
{
	int i;

	if (x < 0 || y < 0 || x >= RAS_W || y >= RAS_H) return;
	i = y * RAS_W + x;
	if (!f->flat) {
		if (z >= f->z[i]) return;
		f->z[i] = z;
	}
	memcpy(&f->px[i * 3], c, 3);
}

/* The marks: lights are a pixel or a square sized by distance, line-work a
 * filled rectangle between two corners - all in the material's own colour,
 * unlit. */
static void ras_marks(struct raster *f, const struct mesh *m, const struct ras_cam *cam,
		      const struct ras_place *at)
{
	int i;

	for (i = 0; i < m->nmark; i++) {
		const struct mesh_mark *k = &m->mark[i];
		float wa[3], wb[3], a[3], b[3], sa[3], sb[3];
		uint8_t c[3];
		int mi = k->mat < MESH_MATS ? k->mat : 0, s;

		if (k->a >= MESH_VERTS || k->b >= MESH_VERTS || !m->vset[k->a] || !m->vset[k->b]) continue;
		ras_shade_amb(c, m->mset[mi] ? m->mr[mi] : 1.0f, m->mset[mi] ? m->mg[mi] : 1.0f,
			      m->mset[mi] ? m->mb[mi] : 1.0f, NULL, 0, 1.0f, f);
		ras_to_world(at, m->vx[k->a], m->vy[k->a], m->vz[k->a], wa);
		ras_to_world(at, m->vx[k->b], m->vy[k->b], m->vz[k->b], wb);
		ras_view(cam, wa[0], wa[1], wa[2], a);
		ras_view(cam, wb[0], wb[1], wb[2], b);
		if (k->kind != MARK_RECT) {
			int r, x, y;
			if (a[2] < RAS_NEAR || !ras_project(a, sa)) continue;
			r = k->kind == MARK_SPOT ?
			    (int)(k->size * (RAS_W / 2.0f) / tanf(0.95f / 2.0f) / a[2]) : 0;
			if (r > 8) r = 8;
			for (y = -r; y <= r; y++)
				for (x = -r; x <= r; x++)
					ras_dot(f, (int)sa[0] + x, (int)sa[1] + y, a[2] - 0.05f, c);
			continue;
		}
		if (a[2] < RAS_NEAR && b[2] < RAS_NEAR) continue;
		if (a[2] < RAS_NEAR || b[2] < RAS_NEAR) {
			float *p = a[2] < RAS_NEAR ? a : b, *q = p == a ? b : a;
			float t = (RAS_NEAR - p[2]) / (q[2] - p[2]);
			p[0] += (q[0] - p[0]) * t; p[1] += (q[1] - p[1]) * t; p[2] = RAS_NEAR;
		}
		if (!ras_project(a, sa) || !ras_project(b, sb)) continue;
		/* `$220` is RPIX then FILL: the screen rectangle between its two
		 * corners, filled, at the nearer one's depth. */
		{
			int x0 = (int)fminf(sa[0], sb[0]), x1 = (int)fmaxf(sa[0], sb[0]);
			int y0 = (int)fminf(sa[1], sb[1]), y1 = (int)fmaxf(sa[1], sb[1]), x;
			float z = fminf(sa[2], sb[2]) - 0.05f;
			if (x0 < 0) x0 = 0;
			if (y0 < 0) y0 = 0;
			if (x1 >= RAS_W) x1 = RAS_W - 1;
			if (y1 >= RAS_H) y1 = RAS_H - 1;
			for (s = y0; s <= y1; s++)
				for (x = x0; x <= x1; x++)
					ras_dot(f, x, s, z, c);
		}
	}
}

/* What the spotlights add to a face, per channel: each one's colour, times
 * how squarely the face is turned to it, faded across its cone and its
 * range. */
static void ras_spots(const struct raster *f, const float (*wp)[3], int n, const float *nrm, float *extra)
{
	float c[3] = { 0, 0, 0 };
	int i, j;

	for (j = 0; j < n; j++) {
		c[0] += wp[j][0] / n; c[1] += wp[j][1] / n; c[2] += wp[j][2] / n;
	}
	for (i = 0; i < f->nspot; i++) {
		const struct ras_spot *s = &f->spot[i];
		float v[3], dist, cosang, lam, cone, fall;
		for (j = 0; j < 3; j++) v[j] = c[j] - s->pos[j];
		dist = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (dist < 1e-3f || dist > s->r_out) continue;
		for (j = 0; j < 3; j++) v[j] /= dist;
		cosang = v[0] * s->dir[0] + v[1] * s->dir[1] + v[2] * s->dir[2];
		if (cosang <= s->cos_out) continue;
		lam = -(nrm[0] * v[0] + nrm[1] * v[1] + nrm[2] * v[2]);
		if (lam <= 0) continue;
		cone = cosang >= s->cos_in ? 1.0f : (cosang - s->cos_out) / (s->cos_in - s->cos_out);
		fall = dist <= s->r_in ? 1.0f : (s->r_out - dist) / (s->r_out - s->r_in);
		for (j = 0; j < 3; j++) extra[j] += s->col[j] * lam * cone * fall;
	}
}

/* One object into an already-prepared frame. Split out of ras_draw so a scene
 * can put hundreds of them in without a merged mesh the size of a map. */
static int ras_draw_at(struct raster *f, const struct mesh *m, const struct ras_cam *cam,
		       const struct ras_place *at, float floor, int shadow)
{
	int i, k, drawn = 0;

	for (k = shadow ? 0 : 1; k < 2; k++) {
		for (i = 0; i < m->npoly; i++) {
			const struct mesh_poly *p = &m->poly[i];
			float scr[MESH_PIDX + 2][3], cpt[MESH_PIDX][3], cut[MESH_PIDX + 2][3], wp[MESH_PIDX][3];
			uint8_t colour[3];
			int j, ok = 1, t, ncut = 0, behind = 0;

			for (j = 0; j < p->n; j++) {
				int v = p->v[j];
				float w[3];
				if (v >= MESH_VERTS || !m->vset[v]) { ok = 0; break; }
				ras_to_world(at, m->vx[v], m->vy[v], m->vz[v], w);
				memcpy(wp[j], w, sizeof w);
				ras_view(cam, w[0], k == 0 ? floor : w[1], w[2], cpt[j]);
				behind += cpt[j][2] < RAS_NEAR;
			}
			if (!ok || p->n < 3 || behind == p->n) continue;
			/* Cut at the near plane, then project what is left. */
			for (j = 0; j < p->n; j++) {
				const float *a = cpt[j], *b = cpt[(j + 1) % p->n];
				int ain = a[2] >= RAS_NEAR, bin = b[2] >= RAS_NEAR;
				if (ain) memcpy(cut[ncut++], a, sizeof cut[0]);
				if (ain != bin) {
					float s = (RAS_NEAR - a[2]) / (b[2] - a[2]);
					cut[ncut][0] = a[0] + (b[0] - a[0]) * s;
					cut[ncut][1] = a[1] + (b[1] - a[1]) * s;
					cut[ncut][2] = RAS_NEAR;
					ncut++;
				}
			}
			for (j = 0; j < ncut; j++)
				if (!ras_project(cut[j], scr[j])) { ok = 0; break; }
			if (!ok || ncut < 3) continue;

			if (k == 0) {
				memcpy(colour, RAS_SHADOW, 3);
			} else {
				float e1[3], e2[3], nrm[3], len, facing, extra[3] = { 0, 0, 0 };
				/* In the world, or as the eye sees it (cpt, eye at 0). */
				float (*q)[3] = f->world_light ? wp : cpt;
				for (j = 0; j < 3; j++) {
					e1[j] = q[1][j] - q[0][j];
					e2[j] = q[2][j] - q[0][j];
				}
				nrm[0] = e1[1] * e2[2] - e1[2] * e2[1];
				nrm[1] = e1[2] * e2[0] - e1[0] * e2[2];
				nrm[2] = e1[0] * e2[1] - e1[1] * e2[0];
				len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
				if (len < 1e-9f) continue;
				for (j = 0; j < 3; j++) nrm[j] /= len;
				/* Turn it to face the eye: drawn from either side. */
				if (f->world_light)
					facing = nrm[0] * (q[0][0] - cam->centre[0]) + nrm[1] * (q[0][1] - cam->centre[1]) +
						 nrm[2] * (q[0][2] - cam->centre[2]);
				else
					facing = nrm[0] * q[0][0] + nrm[1] * q[0][1] + nrm[2] * q[0][2];
				if (facing > 0)
					for (j = 0; j < 3; j++) nrm[j] = -nrm[j];
				if (f->world_light && f->nspot)
					ras_spots(f, wp, p->n, nrm, extra);
				{
					int mi = p->mat < MESH_MATS ? p->mat : 0;
					float r = m->mset[mi] ? m->mr[mi] : 0.7f;
					float g = m->mset[mi] ? m->mg[mi] : 0.7f;
					float b = m->mset[mi] ? m->mb[mi] : 0.7f;
					ras_shade_lit(colour, r, g, b, nrm, m->mset[mi] ? m->mkind[mi] : 1,
						      m->mset[mi] ? m->ma[mi] : 0.35f, f, extra);
				}
				drawn++;
			}
			for (t = 1; t < ncut - 1; t++)
				ras_triangle(f, scr[0], scr[t], scr[t + 1], colour);
		}
	}
	if (!shadow) ras_marks(f, m, cam, at);
	return drawn;
}

static int ras_draw(struct raster *f, const struct mesh *m, const struct ras_cam *cam,
		    float floor, int shadow)
{
	ras_background(f, (int)(RAS_H * 0.52f));
	return ras_draw_at(f, m, cam, &RAS_HERE, floor, shadow);
}

#endif /* BATTLEPOD_RASTER_H */
