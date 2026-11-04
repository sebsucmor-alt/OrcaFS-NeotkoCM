#include "GLGizmoMmuSegmentation.hpp"
#include <sstream>
#include "GLGizmoMeasure.hpp"   // TransformHelper::world_to_ss

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/MixedColorMatchHelpers.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PaintClay.hpp"
#include "libslic3r/Tesselate.hpp"         // erased strokes on screen
#include "libslic3r/NSVGUtils.hpp"   // 2b: Merge reads each SVG
#include "libslic3r/SvgPaint.hpp"
#include <wx/msgdlg.h>

#include <thread>
#include "libslic3r/MeshBoolean.hpp"     // Apply engrave
#include "libslic3r/PaintTransfer.hpp"
#include <wx/utils.h>
#include "slic3r/Utils/UndoRedo.hpp"


#include <GL/glew.h>
#include <wx/filedlg.h>
#include <algorithm>
#include <cmath>
#include "libslic3r/SLA/Hollowing.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {

static inline void show_notification_extruders_limit_exceeded()
{
    wxGetApp()
        .plater()
        ->get_notification_manager()
        ->push_notification(NotificationType::MmSegmentationExceededExtrudersLimit, NotificationManager::NotificationLevel::PrintInfoNotificationLevel,
                            GUI::format(_L("Filament count exceeds the maximum number that painting tool supports. Only the "
                                           "first %1% filaments will be available in painting tool."), GLGizmoMmuSegmentation::EXTRUDERS_LIMIT));
}

// U1: the Vector / Clay panels laid out as rows "label | control"
// of fixed widths, in font units, so the auto-resized panel never chases its own width (it once grew
// frame after frame).
namespace neo_ui {
// The panel's width is taken once at the top of each panel: the room the window already has (the
// filament palette sets it), never less than 22 letters. The rows fill exactly that, so they can't
// push the auto-resized window wider.
static float g_panel_w = 0.f;
static float panel_w() { return g_panel_w > 1.f ? g_panel_w : ImGui::GetFontSize() * 22.f; }
// Tighter spacing inside our panels (the orange headers and Orca's padding made it tall).
struct Compact
{
    Compact()
    {
        g_panel_w = std::max(ImGui::GetFontSize() * 22.f, ImGui::GetContentRegionAvail().x);
        const ImGuiStyle &st = ImGui::GetStyle();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(st.ItemSpacing.x, st.ItemSpacing.y * 0.6f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(st.FramePadding.x, st.FramePadding.y * 0.7f));
    }
    ~Compact() { ImGui::PopStyleVar(2); }
};
static float label_w() { return ImGui::GetFontSize() * 7.5f; }
static float ctrl_w() { return panel_w() - label_w(); }
// Label on the left; the next item starts at the control column, ctrl_w() wide.
static void row(const std::string &label)
{
    const float x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label.c_str());
    ImGui::SameLine(x0 + label_w());
    ImGui::SetNextItemWidth(ctrl_w());
}
// Buttons side by side filling `w`; the chosen one in the accent colour. Returns true when it changed.
static bool segmented(const char *id, const std::vector<std::string> &opts, int &v, float w = -1.f)
{
    if (w <= 0.f)
        w = ctrl_w();
    const ImGuiStyle &st = ImGui::GetStyle();
    const float bw = (w - st.ItemSpacing.x * float(opts.size() - 1)) / float(std::max<size_t>(opts.size(), 1));
    bool changed = false;
    ImGui::PushID(id);
    for (size_t k = 0; k < opts.size(); ++k) {
        if (k > 0)
            ImGui::SameLine();
        const bool on = int(k) == v;
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.59f, 0.53f, 0.55f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.59f, 0.53f, 0.65f));
        }
        if (ImGui::Button((opts[k] + "##" + std::to_string(k)).c_str(), ImVec2(bw, 0.f)) && !on) {
            v       = int(k);
            changed = true;
        }
        if (on)
            ImGui::PopStyleColor(2);
    }
    ImGui::PopID();
    return changed;
}
// A foldable section; ImGui keeps whether it is open by its label.
// Drawn as one full-width button: ImGui's CollapsingHeader counted a click on its arrow AND on the bar,
// so a press on the arrow toggled twice and nothing happened.
static bool section(const std::string &title, bool open_by_default)
{
    ImGui::Dummy(ImVec2(0.f, ImGui::GetFontSize() * 0.05f));
    ImGuiStorage *st   = ImGui::GetStateStorage();
    const ImGuiID id   = ImGui::GetID(title.c_str());
    bool          open = st->GetInt(id, open_by_default ? 1 : 0) != 0;
    const float   fh   = ImGui::GetFrameHeight();
    const size_t  cut  = title.find("##");
    const std::string label = "      " + (cut == std::string::npos ? title : title.substr(0, cut)) + "###sec_" + std::to_string(id);
    ImGui::PushStyleColor(ImGuiCol_Button,        ImGui::GetStyleColorVec4(ImGuiCol_Header));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f));
    if (ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, fh))) {
        open = !open;
        st->SetInt(id, open ? 1 : 0);
        Slic3r::GUI::wxGetApp().imgui()->set_requires_extra_frame();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    // the arrow: down when open, right when folded
    const ImVec2 mn = ImGui::GetItemRectMin();
    const float  r  = fh * 0.22f;
    const ImVec2 c(mn.x + fh * 0.7f, mn.y + fh * 0.5f);
    const ImU32  col = ImGui::GetColorU32(ImGuiCol_Text);
    if (open)
        ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(c.x - r, c.y - r * 0.6f), ImVec2(c.x + r, c.y - r * 0.6f), ImVec2(c.x, c.y + r * 0.8f), col);
    else
        ImGui::GetWindowDrawList()->AddTriangleFilled(ImVec2(c.x - r * 0.6f, c.y - r), ImVec2(c.x - r * 0.6f, c.y + r), ImVec2(c.x + r * 0.8f, c.y), col);
    return open;
}
// Small line icons drawn with ImGui's draw list (no texture files to ship).
enum class Icon { Eye, EyeOff, Lock, Unlock, Move, Rebuild, Edit, Apply, Cancel, Paint, MirrorX, MirrorY, MirrorZ, Freeze, Close };
static void draw_icon(ImDrawList *dl, Icon k, ImVec2 c, float r, ImU32 col)
{
    const float t = std::max(1.5f, r * 0.16f);
    switch (k) {
    case Icon::Eye:
    case Icon::EyeOff: {
        ImVec2 up[13], dn[13];
        for (int i = 0; i <= 12; ++i) {
            const float x = -1.f + i / 6.f, y = 0.55f * (1.f - x * x);
            up[i] = ImVec2(c.x + x * r, c.y - y * r);
            dn[i] = ImVec2(c.x + x * r, c.y + y * r);
        }
        dl->AddPolyline(up, 13, col, 0, t);
        dl->AddPolyline(dn, 13, col, 0, t);
        if (k == Icon::Eye)
            dl->AddCircleFilled(c, r * 0.3f, col);
        else
            dl->AddLine(ImVec2(c.x - r * 0.8f, c.y + r * 0.8f), ImVec2(c.x + r * 0.8f, c.y - r * 0.8f), col, t);
        break;
    }
    case Icon::Lock:
    case Icon::Unlock: {
        dl->AddRectFilled(ImVec2(c.x - r * 0.7f, c.y - r * 0.1f), ImVec2(c.x + r * 0.7f, c.y + r * 0.85f), col, r * 0.15f);
        const float sx = k == Icon::Lock ? 0.f : r * 0.45f;
        dl->PathArcTo(ImVec2(c.x + sx, c.y - r * 0.35f), r * 0.42f, 3.14159265f, 2.f * 3.14159265f, 10);
        dl->PathStroke(col, 0, t);
        dl->AddLine(ImVec2(c.x - r * 0.42f + sx, c.y - r * 0.35f), ImVec2(c.x - r * 0.42f + sx, c.y - r * 0.1f), col, t);
        if (k == Icon::Lock)
            dl->AddLine(ImVec2(c.x + r * 0.42f, c.y - r * 0.35f), ImVec2(c.x + r * 0.42f, c.y - r * 0.1f), col, t);
        break;
    }
    case Icon::Move: {
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, t);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, t);
        const float h = r * 0.35f;
        for (int q = 0; q < 4; ++q) {
            const float ux = q == 0 ? 1.f : q == 1 ? -1.f : 0.f, uy = q == 2 ? 1.f : q == 3 ? -1.f : 0.f;
            const ImVec2 tip(c.x + ux * r, c.y + uy * r);
            dl->AddTriangleFilled(ImVec2(tip.x + ux * h * 0.4f, tip.y + uy * h * 0.4f),
                                  ImVec2(tip.x - ux * h - uy * h, tip.y - uy * h + ux * h),
                                  ImVec2(tip.x - ux * h + uy * h, tip.y - uy * h - ux * h), col);
        }
        break;
    }
    case Icon::Rebuild: {
        dl->PathArcTo(c, r * 0.75f, -3.14159265f * 0.35f, 3.14159265f * 1.35f, 18);
        dl->PathStroke(col, 0, t);
        const float a = -3.14159265f * 0.35f;
        const ImVec2 e(c.x + std::cos(a) * r * 0.75f, c.y + std::sin(a) * r * 0.75f);
        dl->AddTriangleFilled(ImVec2(e.x - r * 0.45f, e.y - r * 0.15f), ImVec2(e.x + r * 0.3f, e.y - r * 0.35f), ImVec2(e.x + r * 0.05f, e.y + r * 0.4f), col);
        break;
    }
    case Icon::Edit: {
        const ImVec2 a(c.x + r * 0.7f, c.y - r * 0.7f), b(c.x - r * 0.5f, c.y + r * 0.5f);
        dl->AddLine(a, b, col, t * 1.8f);
        dl->AddTriangleFilled(ImVec2(b.x - r * 0.25f, b.y + r * 0.25f), ImVec2(b.x - r * 0.1f, b.y - r * 0.2f), ImVec2(b.x + r * 0.2f, b.y + r * 0.1f), col);
        break;
    }
    case Icon::Freeze: {   // a snowflake: three crossed lines with small ticks
        for (int k = 0; k < 3; ++k) {
            const float a = 3.14159265f * (0.5f + k / 3.f), ca = std::cos(a) * r * 0.85f, sa = std::sin(a) * r * 0.85f;
            dl->AddLine(ImVec2(c.x - ca, c.y - sa), ImVec2(c.x + ca, c.y + sa), col, t);
            for (float e : { -1.f, 1.f }) {
                const ImVec2 tip(c.x + e * ca * 0.6f, c.y + e * sa * 0.6f);
                const float  b = a + 0.6f, b2 = a - 0.6f;
                dl->AddLine(tip, ImVec2(tip.x + e * std::cos(b) * r * 0.3f, tip.y + e * std::sin(b) * r * 0.3f), col, t * 0.8f);
                dl->AddLine(tip, ImVec2(tip.x + e * std::cos(b2) * r * 0.3f, tip.y + e * std::sin(b2) * r * 0.3f), col, t * 0.8f);
            }
        }
        break;
    }
    case Icon::Paint: {   // a drop of paint
        ImVec2 pts[20];
        for (int i = 0; i < 20; ++i) {
            const float a = 3.14159265f * (-0.5f + 2.f * i / 19.f);
            const float k = std::abs(std::sin(0.5f * (a + 3.14159265f * 0.5f)));
            pts[i] = ImVec2(c.x + std::cos(a) * r * 0.62f * k, c.y + r * 0.25f + std::sin(a) * r * 0.62f - (1.f - k) * r * 0.55f);
        }
        dl->AddConvexPolyFilled(pts, 20, col);
        break;
    }
    case Icon::Cancel: {
        const float e = r * 0.6f;
        dl->AddLine(ImVec2(c.x - e, c.y - e), ImVec2(c.x + e, c.y + e), col, t * 1.4f);
        dl->AddLine(ImVec2(c.x - e, c.y + e), ImVec2(c.x + e, c.y - e), col, t * 1.4f);
        break;
    }
    case Icon::MirrorX:     // two halves facing each other across the plane: ◁ ┆ ▷
    case Icon::MirrorY:     // the same seen at an angle (front / back)
    case Icon::MirrorZ: {   // up / down
        // axis u runs across the plane; v along it
        const float ux = k == Icon::MirrorX ? 1.f : k == Icon::MirrorY ? 0.7071f : 0.f;
        const float uy = k == Icon::MirrorX ? 0.f : k == Icon::MirrorY ? -0.7071f : 1.f;
        const float vx = -uy, vy = ux;
        for (int i = -3; i <= 2; i += 2)   // dashed plane
            dl->AddLine(ImVec2(c.x + vx * r * i / 3.f, c.y + vy * r * i / 3.f), ImVec2(c.x + vx * r * (i + 1) / 3.f, c.y + vy * r * (i + 1) / 3.f), col, t * 0.8f);
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const float a = 0.25f * r * sgn, b = 0.95f * r * sgn, h = 0.55f * r;
            dl->AddTriangleFilled(ImVec2(c.x + ux * a + vx * h, c.y + uy * a + vy * h), ImVec2(c.x + ux * a - vx * h, c.y + uy * a - vy * h),
                                  ImVec2(c.x + ux * b, c.y + uy * b), col);
        }
        break;
    }
    case Icon::Apply: {
        ImVec2 pts[3] = { ImVec2(c.x - r * 0.75f, c.y), ImVec2(c.x - r * 0.2f, c.y + r * 0.6f), ImVec2(c.x + r * 0.8f, c.y - r * 0.6f) };
        dl->AddPolyline(pts, 3, col, 0, t * 1.4f);
        break;
    }
    case Icon::Close: {   // a ring with a gap and an arrow closing it: the last point joins the first
        const float rr = r * 0.72f, a0 = -3.14159265f * 0.25f, a1 = a0 + 3.14159265f * 1.6f;
        dl->PathArcTo(c, rr, a0, a1, 24);
        dl->PathStroke(col, 0, t * 1.2f);
        const ImVec2 tip(c.x + std::cos(a1) * rr, c.y + std::sin(a1) * rr);
        const ImVec2 dir(-std::sin(a1), std::cos(a1));   // along the arc
        const ImVec2 nor(std::cos(a1), std::sin(a1));
        const float  h = r * 0.42f;
        ImVec2 head[3] = { ImVec2(tip.x + dir.x * h, tip.y + dir.y * h), ImVec2(tip.x + nor.x * h * 0.6f, tip.y + nor.y * h * 0.6f),
                           ImVec2(tip.x - nor.x * h * 0.6f, tip.y - nor.y * h * 0.6f) };
        dl->AddTriangleFilled(head[0], head[1], head[2], col);
        dl->AddCircleFilled(ImVec2(c.x + std::cos(a0) * rr, c.y + std::sin(a0) * rr), t * 1.1f, col);
        break;
    }
    }
}
// A square button of frame height with an icon. `on` paints it in the accent colour.
static bool icon_button(const char *id, Icon k, bool on = false, float size = 0.f)
{
    if (size <= 0.f)
        size = ImGui::GetFrameHeight();
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.59f, 0.53f, 0.55f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.59f, 0.53f, 0.65f));
    }
    const bool   pressed = ImGui::Button(id, ImVec2(size, size));
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    if (on)
        ImGui::PopStyleColor(2);
    draw_icon(ImGui::GetWindowDrawList(), k, ImVec2((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f), size * 0.32f, ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}
// A text button with its icon in front (Close / Finish / Cancel of Draw shape and Draw on surface).
static bool icon_text_button(const std::string &label, Icon k)
{
    const float fh = ImGui::GetFrameHeight();
    const bool  pressed = ImGui::Button(("      " + label).c_str());
    const ImVec2 mn = ImGui::GetItemRectMin(), mx = ImGui::GetItemRectMax();
    draw_icon(ImGui::GetWindowDrawList(), k, ImVec2(mn.x + fh * 0.55f, (mn.y + mx.y) * 0.5f), fh * 0.28f, ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}
// The three mirror planes as icon toggles; returns the axis clicked (0 X, 1 Y, 2 Z) or -1.
static int mirror_toggles(const char *id, int mask, const std::string &tip)
{
    static const Icon icons[3] = { Icon::MirrorX, Icon::MirrorY, Icon::MirrorZ };
    int clicked = -1;
    for (int axis = 0; axis < 3; ++axis) {
        if (axis > 0)
            ImGui::SameLine();
        const std::string bid = std::string("##") + id + std::to_string(axis);
        if (icon_button(bid.c_str(), icons[axis], (mask & (1 << axis)) != 0))
            clicked = axis;
        if (ImGui::IsItemHovered()) {
            const std::string name = axis == 0 ? _u8L("Left/Right") : axis == 1 ? _u8L("Front/Back") : _u8L("Top/Bottom");
            ImGui::SetTooltip("%s", (name + "\n" + tip).c_str());
        }
    }
    return clicked;
}
} // namespace neo_ui

// --- Gradient rendering helpers (ported from MixedFilamentBadge) ---

static ImU32 interpolate_ImU32(ImU32 c0, ImU32 c1, double t)
{
    t = std::max(0.0, std::min(1.0, t));
    int r = int(((c0 >> IM_COL32_R_SHIFT) & 0xFF) * (1.0 - t) + ((c1 >> IM_COL32_R_SHIFT) & 0xFF) * t);
    int g = int(((c0 >> IM_COL32_G_SHIFT) & 0xFF) * (1.0 - t) + ((c1 >> IM_COL32_G_SHIFT) & 0xFF) * t);
    int b = int(((c0 >> IM_COL32_B_SHIFT) & 0xFF) * (1.0 - t) + ((c1 >> IM_COL32_B_SHIFT) & 0xFF) * t);
    return IM_COL32(r, g, b, 255);
}

static ImU32 interpolate_ImU32(const std::vector<ImU32>& colors, double pos)
{
    if (colors.empty())
        return IM_COL32(0x26, 0xA6, 0x9A, 255);
    if (colors.size() == 1)
        return colors.front();

    pos = std::clamp(pos, 0.0, 1.0);
    const double scaled = pos * double(colors.size() - 1);
    const size_t segment = std::min<size_t>(colors.size() - 2, size_t(std::floor(scaled)));
    const double local = scaled - double(segment);
    return interpolate_ImU32(colors[segment], colors[segment + 1], local);
}

static float bT601_luminance(ImU32 c)
{
    float r = ((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
    float g = ((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
    float b = ((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

static bool very_light(ImU32 c)
{
    return ((c >> IM_COL32_R_SHIFT) & 0xFF) > 224
        && ((c >> IM_COL32_G_SHIFT) & 0xFF) > 224
        && ((c >> IM_COL32_B_SHIFT) & 0xFF) > 224;
}

static bool is_gradient_definition(const MixedFilamentDefinition& definition)
{
    return definition.recipe.kind == MixedFilamentRecipeKind::WeightedBlend &&
           definition.behavior.gradient.enabled &&
           definition.recipe.blend.components.size() >= 2;
}

static std::vector<ImU32> mixed_filament_gradient_stops_ImU32(const MixedFilamentDefinition& definition,
                                                              const MixedFilamentDisplayContext& ctx)
{
    const std::vector<wxColour> preview_colors =
        build_mixed_filament_gradient_preview(definition, ctx).sampled_colors;

    std::vector<ImU32> colors;
    colors.reserve(preview_colors.size());
    for (const wxColour& color : preview_colors)
        colors.emplace_back(IM_COL32(color.Red(), color.Green(), color.Blue(), 255));
    return colors;
}

void GLGizmoMmuSegmentation::on_opening()
{
    if (get_extruders_colors().size() > GLGizmoMmuSegmentation::EXTRUDERS_LIMIT)
        show_notification_extruders_limit_exceeded();
}

void GLGizmoMmuSegmentation::on_shutdown()
{
    clay_cancel();
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

std::string GLGizmoMmuSegmentation::on_get_name() const
{
    return _u8L("Color Painting");
}

bool GLGizmoMmuSegmentation::on_is_selectable() const
{
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF
            && /*wxGetApp().get_mode() != comSimple && */wxGetApp().filaments_cnt() > 1);
}

bool GLGizmoMmuSegmentation::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    return !selection.is_empty() && (selection.is_single_full_instance() || selection.is_any_volume()) && wxGetApp().filaments_cnt() > 1;
}

//BBS: use the global one in 3DScene.cpp
/*static std::vector<ColorRGBA> get_extruders_colors()
{
    unsigned char                     rgb_color[3] = {};
    std::vector<std::string>          colors       = Slic3r::GUI::wxGetApp().plater()->get_extruder_colors_from_plater_config();
    std::vector<ColorRGBA> colors_out(colors.size());
    for (const std::string &color : colors) {
        Slic3r::GUI::BitmapCache::parse_color(color, rgb_color);
        size_t color_idx      = &color - &colors.front();
        colors_out[color_idx] = {float(rgb_color[0]) / 255.f, float(rgb_color[1]) / 255.f, float(rgb_color[2]) / 255.f, 1.f};
    }

    return colors_out;
}*/

static std::vector<int> get_extruder_id_for_volumes(const ModelObject &model_object)
{
    std::vector<int> extruders_idx;
    extruders_idx.reserve(model_object.volumes.size());
    for (const ModelVolume *model_volume : model_object.volumes) {
        if (!model_volume->is_model_part())
            continue;

        extruders_idx.emplace_back(model_volume->extruder_id());
    }

    return extruders_idx;
}

static std::vector<unsigned int> get_display_filament_ids(size_t total_filaments)
{
    std::vector<unsigned int> ordered_filament_ids;
    if (wxGetApp().plater() != nullptr)
        ordered_filament_ids = wxGetApp().plater()->sidebar().get_ui_ordered_filament_ids();

    std::vector<unsigned int> sanitized_filament_ids;
    sanitized_filament_ids.reserve(total_filaments);
    std::vector<bool> used_filament_ids(total_filaments + 1, false);
    for (const unsigned int filament_id : ordered_filament_ids) {
        if (filament_id == 0 || filament_id > total_filaments || used_filament_ids[filament_id])
            continue;
        used_filament_ids[filament_id] = true;
        sanitized_filament_ids.emplace_back(filament_id);
    }

    for (unsigned int filament_id = 1; filament_id <= total_filaments; ++filament_id) {
        if (!used_filament_ids[filament_id])
            sanitized_filament_ids.emplace_back(filament_id);
    }

    return sanitized_filament_ids;
}

void GLGizmoMmuSegmentation::init_extruders_data(const std::vector<ColorRGBA> &extruder_colors)
{
    const unsigned int old_selected_filament_id =
        m_selected_extruder_idx < m_display_filament_ids.size() ? m_display_filament_ids[m_selected_extruder_idx] :
        (m_selected_extruder_idx < m_extruders_colors.size() ? unsigned(m_selected_extruder_idx + 1) : 0);

    m_extruders_colors = extruder_colors;
    m_display_filament_ids = get_display_filament_ids(m_extruders_colors.size());

    m_selected_extruder_idx = 0;
    if (!m_display_filament_ids.empty()) {
        auto selected_it = std::find(m_display_filament_ids.begin(), m_display_filament_ids.end(), old_selected_filament_id);
        if (selected_it != m_display_filament_ids.end())
            m_selected_extruder_idx = size_t(std::distance(m_display_filament_ids.begin(), selected_it));
    }

    // keep remap table consistent with current extruder count
    m_extruder_remap.resize(m_display_filament_ids.size());
    for (size_t i = 0; i < m_extruder_remap.size(); ++i)
        m_extruder_remap[i] = i;

    // Keep gradient rendering on the same color engine, TDs, and preview settings as the mixed-filament UI.
    std::vector<std::string> physical_hex = wxGetApp().plater()->get_extruder_colors_from_plater_config(nullptr, false);
    m_mixed_display_context = build_mixed_filament_display_context(physical_hex);
}

void GLGizmoMmuSegmentation::init_extruders_data()
{
    init_extruders_data(get_extruders_colors());
}

bool GLGizmoMmuSegmentation::on_init()
{
    // BBS
    m_shortcut_key = WXK_CONTROL_N;

    // FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
    const wxString ctrl  = _L("Ctrl+");
    // FIXME: maybe should be using GUI::shortkey_alt_prefix() or equivalent?
    const wxString alt   = _L("Alt+");
    const wxString shift = _L("Shift+");

    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"]     = _L("Section view");
    m_desc["reset_direction"]     = _L("Reset direction");
    m_desc["cursor_size_caption"]  = ctrl + _L("Mouse wheel");
    m_desc["cursor_size"]          = _L("Pen size");
    m_desc["cursor_type"]          = _L("Pen shape");

    m_desc["paint_caption"]        = _L("Left mouse button");
    m_desc["paint"]                = _L("Paint");
    m_desc["erase_caption"]        = shift + _L("Left mouse button");
    m_desc["erase"]                = _L("Erase");
    m_desc["shortcut_key_caption"] = _L("Key 1~9");
    m_desc["shortcut_key"]         = _L("Choose filament");
    m_desc["edge_detection"]       = _L("Edge detection");
    m_desc["gap_area_caption"]     = ctrl + _L("Mouse wheel");
    m_desc["gap_area"]             = _L("Gap area");
    m_desc["perform"]              = _L("Perform");

    m_desc["remove_all"]           = _L("Erase all painting");
    m_desc["circle"]               = _L("Circle");
    m_desc["sphere"]               = _L("Sphere");
    m_desc["pointer"]              = _L("Triangles");

    m_desc["filaments"]            = _L("Filaments");
    m_desc["tool_type"]            = _L("Tool type");
    m_desc["tool_brush"]           = _L("Brush");
    m_desc["tool_smart_fill"]      = _L("Smart fill");
    m_desc["tool_bucket_fill"]     = _L("Bucket fill");

    m_desc["smart_fill_angle_caption"] = ctrl + _L("Mouse wheel");
    m_desc["smart_fill_angle"]     = _L("Smart fill angle");

    m_desc["height_range_caption"] = ctrl + _L("Mouse wheel");
    m_desc["height_range"]         = _L("Height range");

    //add toggle wire frame hint
    m_desc["toggle_wireframe_caption"]        = alt + shift + _L("Enter");
    m_desc["toggle_wireframe"]                = _L("Toggle Wireframe");

    // Filament remapping descriptions
    m_desc["perform_remap"]                   = _L("Remap filaments");
    m_desc["remap"]                           = _L("Remap");
    m_desc["cancel_remap"]                    = _L("Cancel");

    init_extruders_data();

    return true;
}

GLGizmoMmuSegmentation::GLGizmoMmuSegmentation(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{
}

void GLGizmoMmuSegmentation::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);
    render_clay_xray();
    sf_render_line();

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
    // leave the GL state as the canvas expects it after the gizmo's
    // frame, whatever a preview, a mask overlay or an early return in between changed. The
    // canvas keeps drawing after this call (and after the gizmo closes) assuming these values.
    glsafe(::glFrontFace(GL_CCW));
    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
    glsafe(::glPolygonOffset(0.0f, 0.0f));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glLineWidth(1.0f));
}


void GLGizmoMmuSegmentation::data_changed(bool is_serializing)
{
    GLGizmoPainterBase::data_changed(is_serializing);
    if (m_state != On || wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF || wxGetApp().extruders_edited_cnt() <= 1)
        return;

    ModelObject* model_object = m_c->selection_info()->model_object();
    const std::vector<ColorRGBA> current_extruder_colors = get_extruders_colors();
    const int prev_extruders_count = int(m_extruders_colors.size());
    const int current_extruders_count = int(current_extruder_colors.size());
    const std::vector<unsigned int> current_display_filament_ids = get_display_filament_ids(current_extruder_colors.size());
    if (prev_extruders_count != current_extruders_count) {
        if (current_extruder_colors.size() > GLGizmoMmuSegmentation::EXTRUDERS_LIMIT)
            show_notification_extruders_limit_exceeded();

        this->init_extruders_data(current_extruder_colors);
        // Reinitialize triangle selectors because of change of extruder count need also change the size of GLIndexedVertexArray
        if (prev_extruders_count != current_extruders_count)
            this->init_model_triangle_selectors();
    }
    else if (current_extruder_colors != m_extruders_colors) {
        this->init_extruders_data(current_extruder_colors);
        this->update_triangle_selectors_colors();
    }
    else if (current_display_filament_ids != m_display_filament_ids) {
        this->init_extruders_data(current_extruder_colors);
    }
    else if (model_object != nullptr && get_extruder_id_for_volumes(*model_object) != m_volumes_extruder_idxs) {
        this->init_model_triangle_selectors();
    }
}

// BBS
bool GLGizmoMmuSegmentation::on_number_key_down(int number)
{
    int extruder_idx = number - 1;
    if (extruder_idx >= 0 && size_t(extruder_idx) < m_display_filament_ids.size())
        m_selected_extruder_idx = extruder_idx;

    return true;
}

bool GLGizmoMmuSegmentation::on_key_down_select_tool_type(int keyCode) {
    // B / V switch the panel mode; a brush tool key also goes to Brush.
    // P: Vector Brush; E: brush / eraser while in it.
    if (keyCode == 'P') {
        set_paint_mode(PaintMode::VectorBrush);
        return true;
    }
    if (keyCode == 'E') {
        if (m_paint_mode != PaintMode::VectorBrush)
            return true;   // swallowed: let through, Orca's E leaves the painter for Brim Ears
        if (!m_vb_nodes.empty())
            vb_line_finish();
        m_vb_eraser = !m_vb_eraser;
        m_parent.set_as_dirty();
        return true;
    }
    if (keyCode == WXK_RETURN || keyCode == WXK_NUMPAD_ENTER) {   // Line tool: finish the line
        if (m_sf_tool != 0 && !m_sf_path.clicks.empty())
            return sf_finish() || true;
        if (m_paint_mode != PaintMode::VectorBrush || m_vb_nodes.empty())
            return false;
        vb_line_finish();
        return true;
    }
    if (keyCode == 'B' || keyCode == 'V' || keyCode == 'K') {
        set_paint_mode(keyCode == 'B' ? PaintMode::Brush : keyCode == 'V' ? PaintMode::Vector : PaintMode::Clay);
        return true;
    }
    if (keyCode == 'F' || keyCode == 'T' || keyCode == 'S' || keyCode == 'C' || keyCode == 'H' || keyCode == 'G')
        set_paint_mode(PaintMode::Brush);
    switch (keyCode)
    {
    case 'F':
        m_current_tool = ImGui::FillButtonIcon;
        break;
    case 'T':
        m_current_tool = ImGui::TriangleButtonIcon;
        break;
    case 'S':
        m_current_tool = ImGui::SphereButtonIcon;
        break;
    case 'C':
        m_current_tool = ImGui::CircleButtonIcon;
        break;
    case 'H':
        m_current_tool = ImGui::HeightRangeIcon;
        break;
    case 'G':
        m_current_tool = ImGui::GapFillIcon;
        break;
    default:
        return false;
        break;
    }
    return true;
}

static void render_extruders_combo(const std::string& label,
                                   const std::vector<std::string>& extruders,
                                   const std::vector<ColorRGBA>& extruders_colors,
                                   size_t& selection_idx)
{
    assert(!extruders_colors.empty());
    assert(extruders.size() == extruders_colors.size());

    size_t selection_out = selection_idx;
    // It is necessary to use BeginGroup(). Otherwise, when using SameLine() is called, then other items will be drawn inside the combobox.
    ImGui::BeginGroup();
    ImVec2 combo_pos = ImGui::GetCursorScreenPos();
    if (ImGui::BeginCombo(label.c_str(), "")) {
        for (size_t extruder_idx = 0; extruder_idx < extruders.size(); ++extruder_idx) {
            ImGui::PushID(int(extruder_idx));
            ImVec2 start_position = ImGui::GetCursorScreenPos();

            if (ImGui::Selectable("", extruder_idx == selection_idx))
                selection_out = extruder_idx;

            ImGui::SameLine();
            ImGuiStyle &style  = ImGui::GetStyle();
            float       height = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(start_position, ImVec2(start_position.x + height + height / 2, start_position.y + height), ImGuiWrapper::to_ImU32(extruders_colors[extruder_idx]));
            ImGui::GetWindowDrawList()->AddRect(start_position, ImVec2(start_position.x + height + height / 2, start_position.y + height), IM_COL32_BLACK);

            ImGui::SetCursorScreenPos(ImVec2(start_position.x + height + height / 2 + style.FramePadding.x, start_position.y));
            ImGui::Text("%s", extruders[extruder_idx].c_str());
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    ImVec2      backup_pos = ImGui::GetCursorScreenPos();
    ImGuiStyle &style      = ImGui::GetStyle();

    ImGui::SetCursorScreenPos(ImVec2(combo_pos.x + style.FramePadding.x, combo_pos.y + style.FramePadding.y));
    ImVec2 p      = ImGui::GetCursorScreenPos();
    float  height = ImGui::GetTextLineHeight();

    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + height + height / 2, p.y + height), ImGuiWrapper::to_ImU32(extruders_colors[selection_idx]));
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + height + height / 2, p.y + height), IM_COL32_BLACK);

    ImGui::SetCursorScreenPos(ImVec2(p.x + height + height / 2 + style.FramePadding.x, p.y));
    ImGui::Text("%s", extruders[selection_out].c_str());
    ImGui::SetCursorScreenPos(backup_pos);
    ImGui::EndGroup();

    selection_idx = selection_out;
}

void GLGizmoMmuSegmentation::show_tooltip_information(float caption_max, float x, float y)
{
    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(std::string_view{": "}).x + 15.f;

    float  scale       = m_parent.get_scale();
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0}); // ORCA: Dont add padding
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max](const wxString &caption, const wxString &text) {
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        std::vector<std::string> tip_items;
        switch (m_tool_type) {
            case ToolType::BRUSH: 
                tip_items = {"paint", "erase", "cursor_size", "clipping_of_view", "toggle_wireframe"};
                break;
            case ToolType::BUCKET_FILL: 
                tip_items = {"paint", "erase", "smart_fill_angle", "clipping_of_view", "toggle_wireframe"};
                break;
            case ToolType::SMART_FILL:
                // TODO:
                break;
            case ToolType::GAP_FILL:
                tip_items = {"gap_area", "toggle_wireframe"};
                break;
            default:
                break;
        }
        for (const auto &t : tip_items) draw_text_with_caption(m_desc.at(t + "_caption") + ": ", m_desc.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GLGizmoMmuSegmentation::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info()->model_object()) return;
    clay_watch_decals();

    const float approx_height = m_imgui->scaled(22.0f);
    y = std::min(y, bottom_limit - approx_height);
    // the panel sits against the right edge of the 3D view instead of under
    // its toolbar icon, where it covered the middle of the model; it grows down to bottom_limit at
    // most and scrolls beyond that. The width is last frame's (0 on the very first frame).
    if (last_input_window_width > 1.f)
        x = std::max(0.f, float(m_parent.get_canvas_size().get_width()) - last_input_window_width - m_imgui->scaled(0.5f));
    // Width capped too (the painter panel once grew frame after frame until it covered the 3D view).
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.f, 0.f), ImVec2(m_imgui->scaled(34.f), std::max(approx_height, bottom_limit - y)));
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always);

    wchar_t old_tool = m_current_tool;

    // BBS
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float space_size = m_imgui->get_style_scaling() * 8;
    const float clipping_slider_left  = std::max(m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x + m_imgui->scaled(1.5f),
        m_imgui->calc_text_size(m_desc.at("reset_direction")).x + m_imgui->scaled(1.5f) + ImGui::GetStyle().FramePadding.x * 2);
    const float cursor_slider_left = m_imgui->calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.5f);
    const float smart_fill_slider_left = m_imgui->calc_text_size(m_desc.at("smart_fill_angle")).x + m_imgui->scaled(1.5f);
    const float edge_detect_slider_left = m_imgui->calc_text_size(m_desc.at("edge_detection")).x + m_imgui->scaled(1.f);
    const float gap_area_slider_left = m_imgui->calc_text_size(m_desc.at("gap_area")).x + m_imgui->scaled(1.5f) + space_size;
    const float height_range_slider_left = m_imgui->calc_text_size(m_desc.at("height_range")).x + m_imgui->scaled(2.f);

    const float remove_btn_width = m_imgui->calc_text_size(m_desc.at("remove_all")).x + m_imgui->scaled(1.f);
    const float filter_btn_width = m_imgui->calc_text_size(m_desc.at("perform")).x + m_imgui->scaled(1.f);
    const float remap_btn_width = m_imgui->calc_text_size(m_desc.at("perform_remap")).x + m_imgui->scaled(1.f);
    const float buttons_width = remove_btn_width + filter_btn_width + remap_btn_width + m_imgui->scaled(2.f);
    const float minimal_slider_width = m_imgui->scaled(4.f);
    const float color_button_width = m_imgui->calc_text_size(std::string_view{""}).x + m_imgui->scaled(1.75f);
    const size_t total_filament_count = m_extruders_colors.size();
    const size_t num_physical = m_mixed_display_context.num_physical;
    const float  font_size = ImGui::GetFontSize() * 0.85f;
    const std::string max_filament_label = (total_filament_count > num_physical) ? "WW" : std::to_string(std::max<size_t>(total_filament_count, 1));
    const ImVec2 max_filament_label_size = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, max_filament_label.c_str());

    float caption_max = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 6>{"paint", "erase", "cursor_size", "smart_fill_angle", "height_range", "clipping_of_view"}) {
        caption_max = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }
    total_text_max += caption_max + m_imgui->scaled(1.f);
    caption_max += m_imgui->scaled(1.f);

    const float circle_max_width = std::max(clipping_slider_left,cursor_slider_left);
    const float height_max_width = std::max(clipping_slider_left,height_range_slider_left);
    const float sliders_left_width = std::max(smart_fill_slider_left,
                                         std::max(cursor_slider_left, std::max(edge_detect_slider_left, std::max(gap_area_slider_left, std::max(height_range_slider_left,
                                                                                                                                              clipping_slider_left))))) + space_size;
    const float slider_icon_width = m_imgui->get_slider_icon_size().x;
    float window_width = minimal_slider_width + sliders_left_width + slider_icon_width;
    const int max_filament_items_per_line = 8;
    const float empty_button_width = m_imgui->calc_button_size("").x;
    const float filament_button_h = std::round(ImGui::GetFrameHeight() + m_imgui->scaled(0.2f));
    const float filament_button_w = filament_button_h;
    const float filament_item_spacing = m_imgui->scaled(0.75f);
    const float filament_item_width = filament_button_w + filament_item_spacing;

    window_width = std::max(window_width, total_text_max);
    window_width = std::max(window_width, buttons_width);
    window_width = std::max(window_width, max_filament_items_per_line * filament_item_width + m_imgui->scaled(0.5f));

    const float sliders_width = m_imgui->scaled(7.0f);
    const float drag_left_width = ImGui::GetStyle().WindowPadding.x + sliders_width - space_size;

    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;
    ImDrawList * draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    static float color_button_high  = 25.0;
    draw_list->AddRectFilled({pos.x - 10.0f, pos.y - 7.0f}, {pos.x + window_width + ImGui::GetFrameHeight(), pos.y + color_button_high}, ImGui::GetColorU32(ImGuiCol_FrameBgActive, 1.0f), 5.0f);

    float color_button = ImGui::GetCursorPos().y;

    m_imgui->text(m_desc.at("filaments"));

    float start_pos_x = ImGui::GetCursorPos().x;
    size_t n_extruder_colors = std::min(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT, m_display_filament_ids.size());
    const bool separate_mixed_row = (n_extruder_colors > 10);
    int current_col = 0;
    bool prev_was_physical = false;

    for (size_t extruder_idx = 0; extruder_idx < n_extruder_colors; ++extruder_idx) {
        const unsigned int actual_filament_id = m_display_filament_ids[extruder_idx];
        if (actual_filament_id == 0 || actual_filament_id > m_extruders_colors.size())
            continue;
        const bool is_mixed = (actual_filament_id > num_physical);

        if (separate_mixed_row && is_mixed && prev_was_physical && current_col > 0) {
            current_col = 0;
        }
        prev_was_physical = !is_mixed;

        const ColorRGBA &extruder_color = m_extruders_colors[actual_filament_id - 1];
        ImVec4           color_vec      = ImGuiWrapper::to_ImVec4(extruder_color);
        std::string color_label = std::string("##extruder color ") + std::to_string(extruder_idx);
        std::string item_text = (!is_mixed)
                                    ? std::to_string(actual_filament_id)
                                    : Slic3r::mixed_filament_index_to_letter(actual_filament_id - num_physical - 1);

        const ImVec2 button_size(filament_button_w, filament_button_h);

        if (current_col > 0) {
            float button_offset = start_pos_x + filament_item_width * current_col;
            ImGui::SameLine(button_offset);
        }

        // Multi-component blends also occupy legacy gradient fields, so the typed behavior flag is authoritative here.
        const std::optional<MixedFilamentDefinition> mixed_definition =
            wxGetApp().preset_bundle != nullptr ?
                wxGetApp().preset_bundle->mixed_filaments.mixed_filament_definition_from_id(
                    actual_filament_id, m_mixed_display_context.num_physical) :
                std::nullopt;
        const bool is_gradient = mixed_definition && is_gradient_definition(*mixed_definition);

        if (is_gradient) {
            // --- Gradient path: ColorButton for sizing + interaction, then draw gradient on top ---
            const std::vector<ImU32> gradient_stops =
                mixed_filament_gradient_stops_ImU32(*mixed_definition, m_mixed_display_context);

            int avg_r = 0;
            int avg_g = 0;
            int avg_b = 0;
            for (const ImU32 stop : gradient_stops) {
                avg_r += int((stop >> IM_COL32_R_SHIFT) & 0xFF);
                avg_g += int((stop >> IM_COL32_G_SHIFT) & 0xFF);
                avg_b += int((stop >> IM_COL32_B_SHIFT) & 0xFF);
            }
            const int stop_count = std::max(1, int(gradient_stops.size()));
            avg_r /= stop_count;
            avg_g /= stop_count;
            avg_b /= stop_count;
            ImVec4 avg_color_vec(float(avg_r) / 255.f, float(avg_g) / 255.f, float(avg_b) / 255.f, 1.f);

            // ColorButton provides exact sizing, click detection, border, and cursor advance
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip;
            if (m_selected_extruder_idx != extruder_idx) flags |= ImGuiColorEditFlags_NoBorder;
            #ifdef __APPLE__
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0);
                bool color_picked = ImGui::ColorButton(color_label.c_str(), avg_color_vec, flags, button_size);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(1);
            #else
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0);
                bool color_picked = ImGui::ColorButton(color_label.c_str(), avg_color_vec, flags, button_size);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(1);
            #endif
            color_button_high = ImGui::GetCursorPos().y - color_button - 2.0;
            if (color_picked) { m_selected_extruder_idx = extruder_idx; }

            // Draw gradient slightly inset to match solid color block visual size
            ImVec2 btn_min = ImGui::GetItemRectMin();
            ImVec2 btn_max = ImGui::GetItemRectMax();
            const float inset = 2.5f;
            ImVec2 fill_min(btn_min.x + inset, btn_min.y + inset);
            ImVec2 fill_max(btn_max.x - inset, btn_max.y - inset);
            float fill_h = fill_max.y - fill_min.y;

            // Per-scanline gradient fill (pos=0 bottom, pos=1 top)
            if (fill_h > 0.0f) {
                for (int y_i = 0; y_i < int(std::round(fill_h)); ++y_i) {
                    double pos = 1.0 - double(y_i) / double(fill_h);
                    ImU32 c = interpolate_ImU32(gradient_stops, pos);
                    draw_list->AddRectFilled(
                        ImVec2(fill_min.x, fill_min.y + float(y_i)),
                        ImVec2(fill_max.x, fill_min.y + float(y_i + 1)), c);
                }
            }

            // Very-light gradient border
            const bool all_stops_light = !gradient_stops.empty() &&
                std::all_of(gradient_stops.begin(), gradient_stops.end(), [](ImU32 color) { return very_light(color); });
            if (all_stops_light)
                draw_list->AddRect(fill_min, fill_max, IM_COL32(128, 128, 128, 255), ImGui::GetStyle().FrameRounding, 0, 1.0f);

            // Tooltip
            if (ImGui::IsItemHovered()) {
                if (extruder_idx < 9)
                    m_imgui->tooltip(_L("Shortcut Key ") + std::to_string(extruder_idx + 1), max_tooltip_width);
                else if (actual_filament_id <= num_physical)
                    m_imgui->tooltip(wxString::Format(_L("Filament %d"), int(actual_filament_id)), max_tooltip_width);
                else
                    m_imgui->tooltip(wxString::Format(_L("Mixed Filament %s"), item_text), max_tooltip_width);
            }

            // Number text centered on button
            float lum_avg = 0.0f;
            for (const ImU32 stop : gradient_stops)
                lum_avg += bT601_luminance(stop);
            lum_avg /= float(stop_count);
            ImU32 text_col = (lum_avg < 0.51f) ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
            float text_w = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, item_text.c_str()).x;
            draw_list->AddText(ImGui::GetFont(), font_size,
                ImVec2(btn_min.x + (btn_max.x - btn_min.x - text_w) / 2.f,
                       btn_min.y + (btn_max.y - btn_min.y - font_size) / 2.f),
                text_col, item_text.c_str());

        } else {
            // draw filament background
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip;
            if (m_selected_extruder_idx != extruder_idx) flags |= ImGuiColorEditFlags_NoBorder;
            #ifdef __APPLE__
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA); // ORCA use orca color for selected filament border
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0);
                bool color_picked = ImGui::ColorButton(color_label.c_str(), color_vec, flags, button_size);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(1);
            #else
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA); // ORCA use orca color for selected filament border
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0);
                bool color_picked = ImGui::ColorButton(color_label.c_str(), color_vec, flags, button_size);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(1);
            #endif
            color_button_high = ImGui::GetCursorPos().y - color_button - 2.0;
            if (color_picked) { m_selected_extruder_idx = extruder_idx; }

            if (ImGui::IsItemHovered()) {
                if (extruder_idx < 9)
                    m_imgui->tooltip(_L("Shortcut Key ") + std::to_string(extruder_idx + 1), max_tooltip_width);
                else if (actual_filament_id <= num_physical)
                    m_imgui->tooltip(wxString::Format(_L("Filament %d"), int(actual_filament_id)), max_tooltip_width);
                else
                    m_imgui->tooltip(wxString::Format(_L("Mixed Filament %s"), item_text), max_tooltip_width);
            }

            // draw filament id centered on button
            ImVec2 btn_min = ImGui::GetItemRectMin();
            ImVec2 btn_max = ImGui::GetItemRectMax();
            float gray = 0.299f * extruder_color.r() + 0.587f * extruder_color.g() + 0.114f * extruder_color.b();
            ImU32 text_col = (gray * 255.f < 130.f) ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
            float text_w = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, item_text.c_str()).x;
            draw_list->AddText(ImGui::GetFont(), font_size,
                ImVec2(btn_min.x + (btn_max.x - btn_min.x - text_w) / 2.f,
                       btn_min.y + (btn_max.y - btn_min.y - font_size) / 2.f),
                text_col, item_text.c_str());
        }

        current_col++;
        if (current_col >= max_filament_items_per_line) {
            current_col = 0;
        }
    }
    //ImGui::NewLine();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (n_extruder_colors > 0) {
        int selected_filament = int(m_selected_extruder_idx) + 1;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Selected filament"));
        ImGui::SameLine();
        ImGui::PushItemWidth(m_imgui->scaled(4.5f));
        if (ImGui::InputInt("##selected_filament", &selected_filament, 1, 10, ImGuiInputTextFlags_CharsDecimal)) {
            selected_filament = std::clamp(selected_filament, 1, int(n_extruder_colors));
            m_selected_extruder_idx = size_t(selected_filament - 1);
        }
        ImGui::SameLine();
        m_imgui->text(wxString::Format(_L("/ %d"), int(n_extruder_colors)));
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
    }

    // Brush | Vector. Same filaments for both; each mode shows only its own tools.
    {
        auto mode_button = [this](const wxString &label, PaintMode mode, const wxString &tip) {
            const bool on = m_paint_mode == mode;
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f, 0.59f, 0.53f, 0.35f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.59f, 0.53f, 0.45f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.f, 0.59f, 0.53f, 0.55f));
                ImGui::PushStyleColor(ImGuiCol_Border,        ImGuiWrapper::COL_ORCA);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            if (m_imgui->button(label))
                set_paint_mode(mode);
            if (on) {
                ImGui::PopStyleVar(1);
                ImGui::PopStyleColor(4);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", into_u8(tip).c_str());
        };
        mode_button(_L("Brush"), PaintMode::Brush, _L("Paint by hand: brushes, fill, height range.\nKey: B"));
        ImGui::SameLine();
        mode_button(_L("Vector"), PaintMode::Vector, _L("Shapes and SVG decals, applied in one go.\nKey: V"));
        ImGui::SameLine();
        mode_button(_L("Clay"), PaintMode::Clay, _L("Raise the decals over the surface as parts of their own,\n"
                                                      "with a height and an edge. The object is not deformed.\nKey: K"));
        ImGui::SameLine();
        mode_button(_L("Vector Brush"), PaintMode::VectorBrush, _L("Draw free-hand strokes, smoothed like a drawing app.\n"
                                                                  "The tick saves them as a decal; place, colour or raise it in Vector / Clay.\nKey: P"));
        // Vector mode: a shape starts only from here, so a plain drag keeps orbiting the camera.
        if (m_paint_mode == PaintMode::Vector) {
            const bool armed = m_shape_draw_armed || !m_shape_nodes.empty();
            if (armed) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.59f, 0.53f, 0.35f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORCA);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            m_imgui->disabled_begin(!m_svg_zones.empty() || !m_shape_nodes.empty());
            if (m_imgui->button(into_u8(std::wstring(1, ImGui::NeotkoDrawShapeIcon)) + " " + _u8L("Draw shape"))) {
                m_shape_draw_armed = !m_shape_draw_armed;
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();
            if (armed) {
                ImGui::PopStyleVar(1);
                ImGui::PopStyleColor(2);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Then click on the object to place the points.\n"
                    "Click: corner point.  Click and drag: curved point.\n"
                    "Click the first point to close the shape.\n"
                    "Drag points or handles to edit.  Click on the outline to add a point.\n"
                    "Alt+click a point: corner <-> curve.  Alt+drag a handle: break the curve.\n"
                    "Right click: cancel.").c_str());
            if (m_shape_draw_armed && m_shape_nodes.empty()) {
                ImGui::SameLine();
                m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Click on the object."));
            }
            // Draw on surface: point by point ON the object, the view free between clicks
            const bool sf_armed = m_sf_tool == 1;
            if (sf_armed) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.59f, 0.53f, 0.35f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORCA);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            m_imgui->disabled_begin(!sf_armed && (!m_svg_zones.empty() || !m_shape_nodes.empty() || m_shape_draw_armed));
            if (m_imgui->button(_u8L("Draw on surface"))) {
                if (sf_armed) {
                    if (m_sf_path.clicks.size() >= 2)
                        sf_finish();
                    sf_cancel();
                    m_sf_tool = 0;
                } else {
                    sf_cancel();
                    m_sf_tool = 1;
                }
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();
            if (sf_armed) {
                ImGui::PopStyleVar(1);
                ImGui::PopStyleColor(2);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Draw ON the object, point by point; turn the view between clicks.\n"
                    "Click: corner.  Click and drag: curve (Alt: only the way out).\n"
                    "Drag a point to slide it along the surface.\n"
                    "Click the first point: a closed shape. Enter: an open line, Width wide.\n"
                    "Its Clay grows piece by piece round the object, not from one view.\n"
                    "Clay only for now: Apply paints it as seen from its middle.").c_str());
            if (sf_armed) {
                neo_ui::row(_u8L("Width"));
                if (ImGui::SliderFloat("##sf_width_v", &m_sf_width, 0.4f, 20.f, "%.1f mm"))
                    sf_update();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("How wide an open line is. A closed shape raises its whole inside.").c_str());
                render_surface_controls();
            }
        }
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
    }

    if (m_paint_mode == PaintMode::Brush) {   // Orca's brush tools
    m_imgui->text(m_desc.at("tool_type"));

    std::array<wchar_t, 6> tool_ids;
    tool_ids = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::HeightRangeIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };
    std::array<wchar_t, 6> icons;
    if (m_is_dark_mode)
        icons = { ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::TriangleButtonDarkIcon, ImGui::HeightRangeDarkIcon, ImGui::FillButtonDarkIcon, ImGui::GapFillDarkIcon };
    else
        icons = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::HeightRangeIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };
    std::array<wxString, 6> tool_tips = { _L("Circle"), _L("Sphere"), _L("Triangle"), _L("Height Range"), _L("Fill"), _L("Gap Fill") };
    for (int i = 0; i < tool_ids.size(); i++) {
        std::string  str_label = std::string("");
        std::wstring btn_name  = icons[i] + boost::nowide::widen(str_label);

        if (i != 0) ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));                     // ORCA Removes button background on dark mode
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));                       // ORCA Fixes icon rendered without colors while using Light theme
        if (m_current_tool == tool_ids[i]) {
            ImGui::PushStyleColor(ImGuiCol_Button,          ImVec4(0.f, 0.59f, 0.53f, 0.25f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,   ImVec4(0.f, 0.59f, 0.53f, 0.25f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,    ImVec4(0.f, 0.59f, 0.53f, 0.30f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_Border,          ImGuiWrapper::COL_ORCA);            // ORCA use orca color for border on selected tool / brush
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0);
        }
        bool btn_clicked = ImGui::Button(into_u8(btn_name).c_str());
        if (m_current_tool == tool_ids[i])
        {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(1);

        if (btn_clicked && m_current_tool != tool_ids[i]) {
            m_current_tool = tool_ids[i];
            for (auto &triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }

        if (ImGui::IsItemHovered()) {
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool != old_tool)
        this->tool_changed(old_tool, m_current_tool);

    if (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::SphereButtonIcon) {
        if (m_current_tool == ImGui::CircleButtonIcon)
            m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        else
             m_cursor_type = TriangleSelector::CursorType::SPHERE;
        m_tool_type = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(circle_max_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + circle_max_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");

        ImGui::Separator();
        if (m_c->object_clipper()->get_position() == 0.f) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("clipping_of_view"));
        }
        else {
            if (m_imgui->button(m_desc.at("reset_direction"))) {
                wxGetApp().CallAfter([this]() {
                    m_c->object_clipper()->set_position_by_ratio(-1., false);
                    });
            }
        }

        auto clp_dist = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(circle_max_width);
        ImGui::PushItemWidth(sliders_width);
        bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + circle_max_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position_by_ratio(clp_dist, true); }

    } else if (m_current_tool == ImGui::TriangleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BRUSH;

        if (m_c->object_clipper()->get_position() == 0.f) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("clipping_of_view"));
        }
        else {
            if (m_imgui->button(m_desc.at("reset_direction"))) {
                wxGetApp().CallAfter([this]() {
                    m_c->object_clipper()->set_position_by_ratio(-1., false);
                    });
            }
        }

        auto clp_dist = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(clipping_slider_left);
        ImGui::PushItemWidth(sliders_width);
        bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + clipping_slider_left);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position_by_ratio(clp_dist, true); }

    } else if (m_current_tool == ImGui::FillButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_imgui->bbl_checkbox(m_desc["edge_detection"], m_detect_geometry_edge);
        m_tool_type = ToolType::BUCKET_FILL;

        if (m_detect_geometry_edge) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc["smart_fill_angle"]);
            std::string format_str = std::string("%.f") + I18N::translate_utf8("°", "Face angle threshold,"
                                                                                    "placed after the number with no whitespace in between.");
            ImGui::SameLine(sliders_left_width);
            ImGui::PushItemWidth(sliders_width);
            if (m_imgui->bbl_slider_float_style("##smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, format_str.data(), 1.0f, true))
                for (auto &triangle_selector : m_triangle_selectors) {
                    triangle_selector->seed_fill_unselect_all_triangles();
                    triangle_selector->request_update_render_data();
                }
            ImGui::SameLine(drag_left_width + sliders_left_width);
            ImGui::PushItemWidth(1.5 * slider_icon_width);
            ImGui::BBLDragFloat("##smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.0f, 0.0f, "%.2f");
        } else {
            // set to negative value to disable edge detection
            m_smart_fill_angle = -1.f;
        }
        ImGui::Separator();
        if (m_c->object_clipper()->get_position() == 0.f) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("clipping_of_view"));
        }
        else {
            if (m_imgui->button(m_desc.at("reset_direction"))) {
                wxGetApp().CallAfter([this]() {
                    m_c->object_clipper()->set_position_by_ratio(-1., false);
                    });
            }
        }

        auto clp_dist = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position_by_ratio(clp_dist, true);}

    } else if (m_current_tool == ImGui::HeightRangeIcon) {
        m_tool_type   = ToolType::BRUSH;
        m_cursor_type = TriangleSelector::CursorType::HEIGHT_RANGE;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["height_range"] + ":");
        ImGui::SameLine(height_max_width);
        ImGui::PushItemWidth(sliders_width);
        std::string format_str = std::string("%.2f") + I18N::translate_utf8("mm", "Heigh range," "Facet in [cursor z, cursor z + height] will be selected.");
        m_imgui->bbl_slider_float_style("##cursor_height", &m_cursor_height, CursorHeightMin, CursorHeightMax, format_str.data(), 1.0f, true);
        ImGui::SameLine(drag_left_width + height_max_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_height_input", &m_cursor_height, 0.05f, 0.0f, 0.0f, "%.2f");

        ImGui::Separator();
        if (m_c->object_clipper()->get_position() == 0.f) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("clipping_of_view"));
        }
        else {
            if (m_imgui->button(m_desc.at("reset_direction"))) {
                wxGetApp().CallAfter([this]() {
                    m_c->object_clipper()->set_position_by_ratio(-1., false);
                    });
            }
        }

        auto clp_dist = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(height_max_width);
        ImGui::PushItemWidth(sliders_width);
        bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + height_max_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position_by_ratio(clp_dist, true); }
    }
    else if (m_current_tool == ImGui::GapFillIcon) {
        m_tool_type = ToolType::GAP_FILL;
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["gap_area"] + ":");
        ImGui::SameLine(gap_area_slider_left);
        ImGui::PushItemWidth(sliders_width);
        std::string format_str = std::string("%.2f") + I18N::translate_utf8("", "Triangle patch area threshold,""triangle patch will be merged to neighbor if its area is less than threshold");
        m_imgui->bbl_slider_float_style("##gap_area", &TriangleSelectorPatch::gap_area, TriangleSelectorPatch::GapAreaMin, TriangleSelectorPatch::GapAreaMax, format_str.data(), 1.0f, true);
        ImGui::SameLine(drag_left_width + gap_area_slider_left);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##gap_area_input", &TriangleSelectorPatch::gap_area, 0.05f, 0.0f, 0.0f, "%.2f");
    }

    ImGui::Separator();
    if(m_imgui->bbl_checkbox(_L("Vertical"), m_vertical_only)){
        if(m_vertical_only){
            m_horizontal_only = false;
        }
    }
    if(m_imgui->bbl_checkbox(_L("Horizontal"), m_horizontal_only)){
        if(m_horizontal_only){
            m_vertical_only = false;
        }
    }
    } // end of Brush mode tools


    // Painter Pro Mode. Ungated from LibreMode: the feature set
    // (F1-F4) is low-risk enough to ship as a permanent part of this fork rather than behind the
    // LibreMode flag.
    ImGui::Separator();
    // Vector mode's own panel goes first, the common options fold below.
    if (m_paint_mode == PaintMode::Vector) {
        render_vector_panel();
        ImGui::Separator();
    }
    if (m_paint_mode == PaintMode::Clay) {
        render_clay_panel();
        ImGui::Separator();
    }
    if (m_paint_mode == PaintMode::VectorBrush) {
        render_vector_brush_panel();
        ImGui::Separator();
    }
    // the decals as a reference while painting by hand.
    // F5b: brush symmetry, every stroke repeated across the mirror planes.
    if (m_paint_mode == PaintMode::Brush) {
        ModelObject *sym_mo = m_c->selection_info()->model_object();
        neo_ui::row(_u8L("Mirror brush"));
        const int ax = neo_ui::mirror_toggles("bmir", sym_mo->paint_symmetry.brush, _u8L("Every brush stroke is painted again on the mirrored side,\n"
            "through the orange mark (the same centre as the decal mirrors).\n"
            "Dashed rings show where the copies land. Where the object is not\n"
            "symmetric and has nothing on the other side, the copy is skipped.\n"
            "Works with the Circle and Sphere brushes."));
        if (ax >= 0) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Mirror brush"), UndoRedo::SnapshotType::GizmoAction);
            sym_mo->paint_symmetry.brush ^= (1 << ax);
            m_parent.set_as_dirty();
        }
        if (sym_mo->paint_symmetry.brush != 0)
            render_mirror_centre_controls();
        ImGui::Separator();
    }
    if (m_paint_mode == PaintMode::Brush && !m_c->selection_info()->model_object()->paint_decals.empty()) {
        if (ImGui::Checkbox(_u8L("Show decals").c_str(), &m_show_decals_in_brush))
            m_parent.set_as_dirty();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Show the SVG decals of the Vector mode while you paint,\n"
                "as a reference. They are not painted until you Apply them.").c_str());
        ImGui::Separator();
    }
    if (neo_ui::section(_u8L("Pro Mode"), false)) {
        render_pro_mode_section(sliders_left_width, sliders_width, slider_icon_width);
        if (m_paint_mode == PaintMode::Brush)
            render_rectangle_mask_option();
    }

    // F4 Sesion A/B: the toolbar row above (lines ~721-881) unconditionally
    // sets m_tool_type from whichever icon is selected, every frame - so rectangle/polygon mask
    // have to win here, after that logic runs, rather than being just another toolbar icon.
    // Vector mode is the shape tool; Brush mode never is.
    if (m_paint_mode == PaintMode::Vector) {
        m_rectangle_mask_active = false;
        m_polygon_mask_active   = true;
        m_tool_type             = ToolType::POLYGON;
    } else {
        m_polygon_mask_active = false;
        if (m_rectangle_mask_active)
            m_tool_type = ToolType::RECTANGLE;
    }

    ImGui::Separator();


    if (m_imgui->button(m_desc.at("perform_remap"))) {
        m_show_filament_remap_ui = !m_show_filament_remap_ui;
        if (m_show_filament_remap_ui) {
            // reset remap to identity on opening
            m_extruder_remap.resize(m_extruders_colors.size());
            for (size_t i = 0; i < m_extruder_remap.size(); ++i)
                m_extruder_remap[i] = i;
        }
    }
    
    // Render filament swap UI if enabled
    if (m_show_filament_remap_ui) {
        ImGui::Separator();
        render_filament_remap_ui(window_width, max_tooltip_width);
    }
    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(caption_max, x, get_cur_y);

    float f_scale =m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::SameLine();

    if (m_current_tool == ImGui::GapFillIcon) {
        if (m_imgui->button(m_desc.at("perform"))) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Gap fill", UndoRedo::SnapshotType::GizmoAction);

            for (int i = 0; i < m_triangle_selectors.size(); i++) {
                TriangleSelectorPatch* ts_mm = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[i].get());
                if (!ts_mm) continue;
                ts_mm->update_selector_triangles();
                ts_mm->request_update_render_data(true);
            }
            update_model_object();
            m_parent.set_as_dirty();
        }

        ImGui::SameLine();
    }

    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);
        ModelObject *        mo  = m_c->selection_info()->model_object();
        int                  idx = -1;
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }

        update_model_object();
        m_parent.set_as_dirty();
    }
    ImGui::PopStyleVar(2);
    GizmoImguiEnd();

    // BBS
    ImGuiWrapper::pop_toolbar_style();
    render_decal_overlay();
}

// U2/U3: on the 3D view, next to the selected decal: a mini-bar with the
// actions used while looking at the model, and in Clay an arrow showing where the parts grow
// (towards the view the decal was placed from). Anchored on its Clay parts if it has any, else on
// the centre of its card.
// what Draw on surface (Vector) and Wrap (Vector Brush) share under their knobs.
void GLGizmoMmuSegmentation::render_surface_controls()
{
    sf_check_edit();
    m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT,
        GUI::format(_L("%1% points. Turn the view freely between clicks."), m_sf_path.clicks.size()));
    if (!m_sf_warn.empty())
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, m_sf_warn);
    if (m_sf_gaps > 0)
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT,
            GUI::format(_L("The line jumps in %1% places (shown red): put more points there."), m_sf_gaps));
    m_imgui->disabled_begin(m_sf_path.clicks.size() < 2);
    if (neo_ui::icon_text_button(m_sf_edit_decal >= 0 ? _u8L("Save") : _u8L("Finish line"), neo_ui::Icon::Apply))
        sf_finish();
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", (m_sf_edit_decal >= 0 ? _u8L("Save the moved points into the decal (Enter).") : _u8L("Keep it as a decal (Enter).")).c_str());
    if (m_sf_tool == 1) {   // Draw on surface: close without hunting for the first point
        ImGui::SameLine();
        m_imgui->disabled_begin(m_sf_path.clicks.size() < 3 || m_sf_path.closed);
        if (neo_ui::icon_text_button(_u8L("Close shape"), neo_ui::Icon::Close)) {
            m_sf_undo.push_back(m_sf_path);
            m_sf_path.closed = true;
            sf_update();
        }
        m_imgui->disabled_end();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Join the last point to the first one: a closed shape.\nYou can still move points; Finish keeps it.").c_str());
    }
    ImGui::SameLine();
    m_imgui->disabled_begin(m_sf_path.clicks.empty());
    if (neo_ui::icon_text_button(m_sf_edit_decal >= 0 ? _u8L("Cancel") : _u8L("Cancel line"), neo_ui::Icon::Cancel))
    {
        const bool was_edit = m_sf_edit_decal >= 0;
        sf_cancel();   // editing: the decal stays as it was saved
        if (was_edit)
            m_sf_tool = 0;
    }
    m_imgui->disabled_end();
    m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, m_sf_edit_decal >= 0 ?
        _u8L("Editing a saved line: Finish updates it (its Clay parts need Rebuild).") :
        _u8L("Finish (or Enter) adds a decal; build its Clay in the Clay tab."));
}

// the Vector Brush panel: only how the stroke is drawn.
// brush pieces (a stroke or an effect's strands / dots) as dots + quads, offset by
// `o` and scaled; with mm_px > 0, what is thinner than 0.4 mm goes red.
static void vb_draw_pieces(ImDrawList *dl, ImVec2 o, float k, const std::vector<stroke::Piece> &pcs, ImU32 col_a, double mm_px, ImU32 col_b = 0)
{
    for (const stroke::Piece &pc : pcs) {
        const ImU32 col = pc.tone == 1 && col_b != 0 ? col_b : col_a;   // Two-tone: the halo's colour
        const std::vector<stroke::Pt> &c = pc.c;
        const std::vector<double>     &w = pc.w;
        if (c.empty()) continue;
        auto at = [&](size_t i, double s) {
            const size_t a = i ? i - 1 : 0, b = std::min(c.size() - 1, i + 1);
            double tx = c[b].x - c[a].x, ty = c[b].y - c[a].y, l = std::hypot(tx, ty);
            if (l <= 0) l = 1;
            return ImVec2(float(o.x + (c[i].x - ty / l * s) * k), float(o.y + (c[i].y + tx / l * s) * k));
        };
        auto colour = [&](double wd) { return mm_px > 0 && wd * mm_px < 0.4 ? IM_COL32(255, 90, 90, 255) : col; };
        for (size_t i = 0; i + 1 < c.size(); ++i)
            dl->AddQuadFilled(at(i, w[i] / 2), at(i + 1, w[i + 1] / 2), at(i + 1, -w[i + 1] / 2), at(i, -w[i] / 2), colour(std::min(w[i], w[i + 1])));
        dl->AddCircleFilled(ImVec2(float(o.x + c.front().x * k), float(o.y + c.front().y * k)), float(w.front() / 2 * k), colour(w.front()), 10);
        if (c.size() > 1)
            dl->AddCircleFilled(ImVec2(float(o.x + c.back().x * k), float(o.y + c.back().y * k)), float(w.back() / 2 * k), colour(w.back()), 10);
    }
}

// The box behind a preview: dark, or mid grey when the filament is dark (black on black showed nothing).
static ImU32 vb_box_colour(ImU32 col)
{
    const float r = float(col & 0xFF), g = float((col >> 8) & 0xFF), b = float((col >> 16) & 0xFF);
    return 0.2126f * r + 0.7152f * g + 0.0722f * b < 70.f ? IM_COL32(96, 98, 104, 255) : IM_COL32(21, 22, 24, 255);
}

// Draws pieces inside [a, b], clipped: at real size if they fit, else scaled down to fit and centred.
// Returns the scale used (1 = real size).
static float vb_draw_fit(ImDrawList *dl, ImVec2 a, ImVec2 b, const std::vector<stroke::Piece> &pcs, ImU32 col, double mm_px, float pad, ImU32 col_b = 0)
{
    double lx = 1e30, ly = 1e30, hx = -1e30, hy = -1e30;
    for (const stroke::Piece &pc : pcs)
        for (size_t i = 0; i < pc.c.size(); ++i) {
            const double r = pc.w[i] * 0.5;
            lx = std::min(lx, pc.c[i].x - r); hx = std::max(hx, pc.c[i].x + r);
            ly = std::min(ly, pc.c[i].y - r); hy = std::max(hy, pc.c[i].y + r);
        }
    if (hx < lx) return 1.f;
    const double bw = std::max(1.0, double(b.x - a.x) - 2 * pad), bh = std::max(1.0, double(b.y - a.y) - 2 * pad);
    const float  k  = float(std::min({ 1.0, bw / std::max(1e-6, hx - lx), bh / std::max(1e-6, hy - ly) }));
    const ImVec2 o(float((a.x + b.x) * 0.5 - (lx + hx) * 0.5 * k), float((a.y + b.y) * 0.5 - (ly + hy) * 0.5 * k));
    dl->PushClipRect(a, b, true);
    vb_draw_pieces(dl, o, k, pcs, col, mm_px, col_b);
    dl->PopClipRect();
    return k;
}

// A sample S stroke in a w x h box: the hand speeds up in the middle and slows at the ends.
static std::vector<stroke::Pt> vb_demo_stroke(double w, double h, double sz)
{
    std::vector<stroke::Pt> raw;
    const double x0 = sz * 0.6 + 6, x1 = w - sz * 0.6 - 6, amp = std::max(0.0, h * 0.5 - sz * 0.6 - 4);
    double t = 0, px = 0, py = 0;
    for (int i = 0; i <= 80; ++i) {
        const double u = i / 80.0, x = x0 + (x1 - x0) * u, y = h * 0.5 + std::sin(u * 2 * M_PI) * amp * 0.8;
        if (i) t += std::hypot(x - px, y - py) / (0.3 + 2.4 * std::sin(u * M_PI));   // px per ms, like a hand
        raw.push_back({ x, y, 1, t });
        px = x; py = y;
    }
    return raw;
}

void GLGizmoMmuSegmentation::render_vector_brush_panel()
{
    neo_ui::Compact compact;
    auto tip = [](const std::string &t) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.c_str()); };
    stroke::Params &P = m_vb_params;
    const stroke::Params knobs_before = P;
    auto pct = [&](const std::string &label, const char *id, double &v, float lo, float hi, const std::string &t) {
        neo_ui::row(label);
        float f = float(v);
        if (ImGui::SliderFloat(id, &f, lo, hi, "%.0f %%")) { v = double(f); m_parent.set_as_dirty(); }
        tip(t);
    };
    if (neo_ui::section(_u8L("Vector Brush") + "###vb_brush", true)) {
        neo_ui::row(_u8L("Tool"));
        int tool = m_vb_eraser ? 3 : m_sf_tool == 2 ? 2 : m_vb_line ? 1 : 0;
        const int tool_before = tool;
        if (neo_ui::segmented("vb_tool", { _u8L("Brush"), _u8L("Line"), _u8L("Wrap"), _u8L("Eraser") }, tool)) {
            if (tool == 2 && (!m_vb_strokes.empty() || !m_vb_nodes.empty())) {   // Wrap turns the view: the screen strokes are pinned to it
                tool = tool_before;
                m_vb_wrap_msg = _u8L("Save (tick) or clear the strokes first: Wrap turns the view.");
            } else {
                m_vb_wrap_msg.clear();
                if (!m_vb_nodes.empty())
                    vb_line_finish();
                if (m_sf_tool == 2 && tool != 2 && m_sf_path.clicks.size() >= 2)
                    sf_finish();   // leaving Wrap: the line goes in
                if (tool != 2)
                    sf_cancel();
                m_vb_eraser = tool == 3;
                if (tool != 3) {
                    m_vb_line = tool == 1;
                    m_sf_tool = tool == 2 ? 2 : 0;
                } else
                    m_sf_tool = 0;
            }
            m_parent.set_as_dirty();
        }
        if (!m_vb_wrap_msg.empty())
            m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, m_vb_wrap_msg);
        tip(_u8L("Brush: free hand.\n"
                 "Line: click point by point; click and drag pulls out a curve\n"
                 "(Alt while dragging = a corner). Enter or Finish line ends it,\n"
                 "a click on the first point closes it. Ctrl+Z takes the last point back.\n"
                 "Wrap: the same, but ON the object: it wraps round it and you can\n"
                 "turn the view between clicks. Open only; its width follows the\n"
                 "knobs below (Width = Size). Its Clay grows piece by piece round the object.\n"
                 "Eraser: erases every stroke it goes over, whatever its filament.\n"
                 "Nothing is lost: Undo stroke brings it back.\n"
                 "Alt + click a stroke to edit its knobs (outlined).\nKey: E"));
        if (m_sf_tool == 2) {   // Wrap: Size in mm
            neo_ui::row(_u8L("Width"));
            if (ImGui::SliderFloat("##sf_width", &m_sf_width, 0.4f, 20.f, "%.1f mm"))
                sf_update();
            tip(_u8L("The full width of the line on the object (the brush Size).\nTaper, Min width and the rest shape it along the line."));
        }
        // Effect: a graphic drop-down, each cell drawn by its own effect
        if (m_sf_tool != 2) {
            static const char *fx_names[] = { "Plain", "Hair / fur", "Stipple", "Dashed", "Grass", "Calligraphy", "Dither edge" };
            static bool fx_open = false;
            ImU32 fcol = IM_COL32(242, 193, 78, 255);
            fcol = ImGuiWrapper::to_ImU32(get_state_color(int(get_left_button_state_type())));   // by filament id, not list position
            const ImU32 fcol_b = ImGuiWrapper::to_ImU32(get_state_color(m_vb_tone_b));
            const float fs = ImGui::GetFontSize();   // every size here follows the UI font (it is scaled on Retina)
            auto thumb = [&](const char *id, stroke::Effect e, float tw, float th, bool sel) {
                const ImVec2 o = ImGui::GetCursorScreenPos();
                const bool   hit = ImGui::InvisibleButton(id, ImVec2(tw, th));
                ImDrawList  *dl  = ImGui::GetWindowDrawList();
                dl->AddRectFilled(o, ImVec2(o.x + tw, o.y + th), vb_box_colour(fcol), 4.f);
                const float  lab = fs + 4.f;          // the name's strip at the bottom
                const float  bh  = th - lab;
                stroke::Params q = P;
                q.fx    = e;
                q.size  = std::max(3.0, double(bh) * 0.3);
                q.drops = 0;
                // Calligraphy needs turns to show: its sample is a tighter S
                const std::vector<stroke::Pt> demo = vb_demo_stroke(e == stroke::Effect::Calligraphy ? tw * 0.5 : tw, bh * 1.6, q.size);
                vb_draw_fit(dl, ImVec2(o.x + 2, o.y + 2), ImVec2(o.x + tw - 2, o.y + bh), stroke::effect_pieces(q, demo, false), fcol, 0, 3.f, fcol_b);
                dl->AddText(ImVec2(o.x + 6, o.y + bh + 1), IM_COL32(210, 210, 210, 255), fx_names[int(e)]);
                if (sel || ImGui::IsItemHovered())
                    dl->AddRect(o, ImVec2(o.x + tw, o.y + th), sel ? IM_COL32(79, 179, 255, 255) : IM_COL32(140, 140, 140, 255), 4.f, 0, 1.5f);
                return hit;
            };
            neo_ui::row(_u8L("Effect"));
            if (thumb("##vb_fx_cur", P.fx, ImGui::GetContentRegionAvail().x, fs * 3.4f, fx_open))
                fx_open = !fx_open;
            if (!fx_open)   // open, it covered the grid
                tip(_u8L("Brush effects: one stroke becomes strands, dots, dashes...\nClick to pick one. Each picture is drawn by the effect itself."));
            if (fx_open) {
                const float full = ImGui::GetContentRegionAvail().x;   // the grid takes the panel's whole width
                const float cw   = (full - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                for (int e = 0; e < 7; ++e) {
                    if (e % 2) ImGui::SameLine();
                    if (thumb((std::string("##vb_fx") + char('0' + e)).c_str(), stroke::Effect(e), cw, fs * 3.8f, P.fx == stroke::Effect(e))) {
                        P.fx   = stroke::Effect(e);
                        fx_open = false;
                        m_parent.set_as_dirty();
                    }
                }
            }
            auto knob = [&](const std::string &label, const char *id, double &v, float lo, float hi, const char *fmt, const std::string &t) {
                neo_ui::row(label);
                float f = float(v);
                if (ImGui::SliderFloat(id, &f, lo, hi, fmt)) { v = double(f); m_parent.set_as_dirty(); }
                tip(t);
            };
            switch (P.fx) {
            case stroke::Effect::Hair:
            case stroke::Effect::Grass:
                knob(_u8L("Density"), "##vb_fx_am", P.fx_amount, 0.f, 100.f, "%.0f %%", _u8L("How many strands along the stroke."));
                knob(_u8L("Length"), "##vb_fx_len", P.fx_len, 50.f, 1000.f, "%.0f %%", _u8L("Strand length, as % of Size."));
                knob(_u8L("Root"), "##vb_fx_dot", P.fx_dot, 5.f, 100.f, "%.0f %%", _u8L("Strand width at its root, as % of Size.\nThe tips get thin: on the object keep them over one line wide."));
                knob(P.fx == stroke::Effect::Hair ? _u8L("Lean") : _u8L("Sway"), "##vb_fx_ang", P.fx_angle, 0.f, 90.f, "%.0f°",
                     P.fx == stroke::Effect::Hair ? _u8L("How much the strands lean along the stroke (0 = straight out).") : _u8L("How much each blade turns from straight out."));
                knob(_u8L("Randomness"), "##vb_fx_jit", P.fx_jit, 0.f, 100.f, "%.0f %%", _u8L("How different each strand is: length, angle, curl."));
                break;
            case stroke::Effect::Stipple:
                knob(_u8L("Density"), "##vb_fx_am", P.fx_amount, 0.f, 100.f, "%.0f %%", _u8L("How many dots."));
                knob(_u8L("Dot size"), "##vb_fx_dot", P.fx_dot, 5.f, 100.f, "%.0f %%", _u8L("Dot size, as % of Size."));
                knob(_u8L("Edge falloff"), "##vb_fx_jit", P.fx_jit, 0.f, 100.f, "%.0f %%", _u8L("Fewer dots towards the edge of the stroke.\nHigh = a soft, see-through edge."));
                break;
            case stroke::Effect::Dashed:
                knob(_u8L("Dash"), "##vb_fx_len", P.fx_len, 50.f, 1000.f, "%.0f %%", _u8L("Dash length, as % of Size."));
                knob(_u8L("Fill"), "##vb_fx_am", P.fx_amount, 0.f, 100.f, "%.0f %%", _u8L("How much of each dash + gap is dash."));
                break;
            case stroke::Effect::TwoTone: {
                knob(_u8L("Halo"), "##vb_fx_len", P.fx_len, 20.f, 400.f, "%.0f %%", _u8L("How far the dots reach past the solid core, each side, as % of Size.\nFor two colours, layer two brushes: save this one, pick another filament\nand paint a plain stroke over it (the newest decal paints on top)."));
                knob(_u8L("Density"), "##vb_fx_am", P.fx_amount, 0.f, 100.f, "%.0f %%", _u8L("How many dots in the halo."));
                knob(_u8L("Dot size"), "##vb_fx_dot", P.fx_dot, 5.f, 100.f, "%.0f %%", _u8L("Dot size, as % of Size."));
                knob(_u8L("Falloff"), "##vb_fx_jit", P.fx_jit, 0.f, 100.f, "%.0f %%", _u8L("How fast the halo thins out: high = a soft edge that fades early."));
                break;
            }
            case stroke::Effect::Calligraphy:
                knob(_u8L("Nib angle"), "##vb_fx_ang", P.fx_angle, 0.f, 180.f, "%.0f°", _u8L("The angle of the flat nib: strokes along it are thin."));
                knob(_u8L("Thin stroke"), "##vb_fx_dot", P.fx_dot, 0.f, 100.f, "%.0f %%", _u8L("The thinnest the line gets, as % of the nib."));
                break;
            default: break;
            }
        }
        neo_ui::row(_u8L("Engine"));
        int en = int(P.engine);
        if (neo_ui::segmented("vb_engine", { _u8L("Sampled"), _u8L("Quill") }, en)) { P.engine = stroke::Engine(en); m_parent.set_as_dirty(); }
        tip(_u8L("How the saved outline is built.\n"
                 "Sampled: many points, smoothed.\n"
                 "Quill: exact quadratic curves, few nodes. Method by\n"
                 "Gabriel Suchowolski (microbians), github.com/microbians/bezier"));
        // Styles: presets of the knobs below (Quill ink and Dirty ink after microbians' brushes)
        neo_ui::row(_u8L("Style"));
        static const char *styles[] = { "Natural", "Quill ink", "Pen", "Marker", "Dirty ink" };
        static int style = 0;
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::Combo("##vb_style", &style, styles, IM_ARRAYSIZE(styles))) {
            const stroke::Engine en = P.engine;
            const double         sz = P.size;
            const stroke::Params keep = P;
            P = stroke::Params();
            P.engine = en;
            P.size   = sz;
            P.fx = keep.fx; P.fx_amount = keep.fx_amount; P.fx_len = keep.fx_len;   // a Style keeps the effect
            P.fx_jit = keep.fx_jit; P.fx_angle = keep.fx_angle; P.fx_dot = keep.fx_dot;
            switch (style) {
            case 1: P.width = stroke::Width::Ink; P.speed = 75; P.minw = 20; P.inertia = 45; P.tin = 5; P.tout = 25; break;
            case 2: P.width = stroke::Width::Speed; P.speed = 20; P.minw = 60; P.tin = 4; P.tout = 8; P.stream = 25; break;
            case 3: P.width = stroke::Width::Constant; P.tin = 0; P.tout = 0; P.stream = 45; break;
            case 4: P.width = stroke::Width::Ink; P.speed = 70; P.minw = 15; P.inertia = 35; P.drops = 25; P.tin = 5; P.tout = 30; break;
            default: break;
            }
            m_parent.set_as_dirty();
        }
        tip(_u8L("A starting point for the knobs below; change any of them after.\n"
                 "Quill ink and Dirty ink follow the quill brushes of\n"
                 "Gabriel Suchowolski (microbians)."));
        neo_ui::row(_u8L("Width"));
        int wm = int(P.width);
        if (neo_ui::segmented("vb_width", { _u8L("Speed"), _u8L("Ink"), _u8L("Constant") }, wm)) { P.width = stroke::Width(wm); m_parent.set_as_dirty(); }
        tip(_u8L("Speed: a fast stroke gets thinner at once.\n"
                 "Ink: the line swells where your hand slows and keeps that\n"
                 "inertia, like a quill.\n"
                 "Constant: the same width all along (tapers still apply)."));
        if (P.width == stroke::Width::Ink)
            pct(_u8L("Inertia"), "##vb_inertia", P.inertia, 0.f, 100.f, _u8L("How slowly the ink follows your hand's speed."));
        pct(_u8L("Drops"), "##vb_drops", P.drops, 0.f, 60.f, _u8L("Now and then the line swells, like a drop of ink."));
        neo_ui::row(_u8L("Size"));
        float sz = float(P.size);
        if (ImGui::SliderFloat("##vb_size", &sz, 2.f, 80.f, "%.0f px", ImGuiSliderFlags_Logarithmic)) { P.size = sz; m_parent.set_as_dirty(); }
        tip(_u8L("Width of the stroke on screen. Zoom in for finer lines on the object."));
        pct(_u8L("StreamLine"), "##vb_stream", P.stream, 0.f, 95.f, _u8L("The tip follows your hand with an elastic lag:\nhigher = smoother, steadier lines."));
        neo_ui::row(_u8L("Pressure"));
        int cv = int(P.curve);
        if (neo_ui::segmented("vb_curve", { _u8L("Soft"), _u8L("Linear"), _u8L("Hard") }, cv)) { P.curve = stroke::Curve(cv); m_parent.set_as_dirty(); }
        tip(_u8L("Pen pressure curve. Pressure comes later (Wacom); now the speed\nof your hand sets the width: faster = thinner."));
        pct(_u8L("Taper in"), "##vb_tin", P.tin, 0.f, 60.f, _u8L("How much of the stroke's start gets thinner, as % of its length."));
        pct(_u8L("Taper out"), "##vb_tout", P.tout, 0.f, 60.f, _u8L("How much of the stroke's end gets thinner, as % of its length."));
        pct(_u8L("Min width"), "##vb_minw", P.minw, 0.f, 100.f, _u8L("The thinnest the stroke can get, as % of Size."));
        pct(_u8L("Speed thins"), "##vb_speed", P.speed, 0.f, 90.f, _u8L("How much a fast stroke gets thinner."));
    }
    // preview: a sample S stroke built with the same knobs, at
    // screen size (Size is in screen px, so this is how it will look). The hand speeds up in the middle
    // and slows at the ends, so Speed / Ink thinning shows. Wrap: Size is mm, red = thinner than 0.4 mm.
    if (!m_vb_eraser) {
        const float  avail = ImGui::GetContentRegionAvail().x;
        const float  fs    = ImGui::GetFontSize();
        const double sz    = std::max(1.0, P.size);
        const ImU32  col   = ImGuiWrapper::to_ImU32(get_state_color(int(get_left_button_state_type())));   // by filament id
        const ImU32  col_b = ImGuiWrapper::to_ImU32(get_state_color(m_vb_tone_b));
        const double mm_px = m_sf_tool == 2 && sz > 0 ? m_sf_width / sz : 0;   // Wrap only: mm per px
        // built first, so the box can grow to what the effect really makes (strands stick out of the stroke)
        stroke::Params pv = P;
        if (m_sf_tool == 2) pv.fx = stroke::Effect::None;   // Wrap has no effects (yet): show what it will draw
        const std::vector<stroke::Piece> pcs = stroke::effect_pieces(pv, vb_demo_stroke(avail, sz * 1.8 + 16.0, sz), false);
        double ly = 1e30, hy = -1e30;
        for (const stroke::Piece &pc : pcs)
            for (size_t i = 0; i < pc.c.size(); ++i) { ly = std::min(ly, pc.c[i].y - pc.w[i] * 0.5); hy = std::max(hy, pc.c[i].y + pc.w[i] * 0.5); }
        const float  boxh = float(std::clamp(hy > ly ? hy - ly + 12.0 : 40.0, double(fs * 2.5f), double(fs * 9.f)));
        const ImVec2 o    = ImGui::GetCursorScreenPos();
        ImDrawList  *dl   = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(o.x + avail, o.y + boxh), vb_box_colour(col), 4.f);
        const float k = vb_draw_fit(dl, o, ImVec2(o.x + avail, o.y + boxh), pcs, col, mm_px, 4.f, col_b);
        if (k < 0.99f) {   // too big to show at real size: say so
            char t[32];
            std::snprintf(t, sizeof(t), "x%.2f", k);
            dl->AddText(ImVec2(o.x + avail - ImGui::CalcTextSize(t).x - 6, o.y + 3), IM_COL32(220, 220, 220, 255), t);
        }
        ImGui::Dummy(ImVec2(avail, boxh));
        tip(m_sf_tool == 2 ? _u8L("Preview of the next line with these knobs.\nRed: thinner than 0.4 mm, the slicer may lose it.") :
                             _u8L("Preview of the next stroke with these knobs, at screen size.\nIt speeds up in the middle, like a hand, so you see how it thins."));
    }
    // 2a: the knobs set the next strokes; with the eraser they also change the selected stroke,
    // one Undo stroke step per drag or click.
    const bool edit_sel = m_vb_eraser && m_vb_sel >= 0 && m_vb_sel < int(m_vb_strokes.size());
    if (m_sf_tool == 2 && P != knobs_before)
        sf_update();   // Wrap follows the knobs
    if (edit_sel && P != knobs_before) {
        if (!m_vb_knob_gesture) {
            m_vb_knob_undo.push_back({ m_vb_sel, m_vb_strokes[size_t(m_vb_sel)].p });
            m_vb_history.push_back(-2 - int(m_vb_knob_undo.size() - 1));
        }
        m_vb_strokes[size_t(m_vb_sel)].p = P;
        m_parent.set_as_dirty();
    }
    m_vb_knob_gesture = edit_sel && P != knobs_before ? ImGui::IsMouseDown(0) : (m_vb_knob_gesture && ImGui::IsMouseDown(0));
    m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, edit_sel ?
        _u8L("The knobs change the stroke you picked (Alt + click), and the next ones.") :
        _u8L("The knobs change the next strokes. With the Eraser, Alt + click a stroke to edit it."));
    if (m_sf_tool == 2 && !m_vb_eraser) {
        render_surface_controls();
        return;
    }
    if (m_vb_line && !m_vb_eraser) {
        m_imgui->disabled_begin(m_vb_nodes.size() < 2);
        if (m_imgui->button(_L("Finish line")))
            vb_line_finish();
        m_imgui->disabled_end();
        ImGui::SameLine();
        m_imgui->disabled_begin(m_vb_nodes.empty());
        if (m_imgui->button(_L("Cancel line"))) {
            m_vb_nodes.clear();
            m_vb_line_closed = false;
            m_parent.set_as_dirty();
        }
        m_imgui->disabled_end();
    }
    m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _u8L("Draw with the left button. Right drag and wheel move the view."));
    if (!m_vb_strokes.empty())
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, _u8L("View locked until you save (tick) or cancel the strokes:\nthey are pinned from this view."));
    m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _u8L("The tick saves the strokes as a decal in the selected filament."));
    m_imgui->disabled_begin(m_vb_strokes.empty());
    if (m_imgui->button(_L("Undo stroke"))) vb_undo();
    ImGui::SameLine();
    if (m_imgui->button(_L("Clear"))) { m_vb_strokes.clear(); m_vb_nodes.clear(); m_vb_history.clear(); m_vb_erase_groups.clear(); m_vb_knob_undo.clear(); m_vb_sel = -1; m_parent.set_as_dirty(); }
    m_imgui->disabled_end();
}

void GLGizmoMmuSegmentation::render_decal_overlay()
{
    // the strokes drawn so far, see-through in the filament's colour, and ✓ / ✕ above them.
    sf_check_edit();   // the decal being edited may have been deleted
    sf_render_points(ImGui::GetForegroundDrawList());
    if (m_paint_mode == PaintMode::VectorBrush) {
        ImDrawList  *fg  = ImGui::GetForegroundDrawList();
        // every stroke keeps the filament it was painted with, and they are drawn in paint
        // order (the newest on top), the way ✓ will flatten them.
        auto scol = [this](int st) {
            const ColorRGBA c = get_state_color(st);
            return IM_COL32(int(c.r() * 255), int(c.g() * 255), int(c.b() * 255), 235);
        };
        const ImU32 col = scol(int(get_left_button_state_type()));
        struct Item { const std::vector<stroke::Pt> *raw = nullptr; const stroke::Params *p = nullptr; const VbStroke *erased = nullptr; ImU32 col = 0; size_t first = 0, last = 0; };
        std::vector<Item> all;
        float top = FLT_MAX, lx = FLT_MAX, hx = -FLT_MAX;
        for (const VbStroke &vs : m_vb_strokes) {
            const ImU32 c = scol(vs.state >= 0 ? vs.state : int(get_left_button_state_type()));
            if (vs.erase.empty()) all.push_back({ &vs.raw, &vs.p, nullptr, c });
            else                  all.push_back({ nullptr, nullptr, &vs, c });
        }
        const bool erasing_now = m_vb_drawing && m_vb_eraser;
        if (m_vb_drawing && !m_vb_eraser && !m_vb_cur.empty()) all.push_back({ &m_vb_cur, &m_vb_params, nullptr, col });
        // the line being drawn, with its real width, then its points and handles on top
        std::vector<stroke::Pt> line_now;
        stroke::Params          line_p = m_vb_params;
        if (!m_vb_nodes.empty()) {
            line_now    = vb_line_samples();
            line_p.stream = 0.;
            if (line_now.size() >= 2)
                all.push_back({ &line_now, &line_p, nullptr, col });
        }
        std::vector<std::vector<stroke::Pt>> ctrs;
        std::vector<std::vector<double>>     ws;
        size_t total = 0;
        std::vector<char> rounds;   // a plain stroke or a dot: round dots all along
        for (Item &it : all) {
            it.first = ctrs.size();
            if (it.raw != nullptr)
                for (stroke::Piece &pc : stroke::effect_pieces(*it.p, *it.raw, false)) {   // an effect: many pieces
                    total += pc.c.size();
                    rounds.push_back(it.p->fx == stroke::Effect::None || pc.c.size() == 1);
                    ctrs.push_back(std::move(pc.c));
                    ws.push_back(std::move(pc.w));
                }
            it.last = ctrs.size();
        }
        // ImGui 1.83 has 16-bit indices: past ~65k vertices in one frame it crashed (long spirals, 31-oct).
        // the renderer now takes VtxOffset (no 64k wrap), so this is only for speed. Effects are
        // many short pieces: each keeps its first and last point, and only long ones are thinned out;
        // a strand gets no round dots (they turned thinned strands into drops), only dots and plain strokes do.
        const size_t stride = std::max<size_t>(1, (total + 19999) / 20000);
        for (const Item &it : all) {
            if (it.erased != nullptr) {   // erased strokes: their real shape (stroke minus erasers), triangulated
                for (const ExPolygon &ex : vb_shape(*it.erased)) {
                    const std::vector<Vec2d> tr = triangulate_expolygon_2d(ex);   // unscaled = screen px
                    for (size_t i = 0; i + 2 < tr.size(); i += 3)
                        fg->AddTriangleFilled(ImVec2(float(tr[i].x()), float(tr[i].y())), ImVec2(float(tr[i + 1].x()), float(tr[i + 1].y())),
                                              ImVec2(float(tr[i + 2].x()), float(tr[i + 2].y())), it.col);
                    for (const Point &q : ex.contour.points) {
                        top = std::min(top, float(unscaled(q.y())));
                        lx  = std::min(lx, float(unscaled(q.x())));
                        hx  = std::max(hx, float(unscaled(q.x())));
                    }
                }
                continue;
            }
            for (size_t s = it.first; s < it.last; ++s) {
                const std::vector<stroke::Pt> &ctr = ctrs[s];
                const std::vector<double>     &w   = ws[s];
                const size_t st    = ctr.size() > 8 * stride ? stride : 1;
                const bool   round = rounds[s] != 0;
                const ImU32  pcol  = it.col;
                size_t prev = SIZE_MAX;
                for (size_t i = 0; i < ctr.size(); i = (i + st < ctr.size() || i + 1 == ctr.size()) ? i + st : ctr.size() - 1) {
                    const ImVec2 p(float(ctr[i].x), float(ctr[i].y));
                    if (round || i == 0 || i + 1 == ctr.size())
                        fg->AddCircleFilled(p, float(w[i] * 0.5), pcol, round ? 8 : 6);
                    if (prev != SIZE_MAX) {
                        const float dx = p.x - float(ctr[prev].x), dy = p.y - float(ctr[prev].y), l = std::sqrt(dx * dx + dy * dy);
                        if (l > 1e-3f) {
                            const float nx = -dy / l, ny = dx / l, ha = float(w[prev] * 0.5), hb = float(w[i] * 0.5);
                            fg->AddQuadFilled(ImVec2(float(ctr[prev].x) + nx * ha, float(ctr[prev].y) + ny * ha), ImVec2(p.x + nx * hb, p.y + ny * hb),
                                              ImVec2(p.x - nx * hb, p.y - ny * hb), ImVec2(float(ctr[prev].x) - nx * ha, float(ctr[prev].y) - ny * ha), pcol);
                        }
                    }
                    prev = i;
                    top = std::min(top, float(ctr[i].y - w[i]));
                    lx  = std::min(lx, float(ctr[i].x));
                    hx  = std::max(hx, float(ctr[i].x));
                }
            }
        }
        // the eraser pass being drawn, see-through grey
        if (erasing_now && !m_vb_cur.empty()) {
            const std::vector<stroke::Pt> e = stroke::resample(stroke::stabilize(m_vb_params, m_vb_cur), m_vb_params.step);
            const std::vector<double>     w = stroke::widths(m_vb_params, e, false);
            const size_t st = std::max<size_t>(1, e.size() / 600);
            for (size_t i = 0; i < e.size(); i += st)
                fg->AddCircleFilled(ImVec2(float(e[i].x), float(e[i].y)), float(w[i] * 0.5), IM_COL32(40, 40, 40, 150), 10);
        }
        for (size_t i = 0; i < m_vb_nodes.size(); ++i) {
            const ShapeNode &nd = m_vb_nodes[i];
            const ImVec2     p(float(nd.p.x()), float(nd.p.y()));
            if (nd.is_curved()) {
                const ImVec2 a(float(nd.p.x() + nd.in.x()), float(nd.p.y() + nd.in.y())), b(float(nd.p.x() + nd.out.x()), float(nd.p.y() + nd.out.y()));
                fg->AddLine(a, b, IM_COL32(255, 255, 255, 160), 1.f);
                fg->AddCircleFilled(a, 3.f, IM_COL32(255, 255, 255, 200));
                fg->AddCircleFilled(b, 3.f, IM_COL32(255, 255, 255, 200));
            }
            fg->AddRectFilled(ImVec2(p.x - 4.f, p.y - 4.f), ImVec2(p.x + 4.f, p.y + 4.f), i == 0 ? IM_COL32(0, 150, 136, 255) : IM_COL32(255, 255, 255, 255));
            fg->AddRect(ImVec2(p.x - 4.f, p.y - 4.f), ImVec2(p.x + 4.f, p.y + 4.f), IM_COL32(0, 0, 0, 200));
        }
        // with the eraser, the selected stroke is outlined
        if (m_vb_eraser && m_vb_sel >= 0 && m_vb_sel < int(m_vb_strokes.size()))
            for (const ExPolygon &ex : vb_shape(m_vb_strokes[m_vb_sel])) {
                std::vector<ImVec2> pl;
                const size_t st = std::max<size_t>(1, ex.contour.points.size() / 1500);
                for (size_t i = 0; i < ex.contour.points.size(); i += st)
                    pl.emplace_back(float(unscaled(ex.contour.points[i].x())), float(unscaled(ex.contour.points[i].y())));
                if (pl.size() > 2)
                    fg->AddPolyline(pl.data(), int(pl.size()), IM_COL32(255, 255, 255, 220), true, 1.5f);
            }
        if (m_vb_strokes.empty() || m_vb_drawing || m_vb_node_drag)
            return;
        if (lx > hx)   // everything erased: put the bar over the raw strokes
            for (const VbStroke &vs : m_vb_strokes)
                for (const stroke::Pt &q : vs.raw) {
                    top = std::min(top, float(q.y));
                    lx  = std::min(lx, float(q.x));
                    hx  = std::max(hx, float(q.x));
                }
        static bool vb_hovered = false;
        ImGui::SetNextWindowPos(ImVec2(0.5f * (lx + hx), top - ImGui::GetFontSize() * 0.6f), ImGuiCond_Always, ImVec2(0.5f, 1.f));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, vb_hovered ? 1.f : 0.7f);
        if (ImGui::Begin("##vb_bar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
            const float isz = ImGui::GetFrameHeight() * 1.25f;
            if (neo_ui::icon_button("##vb_save", neo_ui::Icon::Apply, true, isz)) {
                ModelObject *mo = m_c->selection_info()->model_object();
                if (vb_save() && mo != nullptr)
                    m_selected_decal = int(mo->paint_decals.size()) - 1;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Save: the strokes become a decal, seen from this view.\nPlace, colour or raise it in Vector / Clay.").c_str());
            ImGui::SameLine();
            if (neo_ui::icon_button("##vb_cancel", neo_ui::Icon::Cancel, false, isz)) {
                m_vb_strokes.clear();
                m_vb_nodes.clear();
                m_vb_history.clear(); m_vb_erase_groups.clear();
                m_vb_knob_undo.clear();
                m_vb_sel = -1;
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Cancel: throw the strokes away.").c_str());
        }
        vb_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }
    if (m_paint_mode != PaintMode::Vector && m_paint_mode != PaintMode::Clay)
        return;
    if (!m_svg_zones.empty() || !m_shape_nodes.empty()) {   // something on screen: a Save / Cancel bar above its frame
        if (m_svg_zones.empty() || m_svg_rings.empty())
            return;
        const std::array<Vec2d, 4> box = svg_screen_box();
        double top = box[0].y(), cx = 0.;
        for (const Vec2d &p : box) {
            top = std::min(top, p.y());
            cx += 0.25 * p.x();
        }
        static bool edit_hovered = false;
        ImGui::SetNextWindowPos(ImVec2(float(cx), float(top) - ImGui::GetFontSize() * 0.6f), ImGuiCond_Always, ImVec2(0.5f, 1.f));
        ImGui::SetNextWindowBgAlpha(0.6f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, edit_hovered ? 1.f : 0.7f);
        if (ImGui::Begin("##svg_edit_bar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                                    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
            const float isz     = ImGui::GetFrameHeight() * 1.25f;
            const int   editing = m_svg_edit_decal;
            if (neo_ui::icon_button("##edit_save", neo_ui::Icon::Apply, true, isz)) {
                ModelObject *mo_e = m_c->selection_info()->model_object();
                const int    n    = mo_e ? int(mo_e->paint_decals.size()) : 0;
                if (svg_pin_to_view())
                    m_selected_decal = editing >= 0 ? editing : (mo_e ? int(mo_e->paint_decals.size()) - 1 : n - 1);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", (editing >= 0 ? _u8L("Save the changes to the decal.") : _u8L("Pin to view: keep it as a decal of the object.")).c_str());
            ImGui::SameLine();
            if (neo_ui::icon_button("##edit_cancel", neo_ui::Icon::Cancel, false, isz)) {
                shape_clear();
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Cancel: drop the changes.").c_str());
        }
        edit_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        ImGui::End();
        ImGui::PopStyleVar(3);
        return;
    }
    ModelObject *mo       = m_c->selection_info()->model_object();
    const int    inst_idx = m_parent.get_selection().get_instance_idx();
    if (mo == nullptr || inst_idx < 0 || inst_idx >= int(mo->instances.size()) ||
        m_selected_decal < 0 || m_selected_decal >= int(mo->paint_decals.size()))
        return;
    const size_t      idx = size_t(m_selected_decal);
    const PaintDecal &d   = mo->paint_decals[idx];
    if (d.hidden)
        return;
    const Transform3d inst = mo->instances[size_t(inst_idx)]->get_transformation().get_matrix();
    BoundingBoxf3 bb;
    for (const ModelVolume *v : mo->volumes)
        if (paint_clay_is_volume_of(*v, d))
            bb.merge(v->mesh().transformed_bounding_box(v->get_matrix()));
    const Vec3d anchor = inst * (bb.defined ? bb.center() : d.svg_to_object(Vec2d(d.off[0], d.off[1])));
    Vec3d dir = inst.linear() * Vec3d(d.dir[0], d.dir[1], d.dir[2]);
    if (dir.norm() < 1e-9)
        return;
    dir.normalize();

    const Camera &camera = wxGetApp().plater()->get_camera();
    const Matrix4d proj_view = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const std::array<int, 4> &viewport = camera.get_viewport();
    auto ss = [&](const Vec3d &p) {
        const Vec2d s = TransformHelper::world_to_ss(p, proj_view, viewport);
        return ImVec2(float(s.x()), float(viewport[3] - s.y()));
    };
    const ImVec2 a = ss(anchor);

    if (m_paint_mode == PaintMode::Clay && m_clay_arrow_on) {
        ImDrawList *fg  = ImGui::GetForegroundDrawList();
        const ImU32 col = IM_COL32(255, 221, 85, 150);   // see-through: the paint underneath stays readable
        const ImVec2 b  = ss(anchor + dir * std::max(6., 3. * d.clay.height));
        const float dx = b.x - a.x, dy = b.y - a.y, l = std::sqrt(dx * dx + dy * dy);
        fg->AddCircleFilled(a, 4.f, col);
        if (l < 6.f) {   // pointing at the camera: a target instead of an arrow
            fg->AddCircle(a, 11.f, col, 24, 2.5f);
        } else {
            const float ux = dx / l, uy = dy / l, hs = 11.f;
            fg->AddLine(a, b, col, 3.f);
            fg->AddTriangleFilled(ImVec2(b.x + ux * hs, b.y + uy * hs),
                                  ImVec2(b.x - uy * hs * 0.6f, b.y + ux * hs * 0.6f),
                                  ImVec2(b.x + uy * hs * 0.6f, b.y - ux * hs * 0.6f), col);
        }
    }

    // The mini-bar, centred above the decal.
    ImGui::SetNextWindowPos(ImVec2(a.x, a.y - ImGui::GetFontSize() * 2.5f), ImGuiCond_Always, ImVec2(0.5f, 1.f));
    // See-through until the mouse is on it, so it never hides the paint below.
    static bool bar_hovered = false;
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, bar_hovered ? 1.f : 0.55f);
    if (ImGui::Begin("##decal_minibar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove)) {
        auto snapshot = [](const std::string &name) { Plater::TakeSnapshot(wxGetApp().plater(), name, UndoRedo::SnapshotType::GizmoAction); };
        m_imgui->disabled_begin(d.locked);
        const bool moving = m_decal_move_armed;
        const float isz = ImGui::GetFrameHeight() * 1.25f;
        if (neo_ui::icon_button("##bar_move", neo_ui::Icon::Move, moving, isz)) {
            m_decal_move_armed = !m_decal_move_armed;
            if (m_decal_move_armed)
                m_decal_on_surface = true;
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Move on object: drag on the object to slide the decal over it.\n"
                "The wheel still zooms. Turn and Size are in the panel. Click again to stop.").c_str());
        if (moving) {   // a clear way out: tick = done moving
            ImGui::SameLine();
            if (neo_ui::icon_button("##bar_done", neo_ui::Icon::Apply, true, isz)) {
                m_decal_move_armed = false;
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Done: stop moving it.").c_str());
        }
        if (m_paint_mode == PaintMode::Vector) {
            ImGui::SameLine();
            if (neo_ui::icon_button("##bar_edit", neo_ui::Icon::Edit, false, isz))
                {
                    const ModelObject *emo = m_c->selection_info()->model_object();
                    const int          ei  = m_selected_decal;
                    if (emo != nullptr && ei >= 0 && size_t(ei) < emo->paint_decals.size() && !emo->paint_decals[size_t(ei)].surface.empty()) {
                        set_paint_mode(emo->paint_decals[size_t(ei)].surface.wrap ? PaintMode::VectorBrush : PaintMode::Vector);
                        sf_edit_decal(ei);
                    } else {
                        if (m_sf_tool != 0) {   // a normal decal: the surface tool lets go first
                            if (m_sf_path.clicks.size() >= 2)
                                sf_finish();
                            sf_cancel();
                            m_sf_tool = 0;
                        }
                        set_paint_mode(PaintMode::Vector);   // edited on screen in Vector: a click there is not a brush stroke
                        svg_edit_decal(ei);
                    }
                }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Adjust it on screen from the view it was pinned from.").c_str());
            m_imgui->disabled_end();
            ImGui::SameLine();
            if (neo_ui::icon_button("##bar_apply", neo_ui::Icon::Paint, false, isz)) {
                snapshot(_u8L("Apply SVG decal"));
                decal_apply({ idx }, false);
                update_model_object();
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Paint: paint this decal on the object now.\nYou can paint it again after moving it.").c_str());
        } else {
            ImGui::SameLine();
            if (neo_ui::icon_button("##bar_rebuild", neo_ui::Icon::Rebuild, !bb.defined, isz) && !m_clay_job)
                clay_build(idx);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Make the Clay parts of this decal again with the panel's settings.").c_str());
            m_imgui->disabled_end();
        }
    }
    bar_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImGui::End();
    ImGui::PopStyleVar(3);
}


void GLGizmoMmuSegmentation::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->mmu_segmentation_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        const size_t num_physical = static_cast<size_t>(std::max(wxGetApp().filaments_cnt(), 0));
        size_t       num_total    = num_physical;
        if (wxGetApp().preset_bundle != nullptr)
            num_total = wxGetApp().preset_bundle->mixed_filaments.total_filaments(num_physical);

        size_t max_used_state = 0;
        for (const ModelVolume *mv : mo->volumes) {
            if (!mv->is_model_part())
                continue;
            const auto &used_states = mv->mmu_segmentation_facets.get_data().used_states;
            for (size_t state_idx = static_cast<size_t>(EnforcerBlockerType::Extruder1); state_idx < used_states.size(); ++state_idx) {
                if (used_states[state_idx])
                    max_used_state = std::max(max_used_state, state_idx);
            }
        }

        if (max_used_state > num_physical) {
            BOOST_LOG_TRIVIAL(warning) << "GLGizmoMmuSegmentation::update_model_object painted virtual extruder state detected"
                                       << " max_used_state=" << max_used_state
                                       << " physical_filaments=" << num_physical
                                       << " total_filaments=" << num_total;
        }

        const ModelObjectPtrs &mos = wxGetApp().model().objects;
        size_t obj_idx = std::find(mos.begin(), mos.end(), mo) - mos.begin();
        wxGetApp().obj_list()->update_info_items(obj_idx);
        wxGetApp().plater()->get_partplate_list().notify_instance_update(obj_idx, 0);
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        wxGetApp().plater()->notify_filament_usage_changed();
    }
}

void GLGizmoMmuSegmentation::init_model_triangle_selectors()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    m_volumes_extruder_idxs.clear();

    // Don't continue when extruders colors are not initialized
    if(m_extruders_colors.empty())
        return;

    // BBS: Don't continue when model object is null
    if (mo == nullptr)
        return;

    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        int extruder_idx = (mv->extruder_id() > 0) ? mv->extruder_id() - 1 : 0;
        extruder_idx = std::min(extruder_idx, int(m_extruders_colors.size()) - 1);
        std::vector<ColorRGBA> ebt_colors;
        ebt_colors.push_back(m_extruders_colors[size_t(extruder_idx)]);
        ebt_colors.insert(ebt_colors.end(), m_extruders_colors.begin(), m_extruders_colors.end());

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors, 0.2));
        // Reset of TriangleSelector is done inside TriangleSelectorMmGUI's constructor, so we don't need it to perform it again in deserialize().
        EnforcerBlockerType max_ebt = (EnforcerBlockerType)std::min(m_extruders_colors.size(), (size_t)EnforcerBlockerType::ExtruderMax);
        m_triangle_selectors.back()->deserialize(mv->mmu_segmentation_facets.get_data(), false, max_ebt);
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
        m_volumes_extruder_idxs.push_back(mv->extruder_id());
    }
}

void GLGizmoMmuSegmentation::update_triangle_selectors_colors()
{
    if (m_extruders_colors.empty())
        return;
    for (int i = 0; i < m_triangle_selectors.size(); i++) {
        TriangleSelectorPatch* selector = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[i].get());
        if (!selector) continue;
        int extruder_idx = m_volumes_extruder_idxs[i];
        int extruder_color_idx = std::max(0, extruder_idx - 1);
        extruder_color_idx = std::min(extruder_color_idx, int(m_extruders_colors.size()) - 1);
        std::vector<ColorRGBA> ebt_colors;
        ebt_colors.push_back(m_extruders_colors[extruder_color_idx]);
        ebt_colors.insert(ebt_colors.end(), m_extruders_colors.begin(), m_extruders_colors.end());
        selector->set_ebt_colors(ebt_colors);
    }
}

void GLGizmoMmuSegmentation::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    // Extruder colors need to be reloaded before calling init_model_triangle_selectors to render painted triangles
    // using colors from loaded 3MF and not from printer profile in Slicer.
    const std::vector<ColorRGBA> current_extruder_colors = get_extruders_colors();
    if (int prev_extruders_count = int(m_extruders_colors.size());
        prev_extruders_count != int(current_extruder_colors.size()) || current_extruder_colors != m_extruders_colors)
        this->init_extruders_data(current_extruder_colors);

    this->init_model_triangle_selectors();
}

void GLGizmoMmuSegmentation::tool_changed(wchar_t old_tool, wchar_t new_tool)
{
    if ((old_tool == ImGui::GapFillIcon && new_tool == ImGui::GapFillIcon) ||
        (old_tool != ImGui::GapFillIcon && new_tool != ImGui::GapFillIcon))
        return;

    for (auto& selector_ptr : m_triangle_selectors) {
        TriangleSelectorPatch* tsp = dynamic_cast<TriangleSelectorPatch*>(selector_ptr.get());
        if (!tsp) continue;
        tsp->set_filter_state(new_tool == ImGui::GapFillIcon);
    }
}

PainterGizmoType GLGizmoMmuSegmentation::get_painter_type() const
{
    return PainterGizmoType::MM_SEGMENTATION;
}

// BBS
ColorRGBA GLGizmoMmuSegmentation::get_cursor_hover_color() const
{
    if (m_selected_extruder_idx < m_display_filament_ids.size()) {
        const unsigned int actual_filament_id = m_display_filament_ids[m_selected_extruder_idx];
        if (actual_filament_id >= 1 && actual_filament_id <= m_extruders_colors.size())
            return m_extruders_colors[actual_filament_id - 1];
    }
    return m_extruders_colors.empty() ? ColorRGBA() : m_extruders_colors[0];
}

void GLGizmoMmuSegmentation::on_set_state()
{
    GLGizmoPainterBase::on_set_state();

    if (get_state() == Off) {
        ModelObject* mo = m_c->selection_info()->model_object();
        if (mo) Slic3r::save_object_mesh(*mo);
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_FORCE_UPDATE));
    }
}

wxString GLGizmoMmuSegmentation::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    wxString action_name;
    if (shift_down)
        action_name = _L("Remove painted color");
    else {
        action_name        = GUI::format(_L("Painted using: Filament %1%"), m_selected_extruder_idx + 1);
    }
    return action_name;
}

void GLMmSegmentationGizmo3DScene::release_geometry() {
    if (this->vertices_VBO_id) {
        glsafe(::glDeleteBuffers(1, &this->vertices_VBO_id));
        this->vertices_VBO_id = 0;
    }
    for(auto &triangle_indices_VBO_id : triangle_indices_VBO_ids) {
        glsafe(::glDeleteBuffers(1, &triangle_indices_VBO_id));
        triangle_indices_VBO_id = 0;
    }

    this->clear();
}

void GLMmSegmentationGizmo3DScene::render(size_t triangle_indices_idx) const
{
    assert(triangle_indices_idx < this->triangle_indices_VBO_ids.size());
    assert(this->triangle_patches.size() == this->triangle_indices_VBO_ids.size());
    assert(this->vertices_VBO_id != 0);
    assert(this->triangle_indices_VBO_ids[triangle_indices_idx] != 0);

    GLShaderProgram* shader = wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    // the following binding is needed to set the vertex attributes
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->vertices_VBO_id));
    const GLint position_id = shader->get_attrib_location("v_position");
    if (position_id != -1) {
        glsafe(::glVertexAttribPointer(position_id, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (GLvoid*)0));
        glsafe(::glEnableVertexAttribArray(position_id));
    }

    // Render using the Vertex Buffer Objects.
    if (this->triangle_indices_VBO_ids[triangle_indices_idx] != 0 &&
        this->triangle_indices_sizes[triangle_indices_idx] > 0) {
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->triangle_indices_VBO_ids[triangle_indices_idx]));
        glsafe(::glDrawElements(GL_TRIANGLES, GLsizei(this->triangle_indices_sizes[triangle_indices_idx]), GL_UNSIGNED_INT, nullptr));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    }

    if (position_id != -1)
        glsafe(::glDisableVertexAttribArray(position_id));

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
}

void GLMmSegmentationGizmo3DScene::finalize_vertices()
{
    assert(this->vertices_VBO_id == 0);
    if (!this->vertices.empty()) {
        glsafe(::glGenBuffers(1, &this->vertices_VBO_id));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->vertices_VBO_id));
        glsafe(::glBufferData(GL_ARRAY_BUFFER, this->vertices.size() * sizeof(float), this->vertices.data(), GL_STATIC_DRAW));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
        this->vertices.clear();
    }
}

void GLMmSegmentationGizmo3DScene::finalize_triangle_indices()
{
    triangle_indices_VBO_ids.resize(this->triangle_patches.size());
    triangle_indices_sizes.resize(this->triangle_patches.size());
    assert(std::all_of(triangle_indices_VBO_ids.cbegin(), triangle_indices_VBO_ids.cend(), [](const auto &ti_VBO_id) { return ti_VBO_id == 0; }));

    for (size_t buffer_idx = 0; buffer_idx < this->triangle_patches.size(); ++buffer_idx) {
        std::vector<int>& triangle_indices = this->triangle_patches[buffer_idx].triangle_indices;
        triangle_indices_sizes[buffer_idx] = triangle_indices.size();
        if (!triangle_indices.empty()) {
            glsafe(::glGenBuffers(1, &this->triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, triangle_indices.size() * sizeof(int), triangle_indices.data(), GL_STATIC_DRAW));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
            triangle_indices.clear();
        }
    }
}

// Pro Mode section: brush precision (F3), paint-perimeters-only (F1),
// extra walls on painted regions (F2), rectangle/polygon masks (F4). All convenience wrappers
// over engine options
// for the investigation behind them.
void GLGizmoMmuSegmentation::render_pro_mode_section(float sliders_left_width, float sliders_width, float slider_icon_width)
{
    const float drag_left_width = ImGui::GetStyle().WindowPadding.x + sliders_width - m_imgui->get_style_scaling() * 8;

    // --- F3: brush precision (finer triangle subdivision at the paint boundary) ---
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Precision"));
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(sliders_width);
    m_imgui->bbl_slider_float_style("##precision_factor", &m_precision_factor, PrecisionFactorMin, PrecisionFactorMax, "%.0fx", 1.0f, true);
    ImGui::SameLine(drag_left_width + sliders_left_width);
    ImGui::PushItemWidth(1.5f * slider_icon_width);
    ImGui::BBLDragFloat("##precision_factor_input", &m_precision_factor, 0.5f, PrecisionFactorMin, PrecisionFactorMax, "%.0fx");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Subdivides the mesh more finely while painting, reducing the "
            "staircase look at the edge of a stroke. Higher values cost more memory/CPU on dense meshes.").c_str());

    ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;

    // --- F1: paint perimeters only (reuses the existing mmu_segmented_region_max_width,
    // see cut_segmented_layers() in MultiMaterialSegmentation.cpp) ---
    bool perimeters_only = mo && mo->config.has("mmu_segmented_region_max_width")
        && mo->config.opt_float("mmu_segmented_region_max_width") > 0.0;

    // the painted region is its own LayerRegion, clipped to this
    // width by cut_segmented_layers() *before* Arachne runs; Arachne then only has room to
    // draw as many loops as fit in that width, regardless of wall_loops. So the ring must be
    // sized for wall_loops + extra_walls, not just wall_loops, or F2 "Extra walls" has no
    // physical space to draw into while F1 is active.
    // per-color override helpers (ConfigOptionInts indexed by 1-based
    // paint slot id; entry 0 unused; value 0 = "use the global scalar"). Shared by "Extra walls"
    // and "Surface depth" below.
    auto per_color_get = [mo](const char *key, int state) -> int {
        if (mo == nullptr || !mo->config.has(key))
            return 0;
        const auto *opt = static_cast<const ConfigOptionInts *>(mo->config.option(key));
        if (opt == nullptr || state < 0 || state >= int(opt->values.size()))
            return 0;
        return opt->values[state];
    };
    auto per_color_set = [mo](const char *key, int state, int value) {
        if (mo == nullptr || state <= 0)
            return;
        std::vector<int> vals;
        if (mo->config.has(key))
            if (const auto *opt = static_cast<const ConfigOptionInts *>(mo->config.option(key)); opt != nullptr)
                vals = opt->values;
        if (int(vals.size()) <= state)
            vals.resize(state + 1, 0);
        vals[state] = value;
        if (std::all_of(vals.begin(), vals.end(), [](int v) { return v == 0; }))
            mo->config.erase(key);
        else
            mo->config.set_key_value(key, new ConfigOptionInts(vals));
    };
    auto per_color_max = [mo](const char *key) -> int {
        if (mo == nullptr || !mo->config.has(key))
            return 0;
        const auto *opt = static_cast<const ConfigOptionInts *>(mo->config.option(key));
        int out = 0;
        if (opt != nullptr)
            for (int v : opt->values)
                out = std::max(out, v);
        return out;
    };
    auto perimeters_only_width = [mo, &per_color_max]() -> double {
        const DynamicPrintConfig &print_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        const double nozzle = wxGetApp().preset_bundle->printers.get_edited_preset().config.opt_float("nozzle_diameter", 0);
        double line_width = print_cfg.get_abs_value("line_width", nozzle);
        if (line_width <= 0.0)
            line_width = nozzle > 0.0 ? nozzle : 0.4;
        const int wall_loops = std::max(1, print_cfg.opt_int("wall_loops"));
        int extra_walls = (mo && mo->config.has("mmu_segmented_region_extra_walls"))
            ? mo->config.opt_int("mmu_segmented_region_extra_walls") : 0;
        // Per-color overrides: the ring must fit the color with the MOST walls.
        extra_walls = std::max(extra_walls, per_color_max("mmu_segmented_region_extra_walls_per_color"));
        return (wall_loops + extra_walls) * line_width;
    };

    if (ImGui::Checkbox(_u8L("Paint perimeters only").c_str(), &perimeters_only) && mo) {
        wxGetApp().plater()->take_snapshot("Painter: perimeters only");
        if (perimeters_only)
            mo->config.set_key_value("mmu_segmented_region_max_width", new ConfigOptionFloat(perimeters_only_width()));
        else
            mo->config.erase("mmu_segmented_region_max_width");
        wxGetApp().plater()->update();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Limits the painted color to a ring near the region's contour "
            "(about wall count x line width) instead of filling the whole selected area. Reduces "
            "wasted filament and color changes in solid infill.").c_str());

    // --- Per-color mode toggle: OFF = the two global inputs below; ON = a compact per-color
    // table (one row per filament: color swatch + Walls + Depth), so the user sees every
    // assignment at a glance instead of a hidden value tied to the current paint selection. ---
    ImGui::Checkbox(_u8L("Per color").c_str(), &m_per_color_values);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Assign \"Extra walls\" and \"Surface depth\" per paint color "
            "in a table (0 = use the global value). When disabled, one global value applies to "
            "all painted colors.").c_str());

    if (!m_per_color_values) {
        // --- F2: extra walls on painted regions only (mmu_segmented_region_extra_walls,
        // see generate_print_object_regions() in PrintApply.cpp) ---
        int extra_walls = (mo && mo->config.has("mmu_segmented_region_extra_walls"))
            ? mo->config.opt_int("mmu_segmented_region_extra_walls") : 0;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Extra walls"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        if (ImGui::InputInt("##mmu_extra_walls", &extra_walls, 1, 1) && mo) {
            extra_walls = std::clamp(extra_walls, 0, 8);
            wxGetApp().plater()->take_snapshot("Painter: extra walls");
            if (extra_walls > 0)
                mo->config.set_key_value("mmu_segmented_region_extra_walls", new ConfigOptionInt(extra_walls));
            else
                mo->config.erase("mmu_segmented_region_extra_walls");
            // Widen the perimeters-only ring so the extra loops have room to be drawn.
            if (perimeters_only)
                mo->config.set_key_value("mmu_segmented_region_max_width", new ConfigOptionFloat(perimeters_only_width()));
            wxGetApp().plater()->update();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Adds this many extra perimeter walls to the painted region only, "
                "on top of the object's normal wall count. 0 = disabled.").c_str());

        // --- Idea A: painted-surface depth (mmu_segmented_region_surface_depth, see
        // apply_painted_surface_depth() in PrintObject.cpp) ---
        int surface_depth = (mo && mo->config.has("mmu_segmented_region_surface_depth"))
            ? mo->config.opt_int("mmu_segmented_region_surface_depth") : 0;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Surface depth"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        if (ImGui::InputInt("##mmu_surface_depth", &surface_depth, 1, 1) && mo) {
            surface_depth = std::clamp(surface_depth, 0, 20);
            wxGetApp().plater()->take_snapshot("Painter: surface depth");
            if (surface_depth > 0)
                mo->config.set_key_value("mmu_segmented_region_surface_depth", new ConfigOptionInt(surface_depth));
            else
                mo->config.erase("mmu_segmented_region_surface_depth");
            wxGetApp().plater()->update();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Extends a painted top/bottom surface this many layers into the "
                "object as solid infill of the painted color, following the painted silhouette. "
                "0 = disabled.").c_str());
    } else {
        // --- Per-color table: swatch + Walls + Depth per filament, laid out in TWO column
        // groups (entries alternate left/right) so long filament lists don't push the rest of
        // the panel down. Clicking a swatch also selects that color for painting (same id space
        // as the strip at the top). Input width is sized so the number stays visible next to
        // ImGui's -/+ step buttons. ---
        const float swatch_size  = ImGui::GetFrameHeight();
        const float input_width  = m_imgui->scaled(4.6f);
        const float col_gap      = m_imgui->scaled(0.6f);
        const float base_x       = ImGui::GetCursorPosX();
        const float group_width  = swatch_size + col_gap + (input_width + col_gap) * 2.f;
        const float group_x[2]   = { base_x, base_x + group_width + m_imgui->scaled(1.2f) };
        auto walls_x = [&](int g) { return group_x[g] + swatch_size + col_gap; };
        auto depth_x = [&](int g) { return walls_x(g) + input_width + col_gap; };

        // Header row (the leading blank anchors the line so SameLine targets it, not the checkbox).
        ImGui::AlignTextToFramePadding();
        m_imgui->text(" ");
        for (int g = 0; g < 2; ++g) {
            ImGui::SameLine(walls_x(g));
            m_imgui->text(_L("Walls"));
            ImGui::SameLine(depth_x(g));
            m_imgui->text(_L("Depth"));
        }

        auto render_per_color_entry = [&](size_t row_idx, int g) {
            const unsigned int slot_id = m_display_filament_ids[row_idx];
            if (slot_id == 0 || slot_id > m_extruders_colors.size())
                return;
            const ImVec4 color_vec = ImGuiWrapper::to_ImVec4(m_extruders_colors[slot_id - 1]);
            const std::string row_tag = std::to_string(row_idx);

            // Swatch: highlight the currently selected paint color, click to select it.
            if (g > 0)
                ImGui::SameLine(group_x[g]);
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoTooltip;
            if (row_idx != m_selected_extruder_idx)
                flags |= ImGuiColorEditFlags_NoBorder;
            if (ImGui::ColorButton(("##pcv_color_" + row_tag).c_str(), color_vec, flags, ImVec2(swatch_size, swatch_size)))
                m_selected_extruder_idx = row_idx;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s %u", _u8L("Filament").c_str(), unsigned(row_idx + 1));

            // Walls input.
            ImGui::SameLine(walls_x(g));
            ImGui::PushItemWidth(input_width);
            int row_walls = per_color_get("mmu_segmented_region_extra_walls_per_color", int(slot_id));
            if (ImGui::InputInt(("##pcv_walls_" + row_tag).c_str(), &row_walls, 1, 1) && mo) {
                row_walls = std::clamp(row_walls, 0, 8);
                wxGetApp().plater()->take_snapshot("Painter: extra walls per color");
                per_color_set("mmu_segmented_region_extra_walls_per_color", int(slot_id), row_walls);
                if (perimeters_only)
                    mo->config.set_key_value("mmu_segmented_region_max_width", new ConfigOptionFloat(perimeters_only_width()));
                wxGetApp().plater()->update();
            }
            ImGui::PopItemWidth();

            // Depth input.
            ImGui::SameLine(depth_x(g));
            ImGui::PushItemWidth(input_width);
            int row_depth = per_color_get("mmu_segmented_region_surface_depth_per_color", int(slot_id));
            if (ImGui::InputInt(("##pcv_depth_" + row_tag).c_str(), &row_depth, 1, 1) && mo) {
                row_depth = std::clamp(row_depth, 0, 20);
                wxGetApp().plater()->take_snapshot("Painter: surface depth per color");
                per_color_set("mmu_segmented_region_surface_depth_per_color", int(slot_id), row_depth);
                wxGetApp().plater()->update();
            }
            ImGui::PopItemWidth();
        };

        const size_t n_rows = std::min(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT, m_display_filament_ids.size());
        for (size_t row_idx = 0; row_idx < n_rows; row_idx += 2) {
            render_per_color_entry(row_idx, 0);
            if (row_idx + 1 < n_rows)
                render_per_color_entry(row_idx + 1, 1);
        }
        m_imgui->text(_L("0 = use global value"));
    }

}

// B / V. Leaving Vector keeps the shape or SVG being edited: it only stops
// being drawn and grabbed until Vector comes back.
// Clay mode. One row per decal: raise it
// or not, its height, edge and falloff, paint under, Build / Remove. The parts are normal parts of
// the object named "Clay · <decal> · <n>", so they can be detached or deleted like any other.
void GLGizmoMmuSegmentation::render_clay_panel()
{
    neo_ui::Compact compact;
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    std::vector<PaintDecal> &decals = mo->paint_decals;
    if (decals.empty()) {
        m_imgui->text_wrapped(_L("No decals yet. Make one in Vector (a shape or an SVG), then come back here to raise it."),
                              ImGui::GetFontSize() * 18.f);
        return;
    }
    if (m_selected_decal < 0 || m_selected_decal >= int(decals.size()))
        m_selected_decal = 0;

    render_decal_layers(true);
    if (m_selected_decal < 0 || m_selected_decal >= int(decals.size()))
        return;

    PaintDecal &d = decals[size_t(m_selected_decal)];
    PaintDecalClay c = d.clay;
    bool changed = false, released = false;
    auto tip = [](const std::string &t) { if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t.c_str()); };
    auto let_go = [&released]() { released |= ImGui::IsItemDeactivatedAfterEdit(); };

    // --- Clay: the shape of the part
    if (neo_ui::section(_u8L("Clay") + "###clay_main", true)) {
        float h = float(c.height);
        neo_ui::row(_u8L("Height"));
        if (ImGui::SliderFloat("##height", &h, 0.04f, 5.f, "%.2f mm")) {
            // Scales every colour: the ones with their own height keep their proportion to it.
            const double h1 = std::max(0.04, double(h)), k = h1 / std::max(c.height, 1e-9);
            for (double &gh : c.group_height)
                if (gh > 0.)
                    gh = std::clamp(gh * k, 0.04, 20.);
            c.height = h1;
            changed  = true;
        }
        let_go();
        tip(_u8L("How much the decal stands out of the surface.\n0.4 mm is two layers of 0.2.\n"
            "Colours with their own height below scale with it."));
        // Edge: the stored order is Flat, Chamfer, Round, Dome.
        int pf = std::clamp(c.profile, 0, 3);
        neo_ui::row(_u8L("Edge"));
        if (neo_ui::segmented("edge", { _u8L("Flat"), _u8L("Chamfer"), _u8L("Round"), _u8L("Dome") }, pf)) {
            c.profile = pf;
            changed   = true;
            released  = true;
        }
        tip(_u8L("The side of the raised part:\n"
            "Flat: straight wall.\nChamfer: a slope over the falloff.\n"
            "Round: a quarter circle over the falloff.\nDome: the whole shape rounded, highest in the middle."));
        if (pf == 1 || pf == 2) {
            float f = float(c.falloff);
            neo_ui::row(_u8L("Falloff"));
            if (ImGui::SliderFloat("##falloff", &f, 0.1f, 5.f, "%.2f mm")) {
                c.falloff = std::max(0.1, double(f));
                changed   = true;
            }
            let_go();
            tip(_u8L("How wide the sloped or rounded edge is."));
        }
        float ss = float(c.smooth_surface);
        neo_ui::row(_u8L("Surface smooth"));
        if (ImGui::SliderFloat("##ssmooth", &ss, 0.f, 3.f, ss > 0.f ? "%.2f mm" : "off", ImGuiSliderFlags_Logarithmic)) {   // half the travel under ~0.2 mm
            c.smooth_surface = std::max(0., double(ss) < 0.05 ? 0. : double(ss));
            changed          = true;
        }
        let_go();
        tip(_u8L("Raises the decal from a smoothed copy of the surface:\n"
            "grooves fill and bumps (veins, edges) sink into the part,\n"
            "so its top comes out clean.\n"
            "Off: the top copies the surface as it is.\n"
            "About the width of the detail to hide: 1 mm hides small veins.\n"
            "The top always stays at least 0.4 mm over the real surface."));
        int gr = c.straight ? 0 : 1;
        neo_ui::row(_u8L("Growth"));
        if (neo_ui::segmented("growth", { _u8L("Straight"), _u8L("Normals") }, gr)) {
            c.straight = gr == 0;
            changed    = true;
            released   = true;
        }
        tip(_u8L("Straight: the part grows out along the view the decal was placed from, like a stamp.\n"
            "Over grooves and sharp edges it copies their shape but cannot fold.\n"
            "Normals: it grows along the surface; over a groove the\n"
            "top can fold and tear. Smooth then evens out that direction."));
        if (!c.straight) {
            int sm = std::clamp(c.smooth, 0, 30);
            neo_ui::row(_u8L("Smooth"));
            if (ImGui::SliderInt("##nsmooth", &sm, 0, 30)) {
                c.smooth = sm;
                changed  = true;
            }
            let_go();
            tip(_u8L("Evens out the direction the top is raised along.\n"
                "0: it follows every bump of the surface under it.\n"
                "Higher: the bumps fade and the top gets smoother."));
        }
    }

    // --- Advanced growth (Straight only)
    if (neo_ui::section(_u8L("Advanced growth") + "###clay_adv", false)) {
        // Shown in Normals too (greyed): hidden, Neotko couldn't find it.
        if (!c.straight)
            m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _u8L("Only with Growth: Straight."));
        m_imgui->disabled_begin(!c.straight);
        float fs = float(c.follow_surface), st = float(c.stretch), sk = float(c.stretch_slope), mt = float(c.min_thick);
        neo_ui::row(_u8L("Min thickness"));
        if (ImGui::SliderFloat("##minthick", &mt, 0.f, 1.5f, mt > 0.f ? "%.2f mm" : "off")) {
            c.min_thick = double(mt) < 0.05 ? 0. : double(mt);
            changed     = true;
        }
        let_go();
        tip(_u8L("Least thickness of the part, measured out of the surface.\n"
            "Where the object is seen edge-on from the decal's view, growing towards\n"
            "the view makes a sliver thinner than a line: the slicer drops it and\n"
            "it looks as if the paint failed. Keep it at your line width (0.42)."));
        neo_ui::row(_u8L("Follow surface"));
        if (ImGui::SliderFloat("##follow", &fs, 0.f, 30.f, fs > 0.f ? "%.1f mm" : "off", ImGuiSliderFlags_Logarithmic)) {
            c.follow_surface = double(fs) < 0.2 ? 0. : double(fs);
            changed          = true;
        }
        let_go();
        tip(_u8L("Off: the part grows towards the view the decal was placed from.\n"
            "On: it grows out of the surface, its direction averaged over this radius.\n"
            "Small: follows the shape (a nose gets a cap). Large: close to off.\n"
            "Keep it about twice the height or more, or neighbours' tops can cross."));
        neo_ui::row(_u8L("Stretch gaps"));
        if (ImGui::SliderFloat("##stretch", &st, 0.f, 20.f, st > 0.f ? "%.2f mm" : "off", ImGuiSliderFlags_Logarithmic)) {
            c.stretch = double(st) < 0.05 ? 0. : double(st);
            changed   = true;
        }
        let_go();
        tip(_u8L("Where the surface drops away from the view (an eye socket's rim),\n"
            "the top stretches down over the drop like a rubber sheet,\n"
            "instead of splitting in two layers with dark slits.\n"
            "The value is the deepest drop it bridges; deeper ones stay split.\n"
            "It adds material under the stretch."));
        if (c.stretch > 0.) {
            neo_ui::row(_u8L("Stretch slope"));
            if (ImGui::SliderFloat("##slope", &sk, 0.2f, 3.f, "%.2f")) {
                c.stretch_slope = std::clamp(double(sk), 0.2, 3.);
                changed         = true;
            }
            let_go();
            tip(_u8L("How steeply the stretched top falls: 1 = 45 degrees.\n"
                "Lower: a gentler, longer slope (more material)."));
        }
        m_imgui->disabled_end();
    }

    // --- Height per colour: one row per colour the decal keeps (parts removed before applying are not listed).
    {
        std::ostringstream key;
        key << d.svg_data.size() << '|' << std::hash<std::string>{}(d.svg_data) << '|' << d.zones.size() << '|' << d.parts.size();
        for (const PaintDecalZone &z : d.zones) key << ',' << z.zone << ':' << z.rgb << ':' << z.state << z.hidden;
        for (const PaintDecalPart &pt : d.parts) key << ';' << pt.shape << ':' << pt.state << pt.hidden;
        if (key.str() != m_clay_groups_key) {
            m_clay_groups_key = key.str();
            m_clay_groups     = paint_decal_groups(d.svg_data, d.zones, d.parts);
        }
    }
    if (neo_ui::section(_u8L("Height per colour") + "###clay_colours", true)) {
        tip(_u8L("Each colour can stand out its own height.\nUntil you move it, a colour uses the Height above."));
        const float fh     = ImGui::GetFrameHeight();
        const float mode_w = ImGui::GetFontSize() * 7.f;
        const float reset_w = m_imgui->calc_button_size(_L("Reset")).x;
        const float x0     = ImGui::GetCursorPosX();
        for (size_t g = 0; g < m_clay_groups.groups.size(); ++g) {
            const PaintDecalZone &z = m_clay_groups.groups[g];
            if (z.hidden)
                continue;
            ImGui::PushID(int(g) + 7000);
            const ColorRGBA col = (z.state >= 1 && size_t(z.state) <= m_extruders_colors.size()) ? m_extruders_colors[size_t(z.state - 1)] : ColorRGBA::GRAY();
            ImGui::ColorButton("##sw", ImVec4(col.r(), col.g(), col.b(), 1.f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker, ImVec2(fh, fh));
            ImGui::SameLine();
            int md = int(c.mode_of(g));
            if (neo_ui::segmented("gm", { _u8L("Raise"), _u8L("Engrave"), _u8L("Off") }, md, mode_w * 1.7f)) {
                if (c.group_mode.size() <= g)
                    c.group_mode.resize(g + 1, 0);
                c.group_mode[g] = md;
                changed  = true;
                released = true;
            }
            tip(_u8L("Raise: the colour stands out of the surface.\n"
                "Engrave: it is sunk into the object, as deep as its height\n(a negative part: the slicer cuts it out).\n"
                "Off: no part for this colour."));
            if (md != PaintDecalClay::Off) {
                ImGui::SameLine();
                const bool own = g < c.group_height.size() && c.group_height[g] > 0.;
                float gh = float(c.height_of(g));
                // what is left of the row, keeping room for Reset so the row never widens the panel
                const float used = ImGui::GetCursorPosX() - x0;
                ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 4.f, neo_ui::panel_w() - used - reset_w - ImGui::GetStyle().ItemSpacing.x));
                if (ImGui::SliderFloat("##gh", &gh, 0.04f, 5.f, own ? "%.2f mm" : "%.2f mm (=)")) {
                    if (c.group_height.size() <= g)
                        c.group_height.resize(g + 1, 0.);
                    c.group_height[g] = std::max(0.04, double(gh));
                    changed = true;
                }
                let_go();
                if (own) {
                    ImGui::SameLine();
                    if (m_imgui->button(_L("Reset"))) {
                        c.group_height[g] = 0.;
                        changed  = true;
                        released = true;
                    }
                    tip(_u8L("Back to the Height above."));
                }
            }
            ImGui::PopID();
        }
        if (std::find(c.group_mode.begin(), c.group_mode.end(), int(PaintDecalClay::Engrave)) != c.group_mode.end()) {
            if (ImGui::Checkbox(_u8L("Anchor raise to engrave").c_str(), &c.anchor)) {
                changed  = true;
                released = true;
            }
            tip(_u8L("Where a raised colour sits over an engraved one, it reaches down\n"
                "to the engrave floor (+0.1 mm), so it doesn't float once\n"
                "the engrave is applied. Off: it stays on the surface."));
        }
    }

    ImGui::Checkbox(_u8L("Repair parts").c_str(), &m_clay_repair);
    tip(_u8L("After Build, rebuild each part on fine voxels (0.1 mm) as one clean solid.\n"
        "Where the top folds over itself the slicer leaves holes (black gaps):\n"
        "this closes them. A few seconds more per part."));

    // --- View
    if (neo_ui::section(_u8L("View") + "###clay_view", false)) {
        if (ImGui::Checkbox(_u8L("Show decal").c_str(), &m_decal_preview_colours))
            m_parent.set_as_dirty();
        tip(_u8L("Draw the decal's colours over the object, see-through.\n"
            "Off: only its outline, so the raised parts are seen as they are."));
        ImGui::SameLine(neo_ui::panel_w() * 0.5f);
        if (ImGui::Checkbox(_u8L("X-ray engrave").c_str(), &m_clay_xray_on))
            m_parent.set_as_dirty();
        tip(_u8L("Show the engrave parts in red, see-through, inside the object:\n"
            "the shape of the hole Apply engrave (or the slicer) will cut."));
        if (ImGui::Checkbox(_u8L("Growth arrow").c_str(), &m_clay_arrow_on))
            m_parent.set_as_dirty();
        tip(_u8L("Show on the object where the parts grow:\n"
            "towards the view the decal was placed from (Straight growth)."));
        ImGui::SameLine(neo_ui::panel_w() * 0.5f);
        if (ImGui::Checkbox(_u8L("Paint under").c_str(), &c.paint_under)) {
            changed  = true;
            released = true;
        }
        tip(_u8L("Also paint the decal's colours on the object under the raised part,\n"
            "so the part melts into the same colour when printed."));
    }
    ImGui::Separator();
    if (changed) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Clay settings"), UndoRedo::SnapshotType::GizmoAction);
        d.clay = c;
    }

    const bool built = std::any_of(mo->volumes.begin(), mo->volumes.end(), [&d](const ModelVolume *v) { return paint_clay_is_volume_of(*v, d); });
    // Live, like the Editor: a built decal is rebuilt when a control is let go (a build already
    // running is cancelled and started again with the new values).
    if (released && built && !d.locked) {
        clay_build(size_t(m_selected_decal));
        return;
    }
    clay_poll();
    if (m_clay_job) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_clay_job_start).count();
        const int    tot  = m_clay_job->total.load();
        char buf[64];
        snprintf(buf, sizeof(buf), "  %d / %d  ·  %.1f s", std::min(m_clay_job->done.load() + 1, std::max(tot, 1)), std::max(tot, 1), secs);
        m_imgui->text(_u8L("Building mesh...") + buf);
        if (m_imgui->button(_L("Cancel")))
            clay_cancel();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Stop building. The parts it had stay as they were.").c_str());
        m_parent.schedule_extra_frame(100);   // keep the counter moving and pick up the result
        return;
    }
    m_imgui->disabled_begin(d.locked);
    if (m_imgui->button(built ? _L("Rebuild") : _L("Build")))
        clay_build(size_t(m_selected_decal));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Make the raised parts of this decal, one per visible colour,\n"
            "and replace the ones it had. Each part prints with its colour's filament.").c_str());
    {
        std::vector<std::string> all;   // every decal of the object that has Clay parts
        if (const ModelObject *cmo = m_c->selection_info()->model_object())
            for (const PaintDecal &pd : cmo->paint_decals)
                if (!pd.locked && std::any_of(cmo->volumes.begin(), cmo->volumes.end(), [&pd](const ModelVolume *v) { return paint_clay_is_volume_of(*v, pd); }))
                    all.push_back(pd.name);
        if (all.size() > 1) {
            ImGui::SameLine();
            if (m_imgui->button(_L("Rebuild all")))
                m_clay_queue = std::move(all);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Rebuild the Clay parts of every decal of this object, one after the other.\n"
                    "Do it before Apply engrave: over a hole already cut an engrave folds.").c_str());
        }
    }
    if (built) {
        ImGui::SameLine();
        if (m_imgui->button(_L("Remove"))) {
            const int n = clay_remove_parts(size_t(m_selected_decal));
            m_clay_status = std::to_string(n) + " " + _u8L("Clay parts removed.");
        }
        bool engraved = false, raised = false;
        if (const ModelObject *cmo = m_c->selection_info()->model_object())
            for (const ModelVolume *v : cmo->volumes)
                if (paint_clay_is_volume_of(*v, d))
                    (v->is_negative_volume() ? engraved : raised) = true;
        int engraves = 0;   // every decal's: Apply cuts them all together
        if (const ModelObject *cmo = m_c->selection_info()->model_object())
            for (const ModelVolume *v : cmo->volumes)
                if (v->is_negative_volume() && paint_clay_is_volume(*v))
                    ++engraves;
        if (engraved) {
            ImGui::SameLine();
            if (m_imgui->button(engraves > 1 ? _L("Apply all engraves") : _L("Apply engrave"))) {
                if (clay_apply_engrave(size_t(m_selected_decal)))
                    m_clay_status = _u8L("Engrave applied: the hole is in the object now.");
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Cut the engraved colours out of the object for good\n"
                    "(the painting stays) and delete the engrave parts.\n"
                    "All the decals' engraves go together: built over the uncut\n"
                    "surface they add up where they touch. Rebuilding one over\n"
                    "a hole already cut would fold it.\n"
                    "Not needed to print: the slicer already cuts them.").c_str());
        }
        if (raised)
            ImGui::SameLine();
        if (raised && m_imgui->button(_L("Detach"))) {
            const int n = clay_detach_parts(size_t(m_selected_decal));
            m_clay_status = std::to_string(n) + " " + _u8L("parts detached: they are normal parts now.");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Keep these parts as normal parts of the object.\n"
                "Rebuild and Remove will not touch them any more.").c_str());
    }
    m_imgui->disabled_end();
    if (!m_clay_status.empty())
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, from_u8(m_clay_status));
    if (!m_clay_warning.empty())
        m_imgui->text_wrapped(m_clay_warning, ImGui::GetFontSize() * 18.f);
}

int GLGizmoMmuSegmentation::clay_remove_parts(size_t idx)
{
    ModelObject *mo  = m_c->selection_info()->model_object();
    const int    oid = m_parent.get_selection().get_object_idx();
    if (mo == nullptr || oid < 0 || idx >= mo->paint_decals.size())
        return 0;
    int removed = 0;
    for (int i = int(mo->volumes.size()) - 1; i >= 0; --i)
        if (paint_clay_is_volume_of(*mo->volumes[size_t(i)], mo->paint_decals[idx]) && mo->volumes.size() > 1) {
            mo->delete_volume(size_t(i));
            wxGetApp().obj_list()->delete_volume_from_list(size_t(oid), size_t(i));
            ++removed;
        }
    if (removed > 0) {
        mo->invalidate_bounding_box();
        m_schedule_update = true;
        wxGetApp().plater()->changed_object(oid);
        m_parent.set_as_dirty();
    }
    return removed;
}

void GLGizmoMmuSegmentation::clay_watch_decals()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    // Outside Clay nobody else picks the result up.
    if (m_paint_mode != PaintMode::Clay) {
        clay_poll();
        if (m_clay_job)
            m_parent.schedule_extra_frame(100);
    }
    if (mo->id() != m_clay_watch_object) {
        m_clay_watch.clear();
        m_clay_watch_object = mo->id();
    }
    if (!m_clay_queue.empty()) {
        if (m_clay_job) {
            m_parent.schedule_extra_frame(100);
            return;
        }
        const std::string name = m_clay_queue.front();
        m_clay_queue.erase(m_clay_queue.begin());
        for (size_t i = 0; i < mo->paint_decals.size(); ++i)
            if (mo->paint_decals[i].name == name && !mo->paint_decals[i].locked) {
                clay_build(i);
                break;
            }
        m_parent.schedule_extra_frame(100);
        return;
    }
    const bool mouse_down = wxGetMouseState().LeftIsDown();
    for (size_t i = 0; i < mo->paint_decals.size(); ++i) {
        PaintDecal key = mo->paint_decals[i];
        key.clay   = PaintDecalClay();
        key.hidden = false;
        key.locked = false;
        auto it = m_clay_watch.find(key.name);
        if (it == m_clay_watch.end()) {
            m_clay_watch.emplace(key.name, std::move(key));
            continue;
        }
        if (it->second == key)
            continue;
        // Still dragging, or a build is running: look again next frame.
        if (mouse_down || m_clay_job)
            continue;
        it->second = std::move(key);
        const PaintDecal &d = mo->paint_decals[i];
        const bool built = std::any_of(mo->volumes.begin(), mo->volumes.end(), [&d](const ModelVolume *v) { return paint_clay_is_volume_of(*v, d); });
        if (built && !d.locked) {
            clay_build(i);
            return;   // one at a time; the others are found changed once this one is done
        }
    }
}

void GLGizmoMmuSegmentation::clay_cancel()
{
    if (m_clay_job) {
        // The worker owns its copy of the job and stops at its next check; nothing waits for it.
        m_clay_job->cancel = true;
        m_clay_job.reset();
        m_clay_status = _u8L("Build cancelled.");
    }
}

void GLGizmoMmuSegmentation::clay_build(size_t idx)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || idx >= mo->paint_decals.size() || mo->instances.empty())
        return;
    const bool rebuild = std::any_of(mo->volumes.begin(), mo->volumes.end(),
                                     [&](const ModelVolume *v) { return paint_clay_is_volume_of(*v, mo->paint_decals[idx]); });
    if (m_clay_job)
        m_clay_job->cancel = true;
    auto job           = std::make_shared<ClayJob>();
    m_clay_job         = job;
    m_clay_job_decal   = idx;
    m_clay_job_name    = mo->paint_decals[idx].name;
    m_clay_job_object  = mo->id();
    m_clay_job_start   = std::chrono::steady_clock::now();
    m_clay_job_rebuild = rebuild;
    m_clay_status.clear();
    m_clay_warning.clear();
    // Everything the worker reads is copied here: the model may change while it runs.
    std::thread([job, base = paint_clay_base(*mo), decal = mo->paint_decals[idx], centre = decal_mirror_centre(), repair = m_clay_repair]() {
        std::vector<PaintClayPiece> pieces;
        try {
            pieces = paint_clay_bake(base, decal, centre, [&job]() { return job->cancel.load(); },
                                     [&job](int done, int total) { job->done = done; job->total = total; });
            // Repair: where the top folds over itself the part crosses itself and the slicer empties
            // those spots (black gaps, 30-oct). One closed solid from fine voxels has no crossings.
            if (repair)
                for (PaintClayPiece &pc : pieces) {
                    if (job->cancel.load() || pc.its.indices.empty())
                        continue;
                    const auto   t0       = std::chrono::steady_clock::now();
                    const size_t tris_in  = pc.its.indices.size();
                    sla::RemeshParams rp;
                    rp.voxel      = 0.1;
                    rp.adaptivity = 0.05;
                    rp.cancel     = [&job]() { return job->cancel.load(); };
                    sla::RemeshResult rr = sla::neo_remesh(pc.its, rp);
                    if (rr.cancelled || rr.outer.indices.empty())
                        continue;
                    if (rr.outer.indices.size() > std::max<size_t>(tris_in, 20000))
                        its_quadric_edge_collapse(rr.outer, uint32_t(std::max<size_t>(tris_in, 20000)), nullptr, nullptr, nullptr);
                    std::ostringstream os;
                    os << "[CLAY] repair state=" << pc.state << " mirror=" << pc.mirror << " tris " << tris_in << " -> " << rr.outer.indices.size()
                       << " in " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s";
                    pc.its = std::move(rr.outer);
                }
        } catch (const std::exception &e) {
            pieces.clear();
        } catch (...) {   // anything else thrown in a detached thread ends the app
            pieces.clear();
        }
        job->pieces = std::move(pieces);
        job->finished = true;
    }).detach();
    m_parent.schedule_extra_frame(100);
}

// the engrave parts, red and see-through, over the object: where the hole will be.
void GLGizmoMmuSegmentation::render_clay_xray()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (!m_clay_xray_on || mo == nullptr || mo->instances.empty()) {
        m_clay_xray.clear();
        return;
    }
    std::vector<const ModelVolume *> tools;
    for (const ModelVolume *v : mo->volumes)
        if (v->is_negative_volume() && paint_clay_is_volume(*v))
            tools.push_back(v);
    if (tools.empty()) {
        m_clay_xray.clear();
        return;
    }
    // Cache by volume and triangle count: a rebuild makes a new volume, an edit changes the count.
    std::vector<ClayXray> keep;
    for (const ModelVolume *v : tools) {
        const size_t n  = v->mesh().its.indices.size();
        auto         it = std::find_if(m_clay_xray.begin(), m_clay_xray.end(), [&](const ClayXray &x) { return x.volume == v->id() && x.tris == n; });
        if (it != m_clay_xray.end())
            keep.push_back(std::move(*it));
        else {
            ClayXray x;
            x.volume = v->id();
            x.tris   = n;
            x.model  = std::make_unique<GLModel>();
            x.model->init_from(v->mesh().its);
            keep.push_back(std::move(x));
        }
    }
    m_clay_xray = std::move(keep);

    GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;
    shader->start_using();
    shader->set_uniform("emission_factor", 0.25f);
    const Camera      &camera = wxGetApp().plater()->get_camera();
    const Transform3d &view   = camera.get_view_matrix();
    const Transform3d  inst   = mo->instances[std::max(0, m_parent.get_selection().get_instance_idx())]->get_matrix();
    // Leave the GL state as found: it used to end with culling ON, and the Draw shape overlay drawn
    // right after lost its points and lines whenever an engrave part existed.
    const GLboolean was_cull = ::glIsEnabled(GL_CULL_FACE), was_depth = ::glIsEnabled(GL_DEPTH_TEST), was_blend = ::glIsEnabled(GL_BLEND);
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDepthMask(GL_FALSE));
    // Two passes: faint through the object (x-ray), stronger where it is in front.
    for (int pass = 0; pass < 2; ++pass) {
        if (pass == 0)
            glsafe(::glDisable(GL_DEPTH_TEST));
        else
            glsafe(::glEnable(GL_DEPTH_TEST));
        for (size_t i = 0; i < tools.size() && i < m_clay_xray.size(); ++i) {
            const Transform3d trafo = inst * tools[i]->get_matrix();
            shader->set_uniform("view_model_matrix", view * trafo);
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            const Matrix3d normal = view.matrix().block(0, 0, 3, 3) * trafo.matrix().block(0, 0, 3, 3).inverse().transpose();
            shader->set_uniform("view_normal_matrix", normal);
            m_clay_xray[i].model->set_color(pass == 0 ? ColorRGBA(1.f, 0.15f, 0.15f, 0.30f) : ColorRGBA(1.f, 0.2f, 0.2f, 0.45f));
            m_clay_xray[i].model->render();
        }
    }
    glsafe(::glDepthMask(GL_TRUE));
    if (was_depth) glsafe(::glEnable(GL_DEPTH_TEST)); else glsafe(::glDisable(GL_DEPTH_TEST));
    if (was_cull) glsafe(::glEnable(GL_CULL_FACE)); else glsafe(::glDisable(GL_CULL_FACE));
    if (was_blend) glsafe(::glEnable(GL_BLEND)); else glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

void GLGizmoMmuSegmentation::clay_poll()
{
    if (!m_clay_job || !m_clay_job->finished)
        return;
    std::shared_ptr<ClayJob> job = std::move(m_clay_job);
    ModelObject *mo = m_c->selection_info()->model_object();
    // Another object, or the decal is gone or renamed meanwhile: the result does not belong anywhere.
    if (job->cancel || mo == nullptr || mo->id() != m_clay_job_object || m_clay_job_decal >= mo->paint_decals.size() ||
        mo->paint_decals[m_clay_job_decal].name != m_clay_job_name)
        return;
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_clay_job_start).count();
    const int    n    = clay_finish(m_clay_job_decal, job->pieces);
    char buf[32];
    snprintf(buf, sizeof(buf), " (%.1f s)", secs);
    m_clay_status = n > 0 ? std::to_string(n) + " " + (m_clay_job_rebuild ? _u8L("Clay parts rebuilt.") : _u8L("Clay parts added.")) + buf
                          : _u8L("The decal does not touch the object from where it looks.");
}

int GLGizmoMmuSegmentation::clay_finish(size_t idx, const std::vector<PaintClayPiece> &pieces)
{
    ModelObject *mo  = m_c->selection_info()->model_object();
    const int    oid = m_parent.get_selection().get_object_idx();
    if (mo == nullptr || oid < 0 || idx >= mo->paint_decals.size() || mo->instances.empty())
        return 0;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Build Clay"), UndoRedo::SnapshotType::GizmoAction);
    mo->paint_decals[idx].clay.on = true;
    const PaintDecal decal = mo->paint_decals[idx];
    // Paint under first, while the painter still has the object as it is; then keep it in the model.
    if (decal.clay.paint_under)
        decal_apply({ idx }, false);
    update_model_object();

    // Relief only counts when deeper than ~2.5 layers: shallower grooves don't survive FDM.
    double lh = 0.2;
    if (mo->config.has("layer_height"))
        lh = mo->config.opt_float("layer_height");
    else if (const DynamicPrintConfig &pc = wxGetApp().preset_bundle->prints.get_edited_preset().config; pc.has("layer_height"))
        lh = pc.opt_float("layer_height");
    const int relief_bin = std::clamp(int(std::lround(2.5 * lh / 0.1)) - 1, 0, 7);
    long   folded = 0, top = 0, pinches = 0;
    double relief = 0.;
    for (const PaintClayPiece &piece : pieces) {
        folded  += piece.log.flipped_top;
        top     += (piece.log.part_tris - 2 * piece.log.boundary_edges) / 2;
        pinches += piece.log.pinches_split;
        relief   = std::max(relief, piece.log.relief_hist[relief_bin]);
    }
    m_clay_warning.clear();
    // Straight growth cannot fold (as seen from the card); the folded count then only says the top is
    // not parallel to the surface, which over a groove is the point. What's worth telling is the relief.
    if (mo->paint_decals[idx].clay.straight) {
        folded = 0;
        if (relief >= 5. && mo->paint_decals[idx].clay.smooth_surface <= 0.)
            m_clay_warning = std::to_string(int(std::round(relief))) + "% " +
                _u8L("of the decal sits on sharp relief (grooves, edges): its top copies it.\n"
                     "Raise Surface smooth to fill it, or leave it if that's the look.");
    }
    // Over a hole already cut (an applied engrave) the part follows its walls edge-on: thousands of
    // folds and pinches. Say that first; Smooth won't fix it.
    if (!mo->paint_decals[idx].clay.straight && top > 0 && (folded * 20 >= top || pinches > 200))
        m_clay_warning = std::to_string(int(std::round(100.0 * double(folded) / double(top)))) + "% " +
            _u8L("of the top is folded.\nIf the decal is over an engrave already applied, move it off the hole,\n"
                 "or Undo that Apply and apply all the engraves together.\nElse raise Smooth or lower Height.");
    else if (top > 0 && folded * 100 >= top)
        m_clay_warning = std::to_string(int(std::round(100.0 * double(folded) / double(top)))) + "% " +
            _u8L("of the top is folded: the surface has detail smaller than the height. Raise Smooth or lower Height.");
    clay_remove_parts(idx);
    mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return 0;
    const Transform3d inst = mo->instances.front()->get_matrix();
    // Mixed filaments (Color Mixing) are valid extruders for a part too: count them.
    const size_t n_physical = wxGetApp().filaments_cnt();
    const int    n_filaments = int(wxGetApp().preset_bundle->mixed_filaments.total_filaments(n_physical));
    int added = 0;
    for (const PaintClayPiece &piece : pieces) {
        TriangleMesh mesh(piece.its);
        mesh.transform(inst);   // add_mesh_part_in_place takes the mesh in world coordinates
        const int extruder = (piece.state >= 1 && piece.state <= n_filaments) ? piece.state : -1;
        ModelVolume *v = wxGetApp().obj_list()->add_mesh_part_in_place(oid, std::move(mesh), paint_clay_volume_name(decal, piece.group), piece.engrave,
                                                                     piece.engrave ? -1 : extruder);
        if (v == nullptr)
            continue;
        ++added;
    }
    m_schedule_update = true;
    wxGetApp().plater()->changed_object(oid);
    m_parent.set_as_dirty();
    return added;
}

// Apply engrave: EVERY engrave part of the object (negative Clay volumes, all decals) cut out of the
// model parts they touch, one after the other, with the mcut boolean of Align & Construct; the painting
// carried over by PaintTransfer (the cut walls and floor stay unpainted). All at once on purpose: an
// engrave rebuilt over a hole already cut follows its walls edge-on and folds (30-oct, the skull
// vanished), while all built over the clean surface simply add up where they overlap. Each cut must keep
// at least (part - engrave) of volume, or nothing is changed. One undo step. The painter saves its
// strokes to the model first and reloads them after.
bool GLGizmoMmuSegmentation::clay_apply_engrave(size_t /*idx*/)
{
    ModelObject *mo  = m_c->selection_info()->model_object();
    const int    oid = m_parent.get_selection().get_object_idx();
    if (mo == nullptr || oid < 0 || mo->instances.empty())
        return false;
    std::vector<const ModelVolume *> tools;
    for (const ModelVolume *v : mo->volumes)
        if (v->is_negative_volume() && paint_clay_is_volume(*v))
            tools.push_back(v);
    if (tools.empty())
        return false;
    update_model_object();
    const Transform3d inst = mo->instances.front()->get_matrix();
    // Cut in the OBJECT frame and keep mcut's doubles from one cut to the next: in world coordinates
    // (the skull ~150 mm from the origin) with the result rounded to float between cuts, mcut's second
    // dispatch fails (-2, "incomplete kernel execution") and Orca hands the part back unchanged (30-oct,
    // two overlapping veins, reproduced outside the app; either change alone is enough).
    std::vector<TriangleMesh>         tool_mesh;
    std::vector<MeshBoolean::mcut::McutMeshPtr> tool_mc;
    for (const ModelVolume *t : tools) {
        tool_mesh.push_back(t->mesh());
        tool_mesh.back().transform(t->get_matrix());
        tool_mc.push_back(MeshBoolean::mcut::triangle_mesh_to_mcut(tool_mesh.back().its));
    }

    struct Cut { const ModelVolume *src; TriangleMesh result; };
    std::vector<Cut> cuts;
    wxBusyCursor wait;
    std::ostringstream log;
    m_clay_warning.clear();
    for (const ModelVolume *v : mo->volumes) {
        if (!v->is_model_part() || paint_clay_is_volume(*v))
            continue;
        TriangleMesh part = v->mesh();
        part.transform(v->get_matrix());
        MeshBoolean::mcut::McutMeshPtr cur = MeshBoolean::mcut::triangle_mesh_to_mcut(part.its);
        double before  = MeshBoolean::mcut::volume(*cur);
        bool   touched = false;
        for (size_t ti = 0; ti < tool_mesh.size(); ++ti) {
            if (!part.bounding_box().overlap(tool_mesh[ti].bounding_box()))
                continue;
            // `cur` is cut in place: a failure or a refusal returns with nothing changed in the model.
            // mcut is fast but gives up on some overlaps ("floating polygon", the sticker over the
            // veins' holes, in any order), or hands back a broken mesh that GAINS volume. Then CGAL, exact
            // predicates, from a copy taken before: slower, but it cut those cases in about 2 s.
            const double tool_v = std::abs(MeshBoolean::mcut::volume(*tool_mc[ti]));
            const double tol    = 0.05 + 1e-6 * before;   // mm3
            MeshBoolean::mcut::McutMeshPtr backup = MeshBoolean::mcut::clone(*cur);
            bool   ok    = MeshBoolean::mcut::do_boolean_single(*cur, *tool_mc[ti], "A_NOT_B") && !MeshBoolean::mcut::empty(*cur);
            double after = ok ? MeshBoolean::mcut::volume(*cur) : 0.;
            if (!ok || after > before + tol) {
                log << " [" << tools[ti]->name << ": mcut " << (ok ? "gained volume" : "failed") << ", CGAL]";
                cur   = std::move(backup);
                ok    = MeshBoolean::mcut::cgal_minus_doubles(*cur, *tool_mc[ti]) && !MeshBoolean::mcut::empty(*cur);
                after = ok ? MeshBoolean::mcut::volume(*cur) : 0.;
            }
            if (!ok || after > before + tol) {
                log << " [" << tools[ti]->name << " on " << v->name << ": mcut and CGAL FAILED]";
                m_clay_warning = _u8L("Apply engrave failed: the boolean could not cut") + " " + tools[ti]->name + ".\n" +
                                 _u8L("Nothing was changed. Try Remesh on the object.");
                return false;
            }
            log << " [" << tools[ti]->name << " on " << v->name << ": " << before << " -> " << after << " mm3, engrave " << tool_v << "]";
            if (!(after > 0.) || after < before - tool_v - tol - 0.001 * before) {
                m_clay_warning = _u8L("Apply engrave stopped: the cut would remove more than the engrave itself, nothing was changed.\n"
                                      "Rebuild the engraves over the uncut surface, or Remesh the object.");
                return false;
            }
            if (after < before - tol) {
                before  = after;
                touched = true;
            }
        }
        if (touched)
            part = MeshBoolean::mcut::mcut_to_triangle_mesh(*cur);
        if (touched) {
            part.transform(inst);   // add_mesh_part_in_place takes the mesh in world coordinates
            cuts.push_back({ v, std::move(part) });
        }
    }
    if (cuts.empty()) {
        if (m_clay_warning.empty())
            m_clay_warning = _u8L("The engrave parts do not cut the object.");
        return false;
    }

    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Apply engrave"), UndoRedo::SnapshotType::GizmoAction);
    long paint_layers = 0;
    std::vector<const ModelVolume *> remove;
    for (Cut &c : cuts) {
        PaintTransfer::Source src;
        src.volume   = c.src;
        src.to_world = inst * c.src->get_matrix();
        src.filament = PaintTransfer::effective_filament(*c.src);
        std::vector<PaintTransfer::Source> sources { src };
        for (const ModelVolume *t : tools) {
            PaintTransfer::Source cutter;
            cutter.volume   = t;
            cutter.to_world = inst * t->get_matrix();
            cutter.filament = src.filament;
            cutter.cutter   = true;
            sources.push_back(cutter);
        }
        const int    extruder = c.src->config.has("extruder") ? c.src->config.opt_int("extruder") : 0;
        ModelVolume *res      = wxGetApp().obj_list()->add_mesh_part_in_place(oid, std::move(c.result), c.src->name, false, extruder > 0 ? extruder : -1);
        if (res == nullptr)
            continue;
        const PaintTransfer::Stats st = PaintTransfer::transfer(sources, *res, inst * res->get_matrix(), src.filament);
        paint_layers += long(st.layers);
        remove.push_back(c.src);
    }
    std::vector<std::string> applied;   // decals whose engrave this cut
    for (const PaintDecal &d : mo->paint_decals)
        if (std::any_of(tools.begin(), tools.end(), [&d](const ModelVolume *t) { return paint_clay_is_volume_of(*t, d); }))
            applied.push_back(d.name);
    remove.insert(remove.end(), tools.begin(), tools.end());
    // Delete by pointer, the higher index first so the others keep their place.
    mo = m_c->selection_info()->model_object();
    std::vector<int> idxs;
    for (const ModelVolume *r : remove)
        for (size_t i = 0; i < mo->volumes.size(); ++i)
            if (mo->volumes[i] == r)
                idxs.push_back(int(i));
    std::sort(idxs.rbegin(), idxs.rend());
    for (int i : idxs)
        wxGetApp().obj_list()->delete_from_model_and_list(ItemType::itVolume, oid, i);
    {
        std::ostringstream s;
        s << "[CLAY] apply engrave: " << tools.size() << " engraves, parts cut " << cuts.size() << " paint layers " << paint_layers << log.str();
    }
    // Stop watching the decals whose engrave was cut, and those with nothing left: rebuilt over the hole
    // that is now in the object, they fold.
    for (PaintDecal &d : mo->paint_decals)
        if (d.clay.on && (std::find(applied.begin(), applied.end(), d.name) != applied.end() ||
                          std::none_of(mo->volumes.begin(), mo->volumes.end(), [&](const ModelVolume *v) { return paint_clay_is_volume_of(*v, d); })))
            d.clay.on = false;
    m_schedule_update = true;
    wxGetApp().obj_list()->update_info_items(size_t(oid));
    wxGetApp().plater()->changed_object(oid);
    m_parent.set_as_dirty();
    return true;
}

int GLGizmoMmuSegmentation::clay_detach_parts(size_t idx)
{
    ModelObject *mo  = m_c->selection_info()->model_object();
    const int    oid = m_parent.get_selection().get_object_idx();
    if (mo == nullptr || oid < 0 || idx >= mo->paint_decals.size())
        return 0;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Detach Clay parts"), UndoRedo::SnapshotType::GizmoAction);
    const std::string prefix = paint_clay_volume_name(mo->paint_decals[idx], 0);
    const size_t      cut    = prefix.find(mo->paint_decals[idx].name);   // after "Clay · "
    int n = 0;
    for (size_t i = 0; i < mo->volumes.size(); ++i) {
        ModelVolume *v = mo->volumes[i];
        if (!paint_clay_is_volume_of(*v, mo->paint_decals[idx]) || v->is_negative_volume())
            continue;
        if (cut != std::string::npos && v->name.size() > cut)
            v->name = v->name.substr(cut);
        wxGetApp().obj_list()->update_name_in_list(oid, int(i));
        ++n;
    }
    m_parent.set_as_dirty();
    return n;
}

void GLGizmoMmuSegmentation::set_paint_mode(PaintMode mode)
{
    if (m_paint_mode == mode)
        return;
    if (m_sf_tool != 0) {   // a line being drawn goes in, the tool is the mode's
        if (m_sf_path.clicks.size() >= 2)
            sf_finish();
        sf_cancel();
        m_sf_tool = 0;
    }
    // Clay looks at the raised parts: the decal's see-through colours only get in the way, so it
    // keeps its own "Show decal" (off by default) and Vector / Brush get theirs back on leaving.
    if (mode == PaintMode::Clay) {
        m_clay_saved_preview    = m_decal_preview_colours;
        m_decal_preview_colours = m_clay_show_decal;
    } else if (m_paint_mode == PaintMode::Clay) {
        m_clay_show_decal       = m_decal_preview_colours;
        m_decal_preview_colours = m_clay_saved_preview;
    }
    m_paint_mode = mode;
    m_parent.set_as_dirty();
}

// Brush mode only: drag a box on screen and paint what is under it.
void GLGizmoMmuSegmentation::render_rectangle_mask_option()
{
    if (ImGui::Checkbox(_u8L("Rectangle mask").c_str(), &m_rectangle_mask_active))
        shape_clear();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Drag a rectangle on screen to paint every visible facet under it\n"
            "with the selected filament, instead of brushing by hand.\n"
            "Only what the camera sees is painted.").c_str());
}

bool GLGizmoMmuSegmentation::render_svg_colour_row(int id, uint32_t rgb, bool grad, int &state, bool &hidden)
{
    bool changed = false;
    const float row_start = ImGui::GetCursorScreenPos().x;
    const size_t n_filaments = std::min(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT, m_display_filament_ids.size());
    auto filament_label = [this, n_filaments](int s) {
        for (size_t i = 0; i < n_filaments; ++i)
            if (int(m_display_filament_ids[i]) == s)
                return std::to_string(i + 1);
        return std::string("?");
    };
    const ImGuiColorEditFlags swatch_flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop;
    const ImVec2 swatch_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    ImGui::PushID(id);
    const ImVec4 svg_col(float((rgb >> 16) & 0xFF) / 255.f, float((rgb >> 8) & 0xFF) / 255.f, float(rgb & 0xFF) / 255.f, 1.f);
    ImGui::ColorButton("##svg_colour", svg_col, swatch_flags, swatch_size);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", (grad ? _u8L("Colour in the SVG (a gradient: its mean colour)") : _u8L("Colour in the SVG")).c_str());
    ImGui::SameLine();
    m_imgui->text(">");
    ImGui::SameLine();
    ImGui::ColorButton("##filament_colour", ImGuiWrapper::to_ImVec4(get_state_color(state)), swatch_flags, swatch_size);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(m_imgui->scaled(3.5f));
    if (ImGui::BeginCombo("##filament", filament_label(state).c_str())) {
        for (size_t i = 0; i < n_filaments; ++i) {
            const int s = int(m_display_filament_ids[i]);
            ImGui::PushID(int(i));
            ImGui::ColorButton("##c", ImGuiWrapper::to_ImVec4(get_state_color(s)), swatch_flags, swatch_size);
            ImGui::SameLine();
            if (ImGui::Selectable(std::to_string(i + 1).c_str(), s == state)) {
                state   = s;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Filament this colour paints with.").c_str());
    ImGui::SameLine();
    bool visible = !hidden;
    if (ImGui::Checkbox("##visible", &visible)) {
        hidden  = !visible;
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Off: this colour is not painted and lets through\nwhat the SVG has under it.").c_str());
    m_colour_row_w = std::max(m_colour_row_w, ImGui::GetItemRectMax().x - row_start);
    ImGui::PopID();
    return changed;
}

// F5: Pick on object / Box centre, shared by the decals and the brush.
void GLGizmoMmuSegmentation::render_mirror_centre_controls()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    auto snapshot = [](const std::string &name) { Plater::TakeSnapshot(wxGetApp().plater(), name, UndoRedo::SnapshotType::GizmoAction); };
    m_imgui->text(_L("Mirror centre"));
    ImGui::SameLine();
    const bool picking = m_mirror_pick_armed;
    if (picking) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.59f, 0.53f, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORCA);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    }
    if (m_imgui->button(_L("Pick on object"))) {
        m_mirror_pick_armed = !m_mirror_pick_armed;
        m_parent.set_as_dirty();
    }
    if (picking) {
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Then click on the object: the mirror planes cross there (the orange mark).\n"
            "For an object that is not centred in its box, like a face turned a little.").c_str());
    if (mo->paint_symmetry.custom_centre) {
        ImGui::SameLine();
        if (m_imgui->button(_L("Box centre"))) {
            snapshot(_u8L("Mirror centre"));
            mo->paint_symmetry.custom_centre = false;
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Back to the centre of the object's box.").c_str());
    }
    if (picking)
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Click on the object."));
}

bool GLGizmoMmuSegmentation::render_part_filament_combo(const char *id, int &state)
{
    bool changed = false;
    const size_t n_filaments = std::min(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT, m_display_filament_ids.size());
    std::string label = _u8L("Colour");
    for (size_t i = 0; i < n_filaments; ++i)
        if (int(m_display_filament_ids[i]) == state)
            label = std::to_string(i + 1);
    const ImGuiColorEditFlags swatch_flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop;
    const ImVec2 swatch_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(m_imgui->scaled(5.f));
    if (ImGui::BeginCombo(id, label.c_str())) {
        if (ImGui::Selectable(_u8L("Colour").c_str(), state < 0)) {
            state   = -1;
            changed = true;
        }
        for (size_t i = 0; i < n_filaments; ++i) {
            const int s = int(m_display_filament_ids[i]);
            ImGui::PushID(int(i));
            ImGui::ColorButton("##c", ImGuiWrapper::to_ImVec4(get_state_color(s)), swatch_flags, swatch_size);
            ImGui::SameLine();
            if (ImGui::Selectable(std::to_string(i + 1).c_str(), s == state)) {
                state   = s;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool GLGizmoMmuSegmentation::render_svg_parts()
{
    if (m_svg_shapes.size() < 2)
        return false;   // one shape: its colour row already is the part
    bool changed = false;
    const int hover_before = m_svg_part_hover;
    m_svg_part_hover = -1;
    ImGui::PushID("svg_parts");
    m_imgui->text(GUI::format(_L("Parts: %1%"), m_svg_shapes.size()));
    ImGui::SameLine();
    if (m_imgui->bbl_checkbox(_L("Pick parts"), m_svg_pick_parts))
        m_parent.set_as_dirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Click a part of the SVG to select it, Shift + click to add or remove one.\n"
            "While this is on, dragging does not move the SVG.").c_str());

    const ImGuiColorEditFlags swatch_flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop;
    const ImVec2 swatch_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    auto zone_colour = [this](int shape) {
        const int z = m_svg_shapes[size_t(shape)].zone;
        const uint32_t rgb = (z >= 0 && size_t(z) < m_svg_zones.size()) ? m_svg_zones[size_t(z)].rgb : 0;
        return ImVec4(float((rgb >> 16) & 0xFF) / 255.f, float((rgb >> 8) & 0xFF) / 255.f, float(rgb & 0xFF) / 255.f, 1.f);
    };

    // The selection: one filament and one eye for all of it.
    if (!m_svg_part_sel.empty()) {
        m_imgui->text(GUI::format(_L("Selected: %1%"), m_svg_part_sel.size()));
        ImGui::SameLine();
        const PaintDecalPart *first = svg_part(*m_svg_part_sel.begin(), false);
        int  state  = first ? first->state : -1;
        bool hidden = first ? first->hidden : false;
        if (render_part_filament_combo("##sel_filament", state)) {
            for (int k : m_svg_part_sel)
                svg_part(k, true)->state = state;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Filament the selected parts paint with.\nColour: the one of their colour row.").c_str());
        ImGui::SameLine();
        bool visible = !hidden;
        if (ImGui::Checkbox("##sel_visible", &visible)) {
            for (int k : m_svg_part_sel)
                svg_part(k, true)->hidden = !visible;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Off: the selected parts are not painted and let through\nwhat the SVG has under them.").c_str());
        ImGui::SameLine();
        if (m_imgui->button(_L("Reset"))) {
            for (int k : m_svg_part_sel)
                if (PaintDecalPart *p = svg_part(k, false)) {
                    p->state  = -1;
                    p->hidden = false;
                }
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("The selected parts follow their colour again.").c_str());
        ImGui::SameLine();
        if (m_imgui->button(_L("Clear")))
            m_svg_part_sel.clear();
    }

    // The list: the parts changed or selected, or all of them.
    if (ImGui::TreeNode(_u8L("Part list").c_str())) {
        ImGui::RadioButton(_u8L("Changed").c_str(), &m_svg_parts_filter, 0);
        ImGui::SameLine();
        ImGui::RadioButton(_u8L("All").c_str(), &m_svg_parts_filter, 1);
        std::vector<int> rows;
        if (m_svg_parts_filter == 1) {
            for (size_t k = 0; k < m_svg_shapes.size(); ++k)
                rows.push_back(int(k));
        } else {
            std::set<int> shown(m_svg_part_sel.begin(), m_svg_part_sel.end());
            for (const PaintDecalPart &p : m_svg_parts)
                shown.insert(p.shape);
            rows.assign(shown.begin(), shown.end());
        }
        if (rows.empty())
            m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("No part changed. Pick parts on the SVG, or show All."));
        const float row_h = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##parts", ImVec2(0.f, std::min(float(rows.size()), 8.f) * row_h + 4.f), false);
        ImGuiListClipper clipper;
        clipper.Begin(int(rows.size()), row_h);
        while (clipper.Step())
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const int k = rows[size_t(r)];
                ImGui::PushID(k);
                const PaintDecalPart *p = svg_part(k, false);
                int  state  = p ? p->state : -1;
                bool hidden = p ? p->hidden : false;
                bool sel    = m_svg_part_sel.count(k) != 0;
                const float row_y = ImGui::GetCursorScreenPos().y;
                if (ImGui::Checkbox("##sel", &sel)) {
                    if (sel)
                        m_svg_part_sel.insert(k);
                    else
                        m_svg_part_sel.erase(k);
                    m_parent.set_as_dirty();
                }
                ImGui::SameLine();
                ImGui::ColorButton("##colour", zone_colour(k), swatch_flags, swatch_size);
                ImGui::SameLine();
                m_imgui->text("#" + std::to_string(k + 1));
                ImGui::SameLine();
                if (render_part_filament_combo("##filament", state)) {
                    svg_part(k, true)->state = state;
                    changed = true;
                }
                ImGui::SameLine();
                bool visible = !hidden;
                if (ImGui::Checkbox("##visible", &visible)) {
                    svg_part(k, true)->hidden = !visible;
                    changed = true;
                }
                if (ImGui::IsMouseHoveringRect(ImVec2(ImGui::GetWindowPos().x, row_y), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), row_y + row_h)))
                    m_svg_part_hover = k;
                ImGui::PopID();
            }
        ImGui::EndChild();
        ImGui::TreePop();
    }
    ImGui::PopID();
    if (changed)
        svg_prune_parts();
    if (changed || m_svg_part_hover != hover_before)
        m_parent.set_as_dirty();
    return changed;
}

// 2b — Merge: every picked decal's SVG, carried into the frame of the topmost one through
// the object (its card point -> object -> the top decal's card), as one SVG in mm. Only decals
// looking from about the same side: a decal seen from the side would come out squashed.
void GLGizmoMmuSegmentation::decal_merge()
{
    m_decal_merge_msg.clear();
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || m_decal_multi.size() < 2)
        return;
    std::vector<PaintDecal> &decals = mo->paint_decals;
    std::vector<int> pick;
    for (int i : m_decal_multi)
        if (i >= 0 && i < int(decals.size()) && std::find(pick.begin(), pick.end(), i) == pick.end())
            pick.push_back(i);
    std::sort(pick.begin(), pick.end());
    if (pick.size() < 2)
        return;
    const PaintDecal &T  = decals[size_t(pick.front())];
    const Vec3d       dT = Vec3d(T.dir[0], T.dir[1], T.dir[2]).normalized();
    for (int i : pick) {
        const PaintDecal &D = decals[size_t(i)];
        if (D.locked) {
            m_decal_merge_msg = GUI::format(_L("%1% is locked."), D.name);
            return;
        }
        if (!D.surface.empty()) {   // its Clay comes from its points, not from a view
            m_decal_merge_msg = GUI::format(_L("%1% is drawn on the object (Draw on surface / Wrap): it can't be merged."), D.name);
            return;
        }
        if (Vec3d(D.dir[0], D.dir[1], D.dir[2]).normalized().dot(dT) < 0.966) {   // 15 degrees
            m_decal_merge_msg = GUI::format(_L("%1% looks at the object from another side than %2%:\n"
                                               "merged, it would come out squashed. Merge decals pinned from about the same view."), D.name, T.name);
            return;
        }
    }
    const Matrix4d to_T = T.object_to_svg();
    std::string svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000mm\" height=\"1000mm\" viewBox=\"0 0 1000 1000\">";
    auto hex = [](uint32_t abgr) {   // nanosvg packs 0xAABBGGRR
        char h[8];
        std::snprintf(h, sizeof(h), "#%02X%02X%02X", int(abgr & 0xFF), int((abgr >> 8) & 0xFF), int((abgr >> 16) & 0xFF));
        return std::string(h);
    };
    for (int i : pick) {
        const PaintDecal &D = decals[size_t(i)];
        NSVGimage_ptr img = nsvgParse(D.svg_data, "mm", 96.0f);
        if (!img)
            continue;
        const bool same = i == pick.front();
        auto map = [&](float x, float y) {
            if (same)
                return Vec2d(x, y);
            const Vec3d  o = D.svg_to_object(Vec2d(x, y));
            const Vec4d  h = to_T * Vec4d(o.x(), o.y(), o.z(), 1.);
            return std::abs(h.w()) > 1e-12 ? Vec2d(h.x() / h.w(), h.y() / h.w()) : Vec2d(x, y);
        };
        for (NSVGshape *sh = img->shapes; sh != nullptr; sh = sh->next) {
            if (!(sh->flags & NSVG_FLAGS_VISIBLE) || sh->fill.type == NSVG_PAINT_NONE)
                continue;
            uint32_t c = sh->fill.color;
            if ((sh->fill.type == NSVG_PAINT_LINEAR_GRADIENT || sh->fill.type == NSVG_PAINT_RADIAL_GRADIENT) && sh->fill.gradient != nullptr && sh->fill.gradient->nstops > 0)
                c = sh->fill.gradient->stops[sh->fill.gradient->nstops / 2].color;
            std::string d;
            for (NSVGpath *p = sh->paths; p != nullptr; p = p->next) {
                if (p->npts < 1)
                    continue;
                for (int k = 0; k < p->npts; ++k) {
                    const Vec2d q = map(p->pts[2 * k], p->pts[2 * k + 1]);
                    d += k == 0 ? " M " : (k % 3 == 1 ? " C " : " ");
                    stroke::put_num(d, q.x());
                    d += ' ';
                    stroke::put_num(d, q.y());
                }
                if (p->closed)
                    d += " Z";
            }
            if (d.empty())
                continue;
            svg += "<path fill=\"" + hex(c) + "\"" + (sh->fillRule == NSVG_FILLRULE_EVENODD ? " fill-rule=\"evenodd\"" : "") + " d=\"" + d + "\"/>";
        }
    }
    svg += "</svg>";
    const std::vector<SvgPaint::Zone> zones = SvgPaint::zones(svg);
    if (zones.empty()) {
        m_decal_merge_msg = _u8L("Nothing to merge: the SVGs came out empty.");
        return;
    }
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Merge SVG decals"), UndoRedo::SnapshotType::GizmoAction);
    PaintDecal merged = T;
    merged.svg_data = svg;
    merged.parts.clear();   // shapes set apart are numbered per SVG: they start again
    merged.zones.clear();
    for (size_t k = 0; k < zones.size(); ++k) {
        PaintDecalZone z;
        z.zone  = int(k);
        z.rgb   = zones[k].rgb;
        z.state = T.zones.empty() ? 1 : T.zones.front().state;
        bool found = false;
        for (int i : pick) {   // the colour keeps its filament, from the topmost decal that has it
            for (const PaintDecalZone &o : decals[size_t(i)].zones)
                if (o.rgb == z.rgb) { z.state = o.state; z.hidden = o.hidden; found = true; break; }
            if (found)
                break;
        }
        merged.zones.push_back(z);
    }
    for (auto it = pick.rbegin(); it != pick.rend(); ++it)
        if (*it != pick.front())
            decals.erase(decals.begin() + *it);
    decals[size_t(pick.front())] = std::move(merged);
    m_selected_decal = pick.front();
    m_svg_edit_decal = -1;
    m_decal_multi.clear();
    m_decal_rings_cache.clear();
    m_decal_merge_msg = GUI::format(_L("Merged %1% decals into %2%. Its Clay parts need Rebuild."), pick.size(), decals[size_t(pick.front())].name);
    m_parent.set_as_dirty();
}

// the decal layers, the same list in Vector and Clay: show, lock, colours,
// name (and in Clay its part count), up / down. Fixed widths: the name column takes what is left.
void GLGizmoMmuSegmentation::render_decal_layers(bool clay)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    std::vector<PaintDecal> &decals = mo->paint_decals;
    if (decals.empty())
        return;
    auto snapshot = [](const std::string &name) { Plater::TakeSnapshot(wxGetApp().plater(), name, UndoRedo::SnapshotType::GizmoAction); };
    const ImGuiStyle &style = ImGui::GetStyle();
    if (!neo_ui::section(GUI::format(_L("Decals (%1%)"), decals.size()) + "###decal_layers", true))
        return;
    int move_from = -1, move_to = -1;
    const float dot   = ImGui::GetTextLineHeight() * 0.45f;
    const float fh    = ImGui::GetFrameHeight();
    const float dots_w = dot * 2.4f * 4.f;
    // checkbox ×2, dots, name, arrow ×2, with ItemSpacing between them
    const float name_w = std::max(ImGui::GetFontSize() * 6.f,
        neo_ui::panel_w() - (fh * (clay ? 3.f : 2.f) + dots_w + fh * 2.f + style.ItemSpacing.x * (clay ? 6.f : 5.f)));
    for (size_t i = 0; i < decals.size(); ++i) {
        PaintDecal &d = decals[i];
        ImGui::PushID(int(i) + 7000);
        if (neo_ui::icon_button("##visible", d.hidden ? neo_ui::Icon::EyeOff : neo_ui::Icon::Eye)) {
            snapshot(_u8L("Show/hide SVG decal"));
            d.hidden = !d.hidden;
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Show the card, and include it in Apply all.").c_str());
        ImGui::SameLine();
        m_imgui->disabled_begin(d.clay.frozen);
        if (neo_ui::icon_button("##locked", d.locked ? neo_ui::Icon::Lock : neo_ui::Icon::Unlock, d.locked)) {
            snapshot(_u8L("Lock/unlock SVG decal"));
            d.locked = !d.locked;
        }
        m_imgui->disabled_end();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", d.clay.frozen ? _u8L("Frozen: unfreeze it first.").c_str() : _u8L("Locked: it cannot be edited, deleted or rebuilt.").c_str());
        if (clay) {   // 3: freeze its Clay parts so other decals grow on top of them
            ImGui::SameLine();
            bool has_parts = false;
            for (const ModelVolume *v : mo->volumes)
                if (v->is_model_part() && paint_clay_is_volume_of(*v, d)) { has_parts = true; break; }
            m_imgui->disabled_begin(!has_parts && !d.clay.frozen);
            if (neo_ui::icon_button("##frozen", neo_ui::Icon::Freeze, d.clay.frozen)) {
                if (!d.clay.frozen) {
                    snapshot(_u8L("Freeze Clay"));
                    d.clay.frozen            = true;
                    d.clay.frozen_was_locked = d.locked;
                    d.locked                 = true;
                    m_clay_status = GUI::format(_L("%1% is frozen: Clay of other decals now grows on top of it."), d.name);
                } else {
                    wxMessageDialog dlg(wxGetApp().plater(),
                        GUI::format_wxstr(_L("Unfreeze %1%?\n\nIt can move and rebuild again.\n\n"
                                             "The Clay parts of other decals built on top of it are NOT removed: delete them by hand "
                                             "(or Rebuild those decals so they grow from the object again)."), d.name),
                        _L("Unfreeze Clay"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
                    if (dlg.ShowModal() == wxID_YES) {
                        snapshot(_u8L("Unfreeze Clay"));
                        d.clay.frozen = false;
                        d.locked      = d.clay.frozen_was_locked;
                        m_clay_status = GUI::format(_L("%1% is no longer frozen. Rebuild the decals that were on top of it."), d.name);
                    }
                }
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", (d.clay.frozen ? _u8L("Frozen: its parts stay put and count as part of the object,\n"
                                                              "so other decals' Clay grows on top of them. Click to unfreeze.")
                                                       : _u8L("Freeze: keep these Clay parts as they are and build on top of them.\n"
                                                              "Other decals' Clay will grow from their surface.\nNeeds built parts.")).c_str());
        }
        ImGui::SameLine();
        {   // the filaments it paints with, as small dots
            ImDrawList  *draw  = ImGui::GetWindowDrawList();
            const ImVec2 pos   = ImGui::GetCursorScreenPos();
            const float  cy    = pos.y + fh * 0.5f;
            float        cx    = pos.x + dot;
            int          shown = 0;
            for (const PaintDecalZone &z : d.zones) {
                if (z.hidden || shown >= 4)
                    continue;
                draw->AddCircleFilled(ImVec2(cx, cy), dot, ImGuiWrapper::to_ImU32(get_state_color(z.state)));
                draw->AddCircle(ImVec2(cx, cy), dot, IM_COL32(0, 0, 0, 255));
                cx += dot * 2.4f;
                ++shown;
            }
            ImGui::Dummy(ImVec2(dots_w, fh));
        }
        ImGui::SameLine();
        std::string label = d.name.empty() ? std::string("SVG") : d.name;
        if (clay) {
            int n_parts = 0;
            for (const ModelVolume *v : mo->volumes)
                if (paint_clay_is_volume_of(*v, d))
                    ++n_parts;
            if (n_parts > 0)
                label += "  (" + std::to_string(n_parts) + ")";
        } else {
            label += d.perspective ? "  [P]" : "  [O]";
            if (d.through)
                label += " [T]";
            if (int(i) == m_svg_edit_decal)
                label += "  *";
        }
        ImGui::AlignTextToFramePadding();
        const bool multi = std::find(m_decal_multi.begin(), m_decal_multi.end(), int(i)) != m_decal_multi.end();
        if (ImGui::Selectable(label.c_str(), int(i) == m_selected_decal || multi, 0, ImVec2(name_w, 0.f))) {
            if (ImGui::GetIO().KeyShift) {   // 2b: Shift + click adds or takes away a row for Merge
                if (m_decal_multi.empty() && m_selected_decal >= 0)
                    m_decal_multi.push_back(m_selected_decal);
                auto it = std::find(m_decal_multi.begin(), m_decal_multi.end(), int(i));
                if (it != m_decal_multi.end() && int(i) != m_selected_decal)
                    m_decal_multi.erase(it);
                else if (it == m_decal_multi.end())
                    m_decal_multi.push_back(int(i));
            } else
                m_decal_multi.clear();
            m_selected_decal = int(i);
            m_decal_merge_msg.clear();
            m_clay_status.clear();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", clay ? _u8L("(n) = the Clay parts it has.").c_str()
                                         : _u8L("[P] perspective, [O] parallel rays, [T] through.\n* = being edited on screen.").c_str());
        ImGui::SameLine();
        if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) {
            move_from = int(i);
            move_to   = int(i) - 1;
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("##down", ImGuiDir_Down) && i + 1 < decals.size()) {
            move_from = int(i);
            move_to   = int(i) + 1;
        }
        ImGui::PopID();
    }
    if (m_decal_multi.size() >= 2) {
        if (m_imgui->button(GUI::format(_L("Merge %1% layers"), m_decal_multi.size())))
            decal_merge();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Join the Shift-picked layers into the topmost one: one SVG,\n"
                "each colour keeps its filament. Only layers pinned from about the same view.\n"
                "Undo brings them back.").c_str());
        ImGui::SameLine();
        if (m_imgui->button(_L("Cancel")))
            m_decal_multi.clear();
    } else
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _u8L("Shift + click: pick layers to merge."));
    if (!m_decal_merge_msg.empty())
        m_imgui->text_colored(ImGuiWrapper::COL_ORANGE_LIGHT, m_decal_merge_msg);
    if (move_from >= 0) {
        m_decal_multi.clear();
        snapshot(_u8L("Reorder SVG decals"));
        std::swap(decals[size_t(move_from)], decals[size_t(move_to)]);
        auto follow = [move_from, move_to](int &idx) {
            if (idx == move_from)
                idx = move_to;
            else if (idx == move_to)
                idx = move_from;
        };
        follow(m_selected_decal);
        follow(m_svg_edit_decal);
        m_decal_rings_cache.clear();
        m_parent.set_as_dirty();
    }
}

// Vector mode: actions, the decal layers, the selected decal, and the shape
// or SVG being edited on screen. Same layout as the mockup approved on 27-oct-2026.
void GLGizmoMmuSegmentation::render_vector_panel()
{
    neo_ui::Compact compact;
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    auto snapshot = [](const std::string &name) { Plater::TakeSnapshot(wxGetApp().plater(), name, UndoRedo::SnapshotType::GizmoAction); };
    std::vector<PaintDecal> &decals = mo->paint_decals;
    // Colour rows go in two columns when the panel is wide enough for two of them side by side.
    // The row width is MEASURED (render_svg_colour_row), never guessed: a second column placed past
    // the window's edge widens an auto-resized window, which moves the edge, and so on every frame.
    // The second column is aligned to the right edge, so it can never push it.
    const ImGuiStyle &style = ImGui::GetStyle();
    auto colour_column = [this, &style](size_t k, float line_x, float avail) {
        const float w = m_colour_row_w;
        if (w > 1.f && avail >= 2.f * w + style.ItemSpacing.x * 2.f && k % 2 == 1)
            ImGui::SameLine(line_x + avail - w);
    };
    if (m_selected_decal >= int(decals.size()))
        m_selected_decal = decals.empty() ? -1 : int(decals.size()) - 1;

    // --- Actions
    if (m_imgui->button(_L("Load SVG..."))) {
        wxFileDialog dlg(wxGetApp().plater(), _L("Choose an SVG file"), "", "", "SVG files (*.svg)|*.svg", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK)
            shape_load_svg(std::string(dlg.GetPath().ToUTF8().data()));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Load an SVG, centred on the view.\nEach of its colours gets a filament.").c_str());
    ImGui::SameLine();
    m_imgui->disabled_begin(decals.empty());
    if (m_imgui->button(_L("Apply all"))) {
        std::vector<size_t> idxs;
        for (size_t i = 0; i < decals.size(); ++i)
            if (!decals[i].hidden)
                idxs.push_back(i);
        snapshot(_u8L("Apply SVG decals"));
        decal_apply(idxs, false);
        update_model_object();
        m_parent.set_as_dirty();
    }
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Apply every visible decal, top of the list first\n(the lower ones paint over).").c_str());
    if (!m_shape_svg_error.empty())
        m_imgui->warning_text(m_shape_svg_error);
    ImGui::Checkbox(_u8L("Lock view").c_str(), &m_shape_view_lock);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Freeze the camera while you work on screen:\nno zoom, orbit or pan, view cube or view keys.").c_str());
    ImGui::SameLine();
    if (ImGui::Checkbox(_u8L("Preview colours").c_str(), &m_decal_preview_colours))
        m_parent.set_as_dirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Fill the decal cards with their filaments' colours, see-through,\n"
            "for an idea of the result before Apply.").c_str());
    ImGui::SameLine();
    if (ImGui::Checkbox(_u8L("On surface").c_str(), &m_decal_on_surface))
        m_parent.set_as_dirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Show the cards laid on the object, exactly where they will paint\n"
            "(mirrored copies too). Off: floating around the object, as a reference.\n"
            "On folds and edges the preview can show small gaps: only the preview,\n"
            "Apply paints the whole area.").c_str());
    // A left drag orbits outside the SVG or shape being placed (and always before Draw shape).
    if (!m_shape_view_lock) {
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Orbit: drag outside the SVG or shape. Pan: right drag."));
    }

    // F5: where the mirror planes cross, for every decal of the object (shown while any mirror is on).
    bool any_mirror = m_svg_mirror != 0;
    for (const PaintDecal &d : decals)
        any_mirror = any_mirror || d.mirror != 0;
    if (any_mirror)
        render_mirror_centre_controls();

    // --- Layers
    if (!decals.empty()) {
        render_decal_layers(false);

        // --- The selected decal
        if (m_selected_decal >= 0 && size_t(m_selected_decal) < decals.size() && m_selected_decal == m_svg_edit_decal) {
            ImGui::Separator();
            m_imgui->text(GUI::format(_L("Selected: %1%"), decals[size_t(m_selected_decal)].name));
            m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Being edited on screen, below."));
        } else if (!m_svg_zones.empty() || !m_shape_nodes.empty()) {
            // Something else is on screen: the selected decal's controls wait, so they are not
            // mixed up with the ones of the SVG or shape being placed.
            ImGui::Separator();
            m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Pin or cancel what is on screen to edit the decals."));
        } else if (m_selected_decal >= 0 && size_t(m_selected_decal) < decals.size()) {
            // Own ID scope: ImGui tells buttons apart by their label, and the on-screen block below
            // also has Apply and Erase (the second one never fired).
            ImGui::PushID("selected_decal");
            PaintDecal &d = decals[size_t(m_selected_decal)];
            ImGui::Separator();
            if (m_sf_tool != 0 && m_sf_edit_decal == m_selected_decal) {   // its points are on screen
                m_imgui->text_colored(ImGuiWrapper::COL_ORCA, GUI::format(_L("Editing points: %1%"), d.name));
                render_surface_controls();
                ImGui::PopID();
                return;
            }
            m_imgui->text(GUI::format(_L("Selected: %1%"), d.name.empty() ? std::string("SVG") : d.name));
            m_imgui->disabled_begin(d.locked);
            if (m_imgui->button(_L("Edit")))
                {
                    const ModelObject *emo = m_c->selection_info()->model_object();
                    const int          ei  = m_selected_decal;
                    if (emo != nullptr && ei >= 0 && size_t(ei) < emo->paint_decals.size() && !emo->paint_decals[size_t(ei)].surface.empty()) {
                        set_paint_mode(emo->paint_decals[size_t(ei)].surface.wrap ? PaintMode::VectorBrush : PaintMode::Vector);
                        sf_edit_decal(ei);
                    } else {
                        if (m_sf_tool != 0) {   // a normal decal: the surface tool lets go first
                            if (m_sf_path.clicks.size() >= 2)
                                sf_finish();
                            sf_cancel();
                            m_sf_tool = 0;
                        }
                        set_paint_mode(PaintMode::Vector);   // edited on screen in Vector: a click there is not a brush stroke
                        svg_edit_decal(ei);
                    }
                }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Go to the view it was pinned from and adjust it on screen:\n"
                    "move, scale, rotate, colours. Then Save (editing never paints).").c_str());
            m_imgui->disabled_end();
            ImGui::SameLine();
            if (m_imgui->button(_L("Apply"))) {
                snapshot(_u8L("Apply SVG decal"));
                decal_apply({ size_t(m_selected_decal) }, false);
                update_model_object();
                m_parent.set_as_dirty();
            }
            ImGui::SameLine();
            if (m_imgui->button(_L("Erase"))) {
                snapshot(_u8L("Erase SVG decal"));
                decal_apply({ size_t(m_selected_decal) }, true);
                update_model_object();
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Remove the paint this decal's area covers.").c_str());
            ImGui::SameLine();
            if (m_imgui->button(_L("Duplicate"))) {   // a copy right under it, same place and settings
                snapshot(_u8L("Duplicate SVG decal"));
                PaintDecal copy = d;
                if (copy.clay.frozen) {   // the copy has no parts yet: it starts thawed
                    copy.clay.frozen = false;
                    copy.locked      = copy.clay.frozen_was_locked;
                }
                const size_t at = size_t(m_selected_decal) + 1;
                decals.insert(decals.begin() + at, std::move(copy));
                paint_decal_unique_name(decals, at);
                if (m_svg_edit_decal >= int(at))
                    ++m_svg_edit_decal;
                m_selected_decal = int(at);
                m_decal_multi.clear();
                m_decal_rings_cache.clear();
                m_parent.set_as_dirty();
                ImGui::PopID();
                return;   // `d` points into the list that just grew
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("A copy of this layer right under it, in the same place and with the same settings.\n"
                                             "Move it or change it on its own. Its Clay parts are made with Rebuild.").c_str());
            ImGui::SameLine();
            m_imgui->disabled_begin(d.locked);
            const bool delete_it = m_imgui->button(_L("Delete"));
            m_imgui->disabled_end();
            // F4b: slide it over the object with the mouse, seeing where it paints.
            {
                const bool moving = m_decal_move_armed;
                if (moving) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.59f, 0.53f, 0.35f));
                    ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORCA);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                }
                m_imgui->disabled_begin(d.locked || !d.surface.empty());   // a surface decal moves by its points (Edit)
                if (m_imgui->button(_L("Move on object"))) {
                    m_decal_move_armed = !m_decal_move_armed;
                    if (m_decal_move_armed)
                        m_decal_on_surface = true;   // moving it only makes sense seeing where it lands
                    m_parent.set_as_dirty();
                }
                m_imgui->disabled_end();
                if (moving) {
                    ImGui::PopStyleVar(1);
                    ImGui::PopStyleColor(2);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("Then drag on the object: the decal slides over it, turning around the object,\n"
                        "and you see live where it will paint. Drag off the object to orbit.\n"
                        "Click the button again to stop.").c_str());
                if (moving) {
                    ImGui::SameLine();
                    m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Drag on the object."));
                }
            }

            // F4b: turn and scale it on its own card, around the centre of the SVG. Each slider gesture
            // starts from the decal as it was when grabbed, and is one undo step.
            if (neo_ui::section(_u8L("Place") + "###decal_place", true)) {
            if (!d.surface.empty())
                m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Drawn on the object: Edit moves its points.\nTurn, Size, Projection and Mirror don't apply to it."));
            if (!d.locked && d.surface.empty()) {
                auto gesture = [&](bool changed) {
                    if (ImGui::IsItemActivated()) {
                        snapshot(_u8L("Turn or scale SVG decal"));
                        m_decal_ts_start = d;
                    }
                    if (changed)
                        decal_turn_scale(size_t(m_selected_decal), m_decal_ts_start, m_decal_turn_ui, m_decal_size_ui / 100.);
                    if (ImGui::IsItemDeactivated()) {
                        m_decal_turn_ui = 0.f;
                        m_decal_size_ui = 100.f;
                    }
                    if (changed)
                        m_parent.set_as_dirty();
                };
                neo_ui::row(_u8L("Turn"));
                gesture(ImGui::SliderFloat("##turn", &m_decal_turn_ui, -180.f, 180.f, "%.0f deg"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("Turn the decal on the object, around its centre.").c_str());
                neo_ui::row(_u8L("Size"));
                gesture(ImGui::SliderFloat("##size", &m_decal_size_ui, 25.f, 400.f, "%.0f%%"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("Scale the decal on the object, around its centre.").c_str());
            }
            int proj = d.perspective ? 1 : 0;
            neo_ui::row(_u8L("Projection"));
            if (neo_ui::segmented("proj", { _u8L("Parallel"), _u8L("Perspective") }, proj)) {
                snapshot(_u8L("SVG decal projection"));
                d.perspective = proj == 1;
                if (d.perspective && d.eye_dist <= 0.)
                    d.eye_dist = std::max(3. * mo->raw_mesh_bounding_box().size().norm(), 50.);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Perspective: rays from the eye it was pinned from, as the camera saw it.\n"
                    "Parallel: the same size at any depth.").c_str());
            int depth = d.through ? 1 : 0;
            neo_ui::row(_u8L("Depth"));
            if (neo_ui::segmented("depth", { _u8L("First surface"), _u8L("Through") }, depth)) {
                snapshot(_u8L("SVG decal depth"));
                d.through = depth == 1;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Through: every surface the rays cross, folds and hidden faces too.\n"
                    "It also comes out on the back of the object: limit it with Through depth.").c_str());
            if (d.through) {
                neo_ui::row(_u8L("Through depth"));
                float td = float(d.through_depth);
                if (ImGui::SliderFloat("##through_depth", &td, 0.f, 100.f, td > 0.f ? "%.1f mm" : "all the way", ImGuiSliderFlags_Logarithmic)) {
                    if (ImGui::IsItemActivated())
                        snapshot(_u8L("SVG decal depth"));
                    d.through_depth = double(td) < 0.5 ? 0. : double(td);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("How deep behind the first surface it still paints.\n"
                        "15-20 mm reaches an eye socket's walls and floor\n"
                        "without painting the back of the head. All the way: no limit.\n"
                        "Then press Apply (paint drop).").c_str());
            }
            // F5: symmetry, mirrored copies through the centre of the object's box. Three toggles.
            neo_ui::row(_u8L("Mirror"));
            m_imgui->disabled_begin(!d.surface.empty());
            {
                const int ax = neo_ui::mirror_toggles("dmir", d.mirror, _u8L("Paint mirrored copies through the centre of the object.\n"
                    "Two give three copies. The copies are drawn as cards too and follow the decal."));
                if (ax >= 0) {
                    snapshot(_u8L("SVG decal mirror"));
                    d.mirror ^= (1 << ax);
                    m_parent.set_as_dirty();
                }
            }
            m_imgui->disabled_end();
            if (d.mirror != 0) {
                bool readable = d.mirror_readable;
                if (m_imgui->bbl_checkbox(_L("Keep readable"), readable)) {
                    snapshot(_u8L("SVG decal mirror"));
                    d.mirror_readable = readable;
                    m_parent.set_as_dirty();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("The copies land in the same mirrored place but are not flipped,\n"
                        "so text and logos read the right way.").c_str());
            }
            }   // Place
            if (neo_ui::section(_u8L("Colours and filaments") + "###decal_colours", true)) {
            const float zones_x = ImGui::GetCursorPosX(), zones_avail = ImGui::GetContentRegionAvail().x;
            for (size_t k = 0; k < d.zones.size(); ++k) {
                colour_column(k, zones_x, zones_avail);
                PaintDecalZone &z = d.zones[k];
                int  state  = z.state;
                bool hidden = z.hidden;
                if (render_svg_colour_row(int(k) + 9000, z.rgb, false, state, hidden)) {
                    snapshot(_u8L("SVG decal colours"));
                    z.state  = state;
                    z.hidden = hidden;
                    m_parent.set_as_dirty();
                }
            }
            }   // Colours
            if (delete_it) {
                snapshot(_u8L("Delete SVG decal"));
                const int del = m_selected_decal;
                decals.erase(decals.begin() + del);
                m_decal_multi.clear();
                if (m_svg_edit_decal == del)
                    m_svg_edit_decal = -1;
                else if (m_svg_edit_decal > del)
                    --m_svg_edit_decal;
                m_selected_decal = decals.empty() ? -1 : std::min(del, int(decals.size()) - 1);
                m_decal_rings_cache.clear();
                m_parent.set_as_dirty();
            }
            ImGui::PopID();
        }
    }

    // --- On screen: the shape or SVG being edited
    ImGui::Separator();
    const bool has_svg   = !m_svg_zones.empty();
    const bool has_shape = !m_shape_nodes.empty();
    if (!has_svg && !has_shape) {
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Load an SVG, or Draw shape above."));
        return;
    }
    ImGui::PushID("on_screen");
    if (has_svg)
        m_imgui->text(m_svg_edit_decal >= 0 ? GUI::format(_L("On screen: editing %1%"), m_svg_name)
                                            : GUI::format(_L("On screen: %1% (not pinned)"), m_svg_name.empty() ? std::string("SVG") : m_svg_name));
    else
        m_imgui->text(GUI::format(m_shape_closed ? _L("On screen: shape, %1% points") : _L("On screen: open shape, %1% points"), m_shape_nodes.size()));

    if (has_svg) {
        m_imgui->text(_L("Drag inside its frame to move it."));
        float pct = m_shape_scale_pct;
        const float slider_w = -(std::max({ ImGui::CalcTextSize(_u8L("Scale").c_str()).x, ImGui::CalcTextSize(_u8L("Rotate").c_str()).x, ImGui::CalcTextSize(_u8L("Width").c_str()).x, ImGui::CalcTextSize(_u8L("Height").c_str()).x }) + style.ItemInnerSpacing.x * 2.f);
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat(_u8L("Scale").c_str(), &pct, 25.f, 400.f, "%.0f%%")) {
            shape_scale(pct / m_shape_scale_pct);
            m_shape_scale_pct = pct;
            m_parent.set_as_dirty();
        }
        float deg = m_shape_rotate_deg;
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat(_u8L("Rotate").c_str(), &deg, -180.f, 180.f, "%.0f deg")) {
            shape_rotate(deg - m_shape_rotate_deg);
            m_shape_rotate_deg = deg;
            m_parent.set_as_dirty();
        }
        // F4b: width and height on their own, along the SVG's axes.
        float wpct = m_svg_width_pct;
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat(_u8L("Width").c_str(), &wpct, 25.f, 400.f, "%.0f%%")) {
            svg_stretch(0, wpct / m_svg_width_pct);
            m_svg_width_pct = wpct;
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Stretch or squeeze the SVG along its own width.").c_str());
        float hpct = m_svg_height_pct;
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat(_u8L("Height").c_str(), &hpct, 25.f, 400.f, "%.0f%%")) {
            svg_stretch(1, hpct / m_svg_height_pct);
            m_svg_height_pct = hpct;
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Stretch or squeeze the SVG along its own height.").c_str());
        // F4: corner deform, handles on the corners of the frame.
        if (m_imgui->bbl_checkbox(_L("Deform corners"), m_svg_deform))
            m_parent.set_as_dirty();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Drag a corner of the frame to bend the SVG in perspective.\n"
                "Straight lines stay straight. A corner that would fold the frame stops.").c_str());
        if (svg_is_deformed()) {
            ImGui::SameLine();
            if (m_imgui->button(_L("Reset corners"))) {
                svg_reset_corners();
                m_parent.set_as_dirty();
            }
        }
        // F5: mirror planes, the copies drawn live on the object while you place it.
        neo_ui::row(_u8L("Mirror"));
        if (const int ax = neo_ui::mirror_toggles("smir", m_svg_mirror, _u8L("Mirrored copies through the centre of the object.\n"
                "They show as cards where they will land, and are kept when you save.")); ax >= 0) {
            m_svg_mirror ^= (1 << ax);
            m_parent.set_as_dirty();
        }
        if (m_svg_mirror != 0) {
            if (m_imgui->bbl_checkbox(_L("Keep readable"), m_svg_mirror_readable))
                m_parent.set_as_dirty();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("The copies land in the same mirrored place but are not flipped,\n"
                    "so text and logos read the right way.").c_str());
        }
        bool masks_dirty = false;
        const float zones_x = ImGui::GetCursorPosX(), zones_avail = ImGui::GetContentRegionAvail().x;
        for (size_t z = 0; z < m_svg_zones.size(); ++z) {
            colour_column(z, zones_x, zones_avail);
            SvgZone &zone = m_svg_zones[z];
            const bool hidden_before = zone.hidden;
            if (render_svg_colour_row(int(z), zone.rgb, zone.grad, zone.state, zone.hidden)) {
                // Parts: the paint groups carry the filaments, so any change rebuilds them.
                masks_dirty = true;
                (void) hidden_before;
                m_parent.set_as_dirty();
            }
        }
        masks_dirty = render_svg_parts() || masks_dirty;
        if (svg_is_drawn_shape()) {
            if (m_imgui->button(_L("Edit points")))
                svg_to_shape_nodes();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Turn this drawn shape back into points and handles to edit them.\n"
                    "Save it again to update the decal. A corner deform is only approximated.").c_str());
        }
        if (masks_dirty) {
            svg_rebuild_masks();
            svg_update_screen_rings();
        }
    }

    // Keep it: as a decal pinned to this view (an SVG, or a closed shape turned into one).
    // An SVG on screen is only adjusted here; it paints from its layer once saved, so what is
    // painted is always what is stored.
    if (has_svg) {
        if (m_imgui->button(m_svg_edit_decal >= 0 ? _L("Save") : _L("Pin to view"))) {
            const int editing = m_svg_edit_decal;
            if (svg_pin_to_view())
                m_selected_decal = editing >= 0 ? editing : int(decals.size()) - 1;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", (m_svg_edit_decal >= 0
                ? _u8L("Store the changes in the decal and close. Then Apply it from its layer.")
                : _u8L("Keep it as a decal of the object, seen from this view.\n"
                       "It floats as a card that follows the object.\nThen Apply it from its layer.")).c_str());
        ImGui::SameLine();
        if (m_imgui->button(_L("Cancel"))) {
            shape_clear();
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Drop the changes. A pinned decal keeps its last saved version.").c_str());
        ImGui::PopID();
        return;
    } else if (!m_shape_closed) {
        m_imgui->disabled_begin(m_shape_nodes.size() < 3);
        if (neo_ui::icon_text_button(_u8L("Close shape"), neo_ui::Icon::Close)) {
            m_shape_closed = true;
            m_parent.set_as_dirty();
        }
        m_imgui->disabled_end();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Join the last point to the first one.").c_str());
    } else {
        if (m_imgui->button(m_shape_from_decal >= 0 ? _L("Save") : _L("Save as decal"))) {
            if (shape_to_svg_session() && svg_pin_to_view())
                m_selected_decal = int(decals.size()) - 1;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Keep the shape as a decal pinned to this view, painting with\n"
                "the selected filament. Edit points brings its points back later.").c_str());
    }

    m_imgui->disabled_begin(!m_shape_closed);
    if (m_imgui->button(_L("Apply")))
        shape_apply(get_left_button_state_type(), std::string(handle_snapshot_action_name(false, Button::Left).ToUTF8().data()));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", (has_svg ? _u8L("Paint the SVG from this view, each colour with its filament.")
                                         : _u8L("Paint the shape with the selected filament.")).c_str());
    ImGui::SameLine();
    if (m_imgui->button(_L("Erase")))
        shape_apply(EnforcerBlockerType::NONE, std::string(handle_snapshot_action_name(true, Button::Left).ToUTF8().data()));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Remove the paint inside it.").c_str());
    m_imgui->disabled_end();
    ImGui::SameLine();
    if (neo_ui::icon_text_button(_u8L("Cancel"), neo_ui::Icon::Cancel)) {
        shape_clear();
        m_parent.set_as_dirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Drop what is on screen. A pinned decal keeps its last version.").c_str());
    if (has_shape) {
        ImGui::SameLine();
        if (m_imgui->button(_L("Undo point")))
            shape_undo_point();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Reopen a closed shape, or remove the last point of an open one.").c_str());
    }
    ImGui::Checkbox(_u8L("Keep on screen after Apply").c_str(), &m_shape_keep_after_apply);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Leave it on screen after Apply,\nto paint it again with another filament or tweak it.").c_str());
    ImGui::PopID();
}

ColorRGBA GLGizmoMmuSegmentation::get_shape_fill_color() const
{
    ColorRGBA color = m_selected_extruder_idx < m_extruders_colors.size() ?
        m_extruders_colors[m_selected_extruder_idx] : ColorRGBA(0.0f, 1.0f, 0.3f, 1.0f);
    color.a(0.35f);
    return color;
}

// F2. `state` is a filament id (1-based, Mixed Filaments included).
ColorRGBA GLGizmoMmuSegmentation::get_state_color(int state) const
{
    if (state >= 1 && size_t(state) <= m_extruders_colors.size())
        return m_extruders_colors[size_t(state) - 1];
    return ColorRGBA(0.6f, 0.6f, 0.6f, 1.0f);
}

// The filament whose colour is closest (RGB distance) to the SVG colour, among the ones the
// panel offers; the selected one if there is none.
int GLGizmoMmuSegmentation::svg_default_state(uint32_t rgb) const
{
    const float r = float((rgb >> 16) & 0xFF) / 255.f, g = float((rgb >> 8) & 0xFF) / 255.f, b = float(rgb & 0xFF) / 255.f;
    int   best      = int(get_left_button_state_type());
    float best_dist = std::numeric_limits<float>::max();
    const size_t n = std::min(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT, m_display_filament_ids.size());
    for (size_t i = 0; i < n; ++i) {
        const int state = int(m_display_filament_ids[i]);
        if (state < 1 || size_t(state) > m_extruders_colors.size())
            continue;
        const ColorRGBA &c = m_extruders_colors[size_t(state) - 1];
        const float dist = (c.r() - r) * (c.r() - r) + (c.g() - g) * (c.g() - g) + (c.b() - b) * (c.b() - b);
        if (dist < best_dist) {
            best_dist = dist;
            best      = state;
        }
    }
    return best;
}

void GLGizmoMmuSegmentation::render_filament_remap_ui(float window_width, float max_tooltip_width)
{
    size_t n_extr = std::min((size_t)EnforcerBlockerType::ExtruderMax, m_display_filament_ids.size());

    const std::string max_label = std::to_string(std::max<size_t>(n_extr, 1));
    const ImVec2 max_label_size = ImGui::CalcTextSize(max_label.c_str(), NULL, true);
    const ImVec2 button_size(max_label_size.x + m_imgui->scaled(0.5f), 0.f);
    const int max_items_per_line = 8;
    const float item_width = button_size.x + m_imgui->scaled(1.5f);
    const float start_pos_x = ImGui::GetCursorPosX();

    for (int src = 0; src < (int)n_extr; ++src) {
        const unsigned int dst_filament_id = m_extruder_remap[src] < m_display_filament_ids.size() ? m_display_filament_ids[m_extruder_remap[src]] : 0;
        if (dst_filament_id == 0 || dst_filament_id > m_extruders_colors.size())
            continue;
        const ColorRGBA &dst_col = m_extruders_colors[dst_filament_id - 1];
        ImVec4 col_vec = ImGuiWrapper::to_ImVec4(dst_col);

        if (src % max_items_per_line != 0) {
            ImGui::SameLine(start_pos_x + item_width * (src % max_items_per_line));
        }
        std::string btn_id = "##remap_src_" + std::to_string(src);
        
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs |
                                    ImGuiColorEditFlags_NoLabel  | ImGuiColorEditFlags_NoPicker |
                                    ImGuiColorEditFlags_NoTooltip;
        if (m_selected_extruder_idx != src) flags |= ImGuiColorEditFlags_NoBorder;
        
        #ifdef __APPLE__
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0);
            bool clicked = ImGui::ColorButton(btn_id.c_str(), col_vec, flags, button_size);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(1);
        #else
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0);
            bool clicked = ImGui::ColorButton(btn_id.c_str(), col_vec, flags, button_size);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(1);
        #endif

        // overlay destination number with proper contrast calculation
        const size_t num_physical = m_mixed_display_context.num_physical;
        std::string dst_txt = (dst_filament_id <= num_physical)
                                  ? std::to_string(dst_filament_id)
                                  : Slic3r::mixed_filament_index_to_letter(dst_filament_id - num_physical - 1);
        float gray = 0.299f * dst_col.r() + 0.587f * dst_col.g() + 0.114f * dst_col.b();
        ImVec2 txt_sz = ImGui::CalcTextSize(dst_txt.c_str());
        ImVec2 pos = ImGui::GetItemRectMin();
        ImVec2 size = ImGui::GetItemRectSize();
        
        if (gray * 255.f < 80.f)
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + (size.x - txt_sz.x) * 0.5f, pos.y + (size.y - txt_sz.y) * 0.5f),
                IM_COL32(255,255,255,255), dst_txt.c_str());
        else
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + (size.x - txt_sz.x) * 0.5f, pos.y + (size.y - txt_sz.y) * 0.5f),
                IM_COL32(0,0,0,255), dst_txt.c_str());

        // popup with possible destinations
        std::string pop_id = "popup_" + std::to_string(src);
        if (clicked) {
            // Calculate popup position centered below the current button
            ImVec2 button_pos = ImGui::GetItemRectMin();
            ImVec2 button_size = ImGui::GetItemRectSize();
            ImVec2 popup_pos(button_pos.x + button_size.x * 0.5f, button_pos.y + button_size.y);
            
            // Set popup styling BEFORE opening popup
            ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Appearing, ImVec2(0.5f, -0.1f));
            ImGui::SetNextWindowBgAlpha(1.0f); // Ensure full opacity
            ImGui::OpenPopup(pop_id.c_str());
        }
        
        // Apply popup styling before BeginPopup using standard Orca colors
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, m_is_dark_mode ? ImGuiWrapper::COL_WINDOW_BG_DARK : ImGuiWrapper::COL_WINDOW_BG);
        ImGui::PushStyleColor(ImGuiCol_Border, m_is_dark_mode ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        
        if (ImGui::BeginPopup(pop_id.c_str())) {
            const float popup_start_pos_x = ImGui::GetCursorPosX();
            
            for (int dst = 0; dst < (int)n_extr; ++dst) {
                const unsigned int popup_filament_id = m_display_filament_ids[dst];
                if (popup_filament_id == 0 || popup_filament_id > m_extruders_colors.size())
                    continue;
                const ColorRGBA &dst_col_popup = m_extruders_colors[popup_filament_id - 1];
                ImVec4 dst_vec = ImGuiWrapper::to_ImVec4(dst_col_popup);
                if (dst % max_items_per_line != 0)
                    ImGui::SameLine(popup_start_pos_x + item_width * (dst % max_items_per_line));
                std::string dst_btn = "##dst_" + std::to_string(src) + "_" + std::to_string(dst);
                
                // Apply same styling to destination buttons
                ImGuiColorEditFlags dst_flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs |
                                               ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoPicker |
                                               ImGuiColorEditFlags_NoTooltip;
                // Show border for currently selected destination filament
                if (m_extruder_remap[src] != dst) dst_flags |= ImGuiColorEditFlags_NoBorder;
                
                #ifdef __APPLE__
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0);
                    bool dst_clicked = ImGui::ColorButton(dst_btn.c_str(), dst_vec, dst_flags, button_size);
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor(1);
                #else
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0);
                    bool dst_clicked = ImGui::ColorButton(dst_btn.c_str(), dst_vec, dst_flags, button_size);
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor(1);
                #endif
                
                // overlay destination number on popup buttons
                std::string dst_num_txt = (popup_filament_id <= num_physical)
                                              ? std::to_string(popup_filament_id)
                                              : Slic3r::mixed_filament_index_to_letter(popup_filament_id - num_physical - 1);
                float dst_gray = 0.299f * dst_col_popup.r() + 0.587f * dst_col_popup.g() + 0.114f * dst_col_popup.b();
                ImVec2 dst_txt_sz = ImGui::CalcTextSize(dst_num_txt.c_str());
                ImVec2 dst_pos = ImGui::GetItemRectMin();
                ImVec2 dst_size = ImGui::GetItemRectSize();
                
                if (dst_gray * 255.f < 80.f)
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(dst_pos.x + (dst_size.x - dst_txt_sz.x) * 0.5f, dst_pos.y + (dst_size.y - dst_txt_sz.y) * 0.5f),
                        IM_COL32(255,255,255,255), dst_num_txt.c_str());
                else
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(dst_pos.x + (dst_size.x - dst_txt_sz.x) * 0.5f, dst_pos.y + (dst_size.y - dst_txt_sz.y) * 0.5f),
                        IM_COL32(0,0,0,255), dst_num_txt.c_str());
                
                if (dst_clicked)
                {
                    m_extruder_remap[src] = dst;
                    // update the source button color immediately
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        
        // Clean up popup styling (always pop, whether popup was open or not)
        ImGui::PopStyleColor(2); // PopupBg and Border
        ImGui::PopStyleVar(2);   // PopupRounding and PopupBorderSize
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.3f));

    if (m_imgui->button(m_desc.at("remap"))) {
        remap_filament_assignments();
        m_show_filament_remap_ui = false;
    }

    ImGui::SameLine();
    if (m_imgui->button(m_desc.at("cancel_remap")))
        m_show_filament_remap_ui = false;
}

void GLGizmoMmuSegmentation::remap_filament_assignments()
{
    if (m_extruder_remap.empty())
        return;

    constexpr size_t MAX_EBT = (size_t)EnforcerBlockerType::ExtruderMax;
    EnforcerBlockerStateMap state_map;

    // identity mapping by default
    for (size_t i = 0; i <= MAX_EBT; ++i)
        state_map[i] = static_cast<EnforcerBlockerType>(i);

    size_t n_extr = std::min({m_extruder_remap.size(), m_display_filament_ids.size(), MAX_EBT});
    bool   any_change = false;
    for (size_t src = 0; src < n_extr; ++src) {
        const size_t dst = m_extruder_remap[src];
        if (dst >= m_display_filament_ids.size())
            continue;

        const unsigned int src_state = m_display_filament_ids[src];
        const unsigned int dst_state = m_display_filament_ids[dst];
        if (src_state == 0 || dst_state == 0 || src_state == dst_state)
            continue;

        state_map[src_state] = static_cast<EnforcerBlockerType>(dst_state);
        if (src_state == 1)
            state_map[0] = static_cast<EnforcerBlockerType>(dst_state);

        any_change = true;
    }
    if (!any_change)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(),
                                  "Remap filament assignments",
                                  UndoRedo::SnapshotType::GizmoAction);

    bool updated = false;
    int idx = -1;
    ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo) return;

    for (ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++idx;
        TriangleSelectorGUI* ts = m_triangle_selectors[idx].get();
        if (!ts) continue;
        ts->remap_triangle_state(state_map);
        ts->request_update_render_data(true);
        updated = true;
    }

    if (updated) {
        wxGetApp().plater()->get_notification_manager()->push_notification(
            _L("Filament remapping finished.").ToStdString());
        update_model_object();
        m_parent.set_as_dirty();
    }
}

} // namespace Slic3r
