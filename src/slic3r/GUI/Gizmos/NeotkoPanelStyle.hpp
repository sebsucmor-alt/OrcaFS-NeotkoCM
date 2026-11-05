// Neotko's own look for gizmo panels (the house style of the Neotko fork).
//
// This is NOT Orca's classic panel style, on purpose: one colour ramp for both themes, teal as the
// accent ("this is yours / selected"), amber only for "something is off", sizes in font heights so
// the panel scales with DPI and language, and hand-drawn rows instead of stock ImGui widgets.
// Used by the Primitive Generator. Purely cosmetic: nothing here decides behaviour.
//
// It is a trimmed copy of the fork's GizmoNeotkoStyle.hpp, with only what the Primitive Generator
// needs.
#ifndef slic3r_NeotkoPanelStyle_hpp_
#define slic3r_NeotkoPanelStyle_hpp_

#include <imgui/imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r { namespace GUI {

// One ramp. Teal = accent, amber (Warn) = only "something is off", greys are steps of one ramp.
enum class NeoCol {
    PanelBg,     // the gizmo WINDOW background (see neo_push_window_style)
    Canvas,      // bottom of the preview gradient
    CanvasTop,   // top of it
    Surface,     // widget backgrounds, inactive borders
    SurfaceHi,
    Accent,
    AccentBright,
    AccentDim,
    AccentGhost,
    Warn,        // "something is off" ONLY
    TextDim,
    Ink,         // brightest text
};

// Both themes: the roles stay, only the luminance flips. The theme comes from the same place as the
// rest of Orca.
inline bool neo_is_dark() { return Slic3r::GUI::wxGetApp().dark_mode(); }

inline ImU32 neo_col_u32_raw(NeoCol c)
{
    if (!neo_is_dark()) {
        switch (c) {
        case NeoCol::PanelBg:      return IM_COL32(246, 247, 249, 235);
        case NeoCol::Canvas:       return IM_COL32(231, 235, 240, 255);
        case NeoCol::CanvasTop:    return IM_COL32(243, 246, 249, 255);
        case NeoCol::Surface:      return IM_COL32(222, 227, 233, 255);
        case NeoCol::SurfaceHi:    return IM_COL32(205, 212, 220, 255);
        case NeoCol::Accent:       return IM_COL32(  0, 150, 138, 255);
        case NeoCol::AccentBright: return IM_COL32(  0, 118, 108, 255); // on light, "bright" = more contrast
        case NeoCol::AccentDim:    return IM_COL32(176, 222, 216, 255);
        case NeoCol::AccentGhost:  return IM_COL32(  0, 150, 138,  42);
        case NeoCol::Warn:         return IM_COL32(188,  98,   0, 255);
        case NeoCol::TextDim:      return IM_COL32( 98, 108, 120, 255);
        case NeoCol::Ink:          return IM_COL32( 24,  28,  34, 255);
        }
        return IM_COL32(255, 0, 255, 255); // loud on purpose: an unhandled enum should be seen
    }
    switch (c) {
    case NeoCol::PanelBg:      return IM_COL32( 26,  26,  26, 204); // Orca's own, {0.1, 0.1, 0.1, 0.8}
    case NeoCol::Canvas:       return IM_COL32( 18,  21,  26, 255);
    case NeoCol::CanvasTop:    return IM_COL32( 28,  33,  40, 255);
    case NeoCol::Surface:      return IM_COL32( 44,  50,  58, 255);
    case NeoCol::SurfaceHi:    return IM_COL32( 58,  66,  76, 255);
    case NeoCol::Accent:       return IM_COL32(  0, 170, 155, 255);
    case NeoCol::AccentBright: return IM_COL32( 46, 214, 196, 255);
    case NeoCol::AccentDim:    return IM_COL32(  0, 120, 110, 255);
    case NeoCol::AccentGhost:  return IM_COL32( 46, 214, 196,  30);
    case NeoCol::Warn:         return IM_COL32(255, 150,  50, 255);
    case NeoCol::TextDim:      return IM_COL32(150, 160, 170, 255);
    case NeoCol::Ink:          return IM_COL32(226, 232, 238, 255);
    }
    return IM_COL32(255, 0, 255, 255);
}

// For PushStyleColor (ImGui applies style.Alpha itself).
inline ImVec4 neo_col(NeoCol c) { return ImGui::ColorConvertU32ToFloat4(neo_col_u32_raw(c)); }

// For the draw list: hand-drawn things ignore style.Alpha, so it is applied here.
inline ImU32 neo_col_u32(NeoCol c)
{
    ImVec4 v = neo_col(c);
    v.w *= ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

inline ImU32 neo_fade(NeoCol c, float alpha)
{
    ImVec4 v = neo_col(c);
    v.w *= alpha * ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// The WINDOW style: push BEFORE GizmoImguiBegin(), pop AFTER GizmoImguiEnd(). ImGui resolves the
// window background and title colours when the window opens, so pushing later paints nothing.
// Skipping the pop leaks this background into every other gizmo panel.
inline void neo_push_window_style()
{
    ImGui::PushStyleColor(ImGuiCol_WindowBg,      neo_col(NeoCol::PanelBg));
    ImGui::PushStyleColor(ImGuiCol_PopupBg,       neo_col(NeoCol::PanelBg));
    ImGui::PushStyleColor(ImGuiCol_TitleBg,       neo_col(NeoCol::PanelBg));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, neo_col(NeoCol::PanelBg));
    ImGui::PushStyleColor(ImGuiCol_Border,        neo_col(NeoCol::Surface));
    ImGui::PushStyleColor(ImGuiCol_Separator,     neo_col(NeoCol::Surface));
    ImGui::PushStyleColor(ImGuiCol_Text,          neo_col(NeoCol::Ink));
}
inline void neo_pop_window_style() { ImGui::PopStyleColor(7); }

// The widget style: between GizmoImguiBegin() and the body; pop before GizmoImguiEnd(). An early
// `return` that skips the pop leaks into every other ImGui window.
inline void neo_push_panel_style()
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,   4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding,    4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,   5.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,     ImVec2(8.f, 6.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,    ImVec2(7.f, 4.f));
    ImGui::PushStyleColor(ImGuiCol_Button,           neo_col(NeoCol::Surface));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,    neo_col(NeoCol::AccentDim));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,     neo_col(NeoCol::Accent));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,          neo_col(NeoCol::Surface));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,   neo_col(NeoCol::SurfaceHi));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,    neo_col(NeoCol::SurfaceHi));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,       neo_col(NeoCol::Accent));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, neo_col(NeoCol::AccentBright));
    ImGui::PushStyleColor(ImGuiCol_CheckMark,        neo_col(NeoCol::AccentBright));
    ImGui::PushStyleColor(ImGuiCol_Header,           neo_col(NeoCol::AccentDim));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,    neo_col(NeoCol::SurfaceHi));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,     neo_col(NeoCol::Accent));
}
inline void neo_pop_panel_style()
{
    ImGui::PopStyleColor(12);
    ImGui::PopStyleVar(6);
}

// The preview background: rounded base, then the vertical gradient inset by a pixel.
inline void neo_draw_canvas(ImDrawList* dl, const ImVec2& p0, float width, float height)
{
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), neo_col_u32(NeoCol::Canvas), 6.0f);
    dl->AddRectFilledMultiColor(ImVec2(p0.x + 1.f, p0.y + 1.f), ImVec2(p0.x + width - 1.f, p0.y + height - 1.f),
                                neo_col_u32(NeoCol::CanvasTop), neo_col_u32(NeoCol::CanvasTop),
                                neo_col_u32(NeoCol::Canvas), neo_col_u32(NeoCol::Canvas));
}

// Readable text on any background colour (filament colours can be anything).
inline ImU32 ink_on(ImU32 bg)
{
    const float r = float((bg >> IM_COL32_R_SHIFT) & 0xFF);
    const float g = float((bg >> IM_COL32_G_SHIFT) & 0xFF);
    const float b = float((bg >> IM_COL32_B_SHIFT) & 0xFF);
    return (0.299f * r + 0.587f * g + 0.114f * b) > 140.f ? IM_COL32(18, 21, 26, 255) : IM_COL32(236, 240, 244, 255);
}

// The panel unit is the FONT, not the pixel: every size is in font heights, so the panel grows with
// the font (DPI, language) and nothing gets squeezed.
inline float neo_u() { return ImGui::GetFontSize(); }

inline float neo_label_col_current() { return ImGui::GetContentRegionAvail().x * 0.40f; }

// Small hand-drawn icons.
enum class Glyph { Trash, Warn, Plus, Save };

inline void draw_glyph(ImDrawList* dl, const ImVec2& p0, float s, Glyph g, ImU32 col)
{
    auto        P  = [&](float u, float v) { return ImVec2(p0.x + u * s, p0.y + v * s); };
    const float th = std::max(1.2f, s * 0.075f);
    switch (g) {
    case Glyph::Trash: {
        dl->AddLine(P(0.18f, 0.28f), P(0.82f, 0.28f), col, th);
        dl->AddLine(P(0.40f, 0.20f), P(0.60f, 0.20f), col, th);
        const ImVec2 body[4] = {P(0.26f, 0.32f), P(0.74f, 0.32f), P(0.66f, 0.84f), P(0.34f, 0.84f)};
        dl->AddPolyline(body, 4, col, ImDrawFlags_Closed, th);
        break;
    }
    case Glyph::Warn: {
        const ImVec2 tri[3] = {P(0.50f, 0.14f), P(0.94f, 0.84f), P(0.06f, 0.84f)};
        dl->AddConvexPolyFilled(tri, 3, (col & ~IM_COL32_A_MASK) | (55u << IM_COL32_A_SHIFT));
        dl->AddPolyline(tri, 3, col, ImDrawFlags_Closed, th);
        dl->AddLine(P(0.50f, 0.38f), P(0.50f, 0.60f), col, th * 1.1f);
        dl->AddCircleFilled(P(0.50f, 0.72f), th * 0.75f, col, 8);
        break;
    }
    case Glyph::Plus:
        dl->AddLine(P(0.50f, 0.20f), P(0.50f, 0.80f), col, th * 1.1f);
        dl->AddLine(P(0.20f, 0.50f), P(0.80f, 0.50f), col, th * 1.1f);
        break;
    case Glyph::Save: { // a bookmark: "keep this"
        const ImVec2 mark[5] = {P(0.28f, 0.14f), P(0.72f, 0.14f), P(0.72f, 0.86f), P(0.50f, 0.68f), P(0.28f, 0.86f)};
        dl->AddRectFilled(P(0.28f, 0.14f), P(0.72f, 0.66f), (col & ~IM_COL32_A_MASK) | (90u << IM_COL32_A_SHIFT));
        dl->AddPolyline(mark, 5, col, ImDrawFlags_Closed, th);
        break;
    }
    }
}

// Cuts a label to max_w with an ellipsis, without splitting a UTF-8 character.
inline std::string fit_text(const std::string& s, float max_w)
{
    if (ImGui::CalcTextSize(s.c_str()).x <= max_w)
        return s;
    std::string out = s;
    while (!out.empty()) {
        out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back();
        if (ImGui::CalcTextSize((out + "…").c_str()).x <= max_w)
            break;
    }
    return out + "…";
}

// A section title: the label and a rule to the edge, in one line.
inline void neo_section(const char* label)
{
    ImGui::Spacing();
    ImDrawList*  dl    = ImGui::GetWindowDrawList();
    const ImVec2 p     = ImGui::GetCursorScreenPos();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts    = ImGui::CalcTextSize(label);
    dl->AddText(p, neo_fade(NeoCol::Ink, 0.90f), label);
    const float x0 = p.x + ts.x + 8.f, x1 = p.x + avail;
    if (x1 > x0)
        dl->AddLine(ImVec2(x0, p.y + ts.y * 0.55f), ImVec2(x1, p.y + ts.y * 0.55f), neo_fade(NeoCol::SurfaceHi, 0.85f), 1.f);
    ImGui::Dummy(ImVec2(avail, ts.y + 3.f));
}

// A small icon button. `danger` turns it amber on hover (delete vs. anything else).
inline bool neo_glyph_button(const char* id, float size, Glyph g, bool danger, const char* tip,
                             NeoCol tint = NeoCol::TextDim, bool always_lit = false)
{
    const bool   pressed  = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList*  dl       = ImGui::GetWindowDrawList();
    const ImVec2 a        = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool   hv       = ImGui::IsItemHovered();
    const bool   own_tint = (tint != NeoCol::TextDim);
    if (hv || always_lit)
        dl->AddRectFilled(a, b, danger ? neo_fade(NeoCol::Warn, hv ? 0.22f : 0.10f)
                              : own_tint ? neo_fade(tint, hv ? 0.30f : 0.14f)
                                         : neo_fade(NeoCol::Accent, 0.28f), 4.f);
    draw_glyph(dl, ImVec2(a.x + size * 0.18f, a.y + size * 0.18f), size * 0.64f, g,
               own_tint ? neo_fade(tint, hv ? 1.0f : 0.85f)
                        : (hv ? (danger ? neo_col_u32(NeoCol::Warn) : neo_col_u32(NeoCol::AccentBright))
                              : neo_fade(NeoCol::TextDim, 0.9f)));
    if (hv && tip != nullptr)
        ImGui::SetTooltip("%s", tip);
    return pressed;
}

// A status pill (not a control).
inline void neo_status_chip(const char* text, ImU32 col, bool solid)
{
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float  w = ts.x + 0.95f * neo_u(), h = ts.y + 0.36f * neo_u();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), solid ? col : ((col & ~IM_COL32_A_MASK) | (36u << IM_COL32_A_SHIFT)), h * 0.5f);
    if (!solid)
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), (col & ~IM_COL32_A_MASK) | (170u << IM_COL32_A_SHIFT), h * 0.5f, 0, 1.f);
    dl->AddText(ImVec2(p.x + 0.47f * neo_u(), p.y + 0.18f * neo_u()), solid ? ink_on(col) : col, text);
    ImGui::Dummy(ImVec2(w, h));
}

// A slider with its label in a fixed column on the left, so several sliders read as a table.
inline bool neo_row_slider(const char* id, const char* caption, float* v, float mn, float mx, const char* fmt,
                           const char* tip, float label_col = -1.f, ImGuiSliderFlags flags = 0)
{
    const float col = (label_col > 0.f) ? label_col : neo_label_col_current();
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::TextUnformatted(fit_text(caption, col - 0.4f * neo_u()).c_str()); // cut, never under the slider
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip != nullptr ? tip : caption);
    ImGui::SameLine(col);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool changed = ImGui::SliderFloat("##v", v, mn, mx, fmt, flags);
    if (tip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return changed;
}

// The same row with a check box.
inline bool neo_row_toggle(const char* id, const char* caption, bool* v, const char* tip, float label_col = -1.f)
{
    const float col  = (label_col > 0.f) ? label_col : neo_label_col_current();
    const float box  = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float room = std::max(col - 0.4f * neo_u(), ImGui::GetContentRegionAvail().x - box - 0.4f * neo_u());
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::TextUnformatted(fit_text(caption, room).c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip != nullptr ? tip : caption);
    if (ImGui::GetItemRectSize().x + 0.4f * neo_u() < col)
        ImGui::SameLine(col);
    else
        ImGui::SameLine(0.f, 0.4f * neo_u());
    const bool changed = ImGui::Checkbox("##v", v);
    if (tip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return changed;
}

// A warning strip: amber triangle, the short sentence, the whole reason in the tooltip.
inline void neo_warn_row(const char* id, const char* text, const char* why, bool amber = true)
{
    const ImU32  col   = amber ? neo_col_u32(NeoCol::Warn) : neo_col_u32(NeoCol::TextDim);
    const float  u     = neo_u();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const float  gut   = 1.55f * u;
    const float  wrap  = avail - gut - 0.3f * u;
    const ImVec2 ts    = ImGui::CalcTextSize(text, nullptr, false, wrap);
    const float  h     = std::max(1.3f * u, ts.y + 0.5f * u);
    ImGui::InvisibleButton(id, ImVec2(avail, h));
    ImDrawList*  dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    dl->AddRectFilled(a, b, (col & ~IM_COL32_A_MASK) | (24u << IM_COL32_A_SHIFT), 4.f);
    dl->AddRectFilled(a, ImVec2(a.x + 2.f, b.y), col, 1.f);
    draw_glyph(dl, ImVec2(a.x + 0.4f * u, a.y + (h - 0.8f * u) * 0.5f), 0.8f * u, Glyph::Warn, col);
    dl->AddText(nullptr, 0.f, ImVec2(a.x + gut, a.y + 0.25f * u), col, text, nullptr, wrap);
    if (why != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", why);
}

// "Same line, starting at this SCREEN x". Not ImGui::SameLine(x): inside a group that x is added to
// the group's offset and, in an AlwaysAutoResize window, the panel grows without end.
inline void neo_same_line_at(float screen_x)
{
    ImGui::SameLine();
    const ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(std::max(c.x, screen_x), c.y));
}

// Word buttons: Normal = surface, Ghost = outline only (secondary), Accent = teal outline (the
// action the panel is asking for).
enum class NeoBtn { Normal, Ghost, Accent };

// Where the VISIBLE part of an ImGui label ends ("##" starts the id).
inline const char* neo_label_end(const char* label)
{
    const char* hash = std::strstr(label, "##");
    return hash != nullptr ? hash : label + std::strlen(label);
}

inline bool neo_text_button(const char* label, NeoBtn style = NeoBtn::Normal, bool has_glyph = false, Glyph g = Glyph::Plus,
                            float width = 0.f)
{
    const float  u       = neo_u();
    const char*  end     = neo_label_end(label);
    const ImVec2 ts      = ImGui::CalcTextSize(label, end);
    const float  gs      = has_glyph ? 0.95f * u : 0.f;
    const float  pad     = 0.6f * u;
    const float  h       = 1.75f * u;
    const float  w       = (width > 0.f) ? width : ts.x + gs + (has_glyph ? 0.35f * u : 0.f) + 2.f * pad;
    const bool   pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    ImDrawList*  dl      = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool   hv  = ImGui::IsItemHovered();
    ImU32        txt = neo_col_u32(NeoCol::Ink);
    switch (style) {
    case NeoBtn::Normal: dl->AddRectFilled(a, b, neo_col_u32(hv ? NeoCol::AccentDim : NeoCol::Surface), 4.f); break;
    case NeoBtn::Ghost:
        if (hv) dl->AddRectFilled(a, b, neo_col_u32(NeoCol::AccentGhost), 4.f);
        dl->AddRect(a, b, neo_col_u32(hv ? NeoCol::AccentBright : NeoCol::SurfaceHi), 4.f, 0, 1.f);
        txt = neo_col_u32(hv ? NeoCol::AccentBright : NeoCol::TextDim);
        break;
    case NeoBtn::Accent:
        dl->AddRectFilled(a, b, neo_fade(NeoCol::Accent, hv ? 0.28f : 0.10f), 4.f);
        dl->AddRect(a, b, neo_col_u32(NeoCol::Accent), 4.f, 0, 1.f);
        txt = neo_col_u32(NeoCol::AccentBright);
        break;
    }
    float x = (width > 0.f) ? a.x + (w - (ts.x + gs + (has_glyph ? 0.35f * u : 0.f))) * 0.5f : a.x + pad;
    if (has_glyph) {
        draw_glyph(dl, ImVec2(x, a.y + (h - gs) * 0.5f), gs, g, style == NeoBtn::Normal ? neo_col_u32(NeoCol::AccentBright) : txt);
        x += gs + 0.35f * u;
    }
    dl->AddText(ImVec2(x, a.y + (h - ts.y) * 0.5f), txt, label, end);
    return pressed;
}

}} // namespace Slic3r::GUI

#endif
