// SVG shapes for painting: the SVG read with nanosvg, cleaned, split into colour zones and into
// its single shapes, and turned into one occlusion-resolved mask per group. Used by the
// multi-material painter's Vector mode (SVG decals).
#include "SvgPaint.hpp"
#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "Model.hpp"
#include "NSVGUtils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>

namespace Slic3r {
namespace SvgPaint {

// nanosvg packs colours as 0xAABBGGRR; 0xRRGGBB everywhere else.
static uint32_t nsvg_rgb(unsigned int c)
{
    return ((c & 0xFFu) << 16) | (((c >> 8) & 0xFFu) << 8) | ((c >> 16) & 0xFFu);
}
static uint32_t nsvg_paint_rgb(const NSVGpaint& paint)
{
    if (paint.type == NSVG_PAINT_COLOR) return nsvg_rgb(paint.color);
    if ((paint.type == NSVG_PAINT_LINEAR_GRADIENT || paint.type == NSVG_PAINT_RADIAL_GRADIENT)
        && paint.gradient && paint.gradient->nstops > 0) {
        // A gradient: the mean of its stops.
        unsigned r = 0, g = 0, b = 0;
        const int n = paint.gradient->nstops;
        for (int i = 0; i < n; ++i) {
            const uint32_t c = nsvg_rgb(paint.gradient->stops[i].color);
            r += (c >> 16) & 0xFFu; g += (c >> 8) & 0xFFu; b += c & 0xFFu;
        }
        return ((r / n) << 16) | ((g / n) << 8) | (b / n);
    }
    return 0;
}

// Ends (t = 0, t = 1) of a LINEAR gradient, in nanosvg's output coordinates (mm, Y down).
// nanosvg keeps the output -> gradient matrix with t = v; its inverse takes (0,0) and (0,1) to the
// ends (checked with x1/x2 in userSpaceOnUse and objectBoundingBox).
static bool linear_grad_endpoints(const NSVGpaint& paint, Vec2d& p0, Vec2d& p1)
{
    if (paint.type != NSVG_PAINT_LINEAR_GRADIENT || paint.gradient == nullptr) return false;
    const int n = paint.gradient->nstops;
    if (n < 2) return false;
    const float* t = paint.gradient->xform;
    const double det = double(t[0]) * t[3] - double(t[2]) * t[1];
    if (std::abs(det) < 1e-12) return false;
    // inverse of [a c e; b d f]
    const double ia =  t[3] / det, ib = -t[1] / det, ic = -t[2] / det, id = t[0] / det;
    const double ie = -(ia * t[4] + ic * t[5]), iff = -(ib * t[4] + id * t[5]);
    p0 = Vec2d(ie, iff);
    p1 = Vec2d(ic + ie, id + iff);
    return (p1 - p0).squaredNorm() > 1e-12;
}

// Colour of a gradient at t (linear between the stops around it; before the first and after the
// last, the end colour, like spread="pad").
static uint32_t grad_sample(const NSVGgradient& gr, float t)
{
    int k = 0;
    while (k + 1 < gr.nstops && gr.stops[k + 1].offset < t) ++k;
    const NSVGgradientStop& s0 = gr.stops[k];
    const NSVGgradientStop& s1 = gr.stops[std::min(k + 1, gr.nstops - 1)];
    const float span = s1.offset - s0.offset;
    float w = (span > 1e-6f) ? (t - s0.offset) / span : 0.f;
    w = std::clamp(w, 0.f, 1.f);
    if (t <= gr.stops[0].offset) w = 0.f;
    const uint32_t c0 = nsvg_rgb(s0.color), c1 = nsvg_rgb(s1.color);
    uint32_t out_c = 0;
    for (int sh8 = 0; sh8 <= 16; sh8 += 8) {
        const float v = (1.f - w) * float((c0 >> sh8) & 0xFF) + w * float((c1 >> sh8) & 0xFF);
        out_c |= uint32_t(std::clamp(std::lround(v), 0L, 255L)) << sh8;
    }
    return out_c;
}

struct Lab { float L = 0.f, a = 0.f, b = 0.f; };

static float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

static Lab lab_of(uint32_t c)
{
    const float rl = srgb_to_linear(((c >> 16) & 0xFF) / 255.f);
    const float gl = srgb_to_linear(((c >> 8) & 0xFF) / 255.f);
    const float bl = srgb_to_linear((c & 0xFF) / 255.f);
    const float X = rl * 0.4124564f + gl * 0.3575761f + bl * 0.1804375f;
    const float Y = rl * 0.2126729f + gl * 0.7151522f + bl * 0.0721750f;
    const float Z = rl * 0.0193339f + gl * 0.1191920f + bl * 0.9503041f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
    const float fx = f(X / 0.95047f), fy = f(Y), fz = f(Z / 1.08883f);
    return { 116.f * fy - 16.f, 500.f * (fx - fy), 200.f * (fy - fz) };
}

static float delta_e2000(const Lab& x, const Lab& y)
{
    // CIEDE2000 (Sharma, Wu & Dalal 2005), kL = kC = kH = 1.
    const double L1 = x.L, a1 = x.a, b1 = x.b;
    const double L2 = y.L, a2 = y.a, b2 = y.b;
    const double PI = 3.14159265358979323846;

    const double C1 = std::sqrt(a1 * a1 + b1 * b1);
    const double C2 = std::sqrt(a2 * a2 + b2 * b2);
    const double Cbar = 0.5 * (C1 + C2);
    const double Cbar7 = std::pow(Cbar, 7.0);
    const double G = 0.5 * (1.0 - std::sqrt(Cbar7 / (Cbar7 + std::pow(25.0, 7.0))));

    const double a1p = (1.0 + G) * a1;
    const double a2p = (1.0 + G) * a2;
    const double C1p = std::sqrt(a1p * a1p + b1 * b1);
    const double C2p = std::sqrt(a2p * a2p + b2 * b2);

    auto hp = [&](double ap, double b) -> double {
        if (ap == 0.0 && b == 0.0) return 0.0;
        double h = std::atan2(b, ap);
        if (h < 0.0) h += 2.0 * PI;
        return h * 180.0 / PI;          // grados [0, 360)
    };
    const double h1p = hp(a1p, b1);
    const double h2p = hp(a2p, b2);

    const double dLp = L2 - L1;
    const double dCp = C2p - C1p;

    double dhp = 0.0;
    if (C1p * C2p != 0.0) {
        dhp = h2p - h1p;
        if (dhp > 180.0)       dhp -= 360.0;
        else if (dhp < -180.0) dhp += 360.0;
    }
    const double dHp = 2.0 * std::sqrt(C1p * C2p)
                     * std::sin(dhp * PI / 360.0);   

    const double Lbp = 0.5 * (L1 + L2);
    const double Cbp = 0.5 * (C1p + C2p);

    double hbp = h1p + h2p;             
    if (C1p * C2p != 0.0) {
        if (std::abs(h1p - h2p) > 180.0)
            hbp += (hbp < 360.0) ? 360.0 : -360.0;
        hbp *= 0.5;
    }

    const double T = 1.0
        - 0.17 * std::cos((hbp - 30.0) * PI / 180.0)
        + 0.24 * std::cos((2.0 * hbp) * PI / 180.0)
        + 0.32 * std::cos((3.0 * hbp + 6.0) * PI / 180.0)
        - 0.20 * std::cos((4.0 * hbp - 63.0) * PI / 180.0);

    const double dTheta = 30.0 * std::exp(-((hbp - 275.0) / 25.0) * ((hbp - 275.0) / 25.0));
    const double Cbp7 = std::pow(Cbp, 7.0);
    const double RC = 2.0 * std::sqrt(Cbp7 / (Cbp7 + std::pow(25.0, 7.0)));
    const double Lbp2 = (Lbp - 50.0) * (Lbp - 50.0);
    const double SL = 1.0 + 0.015 * Lbp2 / std::sqrt(20.0 + Lbp2);
    const double SC = 1.0 + 0.045 * Cbp;
    const double SH = 1.0 + 0.015 * Cbp * T;
    const double RT = -std::sin(2.0 * dTheta * PI / 180.0) * RC;

    const double tL = dLp / SL;
    const double tC = dCp / SC;
    const double tH = dHp / SH;
    return (float)std::sqrt(tL * tL + tC * tC + tH * tH + RT * tC * tH);
}

// The SVG parsed once, in its own frame (scaled mm, z = 0): its shapes, the zone of each shape and
// the list of zones. Cached (with a mutex: callers may run in parallel).
namespace {
struct LocalSvg {
    std::string        svg;
    std::vector<Shape> shapes;      // own frame; a shape may have no rings
    std::vector<int>   shape_zone;  // 1:1 with shapes
    std::vector<Zone>  zones;
};
std::mutex                                   g_local_mtx;
std::vector<std::shared_ptr<const LocalSvg>> g_local;   // most recent last
}

// Curve tolerance of the SVG tessellation: 0.1 mm, but never more than
// 1/500 of the drawing's size (min 0.005 mm). A 5 mm SVG enlarged x20 as a decal came out angular
// (chords of 2 mm on the model); big SVGs keep exactly the old 0.1 mm.
static double tolerance_mm(const NSVGimage& image)
{
    float lo[2] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
    float hi[2] = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
    for (const NSVGshape* sh = image.shapes; sh != nullptr; sh = sh->next) {
        lo[0] = std::min(lo[0], sh->bounds[0]); lo[1] = std::min(lo[1], sh->bounds[1]);
        hi[0] = std::max(hi[0], sh->bounds[2]); hi[1] = std::max(hi[1], sh->bounds[3]);
    }
    const double size = std::max(double(hi[0] - lo[0]), double(hi[1] - lo[1]));
    if (!(size > 0.))
        return 0.1;
    return std::clamp(size / 500., 0.005, 0.1);
}

static std::shared_ptr<LocalSvg> build_local_svg(const std::string& svg_data)
{
    auto loc = std::make_shared<LocalSvg>();
    loc->svg = svg_data;
    std::vector<Shape>& out = loc->shapes;
    if (svg_data.empty()) return loc;
    NSVGimage_ptr image = nsvgParse(svg_data, "mm", 96.0f);
    if (image == nullptr || image->shapes == nullptr) {
        return loc;
    }
    
    const Transform3d to_target = Transform3d::Identity();

    const double tol_mm = tolerance_mm(*image);
    NSVGLineParams params{ (tol_mm * tol_mm) / SCALING_FACTOR / SCALING_FACTOR };
    const ExPolygonsWithIds shapes = create_shape_with_ids(*image, params);   // one pass: one centring for all
    if (shapes.empty()) return loc;

    // id = 2 * shape index (+1 for a stroke) -> the colours of that NSVGshape.
    std::vector<const NSVGshape*> by_index;
    for (NSVGshape* sh = image->shapes; sh != nullptr; sh = sh->next) by_index.push_back(sh);
    std::vector<std::array<uint32_t, 3>> zone_stops;   // each zone's gradient sampled at 0, 0.5, 1

    const double det_xy = to_target(0, 0) * to_target(1, 1) - to_target(0, 1) * to_target(1, 0);
    auto project = [&](const Polygon& src, Polygons& dst) {
        Polygon p;
        p.points.reserve(src.points.size());
        for (const Point& q : src.points) {
            const Vec3d v = to_target * Vec3d(unscale<double>(q.x()), unscale<double>(q.y()), 0.0);
            p.points.emplace_back(coord_t(scale_(v.x())), coord_t(scale_(v.y())));
        }
        if (det_xy < 0.0) std::reverse(p.points.begin(), p.points.end());
        if (std::abs(p.area()) < SCALED_EPSILON) return;
        dst.push_back(std::move(p));
    };
    for (const ExPolygonsWithId& sh : shapes) {
        Shape ss;
        ss.is_stroke = (sh.id & 1u) != 0;
        const size_t si = sh.id / 2;
        if (si < by_index.size())
            ss.rgb = nsvg_paint_rgb(ss.is_stroke ? by_index[si]->stroke : by_index[si]->fill);
        Vec2d gp0, gp1;
        if (!ss.is_stroke && si < by_index.size() && !sh.expoly.empty()
            && linear_grad_endpoints(by_index[si]->fill, gp0, gp1)) {
            // create_shape_with_ids() centres the drawing without saying by how much: the shift
            // comes from THIS shape (centre of its tessellated box minus that of its nanosvg
            // `bounds`, Y flipped). Off by at most the tessellation tolerance.
            const NSVGshape& ns = *by_index[si];
            const BoundingBox bb = get_extents(sh.expoly);
            const Vec2d c_int = unscale(bb.center()).cast<double>();
            const Vec2d c_svg(0.5 * (ns.bounds[0] + ns.bounds[2]), -0.5 * (ns.bounds[1] + ns.bounds[3]));
            const Vec2d off = c_int - c_svg;
            auto to_tgt = [&](const Vec2d& q) {
                const Vec3d v = to_target * Vec3d(q.x() + off.x(), -q.y() + off.y(), 0.0);
                return Vec2d(v.x(), v.y());
            };
            ss.grad = true;
            ss.g0 = to_tgt(gp0);
            ss.g1 = to_tgt(gp1);
        }
        for (const ExPolygon& ep : sh.expoly) {
            project(ep.contour, ss.rings);
            for (const Polygon& h : ep.holes) project(h, ss.rings);
        }
        if (ss.rings.empty()) continue;
        // The shape's zone. Flat colours: the first zone within ΔE2000 < 2 (antialiasing,
        // #FE0000 / #FF0000). Gradients: only with another gradient of nearly the same stops.
        uint32_t gs[3] = { 0, 0, 0 };
        if (ss.grad) {
            const NSVGgradient& gr = *by_index[si]->fill.gradient;
            gs[0] = grad_sample(gr, 0.f); gs[1] = grad_sample(gr, 0.5f); gs[2] = grad_sample(gr, 1.f);
        }
        int z = -1;
        for (size_t zi = 0; zi < loc->zones.size() && z < 0; ++zi) {
            const Zone& zz = loc->zones[zi];
            if (zz.grad != ss.grad) continue;
            if (!ss.grad) {
                if (delta_e2000(lab_of(ss.rgb), lab_of(zz.rgb)) < 2.f) z = int(zi);
            } else {
                bool same = true;
                for (int k = 0; k < 3 && same; ++k)
                    same = delta_e2000(lab_of(gs[k]), lab_of(zone_stops[zi][size_t(k)])) < 2.f;
                if (same) z = int(zi);
            }
        }
        if (z < 0) {
            z = int(loc->zones.size());
            loc->zones.push_back({ ss.rgb, ss.grad, 0 });
            zone_stops.push_back({ gs[0], gs[1], gs[2] });
        }
        ++loc->zones[size_t(z)].shapes;
        loc->shape_zone.push_back(z);
        out.push_back(std::move(ss));
    }
    return loc;
}

static std::shared_ptr<const LocalSvg> local_svg(const std::string& svg_data)
{
    {
        std::lock_guard<std::mutex> lk(g_local_mtx);
        for (size_t i = g_local.size(); i-- > 0; )
            if (g_local[i]->svg.size() == svg_data.size() && g_local[i]->svg == svg_data)
                return g_local[i];
    }
    // Built outside the lock: two threads may build the same one at once (same result), which
    // is better than everyone waiting on one parse.
    std::shared_ptr<const LocalSvg> built = build_local_svg(svg_data);
    std::lock_guard<std::mutex> lk(g_local_mtx);
    for (const auto& e : g_local)
        if (e->svg.size() == svg_data.size() && e->svg == svg_data) return e;
    if (g_local.size() >= 16) g_local.erase(g_local.begin());
    g_local.push_back(built);
    return built;
}

// Local shapes -> target frame (rings and gradient ends). Keeps 1:1 with the local list (a shape
// left without rings stays, empty).
static std::vector<Shape> transform_shapes(const std::vector<Shape>& loc,
                                                                  const Transform3d& to_target)
{
    std::vector<Shape> out;
    out.reserve(loc.size());
    const double det_xy = to_target(0, 0) * to_target(1, 1) - to_target(0, 1) * to_target(1, 0);
    for (const Shape& ls : loc) {
        Shape ss;
        ss.rgb = ls.rgb; ss.is_stroke = ls.is_stroke; ss.grad = ls.grad;
        if (ls.grad) {
            const Vec3d a = to_target * Vec3d(ls.g0.x(), ls.g0.y(), 0.0);
            const Vec3d b = to_target * Vec3d(ls.g1.x(), ls.g1.y(), 0.0);
            ss.g0 = Vec2d(a.x(), a.y()); ss.g1 = Vec2d(b.x(), b.y());
        }
        ss.rings.reserve(ls.rings.size());
        for (const Polygon& src : ls.rings) {
            Polygon p;
            p.points.reserve(src.points.size());
            for (const Point& q : src.points) {
                const Vec3d v = to_target * Vec3d(unscale<double>(q.x()), unscale<double>(q.y()), 0.0);
                p.points.emplace_back(coord_t(scale_(v.x())), coord_t(scale_(v.y())));
            }
            if (det_xy < 0.0) std::reverse(p.points.begin(), p.points.end());
            if (std::abs(p.area()) < SCALED_EPSILON) continue;
            ss.rings.push_back(std::move(p));
        }
        out.push_back(std::move(ss));
    }
    return out;
}

std::vector<Shape> shapes(const std::string& svg_data, const Transform3d& to_target)
{
    if (svg_data.empty()) return {};
    return transform_shapes(local_svg(svg_data)->shapes, to_target);
}

std::vector<Zone> zones(const std::string& svg_data)
{
    if (svg_data.empty()) return {};
    return local_svg(svg_data)->zones;
}

std::vector<int> shape_zones(const std::string& svg_data)
{
    if (svg_data.empty()) return {};
    return local_svg(svg_data)->shape_zone;
}

// From the topmost shape down, each one keeps only what is not covered yet.
std::vector<ExPolygons> group_masks(const std::string& svg_data, const Transform3d& to_target,
                                                     const std::vector<int>& shape_group, size_t n_groups)
{
    if (svg_data.empty()) return {};
    const std::shared_ptr<const LocalSvg> loc = local_svg(svg_data);
    const std::vector<Shape> shapes = transform_shapes(loc->shapes, to_target);
    std::vector<ExPolygons> per_group(n_groups);
    Polygons covered;
    for (size_t k = shapes.size(); k-- > 0; ) {
        const int g = (k < shape_group.size()) ? shape_group[k] : -1;
        if (g < 0 || size_t(g) >= per_group.size())
            continue;   // transparent: does not cover what is under it
        const Shape& sh = shapes[k];
        if (sh.rings.empty())
            continue;
        ExPolygons piece = covered.empty() ? union_ex(sh.rings) : diff_ex(union_ex(sh.rings), covered);
        polygons_append(covered, sh.rings);
        covered = union_(covered);
        expolygons_append(per_group[size_t(g)], std::move(piece));
    }
    for (ExPolygons& e : per_group)
        if (!e.empty())
            e = union_ex(e);
    return per_group;
}

std::vector<ExPolygons> zone_masks(const std::string& svg_data, const Transform3d& to_target,
                                                    const std::vector<bool>& zone_hidden)
{
    if (svg_data.empty()) return {};
    const std::shared_ptr<const LocalSvg> loc = local_svg(svg_data);
    std::vector<int> shape_group = loc->shape_zone;
    for (int& z : shape_group)
        if (z >= 0 && size_t(z) < zone_hidden.size() && zone_hidden[size_t(z)])
            z = -1;
    return group_masks(svg_data, to_target, shape_group, loc->zones.size());
}

} // namespace SvgPaint

PaintDecalGroups paint_decal_groups(const std::string& svg_data, const std::vector<PaintDecalZone>& zones,
                                    const std::vector<PaintDecalPart>& parts)
{
    PaintDecalGroups out;
    if (svg_data.empty()) return out;
    const std::shared_ptr<const SvgPaint::LocalSvg> loc = SvgPaint::local_svg(svg_data);
    const size_t n_zones = loc->zones.size();
    out.groups.resize(n_zones);
    for (size_t z = 0; z < n_zones; ++z) {
        out.groups[z].zone = int(z);
        out.groups[z].rgb  = loc->zones[z].rgb;
    }
    for (const PaintDecalZone& dz : zones)
        if (dz.zone >= 0 && size_t(dz.zone) < n_zones) {
            out.groups[size_t(dz.zone)].state  = dz.state;
            out.groups[size_t(dz.zone)].hidden = dz.hidden;
        }
    out.shape_group = loc->shape_zone;
    for (int& g : out.shape_group)
        if (g >= 0 && (size_t(g) >= n_zones || out.groups[size_t(g)].hidden))
            g = -1;
    for (const PaintDecalPart& p : parts) {
        if (p.shape < 0 || size_t(p.shape) >= out.shape_group.size())
            continue;
        if (p.hidden) {
            out.shape_group[size_t(p.shape)] = -1;
        } else if (p.state >= 0) {
            PaintDecalZone g;
            g.zone  = int(out.groups.size());
            const int z = loc->shape_zone[size_t(p.shape)];
            g.rgb   = (z >= 0 && size_t(z) < n_zones) ? loc->zones[size_t(z)].rgb : 0;
            g.state = p.state;
            out.shape_group[size_t(p.shape)] = g.zone;
            out.groups.push_back(g);
        }
    }
    return out;
}

namespace SvgPaint {

std::string inline_css_classes(const std::string& svg)
{
    const auto npos = std::string::npos;
    auto trim = [](std::string v) {
        const size_t b = v.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        const size_t e = v.find_last_not_of(" \t\r\n");
        return v.substr(b, e - b + 1);
    };
    // 1) `.class { decl }` rules of every <style>. Compound selectors (`.a .b`, `g.a`, `#id`,
    //    `:hover`) are ignored: nanosvg would not know what to do with them either.
    std::map<std::string, std::string> cls;
    for (size_t pos = svg.find("<style"); pos != npos; pos = svg.find("<style", pos)) {
        const size_t gt = svg.find('>', pos);
        if (gt == npos) break;
        const size_t end = svg.find("</style", gt);
        if (end == npos) break;
        std::string css = svg.substr(gt + 1, end - gt - 1);
        for (const char* tok : { "<![CDATA[", "]]>" })
            for (size_t k = css.find(tok); k != npos; k = css.find(tok)) css.erase(k, std::strlen(tok));
        for (size_t k = css.find("/*"); k != npos; k = css.find("/*")) {
            const size_t ke = css.find("*/", k + 2);
            css.erase(k, ke == npos ? npos : ke + 2 - k);
        }
        for (size_t i = 0;;) {
            const size_t ob = css.find('{', i);
            if (ob == npos) break;
            const size_t cb = css.find('}', ob);
            if (cb == npos) break;
            std::string decl = trim(css.substr(ob + 1, cb - ob - 1));
            std::replace(decl.begin(), decl.end(), '"', '\'');   // goes inside style="..."
            std::replace(decl.begin(), decl.end(), '\n', ' ');
            if (!decl.empty() && decl.back() != ';') decl += ';';
            const std::string sels = css.substr(i, ob - i);
            size_t a0 = 0;
            while (a0 <= sels.size()) {
                size_t a1 = sels.find(',', a0);
                if (a1 == npos) a1 = sels.size();
                const std::string sel = trim(sels.substr(a0, a1 - a0));
                if (sel.size() > 1 && sel[0] == '.' && sel.find_first_of(" >:+~[#.", 1) == npos)
                    cls[sel.substr(1)] += decl;
                a0 = a1 + 1;
            }
            i = cb + 1;
        }
        pos = end;
    }
    if (cls.empty()) return svg;

    // Value of an attribute inside a tag: [begin, end) of the quoted text.
    auto find_attr = [&](const std::string& tag, const char* name, size_t& vb, size_t& ve) {
        const size_t nl = std::strlen(name);
        for (size_t k = tag.find(name); k != npos; k = tag.find(name, k + 1)) {
            if (k == 0 || !std::isspace((unsigned char)tag[k - 1])) continue;
            size_t e = k + nl;
            while (e < tag.size() && std::isspace((unsigned char)tag[e])) ++e;
            if (e >= tag.size() || tag[e] != '=') continue;
            ++e;
            while (e < tag.size() && std::isspace((unsigned char)tag[e])) ++e;
            if (e >= tag.size() || (tag[e] != '"' && tag[e] != '\'')) continue;
            const size_t close = tag.find(tag[e], e + 1);
            if (close == npos) return false;
            vb = e + 1; ve = close;
            return true;
        }
        return false;
    };

    // 2) Rewrite every opening tag with class="...": the class declarations go at the START of
    //    its style (the element's own style still wins, as in CSS), or in a new style at the end
    //    (it beats fill="...", also as in CSS).
    std::string out;
    out.reserve(svg.size() + svg.size() / 4);
    int n_rewritten = 0;
    size_t i = 0;
    while (i < svg.size()) {
        const size_t lt = svg.find('<', i);
        if (lt == npos) { out.append(svg, i, npos); break; }
        out.append(svg, i, lt - i);
        size_t skip_to = npos;
        if (svg.compare(lt, 4, "<!--") == 0) {
            const size_t e = svg.find("-->", lt);
            skip_to = (e == npos) ? svg.size() : e + 3;
        } else if (svg.compare(lt, 9, "<![CDATA[") == 0) {
            const size_t e = svg.find("]]>", lt);
            skip_to = (e == npos) ? svg.size() : e + 3;
        } else if (svg.compare(lt, 6, "<style") == 0) {
            const size_t e0 = svg.find("</style", lt);
            const size_t e  = (e0 == npos) ? npos : svg.find('>', e0);
            skip_to = (e == npos) ? svg.size() : e + 1;
        }
        if (skip_to != npos) { out.append(svg, lt, skip_to - lt); i = skip_to; continue; }
        // End of the tag, minding quotes.
        size_t j = lt + 1;
        char q = 0;
        for (; j < svg.size(); ++j) {
            const char c = svg[j];
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '>') break;
        }
        if (j >= svg.size()) { out.append(svg, lt, npos); break; }
        std::string tag = svg.substr(lt, j - lt);   // without the '>'
        size_t cb = 0, ce = 0;
        if (tag.size() > 1 && tag[1] != '/' && tag[1] != '!' && tag[1] != '?'
            && find_attr(tag, "class", cb, ce)) {
            std::string decls;
            const std::string names = tag.substr(cb, ce - cb);
            size_t k = 0;
            while (k < names.size()) {
                while (k < names.size() && std::isspace((unsigned char)names[k])) ++k;
                size_t e = k;
                while (e < names.size() && !std::isspace((unsigned char)names[e])) ++e;
                if (e > k) {
                    const auto it = cls.find(names.substr(k, e - k));
                    if (it != cls.end()) decls += it->second;
                }
                k = e;
            }
            if (!decls.empty()) {
                size_t sb = 0, se = 0;
                if (find_attr(tag, "style", sb, se)) {
                    tag.insert(sb, decls);
                } else {
                    size_t ins = tag.size();
                    if (ins > 0 && tag[ins - 1] == '/') --ins;
                    tag.insert(ins, " style=\"" + decls + "\"");
                }
                ++n_rewritten;
            }
        }
        out += tag;
        out += '>';
        i = j + 1;
    }
    return out;
}

std::string flatten(const std::string& svg, bool keep_background, FlattenInfo* info)
{
    FlattenInfo inf;
    if (info) *info = inf;
    // Canvas: a rectangle the size of the viewBox goes in as the FIRST shape. It goes through the
    // same tessellation and centring as the rest, so its polygon IS the canvas in the same frame.
    // Without a viewBox there is no reliable canvas: nothing is clipped.
    std::string src = svg;
    bool has_canvas = false;
    if (const size_t st = src.find("<svg"); st != std::string::npos) {
        size_t j = st + 4; char q = 0;
        for (; j < src.size(); ++j) {
            const char c = src[j];
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '>') break;
        }
        const size_t vb = src.find("viewBox", st);
        if (j < src.size() && src[j - 1] != '/' && vb != std::string::npos && vb < j) {
            const size_t q0 = src.find_first_of("\"'", vb);
            const size_t q1 = (q0 == std::string::npos) ? q0 : src.find(src[q0], q0 + 1);
            if (q1 != std::string::npos && q1 < j) {
                std::string nums = src.substr(q0 + 1, q1 - q0 - 1);
                std::replace(nums.begin(), nums.end(), ',', ' ');
                std::istringstream is(nums);
                is.imbue(std::locale::classic());
                double v[4];
                if ((is >> v[0] >> v[1] >> v[2] >> v[3]) && v[2] > 0.0 && v[3] > 0.0) {
                    std::ostringstream r;
                    r.imbue(std::locale::classic());
                    r << "<rect x=\"" << v[0] << "\" y=\"" << v[1] << "\" width=\"" << v[2]
                      << "\" height=\"" << v[3] << "\" fill=\"#010203\"/>";
                    src.insert(j + 1, r.str());
                    has_canvas = true;
                }
            }
        }
    }
    NSVGimage_ptr image = nsvgParse(src, "mm", 96.0f);
    if (image == nullptr || image->shapes == nullptr) return svg;
    // Same tessellation as everything else. Coordinates: scaled mm, Y flipped.
    const double tol_mm = tolerance_mm(*image);
    NSVGLineParams params{ (tol_mm * tol_mm) / SCALING_FACTOR / SCALING_FACTOR };
    const ExPolygonsWithIds shapes = create_shape_with_ids(*image, params);
    if (shapes.empty()) return svg;
    std::vector<const NSVGshape*> by_index;
    for (NSVGshape* sh = image->shapes; sh != nullptr; sh = sh->next) by_index.push_back(sh);

    BoundingBox all_bb;
    for (const ExPolygonsWithId& sh : shapes) all_bb.merge(get_extents(sh.expoly));
    double all_area = all_bb.defined ? double(all_bb.size().x()) * double(all_bb.size().y()) : 0.0;
    ExPolygons canvas;
    if (has_canvas) {
        for (const ExPolygonsWithId& sh : shapes)
            if (sh.id == 0) { canvas = sh.expoly; break; }
        if (canvas.empty()) has_canvas = false;
        else { all_area = 0.0; for (const ExPolygon& e : canvas) all_area += e.area(); }
    }

    struct Region { uint32_t rgb; ExPolygons area; BoundingBox bb; int grad = -1; };
    std::vector<Region> regs;
    // Linear gradients that survive the flattening (ends in the internal mm frame: centred, Y
    // flipped, the same as the points). -1 = a flat colour. Stops in a vector: a gradient may
    // have many.
    struct GradDesc { Vec2d p0, p1; std::vector<uint32_t> col; std::vector<float> off; };
    std::vector<GradDesc> grads;
    auto blend = [](uint32_t under, uint32_t over, float a) {
        uint32_t out = 0;
        for (int sh = 0; sh <= 16; sh += 8) {
            const float u = float((under >> sh) & 0xFF), o = float((over >> sh) & 0xFF);
            out |= uint32_t(std::clamp(std::lround(a * o + (1.f - a) * u), 0L, 255L)) << sh;
        }
        return out;
    };
    bool first = true;
    for (const ExPolygonsWithId& sh : shapes) {
        const size_t si = sh.id / 2;
        if (si >= by_index.size() || sh.expoly.empty()) continue;
        const bool stroke = (sh.id & 1u) != 0;
        const NSVGshape& ns = *by_index[si];
        if (has_canvas && si == 0) continue;   // the canvas itself
        const NSVGpaint& paint = stroke ? ns.stroke : ns.fill;
        // Opacity = the shape's times the colour's alpha (fill-opacity / stroke-opacity go there).
        float a = ns.opacity;
        if (paint.type == NSVG_PAINT_COLOR) a *= float((paint.color >> 24) & 0xFF) / 255.f;
        const uint32_t rgb = nsvg_paint_rgb(paint);
        ExPolygons S = sh.expoly;
        if (has_canvas) {
            double a0 = 0.0, a1 = 0.0;   // a0 on the UNION: a shape may overlap itself
            for (const ExPolygon& e : union_ex(S)) a0 += e.area();
            S = intersection_ex(S, canvas);
            for (const ExPolygon& e : S) a1 += e.area();
            if (a1 < a0 * 0.999) ++inf.clipped;
            if (S.empty()) continue;
        }
        const BoundingBox sbb = get_extents(S);
        if (first) {
            first = false;
            if (!keep_background && all_area > 0.0) {
                double ar = 0.0;
                for (const ExPolygon& e : S) ar += e.area();
                if (ar >= 0.95 * all_area) { inf.background_dropped = true; continue; }
            }
        }
        if (a < 0.05f) { ++inf.translucent; continue; }   // invisible: covers nothing
        // >= 0.8 counts as OPAQUE: stock SVGs use 0.87-0.91 as "almost solid" and blending them
        // made up colours the designer never asked for. Blending is for shadows and highlights.
        if (a >= 0.8f) {
            for (Region& r : regs)
                if (r.bb.overlap(sbb)) r.area = diff_ex(r.area, S);
            int gi = -1;
            Vec2d gp0, gp1;
            if (!stroke && linear_grad_endpoints(ns.fill, gp0, gp1)) {
                // The same centring shift as in shapes().
                const BoundingBox bb = get_extents(sh.expoly);
                const Vec2d off = unscale(bb.center()).cast<double>()
                                - Vec2d(0.5 * (ns.bounds[0] + ns.bounds[2]), -0.5 * (ns.bounds[1] + ns.bounds[3]));
                GradDesc g;
                g.p0 = Vec2d(gp0.x(), -gp0.y()) + off;
                g.p1 = Vec2d(gp1.x(), -gp1.y()) + off;
                for (int k = 0; k < ns.fill.gradient->nstops; ++k) {
                    g.col.push_back(nsvg_rgb(ns.fill.gradient->stops[k].color));
                    g.off.push_back(ns.fill.gradient->stops[k].offset);
                }
                gi = int(grads.size());
                grads.push_back(g);
            }
            regs.push_back({ rgb, S, sbb, gi });
            continue;
        }
        ++inf.translucent;
        std::vector<Region> add;
        for (Region& r : regs) {
            if (r.area.empty() || !r.bb.overlap(sbb)) continue;
            ExPolygons in = intersection_ex(r.area, S);
            if (in.empty()) continue;
            r.area = diff_ex(r.area, S);
            const BoundingBox ibb = get_extents(in);
            add.push_back({ blend(r.rgb, rgb, a), std::move(in), ibb });
        }
        if (a >= 0.5f) {
            ExPolygons rest = S;
            for (const Region& r : add) if (!rest.empty()) rest = diff_ex(rest, r.area);
            if (!rest.empty()) { const BoundingBox rbb = get_extents(rest); add.push_back({ rgb, std::move(rest), rbb }); }
        }
        for (Region& r : add) regs.push_back(std::move(r));
    }
    if (!inf.background_dropped && inf.translucent == 0 && inf.clipped == 0) {
        if (info) *info = inf;
        return svg;   // nothing to flatten
    }

    // One colour = one region; crumbs (< 0.01 mm²) are dropped.
    std::map<std::pair<uint32_t, int>, ExPolygons> by_col;   // (colour, gradient or -1)
    for (Region& r : regs)
        if (!r.area.empty()) expolygons_append(by_col[{ r.rgb, r.grad }], std::move(r.area));
    const double min_area = 0.01 / (SCALING_FACTOR * SCALING_FACTOR);
    BoundingBox out_bb;
    for (auto& kv : by_col) {
        ExPolygons u = union_ex(kv.second);
        u.erase(std::remove_if(u.begin(), u.end(), [&](const ExPolygon& e) { return e.area() < min_area; }), u.end());
        kv.second = std::move(u);
        out_bb.merge(get_extents(kv.second));
    }
    if (!out_bb.defined) return svg;   // nothing left: better the original than nothing

    // SVG in mm. The internal coordinates have Y flipped (NSVGLineParams::is_y_negative), undone
    // when writing: y_svg = -y. A 0.01 mm margin: the output viewBox is also the canvas if it is
    // flattened again, and without the margin rounding clipped slivers off the edge.
    const double pad = 0.01;
    const double x0 = unscale<double>(out_bb.min.x()) - pad, y0 = -unscale<double>(out_bb.max.y()) - pad;
    const double w  = unscale<double>(out_bb.size().x()) + 2 * pad, h = unscale<double>(out_bb.size().y()) + 2 * pad;
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::fixed << std::setprecision(4);
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "mm\" height=\"" << h
      << "mm\" viewBox=\"" << x0 << " " << y0 << " " << w << " " << h << "\">\n";
    o << "<!-- Neotko sticker: aplanado (fondo/transparencias) -->\n";
    auto ring = [&o](const Polygon& pg) {
        for (size_t k = 0; k < pg.points.size(); ++k)
            o << (k ? " L" : "M") << unscale<double>(pg.points[k].x()) << " " << -unscale<double>(pg.points[k].y());
        o << " Z ";
    };
    int ncol = 0;
    // The gradients used, in <defs>, with their ends in the same mm as the points.
    {
        std::set<int> used;
        for (const auto& kv : by_col) if (!kv.second.empty() && kv.first.second >= 0) used.insert(kv.first.second);
        if (!used.empty()) {
            o << "<defs>\n";
            for (const int gi : used) {
                const GradDesc& g = grads[size_t(gi)];
                o << "<linearGradient id=\"g" << gi << "\" gradientUnits=\"userSpaceOnUse\" x1=\"" << g.p0.x()
                  << "\" y1=\"" << -g.p0.y() << "\" x2=\"" << g.p1.x() << "\" y2=\"" << -g.p1.y() << "\">";
                for (size_t k = 0; k < g.col.size(); ++k) {
                    char sh[8];
                    std::snprintf(sh, sizeof(sh), "#%06X", unsigned(g.col[k]));
                    o << "<stop offset=\"" << g.off[k] << "\" stop-color=\"" << sh << "\"/>";
                }
                o << "</linearGradient>\n";
            }
            o << "</defs>\n";
        }
    }
    for (const auto& kv : by_col) {
        if (kv.second.empty()) continue;
        ++ncol;
        char hex[16];
        if (kv.first.second >= 0) std::snprintf(hex, sizeof(hex), "url(#g%d)", kv.first.second);
        else                      std::snprintf(hex, sizeof(hex), "#%06X", unsigned(kv.first.first));
        // ONE <path> per region with the nonzero rule (contour and holes wound opposite ways,
        // which flipping Y keeps). With one evenodd path per colour, Emboss::heal_polygons did
        // not repair it and made up rectangular frames.
        for (const ExPolygon& e : kv.second) {
            o << "<path fill=\"" << hex << "\" d=\"";
            ring(e.contour);
            for (const Polygon& hpg : e.holes) ring(hpg);
            o << "\"/>\n";
        }
    }
    o << "</svg>\n";
    inf.changed = true;
    inf.colors_out = ncol;
    if (info) *info = inf;
    return o.str();
}

} // namespace SvgPaint
} // namespace Slic3r
