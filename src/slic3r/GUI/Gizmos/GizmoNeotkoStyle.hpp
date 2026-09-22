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

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>

#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r { namespace GUI {

// One ramp, not eleven loose IM_COL32s. Before this the two editors had five different greys
// between them and no two matched. Rules of the palette:
//   - Teal is the accent and means "this is your curve / your selection".
//   - Amber (Warn) is reserved EXCLUSIVELY for "something is off". Never decorative.
//   - Every grey is a step on the same ramp, so a panel reads as one surface.
//   - The semantic colours at the end exist because ALH's editor genuinely encodes meaning in
//     colour (forbidden band, optimal line, locked point) and those meanings predate this file.
enum class NeoCol {
    PanelBg,       // el fondo de la VENTANA del gizmo (ver neo_push_window_style)
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

// s290 — LA RAMPA TIENE DOS CARAS, Y LA VENTANA TAMBIEN.
//
// Primer intento fallido, que conviene tener escrito: solo con dar colores claros a los widgets no
// basta. El fondo de un panel de gizmo lo fija ImGuiWrapper::init_style() (ImGuiWrapper.cpp:2929)
// con COL_WINDOW_BACKGROUND = {0.1, 0.1, 0.1, 0.8}, una vez y SIN rama de tema, y el texto por
// defecto se queda blanco. Pintar widgets claros ahi dentro daba "30 deg" en blanco sobre un
// deslizador claro y botones ilegibles.
//
// 🔑 La ventana se arregla empujando ImGuiCol_WindowBg ANTES del Begin, que es lo que hace
// neo_push_window_style(). Con eso la ventana es nuestra en los dos temas y los widgets encajan.
//
// Los papeles NO cambian de una cara a otra: Canvas sigue siendo el lienzo, Ink el texto mas
// legible, el ambar sigue significando "algo va mal". Solo se invierte la luminancia, y quien
// dibuja no se entera de nada.
//
// 🚨 El tema se lee de wxGetApp().dark_mode(), el MISMO sitio del que come el resto de Orca. No
// inventes aqui una segunda fuente de verdad.
inline bool neo_is_dark() { return Slic3r::GUI::wxGetApp().dark_mode(); }

inline ImU32 neo_col_u32_raw(NeoCol c)
{
    if (! neo_is_dark()) {
        switch (c) {
        case NeoCol::PanelBg:      return IM_COL32(246, 247, 249, 235);
        case NeoCol::Canvas:       return IM_COL32(231, 235, 240, 255);
        case NeoCol::CanvasTop:    return IM_COL32(243, 246, 249, 255);
        case NeoCol::Surface:      return IM_COL32(222, 227, 233, 255);
        case NeoCol::SurfaceHi:    return IM_COL32(205, 212, 220, 255);
        case NeoCol::Grid:         return IM_COL32(158, 167, 178, 110);
        case NeoCol::GridMajor:    return IM_COL32(118, 128, 140, 190);
        case NeoCol::Accent:       return IM_COL32(  0, 150, 138, 255);
        case NeoCol::AccentBright: return IM_COL32(  0, 118, 108, 255); // en claro "brillante" = mas contraste, no mas luz
        case NeoCol::AccentDim:    return IM_COL32(176, 222, 216, 255);
        case NeoCol::AccentGhost:  return IM_COL32(  0, 150, 138,  42);
        case NeoCol::Warn:         return IM_COL32(188,  98,   0, 255);
        case NeoCol::TextDim:      return IM_COL32( 98, 108, 120, 255);
        case NeoCol::Ink:          return IM_COL32( 24,  28,  34, 255);
        case NeoCol::Forbid:       return IM_COL32(190,  44,  44, 255);
        case NeoCol::Optimal:      return IM_COL32( 24, 142,  66, 255);
        case NeoCol::Slope:        return IM_COL32(118,  64, 194, 255);
        case NeoCol::Locked:       return IM_COL32(134, 142, 152, 255);
        case NeoCol::Endpoint:     return IM_COL32(174, 126,  12, 255);
        }
        return IM_COL32(255, 0, 255, 255); // loud on purpose: an unhandled enum should be seen
    }

    switch (c) {
    case NeoCol::PanelBg:      return IM_COL32( 26,  26,  26, 204); // el de Orca, {0.1,0.1,0.1,0.8}
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

// For PushStyleColor: ImGui applies style.Alpha to style colours by itself, so this one stays raw.
inline ImVec4 neo_col(NeoCol c) { return ImGui::ColorConvertU32ToFloat4(neo_col_u32_raw(c)); }

// s318 — for the DRAW LIST. Anything drawn by hand ignores style.Alpha, so a panel greyed out with
// PushStyleVar(Alpha) (ImGuiWrapper::disabled_begin) kept its hand-drawn icons and cards at full
// brightness. Multiplying here makes them fade with everything else. At Alpha = 1 (every panel that
// never disables anything) the result is bit-for-bit what it was.
inline ImU32 neo_col_u32(NeoCol c)
{
    ImVec4 v = neo_col(c);
    v.w *= ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// The same colour at a different opacity, without spelling out the RGB again.
inline ImU32 neo_fade(NeoCol c, float alpha)
{
    ImVec4 v = neo_col(c);
    v.w *= alpha * ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(v);
}

// 🔑 EL ESTILO DE LA VENTANA. Va ANTES de GizmoImguiBegin(), y su pop DESPUES de GizmoImguiEnd().
//
// Tiene que ser antes del Begin porque ImGui resuelve WindowBg y el color del titulo al abrir la
// ventana: empujarlo despues (donde vive neo_push_panel_style) no pinta nada. ImGuiCol_Text va
// aqui, y no en el estilo de widgets, por lo mismo — asi cubre el titulo ademas del cuerpo, y de
// paso todo texto que no pida un color explicito deja de ser blanco fijo.
//
// 🚨 ImGuiCol_ChildBg se queda FUERA a proposito: ALH y HAE abren BeginChild, y un fondo opaco
// ahi se suma al de la ventana y ennegrece esas zonas. Los hijos heredan, que es lo correcto.
//
// 🚨 Si te saltas el pop, el fondo claro se cuela en TODOS los demas paneles de gizmo de Orca.
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


// =================================================================================================
// s318 — EL LENGUAJE DE PANEL, compartido. Nació en GLGizmoSupportZones.cpp (s287) con un solo
// usuario y sube aquí cuando llega el segundo, el ColorStitch Painter, que es exactamente la regla
// con la que nació esta cabecera. Mismo código, mismos comentarios; sólo cambia dónde vive.
// ⚠️ Sigue siendo PURAMENTE cosmético: nada de aquí decide comportamiento.
// =================================================================================================

// Los colores de un filamento se eligen a gusto del usuario, así que un texto encima puede caer
// sobre amarillo puro o sobre negro. La luminancia decide, como en cualquier etiqueta de color.
inline ImU32 ink_on(ImU32 bg)
{
    const float r = float((bg >> IM_COL32_R_SHIFT) & 0xFF);
    const float g = float((bg >> IM_COL32_G_SHIFT) & 0xFF);
    const float b = float((bg >> IM_COL32_B_SHIFT) & 0xFF);
    return (0.299f * r + 0.587f * g + 0.114f * b) > 140.f ? IM_COL32(18, 21, 26, 255)
                                                          : IM_COL32(236, 240, 244, 255);
}

// 🔑 s287-bis — LA UNIDAD DEL PANEL ES LA FUENTE, no el píxel.
//
// Primera versión del rediseño: todo en píxeles fijos. Con la fuente de Orca (18 px de base, y más
// con DPI alto) las etiquetas se comían la columna del deslizador y los iconos quedaban apretados
// contra el texto — lo que se vio en la captura. La cura no es subir los números a ojo: es que cada
// medida se exprese en ALTURAS DE FUENTE, así que el panel se estira solo cuando la fuente crece y
// no hay ninguna combinación de idioma y DPI que lo pueda apretar.
inline float neo_u() { return ImGui::GetFontSize(); }

// La columna donde empiezan los deslizadores. Se mide sobre las etiquetas DE VERDAD, ya traducidas,
// para que la tabla siga cuadrando en alemán o en chino. El tope existe para que una traducción
// larguísima no deje el deslizador sin sitio: a partir de ahí la etiqueta se recorta.
//
// s318 — subida aquí desde GLGizmoSupportZones.cpp. Cada panel mide sobre SUS etiquetas, así que
// la lista entra por parámetro, y cada panel registra al empezar su frame la función que mide su
// columna (neo_set_label_col_fn). neo_row_slider/neo_row_toggle la llaman en el momento de pintar
// la fila, exactamente como hacían antes de mudarse: nada de Zonas cambia de sitio.
inline float neo_label_col_for(std::initializer_list<std::string> caps)
{
    float w = 0.f;
    for (const std::string &c : caps)
        w = std::max(w, ImGui::CalcTextSize(c.c_str()).x);
    return std::min(w + 0.7f * neo_u(), ImGui::GetContentRegionAvail().x * 0.46f);
}

using NeoLabelColFn = float (*)();
inline NeoLabelColFn &neo_label_col_fn_slot() { static NeoLabelColFn fn = nullptr; return fn; }
inline void neo_set_label_col_fn(NeoLabelColFn fn) { neo_label_col_fn_slot() = fn; }
inline float neo_label_col_current()
{
    if (NeoLabelColFn fn = neo_label_col_fn_slot())
        return fn();
    return ImGui::GetContentRegionAvail().x * 0.40f;
}

// -----------------------------------------------------------------------------
// Los iconos
// -----------------------------------------------------------------------------
// Vectoriales y dibujados a mano, como en Align & Stack, y por la misma razón: escalan con el DPI
// sin pedirle nada al empaquetado de recursos, se recolorean solos según el estado, y un icono que
// es código se corrige en el sitio donde se usa.
//
// Todos se dibujan dentro del cuadrado [0,1] escalado por `s`, así que el mismo glifo sirve para un
// botón de 22 px y para un dibujo de 40.
enum class Glyph {
    Overhang,   // el sombreado de voladizos
    GapMap,     // el mapa de lo que nadie sujeta
    Xray,       // la pieza fantasma
    Patch,      // huella: el parche entero
    Round,      // huella: recorte redondo
    Square,     // huella: recorte cuadrado
    Brush,      // huella: pintada a mano (s289)
    Trash,
    Copy,
    Cube,       // "como el objeto"
    Warn,
    Target,     // el 1er clic: la superficie a sujetar
    Landing,    // el 2º clic: dónde aterriza
    Pillar,     // crear
    Lock,       // s288 — la zona nació del gesto y nadie la ha tocado por fuera
    Unlock,     // s288 — la acción de soltarla
    Edit,       // s288 — la acción de volver a editarla
    // s318 — los del ColorStitch Painter. Añadidos AL FINAL a propósito: Zonas guarda y compara
    // estos valores en su sitio y reordenar el enum cambiaría los suyos sin avisar.
    Select,     // herramienta: marcar objetos
    Paint,      // herramienta: smart fill
    Erase,      // herramienta: goma
    Pick,       // herramienta: cuentagotas
    Sticker,    // herramienta: pegatina SVG
    EraseAll,   // borrar TODA la pintura (peligro)
    Plus,
    Save,       // guardar en la biblioteca
    TabPalette, // pestaña Palette
    TabGen,     // pestaña Generator
    TabPro,     // pestaña Pro
    ZoneTop,    // el bloque con su capa de arriba encendida
    ZonePenu,   // … la de debajo
    ZoneBottom, // … la de abajo del todo
    KSolid,     // tipo de pase: Solid
    KStitch,    // tipo de pase: ColorStitch
    KPbHalf,    // tipo de pase: PathBlend Half
    KPbFull,    // tipo de pase: PathBlend Full
    Up,
    Down,
    Angle,      // ángulo de relleno
    Spot,       // resaltar el color activo
    NoAngle,    // marcar bandas sin ángulo
    Eye,        // pintura visible fuera del gizmo
    Load,       // cargar fichero
    Adv,        // editor avanzado (deslizadores)
    Ease,       // curva de la rampa
    Mix,        // MixedFilament
    Group,      // grupo de paleta
    Info,
    ChevR,
    ChevD,
};

inline void draw_glyph(ImDrawList *dl, const ImVec2 &p0, float s, Glyph g, ImU32 col)
{
    auto P = [&](float u, float v) { return ImVec2(p0.x + u * s, p0.y + v * s); };
    const float th = std::max(1.2f, s * 0.075f);

    switch (g) {
    case Glyph::Overhang: {
        // Un techo con su alero, y debajo la ristra de rayas que es lo que el sombreado pinta.
        const ImVec2 roof[4] = { P(0.12f, 0.18f), P(0.88f, 0.18f), P(0.88f, 0.34f), P(0.12f, 0.34f) };
        dl->AddConvexPolyFilled(roof, 4, col);
        const ImVec2 lean[3] = { P(0.12f, 0.34f), P(0.62f, 0.34f), P(0.12f, 0.72f) };
        dl->AddConvexPolyFilled(lean, 3, (col & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT));
        for (int i = 0; i < 3; ++ i) {
            const float u = 0.24f + 0.16f * float(i);
            dl->AddLine(P(u, 0.52f + 0.10f * float(i)), P(u, 0.88f), col, th * 0.7f);
        }
        break;
    }
    case Glyph::GapMap: {
        // La rejilla del mapa: unos puntos cogidos y otros no. Sin color propio — el estado del
        // botón lo pone, porque el rojo y el verde son del 3D, no del icono.
        for (int r = 0; r < 3; ++ r)
            for (int c = 0; c < 3; ++ c) {
                const ImVec2 q = P(0.22f + 0.28f * float(c), 0.22f + 0.28f * float(r));
                if ((r + c) % 2 == 0)
                    dl->AddCircleFilled(q, s * 0.085f, col, 10);
                else
                    dl->AddCircle(q, s * 0.085f, (col & ~IM_COL32_A_MASK) | (140u << IM_COL32_A_SHIFT), 10, th * 0.6f);
            }
        break;
    }
    case Glyph::Xray: {
        // Una caja con la arista de detrás vista a través: es literalmente lo que hace el modo.
        const ImVec2 box[4] = { P(0.16f, 0.24f), P(0.84f, 0.24f), P(0.84f, 0.80f), P(0.16f, 0.80f) };
        dl->AddPolyline(box, 4, col, ImDrawFlags_Closed, th);
        dl->AddLine(P(0.16f, 0.24f), P(0.84f, 0.80f), (col & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT), th * 0.8f);
        dl->AddCircleFilled(P(0.50f, 0.52f), s * 0.13f, (col & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT), 14);
        break;
    }
    case Glyph::Patch: {
        // Un parche con muescas: la forma que sale del flood-fill, que no es ni redonda ni cuadrada.
        const ImVec2 blob[7] = { P(0.14f, 0.34f), P(0.42f, 0.20f), P(0.74f, 0.26f), P(0.88f, 0.50f),
                                 P(0.68f, 0.78f), P(0.34f, 0.76f), P(0.10f, 0.58f) };
        dl->AddConvexPolyFilled(blob, 7, (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT));
        dl->AddPolyline(blob, 7, col, ImDrawFlags_Closed, th);
        break;
    }
    case Glyph::Round:
        dl->AddCircleFilled(P(0.5f, 0.5f), s * 0.30f, (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT), 24);
        dl->AddCircle(P(0.5f, 0.5f), s * 0.30f, col, 24, th);
        break;
    case Glyph::Square:
        dl->AddRectFilled(P(0.22f, 0.22f), P(0.78f, 0.78f), (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT), s * 0.06f);
        dl->AddRect      (P(0.22f, 0.22f), P(0.78f, 0.78f), col, s * 0.06f, 0, th);
        break;
    case Glyph::Brush: {
        // Un rastro de marcas solapadas y el pincel encima: es exactamente lo que hace el modo.
        dl->AddCircleFilled(P(0.30f, 0.66f), s * 0.20f, (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT), 20);
        dl->AddCircleFilled(P(0.52f, 0.58f), s * 0.20f, (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT), 20);
        dl->AddCircle      (P(0.30f, 0.66f), s * 0.20f, col, 20, th * 0.8f);
        dl->AddCircle      (P(0.52f, 0.58f), s * 0.20f, col, 20, th * 0.8f);
        dl->AddLine(P(0.58f, 0.50f), P(0.86f, 0.18f), col, th);
        break;
    }
    case Glyph::Trash:
        dl->AddLine(P(0.18f, 0.28f), P(0.82f, 0.28f), col, th);
        dl->AddLine(P(0.40f, 0.20f), P(0.60f, 0.20f), col, th);
        {
            const ImVec2 body[4] = { P(0.26f, 0.32f), P(0.74f, 0.32f), P(0.66f, 0.84f), P(0.34f, 0.84f) };
            dl->AddPolyline(body, 4, col, ImDrawFlags_Closed, th);
        }
        break;
    case Glyph::Copy:
        dl->AddRect(P(0.16f, 0.16f), P(0.66f, 0.66f), (col & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT), s * 0.06f, 0, th);
        dl->AddRectFilled(P(0.34f, 0.34f), P(0.84f, 0.84f), (col & ~IM_COL32_A_MASK) | (60u << IM_COL32_A_SHIFT), s * 0.06f);
        dl->AddRect      (P(0.34f, 0.34f), P(0.84f, 0.84f), col, s * 0.06f, 0, th);
        break;
    case Glyph::Cube: {
        // El cubo isométrico de Align & Stack, en pequeño: "esto lo hereda del objeto".
        const ImVec2 top[4]   = { P(0.50f, 0.16f), P(0.86f, 0.36f), P(0.50f, 0.56f), P(0.14f, 0.36f) };
        const ImVec2 left[4]  = { P(0.14f, 0.36f), P(0.50f, 0.56f), P(0.50f, 0.88f), P(0.14f, 0.68f) };
        const ImVec2 right[4] = { P(0.50f, 0.56f), P(0.86f, 0.36f), P(0.86f, 0.68f), P(0.50f, 0.88f) };
        dl->AddConvexPolyFilled(top,   4, (col & ~IM_COL32_A_MASK) | (200u << IM_COL32_A_SHIFT));
        dl->AddConvexPolyFilled(left,  4, (col & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT));
        dl->AddConvexPolyFilled(right, 4, (col & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT));
        break;
    }
    case Glyph::Warn: {
        const ImVec2 tri[3] = { P(0.50f, 0.14f), P(0.94f, 0.84f), P(0.06f, 0.84f) };
        dl->AddConvexPolyFilled(tri, 3, (col & ~IM_COL32_A_MASK) | (55u << IM_COL32_A_SHIFT));
        dl->AddPolyline(tri, 3, col, ImDrawFlags_Closed, th);
        dl->AddLine(P(0.50f, 0.38f), P(0.50f, 0.60f), col, th * 1.1f);
        dl->AddCircleFilled(P(0.50f, 0.72f), th * 0.75f, col, 8);
        break;
    }
    case Glyph::Target: {
        // La superficie de arriba y la flecha que la empuja hacia abajo: lo que hay que sujetar.
        dl->AddRectFilled(P(0.12f, 0.16f), P(0.88f, 0.30f), col, s * 0.05f);
        for (int i = 0; i < 2; ++ i) {
            const float u = 0.34f + 0.32f * float(i);
            dl->AddLine(P(u, 0.40f), P(u, 0.74f), col, th * 0.9f);
            const ImVec2 head[3] = { P(u, 0.86f), P(u - 0.10f, 0.68f), P(u + 0.10f, 0.68f) };
            dl->AddConvexPolyFilled(head, 3, col);
        }
        break;
    }
    case Glyph::Landing: {
        // La cama y el punto donde se apoya, con su marca de sitio.
        dl->AddLine(P(0.08f, 0.80f), P(0.92f, 0.80f), col, th * 1.3f);
        dl->AddCircleFilled(P(0.50f, 0.80f), s * 0.10f, col, 14);
        dl->AddLine(P(0.50f, 0.18f), P(0.50f, 0.68f), (col & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT), th * 0.8f);
        for (int i = 0; i < 3; ++ i)
            dl->AddLine(P(0.14f + 0.28f * float(i), 0.86f), P(0.06f + 0.28f * float(i), 0.94f),
                        (col & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT), th * 0.6f);
        break;
    }
    case Glyph::Lock:
    case Glyph::Unlock: {
        // 🚨 s288, dicho por él: el candado del ESTADO se leía como abierto. La culpa era del arco,
        // que estaba hecho con cuatro rectas y a 13 px no cerraba. Ahora es un arco de verdad
        // (`PathArcTo`), y cerrado quiere decir cerrado: las dos patas bajan RECTAS hasta el cuerpo.
        // La versión abierta levanta y gira una de las patas, que es como se dibuja un candado
        // abierto en todas partes.
        dl->AddRectFilled(P(0.22f, 0.48f), P(0.78f, 0.88f), col, s * 0.10f);
        const bool  open = (g == Glyph::Unlock);
        const ImVec2 c   = P(open ? 0.62f : 0.50f, 0.42f);
        const float  r   = s * 0.17f;
        dl->PathClear();
        // 🚨 Literal y no `IM_PI`: esa macro vive en `imgui_internal.h`, que este fichero no incluye
        // ni debe incluir. Media vuelta, de PI a 2·PI, o sea el arco de arriba.
        dl->PathArcTo(c, r, 3.14159265f, 6.28318531f, 14);
        dl->PathStroke(col, 0, th);
        // La pata que baja al cuerpo. Abierto: sólo una, y más corta.
        dl->AddLine(ImVec2(c.x + r, c.y), ImVec2(c.x + r, P(0.f, open ? 0.60f : 0.50f).y), col, th);
        if (! open)
            dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x - r, P(0.f, 0.50f).y), col, th);
        break;
    }
    case Glyph::Edit: {
        // Un lápiz en diagonal: el cuerpo, la punta y la línea de lo escrito.
        const ImVec2 body[4] = { P(0.28f, 0.70f), P(0.68f, 0.18f), P(0.82f, 0.30f), P(0.42f, 0.82f) };
        dl->AddConvexPolyFilled(body, 4, (col & ~IM_COL32_A_MASK) | (70u << IM_COL32_A_SHIFT));
        dl->AddPolyline(body, 4, col, ImDrawFlags_Closed, th);
        const ImVec2 tip[3] = { P(0.42f, 0.82f), P(0.28f, 0.70f), P(0.24f, 0.88f) };
        dl->AddConvexPolyFilled(tip, 3, col);
        break;
    }
    case Glyph::Pillar: {
        // El pilar con rodilla, que es exactamente lo que fabrica el botón.
        const ImVec2 col_pts[6] = { P(0.20f, 0.14f), P(0.44f, 0.14f), P(0.78f, 0.56f),
                                    P(0.78f, 0.86f), P(0.60f, 0.86f), P(0.60f, 0.60f) };
        dl->AddConvexPolyFilled(col_pts, 6, (col & ~IM_COL32_A_MASK) | (90u << IM_COL32_A_SHIFT));
        dl->AddPolyline(col_pts, 6, col, ImDrawFlags_Closed, th);
        dl->AddLine(P(0.06f, 0.92f), P(0.94f, 0.92f), col, th);
        break;
    }
    // ---- s318, ColorStitch Painter. Mismo contrato: caja [0,1], el color lo pone el estado. ----
    // 🚨 Polígonos rellenos en sentido HORARIO en pantalla (imgui.h, AddConvexPolyFilled): si no,
    // el antialias deja un filo claro.
    case Glyph::Select: {
        const ImVec2 arrow[7] = { P(0.24f, 0.14f), P(0.74f, 0.59f), P(0.51f, 0.59f), P(0.63f, 0.84f),
                                  P(0.53f, 0.88f), P(0.41f, 0.63f), P(0.24f, 0.78f) };
        dl->AddPolyline(arrow, 7, col, ImDrawFlags_Closed, th);
        break;
    }
    case Glyph::Paint: {
        // El cubo volcado y su gota: smart fill es "rellenar", no "pincel".
        const ImVec2 bucket[4] = { P(0.46f, 0.16f), P(0.80f, 0.50f), P(0.50f, 0.80f), P(0.16f, 0.46f) };
        const ImVec2 paint[3]  = { P(0.16f, 0.46f), P(0.80f, 0.50f), P(0.50f, 0.80f) };
        dl->AddConvexPolyFilled(paint, 3, (col & ~IM_COL32_A_MASK) | (140u << IM_COL32_A_SHIFT));
        dl->AddPolyline(bucket, 4, col, ImDrawFlags_Closed, th);
        dl->AddCircleFilled(P(0.85f, 0.80f), s * 0.085f, col, 12);
        break;
    }
    case Glyph::Erase: {
        const ImVec2 body[5] = { P(0.16f, 0.60f), P(0.50f, 0.26f), P(0.80f, 0.56f), P(0.52f, 0.84f), P(0.34f, 0.84f) };
        const ImVec2 tip[5]  = { P(0.16f, 0.60f), P(0.33f, 0.43f), P(0.63f, 0.73f), P(0.52f, 0.84f), P(0.34f, 0.84f) };
        dl->AddConvexPolyFilled(tip, 5, (col & ~IM_COL32_A_MASK) | (115u << IM_COL32_A_SHIFT));
        dl->AddPolyline(body, 5, col, ImDrawFlags_Closed, th);
        dl->AddLine(P(0.58f, 0.90f), P(0.88f, 0.90f), col, th);
        break;
    }
    case Glyph::Pick:
        dl->AddLine(P(0.18f, 0.84f), P(0.58f, 0.44f), col, th * 1.3f);
        dl->AddLine(P(0.44f, 0.36f), P(0.66f, 0.58f), col, th);
        dl->AddCircleFilled(P(0.70f, 0.30f), s * 0.15f, col, 16);
        break;
    case Glyph::Sticker: {
        // Un cuadrado con la esquina levantada: una pegatina a medio despegar.
        const ImVec2 sheet[5] = { P(0.16f, 0.16f), P(0.84f, 0.16f), P(0.84f, 0.56f), P(0.56f, 0.84f), P(0.16f, 0.84f) };
        const ImVec2 fold[3]  = { P(0.84f, 0.56f), P(0.58f, 0.82f), P(0.58f, 0.58f) };
        dl->AddConvexPolyFilled(fold, 3, (col & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT));
        dl->AddPolyline(sheet, 5, col, ImDrawFlags_Closed, th);
        break;
    }
    case Glyph::EraseAll: {
        // La gota tachada (mismo significado que el SVG de Fable de s173 al que sustituye).
        const ImVec2 c = P(0.50f, 0.62f);
        dl->PathClear();
        dl->PathArcTo(c, s * 0.24f, -0.30f, 3.14159265f + 0.30f, 16);
        dl->PathLineTo(P(0.50f, 0.12f));
        dl->PathStroke(col, ImDrawFlags_Closed, th);
        dl->AddLine(P(0.14f, 0.16f), P(0.86f, 0.88f), col, th);
        break;
    }
    case Glyph::Plus:
        dl->AddLine(P(0.50f, 0.20f), P(0.50f, 0.80f), col, th * 1.1f);
        dl->AddLine(P(0.20f, 0.50f), P(0.80f, 0.50f), col, th * 1.1f);
        break;
    case Glyph::Save: {
        // El marcapáginas: "me lo quedo".
        const ImVec2 mark[5] = { P(0.28f, 0.14f), P(0.72f, 0.14f), P(0.72f, 0.86f), P(0.50f, 0.68f), P(0.28f, 0.86f) };
        dl->AddRectFilled(P(0.28f, 0.14f), P(0.72f, 0.66f), (col & ~IM_COL32_A_MASK) | (90u << IM_COL32_A_SHIFT));
        dl->AddPolyline(mark, 5, col, ImDrawFlags_Closed, th);
        break;
    }
    case Glyph::TabPalette:
        dl->AddRectFilled(P(0.14f, 0.14f), P(0.46f, 0.46f), col, s * 0.05f);
        dl->AddRectFilled(P(0.54f, 0.14f), P(0.86f, 0.46f), (col & ~IM_COL32_A_MASK) | (160u << IM_COL32_A_SHIFT), s * 0.05f);
        dl->AddRectFilled(P(0.14f, 0.54f), P(0.46f, 0.86f), (col & ~IM_COL32_A_MASK) | (105u << IM_COL32_A_SHIFT), s * 0.05f);
        dl->AddRect      (P(0.54f, 0.54f), P(0.86f, 0.86f), col, s * 0.05f, 0, th);
        break;
    case Glyph::TabGen:
        // Cuatro barras que suben de opacidad: una rampa generada.
        for (int i = 0; i < 4; ++ i)
            dl->AddRectFilled(P(0.12f + 0.18f * float(i), 0.30f), P(0.26f + 0.18f * float(i), 0.70f),
                              (col & ~IM_COL32_A_MASK) | ((50u + 55u * unsigned(i)) << IM_COL32_A_SHIFT));
        dl->AddLine(P(0.12f, 0.84f), P(0.86f, 0.84f), col, th);
        dl->AddLine(P(0.74f, 0.77f), P(0.88f, 0.84f), col, th);
        dl->AddLine(P(0.74f, 0.91f), P(0.88f, 0.84f), col, th);
        break;
    case Glyph::TabPro: {
        // Tres capas apiladas: componer la pila.
        const ImVec2 top[4] = { P(0.50f, 0.14f), P(0.86f, 0.32f), P(0.50f, 0.50f), P(0.14f, 0.32f) };
        dl->AddConvexPolyFilled(top, 4, (col & ~IM_COL32_A_MASK) | (115u << IM_COL32_A_SHIFT));
        dl->AddPolyline(top, 4, col, ImDrawFlags_Closed, th);
        const ImVec2 l1[3] = { P(0.14f, 0.50f), P(0.50f, 0.68f), P(0.86f, 0.50f) };
        const ImVec2 l2[3] = { P(0.14f, 0.68f), P(0.50f, 0.86f), P(0.86f, 0.68f) };
        dl->AddPolyline(l1, 3, col, 0, th);
        dl->AddPolyline(l2, 3, col, 0, th);
        break;
    }
    case Glyph::ZoneTop:
    case Glyph::ZonePenu:
    case Glyph::ZoneBottom: {
        // 🔑 La identidad de la zona es su POSICIÓN en la pieza, no un color: un bloque de cuatro
        // capas con la suya encendida. Así Bottom deja de ser "la naranja" (el ámbar es de avisos).
        const float y0[4] = { 0.16f, 0.33f, 0.50f, 0.67f };
        const int   lit   = (g == Glyph::ZoneTop) ? 0 : (g == Glyph::ZonePenu) ? 1 : 3;
        if (g == Glyph::ZonePenu)
            dl->AddRectFilled(P(0.14f, y0[0]), P(0.86f, y0[1]), (col & ~IM_COL32_A_MASK) | (90u << IM_COL32_A_SHIFT));
        dl->AddRectFilled(P(0.14f, y0[lit]), P(0.86f, y0[lit] + 0.17f), col);
        for (int i = 1; i < 4; ++ i)
            if (i != lit && i != lit + 1)
                dl->AddLine(P(0.14f, y0[i]), P(0.86f, y0[i]), (col & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT), th * 0.7f);
        dl->AddRect(P(0.14f, 0.16f), P(0.86f, 0.84f), col, s * 0.05f, 0, th);
        break;
    }
    case Glyph::KSolid:
        dl->AddRectFilled(P(0.18f, 0.18f), P(0.82f, 0.82f), col, s * 0.06f);
        break;
    case Glyph::KStitch:
        dl->AddRect(P(0.18f, 0.18f), P(0.82f, 0.82f), col, s * 0.06f, 0, th);
        dl->AddLine(P(0.18f, 0.48f), P(0.48f, 0.18f), col, th);
        dl->AddLine(P(0.18f, 0.80f), P(0.80f, 0.18f), col, th);
        dl->AddLine(P(0.48f, 0.82f), P(0.82f, 0.48f), col, th);
        break;
    case Glyph::KPbHalf:
    case Glyph::KPbFull: {
        // La rampa de PathBlend: Half se queda a media altura, Full llega arriba.
        const float top = (g == Glyph::KPbFull) ? 0.22f : 0.50f;
        const ImVec2 ramp[3] = { P(0.16f, 0.78f), P(0.84f, top), P(0.84f, 0.78f) };
        dl->AddConvexPolyFilled(ramp, 3, col);
        dl->AddRect(P(0.16f, 0.22f), P(0.84f, 0.78f), col, s * 0.05f, 0, th);
        break;
    }
    case Glyph::Up: {
        const ImVec2 v[3] = { P(0.26f, 0.62f), P(0.50f, 0.36f), P(0.74f, 0.62f) };
        dl->AddPolyline(v, 3, col, 0, th * 1.1f);
        break;
    }
    case Glyph::Down: {
        const ImVec2 v[3] = { P(0.26f, 0.38f), P(0.50f, 0.64f), P(0.74f, 0.38f) };
        dl->AddPolyline(v, 3, col, 0, th * 1.1f);
        break;
    }
    case Glyph::ChevR: {
        const ImVec2 v[3] = { P(0.38f, 0.24f), P(0.64f, 0.50f), P(0.38f, 0.76f) };
        dl->AddPolyline(v, 3, col, 0, th * 1.1f);
        break;
    }
    case Glyph::ChevD: {
        const ImVec2 v[3] = { P(0.24f, 0.38f), P(0.50f, 0.64f), P(0.76f, 0.38f) };
        dl->AddPolyline(v, 3, col, 0, th * 1.1f);
        break;
    }
    case Glyph::Angle:
        dl->AddLine(P(0.14f, 0.80f), P(0.86f, 0.80f), col, th);
        dl->AddLine(P(0.14f, 0.80f), P(0.70f, 0.28f), col, th);
        dl->PathClear();
        dl->PathArcTo(P(0.14f, 0.80f), s * 0.38f, -0.75f, 0.f, 10);
        dl->PathStroke((col & ~IM_COL32_A_MASK) | (170u << IM_COL32_A_SHIFT), 0, th * 0.8f);
        break;
    case Glyph::Spot:
        dl->AddCircleFilled(P(0.50f, 0.50f), s * 0.16f, col, 16);
        dl->AddCircle(P(0.50f, 0.50f), s * 0.32f, (col & ~IM_COL32_A_MASK) | (140u << IM_COL32_A_SHIFT), 24, th);
        break;
    case Glyph::NoAngle:
        // Círculo a trazos: la dirección "no está", y una aguja suelta.
        for (int i = 0; i < 8; ++ i) {
            const float a0 = 6.28318531f * float(i) / 8.f;
            dl->PathClear();
            dl->PathArcTo(P(0.50f, 0.50f), s * 0.32f, a0, a0 + 0.45f, 4);
            dl->PathStroke(col, 0, th);
        }
        dl->AddLine(P(0.50f, 0.50f), P(0.72f, 0.30f), col, th);
        break;
    case Glyph::Eye:
        dl->PathClear();
        dl->PathLineTo(P(0.10f, 0.50f));
        dl->PathBezierCubicCurveTo(P(0.30f, 0.18f), P(0.70f, 0.18f), P(0.90f, 0.50f), 12);
        dl->PathBezierCubicCurveTo(P(0.70f, 0.82f), P(0.30f, 0.82f), P(0.10f, 0.50f), 12);
        dl->PathStroke(col, ImDrawFlags_Closed, th);
        dl->AddCircleFilled(P(0.50f, 0.50f), s * 0.12f, col, 12);
        break;
    case Glyph::Load: {
        dl->AddLine(P(0.50f, 0.64f), P(0.50f, 0.18f), col, th);
        const ImVec2 head[3] = { P(0.32f, 0.34f), P(0.50f, 0.16f), P(0.68f, 0.34f) };
        dl->AddPolyline(head, 3, col, 0, th);
        const ImVec2 tray[4] = { P(0.16f, 0.60f), P(0.16f, 0.84f), P(0.84f, 0.84f), P(0.84f, 0.60f) };
        dl->AddPolyline(tray, 4, col, 0, th);
        break;
    }
    case Glyph::Adv:
        for (int i = 0; i < 3; ++ i) {
            const float v = 0.30f + 0.20f * float(i);
            const float k = (i == 0) ? 0.34f : (i == 1) ? 0.66f : 0.46f;
            dl->AddLine(P(0.16f, v), P(0.84f, v), (col & ~IM_COL32_A_MASK) | (150u << IM_COL32_A_SHIFT), th * 0.8f);
            dl->AddCircleFilled(P(k, v), s * 0.08f, col, 10);
        }
        break;
    case Glyph::Ease:
        dl->AddLine(P(0.14f, 0.82f), P(0.86f, 0.82f), (col & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT), th * 0.8f);
        dl->PathClear();
        dl->PathLineTo(P(0.14f, 0.82f));
        dl->PathBezierCubicCurveTo(P(0.50f, 0.82f), P(0.50f, 0.18f), P(0.86f, 0.18f), 14);
        dl->PathStroke(col, 0, th);
        break;
    case Glyph::Mix:
        dl->AddCircleFilled(P(0.38f, 0.42f), s * 0.24f, (col & ~IM_COL32_A_MASK) | (140u << IM_COL32_A_SHIFT), 18);
        dl->AddCircleFilled(P(0.62f, 0.42f), s * 0.24f, (col & ~IM_COL32_A_MASK) | (140u << IM_COL32_A_SHIFT), 18);
        dl->AddCircleFilled(P(0.50f, 0.62f), s * 0.24f, (col & ~IM_COL32_A_MASK) | (140u << IM_COL32_A_SHIFT), 18);
        break;
    case Glyph::Group: {
        const ImVec2 tab[4] = { P(0.14f, 0.26f), P(0.14f, 0.18f), P(0.42f, 0.18f), P(0.48f, 0.26f) };
        dl->AddPolyline(tab, 4, col, 0, th);
        dl->AddRect(P(0.14f, 0.26f), P(0.86f, 0.80f), col, s * 0.06f, 0, th);
        break;
    }
    case Glyph::Info:
        dl->AddCircle(P(0.50f, 0.50f), s * 0.36f, col, 24, th);
        dl->AddLine(P(0.50f, 0.46f), P(0.50f, 0.70f), col, th);
        dl->AddCircleFilled(P(0.50f, 0.32f), th * 0.8f, col, 8);
        break;
    }
}

// -----------------------------------------------------------------------------
// Primitivas de dibujo que el alzado y las tarjetas comparten
// -----------------------------------------------------------------------------

inline void dashed_line(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, ImU32 col, float th,
                 float dash = 5.f, float gap = 4.f)
{
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f)
        return;
    const float ux = dx / len, uy = dy / len;
    for (float t = 0.f; t < len; t += dash + gap) {
        const float t1 = std::min(t + dash, len);
        dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t), ImVec2(a.x + ux * t1, a.y + uy * t1), col, th);
    }
}

// El rayado de "esto es la PIEZA". Diagonal, fino, y recortado al rectángulo que se le pide, que es
// lo que le permite seguir un contorno sin calcular intersecciones.
inline void hatch_rect(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, ImU32 col, float spacing = 7.f)
{
    dl->PushClipRect(a, b, true);
    const float w = b.x - a.x, h = b.y - a.y;
    for (float t = -h; t < w; t += spacing)
        dl->AddLine(ImVec2(a.x + t, b.y), ImVec2(a.x + t + h, a.y), col, 1.0f);
    dl->PopClipRect();
}

// Una cota, con sus dos topes y su número flotando encima. Horizontal.
inline void dim_line_h(ImDrawList *dl, float x0, float x1, float y, ImU32 col, const char *label)
{
    if (x1 < x0) std::swap(x0, x1);
    dl->AddLine(ImVec2(x0, y - 4.f), ImVec2(x0, y + 4.f), col, 1.f);
    dl->AddLine(ImVec2(x1, y - 4.f), ImVec2(x1, y + 4.f), col, 1.f);
    dl->AddLine(ImVec2(x0, y), ImVec2(x1, y), col, 1.f);
    if (label != nullptr && label[0] != '\0') {
        const ImVec2 ts = ImGui::CalcTextSize(label);
        neo_draw_pill(dl, ImVec2((x0 + x1) * 0.5f - ts.x * 0.5f, y - ts.y - 6.f), label, col);
    }
}

// El texto que no cabe se corta con puntos suspensivos en vez de estirar la ventana o desbordar la
// tarjeta. 🚨 Se recorta por BYTES sobre UTF-8, así que hay que retroceder hasta un principio de
// carácter o se escribe medio glifo y la fuente pinta basura.
inline std::string fit_text(const std::string &s, float max_w)
{
    if (ImGui::CalcTextSize(s.c_str()).x <= max_w)
        return s;
    std::string out = s;
    while (! out.empty()) {
        out.pop_back();
        while (! out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
            out.pop_back();
        if (ImGui::CalcTextSize((out + "…").c_str()).x <= max_w)
            break;
    }
    return out + "…";
}

// -----------------------------------------------------------------------------
// Los widgets del panel
// -----------------------------------------------------------------------------

// Un título de sección: la etiqueta y una regla que llega hasta el borde. Ocupa una línea y separa
// mejor que un Separator con un Text encima, que son dos.
inline void neo_section(const char *label)
{
    ImGui::Spacing();
    ImDrawList  *dl    = ImGui::GetWindowDrawList();
    const ImVec2 p     = ImGui::GetCursorScreenPos();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts    = ImGui::CalcTextSize(label);
    dl->AddText(p, neo_fade(NeoCol::Ink, 0.90f), label);
    const float x0 = p.x + ts.x + 8.f, x1 = p.x + avail;
    if (x1 > x0)
        dl->AddLine(ImVec2(x0, p.y + ts.y * 0.55f), ImVec2(x1, p.y + ts.y * 0.55f),
                    neo_fade(NeoCol::SurfaceHi, 0.85f), 1.f);
    ImGui::Dummy(ImVec2(avail, ts.y + 3.f));
}

// Un interruptor con icono. Encendido = fondo de acento y el glifo en tinta; apagado = superficie y
// glifo apagado. Sin etiqueta: lo que hace lo cuenta el tooltip, que es donde cabe entero.
inline bool neo_glyph_toggle(const char *id, float size, bool active, Glyph g, const char *tip)
{
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool   hv = ImGui::IsItemHovered();
    dl->AddRectFilled(a, b, active ? neo_fade(NeoCol::Accent, hv ? 1.0f : 0.85f)
                                   : neo_col_u32(hv ? NeoCol::SurfaceHi : NeoCol::Surface), 5.f);
    if (active)
        dl->AddRect(a, b, neo_col_u32(NeoCol::AccentBright), 5.f, 0, 1.2f);
    draw_glyph(dl, ImVec2(a.x + size * 0.16f, a.y + size * 0.16f), size * 0.68f, g,
               active ? neo_col_u32(NeoCol::Ink) : neo_fade(NeoCol::TextDim, hv ? 1.0f : 0.85f));
    if (hv && tip != nullptr)
        ImGui::SetTooltip("%s", tip);
    return pressed;
}

// Un botón de icono pequeño, para las acciones de una tarjeta. `danger` lo pinta de ámbar al pasar
// por encima, que es lo único que distingue borrar de duplicar antes de pulsar.
//
// 🔑 s288 — `tint` rompe esa regla a propósito para UN caso: editar. Editar y el candado son la
// misma idea vista dos veces —se puede editar PORQUE está bloqueada— así que van del mismo color y
// el botón lleva su fondo puesto siempre, no sólo al pasar por encima. Con el gris de las demás
// acciones había que saberse la relación; del mismo verde, se ve.
inline bool neo_glyph_button(const char *id, float size, Glyph g, bool danger, const char *tip,
                      NeoCol tint = NeoCol::TextDim, bool always_lit = false)
{
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool   hv = ImGui::IsItemHovered();
    const bool   own_tint = (tint != NeoCol::TextDim);
    if (hv || always_lit)
        dl->AddRectFilled(a, b, danger      ? neo_fade(NeoCol::Warn, hv ? 0.22f : 0.10f)
                              : own_tint    ? neo_fade(tint, hv ? 0.30f : 0.14f)
                                            : neo_fade(NeoCol::Accent, 0.28f), 4.f);
    draw_glyph(dl, ImVec2(a.x + size * 0.18f, a.y + size * 0.18f), size * 0.64f, g,
               own_tint ? neo_fade(tint, hv ? 1.0f : 0.85f)
                        : (hv ? (danger ? neo_col_u32(NeoCol::Warn) : neo_col_u32(NeoCol::AccentBright))
                              : neo_fade(NeoCol::TextDim, 0.9f)));
    if (hv && tip != nullptr)
        ImGui::SetTooltip("%s", tip);
    return pressed;
}

// Una píldora de estado en la fila del cabecero. No es un control: no se pulsa, sólo cuenta algo.
inline void neo_status_chip(const char *text, ImU32 col, bool solid)
{
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float  w  = ts.x + 0.95f * neo_u(), h = ts.y + 0.36f * neo_u();
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), solid ? col : ((col & ~IM_COL32_A_MASK) | (36u << IM_COL32_A_SHIFT)), h * 0.5f);
    if (! solid)
        dl->AddRect(p, ImVec2(p.x + w, p.y + h), (col & ~IM_COL32_A_MASK) | (170u << IM_COL32_A_SHIFT), h * 0.5f, 0, 1.f);
    dl->AddText(ImVec2(p.x + 0.47f * neo_u(), p.y + 0.18f * neo_u()), solid ? ink_on(col) : col, text);
    ImGui::Dummy(ImVec2(w, h));
}

// Un deslizador con su etiqueta en una columna fija a la izquierda. La columna es lo que hace que
// seis deslizadores distintos se lean como una tabla y no como seis frases.
inline bool neo_row_slider(const char *id, const char *caption, float *v, float mn, float mx,
                    const char *fmt, const char *tip, float label_col = -1.f)
{
    const float col = (label_col > 0.f) ? label_col : neo_label_col_current();
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    // 🚨 Recortada al ancho de la columna: sin esto una etiqueta larga se mete DEBAJO del
    // deslizador, que es exactamente lo que pasaba con "overhangs" y "map detail".
    ImGui::TextUnformatted(fit_text(caption, col - 0.4f * neo_u()).c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip != nullptr ? tip : caption);
    ImGui::SameLine(col);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool changed = ImGui::SliderFloat("##v", v, mn, mx, fmt);
    if (tip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return changed;
}

// s299c — la misma fila, con un interruptor en vez de un deslizador. Misma columna de etiqueta que
// todo lo demás del panel, que es lo que mantiene la rejilla del rediseño.
inline bool neo_row_toggle(const char *id, const char *caption, bool *v, const char *tip, float label_col = -1.f)
{
    // 🚨 s299c — NO se recorta a la columna de etiqueta, y aquí está la diferencia con el
    // deslizador. Un deslizador necesita todo el ancho que queda, así que su etiqueta cede; una
    // casilla ocupa un cuadrado, así que hay sitio de sobra y recortar sólo servía para dejar
    // "only my zo…" en pantalla. Se recorta contra lo que queda de verdad, menos el hueco de la
    // casilla, y la casilla se coloca DESPUÉS del texto cuando éste se pasa de la columna.
    const float col = (label_col > 0.f) ? label_col : neo_label_col_current();
    const float box = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
    const float room = std::max(col - 0.4f * neo_u(),
                                ImGui::GetContentRegionAvail().x - box - 0.4f * neo_u());
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    const std::string label = fit_text(caption, room);
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip != nullptr ? tip : caption);
    // Si la etiqueta cabe en la columna, la casilla se alinea con el resto del panel y la rejilla
    // se mantiene. Si no cabe, va detrás del texto en vez de encima de él.
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

// Una baldosa de dato: rótulo pequeño arriba, número grande abajo, y una barra opcional que dice
// cuánto del presupuesto se ha gastado. Tres de estas cuentan de un vistazo lo que antes eran
// cuatro frases seguidas.
inline void neo_stat_tile(const char *id, float w, const char *caption, const char *value, ImU32 val_col,
                   float bar01, const char *tip)
{
    const float u = neo_u();
    const float h = 2.6f * u;
    const float pad = 0.42f * u;
    ImGui::InvisibleButton(id, ImVec2(w, h));
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    dl->AddRectFilled(a, b, neo_fade(NeoCol::Surface, 0.65f), 5.f);
    dl->AddText(ImGui::GetFont(), 0.64f * u, ImVec2(a.x + pad, a.y + 0.22f * u), neo_fade(NeoCol::TextDim, 0.95f), caption);
    dl->AddText(ImGui::GetFont(), 0.92f * u, ImVec2(a.x + pad, a.y + 0.98f * u), val_col, value);
    if (bar01 >= 0.f) {
        const float y = b.y - 0.24f * u;
        dl->AddLine(ImVec2(a.x + pad, y), ImVec2(b.x - pad, y), neo_fade(NeoCol::SurfaceHi, 0.9f), 2.f);
        const float t = std::min(1.f, std::max(0.f, bar01));
        if (t > 0.f)
            dl->AddLine(ImVec2(a.x + pad, y), ImVec2(a.x + pad + (b.x - a.x - 2.f * pad) * t, y), val_col, 2.f);
    }
    if (tip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
}

// Un aviso: triángulo ámbar, la frase corta, y el porqué entero en el tooltip. 🚨 El ámbar sigue
// significando lo mismo que en GizmoNeotkoStyle — "algo va mal" — y nada más lo usa.
inline void neo_warn_row(const char *id, const char *text, const char *why, bool amber = true)
{
    const ImU32  col   = amber ? neo_col_u32(NeoCol::Warn) : neo_col_u32(NeoCol::TextDim);
    const float  u     = neo_u();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const float  gut   = 1.55f * u;   // el hueco del triángulo
    const float  wrap  = avail - gut - 0.3f * u;
    const ImVec2 ts    = ImGui::CalcTextSize(text, nullptr, false, wrap);
    const float  h     = std::max(1.3f * u, ts.y + 0.5f * u);
    ImGui::InvisibleButton(id, ImVec2(avail, h));
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    dl->AddRectFilled(a, b, (col & ~IM_COL32_A_MASK) | (24u << IM_COL32_A_SHIFT), 4.f);
    dl->AddRectFilled(a, ImVec2(a.x + 2.f, b.y), col, 1.f);
    draw_glyph(dl, ImVec2(a.x + 0.4f * u, a.y + (h - 0.8f * u) * 0.5f), 0.8f * u, Glyph::Warn, col);
    dl->AddText(nullptr, 0.f, ImVec2(a.x + gut, a.y + 0.25f * u), col, text, nullptr, wrap);
    if (why != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", why);
}

// =================================================================================================
// s318 — piezas nuevas del lenguaje, que nacen con el ColorStitch Painter. Misma disciplina que
// las de arriba: se miden en neo_u(), el color sale de la rampa, y nada decide comportamiento.
// =================================================================================================

// Una sección con icono delante y un dato opcional al final de la regla. La hermana de
// neo_section: ésta se usa cuando la sección es una "cosa" con identidad propia (Library, In use…).
inline void neo_section_g(const char *label, Glyph g, const char *aside = nullptr)
{
    ImGui::Spacing();
    ImDrawList  *dl    = ImGui::GetWindowDrawList();
    const float  u     = neo_u();
    const ImVec2 p     = ImGui::GetCursorScreenPos();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts    = ImGui::CalcTextSize(label);
    const float  gs    = 0.95f * u;
    draw_glyph(dl, ImVec2(p.x, p.y + (ts.y - gs) * 0.5f), gs, g, neo_col_u32(NeoCol::AccentBright));
    const float tx = p.x + gs + 0.35f * u;
    dl->AddText(ImVec2(tx, p.y), neo_fade(NeoCol::Ink, 0.92f), label);
    float x1 = p.x + avail;
    if (aside != nullptr && aside[0] != '\0') {
        const ImVec2 as = ImGui::CalcTextSize(aside);
        dl->AddText(ImVec2(x1 - as.x, p.y), neo_col_u32(NeoCol::TextDim), aside);
        x1 -= as.x + 0.4f * u;
    }
    const float x0 = tx + ts.x + 0.5f * u;
    if (x1 > x0)
        dl->AddLine(ImVec2(x0, p.y + ts.y * 0.55f), ImVec2(x1, p.y + ts.y * 0.55f), neo_fade(NeoCol::SurfaceHi, 0.85f), 1.f);
    ImGui::Dummy(ImVec2(avail, ts.y + 0.3f * u));
}

// La misma sección, plegable. Sustituye a ImGui::CollapsingHeader (la barra naranja de Orca): la
// línea entera es el botón y el chevron dice el estado. Devuelve si está abierta.
inline bool neo_section_toggle(const char *id, const char *label, Glyph g, bool *open, const char *tip = nullptr)
{
    ImGui::Spacing();
    ImDrawList  *dl    = ImGui::GetWindowDrawList();
    const float  u     = neo_u();
    const ImVec2 p     = ImGui::GetCursorScreenPos();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 ts    = ImGui::CalcTextSize(label);
    if (ImGui::InvisibleButton(id, ImVec2(avail, ts.y + 0.3f * u)))
        *open = ! *open;
    const bool  hv = ImGui::IsItemHovered();
    const float gs = 0.95f * u;
    draw_glyph(dl, ImVec2(p.x, p.y + (ts.y - gs) * 0.5f), gs, g, neo_col_u32(NeoCol::AccentBright));
    const float tx = p.x + gs + 0.35f * u;
    dl->AddText(ImVec2(tx, p.y), hv ? neo_col_u32(NeoCol::Ink) : neo_fade(NeoCol::Ink, 0.92f), label);
    const float cs = 0.8f * u;
    draw_glyph(dl, ImVec2(p.x + avail - cs, p.y + (ts.y - cs) * 0.5f), cs, *open ? Glyph::ChevD : Glyph::ChevR,
               hv ? neo_col_u32(NeoCol::AccentBright) : neo_col_u32(NeoCol::TextDim));
    const float x0 = tx + ts.x + 0.5f * u, x1 = p.x + avail - cs - 0.4f * u;
    if (x1 > x0)
        dl->AddLine(ImVec2(x0, p.y + ts.y * 0.55f), ImVec2(x1, p.y + ts.y * 0.55f), neo_fade(NeoCol::SurfaceHi, 0.85f), 1.f);
    if (hv && tip != nullptr)
        ImGui::SetTooltip("%s", tip);
    return *open;
}

// Fondo de tarjeta con la esquina de arriba a la derecha BISELADA y, si se pide, un filo de acento a
// la izquierda. Cinco puntos en sentido horario: es convexo, así que va en una sola llamada.
inline void neo_card_bg(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, float bevel, bool accent_edge, ImU32 fill)
{
    const ImVec2 pts[5] = { a, ImVec2(b.x - bevel, a.y), ImVec2(b.x, a.y + bevel), b, ImVec2(a.x, b.y) };
    dl->AddConvexPolyFilled(pts, 5, fill);
    if (accent_edge)
        dl->AddRectFilled(a, ImVec2(a.x + 2.f, b.y), neo_fade(NeoCol::AccentBright, 0.9f));
}

// El contorno a trazos de algo que está VACÍO (una zona sin pases). Mismo idioma que el pilar hueco
// de Zonas: a trazos quiere decir "aquí podría haber algo y no hay nada".
inline void neo_card_dashed(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b)
{
    const ImU32 c = neo_col_u32(NeoCol::SurfaceHi);
    dashed_line(dl, a, ImVec2(b.x, a.y), c, 1.f);
    dashed_line(dl, ImVec2(b.x, a.y), b, c, 1.f);
    dashed_line(dl, b, ImVec2(a.x, b.y), c, 1.f);
    dashed_line(dl, ImVec2(a.x, b.y), a, c, 1.f);
}

// El "brillo" de un resultado, sin blur: tres contornos con alfa decreciente hacia fuera.
inline void neo_halo(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, float rounding = 5.f)
{
    dl->AddRect(a, b, IM_COL32(255, 255, 255, 56), rounding, 0, 1.f);
    dl->AddRect(ImVec2(a.x - 2.5f, a.y - 2.5f), ImVec2(b.x + 2.5f, b.y + 2.5f), neo_fade(NeoCol::AccentBright, 0.16f), rounding + 2.f, 0, 3.f);
    dl->AddRect(ImVec2(a.x - 5.5f, a.y - 5.5f), ImVec2(b.x + 5.5f, b.y + 5.5f), neo_fade(NeoCol::AccentBright, 0.07f), rounding + 5.f, 0, 3.f);
}

// El anillo de "esto es lo elegido" alrededor de un swatch o una ficha: un hueco del color del pozo
// y el teal por fuera, para que se lea igual sobre un filamento claro que sobre uno oscuro.
inline void neo_sel_ring(ImDrawList *dl, const ImVec2 &a, const ImVec2 &b, float rounding = 3.f)
{
    dl->AddRect(ImVec2(a.x - 1.f, a.y - 1.f), ImVec2(b.x + 1.f, b.y + 1.f), neo_col_u32(NeoCol::Canvas), rounding + 1.f, 0, 2.f);
    dl->AddRect(ImVec2(a.x - 2.5f, a.y - 2.5f), ImVec2(b.x + 2.5f, b.y + 2.5f), neo_col_u32(NeoCol::AccentBright), rounding + 2.f, 0, 1.6f);
}

// Una fila de fichas de filamento con el número dentro (R6 de s287: elegir un color se hace
// mirándolo, no abriendo un menú). `cols[i]` es el color real del filamento i; `cur` el elegido
// (-1 = ninguno). Devuelve la ficha pulsada, o -1.
inline int neo_tool_chips(const char *id, const ImU32 *cols, int n, int cur, bool mini = false)
{
    int clicked = -1;
    const float u  = neo_u();
    const float sz = (mini ? 1.05f : 1.45f) * u;
    const float gap = (mini ? 0.18f : 0.3f) * u;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    for (int t = 0; t < n; ++ t) {
        ImGui::PushID(t);
        if (t > 0)
            ImGui::SameLine(0.f, gap);
        if (ImGui::InvisibleButton("##chip", ImVec2(sz, sz)))
            clicked = t;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool   hv = ImGui::IsItemHovered();
        dl->AddRectFilled(a, b, cols[t], mini ? 3.f : 4.f);
        char num[4];
        std::snprintf(num, sizeof(num), "%d", t + 1);
        const float fs = (mini ? 0.62f : 0.78f) * u;
        const ImVec2 ns = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, num);
        dl->AddText(ImGui::GetFont(), fs, ImVec2((a.x + b.x - ns.x) * 0.5f, (a.y + b.y - ns.y) * 0.5f), ink_on(cols[t]), num);
        if (t == cur)
            neo_sel_ring(dl, a, b, mini ? 3.f : 4.f);
        else if (hv)
            dl->AddRect(a, b, neo_col_u32(NeoCol::TextDim), mini ? 3.f : 4.f, 0, 1.2f);
        if (hv)
            ImGui::SetTooltip("T%d", t + 1);
        ImGui::PopID();
    }
    ImGui::PopID();
    return clicked;
}

// Botón con palabra (y glifo opcional delante). Tres caras:
//   Normal = superficie · Ghost = sólo contorno, para acciones secundarias · Accent = contorno teal,
//   para la acción que el panel te está pidiendo (Save all).
// s319 — "sigue en esta línea, pero empezando en esta X de PANTALLA".
// 🚨 NO usar ImGui::SameLine(x) para alinear a la derecha: en ImGui 1.83 esa x se SUMA al
// desplazamiento del BeginGroup que esté abierto, así que dentro de un grupo el item cae más allá
// del borde. En una ventana AlwaysAutoResize eso es un bucle: la ventana se ensancha para que quepa,
// el borde se mueve, el item vuelve a salirse… y el panel crece sin parar hasta tapar el 3D (s319).
// Aquí se coloca en coordenadas de pantalla, que no dependen de grupos ni de sangrías.
inline void neo_same_line_at(float screen_x)
{
    ImGui::SameLine();
    const ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(std::max(c.x, screen_x), c.y));
}

enum class NeoBtn { Normal, Ghost, Accent };

// Dónde acaba lo VISIBLE de una etiqueta ImGui: lo que va tras "##" es el id, no se pinta.
// (ImGui::FindRenderedTextEnd hace esto, pero vive en imgui_internal.h, que aquí no se incluye.)
inline const char *neo_label_end(const char *label)
{
    const char *hash = std::strstr(label, "##");
    return hash != nullptr ? hash : label + std::strlen(label);
}
inline bool neo_text_button(const char *label, NeoBtn style = NeoBtn::Normal, bool has_glyph = false,
                            Glyph g = Glyph::Plus, float width = 0.f)
{
    const float  u   = neo_u();
    const char  *end = neo_label_end(label);
    const ImVec2 ts  = ImGui::CalcTextSize(label, end);
    const float  gs  = has_glyph ? 0.95f * u : 0.f;
    const float  pad = 0.6f * u;
    const float  h   = 1.75f * u;
    const float  w   = (width > 0.f) ? width : ts.x + gs + (has_glyph ? 0.35f * u : 0.f) + 2.f * pad;
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool   hv = ImGui::IsItemHovered();
    ImU32 txt = neo_col_u32(NeoCol::Ink);
    switch (style) {
    case NeoBtn::Normal:
        dl->AddRectFilled(a, b, neo_col_u32(hv ? NeoCol::AccentDim : NeoCol::Surface), 4.f);
        break;
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
        draw_glyph(dl, ImVec2(x, a.y + (h - gs) * 0.5f), gs, g,
                   style == NeoBtn::Normal ? neo_col_u32(NeoCol::AccentBright) : txt);
        x += gs + 0.35f * u;
    }
    dl->AddText(ImVec2(x, a.y + (h - ts.y) * 0.5f), txt, label, end);
    return pressed;
}

// El "añadir" a todo el ancho y a trazos, al final de una lista: se lee como el hueco donde va lo
// siguiente, no como un botón más.
inline bool neo_dashed_button(const char *label, float width = 0.f)
{
    const float  u   = neo_u();
    const char  *end = neo_label_end(label);
    const float  w   = (width > 0.f) ? width : ImGui::GetContentRegionAvail().x;
    const float  h   = 1.6f * u;
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    const ImVec2 a  = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool   hv = ImGui::IsItemHovered();
    const ImU32  c  = neo_col_u32(hv ? NeoCol::AccentBright : NeoCol::SurfaceHi);
    if (hv) dl->AddRectFilled(a, b, neo_col_u32(NeoCol::AccentGhost), 4.f);
    dashed_line(dl, a, ImVec2(b.x, a.y), c, 1.f);
    dashed_line(dl, ImVec2(b.x, a.y), b, c, 1.f);
    dashed_line(dl, b, ImVec2(a.x, b.y), c, 1.f);
    dashed_line(dl, ImVec2(a.x, b.y), a, c, 1.f);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    const float  gs = 0.8f * u;
    const float  x  = a.x + (w - ts.x - gs - 0.3f * u) * 0.5f;
    const ImU32  tc = neo_col_u32(hv ? NeoCol::AccentBright : NeoCol::TextDim);
    draw_glyph(dl, ImVec2(x, a.y + (h - gs) * 0.5f), gs, Glyph::Plus, tc);
    dl->AddText(ImVec2(x + gs + 0.3f * u, a.y + (h - ts.y) * 0.5f), tc, label, end);
    return pressed;
}

// Interruptor de pastilla. Encendido = teal con la bola a la derecha. `disabled` lo pinta al 35% y
// no deja cambiarlo (sin BeginDisabled en esta versión de ImGui). Devuelve true si cambió.
inline bool neo_pill_toggle(const char *id, bool *v, bool disabled = false)
{
    const float u = neo_u();
    const float w = 2.1f * u, h = 1.15f * u;
    const float dy = (ImGui::GetFrameHeight() - h) * 0.5f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    const bool changed = pressed && ! disabled;
    if (changed)
        *v = ! *v;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float al = disabled ? 0.35f : 1.f;
    const ImVec2 a(p.x, p.y + dy), b(p.x + w, p.y + dy + h);
    dl->AddRectFilled(a, b, *v ? neo_fade(NeoCol::Accent, 0.85f * al) : neo_fade(NeoCol::Surface, al), h * 0.5f);
    const float r = h * 0.5f - 2.f;
    const ImVec2 c(*v ? b.x - h * 0.5f : a.x + h * 0.5f, a.y + h * 0.5f);
    dl->AddCircleFilled(c, r, *v ? neo_fade(NeoCol::Ink, al) : neo_fade(NeoCol::TextDim, al), 16);
    return changed;
}

// Una fila de ayuda de vista: glifo, etiqueta y el interruptor al final. Es la forma de las casillas
// "Highlight active colour"… sin la casilla de Orca.
inline bool neo_glyph_toggle_row(const char *id, Glyph g, const char *label, bool *v, const char *tip = nullptr)
{
    const float u = neo_u();
    ImGui::PushID(id);
    const ImVec2 p  = ImGui::GetCursorScreenPos();
    const float  fh = ImGui::GetFrameHeight();
    const float  gs = 0.95f * u;
    ImDrawList  *dl = ImGui::GetWindowDrawList();
    draw_glyph(dl, ImVec2(p.x, p.y + (fh - gs) * 0.5f), gs, g, neo_col_u32(NeoCol::TextDim));
    ImGui::Dummy(ImVec2(gs, fh));
    ImGui::SameLine(0.f, 0.45f * u);
    ImGui::AlignTextToFramePadding();
    const float avail = ImGui::GetContentRegionAvail().x - 2.1f * u - 0.5f * u;
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::TextUnformatted(fit_text(label, avail).c_str());
    ImGui::PopStyleColor();
    if (tip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    neo_same_line_at(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - 2.1f * u);
    const bool changed = neo_pill_toggle("##pill", v);
    if (tip != nullptr && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return changed;
}

}} // namespace Slic3r::GUI

#endif
// NEOTKO_GIZMOSTYLE_TAG_END
