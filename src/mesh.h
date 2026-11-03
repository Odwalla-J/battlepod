/* A cockpit model, decoded in C.
 *
 * A type 1 resource is not a mesh. It is a threaded program of 45 opcodes run
 * by the renderer's own interpreter, and the way to read it is to run it. This
 * is `tools/model.py` ported so the pod can draw its own geometry in the
 * emulator's process instead of shelling out to Python for a picture.
 *
 * The Python stays the reference. Both decoders have to agree model for model
 * on vertices, polygons and materials, and the harness checks that they do -
 * a port that quietly drops an opcode would otherwise look like a rendering
 * bug much later.
 *
 * Where this deliberately differs: `paths="all"` in the Python walks every
 * branch to find everything a model *can* draw, which is right for auditing
 * and wrong for a picture, because a model holding levels of detail behind its
 * branches ends up with all of them stacked in one frame. This walks the
 * fall-through only, which is the single path the Python calls "fall".
 *
 * For a frame, `mesh_eval` (at the end) does better than either: it runs the
 * one path the predicates choose, given the thing's variables and distance,
 * as the renderer did. The walks stay for the checks and the rig assembler.
 */
#ifndef BATTLEPOD_MESH_H
#define BATTLEPOD_MESH_H

#include <stdint.h>
#include <string.h>

#define MESH_HEADER   0x58		/* bytes; the stream starts here */
#define MESH_STREAM   (MESH_HEADER / 4)
#define MESH_VERTS    4096
#define MESH_POLYS    4096
#define MESH_MATS     256
#define MESH_PIDX     16		/* vertices we keep per polygon */
#define MESH_NODES    64

struct mesh_poly {
	int n;
	uint16_t v[MESH_PIDX];
	uint16_t mat;
};

/* What is not a polygon: a pixel ($200), a filled screen rectangle between
 * two vertices ($220) and a sized marker ($280/$2A0) - lights, and the
 * line-work on the ground. RENDERING.md, *Lights*. */
#define MESH_MARKS 1024
#define MARK_POINT 0
#define MARK_RECT  1
#define MARK_SPOT  2

struct mesh_mark {
	uint16_t a, b, mat;
	uint8_t kind;
	float size;
};

struct mesh {
	float vx[MESH_VERTS], vy[MESH_VERTS], vz[MESH_VERTS];
	uint8_t vset[MESH_VERTS];
	int nvert;				/* distinct indices written, as model.py counts */
	int top;				/* highest index written, plus one */
	struct mesh_poly poly[MESH_POLYS];
	int npoly;
	float mr[MESH_MATS], mg[MESH_MATS], mb[MESH_MATS], ma[MESH_MATS];
	uint8_t mkind[MESH_MATS], mset[MESH_MATS];
	struct mesh_mark mark[MESH_MARKS];
	int nmark;
	int nmat;
	int written;				/* every write to a vertex slot, rewrites included */
	int nmat_top;				/* highest material index, plus one */
	float box[7];				/* the model's own stated extent */
	/* $040's node tree. On a skeleton this is the rest pose: each node is a
	 * constant offset from its parent, so running the chain stands the mech
	 * up. See RENDERING.md. */
	float nx[MESH_NODES], ny[MESH_NODES], nz[MESH_NODES];
	int16_t parent[MESH_NODES];
	float ox[MESH_NODES], oy[MESH_NODES], oz[MESH_NODES];
	uint8_t slot[MESH_NODES];		/* its instance transform, one-based */
	uint8_t nset[MESH_NODES];
	int nnode;
	const char *stopped;			/* NULL if it ran to a return */
	uint16_t seen_op[0x600 / 0x20];		/* opcode histogram, for comparing ports */
};

static float mesh_f32(uint32_t v)
{
	float f;

	memcpy(&f, &v, 4);
	return f;
}

/* How far each opcode advances past its own word. Taken from model.py, which
 * settled $040 and $520 by sweeping them against the archive rather than by
 * reading handlers that call out before touching their operands. */
static int mesh_fixed(uint32_t op)
{
	switch (op) {
	case 0x040: return 6;
	case 0x060: return 1;
	case 0x080: return 0;
	case 0x0E0: case 0x100: return 4;
	case 0x120: return 4;
	case 0x180: return 3;
	case 0x1C0: case 0x1E0: return 1;
	case 0x200: return 3;
	case 0x220: return 4;
	case 0x280: case 0x2A0: return 3;
	case 0x2E0: return 2;
	case 0x300: case 0x340: return 2;
	case 0x380: case 0x3A0: return 0;
	case 0x3C0: return 1;
	case 0x3E0: return 2;
	case 0x400: return 0;
	case 0x440: return 1;
	case 0x500: return 0;
	case 0x520: case 0x540: return 5;
	}
	return -1;
}

/* Operand counts inside a predicate program. Anything from $0C0 to $2E0 on a
 * 32 boundary is an operator and takes none. */
static int mesh_pred_len(uint32_t op)
{
	switch (op) {
	case 0x000: return 0;
	case 0x020: case 0x040: case 0x060: case 0x080: case 0x0A0: return 1;
	}
	if (op >= 0x0C0 && op <= 0x2E0 && (op % 0x20) == 0) return 0;
	return -1;
}

static void mesh_put(struct mesh *m, uint32_t i, float x, float y, float z)
{
	if (i >= MESH_VERTS) return;
	m->vx[i] = x; m->vy[i] = y; m->vz[i] = z;
	m->written++;
	if (!m->vset[i]) m->nvert++;
	m->vset[i] = 1;
	if ((int)i + 1 > m->top) m->top = (int)i + 1;
}

/* Skip one predicate program; returns where it ends, past its $000. */
static uint32_t mesh_predicate(const uint32_t *w, uint32_t n, uint32_t at, int *bad)
{
	int guard;

	for (guard = 0; guard < 64; guard++) {
		uint32_t op;
		int len;

		if (at >= n) { *bad = 1; return at; }
		op = w[at++];
		if (op == 0x000) return at;
		len = mesh_pred_len(op);
		if (len < 0) { *bad = 1; return at; }
		at += (uint32_t)len;
	}
	*bad = 1;
	return at;
}

/* A jump or call target: the operand is a bit offset from the resource base. */
static uint32_t mesh_target(uint32_t operand)
{
	return operand / 32;
}

/* Run the stream. Calls are a work list rather than a recursion: `$020` hands
 * its target to the list and carries straight on past its operand, which is
 * what model.py does and what the interpreter's own threading implies. A
 * position already walked is not walked again, so loops terminate.
 */
/* Which way a conditional branch goes.
 *
 * ALL walks both arms, which finds everything a model *can* draw and is what
 * the checks want. It is wrong for a picture: a model that keeps levels of
 * detail behind its branches ends up with all of them stacked in one frame.
 * FALL and TAKE each walk one arm. See mesh_best. */
#define MESH_ALL  0
#define MESH_FALL 1
#define MESH_TAKE 2
/* NEAR is a model seen close up and whole: a test on the distance ($0A0 in
 * the predicate) falls through to the near level of detail, a bare variable
 * test ([$020 v]) is a sub-part's intact value and is taken, and both sides
 * of every face-facing group are drawn. The arms need it: they keep their
 * forearm behind exactly those three kinds of branch. */
#define MESH_NEAR 3

/* What NEAR does with a predicate: 1 to take the jump. */
static int mesh_near_takes(const uint32_t *w, uint32_t at, uint32_t end)
{
	uint32_t i;

	for (i = at; i < end; i++)
		if (w[i] == 0x0A0) return 0;
	return end - at == 3 && w[at] == 0x020;
}

static void mesh_run_mode(struct mesh *m, const uint8_t *data, uint32_t bytes, int mode)
{
	static uint32_t w[16384];
	static uint8_t seen[16384];
	uint32_t work[256];
	int nwork = 0;
	uint32_t n = bytes / 4, i;
	int bad = 0, guard = 0;

	memset(m, 0, sizeof(*m));
	if (n > 16384) n = 16384;
	memset(seen, 0, n);
	for (i = 0; i < n; i++)
		w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
		       ((uint32_t)data[i * 4 + 2] << 8) | data[i * 4 + 3];
	if (n < MESH_STREAM) { m->stopped = "too short"; return; }
	for (i = 0; i < 7; i++) m->box[i] = mesh_f32(w[9 + i]);

	work[nwork++] = MESH_STREAM;
	while (nwork > 0) {
		uint32_t at = work[--nwork];

		while (at < n && guard++ < 400000) {
			uint32_t op;
			int len;

			if (seen[at]) break;
			seen[at] = 1;
			op = w[at++];
			if (op < 0x600 && (op % 0x20) == 0) m->seen_op[op / 0x20]++;

			if (op == 0x000) break;			/* return */

			switch (op) {
			case 0x020:				/* call: queue it, carry on */
				if (nwork < 256) work[nwork++] = mesh_target(w[at]);
				at += 1;
				continue;
			case 0x2C0:				/* jump, always */
				at = mesh_target(w[at]);
				continue;
			case 0x320: case 0x360: {		/* jump on a predicate */
				uint32_t end = mesh_predicate(w, n, at, &bad);
				if (bad || end >= n) { m->stopped = "predicate"; goto done; }
				if (mode == MESH_ALL) {
					if (nwork < 256) work[nwork++] = mesh_target(w[end]);
				} else if (mode == MESH_TAKE ||
					   (mode == MESH_NEAR && mesh_near_takes(w, at, end))) {
					at = mesh_target(w[end]);
					continue;
				}
				at = end + 1;
				continue;
			}
			case 0x420:				/* predicate into a slot */
				at = mesh_predicate(w, n, at, &bad) + 1;
				if (bad) { m->stopped = "predicate"; goto done; }
				continue;
			case 0x300: case 0x340:			/* jump on face facing */
				if (mode == MESH_ALL || mode == MESH_NEAR) {
					if (nwork < 256) work[nwork++] = mesh_target(w[at + 1]);
				} else if (mode == MESH_TAKE) {
					at = mesh_target(w[at + 1]);
					continue;
				}
				at += 2;
				continue;
			case 0x0A0:				/* one vertex */
				if (at + 4 > n) { m->stopped = "short vertex"; goto done; }
				mesh_put(m, w[at], mesh_f32(w[at + 1]), mesh_f32(w[at + 2]),
					 mesh_f32(w[at + 3]));
				at += 4;
				continue;
			case 0x0C0: {				/* a run of vertices */
				uint32_t base = w[at], cnt = w[at + 1], k;
				if (cnt > 4096 || at + 2 + cnt * 3 > n) { m->stopped = "short run"; goto done; }
				for (k = 0; k < cnt; k++) {
					uint32_t o = at + 2 + k * 3;
					mesh_put(m, base + k, mesh_f32(w[o]), mesh_f32(w[o + 1]),
						 mesh_f32(w[o + 2]));
				}
				at += 2 + cnt * 3;
				continue;
			}
			case 0x240: case 0x260: {		/* draw a polygon */
				uint32_t nv = w[at + 1], k;
				if (nv < 3 || nv > 64 || at + 3 + nv > n) { m->stopped = "polygon"; goto done; }
				if (m->npoly < MESH_POLYS) {
					struct mesh_poly *p = &m->poly[m->npoly++];
					p->n = (int)(nv < MESH_PIDX ? nv : MESH_PIDX);
					for (k = 0; k < (uint32_t)p->n; k++)
						p->v[k] = (uint16_t)w[at + 2 + k];
					p->mat = (uint16_t)w[at + 2 + nv];
				}
				at += 3 + nv;
				continue;
			}
			case 0x4C0: case 0x4E0: {		/* define a material */
				uint32_t idx = w[at];
				if (at + 5 > n) { m->stopped = "short material"; goto done; }
				if (idx < MESH_MATS) {
					m->mkind[idx] = (uint8_t)(op == 0x4C0 ? 0 : 1);
					m->mr[idx] = mesh_f32(w[at + 1]);
					m->mg[idx] = mesh_f32(w[at + 2]);
					m->mb[idx] = mesh_f32(w[at + 3]);
					m->ma[idx] = mesh_f32(w[at + 4]);
					if (!m->mset[idx]) m->nmat++;
					m->mset[idx] = 1;
					if ((int)idx + 1 > m->nmat_top) m->nmat_top = (int)idx + 1;
				}
				at += 5;
				continue;
			}
			case 0x160:				/* index, count, 3n floats */
				at += 2 + 3 * w[at + 1];
				continue;
			case 0x1A0:				/* a run of planes */
				at += 2 + 2 * w[at + 1];
				continue;
			case 0x460:				/* draw another model */
				at += 3;
				continue;
			case 0x560: case 0x580:			/* blit bitmaps */
				at += 3 + w[at + 2];
				continue;
			case 0x480:				/* push a sub-part tag */
				at += 4;
				continue;
			case 0x4A0:				/* pop it */
				continue;
			default:
				break;
			}

			if (op == 0x040) {		/* compose a node onto its parent */
				uint32_t nd = w[at], par = w[at + 1];
				if (at + 6 > n) { m->stopped = "short node"; goto done; }
				if (nd < MESH_NODES && par < MESH_NODES) {
					float dx = mesh_f32(w[at + 3]);
					float dy = mesh_f32(w[at + 4]);
					float dz = mesh_f32(w[at + 5]);
					m->parent[nd] = (int16_t)par;
					m->ox[nd] = dx; m->oy[nd] = dy; m->oz[nd] = dz;
					m->slot[nd] = (uint8_t)w[at + 2];
					m->nx[nd] = m->nx[par] + dx;
					m->ny[nd] = m->ny[par] + dy;
					m->nz[nd] = m->nz[par] + dz;
					if (!m->nset[nd]) m->nnode++;
					m->nset[nd] = 1;
				}
				at += 6;
				continue;
			}

			len = mesh_fixed(op);
			if (len < 0) { m->stopped = "unknown opcode"; goto done; }
			at += (uint32_t)len;
		}
	}
done:
	return;
}

static void mesh_run(struct mesh *m, const uint8_t *data, uint32_t bytes)
{
	mesh_run_mode(m, data, bytes, MESH_FALL);
}

/* The walk that draws the most of a model without drawing it twice.
 *
 * A model that writes each vertex slot once is a sequence, not a choice, so
 * the full walk is right. One that writes slots again on each arm is holding
 * levels of detail, and the full walk would stack them - so take whichever
 * single arm drew more. This is tools/render.py's best_model, and the two have
 * to agree.
 */
static void mesh_best(struct mesh *m, const uint8_t *data, uint32_t bytes)
{
	static struct mesh other;

	mesh_run_mode(m, data, bytes, MESH_ALL);
	if (m->written <= (m->nvert > 0 ? m->nvert : 1) * 6 / 5)
		return;
	mesh_run_mode(m, data, bytes, MESH_FALL);
	mesh_run_mode(&other, data, bytes, MESH_TAKE);
	if (other.npoly > m->npoly)
		memcpy(m, &other, sizeof(*m));
}

/* Copy `part` into `out`, turned by `r` (a 3x3, rows; NULL for none) and
 * moved to `at`, its vertex slots and materials after everything already
 * there so the two cannot collide. */
static void mesh_merge(struct mesh *out, const struct mesh *part, const float *r, const float *at)
{
	int vbase = out->top, mbase = out->nmat_top, i;

	for (i = 0; i < part->top; i++) {
		int d = vbase + i;
		float x = part->vx[i], y = part->vy[i], z = part->vz[i];
		if (!part->vset[i] || d >= MESH_VERTS) continue;
		if (r) {
			float rx = r[0] * x + r[1] * y + r[2] * z;
			float ry = r[3] * x + r[4] * y + r[5] * z;
			z = r[6] * x + r[7] * y + r[8] * z;
			x = rx; y = ry;
		}
		out->vx[d] = x + at[0];
		out->vy[d] = y + at[1];
		out->vz[d] = z + at[2];
		if (!out->vset[d]) out->nvert++;
		out->vset[d] = 1;
		if (d + 1 > out->top) out->top = d + 1;
	}
	for (i = 0; i < part->nmat_top && i < MESH_MATS; i++) {
		int d = mbase + i;
		if (!part->mset[i] || d >= MESH_MATS) continue;
		out->mr[d] = part->mr[i];
		out->mg[d] = part->mg[i];
		out->mb[d] = part->mb[i];
		out->ma[d] = part->ma[i];
		out->mkind[d] = part->mkind[i];
		if (!out->mset[d]) out->nmat++;
		out->mset[d] = 1;
		if (d + 1 > out->nmat_top) out->nmat_top = d + 1;
	}
	for (i = 0; i < part->npoly; i++) {
		const struct mesh_poly *p = &part->poly[i];
		struct mesh_poly *q;
		int j, ok = 1;

		if (out->npoly >= MESH_POLYS) break;
		q = &out->poly[out->npoly];
		q->n = p->n;
		for (j = 0; j < p->n; j++) {
			int v = vbase + p->v[j];
			if (v >= MESH_VERTS || !out->vset[v]) { ok = 0; break; }
			q->v[j] = (uint16_t)v;
		}
		if (!ok) continue;
		q->mat = (uint16_t)(mbase + p->mat);
		out->npoly++;
	}
	for (i = 0; i < part->nmark && out->nmark < MESH_MARKS; i++) {
		struct mesh_mark k = part->mark[i];
		if (vbase + k.a >= MESH_VERTS || vbase + k.b >= MESH_VERTS) continue;
		k.a = (uint16_t)(vbase + k.a);
		k.b = (uint16_t)(vbase + k.b);
		k.mat = (uint16_t)(mbase + k.mat);
		out->mark[out->nmark++] = k;
	}
	/* Leave a gap so the next part's slots cannot collide with this one's. */
	if (out->top < vbase + part->top) out->top = vbase + part->top;
	if (out->nmat_top < mbase + MESH_MATS && mbase + part->nmat_top > out->nmat_top)
		out->nmat_top = mbase + part->nmat_top;
}

/* ------------------------------------------------------------ evaluated */

/* What a model is drawn for: the thing's own variables (its display-list
 * record from word 33), how far it is from the eye, and where to find the
 * models `$460` names. The walks above choose a branch without knowing any
 * of that; the renderer knew all three, and its models lean on them for
 * everything - a Mech's paint scheme is var 1, a building's intact state is
 * var 1 too, every level of detail is a distance, and the haze on a distant
 * hill is a material `$520` blends by distance. */
#define MESH_VARMAX 64

struct mesh_env {
	const uint32_t *var;
	int nvar;
	float dist;
	const uint8_t *(*find)(uint32_t id, uint32_t *bytes);	/* NULL: no sub-models */
};

/* A predicate program, run. Postfix on a small stack: $020 v pushes the
 * thing's var v, $040 c a constant, $080 and $0A0 the distance; $0C0 pushes a
 * global the renderer keeps (taken as 0 - its four uses are blinking lights).
 * The operators the archive uses are $160 and, $200 equal, $220 greater,
 * $260 less and $2A0 not; tools/model.py tallies them. */
static int mesh_eval_pred(const uint32_t *w, uint32_t n, uint32_t at, const struct mesh_env *env)
{
	int64_t st[16];
	int sp = 0, guard;

	for (guard = 0; guard < 64 && at < n; guard++) {
		uint32_t op = w[at++];
		int64_t a, b;

		if (op == 0x000) break;
		if (op == 0x020 || op == 0x040 || op == 0x060 || op == 0x080 || op == 0x0A0) {
			uint32_t v = at < n ? w[at++] : 0;
			if (sp >= 16) return 0;
			st[sp++] = op == 0x020 ? (int64_t)(v < (uint32_t)env->nvar ? (int32_t)env->var[v] : 0)
				 : op == 0x040 ? (int64_t)(int32_t)v
				 : op == 0x060 ? 0
				 : (int64_t)env->dist;
			continue;
		}
		if (op == 0x0C0) {
			if (sp >= 16) return 0;
			st[sp++] = 0;
			continue;
		}
		if (op == 0x2A0) {
			if (sp < 1) return 0;
			st[sp - 1] = !st[sp - 1];
			continue;
		}
		if (sp < 2) return 0;
		b = st[--sp];
		a = st[sp - 1];
		st[sp - 1] = op == 0x160 ? (a & b)
			   : op == 0x200 ? (a == b)
			   : op == 0x220 ? (a > b)
			   : op == 0x260 ? (a < b)
			   : 0;
	}
	return sp > 0 && st[sp - 1] != 0;
}

static void mesh_eval_depth(struct mesh *m, const uint8_t *data, uint32_t bytes,
			    const struct mesh_env *env, int depth);

/* Run a model the way the renderer does: one path, the one its predicates
 * choose; calls that return; `$300`/`$340` (skip a group whose face looks
 * away) fall through, since the z-buffer settles what they settled. */
static void mesh_eval(struct mesh *m, const uint8_t *data, uint32_t bytes, const struct mesh_env *env)
{
	/* A model may write its own variables ($420), so it runs on a copy. */
	static uint32_t vars[MESH_VARMAX];
	struct mesh_env e = *env;
	int i;

	for (i = 0; i < MESH_VARMAX; i++)
		vars[i] = i < env->nvar && env->var ? env->var[i] : 0;
	e.var = vars;
	e.nvar = MESH_VARMAX;
	mesh_eval_depth(m, data, bytes, &e, 0);
}

#define MESH_EVAL_DEPTH 3

static void mesh_eval_depth(struct mesh *m, const uint8_t *data, uint32_t bytes,
			    const struct mesh_env *env, int depth)
{
	static uint32_t wbuf[MESH_EVAL_DEPTH][16384];
	static struct mesh sub[MESH_EVAL_DEPTH];
	uint32_t *w = wbuf[depth], stack[32], at, n = bytes / 4, i;
	int sp = 0, guard = 0;

	memset(m, 0, sizeof(*m));
	if (n > 16384) n = 16384;
	for (i = 0; i < n; i++)
		w[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
		       ((uint32_t)data[i * 4 + 2] << 8) | data[i * 4 + 3];
	if (n < MESH_STREAM) { m->stopped = "too short"; return; }
	for (i = 0; i < 7; i++) m->box[i] = mesh_f32(w[9 + i]);

	at = MESH_STREAM;
	while (at < n && guard++ < 200000) {
		uint32_t op = w[at++];
		int len, bad = 0;

		if (op < 0x600 && (op % 0x20) == 0) m->seen_op[op / 0x20]++;
		switch (op) {
		case 0x000:				/* return */
			if (!sp) return;
			at = stack[--sp];
			continue;
		case 0x020:				/* call */
			if (sp >= 32 || at >= n) { m->stopped = "call depth"; return; }
			stack[sp++] = at + 1;
			at = mesh_target(w[at]);
			continue;
		case 0x2C0:				/* jump */
			at = mesh_target(w[at]);
			continue;
		case 0x320: case 0x360: {		/* jump on a predicate */
			uint32_t end = mesh_predicate(w, n, at, &bad);
			int take;
			if (bad || end >= n) { m->stopped = "predicate"; return; }
			take = mesh_eval_pred(w, n, at, env);
			if (op == 0x360) take = !take;
			at = take ? mesh_target(w[end]) : end + 1;
			continue;
		}
		case 0x420: {				/* a predicate into a variable */
			/* `$420 program $000 v`: the arm models set var 52 from
			 * var 3's firing bits this way, then draw the muzzle's
			 * flash only while it is clear. */
			uint32_t end = mesh_predicate(w, n, at, &bad);
			if (bad || end >= n) { m->stopped = "predicate"; return; }
			if (w[end] < MESH_VARMAX)
				((uint32_t *)env->var)[w[end]] = (uint32_t)mesh_eval_pred(w, n, at, env);
			at = end + 1;
			continue;
		}
		case 0x300: case 0x340:			/* skip a group facing away */
			at += 2;
			continue;
		case 0x0A0:
			if (at + 4 > n) { m->stopped = "short vertex"; return; }
			mesh_put(m, w[at], mesh_f32(w[at + 1]), mesh_f32(w[at + 2]), mesh_f32(w[at + 3]));
			at += 4;
			continue;
		case 0x0C0: {
			uint32_t base = w[at], cnt = w[at + 1], k;
			if (cnt > 4096 || at + 2 + cnt * 3 > n) { m->stopped = "short run"; return; }
			for (k = 0; k < cnt; k++) {
				uint32_t o = at + 2 + k * 3;
				mesh_put(m, base + k, mesh_f32(w[o]), mesh_f32(w[o + 1]), mesh_f32(w[o + 2]));
			}
			at += 2 + cnt * 3;
			continue;
		}
		case 0x240: case 0x260: {
			uint32_t nv = w[at + 1], k;
			if (nv < 3 || nv > 64 || at + 3 + nv > n) { m->stopped = "polygon"; return; }
			if (m->npoly < MESH_POLYS) {
				struct mesh_poly *p = &m->poly[m->npoly++];
				p->n = (int)(nv < MESH_PIDX ? nv : MESH_PIDX);
				for (k = 0; k < (uint32_t)p->n; k++) p->v[k] = (uint16_t)w[at + 2 + k];
				p->mat = (uint16_t)w[at + 2 + nv];
			}
			at += 3 + nv;
			continue;
		}
		case 0x4C0: case 0x4E0: {
			uint32_t idx = w[at];
			if (at + 5 > n) { m->stopped = "short material"; return; }
			if (idx < MESH_MATS) {
				m->mkind[idx] = (uint8_t)(op == 0x4C0 ? 0 : 1);
				m->mr[idx] = mesh_f32(w[at + 1]);
				m->mg[idx] = mesh_f32(w[at + 2]);
				m->mb[idx] = mesh_f32(w[at + 3]);
				m->ma[idx] = mesh_f32(w[at + 4]);
				if (!m->mset[idx]) m->nmat++;
				m->mset[idx] = 1;
				if ((int)idx + 1 > m->nmat_top) m->nmat_top = (int)idx + 1;
			}
			at += 5;
			continue;
		}
		case 0x520: case 0x540: {
			/* A material blended from two by distance: (destination,
			 * near, far, A, B) - A up to `near`, B from `far` on. A hill's
			 * sand turns to the haze's grey over 50-250 m this way. */
			uint32_t d = w[at], s0 = w[at + 3], s1 = w[at + 4];
			float nearv = mesh_f32(w[at + 1]), farv = mesh_f32(w[at + 2]), t;
			if (at + 5 > n) { m->stopped = "short blend"; return; }
			if (d < MESH_MATS && s0 < MESH_MATS && s1 < MESH_MATS && m->mset[s0] && m->mset[s1]) {
				if (farv == nearv) t = env->dist < nearv ? 0.0f : 1.0f;
				else t = (env->dist - nearv) / (farv - nearv);
				if (t < 0) t = 0;
				if (t > 1) t = 1;
				m->mr[d] = m->mr[s0] + (m->mr[s1] - m->mr[s0]) * t;
				m->mg[d] = m->mg[s0] + (m->mg[s1] - m->mg[s0]) * t;
				m->mb[d] = m->mb[s0] + (m->mb[s1] - m->mb[s0]) * t;
				m->ma[d] = m->ma[s0] + (m->ma[s1] - m->ma[s0]) * t;
				m->mkind[d] = m->mkind[s0];
				if (!m->mset[d]) m->nmat++;
				m->mset[d] = 1;
				if ((int)d + 1 > m->nmat_top) m->nmat_top = (int)d + 1;
			}
			at += 5;
			continue;
		}
		case 0x200: case 0x220: case 0x280: case 0x2A0:
			if (at + (op == 0x220 ? 4u : 3u) > n) { m->stopped = "short mark"; return; }
			if (m->nmark < MESH_MARKS) {
				struct mesh_mark *k = &m->mark[m->nmark++];
				if (op == 0x200) {		/* face, vertex, material */
					k->kind = MARK_POINT; k->a = k->b = (uint16_t)w[at + 1];
					k->mat = (uint16_t)w[at + 2]; k->size = 0;
				} else if (op == 0x220) {	/* face, vertex, vertex, material */
					k->kind = MARK_RECT; k->a = (uint16_t)w[at + 1];
					k->b = (uint16_t)w[at + 2]; k->mat = (uint16_t)w[at + 3]; k->size = 0;
				} else {			/* vertex, size, material */
					k->kind = MARK_SPOT; k->a = k->b = (uint16_t)w[at];
					k->size = mesh_f32(w[at + 1]); k->mat = (uint16_t)w[at + 2];
				}
			}
			at += op == 0x220 ? 4 : 3;
			continue;
		case 0x460: {				/* draw another model here */
			static const float origin[3] = { 0, 0, 0 };
			uint32_t id = w[at + 2], sb;
			const uint8_t *sd;
			if (env->find && depth + 1 < MESH_EVAL_DEPTH && (sd = env->find(id, &sb)) != NULL) {
				mesh_eval_depth(&sub[depth + 1], sd, sb, env, depth + 1);
				mesh_merge(m, &sub[depth + 1], NULL, origin);
			}
			at += 3;
			continue;
		}
		case 0x160:
			at += 2 + 3 * w[at + 1];
			continue;
		case 0x1A0:
			at += 2 + 2 * w[at + 1];
			continue;
		case 0x560: case 0x580:
			at += 3 + w[at + 2];
			continue;
		case 0x480:
			at += 4;
			continue;
		case 0x4A0:
			continue;
		case 0x040: {
			uint32_t nd = w[at], par = w[at + 1];
			if (at + 6 > n) { m->stopped = "short node"; return; }
			if (nd < MESH_NODES && par < MESH_NODES) {
				float dx = mesh_f32(w[at + 3]), dy = mesh_f32(w[at + 4]), dz = mesh_f32(w[at + 5]);
				m->parent[nd] = (int16_t)par;
				m->ox[nd] = dx; m->oy[nd] = dy; m->oz[nd] = dz;
				m->slot[nd] = (uint8_t)w[at + 2];
				m->nx[nd] = m->nx[par] + dx;
				m->ny[nd] = m->ny[par] + dy;
				m->nz[nd] = m->nz[par] + dz;
				if (!m->nset[nd]) m->nnode++;
				m->nset[nd] = 1;
			}
			at += 6;
			continue;
		}
		default:
			break;
		}
		len = mesh_fixed(op);
		if (len < 0) { m->stopped = "unknown opcode"; return; }
		at += (uint32_t)len;
	}
	if (guard >= 200000) m->stopped = "step limit";
}

#endif /* BATTLEPOD_MESH_H */
