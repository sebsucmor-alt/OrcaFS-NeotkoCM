#include "GLGizmoTextureBump.hpp"
#include "TextureBumpConfigUI.hpp"
#include "TextureBumpOverlayMesh.hpp"
#include "TextureBumpPlaneHandles.hpp"
#include "ZBumpConfigUI.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/NeoDebug.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Geometry.hpp"
#include <wx/filedlg.h>
#include <GL/glew.h>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>

// NEOTKO_TEXTUREBUMP_TAG -- debug system (issues #3/#6 originally, still the shared channel for
// this feature). One log macro for the whole merged gizmo now -- was 2 near-identical copies
// (TEXTUREBUMP_GIZMO_LOG / TEXTUREBUMP_UI_LOG) split across the 2 files this unifies.
#define TEXTUREBUMP_GIZMO_LOG(body) do { if (Slic3r::NeoDebug::enabled(Slic3r::NeoDebug::TEXTUREBUMP)) { \
    std::ostringstream _tbgz_; _tbgz_ << body;                                                          \
    Slic3r::NeoDebug::write(Slic3r::NeoDebug::TEXTUREBUMP, _tbgz_.str()); } } while (0)

// NEOTKO_ZBUMP_TAG -- own channel/log file (ORCA_DEBUG_ZBUMP -> /tmp/neotko_zbump.log), same one
// ZBump.cpp's engine-side code already writes to -- so the overlay's calibration probe
// (update_zbump_overlay_and_grabbers()) and the engine's own probe (ZBumpHeightMap::build())
// land interleaved in the SAME file and can be diffed directly, point for point.
#define ZBUMP_GIZMO_LOG(body) do { if (Slic3r::NeoDebug::enabled(Slic3r::NeoDebug::ZBUMP)) { \
    std::ostringstream _zbgz_; _zbgz_ << body;                                               \
    Slic3r::NeoDebug::write(Slic3r::NeoDebug::ZBUMP, _zbgz_.str()); } } while (0)

namespace Slic3r {
namespace GUI {

namespace {

// Axis helpers mirroring plane_components() in TextureBump.cpp so the viewport preview never
// drifts from the actual wrap-plane math used at slice time (X wraps the YZ plane, Y wraps XZ, Z
// wraps XY). Shared by both modes' overlay builders (All mode's own object overlay, Painter
// mode's selected-zone overlay) -- previously 2 identical copies (one per file), now 1.
Vec3d texture_bump_perp_direction(TextureProjectionAxis axis)
{
    switch (axis) {
        case TextureProjectionAxis::X: return Vec3d::UnitY();
        case TextureProjectionAxis::Y: return Vec3d::UnitX();
        case TextureProjectionAxis::Z:
        default:                       return Vec3d::UnitX();
    }
}

// Rotates a shape built with its native revolution axis along local Z so it instead revolves
// around the selected world axis. Single-axis rotations only, so the result is unambiguous
// regardless of Geometry::rotation_transform()'s internal Euler application order.
Transform3d texture_bump_axis_rotation(TextureProjectionAxis axis)
{
    switch (axis) {
        case TextureProjectionAxis::X: return Geometry::rotation_transform(Vec3d(0.0, 0.5 * M_PI, 0.0));
        case TextureProjectionAxis::Y: return Geometry::rotation_transform(Vec3d(-0.5 * M_PI, 0.0, 0.0));
        case TextureProjectionAxis::Z:
        default:                       return Transform3d::Identity();
    }
}

// NEOTKO_TEXTUREBUMP_TAG -- deterministic preview color when a zone hasn't been given one
// explicitly. Golden-ratio hue stepping -> distinguishable hues for many ids, same idea
// GLGizmoColorMixPainter::fallback_color_for_id uses (kept as its own small copy here rather than
// shared, matching this gizmo's canvas being fully independent from ColorMix's).
ColorRGBA texture_bump_zone_color(int id)
{
    const float hue = std::fmod(0.61803398875f * float(id), 1.f);
    const float s = 0.55f, v = 0.85f;
    const float h6 = hue * 6.f;
    const int   i  = int(std::floor(h6)) % 6;
    const float f  = h6 - std::floor(h6);
    const float p = v * (1 - s);
    const float q = v * (1 - f * s);
    const float t = v * (1 - (1 - f) * s);
    float r = 0, g = 0, b = 0;
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
    return ColorRGBA(r, g, b, 1.f);
}

// NEOTKO_TEXTUREBUMP_TAG -- unification pass: minimal 2-option segmented bar for the All/Painter
// mode switch, same visual mechanism as GLGizmoColorMixPainter's own cs_segmented_bar()
// (GLGizmoColorMixPainter.cpp) -- not shared code (that one is file-local static there too), but
// deliberately the same technique: an InvisibleButton per segment over a hand-drawn background,
// no new widget type introduced.
void tb_mode_bar(const char* const* labels, const char* const* tips, int n, int& active)
{
    const float  avail = ImGui::GetContentRegionAvail().x;
    const float  h     = ImGui::GetFrameHeight();
    const float  seg_w = avail / (float)n;
    const bool   dark  = ImGuiWrapper::is_dark_mode();
    ImDrawList*  dl    = ImGui::GetWindowDrawList();
    const ImVec2 p0    = ImGui::GetCursorScreenPos();

    dl->AddRectFilled(p0, ImVec2(p0.x + avail, p0.y + h),
                      dark ? IM_COL32(32, 32, 35, 255) : IM_COL32(205, 205, 205, 255), 4.f);

    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        const ImVec2 a(p0.x + seg_w * (float)i, p0.y);
        const ImVec2 b(a.x + seg_w, a.y + h);
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##tb_mode_seg", ImVec2(seg_w, h))) active = i;
        const bool hov       = ImGui::IsItemHovered();
        const bool is_active = (i == active);
        if (is_active)
            dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(0.f, 0.59f, 0.53f, 0.85f)), 4.f);
        else if (hov)
            dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 20), 4.f);
        const ImVec2 tsz  = ImGui::CalcTextSize(labels[i]);
        const ImU32  tcol = is_active ? IM_COL32(255, 255, 255, 255)
                          : (dark ? IM_COL32(220, 220, 220, 255) : IM_COL32(50, 58, 61, 255));
        dl->AddText(ImVec2(a.x + (seg_w - tsz.x) * 0.5f, a.y + (h - tsz.y) * 0.5f), tcol, labels[i]);
        if (hov && tips[i] && *tips[i]) ImGui::SetTooltip("%s", tips[i]);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
    ImGui::Dummy(ImVec2(avail, 0.f));
}

} // anonymous namespace

GLGizmoTextureBump::GLGizmoTextureBump(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
{
}

bool GLGizmoTextureBump::on_init()
{
    m_grabbers.push_back(Grabber()); // index 0: V handle (texture_bump_scale)
    m_grabbers.push_back(Grabber()); // index 1: U handle (texture_bump_repeat_u)
    m_grabbers.push_back(Grabber()); // index 2: yaw ring handle (plane_transform rotation)
    m_grabbers.push_back(Grabber()); // index 3: pivot handle (plane_transform translation)
    m_grabbers[0].color       = ColorRGBA(0.3f, 0.6f, 1.0f, 1.0f);
    m_grabbers[0].hover_color = ColorRGBA(0.5f, 0.8f, 1.0f, 1.0f);
    m_grabbers[1].color       = ColorRGBA(1.0f, 0.6f, 0.2f, 1.0f);
    m_grabbers[1].hover_color = ColorRGBA(1.0f, 0.8f, 0.4f, 1.0f);
    m_grabbers[2].color       = ColorRGBA(0.7f, 0.3f, 1.0f, 1.0f);
    m_grabbers[2].hover_color = ColorRGBA(0.85f, 0.5f, 1.0f, 1.0f);
    m_grabbers[3].color       = ColorRGBA(1.0f, 0.3f, 0.5f, 1.0f);
    m_grabbers[3].hover_color = ColorRGBA(1.0f, 0.5f, 0.65f, 1.0f);
    // NEOTKO_ZBUMP_TAG -- index 4: Top mode's pan handle (zbump_offset_x/y). Only ever
    // enabled/rendered while m_mode == Top -- see update_zbump_overlay_and_grabbers() and
    // update_overlay_and_grabbers(), which each disable the other mode's grabbers.
    m_grabbers.push_back(Grabber());
    m_grabbers[4].color       = ColorRGBA(0.2f, 0.9f, 0.4f, 1.0f);
    m_grabbers[4].hover_color = ColorRGBA(0.4f, 1.0f, 0.6f, 1.0f);

    // Painter-mode cursor/section-view captions (GLGizmoPainterBase's own rendering reads these
    // regardless of which mode is active -- cheap, harmless to populate unconditionally).
    m_desc["clipping_of_view_caption"] = _L("Alt+") + _L("Mouse wheel");
    m_desc["clipping_of_view"]         = _L("Section view");
    m_desc["reset_direction"]          = _L("Reset direction");
    m_desc["paint_caption"]            = _L("Left mouse button");
    m_desc["paint"]                    = _L("Paint with selected zone");
    m_desc["erase_caption"]            = _L("Shift+") + _L("Left mouse button");
    m_desc["erase"]                    = _L("Erase paint");
    m_desc["remove_all"]               = _L("Erase all painting");
    return true;
}

std::string GLGizmoTextureBump::on_get_name() const
{
    return _u8L("Bump Mapping Editor");
}

bool GLGizmoTextureBump::on_is_activable() const
{
    // NEOTKO_TEXTUREBUMP_TAG — WIP gate, requested explicitly: only offered when the LibreMode
    // master switch is on AND the user opted into the debug/WIP build via
    // `export ORCA_DEBUG_ALL=1` (or ORCA_DEBUG_TEXTUREBUMP=1 specifically). Mirrors the
    // engine-side gate in PrintObject::make_perimeters(). Layered on top of GLGizmoPainterBase's
    // own FFF + single-full-instance check (needed now that this class is painter-derived).
    return wxGetApp().app_config != nullptr
        && wxGetApp().app_config->get_bool("neotko_libre_enabled")
        && NeoDebug::enabled(NeoDebug::TEXTUREBUMP)
        && GLGizmoPainterBase::on_is_activable();
}

bool GLGizmoTextureBump::on_is_selectable() const
{
    // NEOTKO_TEXTUREBUMP_TAG — controls whether the icon appears in the toolbar at all
    // (GLGizmosManager::get_selectable_idxs() only renders gizmos that pass this check).
    return wxGetApp().app_config != nullptr
        && wxGetApp().app_config->get_bool("neotko_libre_enabled")
        && NeoDebug::enabled(NeoDebug::TEXTUREBUMP)
        && GLGizmoPainterBase::on_is_selectable();
}

ModelObject* GLGizmoTextureBump::get_model_object() const
{
    const Selection& selection = m_parent.get_selection();
    const int        obj_idx   = selection.get_object_idx();
    return (obj_idx >= 0 && selection.get_model() != nullptr)
        ? selection.get_model()->objects[obj_idx] : nullptr;
}

// NEOTKO_TEXTUREBUMP_TAG -- ROOT CAUSE FOUND (real audit, 2026-07-08): confirmed by tracing
// GLCanvas3D::_render_objects(Opaque,...) -- it does `dynamic_cast<GLGizmoPainterBase*>(current
// gizmo)` and, if non-null, skips the normal GLVolumeCollection opaque render pass ENTIRELY for
// the whole scene, delegating all drawing to render_painter_gizmo() instead. Since this gizmo is
// now always GLGizmoPainterBase-derived (both modes), the real GLVolume this function dims is
// NEVER DRAWN while the gizmo is open -- writing its color.a() was always cosmetically inert here,
// in either mode. It compiled and "ran" (see the log this function still writes) but had nothing
// to visually affect. The actual fix lives in build_ebt_colors_for_volume() -- render_triangles()
// (called every frame in render_painter_gizmo(), both branches) IS what draws the visible surface,
// and its shader (mm_gouraud.fs:114, `float alpha = uniform_color.a;`) DOES honor that struct's
// alpha channel.
//
// This function is kept only as a safety net for the moment the gizmo CLOSES: while it was open
// the dimmed color.a() sat on a GLVolume nobody drew, but the instant the gizmo deactivates,
// GLCanvas3D::_render_objects resumes its normal pass and WOULD draw that same (still-dimmed)
// GLVolume -- on_shutdown() calling this with transparent=false is what prevents the object from
// visibly staying at 25% opacity after the gizmo closes. `GLVolume::color` (not `render_color`) is
// still the right field for that: GLVolumeCollection::render() recomputes render_color FROM color
// every frame (3DScene.cpp), so writing render_color directly would just get overwritten.
void GLGizmoTextureBump::set_object_transparent(bool transparent)
{
    Selection& selection = m_parent.get_selection();
    const int  obj_idx   = selection.get_object_idx();
    if (obj_idx < 0)
        return;
    const std::vector<unsigned int> vol_idxs = selection.get_volume_idxs_from_object(static_cast<unsigned int>(obj_idx));

    if (transparent) {
        // Only LEARN the starting alpha on the first frame of a dim session (cache empty) -- on
        // every later frame (called every frame, see above) just re-stamp 0.25f without touching
        // the cache, or by frame 2 we'd "learn" 0.25f as the real original and corrupt the restore.
        if (m_dimmed_volume_original_alpha.empty())
            for (unsigned int idx : vol_idxs)
                if (GLVolume* v = selection.get_volume(idx))
                    m_dimmed_volume_original_alpha.emplace_back(idx, v->color.a());
        for (unsigned int idx : vol_idxs)
            if (GLVolume* v = selection.get_volume(idx))
                v->color.a(0.25f);
    } else {
        for (const auto& [idx, alpha] : m_dimmed_volume_original_alpha)
            if (GLVolume* v = selection.get_volume(idx))
                v->color.a(alpha);
        m_dimmed_volume_original_alpha.clear();
    }
    m_parent.set_as_dirty();
}

// NEOTKO_TEXTUREBUMP_TAG -- unification pass: replaces the old GLGizmoTextureBump::on_set_state().
// GLGizmoPainterBase::on_set_state() (inherited, NOT overridden here) already does the
// picking-disable/on_opening()/on_shutdown() dance the Painter pipeline needs -- overriding
// on_set_state() again would have silently skipped that. on_opening() is the correct extension
// point for "gizmo just turned on" in a GLGizmoPainterBase-derived class.
void GLGizmoTextureBump::on_opening()
{
    // NEOTKO_TEXTUREBUMP_TAG -- fix (real audit, 2026-07-08): GLGizmoPainterBase::on_set_state()
    // calls m_parent.enable_picking(false) right before invoking this -- that disables
    // GLCanvas3D::_picking_pass() entirely (GLCanvas3D.cpp), which is the ONLY thing that updates
    // m_hover_id from mouse movement. All mode's 3D grabbers (use_grabbers()/render_grabbers(),
    // GLGizmoBase's own mechanism) depend on m_hover_id -- with picking off they silently stop
    // responding to hover/click. Same fix GLGizmoColorMixPainter::on_set_state() already applies
    // for the identical reason (GLGizmoColorMixPainter.cpp:164, "en On apaga picking; lo
    // re-encendemos") -- re-enable right after the base's own disable.
    m_parent.enable_picking(true);

    if (ModelObject* model_object = get_model_object()) {
        // resolve_base_texture_bump_config() doesn't read plane_transform (not a ConfigOption,
        // zone-seeding callers intentionally leave it at Identity for a new zone) -- overlay it here.
        m_config = resolve_base_texture_bump_config(model_object);
        m_config.plane_transform = model_object->texture_bump_plane_transform;
        // NEOTKO_ZBUMP_TAG -- Mode::Top's own shadow config, resolved the same "object config
        // falls back to global preset" way, own function/struct (ZBumpConfigUI.hpp).
        m_zbump_config = resolve_base_zbump_config(model_object);
    }
    m_overlay_dirty = true;
    set_object_transparent(true);
}

void GLGizmoTextureBump::on_shutdown()
{
    set_object_transparent(false);
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
    // GLGizmoPainterBase::on_set_state() already re-enables picking on the Off transition before
    // calling this -- explicit here too for clarity/safety, same as GLGizmoColorMixPainter::on_shutdown().
    m_parent.enable_picking(true);
}

PainterGizmoType GLGizmoTextureBump::get_painter_type() const
{
    // No dedicated enum value (informational only, never switched on) -- same choice
    // GLGizmoColorMixPainter makes for its own independent multi-slot canvas.
    return PainterGizmoType::MM_SEGMENTATION;
}

wxString GLGizmoTextureBump::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button /*button_down*/) const
{
    return (shift_down || m_erase_mode) ? _L("Erase Texture Bump paint") : _L("Texture Bump paint");
}

// NEOTKO_TEXTUREBUMP_TAG -- All mode uses GLGizmoBase's grabber pipeline directly (use_grabbers()),
// bypassing GLGizmoPainterBase::on_mouse()'s brush pipeline entirely; Painter mode uses the brush
// pipeline. This was flagged as the one real architectural risk of unifying the 2 gizmos, but
// turned out mechanical: GLGizmoTextureBump::on_mouse() was ALREADY just
// `return use_grabbers(mouse_event);` before this merge, and GLGizmoPainterBase::on_mouse() never
// touches grabbers, so the 2 paths never fight over the same input.
bool GLGizmoTextureBump::on_mouse(const wxMouseEvent& mouse_event)
{
    // NEOTKO_ZBUMP_TAG -- Top now has one 3D handle too (pan, grabber 4) -- same grabber pipeline
    // as All, just a single enabled grabber (see update_zbump_overlay_and_grabbers()).
    if (m_mode == Mode::All || m_mode == Mode::Top)
        return use_grabbers(mouse_event);
    return GLGizmoPainterBase::on_mouse(mouse_event);
}

double GLGizmoTextureBump::compute_repeat_step_mm(const BoundingBoxf3& box) const
{
    const Vec3d  size = box.size();
    double       d1, d2;
    switch (m_config.axis) {
        case TextureProjectionAxis::X: d1 = size.y(); d2 = size.z(); break;
        case TextureProjectionAxis::Y: d1 = size.x(); d2 = size.z(); break;
        case TextureProjectionAxis::Z:
        default:                       d1 = size.x(); d2 = size.y(); break;
    }
    const double reference_span_mm = (m_config.projection_mode == TextureProjectionMode::Cubic) ? std::max(d1, d2) : M_PI * std::max(d1, d2);
    return std::max(reference_span_mm / 24.0, 2.0); // ~24 steps across the full drag range, floor to avoid absurd sensitivity on tiny objects
}

void GLGizmoTextureBump::update_overlay_and_grabbers()
{
    const auto& [box, box_trafo] = m_parent.get_selection().get_bounding_box_in_current_reference_system();
    m_cached_bbox = box;

    const Vec3d  size          = box.size();
    const Vec3d  center        = box.center();
    const Vec3d  perp_dir      = texture_bump_perp_direction(m_config.axis);
    const double repeat_step_mm = compute_repeat_step_mm(box);
    m_drag_repeat_step_mm = repeat_step_mm;

    const double overlay_margin_mm = std::max(0.01 * std::max({ size.x(), size.y(), size.z() }), 0.2);

    BoundingBoxf3 tb_bounds;
    tb_bounds.min = Vec3d(box.min.x(), box.min.y(), 0.0);
    tb_bounds.max = Vec3d(box.max.x(), box.max.y(), size.z());
    const Transform3d overlay_to_tb_bounds = Geometry::translation_transform(Vec3d(0.0, 0.0, -box.min.z()));

    indexed_triangle_set its;
    Transform3d          overlay_local_transform = Transform3d::Identity();
    switch (m_config.projection_mode) {
        case TextureProjectionMode::Cylindrical:
        case TextureProjectionMode::Spherical: {
            const double radius = std::max(0.5 * std::max(size.x(), size.y()), 1.0) + overlay_margin_mm;
            const Vec3d  offset = (m_config.projection_mode == TextureProjectionMode::Cylindrical)
                ? Vec3d(center.x(), center.y(), box.min.z() - overlay_margin_mm)
                : center;
            its = (m_config.projection_mode == TextureProjectionMode::Cylindrical)
                ? its_make_cylinder(radius, std::max(size.z(), 1.0) + 2.0 * overlay_margin_mm)
                : its_make_sphere(radius, double(PI) / 24.0);
            overlay_local_transform = Geometry::translation_transform(offset) * texture_bump_axis_rotation(m_config.axis);
            break;
        }
        case TextureProjectionMode::Cubic: {
            its = its_make_cube(std::max(size.x(), 1.0), std::max(size.y(), 1.0), std::max(size.z(), 1.0));
            overlay_local_transform = Geometry::translation_transform(box.min);
            break;
        }
        case TextureProjectionMode::Planar:
        default: {
            const double thin = std::max(0.02 * std::max({ size.x(), size.y(), size.z() }), 0.2);
            its = its_make_cube(std::max(size.x(), 1.0), std::max(size.y(), 1.0), thin);
            overlay_local_transform = Geometry::translation_transform(Vec3d(box.min.x(), box.min.y(), box.min.z() + m_config.scale));
            break;
        }
    }
    m_overlay_transform = box_trafo * overlay_local_transform;
    m_overlay_model.reset();
    m_overlay_model.init_from(build_texture_bump_overlay_geometry(its, overlay_to_tb_bounds * overlay_local_transform, m_config.projection_mode,
                                                                   m_config.axis, tb_bounds, m_config.scale, m_config.repeat_u, m_config.plane_transform));

    if (ModelObject* model_object = get_model_object()) {
        const DynamicPrintConfig& obj_cfg = model_object->config.get();
        const DynamicPrintConfig& glb_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        std::string image_path;
        if (const ConfigOption* opt = obj_cfg.option("texture_bump_image_path"))
            image_path = static_cast<const ConfigOptionString*>(opt)->value;
        else if (const ConfigOption* opt = glb_cfg.option("texture_bump_image_path"))
            image_path = static_cast<const ConfigOptionString*>(opt)->value;
        if (image_path != m_preview_texture_path) {
            m_preview_texture.reset();
            bool load_ok = true;
            if (!image_path.empty())
                load_ok = m_preview_texture.load_from_file(image_path, true, GLTexture::None, false);
            TEXTUREBUMP_GIZMO_LOG("preview_texture_load path='" << image_path << "' ok=" << load_ok
                << " gl_id=" << m_preview_texture.get_id());
            m_preview_texture_path = image_path;
        }
    }

    const double corner_margin = std::max(0.1 * std::max(size.x(), size.y()), 5.0);
    const Vec3d  v_anchor_xy(box.max.x() + corner_margin, box.min.y() - corner_margin, 0.0);
    m_grabbers[0].matrix = box_trafo;
    m_grabbers[0].center = Vec3d(v_anchor_xy.x(), v_anchor_xy.y(), box.min.z() + m_config.scale);
    m_grabber_ref_point[0] = Vec3d(v_anchor_xy.x(), v_anchor_xy.y(), box.min.z());

    const double perp_half_extent = 0.5 * std::max(size.x(), size.y());
    m_grabbers[1].matrix = box_trafo;
    m_grabbers[1].center = center + perp_dir * (perp_half_extent + double(m_config.repeat_u) * repeat_step_mm);
    m_grabber_ref_point[1] = center;

    double plane_d1, plane_d2;
    switch (m_config.axis) {
        case TextureProjectionAxis::X: plane_d1 = size.y(); plane_d2 = size.z(); break;
        case TextureProjectionAxis::Y: plane_d1 = size.x(); plane_d2 = size.z(); break;
        case TextureProjectionAxis::Z:
        default:                       plane_d1 = size.x(); plane_d2 = size.y(); break;
    }
    const double plane_radius = 0.5 * std::max(plane_d1, plane_d2) * 1.15;
    const double plane_coord  = (m_config.axis == TextureProjectionAxis::X) ? center.x()
                               : (m_config.axis == TextureProjectionAxis::Y) ? center.y() : center.z();
    const double yaw       = texture_bump_plane_transform_yaw(m_config.plane_transform);
    const Vec2d  pivot_ab   = texture_bump_plane_transform_pivot(m_config.plane_transform);
    m_grabbers[2].matrix = box_trafo;
    m_grabbers[2].center = texture_bump_ab_to_world(m_config.axis, center,
        pivot_ab.x() + plane_radius * std::cos(yaw), pivot_ab.y() + plane_radius * std::sin(yaw), plane_coord);
    m_grabbers[3].matrix = box_trafo;
    m_grabbers[3].center = texture_bump_ab_to_world(m_config.axis, center, pivot_ab.x(), pivot_ab.y(), plane_coord);

    // NEOTKO_ZBUMP_TAG -- only All mode's own handles 0-3 should be pickable while this function's
    // result is current; Top mode's pan handle (4) is owned by update_zbump_overlay_and_grabbers().
    for (int i = 0; i < 4; ++i)
        m_grabbers[i].enabled = true;
    if (m_grabbers.size() > 4)
        m_grabbers[4].enabled = false;

    m_overlay_dirty = false;
}

// NEOTKO_ZBUMP_TAG -- Top mode's own overlay + single pan grabber. Always a flat plane at the
// object's own top Z (box.max.z(), where the relief is physically applied) sized to its XY
// footprint -- no axis/projection-mode choice like Texture Bump, so this builds its own minimal
// 2-triangle quad directly rather than reusing build_texture_bump_overlay_geometry() (that one's
// compute_u() handles axis/Cubic/Cylindrical/Spherical cases ZBump doesn't have). UV must match
// ZBumpHeightMap::build()'s own u/v formula (ZBump.cpp) exactly -- what's previewed here is a lie
// otherwise.
void GLGizmoTextureBump::update_zbump_overlay_and_grabbers()
{
    const auto& [box, box_trafo] = m_parent.get_selection().get_bounding_box_in_current_reference_system();
    m_zbump_cached_bbox = box;

    const Vec3d size = box.size();
    // NEOTKO_ZBUMP_TAG -- bug fix (real report + user-confirmed test: removing perimeters made
    // the overlay line up "almost perfect", pointing straight at this). Top-fill never reaches
    // the object's own outer contour -- wall_loops perimeter loops sit in that band first. The
    // overlay previously spanned the full object footprint edge-to-edge, showing an extra ring of
    // pattern that never actually prints (walls are flat, ZBump only touches stTop fill). Crop the
    // quad to the same approximate inset. Clamped to 45% per side so a tiny object with thick
    // walls can't invert the quad.
    const double raw_inset_mm = std::max(0.0, zbump_perimeter_inset_mm(get_model_object()));
    const double inset_mm = std::min({raw_inset_mm, size.x() * 0.45, size.y() * 0.45});
    const Vec3d  inset_min(box.min.x() + inset_mm, box.min.y() + inset_mm, box.max.z());
    const Vec3d  inset_max(box.max.x() - inset_mm, box.max.y() - inset_mm, box.max.z());
    const Vec3d  inset_size(inset_max.x() - inset_min.x(), inset_max.y() - inset_min.y(), 0.0);
    const Transform3d local_to_bounds = Geometry::translation_transform(inset_min);
    m_zbump_overlay_transform = box_trafo * local_to_bounds;

    const double tile_mm = std::max(0.1, m_zbump_config.scale_mm);
    const int    repeat  = std::max(1, m_zbump_config.repeat);
    const Vec3d  bbox_center = box.center();
    // NEOTKO_ZBUMP_TAG -- bug fix (real report: overlay rendered/panned fine but never matched
    // the actual slice): UV must be computed relative to the object's OWN raw-bbox center, the
    // same frame ZBumpHeightMap::build() samples in (Fill.cpp's zbump_bounds spans
    // [-size/2, +size/2] around that center, mirroring PrintObject::trafo_centered() -- see
    // lessons_key.md s161, "trafo_centered(), never trafo(), for masks/paint compared against
    // slice"). box.min()/box.max() from the Selection API are NOT guaranteed centered at zero in
    // whatever "current reference system" they report -- re-basing by box.center() here sidesteps
    // that entirely: "distance from the box's own center" is the same quantity either way. Must
    // match on_dragging()'s handle-4 math below, which already offsets by
    // m_zbump_cached_bbox.center() for the same reason.
    // NEOTKO_ZBUMP_TAG -- bug fix (real report: overlay showed the pattern far too large/
    // stretched compared to the actual slice). ZBumpHeightMap::build() (ZBump.cpp) wraps u/v to
    // [0,1] with fmod because it bakes a per-CELL raster (hundreds of samples across the
    // footprint) -- fine there. This overlay is a flat quad with only 4 VERTICES total; wrapping
    // per-corner and then linearly interpolating BETWEEN already-wrapped corner values can't
    // reproduce a texture meant to repeat many times across the object (confirmed: at scale=4.5mm
    // on a much bigger plate, the overlay rendered one huge stretched smear instead of a fine
    // tiled pattern). Leave u/v RAW and unwrapped -- possibly large values -- and let the GPU's
    // native texture-repeat sampling tile it, same as Texture Bump's own overlay builder already
    // does (TextureBumpOverlayMesh.cpp: `u = u_canonical * num_periods`, no fmod anywhere).
    // NEOTKO_ZBUMP_TAG -- U-flip tried earlier here was the wrong diagnosis (calibration probe
    // proved side=gui's u exactly matched side=engine's u -- this formula was already right) --
    // reverted, stays matching the engine. Root cause found instead in GLTexture::load_from_png()
    // (GLTexture.cpp ~774-790, shared by every GLTexture user in the app -- confirmed by the same
    // mirroring showing up in Texture Bump's OWN mature All-mode overlay too, not just this new
    // one): it copies wxImage's row-major top-to-bottom pixel data straight into glTexImage2D with
    // no vertical flip, so texel row 0 (GL's v=0) ends up holding the SOURCE IMAGE's TOP row, not
    // its bottom -- the textbook OpenGL image-loading gotcha. NOT fixed at that source: it's a
    // shared utility (also used for the plate logo, toolbar backgrounds, environment texture --
    // see grep for load_from_file callers) and flipping its output would silently flip whichever
    // of THOSE already happen to work with the current (unflipped) behavior. Compensating here
    // instead, local to this one texture's consumption.
    auto uv_at = [&](double x_mm, double y_mm) -> Vec2f {
        const double ex = x_mm - bbox_center.x();
        const double ey = y_mm - bbox_center.y();
        const double u =  (ex - m_zbump_config.offset_x_mm) / tile_mm * double(repeat);
        const double v = -(ey - m_zbump_config.offset_y_mm) / tile_mm * double(repeat);
        return Vec2f(float(u), float(v));
    };

    // NEOTKO_ZBUMP_TAG -- calibration probe (GUI/overlay vs. real slice cross-check). Same 5
    // center-relative test points, same log file (ORCA_DEBUG_ZBUMP), as ZBumpHeightMap::build()'s
    // own probe (ZBump.cpp) -- diff the two directly: if object_bounds_center disagrees, or u
    // disagrees (should match engine exactly, confirmed clean), that's still worth flagging. v is
    // now EXPECTED to be the negative of engine's v at each point (the load_from_png() V-flip
    // compensation above) -- gui.v ~= -engine.v is the correct/passing case, not a new bug.
    if (NeoDebug::enabled(NeoDebug::ZBUMP)) {
        ZBUMP_GIZMO_LOG("overlay_build side=gui object_bounds_min=(" << box.min.x() << "," << box.min.y() << ")"
            << " object_bounds_max=(" << box.max.x() << "," << box.max.y() << ")"
            << " object_bounds_center=(" << bbox_center.x() << "," << bbox_center.y() << ")"
            << " inset_min=(" << inset_min.x() << "," << inset_min.y() << ")"
            << " inset_max=(" << inset_max.x() << "," << inset_max.y() << ")"
            << " perimeter_inset_mm=" << inset_mm
            << " tile_mm=" << tile_mm << " repeat=" << repeat
            << " offset_x_mm=" << m_zbump_config.offset_x_mm << " offset_y_mm=" << m_zbump_config.offset_y_mm);
        auto probe = [&](double dx, double dy) {
            const Vec2f uv = uv_at(bbox_center.x() + dx, bbox_center.y() + dy);
            ZBUMP_GIZMO_LOG("uv_probe side=gui dx=" << dx << " dy=" << dy
                << " world_x=" << (bbox_center.x() + dx) << " world_y=" << (bbox_center.y() + dy)
                << " u=" << uv.x() << " v=" << uv.y());
        };
        probe(0, 0);
        probe(10, 0);
        probe(-10, 0);
        probe(0, 10);
        probe(0, -10);

        // NEOTKO_ZBUMP_TAG -- render-pipeline probe, added after the uv_probe above proved the
        // u/v formula itself is NOT the source of the reported mirror (see comment on uv_at()).
        // Two independent checks on the "handedness got flipped somewhere between UV and screen
        // pixels" hypothesis (the projection/box_trafo the user suspected):
        // 1) determinant of box_trafo's linear (rotation+scale) part -- negative means it
        //    contains a reflection (odd number of axis flips) full stop, regardless of vertex
        //    layout or camera. If this comes back negative, that alone explains a mirrored
        //    render and points straight at get_bounding_box_in_current_reference_system()'s
        //    "current reference system" rather than anything specific to this quad.
        // 2) world-space position of the two quad corners with the most different u (inset_min
        //    vs inset_max in X) -- confirms which SCREEN side (more-negative vs more-positive
        //    world X) each end of the image actually lands on, independent of any assumption
        //    about which way "Top view" projects world X onto screen X.
        const double det = box_trafo.linear().determinant();
        const Vec3d  world_corner_lowU = m_zbump_overlay_transform * Vec3d(0.0, 0.0, 0.0);            // local (0,0) -> uv_at(inset_min.x, inset_min.y)
        const Vec3d  world_corner_hiU  = m_zbump_overlay_transform * Vec3d(inset_size.x(), 0.0, 0.0);  // local (W,0) -> uv_at(inset_max.x, inset_min.y)
        ZBUMP_GIZMO_LOG("render_probe side=gui box_trafo_linear_det=" << det << " (negative=contains a reflection)"
            << " lowU=" << uv_at(inset_min.x(), inset_min.y()).x() << " at world_xyz=(" << world_corner_lowU.x() << "," << world_corner_lowU.y() << "," << world_corner_lowU.z() << ")"
            << " hiU="  << uv_at(inset_max.x(), inset_min.y()).x() << " at world_xyz=(" << world_corner_hiU.x()  << "," << world_corner_hiU.y()  << "," << world_corner_hiU.z()  << ")");
        // NEOTKO_ZBUMP_TAG -- same check, V axis (first round only tested U). A left-right
        // "mirror" report and a top-bottom flip can look similar at a glance on garbled/rotated
        // text, so ruling V in or out with the same rigor rather than assuming it's fine.
        const Vec3d world_corner_lowV = m_zbump_overlay_transform * Vec3d(0.0, 0.0, 0.0);            // local (0,0) -> uv_at(inset_min.x, inset_min.y)
        const Vec3d world_corner_hiV  = m_zbump_overlay_transform * Vec3d(0.0, inset_size.y(), 0.0);  // local (0,H) -> uv_at(inset_min.x, inset_max.y)
        ZBUMP_GIZMO_LOG("render_probe_v side=gui"
            << " lowV=" << uv_at(inset_min.x(), inset_min.y()).y() << " at world_xyz=(" << world_corner_lowV.x() << "," << world_corner_lowV.y() << "," << world_corner_lowV.z() << ")"
            << " hiV="  << uv_at(inset_min.x(), inset_max.y()).y() << " at world_xyz=(" << world_corner_hiV.x()  << "," << world_corner_hiV.y()  << "," << world_corner_hiV.z()  << ")");
    }

    GLModel::Geometry data;
    data.format.type          = GLModel::Geometry::EPrimitiveType::Triangles;
    data.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3T2;
    data.reserve_vertices(4);
    data.reserve_indices(6);
    // Local corners span [0,inset_size.x()] x [0,inset_size.y()] at z=0 -- local_to_bounds above
    // already places that quad at world (inset_min.x(), inset_min.y(), box.max.z()).
    data.add_vertex(Vec3f(0.f, 0.f, 0.f), uv_at(inset_min.x(), inset_min.y()));
    data.add_vertex(Vec3f(float(inset_size.x()), 0.f, 0.f), uv_at(inset_max.x(), inset_min.y()));
    data.add_vertex(Vec3f(float(inset_size.x()), float(inset_size.y()), 0.f), uv_at(inset_max.x(), inset_max.y()));
    data.add_vertex(Vec3f(0.f, float(inset_size.y()), 0.f), uv_at(inset_min.x(), inset_max.y()));
    data.add_triangle(0, 1, 2);
    data.add_triangle(0, 2, 3);

    m_zbump_overlay_model.reset();
    m_zbump_overlay_model.init_from(std::move(data));

    // Preview texture: own cache slot, separate from Texture Bump's m_preview_texture (different
    // image/path -- see the header comment on m_zbump_preview_texture).
    if (m_zbump_config.image_path != m_zbump_preview_texture_path) {
        m_zbump_preview_texture.reset();
        if (!m_zbump_config.image_path.empty())
            m_zbump_preview_texture.load_from_file(m_zbump_config.image_path, true, GLTexture::None, false);
        m_zbump_preview_texture_path = m_zbump_config.image_path;
    }

    // Single pan grabber, resting at (object center + current offset) so it's always drawn where
    // the pattern is actually centered, and dragging from its rendered position starts smoothly
    // (see on_dragging()'s handle-4 branch: it sets the offset directly, no delta math needed,
    // because this rest position already encodes the offset). Disabled along with the rest of the
    // overlay when the feature itself is off -- render_painter_gizmo()'s Top branch already skips
    // drawing it in that case, this keeps it un-pickable too instead of an invisible live grabber.
    m_grabbers[4].matrix  = box_trafo;
    m_grabbers[4].center  = Vec3d(box.center().x() + m_zbump_config.offset_x_mm,
                                   box.center().y() + m_zbump_config.offset_y_mm, box.max.z());
    m_grabbers[4].enabled = m_zbump_config.enabled;
    for (int i = 0; i < 4; ++i)
        m_grabbers[i].enabled = false;

    m_zbump_overlay_dirty = false;
}

void GLGizmoTextureBump::on_start_dragging()
{
    assert(m_hover_id >= 0 && m_hover_id <= 4);
    m_dragging_handle_id       = m_hover_id;
    m_drag_starting_scale_mm   = m_config.scale;
    m_drag_starting_repeat_u   = m_config.repeat_u;
    m_drag_starting_plane_transform = m_config.plane_transform;
    if (m_hover_id == 0 || m_hover_id == 1) {
        m_drag_starting_handle_pos = m_grabbers[m_hover_id].matrix * m_grabbers[m_hover_id].center;
        m_drag_starting_box_center = m_grabbers[m_hover_id].matrix * m_grabber_ref_point[m_hover_id];
        if (m_hover_id == 1)
            m_drag_repeat_step_mm = compute_repeat_step_mm(m_cached_bbox);
    } else if (m_hover_id == 4) {
        m_zbump_drag_starting_offset_x = m_zbump_config.offset_x_mm;
        m_zbump_drag_starting_offset_y = m_zbump_config.offset_y_mm;
    }
}

double GLGizmoTextureBump::calc_projection(const UpdateData& data) const
{
    double projection = 0.0;
    const Vec3d  starting_vec     = m_drag_starting_handle_pos - m_drag_starting_box_center;
    const double len_starting_vec = starting_vec.norm();
    if (len_starting_vec != 0.0) {
        const Vec3d mouse_dir  = data.mouse_ray.unit_vector();
        const Vec3d inters     = data.mouse_ray.a + (m_drag_starting_handle_pos - data.mouse_ray.a).dot(mouse_dir) * mouse_dir;
        const Vec3d inters_vec = inters - m_drag_starting_handle_pos;
        projection = inters_vec.dot(starting_vec.normalized());
    }
    return projection;
}

void GLGizmoTextureBump::on_dragging(const UpdateData& data)
{
    if (m_dragging_handle_id == 0 || m_dragging_handle_id == 1) {
        const double delta_mm = calc_projection(data);
        if (m_dragging_handle_id == 0) {
            m_config.scale = std::clamp(m_drag_starting_scale_mm + delta_mm, 0.1, 1000.0);
        } else {
            m_config.repeat_u = std::clamp(m_drag_starting_repeat_u + int(std::lround(delta_mm / m_drag_repeat_step_mm)), 1, 50);
        }
        m_overlay_dirty = true;
    } else if (m_dragging_handle_id == 2 || m_dragging_handle_id == 3) {
        // NEOTKO_ZBUMP_TAG -- bug fix (user-confirmed: same root cause as ZBump's own handle 4).
        // center/normal here were LOCAL space (m_cached_bbox is local, box_trafo is what maps it
        // to world) but data.mouse_ray is world space, and texture_bump_world_to_ab()'s own name
        // says it wants a WORLD center -- silently treated local (0,0) as world (0,0), correct
        // only when the object happens to sit exactly at plate origin. Push center/normal through
        // box_trafo before either call; no separate "convert hit back" step needed afterwards,
        // texture_bump_world_to_ab() already does exactly that conversion internally.
        const auto& [box, box_trafo] = m_parent.get_selection().get_bounding_box_in_current_reference_system();
        const Vec3d center = box_trafo * box.center();
        const Vec3d normal = (box_trafo.linear() * texture_bump_plane_normal(m_config.axis)).normalized();
        const Vec3d hit    = texture_bump_ray_plane_intersection(data.mouse_ray.a, data.mouse_ray.unit_vector(), center, normal);
        const Vec2d hit_ab = texture_bump_world_to_ab(m_config.axis, center, hit);
        if (m_dragging_handle_id == 3) {
            m_config.plane_transform = texture_bump_make_plane_transform(texture_bump_plane_transform_yaw(m_drag_starting_plane_transform), hit_ab);
        } else {
            const Vec2d  starting_pivot = texture_bump_plane_transform_pivot(m_drag_starting_plane_transform);
            const Vec2d  dv = hit_ab - starting_pivot;
            const double new_yaw = (dv.norm() > 1e-6) ? std::atan2(dv.y(), dv.x()) : texture_bump_plane_transform_yaw(m_drag_starting_plane_transform);
            m_config.plane_transform = texture_bump_make_plane_transform(new_yaw, starting_pivot);
        }
        m_overlay_dirty = true;
    } else if (m_dragging_handle_id == 4) {
        // NEOTKO_ZBUMP_TAG -- pan handle: set the offset directly from where the mouse ray hits
        // the object's top-Z plane, relative to the object's own bbox center (not world/plate
        // origin like handle 3's plane-local ab -- ZBump has no axis/plane_transform concept).
        // No drag-start bookkeeping needed: the grabber's rest position already encodes
        // (center + offset) (see update_zbump_overlay_and_grabbers()), so grabbing it and moving
        // the mouse even slightly starts from a hit point ~= the current offset -- continuous by
        // construction, same reasoning that lets handle 3 do a direct set instead of a delta.
        //
        // NEOTKO_ZBUMP_TAG -- bug fix (real report: dragging treated plate origin as the object's
        // center). box/box_trafo from get_bounding_box_in_current_reference_system() are LOCAL
        // space -- box_trafo is what maps them to world -- but data.mouse_ray is WORLD space.
        // Intersecting a plane built straight from local coordinates against a world ray silently
        // treats local (0,0) as world (0,0): correct only if the object happens to sit exactly at
        // plate origin. Fix: build the plane point/normal locally, push them through box_trafo
        // into world space for the intersection, then pull the result back with box_trafo's
        // inverse so it's comparable to box.center() (also local) again. Same root cause as All
        // mode's handles 2/3 above -- fixed there too, same pattern (that one doesn't need the
        // "pull back to local" step since texture_bump_world_to_ab() does that conversion itself).
        const auto& [box, box_trafo] = m_parent.get_selection().get_bounding_box_in_current_reference_system();
        const Vec3d local_plane_pt(0.0, 0.0, box.max.z());
        const Vec3d local_normal(0.0, 0.0, 1.0);
        const Vec3d world_plane_pt = box_trafo * local_plane_pt;
        const Vec3d world_normal   = (box_trafo.linear() * local_normal).normalized();
        const Vec3d world_hit = texture_bump_ray_plane_intersection(data.mouse_ray.a, data.mouse_ray.unit_vector(), world_plane_pt, world_normal);
        const Vec3d local_hit = box_trafo.inverse() * world_hit;
        const Vec3d center    = box.center();
        m_zbump_config.offset_x_mm = local_hit.x() - center.x();
        m_zbump_config.offset_y_mm = local_hit.y() - center.y();
        m_zbump_overlay_dirty = true;
    }
}

void GLGizmoTextureBump::on_stop_dragging()
{
    ModelObject* model_object = get_model_object();
    if (model_object != nullptr) {
        if (m_dragging_handle_id == 0 && m_config.scale != m_drag_starting_scale_mm) {
            wxGetApp().plater()->take_snapshot("Texture Bump: set scale");
            model_object->config.set_key_value("texture_bump_scale", new ConfigOptionFloat(m_config.scale));
            wxGetApp().plater()->update();
        } else if (m_dragging_handle_id == 1 && m_config.repeat_u != m_drag_starting_repeat_u) {
            wxGetApp().plater()->take_snapshot("Texture Bump: set repeat");
            model_object->config.set_key_value("texture_bump_repeat_u", new ConfigOptionInt(m_config.repeat_u));
            wxGetApp().plater()->update();
        } else if ((m_dragging_handle_id == 2 || m_dragging_handle_id == 3)
                   && m_config.plane_transform.matrix() != m_drag_starting_plane_transform.matrix()) {
            wxGetApp().plater()->take_snapshot("Texture Bump: set plane transform");
            model_object->texture_bump_plane_transform = m_config.plane_transform;
            wxGetApp().plater()->update();
        } else if (m_dragging_handle_id == 4
                   && (m_zbump_config.offset_x_mm != m_zbump_drag_starting_offset_x
                       || m_zbump_config.offset_y_mm != m_zbump_drag_starting_offset_y)) {
            wxGetApp().plater()->take_snapshot("Z Bump: pan");
            model_object->config.set_key_value("zbump_offset_x", new ConfigOptionFloat(m_zbump_config.offset_x_mm));
            model_object->config.set_key_value("zbump_offset_y", new ConfigOptionFloat(m_zbump_config.offset_y_mm));
            m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        }
    }
    m_dragging_handle_id = -1;
    if (m_mode == Mode::Top)
        m_zbump_overlay_dirty = true;
    else
        m_overlay_dirty = true;
}

void GLGizmoTextureBump::on_register_raycasters_for_picking()
{
    // the gizmo grabbers are rendered on top of the scene, so the raytraced picker should take it
    // into account (same pattern GLGizmoMove3D uses). Only meaningful in All mode (Painter mode's
    // own raycaster is the CommonGizmosDataID::Raycaster GLGizmoPainterBase already requests).
    m_parent.set_raycaster_gizmos_on_top(true);
}

void GLGizmoTextureBump::on_unregister_raycasters_for_picking()
{
    m_parent.set_raycaster_gizmos_on_top(false);
}

// NEOTKO_TEXTUREBUMP_TAG -- commit callback for render_texture_bump_config_sections() in All mode:
// m_config has already been mutated in place by the shared form by the time this runs -- this only
// decides WHERE that field's new value is persisted.
void GLGizmoTextureBump::commit_config_field(ModelObject* model_object, const char* field)
{
    const std::string f = field;
    bool needs_overlay_refresh = false;

    if (f == "type") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set type");
        model_object->config.set_key_value("texture_bump", new ConfigOptionEnum<TextureBumpType>(m_config.type));
        needs_overlay_refresh = true;
    } else if (f == "image_path") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set image");
        model_object->config.set_key_value("texture_bump_image_path", new ConfigOptionString(m_config.image_path));
        needs_overlay_refresh = true;
    } else if (f == "projection_mode") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set projection");
        model_object->config.set_key_value("texture_bump_projection_mode", new ConfigOptionEnum<TextureProjectionMode>(m_config.projection_mode));
        needs_overlay_refresh = true;
    } else if (f == "axis") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set axis");
        model_object->config.set_key_value("texture_bump_axis", new ConfigOptionEnum<TextureProjectionAxis>(m_config.axis));
        needs_overlay_refresh = true;
    } else if (f == "scale") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set scale");
        model_object->config.set_key_value("texture_bump_scale", new ConfigOptionFloat(m_config.scale));
        needs_overlay_refresh = true;
    } else if (f == "repeat_u") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set repeat");
        model_object->config.set_key_value("texture_bump_repeat_u", new ConfigOptionInt(m_config.repeat_u));
        needs_overlay_refresh = true;
    } else if (f == "plane_yaw" || f == "plane_pivot" || f == "plane_reset") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set plane transform");
        model_object->texture_bump_plane_transform = m_config.plane_transform;
        needs_overlay_refresh = true;
    } else if (f == "thickness") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set thickness");
        model_object->config.set_key_value("texture_bump_thickness", new ConfigOptionFloat(unscale_(m_config.thickness)));
    } else if (f == "point_distance") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set point distance");
        model_object->config.set_key_value("texture_bump_point_distance", new ConfigOptionFloat(unscale_(m_config.point_distance)));
    } else if (f == "first_layer") {
        wxGetApp().plater()->take_snapshot("Texture Bump: toggle first layer");
        model_object->config.set_key_value("texture_bump_first_layer", new ConfigOptionBool(m_config.first_layer));
    } else if (f == "max_angle") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set max angle");
        model_object->config.set_key_value("texture_bump_max_angle", new ConfigOptionFloat(m_config.max_angle_rad * 180.0 / M_PI));
    } else if (f == "blur_strength") {
        wxGetApp().plater()->take_snapshot("Texture Bump: set blur strength");
        model_object->config.set_key_value("texture_bump_blur_strength", new ConfigOptionFloat(m_config.blur_strength));
    }

    // NEOTKO_TEXTUREBUMP_TAG -- fix (real audit, 2026-07-08): was `wxGetApp().plater()->update()`,
    // called SYNCHRONOUSLY from inside this same ImGui frame (this runs from the slider's
    // deactivated_after_edit branch in render_texture_bump_config_sections(), still inside
    // GizmoImguiBegin/End). update() cascades into View3D::reload_scene() ->
    // GLCanvas3D::reload_scene(), which rebuilds the whole GLVolumeCollection and calls
    // m_gizmos.update_data() synchronously, mid-frame -- that's what caused every slider to snap
    // back on release (visually drags fine, but the commit itself re-entered scene/gizmo state
    // before the current ImGui frame had finished). None of the other GLGizmoPainterBase-derived
    // gizmos with working sliders in this codebase (GLGizmoFuzzySkin, GLGizmoMmuSegmentation,
    // GLGizmoColorMixPainter) call plater()->update() from inside a slider's commit path -- they
    // all defer via this exact async event instead (already the pattern update_model_object() in
    // this same file already used for facet-paint commits, GLGizmoTextureBump.cpp above). Scene
    // reload isn't needed here anyway: m_overlay_dirty already drives the gizmo's own overlay
    // refresh next frame; the background slicer picks up the changed config/plane_transform on
    // its own next apply() regardless of how it was scheduled.
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    if (needs_overlay_refresh)
        m_overlay_dirty = true;
}

// NEOTKO_ZBUMP_TAG -- Mode::Top's commit callback, same "one ConfigOption per field" pattern as
// commit_config_field() above, own domain (zbump_* keys, no shared code with Texture Bump).
void GLGizmoTextureBump::commit_zbump_config_field(ModelObject* model_object, const char* field)
{
    const std::string f = field;
    bool needs_overlay_refresh = false;

    if (f == "enabled") {
        wxGetApp().plater()->take_snapshot("Z Bump: toggle enabled");
        model_object->config.set_key_value("zbump_enabled", new ConfigOptionBool(m_zbump_config.enabled));
    } else if (f == "image_path") {
        wxGetApp().plater()->take_snapshot("Z Bump: set image");
        model_object->config.set_key_value("zbump_image_path", new ConfigOptionString(m_zbump_config.image_path));
        needs_overlay_refresh = true;
    } else if (f == "thickness") {
        wxGetApp().plater()->take_snapshot("Z Bump: set height");
        model_object->config.set_key_value("zbump_thickness", new ConfigOptionFloat(m_zbump_config.thickness_mm));
    } else if (f == "scale") {
        wxGetApp().plater()->take_snapshot("Z Bump: set scale");
        model_object->config.set_key_value("zbump_scale", new ConfigOptionFloat(m_zbump_config.scale_mm));
        needs_overlay_refresh = true;
    } else if (f == "repeat") {
        wxGetApp().plater()->take_snapshot("Z Bump: set repeat");
        model_object->config.set_key_value("zbump_repeat", new ConfigOptionInt(m_zbump_config.repeat));
        needs_overlay_refresh = true;
    } else if (f == "offset_x") {
        wxGetApp().plater()->take_snapshot("Z Bump: pan X");
        model_object->config.set_key_value("zbump_offset_x", new ConfigOptionFloat(m_zbump_config.offset_x_mm));
        needs_overlay_refresh = true;
    } else if (f == "offset_y") {
        wxGetApp().plater()->take_snapshot("Z Bump: pan Y");
        model_object->config.set_key_value("zbump_offset_y", new ConfigOptionFloat(m_zbump_config.offset_y_mm));
        needs_overlay_refresh = true;
    } else if (f == "edge_margin") {
        wxGetApp().plater()->take_snapshot("Z Bump: set edge ramp");
        model_object->config.set_key_value("zbump_edge_margin", new ConfigOptionFloat(m_zbump_config.edge_margin_mm));
    } else if (f == "max_slope") {
        wxGetApp().plater()->take_snapshot("Z Bump: set max slope");
        model_object->config.set_key_value("zbump_max_slope", new ConfigOptionFloat(m_zbump_config.max_slope));
    } else if (f == "first_layer") {
        wxGetApp().plater()->take_snapshot("Z Bump: toggle first layer");
        model_object->config.set_key_value("zbump_first_layer", new ConfigOptionBool(m_zbump_config.first_layer));
    } else if (f == "relief_segment") {
        wxGetApp().plater()->take_snapshot("Z Bump: set relief segment length");
        model_object->config.set_key_value("zbump_relief_segment", new ConfigOptionFloat(m_zbump_config.relief_segment_mm));
    } else if (f == "max_passes") {
        wxGetApp().plater()->take_snapshot("Z Bump: set reinforcement passes");
        model_object->config.set_key_value("zbump_max_passes", new ConfigOptionInt(m_zbump_config.max_passes));
    }

    // Same deferred-reslice reasoning as commit_config_field() above (EVT_GLCANVAS_SCHEDULE_
    // BACKGROUND_PROCESS, not a synchronous plater()->update()).
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    if (needs_overlay_refresh)
        m_zbump_overlay_dirty = true;
}

// ============================================================================
// Painter mode (absorbed from the deleted GLGizmoTextureBumpPainter, OLDNOUSE)
// ============================================================================

void GLGizmoTextureBump::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return;
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++idx;
        if (idx >= (int)m_triangle_selectors.size() || !m_triangle_selectors[idx]) continue;
        updated |= mv->texture_bump_paint_facets.set(*m_triangle_selectors[idx]);
    }
    if (updated) {
        const ModelObjectPtrs& mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoTextureBump::update_from_model_object(bool /*first_update*/)
{
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    m_triangle_selectors.clear();
    m_selected_zone_id = 0;
    m_active_slot       = 0;
    if (!mo) return;

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, build_ebt_colors_for_volume(mv)));
        const EnforcerBlockerType max_ebt = static_cast<EnforcerBlockerType>(MAX_SLOTS - 1);
        m_triangle_selectors.back()->deserialize(mv->texture_bump_paint_facets.get_data(), false, max_ebt);
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
    }
}

int GLGizmoTextureBump::slot_for_selected_zone(bool assign_if_missing)
{
    if (m_selected_zone_id == 0) return 0;
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return 0;

    int existing_slot = 0;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        for (int s = 1; s < MAX_SLOTS; ++s) {
            if (mv->texture_bump_slot_to_zone_id[s] == m_selected_zone_id) { existing_slot = s; break; }
        }
        if (existing_slot) break;
    }
    if (existing_slot)
        return existing_slot;
    if (!assign_if_missing)
        return 0;

    for (int s = 1; s < MAX_SLOTS; ++s) {
        bool free_everywhere = true;
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part()) continue;
            if (mv->texture_bump_slot_to_zone_id[s] != 0) { free_everywhere = false; break; }
        }
        if (free_everywhere) {
            for (ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                mv->texture_bump_slot_to_zone_id[s] = m_selected_zone_id;
            }
            return s;
        }
    }
    return 0; // all slots taken -- caller shows the "full" warning.
}

EnforcerBlockerType GLGizmoTextureBump::get_left_button_state_type() const
{
    if (m_erase_mode)
        return EnforcerBlockerType::NONE;
    if (m_selected_zone_id != 0) {
        const int slot = const_cast<GLGizmoTextureBump*>(this)->slot_for_selected_zone(/*assign_if_missing=*/true);
        if (slot >= 1 && slot < MAX_SLOTS) {
            const_cast<GLGizmoTextureBump*>(this)->refresh_ebt_colors();
            return static_cast<EnforcerBlockerType>(slot);
        }
        return EnforcerBlockerType::NONE;
    }
    if (m_active_slot >= 1 && m_active_slot < MAX_SLOTS)
        return static_cast<EnforcerBlockerType>(m_active_slot);
    return EnforcerBlockerType::NONE;
}

void GLGizmoTextureBump::refresh_ebt_colors()
{
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo)
        return;
    int idx = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++idx;
        if (idx >= int(m_triangle_selectors.size())) break;
        if (auto* tsp = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[idx].get())) {
            tsp->set_ebt_colors(build_ebt_colors_for_volume(mv));
            tsp->request_update_render_data();
        }
    }
}

// NEOTKO_TEXTUREBUMP_TAG -- fix (real audit, 2026-07-08): this is the ACTUAL surface the user
// sees while this gizmo is open, in BOTH modes -- render_triangles()'s "mm_gouraud" shader reads
// `uniform_color.a` straight into the fragment's output alpha (resources/shaders/140/mm_gouraud.fs:114),
// so it DOES respect alpha here. `set_object_transparent()`/`GLVolume::color.a()` do not: while
// this gizmo is the current gizmo, GLCanvas3D::_render_objects(Opaque,...) skips the normal
// GLVolumeCollection render pass entirely for ANY GLGizmoPainterBase-derived gizmo
// (GLCanvas3D.cpp, `dynamic_cast<GLGizmoPainterBase*>` gate) and defers ALL drawing to
// render_painter_gizmo() -- the real GLVolume is never drawn while the gizmo is open, in either
// mode, so dimming its color was always a no-op for visibility (kept only as a restore-on-close
// safety net, see set_object_transparent()'s own comment). Alpha < 1 here is what actually lets
// the user see through the object -- same uniform dim level for the unpainted surface (slot 0)
// and any painted zone (the zone's own PNG preview is a SEPARATE overlay mesh, render_zone_overlay(),
// drawn on top -- dimming the base mesh doesn't hide that).
std::vector<ColorRGBA> GLGizmoTextureBump::build_ebt_colors_for_volume(const ModelVolume* mv) const
{
    static_assert(MAX_SLOTS == ModelVolume::COLORMIX_SLOT_COUNT,
                  "gizmo MAX_SLOTS must match ModelVolume::COLORMIX_SLOT_COUNT (3mf slot table)");
    // NEOTKO_TEXTUREBUMP_TAG -- fix (2026-07-08, real user report on the shipped s183 fix): a single
    // shared alpha for both the unpainted base surface and the painted-zone highlight was wrong on
    // two counts once actually seen live -- (1) the base surface needs to be MORE transparent than
    // 0.35 alone suggests, because render_triangles() draws with GL_CULL_FACE disabled
    // (GLGizmoPainterBase.cpp) -- front AND back faces both blend, compounding 0.35 to ~58%
    // effective opacity, well past "see-through"; (2) the painted-zone highlight needs to read MORE
    // solidly than the base so the user can actually tell which triangles are assigned -- 0.35 made
    // it look about as transparent as the empty parts of the object. Two separate constants now.
    // NEOTKO_TEXTUREBUMP_TAG -- tuning (2026-07-08): base bumped 0.15 -> 0.18 (-20% transparency,
    // i.e. +20% relative opacity) per user request, applies to both modes (shared function).
    constexpr float kBaseAlpha = 0.18f;
    constexpr float kZoneAlpha = 0.75f;
    std::vector<ColorRGBA> ebt(MAX_SLOTS, ColorRGBA(0.6f, 0.6f, 0.6f, kBaseAlpha));
    ebt[0] = ColorRGBA(GLVolume::NEUTRAL_COLOR.r(), GLVolume::NEUTRAL_COLOR.g(), GLVolume::NEUTRAL_COLOR.b(), kBaseAlpha);
    if (mv) {
        for (int s = 1; s < MAX_SLOTS; ++s) {
            const int zid = mv->texture_bump_slot_to_zone_id[s];
            if (zid == 0) continue;
            if (TextureBumpZoneManager::get().find(zid)) {
                const ColorRGBA c = texture_bump_zone_color(zid);
                ebt[s] = ColorRGBA(c.r(), c.g(), c.b(), kZoneAlpha);
            }
        }
    }
    return ebt;
}

void GLGizmoTextureBump::create_new_zone_from_base_config()
{
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    TextureBumpZoneProfile zone;
    zone.config = resolve_base_texture_bump_config(mo);
    m_selected_zone_id = TextureBumpZoneManager::get().add(std::move(zone));
}

void GLGizmoTextureBump::create_new_blank_zone()
{
    TextureBumpZoneProfile zone;
    zone.config = resolve_base_texture_bump_config(nullptr);
    m_selected_zone_id = TextureBumpZoneManager::get().add(std::move(zone));
}

void GLGizmoTextureBump::commit_and_reslice()
{
    // NEOTKO_TEXTUREBUMP_TAG -- fix (real audit, 2026-07-08): same root cause and same fix as
    // commit_config_field() above -- was a synchronous plater()->update() call from inside the
    // zone editor's own slider commit path (render_texture_bump_config_sections(), called from
    // render_zone_editor(), still inside the same ImGui frame), which is what caused every zone
    // slider to snap back on release (this was the original, never-fixed s181 bug -- the save/
    // restore of m_selected_zone_id/m_active_slot below was a workaround for reload_scene()'s
    // update_from_model_object() zeroing them, not for the revert itself). The async event
    // doesn't trigger reload_scene() synchronously, so the save/restore is likely no longer
    // strictly needed, but left in place as harmless insurance.
    const int saved_zone_id = m_selected_zone_id;
    const int saved_slot    = m_active_slot;
    m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    m_selected_zone_id = saved_zone_id;
    m_active_slot      = saved_slot;
}

GLTexture* GLGizmoTextureBump::zone_thumbnail(const TextureBumpZoneProfile& zone)
{
    if (zone.config.image_path.empty())
        return nullptr;
    GLTexture& tex = m_zone_thumbnails[zone.id];
    if (m_zone_thumbnail_paths[zone.id] != zone.config.image_path) {
        tex.reset();
        tex.load_from_file(zone.config.image_path, true, GLTexture::None, false);
        m_zone_thumbnail_paths[zone.id] = zone.config.image_path;
    }
    return (tex.get_id() != 0) ? &tex : nullptr;
}

void GLGizmoTextureBump::render_zone_list()
{
    const auto& zones = TextureBumpZoneManager::get().list();
    bump_ui_section(bump_section_icons().projection, _u8L("Zones"));
    if (zones.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        m_imgui->text_wrapped(_u8L("No zones yet. Create one, then paint with it."),
                              bump_ui_label_width() + bump_ui_control_width());
        ImGui::PopStyleColor();
    }
    for (const auto& z : zones) {
        ImGui::PushID(z.id);
        const bool  selected = (z.id == m_selected_zone_id);
        const float row_h    = ImGui::GetFrameHeight();

        const ColorRGBA swatch = texture_bump_zone_color(z.id);
        const ImVec2    p0     = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + row_h, p0.y + row_h),
            ImGui::ColorConvertFloat4ToU32(ImVec4(swatch.r(), swatch.g(), swatch.b(), 1.0f)), 3.0f);
        ImGui::Dummy(ImVec2(row_h, row_h));
        ImGui::SameLine();

        if (GLTexture* thumb = zone_thumbnail(z)) {
            ImGui::Image((ImTextureID)(intptr_t) thumb->get_id(), ImVec2(row_h, row_h));
            ImGui::SameLine();
        }

        if (ImGui::Selectable((z.name + "##tb_zone_select").c_str(), selected))
            m_selected_zone_id = selected ? 0 : z.id;
        ImGui::PopID();
    }
    if (m_imgui->button(_u8L("New zone"))) {
        create_new_blank_zone();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Create a blank zone with default settings.").c_str());
    ImGui::SameLine();
    if (m_imgui->button(_u8L("From object"))) {
        create_new_zone_from_base_config();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Create a zone pre-filled with the object's base (All mode) settings.").c_str());
}

void GLGizmoTextureBump::render_zone_editor(TextureBumpZoneProfile& zone)
{
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.35f));
    ImGui::Separator();

    // Header row: zone color swatch + name, Delete right-aligned. Delete is handled FIRST so the
    // rest of the editor (which holds `zone` by reference) is never drawn on the removal frame.
    ImGui::AlignTextToFramePadding();
    const ColorRGBA swatch = texture_bump_zone_color(zone.id);
    const float     swz    = ImGui::GetFontSize() * 0.85f;
    const ImVec2    sp     = ImGui::GetCursorScreenPos();
    const float     cy     = sp.y + ImGui::GetFrameHeight() * 0.5f;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(sp.x, cy - swz * 0.5f), ImVec2(sp.x + swz, cy + swz * 0.5f),
        ImGui::ColorConvertFloat4ToU32(ImVec4(swatch.r(), swatch.g(), swatch.b(), 1.0f)), 3.0f);
    ImGui::Dummy(ImVec2(swz, 0.0f));
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.35f);
    m_imgui->text(zone.name);

    const float btn_w = ImGui::CalcTextSize(_u8L("Delete").c_str()).x + ImGui::GetStyle().FramePadding.x * 4.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + ImGui::GetFontSize(),
                             bump_ui_label_width() + bump_ui_control_width() - btn_w));
    const bool delete_clicked = m_imgui->button(_u8L("Delete"));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Delete this zone and clear everything painted with it.").c_str());

    if (!delete_clicked) {
        render_texture_bump_config_sections(m_imgui, zone.config,
            [&](const char* /*field*/) { commit_and_reslice(); },
            /*show_plane_handles_hint=*/false);
        return;
    }

    {
        if (ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr) {
            int idx = -1;
            for (ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                ++idx;
                EnforcerBlockerStateMap state_map;
                for (size_t i = 0; i < state_map.size(); ++i)
                    state_map[i] = static_cast<EnforcerBlockerType>(i); // identity by default
                bool any_slot_cleared = false;
                for (int s = 1; s < MAX_SLOTS; ++s) {
                    if (mv->texture_bump_slot_to_zone_id[s] == zone.id) {
                        mv->texture_bump_slot_to_zone_id[s] = 0;
                        state_map[s] = EnforcerBlockerType::NONE;
                        any_slot_cleared = true;
                    }
                }
                if (any_slot_cleared && idx < int(m_triangle_selectors.size()) && m_triangle_selectors[idx]) {
                    m_triangle_selectors[idx]->remap_triangle_state(state_map);
                    m_triangle_selectors[idx]->request_update_render_data(true);
                }
            }
        }
        TextureBumpZoneManager::get().remove(zone.id);
        m_zone_thumbnails.erase(zone.id);
        m_zone_thumbnail_paths.erase(zone.id);
        m_selected_zone_id = 0;
        m_active_slot      = 0;
        refresh_ebt_colors();
        update_model_object(); // persists the remapped (now-cleared) facets + schedules a re-slice
        wxGetApp().plater()->update();
    }
}

void GLGizmoTextureBump::render_zone_overlay()
{
    if (m_selected_zone_id == 0)
        return;
    const TextureBumpZoneProfile* zone = TextureBumpZoneManager::get().find(m_selected_zone_id);
    if (!zone || zone->config.image_path.empty())
        return;

    const auto& [box, box_trafo] = m_parent.get_selection().get_bounding_box_in_current_reference_system();

    const bool dirty_zone_id = m_zone_overlay_zone_id != m_selected_zone_id;
    const bool dirty_mode    = m_zone_overlay_cached_mode != zone->config.projection_mode;
    const bool dirty_axis    = m_zone_overlay_cached_axis != zone->config.axis;
    const bool dirty_scale   = m_zone_overlay_cached_scale != zone->config.scale;
    const bool dirty_repeat  = m_zone_overlay_cached_repeat_u != zone->config.repeat_u;
    const bool dirty_plane   = m_zone_overlay_cached_plane_transform.matrix() != zone->config.plane_transform.matrix();
    const bool dirty_bbox    = !(m_zone_overlay_cached_bbox == box);
    const bool dirty = dirty_zone_id || dirty_mode || dirty_axis || dirty_scale || dirty_repeat || dirty_plane || dirty_bbox;

    if (dirty) {
        const Vec3d  size   = box.size();
        const Vec3d  center = box.center();
        const double scale_mm = std::max(zone->config.scale, 1e-6);
        const double overlay_margin_mm = std::max(0.01 * std::max({ size.x(), size.y(), size.z() }), 0.2);
        BoundingBoxf3 tb_bounds;
        tb_bounds.min = Vec3d(box.min.x(), box.min.y(), 0.0);
        tb_bounds.max = Vec3d(box.max.x(), box.max.y(), size.z());
        const Transform3d overlay_to_tb_bounds = Geometry::translation_transform(Vec3d(0.0, 0.0, -box.min.z()));

        indexed_triangle_set its;
        Transform3d          overlay_local_transform = Transform3d::Identity();
        switch (zone->config.projection_mode) {
            case TextureProjectionMode::Cylindrical:
            case TextureProjectionMode::Spherical: {
                const double radius = std::max(0.5 * std::max(size.x(), size.y()), 1.0) + overlay_margin_mm;
                const Vec3d  offset = (zone->config.projection_mode == TextureProjectionMode::Cylindrical)
                    ? Vec3d(center.x(), center.y(), box.min.z() - overlay_margin_mm)
                    : center;
                its = (zone->config.projection_mode == TextureProjectionMode::Cylindrical)
                    ? its_make_cylinder(radius, std::max(size.z(), 1.0) + 2.0 * overlay_margin_mm)
                    : its_make_sphere(radius, double(PI) / 24.0);
                overlay_local_transform = Geometry::translation_transform(offset) * texture_bump_axis_rotation(zone->config.axis);
                break;
            }
            case TextureProjectionMode::Cubic:
                overlay_local_transform = Geometry::translation_transform(box.min);
                break; // its unused for Cubic (see build_texture_bump_overlay_geometry())
            case TextureProjectionMode::Planar:
            default: {
                const double thin = std::max(0.02 * std::max({ size.x(), size.y(), size.z() }), 0.2);
                its = its_make_cube(std::max(size.x(), 1.0), std::max(size.y(), 1.0), thin);
                overlay_local_transform = Geometry::translation_transform(Vec3d(box.min.x(), box.min.y(), box.min.z() + scale_mm));
                break;
            }
        }
        m_zone_overlay_transform = box_trafo * overlay_local_transform;
        m_zone_overlay_model.reset();
        m_zone_overlay_model.init_from(build_texture_bump_overlay_geometry(its, overlay_to_tb_bounds * overlay_local_transform,
                                                                       zone->config.projection_mode, zone->config.axis, tb_bounds,
                                                                       scale_mm, zone->config.repeat_u, zone->config.plane_transform));

        if (zone->config.image_path != m_zone_overlay_texture_path) {
            m_zone_overlay_texture.reset();
            const bool load_ok = m_zone_overlay_texture.load_from_file(zone->config.image_path, true, GLTexture::None, false);
            TEXTUREBUMP_GIZMO_LOG("zone_overlay_texture_load zone_id=" << zone->id << " path='" << zone->config.image_path
                << "' ok=" << load_ok << " gl_id=" << m_zone_overlay_texture.get_id());
            m_zone_overlay_texture_path = zone->config.image_path;
        }

        m_zone_overlay_zone_id              = m_selected_zone_id;
        m_zone_overlay_cached_mode          = zone->config.projection_mode;
        m_zone_overlay_cached_axis          = zone->config.axis;
        m_zone_overlay_cached_scale         = zone->config.scale;
        m_zone_overlay_cached_repeat_u      = zone->config.repeat_u;
        m_zone_overlay_cached_plane_transform = zone->config.plane_transform;
        m_zone_overlay_cached_bbox          = box;
    }

    if (m_zone_overlay_texture.get_id() == 0)
        return;

    // NEOTKO_TEXTUREBUMP_TAG -- fix (s184, real user report): depth-mask-off alone wasn't enough --
    // GL_DEPTH_TEST was still ON (enabled by the caller for the whole Painter branch in
    // render_painter_gizmo()), so this quad was still depth-TESTED against the object's surface
    // (written by render_triangles() right before) and lost the test almost everywhere, since this
    // preview sits at/behind the very surface it's projecting onto. That's the reported "the cube
    // still covers the visible texture zone, making it not very useful" -- and it defeats the whole
    // point of dimming the object (s183 transparency fix): this preview must be visible THROUGH the
    // object, not occluded by it. Disable depth test for this draw only, restore it right after.
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glDepthMask(GL_FALSE));
    GLShaderProgram* shader = wxGetApp().get_shader("flat_texture");
    if (shader != nullptr) {
        shader->start_using();
        const Camera& camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        shader->set_uniform("view_model_matrix", camera.get_view_matrix() * m_zone_overlay_transform);
        glsafe(::glBlendColor(0.0f, 0.0f, 0.0f, 0.45f));
        glsafe(::glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_zone_overlay_texture.get_id()));
        m_zone_overlay_model.render();
        glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        shader->stop_using();
    }
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

// ============================================================================
// Render dispatch (All mode = old on_render() body; Painter mode = old
// GLGizmoTextureBumpPainter::render_painter_gizmo() body, unchanged)
// ============================================================================

void GLGizmoTextureBump::render_painter_gizmo()
{
    // Reapplied every frame regardless of mode -- see set_object_transparent()'s own comment.
    set_object_transparent(true);

    if (m_mode == Mode::All) {
        const Selection& selection = m_parent.get_selection();
        if (!selection.is_single_full_instance())
            return;

        if (m_overlay_dirty)
            update_overlay_and_grabbers();

        glsafe(::glEnable(GL_DEPTH_TEST));
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

        // NEOTKO_TEXTUREBUMP_TAG -- fix (real audit, 2026-07-08): draw the real object ourselves
        // now, using the SAME render_triangles() Painter mode already relies on (dimmed via
        // build_ebt_colors_for_volume()'s alpha, see its own comment). GLCanvas3D::_render_objects
        // (Opaque,...) skips the normal GLVolumeCollection render pass entirely whenever ANY
        // GLGizmoPainterBase-derived gizmo is current -- true in both modes since the unification
        // merge, so nothing else draws the object anymore; previously (as a plain GLGizmoBase) the
        // normal scene pass always drew it before this gizmo's own on_render() ran.
        render_triangles(selection);

        // NEOTKO_TEXTUREBUMP_TAG -- fix (2026-07-08, real bug found while investigating the "object
        // disappears" report): the plane-overlay draw below had NO gate on m_config.type at all --
        // it always rendered as soon as ANY preview texture had ever been loaded, even with
        // Texture Bump set to None. Confirmed live: screenshot with "Texture Bump: None" still shows
        // the full textured projection surface. Gate it here.
        if (m_config.type != TextureBumpType::None) {
        // NEOTKO_TEXTUREBUMP_TAG -- fix (2026-07-08, real user report, round 2): the previous fix
        // here (real depth-test against the object, no clear) DID stop the plane from always
        // covering the object -- but it introduced the opposite problem, reported live: on curved
        // projections (Spherical/Cylindrical) the plane's curved overlay interleaves with the
        // object's own (possibly non-matching) surface in a per-pixel-varying way, so it comes out
        // visibly "cut"/patchy wherever the object's surface happens to be nearer the camera.
        // Cubic barely showed it (flat faces, little depth ambiguity against the bbox), which is
        // exactly why it went unnoticed until Spherical/Cylindrical were tested. Painter mode's own
        // projection overlay (render_zone_overlay()) hit the same "object covers overlay" complaint
        // and was fixed by disabling depth test entirely (x-ray) -- confirmed working well by the
        // user once the object's own base alpha was properly tuned down (kBaseAlpha=0.15). Applying
        // the same x-ray treatment here for consistency: with the object now genuinely transparent,
        // "always on top" no longer means "hides the object" the way it did back when this
        // complaint was first raised (the object wasn't rendering correctly at all at that point).
        glsafe(::glDisable(GL_DEPTH_TEST));
        glsafe(::glDepthMask(GL_FALSE));

        const bool has_preview_texture = m_preview_texture.get_id() != 0;
        GLShaderProgram* shader = wxGetApp().get_shader(has_preview_texture ? "flat_texture" : "flat");
        if (shader != nullptr) {
            shader->start_using();
            const Camera& camera = wxGetApp().plater()->get_camera();
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            shader->set_uniform("view_model_matrix", camera.get_view_matrix() * m_overlay_transform);
            if (has_preview_texture) {
                glsafe(::glBlendColor(0.0f, 0.0f, 0.0f, 0.45f));
                glsafe(::glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA));
                glsafe(::glBindTexture(GL_TEXTURE_2D, m_preview_texture.get_id()));
                m_overlay_model.render();
                glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
                glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            } else {
                m_overlay_model.set_color(ColorRGBA(0.3f, 0.6f, 1.0f, 0.35f));
                m_overlay_model.render();
            }
            shader->stop_using();
        }
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glEnable(GL_DEPTH_TEST));
        } // m_config.type != TextureBumpType::None
        glsafe(::glDisable(GL_BLEND));

        // NEOTKO_TEXTUREBUMP_TAG -- fix (2026-07-08, real bug, confirmed via GLCanvas3D.cpp): this
        // used to be glClear(GL_DEPTH_BUFFER_BIT), which wipes the depth buffer for the WHOLE
        // canvas, not just where the grabbers sit. That was harmless in the old pre-unification code
        // (a plain GLGizmoBase), where the normal GLVolumeCollection pass drew the object and its
        // real depth was safe by the time this ran. Post-unification this IS
        // GLGizmoPainterBase-derived, so _render_objects(Opaque,...) (GLCanvas3D.cpp) skips the
        // normal pass and calls THIS function instead to draw the object -- and _render_bed() runs
        // right AFTER _render_objects(Opaque,...) in the same frame (GLCanvas3D.cpp:1977-1981). A
        // full-buffer clear here erases the object's just-written depth before the bed draws,
        // so the bed's opaque geometry then passes the depth test everywhere and paints over the
        // object with zero protection -- confirmed live: from above, the bed visibly covers the
        // object; from below (bed's backface likely culled, nothing draws there to overwrite
        // anything), the object shows fine. Fix: don't touch the depth buffer's contents at all --
        // just disable the depth TEST for the grabbers draw (same x-ray technique already used for
        // the plane above and Painter's own zone overlay), so nothing after this point in the frame
        // loses the real depth information the object and bed both need.
        glsafe(::glDisable(GL_DEPTH_TEST));
        render_grabbers(m_cached_bbox);
        glsafe(::glEnable(GL_DEPTH_TEST));
    } else if (m_mode == Mode::Painter) {
        const Selection& selection = m_parent.get_selection();

        glsafe(::glEnable(GL_BLEND));
        glsafe(::glEnable(GL_DEPTH_TEST));

        render_triangles(selection);
        m_c->object_clipper()->render_cut();
        m_c->instances_hider()->render_cut();
        render_zone_overlay();
        render_cursor();

        glsafe(::glDisable(GL_BLEND));
    } else {
        // NEOTKO_ZBUMP_TAG -- Mode::Top: height-map overlay + pan handle. Still need to draw the
        // real object ourselves (GLGizmoPainterBase suppresses the normal opaque pass for every
        // mode of this gizmo, same reasoning as the All-mode comment above).
        const Selection& selection = m_parent.get_selection();
        glsafe(::glEnable(GL_DEPTH_TEST));
        render_triangles(selection);

        if (m_zbump_overlay_dirty)
            update_zbump_overlay_and_grabbers();

        if (m_zbump_config.enabled) {
            // Same x-ray treatment (depth test off, depth mask off) as All mode's overlay above --
            // this is a flat Planar-equivalent plane, the shape least prone to the patchy-overlap
            // bug that treatment was fixed for, but applying it anyway for consistency (and
            // because the object's own base alpha is already tuned assuming every mode does this).
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            glsafe(::glDisable(GL_DEPTH_TEST));
            glsafe(::glDepthMask(GL_FALSE));

            const bool has_preview_texture = m_zbump_preview_texture.get_id() != 0;
            GLShaderProgram* shader = wxGetApp().get_shader(has_preview_texture ? "flat_texture" : "flat");
            if (shader != nullptr) {
                shader->start_using();
                const Camera& camera = wxGetApp().plater()->get_camera();
                shader->set_uniform("projection_matrix", camera.get_projection_matrix());
                shader->set_uniform("view_model_matrix", camera.get_view_matrix() * m_zbump_overlay_transform);
                if (has_preview_texture) {
                    glsafe(::glBlendColor(0.0f, 0.0f, 0.0f, 0.45f));
                    glsafe(::glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA));
                    glsafe(::glBindTexture(GL_TEXTURE_2D, m_zbump_preview_texture.get_id()));
                    m_zbump_overlay_model.render();
                    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
                    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
                } else {
                    m_zbump_overlay_model.set_color(ColorRGBA(0.2f, 0.9f, 0.4f, 0.35f));
                    m_zbump_overlay_model.render();
                }
                shader->stop_using();
            }
            glsafe(::glDepthMask(GL_TRUE));
            glsafe(::glEnable(GL_DEPTH_TEST));
            glsafe(::glDisable(GL_BLEND));

            glsafe(::glDisable(GL_DEPTH_TEST));
            render_grabbers(m_zbump_cached_bbox);
            glsafe(::glEnable(GL_DEPTH_TEST));
        }
    }
}

void GLGizmoTextureBump::on_render_input_window(float x, float y, float bottom_limit)
{
    // Painter mode had no content without a selected model object; All mode always has some UI
    // (the mode bar itself). Keep the window itself always drawn, gate only the mode-specific body.
    y = std::min(y, bottom_limit - ImGui::GetWindowHeight());
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin("TextureBump", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    m_imgui->text(_u8L("Bump Mapping Editor"));

    // NEOTKO_TEXTUREBUMP_TAG -- unification pass: All/Painter mode switch, same segmented-bar
    // technique as GLGizmoColorMixPainter's department bar (tb_mode_bar(), anonymous namespace
    // above).
    // NEOTKO_ZBUMP_TAG -- 3rd mode, own domain (Top Surface Z relief, Feature/ZBump/), reusing
    // this gizmo's shell only (same mode-bar widget, same panel window) -- zero shared code with
    // Texture Bump's engine/config. See docs/WIP/ZBUMP_TOP_SURFACE_PLAN.md.
    int mode_idx = (m_mode == Mode::All) ? 0 : (m_mode == Mode::Painter) ? 1 : 2;
    static const char* labels[3] = { "All", "Painter", "Top" };
    static const char* tips[3]   = { "Edit the object's base Texture Bump settings",
                                     "Paint zones, each with its own Texture Bump settings",
                                     "Edit the object's Z Bump (top surface relief) settings" };
    tb_mode_bar(labels, tips, 3, mode_idx);
    const Mode new_mode = (mode_idx == 0) ? Mode::All : (mode_idx == 1) ? Mode::Painter : Mode::Top;
    if (new_mode != m_mode) {
        m_mode = new_mode;
        // Both dirty flags, regardless of which mode we're switching TO: entering a mode needs its
        // own overlay/grabbers rebuilt, and LEAVING a mode needs its grabbers re-disabled (each
        // update function also re-enables its own 0-3/4 and disables the other's, see
        // update_overlay_and_grabbers()/update_zbump_overlay_and_grabbers()) -- but only the
        // active mode's render branch actually calls its update function, so only that flag
        // matters in practice; setting both is just cheap insurance against future reordering.
        m_overlay_dirty = true;
        m_zbump_overlay_dirty = true;
    }
    // No separator here -- the mode bar is its own visual break, and each mode's first section
    // header brings its own spacing + rule.
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.2f));

    if (m_mode == Mode::All) {
        if (ModelObject* model_object = get_model_object()) {
            render_texture_bump_config_sections(m_imgui, m_config,
                [&](const char* field) { commit_config_field(model_object, field); },
                /*show_plane_handles_hint=*/true);
        }
    } else if (m_mode == Mode::Top) {
        if (ModelObject* model_object = get_model_object()) {
            render_zbump_config_section(m_imgui, m_zbump_config,
                [&](const char* field) { commit_zbump_config_field(model_object, field); });
        }
    } else if (m_c->selection_info() && m_c->selection_info()->model_object()) {
        ImGui::TextDisabled("%s", _u8L("Paint zones on the model, each with its own texture.").c_str());

        // get_left_button_state_type() reads m_erase_mode live; nothing else to do on toggle.
        m_imgui->checkbox(_L("Erase mode"), m_erase_mode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Painting removes the zone instead of adding it.").c_str());

        render_zone_list();

        if (m_selected_zone_id != 0) {
            if (TextureBumpZoneProfile* zone = TextureBumpZoneManager::get().find_mut(m_selected_zone_id))
                render_zone_editor(*zone);
        }

        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.3f));
        ImGui::Separator();
        const bool erase_all_clicked = m_imgui->button(_u8L("Erase all painting"));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Clear every painted zone from the object (zones themselves are kept).").c_str());
        if (erase_all_clicked) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset selection"), UndoRedo::SnapshotType::GizmoAction);
            ModelObject* mo = m_c->selection_info()->model_object();
            int idx = -1;
            for (ModelVolume* mv : mo->volumes)
                if (mv->is_model_part()) {
                    ++idx;
                    m_triangle_selectors[idx]->reset();
                    m_triangle_selectors[idx]->request_update_render_data(true);
                }
            update_model_object();
            m_parent.set_as_dirty();
        }
    }

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace GUI
} // namespace Slic3r
