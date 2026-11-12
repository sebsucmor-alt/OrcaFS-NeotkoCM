// NEOTKO_VECTORBRUSH_TAG — Vector Brush (2_50): a hand-drawn stroke made "natural" (Procreate-like) and
// turned into a filled outline, ready to become a decal's SVG path.
// Only std: the same file goes to the Editor (ControlCenterApp). It is a transliteration of the
// pipeline in docs/WIP/BRUSHES_BOCETO.html and was checked against it point by point (31-oct-2026).
// Plan: docs/WIP/BRUSHES_PLAN_2_50.md.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Slic3r { namespace stroke {

struct Pt { double x = 0, y = 0, p = 1, t = 0; };   // screen px, pressure 0..1, time ms
struct XY { double x = 0, y = 0; };

enum class Curve { Soft, Linear, Hard };
enum class Engine { Sampled, Quill };
// How the width moves along the stroke: Speed (fast = thin, at once), Ink (like a quill: the line
// swells where the hand slows and keeps that inertia, after microbians' quill brush), Constant.
enum class Width { Speed, Ink, Constant };
// Brush effects (4-nov-2026, prototype docs/FUTURE/BRUSH_EFFECTS_PROTO.html): one stroke becomes many
// small shapes (strands, dots, dashes...). None = the plain stroke.
enum class Effect { None, Hair, Stipple, Dashed, Grass, Calligraphy, TwoTone };

struct Params
{
    Engine engine = Engine::Quill;
    double size   = 14;   // px, full width
    double stream = 35;   // StreamLine, 0..95 %
    Curve  curve  = Curve::Soft;
    double tin    = 15;   // taper in, % of the length
    double tout   = 30;   // taper out, % of the length
    double minw   = 25;   // min width, % of size
    double speed  = 45;   // without pressure: how much speed thins, %
    double step   = 1.0;  // resample step, px
    double simplify = 0.15; // RDP tolerance, px
    Width  width  = Width::Speed;
    double inertia = 40;  // Ink: how slowly the width follows the hand, %
    double drops   = 0;   // ink drops: random swellings, %
    Effect fx      = Effect::None;
    double fx_amount = 50;   // density (Hair, Grass, Stipple) or how little gap (Dashed), %
    double fx_len    = 300;  // strand / blade / dash length, % of size
    double fx_jit    = 40;   // randomness, %
    double fx_angle  = 25;   // Hair lean, Grass sway, Calligraphy nib angle, degrees
    double fx_dot    = 40;   // strand root / dot size, % of size; Calligraphy: thin stroke, % of the nib
    int    fx_mode   = 1;    // Two-tone: 0 = dither (the halo in the stroke's own colour), 1 = two tones

    bool operator==(const Params &o) const
    {
        return engine == o.engine && size == o.size && stream == o.stream && curve == o.curve && tin == o.tin && tout == o.tout &&
               minw == o.minw && speed == o.speed && step == o.step && simplify == o.simplify && width == o.width &&
               inertia == o.inertia && drops == o.drops && fx == o.fx && fx_amount == o.fx_amount && fx_len == o.fx_len &&
               fx_jit == o.fx_jit && fx_angle == o.fx_angle && fx_dot == o.fx_dot && fx_mode == o.fx_mode;
    }
    bool operator!=(const Params &o) const { return !(*this == o); }
};

inline double curve_map(const Params &P, double p)
{
    if (P.curve == Curve::Soft) return std::pow(p, 0.6);
    if (P.curve == Curve::Hard) return std::pow(p, 1.8);
    return p;
}

// 1. StreamLine: the tip follows the input with an elastic lag, then catches up with the pen.
inline std::vector<Pt> stabilize(const Params &P, const std::vector<Pt> &raw)
{
    std::vector<Pt> out;
    if (raw.empty()) return out;
    const double k = 1 - P.stream / 100 * 0.92;
    double x = raw[0].x, y = raw[0].y;
    for (const Pt &q : raw) { x += (q.x - x) * k; y += (q.y - y) * k; out.push_back({ x, y, q.p, q.t }); }
    const Pt &last = raw.back();
    for (int i = 0; i < 6 && P.stream > 0; ++i) { x += (last.x - x) * 0.5; y += (last.y - y) * 0.5; out.push_back({ x, y, last.p, last.t }); }
    return out;
}

// 2. Resample at a fixed step along the length; pressure and time interpolated.
inline std::vector<Pt> resample(const std::vector<Pt> &pts, double step)
{
    if (pts.size() < 2) return pts;
    std::vector<Pt> out{ pts[0] };
    double carry = 0;
    for (size_t i = 1; i < pts.size(); ++i) {
        const Pt &a = pts[i - 1], &b = pts[i];
        const double d = std::hypot(b.x - a.x, b.y - a.y);
        double s = step - carry;
        while (s <= d) {
            const double f = s / d;
            out.push_back({ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.p + (b.p - a.p) * f, a.t + (b.t - a.t) * f });
            s += step;
        }
        carry = d - (s - step);
    }
    const Pt &L = pts.back();
    if (std::hypot(L.x - out.back().x, L.y - out.back().y) > step * 0.3) out.push_back(L);
    return out;
}

// 3. Width per point: pressure (pen) or speed (mouse), smoothed, then tapered at both ends.
inline std::vector<double> widths(const Params &P, const std::vector<Pt> &pts, bool pen)
{
    const size_t n = pts.size();
    std::vector<double> w(n);
    const double minf = P.minw / 100;
    for (size_t i = 0; i < n; ++i) {
        double f;
        if (pen) f = curve_map(P, pts[i].p);
        else {
            const size_t j = i >= 3 ? i - 3 : 0;
            const double dt = std::max(1.0, pts[i].t - pts[j].t);
            const double v  = std::hypot(pts[i].x - pts[j].x, pts[i].y - pts[j].y) / dt;   // px per ms
            f = 1 - (P.speed / 100) * std::min(1.0, v / 2.5);
        }
        w[i] = std::max(minf, f);
    }
    if (P.width == Width::Constant)
        std::fill(w.begin(), w.end(), 1.0);
    else if (P.width == Width::Ink && n > 0) {
        // low-pass over the distance travelled: the slower the hand, the more ink pools
        const double L = std::max(1.0, P.size * (0.2 + P.inertia / 100 * 6));
        double f = w[0];
        for (size_t i = 0; i < n; ++i) {
            const double ds = i ? std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y) : 0;
            f += (w[i] - f) * (1 - std::exp(-ds / L));
            w[i] = f;
        }
    }
    if (P.drops > 0 && n > 0) {
        // drops: now and then the width swells and fades back; seeded by where the stroke starts so a
        // rebuild gives the same stroke (32-bit LCG, same numbers as the sketch)
        uint32_t seed = uint32_t(int64_t(std::floor(pts[0].x * 7 + pts[0].y * 13)) & 0x7fffffff) | 1u;
        auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return double(seed >> 8) / 16777216.0; };
        double bump = 0, acc = 0;
        for (size_t i = 0; i < n; ++i) {
            const double ds = i ? std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y) : 0;
            bump *= std::exp(-ds / P.size);   // fades over about one Size of length
            acc += ds;
            while (acc >= P.size) {           // one chance per Size of length
                acc -= P.size;
                if (rnd() * 100 < P.drops) bump = std::max(bump, 0.3 + 0.7 * rnd());
            }
            w[i] *= 1 + bump;
        }
    }
    for (int it = 0; it < 4; ++it)
        for (size_t i = 1; i + 1 < n; ++i) w[i] = (w[i - 1] + 2 * w[i] + w[i + 1]) / 4;   // in place, like the sketch
    double len = 0;
    std::vector<double> acc{ 0 };
    for (size_t i = 1; i < n; ++i) { len += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y); acc.push_back(len); }
    const double ti = len * P.tin / 100, to = len * P.tout / 100;
    for (size_t i = 0; i < n; ++i) {
        double f = 1;
        if (ti > 0 && acc[i] < ti) f = std::min(f, acc[i] / ti);
        if (to > 0 && len - acc[i] < to) f = std::min(f, (len - acc[i]) / to);
        f = f * f * (3 - 2 * f);
        w[i] = P.size * std::max(w[i] * f, minf * 0.35 * (f > 0 ? 1 : 0.6));
    }
    return w;
}

// 4. Outline: left side forward, round cap, right side back, round cap.
inline std::vector<XY> outline(const std::vector<Pt> &pts, const std::vector<double> &w)
{
    const size_t n = pts.size();
    std::vector<XY> poly;
    if (n == 0) return poly;
    if (n == 1) {
        for (int k = 0; k < 16; ++k) { const double a = k / 16.0 * 2 * M_PI; poly.push_back({ pts[0].x + std::cos(a) * w[0] / 2, pts[0].y + std::sin(a) * w[0] / 2 }); }
        return poly;
    }
    std::vector<XY> L, R;
    for (size_t i = 0; i < n; ++i) {
        const Pt &a = pts[i > 0 ? i - 1 : 0], &b = pts[std::min(n - 1, i + 1)];
        double dx = b.x - a.x, dy = b.y - a.y;
        double l = std::hypot(dx, dy); if (l == 0) l = 1;
        dx /= l; dy /= l;
        const double h = w[i] / 2;
        L.push_back({ pts[i].x - dy * h, pts[i].y + dx * h });
        R.push_back({ pts[i].x + dy * h, pts[i].y - dx * h });
    }
    auto cap = [&poly](const Pt &c, const XY &from, double h) {
        const double a0 = std::atan2(from.y - c.y, from.x - c.x);
        for (int k = 1; k < 8; ++k) { const double a = a0 - k / 8.0 * M_PI; poly.push_back({ c.x + std::cos(a) * h, c.y + std::sin(a) * h }); }
    };
    poly = L;
    cap(pts[n - 1], L[n - 1], w[n - 1] / 2);
    for (size_t i = n; i-- > 0;) poly.push_back(R[i]);
    cap(pts[0], R[0], w[0] / 2);
    return poly;
}

// 5. Simplify (Ramer-Douglas-Peucker) with an explicit stack, same order as the sketch.
inline std::vector<XY> rdp(const std::vector<XY> &pts, double eps)
{
    if (pts.size() < 4) return pts;
    std::vector<char> keep(pts.size(), 0);
    keep[0] = keep[pts.size() - 1] = 1;
    std::vector<std::pair<size_t, size_t>> st{ { 0, pts.size() - 1 } };
    while (!st.empty()) {
        const auto [i, j] = st.back(); st.pop_back();
        long best = -1; double bd = eps;
        const double ax = pts[i].x, ay = pts[i].y, dx = pts[j].x - ax, dy = pts[j].y - ay;
        double l = std::hypot(dx, dy); if (l == 0) l = 1;
        for (size_t k = i + 1; k < j; ++k) {
            const double d = std::abs((pts[k].x - ax) * dy - (pts[k].y - ay) * dx) / l;
            if (d > bd) { bd = d; best = long(k); }
        }
        if (best >= 0) { keep[size_t(best)] = 1; st.push_back({ i, size_t(best) }); st.push_back({ size_t(best), j }); }
    }
    std::vector<XY> out;
    for (size_t k = 0; k < pts.size(); ++k) if (keep[k]) out.push_back(pts[k]);
    return out;
}

struct Built { std::vector<Pt> center; std::vector<XY> ring; };

inline Built build(const Params &P, const std::vector<Pt> &raw, bool pen)
{
    Built b;
    if (raw.empty()) return b;
    b.center = resample(stabilize(P, raw), P.step);
    b.ring   = rdp(outline(b.center, widths(P, b.center, pen)), P.simplify);
    return b;
}

// A number with 2 decimals and a '.', whatever the locale: printf follows LC_NUMERIC, and with the UI in
// Spanish, German... it wrote "12,34", which an SVG reads as two numbers (audit 1-nov).
inline void put_num(std::string &d, double v)
{
    long long q = std::llround(v * 100.0);
    if (q < 0) { d += '-'; q = -q; }
    d += std::to_string(q / 100);
    d += '.';
    const int f = int(q % 100);
    d += char('0' + f / 10);
    d += char('0' + f % 10);
}

// 6. Closed ring -> SVG path with cubic Béziers (Catmull-Rom through the points).
inline std::string to_path(const std::vector<XY> &ring)
{
    const size_t n = ring.size();
    if (n < 3) return {};
    std::string d = "M ";
    put_num(d, ring[0].x); d += ' '; put_num(d, ring[0].y);
    for (size_t i = 0; i < n; ++i) {
        const XY &p0 = ring[(i + n - 1) % n], &p1 = ring[i], &p2 = ring[(i + 1) % n], &p3 = ring[(i + 2) % n];
        const XY c1{ p1.x + (p2.x - p0.x) / 6, p1.y + (p2.y - p0.y) / 6 }, c2{ p2.x - (p3.x - p1.x) / 6, p2.y - (p3.y - p1.y) / 6 };
        d += " C ";
        for (double v : { c1.x, c1.y, c2.x, c2.y, p2.x }) { put_num(d, v); d += ' '; }
        put_num(d, p2.y);
    }
    return d + " Z";
}


// ---- Quill engine -------------------------------------------------------------------------------
// Variable-width stroke by offsetting quadratic Béziers, by Gabriel Suchowolski (microbians), MIT:
// https://github.com/microbians/bezier (the stroke demo) · https://github.com/microbians/sktch ·
// the maths: https://microbians.com/math ("Quadratic bezier offsetting with selective subdivision",
// "Quadratic bezier through three points"). Transliteration of quillBuild() in BRUSHES_BOCETO.html.
// The centre line is a chain of quads (midpoint -> input point -> midpoint). Each side is that chain
// offset by half the width: split at the point nearest the control (a cubic) and where the normal turns
// more than 45°, and each piece's offset control is nc + m * d / |m|^2 (m = sum of the end normals).

struct QSeg { XY c, p; };                     // quadratic piece: control, end point
struct QRing { XY start; std::vector<QSeg> segs; };

namespace qd {
inline XY add(XY a, XY b) { return { a.x + b.x, a.y + b.y }; }
inline XY sub(XY a, XY b) { return { a.x - b.x, a.y - b.y }; }
inline XY mul(XY a, double k) { return { a.x * k, a.y * k }; }
inline double dot(XY a, XY b) { return a.x * b.x + a.y * b.y; }
inline double len(XY a) { return std::hypot(a.x, a.y); }
inline XY lrp(XY a, XY b, double t) { return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t }; }
inline XY uni(XY a) { double l = std::hypot(a.x, a.y); if (l == 0) l = 1; return { a.x / l, a.y / l }; }
inline XY per(XY a) { return { -a.y, a.x }; }
struct Quad { XY p1, pc, p2; double d1, d2; };
inline XY at(const Quad &b, double t) { return lrp(lrp(b.p1, b.pc, t), lrp(b.pc, b.p2, t), t); }
inline XY ctl(const Quad &b, double t0, double t1) { return lrp(lrp(b.p1, b.pc, t0), lrp(b.pc, b.p2, t0), t1); }
inline XY nrm(const Quad &b, double t)
{
    XY d = add(mul(add(sub(b.p1, mul(b.pc, 2)), b.p2), t), sub(b.pc, b.p1));
    if (len(d) < 1e-9) d = sub(b.p2, b.p1);
    return uni(per(d));
}
// t of the curve point closest to the control point (his closestTtoPc: roots of a cubic)
inline double bend_t(const Quad &b)
{
    const XY v0 = sub(b.pc, b.p1), v1 = sub(b.p2, b.pc), w = sub(v1, v0);
    const double c0 = -dot(v0, v0), c1 = 3 * dot(v0, v0) - dot(v1, v0), c2 = 3 * (dot(v1, v0) - dot(v0, v0)), c3 = dot(w, w);
    const double l0 = len(v0), l1 = len(v1), fb = l0 + l1 > 0 ? l0 / (l0 + l1) : 0.5;
    if (c3 < 1e-12) return fb;
    const double a1 = c2 / c3, a2 = c1 / c3, a3 = c0 / c3, Q = (a1 * a1 - 3 * a2) / 9, R = (2 * a1 * a1 * a1 - 9 * a1 * a2 + 27 * a3) / 54, d = Q * Q * Q - R * R;
    std::vector<double> roots;
    if (d >= 0) {
        const double th = std::acos(std::max(-1.0, std::min(1.0, R / std::sqrt(Q * Q * Q)))), s = std::sqrt(Q);
        for (int k : { 0, 2, 4 }) roots.push_back(-2 * s * std::cos((th + k * M_PI) / 3) - a1 / 3);
    } else {
        double e = std::pow(std::sqrt(-d) + std::abs(R), 1.0 / 3); if (R > 0) e = -e;
        roots.push_back(e + Q / e - a1 / 3);
    }
    for (double t : roots) if (t >= 0 && t <= 1 && std::isfinite(t)) return t;
    return fb;
}
inline void cap(std::vector<QSeg> &out, XY c, XY from, double r)   // half circle as 4 quads, clockwise like outline()
{
    const double a0 = std::atan2(from.y - c.y, from.x - c.x), step = -M_PI / 4, rc = r / std::cos(M_PI / 8);
    for (int k = 0; k < 4; ++k) {
        const double am = a0 + step * (k + .5), a1 = a0 + step * (k + 1);
        out.push_back({ { c.x + std::cos(am) * rc, c.y + std::sin(am) * rc }, { c.x + std::cos(a1) * r, c.y + std::sin(a1) * r } });
    }
}
} // namespace qd

inline QRing quill(const Params &P, const std::vector<Pt> &raw, bool pen)
{
    using namespace qd;
    QRing ring;
    if (raw.empty()) return ring;
    const std::vector<Pt> st = stabilize(P, raw);
    const double gap = std::max(4.0, P.size * 0.6);
    std::vector<Pt> q{ st[0] };
    for (const Pt &p : st) if (std::hypot(p.x - q.back().x, p.y - q.back().y) >= gap) q.push_back(p);
    // the JS keeps `last` unless it is the very object already pushed: same test by value
    { const Pt &L = st.back(), &B = q.back(); if (q.size() == 1 || !(L.x == B.x && L.y == B.y && L.p == B.p && L.t == B.t)) q.push_back(L); }
    const std::vector<double> w = widths(P, q, pen);
    const size_t n = q.size();
    auto xy = [](const Pt &p) { return XY{ p.x, p.y }; };
    if (n < 2 || std::hypot(q[n - 1].x - q[0].x, q[n - 1].y - q[0].y) < 0.5) {   // a dot: 8 quads
        const double r = w[0] / 2, rc = r / std::cos(M_PI / 8);
        ring.start = { q[0].x + r, q[0].y };
        for (int k = 0; k < 8; ++k) {
            const double am = (k + .5) * M_PI / 4, a1 = (k + 1) * M_PI / 4;
            ring.segs.push_back({ { q[0].x + std::cos(am) * rc, q[0].y + std::sin(am) * rc }, { q[0].x + std::cos(a1) * r, q[0].y + std::sin(a1) * r } });
        }
        return ring;
    }
    std::vector<Quad> segs;
    if (n == 2) segs.push_back({ xy(q[0]), lrp(xy(q[0]), xy(q[1]), .5), xy(q[1]), w[0], w[1] });
    for (size_t i = 1; i + 1 < n; ++i) {
        const XY p1 = i == 1 ? xy(q[0]) : lrp(xy(q[i - 1]), xy(q[i]), .5), p2 = i == n - 2 ? xy(q[n - 1]) : lrp(xy(q[i]), xy(q[i + 1]), .5);
        const double d1 = i == 1 ? w[0] : (w[i - 1] + w[i]) / 2, d2 = i == n - 2 ? w[n - 1] : (w[i] + w[i + 1]) / 2;
        segs.push_back({ p1, xy(q[i]), p2, d1, d2 });
    }
    struct Side { XY a, c, b; };
    std::vector<Side> L, R;
    for (const Quad &b : segs) {
        std::vector<double> ts{ 0, 1 };
        const double tb = bend_t(b);
        if (tb > 0.02 && tb < 0.98) ts.insert(ts.begin() + 1, tb);
        for (size_t k = 0; k + 1 < ts.size() && ts.size() < 9;) {
            if (dot(nrm(b, ts[k]), nrm(b, ts[k + 1])) < std::cos(M_PI / 4)) ts.insert(ts.begin() + k + 1, (ts[k] + ts[k + 1]) / 2);
            else ++k;
        }
        auto dAt = [&b](double t) { return b.d1 + (b.d2 - b.d1) * t; };
        for (size_t k = 0; k + 1 < ts.size(); ++k) {
            const double t0 = ts[k], t1 = ts[k + 1];
            const XY a = at(b, t0), c = at(b, t1), nc = ctl(b, t0, t1), na = nrm(b, t0), nb = nrm(b, t1), m = add(na, nb);
            const double m2 = std::max(1e-6, dot(m, m)), dm = dAt((t0 + t1) / 2), h0 = dAt(t0) / 2, h1 = dAt(t1) / 2;
            L.push_back({ add(a, mul(na, h0)), add(nc, mul(m, dm / m2)), add(c, mul(nb, h1)) });
            R.push_back({ sub(a, mul(na, h0)), sub(nc, mul(m, dm / m2)), sub(c, mul(nb, h1)) });
        }
    }
    ring.start = L[0].a;
    for (const Side &s : L) ring.segs.push_back({ s.c, s.b });
    cap(ring.segs, xy(q[n - 1]), L.back().b, w[n - 1] / 2);
    for (size_t i = R.size(); i-- > 0;) ring.segs.push_back({ R[i].c, R[i].a });
    cap(ring.segs, xy(q[0]), R[0].a, w[0] / 2);
    return ring;
}

inline std::string quill_path(const QRing &ring)
{
    if (ring.segs.empty()) return {};
    std::string d = "M ";
    put_num(d, ring.start.x); d += ' '; put_num(d, ring.start.y);
    for (const QSeg &s : ring.segs) {
        d += " Q ";
        put_num(d, s.c.x); d += ' '; put_num(d, s.c.y); d += ' '; put_num(d, s.p.x); d += ' '; put_num(d, s.p.y);
    }
    return d + " Z";
}

// The SVG path of one stroke with the chosen engine, and the points that bound it (curve ends and
// controls: a quad lies inside them).
inline std::string stroke_path(const Params &P, const std::vector<Pt> &raw, bool pen, std::vector<XY> &bound)
{
    if (P.engine == Engine::Quill) {
        const QRing r = quill(P, raw, pen);
        bound.push_back(r.start);
        for (const QSeg &s : r.segs) { bound.push_back(s.c); bound.push_back(s.p); }
        return quill_path(r);
    }
    const Built b = build(P, raw, pen);
    bound.insert(bound.end(), b.ring.begin(), b.ring.end());
    return to_path(b.ring);
}

// The outline of one stroke as a closed polyline (Quill curves flattened, `tol` px of error), for the
// eraser's boolean and hit tests.
inline std::vector<XY> stroke_ring(const Params &P, const std::vector<Pt> &raw, bool pen, double tol = 0.25)
{
    if (P.engine != Engine::Quill) return build(P, raw, pen).ring;
    const QRing r = quill(P, raw, pen);
    std::vector<XY> out;
    if (r.segs.empty()) return out;
    XY a = r.start;
    out.push_back(a);
    for (const QSeg &s : r.segs) {
        // a quad's distance from its chord is at most |a - 2c + p| / 4
        const double dev = std::hypot(a.x - 2 * s.c.x + s.p.x, a.y - 2 * s.c.y + s.p.y) / 4;
        const int    k   = std::max(1, std::min(32, int(std::ceil(std::sqrt(dev / tol)))));
        for (int i = 1; i <= k; ++i) {
            const double t = double(i) / k, u = 1 - t;
            out.push_back({ u * u * a.x + 2 * u * t * s.c.x + t * t * s.p.x, u * u * a.y + 2 * u * t * s.c.y + t * t * s.p.y });
        }
        a = s.p;
    }
    out.pop_back();   // the last point closes back on start
    return out;
}

// ---- Brush effects ------------------------------------------------------------------------------
// Every effect is a list of pieces, each a centre line with a full width per point (one point = a dot),
// in the same screen px as the stroke. The screen draws them as dots + quads, the saved shape is the
// union of their outlines. Seeded by where the stroke starts, so a rebuild gives the same strands.
struct Piece { std::vector<Pt> c; std::vector<double> w; int tone = 0; };   // tone 1 = Two-tone's second colour

inline std::vector<Piece> effect_pieces(const Params &P, const std::vector<Pt> &raw, bool pen)
{
    std::vector<Piece> out;
    if (raw.empty()) return out;
    const std::vector<Pt>     c = resample(stabilize(P, raw), P.step);
    const std::vector<double> w = widths(P, c, pen);
    const size_t n = c.size();
    if (P.fx == Effect::None || n < 2) { out.push_back({ c, w }); return out; }
    // audit 4-nov: a tiny Size with high density made tens of thousands of dots or strands (each a
    // polygon in the union and the SVG); past this many the effect stops adding pieces.
    const size_t cap = 4000;
    uint32_t seed = uint32_t(int64_t(std::floor(raw[0].x * 7 + raw[0].y * 13)) & 0x7fffffff) | 1u;
    auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return double(seed >> 8) / 16777216.0; };
    const double S = std::max(1.0, P.size), jit = P.fx_jit / 100, deg = M_PI / 180;
    auto tan_at = [&](size_t i, double &tx, double &ty) {
        const Pt &a = c[i ? i - 1 : 0], &b = c[std::min(n - 1, i + 1)];
        tx = b.x - a.x; ty = b.y - a.y;
        double l = std::hypot(tx, ty); if (l == 0) l = 1;
        tx /= l; ty /= l;
    };
    std::vector<double> s(n, 0);
    for (size_t i = 1; i < n; ++i) s[i] = s[i - 1] + std::hypot(c[i].x - c[i - 1].x, c[i].y - c[i - 1].y);
    // a strand: from (x, y) along (dx, dy), turning a little each step, tapered from root to tip
    auto strand = [&](double x, double y, double dx, double dy, double len, double root, double bend, double tip) {
        Piece pc;
        const int N = std::max(4, int(len / 2));
        for (int j = 0; j <= N; ++j) {
            const double u = double(j) / N;
            pc.c.push_back({ x, y, 1, 0 });
            pc.w.push_back(std::max(root * tip, root * (1 - u * u * (2 - tip))));
            const double cb = std::cos(bend), sb = std::sin(bend), nx = dx * cb - dy * sb, ny = dx * sb + dy * cb;
            dx = nx; dy = ny; x += dx * len / N; y += dy * len / N;
        }
        out.push_back(std::move(pc));
    };
    if (P.fx == Effect::Calligraphy) {
        Piece pc{ c, w };
        const double thin = P.fx_dot / 100, nib = P.fx_angle * deg;
        for (size_t i = 0; i < n; ++i) {
            double tx, ty; tan_at(i, tx, ty);
            pc.w[i] *= thin + (1 - thin) * std::abs(std::sin(std::atan2(ty, tx) - nib));
        }
        out.push_back(std::move(pc));
        return out;
    }
    if (P.fx == Effect::TwoTone) {
        // a solid core (Size) and a halo of dots round it that thins out towards its edge; with two
        // tones the dots are the second colour, with dither the stroke's own (a see-through edge)
        out.push_back({ c, w, 0 });
        const double dot = S * P.fx_dot / 100, halo = S * P.fx_len / 100, per = P.fx_amount / 100 * 0.25;
        const double fall = 0.3 + 3.7 * jit;   // how fast the halo thins out
        const int    tone = 0;   // 4-nov: one colour; two colours = two brushes layered (Neotko)
        double acc = 0;
        for (size_t i = 1; i < n; ++i) {
            const double env = w[i] / S, hb = halo * env;
            acc += (s[i] - s[i - 1]) * per * (2 * hb / std::max(1e-6, dot));
            double tx, ty; tan_at(i, tx, ty);
            while (acc >= 1 && out.size() < cap) {
                acc -= 1;
                const double u = rnd();
                if (rnd() > std::pow(1 - u, fall)) continue;   // fewer dots further out
                const double side = rnd() < 0.5 ? -1 : 1, off = side * (w[i] / 2 + u * hb), along = (rnd() - 0.5) * 2 * P.step;
                const double d = dot * (0.7 + 0.6 * rnd()) * std::max(0.4, env);
                out.push_back({ { { c[i].x - ty * off + tx * along, c[i].y + tx * off + ty * along, 1, 0 } }, { d }, tone });
            }
        }
        return out;
    }
    if (P.fx == Effect::Dashed) {
        const double dash = std::max(2.0, S * P.fx_len / 100), gap = dash * (1.05 - P.fx_amount / 100);
        size_t i = 0;
        while (i < n) {
            const double s0 = s[i];
            Piece pc;
            for (; i < n && s[i] - s0 <= dash; ++i) { pc.c.push_back(c[i]); pc.w.push_back(w[i]); }
            if (pc.c.size() >= 2) out.push_back(std::move(pc));
            while (i < n && s[i] - s0 < dash + gap) ++i;
        }
        return out;
    }
    if (P.fx == Effect::Stipple) {
        const double dot = S * P.fx_dot / 100, per = P.fx_amount / 100 * 0.25;   // dots per px of length, per Size
        double acc = 0;
        for (size_t i = 1; i < n; ++i) {
            acc += (s[i] - s[i - 1]) * per * (w[i] / dot);
            double tx, ty; tan_at(i, tx, ty);
            while (acc >= 1 && out.size() < cap) {
                acc -= 1;
                const double u = rnd() * 2 - 1;
                if (rnd() < jit * std::abs(u)) continue;   // fewer dots towards the edge
                const double off = u * w[i] / 2, along = (rnd() - 0.5) * 2 * P.step, d = dot * (1 - 0.5 * jit * rnd());
                out.push_back({ { { c[i].x - ty * off + tx * along, c[i].y + tx * off + ty * along, 1, 0 } }, { d } });
            }
        }
        return out;
    }
    // Hair and Grass: one strand every `gap` px along the stroke
    const double gap = std::max(1.0, S * (1.2 - P.fx_amount / 100 * 1.05) * (P.fx == Effect::Grass ? 0.6 : 0.35));
    for (double at = 0; at <= s[n - 1] && out.size() < cap; at += gap * (0.7 + 0.6 * rnd())) {
        size_t i = 0;
        while (i + 1 < n && s[i + 1] < at) ++i;
        double tx, ty; tan_at(i, tx, ty);
        const double env = w[i] / S;   // the stroke's taper thins the strands too
        if (env <= 0.02) continue;
        const double len  = S * P.fx_len / 100 * (1 - 0.6 * jit * rnd()) * std::sqrt(env);
        const double root = std::max(1.0, S * P.fx_dot / 100 * env);
        if (P.fx == Effect::Hair) {
            const double off  = (rnd() - 0.5) * w[i], side = off >= 0 ? 1 : -1;
            const double a    = (P.fx_angle + (rnd() * 2 - 1) * 30 * jit) * deg;
            const double dx   = tx * std::cos(a) - ty * side * std::sin(a), dy = ty * std::cos(a) + tx * side * std::sin(a);
            strand(c[i].x - ty * off, c[i].y + tx * off, dx, dy, len, root, side * (0.02 + 0.06 * jit * rnd()), 0.15);
        } else {   // Grass: blades out of one side, swaying
            const double a  = (rnd() * 2 - 1) * P.fx_angle * deg;
            const double nx = -ty, ny = tx;   // the stroke's left side
            const double dx = nx * std::cos(a) + tx * std::sin(a), dy = ny * std::cos(a) + ty * std::sin(a);
            strand(c[i].x, c[i].y, -dx, -dy, len, root, (rnd() - 0.5) * 0.12 * (0.3 + jit), 0.1);
        }
    }
    return out;
}

// The outlines of every piece (the plain stroke: stroke_ring), for the eraser, hit tests and saving.
// tone -1 = every piece; 0 / 1 = only that Two-tone colour.
inline std::vector<std::vector<XY>> effect_rings(const Params &P, const std::vector<Pt> &raw, bool pen, int tone = -1)
{
    if (P.fx == Effect::None) return { stroke_ring(P, raw, pen) };
    std::vector<std::vector<XY>> out;
    for (const Piece &pc : effect_pieces(P, raw, pen))
        if (tone < 0 || pc.tone == tone)
            out.push_back(rdp(outline(pc.c, pc.w), P.simplify));
    return out;
}

}} // namespace Slic3r::stroke
