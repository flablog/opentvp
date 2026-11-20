/*
 * opentvp — reading and rendering TVPaint 11 files (.tvpp) without TVPaint.
 *
 *   opentvp info   file.tvpp [--json]
 *   opentvp render file.tvpp folder [--clip N] [--frames A-B] [--bg project|transparent|#RRGGBB]
 *                                 [--scale S] [--blur-samples N] [--threads N] [--prefix NAME]
 *
 * Format reverse-engineered (Flavio Perez, 2026), validated against
 * TVPaint 11.7 renders. Dependency: libdeflate (MIT), for decompressing
 * ZCHK chunks and compressing PNGs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define _FILE_OFFSET_BITS 64
#ifndef OPENTVP_VERSION
#define OPENTVP_VERSION "dev"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <shellapi.h>
static wchar_t *widen(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (n + 1));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static FILE *fopen_u8(const char *p, const char *m) {
    wchar_t *wp = widen(p), *wm = widen(m);
    FILE *f = _wfopen(wp, wm);
    free(wp); free(wm);
    return f;
}
static int mkdir_u8(const char *p) { wchar_t *w = widen(p); int r = _wmkdir(w); free(w); return r; }
#define MKDIR(p) mkdir_u8(p)
#else
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#define fopen_u8 fopen
#endif

#include "third_party/libdeflate/libdeflate.h"

/* ------------------------------------------------------------------ utilities */

static void (*g_die_hook)(const char *msg);   /* set by a GUI front end: shows the error, or recovers from it */
static void die(const char *msg) {
    if (g_die_hook) g_die_hook(msg);
    fprintf(stderr, "error: %s\n", msg);
    exit(1);
}
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("out of memory"); return p; }
static void *xcalloc(size_t n) { void *p = calloc(1, n ? n : 1); if (!p) die("out of memory"); return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) die("out of memory"); return p; }

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

typedef struct { uint8_t *d; size_t n; } Buf;

static Buf read_file(const char *path) {
    Buf b = {0, 0};
    FILE *f = fopen_u8(path, "rb");
    if (!f) { char msg[2100]; snprintf(msg, sizeof msg, "cannot open %s", path); die(msg); }
    size_t cap = 1 << 24;
    b.d = xmalloc(cap);
    for (;;) {
        if (b.n == cap) { cap *= 2; b.d = xrealloc(b.d, cap); }
        size_t r = fread(b.d + b.n, 1, cap - b.n, f);
        if (r == 0) break;
        b.n += r;
    }
    fclose(f);
    return b;
}

static const uint8_t *memfind(const uint8_t *h, size_t hn, const char *needle) {
    size_t nn = strlen(needle);
    if (nn == 0 || hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == (uint8_t)needle[0] && memcmp(h + i, needle, nn) == 0) return h + i;
    return NULL;
}

/* UTF-16 BE -> UTF-8 */
static char *utf16be_to_utf8(const uint8_t *p, int nchars) {
    char *s = xmalloc((size_t)nchars * 4 + 1);
    int o = 0;
    for (int i = 0; i < nchars; i++) {
        uint32_t c = be16(p + 2 * i);
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < nchars) {
            uint32_t c2 = be16(p + 2 * (i + 1));
            c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); i++;
        }
        if (c < 0x80) s[o++] = (char)c;
        else if (c < 0x800) { s[o++] = (char)(0xC0 | (c >> 6)); s[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { s[o++] = (char)(0xE0 | (c >> 12)); s[o++] = (char)(0x80 | ((c >> 6) & 63)); s[o++] = (char)(0x80 | (c & 63)); }
        else { s[o++] = (char)(0xF0 | (c >> 18)); s[o++] = (char)(0x80 | ((c >> 12) & 63)); s[o++] = (char)(0x80 | ((c >> 6) & 63)); s[o++] = (char)(0x80 | (c & 63)); }
    }
    s[o] = 0;
    return s;
}

/* ------------------------------------------------------------------ key/value lists */

typedef struct { char *k, *v; } KV;
typedef struct { KV *a; int n; } KVList;

static const char *kv_get(const KVList *l, const char *k) {
    for (int i = 0; i < l->n; i++) if (strcmp(l->a[i].k, k) == 0) return l->a[i].v;
    return NULL;
}
static double kv_num(const KVList *l, const char *k, double dflt) {
    const char *v = kv_get(l, k);
    return v ? atof(v) : dflt;
}

/* u32 number of pairs, then (u16 length + UTF-16 BE) x 2 */
static KVList read_kv_utf16(const uint8_t *p, const uint8_t *end) {
    KVList l = {0, 0};
    if (p + 4 > end) return l;
    uint32_t n = be32(p); p += 4;
    if (n > 100000) return l;
    l.a = xcalloc(sizeof(KV) * (n ? n : 1));
    for (uint32_t i = 0; i < n; i++) {
        char *pair[2];
        for (int j = 0; j < 2; j++) {
            if (p + 2 > end) return l;
            int len = be16(p); p += 2;
            if (p + 2 * len > end) return l;
            pair[j] = utf16be_to_utf8(p, len); p += 2 * len;
        }
        l.a[l.n].k = pair[0]; l.a[l.n].v = pair[1]; l.n++;
    }
    return l;
}

/* INI section "[name]": key=value lines up to the next section */
static KVList read_ini_section(const uint8_t *h, size_t hn, const char *name) {
    KVList l = {0, 0};
    char hdr[128];
    snprintf(hdr, sizeof hdr, "[%s]\n", name);
    const uint8_t *p = memfind(h, hn, hdr);
    if (!p) return l;
    p += strlen(hdr);
    const uint8_t *end = h + hn;
    int cap = 64;
    l.a = xcalloc(sizeof(KV) * cap);
    while (p < end && *p != '[' && *p != '\n' && *p >= 0x20) {
        const uint8_t *e = p;
        while (e < end && *e != '\n') e++;
        const uint8_t *eq = memchr(p, '=', (size_t)(e - p));
        if (eq) {
            if (l.n == cap) { cap *= 2; l.a = xrealloc(l.a, sizeof(KV) * cap); }
            size_t kl = (size_t)(eq - p), vl = (size_t)(e - eq - 1);
            l.a[l.n].k = xmalloc(kl + 1); memcpy(l.a[l.n].k, p, kl); l.a[l.n].k[kl] = 0;
            l.a[l.n].v = xmalloc(vl + 1); memcpy(l.a[l.n].v, eq + 1, vl); l.a[l.n].v[vl] = 0;
            if (vl && l.a[l.n].v[vl - 1] == '\r') l.a[l.n].v[vl - 1] = 0;
            l.n++;
        }
        p = e + 1;
    }
    return l;
}

/* ------------------------------------------------------------------ ZCHK chunks (zlib) */

/* ZCHK body -> complete decompressed inner chunk (tag + size + content) */
static uint8_t *zchk_inflate(const uint8_t *body, uint32_t size, uint32_t *outlen, int first_block_only) {
    if (size < 20 || memcmp(body + 8, "czmp", 4) != 0) return NULL;
    uint32_t nb = be32(body + 16);
    size_t cap = 0;
    const uint8_t *q = body + 20, *end = body + size;
    /* total size = sum of the raw sizes */
    const uint8_t *s = q;
    for (uint32_t i = 0; i < nb && s + 12 <= end; i++) { cap += be32(s + 4); s += 12 + be32(s + 8); }
    if (first_block_only && nb > 0) cap = be32(q + 4);
    uint8_t *out = xmalloc(cap + 16);
    size_t o = 0;
    for (uint32_t i = 0; i < nb && q + 12 <= end; i++) {
        uint32_t method = be32(q), raw = be32(q + 4), comp = be32(q + 8);
        q += 12;
        if (q + comp > end) break;
        if (method == 100) {
            struct libdeflate_decompressor *dz = libdeflate_alloc_decompressor();
            size_t got = 0;
            enum libdeflate_result r = libdeflate_zlib_decompress(dz, q, comp, out + o, raw, &got);
            libdeflate_free_decompressor(dz);
            if (r != LIBDEFLATE_SUCCESS) { free(out); return NULL; }
            o += got;
        } else {
            memcpy(out + o, q, comp); o += comp;
        }
        q += comp;
        if (first_block_only) break;
    }
    *outlen = (uint32_t)o;
    return out;
}

/* ------------------------------------------------------------------ structures */

typedef struct {
    const uint8_t *p;   /* content (uncompressed) or ZCHK body */
    uint32_t size;
    int zchk;
    uint32_t kind;      /* SRAW: 0x40 image, 6 hold, 5 empty, 2 reference; DBOD: 0 */
    uint32_t ref;       /* type 2: source frame */
} Entry;

typedef struct { Entry *a; int n, cap; } EntryList;

static void elist_push(EntryList *l, Entry e) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->a = xrealloc(l->a, sizeof(Entry) * l->cap); }
    l->a[l->n++] = e;
}

typedef struct {
    char *name;
    int ctg;
    uint32_t start, end, count;
    double opacity;
    int visible, locked;
    int pre, post;          /* 0 none, 1 repeat, 2 ping-pong, 3 hold */
    int blend;
    int has_dbod, has_rdbod, has_result;
    Entry dbod, rdbod;
    EntryList sraw, rsraw;
} Layer;

typedef struct { double x, y, rot, zoom; } CamPt;

typedef struct {
    char *name;
    int hidden, mark_in, mark_out;   /* -1 if absent */
    Layer *layers; int nlayers;
    int frames, visible_frames;
    CamPt *pts; int npts;
    int prof_type;                   /* 0 curve, 1 linear, 2 polynomial */
    double prof[64][2]; int nprof;
    double blur;
    uint8_t bg1[4], bg2[4]; int has_bg;
} Clip;

typedef struct {
    Buf file;
    int width, height;
    double fps, out_fps;
    int out_w, out_h;
    char *author, *modified;
    Clip *clips; int nclips;
} Project;

static const char *BLEND_NAMES[] = {
    "Color", "Behind", "Erase", "Shade", "Light", "Colorize", "Hue", "Add", "Sub", "Multiply",
    "Screen", "Replace", "Copy", "?13", "?14", "Difference", "Divide", "Overlay", "Light2", "Shade2",
    "HardLight", "SoftLight", "GrainExtract", "GrainMerge", "Sub2", "Darken", "Lighten", "Value", "Saturation"};
static const char *blend_name(int b) { return (b >= 0 && b <= 28) ? BLEND_NAMES[b] : "?"; }
static const char *BEHAV[] = {"none", "repeat", "pingpong", "hold"};

/* ------------------------------------------------------------------ reading layers */

static int is_tag(const uint8_t *p) {
    for (int i = 0; i < 4; i++) {
        uint8_t c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ')) return 0;
    }
    return 1;
}

static char *cp1252_to_utf8(const uint8_t *p, size_t n) {
    static const uint16_t hi[32] = {0x20AC,0x81,0x201A,0x192,0x201E,0x2026,0x2020,0x2021,0x2C6,0x2030,0x160,0x2039,0x152,0x8D,0x17D,0x8F,
                                    0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x2DC,0x2122,0x161,0x203A,0x153,0x9D,0x17E,0x178};
    char *s = xmalloc(n * 3 + 1); size_t o = 0;
    for (size_t i = 0; i < n && p[i]; i++) {
        uint32_t c = p[i];
        if (c >= 0x80 && c < 0xA0) c = hi[c - 0x80];
        if (c < 0x80) s[o++] = (char)c;
        else if (c < 0x800) { s[o++] = (char)(0xC0 | (c >> 6)); s[o++] = (char)(0x80 | (c & 63)); }
        else { s[o++] = (char)(0xE0 | (c >> 12)); s[o++] = (char)(0x80 | ((c >> 6) & 63)); s[o++] = (char)(0x80 | (c & 63)); }
    }
    s[o] = 0;
    return s;
}

static void read_layers(Clip *c, const uint8_t *a, const uint8_t *b) {
    const uint8_t *p = memfind(a, (size_t)(b - a), "LNAM");
    int cap = 16;
    c->layers = xcalloc(sizeof(Layer) * cap);
    Layer *L = NULL;
    int result_mode = 0;
    while (p && p + 8 <= b) {
        if (!is_tag(p)) {
            /* possible padding byte after an odd-sized chunk */
            if (p + 9 <= b && is_tag(p + 1)) { p++; continue; }
            break;
        }
        uint32_t size = be32(p + 4);
        const uint8_t *body = p + 8;
        if (body + size > b) break;
        char tag[5]; memcpy(tag, p, 4); tag[4] = 0;
        const uint8_t *ib = body; uint32_t isz = size; int z = 0;
        char itag[5]; memcpy(itag, tag, 5);
        uint8_t *tmp = NULL;
        if (strcmp(tag, "ZCHK") == 0) {
            uint32_t ol = 0;
            tmp = zchk_inflate(body, size, &ol, 1);
            if (tmp && ol >= 8) { memcpy(itag, tmp, 4); itag[4] = 0; }
            z = 1;
        }
        if (strcmp(itag, "LNAM") == 0) {
            if (c->nlayers == cap) { cap *= 2; c->layers = xrealloc(c->layers, sizeof(Layer) * cap); }
            L = &c->layers[c->nlayers++];
            memset(L, 0, sizeof *L);
            L->name = cp1252_to_utf8(body, size);
            result_mode = 0;
        } else if (L && strcmp(itag, "LNAW") == 0) {
            size_t n = 0; while (n < size && body[n]) n++;
            free(L->name); L->name = xmalloc(n + 1); memcpy(L->name, body, n); L->name[n] = 0;
        } else if (L && (strcmp(itag, "LRHD") == 0 || strcmp(itag, "LRSH") == 0) && size >= 64) {
            L->ctg = itag[3] == 'H' && itag[2] == 'S';
            L->start = be32(body + 4); L->end = be32(body + 8); L->count = be32(body + 12);
            L->opacity = body[19] / 255.0;
            uint32_t flags = be32(body + 28);
            L->visible = !(flags & 1); L->locked = (flags & 0x10) != 0;
            L->post = be16(body + 44); L->pre = be16(body + 52);
            L->blend = (int)be32(body + 60);
        } else if (L && strcmp(itag, "LRSR") == 0) {
            result_mode = 1; L->has_result = 1;
        } else if (L && (strcmp(itag, "DBOD") == 0 || strcmp(itag, "SRAW") == 0)) {
            Entry e = {ib, isz, z, 0, 0};
            if (itag[0] == 'S') {
                const uint8_t *k = z ? (tmp + 8) : body;
                e.kind = be32(k);
                if (e.kind == 2) e.ref = be32(k + 4);
            }
            if (itag[0] == 'D') {
                if (result_mode) { L->rdbod = e; L->has_rdbod = 1; } else { L->dbod = e; L->has_dbod = 1; }
            } else {
                elist_push(result_mode ? &L->rsraw : &L->sraw, e);
            }
        }
        free(tmp);
        p = body + size;
        if (strcmp(tag, "XSRC") == 0 || strcmp(tag, "FCFG") == 0) {
            /* end of the last layer: keep going to find any other LNAM */
            const uint8_t *q = memfind(p, (size_t)(b - p), "LNAM");
            if (!q) break;
            p = q;
        }
    }
}

/* ------------------------------------------------------------------ project, clips, camera */

#define SEC_ROOT "\x33\x84\x78\x0e"
#define SEC_SHOT "\x33\x86\x31\xb2"
#define SEC_CLIP "\x33\x89\xb8\x46"
#define SEC_CLIP_HDR "\x33\x87\xe3\x4a"
#define SEC_CLIP_BODY "\x33\x87\x11\x54"

/* subsections (16-byte GUID + u64 size) with the given id in [a, b) */
static int find_sections(const uint8_t *a, const uint8_t *b, const char *id, const uint8_t **out_a, const uint8_t **out_b, int max) {
    int n = 0;
    const uint8_t *p = a;
    while (p + 24 <= b && n < max) {
        uint64_t sz = be64(p + 16);
        if (sz > (uint64_t)(b - p - 24)) break;
        if (memcmp(p, id, 4) == 0) { out_a[n] = p + 24; out_b[n] = p + 24 + sz; n++; }
        p += 24 + sz;
    }
    return n;
}

static int cmp_dbl2(const void *x, const void *y) {
    double a = ((const double *)x)[0], b = ((const double *)y)[0];
    return (a > b) - (a < b);
}

static void read_camera(Clip *c, const uint8_t *a, const uint8_t *b) {
    KVList kv = read_ini_section(a, (size_t)(b - a), "cameradata");
    int n = (int)kv_num(&kv, "pathcount", 0);
    c->npts = n;
    c->pts = xcalloc(sizeof(CamPt) * (n ? n : 1));
    char k[64];
    for (int i = 0; i < n; i++) {
        snprintf(k, sizeof k, "mpoints-%d-x", i); c->pts[i].x = kv_num(&kv, k, 0);
        snprintf(k, sizeof k, "mpoints-%d-y", i); c->pts[i].y = kv_num(&kv, k, 0);
        snprintf(k, sizeof k, "mpoints-%d-rotation", i); c->pts[i].rot = kv_num(&kv, k, 0);
        snprintf(k, sizeof k, "mpoints-%d-zoomfactor", i); c->pts[i].zoom = kv_num(&kv, k, 1);
    }
    c->blur = kv_num(&kv, "mmotionblur", 0);
    int t = (int)kv_num(&kv, "mprofile_prof_linear", 1);
    c->prof_type = (t == 1) ? 1 : (t == 2 ? 2 : 0);
    int np = (int)kv_num(&kv, "mprofile_prof_numpoint", 0);
    if (np > 64) np = 64;
    c->nprof = 0;
    for (int i = 0; i < np; i++) {
        snprintf(k, sizeof k, "mprofile_prof_point%d", i);
        const char *v = kv_get(&kv, k);
        if (v && sscanf(v, "%lf %lf", &c->prof[c->nprof][0], &c->prof[c->nprof][1]) == 2) c->nprof++;
    }
    if (c->nprof < 2) { c->nprof = 2; c->prof[0][0] = 0; c->prof[0][1] = 0; c->prof[1][0] = 1; c->prof[1][1] = 1; }
    qsort(c->prof, (size_t)c->nprof, sizeof c->prof[0], cmp_dbl2);
}

static Project load_project(const char *path) {
    Project P;
    memset(&P, 0, sizeof P);
    P.file = read_file(path);
    const uint8_t *d = P.file.d, *e = d + P.file.n;
    if (P.file.n < 0x40) die("file too short");
    KVList hdr = read_kv_utf16(d + 0x30, e);
    P.width = (int)kv_num(&hdr, "Width", 0);
    P.height = (int)kv_num(&hdr, "Height", 0);
    P.fps = kv_num(&hdr, "FrameRate", 0);
    P.out_w = (int)kv_num(&hdr, "Camera.Width", P.width);
    P.out_h = (int)kv_num(&hdr, "Camera.Height", P.height);
    P.out_fps = kv_num(&hdr, "Camera.FrameRate", P.fps);
    const char *au = kv_get(&hdr, "Author"), *md = kv_get(&hdr, "LastChangeDate");
    P.author = au ? strdup(au) : strdup("");
    P.modified = md ? strdup(md) : strdup("");
    if (P.width <= 0 || P.height <= 0) die("unreadable project header (not a TVPaint 11 .tvpp?)");

    const uint8_t *ra[4], *rb[4], *sa[64], *sb[64], *ca[256], *cb[256], *xa[2], *xb[2];
    int nr = find_sections(d, e, SEC_ROOT, ra, rb, 4);
    int cap = 8; P.clips = xcalloc(sizeof(Clip) * cap);
    for (int r = 0; r < nr; r++) {
        int ns = find_sections(ra[r], rb[r], SEC_SHOT, sa, sb, 64);
        for (int s = 0; s < ns; s++) {
            int nc = find_sections(sa[s], sb[s], SEC_CLIP, ca, cb, 256);
            for (int k = 0; k < nc; k++) {
                if (P.nclips == cap) { cap *= 2; P.clips = xrealloc(P.clips, sizeof(Clip) * cap); }
                Clip *c = &P.clips[P.nclips];
                memset(c, 0, sizeof *c);
                if (find_sections(ca[k], cb[k], SEC_CLIP_HDR, xa, xb, 1) < 1) continue;
                KVList ch = read_kv_utf16(xa[0], xb[0]);
                if (find_sections(ca[k], cb[k], SEC_CLIP_BODY, xa, xb, 1) < 1) continue;
                const char *nm = kv_get(&ch, "Name");
                c->name = strdup(nm ? nm : "clip");
                c->hidden = kv_num(&ch, "Hidden", 0) == 1;
                c->mark_in = kv_num(&ch, "MarkIn", 0) == 1 ? (int)kv_num(&ch, "MarkInPosition", 0) : -1;
                c->mark_out = kv_num(&ch, "MarkOut", 0) == 1 ? (int)kv_num(&ch, "MarkOutPosition", 0) : -1;
                read_layers(c, xa[0], xb[0]);
                read_camera(c, xa[0], xb[0]);
                const uint8_t *g1 = memfind(xa[0], (size_t)(xb[0] - xa[0]), "BGP1");
                const uint8_t *g2 = memfind(xa[0], (size_t)(xb[0] - xa[0]), "BGP2");
                if (g1 && g2) { memcpy(c->bg1, g1 + 8, 4); memcpy(c->bg2, g2 + 8, 4); c->has_bg = 1; }
                int fr = 0, vf = 0;
                for (int i = 0; i < c->nlayers; i++) {
                    int en = (int)c->layers[i].end + 1;
                    if (en > fr) fr = en;
                    if (c->layers[i].visible && en > vf) vf = en;
                }
                c->frames = fr; c->visible_frames = vf;
                P.nclips++;
            }
        }
    }
    if (P.nclips == 0) die("no clip found");
    return P;
}

/* ------------------------------------------------------------------ camera: time distribution */

static void camera_weights(const CamPt *p, int np, double *w) {
    int n = np - 1;
    double L[1024];
    for (int i = 0; i < n && i < 1024; i++) L[i] = hypot(p[i + 1].x - p[i].x, p[i + 1].y - p[i].y);
    w[0] = L[0];
    for (int i = 1; i < n; i++) w[i] = (n - 1) * L[i - 1] + L[i];
    double tot = 0; for (int i = 0; i < n; i++) tot += w[i];
    if (tot == 0) for (int i = 0; i < n; i++) w[i] = 1;   /* zoom / rotation only */
}

static CamPt camera_at(const CamPt *p, int np, double u) {
    if (np == 1 || u <= 0) return p[0];
    double w[1024], tot = 0;
    camera_weights(p, np, w);
    for (int i = 0; i < np - 1; i++) tot += w[i];
    double t = u * tot, acc = 0;
    for (int i = 0; i < np - 1; i++) {
        if (w[i] > 0 && t <= acc + w[i] + 1e-12) {
            double f = (t - acc) / w[i];
            CamPt r = {p[i].x + (p[i + 1].x - p[i].x) * f, p[i].y + (p[i + 1].y - p[i].y) * f,
                       p[i].rot + (p[i + 1].rot - p[i].rot) * f, p[i].zoom + (p[i + 1].zoom - p[i].zoom) * f};
            return r;
        }
        acc += w[i];
    }
    return p[np - 1];
}

static double profile_eval(const Clip *c, double t) {
    int n = c->nprof;
    const double (*P)[2] = c->prof;
    if (t < P[0][0]) t = P[0][0];
    if (t > P[n - 1][0]) t = P[n - 1][0];
    double v;
    if (c->prof_type == 2) {               /* Lagrange polynomial */
        v = 0;
        for (int i = 0; i < n; i++) {
            double li = 1;
            for (int j = 0; j < n; j++) if (j != i) li *= (t - P[j][0]) / (P[i][0] - P[j][0]);
            v += P[i][1] * li;
        }
    } else {
        int k = n - 2;
        for (int i = 0; i < n - 1; i++) if (t < P[i + 1][0]) { k = i; break; }
        double h = P[k + 1][0] - P[k][0];
        double s = h > 0 ? (t - P[k][0]) / h : 0;
        if (c->prof_type == 1 || n == 2) {
            v = P[k][1] + (P[k + 1][1] - P[k][1]) * s;
        } else {                           /* Hermite, Catmull-Rom tangents, flat ends */
            double m0 = (k == 0) ? 0 : (P[k + 1][1] - P[k - 1][1]) / (P[k + 1][0] - P[k - 1][0]);
            double m1 = (k + 1 == n - 1) ? 0 : (P[k + 2][1] - P[k][1]) / (P[k + 2][0] - P[k][0]);
            m0 *= h; m1 *= h;
            double s2 = s * s, s3 = s2 * s;
            v = (2 * s3 - 3 * s2 + 1) * P[k][1] + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * P[k + 1][1] + (s3 - s2) * m1;
        }
    }
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static CamPt camera_at_t(const Project *P, const Clip *c, double t) {
    if (c->npts == 0) { CamPt d = {P->width / 2.0, P->height / 2.0, 0, 1}; return d; }
    if (c->npts == 1) return c->pts[0];
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return camera_at(c->pts, c->npts, profile_eval(c, t));
}

/* ------------------------------------------------------------------ pixel decoding */

/* PackBits on BGRA pixels, row by row, written to dst (row pitch: stride bytes) */
static size_t unpackbits(const uint8_t *s, size_t n, uint8_t *dst, int w, int h, size_t stride) {
    size_t i = 0;
    for (int y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)y * stride;
        int x = 0;
        while (x < w && i < n) {
            int c = s[i++];
            if (c >= 128) {
                int cnt = 257 - c;
                if (i + 4 > n) return i;
                uint32_t px; memcpy(&px, s + i, 4); i += 4;
                for (int k = 0; k < cnt && x < w; k++, x++) memcpy(row + 4 * x, &px, 4);
            } else {
                int cnt = c + 1;
                int m = cnt; if (x + m > w) m = w - x;
                if (i + 4 * (size_t)cnt > n) return i;
                memcpy(row + 4 * x, s + i, 4 * (size_t)m);
                i += 4 * (size_t)cnt; x += m;
            }
        }
    }
    return i;
}

/* content of an entry (decompressed if needed); *own is set if it must be freed */
static const uint8_t *entry_body(const Entry *e, uint32_t *len, uint8_t **own) {
    *own = NULL;
    if (!e->zchk) { *len = e->size; return e->p; }
    uint32_t ol = 0;
    uint8_t *b = zchk_inflate(e->p, e->size, &ol, 0);
    if (!b || ol < 8) die("unreadable compressed chunk");
    *own = b; *len = ol - 8;
    return b + 8;
}

typedef struct LayerState {
    const Layer *L;
    int w, h;
    const Entry *first;       /* DBOD of the series in use, or NULL */
    const EntryList *seq;
    int off;                  /* entry for frame k: seq[k + off] */
    uint8_t *img;             /* current image (premultiplied BGRA) */
    int idx;
    int *keep; int nkeep;     /* frames reused later (type 2) */
    uint8_t **cache;          /* kept images, indexed like keep */
} LayerState;

static uint8_t *layer_get(LayerState *S, int k);

static void layer_init(LayerState *S, const Layer *L, int w, int h) {
    memset(S, 0, sizeof *S);
    S->L = L; S->w = w; S->h = h;
    int use_result = L->ctg && (L->rsraw.n > 0 || L->has_rdbod);
    if (use_result) { S->first = L->has_rdbod ? &L->rdbod : NULL; S->seq = &L->rsraw; }
    else { S->first = L->has_dbod ? &L->dbod : NULL; S->seq = &L->sraw; }
    S->off = S->first ? -1 : 0;
    S->idx = -1;
    for (int i = 0; i < S->seq->n; i++) if (S->seq->a[i].kind == 2) S->nkeep++;
    S->keep = xcalloc(sizeof(int) * (S->nkeep + 1));
    S->cache = xcalloc(sizeof(uint8_t *) * (S->nkeep + 1));
    int j = 0;
    for (int i = 0; i < S->seq->n; i++) if (S->seq->a[i].kind == 2) S->keep[j++] = (int)S->seq->a[i].ref;
}

static void layer_store(LayerState *S) {
    size_t sz = (size_t)S->w * S->h * 4;
    for (int i = 0; i < S->nkeep; i++)
        if (S->keep[i] == S->idx && !S->cache[i]) { S->cache[i] = xmalloc(sz); memcpy(S->cache[i], S->img, sz); }
}

static const uint8_t *layer_lookup(LayerState *S, int src) {
    for (int i = 0; i < S->nkeep; i++) if (S->keep[i] == src && S->cache[i]) return S->cache[i];
    /* not cached: replay an independent chain */
    LayerState T; layer_init(&T, S->L, S->w, S->h);
    uint8_t *im = layer_get(&T, src);
    for (int i = 0; i < S->nkeep; i++) if (S->keep[i] == src && !S->cache[i]) { S->cache[i] = im; im = NULL; T.img = NULL; return S->cache[i]; }
    return im;
}

/* applies entry e to S->img (image of the previous frame) */
static void apply_entry(LayerState *S, const Entry *e) {
    int w = S->w, h = S->h;
    size_t sz = (size_t)w * h * 4;
    if (e->kind == 6) return;
    if (e->kind == 5) { memset(S->img, 0, sz); return; }
    if (e->kind == 2) { memcpy(S->img, layer_lookup(S, (int)e->ref), sz); return; }
    if (e->kind != 0x40) { fprintf(stderr, "  ⚠ unknown frame type %u (layer %s)\n", e->kind, S->L->name); return; }
    uint32_t len; uint8_t *own;
    const uint8_t *b = entry_body(e, &len, &own);
    uint32_t ts = be32(b), thumb = be32(b + 4);
    size_t i = 16 + (size_t)thumb - 8;
    uint32_t ntiles = be32(b + i); i += 4;
    int tx = (w + (int)ts - 1) / (int)ts;
    uint8_t *prev = xmalloc(sz);
    memcpy(prev, S->img, sz);
    for (uint32_t k = 0; k < ntiles && i + 4 <= len; k++) {
        int x0 = (int)(k % tx) * (int)ts, y0 = (int)(k / tx) * (int)ts;
        int tw = w - x0 < (int)ts ? w - x0 : (int)ts, th = h - y0 < (int)ts ? h - y0 : (int)ts;
        uint32_t size = be32(b + i); i += 4;
        uint8_t *dst = S->img + ((size_t)y0 * w + x0) * 4;
        if (size) {
            unpackbits(b + i, size, dst, tw, th, (size_t)w * 4);
            i += size;
        } else {
            uint32_t src = be32(b + i), idx = be32(b + i + 4); i += 8;
            const uint8_t *from = src == 1 ? prev : S->img;
            if (src == 1 && idx == k) continue;          /* unchanged tile */
            int sx = (int)(idx % tx) * (int)ts, sy = (int)(idx / tx) * (int)ts;
            const uint8_t *s0 = from + ((size_t)sy * w + sx) * 4;
            for (int y = 0; y < th; y++) memmove(dst + (size_t)y * w * 4, s0 + (size_t)y * w * 4, (size_t)tw * 4);
        }
    }
    free(prev);
    free(own);
}

static void layer_restart(LayerState *S) {
    size_t sz = (size_t)S->w * S->h * 4;
    if (!S->img) S->img = xmalloc(sz);
    if (S->first) {
        uint32_t len; uint8_t *own;
        const uint8_t *b = entry_body(S->first, &len, &own);
        memset(S->img, 0, sz);
        unpackbits(b, len, S->img, S->w, S->h, (size_t)S->w * 4);
        free(own);
    } else {
        memset(S->img, 0, sz);
        if (S->seq->n) apply_entry(S, &S->seq->a[0]);
    }
    S->idx = 0;
    layer_store(S);
}

static uint8_t *layer_get(LayerState *S, int k) {
    if (S->idx < 0 || k < S->idx) layer_restart(S);
    while (S->idx < k) {
        int j = S->idx + 1 + S->off;
        if (j >= S->seq->n) break;
        apply_entry(S, &S->seq->a[j]);
        S->idx++;
        layer_store(S);
    }
    return S->img;
}

/* layer image shown at frame f (layer frame index), or -1 */
static int behave(int rel, int n, int mode) {
    if (mode == 1) return ((rel % n) + n) % n;
    if (mode == 2) {
        if (n == 1) return 0;
        int per = 2 * n - 2, k = ((rel % per) + per) % per;
        return k < n ? k : per - k;
    }
    if (mode == 3) return rel < 0 ? 0 : n - 1;
    return -1;
}

static int frame_source(const Layer *L, int f) {
    if (L->count == 0) return (f >= (int)L->start && f <= (int)L->end) ? 0 : -1;
    int rel = f - (int)L->start, n = (int)L->count;
    if (rel >= 0 && rel < n) return rel;
    return behave(rel, n, rel < 0 ? L->pre : L->post);
}

/* ------------------------------------------------------------------ rendering */

typedef struct {
    const Project *P; const Clip *C;
    LayerState *ls; int nls;
    int ow, oh; double scale;
    int blur_samples;
    int canvas;                /* the whole canvas, without the camera (player only) */
} Renderer;

typedef struct {
    double a, b, c, d, e, f;   /* output -> project */
} Affine;

static Affine camera_affine(const CamPt *cam, int ow, int oh, double scale) {
    double z = cam->zoom / scale, th = cam->rot * M_PI / 180.0;
    double cs = cos(th), sn = sin(th), ox = ow / 2.0, oy = oh / 2.0;
    Affine A;
    A.a = z * cs; A.b = z * sn; A.d = -z * sn; A.e = z * cs;
    A.c = cam->x - A.a * ox - A.b * oy;
    A.f = cam->y - A.d * ox - A.e * oy;
    return A;
}

/* bilinear sampling (Pillow convention); returns 0 outside the image */
static inline int bilinear(const uint8_t *img, int w, int h, double xin, double yin, float out[4]) {
    if (xin < 0.0 || xin >= w || yin < 0.0 || yin >= h) return 0;
    xin -= 0.5; yin -= 0.5;
    int x = (int)floor(xin), y = (int)floor(yin);
    double dx = xin - x, dy = yin - y;
    int x0 = x < 0 ? 0 : (x >= w ? w - 1 : x), x1 = x + 1 < 0 ? 0 : (x + 1 >= w ? w - 1 : x + 1);
    int y0 = y < 0 ? 0 : (y >= h ? h - 1 : y);
    const uint8_t *r0 = img + (size_t)y0 * w * 4;
    const uint8_t *r1 = (y + 1 >= 0 && y + 1 < h) ? img + (size_t)(y + 1) * w * 4 : r0;
    for (int b = 0; b < 4; b++) {
        double v1 = r0[x0 * 4 + b] + (r0[x1 * 4 + b] - r0[x0 * 4 + b]) * dx;
        double v2 = r1[x0 * 4 + b] + (r1[x1 * 4 + b] - r1[x0 * 4 + b]) * dx;
        double v = v1 + (v2 - v1) * dy;
        out[b] = (float)(int)v;            /* 8-bit truncation, like Pillow */
    }
    return 1;
}

static inline void blend_px(float *dst, const float *src, int mode) {
    float sa = src[3], da = dst[3];
    if (mode == 0 || mode == 11 || mode == 12) {             /* Color (normal) */
        for (int i = 0; i < 4; i++) dst[i] = src[i] + dst[i] * (1 - sa);
        return;
    }
    if (mode == 1) { for (int i = 0; i < 4; i++) dst[i] = dst[i] + src[i] * (1 - da); return; }   /* Behind */
    if (mode == 2) { for (int i = 0; i < 4; i++) dst[i] = dst[i] * (1 - sa); return; }          /* Erase */
    float Cs[3], Cb[3], B[3];
    for (int i = 0; i < 3; i++) {
        Cs[i] = sa > 1e-6f ? src[i] / sa : 0;
        Cb[i] = da > 1e-6f ? dst[i] / da : 0;
    }
    for (int i = 0; i < 3; i++) {
        float s = Cs[i], b = Cb[i];
        switch (mode) {
        case 9:  B[i] = s * b; break;                                   /* Multiply */
        case 10: B[i] = s + b - s * b; break;                           /* Screen */
        case 7:  B[i] = s + b > 1 ? 1 : s + b; break;                   /* Add */
        case 25: B[i] = s < b ? s : b; break;                           /* Darken */
        case 26: B[i] = s > b ? s : b; break;                           /* Lighten */
        case 15: B[i] = fabsf(s - b); break;                            /* Difference */
        case 17: B[i] = b <= 0.5f ? 2 * s * b : 1 - 2 * (1 - s) * (1 - b); break;   /* Overlay */
        default: for (int j = 0; j < 4; j++) dst[j] = src[j] + dst[j] * (1 - sa); return;
        }
    }
    float oa = sa + da * (1 - sa);
    for (int i = 0; i < 3; i++) dst[i] = (1 - da) * src[i] + (1 - sa) * dst[i] + sa * da * B[i];
    dst[3] = oa;
}

typedef struct {
    const Renderer *R;
    const uint8_t **imgs; const LayerState **layers; int n;
    const Affine *aff; int naff;
    float *acc; float *cover;
    int y0, y1;
} Job;

static void render_rows(Job *J) {
    const Renderer *R = J->R;
    int pw = R->P->width, ph = R->P->height, ow = R->ow;
    for (int y = J->y0; y < J->y1; y++) {
        for (int x = 0; x < ow; x++) {
            float acc[4] = {0, 0, 0, 0}, cov = 0;
            for (int s = 0; s < J->naff; s++) {
                const Affine *A = &J->aff[s];
                double xo = x + 0.5, yo = y + 0.5;
                double xin = A->a * xo + A->b * yo + A->c, yin = A->d * xo + A->e * yo + A->f;
                /* share covered by the canvas (constant 255 mask, same filter) */
                if (xin >= 0 && xin < pw && yin >= 0 && yin < ph) cov += 1.0f;
                float out[4] = {0, 0, 0, 0};
                for (int l = 0; l < J->n; l++) {
                    float px[4];
                    if (!bilinear(J->imgs[l], pw, ph, xin, yin, px)) continue;
                    if (px[3] == 0 && px[0] == 0 && px[1] == 0 && px[2] == 0) continue;
                    double op = J->layers[l]->L->opacity;
                    float src[4] = {(float)(px[2] / 255.0 * op), (float)(px[1] / 255.0 * op),
                                    (float)(px[0] / 255.0 * op), (float)(px[3] / 255.0 * op)};
                    blend_px(out, src, J->layers[l]->L->blend);
                }
                for (int i = 0; i < 4; i++) acc[i] += out[i];
            }
            float *o = J->acc + ((size_t)y * ow + x) * 4;
            for (int i = 0; i < 4; i++) o[i] = acc[i] / J->naff;
            J->cover[(size_t)y * ow + x] = cov / J->naff;
        }
    }
}

#ifdef _WIN32
static DWORD WINAPI job_thread(LPVOID p) { render_rows((Job *)p); return 0; }
static int cpu_count(void) { SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors; }
#else
static void *job_thread(void *p) { render_rows((Job *)p); return NULL; }
static int cpu_count(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1; }
#endif

static void run_jobs(Job *jobs, int n) {
#ifdef _WIN32
    HANDLE *h = xmalloc(sizeof(HANDLE) * n);
    for (int i = 0; i < n; i++) h[i] = CreateThread(NULL, 0, job_thread, &jobs[i], 0, NULL);
    for (int i = 0; i < n; i++) { WaitForSingleObject(h[i], INFINITE); CloseHandle(h[i]); }
    free(h);
#else
    pthread_t *t = xmalloc(sizeof(pthread_t) * n);
    for (int i = 0; i < n; i++) pthread_create(&t[i], NULL, job_thread, &jobs[i]);
    for (int i = 0; i < n; i++) pthread_join(t[i], NULL);
    free(t);
#endif
}

static int g_threads = 0;

/* renders frame f: acc (premultiplied float RGBA) and cover (canvas share) */
static void render_frame(Renderer *R, int f, float *acc, float *cover) {
    const Clip *C = R->C;
    int N = C->visible_frames > 0 ? C->visible_frames : 1;
    double n = N > 1 ? N - 1 : 1;
    double t0 = f / n;
    Affine aff[64]; int naff = 1;
    if (R->canvas) {
        Affine id = {1 / R->scale, 0, 0, 0, 1 / R->scale, 0};
        aff[0] = id;
    } else if (C->blur > 0 && R->blur_samples > 1) {
        naff = R->blur_samples > 64 ? 64 : R->blur_samples;
        for (int i = 0; i < naff; i++) {
            CamPt cp = camera_at_t(R->P, C, t0 + C->blur / n * ((double)i / (naff - 1)));
            aff[i] = camera_affine(&cp, R->ow, R->oh, R->scale);
        }
    } else {
        CamPt cp = camera_at_t(R->P, C, t0);
        aff[0] = camera_affine(&cp, R->ow, R->oh, R->scale);
    }
    /* visible layers, bottom to top */
    const uint8_t **imgs = xmalloc(sizeof(uint8_t *) * (R->nls + 1));
    const LayerState **lays = xmalloc(sizeof(LayerState *) * (R->nls + 1));
    int n_on = 0;
    for (int i = R->nls - 1; i >= 0; i--) {
        LayerState *S = &R->ls[i];
        if (!S->L->visible) continue;
        int k = frame_source(S->L, f);
        if (k < 0) continue;
        imgs[n_on] = layer_get(S, k);
        lays[n_on] = S;
        n_on++;
    }
    int nt = g_threads > 0 ? g_threads : cpu_count();
    if (nt > R->oh) nt = R->oh;
    Job *jobs = xcalloc(sizeof(Job) * nt);
    for (int i = 0; i < nt; i++) {
        jobs[i].R = R; jobs[i].imgs = imgs; jobs[i].layers = lays; jobs[i].n = n_on;
        jobs[i].aff = aff; jobs[i].naff = naff; jobs[i].acc = acc; jobs[i].cover = cover;
        jobs[i].y0 = R->oh * i / nt; jobs[i].y1 = R->oh * (i + 1) / nt;
    }
    run_jobs(jobs, nt);
    free(jobs); free(imgs); free(lays);
}

/* ------------------------------------------------------------------ fast PNG (libdeflate) */

static int g_png_level = 6;
static int g_png_filter = 0;      /* 0 none (smallest here: flat colours), 1 Sub, 2 Up, 4 Paeth */

typedef struct { uint8_t *d; size_t n, cap; } Bytes;
static void bytes_put(Bytes *b, const void *p, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2; b->d = xrealloc(b->d, b->cap); }
    memcpy(b->d + b->n, p, n); b->n += n;
}
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void png_chunk(Bytes *b, const char *tag, const uint8_t *data, size_t n) {
    uint8_t h[8]; put_be32(h, (uint32_t)n); memcpy(h + 4, tag, 4);
    bytes_put(b, h, 8);
    if (n) bytes_put(b, data, n);
    uint32_t crc = libdeflate_crc32(0, tag, 4);
    if (n) crc = libdeflate_crc32(crc, data, n);
    uint8_t c[4]; put_be32(c, crc); bytes_put(b, c, 4);
}

/* encodes an 8-bit image (ch = 3 or 4 channels) into out (reusable) */
static void png_encode(Bytes *out, const uint8_t *px, int w, int h, int ch) {
    size_t stride = (size_t)w * ch, rawn = (stride + 1) * h;
    uint8_t *raw = xmalloc(rawn);
    for (int y = 0; y < h; y++) {
        const uint8_t *cur = px + y * stride, *up = y ? cur - stride : NULL;
        uint8_t *o = raw + y * (stride + 1);
        int f = g_png_filter;
        if (f == 2 && !up) f = 1;
        if (f == 4 && !up) f = 1;
        o[0] = (uint8_t)f; o++;
        size_t x;
        switch (f) {
        case 0: memcpy(o, cur, stride); break;
        case 1:
            memcpy(o, cur, (size_t)ch);
            for (x = (size_t)ch; x < stride; x++) o[x] = (uint8_t)(cur[x] - cur[x - ch]);
            break;
        case 2:
            for (x = 0; x < stride; x++) o[x] = (uint8_t)(cur[x] - up[x]);
            break;
        default:
            for (x = 0; x < (size_t)ch; x++) o[x] = (uint8_t)(cur[x] - up[x]);
            for (; x < stride; x++) {
                int a = cur[x - ch], b = up[x], c = up[x - ch];
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                o[x] = (uint8_t)(cur[x] - ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c)));
            }
        }
    }
    struct libdeflate_compressor *cmp = libdeflate_alloc_compressor(g_png_level);
    size_t bound = libdeflate_zlib_compress_bound(cmp, rawn);
    uint8_t *z = xmalloc(bound);
    size_t zn = libdeflate_zlib_compress(cmp, raw, rawn, z, bound);
    libdeflate_free_compressor(cmp);
    free(raw);
    out->n = 0;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    bytes_put(out, sig, 8);
    uint8_t ihdr[13]; put_be32(ihdr, (uint32_t)w); put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = ch == 4 ? 6 : 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(out, "IHDR", ihdr, 13);
    png_chunk(out, "IDAT", z, zn);
    png_chunk(out, "IEND", NULL, 0);
    free(z);
}

static int bytes_save(const char *path, const Bytes *b);

/* 8-bit pixels from a rendered frame: RGB (mode 0) or straight-alpha RGBA (mode 1) */
static void compose_pixels(const float *acc, const float *cover, int w, int h, int mode, const float bg[3], uint8_t *px) {
    /* mode 0: background (bg colour on the canvas, black elsewhere); 1: transparent */
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const float *a = acc + i * 4;
        if (mode == 1) {
            float al = a[3];
            for (int c = 0; c < 3; c++) {
                float v = al > 1e-6f ? a[c] / al : 0;
                v = v < 0 ? 0 : (v > 1 ? 1 : v);
                px[i * 4 + c] = (uint8_t)(v * 255 + 0.5f);
            }
            px[i * 4 + 3] = (uint8_t)((al < 0 ? 0 : (al > 1 ? 1 : al)) * 255 + 0.5f);
        } else {
            for (int c = 0; c < 3; c++) {
                float v = a[c] + bg[c] * cover[i] * (1 - a[3]);
                v = v < 0 ? 0 : (v > 1 ? 1 : v);
                px[i * 3 + c] = (uint8_t)(v * 255 + 0.5f);
            }
        }
    }
}

static int write_png(const char *path, const float *acc, const float *cover, int w, int h, int mode, const float bg[3]) {
    int ch = mode == 1 ? 4 : 3;
    uint8_t *px = xmalloc((size_t)w * h * ch);
    compose_pixels(acc, cover, w, h, mode, bg, px);
    Bytes b = {0, 0, 0};
    png_encode(&b, px, w, h, ch);
    int ok = bytes_save(path, &b);
    free(b.d);
    free(px);
    return ok;
}

/* ------------------------------------------------------------------ commands */

static void json_str(FILE *o, const char *s) {
    fputc('"', o);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', o); fputc(c, o); }
        else if (c < 0x20) fprintf(o, "\\u%04x", c);
        else fputc(c, o);
    }
    fputc('"', o);
}

static int cmd_info(const char *path, int json) {
    Project P = load_project(path);
    if (json) {
        printf("{\n  \"file\": "); json_str(stdout, path);
        printf(",\n  \"width\": %d, \"height\": %d, \"fps\": %g,\n", P.width, P.height, P.fps);
        printf("  \"output\": {\"width\": %d, \"height\": %d, \"fps\": %g},\n", P.out_w, P.out_h, P.out_fps);
        printf("  \"clips\": [\n");
        for (int i = 0; i < P.nclips; i++) {
            Clip *c = &P.clips[i];
            printf("    {\"index\": %d, \"name\": ", i + 1); json_str(stdout, c->name);
            printf(", \"hidden\": %s, \"frames\": %d, \"visible_frames\": %d, \"mark_in\": %d, \"mark_out\": %d,\n",
                   c->hidden ? "true" : "false", c->frames, c->visible_frames, c->mark_in, c->mark_out);
            printf("     \"camera\": {\"motion_blur\": %g, \"profile\": \"%s\", \"points\": [", c->blur,
                   c->prof_type == 1 ? "linear" : (c->prof_type == 2 ? "polynomial" : "curve"));
            for (int k = 0; k < c->npts; k++)
                printf("%s{\"x\": %g, \"y\": %g, \"rotation\": %g, \"zoom\": %g}", k ? ", " : "", c->pts[k].x, c->pts[k].y, c->pts[k].rot, c->pts[k].zoom);
            printf("],\n      \"track\": [");
            int N = c->visible_frames;
            for (int f = 0; f < N && c->npts > 1; f++) {
                CamPt cp = camera_at_t(&P, c, N > 1 ? (double)f / (N - 1) : 0);
                printf("%s[%.3f, %.3f, %.4f, %.5f]", f ? ", " : "", cp.x, cp.y, cp.rot, cp.zoom);
            }
            printf("]},\n     \"layers\": [\n");
            for (int l = 0; l < c->nlayers; l++) {
                Layer *L = &c->layers[l];
                printf("       {\"index\": %d, \"name\": ", l + 1); json_str(stdout, L->name);
                printf(", \"type\": \"%s\", \"start\": %u, \"end\": %u, \"visible\": %s, \"locked\": %s, \"opacity\": %d, \"blending\": \"%s\", \"pre\": \"%s\", \"post\": \"%s\"}%s\n",
                       L->ctg ? "ctg" : (L->count ? "anim" : "image"), L->start, L->end, L->visible ? "true" : "false",
                       L->locked ? "true" : "false", (int)(L->opacity * 100 + 0.5), blend_name(L->blend),
                       BEHAV[L->pre & 3], BEHAV[L->post & 3], l + 1 < c->nlayers ? "," : "");
            }
            printf("     ]}%s\n", i + 1 < P.nclips ? "," : "");
        }
        printf("  ]\n}\n");
        return 0;
    }
    printf("%s\n", path);
    printf("  Resolution   %d x %d\n  Output       %d x %d (camera)\n  Frame rate   %g fps", P.width, P.height, P.out_w, P.out_h, P.fps);
    if (fabs(P.out_fps - P.fps) > 1e-3) printf("  (camera %g fps: TVPaint resamples at render)", P.out_fps);
    printf("\n  Modified     %s by %s\n  Clips        %d\n", P.modified, P.author, P.nclips);
    for (int i = 0; i < P.nclips; i++) {
        Clip *c = &P.clips[i];
        printf("\n  Clip %d \"%s\"%s — %d frames, visible content %d", i + 1, c->name, c->hidden ? " (hidden)" : "", c->frames, c->visible_frames);
        if (c->mark_in >= 0 || c->mark_out >= 0) printf(", marks %d -> %d", c->mark_in, c->mark_out);
        printf("\n");
        for (int l = 0; l < c->nlayers; l++) {
            Layer *L = &c->layers[l];
            printf("    %2d  %-30s %-5s %4u-%-4u %-4s %3d%%  %-10s pre:%-8s post:%-8s%s\n", l + 1, L->name,
                   L->ctg ? "ctg" : (L->count ? "anim" : "image"), L->start, L->end, L->visible ? "vis" : "hid",
                   (int)(L->opacity * 100 + 0.5), blend_name(L->blend), BEHAV[L->pre & 3], BEHAV[L->post & 3],
                   L->locked ? "  locked" : "");
        }
        if (c->npts > 0) {
            printf("    Camera: %d point(s)", c->npts);
            if (c->blur > 0) printf(", blur %g", c->blur);
            if (c->nprof > 2 || c->prof_type != 1) printf(", profile %s", c->prof_type == 1 ? "linear" : (c->prof_type == 2 ? "polynomial" : "curve"));
            printf("\n");
            for (int k = 0; k < c->npts; k++)
                printf("      point %d: centre (%g, %g) zoom %g rotation %g°\n", k, c->pts[k].x, c->pts[k].y, c->pts[k].zoom, c->pts[k].rot);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ layer-by-layer export */

typedef struct {
    const char *path, *outdir, *layers, *json;
    int clip, f0, f1, keyframes, camera, include_hidden, skip_empty, png_level, blur_samples;
    double scale;
} LayerOpts;

static void mkdir_p(const char *path) {
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            if (p > tmp && p[-1] == ':') continue;          /* C:/ */
            char c = *p; *p = 0; MKDIR(tmp); *p = c;
        }
    }
    MKDIR(tmp);
}

/* like re.sub(r'[^\w.-]+', '_', s).strip('_') */
static char *safe_name(const char *s) {
    size_t n = strlen(s);
    char *o = xmalloc(n + 8); size_t k = 0;
    int run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int ok = (c >= 0x80) || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '.' || c == '-';
        if (ok) { o[k++] = (char)c; run = 0; }
        else if (!run) { o[k++] = '_'; run = 1; }
    }
    o[k] = 0;
    size_t a = 0; while (o[a] == '_') a++;
    while (k > a && o[k - 1] == '_') o[--k] = 0;
    if (a) memmove(o, o + a, k - a + 1);
    if (!o[0]) strcpy(o, "layer");
    return o;
}

/* identity of the drawing shown at each layer frame, -1 = empty */
static int *drawing_ids(const LayerState *S, int count) {
    int n = count > 0 ? count : 1;
    int *ids = xmalloc(sizeof(int) * n);
    for (int k = 0; k < n; k++) {
        int prev = k ? ids[k - 1] : -1;
        if (k == 0 && S->first) { ids[k] = 0; continue; }
        int j = k + S->off;
        if (j < 0 || j >= S->seq->n) { ids[k] = prev; continue; }
        const Entry *e = &S->seq->a[j];
        if (e->kind == 0x40) ids[k] = k;
        else if (e->kind == 6) ids[k] = prev;
        else if (e->kind == 5) ids[k] = -1;
        else if (e->kind == 2) ids[k] = (int)e->ref < k ? ids[e->ref] : (int)e->ref;
        else ids[k] = k;
    }
    return ids;
}

/* premultiplied RGBA -> straight alpha, rounded like TVPaint's export */
static void unpremultiply_rgba(uint8_t *px, size_t n) {
    for (size_t i = 0; i < n; i++, px += 4) {
        uint32_t a = px[3];
        if (a == 0 || a == 255) continue;
        for (int c = 0; c < 3; c++) {
            uint32_t v = (px[c] * 510u + a) / (2 * a);
            px[c] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

/* separable resize, Pillow-style (bilinear filter, antialiased) */
static void resample_axis(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh, int horiz) {
    int in = horiz ? sw : sh, out = horiz ? dw : dh;
    double scale = (double)in / out, fs = scale > 1 ? scale : 1, support = fs;
    int ksize = (int)ceil(support) * 2 + 1;
    double *w = xmalloc(sizeof(double) * ksize * out);
    int *b0 = xmalloc(sizeof(int) * out), *bn = xmalloc(sizeof(int) * out);
    for (int x = 0; x < out; x++) {
        double center = (x + 0.5) * scale, ss = 1.0 / fs, tot = 0;
        int xmin = (int)(center - support + 0.5), xmax = (int)(center + support + 0.5);
        if (xmin < 0) xmin = 0;
        if (xmax > in) xmax = in;
        int cnt = xmax - xmin; if (cnt > ksize) cnt = ksize;
        for (int k = 0; k < cnt; k++) {
            double t = fabs((k + xmin - center + 0.5) * ss);
            double v = t < 1 ? 1 - t : 0;
            w[x * ksize + k] = v; tot += v;
        }
        for (int k = 0; k < cnt; k++) if (tot) w[x * ksize + k] /= tot;
        b0[x] = xmin; bn[x] = cnt;
    }
    int lines = horiz ? sh : dw;
    for (int l = 0; l < lines; l++) {
        for (int x = 0; x < out; x++) {
            double acc[4] = {0.5, 0.5, 0.5, 0.5};
            for (int k = 0; k < bn[x]; k++) {
                int p = b0[x] + k;
                const uint8_t *s = horiz ? src + ((size_t)l * sw + p) * 4 : src + ((size_t)p * sw + l) * 4;
                double ww = w[x * ksize + k];
                for (int c = 0; c < 4; c++) acc[c] += s[c] * ww;
            }
            uint8_t *d = horiz ? dst + ((size_t)l * dw + x) * 4 : dst + ((size_t)x * dw + l) * 4;
            for (int c = 0; c < 4; c++) d[c] = (uint8_t)(acc[c] < 0 ? 0 : (acc[c] > 255 ? 255 : acc[c]));
        }
    }
    free(w); free(b0); free(bn);
}

typedef struct {
    const Project *P; const Clip *C; const LayerOpts *o;
    int iw, ih;                 /* size of the written images */
    int idx;                    /* layer index in the clip */
    char *name, *folder;
    /* results */
    char **timeline;            /* f1 - f0 + 1 entries (relative paths) */
    int *first_frames; int nfirst;      /* key_frames */
    int *dr_first, *dr_lframe; int ndr; /* exported drawings */
    int nfiles;
} LayerJob;

typedef struct { LayerJob *jobs; int n, next; } JobQueue;

#ifdef _WIN32
static CRITICAL_SECTION g_lock; static int g_lock_init = 0;
static void lock(void) { if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; } EnterCriticalSection(&g_lock); }
static void unlock(void) { LeaveCriticalSection(&g_lock); }
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void) { pthread_mutex_lock(&g_lock); }
static void unlock(void) { pthread_mutex_unlock(&g_lock); }
#endif

typedef Bytes MemBuf;
static void write_bytes(const char *path, const MemBuf *m) {
    if (!bytes_save(path, m)) { fprintf(stderr, "write error: %s\n", path); exit(1); }
}
static void encode_png(MemBuf *m, const uint8_t *rgba, int w, int h) { png_encode(m, rgba, w, h, 4); }

/* image written for frame f from the layer pixels (premultiplied BGRA) */
static void make_layer_image(const LayerJob *J, const uint8_t *bgra, int f, uint8_t *out, uint8_t *tmp) {
    const Project *P = J->P; const Clip *C = J->C; const LayerOpts *o = J->o;
    int pw = P->width, ph = P->height;
    size_t npx = (size_t)J->iw * J->ih;
    if (o->camera) {
        int N = C->visible_frames > 0 ? C->visible_frames : 1;
        double n = N > 1 ? N - 1 : 1, t0 = f / n;
        int ns = (C->blur > 0 && o->blur_samples > 1) ? o->blur_samples : 1;
        if (ns > 64) ns = 64;
        float *acc = xcalloc(sizeof(float) * 4 * npx);
        for (int s = 0; s < ns; s++) {
            double t = ns > 1 ? t0 + C->blur / n * ((double)s / (ns - 1)) : t0;
            CamPt cp = camera_at_t(P, C, t);
            Affine A = camera_affine(&cp, J->iw, J->ih, o->scale);
            for (int y = 0; y < J->ih; y++)
                for (int x = 0; x < J->iw; x++) {
                    double xo = x + 0.5, yo = y + 0.5;
                    float px[4];
                    if (!bilinear(bgra, pw, ph, A.a * xo + A.b * yo + A.c, A.d * xo + A.e * yo + A.f, px)) continue;
                    float *d = acc + ((size_t)y * J->iw + x) * 4;
                    d[0] += px[2]; d[1] += px[1]; d[2] += px[0]; d[3] += px[3];
                }
        }
        for (size_t i = 0; i < npx * 4; i++) out[i] = (uint8_t)(acc[i] / ns + 0.5f);
        free(acc);
    } else {
        const uint8_t *s = bgra;
        if (J->iw != pw || J->ih != ph) {
            uint8_t *mid = xmalloc((size_t)J->iw * ph * 4);
            resample_axis(bgra, pw, ph, mid, J->iw, ph, 1);
            resample_axis(mid, J->iw, ph, tmp, J->iw, J->ih, 0);
            free(mid);
            s = tmp;
        }
        for (size_t i = 0; i < npx; i++) {
            out[i * 4] = s[i * 4 + 2]; out[i * 4 + 1] = s[i * 4 + 1];
            out[i * 4 + 2] = s[i * 4]; out[i * 4 + 3] = s[i * 4 + 3];
        }
    }
    unpremultiply_rgba(out, npx);
}

static void export_one_layer(LayerJob *J) {
    const Project *P = J->P; const Clip *C = J->C; const LayerOpts *o = J->o;
    const Layer *L = &C->layers[J->idx];
    int nfr = o->f1 - o->f0 + 1;
    LayerState S; layer_init(&S, L, P->width, P->height);
    int *ids = drawing_ids(&S, (int)L->count);
    int nids = L->count > 0 ? (int)L->count : 1;
    int *did = xmalloc(sizeof(int) * nfr);
    for (int i = 0; i < nfr; i++) {
        int k = frame_source(L, o->f0 + i);
        did[i] = (k < 0 || k >= nids) ? -1 : ids[k];
    }
    char dir[2048];
    snprintf(dir, sizeof dir, "%s/%s", o->outdir, J->folder);
    mkdir_p(dir);
    size_t npx = (size_t)J->iw * J->ih;
    uint8_t *img = xmalloc(npx * 4), *tmp = (J->iw != P->width || J->ih != P->height) ? xmalloc(npx * 4) : NULL;
    MemBuf png = {0, 0, 0}, blank = {0, 0, 0};
    J->timeline = xcalloc(sizeof(char *) * nfr);
    J->first_frames = xmalloc(sizeof(int) * nfr);
    J->dr_first = xmalloc(sizeof(int) * nfr); J->dr_lframe = xmalloc(sizeof(int) * nfr);
    char fn[2400], rel[2400];
    /* first appearance of each drawing in the range */
    int *first = xmalloc(sizeof(int) * nfr), nf = 0;
    for (int i = 0; i < nfr; i++) {
        if (did[i] < 0) continue;
        int seen = 0;
        for (int j = 0; j < nf; j++) if (did[first[j]] == did[i]) { seen = 1; break; }
        if (!seen) first[nf++] = i;
    }
    if (o->keyframes) {
        int *empty = xcalloc(sizeof(int) * (nf + 1));
        for (int j = 0; j < nf; j++) {
            int i = first[j], f = o->f0 + i;
            const uint8_t *px = layer_get(&S, did[i]);
            int any = 0;
            for (size_t p = 3; p < (size_t)P->width * P->height * 4; p += 4) if (px[p]) { any = 1; break; }
            if (!any) { empty[j] = 1; continue; }
            make_layer_image(J, px, f, img, tmp);
            encode_png(&png, img, J->iw, J->ih);
            snprintf(fn, sizeof fn, "%s/%s_%04d.png", dir, J->name, f);
            write_bytes(fn, &png);
            J->dr_first[J->ndr] = f; J->dr_lframe[J->ndr] = did[i]; J->ndr++;
            J->first_frames[J->nfirst++] = f;
        }
        for (int i = 0; i < nfr; i++) {
            if (did[i] < 0) continue;
            for (int j = 0; j < nf; j++)
                if (did[first[j]] == did[i] && !empty[j]) {
                    snprintf(rel, sizeof rel, "%s/%s_%04d.png", J->folder, J->name, o->f0 + first[j]);
                    J->timeline[i] = strdup(rel);
                    break;
                }
        }
        J->nfiles = J->ndr;
        free(empty);
    } else {
        for (int j = 0; j < nf; j++) J->first_frames[J->nfirst++] = o->f0 + first[j];
        int last_key = -2, last_f = -1;
        for (int i = 0; i < nfr; i++) {
            int f = o->f0 + i;
            snprintf(fn, sizeof fn, "%s/%s_%04d.png", dir, J->name, f);
            if (did[i] < 0) {
                if (o->skip_empty) continue;
                if (!blank.n) { memset(img, 0, npx * 4); encode_png(&blank, img, J->iw, J->ih); }
                write_bytes(fn, &blank);
            } else if (did[i] == last_key && !o->camera && png.n) {
                write_bytes(fn, &png);
            } else {
                make_layer_image(J, layer_get(&S, did[i]), f, img, tmp);
                encode_png(&png, img, J->iw, J->ih);
                write_bytes(fn, &png);
                last_key = did[i]; last_f = f;
            }
            snprintf(rel, sizeof rel, "%s/%s_%04d.png", J->folder, J->name, f);
            J->timeline[i] = strdup(rel);
            J->nfiles++;
        }
        (void)last_f;
    }
    lock();
    printf("  %s: %d image(s)\n", L->name, J->nfiles); fflush(stdout);
    unlock();
    free(first); free(did); free(ids); free(img); free(tmp); free(png.d); free(blank.d);
    free(S.img);
    for (int i = 0; i < S.nkeep; i++) free(S.cache[i]);
    free(S.cache); free(S.keep);
}

#ifdef _WIN32
static DWORD WINAPI layer_worker(LPVOID p)
#else
static void *layer_worker(void *p)
#endif
{
    JobQueue *q = p;
    for (;;) {
        lock();
        int i = q->next++;
        unlock();
        if (i >= q->n) break;
        export_one_layer(&q->jobs[i]);
    }
    return 0;
}

#ifdef _WIN32
#define strtok_r strtok_s
#endif

static int select_layer(const Clip *C, const char *tok) {
    char *end;
    long v = strtol(tok, &end, 10);
    if (*tok && !*end && v >= 1 && v <= C->nlayers) return (int)v - 1;
    for (int i = 0; i < C->nlayers; i++) if (!strcmp(C->layers[i].name, tok)) return i;
#ifdef _WIN32
    for (int i = 0; i < C->nlayers; i++) if (!_stricmp(C->layers[i].name, tok)) return i;
#else
    for (int i = 0; i < C->nlayers; i++) if (!strcasecmp(C->layers[i].name, tok)) return i;
#endif
    return -1;
}

static int export_layers(LayerOpts *o) {
    time_t w_start = time(NULL);
    Project P = load_project(o->path);
    if (o->clip < 1 || o->clip > P.nclips) die("invalid clip number");
    Clip *C = &P.clips[o->clip - 1];
    if (o->camera && o->keyframes)
        die("--camera cannot be used with --keyframes: a held drawing is framed differently on every frame. "
            "The camera track is in the JSON.");
    /* selection */
    int *sel = xcalloc(sizeof(int) * (C->nlayers + 1)), nsel = 0;
    int all = !o->layers || !*o->layers || !strcmp(o->layers, "all") || !strcmp(o->layers, "tous");
    if (all) {
        for (int i = 0; i < C->nlayers; i++) if (o->include_hidden || C->layers[i].visible) sel[nsel++] = i;
    } else {
        char *spec = strdup(o->layers), *save = NULL;
        int *mark = xcalloc(sizeof(int) * (C->nlayers + 1));
        for (char *t = strtok_r(spec, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
            while (*t == ' ') t++;
            char *e = t + strlen(t); while (e > t && e[-1] == ' ') *--e = 0;
            if (!*t) continue;
            int k = select_layer(C, t);
            if (k < 0) { fprintf(stderr, "error: layer not found: \"%s\" (see opentvp info)\n", t); return 2; }
            mark[k] = 1;
        }
        for (int i = 0; i < C->nlayers; i++) if (mark[i]) sel[nsel++] = i;
    }
    int total = C->frames > 0 ? C->frames : 1;
    if (o->f0 < 0) {
        if (C->mark_in >= 0 || C->mark_out >= 0) { o->f0 = C->mark_in >= 0 ? C->mark_in : 0; o->f1 = C->mark_out >= 0 ? C->mark_out : total - 1; }
        else { o->f0 = 0; o->f1 = total - 1; }
    }
    if (o->f1 < o->f0) o->f1 = o->f0;
    int nfr = o->f1 - o->f0 + 1;
    double sc = o->scale > 0 ? o->scale : 1;
    int iw, ih;
    if (o->camera) { iw = (int)(P.out_w * sc + 0.5); ih = (int)(P.out_h * sc + 0.5); }
    else { iw = (int)(P.width * sc + 0.5); ih = (int)(P.height * sc + 0.5); }
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    mkdir_p(o->outdir);
    printf("%s — clip \"%s\" — %d layer(s) — frames %d to %d — %s%s\n", o->path, C->name, nsel, o->f0, o->f1,
           o->keyframes ? "drawings only" : "every frame", o->camera ? " — through the camera" : "");
    g_png_level = o->png_level;

    LayerJob *jobs = xcalloc(sizeof(LayerJob) * (nsel + 1));
    for (int j = 0; j < nsel; j++) {
        LayerJob *J = &jobs[j];
        J->P = &P; J->C = C; J->o = o; J->iw = iw; J->ih = ih; J->idx = sel[j];
        J->name = safe_name(C->layers[sel[j]].name);
        int dup = 0;
        for (int k = 0; k < nsel; k++) if (k != j) { char *s = safe_name(C->layers[sel[k]].name); dup |= !strcmp(s, J->name); free(s); }
        if (dup) { J->folder = xmalloc(strlen(J->name) + 8); sprintf(J->folder, "%02d_%s", sel[j] + 1, J->name); }
        else J->folder = strdup(J->name);
    }
    /* threads: one layer per thread */
    int nt = g_threads > 0 ? g_threads : cpu_count();
    size_t per = (size_t)P.width * P.height * 4 * 4;      /* approximate memory per layer in progress */
    if (!g_threads && per > ((size_t)400 << 20)) { int lim = 8; if (nt > lim) nt = lim; }
    if (nt > nsel) nt = nsel;
    if (nt < 1) nt = 1;
    JobQueue q = {jobs, nsel, 0};
#ifdef _WIN32
    HANDLE *th = xmalloc(sizeof(HANDLE) * nt);
    lock(); unlock();
    for (int i = 0; i < nt; i++) th[i] = CreateThread(NULL, 0, layer_worker, &q, 0, NULL);
    for (int i = 0; i < nt; i++) { WaitForSingleObject(th[i], INFINITE); CloseHandle(th[i]); }
#else
    pthread_t *th = xmalloc(sizeof(pthread_t) * nt);
    for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, layer_worker, &q);
    for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
#endif
    free(th);

    /* JSON */
    char jpath[2400], *cname = safe_name(C->name);
    if (o->json) snprintf(jpath, sizeof jpath, "%s/%s", o->outdir, o->json);
    else snprintf(jpath, sizeof jpath, "%s/%s_%s.json", o->outdir, cname, o->keyframes ? "exposure" : "layers");
    FILE *js = fopen_u8(jpath, "wb");
    if (!js) die("cannot write the JSON");
    const char *base = o->path + strlen(o->path);
    while (base > o->path && base[-1] != '/' && base[-1] != '\\') base--;
    fprintf(js, "{\n \"source\": "); json_str(js, base);
    fprintf(js, ",\n \"clip\": "); json_str(js, C->name);
    fprintf(js, ",\n \"mode\": \"%s\",\n \"space\": \"%s\",\n \"fps\": %g,\n", o->keyframes ? "keyframes" : "frames", o->camera ? "camera" : "canvas", P.fps);
    fprintf(js, " \"canvas\": {\"width\": %d, \"height\": %d},\n \"image_size\": {\"width\": %d, \"height\": %d},\n \"scale\": %g,\n",
            P.width, P.height, iw, ih, sc);
    fprintf(js, " \"frame_range\": [%d, %d],\n \"frame_numbering\": \"0 = first frame of the project (the TVPaint timeline shows +1)\",\n \"alpha\": \"straight\",\n", o->f0, o->f1);
    fprintf(js, " \"camera\": {\n  \"output\": {\"width\": %d, \"height\": %d, \"fps\": %g},\n  \"motion_blur\": %g,\n  \"track\": [\n", P.out_w, P.out_h, P.out_fps, C->blur);
    int N = C->visible_frames;
    for (int i = 0; i < nfr; i++) {
        int f = o->f0 + i;
        CamPt cp = camera_at_t(&P, C, (double)f / (N > 1 ? N - 1 : 1));
        fprintf(js, "   {\"frame\": %d, \"x\": %.4f, \"y\": %.4f, \"rotation\": %.5f, \"zoom\": %.6f}%s\n", f, cp.x, cp.y, cp.rot, cp.zoom, i + 1 < nfr ? "," : "");
    }
    fprintf(js, "  ]\n },\n \"layer_order\": \"top to bottom, as in TVPaint; composite starting from the last one\",\n \"layers\": [\n");
    for (int j = 0; j < nsel; j++) {
        LayerJob *J = &jobs[j]; const Layer *L = &C->layers[J->idx];
        fprintf(js, "  {\n   \"index\": %d,\n   \"name\": ", J->idx + 1); json_str(js, L->name);
        fprintf(js, ",\n   \"folder\": "); json_str(js, J->folder);
        fprintf(js, ",\n   \"type\": \"%s\",\n   \"visible\": %s,\n   \"opacity\": %g,\n   \"blending\": \"%s\",\n",
                L->ctg ? "ctg" : (L->count ? "anim" : "image"), L->visible ? "true" : "false",
                (int)(L->opacity * 100 + 0.5) / 100.0, blend_name(L->blend));
        fprintf(js, "   \"pre_behavior\": \"%s\",\n   \"post_behavior\": \"%s\",\n   \"start\": %u,\n   \"end\": %u,\n",
                BEHAV[L->pre & 3], BEHAV[L->post & 3], L->start, L->end);
        if (o->keyframes) {
            fprintf(js, "   \"drawings\": [");
            for (int k = 0; k < J->ndr; k++) {
                fprintf(js, "%s\n    {\"file\": \"", k ? "," : "");
                fprintf(js, "%s/%s_%04d.png\", \"first_frame\": %d, \"layer_frame\": %d}", J->folder, J->name, J->dr_first[k], J->dr_first[k], J->dr_lframe[k]);
            }
            fprintf(js, "\n   ],\n   \"exposures\": [");
            int ne = 0;
            for (int i = 0; i < nfr; ) {
                int k = i + 1;
                while (k < nfr && ((J->timeline[k] == NULL) == (J->timeline[i] == NULL)) &&
                       (J->timeline[i] == NULL || !strcmp(J->timeline[k], J->timeline[i]))) k++;
                if (J->timeline[i]) {
                    fprintf(js, "%s\n    {\"start\": %d, \"duration\": %d, \"file\": ", ne++ ? "," : "", o->f0 + i, k - i);
                    json_str(js, J->timeline[i]); fputc('}', js);
                }
                i = k;
            }
            fprintf(js, "\n   ],\n");
        }
        fprintf(js, "   \"key_frames\": [");
        for (int k = 0; k < J->nfirst; k++) fprintf(js, "%s%d", k ? ", " : "", J->first_frames[k]);
        fprintf(js, "],\n   \"timeline\": [");
        for (int i = 0; i < nfr; i++) {
            fprintf(js, "%s\n    ", i ? "," : "");
            if (J->timeline[i]) json_str(js, J->timeline[i]); else fprintf(js, "null");
        }
        fprintf(js, "\n   ]\n  }%s\n", j + 1 < nsel ? "," : "");
    }
    fprintf(js, " ]\n}\n");
    fclose(js);
    const char *jb = jpath + strlen(jpath);
    while (jb > jpath && jb[-1] != '/' && jb[-1] != '\\') jb--;
    printf("%s written — %ld s\n", jb, (long)(time(NULL) - w_start));
    return 0;
}

static int bytes_save(const char *path, const Bytes *b) {
    FILE *f = fopen_u8(path, "wb");
    if (!f) return 0;
    size_t w = fwrite(b->d, 1, b->n, f);
    return fclose(f) == 0 && w == b->n;
}

static void usage(void) {
    fprintf(stderr,
        "opentvp " OPENTVP_VERSION " — reading and rendering TVPaint 11 files (.tvpp) without TVPaint\n\n"
        "  opentvp info   file.tvpp [--json]\n"
        "  opentvp render file.tvpp folder [options]\n\n"
        "render options:\n"
        "  --clip N            clip to render (1)\n"
        "  --frames A-B        range, from 0 (default: in/out marks, otherwise all visible content)\n"
        "  --bg MODE           project (default) | transparent | #RRGGBB\n"
        "  --scale S           output scale (e.g. 0.5 for a preview)\n"
        "  --blur-samples N    motion blur samples (8)\n"
        "  --threads N         number of threads (default: all cores)\n"
        "  --prefix NAME       image prefix (default: clip name)\n\n"
        "layer-by-layer export (straight alpha images, whole canvas, + JSON):\n"
        "  --layers [LIST]     all visible layers, or \"name,name,3\" (numbers from opentvp info)\n"
        "  --keyframes         one image per drawing + exposure JSON (otherwise every frame)\n"
        "  --camera            every frame, layers framed by the camera\n"
        "  --include-hidden    include hidden layers\n"
        "  --skip-empty        no transparent image when the layer is empty\n"
        "  --png-level N       PNG compression 0-12 (default 6)\n"
        "  --json NAME         JSON file name\n");
}

static void sanitize(char *s) {
    for (; *s; s++) if (*s == '/' || *s == '\\' || *s == ':' || *s == '*' || *s == '?' || *s == '"' || *s == '<' || *s == '>' || *s == '|' || *s == ' ') *s = '_';
}

/* final render setup for a clip (warns about unsupported blending modes) */
static Clip *renderer_setup(Renderer *R, Project *P, int clip, double scale, int blur_samples) {
    if (clip < 1 || clip > P->nclips) die("invalid clip number");
    Clip *C = &P->clips[clip - 1];
    memset(R, 0, sizeof *R);
    R->P = P; R->C = C; R->scale = scale > 0 ? scale : 1; R->blur_samples = blur_samples;
    R->ow = (int)(P->out_w * R->scale + 0.5); R->oh = (int)(P->out_h * R->scale + 0.5);
    if (R->ow < 1) R->ow = 1;
    if (R->oh < 1) R->oh = 1;
    R->nls = C->nlayers;
    R->ls = xcalloc(sizeof(LayerState) * (R->nls + 1));
    for (int i = 0; i < R->nls; i++) {
        layer_init(&R->ls[i], &C->layers[i], P->width, P->height);
        const Layer *L = &C->layers[i];
        int b = L->blend;
        if (L->visible && !(b == 0 || b == 1 || b == 2 || b == 7 || b == 9 || b == 10 || b == 11 || b == 12 || b == 15 || b == 17 || b == 25 || b == 26))
            fprintf(stderr, "  ⚠ layer \"%s\": unsupported mode %s, rendered as normal\n", L->name, blend_name(b));
    }
    return C;
}

/* rendered range: --frames if given (f0 >= 0), otherwise in/out marks, otherwise all visible content */
static void render_range(const Clip *C, int *f0, int *f1) {
    int N = C->visible_frames;
    if (*f0 < 0) {
        if (C->mark_in >= 0 || C->mark_out >= 0) { *f0 = C->mark_in >= 0 ? C->mark_in : 0; *f1 = C->mark_out >= 0 ? C->mark_out : N - 1; }
        else { *f0 = 0; *f1 = N - 1; }
    }
    if (*f1 > N - 1) *f1 = N - 1;
}

/* --bg mode -> 0 (colour in bg) or 1 (transparent) */
static int render_background(const Clip *C, const char *bgmode, float bg[3]) {
    int mode = 0;
    bg[0] = bg[1] = bg[2] = 0;
    if (!strcmp(bgmode, "transparent")) mode = 1;
    else if (bgmode[0] == '#' && strlen(bgmode) >= 7) {
        unsigned r, g, b; sscanf(bgmode + 1, "%2x%2x%2x", &r, &g, &b);
        bg[0] = r / 255.f; bg[1] = g / 255.f; bg[2] = b / 255.f;
    } else if (C->has_bg) {
        /* BGP1 / BGP2: bytes in R G B A order */
        bg[0] = C->bg1[0] / 255.f; bg[1] = C->bg1[1] / 255.f; bg[2] = C->bg1[2] / 255.f;
        if (memcmp(C->bg1, C->bg2, 3) != 0)
            fprintf(stderr, "  ⚠ two-colour project background (probably a checkerboard): rendered on the first one\n");
    }
    return mode;
}

static int cmd_render(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *path = argv[0], *outdir = argv[1];
    int clip = 1, f0 = -1, f1 = -1, blur_samples = 8;
    double scale = 1.0;
    const char *bgmode = "project", *prefix = NULL;
    LayerOpts lo; memset(&lo, 0, sizeof lo);
    int layer_mode = 0, png_level = -1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--layers")) { layer_mode = 1; lo.layers = (i + 1 < argc && strncmp(argv[i + 1], "--", 2)) ? argv[++i] : "all"; continue; }
        if (!strcmp(argv[i], "--keyframes")) { layer_mode = 1; lo.keyframes = 1; continue; }
        if (!strcmp(argv[i], "--camera")) { lo.camera = 1; continue; }
        if (!strcmp(argv[i], "--include-hidden")) { lo.include_hidden = 1; continue; }
        if (!strcmp(argv[i], "--skip-empty")) { lo.skip_empty = 1; continue; }
        if (!strcmp(argv[i], "--png-level") && i + 1 < argc) { png_level = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "--json") && i + 1 < argc) { lo.json = argv[++i]; continue; }
        if (!strcmp(argv[i], "--clip") && i + 1 < argc) clip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { if (sscanf(argv[++i], "%d-%d", &f0, &f1) == 1) f1 = f0; }
        else if (!strcmp(argv[i], "--bg") && i + 1 < argc) bgmode = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--blur-samples") && i + 1 < argc) blur_samples = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) g_threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--prefix") && i + 1 < argc) prefix = argv[++i];
        else { fprintf(stderr, "unknown option: %s\n", argv[i]); usage(); return 2; }
    }
    if (layer_mode) {
        lo.path = path; lo.outdir = outdir; lo.clip = clip; lo.f0 = f0; lo.f1 = f1;
        lo.scale = scale; lo.blur_samples = blur_samples; lo.png_level = png_level >= 0 ? png_level : 6;
        return export_layers(&lo);
    }
    if (lo.camera) fprintf(stderr, "  (--camera only applies with --layers: the final render always goes through the camera)\n");
    clock_t t_start = clock();
    time_t w_start = time(NULL);
    Project P = load_project(path);
    Renderer R;
    Clip *C = renderer_setup(&R, &P, clip, scale, blur_samples);
    render_range(C, &f0, &f1);
    float bg[3];
    int mode = render_background(C, bgmode, bg);
    MKDIR(outdir);
    char pre[512];
    snprintf(pre, sizeof pre, "%s", prefix ? prefix : C->name);
    sanitize(pre);
    printf("%s — clip \"%s\" — frames %d to %d — output %dx%d%s\n", path, C->name, f0, f1, R.ow, R.oh, C->blur > 0 ? " — motion blur" : "");
    g_png_level = png_level >= 0 ? png_level : 6;
    float *acc = xmalloc(sizeof(float) * 4 * (size_t)R.ow * R.oh);
    float *cover = xmalloc(sizeof(float) * (size_t)R.ow * R.oh);
    char fn[1200];
    for (int f = f0; f <= f1; f++) {
        render_frame(&R, f, acc, cover);
        snprintf(fn, sizeof fn, "%s/%s_%04d.png", outdir, pre, f);
        if (!write_png(fn, acc, cover, R.ow, R.oh, mode, bg)) { fprintf(stderr, "write error: %s\n", fn); return 1; }
        printf("\r  frame %d", f); fflush(stdout);
    }
    (void)t_start;
    printf("\n%d images in %ld s\n", f1 - f0 + 1, (long)(time(NULL) - w_start));
    return 0;
}

/* Windows: UTF-8 console output and arguments (paths with accents) */
static void utf8_args(int *argc, char ***argv) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    int wc; wchar_t **wv = CommandLineToArgvW(GetCommandLineW(), &wc);
    if (wv) {
        char **a = malloc(sizeof(char *) * (wc + 1));
        for (int i = 0; i < wc; i++) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, NULL, 0, NULL, NULL);
            a[i] = malloc(n); WideCharToMultiByte(CP_UTF8, 0, wv[i], -1, a[i], n, NULL, NULL);
        }
        a[wc] = NULL;
        *argc = wc; *argv = a;
    }
#else
    (void)argc; (void)argv;
#endif
}

#ifndef OPENTVP_NO_MAIN
int main(int argc, char **argv) {
    utf8_args(&argc, &argv);
    if (argc >= 2 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "version"))) { printf("opentvp %s\n", OPENTVP_VERSION); return 0; }
    if (argc < 3) { usage(); return 2; }
    if (!strcmp(argv[1], "info")) return cmd_info(argv[2], argc > 3 && !strcmp(argv[3], "--json"));
    if (!strcmp(argv[1], "render")) return cmd_render(argc - 2, argv + 2);
    usage();
    return 2;
}
#endif
