// NEOTKO_ALIGNSTACK_TAG_START
#include "GLGizmoAlignStack.hpp"
#include "GLGizmoMeasure.hpp" // for TransformHelper::world_to_ss
#include "GizmoNeotkoStyle.hpp" // s290 — solo el estilo de VENTANA, no la rampa de widgets

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/NeoDebug.hpp"
#include "libslic3r/NeotkoSurfaceExtrude.hpp" // NEOTKO_CONSTRUCT_TAG s363
#include "libslic3r/MeshBoolean.hpp"          // NEOTKO_CONSTRUCT_TAG s363b
#include "libslic3r/PaintTransfer.hpp"        // NEOTKO_PAINTCARRY_TAG s366
#include <wx/busyinfo.h> // NEOTKO_PAINTCARRY_TAG
#include "slic3r/GUI/GUI_ObjectList.hpp"

#include <imgui/imgui.h>

#include <GL/glew.h>
#include <wx/glcanvas.h> // NEOTKO_PAINTCARRY_TAG — wxBusyInfo needs the canvas as a wxWindow (after glew)

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <set>
#include <sstream>

namespace Slic3r { namespace GUI {

// =============================================================================
// Visual language: per-letter colors for badges/chips, and procedurally drawn
// isometric mini-cube icon buttons (no SVG assets, no toolbar tint pipeline).
// =============================================================================

namespace {

// This gizmo only ever relates one object against another (#1 anchor, #2 the
// one that moves) — an ordered chain of 3+ was never actually used, just
// confusing. Picking a third object swaps out #2 instead of extending it.
constexpr size_t kMaxOrdered = 2;

// NEOTKO_CONSTRUCT_TAG s363 — the hovered face lights up loosely (a patch on curved surfaces),
// the picked one strictly: only what is really coplanar, which is what Construct extrudes.
constexpr float kHoverFaceCos  = 0.94f;   // ~20 degrees
constexpr float kPickedFaceCos = 0.9994f; // ~2 degrees

const ImU32 kOrderColors[] = {
    IM_COL32(255, 140,   0, 235), // A orange
    IM_COL32(240, 200,   0, 235), // B yellow
    IM_COL32( 70, 180,  80, 235), // C green
    IM_COL32( 40, 180, 200, 235), // D cyan
    IM_COL32(180,  90, 230, 235), // E violet
    IM_COL32(230,  80, 120, 235), // F pink
};
constexpr size_t kOrderColorCount = sizeof(kOrderColors) / sizeof(kOrderColors[0]);

inline ImU32 order_color(size_t i) { return kOrderColors[i % kOrderColorCount]; }

// Same palette as ColorRGBA for tinting scene volumes (kept in sync above).
inline ColorRGBA order_color_rgba(size_t i)
{
    const ImU32 c = order_color(i);
    return ColorRGBA((float)((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                     (float)((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                     (float)((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f,
                     1.0f);
}

// s363d — the panel's own pieces, in the shared Neotko panel language (GizmoNeotkoStyle.hpp):
// measured in font heights (neo_u), colours off the ramp, teal = yours / selected. The isometric
// mini-cubes are gone: they drew a view nobody was looking at. The pad is the bed seen from above
// (up = +Y), fixed to the bed axes like Orca's own Move, and the ghost in the scene says where #2
// will land.
const ImU32 kAxisX = IM_COL32(232, 96, 96, 255);
const ImU32 kAxisY = IM_COL32(104, 200, 104, 255);

// The frame every hand-drawn button here shares: surface, teal wash on hover, teal when selected.
void btn_frame(ImDrawList* dl, const ImVec2& a, const ImVec2& b, bool hov, bool selected)
{
    if (selected) {
        dl->AddRectFilled(a, b, neo_fade(NeoCol::Accent, hov ? 1.0f : 0.85f), 5.f);
        dl->AddRect(a, b, neo_col_u32(NeoCol::AccentBright), 5.f, 0, 1.2f);
    } else
        dl->AddRectFilled(a, b, neo_col_u32(hov ? NeoCol::AccentDim : NeoCol::Surface), 5.f);
}

std::string format_mm(double v) { char b[32]; std::snprintf(b, sizeof b, "%.3f mm", v); return b; }

// A chip for #1 / #2: the object's own colour, its number and its name. Click = let it go.
bool order_chip(const char* id, const std::string& text, ImU32 col)
{
    const float  u  = neo_u();
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const ImVec2 sz(ts.x + 1.0f * u, 1.55f * u);
    const bool pressed = ImGui::InvisibleButton(id, sz);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, col, sz.y * 0.5f);
    if (ImGui::IsItemHovered()) dl->AddRect(a, b, neo_col_u32(NeoCol::Ink), sz.y * 0.5f, 0, 1.5f);
    dl->AddText(ImVec2(a.x + 0.5f * u, a.y + (sz.y - ts.y) * 0.5f), ink_on(col), text.c_str());
    return pressed;
}

// One pad button: an arrow in its axis colour (dx, dy in pad space, y down), or the centre mark.
bool pad_button(const char* id, float size, int dx, int dy, ImU32 col, bool& hovered)
{
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    btn_frame(dl, a, b, hovered, false);
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    if (dx == 0 && dy == 0) {                      // centre: a small square inside a bigger one
        const float r = size * 0.24f, q = size * 0.10f;
        dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), neo_col_u32(NeoCol::Ink), 2.f, 0, 1.6f);
        dl->AddRectFilled(ImVec2(c.x - q, c.y - q), ImVec2(c.x + q, c.y + q), neo_col_u32(NeoCol::AccentBright), 1.f);
    } else {
        const float k = size * 0.20f, fx = (float)dx, fy = (float)dy;
        const ImVec2 tip(c.x + fx * k * 1.25f, c.y + fy * k * 1.25f);
        const ImVec2 l(c.x - fx * k * 0.55f - fy * k, c.y - fy * k * 0.55f + fx * k);
        const ImVec2 r(c.x - fx * k * 0.55f + fy * k, c.y - fy * k * 0.55f - fx * k);
        dl->AddTriangleFilled(tip, l, r, col);
    }
    return pressed;
}

// The little compass in the pad's corner: which way X and Y run, in the bed's own colours.
void pad_compass(float size)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float m = size * 0.24f, len = size * 0.44f, fs = ImGui::GetFontSize() * 0.78f;
    const ImVec2 o(p.x + m, p.y + size - m);
    dl->AddLine(o, ImVec2(o.x + len, o.y), kAxisX, 2.f);
    dl->AddLine(o, ImVec2(o.x, o.y - len), kAxisY, 2.f);
    dl->AddText(ImGui::GetFont(), fs, ImVec2(o.x + len + 2.f, o.y - fs * 0.55f), kAxisX, "X");
    dl->AddText(ImGui::GetFont(), fs, ImVec2(o.x - fs * 0.30f, o.y - len - fs), kAxisY, "Y");
}

// One of the two "how" cards: a little drawing of #1 (big) and #2 (small) side by side, or with
// one edge lined up, plus its name. Teal when it is the one in use.
bool mode_card(const char* id, const std::string& label, bool edge, bool selected, float w)
{
    const float u = neo_u(), h = 2.5f * u;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    btn_frame(dl, a, b, ImGui::IsItemHovered(), selected);
    const float x0 = a.x + 0.55f * u, y0 = a.y + 0.55f * u, bw = 1.7f * u, bh = h - 1.1f * u, sw = 0.75f * u, sh = bh * 0.62f;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + bw, y0 + bh), order_color(0), 2.f);
    const float sx = edge ? x0 + bw - sw : x0 + bw;
    dl->AddRectFilled(ImVec2(sx, y0 + bh - sh), ImVec2(sx + sw, y0 + bh), order_color(1), 2.f);
    dl->AddRect(ImVec2(sx, y0 + bh - sh), ImVec2(sx + sw, y0 + bh), IM_COL32(30, 30, 30, 255), 2.f, 0, 1.2f);
    if (edge) dl->AddLine(ImVec2(x0 + bw, a.y + 0.25f * u), ImVec2(x0 + bw, b.y - 0.25f * u), neo_col_u32(NeoCol::Ink), 1.5f);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(x0 + bw + sw + 0.6f * u, a.y + (h - ts.y) * 0.5f), neo_col_u32(NeoCol::Ink), label.c_str());
    return pressed;
}

// A height button: a side view of #1 with #2 where it would end up, and the name beside it.
// kind: 0 = on top, 1 = centred in height, 2 = on the bed.
bool height_button(const char* id, const std::string& label, int kind, float w, float h, bool& hovered)
{
    const float u = neo_u();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    hovered = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    btn_frame(dl, a, b, hovered, false);
    const float x0 = a.x + 0.5f * u, yb = b.y - 0.45f * u, bw = 1.0f * u, bh = h - 1.25f * u, sw = 0.7f * u, sh = 0.42f * u;
    dl->AddLine(ImVec2(x0 - 0.15f * u, yb), ImVec2(x0 + bw + sw + 0.35f * u, yb), neo_col_u32(NeoCol::TextDim), 1.f);   // the bed
    dl->AddRectFilled(ImVec2(x0, yb - bh), ImVec2(x0 + bw, yb), order_color(0), 2.f);
    float sx = x0 + bw + 0.2f * u, sy = yb - sh;                                                                           // 2: on the bed, beside
    if (kind == 0) { sx = x0 + (bw - sw) * 0.5f; sy = yb - bh - sh - 1.f; }
    if (kind == 1) { sy = yb - bh * 0.5f - sh * 0.5f; }
    dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + sw, sy + sh), order_color(1), 1.5f);
    dl->AddRect(ImVec2(sx, sy), ImVec2(sx + sw, sy + sh), IM_COL32(30, 30, 30, 255), 1.5f, 0, 1.f);
    const std::string t = fit_text(label, w - (bw + sw + 1.5f * u));
    const ImVec2 ts = ImGui::CalcTextSize(t.c_str());
    dl->AddText(ImVec2(x0 + bw + sw + 0.75f * u, a.y + (h - ts.y) * 0.5f), neo_col_u32(NeoCol::Ink), t.c_str());
    return pressed;
}

// A segment of a two-way choice (Part / Object): teal when chosen.
bool seg_button(const char* id, const std::string& label, bool selected, float w)
{
    const float u = neo_u(), h = 1.75f * u;
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    btn_frame(dl, a, b, ImGui::IsItemHovered(), selected);
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(a.x + (w - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f), neo_col_u32(NeoCol::Ink), label.c_str());
    return pressed;
}

} // anonymous namespace

// =============================================================================
// Gizmo
// =============================================================================

GLGizmoAlignStack::GLGizmoAlignStack(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoAlignStack::on_init()
{
    m_shortcut_key = 0; // no global shortcut by default
    return true;
}

std::string GLGizmoAlignStack::on_get_name() const
{
    return _u8L("Align & Construct");
}

bool GLGizmoAlignStack::on_is_activable() const
{
    // s367 — offered to everyone, LibreMode or not: Construct and the booleans are useful in any
    // way of working, and picking parts lets it work inside an assembled object. Usable even with
    // an empty scene/selection: the user builds the order by clicking objects.
    return true;
}

void GLGizmoAlignStack::on_set_state()
{
    if (get_state() == On) {
        seed_order_from_selection();
        apply_highlight();
    } else if (get_state() == Off) {
        restore_highlight();
        m_ordered.clear();
        clear_face_pick();
        m_ghost_mesh.reset(); m_ghost_mesh_pick = Pick();
        for (size_t i = 0; i < 2; ++i) { m_pick_overlay[i].reset(); m_pick_overlay_pick[i] = Pick(); }
        m_hover_op = Op::None; m_ghost_visible = false;
    }
}

void GLGizmoAlignStack::data_changed(bool /*is_serializing*/)
{
    // Fires on selection changes and on volume rebuilds. apply_highlight()
    // restores via stable ids first (no dangling pointers), then re-tints —
    // so tints never accumulate and removed objects regain their color.
    prune_dead_objects();
    m_ghost_mesh_pick = Pick();  // s363d: #2's mesh may have changed; the ghost rebuilds on next hover
    if (get_state() == On)
        apply_highlight();
}

// -----------------------------------------------------------------------------
// Order tracking — owned by the gizmo, selection is mirrored so that
// Selection::translate() keeps acting on every ordered object.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::seed_order_from_selection()
{
    m_ordered.clear();
    for (const auto& kv : m_parent.get_selection().get_content()) {
        if (m_ordered.size() >= kMaxOrdered)
            break;
        Pick p;
        p.obj = kv.first;
        m_ordered.push_back(p);
    }
}

void GLGizmoAlignStack::prune_dead_objects()
{
    const Model* model = m_parent.get_selection().get_model();
    const int n_objects = model ? (int)model->objects.size() : 0;
    std::vector<Pick> alive;
    for (Pick p : m_ordered) {
        if (p.obj < 0 || p.obj >= n_objects)
            continue;
        if (p.is_part()) {
            // s367 — follow the part by its id: deleting another part shifts the indices.
            const ModelVolumePtrs& vols = model->objects[p.obj]->volumes;
            if (p.vol >= (int)vols.size() || vols[p.vol]->id() != p.vol_id) {
                p.vol = -1;
                for (size_t i = 0; i < vols.size(); ++i)
                    if (vols[i]->id() == p.vol_id) { p.vol = (int)i; break; }
                if (p.vol < 0)
                    continue; // the part is gone
            }
        }
        alive.push_back(p);
    }
    m_ordered.swap(alive);
}

void GLGizmoAlignStack::sync_selection_of(int object_idx)
{
    Selection& sel = m_parent.get_selection();
    const bool used = std::any_of(m_ordered.begin(), m_ordered.end(), [&](const Pick& p) { return p.obj == object_idx; });
    if (used)
        sel.add_object((unsigned int)object_idx, false);
    else
        sel.remove_object((unsigned int)object_idx);
}

void GLGizmoAlignStack::remove_pick(size_t i)
{
    if (i >= m_ordered.size())
        return;
    const int obj = m_ordered[i].obj;
    m_ordered.erase(m_ordered.begin() + i);
    sync_selection_of(obj);
    // The anchor may have changed → a previously picked face on it is stale.
    if (m_face_pick_mode || m_has_picked_face)
        clear_face_pick();
    m_ghost_mesh_pick = Pick();
    refresh_highlight();
}

void GLGizmoAlignStack::clear_order()
{
    while (!m_ordered.empty())
        remove_pick(m_ordered.size() - 1);
}

// s367 — the first click on an object takes it whole; another click on it takes just the part
// that was touched (only when it has several parts); a click on a picked part lets it go. A click
// on another part of an object whose part is already picked picks that part too (#1 and #2 may be
// two parts of the same object).
void GLGizmoAlignStack::click_object(int object_idx, int vol_idx)
{
    const Model* model = m_parent.get_selection().get_model();
    if (!model || object_idx < 0 || object_idx >= (int)model->objects.size())
        return;
    const ModelObjectPtrs& objs = model->objects;
    const bool can_be_part = vol_idx >= 0 && vol_idx < (int)objs[object_idx]->volumes.size() &&
                             objs[object_idx]->volumes[vol_idx]->is_model_part() &&
                             model_part_count(object_idx) > 1;

    for (size_t i = 0; i < m_ordered.size(); ++i) {
        Pick& p = m_ordered[i];
        if (p.obj != object_idx)
            continue;
        if (!p.is_part()) {
            if (!can_be_part) { remove_pick(i); return; }
            p.vol    = vol_idx;                       // whole object → just this part, same place
            p.vol_id = objs[object_idx]->volumes[vol_idx]->id();
            if (i == 0 && (m_face_pick_mode || m_has_picked_face))
                clear_face_pick();
            m_ghost_mesh_pick = Pick();
            refresh_highlight();
            return;
        }
        if (p.vol == vol_idx || !can_be_part) { remove_pick(i); return; }
    }

    Pick np;
    np.obj = object_idx;
    // A new object is taken whole; a part of an object that already has a part picked is a part.
    const bool has_part_here = std::any_of(m_ordered.begin(), m_ordered.end(),
                                           [&](const Pick& p) { return p.obj == object_idx && p.is_part(); });
    if (has_part_here && can_be_part) {
        np.vol    = vol_idx;
        np.vol_id = objs[object_idx]->volumes[vol_idx]->id();
    }
    // Only #1 (anchor) and #2 (the one that moves) ever matter here.
    // Clicking a third one swaps out #2 rather than growing a chain.
    if (m_ordered.size() >= kMaxOrdered) {
        const int old_obj = m_ordered.back().obj;
        m_ordered.pop_back();
        sync_selection_of(old_obj);
    }
    m_ordered.push_back(np);
    sync_selection_of(object_idx);
    if (m_ordered.size() == 1 && (m_face_pick_mode || m_has_picked_face))
        clear_face_pick();
    m_ghost_mesh_pick = Pick();
    refresh_highlight();
}

// -----------------------------------------------------------------------------
// s367 — what a pick is made of
// -----------------------------------------------------------------------------

const ModelObject* GLGizmoAlignStack::pick_object(const Pick& p) const
{
    const Model* model = m_parent.get_selection().get_model();
    if (!model || p.obj < 0 || p.obj >= (int)model->objects.size())
        return nullptr;
    const ModelObject* mo = model->objects[p.obj];
    if (mo->instances.empty() || (p.is_part() && p.vol >= (int)mo->volumes.size()))
        return nullptr;
    return mo;
}

const ModelVolume* GLGizmoAlignStack::pick_volume(const Pick& p) const
{
    const ModelObject* mo = pick_object(p);
    return (mo && p.is_part()) ? mo->volumes[p.vol] : nullptr;
}

Transform3d GLGizmoAlignStack::pick_trafo(const Pick& p) const
{
    const ModelObject* mo = pick_object(p);
    if (!mo)
        return Transform3d::Identity();
    const Transform3d inst = mo->instances.front()->get_matrix();
    return p.is_part() ? Transform3d(inst * mo->volumes[p.vol]->get_matrix()) : inst;
}

TriangleMesh GLGizmoAlignStack::pick_mesh(const Pick& p) const
{
    const ModelObject* mo = pick_object(p);
    if (!mo)
        return TriangleMesh();
    return p.is_part() ? mo->volumes[p.vol]->mesh() : mo->raw_mesh();
}

std::string GLGizmoAlignStack::pick_name(const Pick& p) const
{
    const ModelObject* mo = pick_object(p);
    if (!mo)
        return "?";
    return p.is_part() ? mo->volumes[p.vol]->name : mo->name;
}

int GLGizmoAlignStack::model_part_count(int object_idx) const
{
    const Model* model = m_parent.get_selection().get_model();
    if (!model || object_idx < 0 || object_idx >= (int)model->objects.size())
        return 0;
    int n = 0;
    for (const ModelVolume* v : model->objects[object_idx]->volumes)
        if (v->is_model_part()) ++n;
    return n;
}

// -----------------------------------------------------------------------------
// Scene highlight — tint each ordered object's volumes with its order color
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::restore_highlight()
{
    if (m_saved_colors.empty())
        return;
    // Restore by matching live volumes against saved ids; stale ids (volumes
    // that no longer exist after a rebuild) simply don't match and are dropped.
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

void GLGizmoAlignStack::apply_highlight()
{
    // Always restore first so we never save an already-tinted color as the
    // "original" (which would make tints accumulate/darken on each refresh).
    restore_highlight();

    // While picking a face, ghost the whole scene so the highlighted face pops.
    // (render_color picks up the lowered alpha → goes to the transparent pass.)
    const bool  ghost   = m_face_pick_mode;
    const float ghost_a = 0.30f; // dial: lower = fainter scene
    // s367 — an object with one of its parts picked: the rest of it turns see-through, so the
    // part stands out inside the assembly.
    const float glass_a = 0.25f;
    auto part_picked_here = [&](int obj) {
        return std::any_of(m_ordered.begin(), m_ordered.end(), [&](const Pick& p) { return p.obj == obj && p.is_part(); });
    };

    const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
    for (GLVolume* v : volumes) {
        if (v == nullptr || v->is_modifier || v->is_wipe_tower)
            continue;

        // Order index of this volume, if it is part of the order (its whole object, or s367 this
        // very part).
        int order = -1;
        for (size_t i = 0; i < m_ordered.size(); ++i)
            if (m_ordered[i].obj == v->object_idx() &&
                (!m_ordered[i].is_part() || m_ordered[i].vol == v->volume_idx())) { order = (int)i; break; }

        // #1 stays untinted while picking a face on it so the overlay reads
        // cleanly; every other ordered object keeps its order color.
        const bool do_tint = (order >= 0) && !(m_face_pick_mode && order == 0);
        const bool glass   = order < 0 && part_picked_here(v->object_idx());
        if (!do_tint && !ghost && !glass)
            continue; // nothing to do for this volume

        const ColorRGBA orig = v->color;
        ColorRGBA out = orig;
        if (do_tint) {
            const ColorRGBA letter = order_color_rgba((size_t)order);
            // Blend 72% toward the letter color so geometry shading still reads.
            for (int k = 0; k < 3; ++k)
                out[k] = 0.28f * orig[k] + 0.72f * letter[k];
        }
        out[3] = glass ? glass_a : (ghost ? ghost_a : orig[3]);

        m_saved_colors.emplace(std::make_tuple(v->object_idx(), v->volume_idx(), v->instance_idx()),
                               std::make_pair(orig, (bool)v->force_native_color));
        v->set_color(out);
        // s367 — a painted volume only takes the lowered alpha with its native colour forced.
        if (out[3] < 1.f)
            v->force_native_color = true;
    }
}

void GLGizmoAlignStack::refresh_highlight()
{
    apply_highlight();
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

// -----------------------------------------------------------------------------
// World bounding box helper
// -----------------------------------------------------------------------------

BoundingBoxf3 GLGizmoAlignStack::world_bbox_of(const Pick& p) const
{
    BoundingBoxf3 out;
    const ModelObject* mo = pick_object(p);
    if (!mo)
        return out;
    if (!p.is_part()) {
        for (size_t i = 0; i < mo->instances.size(); ++i)
            out.merge(mo->instance_bounding_box(i));
        return out;
    }
    // s367 — a part: its own mesh, placed with every instance of its object.
    const ModelVolume* mv = mo->volumes[p.vol];
    for (const ModelInstance* inst : mo->instances)
        out.merge(mv->mesh().transformed_bounding_box(inst->get_matrix() * mv->get_matrix()));
    return out;
}

// -----------------------------------------------------------------------------
// Render — colored letter badges, no 3D handles
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::on_render()
{
    prune_dead_objects();
    render_pick_overlays();   // s367
    render_face_highlights();
    render_construct_ghost(); // NEOTKO_CONSTRUCT_TAG s363
    render_place_ghost();     // s363d
    render_badges();
}

void GLGizmoAlignStack::render_pick_overlays()
{
    GLShaderProgram* shader = nullptr;
    for (size_t i = 0; i < 2; ++i) {
        const bool show = i < m_ordered.size() && !(i == 0 && m_face_pick_mode);
        const ModelObject* mo = show ? pick_object(m_ordered[i]) : nullptr;
        if (mo == nullptr) {
            if (m_pick_overlay[i].is_initialized())
                m_pick_overlay[i].reset();
            m_pick_overlay_pick[i] = Pick();
            continue;
        }
        const Pick& p = m_ordered[i];
        if (!m_pick_overlay[i].is_initialized() || !m_pick_overlay_pick[i].same(p) ||
            m_pick_overlay_volumes[i] != mo->volumes.size()) {
            m_pick_overlay[i].reset();
            const TriangleMesh mesh = pick_mesh(p);
            m_pick_overlay_pick[i]    = p;
            m_pick_overlay_volumes[i] = mo->volumes.size();
            if (mesh.empty())
                continue;
            m_pick_overlay[i].init_from(mesh.its);
            ColorRGBA c = order_color_rgba(i);
            c.a(0.45f);
            m_pick_overlay[i].set_color(c);
        }
        if (!m_pick_overlay[i].is_initialized())
            continue;

        if (shader == nullptr) {
            shader = wxGetApp().get_shader("flat");
            if (shader == nullptr)
                return;
            shader->start_using();
            glsafe(::glEnable(GL_DEPTH_TEST));
            glsafe(::glDepthFunc(GL_LEQUAL));
            glsafe(::glDepthMask(GL_FALSE));
            glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
            glsafe(::glPolygonOffset(-1.0f, -1.0f)); // wins over the volume's own, coplanar surface
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            shader->set_uniform("projection_matrix", wxGetApp().plater()->get_camera().get_projection_matrix());
        }
        shader->set_uniform("view_model_matrix", wxGetApp().plater()->get_camera().get_view_matrix() * pick_trafo(p));
        m_pick_overlay[i].render();
    }
    if (shader != nullptr) {
        glsafe(::glDisable(GL_BLEND));
        glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glDepthFunc(GL_LESS));
        shader->stop_using();
    }
}

void GLGizmoAlignStack::log_click(const char* event, int hovered, int obj, int vol, bool modifier) const
{
    if (!NeoDebug::enabled(NeoDebug::GRAVITY))
        return;
    std::ostringstream os;
    os << "ALIGN_CLICK event=" << event << " glvolume=" << hovered << " obj=" << obj << " vol=" << vol
       << " modifier=" << (modifier ? 1 : 0) << " parts_in_obj=" << model_part_count(obj) << " order_after=";
    for (size_t i = 0; i < m_ordered.size(); ++i)
        os << (i ? "," : "") << "#" << (i + 1) << "=" << m_ordered[i].obj << ":" << m_ordered[i].vol
           << "(" << pick_name(m_ordered[i]) << ")";
    if (m_ordered.empty())
        os << "none";
    NeoDebug::write(NeoDebug::GRAVITY, os.str());
}

void GLGizmoAlignStack::render_badges()
{
    if (m_ordered.empty())
        return;

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Matrix4d proj_view = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const std::array<int, 4>& viewport = camera.get_viewport();

    ImDrawList* fg = ImGui::GetForegroundDrawList();

    for (size_t i = 0; i < m_ordered.size(); ++i) {
        const BoundingBoxf3 bb = world_bbox_of(m_ordered[i]);
        if (!bb.defined)
            continue;

        // Billboard a big numbered disc just above the object's top.
        Vec3d anchor = bb.center();
        anchor.z()   = bb.max.z() + 2.0;
        const Vec2d ss = TransformHelper::world_to_ss(anchor, proj_view, viewport);
        const ImVec2 center((float)ss.x(), (float)(viewport[3] - ss.y()));

        const ImU32 col      = order_color(i);
        const float radius   = 17.0f;
        const bool  anchor_n = (i == 0); // #1 = the anchor

        // Disc + outline (anchor gets a thicker ring to read as "the base").
        fg->AddCircleFilled(center, radius, col, 32);
        fg->AddCircle(center, radius, IM_COL32(43, 52, 62, 255),
                      32, anchor_n ? 3.5f : 1.8f);

        // Big centered number.
        const std::string num = std::to_string((int)i + 1);
        ImFont* font = ImGui::GetFont();
        const float fsize = 26.0f;
        const ImVec2 tsz  = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, num.c_str());
        fg->AddText(font, fsize,
                    ImVec2(center.x - tsz.x * 0.5f, center.y - tsz.y * 0.5f),
                    IM_COL32(20, 20, 20, 255), num.c_str());
    }

    // s363d — where #2 will land: a dashed line from where it is to a target mark, both drawn at
    // a fixed size on screen, so the landing spot of a 3 mm part reads as well as a big one's.
    if (m_ghost_visible) {
        auto ss = [&](const Vec3d& w) {
            const Vec2d q = TransformHelper::world_to_ss(w, proj_view, viewport);
            return ImVec2((float)q.x(), (float)(viewport[3] - q.y()));
        };
        const ImVec2 a = ss(m_ghost_from), b = ss(m_ghost_to);
        const ImU32 ghost_col = neo_col_u32_raw(NeoCol::AccentBright);
        const float len = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
        if (len > 1.f) {
            const float ux = (b.x - a.x) / len, uy = (b.y - a.y) / len;
            for (float d = 0.f; d < len; d += 12.f) {
                const float e = std::min(d + 7.f, len);
                fg->AddLine(ImVec2(a.x + ux * d, a.y + uy * d), ImVec2(a.x + ux * e, a.y + uy * e), ghost_col, 2.5f);
            }
        }
        fg->AddCircle(b, 11.f, IM_COL32(20, 20, 20, 255), 32, 4.5f);
        fg->AddCircle(b, 11.f, ghost_col, 32, 2.5f);
        fg->AddCircleFilled(b, 3.5f, ghost_col, 16);
    }
}

// -----------------------------------------------------------------------------
// Mouse — click-to-order in the scene, plus face-pick mode
// -----------------------------------------------------------------------------

bool GLGizmoAlignStack::on_mouse(const wxMouseEvent& mouse_event)
{
    if (m_face_pick_mode) {
        // Hover feedback: remember where the cursor is so on_render can light up
        // the triangle under it. Don't consume Moving — camera/hover stay live.
        if (mouse_event.Moving()) {
            m_hover_mouse_pos = Vec2d(mouse_event.GetX(), mouse_event.GetY());
            m_have_hover_pos  = true;
            m_parent.set_as_dirty();
            m_parent.request_extra_frame();
            return false;
        }
        // s367 — a quick second click also arrives as a LeftDown; the DClick after it is swallowed.
        if (mouse_event.LeftDClick())
            return true;
        if (mouse_event.LeftDown()) {
            const int a_idx = ordered_obj(0);
            if (a_idx < 0)
                return false;

            ensure_face_raycaster_for_A();
            if (!m_face_raycaster)
                return false;

            const Camera& camera = wxGetApp().plater()->get_camera();
            const Vec2d mouse_pos(mouse_event.GetX(), mouse_event.GetY());

            Vec3f  hit_local  { 0.f, 0.f, 0.f };
            Vec3f  hit_normal { 0.f, 0.f, 1.f };
            size_t facet_idx  = 0;

            if (m_face_raycaster->unproject_on_mesh(mouse_pos,
                                                    m_face_raycaster_world_trafo,
                                                    camera, hit_local, hit_normal,
                                                    nullptr, &facet_idx)) {
                const Vec3d hit_world =
                    m_face_raycaster_world_trafo * hit_local.cast<double>();
                m_picked_face_world_pos    = hit_world;
                m_picked_face_world_normal = (m_face_raycaster_world_trafo.linear()
                                               * hit_normal.cast<double>()).normalized();
                m_picked_face_world_z      = hit_world.z();
                m_has_picked_face          = true;
                m_picked_facet_idx         = (int)facet_idx; // confirmed-face overlay
                m_parent.set_as_dirty();
                m_parent.request_extra_frame();
                // Stay in pick mode so the user can re-pick.
                return true;
            }
        }
        return false;
    }

    // Click-to-order: clicking an object in the scene assigns the next number; s367 clicking it
    // again picks just the part touched, and once more lets it go (click_object()).
    // s367 — on macOS a quick second click arrives TWICE: as a LeftDown and then as a LeftDClick
    // (seen in the GRAVITY log). Only the LeftDown counts; the DClick is swallowed, or one click
    // would take the part and let it go at once. The press is consumed, so Orca never starts
    // dragging the object under it.
    if (mouse_event.LeftDClick()) {
        if (m_parent.get_first_hover_volume_idx() >= 0) {
            log_click("dclick-ignored", m_parent.get_first_hover_volume_idx(), -1, -1, false);
            return true;
        }
        return false;
    }
    if (mouse_event.LeftDown() && !mouse_event.Dragging()) {
        const int hovered = m_parent.get_first_hover_volume_idx();
        if (hovered < 0)
            return false; // empty space → camera keeps working as usual
        const GLVolumePtrs& volumes = m_parent.get_volumes().volumes;
        if (hovered >= (int)volumes.size() || volumes[hovered] == nullptr)
            return false;
        const GLVolume* gv     = volumes[hovered];
        const int       obj_idx = gv->object_idx();
        const Model* model = m_parent.get_selection().get_model();
        if (!model || obj_idx < 0 || obj_idx >= (int)model->objects.size())
            return false;
        click_object(obj_idx, gv->is_modifier ? -1 : gv->volume_idx());
        log_click("down", hovered, obj_idx, gv->volume_idx(), gv->is_modifier);
        return true; // consume: keep global selection stable while ordering
    }
    return false;
}

void GLGizmoAlignStack::ensure_face_raycaster_for_A()
{
    if (m_ordered.empty() || pick_object(m_ordered[0]) == nullptr) {
        clear_face_pick();
        return;
    }
    const Pick a = m_ordered[0];

    // Refresh the world transform every call so a moved/rotated #1 never leaves
    // a stale raycaster (cheap); only rebuild the mesh + raycaster when #1 itself
    // changes (the mesh is object-space — s367 or the part's own space — so we
    // render with view * this trafo).
    m_face_raycaster_world_trafo = pick_trafo(a);
    if (m_face_raycaster && m_face_raycaster_pick.same(a))
        return;

    m_face_mesh = pick_mesh(a);
    m_face_raycaster.reset(new MeshRaycaster(m_face_mesh)); // copies internally
    m_face_raycaster_pick = a;
    // Cache adjacency + normals for the coplanar flood-fill (computed once per #1).
    m_face_normals   = its_face_normals(m_face_mesh.its);
    m_face_neighbors = its_face_neighbors(m_face_mesh.its);
    m_hover_facet_idx = m_hover_model_facet = -1; // force highlight rebuild
}

void GLGizmoAlignStack::clear_face_pick()
{
    m_face_pick_mode         = false;
    m_has_picked_face        = false;
    m_face_raycaster.reset();
    m_face_raycaster_pick    = Pick();
    m_have_hover_pos         = false;
    m_hover_facet_idx        = -1;
    m_hover_model_facet      = -1;
    m_picked_facet_idx       = -1;
    m_picked_model_facet     = -1;
    m_hover_face_model.reset();
    m_picked_face_model.reset();
    // NEOTKO_CONSTRUCT_TAG s363
    m_construct_ghost.reset();
    m_construct_ghost_facet = -2;
    m_construct_block_reason.clear();
}

// -----------------------------------------------------------------------------
// Face highlight — light up the triangle under the cursor (and the picked one)
// so the user gets feedback that a face is about to be / has been chosen.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::build_face_model(GLModel& model, int facet_idx, const ColorRGBA& col, float cos_thresh)
{
    model.reset();
    const int n_facets = (int)m_face_mesh.its.indices.size();
    if (facet_idx < 0 || facet_idx >= n_facets ||
        (int)m_face_normals.size()   != n_facets ||
        (int)m_face_neighbors.size() != n_facets)
        return;

    // Grow the connected region of facets whose normal stays within a small
    // angle of the seed: a flat face fills completely, a curved one yields a
    // patch around the cursor. Capped so a near-flat huge mesh can't explode.
    const Vec3f            seed_n = m_face_normals[facet_idx].normalized();
    const std::vector<int> region = SurfaceExtrude::coplanar_region(m_face_normals, m_face_neighbors,
                                                                    facet_idx, cos_thresh);

    const float lift = 0.10f; // along each facet's own normal, beats z-fighting
    indexed_triangle_set its;
    its.vertices.reserve(region.size() * 3);
    its.indices.reserve(region.size());
    int base = 0;
    for (int f : region) {
        const Vec3i32 tri = m_face_mesh.its.indices[f];
        Vec3f n = m_face_normals[f];
        const float nl = n.norm();
        n = (nl > 1e-6f) ? (n / nl) : seed_n;
        its.vertices.push_back(m_face_mesh.its.vertices[tri[0]] + n * lift);
        its.vertices.push_back(m_face_mesh.its.vertices[tri[1]] + n * lift);
        its.vertices.push_back(m_face_mesh.its.vertices[tri[2]] + n * lift);
        its.indices.emplace_back(base, base + 1, base + 2);
        base += 3;
    }

    if (its.indices.empty())
        return;
    model.init_from(its);
    model.set_color(col);
}

void GLGizmoAlignStack::update_hover_face(const Vec2d& mouse_pos)
{
    ensure_face_raycaster_for_A();
    if (!m_face_raycaster) {
        m_hover_facet_idx = -1;
        return;
    }

    const Camera& camera = wxGetApp().plater()->get_camera();
    Vec3f  hit_local  { 0.f, 0.f, 0.f };
    Vec3f  hit_normal { 0.f, 0.f, 1.f };
    size_t facet_idx  = 0;

    if (m_face_raycaster->unproject_on_mesh(mouse_pos, m_face_raycaster_world_trafo,
                                            camera, hit_local, hit_normal,
                                            nullptr, &facet_idx))
        m_hover_facet_idx = (int)facet_idx;
    else
        m_hover_facet_idx = -1;
}

void GLGizmoAlignStack::render_face_highlights()
{
    if (!m_face_pick_mode)
        return;

    // Refresh the hovered facet from the last known cursor position.
    if (m_have_hover_pos)
        update_hover_face(m_hover_mouse_pos);

    // (Re)build overlays only when their facet changed.
    if (m_hover_facet_idx != m_hover_model_facet) {
        if (m_hover_facet_idx >= 0)
            build_face_model(m_hover_face_model, m_hover_facet_idx,
                             ColorRGBA(0.10f, 0.80f, 0.74f, 0.55f), kHoverFaceCos); // teal hover
        else
            m_hover_face_model.reset();
        m_hover_model_facet = m_hover_facet_idx;
    }
    if (m_picked_facet_idx != m_picked_model_facet) {
        if (m_picked_facet_idx >= 0)
            build_face_model(m_picked_face_model, m_picked_facet_idx,
                             ColorRGBA(0.16f, 0.85f, 0.30f, 0.60f), kPickedFaceCos); // green picked
        else
            m_picked_face_model.reset();
        m_picked_model_facet = m_picked_facet_idx;
    }

    if (!m_hover_face_model.is_initialized() && !m_picked_face_model.is_initialized())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE)); // raw-mesh facet winding varies; show both sides
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera& camera = wxGetApp().plater()->get_camera();
    const Transform3d view_model_matrix = camera.get_view_matrix() * m_face_raycaster_world_trafo;
    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    // Picked face underneath, live hover on top.
    if (m_picked_face_model.is_initialized())
        m_picked_face_model.render();
    if (m_hover_face_model.is_initialized())
        m_hover_face_model.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

// -----------------------------------------------------------------------------
// NEOTKO_CONSTRUCT_TAG_START s363 — Construct: extrude the picked flat face of #1
// into a sheet of the chosen height (outline optionally grown/shrunk), added as
// a part of #1 or as a separate object. s367: any flat face, along its own normal;
// optionally both ways (half out, half in) and optionally just its holes, filled.
// -----------------------------------------------------------------------------

bool GLGizmoAlignStack::build_construct_mesh(indexed_triangle_set& out, std::string& why, bool preview,
                                             size_t* n_facets_out, double* face_z_out)
{
    out.clear();
    why.clear();
    const int n_facets = (int)m_face_mesh.its.indices.size();
    if (!m_has_picked_face || m_picked_facet_idx < 0 || m_picked_facet_idx >= n_facets ||
        (int)m_face_normals.size() != n_facets || (int)m_face_neighbors.size() != n_facets) {
        why = _u8L("Pick a face on #1 first.");
        return false;
    }
    if (m_construct_height <= 0.f) {
        why = _u8L("Height must be above 0.");
        return false;
    }

    // Normal of the picked facet in world space (inverse transpose: survives scaling).
    const Matrix3d lin     = m_face_raycaster_world_trafo.linear();
    const Vec3d    world_n = (lin.inverse().transpose() *
                              m_face_normals[m_picked_facet_idx].cast<double>()).normalized();
    if (!world_n.allFinite()) {
        why = _u8L("That face has no usable outline.");
        return false;
    }

    // s367 — any face: turn the world so the face looks straight up (+Z), extrude there exactly as
    // a top face, and turn the solid back. `to_flat` is a pure rotation, so nothing is distorted.
    const Eigen::Quaterniond q_flat = Eigen::Quaterniond::FromTwoVectors(world_n, Vec3d::UnitZ());
    Transform3d to_flat = Transform3d::Identity();
    to_flat.linear()    = q_flat.toRotationMatrix();

    const std::vector<int> region = SurfaceExtrude::coplanar_region(m_face_normals, m_face_neighbors,
                                                                    m_picked_facet_idx, kPickedFaceCos);
    double face_z = 0.;
    ExPolygons outline = SurfaceExtrude::region_outline(m_face_mesh.its, region,
                                                        to_flat * m_face_raycaster_world_trafo, &face_z);
    if (outline.empty()) {
        why = _u8L("That face has no usable outline.");
        return false;
    }
    if (m_construct_holes) {
        // s367 — the holes of the face, each one filled: e.g. the circle of a screw, ready to
        // carve a bigger hole with a boolean (Offset grows them).
        outline = SurfaceExtrude::outline_holes(outline);
        if (outline.empty()) {
            why = _u8L("That face has no holes.");
            return false;
        }
    }

    // Outwards from the face, or s367 half outwards and half inwards.
    const double h  = (double)m_construct_height;
    const double z0 = m_construct_both ? face_z - 0.5 * h : face_z;
    if (curve_active())
        out = SurfaceExtrude::extrude_outline_curved(outline, (double)m_construct_offset, z0, h, m_curve,
                                                     false, preview ? std::max(0.025, h / 60.) : 0.025);
    else
        out = SurfaceExtrude::extrude_outline(outline, (double)m_construct_offset, z0, h);
    if (out.indices.empty()) {
        why = _u8L("With that offset nothing is left of the shape.");
        return false;
    }
    its_transform(out, to_flat.inverse());
    if (n_facets_out) *n_facets_out = region.size();
    if (face_z_out)   *face_z_out   = face_z;
    return true;
}

void GLGizmoAlignStack::update_construct_ghost()
{
    if (!m_has_picked_face) {
        if (m_construct_ghost.is_initialized())
            m_construct_ghost.reset();
        m_construct_ghost_facet = -2;
        m_construct_block_reason.clear();
        return;
    }
    if (m_construct_ghost_facet == m_picked_facet_idx &&
        m_construct_ghost_height == m_construct_height &&
        m_construct_ghost_offset == m_construct_offset &&
        m_construct_ghost_both   == m_construct_both &&
        m_construct_ghost_holes  == m_construct_holes &&
        m_construct_ghost_curve_on == curve_active() &&
        m_construct_ghost_curve_rev == m_curve_rev &&
        m_construct_ghost_trafo.isApprox(m_face_raycaster_world_trafo))
        return;
    m_construct_ghost_curve_on  = curve_active();
    m_construct_ghost_curve_rev = m_curve_rev;

    m_construct_ghost_facet  = m_picked_facet_idx;
    m_construct_ghost_height = m_construct_height;
    m_construct_ghost_offset = m_construct_offset;
    m_construct_ghost_both   = m_construct_both;
    m_construct_ghost_holes  = m_construct_holes;
    m_construct_ghost_trafo  = m_face_raycaster_world_trafo;

    m_construct_ghost.reset();
    indexed_triangle_set its;
    if (build_construct_mesh(its, m_construct_block_reason, true)) {
        m_construct_ghost.init_from(its);
        m_construct_ghost.set_color(ColorRGBA(1.0f, 0.55f, 0.0f, 0.55f));
    }
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void GLGizmoAlignStack::render_construct_ghost()
{
    if (!m_has_picked_face)
        return;
    update_construct_ghost();
    if (!m_construct_ghost.is_initialized())
        return;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    // The sheet is already in world coordinates.
    const Camera& camera = wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix());
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_construct_ghost.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

void GLGizmoAlignStack::apply_construct()
{
    const int a_idx = ordered_obj(0);
    if (a_idx < 0)
        return;

    indexed_triangle_set its;
    std::string          why;
    size_t               n_facets = 0;
    double               face_z   = 0.;
    if (!build_construct_mesh(its, why, false, &n_facets, &face_z))
        return;

    const bool        as_object = m_construct_as_object;
    const std::string name      = _u8L("Extruded surface");
    const size_t      n_tris    = its.indices.size();
    {
        Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Construct: extrude surface");
        TriangleMesh mesh(std::move(its));
        if (as_object)
            wxGetApp().obj_list()->load_mesh_object(mesh, from_u8(name), false);
        else
            wxGetApp().obj_list()->neotko_add_mesh_part(a_idx, std::move(mesh), name);
    }

    if (NeoDebug::enabled(NeoDebug::GRAVITY)) {
        std::ostringstream os;
        os << "CONSTRUCT_EXTRUDE obj=" << a_idx << " facets=" << n_facets << " face_z=" << face_z
           << " height=" << m_construct_height << " offset=" << m_construct_offset
           << " tris=" << n_tris << " as=" << (as_object ? "object" : "part")
           << " curve=" << (curve_active() ? (int)m_curve.size() : 0);
        NeoDebug::write(NeoDebug::GRAVITY, os.str());
    }

    // #1's mesh may have just grown a part: the cached raycaster and the pick are stale.
    clear_face_pick();
    refresh_highlight();
}

// s363c — extrusion along an editable curve (hidden, debug mode only, undocumented on purpose).
// Seen from the side: up = height above the picked face, right = the outline grows, left = it
// shrinks. Red lines are heights the curve must pass through; squares pull the span between two.
bool GLGizmoAlignStack::curve_active() const
{
    return m_curve_on && m_curve.size() >= 2 && NeoDebug::console_enabled();
}

void GLGizmoAlignStack::reset_curve()
{
    m_curve.assign(2, SurfaceExtrude::CurveNode());
    m_curve[0].t = 0.; m_curve[0].ht = 0.5;
    m_curve[1].t = 1.; m_curve[1].ht = 1.;
    m_curve_sel = m_curve_drag = -1;
    ++m_curve_rev;
}

void GLGizmoAlignStack::set_curve_point(int code, double t, double e)
{
    const int n = (int)m_curve.size();
    const int i = code / 2;
    if (code < 0 || i >= n)
        return;
    e = std::clamp(e, -100., 100.);
    if (code % 2 == 0) {
        SurfaceExtrude::CurveNode& nd = m_curve[i];
        if (i == 0)          t = 0.;
        else if (i == n - 1) t = 1.;
        else                 t = std::clamp(t, m_curve[i - 1].t + 0.02, m_curve[i + 1].t - 0.02);
        nd.t = t;
        nd.e = e;
    } else if (i + 1 < n) {
        m_curve[i].ht = t;
        m_curve[i].he = e;
    }
    // A handle never leaves the heights of its own span: the curve always climbs.
    for (int k = 0; k + 1 < n; ++k)
        m_curve[k].ht = std::clamp(m_curve[k].ht, m_curve[k].t, m_curve[k + 1].t);
    ++m_curve_rev;
}

void GLGizmoAlignStack::render_curve_editor()
{
    if (m_curve.size() < 2)
        reset_curve();
    const int n = (int)m_curve.size();

    const float W = ImGui::GetContentRegionAvail().x, Hpx = 10.f * neo_u(), pad = 0.8f * neo_u();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##construct_curve", ImVec2(W, Hpx));
    const bool active = ImGui::IsItemActive();

    if (m_curve_drag < 0) { // the scale only follows the curve while nothing is being dragged
        float r = 1.f;
        for (const SurfaceExtrude::CurveNode& nd : m_curve)
            r = std::max(r, 1.3f * (float)std::max(std::abs(nd.e), std::abs(nd.he)));
        m_curve_range = r;
    }
    const float R    = m_curve_range;
    const float half = W * 0.5f - pad;
    const float span = Hpx - 2.f * pad;
    auto to_px = [&](double t, double e) {
        return ImVec2(p0.x + W * 0.5f + (float)e / R * half, p0.y + Hpx - pad - (float)t * span);
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    neo_draw_canvas(dl, p0, W, Hpx);
    dl->AddLine(to_px(0., 0.), to_px(1., 0.), neo_col_u32(NeoCol::GridMajor), 1.f); // the face's own outline
    for (int i = 0; i < n; ++i) // tension lines
        dl->AddLine(ImVec2(p0.x + pad * 0.5f, to_px(m_curve[i].t, 0.).y), ImVec2(p0.x + W - pad * 0.5f, to_px(m_curve[i].t, 0.).y),
                    IM_COL32(220, 60, 60, 150), 1.f);

    const std::vector<Vec2d> samples = SurfaceExtrude::sample_curve(m_curve, 24);
    std::vector<ImVec2> pts;
    pts.reserve(samples.size());
    for (const Vec2d& s : samples)
        pts.emplace_back(to_px(s.x(), s.y()));
    dl->AddPolyline(pts.data(), (int)pts.size(), neo_col_u32(NeoCol::AccentBright), ImDrawFlags_None, 2.2f);

    for (int i = 0; i + 1 < n; ++i) { // tension points
        const ImVec2 h = to_px(m_curve[i].ht, m_curve[i].he);
        dl->AddLine(to_px(m_curve[i].t, m_curve[i].e), h, neo_col_u32(NeoCol::Grid), 1.f);
        dl->AddLine(to_px(m_curve[i + 1].t, m_curve[i + 1].e), h, neo_col_u32(NeoCol::Grid), 1.f);
        const float s = m_curve_sel == 2 * i + 1 ? 6.f : 4.5f;
        dl->AddRectFilled(ImVec2(h.x - s, h.y - s), ImVec2(h.x + s, h.y + s), neo_col_u32(NeoCol::Ink), 1.f);
    }
    for (int i = 0; i < n; ++i)
        dl->AddCircleFilled(to_px(m_curve[i].t, m_curve[i].e), m_curve_sel == 2 * i ? 7.f : 5.f, IM_COL32(230, 50, 50, 255));

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (ImGui::IsItemActivated()) {
        int   best   = -1;
        float best_d = 12.f * 12.f;
        auto consider = [&](int code, const ImVec2& p) {
            const float d = (p.x - mouse.x) * (p.x - mouse.x) + (p.y - mouse.y) * (p.y - mouse.y);
            if (d < best_d) { best_d = d; best = code; }
        };
        for (int i = 0; i < n; ++i)
            consider(2 * i, to_px(m_curve[i].t, m_curve[i].e));
        for (int i = 0; i + 1 < n; ++i)
            consider(2 * i + 1, to_px(m_curve[i].ht, m_curve[i].he));
        m_curve_drag = best;
        m_curve_sel  = best;
    }
    if (active && m_curve_drag >= 0)
        set_curve_point(m_curve_drag, (double)((p0.y + Hpx - pad - mouse.y) / span),
                        (double)((mouse.x - p0.x - W * 0.5f) / half * R));
    if (ImGui::IsItemDeactivated())
        m_curve_drag = -1;

    if (neo_text_button(_u8L("Add line").c_str(), NeoBtn::Ghost)) {
        int    widest = 0;
        double gap    = 0.;
        for (int i = 0; i + 1 < n; ++i)
            if (m_curve[i + 1].t - m_curve[i].t > gap) { gap = m_curve[i + 1].t - m_curve[i].t; widest = i; }
        if (gap > 0.08) {
            SurfaceExtrude::CurveNode nd;
            nd.t = 0.5 * (m_curve[widest].t + m_curve[widest + 1].t);
            nd.e = SurfaceExtrude::curve_value(samples, nd.t);
            // Both new spans start straight.
            nd.ht = 0.5 * (nd.t + m_curve[widest + 1].t);
            nd.he = 0.5 * (nd.e + m_curve[widest + 1].e);
            m_curve[widest].ht = 0.5 * (m_curve[widest].t + nd.t);
            m_curve[widest].he = 0.5 * (m_curve[widest].e + nd.e);
            m_curve.insert(m_curve.begin() + widest + 1, nd);
            m_curve_sel = 2 * (widest + 1);
            ++m_curve_rev;
        }
    }
    ImGui::SameLine();
    const int  sel_node   = (m_curve_sel >= 0 && m_curve_sel % 2 == 0) ? m_curve_sel / 2 : -1;
    const bool can_remove = sel_node > 0 && sel_node < (int)m_curve.size() - 1;
    m_imgui->disabled_begin(!can_remove);
    if (neo_text_button(_u8L("Remove line").c_str(), NeoBtn::Ghost) && can_remove) {
        m_curve.erase(m_curve.begin() + sel_node);
        m_curve[sel_node - 1].ht = 0.5 * (m_curve[sel_node - 1].t + m_curve[sel_node].t);
        m_curve[sel_node - 1].he = 0.5 * (m_curve[sel_node - 1].e + m_curve[sel_node].e);
        m_curve_sel = -1;
        ++m_curve_rev;
    }
    m_imgui->disabled_end();
    ImGui::SameLine();
    if (neo_text_button(_u8L("Straight").c_str(), NeoBtn::Ghost))
        reset_curve();

    // The two numbers of whatever is selected.
    if (m_curve_sel >= 0 && m_curve_sel / 2 < (int)m_curve.size()) {
        const int  i      = m_curve_sel / 2;
        const bool handle = m_curve_sel % 2 == 1;
        if (!handle || i + 1 < (int)m_curve.size()) {
            const double h_mm = (double)m_construct_height;
            float z_mm = (float)((handle ? m_curve[i].ht : m_curve[i].t) * h_mm);
            float e_mm = (float)(handle ? m_curve[i].he : m_curve[i].e);
            bool  edit = false;
            ImGui::SetNextItemWidth(4.6f * neo_u());
            edit |= ImGui::InputFloat(_u8L("at height").c_str(), &z_mm, 0.0f, 0.0f, "%.3f");
            ImGui::SameLine(0.f, 0.9f * neo_u());
            ImGui::SetNextItemWidth(4.6f * neo_u());
            edit |= ImGui::InputFloat(_u8L("grows").c_str(), &e_mm, 0.0f, 0.0f, "%.3f");
            if (edit && h_mm > 0.)
                set_curve_point(m_curve_sel, (double)z_mm / h_mm, (double)e_mm);
        }
    }
}

// s363b — Boolean of #1 with #2 as whole objects. Orca's own boolean only works between the
// parts of ONE object, which forces an "assemble" first; here both meshes are taken where they
// sit in the scene and the result comes out as a new object in the same place.
void GLGizmoAlignStack::apply_boolean(const std::string& op, const std::string& suffix)
{
    m_bool_error.clear();
    prune_dead_objects();
    if (m_ordered.size() < 2)
        return;
    const Pick pa = m_ordered[0];
    const Pick pb = m_ordered[1];
    Model& model = wxGetApp().plater()->model();
    const ModelObject* a = pick_object(pa);
    const ModelObject* b = pick_object(pb);
    if (a == nullptr || b == nullptr || pa.same(pb))
        return;
    // s367 — a whole object and one of its own parts overlap by definition: nothing sensible.
    if (pa.obj == pb.obj && (!pa.is_part() || !pb.is_part())) {
        m_bool_error = _u8L("One of them is inside the other. Pick two different parts, or two objects.");
        return;
    }
    // s367 — two parts of the same object: the result stays a part of that object, in place.
    const bool in_place = pa.obj == pb.obj;

    TriangleMesh mesh_a = pick_mesh(pa);
    mesh_a.transform(pick_trafo(pa));
    TriangleMesh mesh_b = pick_mesh(pb);
    mesh_b.transform(pick_trafo(pb));

    std::vector<TriangleMesh> results;
    const auto t_bool = std::chrono::steady_clock::now(); // NEOTKO_PAINTCARRY_TAG
    MeshBoolean::mcut::make_boolean(mesh_a, mesh_b, results, op);
    const long boolean_ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_bool).count());
    long       paint_ms   = 0;
    if (results.empty() || results.front().empty()) {
        m_bool_error = _u8L("The operation gave no result. The two objects may not overlap, or one of them is not a closed solid.");
        return;
    }

    const std::string name     = pick_name(pa) + " - " + suffix;
    const int         extruder = pa.is_part() ? PaintTransfer::effective_filament(*pick_volume(pa))
                                              : (a->config.has("extruder") ? a->config.opt_int("extruder") : 0);
    const bool        keep     = m_bool_keep_inputs;
    const size_t      n_tris   = results.front().facets_count();
    // NEOTKO_PAINTCARRY_TAG s366 — where the result sits before it is placed: load_mesh_object()
    // centres the mesh on its bounding box (and may drop the object to the bed afterwards).
    const Vec3d                        result_centre = results.front().bounding_box().center();
    std::vector<PaintTransfer::Source> paint_sources;
    // s367 — a whole object gives all its parts; a part gives just itself.
    auto add_sources = [&](const Pick& p, const ModelObject& mo, bool cutter) {
        if (!p.is_part()) {
            PaintTransfer::append_object_sources(mo, cutter, paint_sources);
            return;
        }
        PaintTransfer::Source src;
        src.volume   = mo.volumes[p.vol];
        src.to_world = pick_trafo(p);
        src.filament = PaintTransfer::effective_filament(*src.volume);
        src.cutter   = cutter;
        paint_sources.push_back(src);
    };
    add_sources(pa, *a, false);
    add_sources(pb, *b, op == "A_NOT_B"); // the cut walls stay unpainted
    // What to delete afterwards, by pointer: indices move while the model changes.
    const ModelObject* del_obj[2] = { a, b };
    const ModelVolume* del_vol[2] = { pick_volume(pa), pick_volume(pb) };
    PaintTransfer::Stats paint_stats;
    // Keeping the painting can take a few seconds on big meshes. Shown (and painted, with a yield)
    // before the model changes, so the yield never draws it half done.
    std::unique_ptr<wxBusyInfo> busy;
    if (PaintTransfer::worth_transferring(paint_sources)) {
        busy = std::make_unique<wxBusyInfo>(_L("Keeping the painting..."), m_parent.get_wxglcanvas());
        wxSafeYield(nullptr, true);
    }
    {
        Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Construct: boolean");
        // Leave the order (and the mirrored selection) empty before the model changes.
        clear_order();

        ModelVolume* result     = nullptr;
        int          result_obj = -1;
        Transform3d  result_to_world = Transform3d::Identity();
        if (in_place) {
            result_obj = pa.obj;
            result     = wxGetApp().obj_list()->neotko_add_mesh_part(result_obj, results.front(), name);
            if (result != nullptr) {
                if (extruder > 0)
                    result->config.set_key_value("extruder", new ConfigOptionInt(extruder));
                result_to_world = model.objects[result_obj]->instances.front()->get_matrix() * result->get_matrix();
            }
        } else {
            wxGetApp().obj_list()->load_mesh_object(results.front(), from_u8(name), false);
            if (extruder > 0 && !model.objects.empty())
                model.objects.back()->config.set_key_value("extruder", new ConfigOptionInt(extruder));
            if (!model.objects.empty() && !model.objects.back()->volumes.empty()) {
                result_obj      = int(model.objects.size()) - 1;
                result          = model.objects.back()->volumes.front();
                result_to_world = Geometry::translation_transform(result_centre) * result->get_matrix();
            }
        }

        // NEOTKO_PAINTCARRY_TAG s366 — the result keeps the painting of #1 and #2, and the filament
        // each one had. Before deleting the inputs: the sources point into them.
        if (result != nullptr) {
            const auto t_paint = std::chrono::steady_clock::now();
            paint_stats = PaintTransfer::transfer(paint_sources, *result, result_to_world,
                                                  PaintTransfer::effective_filament(*result));
            paint_ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_paint).count());
            if (paint_stats.layers > 0)
                wxGetApp().obj_list()->update_info_items(size_t(result_obj));
        }

        if (!keep) {
            auto obj_index = [&](const ModelObject* mo) {
                for (size_t i = 0; i < model.objects.size(); ++i)
                    if (model.objects[i] == mo) return int(i);
                return -1;
            };
            // Parts first (deleting a part never moves an object), the higher index first so the
            // other keeps its place; then whole objects, the higher index first as well.
            std::vector<std::pair<int, int>> parts;
            for (int k = 0; k < 2; ++k) {
                if (del_vol[k] == nullptr)
                    continue;
                const int oi = obj_index(del_obj[k]);
                if (oi < 0)
                    continue;
                const ModelVolumePtrs& vols = model.objects[oi]->volumes;
                for (size_t vi = 0; vi < vols.size(); ++vi)
                    if (vols[vi] == del_vol[k]) { parts.emplace_back(oi, int(vi)); break; }
            }
            std::sort(parts.begin(), parts.end(), [](const auto& l, const auto& r) { return l.second > r.second; });
            for (const auto& pv : parts)
                wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itVolume, pv.first, pv.second);
            std::vector<int> objs;
            for (int k = 0; k < 2; ++k)
                if (del_vol[k] == nullptr) {
                    const int oi = obj_index(del_obj[k]);
                    if (oi >= 0) objs.push_back(oi);
                }
            std::sort(objs.rbegin(), objs.rend());
            for (int oi : objs)
                wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itObject, oi, -1);
        }
    }

    if (NeoDebug::enabled(NeoDebug::GRAVITY)) {
        std::ostringstream os;
        os << "CONSTRUCT_BOOLEAN op=" << op << " a=" << pa.obj << ":" << pa.vol << " b=" << pb.obj << ":" << pb.vol
           << " in_place=" << (in_place ? 1 : 0) << " tris=" << n_tris
           << " keep_inputs=" << (keep ? 1 : 0)
           << " paint_sources=" << paint_sources.size() << " paint_layers=" << paint_stats.layers
           << " paint_samples=" << paint_stats.samples << " paint_searches=" << paint_stats.searches << " boolean_ms=" << boolean_ms << " paint_ms=" << paint_ms;
        NeoDebug::write(NeoDebug::GRAVITY, os.str());
    }
    refresh_highlight();
}
// NEOTKO_CONSTRUCT_TAG_END

// -----------------------------------------------------------------------------
// Transform application — #1 is always the anchor; #2 moves toward it.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::translate_pick(const Pick& p, const Vec3d& delta)
{
    if (delta.norm() < 1e-9)
        return;
    const ModelObject* mo = pick_object(p);
    if (mo == nullptr)
        return;
    Selection& sel = m_parent.get_selection();
    if (!p.is_part()) {
        for (size_t i = 0; i < mo->instances.size(); ++i)
            sel.translate((unsigned int)p.obj, (unsigned int)i, delta);
        m_parent.do_move("");
        return;
    }
    // s367 — a part moves inside its object: its offset lives in the instance's space, so the world
    // move is taken back through the first instance (one ModelVolume serves every instance). The
    // scene volumes are moved directly and do_move() in Volume mode writes them into the model,
    // exactly as when a part is moved with Orca's own Move.
    const Vec3d local = mo->instances.front()->get_matrix().linear().inverse() * delta;
    for (GLVolume* v : m_parent.get_volumes().volumes)
        if (v != nullptr && v->object_idx() == p.obj && v->volume_idx() == p.vol)
            v->set_volume_offset(v->get_volume_offset() + local);
    const Selection::EMode old_mode = sel.get_mode();
    sel.set_mode(Selection::Volume);
    m_parent.do_move("");
    sel.set_mode(old_mode);
}

Vec3d GLGizmoAlignStack::compute_place_delta(int axis, int dir, bool flush,
                                             const BoundingBoxf3& a_bb, const BoundingBoxf3& b_bb) const
{
    Vec3d delta = Vec3d::Zero();
    if (!a_bb.defined || !b_bb.defined)
        return delta;
    if (flush) {
        const double target = (dir > 0) ? a_bb.max(axis) : a_bb.min(axis);
        delta(axis) = target - ((dir > 0) ? b_bb.max(axis) : b_bb.min(axis));
    } else {
        // Z contact gets the epsilon gap (slicing sanity); lateral contact is exact.
        const double gap = (axis == 2) ? (double)m_epsilon_mm : 0.0;
        delta(axis) = (dir > 0) ? (a_bb.max(axis) + gap - b_bb.min(axis))
                                : (a_bb.min(axis) - gap - b_bb.max(axis));
    }
    return delta;
}

Vec3d GLGizmoAlignStack::compute_center_delta(int axis, const BoundingBoxf3& a_bb, const BoundingBoxf3& b_bb) const
{
    Vec3d delta = Vec3d::Zero();
    if (a_bb.defined && b_bb.defined)
        delta(axis) = a_bb.center()(axis) - b_bb.center()(axis);
    return delta;
}

// s363d — every way of placing #2 goes through here, for the click AND for the ghost, so the
// ghost cannot lie about what the click will do.
bool GLGizmoAlignStack::op_delta(Op op, Vec3d& delta) const
{
    delta = Vec3d::Zero();
    if (op == Op::None || m_ordered.size() < 2)
        return false;
    const BoundingBoxf3 a = world_bbox_of(m_ordered[0]);
    const BoundingBoxf3 b = world_bbox_of(m_ordered[1]);
    if (!a.defined || !b.defined)
        return false;
    switch (op) {
    case Op::XNeg:     delta = compute_place_delta(0, -1, m_flush_mode, a, b); break;
    case Op::XPos:     delta = compute_place_delta(0, +1, m_flush_mode, a, b); break;
    case Op::YNeg:     delta = compute_place_delta(1, -1, m_flush_mode, a, b); break;
    case Op::YPos:     delta = compute_place_delta(1, +1, m_flush_mode, a, b); break;
    case Op::OnTop:    delta = compute_place_delta(2, +1, m_flush_mode, a, b); break;
    case Op::CenterXY: delta = compute_center_delta(0, a, b) + compute_center_delta(1, a, b); break;
    case Op::CenterZ:  delta = compute_center_delta(2, a, b); break;
    case Op::Bed:      delta = Vec3d(0.0, 0.0, -b.min.z()); break;
    case Op::Face:
        if (!m_has_picked_face) return false;
        delta = Vec3d(0.0, 0.0, m_picked_face_world_z + (double)m_epsilon_mm - b.min.z());
        break;
    default: return false;
    }
    return true;
}

void GLGizmoAlignStack::apply_op(Op op)
{
    Vec3d delta;
    if (!op_delta(op, delta))
        return;
    Plater::TakeSnapshot snap(wxGetApp().plater(), "Align & Construct: place");
    translate_pick(m_ordered[1], delta);
}

void GLGizmoAlignStack::swap_order()
{
    if (m_ordered.size() < 2)
        return;
    std::swap(m_ordered[0], m_ordered[1]);
    if (m_face_pick_mode || m_has_picked_face)
        clear_face_pick();          // the picked face belonged to the old #1
    m_ghost_mesh_pick = Pick();
    refresh_highlight();
}

// -----------------------------------------------------------------------------
// s363d — Placement ghost. Only ONE at a time: the one for the button under the mouse. It is
// #2's real mesh, translucent, drawn where the click would leave it.
// -----------------------------------------------------------------------------

void GLGizmoAlignStack::render_place_ghost()
{
    m_ghost_visible = false;
    Vec3d delta;
    if (!op_delta(m_hover_op, delta))
        return;
    const Pick         pb = m_ordered[1];
    const ModelObject* mo = pick_object(pb);
    if (mo == nullptr)
        return;

    if (!m_ghost_mesh.is_initialized() || !m_ghost_mesh_pick.same(pb) || m_ghost_mesh_volumes != mo->volumes.size()) {
        m_ghost_mesh.reset();
        const TriangleMesh mesh = pick_mesh(pb);          // object space, s367 or the part's own
        if (mesh.empty())
            return;
        m_ghost_mesh.init_from(mesh.its);
        m_ghost_mesh.set_color(ColorRGBA(0.18f, 0.84f, 0.77f, 0.50f));   // the panel's teal
        m_ghost_mesh_pick    = pb;
        m_ghost_mesh_volumes = mo->volumes.size();
    }

    const BoundingBoxf3 bb = world_bbox_of(pb);
    m_ghost_from    = bb.center();
    m_ghost_to      = bb.center() + delta;
    m_ghost_visible = true;

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    shader->start_using();
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    const Camera& camera = wxGetApp().plater()->get_camera();
    Transform3d placed = Transform3d::Identity();
    placed.translate(delta);
    placed = placed * pick_trafo(pb);
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * placed);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_ghost_mesh.render();

    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_CULL_FACE));
    shader->stop_using();
}

void GLGizmoAlignStack::on_render_input_window(float x, float y, float bottom_limit)
{
    prune_dead_objects();

    // s363d — the house look (GizmoNeotkoStyle.hpp), like the ColorStitch Painter and NeoStroke:
    // fixed width in font heights, so nothing is clipped whatever the DPI or the language.
    const float u     = neo_u();
    const float win_w = 22.f * u;
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(win_w, 0.f), ImVec2(win_w, FLT_MAX));
    neo_push_window_style();   // 🚨 before Begin
    GizmoImguiBegin(on_get_name(),
                    ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);
    ImGui::PushTextWrapPos(win_w - 16.f);
    neo_push_panel_style();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(0.45f * u, 0.42f * u));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.45f * u, 0.28f * u));

    render_panel_body();       // 🚨 its own function: no early exit in there can skip the Pops

    ImGui::PopStyleVar(2);
    neo_pop_panel_style();
    ImGui::PopTextWrapPos();
    GizmoImguiEnd();
    neo_pop_window_style();
}

void GLGizmoAlignStack::render_panel_body()
{
    const float  u   = neo_u();
    const size_t n   = m_ordered.size();
    const bool   two = n >= 2;
    Op hovered = Op::None;      // the placing button under the mouse this frame, if any

    auto tip  = [](const std::string& text) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text.c_str()); };
    auto hint = [](const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    };
    auto dim_label = [](const std::string& text) {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
    };

    // --- The two objects ------------------------------------------------------
    for (size_t i = 0; i < n; ++i) {
        const Pick& pk = m_ordered[i];
        if (i > 0) ImGui::SameLine(0.0f, 0.35f * u);
        std::string name = pick_name(pk);
        if (name.size() > 10) name = name.substr(0, 9) + "…";
        // s367 — a part shows a small mark, so it reads apart from a whole object.
        const std::string id = "##chip" + std::to_string(i);
        if (order_chip(id.c_str(), "#" + std::to_string((int)i + 1) + (pk.is_part() ? "  · " : "  ") + name, order_color(i))) {
            remove_pick(i); // click chip = remove from order
            break;
        }
        const std::string what = pk.is_part() ? _u8L("Just this part of its object.") : _u8L("The whole object.");
        tip((i == 0 ? _u8L("#1 stays where it is.") : _u8L("#2 is the one that moves.")) + "\n" + what + "\n" + _u8L("Click to let it go."));
    }
    if (two) {
        ImGui::SameLine(0.0f, 0.5f * u);
        if (neo_text_button(_u8L("Swap").c_str(), NeoBtn::Ghost))
            swap_order();
        tip(_u8L("#1 becomes #2 and #2 becomes #1."));
    }
    if (n > 0) {
        ImGui::SameLine(0.0f, 0.35f * u);
        if (neo_text_button(_u8L("Reset").c_str(), NeoBtn::Ghost)) {
            clear_order();
        }
        tip(_u8L("Let both objects go and start again."));
    }
    if (n == 0)
        hint(_u8L("Click an object: it becomes #1, the one that stays. Then click another: #2, the one that moves. Click an object made of several parts again to take just the part you touch."));
    else if (n == 1)
        hint(_u8L("Click a second object to place it against #1, or pick a face of #1 below. Click #1 again to take just the part you touch."));

    // --- Place #2 ---------------------------------------------------------------
    neo_section_g(_u8L("Place #2").c_str(), Glyph::Landing);
    m_imgui->disabled_begin(!two);
    {
        const float avail = ImGui::GetContentRegionAvail().x;
        const float gap   = 0.35f * u;
        const float mw    = (avail - gap) * 0.5f;
        if (mode_card("##as_next", _u8L("Next to"), false, !m_flush_mode, mw)) m_flush_mode = false;
        tip(_u8L("#2 ends up beside #1, touching it\non the side you choose."));
        ImGui::SameLine(0.0f, gap);
        if (mode_card("##as_edge", _u8L("Align edge"), true, m_flush_mode, mw)) m_flush_mode = true;
        tip(_u8L("#2 moves until its edge on the side you choose\nlines up with the same edge of #1."));

        const float bs = 2.5f * u;
        const std::string how = m_flush_mode ? _u8L("Lines up this edge of #2 with #1's.") : _u8L("Puts #2 on this side of #1, touching it.");
        struct Cell { const char* id; int dx, dy; const char* axis; ImU32 col; Op op; };
        const Cell cells[9] = {
            { nullptr, 9, 9, "", 0, Op::None },             { "##as_yp", 0, -1, "Y+", kAxisY, Op::YPos },  { nullptr, 0, 0, "", 0, Op::None },
            { "##as_xn", -1, 0, "X-", kAxisX, Op::XNeg },   { "##as_c", 0, 0, "", 0, Op::CenterXY },       { "##as_xp", +1, 0, "X+", kAxisX, Op::XPos },
            { nullptr, 0, 0, "", 0, Op::None },             { "##as_yn", 0, +1, "Y-", kAxisY, Op::YNeg },  { nullptr, 0, 0, "", 0, Op::None },
        };
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (int k = 0; k < 9; ++k) {
            if (k % 3 != 0) ImGui::SameLine();
            const Cell& c = cells[k];
            if (c.id == nullptr) { if (c.dx == 9) pad_compass(bs); else ImGui::Dummy(ImVec2(bs, bs)); continue; }
            bool hov = false;
            if (pad_button(c.id, bs, c.dx, c.dy, c.col, hov)) apply_op(c.op);
            if (hov) {
                hovered = c.op;
                ImGui::SetTooltip("%s", (c.op == Op::CenterXY ? _u8L("Centres #2 on #1, seen from above.")
                                                               : std::string(c.axis) + "  ·  " + how).c_str());
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndGroup();

        ImGui::SameLine(0.0f, 0.8f * u);
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        const float cw = ImGui::GetContentRegionAvail().x;
        struct Up { const char* id; std::string label; int kind; Op op; std::string tip; };
        const Up ups[3] = {
            { "##as_top", m_flush_mode ? _u8L("Align tops") : _u8L("On top"), 0, Op::OnTop,
              m_flush_mode ? _u8L("#2 moves up or down until its top\nis level with #1's.") : _u8L("#2 goes up until it rests on top of #1,\nleaving the Z gap.") },
            { "##as_cz", _u8L("Centre in height"), 1, Op::CenterZ, _u8L("Centres #2 on #1 in height.") },
            { "##as_bed", _u8L("To bed"), 2, Op::Bed, _u8L("#2 drops to the bed.") },
        };
        for (const Up& up : ups) {
            bool hov = false;
            if (height_button(up.id, up.label, up.kind, cw, bs, hov)) apply_op(up.op);
            if (hov) { hovered = up.op; ImGui::SetTooltip("%s", up.tip.c_str()); }
        }
        ImGui::PopStyleVar();
        ImGui::EndGroup();

        dim_label(_u8L("Z gap"));
        tip(_u8L("Air left between the two parts when one\nis put on top of the other, in mm."));
        ImGui::SameLine(0.0f, 0.5f * u);
        ImGui::SetNextItemWidth(4.6f * u);
        ImGui::InputFloat("##as_zgap", &m_epsilon_mm, 0.0f, 0.0f, "%.3f mm");
        if (m_epsilon_mm < 0.f)   m_epsilon_mm = 0.f;
        if (m_epsilon_mm > 0.1f)  m_epsilon_mm = 0.1f;
    }
    m_imgui->disabled_end();

    // --- A face of #1: drop #2 on it, or extrude from it ------------------------------
    neo_section_g(_u8L("Face of #1").c_str(), Glyph::Target,
                  m_has_picked_face ? (std::string("Z ") + format_mm(m_picked_face_world_z)).c_str() : nullptr);
    m_imgui->disabled_begin(n < 1);
    if (neo_glyph_toggle_row("##as_pick", Glyph::Pick, _u8L("Pick a face").c_str(), &m_face_pick_mode,
                             _u8L("Then click a face of #1 in the scene.\nA flat face lights up whole.").c_str())) {
        if (m_face_pick_mode) {
            m_has_picked_face = false;
            ensure_face_raycaster_for_A();
        } else {
            clear_face_pick();
        }
        // #1's tint is suppressed while picking — refresh so it shows real
        // color on entry and regains its order color on exit.
        refresh_highlight();
    }
    m_imgui->disabled_end();
    if (m_has_picked_face) {
        m_imgui->disabled_begin(!two);
        if (neo_text_button(_u8L("Drop #2 on it").c_str(), NeoBtn::Normal, true, Glyph::Landing))
            apply_op(Op::Face);
        if (ImGui::IsItemHovered()) { hovered = Op::Face; ImGui::SetTooltip("%s", _u8L("#2 goes up or down until it rests\nat the height of that face.").c_str()); }
        m_imgui->disabled_end();
    } else if (m_face_pick_mode)
        hint(_u8L("Click a face of #1 in the scene."));

    // --- Extrude from that face (NEOTKO_CONSTRUCT_TAG s363) ---------------------------
    neo_section_g(_u8L("Extrude from the face").c_str(), Glyph::Pillar);
    if (!m_has_picked_face) {
        hint(_u8L("Pick a flat face first."));
    } else {
        // s367 — the face itself, or only its holes (filled): e.g. the circle of a screw, to carve
        // a bigger hole with a boolean.
        {
            const float sw = 4.2f * u;
            if (seg_button("##as_face", _u8L("Face"), !m_construct_holes, sw)) m_construct_holes = false;
            tip(_u8L("Extrudes the face as it is."));
            ImGui::SameLine(0.0f, 0.2f * u);
            if (seg_button("##as_holes", _u8L("Holes"), m_construct_holes, sw)) m_construct_holes = true;
            tip(_u8L("Extrudes only the holes of the face, filled.\nPick the flat face around a screw hole and\nraise Offset to make a bigger hole with\na Difference."));
        }

        dim_label(_u8L("Height"));
        tip(_u8L("How thick the new solid is, in mm.\nIt grows along the face's own direction."));
        ImGui::SameLine(0.0f, 0.5f * u);
        ImGui::SetNextItemWidth(4.6f * u);
        ImGui::InputFloat("##as_h", &m_construct_height, 0.0f, 0.0f, "%.3f");
        if (m_construct_height < 0.01f)  m_construct_height = 0.01f;
        if (m_construct_height > 100.f)  m_construct_height = 100.f;
        ImGui::SameLine(0.0f, 1.0f * u);
        dim_label(_u8L("Offset"));
        tip(_u8L("Grows the outline of the face, in mm.\nNegative values shrink it."));
        ImGui::SameLine(0.0f, 0.5f * u);
        ImGui::SetNextItemWidth(4.6f * u);
        ImGui::InputFloat("##as_off", &m_construct_offset, 0.0f, 0.0f, "%.3f");
        if (m_construct_offset < -100.f) m_construct_offset = -100.f;
        if (m_construct_offset > 100.f)  m_construct_offset = 100.f;

        // s367 — both ways: half outwards, half into the object, so it goes through without moving it.
        neo_glyph_toggle_row("##as_both", Glyph::Pillar, _u8L("Both directions").c_str(), &m_construct_both,
                             _u8L("Off: the solid grows out of the face.\nOn: half the height grows out and half\ngoes into the object, so it passes through\nthe face (handy for booleans).").c_str());

        // s363c — hidden: only in debug mode, not documented anywhere on purpose.
        if (NeoDebug::console_enabled()) {
            if (neo_text_button("~##construct_curve_on", m_curve_on ? NeoBtn::Accent : NeoBtn::Ghost))
                m_curve_on = !m_curve_on;
            if (m_curve_on)
                render_curve_editor();
        }

        const float sw = 4.2f * u;
        if (seg_button("##as_part", _u8L("Part"), !m_construct_as_object, sw)) m_construct_as_object = false;
        tip(_u8L("A new part of #1's object, in place.\nIt shares that object's layer height."));
        ImGui::SameLine(0.0f, 0.2f * u);
        if (seg_button("##as_obj", _u8L("Object"), m_construct_as_object, sw)) m_construct_as_object = true;
        tip(_u8L("A separate object sitting on the face,\nwith its own layer height."));

        update_construct_ghost();
        const bool can_create = m_construct_block_reason.empty();
        const float bw = 5.2f * u;
        neo_same_line_at(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - bw);
        m_imgui->disabled_begin(!can_create);
        if (neo_text_button(_u8L("Create").c_str(), NeoBtn::Accent, true, Glyph::Plus, bw))
            apply_construct();
        m_imgui->disabled_end();
        if (!can_create)
            neo_warn_row("##as_why", m_construct_block_reason.c_str(), nullptr);
    }

    // --- Boolean (NEOTKO_CONSTRUCT_TAG s363b) ----------------------------------------
    neo_section_g(_u8L("Boolean: #1 with #2").c_str(), Glyph::Mix);
    m_imgui->disabled_begin(!two);
    {
        const float gap = 0.35f * u;
        const float bw  = (ImGui::GetContentRegionAvail().x - 2.f * gap) / 3.f;
        if (neo_text_button(_u8L("Union").c_str(), NeoBtn::Normal, false, Glyph::Plus, bw))
            apply_boolean("UNION", "union");
        tip(_u8L("#1 and #2 fused into one solid."));
        ImGui::SameLine(0.0f, gap);
        if (neo_text_button(_u8L("Difference").c_str(), NeoBtn::Normal, false, Glyph::Plus, bw))
            apply_boolean("A_NOT_B", "difference");
        tip(_u8L("#1 with the shape of #2 carved out of it."));
        ImGui::SameLine(0.0f, gap);
        if (neo_text_button(_u8L("Intersection").c_str(), NeoBtn::Normal, false, Glyph::Plus, bw))
            apply_boolean("INTERSECTION", "intersection");
        tip(_u8L("Only the volume that #1 and #2 share."));
        neo_glyph_toggle_row("##as_keep", Glyph::Copy, _u8L("Keep #1 and #2").c_str(), &m_bool_keep_inputs,
                             _u8L("Off: the result replaces both objects.\nOn: the result is added and both stay.").c_str());
    }
    m_imgui->disabled_end();
    if (!m_bool_error.empty())
        neo_warn_row("##as_boolerr", m_bool_error.c_str(), nullptr);

    // The ghost follows the button under the mouse: tell the scene when that changes.
    if (hovered != m_hover_op) {
        m_hover_op = hovered;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
    }
}

}} // namespace Slic3r::GUI
// NEOTKO_ALIGNSTACK_TAG_END
