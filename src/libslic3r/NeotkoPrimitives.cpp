// Primitive Generator. See NeotkoPrimitives.hpp.
#include "NeotkoPrimitives.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

namespace Slic3r {
namespace NeoPrim {

// -------------------------------------------------------------------------------------------------
// Catalogue
// -------------------------------------------------------------------------------------------------

static Param P(const char* key, const char* label, double def, double mn, double mx, const char* unit,
               int mark = 0, const char* tip = "")
{
    Param p;
    p.key = key; p.label = label; p.def = def; p.mn = mn; p.mx = mx; p.unit = unit; p.mark = mark; p.tip = tip;
    return p;
}
static Param PI_(const char* key, const char* label, double def, double mn, double mx, int mark = 0, const char* tip = "")
{
    Param p = P(key, label, def, mn, mx, "", mark, tip);
    p.integer = true;
    return p;
}
static Param PC(const char* key, const char* label, int def, std::vector<std::string> choices, const char* tip = "")
{
    Param p = P(key, label, def, 0, double(choices.size() - 1), "", 0, tip);
    p.integer = true;
    p.choices = std::move(choices);
    return p;
}

const std::vector<Shape>& shapes()
{
    static const std::vector<Shape> s = [] {
        std::vector<Shape> v;
        v.push_back({"ring", "Ring", "Basic", "A ring band: inside diameter, band width and height.",
                     {P("d", "inside diameter", 18, 3, 120, "mm", 1,
                        "At the narrowest point, where the finger goes.\nA ring size 16 is about 16.5 mm."),
                      P("w", "band width", 2.5, 0.6, 20, "mm", 2, "From the narrowest point of the hole to the outside."),
                      P("h", "height", 6, 0.6, 60, "mm", 3),
                      PC("inside", "inside", 0, {"comfort fit", "flat"},
                         "Comfort fit: the inside is domed, like a wedding band.\nFlat: straight inside with its own fillet."),
                      P("dome", "comfort dome", 0.6, 0.1, 5, "mm", 0, "How much the inside bulges towards the finger."),
                      P("fo", "outside fillet", 1.0, 0, 10, "mm", 0, "Radius of the rounded outside edges."),
                      P("fi", "inside fillet", 0.4, 0, 10, "mm", 0, "Radius of the rounded inside edges.\nWith comfort fit it rounds where the dome meets the top and bottom."),
                      PI_("seg", "smoothness", 128, 16, 512)}});
        v.push_back({"tube", "Tube", "Basic", "A hollow cylinder with a set wall.",
                     {P("d", "outside diameter", 30, 2, 300, "mm", 1),
                      P("wall", "wall", 2, 0.4, 100, "mm", 2),
                      P("h", "height", 30, 0.2, 300, "mm", 3),
                      PI_("seg", "smoothness", 128, 3, 512)}});
        v.push_back({"rbox", "Rounded box", "Basic", "A box with rounded corners and edges.",
                     {P("x", "width", 30, 1, 300, "mm", 1),
                      P("y", "depth", 20, 1, 300, "mm", 2),
                      P("z", "height", 15, 0.2, 300, "mm", 3),
                      P("r", "corner radius", 3, 0, 50, "mm", 4),
                      PI_("seg", "smoothness", 32, 4, 96)}});
        v.push_back({"capsule", "Capsule", "Basic", "A cylinder with round ends.",
                     {P("d", "diameter", 15, 1, 200, "mm", 1),
                      P("h", "length", 40, 1, 300, "mm", 2),
                      PI_("seg", "smoothness", 64, 8, 256)}});
        v.push_back({"frustum", "Cone and pyramid", "Basic",
                     "Bottom and top diameter. Top 0 = a point. Few sides = a pyramid or a prism.",
                     {P("d1", "bottom diameter", 30, 0, 300, "mm", 1),
                      P("d2", "top diameter", 12, 0, 300, "mm", 2),
                      P("h", "height", 30, 0.2, 300, "mm", 3),
                      PI_("sides", "sides", 128, 3, 512)}});
        v.push_back({"wedge", "Wedge", "Basic", "A ramp: full height at the back, zero at the front.",
                     {P("x", "width", 30, 1, 300, "mm", 1),
                      P("y", "depth", 40, 1, 300, "mm", 2),
                      P("z", "height", 15, 0.2, 300, "mm", 3)}});
        v.push_back({"platonic", "Platonic solid", "Polyhedra", "The five regular solids, lying on a face.",
                     {PC("kind", "solid", 4, {"tetrahedron", "cube", "octahedron", "dodecahedron", "icosahedron"}),
                      P("d", "size", 30, 2, 300, "mm", 1, "Diameter of the ball that touches every corner.")}});
        v.push_back({"geodesic", "Geodesic ball", "Polyhedra", "An icosahedron split into smaller triangles.",
                     {P("d", "diameter", 30, 2, 300, "mm", 1),
                      PI_("sub", "splits", 2, 0, 5, 0, "0 = icosahedron. Each step makes 4x more faces.")}});
        v.push_back({"antiprism", "Antiprism", "Polyhedra", "Two polygons, the top one turned half a side.",
                     {PI_("sides", "sides", 6, 3, 48),
                      P("d", "diameter", 30, 2, 300, "mm", 1),
                      P("h", "height", 20, 0.2, 300, "mm", 2)}});
        v.push_back({"star", "Star", "Mechanical", "An extruded star. Twist turns the top.",
                     {PI_("n", "points", 5, 3, 48),
                      P("d1", "outer diameter", 40, 2, 300, "mm", 1),
                      P("d2", "inner diameter", 20, 1, 300, "mm", 2),
                      P("h", "height", 8, 0.2, 300, "mm", 3),
                      P("tw", "twist", 0, -720, 720, "°", 0, "How much the top turns against the bottom.")}});
        v.push_back({"spring", "Spring", "Mechanical", "A coil of round wire.",
                     {P("d", "coil diameter", 20, 2, 300, "mm", 1),
                      P("wire", "wire", 2, 0.4, 30, "mm", 2),
                      P("turns", "turns", 5, 0.5, 60, "", 0),
                      P("pitch", "pitch", 5, 0.5, 100, "mm", 3, "Height per turn. Kept a bit over the wire."),
                      PI_("seg", "smoothness", 48, 8, 128)}});
        v.push_back({"superformula", "Superformula", "Organic",
                     "Gielis superformula: a few numbers, very different shapes. Try Vary.",
                     {P("m1", "lobes around", 6, 0, 20, "", 0),
                      P("m2", "lobes up", 4, 0, 20, "", 0),
                      P("n1", "n1", 1, 0.1, 30, "", 0),
                      P("n2", "n2", 1.7, 0.1, 30, "", 0),
                      P("n3", "n3", 1.7, 0.1, 30, "", 0),
                      P("d", "size", 40, 2, 300, "mm", 1),
                      PI_("res", "smoothness", 96, 24, 256)}});
        return v;
    }();
    return s;
}

const Shape* find_shape(const std::string& id)
{
    for (const Shape& s : shapes())
        if (s.id == id)
            return &s;
    return nullptr;
}

Values defaults(const Shape& s)
{
    Values v;
    for (const Param& p : s.params)
        v[p.key] = p.def;
    return v;
}

void clamp_values(const Shape& s, Values& v)
{
    for (const Param& p : s.params) {
        auto it = v.find(p.key);
        double x = it == v.end() ? p.def : it->second;
        if (!std::isfinite(x))
            x = p.def;
        x = std::clamp(x, p.mn, p.mx);
        if (p.integer)
            x = std::round(x);
        v[p.key] = x;
    }
}

// -------------------------------------------------------------------------------------------------
// Mesh helpers
// -------------------------------------------------------------------------------------------------

using Ring = std::vector<Vec3f>;

// Rings of the same size joined by quads; each ring wraps. Optional fans close the first / last ring
// towards a centre point. `wrap_rings` joins the last ring back to the first (a torus).
static indexed_triangle_set loft(const std::vector<Ring>& rings, bool wrap_rings, bool cap_first, bool cap_last,
                                 const Vec3f* c_first = nullptr, const Vec3f* c_last = nullptr)
{
    indexed_triangle_set its;
    if (rings.size() < 2)
        return its;
    const int m = int(rings.front().size());
    const int n = int(rings.size());
    for (const Ring& r : rings)
        its.vertices.insert(its.vertices.end(), r.begin(), r.end());
    auto id = [m](int i, int j) { return i * m + (j % m); };
    const int last = wrap_rings ? n : n - 1;
    for (int i = 0; i < last; ++i) {
        const int i2 = (i + 1) % n;
        for (int j = 0; j < m; ++j) {
            its.indices.emplace_back(id(i, j), id(i2, j), id(i2, j + 1));
            its.indices.emplace_back(id(i, j), id(i2, j + 1), id(i, j + 1));
        }
    }
    auto centroid = [](const Ring& r) {
        Vec3f c = Vec3f::Zero();
        for (const Vec3f& p : r) c += p;
        return Vec3f(c / float(r.size()));
    };
    if (!wrap_rings && cap_first) {
        const int c = int(its.vertices.size());
        its.vertices.push_back(c_first ? *c_first : centroid(rings.front()));
        for (int j = 0; j < m; ++j)
            its.indices.emplace_back(c, id(0, j), id(0, j + 1));
    }
    if (!wrap_rings && cap_last) {
        const int c = int(its.vertices.size());
        its.vertices.push_back(c_last ? *c_last : centroid(rings.back()));
        for (int j = 0; j < m; ++j)
            its.indices.emplace_back(c, id(n - 1, j + 1), id(n - 1, j));
    }
    return its;
}

static void outward(indexed_triangle_set& its)
{
    if (!its.indices.empty() && its_volume(its) < 0.f)
        its_flip_triangles(its);
}

static void sphere_points(std::vector<Vec3f>& out, const Vec3f& c, float r, int seg)
{
    seg = std::max(seg, 4);
    const int lat = std::max(2, seg / 2);
    out.push_back(c + Vec3f(0, 0, r));
    out.push_back(c - Vec3f(0, 0, r));
    for (int i = 1; i < lat; ++i) {
        const double th = PI * double(i) / lat;
        for (int j = 0; j < seg; ++j) {
            const double ph = 2. * PI * double(j) / seg;
            out.emplace_back(c + Vec3f(float(r * std::sin(th) * std::cos(ph)), float(r * std::sin(th) * std::sin(ph)),
                                       float(r * std::cos(th))));
        }
    }
}

static void circle_points(std::vector<Vec3f>& out, float r, float z, int n, double phase = 0.)
{
    if (r <= 0.f) {
        out.emplace_back(0.f, 0.f, z);
        return;
    }
    for (int j = 0; j < n; ++j) {
        const double a = phase + 2. * PI * double(j) / n;
        out.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), z);
    }
}

// Revolves a closed (r, z) profile around Z. Every r must be > 0.
static indexed_triangle_set revolve(const std::vector<Vec2d>& prof, int seg)
{
    std::vector<Ring> rings;
    for (int i = 0; i < seg; ++i) {
        const double a = 2. * PI * double(i) / seg;
        const double c = std::cos(a), s = std::sin(a);
        Ring r;
        for (const Vec2d& p : prof)
            r.emplace_back(float(p.x() * c), float(p.x() * s), float(p.y()));
        rings.push_back(std::move(r));
    }
    return loft(rings, true, false, false);
}

// -------------------------------------------------------------------------------------------------
// Shapes
// -------------------------------------------------------------------------------------------------

static double val(const Values& v, const char* k) { auto it = v.find(k); return it == v.end() ? 0. : it->second; }

// Rounds the corner of a closed polyline at the point equal to `at` with a circular arc of radius r.
// The legs are taken as straight near the corner (true for lines, close enough for a gentle dome).
static void round_corner(std::vector<Vec2d>& p, const Vec2d& at, double r, int steps)
{
    if (r <= 1e-4 || p.size() < 3)
        return;
    const int n = int(p.size());
    int i = -1;
    for (int k = 0; k < n; ++k)
        if ((p[k] - at).norm() < 1e-6) { i = k; break; }
    if (i < 0)
        return;
    const Vec2d P = p[i];
    auto far = [&](int dir) {
        for (int k = 1; k < n; ++k) {
            const Vec2d& q = p[((i + dir * k) % n + n) % n];
            if ((q - P).norm() >= r) return q;
        }
        return p[((i + dir) % n + n) % n];
    };
    const Vec2d a = (P - far(-1)).normalized(); // incoming
    const Vec2d b = (far(+1) - P).normalized(); // outgoing
    const double th = std::acos(std::clamp(-a.dot(b), -1., 1.)); // angle between the legs
    if (th > PI - 1e-3 || th < 1e-3)
        return;
    const double t = r / std::tan(0.5 * th);
    const Vec2d  A = P - a * t, B = P + b * t;
    const Vec2d  C = P + (b - a).normalized() * (r / std::sin(0.5 * th));
    double a0 = std::atan2(A.y() - C.y(), A.x() - C.x());
    double a1 = std::atan2(B.y() - C.y(), B.x() - C.x());
    double da = a1 - a0;
    while (da > PI) da -= 2. * PI;
    while (da < -PI) da += 2. * PI;
    // drop the points of both legs closer than t to the corner
    std::vector<char> drop(n, 0);
    drop[i] = 1;
    for (int k = 1; k < n / 2; ++k) { const int j = ((i - k) % n + n) % n; if ((p[j] - P).norm() < t - 1e-9) drop[j] = 1; else break; }
    for (int k = 1; k < n / 2; ++k) { const int j = (i + k) % n; if ((p[j] - P).norm() < t - 1e-9) drop[j] = 1; else break; }
    std::vector<Vec2d> out;
    for (int k = 0; k < n; ++k) {
        if (k == i)
            for (int s = 0; s <= steps; ++s) {
                const double ang = a0 + da * double(s) / steps;
                out.emplace_back(C.x() + r * std::cos(ang), C.y() + r * std::sin(ang));
            }
        if (!drop[k])
            out.push_back(p[k]);
    }
    p = std::move(out);
}

static indexed_triangle_set make_ring(const Values& v)
{
    const double ri = 0.5 * val(v, "d"), w = val(v, "w"), h = val(v, "h");
    const double ro = ri + w;
    const bool   comfort = int(val(v, "inside")) == 0;
    const int    seg = int(val(v, "seg"));
    const int    arc = std::max(6, seg / 8); // fillet steps follow the smoothness
    double fo   = std::clamp(val(v, "fo"), 0., 0.49 * std::min(w, h));
    double dome = comfort ? std::clamp(val(v, "dome"), 0.05, 0.6 * w) : 0.;
    if (comfort && dome + fo > 0.95 * w) // keep a flat strip on top between dome and fillet
        fo = std::max(0., 0.95 * w - dome);
    const double fi = comfort ? std::clamp(val(v, "fi"), 0., std::min(0.4 * h, 0.9 * (w - fo - dome)))
                              : std::clamp(val(v, "fi"), 0., std::min(0.49 * h, 0.98 * w - fo));

    std::vector<Vec2d> prof; // closed (r, z) loop; orientation is fixed by outward()
    auto fillet = [&](double cr, double cz, double r, double a0, double a1) {
        if (r <= 1e-4) { prof.emplace_back(cr + r * std::cos(a0), cz + r * std::sin(a0)); return; }
        for (int k = 0; k <= arc; ++k) {
            const double a = a0 + (a1 - a0) * double(k) / arc;
            prof.emplace_back(cr + r * std::cos(a), cz + r * std::sin(a));
        }
    };
    // outside: bottom corner, wall, top corner
    fillet(ro - fo, fo, fo, -0.5 * PI, 0.);
    fillet(ro - fo, h - fo, fo, 0., 0.5 * PI);
    if (comfort) {
        // inside from top to bottom: r = ri at mid height (the finger), ri + dome at the edges
        const int n = std::max(8, seg / 4);
        for (int k = 0; k <= n; ++k) {
            const double z = h * (1. - double(k) / n);
            prof.emplace_back(ri + dome * (1. - std::sin(PI * z / h)), z);
        }
    } else {
        fillet(ri + fi, h - fi, fi, 0.5 * PI, PI);
        fillet(ri + fi, fi, fi, PI, 1.5 * PI);
    }
    // drop repeated points (zero fillets)
    std::vector<Vec2d> clean;
    for (const Vec2d& p : prof)
        if (clean.empty() || (p - clean.back()).norm() > 1e-6)
            clean.push_back(p);
    if (clean.size() > 2 && (clean.front() - clean.back()).norm() < 1e-6)
        clean.pop_back();
    if (comfort && fi > 1e-4) {
        round_corner(clean, Vec2d(ri + dome, h), fi, arc);
        round_corner(clean, Vec2d(ri + dome, 0.), fi, arc);
    }
    return revolve(clean, seg);
}

static indexed_triangle_set make_tube(const Values& v)
{
    const double ro = 0.5 * val(v, "d");
    const double ri = std::max(0.05, ro - std::min(val(v, "wall"), ro - 0.05));
    const double h  = val(v, "h");
    return revolve({{ri, 0}, {ro, 0}, {ro, h}, {ri, h}}, int(val(v, "seg")));
}

static indexed_triangle_set make_rbox(const Values& v)
{
    const float x = float(val(v, "x")), y = float(val(v, "y")), z = float(val(v, "z"));
    const float r = std::min(float(val(v, "r")), 0.499f * std::min({x, y, z}));
    std::vector<Vec3f> pts;
    for (int i = 0; i < 8; ++i) {
        const Vec3f c((i & 1) ? x - r : r, (i & 2) ? y - r : r, (i & 4) ? z - r : r);
        if (r <= 1e-4f)
            pts.push_back(c);
        else
            sphere_points(pts, c, r, int(val(v, "seg")));
    }
    return its_convex_hull(pts);
}

static indexed_triangle_set make_capsule(const Values& v)
{
    const float r = 0.5f * float(val(v, "d"));
    const float h = std::max(float(val(v, "h")), 2.f * r);
    std::vector<Vec3f> pts;
    sphere_points(pts, Vec3f(0, 0, r), r, int(val(v, "seg")));
    sphere_points(pts, Vec3f(0, 0, h - r), r, int(val(v, "seg")));
    return its_convex_hull(pts);
}

static indexed_triangle_set make_frustum(const Values& v)
{
    float r1 = 0.5f * float(val(v, "d1")), r2 = 0.5f * float(val(v, "d2"));
    if (r1 <= 0.f && r2 <= 0.f)
        r1 = 1.f;
    const int n = int(val(v, "sides"));
    // Odd polygons look better with a flat side to the front.
    const double phase = (n % 2 == 1) ? -0.5 * PI : PI / n;
    std::vector<Vec3f> pts;
    circle_points(pts, r1, 0.f, n, phase);
    circle_points(pts, r2, float(val(v, "h")), n, phase);
    return its_convex_hull(pts);
}

static indexed_triangle_set make_wedge(const Values& v)
{
    const float x = float(val(v, "x")), y = float(val(v, "y")), z = float(val(v, "z"));
    return its_convex_hull(std::vector<Vec3f>{{0, 0, 0}, {x, 0, 0}, {0, y, 0}, {x, y, 0}, {0, y, z}, {x, y, z}});
}

static void lay_on_face(indexed_triangle_set& its)
{
    if (its.indices.empty())
        return;
    const auto& f = its.indices.front();
    const Vec3f a = its.vertices[f[0]], b = its.vertices[f[1]], c = its.vertices[f[2]];
    Vec3f n = (b - a).cross(c - a);
    if (n.norm() < 1e-9f)
        return;
    n.normalize();
    const Eigen::Quaternionf q = Eigen::Quaternionf::FromTwoVectors(n, Vec3f(0, 0, -1));
    for (Vec3f& p : its.vertices)
        p = q * p;
}

static indexed_triangle_set make_platonic(const Values& v)
{
    const float t = float((1. + std::sqrt(5.)) / 2.);
    const float k = 1.f / t;
    std::vector<Vec3f> p;
    switch (int(val(v, "kind"))) {
    case 0: p = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}}; break;
    case 1: for (int i = 0; i < 8; ++i) p.emplace_back((i & 1) ? 1 : -1, (i & 2) ? 1 : -1, (i & 4) ? 1 : -1); break;
    case 2: p = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}; break;
    case 3:
        for (int i = 0; i < 8; ++i) p.emplace_back((i & 1) ? 1 : -1, (i & 2) ? 1 : -1, (i & 4) ? 1 : -1);
        for (int s1 : {-1, 1})
            for (int s2 : {-1, 1}) {
                p.emplace_back(0, s1 * k, s2 * t);
                p.emplace_back(s1 * k, s2 * t, 0);
                p.emplace_back(s1 * t, 0, s2 * k);
            }
        break;
    default:
        for (int s1 : {-1, 1})
            for (int s2 : {-1, 1}) {
                p.emplace_back(0, s1, s2 * t);
                p.emplace_back(s1, s2 * t, 0);
                p.emplace_back(s1 * t, 0, s2);
            }
    }
    const float scale = 0.5f * float(val(v, "d")) / p.front().norm();
    for (Vec3f& q : p) q *= scale;
    indexed_triangle_set its = its_convex_hull(p);
    outward(its);
    lay_on_face(its);
    return its;
}

static indexed_triangle_set make_geodesic(const Values& v)
{
    const float t = float((1. + std::sqrt(5.)) / 2.);
    indexed_triangle_set its;
    its.vertices = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                    {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    its.indices = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                   {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
                   {3, 8, 9},  {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
    for (Vec3f& p : its.vertices) p.normalize();
    const int sub = int(val(v, "sub"));
    for (int s = 0; s < sub; ++s) {
        std::map<std::pair<int, int>, int> mid;
        auto midpoint = [&](int a, int b) {
            const auto key = std::minmax(a, b);
            auto it = mid.find(key);
            if (it != mid.end()) return it->second;
            const int id = int(its.vertices.size());
            its.vertices.push_back((its.vertices[a] + its.vertices[b]).normalized());
            mid.emplace(key, id);
            return id;
        };
        std::vector<stl_triangle_vertex_indices> next;
        next.reserve(its.indices.size() * 4);
        for (const auto& f : its.indices) {
            const int a = f[0], b = f[1], c = f[2];
            const int ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            next.emplace_back(a, ab, ca);
            next.emplace_back(b, bc, ab);
            next.emplace_back(c, ca, bc);
            next.emplace_back(ab, bc, ca);
        }
        its.indices = std::move(next);
    }
    const float r = 0.5f * float(val(v, "d"));
    for (Vec3f& p : its.vertices) p *= r;
    outward(its);
    return its;
}

static indexed_triangle_set make_antiprism(const Values& v)
{
    const int n = int(val(v, "sides"));
    const float r = 0.5f * float(val(v, "d"));
    std::vector<Vec3f> pts;
    circle_points(pts, r, 0.f, n, 0.);
    circle_points(pts, r, float(val(v, "h")), n, PI / n);
    return its_convex_hull(pts);
}

static indexed_triangle_set make_star(const Values& v)
{
    const int    n  = int(val(v, "n"));
    const double r1 = 0.5 * val(v, "d1");
    const double r2 = std::min(0.5 * val(v, "d2"), r1 * 0.98);
    const double h  = val(v, "h");
    const double tw = val(v, "tw") * PI / 180.;
    const int    layers = std::max(1, int(std::ceil(std::abs(val(v, "tw")) / 4.)));
    std::vector<Ring> rings;
    for (int L = 0; L <= layers; ++L) {
        const double f = double(L) / layers;
        const double rot = tw * f + 0.5 * PI;
        Ring r;
        for (int j = 0; j < 2 * n; ++j) {
            const double a  = rot + PI * double(j) / n;
            const double rr = (j % 2 == 0) ? r1 : r2;
            r.emplace_back(float(rr * std::cos(a)), float(rr * std::sin(a)), float(h * f));
        }
        rings.push_back(std::move(r));
    }
    const Vec3f c0(0, 0, 0), c1(0, 0, float(h));
    indexed_triangle_set its = loft(rings, false, true, true, &c0, &c1);
    outward(its);
    return its;
}

static indexed_triangle_set make_spring(const Values& v)
{
    const double R     = 0.5 * val(v, "d");
    const double rw    = std::min(0.5 * val(v, "wire"), 0.45 * R);
    const double turns = val(v, "turns");
    const double pitch = std::max(val(v, "pitch"), 2. * rw * 1.08);
    const int    seg   = int(val(v, "seg"));
    const int    steps = std::max(8, int(std::ceil(turns * seg)));
    std::vector<Ring> rings;
    for (int i = 0; i <= steps; ++i) {
        const double t = turns * 2. * PI * double(i) / steps;
        const Vec3d  c(R * std::cos(t), R * std::sin(t), pitch * t / (2. * PI) + rw);
        // frame: tangent T, normal N towards the axis, binormal B
        Vec3d T(-R * std::sin(t), R * std::cos(t), pitch / (2. * PI));
        T.normalize();
        const Vec3d N(-std::cos(t), -std::sin(t), 0.);
        const Vec3d B = T.cross(N);
        Ring r;
        const int m = std::max(8, seg / 2);
        for (int j = 0; j < m; ++j) {
            const double a = 2. * PI * double(j) / m;
            r.push_back((c + rw * (std::cos(a) * N + std::sin(a) * B)).cast<float>());
        }
        rings.push_back(std::move(r));
    }
    indexed_triangle_set its = loft(rings, false, true, true);
    outward(its);
    return its;
}

static double superformula(double a, double m, double n1, double n2, double n3)
{
    const double t1 = std::pow(std::abs(std::cos(m * a / 4.)), n2);
    const double t2 = std::pow(std::abs(std::sin(m * a / 4.)), n3);
    const double s  = t1 + t2;
    if (s <= 1e-12)
        return 0.;
    return std::pow(s, -1. / n1);
}

static indexed_triangle_set make_superformula(const Values& v)
{
    const double m1 = val(v, "m1"), m2 = val(v, "m2"), n1 = val(v, "n1"), n2 = val(v, "n2"), n3 = val(v, "n3");
    const int    res = int(val(v, "res"));
    const int    lat = std::max(8, res / 2);
    auto point = [&](double th, double ph) { // th longitude, ph latitude
        const double r1 = superformula(th, m1, n1, n2, n3);
        const double r2 = superformula(ph, m2, n1, n2, n3);
        return Vec3f(float(r1 * std::cos(th) * r2 * std::cos(ph)), float(r1 * std::sin(th) * r2 * std::cos(ph)),
                     float(r2 * std::sin(ph)));
    };
    std::vector<Ring> rings;
    for (int i = 1; i < lat; ++i) {
        const double ph = -0.5 * PI + PI * double(i) / lat;
        Ring r;
        for (int j = 0; j < res; ++j)
            r.push_back(point(-PI + 2. * PI * double(j) / res, ph));
        rings.push_back(std::move(r));
    }
    const Vec3f s = point(0., -0.5 * PI), n = point(0., 0.5 * PI);
    indexed_triangle_set its = loft(rings, false, true, true, &s, &n);
    // fit to the size: largest extent = d
    Vec3f mn = Vec3f::Constant(1e30f), mx = Vec3f::Constant(-1e30f);
    for (const Vec3f& p : its.vertices) { mn = mn.cwiseMin(p); mx = mx.cwiseMax(p); }
    const float ext = (mx - mn).maxCoeff();
    if (ext > 1e-9f) {
        const float k = float(val(v, "d")) / ext;
        for (Vec3f& p : its.vertices) p *= k;
    }
    outward(its);
    return its;
}

indexed_triangle_set build(const std::string& id, const Values& in)
{
    const Shape* s = find_shape(id);
    if (s == nullptr)
        return {};
    Values v = in;
    clamp_values(*s, v);
    indexed_triangle_set its;
    if      (id == "ring")         its = make_ring(v);
    else if (id == "tube")         its = make_tube(v);
    else if (id == "rbox")         its = make_rbox(v);
    else if (id == "capsule")      its = make_capsule(v);
    else if (id == "frustum")      its = make_frustum(v);
    else if (id == "wedge")        its = make_wedge(v);
    else if (id == "platonic")     its = make_platonic(v);
    else if (id == "geodesic")     its = make_geodesic(v);
    else if (id == "antiprism")    its = make_antiprism(v);
    else if (id == "star")         its = make_star(v);
    else if (id == "spring")       its = make_spring(v);
    else if (id == "superformula") its = make_superformula(v);
    if (its.vertices.empty())
        return its;
    outward(its);
    // sit on z = 0, centred on X/Y
    Vec3f mn = Vec3f::Constant(1e30f), mx = Vec3f::Constant(-1e30f);
    for (const Vec3f& p : its.vertices) { mn = mn.cwiseMin(p); mx = mx.cwiseMax(p); }
    const Vec3f off(-0.5f * (mn.x() + mx.x()), -0.5f * (mn.y() + mx.y()), -mn.z());
    for (Vec3f& p : its.vertices) p += off;
    return its;
}

// -------------------------------------------------------------------------------------------------
// Recipe. Hand-written number format: no locale (wx may set a comma decimal separator).
// -------------------------------------------------------------------------------------------------

static std::string num_to_str(double x)
{
    const bool neg = x < 0;
    long long  q   = (long long) std::llround(std::abs(x) * 10000.);
    std::string s  = std::to_string(q / 10000);
    long long  fr  = q % 10000;
    if (fr != 0) {
        std::string f = std::to_string(fr);
        f.insert(0, 4 - f.size(), '0');
        while (!f.empty() && f.back() == '0') f.pop_back();
        s += "." + f;
    }
    return (neg && q != 0 ? "-" : "") + s;
}

static bool str_to_num(const std::string& s, double& out)
{
    size_t i = 0;
    bool   neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    double v = 0., scale = 1.;
    bool   any = false, dot = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '.' && !dot) { dot = true; continue; }
        if (c < '0' || c > '9') return false;
        any = true;
        if (dot) { scale /= 10.; v += (c - '0') * scale; }
        else v = v * 10. + (c - '0');
    }
    if (!any) return false;
    out = neg ? -v : v;
    return true;
}

std::string encode(const std::string& id, const Values& v)
{
    std::string out = id;
    if (const Shape* s = find_shape(id))
        for (const Param& p : s->params) {
            auto it = v.find(p.key);
            out += ";" + p.key + "=" + num_to_str(it == v.end() ? p.def : it->second);
        }
    return out;
}

bool decode(const std::string& recipe, std::string& id, Values& v)
{
    v.clear();
    size_t pos = recipe.find(';');
    id = recipe.substr(0, pos);
    const Shape* s = find_shape(id);
    if (s == nullptr)
        return false;
    v = defaults(*s);
    while (pos != std::string::npos) {
        const size_t next = recipe.find(';', pos + 1);
        const std::string kv = recipe.substr(pos + 1, next == std::string::npos ? std::string::npos : next - pos - 1);
        const size_t eq = kv.find('=');
        double x;
        if (eq != std::string::npos && str_to_num(kv.substr(eq + 1), x))
            v[kv.substr(0, eq)] = x;
        pos = next;
    }
    clamp_values(*s, v);
    return true;
}

} // namespace NeoPrim
} // namespace Slic3r
