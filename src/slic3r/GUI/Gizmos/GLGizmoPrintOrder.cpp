// NEOTKO_PRINTORDER_TAG_START — 2_49, see GLGizmoPrintOrder.hpp
#include "GLGizmoPrintOrder.hpp"
#include "GLGizmoMeasure.hpp"   // TransformHelper::world_to_ss
#include "GizmoNeotkoStyle.hpp" // house window look
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Model.hpp"

#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace Slic3r {
namespace GUI {

static Model& po_model() { return wxGetApp().plater()->model(); }

static ModelObject* po_object(int idx)
{
    Model& m = po_model();
    return (idx >= 0 && idx < (int) m.objects.size()) ? m.objects[idx] : nullptr;
}

GLGizmoPrintOrder::GLGizmoPrintOrder(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoPrintOrder::on_init()
{
    m_shortcut_key = 0;
    return true;
}

std::string GLGizmoPrintOrder::on_get_name() const
{
    return _u8L("Print order (Experimental, handle with care)");
}

bool GLGizmoPrintOrder::on_is_activable() const
{
    // LibreMode, the same switch the engine mirror reads (neotko_print_order_manual).
    return wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool("neotko_libre_mode");
}

void GLGizmoPrintOrder::on_set_state()
{
    if (get_state() == On) {
        m_edit_obj = -1;
        m_hover_obj = m_hover_vol = -1;
        m_panel_hover_obj = m_panel_hover_vol = -1;
        apply_highlight();
    } else if (get_state() == Off) {
        restore_highlight();
        m_edit_obj = -1;
        m_hover_obj = m_hover_vol = -1;
        m_panel_hover_obj = m_panel_hover_vol = -1;
    }
}

void GLGizmoPrintOrder::data_changed(bool /*is_serializing*/)
{
    // Volumes rebuilt (undo, delete, reload): drop a part level whose object is gone, re-light.
    if (m_edit_obj >= 0 && model_part_count(po_object(m_edit_obj)) < 2)
        m_edit_obj = -1;
    if (get_state() == On)
        apply_highlight();
}

// -----------------------------------------------------------------------------
// Objects
// -----------------------------------------------------------------------------

std::vector<int> GLGizmoPrintOrder::numbered_objects() const
{
    const Model&     m = po_model();
    std::vector<int> out;
    for (int i = 0; i < (int) m.objects.size(); ++i)
        if (m.objects[i] && m.objects[i]->neotko_print_rank > 0)
            out.push_back(i);
    // by number; same number keeps the object list order (a group prints together)
    std::stable_sort(out.begin(), out.end(),
                     [&m](int a, int b) { return m.objects[a]->neotko_print_rank < m.objects[b]->neotko_print_rank; });
    return out;
}

std::vector<int> GLGizmoPrintOrder::free_objects() const
{
    const Model&     m = po_model();
    std::vector<int> out;
    for (int i = 0; i < (int) m.objects.size(); ++i)
        if (m.objects[i] && m.objects[i]->neotko_print_rank <= 0)
            out.push_back(i);
    return out;
}

int GLGizmoPrintOrder::next_object_rank() const
{
    int mx = 0;
    for (const ModelObject* mo : po_model().objects)
        if (mo)
            mx = std::max(mx, mo->neotko_print_rank);
    return mx + 1;
}

void GLGizmoPrintOrder::set_object_rank(int obj_idx, int rank, const char* snapshot)
{
    ModelObject* mo = po_object(obj_idx);
    if (mo == nullptr || mo->neotko_print_rank == rank)
        return;
    wxGetApp().plater()->take_snapshot(snapshot);
    mo->neotko_print_rank = std::max(0, rank);
    commit();
}

void GLGizmoPrintOrder::swap_objects(int a, int b)
{
    ModelObject *ma = po_object(a), *mb = po_object(b);
    if (ma == nullptr || mb == nullptr)
        return;
    wxGetApp().plater()->take_snapshot("Print order: move");
    std::swap(ma->neotko_print_rank, mb->neotko_print_rank);
    commit();
}

void GLGizmoPrintOrder::start_over()
{
    Model& m   = po_model();
    bool   any = false;
    for (const ModelObject* mo : m.objects)
        if (mo) {
            any |= mo->neotko_print_rank > 0;
            for (const ModelVolume* mv : mo->volumes)
                any |= mv->neotko_print_rank > 0;
        }
    if (!any)
        return;
    wxGetApp().plater()->take_snapshot("Print order: start over");
    for (ModelObject* mo : m.objects)
        if (mo) {
            mo->neotko_print_rank = 0;
            for (ModelVolume* mv : mo->volumes)
                mv->neotko_print_rank = 0;
        }
    m_edit_obj = -1;
    commit();
}

// -----------------------------------------------------------------------------
// Parts of m_edit_obj
// -----------------------------------------------------------------------------

int GLGizmoPrintOrder::model_part_count(const ModelObject* mo)
{
    if (mo == nullptr)
        return 0;
    return (int) std::count_if(mo->volumes.begin(), mo->volumes.end(), [](const ModelVolume* v) { return v->is_model_part(); });
}

std::vector<int> GLGizmoPrintOrder::numbered_parts() const
{
    std::vector<int>   out;
    const ModelObject* mo = po_object(m_edit_obj);
    if (mo == nullptr)
        return out;
    for (int v = 0; v < (int) mo->volumes.size(); ++v)
        if (mo->volumes[v]->is_model_part() && mo->volumes[v]->neotko_print_rank > 0)
            out.push_back(v);
    std::stable_sort(out.begin(), out.end(),
                     [mo](int a, int b) { return mo->volumes[a]->neotko_print_rank < mo->volumes[b]->neotko_print_rank; });
    return out;
}

std::vector<int> GLGizmoPrintOrder::free_parts() const
{
    std::vector<int>   out;
    const ModelObject* mo = po_object(m_edit_obj);
    if (mo == nullptr)
        return out;
    for (int v = 0; v < (int) mo->volumes.size(); ++v)
        if (mo->volumes[v]->is_model_part() && mo->volumes[v]->neotko_print_rank <= 0)
            out.push_back(v);
    return out;
}

int GLGizmoPrintOrder::next_part_rank() const
{
    int                mx = 0;
    const ModelObject* mo = po_object(m_edit_obj);
    if (mo)
        for (const ModelVolume* mv : mo->volumes)
            mx = std::max(mx, mv->neotko_print_rank);
    return mx + 1;
}

void GLGizmoPrintOrder::set_part_rank(int vol_idx, int rank, const char* snapshot)
{
    ModelObject* mo = po_object(m_edit_obj);
    if (mo == nullptr || vol_idx < 0 || vol_idx >= (int) mo->volumes.size() || !mo->volumes[vol_idx]->is_model_part())
        return;
    if (mo->volumes[vol_idx]->neotko_print_rank == rank)
        return;
    wxGetApp().plater()->take_snapshot(snapshot);
    mo->volumes[vol_idx]->neotko_print_rank = std::max(0, rank);
    commit();
}

void GLGizmoPrintOrder::swap_parts(int a, int b)
{
    ModelObject* mo = po_object(m_edit_obj);
    if (mo == nullptr || a < 0 || b < 0 || a >= (int) mo->volumes.size() || b >= (int) mo->volumes.size())
        return;
    wxGetApp().plater()->take_snapshot("Print order: move part");
    std::swap(mo->volumes[a]->neotko_print_rank, mo->volumes[b]->neotko_print_rank);
    commit();
}

void GLGizmoPrintOrder::enter_parts(int obj_idx)
{
    if (model_part_count(po_object(obj_idx)) < 2)
        return;
    m_edit_obj = obj_idx;
    apply_highlight();
    m_parent.set_as_dirty();
}

void GLGizmoPrintOrder::leave_parts()
{
    m_edit_obj = -1;
    apply_highlight();
    m_parent.set_as_dirty();
}

// -----------------------------------------------------------------------------

void GLGizmoPrintOrder::compact()
{
    // Distinct numbers → 1..k, ties stay tied.
    auto squeeze = [](std::vector<int*> ranks) {
        std::map<int, int> remap;
        for (int* r : ranks)
            if (*r > 0)
                remap[*r] = 0;
        int k = 0;
        for (auto& kv : remap)
            kv.second = ++k;
        for (int* r : ranks)
            if (*r > 0)
                *r = remap[*r];
    };
    std::vector<int*> objs;
    for (ModelObject* mo : po_model().objects)
        if (mo) {
            objs.push_back(&mo->neotko_print_rank);
            std::vector<int*> parts;
            for (ModelVolume* mv : mo->volumes)
                parts.push_back(&mv->neotko_print_rank);
            squeeze(parts);
        }
    squeeze(objs);
}

void GLGizmoPrintOrder::commit()
{
    compact();
    wxGetApp().plater()->schedule_background_process(); // G-code only (Print.cpp steps_gcode)
    apply_highlight();
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

// -----------------------------------------------------------------------------
// Mouse: click = next number, right click / ⌥ + click = Free
// -----------------------------------------------------------------------------

bool GLGizmoPrintOrder::on_mouse(const wxMouseEvent& mouse_event)
{
    const int           hovered = m_parent.get_first_hover_volume_idx();
    const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
    const GLVolume*     gv      = (hovered >= 0 && hovered < (int) volumes.size()) ? volumes[hovered] : nullptr;
    const int           obj_idx = (gv && !gv->is_wipe_tower) ? gv->object_idx() : -1;
    const int           vol_idx = (gv && !gv->is_modifier) ? gv->volume_idx() : -1;
    ModelObject*        mo      = po_object(obj_idx);

    // Light what is under the mouse BEFORE the click.
    if (mouse_event.Moving()) {
        const int hv = m_edit_obj >= 0 ? (obj_idx == m_edit_obj ? vol_idx : -1) : -1;
        const int ho = mo ? obj_idx : -1;
        if (ho != m_hover_obj || hv != m_hover_vol) {
            m_hover_obj = ho;
            m_hover_vol = hv;
            apply_highlight();
            m_parent.set_as_dirty();
        }
        return false;
    }

    // macOS sends a quick second click twice (LeftDown + LeftDClick); only the LeftDown counts.
    if (mouse_event.LeftDClick())
        return mo != nullptr;

    // Locked: a click over an object is swallowed (nothing selected, dragged or renumbered) and a
    // click on empty space only moves the camera; in neither case do we leave the parts level.
    if (m_locked)
        return mo != nullptr && (mouse_event.LeftDown() || mouse_event.RightDown() || mouse_event.RightUp());

    // Parts level.
    if (m_edit_obj >= 0) {
        if (mouse_event.LeftDown() && !mouse_event.Dragging()) {
            if (obj_idx != m_edit_obj) {
                leave_parts(); // empty space or another object: back to objects
                return mo != nullptr;
            }
            if (mouse_event.AltDown())
                set_part_rank(vol_idx, 0, "Print order: part Free");
            else if (vol_idx >= 0 && mo->volumes[vol_idx]->neotko_print_rank <= 0)
                set_part_rank(vol_idx, next_part_rank(), "Print order: number part");
            return true;
        }
        if (mouse_event.RightDown() && obj_idx == m_edit_obj) {
            set_part_rank(vol_idx, 0, "Print order: part Free");
            return true;
        }
        if (mouse_event.RightUp() && mo)
            return true;
        return false;
    }

    // Objects level.
    if (mo == nullptr)
        return false; // empty space: the camera works as usual
    if (mouse_event.LeftDown() && !mouse_event.Dragging()) {
        if (mouse_event.AltDown())
            set_object_rank(obj_idx, 0, "Print order: Free");
        else if (mo->neotko_print_rank <= 0)
            set_object_rank(obj_idx, next_object_rank(), "Print order: number");
        else if (model_part_count(mo) > 1) {
            // another click on a numbered object of several parts: into its parts, and the touched
            // part takes the first free part number (the Align & Construct scheme).
            enter_parts(obj_idx);
            if (vol_idx >= 0 && mo->volumes[vol_idx]->neotko_print_rank <= 0)
                set_part_rank(vol_idx, next_part_rank(), "Print order: number part");
        }
        return true; // consumed: nothing gets dragged while ordering
    }
    if (mouse_event.RightDown()) {
        set_object_rank(obj_idx, 0, "Print order: Free");
        return true;
    }
    if (mouse_event.RightUp())
        return true; // no context menu over an object while ordering
    return false;
}

// -----------------------------------------------------------------------------
// Look: see-through scene, lit hover, glass around the part level
// -----------------------------------------------------------------------------

void GLGizmoPrintOrder::restore_highlight()
{
    if (m_saved_colors.empty())
        return;
    for (GLVolume* v : m_parent.get_volumes().volumes) {
        if (v == nullptr)
            continue;
        auto it = m_saved_colors.find(std::make_tuple(v->object_idx(), v->volume_idx(), v->instance_idx()));
        if (it != m_saved_colors.end()) {
            v->set_color(it->second.first);
            v->force_native_color = it->second.second;
        }
    }
    m_saved_colors.clear();
}

void GLGizmoPrintOrder::apply_highlight()
{
    restore_highlight(); // never save an already tinted colour as the original
    if (get_state() != On)
        return;

    const ColorRGBA lit(0.f, 0.80f, 0.72f, 1.f); // the accent, in the scene
    for (GLVolume* v : m_parent.get_volumes().volumes) {
        if (v == nullptr || v->is_modifier || v->is_wipe_tower)
            continue;
        const int  o = v->object_idx();
        bool       hover;
        float      alpha;
        if (m_edit_obj >= 0) {
            hover = (o == m_edit_obj && hl_obj() == m_edit_obj && v->volume_idx() == hl_vol());
            alpha = (o == m_edit_obj) ? (hover ? 1.f : 0.55f) : 0.12f;
        } else {
            hover = (o == hl_obj());
            alpha = hover ? 1.f : 0.55f;
        }
        const ColorRGBA orig = v->color;
        ColorRGBA       out  = orig;
        if (hover)
            for (int k = 0; k < 3; ++k)
                out[k] = 0.35f * orig[k] + 0.65f * lit[k];
        out[3] = std::min(orig[3], alpha);
        m_saved_colors.emplace(std::make_tuple(o, v->volume_idx(), v->instance_idx()),
                               std::make_pair(orig, (bool) v->force_native_color));
        v->set_color(out);
        // a painted volume only shows the tint / lowered alpha with its native colour forced
        if (hover || out[3] < 1.f)
            v->force_native_color = true;
    }
}

// -----------------------------------------------------------------------------
// 3D: discs and arrows
// -----------------------------------------------------------------------------

namespace {
struct Ss
{
    Matrix4d           pv;
    std::array<int, 4> vp;
    ImVec2 operator()(const Vec3d& w) const
    {
        const Vec2d q = TransformHelper::world_to_ss(w, pv, vp);
        return ImVec2((float) q.x(), (float) (vp[3] - q.y()));
    }
};

Ss make_ss()
{
    const Camera& camera = wxGetApp().plater()->get_camera();
    return Ss{ camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix(), camera.get_viewport() };
}

void draw_arrow(ImDrawList* fg, ImVec2 p0, ImVec2 p1, ImU32 col)
{
    const ImU32 ink = IM_COL32(20, 20, 20, 255);
    const float dx = p1.x - p0.x, dy = p1.y - p0.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 40.f)
        return;
    const float  ux = dx / len, uy = dy / len;
    const ImVec2 s(p0.x + ux * 20.f, p0.y + uy * 20.f), e(p1.x - ux * 20.f, p1.y - uy * 20.f);
    fg->AddLine(s, e, ink, 6.f);
    fg->AddLine(s, e, col, 3.f);
    const ImVec2 l(e.x - ux * 14.f - uy * 8.f, e.y - uy * 14.f + ux * 8.f);
    const ImVec2 r(e.x - ux * 14.f + uy * 8.f, e.y - uy * 14.f - ux * 8.f);
    fg->AddTriangleFilled(e, l, r, col);
    fg->AddTriangle(e, l, r, ink, 1.5f);
}

void draw_disc(ImDrawList* fg, ImVec2 c, const std::string& txt, bool numbered, bool lit)
{
    ImFont*     font   = ImGui::GetFont();
    const ImU32 accent = neo_col_u32_raw(NeoCol::AccentBright);
    const float radius = (numbered ? 15.f : 11.f) + 4.f * float(txt.size() > 2);
    fg->AddCircleFilled(c, radius, numbered ? accent : IM_COL32(150, 150, 150, 230), 32);
    fg->AddCircle(c, radius, lit ? IM_COL32(255, 255, 255, 255) : IM_COL32(43, 52, 62, 255), 32, lit ? 3.f : 1.8f);
    const float  fsize = numbered ? (txt.size() > 2 ? 18.f : 24.f) : 16.f;
    const ImVec2 tsz   = font->CalcTextSizeA(fsize, FLT_MAX, 0.f, txt.c_str());
    fg->AddText(font, fsize, ImVec2(c.x - tsz.x * 0.5f, c.y - tsz.y * 0.5f), IM_COL32(20, 20, 20, 255), txt.c_str());
}

// world box of one part of an object, with its first instance
BoundingBoxf3 part_box(const ModelObject* mo, int v)
{
    if (mo == nullptr || mo->instances.empty() || v < 0 || v >= (int) mo->volumes.size())
        return {};
    const ModelVolume* mv = mo->volumes[v];
    return mv->mesh().transformed_bounding_box(mo->instances[0]->get_matrix() * mv->get_matrix());
}
} // namespace

void GLGizmoPrintOrder::on_render()
{
    if (m_edit_obj >= 0)
        render_parts_layer();
    else
        render_objects_layer();
}

void GLGizmoPrintOrder::render_objects_layer()
{
    const Model& m  = po_model();
    const Ss     ss = make_ss();
    ImDrawList*  fg = ImGui::GetForegroundDrawList();
    const ImU32  accent = neo_col_u32_raw(NeoCol::AccentBright);

    // Arrows first, under the discs: from each numbered object to the next one, on the plate.
    const std::vector<int> seq = numbered_objects();
    for (size_t i = 0; i + 1 < seq.size(); ++i) {
        const ModelObject *a = m.objects[seq[i]], *b = m.objects[seq[i + 1]];
        if (a->instances.empty() || b->instances.empty())
            continue;
        Vec3d pa = a->instance_bounding_box(0).center(), pb = b->instance_bounding_box(0).center();
        pa.z() = pb.z() = 0.;
        draw_arrow(fg, ss(pa), ss(pb), accent);
    }
    // A disc per copy, over its top: the number, or «F» when Free.
    for (int oi = 0; oi < (int) m.objects.size(); ++oi) {
        const ModelObject* mo = m.objects[oi];
        if (mo == nullptr)
            continue;
        const int r = mo->neotko_print_rank;
        for (size_t i = 0; i < mo->instances.size(); ++i) {
            const BoundingBoxf3 box = mo->instance_bounding_box(i);
            if (!box.defined)
                continue;
            Vec3d top = box.center();
            top.z()   = box.max.z() + 2.0;
            draw_disc(fg, ss(top), r > 0 ? std::to_string(r) : "F", r > 0, oi == hl_obj());
        }
    }
}

void GLGizmoPrintOrder::render_parts_layer()
{
    const ModelObject* mo = po_object(m_edit_obj);
    if (mo == nullptr)
        return;
    const Ss     ss = make_ss();
    ImDrawList*  fg = ImGui::GetForegroundDrawList();
    const ImU32  accent = neo_col_u32_raw(NeoCol::AccentBright);
    const std::string base = mo->neotko_print_rank > 0 ? std::to_string(mo->neotko_print_rank) + "." : "";

    const std::vector<int> seq = numbered_parts();
    for (size_t i = 0; i + 1 < seq.size(); ++i) {
        const BoundingBoxf3 a = part_box(mo, seq[i]), b = part_box(mo, seq[i + 1]);
        if (a.defined && b.defined)
            draw_arrow(fg, ss(Vec3d(a.center().x(), a.center().y(), a.max.z())),
                       ss(Vec3d(b.center().x(), b.center().y(), b.max.z())), accent);
    }
    // The disc sits on the part's own top, so small parts next to each other stay readable.
    for (int v = 0; v < (int) mo->volumes.size(); ++v) {
        if (!mo->volumes[v]->is_model_part())
            continue;
        const BoundingBoxf3 box = part_box(mo, v);
        if (!box.defined)
            continue;
        const int r = mo->volumes[v]->neotko_print_rank;
        draw_disc(fg, ss(Vec3d(box.center().x(), box.center().y(), box.max.z() + 1.0)),
                  r > 0 ? base + std::to_string(r) : base + "F", r > 0, hl_obj() == m_edit_obj && v == hl_vol());
    }
}

// -----------------------------------------------------------------------------
// Panel
// -----------------------------------------------------------------------------

void GLGizmoPrintOrder::on_render_input_window(float x, float y, float bottom_limit)
{
    const float u     = neo_u();
    const float win_w = 22.f * u;
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(win_w, 0.f), ImVec2(win_w, FLT_MAX));
    neo_push_window_style(); // before Begin
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);
    ImGui::PushTextWrapPos(win_w - 16.f);
    neo_push_panel_style();

    const Model& m        = po_model();
    ImDrawList*  dl       = ImGui::GetWindowDrawList();
    const float  btn      = 1.25f * u;         // glyph buttons
    const float  row_h    = 1.75f * u;
    bool         changed  = false;             // a button changed the model: stop drawing rows
    int          new_hobj = -1, new_hvol = -1; // panel row under the mouse this frame

    // ---- header: where we are, the lock, start over -------------------------------------------
    const ModelObject* emo = po_object(m_edit_obj);
    if (emo != nullptr)
        neo_status_chip(fit_text(_u8L("Parts of") + " " + emo->name, 12.f * u).c_str(), neo_col_u32(NeoCol::AccentBright), true);
    else
        neo_status_chip(_u8L("Objects").c_str(), neo_col_u32(NeoCol::AccentBright), false);
    ImGui::SameLine(win_w - 2.f * btn - 1.6f * u);
    if (neo_glyph_toggle("##po_lock", btn, m_locked, m_locked ? Glyph::Lock : Glyph::Unlock,
                         m_locked ? _u8L("Locked: clicks in the scene do nothing,\n"
                                         "so a stray click can't renumber or leave the parts.\n"
                                         "The list still works. Click to unlock.").c_str()
                                  : _u8L("Lock the scene clicks against miss-clicks.\n"
                                         "The list keeps working.").c_str()))
        m_locked = !m_locked;
    ImGui::SameLine();
    if (neo_glyph_button("##po_reset", btn, Glyph::EraseAll, true,
                         _u8L("Start over: every object and every part back to Free.").c_str()))
        { start_over(); changed = true; }

    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::AccentBright));
    ImGui::TextUnformatted(_u8L("Experimental. Handle with care.").c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    if (emo != nullptr)
        ImGui::TextWrapped("%s", _u8L("Click a part: next number. Right click / Option + click: Free. "
                                      "Parts that touch in a layer print together.").c_str());
    else
        ImGui::TextWrapped("%s", _u8L("Click an object: next number. Right click / Option + click: Free. "
                                      "Click a numbered assembly again to order its parts.").c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Every layer prints the numbered objects in this order, skipping the ones\n"
                                     "already finished. Free ones go after them, as Orca does.\n"
                                     "With several filaments, each filament is printed whole in this order.").c_str());

    // ---- one row: disc, name, buttons on the right; lights its piece in 3D while hovered ----------
    // returns true while hovered. `buttons` draws right-aligned glyph buttons and may set changed.
    auto row = [&](const std::string& disc, bool numbered, const std::string& name, int hobj, int hvol, auto&& buttons) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float  w  = ImGui::GetContentRegionAvail().x;
        const ImVec2 p1(p0.x + w, p0.y + row_h);
        const bool   hv = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(p0, p1);
        const bool   lit = hv || (hobj == hl_obj() && hvol == hl_vol() && hl_obj() >= 0);
        dl->AddRectFilled(p0, p1, lit ? neo_fade(NeoCol::Accent, 0.22f) : neo_fade(NeoCol::SurfaceHi, 0.30f), 6.f);
        if (lit)
            dl->AddRect(p0, p1, neo_fade(NeoCol::AccentBright, 0.8f), 6.f, 0, 1.2f);
        // the disc, same look as in the scene
        const float  rr = 0.62f * u;
        const ImVec2 c(p0.x + 0.85f * u, p0.y + row_h * 0.5f);
        dl->AddCircleFilled(c, rr + (disc.size() > 2 ? 0.18f * u : 0.f), numbered ? neo_col_u32(NeoCol::AccentBright) : IM_COL32(150, 150, 150, 230), 24);
        const ImVec2 ts = ImGui::CalcTextSize(disc.c_str());
        dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(20, 20, 20, 255), disc.c_str());
        // the name
        const float  name_w = w - 2.1f * u - 3.f * (btn + 0.25f * u);
        ImGui::SetCursorScreenPos(ImVec2(p0.x + 1.9f * u, p0.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f));
        ImGui::TextUnformatted(fit_text(name, name_w).c_str());
        // buttons, from the right edge
        ImGui::SetCursorScreenPos(ImVec2(p1.x - 3.f * (btn + 0.25f * u), p0.y + (row_h - btn) * 0.5f));
        buttons();
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 0.22f * u));
        ImGui::Dummy(ImVec2(0.f, 0.f));
        if (hv) { new_hobj = hobj; new_hvol = hvol; }
    };
    // a fixed slot: either the button or an empty space of the same size, so columns line up
    auto slot = [&](bool show, const char* id, Glyph g, bool danger, const char* tip, auto&& on_click) {
        if (show && !changed) {
            if (neo_glyph_button(id, btn, g, danger, tip)) { on_click(); changed = true; }
        } else
            ImGui::Dummy(ImVec2(btn, btn));
        ImGui::SameLine(0.f, 0.25f * u);
    };
    // up / down: real arrows, same slot width
    auto arrow = [&](bool show, const char* id, ImGuiDir dir, const char* tip, auto&& on_click) {
        if (show && !changed) {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2((btn - ImGui::GetFontSize()) * 0.5f, (btn - ImGui::GetFontSize()) * 0.5f));
            const bool pressed = ImGui::ArrowButton(id, dir);
            ImGui::PopStyleVar();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", tip);
            if (pressed) { on_click(); changed = true; }
        } else
            ImGui::Dummy(ImVec2(btn, btn));
        ImGui::SameLine(0.f, 0.25f * u);
    };

    if (!changed && emo != nullptr) {
        // ---- parts level ----
        const std::string base = emo->neotko_print_rank > 0 ? std::to_string(emo->neotko_print_rank) + "." : "";
        if (ImGui::Button(_u8L("< Back to objects").c_str()))
            { leave_parts(); changed = true; }
        const std::vector<int> seq = numbered_parts();
        neo_section(_u8L("Order inside the object").c_str());
        if (seq.empty())
            ImGui::TextDisabled("%s", _u8L("Nothing numbered yet: Orca decides.").c_str());
        for (size_t i = 0; i < seq.size() && !changed; ++i) {
            const int v = seq[i];
            ImGui::PushID(v);
            row(base + std::to_string(emo->volumes[v]->neotko_print_rank), true, emo->volumes[v]->name, m_edit_obj, v, [&] {
                arrow(i > 0, "##up", ImGuiDir_Up, _u8L("Earlier").c_str(), [&] { swap_parts(v, seq[i - 1]); });
                arrow(i + 1 < seq.size(), "##dn", ImGuiDir_Down, _u8L("Later").c_str(), [&] { swap_parts(v, seq[i + 1]); });
                slot(true, "##free", Glyph::Erase, true, _u8L("Back to Free").c_str(), [&] { set_part_rank(v, 0, "Print order: part Free"); });
            });
            ImGui::PopID();
        }
        if (!changed) {
            const std::vector<int> fr = free_parts();
            if (!fr.empty())
                neo_section(_u8L("Free (as Orca does)").c_str());
            for (size_t i = 0; i < fr.size() && !changed; ++i) {
                const int v = fr[i];
                ImGui::PushID(100000 + v);
                row(base + "F", false, emo->volumes[v]->name, m_edit_obj, v, [&] {
                    slot(false, "##a", Glyph::Plus, false, "", [] {});
                    slot(false, "##b", Glyph::Plus, false, "", [] {});
                    slot(true, "##num", Glyph::Plus, false, _u8L("Give it the next number").c_str(),
                         [&] { set_part_rank(v, next_part_rank(), "Print order: number part"); });
                });
                ImGui::PopID();
            }
        }
    } else if (!changed) {
        // ---- objects level ----
        const std::vector<int> seq = numbered_objects();
        neo_section(_u8L("Order").c_str());
        if (seq.empty())
            ImGui::TextDisabled("%s", _u8L("Nothing numbered yet: Orca decides.").c_str());
        for (size_t i = 0; i < seq.size() && !changed; ++i) {
            const int          oi = seq[i];
            const ModelObject* mo = m.objects[oi];
            const bool         assembly = model_part_count(mo) > 1;
            ImGui::PushID(oi);
            row(std::to_string(mo->neotko_print_rank), true, mo->name, oi, -1, [&] {
                arrow(i > 0, "##up", ImGuiDir_Up, _u8L("Earlier").c_str(), [&] { swap_objects(oi, seq[i - 1]); });
                arrow(i + 1 < seq.size(), "##dn", ImGuiDir_Down, _u8L("Later").c_str(), [&] { swap_objects(oi, seq[i + 1]); });
                if (assembly)
                    slot(true, "##parts", Glyph::Edit, false, _u8L("Order its parts").c_str(), [&] { enter_parts(oi); });
                else
                    slot(true, "##free", Glyph::Erase, true, _u8L("Back to Free").c_str(), [&] { set_object_rank(oi, 0, "Print order: Free"); });
            });
            ImGui::PopID();
        }
        if (!changed) {
            const std::vector<int> fr = free_objects();
            if (!fr.empty())
                neo_section(_u8L("Free (as Orca does)").c_str());
            for (size_t i = 0; i < fr.size() && !changed; ++i) {
                const int oi = fr[i];
                ImGui::PushID(100000 + oi);
                row("F", false, m.objects[oi]->name, oi, -1, [&] {
                    slot(false, "##a", Glyph::Plus, false, "", [] {});
                    slot(false, "##b", Glyph::Plus, false, "", [] {});
                    slot(true, "##num", Glyph::Plus, false, _u8L("Give it the next number").c_str(),
                         [&] { set_object_rank(oi, next_object_rank(), "Print order: number"); });
                });
                ImGui::PopID();
            }
        }
    }

    neo_pop_panel_style();
    ImGui::PopTextWrapPos();
    GizmoImguiEnd();
    neo_pop_window_style();

    // the row under the mouse lights its piece in the scene
    if (new_hobj != m_panel_hover_obj || new_hvol != m_panel_hover_vol) {
        m_panel_hover_obj = new_hobj;
        m_panel_hover_vol = new_hvol;
        apply_highlight();
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }
}

} // namespace GUI
} // namespace Slic3r
// NEOTKO_PRINTORDER_TAG_END
