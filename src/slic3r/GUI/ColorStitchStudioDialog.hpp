// NEOTKO_COLORSCI_TAG_START — GD2+GD3 (P2+P3 de docs/FUTURE/COLORSTITCH_STUDIO_PLAN.md)
// ColorStitch Studio — diálogo contenedor (wxNotebook). v1 trae UNA pestaña:
// el Gradient Designer (diseño de rampas A→penu→B + preview TD-aware + export
// de paleta al 3D Painter). Las pestañas Weaves y TD/Materials llegan en P5
// sin reestructurar este fichero.
//
// DRAFT pre-sesión CS-3 — NO está registrado en src/slic3r/CMakeLists.txt ni
// tiene botón de entrada todavía (snippets exactos en el plan, §Anexo).
// Compila contra: libslic3r/ColorSci/{ColorSci,StackFlatten,GradientRamp,
// WeaveLibrary} + SurfaceEffectProfile (manager) — cero dependencias nuevas.
//
// Entrada prevista (CS-3): botón "Gradient Designer…" en SandwichDialog
// (Tab.cpp ~5964, junto a "Save as profile…"):
//   ColorStitchStudioDialog dlg(this, m_fcolors, layer_height_mm());
//   dlg.ShowModal();
#ifndef slic3r_GUI_ColorStitchStudioDialog_hpp_
#define slic3r_GUI_ColorStitchStudioDialog_hpp_

#include <wx/dialog.h>
#include <wx/panel.h>

#include <array>
#include <string>
#include <vector>

#include "../../libslic3r/ColorSci/ColorSci.hpp"
#include "../../libslic3r/ColorSci/GradientRamp.hpp"

class wxChoice;
class wxTextCtrl;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticText;
class wxNotebook;

namespace Slic3r {
namespace GUI {

class RampStripPanel;

class ColorStitchStudioDialog : public wxDialog
{
public:
    // filament_colours: strings "#RRGGBB" de filament_colour (mismo formato
    // que SandwichDialog::m_fcolors). layer_height: LH del preset activo —
    // los ratios del stack son fracción de este valor.
    ColorStitchStudioDialog(wxWindow* parent,
                            const std::vector<std::string>& filament_colours,
                            double layer_height);

private:
    // --- estado ---
    ColorSci::GradientSpec               m_spec;
    std::array<ColorSci::Material, 4>    m_mats;       // colores + TD (app_config)
    std::vector<ColorSci::GradientStep>  m_ramp;
    std::vector<bool>                    m_selected;   // swatches marcados para export

    // --- widgets (solo los que se releen) ---
    wxNotebook*       m_notebook     = nullptr;
    wxChoice*         m_tool_a       = nullptr;
    wxChoice*         m_tool_b       = nullptr;
    wxChoice*         m_weave        = nullptr;
    wxTextCtrl*       m_pattern      = nullptr;
    wxSpinCtrl*       m_steps        = nullptr;
    wxSpinCtrlDouble* m_split_min    = nullptr;
    wxSpinCtrlDouble* m_split_max    = nullptr;
    wxTextCtrl*       m_ramp_name    = nullptr;
    wxStaticText*     m_warn_label   = nullptr;
    RampStripPanel*   m_strip        = nullptr;

    // TD por tool desde app_config: per-channel neotko_td_{N}_{r,g,b} si
    // existen, si no el escalar legacy neotko_td_{N} (r=g=b). Mismo esquema
    // de back-compat que define COLOR_PREDICTION §Fase A.2.
    void load_materials(const std::vector<std::string>& filament_colours);

    wxPanel* build_gradient_page(wxWindow* parent);
    void read_spec_from_ui();        // widgets → m_spec (sin sanitize)
    void recalc();                   // sanitize + build_ramp + refresh strip
    void on_weave_selected();        // preset → campo pattern
    void on_export();                // GD.3 — perfiles al manager

    // Color de un step para swatch/preview_argb: composición física apilada
    // sobre fondo negro (mismo bg que el suggest legacy single-layer).
    wxColour step_colour(const ColorSci::GradientStep& g) const;

    friend class RampStripPanel;
};

// Tira de N swatches clicables (toggle de selección para export). Paint
// bufferizado, layout horizontal con scroll virtual si N*size excede el ancho.
class RampStripPanel : public wxPanel
{
public:
    RampStripPanel(wxWindow* parent, ColorStitchStudioDialog* owner);

private:
    ColorStitchStudioDialog* m_owner;
    int swatch_at(int x, int y) const;   // -1 si fuera
    void on_paint(wxPaintEvent&);
    void on_click(wxMouseEvent&);
    void on_motion(wxMouseEvent&);       // tooltip "A x.xx / B y.yy mm"

    static constexpr int kSwatchW = 36;
    static constexpr int kSwatchH = 48;
    static constexpr int kGap     = 4;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ColorStitchStudioDialog_hpp_
// NEOTKO_COLORSCI_TAG_END
