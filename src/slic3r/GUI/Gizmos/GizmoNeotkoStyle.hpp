// NEOTKO_GIZMOSTYLE_TAG_START — shared look for the Neotko curve gizmos (s248).
//
// Born inside GLGizmoHeightAdaptiveEffects and pulled out here the moment a SECOND gizmo wanted
// it, which is the only honest reason to share anything. Project owner's framing: "Orca's style is
// Orca's, but these gizmos are ours" — so this is the one place that decides what ours looks like.
//
// Users today: GLGizmoHeightAdaptiveEffects, GLGizmoPrecisionALH. They are near-twins by
// construction (the HAE editor was cloned from the ALH one: same per-object session, same
// click-to-add / drag / right-click-to-delete, same curve drawn over a Z axis), so them looking
// like two unrelated tools was an accident of history, not a decision.
//
// ⚠️ This header is PURELY cosmetic. Nothing here may ever gate behaviour, and no colour may
// acquire a meaning that is not already carried by the code that draws it.
#ifndef slic3r_GizmoNeotkoStyle_hpp_
#define slic3r_GizmoNeotkoStyle_hpp_

#include <imgui/imgui.h>

namespace Slic3r { namespace GUI {

// One ramp, not eleven loose IM_COL32s. Before this the two editors had five different greys
// between them and no two matched. Rules of the palette:
//   - Teal is the accent and means "this is your curve / your selection".
//   - Amber (Warn) is reserved EXCLUSIVELY for "something is off". Never decorative.
//   - Every grey is a step on the same ramp, so a panel reads as one surface.
//   - The semantic colours at the end exist because ALH's editor genuinely encodes meaning in
//     colour (forbidden band, optimal line, locked point) and those meanings predate this file.
enum class NeoCol {
    Canvas,        // bottom of the graph gradient
    CanvasTop,     // top of it
    Surface,       // widget backgrounds, inactive borders
    SurfaceHi,
    Grid,          // minor gridlines
    GridMajor,     // labelled ticks, first-layer marker
    Accent,
    AccentBright,
    AccentDim,
    AccentGhost,   // area fill under a curve
    Warn,          // ⚠️ "something is off" ONLY
    TextDim,
    Ink,           // brightest text
    // Semantic, ALH's editor:
    Forbid,        // band the envelope rules out
    Optimal,       // the suggested-height line
    Slope,         // informational slope-exposure shading
    Locked,        // a point the user cannot move
    Endpoint,      // the top endpoint, height-movable only
};

inline ImU32 neo_col_u32(NeoCol c)
{
    switch (c) {
    case NeoCol::Canvas:       return IM_COL32( 18,  21,  26, 255);
    case NeoCol::CanvasTop:    return IM_COL32( 28,  33,  40, 255);
    case NeoCol::Surface:      return IM_COL32( 44,  50,  58, 255);
    case NeoCol::SurfaceHi:    return IM_COL32( 58,  66,  76, 255);
    case NeoCol::Grid:         return IM_COL32( 52,  60,  69, 110);
    case NeoCol::GridMajor:    return IM_COL32( 88, 100, 112, 190);
    case NeoCol::Accent:       return IM_COL32(  0, 170, 155, 255);
    case NeoCol::AccentBright: return IM_COL32( 46, 214, 196, 255);
    case NeoCol::AccentDim:    return IM_COL32(  0, 120, 110, 255);
    case NeoCol::AccentGhost:  return IM_COL32( 46, 214, 196,  30);
    case NeoCol::Warn:         return IM_COL32(255, 150,  50, 255);
    case NeoCol::TextDim:      return IM_COL32(150, 160, 170, 255);
    case NeoCol::Ink:          return IM_COL32(226, 232, 238, 255);
    case NeoCol::Forbid:       return IM_COL32(214,  69,  69, 255);
    case NeoCol::Optimal:      return IM_COL32( 74, 222, 128, 255);
    case NeoCol::Slope:        return IM_COL32(167, 110, 232, 255);
    case NeoCol::Locked:       return IM_COL32(126, 136, 146, 255);
    case NeoCol::Endpoint:     return IM_COL32(240, 190,  60, 255);
    }
    return IM_COL32(255, 0, 255, 255); // loud on purpose: an unhandled enum should be seen
}

inline ImVec4 neo_col(NeoCol c) { return ImGui::ColorConvertU32ToFloat4(neo_col_u32(c)); }

// The same colour at a different opacity, without spelling out the RGB again.
inline ImU32 neo_fade(NeoCol c, float alpha)
{
    ImVec4 v = neo_col(c);
    v.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// The widget styling both panels push. Call between GizmoImguiBegin() and the panel body, and pair
// it with neo_pop_panel_style() before GizmoImguiEnd().
//
// 🚨 A PushStyleColor whose Pop is skipped by an early `return` LEAKS INTO EVERY OTHER IMGUI
// WINDOW. If the panel body has early exits, put the body in its own function and keep the
// push/pop in the caller — that is exactly why GLGizmoHeightAdaptiveEffects has
// render_panel_body().
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

// The graph background both editors sit on: rounded base, then the vertical gradient inset by a
// pixel. AddRectFilledMultiColor has no rounding parameter, which is why it takes two calls.
inline void neo_draw_canvas(ImDrawList* dl, const ImVec2& p0, float width, float height)
{
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + height), neo_col_u32(NeoCol::Canvas), 6.0f);
    dl->AddRectFilledMultiColor(ImVec2(p0.x + 1.f, p0.y + 1.f),
                                ImVec2(p0.x + width - 1.f, p0.y + height - 1.f),
                                neo_col_u32(NeoCol::CanvasTop), neo_col_u32(NeoCol::CanvasTop),
                                neo_col_u32(NeoCol::Canvas),    neo_col_u32(NeoCol::Canvas));
}

// A floating readout: pill, not a box. Draws at `pos` and sizes itself to `text`.
inline void neo_draw_pill(ImDrawList* dl, const ImVec2& pos, const char* text, ImU32 border)
{
    const ImVec2 tsz = ImGui::CalcTextSize(text);
    const float  r   = (tsz.y + 4.f) * 0.5f;
    const ImVec2 a(pos.x - 7.f, pos.y - 2.f), b(pos.x + tsz.x + 7.f, pos.y + tsz.y + 2.f);
    dl->AddRectFilled(a, b, neo_fade(NeoCol::Canvas, 0.95f), r);
    dl->AddRect(a, b, border, r, 0, 1.2f);
    dl->AddText(pos, neo_col_u32(NeoCol::Ink), text);
}

// A curve point: optional halo when live, filled disc, and a ring in the CANVAS colour rather than
// black — a black outline on a dark gradient reads as a hole punched in the graph, this reads as a
// bead sitting on the curve.
inline void neo_draw_node(ImDrawList* dl, const ImVec2& c, float radius, ImU32 fill, bool lit)
{
    if (lit) {
        ImVec4 halo = ImGui::ColorConvertU32ToFloat4(fill);
        halo.w = 0.22f;
        dl->AddCircleFilled(c, radius + 5.f, ImGui::ColorConvertFloat4ToU32(halo), 20);
    }
    dl->AddCircleFilled(c, radius, fill, 20);
    dl->AddCircle(c, radius, neo_col_u32(NeoCol::Canvas), 20, 1.6f);
}

}} // namespace Slic3r::GUI

#endif
// NEOTKO_GIZMOSTYLE_TAG_END
