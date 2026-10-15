// s348 — ver ColorStitchStickerPreview.hpp.
#include <cassert>
#include "ColorStitchStickerPreview.hpp"
#include "ColorStitchPaintPreview.hpp"          // color_for_profile

#include "libslic3r/Model.hpp"
#include "libslic3r/ColorStitch.hpp"
#include "libslic3r/SurfaceEffectProfile.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"   // s349: sección del objeto bajo el sticker
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/NeoDebug.hpp"

#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/3DScene.hpp"

#include <GL/glew.h>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace Slic3r { namespace GUI { namespace ColorStitchStickerPreview {

namespace {
// Geometría en frame LOCAL del sticker (z=0). s349: los rellenos van RECORTADOS a la sección del
// objeto a la altura del sticker (lo que cae fuera no se imprime y tapaba otros objetos), así que
// la clave lleva también el transform del sticker y la forma del objeto. Las máscaras del SVG, que
// son lo caro (parseo + uniones), van en su propia caché y no se rehacen al mover.
struct Cached {
    std::vector<std::unique_ptr<GLModel>> fills;
    std::vector<int>                      color_idx;   // -1 = sticker de 1 color
};
std::unordered_map<size_t, Cached> g_cache;
std::unordered_map<size_t, std::vector<ColorStitch::StickerColorMask>> g_masks;   // por sticker_key
std::unordered_map<size_t, ExPolygons> g_sections;                              // por section_key

inline void hmix(size_t& h, size_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); }

size_t sticker_key(const ColorStitchSticker& st)
{
    auto& mgr = SurfaceEffectProfileManager::get();
    size_t h = std::hash<std::string>{}(st.svg_data);
    auto mix = [&h](size_t v) { hmix(h, v); };
    mix(std::hash<int>{}(st.profile_id));
    mix(mgr.find(st.profile_id) ? 1u : 0u);
    for (const ColorStitchStickerColor& c : st.colors) {
        mix(c.rgb); mix(std::hash<int>{}(c.profile_id)); mix(c.hidden ? 7u : 3u);
        mix(mgr.find(c.profile_id) ? 1u : 0u);
        mix(std::hash<int>{}(c.zone));   // s350
        mix(c.band_profiles.size());     // s350: sin esto, recalcular bandas no refrescaba la vista
        for (const int bp : c.band_profiles) { mix(std::hash<int>{}(bp)); mix(mgr.find(bp) ? 1u : 0u); }
    }
    return h;
}

size_t transform_key(const Transform3d& t)
{
    size_t h = 0;
    for (int i = 0; i < 16; ++i) hmix(h, std::hash<long long>{}(std::llround(t.matrix().data()[i] * 1e5)));
    return h;
}

// Plano de corte: un pelo por debajo del ancla, dentro del material de la cara superior.
constexpr double kSectionBelowMm = 0.05;

size_t section_key(const ModelObject& mo, double z)
{
    size_t h = std::hash<size_t>{}(mo.id().id);   // (z ya lleva el lado: ±kSectionBelowMm, ver build)
    hmix(h, std::hash<long long>{}(std::llround(z * 1000.0)));
    for (const ModelVolume* v : mo.volumes) {
        if (!v || !v->is_model_part()) continue;
        hmix(h, v->mesh().its.indices.size());
        hmix(h, transform_key(v->get_matrix()));
    }
    return h;
}

// Sección del objeto (frame OBJETO, sin instancia — el mismo que st.transform) a la altura z.
const ExPolygons& object_section(const ModelObject& mo, double z)
{
    const size_t key = section_key(mo, z);
    auto it = g_sections.find(key);
    if (it != g_sections.end()) return it->second;
    if (g_sections.size() > 64) g_sections.clear();
    ExPolygons sec;
    const indexed_triangle_set its = mo.raw_indexed_triangle_set();
    if (!its.indices.empty()) {
        std::vector<ExPolygons> sl = slice_mesh_ex(its, { float(z) });   // s350: z ya viene desplazada hacia dentro
        if (!sl.empty()) sec = std::move(sl.front());
    }
    NEOTKO_LOG(PROFILE, "STICKER_SECTION obj='" << mo.name << "' z=" << z << " regions=" << sec.size());
    return g_sections.emplace(key, std::move(sec)).first->second;
}

const std::vector<ColorStitch::StickerColorMask>& local_masks(const ColorStitchSticker& st, size_t skey)
{
    auto it = g_masks.find(skey);
    if (it != g_masks.end()) return it->second;
    if (g_masks.size() > 64) g_masks.clear();
    return g_masks.emplace(skey, ColorStitch::sticker_color_masks(st, Transform3d::Identity())).first->second;
}

void build(const ModelObject& mo, const ColorStitchSticker& st, size_t skey, Cached& out)
{
    // Sección del objeto llevada al frame LOCAL del sticker (las máscaras están en mm escalados).
    // s350 — el plano de corte, un pelo DENTRO del material: debajo del ancla en un sticker top,
    // encima en uno bottom (mira hacia abajo).
    const double z_cut = st.transform.translation().z()
                       + (ColorStitch::sticker_faces_down(st) ? kSectionBelowMm : -kSectionBelowMm);
    const ExPolygons& sec_obj = object_section(mo, z_cut);
    const Transform3d inv = st.transform.inverse();
    ExPolygons sec = sec_obj;
    auto to_local = [&inv](Polygon& pg) {
        for (Point& q : pg.points) {
            const Vec3d v = inv * Vec3d(unscale<double>(q.x()), unscale<double>(q.y()), 0.0);
            q = Point(coord_t(scale_(v.x())), coord_t(scale_(v.y())));
        }
    };
    for (ExPolygon& e : sec) { to_local(e.contour); for (Polygon& h : e.holes) to_local(h); }

    for (const ColorStitch::StickerColorMask& cm : local_masks(st, skey)) {
        const ExPolygons clipped = intersection_ex(cm.mask, sec);
        const std::vector<Vec2f> tris = triangulate_expolygons_2f(clipped);
        if (tris.size() < 3) continue;
        indexed_triangle_set its;
        its.vertices.reserve(tris.size());
        for (const Vec2f& p : tris) its.vertices.emplace_back(p.x(), p.y(), 0.f);
        its.indices.reserve(tris.size() / 3);
        for (size_t i = 0; i + 2 < tris.size(); i += 3)
            its.indices.emplace_back(int(i), int(i + 1), int(i + 2));
        auto m = std::make_unique<GLModel>();
        m->init_from(its);
        out.fills.push_back(std::move(m));
        out.color_idx.push_back(cm.color_idx);
    }
}

const Cached& cached_for(const ModelObject& mo, const ColorStitchSticker& st)
{
    const size_t skey = sticker_key(st);
    size_t key = skey;
    hmix(key, transform_key(st.transform));
    hmix(key, section_key(mo, st.transform.translation().z() + (ColorStitch::sticker_faces_down(st) ? 1.0 : -1.0)));
    auto it = g_cache.find(key);
    if (it == g_cache.end()) {
        if (g_cache.size() > 256) g_cache.clear();   // poda burda: mover un sticker deja claves viejas
        it = g_cache.emplace(key, Cached{}).first;
        build(mo, st, skey, it->second);
    }
    return it->second;
}

void draw_fills(const Cached& c, const ColorStitchSticker& st, float alpha,
                int focus_color = -1, int focus_b0 = -1, int focus_b1 = -1, float pulse = 0.f)
{
    auto& mgr = SurfaceEffectProfileManager::get();
    for (size_t k = 0; k < c.fills.size(); ++k) {
        GLModel& m = *c.fills[k];
        if (!m.is_initialized()) continue;
        const int pid = ColorStitch::sticker_profile_for(st, c.color_idx[k]);
        const SurfaceEffectProfile* p = pid ? mgr.find(pid) : nullptr;
        ColorRGBA col = p ? ColorStitchPaintPreview::color_for_profile(*p) : ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
        col[3] = alpha;
        if (focus_color >= 0) {   // s350 — resaltar la zona que se edita en el panel
            const int cidx = c.color_idx[k];
            const int ci = cidx >= ColorStitch::kBandStride ? cidx % ColorStitch::kBandStride : cidx;
            const int bi = cidx >= ColorStitch::kBandStride ? cidx / ColorStitch::kBandStride - 1 : -1;
            const bool on = ci == focus_color && (focus_b0 < 0 || (bi >= focus_b0 && bi <= focus_b1));
            if (on) {   // late: hacia blanco y opaco
                const float w = 0.12f + 0.38f * pulse;
                for (int i = 0; i < 3; ++i) col[i] = col[i] * (1.f - w) + w;
                col[3] = 1.f;
            } else {    // el resto, apagado
                for (int i = 0; i < 3; ++i) col[i] = 0.35f * col[i] + 0.2f;
                col[3] = alpha * 0.6f;
            }
        }
        m.set_color(col);
        m.render();
    }
}
} // namespace

namespace { std::unordered_map<size_t, Flat2D> g_flat2d; }

const Flat2D& flat_2d(const ColorStitchSticker& st)
{
    const size_t skey = sticker_key(st);
    auto it = g_flat2d.find(skey);
    if (it != g_flat2d.end()) return it->second;
    if (g_flat2d.size() > 64) g_flat2d.clear();
    Flat2D f;
    bool first = true;
    for (const ColorStitch::StickerColorMask& cm : local_masks(st, skey)) {
        std::vector<Vec2f> tris = triangulate_expolygons_2f(cm.mask);
        if (tris.size() < 3) continue;
        for (Vec2f& p : tris) {
            p = Vec2f(float(unscale<double>(p.x())), float(unscale<double>(p.y())));
            if (first) { f.bb_min = f.bb_max = p; first = false; }
            f.bb_min = f.bb_min.cwiseMin(p);
            f.bb_max = f.bb_max.cwiseMax(p);
        }
        f.tris.push_back(std::move(tris));
        f.color_idx.push_back(cm.color_idx);
        f.grad.push_back(cm.grad ? 1 : 0);
        f.g0.push_back(cm.g0);
        f.g1.push_back(cm.g1);
    }
    return g_flat2d.emplace(skey, std::move(f)).first->second;
}

void clear_cache() { g_cache.clear(); g_masks.clear(); g_sections.clear(); g_flat2d.clear(); }

double sticker_lift_mm(int pile_index) { return 0.03 + 0.02 * double(std::max(0, pile_index)); }
double sticker_lift_mm(const ColorStitchSticker& st, int pile_index)
{
    return ColorStitch::sticker_faces_down(st) ? -sticker_lift_mm(pile_index) : sticker_lift_mm(pile_index);
}

void render(const Model& model, const Camera& camera, int skip_object, int skip_sticker)
{
    bool any = false;
    for (const ModelObject* mo : model.objects)
        if (mo && !mo->colorstitch_stickers.empty()) { any = true; break; }
    if (!any) return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (!shader) return;

    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glEnable(GL_DEPTH_TEST));

    for (int oi = 0; oi < (int)model.objects.size(); ++oi) {
        const ModelObject* mo = model.objects[oi];
        if (!mo) continue;
        for (int si = 0; si < (int)mo->colorstitch_stickers.size(); ++si) {
            if (oi == skip_object && si == skip_sticker) continue;
            const ColorStitchSticker& st = mo->colorstitch_stickers[si];
            if (st.hidden || st.svg_data.empty()) continue;
            const Cached& c = cached_for(*mo, st);
            // Alzado en Z MUNDO contra el z-fighting, ESCALONADO por pila: dos stickers que se
            // solapan a la misma altura se mezclaban a franjas; el de arriba de la pila sube más.
            const Transform3d lift = Geometry::translation_transform(Vec3d(0.0, 0.0, sticker_lift_mm(st, si)));   // s350: bottom, hacia abajo
            for (const ModelInstance* mi : mo->instances) {
                if (!mi) continue;
                shader->set_uniform("view_model_matrix",
                    camera.get_view_matrix() * lift * mi->get_transformation().get_matrix() * st.transform);
                draw_fills(c, st, 1.f);
            }
        }
    }

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

void render_one(const ModelObject& mo, int sticker_idx, const Transform3d& instance_matrix,
                const Camera& camera, float alpha, int focus_color, int focus_b0, int focus_b1, float pulse)
{
    if (sticker_idx < 0 || sticker_idx >= (int)mo.colorstitch_stickers.size()) return;
    const ColorStitchSticker& st = mo.colorstitch_stickers[sticker_idx];
    if (st.svg_data.empty()) return;
    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (!shader) return;
    const Cached& c = cached_for(mo, st);
    const Transform3d lift = Geometry::translation_transform(Vec3d(0.0, 0.0, sticker_lift_mm(st, sticker_idx)));
    shader->start_using();
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * lift * instance_matrix * st.transform);
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glEnable(GL_DEPTH_TEST));
    draw_fills(c, st, alpha, focus_color, focus_b0, focus_b1, pulse);
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

}}} // namespace Slic3r::GUI::ColorStitchStickerPreview
