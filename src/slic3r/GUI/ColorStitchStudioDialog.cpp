// NEOTKO_COLORSCI_TAG_START — GD2+GD3 (P2+P3)
#include "ColorStitchStudioDialog.hpp"

#include <wx/button.h>
#include <wx/dcbuffer.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tooltip.h>

#include <algorithm>
#include <cstdlib>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "../../libslic3r/AppConfig.hpp"
#include "../../libslic3r/ColorSci/StackFlatten.hpp"
#include "../../libslic3r/ColorSci/WeaveLibrary.hpp"
#include "../../libslic3r/SurfaceEffectProfile.hpp"

namespace Slic3r {
namespace GUI {

// ---------------------------------------------------------------------------
// ColorStitchStudioDialog

ColorStitchStudioDialog::ColorStitchStudioDialog(
        wxWindow* parent,
        const std::vector<std::string>& filament_colours,
        double layer_height)
    : wxDialog(parent, wxID_ANY, _L("ColorStitch Studio"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    m_spec.layer_height = layer_height > 0.0 ? layer_height : 0.2;
    load_materials(filament_colours);

    auto* root = new wxBoxSizer(wxVERTICAL);
    m_notebook = new wxNotebook(this, wxID_ANY);
    m_notebook->AddPage(build_gradient_page(m_notebook), _L("Gradient Designer"), true);
    // P5: m_notebook->AddPage(build_weaves_page(...), _L("Weaves"));
    // P5: m_notebook->AddPage(build_td_page(...),     _L("TD / Materials"));
    root->Add(m_notebook, 1, wxEXPAND | wxALL, 8);

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    auto* btn_export = new wxButton(this, wxID_ANY, _L("Export palette…"));
    btn_export->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_export(); });
    btns->Add(btn_export, 0, wxRIGHT, 8);
    btns->AddStretchSpacer(1);
    btns->Add(new wxButton(this, wxID_CLOSE, _L("Close")), 0);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
    root->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizerAndFit(root);
    SetMinSize(wxSize(560, 360));
    CentreOnParent();
    recalc();
}

void ColorStitchStudioDialog::load_materials(
        const std::vector<std::string>& filament_colours)
{
    auto* ac = wxGetApp().app_config;
    for (int i = 0; i < 4; ++i) {
        const std::string hex =
            i < (int)filament_colours.size() ? filament_colours[i] : std::string();
        // Per-channel primero (A.2), escalar legacy como fallback. Mismo
        // parse laxo que el panel TD del SandwichDialog (atof, default 0).
        const std::string base = "neotko_td_" + std::to_string(i + 1);
        const std::string sr = ac ? ac->get(base + "_r") : "";
        const std::string sg = ac ? ac->get(base + "_g") : "";
        const std::string sb = ac ? ac->get(base + "_b") : "";
        if (!sr.empty() || !sg.empty() || !sb.empty()) {
            m_mats[i] = ColorSci::material_from_hex(
                hex,
                (float)std::atof(sr.c_str()),
                (float)std::atof(sg.c_str()),
                (float)std::atof(sb.c_str()));
        } else {
            const std::string ss = ac ? ac->get(base) : "";
            m_mats[i] = ColorSci::material_from_hex(hex, (float)std::atof(ss.c_str()));
        }
    }
}

wxPanel* ColorStitchStudioDialog::build_gradient_page(wxWindow* parent)
{
    auto* page  = new wxPanel(parent);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // --- fila 1: tools + ligamento + pattern --------------------------------
    auto* row1 = new wxBoxSizer(wxHORIZONTAL);
    wxArrayString tools;
    for (int i = 1; i <= 4; ++i)
        tools.Add(wxString::Format("T%d", i));
    row1->Add(new wxStaticText(page, wxID_ANY, _L("Tool A (top)")), 0,
              wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_tool_a = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, tools);
    m_tool_a->SetSelection(0);
    row1->Add(m_tool_a, 0, wxRIGHT, 12);
    row1->Add(new wxStaticText(page, wxID_ANY, _L("Tool B (contrast)")), 0,
              wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_tool_b = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, tools);
    m_tool_b->SetSelection(1);
    row1->Add(m_tool_b, 0, wxRIGHT, 12);

    row1->Add(new wxStaticText(page, wxID_ANY, _L("Weave")), 0,
              wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    wxArrayString weaves;
    for (const auto& w : ColorSci::weave_presets())
        weaves.Add(_L(w.name));
    m_weave = new wxChoice(page, wxID_ANY, wxDefaultPosition, wxDefaultSize, weaves);
    m_weave->SetSelection(0);
    m_weave->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { on_weave_selected(); });
    row1->Add(m_weave, 0, wxRIGHT, 12);

    row1->Add(new wxStaticText(page, wxID_ANY, _L("Penu pattern")), 0,
              wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_pattern = new wxTextCtrl(page, wxID_ANY, "12");
    m_pattern->SetToolTip(_L("Digits 1-4; one digit = one dither line of that tool"));
    row1->Add(m_pattern, 1);
    sizer->Add(row1, 0, wxEXPAND | wxALL, 8);

    // --- fila 2: sweep -------------------------------------------------------
    auto* row2 = new wxBoxSizer(wxHORIZONTAL);
    row2->Add(new wxStaticText(page, wxID_ANY, _L("Steps")), 0,
              wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_steps = new wxSpinCtrl(page, wxID_ANY, "8", wxDefaultPosition,
                             wxSize(64, -1), wxSP_ARROW_KEYS, 1, 30, 8);
    row2->Add(m_steps, 0, wxRIGHT, 12);

    // Rango permitido [kMinSweepMM, LH - kMinSweepMM] — fuera de ahí el
    // normalizador del SandwichDialog plegaría el pass (kMinPassMM).
    const double lo = ColorSci::kMinSweepMM;
    const double hi = std::max(lo, m_spec.layer_height - ColorSci::kMinSweepMM);
    row2->Add(new wxStaticText(page, wxID_ANY, _L("Top split min (mm)")), 0,
              wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_split_min = new wxSpinCtrlDouble(page, wxID_ANY, "0.04", wxDefaultPosition,
                                       wxSize(80, -1), wxSP_ARROW_KEYS, lo, hi, 0.04, 0.01);
    row2->Add(m_split_min, 0, wxRIGHT, 12);
    row2->Add(new wxStaticText(page, wxID_ANY, _L("max (mm)")), 0,
              wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_split_max = new wxSpinCtrlDouble(page, wxID_ANY, "0.16", wxDefaultPosition,
                                       wxSize(80, -1), wxSP_ARROW_KEYS, lo, hi, 0.16, 0.01);
    row2->Add(m_split_max, 0, wxRIGHT, 12);
    row2->Add(new wxStaticText(page, wxID_ANY,
                  wxString::Format(_L("(layer height %.2f mm)"), m_spec.layer_height)),
              0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(row2, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    // --- tira de preview -----------------------------------------------------
    auto* box = new wxStaticBoxSizer(wxVERTICAL, page,
        _L("Ramp preview — click a swatch to include/exclude it from export"));
    m_strip = new RampStripPanel(box->GetStaticBox(), this);
    box->Add(m_strip, 1, wxEXPAND | wxALL, 4);
    sizer->Add(box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    m_warn_label = new wxStaticText(page, wxID_ANY, "");
    m_warn_label->SetForegroundColour(wxColour(200, 120, 0));
    sizer->Add(m_warn_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    // --- nombre de la rampa --------------------------------------------------
    auto* row4 = new wxBoxSizer(wxHORIZONTAL);
    row4->Add(new wxStaticText(page, wxID_ANY, _L("Palette name")), 0,
              wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
    m_ramp_name = new wxTextCtrl(page, wxID_ANY, _L("Ramp"));
    row4->Add(m_ramp_name, 1);
    sizer->Add(row4, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    // Recalc en vivo en cualquier knob (N llamadas a un helper puro — barato).
    auto rebind = [this](wxWindow* w) {
        w->Bind(wxEVT_CHOICE,         [this](wxCommandEvent&)    { recalc(); });
        w->Bind(wxEVT_TEXT,           [this](wxCommandEvent&)    { recalc(); });
        w->Bind(wxEVT_SPINCTRL,       [this](wxSpinEvent&)       { recalc(); });
        w->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { recalc(); });
    };
    rebind(m_tool_a); rebind(m_tool_b); rebind(m_pattern);
    rebind(m_steps);  rebind(m_split_min); rebind(m_split_max);

    page->SetSizer(sizer);
    return page;
}

void ColorStitchStudioDialog::read_spec_from_ui()
{
    m_spec.tool_a       = m_tool_a->GetSelection();
    m_spec.tool_b       = m_tool_b->GetSelection();
    m_spec.penu_pattern = m_pattern->GetValue().ToStdString();
    m_spec.steps        = m_steps->GetValue();
    m_spec.split_min_mm = m_split_min->GetValue();
    m_spec.split_max_mm = m_split_max->GetValue();
}

void ColorStitchStudioDialog::recalc()
{
    if (!m_strip)  // llamadas durante la construcción de la página
        return;
    read_spec_from_ui();
    std::vector<std::string> warnings;
    const ColorSci::GradientSpec s = ColorSci::sanitize(m_spec, &warnings);
    m_ramp = ColorSci::build_ramp(s);
    // La selección persiste por índice mientras N no cambie; si cambia, reset
    // a todo-seleccionado (criterio simple v1).
    if (m_selected.size() != m_ramp.size())
        m_selected.assign(m_ramp.size(), true);
    if (m_warn_label) {
        wxString w;
        for (const auto& msg : warnings) {
            if (!w.empty()) w += "  ·  ";
            w += wxString::FromUTF8(msg);
        }
        m_warn_label->SetLabel(w);
    }
    m_strip->Refresh();
}

void ColorStitchStudioDialog::on_weave_selected()
{
    const auto& presets = ColorSci::weave_presets();
    const int sel = m_weave->GetSelection();
    if (sel <= 0 || sel >= (int)presets.size())
        return;     // Custom → no tocar el campo
    const auto& w = presets[sel];
    // En el Designer el pattern del usuario ES la zona penu: para presets
    // paired (houndstooth) usamos su variante penu; el par top/penu atómico
    // completo es de la pestaña Weaves / SandwichDialog (C.3, P5).
    const char* pat = (w.pattern_penu && w.pattern_penu[0]) ? w.pattern_penu
                                                            : w.pattern_top;
    m_pattern->ChangeValue(wxString::FromUTF8(pat));   // ChangeValue: no re-evento
    recalc();
}

wxColour ColorStitchStudioDialog::step_colour(const ColorSci::GradientStep& g) const
{
    // Composición física apilada (la razón de ser del sweep — ver
    // StackFlatten.hpp). bg negro = mismo convenio que mp_beer_blend default.
    const float bg[3] = { 0.f, 0.f, 0.f };
    float rgb[3];
    ColorSci::sandwich_colour_stacked(g.top, g.penu, m_mats.data(), bg, rgb);
    return wxColour((unsigned char)std::min(255.f, rgb[0] * 255.f),
                    (unsigned char)std::min(255.f, rgb[1] * 255.f),
                    (unsigned char)std::min(255.f, rgb[2] * 255.f));
}

void ColorStitchStudioDialog::on_export()
{
    recalc();   // asegurar coherencia spec/ramp/selección
    int n_sel = 0;
    for (bool b : m_selected) n_sel += b ? 1 : 0;
    if (n_sel == 0) {
        wxMessageBox(_L("No swatches selected — click swatches in the preview "
                        "strip to include them."),
                     _L("Export palette"), wxOK | wxICON_WARNING, this);
        return;
    }

    const std::string ramp_name = m_ramp_name->GetValue().ToStdString();
    auto& mgr = Slic3r::SurfaceEffectProfileManager::get();
    int created = 0;
    for (size_t i = 0; i < m_ramp.size(); ++i) {
        if (!m_selected[i])
            continue;
        // Mismo camino que SandwichDialog::on_save_profile (Tab.cpp:7375):
        // solo blobs autoritativos; payloads legacy quedan present=false
        // (el painter Fase 6b consume los blobs — ver PAINTER_3D.md).
        // El ColorMix penu nace self-contained (kv canónico completo), así
        // que no necesita el bake de zone_colormix_snapshot.
        Slic3r::SurfaceEffectProfile p;
        p.name = wxString::Format("%s %d/%d (A%.2f B%.2f)",
                                  wxString::FromUTF8(ramp_name),
                                  (int)i + 1, (int)m_ramp.size(),
                                  m_ramp[i].a_mm, m_ramp[i].b_mm).ToStdString();
        p.stack_top_json  = m_ramp[i].top.to_json();
        p.stack_penu_json = m_ramp[i].penu.to_json();
        const wxColour c  = step_colour(m_ramp[i]);
        p.preview_argb    = 0xFF000000u
                          | ((uint32_t)c.Red()   << 16)
                          | ((uint32_t)c.Green() <<  8)
                          |  (uint32_t)c.Blue();
        mgr.add(std::move(p));
        ++created;
    }
    wxMessageBox(wxString::Format(
                     _L("%d profiles created — available as a palette in the "
                        "3D Painter (Sandwich)."), created),
                 _L("Export palette"), wxOK | wxICON_INFORMATION, this);
}

// ---------------------------------------------------------------------------
// RampStripPanel

RampStripPanel::RampStripPanel(wxWindow* parent, ColorStitchStudioDialog* owner)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition,
              wxSize(8 * (kSwatchW + kGap) + kGap, kSwatchH + 2 * kGap))
    , m_owner(owner)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT,     &RampStripPanel::on_paint,  this);
    Bind(wxEVT_LEFT_DOWN, &RampStripPanel::on_click,  this);
    Bind(wxEVT_MOTION,    &RampStripPanel::on_motion, this);
}

int RampStripPanel::swatch_at(int x, int y) const
{
    if (y < kGap || y > kGap + kSwatchH)
        return -1;
    const int idx = (x - kGap) / (kSwatchW + kGap);
    const int x0  = kGap + idx * (kSwatchW + kGap);
    if (x < x0 || x > x0 + kSwatchW)
        return -1;
    return (idx >= 0 && idx < (int)m_owner->m_ramp.size()) ? idx : -1;
}

void RampStripPanel::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
    dc.Clear();
    const auto& ramp = m_owner->m_ramp;
    for (size_t i = 0; i < ramp.size(); ++i) {
        const int x = kGap + (int)i * (kSwatchW + kGap);
        const bool sel = i < m_owner->m_selected.size() && m_owner->m_selected[i];
        const wxColour c = m_owner->step_colour(ramp[i]);
        dc.SetBrush(wxBrush(c));
        dc.SetPen(sel ? wxPen(wxColour(0, 120, 255), 2)
                      : wxPen(wxColour(140, 140, 140), 1));
        dc.DrawRectangle(x, kGap, kSwatchW, kSwatchH);
        if (!sel) {
            // velo "excluido" — hatch diagonal gris
            dc.SetBrush(wxBrush(wxColour(128, 128, 128), wxBRUSHSTYLE_BDIAGONAL_HATCH));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(x, kGap, kSwatchW, kSwatchH);
        }
    }
}

void RampStripPanel::on_click(wxMouseEvent& e)
{
    const int idx = swatch_at(e.GetX(), e.GetY());
    if (idx >= 0 && idx < (int)m_owner->m_selected.size()) {
        m_owner->m_selected[idx] = !m_owner->m_selected[idx];
        Refresh();
    }
}

void RampStripPanel::on_motion(wxMouseEvent& e)
{
    const int idx = swatch_at(e.GetX(), e.GetY());
    if (idx >= 0) {
        const auto& g = m_owner->m_ramp[idx];
        SetToolTip(wxString::Format("A %.2f / B %.2f mm", g.a_mm, g.b_mm));
    } else {
        UnsetToolTip();
    }
    e.Skip();
}

} // namespace GUI
} // namespace Slic3r
// NEOTKO_COLORSCI_TAG_END
