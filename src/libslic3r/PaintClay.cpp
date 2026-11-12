// NEOTKO_CLAY_TAG — Clay Tool (2_50): decals raised as parts. The core is PaintClay.hpp (std only);
// this side brings the decal (SVG colour masks, card, mirror copies) and the ray visibility.
#include "PaintClay.hpp"

#include "AABBMesh.hpp"
#include "ColorStitch.hpp"
#include "Model.hpp"
#include "NeoDebug.hpp"

#include <sstream>

namespace Slic3r {

static const char *CLAY_PREFIX = "Clay \xC2\xB7 ";   // "Clay · "

indexed_triangle_set paint_clay_base(const ModelObject& mo)
{
    indexed_triangle_set out;
    // 3 (3-nov): the parts of a frozen decal are part of the object: Clay grows on top of them
    auto frozen_part = [&mo](const ModelVolume &v) {
        for (const PaintDecal &d : mo.paint_decals)
            if (d.clay.frozen && paint_clay_is_volume_of(v, d))
                return true;
        return false;
    };
    for (const ModelVolume *v : mo.volumes) {
        if (!v->is_model_part() || (paint_clay_is_volume(*v) && !frozen_part(*v)))
            continue;
        const size_t i = out.vertices.size(), j = out.indices.size();
        append(out.vertices, v->mesh().its.vertices);
        append(out.indices, v->mesh().its.indices);
        for (size_t k = j; k < out.indices.size(); ++k)
            out.indices[k] += stl_triangle_vertex_indices(int(i), int(i), int(i));
        const Transform3d &m = v->get_matrix();
        for (size_t k = i; k < out.vertices.size(); ++k)
            out.vertices[k] = (m * out.vertices[k].cast<double>()).cast<float>().eval();
        if (v->is_left_handed())
            for (size_t k = j; k < out.indices.size(); ++k)
                std::swap(out.indices[k][0], out.indices[k][1]);
    }
    return out;
}

// NEOTKO_SURFACE_TAG — On surface (Editor study VECTOR_SUPERFICIE.md §8-§11, ported 3-nov-2026).
bool paint_surface_paste(const AABBMesh& aabb, Vec3d& p, Vec3d& n, double reach)
{
    if (n.squaredNorm() < 1e-20)
        return false;
    const Vec3d u = n.normalized();
    double best = reach;
    Vec3d  bp, bn;
    bool   found = false;
    for (const Vec3d &dir : { u, Vec3d(-u) }) {
        const AABBMesh::hit_result h = aabb.query_ray_hit(p, dir);
        if (h.is_hit() && h.distance() <= best) {
            best  = h.distance();
            bp    = h.position();
            bn    = h.normal();
            found = true;
        }
    }
    if (!found)
        return false;
    if (bn.dot(u) < 0.)   // a face seen from inside keeps the side the line was on
        bn = -bn;
    p = bp;
    n = bn.normalized();
    return true;
}

std::vector<PaintSurfaceSample> paint_surface_curve(const AABBMesh& aabb, const PaintSurfacePath& path, int* gaps)
{
    std::vector<PaintSurfaceSample> out;
    if (gaps)
        *gaps = 0;
    const size_t nc = path.clicks.size();
    if (nc < 2)
        return out;
    auto v3 = [](const std::array<double, 3> &a) { return Vec3d(a[0], a[1], a[2]); };
    auto mid_normal = [](const PaintSurfaceSample &a, const PaintSurfaceSample &b) {
        Vec3d m = a.n + b.n;
        if (m.squaredNorm() < 1e-8) {   // opposite normals: the one across the chord
            const Vec3d d = b.p - a.p;
            m = d.cross(a.n.cross(d));
        }
        return m.squaredNorm() > 1e-20 ? Vec3d(m.normalized()) : a.n;
    };
    // a -> b, both pasted: halve while they are > 0.3 mm apart (depth 14), b not pushed
    std::function<void(const PaintSurfaceSample &, const PaintSurfaceSample &, int)> fill =
        [&](const PaintSurfaceSample &a, const PaintSurfaceSample &b, int depth) {
            const double d = (b.p - a.p).norm();
            if (d <= 0.3 || depth >= 14) {
                if (d > 1. && gaps)
                    ++*gaps;
                return;
            }
            PaintSurfaceSample m { 0.5 * (a.p + b.p), mid_normal(a, b) };
            if (!paint_surface_paste(aabb, m.p, m.n, d * 0.5 + 1.)) {
                if (gaps)
                    ++*gaps;
                return;
            }
            fill(a, m, depth + 1);
            out.push_back(m);
            fill(m, b, depth + 1);
        };
    const size_t segs = path.closed && nc > 2 ? nc : nc - 1;
    PaintSurfaceSample prev { v3(path.clicks[0].p), v3(path.clicks[0].n).normalized() };
    paint_surface_paste(aabb, prev.p, prev.n, 1.);
    out.push_back(prev);
    for (size_t s = 0; s < segs; ++s) {
        const PaintSurfaceClick &A = path.clicks[s], &B = path.clicks[(s + 1) % nc];
        const Vec3d  P0 = v3(A.p), P1 = v3(A.p) + v3(A.out), P2 = v3(B.p) + v3(B.in), P3 = v3(B.p);
        const Vec3d  na = v3(A.n).normalized(), nb = v3(B.n).normalized();
        const double reach = 0.5 * ((P1 - P0).norm() + (P2 - P1).norm() + (P3 - P2).norm()) + 1.;
        for (int k = 1; k <= 6; ++k) {
            const double t = k / 6.0, v = 1. - t;
            PaintSurfaceSample q;
            q.p = v * v * v * P0 + 3. * v * v * t * P1 + 3. * v * t * t * P2 + t * t * t * P3;
            q.n = v * na + t * nb;
            q.n = q.n.squaredNorm() > 1e-12 ? Vec3d(q.n.normalized()) : na;
            if (!paint_surface_paste(aabb, q.p, q.n, reach)) {
                if (gaps)
                    ++*gaps;
                continue;
            }
            fill(prev, q, 0);
            out.push_back(q);
            prev = q;
        }
    }
    return out;
}

// Side of the ribbon at sample i: on the tangent plane, across the line.
static Vec3d surface_side(const std::vector<PaintSurfaceSample> &c, size_t i)
{
    const Vec3d t = c[std::min(i + 1, c.size() - 1)].p - c[i > 0 ? i - 1 : 0].p;
    const Vec3d l = t.cross(c[i].n);
    return l.squaredNorm() > 1e-20 ? Vec3d(l.normalized()) : Vec3d::Zero();
}

std::vector<double> paint_surface_half_widths(const std::vector<PaintSurfaceSample>& curve, const PaintSurfacePath& path)
{
    std::vector<double> h(curve.size(), 0.5 * path.width);
    if (!path.wrap || curve.size() < 2 || path.width <= 0.)
        return h;
    // the brush works in px with its Size: lay the line out flat along its length at px = mm * k,
    // at the calm speed of the screen Line (0.6 px per ms), and read the widths back in mm
    const double k = std::max(1e-6, path.brush.size) / path.width;
    std::vector<stroke::Pt> pts;
    double run = 0., t = 0.;
    for (size_t i = 0; i < curve.size(); ++i) {
        if (i > 0) {
            const double d = (curve[i].p - curve[i - 1].p).norm() * k;
            run += d;
            t += d / 0.6;
        }
        pts.push_back({ run, 0., 1., t });
    }
    const std::vector<double> w = stroke::widths(path.brush, pts, false);
    for (size_t i = 0; i < h.size() && i < w.size(); ++i)
        h[i] = 0.5 * w[i] / k;
    return h;
}

std::vector<Vec3d> paint_surface_outline(const std::vector<PaintSurfaceSample>& curve, const PaintSurfacePath& path)
{
    std::vector<Vec3d> ring;
    if (curve.size() < 2)
        return ring;
    if (path.closed) {
        for (const PaintSurfaceSample &s : curve)
            ring.push_back(s.p);
        return ring;
    }
    const std::vector<double> h = paint_surface_half_widths(curve, path);
    for (size_t i = 0; i < curve.size(); ++i)
        ring.push_back(curve[i].p + surface_side(curve, i) * h[i]);
    for (size_t i = curve.size(); i-- > 0;)
        ring.push_back(curve[i].p - surface_side(curve, i) * h[i]);
    return ring;
}

namespace {
// One piece of an On surface line: its outline in 3D and the normal its card looks along.
struct SurfacePiece { std::vector<Vec3d> ring; Vec3d n; };

Vec3d mean_normal(const std::vector<PaintSurfaceSample> &c, size_t a, size_t b)
{
    Vec3d m = Vec3d::Zero();
    for (size_t i = a; i <= b; ++i)
        m += c[i].n;
    return m.squaredNorm() > 1e-20 ? Vec3d(m.normalized()) : c[a].n;
}

// Open line: pieces whose normals stay within 30 degrees of the piece's mean, each overlapping the
// one before by max(1 mm, width) (less leaves a valley at the seam: 0.58 of 0.8 at 0.5 mm, Editor §10).
std::vector<SurfacePiece> ribbon_pieces(const std::vector<PaintSurfaceSample> &c, double width, const std::vector<double> &h)
{
    std::vector<SurfacePiece> out;
    const double cos30 = std::cos(M_PI / 6.), overlap = std::max(1., width);
    size_t a = 0;
    while (a + 1 < c.size()) {
        size_t b = a + 1;
        while (b + 1 < c.size()) {
            const Vec3d m  = mean_normal(c, a, b + 1);
            bool        ok = true;
            for (size_t i = a; i <= b + 1 && ok; ++i)
                ok = c[i].n.dot(m) >= cos30;
            if (!ok)
                break;
            ++b;
        }
        SurfacePiece pc;
        pc.n = mean_normal(c, a, b);
        for (size_t i = a; i <= b; ++i)
            pc.ring.push_back(c[i].p + surface_side(c, i) * h[i]);
        for (size_t i = b + 1; i-- > a;)
            pc.ring.push_back(c[i].p - surface_side(c, i) * h[i]);
        out.push_back(std::move(pc));
        if (b + 1 >= c.size())
            break;
        size_t next = b;   // step back the overlap along the line
        for (double run = 0.; next > a + 1 && run < overlap; --next)
            run += (c[next].p - c[next - 1].p).norm();
        a = std::max(next, a + 1);
    }
    return out;
}

// Closed line: one piece if every normal is within 30 degrees of the mean; else strips across the
// main axis the normals turn about (Editor §3.6): k = turn / 30 degrees, 2 mm overlap each side.
std::vector<SurfacePiece> closed_pieces(const std::vector<PaintSurfaceSample> &c)
{
    std::vector<SurfacePiece> out;
    if (c.size() < 3)
        return out;
    const Vec3d  nm    = mean_normal(c, 0, c.size() - 1);
    const double cos30 = std::cos(M_PI / 6.);
    bool         flat  = true;
    for (const PaintSurfaceSample &s : c)
        flat = flat && s.n.dot(nm) >= cos30;
    if (flat) {
        SurfacePiece pc { {}, nm };
        for (const PaintSurfaceSample &s : c)
            pc.ring.push_back(s.p);
        out.push_back(std::move(pc));
        return out;
    }
    const Vec3d ax = Vec3d(nm.cross(std::abs(nm.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitX())).normalized();
    const Vec3d ay = nm.cross(ax);
    double cxx = 0., cyy = 0., cxy = 0.;
    for (const PaintSurfaceSample &s : c) {
        const double x = s.n.dot(ax), y = s.n.dot(ay);
        cxx += x * x; cyy += y * y; cxy += x * y;
    }
    const double ang = 0.5 * std::atan2(2. * cxy, cxx - cyy);
    const Vec3d  u   = std::cos(ang) * ax + std::sin(ang) * ay;
    double amin = 1e30, amax = -1e30, smin = 1e30, smax = -1e30;
    for (const PaintSurfaceSample &s : c) {
        const double a = std::atan2(s.n.dot(u), s.n.dot(nm));
        amin = std::min(amin, a); amax = std::max(amax, a);
        smin = std::min(smin, s.p.dot(u)); smax = std::max(smax, s.p.dot(u));
    }
    const int    k = std::max(1, int(std::ceil((amax - amin) * 180. / M_PI / 30. - 1e-9)));
    const double W = (smax - smin) / k;
    for (int j = 0; j < k; ++j) {
        const double lo = smin + j * W - (j > 0 ? 2. : 0.) - (j == 0 ? 1. : 0.);
        const double hi = smin + (j + 1) * W + (j < k - 1 ? 2. : 0.) + (j == k - 1 ? 1. : 0.);
        // Sutherland-Hodgman against lo <= u.p <= hi, normals carried along the cut
        std::vector<PaintSurfaceSample> poly = c;
        for (int side = 0; side < 2; ++side) {
            std::vector<PaintSurfaceSample> res;
            auto in = [&](const PaintSurfaceSample &q) { const double d = q.p.dot(u); return side == 0 ? d >= lo : d <= hi; };
            auto cut = [&](const PaintSurfaceSample &a, const PaintSurfaceSample &b) {
                const double da = a.p.dot(u), db = b.p.dot(u), lim = side == 0 ? lo : hi;
                const double t  = std::abs(db - da) > 1e-12 ? (lim - da) / (db - da) : 0.5;
                PaintSurfaceSample q { a.p + (b.p - a.p) * t, a.n + (b.n - a.n) * t };
                q.n = q.n.squaredNorm() > 1e-12 ? Vec3d(q.n.normalized()) : a.n;
                return q;
            };
            for (size_t i = 0; i < poly.size(); ++i) {
                const PaintSurfaceSample &A = poly[i], &B = poly[(i + 1) % poly.size()];
                const bool ia = in(A), ib = in(B);
                if (ia)
                    res.push_back(A);
                if (ia != ib)
                    res.push_back(cut(A, B));
            }
            poly = std::move(res);
            if (poly.size() < 3)
                break;
        }
        if (poly.size() < 3)
            continue;
        SurfacePiece pc { {}, Vec3d::Zero() };
        for (const PaintSurfaceSample &q : poly) {
            pc.ring.push_back(q.p);
            pc.n += q.n;
        }
        pc.n = pc.n.squaredNorm() > 1e-20 ? Vec3d(pc.n.normalized()) : nm;
        out.push_back(std::move(pc));
    }
    return out;
}

// Every piece raised with its own parallel card along its normal. The pieces overlap and come out as
// separate parts of one filament: the slicer joins them (no boolean, Editor §10 measured the seams).
std::vector<PaintClayPiece> bake_surface(const clay::Mesh &m, const AABBMesh &aabb, const PaintDecal &decal, clay::Params q,
                                         const std::function<bool()> &stop, const std::function<void(int, int)> &progress)
{
    std::vector<PaintClayPiece> out;
    int gaps = 0;
    const std::vector<PaintSurfaceSample> curve = paint_surface_curve(aabb, decal.surface, &gaps);
    const std::vector<SurfacePiece> pieces = decal.surface.closed ? closed_pieces(curve) : ribbon_pieces(curve, decal.surface.width, paint_surface_half_widths(curve, decal.surface));
    PaintDecalGroups groups = paint_decal_groups(decal.svg_data, decal.zones, decal.parts);
    const int  state   = groups.groups.empty() ? (decal.zones.empty() ? 1 : decal.zones.front().state) : groups.groups.front().state;
    const bool engrave = decal.clay.mode_of(0) == PaintDecalClay::Engrave;
    if (decal.clay.mode_of(0) == PaintDecalClay::Off || (!groups.groups.empty() && groups.groups.front().hidden))
        return out;
    q.height  = decal.clay.height_of(0);
    q.engrave = engrave;
    q.anchor.clear();
    q.anchor_depth.clear();
    {
        std::ostringstream s;
        s << "[CLAY] surface '" << decal.name << "' " << (decal.surface.closed ? "closed" : "open") << " clicks " << decal.surface.clicks.size()
          << " samples " << curve.size() << " gaps " << gaps << " pieces " << pieces.size();
        NeoDebug::write(NeoDebug::COLORSTITCH, s.str());
    }
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (stop())
            return {};
        if (progress)
            progress(int(i), int(pieces.size()));
        const SurfacePiece &sp = pieces[i];
        if (sp.ring.size() < 3)
            continue;
        Vec3d c = Vec3d::Zero();
        for (const Vec3d &p : sp.ring)
            c += p;
        c /= double(sp.ring.size());
        const Vec3d up0 = sp.n.cross(std::abs(sp.n.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitX());
        clay::Card  card({ c.x(), c.y(), c.z() }, { sp.n.x(), sp.n.y(), sp.n.z() }, { up0.x(), up0.y(), up0.z() });
        const Vec3d dir = sp.n;
        q.face_visible = [&](int f) {
            const auto &t = m.f[size_t(f)];
            Vec3d p = (Vec3d(m.v[t[0]].x, m.v[t[0]].y, m.v[t[0]].z) + Vec3d(m.v[t[1]].x, m.v[t[1]].y, m.v[t[1]].z) +
                       Vec3d(m.v[t[2]].x, m.v[t[2]].y, m.v[t[2]].z)) / 3.;
            const AABBMesh::hit_result hit = aabb.query_ray_hit(p + dir * 0.01, dir);
            return !hit.is_hit();
        };
        clay::Ring ring;
        for (const Vec3d &p : sp.ring) {
            const clay::V2 uv = card.to_svg({ p.x(), p.y(), p.z() });
            ring.push_back(uv);
        }
        PaintClayPiece piece;
        piece.group   = engrave ? -1 : 0;
        piece.state   = state;
        piece.engrave = engrave;
        const clay::Mesh r = clay::bake(m, card, { ring }, q, piece.log);
        {
            std::ostringstream s;
            s << "[CLAY] surface piece " << i << ": ring " << ring.size() << " tris " << piece.log.part_tris << " open " << piece.log.open_edges
              << " vol " << piece.log.volume_mm3;
            NeoDebug::write(NeoDebug::COLORSTITCH, s.str());
        }
        if (r.f.empty())
            continue;
        piece.its.vertices.reserve(r.v.size());
        for (const clay::V3 &v : r.v)
            piece.its.vertices.emplace_back(float(v.x), float(v.y), float(v.z));
        piece.its.indices.reserve(r.f.size());
        for (const clay::Tri &t : r.f)
            piece.its.indices.emplace_back(t[0], t[1], t[2]);
        out.push_back(std::move(piece));
    }
    return out;
}
} // namespace

std::vector<PaintClayPiece> paint_clay_bake(const indexed_triangle_set& base, const PaintDecal& decal, const Vec3d& mirror_centre,
                                            const std::function<bool()>& cancelled, const std::function<void(int, int)>& progress)
{
    auto stop = [&cancelled]() { return cancelled && cancelled(); };
    std::vector<PaintClayPiece> out;
    if (base.indices.empty())
        return out;
    clay::Mesh m;
    m.v.reserve(base.vertices.size());
    for (const Vec3f &p : base.vertices)
        m.v.push_back({ double(p.x()), double(p.y()), double(p.z()) });
    m.f.reserve(base.indices.size());
    for (const stl_triangle_vertex_indices &t : base.indices)
        m.f.push_back({ t[0], t[1], t[2] });

    PaintDecalGroups groups = paint_decal_groups(decal.svg_data, decal.zones, decal.parts);
    const std::vector<ExPolygons> masks = ColorStitch::svg_group_masks(decal.svg_data, Transform3d::Identity(), groups.shape_group, groups.groups.size());
    const AABBMesh aabb(base);
    if (stop())
        return {};

    clay::Params q;
    q.height  = decal.clay.height;
    q.profile = clay::Profile(std::clamp(decal.clay.profile, 0, 3));
    q.falloff = decal.clay.falloff;
    q.through = false;   // Clay never grows through: from hidden faces it makes monsters (walls, the back of the head)
    q.smooth_normals = std::max(0, decal.clay.smooth);
    q.straight       = decal.clay.straight;
    q.smooth_surface = std::max(0., decal.clay.smooth_surface);
    q.min_thick      = decal.clay.straight ? std::max(0., decal.clay.min_thick) : 0.;
    q.follow_surface = decal.clay.straight ? std::max(0., decal.clay.follow_surface) : 0.;
    q.stretch_max    = decal.clay.straight ? std::max(0., decal.clay.stretch) : 0.;
    q.stretch_slope  = std::max(0.1, decal.clay.stretch_slope);
    q.cancelled      = cancelled;
    // The bottom skin 0.05 mm under the surface flickers against it in the 3D view (z-fighting);
    // 0.2 mm is out of reach of the depth buffer and still melts into the object when sliced.
    q.sink           = 0.2;
    if (!decal.surface.empty())   // NEOTKO_SURFACE_TAG: drawn on the object, no card view, no mirror copies
        return bake_surface(m, aabb, decal, q, stop, progress);

    const std::vector<PaintDecal> copies = decal.with_mirror_copies(mirror_centre);
    for (size_t ci = 0; ci < copies.size(); ++ci) {
        const PaintDecal &d = copies[ci];
        const Vec3d c(d.centre[0], d.centre[1], d.centre[2]);
        const Vec3d dir = Vec3d(d.dir[0], d.dir[1], d.dir[2]).normalized();
        clay::Card  card({ c.x(), c.y(), c.z() }, { dir.x(), dir.y(), dir.z() }, { d.up[0], d.up[1], d.up[2] });
        const bool  persp = d.perspective && d.eye_dist > 0.;
        const Vec3d eye   = d.eye_object();
        if (persp)
            card.set_eye({ eye.x(), eye.y(), eye.z() });
        Matrix2d A;
        A << d.lin[0], d.lin[1], d.lin[2], d.lin[3];
        const Matrix3d W = d.warp_matrix();
        auto to_card = [&](const Point &pt) {
            const Vec2d   s  = pt.cast<double>() * SCALING_FACTOR;
            const Vec3d   h  = W * Vec3d(s.x(), s.y(), 1.);
            const Vec2d   pl = std::abs(h.z()) > 1e-12 ? Vec2d(h.x() / h.z(), h.y() / h.z()) : s;
            const Vec2d   uv = A * pl + Vec2d(d.off[0], d.off[1]);
            return clay::V2 { uv.x(), uv.y() };
        };
        // Ray from the face centre towards the card's eye; any hit before it hides the face.
        q.face_visible = [&](int f) {
            const auto &t = m.f[size_t(f)];
            const Vec3d p = (Vec3d(m.v[t[0]].x, m.v[t[0]].y, m.v[t[0]].z) + Vec3d(m.v[t[1]].x, m.v[t[1]].y, m.v[t[1]].z) +
                             Vec3d(m.v[t[2]].x, m.v[t[2]].y, m.v[t[2]].z)) / 3.;
            Vec3d        r   = persp ? Vec3d(eye - p) : dir;
            const double max = persp ? r.norm() : std::numeric_limits<double>::max();
            r.normalize();
            const AABBMesh::hit_result hit = aabb.query_ray_hit(p + r * 0.01, r);
            return !hit.is_hit() || hit.distance() >= max;
        };

        auto rings_of = [&](const ExPolygons &exs) {
            std::vector<clay::Ring> rings;
            for (const ExPolygon &ex : exs) {
                auto add = [&](const Polygon &poly) {
                    if (poly.points.size() < 3)
                        return;
                    clay::Ring r;
                    for (const Point &pt : poly.points)
                        r.push_back(to_card(pt));
                    rings.push_back(std::move(r));
                };
                add(ex.contour);
                for (const Polygon &h : ex.holes)
                    add(h);
            }
            return rings;
        };
        // Raised colours over an engraved one reach its floor + 0.1 mm: cut out, it would leave them floating.
        q.anchor.clear();
        q.anchor_depth.clear();
        if (decal.clay.anchor)
            for (size_t g = 0; g < groups.groups.size() && g < masks.size(); ++g)
                if (!groups.groups[g].hidden && !masks[g].empty() && decal.clay.mode_of(g) == PaintDecalClay::Engrave) {
                    q.anchor.push_back(rings_of(masks[g]));
                    q.anchor_depth.push_back(decal.clay.height_of(g) + 0.1);
                }

        for (size_t g = 0; g < groups.groups.size() && g < masks.size(); ++g) {
            if (stop())
                return {};
            if (progress)
                progress(int(ci * groups.groups.size() + g), int(copies.size() * groups.groups.size()));
            if (groups.groups[g].hidden || masks[g].empty() || decal.clay.mode_of(g) != PaintDecalClay::Raise)
                continue;
            std::vector<clay::Ring> rings;
            for (const ExPolygon &ex : masks[g]) {
                auto add = [&](const Polygon &poly) {
                    if (poly.points.size() < 3)
                        return;
                    clay::Ring r;
                    for (const Point &pt : poly.points)
                        r.push_back(to_card(pt));
                    rings.push_back(std::move(r));
                };
                add(ex.contour);
                for (const Polygon &h : ex.holes)
                    add(h);
            }
            if (rings.empty())
                continue;
            q.height  = decal.clay.height_of(g);
            q.engrave = false;
            PaintClayPiece piece;
            piece.group  = int(g);
            piece.state  = groups.groups[g].state;
            piece.mirror = int(ci);
            const clay::Mesh r = clay::bake(m, card, rings, q, piece.log);
            {
                std::ostringstream s;
                s << "[CLAY] decal '" << decal.name << "' copy " << ci << " group " << g << (persp ? " (perspective)" : "")
                  << ": in " << piece.log.input_tris << " cand " << piece.log.candidate_tris << " tris " << piece.log.part_tris
                  << " open " << piece.log.open_edges << " pinches " << piece.log.pinches_split << " anchored " << piece.log.anchored << " relief " << piece.log.relief_pct << "% lifted " << piece.log.lifted << " flipped " << piece.log.flipped_top << " vol " << piece.log.volume_mm3;
                NeoDebug::write(NeoDebug::COLORSTITCH, s.str());
            }
            if (r.f.empty())
                continue;
            piece.its.vertices.reserve(r.v.size());
            for (const clay::V3 &v : r.v)
                piece.its.vertices.emplace_back(float(v.x), float(v.y), float(v.z));
            piece.its.indices.reserve(r.f.size());
            for (const clay::Tri &t : r.f)
                piece.its.indices.emplace_back(t[0], t[1], t[2]);
            out.push_back(std::move(piece));
        }

        // Engrave: every engraved colour in ONE negative part. Separate parts would share their walls
        // exactly where two colours meet, and booleans choke on faces lying on faces. The outline is the
        // union of the colours; each colour keeps its depth as a zone (a step where two meet).
        ExPolygons all;
        clay::Params e = q;
        e.engrave = true;
        e.height  = 0.;
        e.anchor.clear();
        e.anchor_depth.clear();
        int first = -1;
        for (size_t g = 0; g < groups.groups.size() && g < masks.size(); ++g) {
            if (groups.groups[g].hidden || masks[g].empty() || decal.clay.mode_of(g) != PaintDecalClay::Engrave)
                continue;
            if (first < 0)
                first = int(g);
            append(all, masks[g]);
            e.zones.push_back(rings_of(masks[g]));
            e.zone_height.push_back(decal.clay.height_of(g));
            e.height = std::max(e.height, decal.clay.height_of(g));
        }
        if (first >= 0 && !stop()) {
            const std::vector<clay::Ring> outline = rings_of(union_ex(all));
            PaintClayPiece piece;
            piece.group   = -1;
            piece.state   = groups.groups[size_t(first)].state;
            piece.mirror  = int(ci);
            piece.engrave = true;
            const clay::Mesh r = outline.empty() ? clay::Mesh() : clay::bake(m, card, outline, e, piece.log);
            {
                std::ostringstream s;
                s << "[CLAY] decal '" << decal.name << "' copy " << ci << " engrave (" << e.zones.size() << " colours): tris " << piece.log.part_tris
                  << " open " << piece.log.open_edges << " pinches " << piece.log.pinches_split << " vol " << piece.log.volume_mm3;
                NeoDebug::write(NeoDebug::COLORSTITCH, s.str());
            }
            if (!r.f.empty()) {
                piece.its.vertices.reserve(r.v.size());
                for (const clay::V3 &v : r.v)
                    piece.its.vertices.emplace_back(float(v.x), float(v.y), float(v.z));
                piece.its.indices.reserve(r.f.size());
                for (const clay::Tri &t : r.f)
                    piece.its.indices.emplace_back(t[0], t[1], t[2]);
                out.push_back(std::move(piece));
            }
        }
    }
    return out;
}

std::string paint_clay_volume_name(const PaintDecal& decal, int group)
{
    return std::string(CLAY_PREFIX) + decal.name + " \xC2\xB7 " + (group < 0 ? std::string("engrave") : std::to_string(group + 1));
}


void paint_decal_unique_name(std::vector<PaintDecal>& decals, size_t idx)
{
    if (idx >= decals.size())
        return;
    auto taken = [&](const std::string &n) {
        for (size_t i = 0; i < decals.size(); ++i)
            if (i != idx && decals[i].name == n)
                return true;
        return false;
    };
    const std::string base = decals[idx].name;
    for (int k = 2; taken(decals[idx].name); ++k)
        decals[idx].name = base + " " + std::to_string(k);
}

bool paint_clay_is_volume(const ModelVolume& v) { return v.name.rfind(CLAY_PREFIX, 0) == 0; }

bool paint_clay_is_volume_of(const ModelVolume& v, const PaintDecal& decal)
{
    return v.name.rfind(std::string(CLAY_PREFIX) + decal.name + " \xC2\xB7 ", 0) == 0;
}

} // namespace Slic3r
