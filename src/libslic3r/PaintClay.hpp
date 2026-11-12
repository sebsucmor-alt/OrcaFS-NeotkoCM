// NEOTKO_CLAY_TAG — Clay Tool (2_50, docs/WIP/RELIEF_MODE_2_50_PLAN.md): a 2D shape raised over a
// surface as a new closed part. Std-only core, copied from the Editor lab
// (EDITOR_GITHUB/Sources/CharmCore/ClayCore.hpp), which is an exact transliteration of the Python
// reference docs/WIP/clay_r0/clay_r0.py. Change one, change the others and compare.
// Only difference with the Editor copy: Params::face_visible (the fork tests visibility by ray).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_map>
#include <vector>

namespace Slic3r { namespace clay {

struct V3 { double x = 0, y = 0, z = 0; };
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, double k) { return { a.x * k, a.y * k, a.z * k }; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline double len(V3 a) { return std::sqrt(dot(a, a)); }
struct V2 { double x = 0, y = 0; };
using Ring  = std::vector<V2>;
using Tri   = std::array<int, 3>;

/// La tarjeta: plano por `c`, perpendicular a `d` (unitario, de la pieza hacia quien mira), con su
/// vertical `up`. Un punto de la forma (u, v) en mm cae en c + u·right + v·up. Ortográfica.
struct Card {
    V3 c, d, up, right;
    Card(V3 centre, V3 dir, V3 up0) : c(centre) {
        d = dir * (1.0 / len(dir));
        up = up0 - d * dot(up0, d); up = up * (1.0 / len(up));
        right = cross(up, d);
    }
    // Perspective (fork): seen from `eye`; a point projects onto the card plane along eye→p.
    bool persp = false;
    V3   eye {};
    void set_eye(V3 e) { persp = true; eye = e; }
    V2 to_svg(V3 p) const {
        if (persp) {
            const V3     r  = p - eye;
            const double rd = dot(r, d);
            if (std::abs(rd) > 1e-12) {
                const V3 q = eye + r * (dot(c - eye, d) / rd) - c;
                return { dot(q, right), dot(q, up) };
            }
        }
        const V3 q = p - c; return { dot(q, right), dot(q, up) };
    }
    // Unit vector from p towards whoever looks (d in ortho).
    V3 view(V3 p) const { if (!persp) return d; const V3 r = eye - p; return r * (1.0 / std::max(len(r), 1e-300)); }
};

enum class Profile { Flat, Chamfer, Round, Dome };

struct Params {
    double  height = 0.4;           // mm; la altura máxima
    Profile profile = Profile::Round;
    double  falloff = 0.6;          // mm; la caída del canto (no en Flat ni Dome)
    double  sink = 0.05;            // mm; la piel de abajo, hundida en la pieza para que se funda
    // Fork (R4): engrave = the part goes INTO the object `height` deep and stands `lift` out of it,
    // so as a negative volume it cuts the surface cleanly.
    bool    engrave = false;
    double  lift = 0.4;             // mm; engrave only
    // Fork: several colours in one part (engrave). `rings` is then their union (the outline); each zone
    // keeps its own height (a step where two meet) and its edges split the triangles like the outline.
    std::vector<std::vector<Ring>> zones;
    std::vector<double>            zone_height;
    // Fork: raise only. Where the part lies over one of these zones (an engrave of the same decal) its
    // bottom goes down along the view to that zone's depth, so it still stands on something once cut.
    std::vector<std::vector<Ring>> anchor;
    std::vector<double>            anchor_depth;
    // Fork: raise only. Straight = the part grows along the card's view, like a stamp: over a groove or
    // a sharp edge it copies the shape but cannot fold. Along the normals (false) a groove's walls point
    // sideways and the top crosses itself (30-oct: sticker over engraved veins, torn).
    bool                           straight = true;
    // Fork: raise only. > 0: gaussian sigma (mm) of the surface heights seen from the card; the top is
    // raised from max(real, blurred), so dips and grooves fill and nothing goes down.
    double                         smooth_surface = 0;
    double                         min_cover      = 0.4;   // smooth_surface: least thickness over the real surface
    double                         follow_surface = 0;     // straight: growth follows the surface normal averaged over this radius, mm (0 = the view)
    double                         stretch_max    = 0;     // straight: top stretches over depth jumps up to this, mm (0 = off)
    double                         stretch_slope  = 1;     // ...falling at this slope (1 = 45 degrees)
    double                         min_thick      = 0;     // straight: least thickness along the surface normal, mm (0 = off)
    double  eps = 0.1;              // mm; hasta dónde se parten los triángulos del borde (fork 1-nov: 0,05 → 0,1, el borde ya se corta exacto)
    bool    through = false;        // también lo que no se ve desde la tarjeta
    int     smooth_normals = 2;
    double  max_edge = -1;          // < 0: automático (¼ de la caída, mín. 0,1; sin afinar en Flat)
    double  min_island_mm2 = 0.5;   // migas fuera
    double  slope_lo = 15, slope_hi = 35;   // grados entre superficie y vista; slope_hi <= 0: sin caída
    double  zcell = 0.25, ztol = 0.4;       // visibilidad del prototipo (z-buffer)
    // Fork: when set, replaces the z-buffer. Index into Mesh::f; called only on faces that face the card.
    std::function<bool(int)> face_visible;
    // Fork: polled between steps; true = stop, bake() returns an empty mesh.
    std::function<bool()> cancelled;
};

struct Log {
    long input_tris = 0, candidate_tris = 0, footprint_tris_raw = 0, splits = 0, interior_refines = 0,
         t_junction_faces_fixed = 0, islands = 0, islands_dropped = 0, nonmanifold_footprint_edges = 0,
         boundary_edges = 0, part_verts = 0, part_tris = 0, open_edges = 0, flipped_top = 0, pinches_split = 0, anchored = 0, lifted = 0, thickened = 0, clipped = 0, clip_fallbacks = 0;
    double relief_pct = 0;   // % of the footprint more than 0.2 mm under its 1 mm blur: grooves, sharp edges
    double relief_hist[8] = {}; // same, more than 0.1·(k+1) mm under it: the caller picks the depth that prints
    double volume_mm3 = 0, h_max = 0, h_at_edge_max = 0;
};

struct Mesh { std::vector<V3> v; std::vector<Tri> f; };

// ───────── 2D ─────────
inline bool inside(const std::vector<Ring> &rings, V2 p) {
    bool r = false;
    for (const Ring &ring : rings)
        for (size_t i = 0, n = ring.size(); i < n; ++i) {
            const V2 a = ring[i], b = ring[(i + 1) % n];
            if (((a.y > p.y) != (b.y > p.y)) && (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y + 1e-300) + a.x)) r = !r;
        }
    return r;
}
inline double dist_to_edges(const std::vector<Ring> &rings, V2 p) {
    double d = std::numeric_limits<double>::infinity();
    for (const Ring &ring : rings)
        for (size_t i = 0, n = ring.size(); i < n; ++i) {
            const V2 a = ring[i], b = ring[(i + 1) % n];
            const double abx = b.x - a.x, aby = b.y - a.y, ab2 = std::max(abx * abx + aby * aby, 1e-300);
            const double t = std::clamp(((p.x - a.x) * abx + (p.y - a.y) * aby) / ab2, 0.0, 1.0);
            const double dx = p.x - (a.x + t * abx), dy = p.y - (a.y + t * aby);
            d = std::min(d, std::sqrt(dx * dx + dy * dy));
        }
    return d;
}

// ───────── soldar por posición (redondeo a 1e-7 mm, como el prototipo) ─────────
struct Weld {
    struct K { int64_t x, y, z; bool operator==(const K &o) const { return x == o.x && y == o.y && z == o.z; } };
    struct H { size_t operator()(const K &k) const { return std::hash<int64_t>()(k.x * 73856093 ^ k.y * 19349663 ^ k.z * 83492791); } };
    std::unordered_map<K, int, H> key;
    std::vector<V3> verts;
    int id(V3 p) {
        const K k { std::llround(p.x * 1e7), std::llround(p.y * 1e7), std::llround(p.z * 1e7) };
        auto it = key.find(k);
        if (it != key.end()) return it->second;
        key.emplace(k, int(verts.size())); verts.push_back(p);
        return int(verts.size()) - 1;
    }
};
inline uint64_t ekey(int a, int b) { return (uint64_t(uint32_t(std::min(a, b))) << 32) | uint32_t(std::max(a, b)); }

/// Lo que mira a la tarjeta, toca la forma y se ve (z-buffer con tolerancia que crece con la pendiente).
inline std::vector<Tri> prefilter(const Mesh &m, const Card &card, const std::vector<Ring> &rings, const Params &p, Log &log) {
    V2 lo { 1e300, 1e300 }, hi { -1e300, -1e300 };
    for (const Ring &r : rings) for (V2 q : r) { lo.x = std::min(lo.x, q.x); lo.y = std::min(lo.y, q.y); hi.x = std::max(hi.x, q.x); hi.y = std::max(hi.y, q.y); }
    lo.x -= 1; lo.y -= 1; hi.x += 1; hi.y += 1;
    struct Cand { int f; double z, cosv; int64_t key; };
    std::vector<Cand> cand;
    std::vector<char> keep(m.f.size(), 0);
    for (size_t i = 0; i < m.f.size(); ++i) {
        const V3 a = m.v[m.f[i][0]], b = m.v[m.f[i][1]], c = m.v[m.f[i][2]];
        const V2 ua = card.to_svg(a), ub = card.to_svg(b), uc = card.to_svg(c);
        if (std::max({ ua.x, ub.x, uc.x }) < lo.x || std::max({ ua.y, ub.y, uc.y }) < lo.y ||
            std::min({ ua.x, ub.x, uc.x }) > hi.x || std::min({ ua.y, ub.y, uc.y }) > hi.y) continue;
        const V3 n = cross(b - a, c - a);
        if (p.through) { keep[i] = 1; continue; }
        if (dot(n, card.view((a + b + c) * (1.0 / 3))) <= 0) continue;
        if (p.face_visible) { keep[i] = p.face_visible(int(i)) ? 1 : 0; continue; }
        const V2 cen { (ua.x + ub.x + uc.x) / 3, (ua.y + ub.y + uc.y) / 3 };
        const int64_t ix = int64_t(std::floor((cen.x - lo.x) / p.zcell)), iy = int64_t(std::floor((cen.y - lo.y) / p.zcell));
        const double z = dot((a + b + c) * (1.0 / 3) - card.c, card.d);
        cand.push_back({ int(i), z, dot(n, card.d) / std::max(len(n), 1e-300), ix * 100000 + iy });
    }
    if (!p.through && !p.face_visible) {
        std::unordered_map<int64_t, double> top;
        for (const Cand &c : cand) { auto it = top.find(c.key); if (it == top.end() || c.z > it->second) top[c.key] = c.z; }
        for (const Cand &c : cand) {
            const double cs = std::clamp(c.cosv, 1e-3, 1.0);
            const double tol = p.ztol + 2 * p.zcell * std::min(std::sqrt(1 - cs * cs) / cs, 20.0);
            if (c.z >= top[c.key] - tol) keep[c.f] = 1;
        }
    }
    std::vector<Tri> out;
    for (size_t i = 0; i < m.f.size(); ++i) if (keep[i]) out.push_back(m.f[i]);
    log.input_tris = long(m.f.size()); log.candidate_tris = long(out.size());
    return out;
}

// ───────── Fork (1-nov): exact outline ─────────
// Ring segments bucketed on a grid, and for each ring which side is the inside (left or right).
struct RingIndex {
    const std::vector<Ring> *rings = nullptr;
    double cell = 0.05;
    std::unordered_map<int64_t, std::vector<std::pair<int, int>>> grid;   // cell -> (ring, segment)
    std::vector<char> left_in;                                            // per ring: inside is on the left
    static int64_t key(int64_t ix, int64_t iy) { return (ix << 32) ^ (iy & 0xffffffff); }
    RingIndex(const std::vector<Ring> &rs, double c) : rings(&rs), cell(c) {
        for (int r = 0; r < int(rs.size()); ++r) {
            const Ring &R = rs[size_t(r)];
            const size_t n = R.size();
            for (size_t i = 0; i < n; ++i) {
                const V2 a = R[i], b = R[(i + 1) % n];
                const int64_t x0 = int64_t(std::floor(std::min(a.x, b.x) / cell)), x1 = int64_t(std::floor(std::max(a.x, b.x) / cell));
                const int64_t y0 = int64_t(std::floor(std::min(a.y, b.y) / cell)), y1 = int64_t(std::floor(std::max(a.y, b.y) / cell));
                if ((x1 - x0 + 1) * (y1 - y0 + 1) > 4000000) continue;   // absurd segment: skip rather than hang
                for (int64_t x = x0; x <= x1; ++x)
                    for (int64_t y = y0; y <= y1; ++y) grid[key(x, y)].push_back({ r, int(i) });
            }
            // the side of the longest segment, tested once with the full even-odd rule
            size_t best = 0; double bl = -1;
            for (size_t i = 0; i < n; ++i) {
                const V2 a = R[i], b = R[(i + 1) % n];
                const double l = std::hypot(b.x - a.x, b.y - a.y);
                if (l > bl) { bl = l; best = i; }
            }
            char li = 1;
            if (n >= 3 && bl > 0) {
                const V2 a = R[best], b = R[(best + 1) % n];
                const double ex = (b.x - a.x) / bl, ey = (b.y - a.y) / bl, h = std::min(1e-4, bl * 1e-3);
                li = inside(rs, { (a.x + b.x) / 2 - ey * h, (a.y + b.y) / 2 + ex * h }) ? 1 : 0;
            }
            left_in.push_back(li);
        }
    }
};

inline double cross2(V2 o, V2 a, V2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Ear clipping of a simple polygon (CCW, may have collinear points).
inline void ear_clip(std::vector<int> poly, const std::vector<V2> &uv, std::vector<std::array<int, 3>> &out) {
    size_t guard = 0;
    while (poly.size() > 3 && guard++ < 100000) {
        const size_t n = poly.size();
        bool cut = false;
        for (size_t i = 0; i < n && !cut; ++i) {
            const int a = poly[(i + n - 1) % n], b = poly[i], c = poly[(i + 1) % n];
            const double ar = cross2(uv[a], uv[b], uv[c]);
            if (ar <= 1e-18) {
                if (std::abs(ar) <= 1e-18 && (uv[a].x - uv[b].x) * (uv[c].x - uv[b].x) + (uv[a].y - uv[b].y) * (uv[c].y - uv[b].y) < 0) {
                    poly.erase(poly.begin() + long(i)); cut = true;   // straight-through point: drop, no triangle
                }
                continue;
            }
            bool clean = true;
            for (size_t j = 0; j < n && clean; ++j) {
                const int q = poly[j];
                if (q == a || q == b || q == c) continue;
                if (cross2(uv[a], uv[b], uv[q]) > 0 && cross2(uv[b], uv[c], uv[q]) > 0 && cross2(uv[c], uv[a], uv[q]) > 0) clean = false;
            }
            if (!clean) continue;
            out.push_back({ a, b, c });
            poly.erase(poly.begin() + long(i));
            cut = true;
        }
        if (!cut) break;   // degenerate: give up on the rest
    }
    if (poly.size() == 3 && cross2(uv[poly[0]], uv[poly[1]], uv[poly[2]]) > 1e-18) out.push_back({ poly[0], poly[1], poly[2] });
}

// Cuts each leaf triangle along the outline (rings, even-odd) and keeps the inside. The leaf's sides
// carry the midpoints its neighbours made (no T-junctions). Where the outline crosses a side the point
// is found on the 3D edge (also in perspective) from the side's two vertices in a fixed order, so the
// neighbour on the other side finds the very same point and the weld joins them.
template<class EdgePoints>
inline void clip_leaves(Weld &w, const Card &card, const std::vector<Ring> &rings, const std::vector<Tri> &leaves, EdgePoints &edge_points,
                        std::vector<int> &ring, std::vector<Tri> &conf, Log &log) {
    if (leaves.empty()) return;
    const RingIndex idx(rings, 0.05);
    std::vector<V2> uv;          // local 2D of the window and the new points
    std::vector<int> gid;        // their weld ids (-1 = not made yet)
    for (const Tri &t : leaves) {
        ring.clear();
        ring.push_back(t[0]); edge_points(t[0], t[1]); ring.push_back(t[1]); edge_points(t[1], t[2]); ring.push_back(t[2]); edge_points(t[2], t[0]);
        const V2 c0 = card.to_svg(w.verts[t[0]]), c1 = card.to_svg(w.verts[t[1]]), c2 = card.to_svg(w.verts[t[2]]);
        const double area = cross2(c0, c1, c2);
        auto centre_rule = [&]() {
            const V2 cen { (c0.x + c1.x + c2.x) / 3, (c0.y + c1.y + c2.y) / 3 };
            if (!inside(rings, cen)) return;
            if (ring.size() == 3) { conf.push_back(t); return; }
            const int cid = w.id((w.verts[t[0]] + w.verts[t[1]] + w.verts[t[2]]) * (1.0 / 3));
            for (size_t i = 0; i < ring.size(); ++i) conf.push_back({ ring[i], ring[(i + 1) % ring.size()], cid });
        };
        if (std::abs(area) < 1e-16) { centre_rule(); continue; }
        // window, counter-clockwise in 2D
        std::vector<int> win = ring;
        const bool flip = area < 0;
        if (flip) { std::reverse(win.begin(), win.end()); std::rotate(win.begin(), win.end() - 1, win.end()); }   // starts at t[0] again
        const size_t nw = win.size();
        uv.clear(); gid.clear();
        for (int v : win) { uv.push_back(card.to_svg(w.verts[v])); gid.push_back(v); }
        // The corner triangle CCW. Every side test uses the corner line (never a midpoint's), and a point
        // ON a side counts as outside, so a ring vertex lying exactly on a side is seen the same way by
        // both segments that meet there.
        const V2 Kc[3] = { c0, flip ? c2 : c1, flip ? c1 : c2 };
        auto in_tri = [&](V2 q) { return cross2(Kc[0], Kc[1], q) > 0 && cross2(Kc[1], Kc[2], q) > 0 && cross2(Kc[2], Kc[0], q) > 0; };
        std::vector<int> side_of(nw);   // which corner side each window side lies on
        { int sd = -1; for (size_t k = 0; k < nw; ++k) { if (gid[k] == t[0] || gid[k] == t[1] || gid[k] == t[2]) ++sd; side_of[k] = std::clamp(sd, 0, 2); } }
        double lx = std::min({ c0.x, c1.x, c2.x }), hx = std::max({ c0.x, c1.x, c2.x });
        double ly = std::min({ c0.y, c1.y, c2.y }), hy = std::max({ c0.y, c1.y, c2.y });
        // candidate segments, per ring, sorted
        std::unordered_map<int, std::vector<int>> cand;
        for (int64_t x = int64_t(std::floor(lx / idx.cell)); x <= int64_t(std::floor(hx / idx.cell)); ++x)
            for (int64_t y = int64_t(std::floor(ly / idx.cell)); y <= int64_t(std::floor(hy / idx.cell)); ++y) {
                auto it = idx.grid.find(RingIndex::key(x, y));
                if (it != idx.grid.end()) for (const auto &rs : it->second) cand[rs.first].push_back(rs.second);
            }
        if (cand.empty()) { centre_rule(); continue; }
        // A crossing of a ring segment with window side k (win[k] -> win[k+1]).
        struct Cross { double pos; int id; };   // pos = k + t along the window, CCW
        struct Chord { Cross in, out; std::vector<int> pts; };
        std::vector<Chord> chords;
        bool bad = false;
        auto side_point = [&](size_t k, V2 sa, V2 sb, double &tw, double &ts) {
            const V2 L0 = Kc[side_of[k]], L1 = Kc[(side_of[k] + 1) % 3];
            if ((cross2(L0, L1, sa) > 0) == (cross2(L0, L1, sb) > 0)) return false;   // same side of the line
            const V2 a = uv[k], b = uv[(k + 1) % nw];
            const double d = (b.x - a.x) * (sb.y - sa.y) - (b.y - a.y) * (sb.x - sa.x);
            if (std::abs(d) < 1e-300) return false;
            tw = ((sa.x - a.x) * (sb.y - sa.y) - (sa.y - a.y) * (sb.x - sa.x)) / d;
            ts = ((sa.x - a.x) * (b.y - a.y) - (sa.y - a.y) * (b.x - a.x)) / d;
            return tw >= 0 && tw < 1;
        };
        // The 3D point where segment (sa, sb) crosses the 3D edge between weld ids e0 < e1.
        auto edge_cross = [&](int e0, int e1, V2 sa, V2 sb) {
            const V3 P0 = w.verts[e0], P1 = w.verts[e1];
            auto g = [&](double t) { const V2 q = card.to_svg(P0 + (P1 - P0) * t); return cross2(sa, sb, q); };
            double t0 = 0, t1 = 1, g0 = g(t0), g1 = g(t1);
            for (int it = 0; it < 6 && std::abs(g1 - g0) > 1e-300; ++it) {
                const double t2 = t1 - g1 * (t1 - t0) / (g1 - g0);
                t0 = t1; g0 = g1; t1 = t2; g1 = g(t1);
            }
            t1 = std::clamp(t1, 0.0, 1.0);
            return w.id(P0 + (P1 - P0) * t1);
        };
        for (auto &kv : cand) {
            const Ring &R = rings[size_t(kv.first)];
            const int n = int(R.size());
            std::vector<int> &segs = kv.second;
            std::sort(segs.begin(), segs.end());
            segs.erase(std::unique(segs.begin(), segs.end()), segs.end());
            // start the walk at a candidate whose first point is outside the window
            size_t s0 = segs.size();
            for (size_t i = 0; i < segs.size(); ++i) if (!in_tri(R[size_t(segs[i])])) { s0 = i; break; }
            if (s0 == segs.size()) continue;   // a ring all inside one leaf (a crumb): the centre rule ignores it too
            const bool   li          = idx.left_in[size_t(kv.first)] != 0;
            const size_t first_chord = chords.size();
            bool open = false;
            Chord cur;
            int prev_seg = -2;
            for (size_t c = 0; c < segs.size(); ++c) {
                const int si = segs[(s0 + c) % segs.size()];
                if (open && si != (prev_seg + 1) % n) { bad = true; break; }   // a chord left the candidates
                prev_seg = si;
                const V2 sa = R[size_t(si)], sb = R[size_t((si + 1) % n)];
                // crossings of this segment with every window side, along the segment
                std::vector<std::pair<double, Cross>> xs;
                for (size_t k = 0; k < nw; ++k) {
                    double tw, ts;
                    if (!side_point(k, sa, sb, tw, ts)) continue;
                    const int e0 = std::min(gid[k], gid[(k + 1) % nw]), e1 = std::max(gid[k], gid[(k + 1) % nw]);
                    const int id = edge_cross(e0, e1, sa, sb);
                    xs.push_back({ ts, { double(k) + tw, id } });
                }
                std::sort(xs.begin(), xs.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
                for (const auto &x : xs) {
                    if (!open) { cur = Chord(); cur.in = x.second; open = true; }
                    else { cur.out = x.second; open = false; chords.push_back(cur); }
                }
                if (open && in_tri(sb)) {
                    const V2 q = sb;
                    // lift a ring point inside the leaf onto the leaf's plane (barycentric)
                    const double l0 = cross2(c1, c2, q) / area, l1 = cross2(c2, c0, q) / area, l2 = 1 - l0 - l1;
                    cur.pts.push_back(w.id(w.verts[t[0]] * l0 + w.verts[t[1]] * l1 + w.verts[t[2]] * l2));
                }
            }
            if (open) bad = true;
            if (bad) break;
            // the walk keeps the inside on its left; where this ring has it on the right, turn its chords
            if (!li)
                for (size_t i = first_chord; i < chords.size(); ++i) {
                    std::swap(chords[i].in, chords[i].out);
                    std::reverse(chords[i].pts.begin(), chords[i].pts.end());
                }
        }
        if (bad || chords.empty()) { if (bad) ++log.clip_fallbacks; centre_rule(); continue; }
        ++log.clipped;
        // walk: chord, then the window CCW from its exit to the next entry, ... back to the start
        std::vector<char> used(chords.size(), 0);
        auto next_entry = [&](double from) {
            int best = -1; double bd = 1e300;
            for (size_t i = 0; i < chords.size(); ++i) {
                double d = chords[i].in.pos - from;
                if (d < 0) d += double(nw);
                if (d < bd) { bd = d; best = int(i); }
            }
            return best;
        };
        std::vector<int> poly;
        std::vector<Tri> mine;   // committed only if the whole leaf worked
        for (size_t s = 0; s < chords.size() && !bad; ++s) {
            if (used[s]) continue;
            poly.clear();
            int c = int(s);
            size_t steps = 0;
            while (!bad) {
                if (++steps > chords.size() + 1) { bad = true; break; }
                used[size_t(c)] = 1;
                const Chord &ch = chords[size_t(c)];
                poly.push_back(ch.in.id);
                poly.insert(poly.end(), ch.pts.begin(), ch.pts.end());
                poly.push_back(ch.out.id);
                const int nx = next_entry(ch.out.pos);
                // window corners passed from the exit to that entry
                double a = ch.out.pos, b = chords[size_t(nx)].in.pos;
                if (b < a) b += double(nw);
                for (int k = int(std::floor(a)) + 1; double(k) < b; ++k) poly.push_back(gid[size_t(k % int(nw))]);
                if (nx == int(s)) break;
                if (used[size_t(nx)]) { bad = true; break; }
                c = nx;
            }
            if (bad) break;
            // triangulate in 2D (card), emit with the leaf's 3D orientation
            std::vector<int> local; std::vector<V2> luv; std::unordered_map<int, int> lid;
            for (int g : poly) {
                if (!local.empty() && local.back() == g) continue;
                if (lid.find(g) == lid.end()) { lid[g] = int(luv.size()); luv.push_back(card.to_svg(w.verts[g])); }
                local.push_back(g);
            }
            if (local.size() > 1 && local.front() == local.back()) local.pop_back();
            std::vector<int> lp; for (int g : local) lp.push_back(lid[g]);
            std::vector<int> back(luv.size()); for (auto &kv : lid) back[size_t(kv.second)] = kv.first;
            std::vector<std::array<int, 3>> tr;
            ear_clip(lp, luv, tr);
            for (const auto &q : tr) {
                const Tri f { back[size_t(q[0])], back[size_t(q[1])], back[size_t(q[2])] };
                mine.push_back(flip ? Tri{ f[0], f[2], f[1] } : f);
            }
        }
        if (bad) { ++log.clip_fallbacks; centre_rule(); }
        else conf.insert(conf.end(), mine.begin(), mine.end());
    }
}

/// La pieza Clay de `rings` (anillos de la forma, par/impar) sobre la malla `m`, vista desde `card`.
/// Vacía si la forma no toca nada. La pieza sale cerrada (comprobado en `log.open_edges`).
inline Mesh bake(const Mesh &m, const Card &card, const std::vector<Ring> &rings, Params p, Log &log) {
    auto stop = [&p]() { return p.cancelled && p.cancelled(); };
    if (stop()) return {};
    const std::vector<Tri> F = prefilter(m, card, rings, p, log);
    if (stop()) return {};
    double max_edge = p.max_edge;
    if (max_edge < 0) max_edge = p.profile == Profile::Flat ? std::numeric_limits<double>::infinity()
                                                              : std::max(0.1, (p.profile != Profile::Dome ? p.falloff : 2.0) / 4);
    Weld w;
    std::vector<Tri> tris; tris.reserve(F.size());
    for (const Tri &t : F) tris.push_back({ w.id(m.v[t[0]]), w.id(m.v[t[1]]), w.id(m.v[t[2]]) });
    std::unordered_map<uint64_t, int> mids;
    auto mid = [&](int a, int b) {
        const uint64_t k = ekey(a, b);
        auto it = mids.find(k);
        if (it != mids.end()) return it->second;
        const int id = w.id((w.verts[a] + w.verts[b]) * 0.5);
        mids.emplace(k, id);
        return id;
    };
    auto facing = [&](const Tri &t) { return p.through || dot(cross(w.verts[t[1]] - w.verts[t[0]], w.verts[t[2]] - w.verts[t[0]]), card.view(w.verts[t[0]])) > 0; };
    auto d2 = [](V2 a, V2 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y)); };

    // 1. huella
    std::vector<Tri> out, stack;
    for (const Tri &t : tris) {
        if (!facing(t)) continue;
        const V2 u0 = card.to_svg(w.verts[t[0]]), u1 = card.to_svg(w.verts[t[1]]), u2 = card.to_svg(w.verts[t[2]]);
        const V2 cen { (u0.x + u1.x + u2.x) / 3, (u0.y + u1.y + u2.y) / 3 };
        const bool all_in = inside(rings, u0) && inside(rings, u1) && inside(rings, u2) && inside(rings, cen);
        if (all_in && std::isinf(max_edge) && dist_to_edges(rings, cen) > std::max({ d2(u0, cen), d2(u1, cen), d2(u2, cen) })) { out.push_back(t); continue; }
        stack.push_back(t);
    }
    std::vector<Ring> zone_edges;
    for (const std::vector<Ring> &z : p.zones) zone_edges.insert(zone_edges.end(), z.begin(), z.end());
    for (const std::vector<Ring> &z : p.anchor) zone_edges.insert(zone_edges.end(), z.begin(), z.end());
    std::vector<Tri> leaves;   // Fork (1-nov): smallest triangles the outline crosses
    size_t polled = 0;
    while (!stack.empty()) {
        if ((++polled & 0x3fff) == 0 && stop()) return {};
        const Tri t = stack.back(); stack.pop_back();
        const V3 P0 = w.verts[t[0]], P1 = w.verts[t[1]], P2 = w.verts[t[2]];
        const V2 u0 = card.to_svg(P0), u1 = card.to_svg(P1), u2 = card.to_svg(P2);
        const double L  = std::max({ d2(u0, u1), d2(u1, u2), d2(u2, u0) });
        const double L3 = std::max({ len(P0 - P1), len(P1 - P2), len(P2 - P0) });
        const V2 cen { (u0.x + u1.x + u2.x) / 3, (u0.y + u1.y + u2.y) / 3 };
        const double dmin = dist_to_edges(rings, cen);
        auto split = [&]() {
            const int ab = mid(t[0], t[1]), bc = mid(t[1], t[2]), ca = mid(t[2], t[0]);
            stack.push_back({ t[0], ab, ca }); stack.push_back({ t[1], bc, ab }); stack.push_back({ t[2], ca, bc }); stack.push_back({ ab, bc, ca });
        };
        if (dmin > L) {       // lejos del borde: entero dentro o fuera
            if (!inside(rings, cen)) continue;
            if (!zone_edges.empty() && L > p.eps && dist_to_edges(zone_edges, cen) <= L) { ++log.splits; split(); continue; }
            // Finer triangles only where the height changes: the falloff band (Chamfer / Round) or the
            // whole shape (Dome). Further in, the top is a constant height over the surface as it is.
            const bool flat_here = (p.profile == Profile::Chamfer || p.profile == Profile::Round) && dmin - L > p.falloff;
            if (L3 <= max_edge || flat_here) { out.push_back(t); continue; }
            ++log.interior_refines; split(); continue;
        }
        if (L <= p.eps) { leaves.push_back(t); continue; }   // the outline crosses it: cut exactly in step 2
        ++log.splits; split();
    }
    log.footprint_tris_raw = long(out.size() + leaves.size());
    if (out.empty() && leaves.empty()) return {};

    if (stop()) return {};
    // 2. sin T-junctions: cada cara con los puntos que le salieron en sus lados, en abanico desde su centro
    std::vector<int> ring;
    std::function<void(int, int)> edge_points = [&](int a, int b) {
        auto it = mids.find(ekey(a, b));
        if (it == mids.end()) return;
        const int mm = it->second;
        edge_points(a, mm); ring.push_back(mm); edge_points(mm, b);
    };
    std::vector<Tri> conf; conf.reserve(out.size());
    for (const Tri &t : out) {
        ring.clear();
        ring.push_back(t[0]); edge_points(t[0], t[1]); ring.push_back(t[1]); edge_points(t[1], t[2]); ring.push_back(t[2]); edge_points(t[2], t[0]);
        if (ring.size() == 3) { conf.push_back(t); continue; }
        ++log.t_junction_faces_fixed;
        const int cid = w.id((w.verts[t[0]] + w.verts[t[1]] + w.verts[t[2]]) * (1.0 / 3));
        for (size_t i = 0; i < ring.size(); ++i) conf.push_back({ ring[i], ring[(i + 1) % ring.size()], cid });
    }
    if (stop()) return {};
    // Fork (1-nov): the triangles the outline crosses are cut along it, not kept or dropped whole by
    // their centre. Before, the edge was a staircase of 0.05 mm teeth: on small text the slicer's walls
    // came out torn (Classic) or wobbly (Arachne), and the part had millions of triangles.
    clip_leaves(w, card, rings, leaves, edge_points, ring, conf, log);

    // reindexar a lo usado
    std::vector<int> remap(w.verts.size(), -1);
    std::vector<V3> P;
    std::vector<int> used;
    for (const Tri &t : conf) for (int i : t) if (remap[i] < 0) { remap[i] = 0; used.push_back(i); }
    std::sort(used.begin(), used.end());
    for (int i : used) { remap[i] = int(P.size()); P.push_back(w.verts[i]); }
    std::vector<Tri> T; T.reserve(conf.size());
    for (const Tri &t : conf) T.push_back({ remap[t[0]], remap[t[1]], remap[t[2]] });

    if (stop()) return {};
    // islas
    std::vector<int> parent(P.size());
    for (size_t i = 0; i < P.size(); ++i) parent[i] = int(i);
    std::function<int(int)> find = [&](int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
    for (const Tri &t : T) { parent[find(t[1])] = find(t[0]); parent[find(t[2])] = find(t[0]); }
    std::vector<int> island(P.size());
    for (size_t i = 0; i < P.size(); ++i) island[i] = find(int(i));
    { std::vector<int> s = island; std::sort(s.begin(), s.end()); log.islands = long(std::unique(s.begin(), s.end()) - s.begin()); }
    if (p.min_island_mm2 > 0) {
        std::unordered_map<int, double> area;
        for (const Tri &t : T) area[island[t[0]]] += 0.5 * len(cross(P[t[1]] - P[t[0]], P[t[2]] - P[t[0]]));
        for (auto &kv : area) if (kv.second < p.min_island_mm2) ++log.islands_dropped;
        std::vector<Tri> T2;
        for (const Tri &t : T) if (area[island[t[0]]] >= p.min_island_mm2) T2.push_back(t);
        std::vector<int> rm(P.size(), -1); std::vector<V3> P2; std::vector<int> isl2;
        std::vector<char> u(P.size(), 0);
        for (const Tri &t : T2) for (int i : t) u[i] = 1;
        for (size_t i = 0; i < P.size(); ++i) if (u[i]) { rm[i] = int(P2.size()); P2.push_back(P[i]); isl2.push_back(island[i]); }
        for (Tri &t : T2) t = { rm[t[0]], rm[t[1]], rm[t[2]] };
        P.swap(P2); T.swap(T2); island.swap(isl2);
    }
    if (T.empty()) return {};

    // Fork: vértices "pellizco" (el contorno se toca a sí mismo en un punto, o dos islas comparten
    // un vértice): sin esto, la arista vertical de la pared en ese punto la comparten 4 caras y
    // la pieza no es manifold (las booleanas y el split fallan). Cada abanico de caras alrededor
    // del vértice se queda su propia copia, metida 0,01 mm hacia su abanico para que no se toquen.
    {
        std::vector<std::vector<int>> vt(P.size());
        for (size_t f = 0; f < T.size(); ++f) for (int k = 0; k < 3; ++k) vt[T[f][k]].push_back(int(f));
        const size_t np = P.size();
        for (size_t v = 0; v < np; ++v) {
            const std::vector<int> &fs = vt[v];
            if (fs.size() < 2) continue;
            // abanicos: caras de v unidas si comparten una arista que sale de v
            std::vector<int> fp(fs.size());
            for (size_t i = 0; i < fs.size(); ++i) fp[i] = int(i);
            std::function<int(int)> ff = [&](int x) { while (fp[x] != x) { fp[x] = fp[fp[x]]; x = fp[x]; } return x; };
            std::unordered_map<int, int> by_other;   // otro extremo de una arista de v → cara local
            for (size_t i = 0; i < fs.size(); ++i)
                for (int k = 0; k < 3; ++k) {
                    const int o = T[fs[i]][k];
                    if (o == int(v)) continue;
                    auto it = by_other.find(o);
                    if (it == by_other.end()) by_other[o] = int(i); else fp[ff(int(i))] = ff(it->second);
                }
            std::unordered_map<int, std::vector<int>> fans;
            for (size_t i = 0; i < fs.size(); ++i) fans[ff(int(i))].push_back(fs[i]);
            if (fans.size() < 2) continue;
            ++log.pinches_split;
            bool first = true;
            for (auto &kv : fans) {
                int vid = int(v);
                if (!first) { vid = int(P.size()); P.push_back(P[v]); island.push_back(island[v]); }
                V3 c { 0, 0, 0 }; int nc = 0;
                for (int f : kv.second) for (int k = 0; k < 3; ++k) if (T[f][k] != int(v)) { c = c + P[T[f][k]]; ++nc; }
                for (int f : kv.second) for (int k = 0; k < 3; ++k) if (T[f][k] == int(v)) T[f][k] = vid;
                const V3 dv = c * (1.0 / std::max(nc, 1)) - P[v];
                const double l = len(dv);
                if (l > 1e-9) P[vid] = P[v] + dv * (std::min(0.01, 0.25 * l) / l);
                first = false;
            }
        }
    }

    // bordes: aristas de una sola cara
    std::unordered_map<uint64_t, int> ec;
    for (const Tri &t : T) for (int k = 0; k < 3; ++k) ++ec[ekey(t[k], t[(k + 1) % 3])];
    for (auto &kv : ec) if (kv.second > 2) ++log.nonmanifold_footprint_edges;
    std::vector<std::array<int, 2>> bnd;
    for (const Tri &t : T) for (int k = 0; k < 3; ++k) if (ec[ekey(t[k], t[(k + 1) % 3])] == 1) bnd.push_back({ t[k], t[(k + 1) % 3] });
    log.boundary_edges = long(bnd.size());

    if (stop()) return {};
    // 3. altura
    const size_t n = P.size();
    std::vector<double> d(n), H(n), hz(n, p.height);
    for (size_t i = 0; i < n; ++i) d[i] = dist_to_edges(rings, card.to_svg(P[i]));
    if (!p.zones.empty())   // height of the zone the point is in (on a shared edge: the nearest)
        for (size_t i = 0; i < n; ++i) {
            const V2 u = card.to_svg(P[i]);
            double best = std::numeric_limits<double>::infinity();
            for (size_t z = 0; z < p.zones.size() && z < p.zone_height.size(); ++z) {
                if (inside(p.zones[z], u)) { hz[i] = p.zone_height[z]; break; }
                const double dz = dist_to_edges(p.zones[z], u);
                if (dz < best) { best = dz; hz[i] = p.zone_height[z]; }
            }
        }
    if (p.profile == Profile::Dome) {
        std::unordered_map<int, double> dmax;
        for (size_t i = 0; i < n; ++i) dmax[island[i]] = std::max(dmax[island[i]], d[i]);
        for (size_t i = 0; i < n; ++i) { const double t = d[i] / std::max(dmax[island[i]], 1e-9); H[i] = hz[i] * t * t * (3 - 2 * t); }
    } else
        for (size_t i = 0; i < n; ++i) {
            const double t = std::clamp(d[i] / std::max(p.falloff, 1e-9), 0.0, 1.0);
            const double pr = p.profile == Profile::Flat ? 1.0 : p.profile == Profile::Chamfer ? t : std::sqrt(std::clamp(1 - (1 - t) * (1 - t), 0.0, 1.0));
            H[i] = hz[i] * pr;
        }

    // 3b. the surface heights seen from the card, as an image: blurred (gaussian) to flatten bumps and dips when asked
    // (smooth_surface), and always at 1 mm to measure how much sharp relief lies under the part.
    std::vector<V3> B = P;   // the surface the top is raised from
    if (!p.engrave && n > 0) {
        std::vector<V2> uv(n);
        std::vector<double> e(n);
        double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
        for (size_t i = 0; i < n; ++i) {
            uv[i] = card.to_svg(P[i]);
            e[i]  = dot(P[i] - card.c, card.d);   // elevation towards the card
            u0 = std::min(u0, uv[i].x); u1 = std::max(u1, uv[i].x); v0 = std::min(v0, uv[i].y); v1 = std::max(v1, uv[i].y);
        }
        auto blur_diff = [&](double sigma, bool raw, std::vector<float> &diff, std::vector<char> &has, double &g, int &nx, int &ny) {
            g  = std::max(sigma / 3, 0.05);
            nx = int((u1 - u0) / g) + 2; ny = int((v1 - v0) / g) + 2;
            if (double(nx) * ny > 2e7) return false;
            std::vector<double> h(size_t(nx) * ny, -1e300);
            for (const Tri &t : T) {   // the triangles fill the cells under them, the highest wins
                const V2 a = uv[t[0]], b = uv[t[1]], c = uv[t[2]];
                const double den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
                if (std::abs(den) < 1e-18) continue;
                for (int y = std::max(0, int((std::min({ a.y, b.y, c.y }) - v0) / g)); y <= std::min(ny - 1, int((std::max({ a.y, b.y, c.y }) - v0) / g)); ++y)
                    for (int x = std::max(0, int((std::min({ a.x, b.x, c.x }) - u0) / g)); x <= std::min(nx - 1, int((std::max({ a.x, b.x, c.x }) - u0) / g)); ++x) {
                        const V2 q { u0 + (x + 0.5) * g, v0 + (y + 0.5) * g };
                        const double l0 = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / den;
                        const double l1 = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / den, l2 = 1 - l0 - l1;
                        if (l0 < -1e-9 || l1 < -1e-9 || l2 < -1e-9) continue;
                        double &cc = h[size_t(y) * nx + x];
                        cc = std::max(cc, l0 * e[t[0]] + l1 * e[t[1]] + l2 * e[t[2]]);
                    }
            }
            // Separable gaussian, normalised by the weight of the cells that exist (edges, holes).
            std::vector<double> hs(h.size(), 0), hw(h.size(), 0);
            has.assign(h.size(), 0);
            for (size_t k = 0; k < h.size(); ++k) if (h[k] > -1e299) { hs[k] = h[k]; hw[k] = 1; has[k] = 1; }
            const int R = int(std::ceil(3 * sigma / g));
            std::vector<double> ker(2 * size_t(R) + 1);
            for (int k = -R; k <= R; ++k) ker[size_t(k + R)] = std::exp(-0.5 * (k * g / sigma) * (k * g / sigma));
            auto pass = [&](std::vector<double> &a, bool horiz) {
                std::vector<double> o(a.size(), 0);
                for (int y = 0; y < ny; ++y) for (int x = 0; x < nx; ++x) {
                    double s2 = 0;
                    for (int k = -R; k <= R; ++k) {
                        const int X = horiz ? x + k : x, Y = horiz ? y : y + k;
                        if (X >= 0 && Y >= 0 && X < nx && Y < ny) s2 += ker[size_t(k + R)] * a[size_t(Y) * nx + X];
                    }
                    o[size_t(y) * nx + x] = s2;
                }
                a.swap(o);
            };
            pass(hs, true); pass(hs, false); pass(hw, true); pass(hw, false);
            diff.assign(h.size(), 0.f);
            for (size_t k = 0; k < h.size(); ++k) if (has[k] && hw[k] > 1e-9) diff[k] = float(raw ? hs[k] / hw[k] : std::max(0.0, hs[k] / hw[k] - h[k]));
            return true;
        };
        double g; int nx, ny;
        std::vector<float> diff; std::vector<char> has;
        if (blur_diff(1.0, false, diff, has, g, nx, ny)) {
            size_t cells = 0, sharp = 0, deeper[8] = {};
            for (size_t k = 0; k < diff.size(); ++k) if (has[k]) {
                ++cells; if (diff[k] > 0.2f) ++sharp;
                for (int j = 0; j < 8; ++j) if (diff[k] > 0.1f * float(j + 1)) ++deeper[j];
            }
            log.relief_pct = cells ? 100.0 * double(sharp) / double(cells) : 0.;
            for (int j = 0; j < 8; ++j) log.relief_hist[j] = cells ? 100.0 * double(deeper[j]) / double(cells) : 0.;
        }
        if (stop()) return {};
        if (p.smooth_surface > 0 && blur_diff(p.smooth_surface, true, diff, has, g, nx, ny))
            for (size_t i = 0; i < n; ++i) {   // read back smoothly (bilinear over the cell centres)
                const double fx = (uv[i].x - u0) / g - 0.5, fy = (uv[i].y - v0) / g - 0.5;
                const int    x0 = std::clamp(int(std::floor(fx)), 0, nx - 2), y0 = std::clamp(int(std::floor(fy)), 0, ny - 2);
                const double tx = std::clamp(fx - x0, 0.0, 1.0), ty = std::clamp(fy - y0, 0.0, 1.0);
                double fs = 0, fw = 0;   // over the cells that exist only (an empty one reads 0: square dents)
                auto add = [&](int x, int y, double w) { const size_t k = size_t(y) * nx + x; if (has[k] && w > 0) { fs += w * diff[k]; fw += w; } };
                add(x0, y0, (1 - tx) * (1 - ty)); add(x0 + 1, y0, tx * (1 - ty)); add(x0, y0 + 1, (1 - tx) * ty); add(x0 + 1, y0 + 1, tx * ty);
                if (fw < 1e-9) continue;
                // The base is the blurred surface, up AND down: bumps (raised veins) sink into the part,
                // grooves fill; the top still covers the real surface by at least min_cover.
                const double lift = std::max(fs / fw - e[i], std::min(0.0, p.min_cover - H[i]));
                if (std::abs(lift) > 0.01) { B[i] = P[i] + card.d * lift; ++log.lifted; }
            }
    }

    // 4. normales (ponderadas por ángulo) y suavizado
    std::vector<V3> N(n);
    for (const Tri &t : T) {
        const V3 q[3] = { B[t[0]], B[t[1]], B[t[2]] };
        V3 nn = cross(q[1] - q[0], q[2] - q[0]);
        const double ln = len(nn);
        if (ln == 0) continue;
        nn = nn * (1 / ln);
        for (int k = 0; k < 3; ++k) {
            const V3 e1 = q[(k + 1) % 3] - q[k], e2 = q[(k + 2) % 3] - q[k];
            const double ang = std::acos(std::clamp(dot(e1, e2) / (len(e1) * len(e2) + 1e-300), -1.0, 1.0));
            N[t[k]] = N[t[k]] + nn * ang;
        }
    }
    for (V3 &v : N) v = v * (1 / std::max(len(v), 1e-300));
    if (p.smooth_normals > 0) {
        std::vector<std::vector<int>> nb(n);
        for (const Tri &t : T) { nb[t[0]].push_back(t[1]); nb[t[0]].push_back(t[2]); nb[t[1]].push_back(t[0]); nb[t[1]].push_back(t[2]); nb[t[2]].push_back(t[0]); nb[t[2]].push_back(t[1]); }
        for (int it = 0; it < p.smooth_normals; ++it) {
            if (stop()) return {};
            std::vector<V3> N2(n);
            for (size_t i = 0; i < n; ++i) { V3 s = N[i]; for (int j : nb[i]) s = s + N[j]; N2[i] = s * (1 / std::max(len(s), 1e-300)); }
            N.swap(N2);
        }
    }
    if (p.slope_hi > 0) {
        const double lo_c = std::sin(p.slope_lo * M_PI / 180), hi_c = std::sin(p.slope_hi * M_PI / 180);
        for (size_t i = 0; i < n; ++i) {
            const double t = std::clamp((std::clamp(dot(N[i], card.view(P[i])), 0.0, 1.0) - lo_c) / (hi_c - lo_c), 0.0, 1.0);
            H[i] *= t * t * (3 - 2 * t);
        }
    }

    if (stop()) return {};
    // 5. pieles y paredes
    Mesh r;
    // Direction field: the surface's normal averaged over ~R mm (seen from the card), so the growth
    // follows the shape slowly enough that neighbours' tops can't cross (R about twice the height).
    std::vector<V3> Dir;
    if (p.follow_surface > 0 && !p.engrave && n > 0) {
        const double R = p.follow_surface, g = std::max(R / 4, 0.25);
        double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
        std::vector<V2> uv(n);
        for (size_t i = 0; i < n; ++i) { uv[i] = card.to_svg(P[i]); u0 = std::min(u0, uv[i].x); u1 = std::max(u1, uv[i].x); v0 = std::min(v0, uv[i].y); v1 = std::max(v1, uv[i].y); }
        const int nx = int((u1 - u0) / g) + 2, ny = int((v1 - v0) / g) + 2;
        std::vector<V3> acc(size_t(nx) * ny, V3 { 0, 0, 0 });
        for (const Tri &t : T) {
            const V3 nn = cross(P[t[1]] - P[t[0]], P[t[2]] - P[t[0]]);   // area-weighted
            const V2 cu { (uv[t[0]].x + uv[t[1]].x + uv[t[2]].x) / 3, (uv[t[0]].y + uv[t[1]].y + uv[t[2]].y) / 3 };
            const int x = std::clamp(int((cu.x - u0) / g), 0, nx - 1), y = std::clamp(int((cu.y - v0) / g), 0, ny - 1);
            acc[size_t(y) * nx + x] = acc[size_t(y) * nx + x] + nn;
        }
        const int K = int(std::ceil(2.5 * R / g));
        std::vector<double> ker(2 * size_t(K) + 1);
        for (int k = -K; k <= K; ++k) ker[size_t(k + K)] = std::exp(-0.5 * (k * g / R) * (k * g / R));
        auto pass = [&](bool horiz) {
            std::vector<V3> o(acc.size(), V3 { 0, 0, 0 });
            for (int y = 0; y < ny; ++y) for (int x = 0; x < nx; ++x) {
                V3 s2 { 0, 0, 0 };
                for (int k = -K; k <= K; ++k) {
                    const int X = horiz ? x + k : x, Y = horiz ? y : y + k;
                    if (X >= 0 && Y >= 0 && X < nx && Y < ny) s2 = s2 + acc[size_t(Y) * nx + X] * ker[size_t(k + K)];
                }
                o[size_t(y) * nx + x] = s2;
            }
            acc.swap(o);
        };
        pass(true); pass(false);
        Dir.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const double fx = (uv[i].x - u0) / g - 0.5, fy = (uv[i].y - v0) / g - 0.5;
            const int x0 = std::clamp(int(std::floor(fx)), 0, std::max(nx - 2, 0)), y0 = std::clamp(int(std::floor(fy)), 0, std::max(ny - 2, 0));
            const double tx = std::clamp(fx - x0, 0.0, 1.0), ty = std::clamp(fy - y0, 0.0, 1.0);
            auto at = [&](int x, int y) { return acc[size_t(std::min(y, ny - 1)) * nx + std::min(x, nx - 1)]; };
            V3 d = (at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx) * (1 - ty) + (at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx) * ty;
            const double l = len(d);
            Dir[i] = l > 1e-12 ? d * (1 / l) : card.view(P[i]);
        }
    }
    r.v.resize(2 * n);
    // Engrave: outer skin above the surface, inner one (the floor) H deep; same faces, same winding.
    // Pushed along the card's view, like a stamp, not along the normals: on a fine noisy surface
    // (voxel remesh, 0.03 mm triangles) normals offset 1-2 mm deep crumple and cross the floor, and
    // booleans then fail. Along one direction the floor is a height field over the card: it cannot fold.
    if (p.engrave)
        for (size_t i = 0; i < n; ++i) { const V3 v = card.view(P[i]); r.v[i] = P[i] + v * p.lift; r.v[i + n] = P[i] - v * H[i]; }
    else
        for (size_t i = 0; i < n; ++i) {
            const V3 v = Dir.empty() ? card.view(P[i]) : Dir[i];
            // Straight: top and bottom along the view, the part a stamp of the surface. The bottom stays on
            // the real surface (P), so under a smoothed top the part fills the dips.
            r.v[i]     = p.straight ? B[i] + v * H[i] : B[i] + N[i] * H[i];
            r.v[i + n] = p.straight ? P[i] - v * p.sink : P[i] - N[i] * p.sink;
            const V2 u = card.to_svg(P[i]);
            for (size_t z = 0; z < p.anchor.size() && z < p.anchor_depth.size(); ++z)
                // Clearly inside only: a raise whose outline IS the engrave's edge (the SVG cut a hole in the
                // engraved colour for it) stands on uncut surface; deciding there point by point made a comb.
                if (inside(p.anchor[z], u) && dist_to_edges(p.anchor[z], u) > 2 * p.eps) { r.v[i + n] = P[i] - card.view(P[i]) * p.anchor_depth[z]; ++log.anchored; break; }
        }
    const int ni = int(n);

    // Stretch over gaps (elastic top): where the surface jumps in depth (rim -> socket), the top stretches down from the
    // higher side at slope k instead of splitting in two layers; only for jumps up to p.stretch_max mm.
    if (p.stretch_max > 0 && p.stretch_slope > 0 && !p.engrave && n > 0) {
        std::vector<V2> uv(n); std::vector<double> et(n);
        double u0 = 1e300, v0 = 1e300, u1 = -1e300, v1 = -1e300;
        for (size_t i = 0; i < n; ++i) { uv[i] = card.to_svg(P[i]); et[i] = dot(r.v[i] - card.c, card.d);
            u0 = std::min(u0, uv[i].x); u1 = std::max(u1, uv[i].x); v0 = std::min(v0, uv[i].y); v1 = std::max(v1, uv[i].y); }
        const double g = 0.15;
        const int nx = int((u1 - u0) / g) + 2, ny = int((v1 - v0) / g) + 2;
        std::vector<double> top(size_t(nx) * ny, -1e300);
        for (const Tri &t : T) {
            const V2 a = uv[t[0]], b = uv[t[1]], c = uv[t[2]];
            const double den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::abs(den) < 1e-18) continue;
            for (int y = std::max(0, int((std::min({ a.y, b.y, c.y }) - v0) / g)); y <= std::min(ny - 1, int((std::max({ a.y, b.y, c.y }) - v0) / g)); ++y)
                for (int x = std::max(0, int((std::min({ a.x, b.x, c.x }) - u0) / g)); x <= std::min(nx - 1, int((std::max({ a.x, b.x, c.x }) - u0) / g)); ++x) {
                    const V2 q { u0 + (x + 0.5) * g, v0 + (y + 0.5) * g };
                    const double l0 = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / den;
                    const double l1 = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / den, l2 = 1 - l0 - l1;
                    if (l0 < -1e-9 || l1 < -1e-9 || l2 < -1e-9) continue;
                    double &cc = top[size_t(y) * nx + x];
                    cc = std::max(cc, l0 * et[t[0]] + l1 * et[t[1]] + l2 * et[t[2]]);
                }
        }
        // top image per cell = the highest top over it; then a cone max: T(x) = max_n top(n) - k*|x-n|,
        // taking n only when it's at most p.stretch_max above the local top (no bridges over huge drops).
        const int R = int(std::ceil(p.stretch_max / p.stretch_slope / g));
        std::vector<std::array<int, 3>> disc;
        for (int dy = -R; dy <= R; ++dy) for (int dx = -R; dx <= R; ++dx) { const double dd = std::sqrt(double(dx * dx + dy * dy)) * g; if (dd <= R * g && (dx || dy)) disc.push_back({ dx, dy, int(std::lround(dd * 1000)) }); }
        std::vector<double> T2(top);
        for (int y = 0; y < ny; ++y) for (int x = 0; x < nx; ++x) {
            const double here = top[size_t(y) * nx + x];
            if (here < -1e299) continue;
            double best = here;
            for (auto &dd : disc) {
                const int X = x + dd[0], Y = y + dd[1];
                if (X < 0 || Y < 0 || X >= nx || Y >= ny) continue;
                const double o = top[size_t(Y) * nx + X];
                if (o < -1e299 || o - here > p.stretch_max) continue;
                best = std::max(best, o - p.stretch_slope * dd[2] * 0.001);
            }
            T2[size_t(y) * nx + x] = best;
        }
        for (size_t i = 0; i < n; ++i) {
            const int x = std::clamp(int((uv[i].x - u0) / g), 0, nx - 1), y = std::clamp(int((uv[i].y - v0) / g), 0, ny - 1);
            const double tgt = T2[size_t(y) * nx + x];
            if (tgt > et[i] + 0.01) { const V3 v = Dir.empty() ? card.view(P[i]) : Dir[i]; r.v[i] = r.v[i] + v * ((tgt - et[i]) / std::max(dot(v, card.d), 0.2)); }
        }
    }
    // Least thickness along the normal: where the skin is seen edge-on from the card, a top grown towards the
    // view slides along it and the part is a sliver thinner than a line, which the slicer drops (black gaps
    // in the paint, 30-oct). Push such tops out along the normal; the falloff edge keeps its taper (H < min).
    if (p.straight && p.min_thick > 0 && !p.engrave)
        for (size_t i = 0; i < n; ++i) {
            if (H[i] <= 1e-6) continue;
            const double want = std::min(p.min_thick, H[i]);
            const double have = dot(r.v[i] - P[i], N[i]);
            if (have < want) { r.v[i] = r.v[i] + N[i] * (want - have); ++log.thickened; }
        }
    for (const Tri &t : T) r.f.push_back(t);
    for (const Tri &t : T) r.f.push_back({ t[2] + ni, t[1] + ni, t[0] + ni });
    for (auto e : bnd) { r.f.push_back({ e[1], e[0], e[0] + ni }); r.f.push_back({ e[1], e[0] + ni, e[1] + ni }); }

    // 6. comprobaciones
    std::unordered_map<uint64_t, int> de;   // arista dirigida a→b
    auto dk = [](int a, int b) { return (uint64_t(uint32_t(a)) << 32) | uint32_t(b); };
    for (const Tri &t : r.f) for (int k = 0; k < 3; ++k) ++de[dk(t[k], t[(k + 1) % 3])];
    for (auto &kv : de) { auto it = de.find(dk(int(kv.first & 0xffffffff), int(kv.first >> 32))); if (it == de.end() || it->second != kv.second) ++log.open_edges; }
    double vol = 0;
    for (const Tri &t : r.f) vol += dot(r.v[t[0]], cross(r.v[t[1]], r.v[t[2]]));
    log.volume_mm3 = vol / 6;
    for (const Tri &t : T) {
        const V3 n0 = cross(B[t[1]] - B[t[0]], B[t[2]] - B[t[0]]), n1 = cross(r.v[t[1]] - r.v[t[0]], r.v[t[2]] - r.v[t[0]]);
        if (dot(n0, n1) <= 0 && len(n0) > 1e-12) ++log.flipped_top;
    }
    log.part_verts = long(r.v.size()); log.part_tris = long(r.f.size());
    for (double h : H) log.h_max = std::max(log.h_max, h);
    for (auto e : bnd) log.h_at_edge_max = std::max(log.h_at_edge_max, H[e[0]]);
    return r;
}

/// Soldar una sopa de triángulos (x,y,z ×3 por triángulo) en malla indexada.
inline Mesh from_soup(const float *T, size_t ntri) {
    Mesh m; Weld w;
    m.f.reserve(ntri);
    for (size_t i = 0; i < ntri; ++i) {
        Tri t;
        for (int k = 0; k < 3; ++k) t[k] = w.id({ T[9 * i + 3 * k], T[9 * i + 3 * k + 1], T[9 * i + 3 * k + 2] });
        m.f.push_back(t);
    }
    m.v = std::move(w.verts);
    return m;
}

}} // namespace Slic3r::clay

// ───────── Fork side (PaintClay.cpp): decals → Clay parts ─────────
#include "Point.hpp"
#include "admesh/stl.h"
#include <string>

namespace Slic3r {
struct PaintDecal;
struct PaintSurfacePath;
class ModelObject;
class ModelVolume;
class AABBMesh;

// NEOTKO_SURFACE_TAG — On surface: one point of a line pasted on the mesh (object frame), with the
// surface normal there.
struct PaintSurfaceSample
{
    Vec3d p = Vec3d::Zero();
    Vec3d n = Vec3d::UnitZ();
};
// The dense curve of `path` on `aabb`: each segment a cubic between two clicks (their handles),
// 6 samples, each pasted by a ray along its own normal (not from the camera: that misses what lies
// behind), then halved until neighbours are <= 0.3 mm apart. Measured in the Editor at <= 0.02 mm
// from a sphere and a cylinder (3-nov). `gaps` = segments still > 1 mm apart (a jump).
std::vector<PaintSurfaceSample> paint_surface_curve(const AABBMesh& aabb, const PaintSurfacePath& path, int* gaps = nullptr);
// Pastes one point: ray along +-n up to `reach` mm, nearest hit to p wins. False = nothing there.
bool paint_surface_paste(const AABBMesh& aabb, Vec3d& p, Vec3d& n, double reach);
// The outline the line raises, in 3D: an open line's ribbon (`width` wide, on the tangent plane) or a
// closed line's own points. What the decal's flat SVG and its layer icon are drawn from.
std::vector<Vec3d> paint_surface_outline(const std::vector<PaintSurfaceSample>& curve, const PaintSurfacePath& path);
// Half the ribbon's width at each sample, mm: width / 2 all along, or for Wrap the brush's width
// model (taper, min width, drops...) run along the line's length, Size = path.width.
std::vector<double> paint_surface_half_widths(const std::vector<PaintSurfaceSample>& curve, const PaintSurfacePath& path);

// One Clay part: what one colour (group) of one decal copy raises. Object frame.
struct PaintClayPiece
{
    int                  group  = -1;   // paint_decal_groups() group; -1 = the decal's engrave (all engraved colours)
    int                  state  = 1;    // filament it paints with
    int                  mirror = 0;    // 0 = the decal, k = its k-th mirrored copy
    bool                 engrave = false;   // a negative volume, sunk into the object
    indexed_triangle_set its;
    clay::Log            log;
};

// The base the Clay sits on: the object's model parts in the object frame, without Clay parts.
indexed_triangle_set paint_clay_base(const ModelObject& mo);
// Every Clay part of `decal` (and its mirrored copies) over `base`, with decal.clay settings
// (decal.clay.on is not checked). Visibility by ray from each face towards the card, as decal_apply()
// paints; through = everything. Perspective cards project from their eye, like the paint.
// Can run on a worker thread: `cancelled` is polled often (true = stop, the result is then empty),
// `progress(done, total)` is called after each piece, counted over colours × mirror copies.
std::vector<PaintClayPiece> paint_clay_bake(const indexed_triangle_set& base, const PaintDecal& decal, const Vec3d& mirror_centre,
                                            const std::function<bool()>& cancelled = {},
                                            const std::function<void(int, int)>& progress = {});
// Name of a Clay part (group -1 = the engrave), and whether a volume is one (and of which decal, by name).
std::string paint_clay_volume_name(const PaintDecal& decal, int group);
bool        paint_clay_is_volume_of(const ModelVolume& v, const PaintDecal& decal);
bool        paint_clay_is_volume(const ModelVolume& v);
// Clay parts find their decal by name, so two decals with one name steal each other's parts (and the
// auto-rebuild loops). Gives decals[idx] a name no other decal has: "name 2", "name 3"...
void        paint_decal_unique_name(std::vector<PaintDecal>& decals, size_t idx);
} // namespace Slic3r
