// #include "libslic3r/GCodeSender.hpp"
//#include "slic3r/Utils/Serial.hpp"
#include "Tab.hpp"
#include "PresetHints.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "WipeTowerDialog.hpp"

#include "Search.hpp"
#include "OG_CustomCtrl.hpp"
#include "NeoArachnePreviewPanel.hpp"  // NEOTKO_NEOARACHNE_TAG preview-lab

#include <wx/app.h>
#include <wx/button.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/settings.h>
#include <wx/filedlg.h>
#include <wx/textdlg.h> // NEOTKO_PROFILE_TAG: wxTextEntryDialog
#include <wx/listbox.h> // NEOTKO_PROFILE_TAG: wxListBox
#include <wx/radiobox.h> // NEOTKO_SANDWICH_TAG: wxRadioBox slot selector
#include <wx/menu.h>     // NEOTKO_SANDWICH_TAG: wxMenu tool picker

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include "libslic3r/libslic3r.h"
#include "slic3r/GUI/OptionsGroup.hpp"
#include "wxExtensions.hpp"
#include "PresetComboBoxes.hpp"
#include <wx/wupdlock.h>

#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "format.hpp"
#include "UnsavedChangesDialog.hpp"
#include "SavePresetDialog.hpp"
#include "EditGCodeDialog.hpp"
#include "MsgDialog.hpp"
#include "Notebook.hpp"

#include "Widgets/Label.hpp"
#include "Widgets/TabCtrl.hpp"
#include "MarkdownTip.hpp"
#include "Search.hpp"
#include "BedShapeDialog.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "NotificationManager.hpp"

#include "BedShapeDialog.hpp"
// #include "BonjourDialog.hpp"
#ifdef WIN32
	#include <commctrl.h>
#endif // WIN32

#include "libslic3r/SurfaceColorMix.hpp" // NEOTKO_COLORMIX_TAG + PathBlend
                                         // ingredient (runtimes for s88 toggles).
#include "slic3r/GUI/ColorStitchPatternLauncher.hpp" // NEOTKO_COLORSTITCH_TAG — puente al 3D Painter
#include "libslic3r/SurfaceEffectProfile.hpp" // NEOTKO_PROFILE_TAG
// NEOTKO_COLORSCI_TAG GD1+GD2+GD3 — Gradient Designer embebido en SandwichDialog
// (docs/FUTURE/COLORSTITCH_STUDIO_PLAN.md P1-P3). Engine en libslic3r/ColorSci/.
#include "libslic3r/ColorSci/GradientRamp.hpp"
#include "libslic3r/ColorSci/StackFlatten.hpp"
#include "libslic3r/ColorSci/WeaveLibrary.hpp"
#include "libslic3r/ColorSci/ColorPredict.hpp"
#include <fstream>    // NEOTKO_COLORMIX_TAG (preset save/load)
#include <sstream>    // NEOTKO_PROFILE_TAG: std::ostringstream for MP payload serialisation
#include <functional> // NEOTKO_COLORMIX_TAG (std::function in MultiPassPreviewPanel)
#include <map>        // NEOTKO_SURFACE_MIXER_TAG (blend_preview)
#include <cstdio>     // NEOTKO_SURFACE_MIXER_TAG (std::snprintf for TD values)
#include <numeric>    // NEOTKO_MULTIPASS_TAG (std::iota in Beer-Lambert suggest)
#include <cmath>      // NEOTKO_MULTIPASS_TAG (std::cbrt, std::pow in mp_rgb_to_lab)
#include <algorithm>  // NEOTKO_MULTIPASS_TAG (std::next_permutation, std::stable_sort)

namespace Slic3r {
namespace GUI {

// NEOTKO_COLORMIX_TAG_START - ColorMix Pattern Dialog (no separate file needed)
// Two-band dialog: Top surface + Penultimate surface, each with checkbox toggle
// and direct pattern editing. Checkboxes control interlayer_colormix_surface.
// Surface values: 0=Both, 1=Top only, 2=Penultimate only.
namespace {

// NEOTKO_COLORMIX_TAG — s60: GradientStripPanel
// Paints a horizontal strip showing the actual colours produced by the current
// gradient settings. Each line in the dither sequence becomes one 1-px-wide
// vertical stripe coloured with the chosen filament. This gives the user the
// visual feedback they asked for ("represención del gradiente que se está
// creando") instead of the abstract digit-string preview.
class GradientStripPanel : public wxPanel {
public:
    GradientStripPanel(wxWindow* parent, int width = 360, int height = 28)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(width, height))
    {
        SetMinSize(wxSize(width, height));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &GradientStripPanel::on_paint, this);
    }
    void set_sequence(const std::vector<int>& tool_seq,
                      const std::vector<wxColour>& tool_colors)
    {
        m_seq    = tool_seq;
        m_colors = tool_colors;
        Refresh();
    }
private:
    std::vector<int>      m_seq;
    std::vector<wxColour> m_colors;

    void on_paint(wxPaintEvent&) {
        wxPaintDC dc(this);
        const wxSize sz = GetSize();
        dc.SetBackground(wxBrush(wxColour(45, 45, 45)));
        dc.Clear();
        if (m_seq.empty()) {
            dc.SetTextForeground(wxColour(150, 150, 150));
            dc.DrawText("(no preview)", 6, sz.y / 2 - 8);
            return;
        }
        const int n = (int)m_seq.size();
        const double sw = double(sz.x) / double(n);
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (int i = 0; i < n; ++i) {
            const int tool = m_seq[i];
            wxColour c = (tool >= 0 && tool < (int)m_colors.size())
                ? m_colors[tool] : wxColour(160, 160, 160);
            dc.SetBrush(wxBrush(c));
            const int x0 = (int)std::floor(i * sw);
            const int x1 = (int)std::ceil((i + 1) * sw);
            dc.DrawRectangle(x0, 0, std::max(1, x1 - x0), sz.y);
        }
        // Thin border
        dc.SetPen(wxPen(wxColour(30, 30, 30), 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(0, 0, sz.x, sz.y);
    }
};

// NEOTKO_MULTIPASS_TAG — ColorSwatch (forward-moved from below to be visible
// from ColorMixPatternDialog::build_gradient_section). Small coloured panel
// used in the gradient slot pickers.
class ColorSwatch : public wxPanel {
    wxColour m_col;
public:
    ColorSwatch(wxWindow* parent, const wxColour& col)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(16, 16))
        , m_col(col.IsOk() ? col : wxColour(160, 160, 160))
    {
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC dc(this);
            dc.SetBrush(wxBrush(m_col));
            dc.SetPen(wxPen(wxColour(80, 80, 80)));
            const wxSize sz = GetSize();
            dc.DrawRectangle(0, 0, sz.x, sz.y);
        });
    }
    void set_color(const wxColour& c) {
        m_col = c.IsOk() ? c : wxColour(160, 160, 160);
        Refresh();
    }
};

class ColorMixPatternDialog : public wxDialog
{
public:
    // Single pattern — surface (Top/Penu/Both) is controlled by the SurfaceColorMixer combos.
    // use_virtual: when true, virtual Mixed Filament buttons are shown and pattern digits
    // 5-9 are accepted (requires interlayer_colormix_use_virtual=true in config, C0 gate).
    ColorMixPatternDialog(wxWindow*                                       parent,
                          const std::vector<Slic3r::ColorMixOption>&      options,
                          const std::vector<std::string>&                 filament_colours,
                          const std::string&                              cur_pattern,
                          bool                                            use_virtual = false,
                          DynamicPrintConfig*                             cfg = nullptr,
                          int                                             surface_id = 0)
        : wxDialog(parent, wxID_ANY,
                   surface_id == 0 ? _L("ColorStitch — Top surface") : _L("ColorStitch — Penultimate"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_colours(filament_colours)
        , m_pattern(cur_pattern)
        , m_use_virtual(use_virtual)
        , m_cfg(cfg)
        , m_surface_id(surface_id)
    {
        // Build digit→color map from options so paint_pattern() handles both
        // physical (digits '1'-'4') and virtual (digits '5'-'9') uniformly.
        for (const auto& opt : options) {
            if (opt.filament_id >= 1 && opt.filament_id <= 9) {
                char digit = static_cast<char>('0' + opt.filament_id);
                m_digit_colors[digit] = hex_to_colour(opt.display_color);
            }
        }
        // Pull current gradient config values from m_cfg (read-only here;
        // write-back happens in get_*() accessors consumed by open_edit_for).
        if (m_cfg) load_grad_from_config();
        build_ui(options);
    }

    std::string get_pattern() const { return m_pattern; }
    // Compatibility aliases — both surfaces share the same pattern now.
    std::string get_top()     const { return m_pattern; }
    std::string get_penu()    const { return m_pattern; }
    int         get_surface() const { return 0; }

    // NEOTKO_COLORMIX_TAG — s60: gradient getters consumed by open_edit_for()
    // after the dialog closes with OK. Each returns the value as edited in the
    // dialog widgets; the caller writes them back to DynamicPrintConfig.
    int    get_grad_mode()       const { return m_choice_grad_mode  ? m_choice_grad_mode->GetSelection() : m_grad_mode; }
    int    get_grad_pct_a()      const { return m_sl_pct_a          ? m_sl_pct_a->GetValue()             : m_grad_pct_a; }
    int    get_grad_pct_b()      const { return m_sl_pct_b          ? m_sl_pct_b->GetValue()             : m_grad_pct_b; }
    int    get_grad_easing()     const { return m_choice_easing     ? m_choice_easing->GetSelection()    : m_grad_easing; }
    double get_grad_gamma()      const { return m_sc_gamma          ? m_sc_gamma->GetValue()             : m_grad_gamma; }
    int    get_grad_min_lines()  const { return m_sc_min_lines      ? m_sc_min_lines->GetValue()         : m_grad_min_lines; }
    double get_grad_overlap()    const { return m_sl_overlap        ? m_sl_overlap->GetValue() / 100.0   : m_grad_overlap; }
    bool   get_grad_invert()     const { return m_chk_invert        ? m_chk_invert->GetValue()           : m_grad_invert; }
    int    get_grad_band_a()     const { return m_sc_band_a         ? m_sc_band_a->GetValue()            : m_grad_band_a; }
    int    get_grad_band_b()     const { return m_sc_band_b         ? m_sc_band_b->GetValue()            : m_grad_band_b; }
    int    get_grad_band_c()     const { return m_sc_band_c         ? m_sc_band_c->GetValue()            : m_grad_band_c; }
    int    get_grad_band_d()     const { return m_sc_band_d         ? m_sc_band_d->GetValue()            : m_grad_band_d; }
    int    get_tool_a()          const { return m_choice_tool_a     ? m_choice_tool_a->GetSelection()    : m_tool_a; }
    int    get_tool_b()          const { return m_choice_tool_b     ? m_choice_tool_b->GetSelection()    : m_tool_b; }
    int    get_tool_c()          const { return m_choice_tool_c     ? m_choice_tool_c->GetSelection()    : m_tool_c; }
    int    get_tool_d()          const { return m_choice_tool_d     ? m_choice_tool_d->GetSelection()    : m_tool_d; }
    int    get_grad_angle()      const { return m_sc_angle           ? m_sc_angle->GetValue()              : m_grad_angle; }
    int    get_grad_repetitions() const { return m_sc_repetitions     ? m_sc_repetitions->GetValue()        : m_grad_repetitions; } // NEOTKO_COLORMIX_TAG — s80
    // NEOTKO_COLORMIX_TAG — s90: ported from the legacy SurfaceColorMixerDialog.
    // Global (single key, no penu mirror) — both Top/Penu Advanced dialogs read
    // and write the SAME value; last-write-wins is the intended behavior.
    double get_grad_min_length() const { return m_sc_min_length      ? m_sc_min_length->GetValue()        : m_grad_min_length; }

private:
    std::vector<std::string>     m_colours;
    std::string                  m_pattern;
    bool                         m_use_virtual = false;
    DynamicPrintConfig*          m_cfg         = nullptr;
    int                          m_surface_id  = 0; // 0 = Top, 1 = Penultimate
    wxPanel*                     m_disp = nullptr;
    std::map<char, wxColour>     m_digit_colors; // digit '1'-'9' → display color

    // ── Gradient state (mirrors config; populated by load_grad_from_config) ──
    int    m_grad_mode      = 0;
    int    m_grad_pct_a     = 50;
    int    m_grad_pct_b     = 33;
    int    m_grad_easing    = 0;
    double m_grad_gamma     = 1.0;
    int    m_grad_min_lines = 3;
    double m_grad_overlap   = 0.6;
    bool   m_grad_invert    = false;
    int    m_grad_band_a    = 10;
    int    m_grad_band_b    = 10;
    int    m_grad_band_c    = 0;
    int    m_grad_band_d    = 0;
    int    m_tool_a         = 0;
    int    m_tool_b         = 1;
    int    m_tool_c         = 2;
    int    m_tool_d         = 3;
    int    m_grad_angle     = -1; // -1 = Auto (use OrcaSlicer defaults)
    int    m_grad_repetitions = 1; // NEOTKO_COLORMIX_TAG — s80: gradient repeats
    double m_grad_min_length  = 1.0; // NEOTKO_COLORMIX_TAG — s90: global "ColorMix min. line length" (mm)

    // Gradient widget pointers (created in build_ui when m_cfg != nullptr).
    wxStaticBoxSizer* m_gd_sb            = nullptr;
    wxChoice*         m_choice_grad_mode = nullptr;
    wxChoice*         m_choice_tool_a    = nullptr;
    wxChoice*         m_choice_tool_b    = nullptr;
    wxChoice*         m_choice_tool_c    = nullptr;
    wxChoice*         m_choice_tool_d    = nullptr;
    ColorSwatch*      m_sw_tool_a        = nullptr;
    ColorSwatch*      m_sw_tool_b        = nullptr;
    ColorSwatch*      m_sw_tool_c        = nullptr;
    ColorSwatch*      m_sw_tool_d        = nullptr;
    wxSlider*         m_sl_pct_a         = nullptr;
    wxSlider*         m_sl_pct_b         = nullptr;
    wxStaticText*     m_lbl_pct_a        = nullptr;
    wxStaticText*     m_lbl_pct_b        = nullptr;
    wxStaticText*     m_lbl_pct_c        = nullptr;
    wxChoice*         m_choice_easing    = nullptr;
    wxSpinCtrlDouble* m_sc_gamma         = nullptr;
    wxSpinCtrl*       m_sc_min_lines     = nullptr;
    wxSpinCtrl*       m_sc_band_a        = nullptr;
    wxSpinCtrl*       m_sc_band_b        = nullptr;
    wxSpinCtrl*       m_sc_band_c        = nullptr;
    wxSpinCtrl*       m_sc_band_d        = nullptr;
    wxStaticText*     m_lbl_preview      = nullptr;
    wxStaticText*     m_lbl_lines_est    = nullptr;
    wxPanel*          m_panel_linear     = nullptr;
    wxPanel*          m_panel_bands      = nullptr;
    wxStaticText*     m_lbl_mf_lock_note = nullptr; // shown when pattern is using a MixedFilament digit
    GradientStripPanel* m_strip          = nullptr; // visual colour preview
    wxSlider*         m_sl_overlap       = nullptr; // s60: color overlap slider
    wxStaticText*     m_lbl_overlap      = nullptr;
    wxCheckBox*       m_chk_invert       = nullptr; // s60: invert gradient direction
    wxButton*         m_btn_invert       = nullptr; // button to invert/reverse custom pattern string
    wxSpinCtrl*       m_sc_angle         = nullptr; // infill angle override spin control
    wxSpinCtrl*       m_sc_repetitions   = nullptr; // NEOTKO_COLORMIX_TAG — s80: gradient repetitions
    wxSpinCtrlDouble* m_sc_min_length    = nullptr; // NEOTKO_COLORMIX_TAG — s90: global min line length (mm)
    bool                         m_mixed_filament_selected = false;
    std::vector<wxButton*>       m_physical_buttons;

    // NEOTKO_COLORMIX_TAG — s61: role-aware config key prefix.
    // m_surface_id 0 = Top → reads/writes the original (top-role) keys.
    // m_surface_id 1 = Penultimate → reads/writes the `_penu_` mirror keys.
    // Returns the full config key string for a given base name. Pattern keys
    // (`pattern_top` / `pattern_penultimate`) are NOT routed through this —
    // they keep their explicit names and the caller picks the right one.
    std::string grad_key(const char* base) const {
        std::string out = "interlayer_colormix_";
        if (m_surface_id == 1) out += "penu_";
        out += base;
        return out;
    }

    void load_grad_from_config()
    {
        if (!m_cfg) return;
        auto gi = [&](const std::string& k, int def) -> int {
            auto* o = m_cfg->option<ConfigOptionInt>(k);
            return o ? o->value : def;
        };
        auto gf = [&](const std::string& k, double def) -> double {
            auto* o = m_cfg->option<ConfigOptionFloat>(k);
            return o ? o->value : def;
        };
        auto gb = [&](const std::string& k, bool def) -> bool {
            auto* o = m_cfg->option<ConfigOptionBool>(k);
            return o ? o->value : def;
        };
        m_grad_mode      = gi(grad_key("mode"), 0);
        m_grad_pct_a     = gi(grad_key("pct_a"), 50);
        m_grad_pct_b     = gi(grad_key("pct_b"), 33);
        m_grad_easing    = gi(grad_key("easing"), 0);
        m_grad_gamma     = gf(grad_key("gamma"), 1.0);
        m_grad_min_lines = gi(grad_key("min_surface_lines"), 3);
        m_grad_overlap   = gf(grad_key("overlap"), 0.6);
        m_grad_invert    = gb(grad_key("invert"), false);
        m_grad_band_a    = gi(grad_key("band_count_a"), 10);
        m_grad_band_b    = gi(grad_key("band_count_b"), 10);
        m_grad_band_c    = gi(grad_key("band_count_c"), 0);
        m_grad_band_d    = gi(grad_key("band_count_d"), 0);
        // Legacy config defaulted tool_c/_d to -1 ("off"). Treat any negative
        // value as "user hasn't configured this slot yet" and default it to a
        // sensible physical index in the dialog (2, 3) so the slot picker
        // shows a real tool. On save, the chosen tool index is written back —
        // never -1 — so the s60 SurfaceColorMix path always sees a real slot.
        m_tool_a         = gi(grad_key("tool_a"), 0);
        if (m_tool_a < 0) m_tool_a = 0;
        m_tool_b         = gi(grad_key("tool_b"), 1);
        if (m_tool_b < 0) m_tool_b = 1;
        m_tool_c         = gi(grad_key("tool_c"), 2);
        if (m_tool_c < 0) m_tool_c = 2;
        m_tool_d         = gi(grad_key("tool_d"), 3);
        if (m_tool_d < 0) m_tool_d = 3;
        m_grad_angle     = gi(grad_key("angle"), -1);
        m_grad_repetitions = gi(grad_key("repetitions"), 1); // NEOTKO_COLORMIX_TAG — s80
        // NEOTKO_COLORMIX_TAG — s90: global key (NO grad_key() prefix). Both
        // Top and Penu Advanced dialogs read/write this single value.
        m_grad_min_length  = gf("interlayer_colormix_min_length", 1.0);
    }

    // Is the current pattern using a virtual MixedFilament digit (5-9)? If
    // yes, gray out the gradient section — MixedFilament IS the pattern and
    // mixing it with a numeric dither produces unpredictable results.
    bool pattern_uses_mixed_filament() const {
        for (char c : m_pattern) if (c >= '5' && c <= '9') return true;
        return false;
    }

    static wxColour hex_to_colour(const std::string& hex)
    {
        unsigned long rgb = 0;
        wxString s = wxString::FromUTF8(hex);
        if (s.StartsWith("#")) s = s.Mid(1);
        if (!s.ToULong(&rgb, 16)) return wxColour(128, 128, 128);
        return wxColour(static_cast<unsigned char>((rgb >> 16) & 0xFF),
                        static_cast<unsigned char>((rgb >>  8) & 0xFF),
                        static_cast<unsigned char>( rgb        & 0xFF));
    }

    // Draw coloured digit-blocks for a pattern string into dc.
    // Uses m_digit_colors so both physical and virtual digits render with the right color.
    void paint_pattern(wxPaintDC& dc, const std::string& pat, wxPanel* p)
    {
        wxRect r = p->GetClientRect();
        dc.SetBackground(wxBrush(GetBackgroundColour()));
        dc.Clear();
        const int BW = 24, GAP = 2;
        int x = 0;
        dc.SetFont(GetFont());
        for (char c : pat) {
            auto it = m_digit_colors.find(c);
            wxColour col = (it != m_digit_colors.end()) ? it->second : wxColour(128, 128, 128);
            dc.SetBrush(wxBrush(col));
            dc.SetPen(*wxBLACK_PEN);
            dc.DrawRectangle(x, 0, BW, r.height);
            int lum = (col.Red()*299 + col.Green()*587 + col.Blue()*114) / 1000;
            dc.SetTextForeground(lum > 128 ? *wxBLACK : *wxWHITE);
            wxString ch(c);
            wxCoord tw, th;
            dc.GetTextExtent(ch, &tw, &th);
            dc.DrawText(ch, x + (BW - tw) / 2, (r.height - th) / 2);
            x += BW + GAP;
        }
    }

    void append_digit(int filament_id)
    {
        if (filament_id < 1 || filament_id > 9) return;
        m_pattern += static_cast<char>('0' + filament_id);
        const int needed = (int)m_pattern.size() * 26 + 8;
        if (needed > m_disp->GetMinWidth()) {
            m_disp->SetMinSize(wxSize(needed, 26));
            Fit();
        }
        m_disp->Refresh();
        refresh_grad_lock(); // a newly-appended virtual digit (5-9) locks gradient
        refresh_grad_preview(); // strip should reflect the pattern in legacy mode
    }

    void build_ui(const std::vector<Slic3r::ColorMixOption>& options)
    {
        m_mixed_filament_selected = false;
        if (!m_pattern.empty()) {
            for (const auto& opt : options) {
                if (!opt.is_physical) {
                    std::string rev = opt.pattern;
                    std::reverse(rev.begin(), rev.end());
                    if (opt.pattern == m_pattern || rev == m_pattern) {
                        m_mixed_filament_selected = true;
                        break;
                    }
                }
            }
        }
        m_physical_buttons.clear();

        const int PAD = 6;
        auto* vs = new wxBoxSizer(wxVERTICAL);

        vs->Add(new wxStaticText(this, wxID_ANY,
                                  _L("Click filaments to build pattern:")),
                0, wxLEFT|wxTOP|wxBOTTOM, PAD);

        auto* btn_row = new wxBoxSizer(wxHORIZONTAL);

        // Physical filament buttons — always shown (existing behaviour).
        for (const auto& opt : options) {
            if (!opt.is_physical || opt.filament_id < 1 || opt.filament_id > 9) continue;
            auto* b = new wxButton(this, wxID_ANY, wxString::FromUTF8(opt.label),
                                   wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            b->SetBackgroundColour(hex_to_colour(opt.display_color));
            b->SetToolTip(wxString::FromUTF8(opt.display_color));
            int fid = opt.filament_id;
            b->Bind(wxEVT_BUTTON, [this, fid](wxCommandEvent&) { append_digit(fid); });
            btn_row->Add(b, 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 2);
            m_physical_buttons.push_back(b);
        }

        // Virtual (Mixed Filament) buttons — shown only when use_virtual gate is ON.
        // NEOTKO_COLORMIX_TAG_START — virtual filament buttons
        if (m_use_virtual) {
            bool separator_added = false;
            for (const auto& opt : options) {
                if (opt.is_physical || opt.filament_id < 1 || opt.filament_id > 9) continue;
                if (!separator_added) {
                    // Vertical line separator between physical and virtual groups.
                    auto* sep = new wxStaticLine(this, wxID_ANY,
                                                 wxDefaultPosition, wxDefaultSize,
                                                 wxLI_VERTICAL);
                    btn_row->Add(sep, 0, wxEXPAND|wxLEFT|wxRIGHT, 4);
                    separator_added = true;
                }
                // Short label: strip "Mixed (" prefix for compact buttons.
                wxString short_lbl = wxString::FromUTF8(opt.label);
                if (short_lbl.StartsWith("Mixed (") && short_lbl.EndsWith(")"))
                    short_lbl = short_lbl.Mid(7, short_lbl.Len() - 8); // "F1+F2"
                auto* b = new wxButton(this, wxID_ANY, short_lbl,
                                       wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                b->SetBackgroundColour(hex_to_colour(opt.display_color));
                b->SetToolTip(wxString::FromUTF8(opt.label)
                              + wxString::Format(" [digit %d]", opt.filament_id));
                std::string recipe = opt.pattern;
                b->Bind(wxEVT_BUTTON, [this, recipe](wxCommandEvent&) {
                    m_pattern = recipe;
                    m_mixed_filament_selected = true;
                    const int needed = (int)m_pattern.size() * 26 + 8;
                    if (needed > m_disp->GetMinWidth()) {
                        m_disp->SetMinSize(wxSize(needed, 26));
                        Fit();
                    }
                    m_disp->Refresh();
                    this->refresh_grad_lock();
                    this->refresh_grad_preview();
                });
                btn_row->Add(b, 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 2);
            }
        }
        // NEOTKO_COLORMIX_TAG_END

        vs->Add(btn_row, 0, wxLEFT|wxRIGHT|wxBOTTOM, PAD);

        // Pattern display panel
        const int init_w = std::max(160, (int)m_pattern.size() * 26 + 8);
        m_disp = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(init_w, 26));
        m_disp->SetMinSize(wxSize(160, 26));
        m_disp->SetBackgroundStyle(wxBG_STYLE_PAINT);
        m_disp->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxPaintDC dc(m_disp);
            paint_pattern(dc, m_pattern, m_disp);
        });
        vs->Add(m_disp, 0, wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM, PAD);

        // Action buttons row below pattern display
        auto* btn_action_row = new wxBoxSizer(wxHORIZONTAL);

        auto* bcl = new wxButton(this, wxID_ANY, _L("Clear"),
                                  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        bcl->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            m_pattern.clear();
            m_mixed_filament_selected = false;
            m_disp->SetMinSize(wxSize(160, 26));
            Fit();
            m_disp->Refresh();
            this->refresh_grad_lock();   // pattern changed → re-evaluate MF gray-out
            this->refresh_grad_preview(); // strip should reflect empty pattern
        });
        btn_action_row->Add(bcl, 0, wxRIGHT, 12); // Premium 12px spacing to prevent clutter

        m_btn_invert = new wxButton(this, wxID_ANY, _L("Invert"),
                                    wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        m_btn_invert->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (!m_pattern.empty()) {
                std::reverse(m_pattern.begin(), m_pattern.end());
                m_disp->Refresh();
                this->refresh_grad_preview();
            }
        });
        btn_action_row->Add(m_btn_invert, 0, 0, 0);

        vs->Add(btn_action_row, 0, wxLEFT|wxBOTTOM, PAD);

        // ── Gradient section (numeric dither) — s60. ────────────────────────
        // Only built when m_cfg is provided AND the dialog is in ColorMix
        // context (the caller passes m_cfg from SurfaceColorMixerDialog).
        // For MultiPass/PathBlend roles the dialog is never opened, so this
        // is always safe to add when m_cfg != nullptr.
        if (m_cfg) build_gradient_section(vs, options);

        vs->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL),
                0, wxALL|wxALIGN_RIGHT, PAD);
        this->refresh_grad_lock();
        SetSizerAndFit(vs);
    }

    // ────────────────────────────────────────────────────────────────────────
    // NEOTKO_COLORMIX_TAG — s60: Gradient (numeric dither) section.
    // Inserted into the per-role ColorMix Advanced dialog. Edits 14 config
    // keys (mode, pct_a/b, easing, gamma, min_lines, band_a/b/c/d, tool_a/b/c/d)
    // that apply to the active surface(s). Tool slots A/B/C/D are exposed as
    // wxChoice combos showing the physical tool index (T0/T1/…) and a colour
    // swatch — the user picks WHICH physical tool occupies each slot, then
    // the gradient/bands controls reference the slot.
    // ────────────────────────────────────────────────────────────────────────
    void build_gradient_section(wxBoxSizer* vs, const std::vector<Slic3r::ColorMixOption>& options)
    {
        const int PAD = 6;
        m_gd_sb = new wxStaticBoxSizer(wxVERTICAL, this, _L("Gradient (numeric dither)"));

        // ── Style row (friendlier than "Mode") ───────────────────────────
        {
            wxArrayString labels;
            labels.Add(_L("Custom text pattern  (use the colour buttons above)"));
            labels.Add(_L("Smooth blend — 2 colours"));
            labels.Add(_L("Smooth blend — 3 colours"));
            labels.Add(_L("Stripes — manual band sizes"));
            m_choice_grad_mode = new wxChoice(this, wxID_ANY,
                wxDefaultPosition, wxSize(360, -1), labels);
            m_choice_grad_mode->SetSelection(std::clamp(m_grad_mode, 0, 3));
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(this, wxID_ANY, _L("Style:")),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(m_choice_grad_mode, 1, wxALIGN_CENTER_VERTICAL);
            m_gd_sb->Add(row, 0, wxEXPAND | wxALL, PAD / 2);
        }

        // ── Tool slots A/B/C/D picker row ──────────────────────────────
        wxArrayString tool_labels;
        std::vector<wxColour> tool_colors_for_idx;
        for (const auto& opt : options) {
            if (!opt.is_physical) continue;
            if (opt.filament_id < 1 || opt.filament_id > 4) continue;
            const int idx = opt.filament_id - 1;
            while ((int)tool_colors_for_idx.size() <= idx)
                tool_colors_for_idx.push_back(wxColour(160,160,160));
            tool_colors_for_idx[idx] = hex_to_colour(opt.display_color);
            tool_labels.Add(wxString::Format("T%d", idx));
        }
        if (tool_labels.IsEmpty()) {
            for (int i = 0; i < 4; ++i) {
                tool_labels.Add(wxString::Format("T%d", i));
                tool_colors_for_idx.push_back(wxColour(160,160,160));
            }
        }
        auto colour_for_tool = [tool_colors_for_idx](int t) -> wxColour {
            if (t >= 0 && t < (int)tool_colors_for_idx.size()) return tool_colors_for_idx[t];
            return wxColour(160,160,160);
        };
        {
            auto* sb_tools = new wxStaticBoxSizer(wxHORIZONTAL, this, _L("Colours used"));
            auto make_slot = [&](const wxString& slot_lbl, int cur_tool,
                                 wxChoice*& out_choice, ColorSwatch*& out_swatch)
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(this, wxID_ANY, slot_lbl),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                out_choice = new wxChoice(this, wxID_ANY,
                    wxDefaultPosition, wxSize(60, -1), tool_labels);
                const int sel = std::clamp(cur_tool, 0, (int)tool_labels.GetCount() - 1);
                out_choice->SetSelection(sel);
                row->Add(out_choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                out_swatch = new ColorSwatch(this, colour_for_tool(sel));
                row->Add(out_swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
                out_choice->Bind(wxEVT_CHOICE, [this, out_choice, out_swatch, colour_for_tool](wxCommandEvent&) {
                    out_swatch->set_color(colour_for_tool(out_choice->GetSelection()));
                    this->refresh_grad_preview();
                });
                sb_tools->Add(row, 0, wxALIGN_CENTER_VERTICAL);
            };
            make_slot(_L("Color 1:"), m_tool_a, m_choice_tool_a, m_sw_tool_a);
            make_slot(_L("Color 2:"), m_tool_b, m_choice_tool_b, m_sw_tool_b);
            make_slot(_L("Color 3:"), m_tool_c, m_choice_tool_c, m_sw_tool_c);
            make_slot(_L("Color 4:"), m_tool_d, m_choice_tool_d, m_sw_tool_d);
            m_gd_sb->Add(sb_tools, 0, wxEXPAND | wxALL, PAD / 2);
        }

        // ── Linear panel (modes 1 + 2) ─────────────────────────────────
        m_panel_linear = new wxPanel(this);
        {
            auto* pv = new wxBoxSizer(wxVERTICAL);
            auto add_pct_row = [&](const wxString& tag, int init_v,
                                   wxSlider*& out_sl, wxStaticText*& out_lbl)
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(m_panel_linear, wxID_ANY, tag),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                out_sl = new wxSlider(m_panel_linear, wxID_ANY, init_v, 0, 100,
                    wxDefaultPosition, wxSize(220, -1), wxSL_HORIZONTAL);
                out_lbl = new wxStaticText(m_panel_linear, wxID_ANY,
                    wxString::Format("%d%%", init_v));
                row->Add(out_sl, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                row->Add(out_lbl, 0, wxALIGN_CENTER_VERTICAL);
                pv->Add(row, 0, wxEXPAND | wxALL, 2);
            };
            add_pct_row(_L("How much Color 1:"), m_grad_pct_a, m_sl_pct_a, m_lbl_pct_a);
            add_pct_row(_L("How much Color 2 (3-colour blend only):"),
                        m_grad_pct_b, m_sl_pct_b, m_lbl_pct_b);
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(m_panel_linear, wxID_ANY,
                            _L("Color 3 fills the rest (auto):")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                m_lbl_pct_c = new wxStaticText(m_panel_linear, wxID_ANY, "17%");
                row->Add(m_lbl_pct_c, 0, wxALIGN_CENTER_VERTICAL);
                pv->Add(row, 0, wxEXPAND | wxALL, 2);
            }
            // ── Color overlap slider (controls how much colours bleed) ──
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(m_panel_linear, wxID_ANY,
                            _L("Color overlap (soft ← hard zones):")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                const int init_ov_pct = static_cast<int>(std::lround(m_grad_overlap * 100.0));
                m_sl_overlap = new wxSlider(m_panel_linear, wxID_ANY,
                    init_ov_pct, 0, 100,
                    wxDefaultPosition, wxSize(180, -1), wxSL_HORIZONTAL);
                m_sl_overlap->SetToolTip(_L(
                    "0%   = hard zones (sharp bands)\n"
                    "60% = default — soft transitions, colours sprinkle into "
                    "neighbours\n"
                    "100% = strong overlap — every colour appears throughout"));
                m_lbl_overlap = new wxStaticText(m_panel_linear, wxID_ANY,
                    wxString::Format("%d%%", init_ov_pct));
                row->Add(m_sl_overlap, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                row->Add(m_lbl_overlap, 0, wxALIGN_CENTER_VERTICAL);
                pv->Add(row, 0, wxEXPAND | wxALL, 2);
            }
            {
                wxArrayString ease_labels;
                ease_labels.Add(_L("Even — same density everywhere"));
                ease_labels.Add(_L("Slow start"));
                ease_labels.Add(_L("Slow end"));
                ease_labels.Add(_L("S-curve (smooth start & end)"));
                ease_labels.Add(_L("Custom shape (set γ →)"));
                ease_labels.Add(_L("Hard step"));
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(m_panel_linear, wxID_ANY,
                            _L("Transition shape:")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                m_choice_easing = new wxChoice(m_panel_linear, wxID_ANY,
                    wxDefaultPosition, wxSize(180, -1), ease_labels);
                m_choice_easing->SetSelection(std::clamp(m_grad_easing, 0, 5));
                row->Add(m_choice_easing, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
                row->Add(new wxStaticText(m_panel_linear, wxID_ANY, _L("γ:")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                m_sc_gamma = new wxSpinCtrlDouble(m_panel_linear, wxID_ANY,
                    wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                    wxSP_ARROW_KEYS, 0.1, 10.0,
                    std::clamp(m_grad_gamma, 0.1, 10.0), 0.1);
                m_sc_gamma->SetDigits(2);
                m_sc_gamma->SetToolTip(_L(
                    "Only used when Transition shape = \"Custom shape\".\n"
                    "γ = 1   linear\n"
                    "γ > 1   delays the change toward the end\n"
                    "γ < 1   pushes the change toward the start"));
                row->Add(m_sc_gamma, 0, wxALIGN_CENTER_VERTICAL);
                pv->Add(row, 0, wxEXPAND | wxALL, 2);
            }
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(m_panel_linear, wxID_ANY,
                            _L("Skip tiny areas — fewer than N lines use Color 1 only:")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                m_sc_min_lines = new wxSpinCtrl(m_panel_linear, wxID_ANY,
                    wxString::Format("%d", m_grad_min_lines),
                    wxDefaultPosition, wxSize(80, -1),
                    wxSP_ARROW_KEYS, 0, 100, m_grad_min_lines);
                row->Add(m_sc_min_lines, 0, wxALIGN_CENTER_VERTICAL);
                pv->Add(row, 0, wxEXPAND | wxALL, 2);
            }
            // NEOTKO_COLORMIX_TAG — s90: ported from the legacy
            // SurfaceColorMixerDialog. Global "ColorMix min. line length" — skips
            // lines shorter than this value (they keep the region's default tool).
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(m_panel_linear, wxID_ANY,
                            _L("ColorStitch min. line length:")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                const double cur_ml = std::max(0.0, std::min(50.0, m_grad_min_length));
                m_sc_min_length = new wxSpinCtrlDouble(m_panel_linear, wxID_ANY,
                    wxEmptyString, wxDefaultPosition, wxSize(80, -1),
                    wxSP_ARROW_KEYS, 0.0, 50.0, cur_ml, 0.5);
                m_sc_min_length->SetDigits(1);
                m_sc_min_length->SetToolTip(
                    _L("ColorStitch skips lines shorter than this value — they keep the "
                       "region's default tool.\n"
                       "Higher values = fewer tool changes but more uncoloured gaps near edges.\n"
                       "0 = colour every line regardless of length.\n"
                       "Tip: if you see empty zones, lower this value.\n"
                       "(Global setting — applies to all ColorMix passes, both Top and Penu.)"));
                row->Add(m_sc_min_length, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                row->Add(new wxStaticText(m_panel_linear, wxID_ANY, _L("mm")),
                         0, wxALIGN_CENTER_VERTICAL);
                pv->Add(row, 0, wxEXPAND | wxALL, 2);
            }
            // ── Visual gradient preview ─────────────────────────────────
            {
                // Header row: label on the left, "Invert direction ⇆" toggle
                // on the right. Placing the invert toggle next to the preview
                // makes the cause/effect immediately visible — flip the box
                // and the strip flips with it.
                auto* hdr = new wxBoxSizer(wxHORIZONTAL);
                hdr->Add(new wxStaticText(m_panel_linear, wxID_ANY,
                            _L("Preview of the resulting gradient:")),
                         1, wxALIGN_CENTER_VERTICAL);
                m_chk_invert = new wxCheckBox(m_panel_linear, wxID_ANY,
                    _L("Invert direction ⇆"));
                m_chk_invert->SetValue(m_grad_invert);
                m_chk_invert->SetToolTip(_L(
                    "Reverses the gradient order. Useful when the lower layer's\n"
                    "natural fill direction makes the gradient look mirrored —\n"
                    "flipping this is faster than swapping Color 1 / Color 2."));
                hdr->Add(m_chk_invert, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
                pv->Add(hdr, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);

                m_strip = new GradientStripPanel(m_panel_linear, 360, 32);
                pv->Add(m_strip, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);

                m_lbl_lines_est = new wxStaticText(m_panel_linear, wxID_ANY,
                    _L("On a 60×60 mm surface — about: —"));
                m_lbl_lines_est->SetForegroundColour(wxColour(100, 100, 100));
                pv->Add(m_lbl_lines_est, 0, wxEXPAND | wxLEFT | wxRIGHT, 4);

                // The legacy ASCII preview is hidden by default — the strip
                // above is the new visual feedback. We keep the widget alive
                // (Tools that read it during refresh_grad_preview).
                m_lbl_preview = new wxStaticText(m_panel_linear, wxID_ANY, "");
                m_lbl_preview->Hide();
                pv->Add(m_lbl_preview, 0);
            }
            m_panel_linear->SetSizer(pv);
            m_gd_sb->Add(m_panel_linear, 0, wxEXPAND | wxALL, PAD / 2);
        }

        // ── Custom bands panel (mode 3) ────────────────────────────────
        m_panel_bands = new wxPanel(this);
        {
            auto* pv = new wxBoxSizer(wxVERTICAL);
            auto make_band_row = [&](const wxString& slot_lbl, int cur_tool,
                                     int init_count, wxSpinCtrl*& out_spin)
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(m_panel_bands, wxID_ANY,
                            wxString::Format("%s  T%d", slot_lbl, cur_tool)),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                row->Add(new ColorSwatch(m_panel_bands, colour_for_tool(cur_tool)),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                row->Add(new wxStaticText(m_panel_bands, wxID_ANY, _L("count:")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                out_spin = new wxSpinCtrl(m_panel_bands, wxID_ANY,
                    wxString::Format("%d", init_count),
                    wxDefaultPosition, wxSize(80, -1),
                    wxSP_ARROW_KEYS, 0, 200, init_count);
                row->Add(out_spin, 0, wxALIGN_CENTER_VERTICAL);
                pv->Add(row, 0, wxEXPAND | wxALL, 2);
            };
            make_band_row(_L("Color 1"), m_tool_a, m_grad_band_a, m_sc_band_a);
            make_band_row(_L("Color 2"), m_tool_b, m_grad_band_b, m_sc_band_b);
            make_band_row(_L("Color 3"), m_tool_c, m_grad_band_c, m_sc_band_c);
            make_band_row(_L("Color 4"), m_tool_d, m_grad_band_d, m_sc_band_d);
            auto* note = new wxStaticText(m_panel_bands, wxID_ANY,
                _L("Stripes repeat: [Color 1 × count, Color 2 × count, …] until\n"
                   "the surface is filled. Set count = 0 to skip a colour."));
            note->SetForegroundColour(wxColour(100, 100, 100));
            pv->Add(note, 0, wxEXPAND | wxALL, 2);
            m_panel_bands->SetSizer(pv);
            m_gd_sb->Add(m_panel_bands, 0, wxEXPAND | wxALL, PAD / 2);
        }

        // ── MixedFilament gray-out note (hidden by default) ────────────
        m_lbl_mf_lock_note = new wxStaticText(this, wxID_ANY,
            _L("⚠ Pattern uses a MixedFilament digit (5-9). Gradient options "
               "are disabled — the MixedFilament IS the pattern."));
        m_lbl_mf_lock_note->SetForegroundColour(wxColour(180, 100, 0));
        m_lbl_mf_lock_note->Hide();
        m_gd_sb->Add(m_lbl_mf_lock_note, 0, wxEXPAND | wxALL, 4);

        // ── Infill angle override ─────────────────────────────────────────
        // -1 = Auto (standard OrcaSlicer solid_infill_direction/rotate_template).
        // >= 0 = Force this fixed angle (degrees) on all ColorMix-active layers.
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(this, wxID_ANY, _L("Infill angle override:")),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            m_sc_angle = new wxSpinCtrl(this, wxID_ANY,
                wxString::Format("%d", m_grad_angle),
                wxDefaultPosition, wxSize(80, -1),
                wxSP_ARROW_KEYS, -1, 359, m_grad_angle);
            m_sc_angle->SetToolTip(_L(
                "-1 = Auto — use standard solid infill rotation settings.\n"
                "0 to 359 = Force this fixed angle in degrees on all layers\n"
                "where ColorMix is active, overriding solid_infill_direction\n"
                "and any rotation template."));
            row->Add(m_sc_angle, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            auto* lbl_auto = new wxStaticText(this, wxID_ANY, _L("(-1 = Auto)"));
            lbl_auto->SetForegroundColour(wxColour(120, 120, 120));
            row->Add(lbl_auto, 0, wxALIGN_CENTER_VERTICAL);
            m_gd_sb->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD / 2);
        }

        // ── Gradient repetitions ──────────────────────────────────────────
        // NEOTKO_COLORMIX_TAG — s80: repeat the whole gradient N times across
        // the surface (1 = single sweep). The surface is still analysed line by
        // line; the gradient is built over a 1/N slice and tiled N times.
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(this, wxID_ANY, _L("Gradient repetitions:")),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            m_sc_repetitions = new wxSpinCtrl(this, wxID_ANY,
                wxString::Format("%d", m_grad_repetitions),
                wxDefaultPosition, wxSize(80, -1),
                wxSP_ARROW_KEYS, 1, 50, std::max(1, m_grad_repetitions));
            m_sc_repetitions->SetToolTip(_L(
                "How many times the gradient repeats across the surface.\n"
                "1 = a single A→B sweep.\n"
                "N = the same gradient repeated N times back-to-back\n"
                "(e.g. a 1→2 gradient with 3 = three 1→2 gradients)."));
            row->Add(m_sc_repetitions, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            auto* lbl_one = new wxStaticText(this, wxID_ANY, _L("(1 = single)"));
            lbl_one->SetForegroundColour(wxColour(120, 120, 120));
            row->Add(lbl_one, 0, wxALIGN_CENTER_VERTICAL);
            m_gd_sb->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD / 2);
        }

        vs->Add(m_gd_sb, 0, wxEXPAND | wxALL, PAD / 2);

        // ── Listeners ──────────────────────────────────────────────────
        m_choice_grad_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            refresh_grad_visibility();
            refresh_grad_preview();
        });
        m_sl_pct_a->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { refresh_grad_preview(); });
        m_sl_pct_b->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { refresh_grad_preview(); });
        if (m_sl_overlap)
            m_sl_overlap->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { refresh_grad_preview(); });
        if (m_chk_invert)
            m_chk_invert->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { refresh_grad_preview(); });
        m_choice_easing->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { refresh_grad_preview(); });
        if (m_sc_repetitions)
            m_sc_repetitions->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { refresh_grad_preview(); });
        m_sc_gamma->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { refresh_grad_preview(); });
        m_sc_band_a->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { refresh_grad_preview(); });
        m_sc_band_b->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { refresh_grad_preview(); });
        m_sc_band_c->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { refresh_grad_preview(); });
        m_sc_band_d->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { refresh_grad_preview(); });

        refresh_grad_visibility();
        refresh_grad_preview();
        refresh_grad_lock();
    }

    void refresh_grad_visibility()
    {
        if (!m_choice_grad_mode) return;
        const int m = m_choice_grad_mode->GetSelection();
        const bool linear = (m == 1 || m == 2);
        const bool bands  = (m == 3);
        if (m_panel_linear) m_panel_linear->Show(linear);
        if (m_panel_bands)  m_panel_bands ->Show(bands);
        const bool b_active = (m == 2);
        if (m_sl_pct_b)  m_sl_pct_b ->Enable(b_active);
        if (m_lbl_pct_b) m_lbl_pct_b->Enable(b_active);
        if (m_lbl_pct_c) m_lbl_pct_c->Enable(b_active);
        Layout(); Fit();
    }

    void refresh_grad_preview()
    {
        if (!m_choice_grad_mode) return;
        const int mode    = m_choice_grad_mode->GetSelection();
        const int pct_a   = m_sl_pct_a ? m_sl_pct_a->GetValue() : m_grad_pct_a;
        const int pct_b   = m_sl_pct_b ? m_sl_pct_b->GetValue() : m_grad_pct_b;
        const int easing  = m_choice_easing ? m_choice_easing->GetSelection() : m_grad_easing;
        const double gamma= m_sc_gamma ? m_sc_gamma->GetValue() : m_grad_gamma;
        const double overlap = m_sl_overlap ? m_sl_overlap->GetValue() / 100.0 : m_grad_overlap;
        if (m_lbl_pct_a) m_lbl_pct_a->SetLabel(wxString::Format("%d%%", pct_a));
        if (m_lbl_pct_b) m_lbl_pct_b->SetLabel(wxString::Format("%d%%", pct_b));
        if (m_lbl_pct_c) {
            const int pct_c = std::max(0, 100 - pct_a - pct_b);
            m_lbl_pct_c->SetLabel(wxString::Format("%d%%", pct_c));
        }
        if (m_lbl_overlap) {
            const int ov_pct = m_sl_overlap ? m_sl_overlap->GetValue() : (int)std::lround(overlap * 100);
            m_lbl_overlap->SetLabel(wxString::Format("%d%%", ov_pct));
        }

        // Build the colour table for the strip preview. We map LOCAL slot
        // indices 0..3 to whatever physical tool the user has assigned via
        // the slot pickers, then look up the filament colour. This guarantees
        // the strip shows the actual print colours, not generic placeholders.
        auto pick_color = [&](int slot_idx) -> wxColour {
            int tool = 0;
            switch (slot_idx) {
                case 0: tool = m_choice_tool_a ? m_choice_tool_a->GetSelection() : m_tool_a; break;
                case 1: tool = m_choice_tool_b ? m_choice_tool_b->GetSelection() : m_tool_b; break;
                case 2: tool = m_choice_tool_c ? m_choice_tool_c->GetSelection() : m_tool_c; break;
                case 3: tool = m_choice_tool_d ? m_choice_tool_d->GetSelection() : m_tool_d; break;
            }
            if (tool >= 0 && tool < (int)m_colours.size())
                return hex_to_colour(m_colours[tool]);
            return wxColour(160, 160, 160);
        };
        std::vector<wxColour> slot_colors {
            pick_color(0), pick_color(1), pick_color(2), pick_color(3) };

        std::vector<int> seq;
        const int N = 240; // wider strip = more detail
        // NEOTKO_COLORMIX_TAG — s80: mirror the engine's repetitions tiling so
        // the preview shows the repeated gradients. Build over N/reps, tile to N.
        const int reps    = m_sc_repetitions ? std::max(1, m_sc_repetitions->GetValue())
                                             : std::max(1, m_grad_repetitions);
        const int build_N = (reps > 1) ? std::max(2, N / reps) : N;
        if (mode == 1) {
            seq = Slic3r::SurfaceColorMix::build_dithered_tools_2color(
                build_N, 0, 1, pct_a, easing, gamma);
        } else if (mode == 2) {
            seq = Slic3r::SurfaceColorMix::build_dithered_tools_3color(
                build_N, 0, 1, 2, pct_a, pct_b, easing, gamma, overlap);
        } else if (mode == 3) {
            const int ca = m_sc_band_a ? m_sc_band_a->GetValue() : 0;
            const int cb = m_sc_band_b ? m_sc_band_b->GetValue() : 0;
            const int cc = m_sc_band_c ? m_sc_band_c->GetValue() : 0;
            const int cd = m_sc_band_d ? m_sc_band_d->GetValue() : 0;
            seq = Slic3r::SurfaceColorMix::build_custom_bands(
                build_N, 0, ca, 1, cb, 2, cc, 3, cd);
        } else {
            // Pattern-string mode: render the actual pattern (if any) so the
            // user sees its real colour layout. Map each digit 1..4 to slot
            // 0..3 (digits 5-9 are MixedFilament — show as neutral grey).
            for (char c : m_pattern) {
                if (c >= '1' && c <= '4') seq.push_back(c - '1');
                else                       seq.push_back(-1); // unknown → grey
            }
            if (seq.empty()) seq.push_back(-1);
        }
        // NEOTKO_COLORMIX_TAG — s80: tile the built period to fill the strip
        // (modes 1-3 only; pattern-string mode is verbatim).
        if (reps > 1 && mode >= 1 && mode <= 3 && !seq.empty() && (int)seq.size() < N) {
            const int period = (int)seq.size();
            std::vector<int> tiled; tiled.reserve(N);
            for (int i = 0; i < N; ++i) tiled.push_back(seq[i % period]);
            seq.swap(tiled);
        }

        // s60: invert mirrors the sequence so the strip preview matches what
        // the slicer will produce when the checkbox is enabled.
        const bool invert = m_chk_invert ? m_chk_invert->GetValue() : m_grad_invert;
        if (invert && seq.size() > 1)
            std::reverse(seq.begin(), seq.end());
        if (m_strip) m_strip->set_sequence(seq, slot_colors);

        // Keep the hidden ASCII preview in sync for tools that read it.
        if (m_lbl_preview) {
            std::string preview;
            for (int t : seq)
                preview += (t < 0 ? '?' : static_cast<char>('1' + std::clamp(t, 0, 3)));
            m_lbl_preview->SetLabel(wxString::FromUTF8(preview));
        }

        if (m_lbl_lines_est && m_cfg) {
            double lw = 0.4;
            if (auto* o = m_cfg->option<ConfigOptionFloatOrPercent>("top_surface_line_width"))
                if (!o->percent) lw = std::max(0.05, o->value);
            const int est = Slic3r::SurfaceColorMix::estimate_surface_line_count(
                60.0 * 60.0, lw, 0.0, 1.0);
            m_lbl_lines_est->SetLabel(wxString::Format(
                _L("On a 60×60 mm surface — about %d lines  (filament width %.2f mm)"),
                est, lw));
        }
    }

    void refresh_grad_lock()
    {
        const bool locked = pattern_uses_mixed_filament() || m_mixed_filament_selected;
        if (m_panel_linear) m_panel_linear->Enable(!locked);
        if (m_panel_bands)  m_panel_bands ->Enable(!locked);
        if (m_choice_grad_mode) m_choice_grad_mode->Enable(!locked);
        if (m_choice_tool_a) m_choice_tool_a->Enable(!locked);
        if (m_choice_tool_b) m_choice_tool_b->Enable(!locked);
        if (m_choice_tool_c) m_choice_tool_c->Enable(!locked);
        if (m_choice_tool_d) m_choice_tool_d->Enable(!locked);
        if (m_lbl_mf_lock_note) {
            if (m_mixed_filament_selected) {
                m_lbl_mf_lock_note->SetLabel(_L("⚠ A MixedFilament recipe is active. Gradient options are disabled — the MixedFilament IS the pattern."));
            } else {
                m_lbl_mf_lock_note->SetLabel(_L("⚠ Pattern uses a MixedFilament digit (5-9). Gradient options are disabled — the MixedFilament IS the pattern."));
            }
            m_lbl_mf_lock_note->Show(locked);
            Layout(); Fit();
        }
        for (auto* btn : m_physical_buttons) {
            if (btn) btn->Enable(!m_mixed_filament_selected);
        }
        if (m_btn_invert) {
            m_btn_invert->Enable(m_mixed_filament_selected);
        }
    }
};

// NEOTKO_MULTIPASS_TAG_START - MultiPass Blend Config Dialog
// Grid dialog: num_passes, surface (checkboxes), per-pass tool+ratio+fan+speed+gcode, vary_pattern.
// Uses wxSpinCtrl (int) + wxTextCtrl (float/string) — wxSpinCtrlDouble crashes on macOS.
// No wxMemoryDC, no Line{""} — safe on Snapmaker fork.
// NEOTKO_MULTIPASS_TAG — ColorSwatch class moved above ColorMixPatternDialog
// (see top of namespace) so the gradient sub-section can use it.

// ── DragTextCtrl ─────────────────────────────────────────────────────────────
// wxTextCtrl with click+drag and scroll-wheel value editing.
// Drag horizontally to change value (3px dead-zone before mouse capture).
// Scroll wheel applies ±step*5 per tick. Cursor shows SIZEWE as affordance.
class DragTextCtrl : public wxTextCtrl
{
public:
    DragTextCtrl(wxWindow* parent, double init, double vmin, double vmax,
                 double step, const wxString& fmt = "%.3f", int w = 60)
        : wxTextCtrl(parent, wxID_ANY, wxString::Format(fmt, init),
                     wxDefaultPosition, wxSize(w, -1))
        , m_min(vmin), m_max(vmax), m_step(step), m_fmt(fmt)
    {
        SetCursor(wxCursor(wxCURSOR_SIZEWE));
        Bind(wxEVT_LEFT_DOWN,  &DragTextCtrl::on_down,  this);
        Bind(wxEVT_LEFT_UP,    &DragTextCtrl::on_up,    this);
        Bind(wxEVT_MOTION,     &DragTextCtrl::on_move,  this);
        Bind(wxEVT_MOUSEWHEEL, &DragTextCtrl::on_wheel, this);
    }

    double get_value() const {
        double v = m_min;
        GetValue().ToDouble(&v);
        return std::max(m_min, std::min(m_max, v));
    }
    void set_value(double v, bool notify = true) {
        v = std::max(m_min, std::min(m_max, v));
        ChangeValue(wxString::Format(m_fmt, v));
        if (notify) {
            wxCommandEvent evt(wxEVT_TEXT, GetId());
            evt.SetEventObject(this);
            GetEventHandler()->ProcessEvent(evt);
        }
    }

private:
    double   m_min, m_max, m_step;
    wxString m_fmt;
    bool     m_potential = false;
    bool     m_dragging  = false;
    int      m_start_x   = 0;
    double   m_start_v   = 0.0;

    void on_down(wxMouseEvent& e) {
        m_potential = true; m_dragging = false;
        m_start_x = e.GetX(); m_start_v = get_value();
        e.Skip();
    }
    void on_up(wxMouseEvent& e) {
        if (m_dragging && HasCapture()) ReleaseMouse();
        m_potential = m_dragging = false;
        e.Skip();
    }
    void on_move(wxMouseEvent& e) {
        if (!m_potential || !e.LeftIsDown()) { e.Skip(); return; }
        const int dx = e.GetX() - m_start_x;
        if (!m_dragging && std::abs(dx) > 3) { m_dragging = true; CaptureMouse(); }
        if (m_dragging) set_value(m_start_v + dx * m_step);
        e.Skip();
    }
    void on_wheel(wxMouseEvent& e) {
        set_value(get_value() + (e.GetWheelRotation() > 0 ? 1.0 : -1.0) * m_step * 5.0);
    }
};

// ── MultiPassPreviewPanel ─────────────────────────────────────────────────────
// Interactive visual preview of the MultiPass pass layout.
//   • N colored bands proportional to layer ratios (fraction of layer height per pass).
//     Bands are shown side-by-side for readability; physically passes are Z-stacked.
//   • Hatch lines at the configured angle inside each band (auto=45°).
//   • Drag a border between bands → adjusts the two adjacent ratios.
//   • Scroll wheel inside a band → cycles the fill angle by 5° steps.
//   • Σ ratio indicator (green/orange/red) in top-right corner.
//   • Callbacks: on_ratio_change(idx, new_ratio) / on_angle_change(idx, new_angle).
class MultiPassPreviewPanel : public wxPanel
{
public:
    struct PassData {
        int      tool  = 0;
        double   ratio = 0.5;
        int      angle = -1;           // -1 = auto (shown as 45°)
        wxColour color { 160, 160, 160 };
    };

    std::function<void(int, double)> on_ratio_change;
    std::function<void(int, int)>    on_angle_change;

    explicit MultiPassPreviewPanel(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 90))
    {
        SetMinSize(wxSize(260, 90));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT,        &MultiPassPreviewPanel::on_paint, this);
        Bind(wxEVT_LEFT_DOWN,    &MultiPassPreviewPanel::on_down,  this);
        Bind(wxEVT_LEFT_UP,      &MultiPassPreviewPanel::on_up,    this);
        Bind(wxEVT_MOTION,       &MultiPassPreviewPanel::on_move,  this);
        Bind(wxEVT_MOUSEWHEEL,   &MultiPassPreviewPanel::on_wheel, this);
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            if (m_hover != -1) { m_hover = -1; Refresh(); }
            SetCursor(wxCursor(wxCURSOR_ARROW));
        });
    }

    void set_data(int passes, const PassData pd[3]) {
        m_passes = std::max(1, std::min(3, passes));
        for (int i = 0; i < 3; ++i) m_pd[i] = pd[i];
        Refresh();
    }
    void update_ratio(int i, double v) { if (i>=0&&i<3){m_pd[i].ratio=v;Refresh();} }
    void update_angle(int i, int   v) { if (i>=0&&i<3){m_pd[i].angle=v;Refresh();} }
    void set_passes (int n)           { m_passes=std::max(1,std::min(3,n));Refresh(); }

private:
    int      m_passes   = 2;
    PassData m_pd[3];
    int      m_hover    = -1;
    int      m_drag_bdr = -1;
    int      m_drag_sx  = 0;
    double   m_drag_r0  = 0.0, m_drag_r1 = 0.0;

    int band_x(int i, int W) const {
        double tot = 0.0;
        for (int j = 0; j < m_passes; ++j) tot += m_pd[j].ratio;
        if (tot <= 0.0) tot = 1.0;
        double cum = 0.0;
        for (int j = 0; j < i; ++j) cum += m_pd[j].ratio;
        return (int)(cum / tot * W);
    }
    int hit_border(int mx, int W) const {
        for (int i = 1; i < m_passes; ++i)
            if (std::abs(mx - band_x(i, W)) <= 7) return i - 1;
        return -1;
    }
    int hit_band(int mx, int W) const {
        for (int i = 0; i < m_passes; ++i) {
            int x0 = band_x(i, W), x1 = (i == m_passes-1) ? W : band_x(i+1, W);
            if (mx >= x0 && mx < x1) return i;
        }
        return -1;
    }

    static void draw_hatch(wxDC& dc, int x0, int y0, int bw, int bh, int angle_deg) {
        if (bw <= 0 || bh <= 0) return;
        dc.SetClippingRegion(x0, y0, bw, bh);
        const double rad = angle_deg * 3.14159265358979323846 / 180.0;
        const double ca  = std::cos(rad), sa = std::sin(rad);
        const int sp = 10, cx = x0+bw/2, cy = y0+bh/2, reach = bw+bh+sp;
        for (int k = -(reach/sp+1); k <= (reach/sp+1); ++k) {
            int px = cx + (int)(-sa*k*sp), py = cy + (int)(ca*k*sp);
            dc.DrawLine(px-(int)(ca*reach), py-(int)(sa*reach),
                        px+(int)(ca*reach), py+(int)(sa*reach));
        }
        dc.DestroyClippingRegion();
    }

    void on_paint(wxPaintEvent&) {
        wxPaintDC dc(this);
        const int W = GetSize().x, H = GetSize().y;
        dc.SetBackground(wxBrush(wxColour(45, 45, 45)));
        dc.Clear();
        if (!m_passes) return;

        for (int i = 0; i < m_passes; ++i) {
            const PassData& p = m_pd[i];
            int x0 = band_x(i, W), x1 = (i == m_passes-1) ? W : band_x(i+1, W), bw = x1-x0;
            if (bw <= 0) continue;
            dc.SetBrush(wxBrush(p.color)); dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(x0, 0, bw, H);
            dc.SetPen(wxPen(p.color.ChangeLightness(50), 1));
            draw_hatch(dc, x0, 0, bw, H, p.angle >= 0 ? p.angle : 45);
            const int lum = p.color.Red()*299 + p.color.Green()*587 + p.color.Blue()*114;
            dc.SetTextForeground(lum < 128000 ? *wxWHITE : *wxBLACK);
            dc.SetBackgroundMode(wxTRANSPARENT);
            wxString lbl = wxString::Format("T%d  %.2f  %s", p.tool, p.ratio,
                p.angle >= 0 ? wxString::Format("%d\u00b0", p.angle) : wxString("auto"));
            wxSize ts = dc.GetTextExtent(lbl);
            dc.DrawText(lbl, x0 + std::max(2, (bw - ts.x)/2), std::max(0, (H-14-ts.y)/2));
        }
        // Inter-band draggable borders
        for (int i = 1; i < m_passes; ++i) {
            int bx = band_x(i, W); bool hi = (m_hover == i-1 || m_drag_bdr == i-1);
            dc.SetPen(wxPen(hi ? wxColour(255,255,255) : wxColour(30,30,30), hi ? 3 : 2));
            dc.DrawLine(bx, 0, bx, H-14);
        }
        // Sigma indicator
        double sum = 0.0; for (int i = 0; i < m_passes; ++i) sum += m_pd[i].ratio;
        double diff = std::abs(sum - 1.0);
        dc.SetTextForeground(diff<0.05 ? wxColour(0,200,80) : diff<0.15 ? wxColour(220,140,0) : wxColour(220,50,50));
        wxString ss = wxString::Format("\u03a3 %.2f", sum);
        wxSize ts2 = dc.GetTextExtent(ss);
        dc.DrawText(ss, W - ts2.x - 4, 4);
        // Hint bar
        dc.SetTextForeground(wxColour(150, 150, 150));
        wxFont sf = dc.GetFont();
        sf.SetPointSize(std::max(7, sf.GetPointSize() - 2));
        dc.SetFont(sf);
        dc.DrawText(_L("drag border \u2194 ratio   \u2022   scroll inside band \u2194 angle"), 4, H - 13);
    }

    void on_down(wxMouseEvent& e) {
        int mx = e.GetX(), W = GetSize().x;
        m_drag_bdr = hit_border(mx, W);
        if (m_drag_bdr >= 0 && m_drag_bdr < m_passes-1) {
            m_drag_sx = mx;
            m_drag_r0 = m_pd[m_drag_bdr].ratio;
            m_drag_r1 = m_pd[m_drag_bdr+1].ratio;
            CaptureMouse();
        }
        e.Skip();
    }
    void on_up(wxMouseEvent& e) {
        if (m_drag_bdr >= 0 && HasCapture()) ReleaseMouse();
        m_drag_bdr = -1; e.Skip();
    }
    void on_move(wxMouseEvent& e) {
        int mx = e.GetX(), W = GetSize().x;
        if (m_drag_bdr >= 0 && e.LeftIsDown()) {
            double tot = 0.0;
            for (int i = 0; i < m_passes; ++i) tot += m_pd[i].ratio;
            if (tot <= 0.0) tot = 1.0;
            double d = (double)(mx - m_drag_sx) / W * tot;
            double r0 = std::max(0.05, m_drag_r0 + d);
            double r1 = std::max(0.05, m_drag_r1 - d);
            m_pd[m_drag_bdr  ].ratio = r0;
            m_pd[m_drag_bdr+1].ratio = r1;
            if (on_ratio_change) { on_ratio_change(m_drag_bdr, r0); on_ratio_change(m_drag_bdr+1, r1); }
            Refresh();
        } else {
            int hb = hit_border(mx, W);
            if (hb != m_hover) {
                m_hover = hb;
                SetCursor(wxCursor(hb >= 0 ? wxCURSOR_SIZEWE : wxCURSOR_ARROW));
                Refresh();
            }
        }
        e.Skip();
    }
    void on_wheel(wxMouseEvent& e) {
        int mx = e.GetX(), W = GetSize().x;
        int band = hit_band(mx, W);
        if (band >= 0 && band < m_passes) {
            int cur = m_pd[band].angle;
            int d   = e.GetWheelRotation() > 0 ? 5 : -5;
            cur = cur < 0 ? 0 : (cur + d + 360) % 360;
            m_pd[band].angle = cur;
            if (on_angle_change) on_angle_change(band, cur);
            Refresh();
        }
        e.Skip();
    }
};

// NEOTKO_MULTIPASS_TAG_START — Beer-Lambert TD helpers
// Used by MultiPassConfigDialog (single-layer suggest) and
// SurfaceColorMixerDialog (joint Top+Penultimate suggest).
// All helpers are pure functions, no wxWidgets dependency.

struct MpLabColor { double L, a, b; };

static MpLabColor mp_rgb_to_lab(double r, double g, double b)
{
    auto lin = [](double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    const double rl = lin(r), gl = lin(g), bl = lin(b);
    const double X = rl*0.4124564 + gl*0.3575761 + bl*0.1804375;
    const double Y = rl*0.2126729 + gl*0.7151522 + bl*0.0721750;
    const double Z = rl*0.0193339 + gl*0.1191920 + bl*0.9503041;
    const double xn = X / 0.95047, yn = Y, zn = Z / 1.08883;
    auto f = [](double t) { return t > 0.008856 ? std::cbrt(t) : 7.787*t + 16.0/116.0; };
    return { 116.0*f(yn) - 16.0, 500.0*(f(xn) - f(yn)), 200.0*(f(yn) - f(zn)) };
}

static double mp_delta_e(const MpLabColor& a, const MpLabColor& b)
{
    return std::sqrt((a.L-b.L)*(a.L-b.L) + (a.a-b.a)*(a.a-b.a) + (a.b-b.b)*(a.b-b.b));
}

// Beer-Lambert stack. layers = bottom-to-top [{r,g,b} in sRGB [0..1], td, ratio].
// bg_r/g/b: background sRGB (black = 0,0,0 for single layer).
// Internally converts sRGB→linear before Beer-Lambert (physically correct),
// returns sRGB output so callers / mp_rgb_to_lab remain unchanged.
struct MpBLLayer { double r, g, b, td, ratio; };

static std::tuple<double,double,double> mp_beer_blend(
    const std::vector<MpBLLayer>& layers,
    double bg_r = 0.0, double bg_g = 0.0, double bg_b = 0.0)
{
    auto to_lin = [](double c) {
        return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    };
    auto to_srgb = [](double c) -> double {
        c = std::max(0.0, std::min(1.0, c));
        return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    };
    double r = to_lin(bg_r), g = to_lin(bg_g), bv = to_lin(bg_b);
    for (const auto& lyr : layers) {
        const double lr = to_lin(lyr.r), lg = to_lin(lyr.g), lb = to_lin(lyr.b);
        if (lyr.td < 1e-6) { r = lr; g = lg; bv = lb; continue; }
        const double t  = std::pow(0.1, lyr.ratio / lyr.td);
        const double op = 1.0 - t;
        r  = lr * op + r  * t;
        g  = lg * op + g  * t;
        bv = lb * op + bv * t;
    }
    return { to_srgb(r), to_srgb(g), to_srgb(bv) };
}

struct MpSuggestResult {
    std::vector<int>    order;    // permutation of [0..n-1], bottom→top
    std::vector<double> ratios;   // layer ratio per pass in that order
    double              score  = std::numeric_limits<double>::max();
    double              res_r  = 0.5, res_g = 0.5, res_b = 0.5; // simulated result color
};

// Exhaustive permutation + ratio grid search for a single layer (or joint two-layer mode).
// pass_colors: per-pass {r,g,b}; tds: per-pass TD [0..1].
// joint_two_layers: if true, simulates same passes on TWO stacked layers (Top+Penultimate mode):
//   bg_after_penu = beer_blend(passes, black)
//   result        = beer_blend(passes, bg=bg_after_penu)
// bg_r/g/b: only used when joint_two_layers==false (external background).
static MpSuggestResult mp_suggest(
    const std::vector<std::array<double,3>>& pass_colors,
    const std::vector<double>&               tds,
    double target_r, double target_g, double target_b,
    double min_ratio         = 0.05,
    bool   joint_two_layers  = false,
    double bg_r = 0.0, double bg_g = 0.0, double bg_b = 0.0)
{
    const int n = static_cast<int>(pass_colors.size());
    if (n == 0) return {};

    const MpLabColor tlab = mp_rgb_to_lab(target_r, target_g, target_b);
    static constexpr int STEPS = 16;

    std::vector<int> perm(n);
    std::iota(perm.begin(), perm.end(), 0);

    MpSuggestResult best;
    best.order.resize(n);  std::iota(best.order.begin(), best.order.end(), 0);
    best.ratios.assign(n, 1.0 / n);

    auto blend_result = [&](const std::vector<MpBLLayer>& stack)
        -> std::tuple<double,double,double>
    {
        if (joint_two_layers) {
            auto [pr,pg,pb] = mp_beer_blend(stack);
            return mp_beer_blend(stack, pr, pg, pb);
        }
        return mp_beer_blend(stack, bg_r, bg_g, bg_b);
    };

    do {
        // Build ordered layer descriptors for this permutation
        std::vector<MpBLLayer> tmpl(n);
        for (int i = 0; i < n; ++i)
            tmpl[i] = { pass_colors[perm[i]][0], pass_colors[perm[i]][1],
                        pass_colors[perm[i]][2], tds[perm[i]], 0.0 /*ratio filled below*/ };

        if (n == 1) {
            tmpl[0].ratio = 1.0;
            auto [r,g,bv] = blend_result(tmpl);
            const double sc = mp_delta_e(mp_rgb_to_lab(r,g,bv), tlab);
            if (sc < best.score) {
                best.score=sc; best.order=perm; best.ratios={1.0};
                best.res_r=r; best.res_g=g; best.res_b=bv;
            }
        } else if (n == 2) {
            const double mr = std::max(min_ratio, 0.01);
            for (int s = 0; s <= STEPS; ++s) {
                const double r0 = mr + (1.0 - 2.0*mr) * s / STEPS;
                const double r1 = 1.0 - r0;
                if (r1 < mr - 1e-9) continue;
                tmpl[0].ratio = r0; tmpl[1].ratio = r1;
                auto [r,g,bv] = blend_result(tmpl);
                const double sc = mp_delta_e(mp_rgb_to_lab(r,g,bv), tlab);
                if (sc < best.score) {
                    best.score=sc; best.order=perm; best.ratios={r0,r1};
                    best.res_r=r; best.res_g=g; best.res_b=bv;
                }
            }
        } else { // n == 3
            const double mr = std::max(min_ratio, 0.01);
            for (int s0 = 0; s0 <= STEPS; ++s0) {
                const double r0     = mr + (1.0 - 3.0*mr) * s0 / STEPS;
                const double r1_max = 1.0 - r0 - mr;
                if (r1_max < mr - 1e-9) continue;
                for (int s1 = 0; s1 <= STEPS; ++s1) {
                    const double r1 = mr + (r1_max - mr) * s1 / STEPS;
                    const double r2 = 1.0 - r0 - r1;
                    if (r2 < mr - 1e-9) continue;
                    tmpl[0].ratio=r0; tmpl[1].ratio=r1; tmpl[2].ratio=r2;
                    auto [r,g,bv] = blend_result(tmpl);
                    const double sc = mp_delta_e(mp_rgb_to_lab(r,g,bv), tlab);
                    if (sc < best.score) {
                        best.score=sc; best.order=perm; best.ratios={r0,r1,r2};
                        best.res_r=r; best.res_g=g; best.res_b=bv;
                    }
                }
            }
        }
    } while (std::next_permutation(perm.begin(), perm.end()));

    return best;
}
// NEOTKO_MULTIPASS_TAG_END

// NEOTKO_MULTIPASS_SURFACES_TAG — zone selector for per-zone dialogs
enum class SurfaceZone { TOP, PENULTIMATE };

class MultiPassConfigDialog : public wxDialog
{
public:
    MultiPassConfigDialog(wxWindow* parent,
                          SurfaceZone zone,
                          int    cur_passes,
                          int    cur_surface,
                          int    cur_tool1,    int    cur_tool2,    int    cur_tool3,
                          double cur_ratio1,   double cur_ratio2,   double cur_ratio3,
                          bool   cur_vary,
                          int    cur_angle1,   int    cur_angle2,   int    cur_angle3,
                          int    cur_fan1,     int    cur_fan2,     int    cur_fan3,
                          int    cur_spd1,     int    cur_spd2,     int    cur_spd3,
                          const std::string& cur_gs1, const std::string& cur_gs2, const std::string& cur_gs3,
                          const std::string& cur_ge1, const std::string& cur_ge2, const std::string& cur_ge3,
                          const std::vector<std::string>& filament_colours = {},
                          const std::vector<Slic3r::ColorMixOption>& mix_options = {},
                          int    cur_pa_mode  = 0,
                          double cur_pa_value = 0.1,
                          double cur_prime_volume = 0.0,
                          double cur_layer_height = 0.0,  // NEOTKO_MULTIPASS_MINLAYER_TAG
                          // NEOTKO_MULTIPASS_SURFACES_TAG — Penultimate Surface independent config
                          bool   penu_enabled = false,
                          int    penu_passes  = 2,
                          int    penu_t1 = 0,  int penu_t2 = 1,  int penu_t3 = -1,
                          double penu_r1 = 0.5, double penu_r2 = 0.5, double penu_r3 = 0.34,
                          int    penu_a1 = -1,  int penu_a2 = -1,  int penu_a3 = -1,
                          int    penu_f1 = -1,  int penu_f2 = -1,  int penu_f3 = -1,
                          int    penu_s1 = 100, int penu_s2 = 100, int penu_s3 = 100,
                          bool   penu_vary = false,
                          std::string penu_gs1 = {}, std::string penu_gs2 = {}, std::string penu_gs3 = {},
                          std::string penu_ge1 = {}, std::string penu_ge2 = {}, std::string penu_ge3 = {},
                          double penu_prime_vol = 0.0)
        : wxDialog(parent, wxID_ANY,
                   zone == SurfaceZone::TOP
                       ? _L("MultiPass \u2014 Top Surface")
                       : _L("MultiPass \u2014 Penultimate Surface"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_surface(cur_surface)
        , m_zone(zone)
        , m_colours(filament_colours)
        , m_mix_options(mix_options)
    {
        const int angles[3] = {cur_angle1, cur_angle2, cur_angle3};
        const int fans[3]   = {cur_fan1, cur_fan2, cur_fan3};
        const int spds[3]   = {cur_spd1, cur_spd2, cur_spd3};
        const std::string gs[3] = {cur_gs1, cur_gs2, cur_gs3};
        const std::string ge[3] = {cur_ge1, cur_ge2, cur_ge3};
        const int penu_tools[3]   = {penu_t1, penu_t2, penu_t3};
        const double penu_ratios[3] = {penu_r1, penu_r2, penu_r3};
        const int penu_angles[3]  = {penu_a1, penu_a2, penu_a3};
        const int penu_fans[3]    = {penu_f1, penu_f2, penu_f3};
        const int penu_speeds[3]  = {penu_s1, penu_s2, penu_s3};
        const std::string penu_gs[3] = {penu_gs1, penu_gs2, penu_gs3};
        const std::string penu_ge[3] = {penu_ge1, penu_ge2, penu_ge3};
        build_ui(cur_passes, cur_surface,
                 cur_tool1, cur_tool2, cur_tool3,
                 cur_ratio1, cur_ratio2, cur_ratio3,
                 cur_vary, angles, fans, spds, gs, ge,
                 cur_pa_mode, cur_pa_value, cur_prime_volume, cur_layer_height,
                 penu_enabled, penu_passes, penu_tools, penu_ratios, penu_angles,
                 penu_fans, penu_speeds, penu_vary, penu_gs, penu_ge, penu_prime_vol);
    }

    int    get_passes()  const { return m_sc_passes ? m_sc_passes->GetValue() : 2; }
    int    get_surface() const { return m_surface; }
    // Spinners display F-notation (F1=1, F2=2, …); config stores 0-based (T0=0, T1=1, …).
    // Pass 3: display 0 = disabled → stored -1.
    int    get_tool1()   const { return m_sc_tool[0] ? m_sc_tool[0]->GetValue() - 1 : 0; }
    int    get_tool2()   const { return m_sc_tool[1] ? m_sc_tool[1]->GetValue() - 1 : 1; }
    int    get_tool3()   const { if (!m_sc_tool[2]) return -1; const int v = m_sc_tool[2]->GetValue(); return v > 0 ? v - 1 : -1; }
    bool   get_vary()    const { return m_cb_vary ? m_cb_vary->GetValue() : false; }

    double get_ratio(int idx, double fallback) const {
        return m_tc_ratio[idx] ? m_tc_ratio[idx]->get_value() : fallback;
    }
    double get_ratio1()  const { return get_ratio(0, 0.50); }
    double get_ratio2()  const { return get_ratio(1, 0.50); }
    double get_ratio3()  const { return get_ratio(2, 0.34); }

    int get_angle(int idx) const {
        return m_sc_angle[idx] ? m_sc_angle[idx]->GetValue() : -1;
    }
    int get_angle1() const { return get_angle(0); }
    int get_angle2() const { return get_angle(1); }
    int get_angle3() const { return get_angle(2); }

    int get_fan(int idx) const {
        return m_sc_fan[idx] ? std::max(-1, std::min(255, m_sc_fan[idx]->GetValue())) : -1;
    }
    int get_fan1() const { return get_fan(0); }
    int get_fan2() const { return get_fan(1); }
    int get_fan3() const { return get_fan(2); }

    int get_speed(int idx) const {
        return m_sc_speed[idx] ? std::max(1, std::min(200, m_sc_speed[idx]->GetValue())) : 100;
    }
    int get_speed1() const { return get_speed(0); }
    int get_speed2() const { return get_speed(1); }
    int get_speed3() const { return get_speed(2); }

    std::string get_gcode_start(int idx) const {
        return m_tc_gstart[idx] ? m_tc_gstart[idx]->GetValue().ToStdString() : "";
    }
    std::string get_gcode_start1() const { return get_gcode_start(0); }
    std::string get_gcode_start2() const { return get_gcode_start(1); }
    std::string get_gcode_start3() const { return get_gcode_start(2); }

    std::string get_gcode_end(int idx) const {
        return m_tc_gend[idx] ? m_tc_gend[idx]->GetValue().ToStdString() : "";
    }
    std::string get_gcode_end1() const { return get_gcode_end(0); }
    std::string get_gcode_end2() const { return get_gcode_end(1); }
    std::string get_gcode_end3() const { return get_gcode_end(2); }

    int get_pa_mode() const {
        if (m_rb_pa[0] && m_rb_pa[0]->GetValue()) return 1;
        if (m_rb_pa[1] && m_rb_pa[1]->GetValue()) return 2;
        return 0;
    }
    double get_pa_value() const {
        double v = 0.1;
        if (m_tc_pa_val) m_tc_pa_val->GetValue().ToDouble(&v);
        return std::max(0.0, v);
    }

    // NEOTKO_MULTIPASS_PRIME_TAG
    double get_prime_volume() const {
        return m_sc_prime ? std::max(0.0, m_sc_prime->GetValue()) : 0.0;
    }

    // NEOTKO_MULTIPASS_SURFACES_TAG — Penultimate Surface getters
    bool   get_penu_enabled() const {
        if (m_zone == SurfaceZone::PENULTIMATE) return true;
        return m_cb_penu_enabled ? m_cb_penu_enabled->GetValue() : false;
    }
    int    get_penu_passes()  const { return m_sc_penu_passes  ? m_sc_penu_passes->GetValue()  : 2; }
    int    get_penu_tool(int i) const {
        if (!m_sc_penu_tool[i]) return (i < 2) ? i : -1;
        const int v = m_sc_penu_tool[i]->GetValue();
        return (i == 2 && v == 0) ? -1 : v - 1;
    }
    int    get_penu_tool1()  const { return get_penu_tool(0); }
    int    get_penu_tool2()  const { return get_penu_tool(1); }
    int    get_penu_tool3()  const { return get_penu_tool(2); }
    double get_penu_ratio(int i) const { return m_tc_penu_ratio[i] ? m_tc_penu_ratio[i]->get_value() : 0.5; }
    double get_penu_ratio1() const { return get_penu_ratio(0); }
    double get_penu_ratio2() const { return get_penu_ratio(1); }
    double get_penu_ratio3() const { return get_penu_ratio(2); }
    int    get_penu_angle(int i) const { return m_sc_penu_angle[i] ? m_sc_penu_angle[i]->GetValue() : -1; }
    int    get_penu_angle1() const { return get_penu_angle(0); }
    int    get_penu_angle2() const { return get_penu_angle(1); }
    int    get_penu_angle3() const { return get_penu_angle(2); }
    double get_penu_prime()  const { return m_sc_penu_prime ? std::max(0.0, m_sc_penu_prime->GetValue()) : 0.0; }
    int    get_penu_fan(int i)  const { return m_sc_penu_fan[i]   ? m_sc_penu_fan[i]->GetValue()   : -1; }
    int    get_penu_fan1()  const { return get_penu_fan(0); }
    int    get_penu_fan2()  const { return get_penu_fan(1); }
    int    get_penu_fan3()  const { return get_penu_fan(2); }
    int    get_penu_speed(int i) const { return m_sc_penu_speed[i] ? m_sc_penu_speed[i]->GetValue() : 100; }
    int    get_penu_speed1() const { return get_penu_speed(0); }
    int    get_penu_speed2() const { return get_penu_speed(1); }
    int    get_penu_speed3() const { return get_penu_speed(2); }
    bool   get_penu_vary()  const { return m_cb_penu_vary ? m_cb_penu_vary->GetValue() : false; }
    std::string get_penu_gcode_start(int i) const {
        return m_tc_penu_gstart[i] ? m_tc_penu_gstart[i]->GetValue().ToStdString() : "";
    }
    std::string get_penu_gcode_start1() const { return get_penu_gcode_start(0); }
    std::string get_penu_gcode_start2() const { return get_penu_gcode_start(1); }
    std::string get_penu_gcode_start3() const { return get_penu_gcode_start(2); }
    std::string get_penu_gcode_end(int i) const {
        return m_tc_penu_gend[i] ? m_tc_penu_gend[i]->GetValue().ToStdString() : "";
    }
    std::string get_penu_gcode_end1() const { return get_penu_gcode_end(0); }
    std::string get_penu_gcode_end2() const { return get_penu_gcode_end(1); }
    std::string get_penu_gcode_end3() const { return get_penu_gcode_end(2); }

private:
    int                      m_surface      = 0;
    SurfaceZone              m_zone         = SurfaceZone::TOP;
    std::vector<std::string> m_colours;
    wxSpinCtrl*   m_sc_passes    = nullptr;
    wxSpinCtrl*   m_sc_tool[3]   = {nullptr, nullptr, nullptr};
    ColorSwatch*  m_swatch[3]    = {nullptr, nullptr, nullptr};
    wxSpinCtrl*   m_sc_angle[3]  = {nullptr, nullptr, nullptr};
    DragTextCtrl* m_tc_ratio[3]  = {nullptr, nullptr, nullptr};
    wxSpinCtrl*   m_sc_fan[3]    = {nullptr, nullptr, nullptr};
    wxSpinCtrl*   m_sc_speed[3]  = {nullptr, nullptr, nullptr};
    wxTextCtrl*   m_tc_gstart[3] = {nullptr, nullptr, nullptr};
    wxTextCtrl*   m_tc_gend[3]   = {nullptr, nullptr, nullptr};
    wxCheckBox*           m_cb_vary    = nullptr;
    // NEOTKO_MULTIPASS_TAG_START — TD spinners (one per pass, synced with neotko_td_N in app_config)
    wxSpinCtrlDouble* m_sc_td[3]       = {nullptr, nullptr, nullptr};
    // NEOTKO_MULTIPASS_TAG_END
    wxStaticText*         m_lbl_sum    = nullptr;
    wxRadioButton*        m_rb_pa[3]   = {nullptr, nullptr, nullptr};
    wxTextCtrl*           m_tc_pa_val  = nullptr;
    wxSpinCtrlDouble*     m_sc_prime   = nullptr; // NEOTKO_MULTIPASS_PRIME_TAG
    double                m_layer_height = 0.0;   // NEOTKO_MULTIPASS_MINLAYER_TAG
    // NEOTKO_MULTIPASS_SURFACES_TAG — Penultimate Surface independent section
    wxCheckBox*           m_cb_penu_enabled  = nullptr;
    wxSpinCtrl*           m_sc_penu_passes   = nullptr;
    wxSpinCtrl*           m_sc_penu_tool[3]   = {nullptr, nullptr, nullptr};
    ColorSwatch*          m_swatch_penu[3]    = {nullptr, nullptr, nullptr};
    DragTextCtrl*         m_tc_penu_ratio[3]  = {nullptr, nullptr, nullptr};
    wxSpinCtrl*           m_sc_penu_angle[3]  = {nullptr, nullptr, nullptr};
    wxSpinCtrl*           m_sc_penu_fan[3]    = {nullptr, nullptr, nullptr};
    wxSpinCtrl*           m_sc_penu_speed[3]  = {nullptr, nullptr, nullptr};
    wxCheckBox*           m_cb_penu_vary      = nullptr;
    wxTextCtrl*           m_tc_penu_gstart[3] = {nullptr, nullptr, nullptr};
    wxTextCtrl*           m_tc_penu_gend[3]   = {nullptr, nullptr, nullptr};
    wxSpinCtrlDouble*     m_sc_penu_prime     = nullptr;
    std::vector<wxWindow*> m_penu_pass3_widgets;
    MultiPassPreviewPanel* m_preview   = nullptr;
    wxComboBox*           m_preset_combo  = nullptr;
    wxComboBox*           m_combo_mixed   = nullptr; // NEOTKO: MixedColor normalize picker
    wxPanel*              m_swatch_mixed  = nullptr; // colour of selected MixedColor
    std::vector<Slic3r::ColorMixOption>          m_mix_options;
    std::vector<std::pair<wxString, wxString>>   m_presets;
    std::vector<wxWindow*> m_pass3_widgets;

    void update_pass3_state(bool enable)
    {
        for (auto* w : m_pass3_widgets)
            if (w) w->Enable(enable);
    }

    void update_sum_display()
    {
        if (!m_lbl_sum || !m_sc_passes) return;
        const int passes = m_sc_passes->GetValue();
        double sum = 0.0;
        for (int i = 0; i < passes; ++i) {
            double v = 0.0;
            if (m_tc_ratio[i]) m_tc_ratio[i]->GetValue().ToDouble(&v);
            sum += v;
        }
        const double diff = std::abs(sum - 1.0);
        wxString label = wxString::Format("\u03a3 = %.3f", sum);
        if      (diff < 0.05) label += _L("  \u2713 good");
        else if (diff < 0.15) label += _L("  \u26a0 aim for 1.0");
        else                  label += _L("  \u2717 far from 1.0");
        m_lbl_sum->SetLabel(label);
        if      (diff < 0.05) m_lbl_sum->SetForegroundColour(wxColour( 30, 160,  30));
        else if (diff < 0.15) m_lbl_sum->SetForegroundColour(wxColour(200, 140,   0));
        else                  m_lbl_sum->SetForegroundColour(wxColour(200,  30,  30));
        m_lbl_sum->Refresh();
        Layout();
    }

    void update_preview() {
        if (!m_preview) return;
        const int passes = m_sc_passes ? m_sc_passes->GetValue() : 2;
        MultiPassPreviewPanel::PassData pd[3];
        for (int i = 0; i < 3; ++i) {
            const int fv = m_sc_tool[i] ? m_sc_tool[i]->GetValue() : i + 1; // F-notation
            pd[i].tool  = fv;
            pd[i].ratio = m_tc_ratio[i] ? m_tc_ratio[i]->get_value() : 0.5;
            pd[i].angle = m_sc_angle[i] ? m_sc_angle[i]->GetValue()  : -1;
            const int t = fv > 0 ? fv - 1 : -1; // 0-based for color lookup
            if (t >= 0 && t < (int)m_colours.size() && !m_colours[t].empty())
                pd[i].color = wxColour(m_colours[t]);
            else
                pd[i].color = wxColour(160, 160, 160);
        }
        m_preview->set_data(passes, pd);
    }

    static wxString preset_file_path() {
        return wxGetHomeDir() + wxFILE_SEP_PATH + ".orca_mp_presets.txt";
    }

    wxString collect_state() const {
        wxString d;
        auto ai = [&](const char* k, int    v){ d += (d.empty()?"":",")+wxString(k)+"="+wxString::Format("%d",v); };
        auto af = [&](const char* k, double v){ d += (d.empty()?"":",")+wxString(k)+"="+wxString::Format("%.4f",v); };
        ai("passes",  m_sc_passes ? m_sc_passes->GetValue() : 2);
        ai("surface", m_surface);
        for (int i = 0; i < 3; ++i) {
            // Store 0-based tool index (not raw spinner F-value) for format stability.
            // Pass 3: spinner 0 = disabled → store -1. Passes 1&2: spinner F→ store F-1.
            ai(wxString::Format("t%d", i).c_str(), m_sc_tool[i] ? (m_sc_tool[i]->GetValue() > 0 ? m_sc_tool[i]->GetValue() - 1 : -1) : (i < 2 ? 0 : -1));
            af(wxString::Format("r%d",   i).c_str(), m_tc_ratio[i] ? m_tc_ratio[i]->get_value() : 0.5);
            ai(wxString::Format("a%d",   i).c_str(), m_sc_angle[i] ? m_sc_angle[i]->GetValue()  : -1);
            ai(wxString::Format("fan%d", i).c_str(), m_sc_fan[i]   ? m_sc_fan[i]->GetValue()    : -1);
            ai(wxString::Format("spd%d", i).c_str(), m_sc_speed[i] ? m_sc_speed[i]->GetValue()  : 100);
        }
        ai("vary",    m_cb_vary ? (m_cb_vary->GetValue() ? 1 : 0) : 0);
        ai("pa_mode", get_pa_mode());
        af("pa_val",  get_pa_value());
        return d;
    }

    void apply_state(const wxString& data) {
        std::vector<wxString> pairs;
        wxString tok;
        for (size_t i = 0; i <= data.size(); ++i) {
            if (i == data.size() || data[i] == ',') { pairs.push_back(tok); tok.clear(); }
            else tok += data[i];
        }
        for (const wxString& pair : pairs) {
            int eq = pair.Find('=');
            if (eq < 0) continue;
            wxString key = pair.Left(eq), val = pair.Mid(eq+1);
            long lv = 0; double dv = 0.0;
            val.ToLong(&lv); val.ToDouble(&dv);
            if      (key=="passes" && m_sc_passes) m_sc_passes->SetValue(lv);
            else if (key=="vary"   && m_cb_vary)   m_cb_vary->SetValue(lv!=0);
            else if (key=="pa_mode") {
                if (m_rb_pa[0]&&m_rb_pa[1]&&m_rb_pa[2]) {
                    m_rb_pa[0]->SetValue(lv==1); m_rb_pa[1]->SetValue(lv==2); m_rb_pa[2]->SetValue(lv==0);
                }
            }
            else if (key=="pa_val" && m_tc_pa_val)
                m_tc_pa_val->ChangeValue(wxString::Format("%.3f", dv));
            else for (int i = 0; i < 3; ++i) {
                if (key==wxString::Format("t%d",i) && m_sc_tool[i]) {
                    // Stored as 0-based index (-1=disabled). Convert to F-notation for spinner.
                    // Clamp to spinner's [min,max] so invalid stored values don't OOB.
                    const int fv = (lv >= 0) ? static_cast<int>(lv) + 1 : 0;
                    m_sc_tool[i]->SetValue(std::clamp(fv, m_sc_tool[i]->GetMin(), m_sc_tool[i]->GetMax()));
                }
                else if (key==wxString::Format("r%d",i)   && m_tc_ratio[i]) m_tc_ratio[i]->set_value(dv,true);
                else if (key==wxString::Format("a%d",i)   && m_sc_angle[i]) m_sc_angle[i]->SetValue(lv);
                else if (key==wxString::Format("fan%d",i) && m_sc_fan[i])   m_sc_fan[i]->SetValue(lv);
                else if (key==wxString::Format("spd%d",i) && m_sc_speed[i]) m_sc_speed[i]->SetValue(lv);
            }
        }
        update_pass3_state(m_sc_passes ? m_sc_passes->GetValue() >= 3 : false);
        update_sum_display();
        update_preview();
    }

    void load_presets() {
        m_presets.clear();
        std::ifstream f(preset_file_path().ToStdString());
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0]=='#') continue;
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            wxString name = wxString::FromUTF8(line.substr(0, tab));
            wxString data = wxString::FromUTF8(line.substr(tab+1));
            m_presets.emplace_back(name, data);
        }
        if (m_preset_combo) {
            const wxString cur = m_preset_combo->GetValue();
            m_preset_combo->Clear();
            for (auto& [n, _] : m_presets) m_preset_combo->Append(n);
            m_preset_combo->SetValue(cur);
        }
    }

    void save_presets_file() {
        std::ofstream f(preset_file_path().ToStdString());
        f << "# OrcaSlicer MultiPass Presets\n";
        for (auto& [name, data] : m_presets)
            f << name.ToUTF8().data() << "\t" << data.ToUTF8().data() << "\n";
    }

    void build_ui(int cur_passes, int cur_surface,
                  int t1, int t2, int t3,
                  double r1, double r2, double r3,
                  bool cur_vary,
                  const int angles[3],
                  const int fans[3], const int spds[3],
                  const std::string gs[3], const std::string ge[3],
                  int cur_pa_mode = 0, double cur_pa_value = 0.1,
                  double cur_prime_volume = 0.0,
                  double cur_layer_height = 0.0,  // NEOTKO_MULTIPASS_MINLAYER_TAG
                  // NEOTKO_MULTIPASS_SURFACES_TAG — Penultimate Surface
                  bool penu_enabled = false, int penu_passes = 2,
                  const int penu_tools[3] = nullptr,
                  const double penu_ratios[3] = nullptr,
                  const int penu_angles[3] = nullptr,
                  const int penu_fans[3] = nullptr,
                  const int penu_speeds[3] = nullptr,
                  bool penu_vary = false,
                  const std::string penu_gs[3] = nullptr,
                  const std::string penu_ge[3] = nullptr,
                  double penu_prime_vol = 0.0)
    {
        m_layer_height = cur_layer_height; // NEOTKO_MULTIPASS_MINLAYER_TAG
        const int PAD = 6;
        auto* vs = new wxBoxSizer(wxVERTICAL);

        if (m_zone == SurfaceZone::TOP) {
        // ---- Row 1: Passes ----
        auto* row1 = new wxBoxSizer(wxHORIZONTAL);
        row1->Add(new wxStaticText(this, wxID_ANY, _L("Passes:")),
                  0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);
        const int safe_passes = (cur_passes >= 1 && cur_passes <= 3) ? cur_passes : 2;
        m_sc_passes = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                     wxDefaultPosition, wxSize(60,-1),
                                     wxSP_ARROW_KEYS, 1, 3, safe_passes);
        row1->Add(m_sc_passes, 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 16);
        vs->Add(row1, 0, wxALL, PAD);

        vs->Add(new wxStaticLine(this), 0, wxEXPAND|wxLEFT|wxRIGHT, PAD);

        // ---- LWTS override notice ----
        {
            auto* lbl_ov = new wxStaticText(this, wxID_ANY,
                _L("\u26a0 MultiPass prints each pass at full line width, stacked on the same XY path.\n"
                   "  Ratio = fraction of layer height. \u03a3 ratios \u2248 1.0 \u2192 solid layer."));
            lbl_ov->SetForegroundColour(wxColour(180, 80, 0));
            vs->Add(lbl_ov, 0, wxLEFT|wxRIGHT|wxBOTTOM, PAD);
        }

        // NEOTKO_PROFILE_TAG — Fase G cleanup: MP dialog's internal preset bar
        // (combo + Save/Delete writing ~/.orca_mp_presets.txt) is disabled.
        // The SurfaceEffectProfile system (Save as profile… below) is the
        // single source of truth for MP recipes. The old preset bar shipped
        // for early MP testing and would now confuse users with two parallel
        // "save MP" flows. Code kept under #if 0 for reference; the
        // collect_state/apply_state/load_presets/save_presets_file helpers
        // and m_preset_combo / m_presets members are now unused.
#if 0
        // ---- Preset bar (DISABLED — superseded by SurfaceEffectProfile) ----
        {
            auto* row_p = new wxBoxSizer(wxHORIZONTAL);
            row_p->Add(new wxStaticText(this, wxID_ANY, _L("Preset:")),
                       0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);
            m_preset_combo = new wxComboBox(this, wxID_ANY, wxEmptyString,
                                            wxDefaultPosition, wxSize(200,-1),
                                            0, nullptr, wxCB_DROPDOWN);
            m_preset_combo->SetToolTip(_L("Type a name and click Save, or select an existing preset to load it."));
            load_presets();
            row_p->Add(m_preset_combo, 1, wxALIGN_CENTER_VERTICAL|wxRIGHT, 6);

            auto* btn_save = new wxButton(this, wxID_ANY, _L("Save"),
                                          wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            btn_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                wxString name = m_preset_combo->GetValue().Trim();
                if (name.empty()) return;
                wxString data = collect_state();
                bool found = false;
                for (auto& [n, d] : m_presets) if (n == name) { d = data; found = true; break; }
                if (!found) m_presets.emplace_back(name, data);
                save_presets_file();
                load_presets();
                m_preset_combo->SetValue(name);
            });
            row_p->Add(btn_save, 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);

            auto* btn_del = new wxButton(this, wxID_ANY, _L("Delete"),
                                         wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            btn_del->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                wxString name = m_preset_combo->GetValue().Trim();
                m_presets.erase(std::remove_if(m_presets.begin(), m_presets.end(),
                    [&name](const std::pair<wxString,wxString>& p){ return p.first==name; }),
                    m_presets.end());
                save_presets_file();
                load_presets();
                m_preset_combo->SetValue(wxEmptyString);
            });
            row_p->Add(btn_del, 0, wxALIGN_CENTER_VERTICAL);

            vs->Add(row_p, 0, wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM, PAD);

            m_preset_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) {
                wxString sel = e.GetString();
                for (auto& [n, d] : m_presets) if (n == sel) { apply_state(d); break; }
            });
        }
#endif

        // ---- Interactive preview panel ----
        m_preview = new MultiPassPreviewPanel(this);
        vs->Add(m_preview, 0, wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM, PAD);

        // ---- Main grid ----
        auto* grid = new wxFlexGridSizer(6 /*cols*/, PAD, PAD*2);

        grid->Add(new wxStaticText(this, wxID_ANY, _L("Pass")),    0, wxALIGN_CENTER_VERTICAL);
        {
            auto* hdr = new wxStaticText(this, wxID_ANY, _L("Filament [F1\u2013FN]"));
            hdr->SetToolTip(_L("Filament number (1-based: F1 = T0, F2 = T1, \u2026). Matches MixedColor F-notation.\nPass 3: 0 = disabled."));
            grid->Add(hdr, 0, wxALIGN_CENTER_VERTICAL);
        }
        {
            auto* hdr = new wxStaticText(this, wxID_ANY, _L("Angle°\n(-1=auto)"));
            hdr->SetToolTip(_L("-1 = follow top surface fill angle.\n"
                               "0-359 = custom fill angle for this pass.\n"
                               "Different angles per pass = NeoSanding cross-hatch."));
            grid->Add(hdr, 0, wxALIGN_CENTER_VERTICAL);
        }
        grid->Add(new wxStaticText(this, wxID_ANY, _L("% of layer\n(height ratio)")), 0, wxALIGN_CENTER_VERTICAL);
        // NEOTKO_MULTIPASS_TAG — s58: Fan PWM / Speed % headers hidden (controls below also hidden).
        // Backend config keys preserved for preset compatibility.
        // grid->Add(new wxStaticText(this, wxID_ANY, _L("Fan PWM\n(-1=off)")),  0, wxALIGN_CENTER_VERTICAL);
        // grid->Add(new wxStaticText(this, wxID_ANY, _L("Speed %\n(100=off)")), 0, wxALIGN_CENTER_VERTICAL);

        const int    tools_def[3]  = {t1, t2, t3};
        const double ratios_def[3] = {r1, r2, r3};
        // F-notation: F1=T0, F2=T1 … FN=T(N-1).  Pass 3 uses 0 = "disabled".
        // tool_min: passes 1&2 require at least F1; pass 3 can be 0 (off).
        const int    tool_min[3]   = {1, 1, 0};
        const int    max_tool      = std::max(4, (int)m_colours.size());

        // tool_color always takes a 0-based physical index.
        auto tool_color = [this](int t) -> wxColour {
            if (t >= 0 && t < (int)m_colours.size() && !m_colours[t].empty())
                return wxColour(m_colours[t]);
            return wxColour(160, 160, 160);
        };

        for (int i = 0; i < 3; ++i) {
            auto* lbl = new wxStaticText(this, wxID_ANY,
                                          wxString::Format(_L("Pass %d:"), i+1));
            if (i == 2) m_pass3_widgets.push_back(lbl);
            grid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);

            // Convert stored 0-based tool index to F-notation (1-based display).
            // Pass 3: stored -1 (disabled) → display 0; stored N → display N+1.
            const int stored_t  = tools_def[i];
            const int display_t = (stored_t >= 0) ? (stored_t + 1) : 0;
            const int safe_tool = std::clamp(display_t, tool_min[i], max_tool);
            m_sc_tool[i] = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                           wxDefaultPosition, wxSize(50,-1),
                                           wxSP_ARROW_KEYS, tool_min[i], max_tool, safe_tool);
            // Swatch uses 0-based index → subtract 1 from F-notation; 0 → gray.
            m_swatch[i] = new ColorSwatch(this, tool_color(safe_tool > 0 ? safe_tool - 1 : -1));
            // NEOTKO_MULTIPASS_TAG_START — TD spinner: reads neotko_td_{tool+1} from app_config
            {
                double init_td = 0.5;
                if (stored_t >= 0) {
                    auto* ac = wxGetApp().app_config;
                    const std::string td_val = ac ? ac->get("neotko_td_" + std::to_string(stored_t + 1)) : "";
                    try { if (!td_val.empty()) init_td = std::stod(td_val); } catch (...) {}
                    init_td = std::clamp(init_td, 0.01, 10.0);
                }
                m_sc_td[i] = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                                                   wxDefaultPosition, wxSize(62,-1),
                                                   wxSP_ARROW_KEYS, 0.01, 10.0, init_td, 0.05);
                m_sc_td[i]->SetDigits(2);
                m_sc_td[i]->SetToolTip(_L("TD (Tinting Density) for this pass's filament.\n"
                                           "0.1=highly opaque, 0.5-3=semi-opaque, 3-7=translucent, 7-10+=highly translucent.\n"
                                           "Lower TD = fewer passes needed to cover underlying color.\n"
                                           "Synced with the Beer-Lambert TD sliders in Surface Color Mixer.\n"
                                           "Used by 'Suggest (TD + %)' to compute optimal pass order and ratios."));
                m_sc_td[i]->Bind(wxEVT_TEXT, [this, i](wxCommandEvent&) {
                    if (!m_sc_tool[i] || !m_sc_td[i]) return;
                    const int fv = m_sc_tool[i]->GetValue();
                    if (fv <= 0) return;
                    auto* ac = wxGetApp().app_config;
                    if (ac) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%.3f", m_sc_td[i]->GetValue());
                        ac->set("neotko_td_" + std::to_string(fv), buf);
                    }
                });
            }
            // NEOTKO_MULTIPASS_TAG_END
            auto* tool_cell = new wxBoxSizer(wxHORIZONTAL);
            tool_cell->Add(m_sc_tool[i], 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 3);
            tool_cell->Add(m_swatch[i],  0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);
            tool_cell->Add(new wxStaticText(this, wxID_ANY, _L("TD:")), 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 2);
            tool_cell->Add(m_sc_td[i],   0, wxALIGN_CENTER_VERTICAL);
            if (i == 2) {
                m_pass3_widgets.push_back(m_sc_tool[i]);
                m_pass3_widgets.push_back(m_swatch[i]);
                m_pass3_widgets.push_back(m_sc_td[i]);
            }
            grid->Add(tool_cell, 0, wxALIGN_CENTER_VERTICAL);
            m_sc_tool[i]->Bind(wxEVT_SPINCTRL, [this, i, tool_color](wxSpinEvent& e) {
                const int fv = e.GetValue(); // F-notation (1-based); 0 = disabled (pass 3 only)
                if (m_swatch[i]) m_swatch[i]->set_color(tool_color(fv > 0 ? fv - 1 : -1));
                // NEOTKO_MULTIPASS_TAG_START — reload TD for newly selected tool
                if (m_sc_td[i] && fv > 0) {
                    auto* ac = wxGetApp().app_config;
                    const std::string td_val = ac ? ac->get("neotko_td_" + std::to_string(fv)) : "";
                    double v = 0.5;
                    try { if (!td_val.empty()) v = std::stod(td_val); } catch (...) {}
                    m_sc_td[i]->SetValue(std::clamp(v, 0.01, 10.0));
                }
                // NEOTKO_MULTIPASS_TAG_END
            });

            const int safe_angle = (angles[i] >= -1 && angles[i] <= 359) ? angles[i] : -1;
            m_sc_angle[i] = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                            wxDefaultPosition, wxSize(60,-1),
                                            wxSP_ARROW_KEYS, -1, 359, safe_angle);
            m_sc_angle[i]->SetToolTip(_L("-1 = auto (follow fill angle).\n"
                                          "0-359 = custom fill angle for NeoSanding."));
            if (i == 2) m_pass3_widgets.push_back(m_sc_angle[i]);
            grid->Add(m_sc_angle[i], 0, wxALIGN_CENTER_VERTICAL);

            const double safe_ratio = (ratios_def[i] > 0.0 && ratios_def[i] < 10.0)
                                      ? ratios_def[i] : (i == 2 ? 0.34 : 0.50);
            // NEOTKO_MULTIPASS_MINLAYER_TAG — minimum ratio so each pass height ≥ 0.04mm.
            const double min_r = (m_layer_height > 1e-6)
                ? std::max(0.05, 0.04 / m_layer_height)
                : 0.05;
            m_tc_ratio[i] = new DragTextCtrl(this, std::max(safe_ratio, min_r), min_r, 5.0, 0.01);
            if (i == 2) m_pass3_widgets.push_back(m_tc_ratio[i]);
            grid->Add(m_tc_ratio[i], 0, wxALIGN_CENTER_VERTICAL);

            // NEOTKO_MULTIPASS_TAG — s58: Fan / Speed widgets hidden (not added to grid).
            // The wxSpinCtrl members stay nullptr; read/write code at lines ~973/1013/1507/1572
            // already handles nullptr via the `m_sc_fan[i] ? ... : -1` guards.
            m_sc_fan[i]   = nullptr;
            m_sc_speed[i] = nullptr;
            (void)fans; (void)spds;  // silence unused-param warnings
        }
        vs->Add(grid, 0, wxALL, PAD);

        update_pass3_state(safe_passes >= 3);
        m_sc_passes->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent& e) {
            update_pass3_state(e.GetValue() >= 3);
            update_sum_display();
            if (m_preview) m_preview->set_passes(e.GetValue());
            update_preview();
        });
        for (int i = 0; i < 3; ++i) {
            if (m_tc_ratio[i])
                m_tc_ratio[i]->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
                    update_sum_display();
                    update_preview();
                });
            if (m_sc_tool[i])
                m_sc_tool[i]->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { update_preview(); });
            if (m_sc_angle[i])
                m_sc_angle[i]->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) { update_preview(); });
        }
        if (m_preview) {
            m_preview->on_ratio_change = [this](int idx, double v) {
                if (idx >= 0 && idx < 3 && m_tc_ratio[idx]) {
                    m_tc_ratio[idx]->set_value(v, false);
                    update_sum_display();
                }
            };
            m_preview->on_angle_change = [this](int idx, int v) {
                if (idx >= 0 && idx < 3 && m_sc_angle[idx])
                    m_sc_angle[idx]->SetValue(v);
            };
        }
        update_preview();

        // ---- Flow sum display + auto-fill button ----
        {
            auto* row_sum = new wxBoxSizer(wxHORIZONTAL);
            row_sum->Add(new wxStaticText(this, wxID_ANY, _L("Normalize to layer height:")),
                         0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 6);
            m_lbl_sum = new wxStaticText(this, wxID_ANY, "\u03a3 = ?");
            row_sum->Add(m_lbl_sum, 1, wxALIGN_CENTER_VERTICAL|wxRIGHT, 12);
            auto* btn_eq = new wxButton(this, wxID_ANY, _L("Equal (1/N)"),
                                        wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            btn_eq->SetToolTip(_L("Set all active pass ratios to 1/N (equal split, \u03a3 = 1.0 exactly)."));
            btn_eq->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                const int passes = m_sc_passes ? m_sc_passes->GetValue() : 2;
                const double r = 1.0 / std::max(1, passes);
                for (int i = 0; i < passes; ++i)
                    if (m_tc_ratio[i])
                        m_tc_ratio[i]->SetValue(wxString::Format("%.3f", r));
                update_sum_display();
            });
            row_sum->Add(btn_eq, 0, wxALIGN_CENTER_VERTICAL);
            vs->Add(row_sum, 0, wxLEFT|wxRIGHT|wxTOP, PAD);
        }
        // NEOTKO_MULTIPASS_TAG_START — "Normalize to MixedColor %" row
        if (!m_mix_options.empty()) {
            auto* row_mc = new wxBoxSizer(wxHORIZONTAL);
            row_mc->Add(new wxStaticText(this, wxID_ANY, _L("From MixedColor:")),
                        0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 6);
            wxArrayString combo_labels;
            for (auto& opt : m_mix_options)
                combo_labels.Add(wxString::FromUTF8(opt.label));
            m_combo_mixed = new wxComboBox(this, wxID_ANY, combo_labels.empty() ? wxString() : combo_labels[0],
                                           wxDefaultPosition, wxSize(160, -1),
                                           combo_labels, wxCB_READONLY);
            m_combo_mixed->SetToolTip(_L("Select a virtual MixedColor filament to read its blend recipe."));
            row_mc->Add(m_combo_mixed, 1, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);

            // Colour swatch for the selected MixedColor
            m_swatch_mixed = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(18, 18));
            m_swatch_mixed->SetToolTip(_L("Display colour of the selected MixedColor."));
            auto update_swatch_mixed = [this]() {
                if (!m_swatch_mixed) return;
                const int sel = m_combo_mixed ? m_combo_mixed->GetSelection() : -1;
                wxColour c(128,128,128);
                if (sel >= 0 && sel < (int)m_mix_options.size()) {
                    const std::string& dc = m_mix_options[sel].display_color;
                    if (dc.size() >= 7 && dc[0] == '#') {
                        unsigned long rgb = 0;
                        if (wxString::FromUTF8(dc.substr(1)).ToULong(&rgb, 16))
                            c = wxColour((rgb>>16)&0xFF, (rgb>>8)&0xFF, rgb&0xFF);
                    }
                }
                m_swatch_mixed->SetBackgroundColour(c);
                m_swatch_mixed->Refresh();
            };
            update_swatch_mixed();
            m_combo_mixed->Bind(wxEVT_COMBOBOX, [update_swatch_mixed](wxCommandEvent&) {
                update_swatch_mixed();
            });
            row_mc->Add(m_swatch_mixed, 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 6);

            auto* btn_mc = new wxButton(this, wxID_ANY, _L("Normalize %"),
                                        wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            btn_mc->SetToolTip(_L("Apply the selected MixedColor recipe proportions as layer_ratio for each pass.\n"
                                  "Pass order (and tools) are kept as you set them; only the ratios are updated."));
            btn_mc->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                if (!m_combo_mixed) return;
                const int sel = m_combo_mixed->GetSelection();
                if (sel < 0 || sel >= (int)m_mix_options.size()) return;
                const auto& opt = m_mix_options[sel];
                if (opt.tool_weights.empty()) return;

                const int passes = m_sc_passes ? m_sc_passes->GetValue() : 2;

                // For each active pass, look up its tool in the recipe weights.
                // If the tool is not in the recipe, weight = 0 (pass gets 0 flow).
                std::vector<double> raw(passes, 0.0);
                for (int i = 0; i < passes; ++i) {
                    if (!m_sc_tool[i]) continue;
                    const int tool_0based = m_sc_tool[i]->GetValue() - 1; // spinner is 1-based
                    auto it = opt.tool_weights.find(tool_0based);
                    if (it != opt.tool_weights.end())
                        raw[i] = static_cast<double>(it->second);
                }

                // Normalize so Σ = 1.0
                double total = 0.0;
                for (double v : raw) total += v;
                if (total < 1e-9) {
                    wxMessageBox(_L("None of the current pass tools appear in the selected recipe.\n"
                                    "Assign tools that match the recipe first."),
                                 _L("Normalize — no match"), wxOK | wxICON_INFORMATION, this);
                    return;
                }
                for (double& v : raw) v /= total;

                // Push values into the ratio text controls
                for (int i = 0; i < passes; ++i)
                    if (m_tc_ratio[i])
                        m_tc_ratio[i]->SetValue(wxString::Format("%.3f", raw[i]));
                update_sum_display();
            });
            row_mc->Add(btn_mc, 0, wxALIGN_CENTER_VERTICAL);
            vs->Add(row_mc, 0, wxLEFT|wxRIGHT|wxTOP, PAD);
        }
        // NEOTKO_MULTIPASS_TAG_END
        // NEOTKO_MULTIPASS_TAG_START — "Suggest (TD + %)" Beer-Lambert optimizer
        {
            auto* row_td = new wxBoxSizer(wxHORIZONTAL);
            auto* btn_suggest = new wxButton(this, wxID_ANY, _L("Suggest (TD + %)"),
                                             wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            btn_suggest->SetToolTip(_L(
                "Uses Beer-Lambert optics to find the optimal pass ORDER and layer RATIOS.\n"
                "If a MixedColor is selected above: simulates the blend and minimises ΔE vs. that target colour.\n"
                "Otherwise: sorts passes by TD ascending (most opaque first = pass 1 base coat).\n"
                "TD values are read from the spinners next to each filament selector.\n"
                "Also reorders angles, fan, speed and GCode entries to match the new order."));

            // Result swatch + ΔE label (updated after each calculation)
            auto* swatch_result = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(22,22));
            swatch_result->SetBackgroundColour(wxColour(128,128,128));
            swatch_result->SetToolTip(_L("Simulated result colour after Beer-Lambert blend."));
            auto* lbl_de = new wxStaticText(this, wxID_ANY, _L("  ΔE: ---"));

            btn_suggest->Bind(wxEVT_BUTTON, [this, swatch_result, lbl_de](wxCommandEvent&) {
                const int passes = m_sc_passes ? m_sc_passes->GetValue() : 2;
                if (passes < 2) return;

                // NEOTKO_MULTIPASS_TAG_START — auto-populate tools from MixedColor recipe
                if (m_combo_mixed) {
                    const int sel = m_combo_mixed->GetSelection();
                    if (sel >= 0 && sel < (int)m_mix_options.size()) {
                        const auto& tw = m_mix_options[sel].tool_weights;
                        if (!tw.empty()) {
                            // Sort tools by weight descending: dominant first
                            std::vector<std::pair<int,float>> sorted_tw(tw.begin(), tw.end());
                            std::stable_sort(sorted_tw.begin(), sorted_tw.end(),
                                [](const auto& a, const auto& b){ return a.second > b.second; });
                            // Pattern: 2-pass→[dom,sec], 3-pass→[dom,sec,dom]
                            const int t_dom = sorted_tw[0].first;
                            const int t_sec = sorted_tw.size() > 1 ? sorted_tw[1].first : t_dom;
                            std::vector<int> pattern;
                            if (passes == 2)      pattern = {t_dom, t_sec};
                            else if (passes == 3) pattern = {t_dom, t_sec, t_dom};
                            else {
                                pattern.resize(passes);
                                for (int i = 0; i < passes; ++i)
                                    pattern[i] = (i % 2 == 0) ? t_dom : t_sec;
                            }
                            auto* ac = wxGetApp().app_config;
                            for (int i = 0; i < passes; ++i) {
                                const int fv = pattern[i] + 1; // 1-based F-notation
                                if (m_sc_tool[i]) {
                                    m_sc_tool[i]->SetValue(fv);
                                    if (m_swatch[i])
                                        m_swatch[i]->set_color(
                                            (pattern[i] >= 0 && pattern[i] < (int)m_colours.size()
                                             && !m_colours[pattern[i]].empty())
                                            ? wxColour(m_colours[pattern[i]])
                                            : wxColour(128,128,128));
                                }
                                if (m_sc_td[i] && fv > 0) {
                                    const std::string td_key = "neotko_td_" + std::to_string(fv);
                                    const std::string td_val = ac ? ac->get(td_key) : "";
                                    double v = 0.5;
                                    try { if (!td_val.empty()) v = std::stod(td_val); } catch (...) {}
                                    m_sc_td[i]->SetValue(std::clamp(v, 0.01, 10.0));
                                }
                            }
                        }
                    }
                }
                // NEOTKO_MULTIPASS_TAG_END

                struct PassState {
                    int tool_0; double ratio, td; int angle, fan, speed;
                    std::string gs, ge;
                };
                std::vector<PassState> states(passes);
                for (int i = 0; i < passes; ++i) {
                    states[i].tool_0 = m_sc_tool[i]  ? m_sc_tool[i]->GetValue() - 1 : i;
                    states[i].ratio  = m_tc_ratio[i]  ? m_tc_ratio[i]->get_value()  : 0.5;
                    states[i].td     = m_sc_td[i]     ? m_sc_td[i]->GetValue()      : 0.5;
                    states[i].angle  = m_sc_angle[i]  ? m_sc_angle[i]->GetValue()   : -1;
                    states[i].fan    = m_sc_fan[i]    ? m_sc_fan[i]->GetValue()     : -1;
                    states[i].speed  = m_sc_speed[i]  ? m_sc_speed[i]->GetValue()   : 100;
                    states[i].gs     = m_tc_gstart[i] ? m_tc_gstart[i]->GetValue().ToStdString() : "";
                    states[i].ge     = m_tc_gend[i]   ? m_tc_gend[i]->GetValue().ToStdString()   : "";
                }

                // Build per-pass color + TD arrays
                std::vector<std::array<double,3>> colors(passes);
                std::vector<double> tds(passes);
                for (int i = 0; i < passes; ++i) {
                    const int t = states[i].tool_0;
                    wxColour wc = (t >= 0 && t < (int)m_colours.size() && !m_colours[t].empty())
                                  ? wxColour(m_colours[t]) : wxColour(128,128,128);
                    colors[i] = {wc.Red()/255.0, wc.Green()/255.0, wc.Blue()/255.0};
                    tds[i]    = std::max(0.01, states[i].td);
                }

                MpSuggestResult res;
                bool used_bl = false;

                // Try Beer-Lambert with MixedColor target
                if (m_combo_mixed) {
                    const int sel = m_combo_mixed->GetSelection();
                    if (sel >= 0 && sel < (int)m_mix_options.size()) {
                        const std::string& dc = m_mix_options[sel].display_color;
                        if (dc.size() >= 7 && dc[0] == '#') {
                            unsigned long rgb = 0;
                            if (wxString::FromUTF8(dc.substr(1)).ToULong(&rgb, 16)) {
                                res = mp_suggest(colors, tds,
                                                 ((rgb>>16)&0xFF)/255.0,
                                                 ((rgb>> 8)&0xFF)/255.0,
                                                 ( rgb     &0xFF)/255.0,
                                                 0.05, false);
                                used_bl = true;
                            }
                        }
                    }
                }

                if (!used_bl) {
                    // Simple TD-ascending sort (most opaque → pass 1)
                    res.order.resize(passes); std::iota(res.order.begin(), res.order.end(), 0);
                    std::stable_sort(res.order.begin(), res.order.end(),
                        [&states](int a, int b){ return states[a].td < states[b].td; });
                    res.ratios.assign(passes, 1.0 / passes);
                    // Compute simulated result with these ratios
                    std::vector<MpBLLayer> stack;
                    for (int idx : res.order)
                        stack.push_back({colors[idx][0], colors[idx][1], colors[idx][2],
                                         tds[idx], res.ratios[0]});
                    auto [r,g,bv] = mp_beer_blend(stack);
                    res.res_r=r; res.res_g=g; res.res_b=bv;
                    res.score = 999.0;
                }

                // Apply reordering to all spinners
                std::vector<PassState> reordered;
                reordered.reserve(passes);
                for (int idx : res.order) reordered.push_back(states[idx]);

                for (int i = 0; i < passes; ++i) {
                    const auto& s = reordered[i];
                    if (m_sc_tool[i])  m_sc_tool[i]->SetValue(s.tool_0 + 1);
                    if (m_tc_ratio[i]) m_tc_ratio[i]->set_value(res.ratios[i], true);
                    if (m_sc_angle[i]) m_sc_angle[i]->SetValue(s.angle);
                    if (m_sc_fan[i])   m_sc_fan[i]->SetValue(s.fan);
                    if (m_sc_speed[i]) m_sc_speed[i]->SetValue(s.speed);
                    if (m_sc_td[i])    m_sc_td[i]->SetValue(s.td);
                    if (m_tc_gstart[i]) m_tc_gstart[i]->SetValue(wxString::FromUTF8(s.gs));
                    if (m_tc_gend[i])   m_tc_gend[i]->SetValue(wxString::FromUTF8(s.ge));
                    const int t = s.tool_0;
                    if (m_swatch[i]) {
                        m_swatch[i]->set_color((t >= 0 && t < (int)m_colours.size() && !m_colours[t].empty())
                                               ? wxColour(m_colours[t]) : wxColour(128,128,128));
                    }
                }
                update_sum_display();
                update_preview();

                // Update inline result display
                const wxColour rc(static_cast<unsigned char>(std::clamp(res.res_r*255,0.0,255.0)),
                                  static_cast<unsigned char>(std::clamp(res.res_g*255,0.0,255.0)),
                                  static_cast<unsigned char>(std::clamp(res.res_b*255,0.0,255.0)));
                swatch_result->SetBackgroundColour(rc);
                swatch_result->Refresh();
                const wxString de_str = used_bl
                    ? wxString::Format(_L("  ΔE: %.1f"), res.score)
                    : _L("  (sorted by TD)");
                lbl_de->SetLabel(de_str);
                if (used_bl)
                    lbl_de->SetForegroundColour(res.score < 5.0  ? wxColour(30,140,30) :
                                                res.score < 10.0 ? wxColour(190,130,0) :
                                                                   wxColour(180,40,40));
                else
                    lbl_de->SetForegroundColour(wxColour(80,80,80));
                lbl_de->Refresh();
                Layout();
            });

            row_td->Add(btn_suggest, 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 6);
            row_td->Add(swatch_result, 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);
            row_td->Add(lbl_de, 1, wxALIGN_CENTER_VERTICAL);
            vs->Add(row_td, 0, wxLEFT|wxRIGHT|wxTOP, PAD);
        }
        // NEOTKO_MULTIPASS_TAG_END
        {
            auto* hint = new wxStaticText(this, wxID_ANY,
                _L("\u03a3 \u2248 1.0 = full layer height covered. Each pass occupies its ratio of the layer height."));
            hint->SetForegroundColour(wxColour(100, 100, 100));
            vs->Add(hint, 0, wxLEFT|wxRIGHT|wxBOTTOM, PAD);
        }
        update_sum_display();

        // NEOTKO_MULTIPASS_TAG — s58: Custom GCode per-pass section hidden.
        // Members m_tc_gstart / m_tc_gend stay nullptr; read/write at lines
        // ~990/1019/1509/1575 already guards with `m_tc_gstart[i] ? ... : ""`.
        for (int i = 0; i < 3; ++i) {
            m_tc_gstart[i] = nullptr;
            m_tc_gend[i]   = nullptr;
        }
        (void)gs; (void)ge;  // silence unused-param warnings

        // ---- Pressure Advance (PA) section ----
        vs->Add(new wxStaticText(this, wxID_ANY, _L("Pressure Advance (PA) during MultiPass:")),
                0, wxLEFT|wxTOP|wxRIGHT, PAD);
        {
            auto* pa_row = new wxBoxSizer(wxHORIZONTAL);

            m_rb_pa[0] = new wxRadioButton(this, wxID_ANY, _L("PA ="),
                                            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
            const double safe_pa = (cur_pa_value >= 0.0 && cur_pa_value <= 2.0)
                                   ? cur_pa_value : 0.1;
            m_tc_pa_val = new wxTextCtrl(this, wxID_ANY,
                                          wxString::Format("%.3f", safe_pa),
                                          wxDefaultPosition, wxSize(60,-1));
            m_tc_pa_val->SetToolTip(_L("Pressure Advance value applied before each MultiPass region.\n"
                                       "Filament's configured PA is restored after."));
            pa_row->Add(m_rb_pa[0], 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);
            pa_row->Add(m_tc_pa_val, 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 16);

            m_rb_pa[1] = new wxRadioButton(this, wxID_ANY, _L("PA = 0"));
            m_rb_pa[1]->SetToolTip(_L("Disable Pressure Advance during MultiPass passes.\n"
                                       "Useful for very narrow lines (low flow ratio) where PA over-compensates."));
            pa_row->Add(m_rb_pa[1], 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 16);

            m_rb_pa[2] = new wxRadioButton(this, wxID_ANY, _L("No changes (default)"));
            pa_row->Add(m_rb_pa[2], 0, wxALIGN_CENTER_VERTICAL);

            const int safe_mode = (cur_pa_mode >= 0 && cur_pa_mode <= 2) ? cur_pa_mode : 0;
            if      (safe_mode == 1) m_rb_pa[0]->SetValue(true);
            else if (safe_mode == 2) m_rb_pa[1]->SetValue(true);
            else                     m_rb_pa[2]->SetValue(true);

            vs->Add(pa_row, 0, wxLEFT|wxRIGHT|wxTOP, PAD);

            auto update_pa_ctrl = [this]() {
                if (m_tc_pa_val)
                    m_tc_pa_val->Enable(m_rb_pa[0] && m_rb_pa[0]->GetValue());
            };
            update_pa_ctrl();
            m_rb_pa[0]->Bind(wxEVT_RADIOBUTTON, [update_pa_ctrl](wxCommandEvent&) { update_pa_ctrl(); });
            m_rb_pa[1]->Bind(wxEVT_RADIOBUTTON, [update_pa_ctrl](wxCommandEvent&) { update_pa_ctrl(); });
            m_rb_pa[2]->Bind(wxEVT_RADIOBUTTON, [update_pa_ctrl](wxCommandEvent&) { update_pa_ctrl(); });
        }
        {
            auto* lbl_pa = new wxStaticText(this, wxID_ANY,
                _L("PA = value / PA = 0: Orca injects SET_PRESSURE_ADVANCE before each MultiPass region\n"
                   "and restores the filament's configured PA value after. 'No changes' leaves PA untouched."));
            lbl_pa->SetForegroundColour(wxColour(100, 100, 100));
            vs->Add(lbl_pa, 0, wxLEFT|wxRIGHT|wxBOTTOM, PAD);
        }

        vs->Add(new wxStaticLine(this), 0, wxEXPAND|wxLEFT|wxRIGHT, PAD);

        // NEOTKO_MULTIPASS_PRIME_TAG — s58: Wipe tower prime spinner moved OUT of this dialog
        // to the global Print Settings optgroup (Tab.cpp, right below `neotko_wipe_tower`).
        // The config key `multipass_prime_volume` is now global (applies to both Top and
        // Penultimate; `penultimate_multipass_prime_volume` is removed).
        // m_sc_prime stays nullptr; get_prime_volume() guards it.
        (void)cur_prime_volume;
        // NEOTKO_MULTIPASS_PRIME_TAG_END

        // NEOTKO_MULTIPASS_MINLAYER_TAG — Layer height info + minimum ratio warning
        if (m_layer_height > 1e-6) {
            vs->Add(new wxStaticLine(this), 0, wxEXPAND|wxLEFT|wxRIGHT, PAD);
            const double min_r = std::max(0.05, 0.04 / m_layer_height);
            wxString info_text;
            if (m_layer_height < 0.04) {
                info_text = wxString::Format(
                    _L("\u26a0 Layer height %.3f mm is below 0.04 mm — passes thinner than\n"
                       "0.04 mm may not extrude reliably. Increase layer height."),
                    m_layer_height);
            } else {
                info_text = wxString::Format(
                    _L("Layer height: %.3f mm  \u2502  Min pass ratio: %.2f (= 0.04 mm min height)"),
                    m_layer_height, min_r);
            }
            auto* lbl_lh = new wxStaticText(this, wxID_ANY, info_text);
            lbl_lh->SetForegroundColour(
                (m_layer_height < 0.04) ? wxColour(200, 30, 30) : wxColour(100, 100, 100));
            vs->Add(lbl_lh, 0, wxLEFT|wxRIGHT|wxBOTTOM, PAD);
        }
        // NEOTKO_MULTIPASS_MINLAYER_TAG_END
        }  // end SurfaceZone::TOP

        // NEOTKO_MULTIPASS_SURFACES_TAG — Penultimate Surface independent section
        if (m_zone == SurfaceZone::PENULTIMATE) {
            auto* penu_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Penultimate MultiPass"));

            // Passes + sum label
            auto* pr1 = new wxBoxSizer(wxHORIZONTAL);
            pr1->Add(new wxStaticText(this, wxID_ANY, _L("Passes:")),
                     0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);
            const int safe_pp = (penu_passes >= 1 && penu_passes <= 3) ? penu_passes : 2;
            m_sc_penu_passes = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                              wxDefaultPosition, wxSize(60, -1),
                                              wxSP_ARROW_KEYS, 1, 3, safe_pp);
            pr1->Add(m_sc_penu_passes, 0, wxALIGN_CENTER_VERTICAL);
            penu_box->Add(pr1, 0, wxLEFT|wxRIGHT|wxBOTTOM, PAD);

            // Per-pass rows: Tool | Ratio | Angle | Fan | Speed | GCode start | GCode end
            const int def_tools[3]   = {0, 1, -1};
            const double def_ratios[3] = {0.5, 0.5, 0.34};
            const int def_angles[3]  = {-1, -1, -1};
            const int def_fans[3]    = {-1, -1, -1};
            const int def_speeds[3]  = {100, 100, 100};
            auto penu_tool_color = [this](int t) -> wxColour {
                if (t >= 0 && t < (int)m_colours.size() && !m_colours[t].empty())
                    return wxColour(m_colours[t]);
                return wxColour(160, 160, 160);
            };
            for (int i = 0; i < 3; ++i) {
                const int    pt  = penu_tools  ? penu_tools[i]  : def_tools[i];
                const double pr  = penu_ratios ? penu_ratios[i] : def_ratios[i];
                const int    pa  = penu_angles ? penu_angles[i] : def_angles[i];
                const int    pf  = penu_fans   ? penu_fans[i]   : def_fans[i];
                const int    ps  = penu_speeds  ? penu_speeds[i] : def_speeds[i];
                const std::string pgs = (penu_gs && !penu_gs[i].empty()) ? penu_gs[i] : "";
                const std::string pge = (penu_ge && !penu_ge[i].empty()) ? penu_ge[i] : "";

                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(this, wxID_ANY,
                          wxString::Format(_L("Pass %d:"), i + 1)),
                          0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 6);

                // Tool spinner + swatch
                const int fv    = (pt >= 0) ? pt + 1 : 0;
                const int max_t = std::max(4, (int)m_colours.size());
                m_sc_penu_tool[i] = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                    wxDefaultPosition, wxSize(55, -1), wxSP_ARROW_KEYS,
                    (i < 2) ? 1 : 0, max_t, std::clamp(fv, (i < 2) ? 1 : 0, max_t));
                m_swatch_penu[i] = new ColorSwatch(this, penu_tool_color(fv > 0 ? fv - 1 : -1));
                m_sc_penu_tool[i]->Bind(wxEVT_SPINCTRL, [this, i, penu_tool_color](wxSpinEvent& e) {
                    const int v = e.GetValue();
                    if (m_swatch_penu[i]) m_swatch_penu[i]->set_color(penu_tool_color(v > 0 ? v - 1 : -1));
                });
                row->Add(new wxStaticText(this, wxID_ANY, _L("Tool:")),
                         0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);
                row->Add(m_sc_penu_tool[i],  0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 3);
                row->Add(m_swatch_penu[i],   0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 10);

                // Ratio drag-text
                const double min_r = (m_layer_height > 1e-6) ? std::max(0.05, 0.04 / m_layer_height) : 0.05;
                m_tc_penu_ratio[i] = new DragTextCtrl(this, std::max(pr, min_r), min_r, 5.0, 0.01);
                m_tc_penu_ratio[i]->SetMinSize(wxSize(60, -1));
                row->Add(new wxStaticText(this, wxID_ANY, _L("Ratio:")),
                         0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);
                row->Add(m_tc_penu_ratio[i], 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 10);

                // Angle spinner
                const int safe_pa = (pa >= -1 && pa <= 359) ? pa : -1;
                m_sc_penu_angle[i] = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                    wxDefaultPosition, wxSize(60, -1), wxSP_ARROW_KEYS, -1, 359, safe_pa);
                row->Add(new wxStaticText(this, wxID_ANY, _L("Angle:")),
                         0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);
                row->Add(m_sc_penu_angle[i], 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 10);

                // NEOTKO_MULTIPASS_TAG — s58: Fan / Speed / GCode per-pass widgets hidden.
                // Members stay nullptr; read/write code guards with `m_sc_penu_fan[i] ? ...`.
                m_sc_penu_fan[i]    = nullptr;
                m_sc_penu_speed[i]  = nullptr;
                m_tc_penu_gstart[i] = nullptr;
                m_tc_penu_gend[i]   = nullptr;
                (void)pf; (void)ps; (void)pgs; (void)pge;  // silence unused-var warnings

                penu_box->Add(row, 0, wxLEFT|wxRIGHT|wxBOTTOM, PAD);

                if (i == 2) {
                    m_penu_pass3_widgets.push_back(m_sc_penu_tool[i]);
                    m_penu_pass3_widgets.push_back(m_swatch_penu[i]);
                    m_penu_pass3_widgets.push_back(m_tc_penu_ratio[i]);
                    m_penu_pass3_widgets.push_back(m_sc_penu_angle[i]);
                    // Fan/Speed/GCode hidden — not pushed.
                }
            }

            // Vary pattern checkbox
            {
                auto* vrow = new wxBoxSizer(wxHORIZONTAL);
                m_cb_penu_vary = new wxCheckBox(this, wxID_ANY, _L("Alternate pass direction"));
                m_cb_penu_vary->SetValue(penu_vary);
                m_cb_penu_vary->SetToolTip(_L("Each successive pass reverses the polyline traversal order (180 deg flip)."));
                vrow->Add(m_cb_penu_vary, 0, wxALIGN_CENTER_VERTICAL);
                penu_box->Add(vrow, 0, wxLEFT|wxRIGHT|wxBOTTOM, PAD);
            }

            // NEOTKO_MULTIPASS_PRIME_TAG — s58: Penu prime spinner moved OUT (global setting now).
            // m_sc_penu_prime stays nullptr; get_penu_prime() guards it.
            (void)penu_prime_vol;

            // Enable/disable pass-3 widgets based on penu_passes
            const bool p3_on = (safe_pp >= 3);
            for (auto* w : m_penu_pass3_widgets) if (w) w->Enable(p3_on);

            m_sc_penu_passes->Bind(wxEVT_SPINCTRL, [this](wxCommandEvent&) {
                const bool on = (m_sc_penu_passes->GetValue() >= 3);
                for (auto* w : m_penu_pass3_widgets) if (w) w->Enable(on);
                Layout();
            });

            vs->Add(penu_box, 0, wxEXPAND|wxALL, PAD);
        }
        // NEOTKO_MULTIPASS_SURFACES_TAG_END

        // NEOTKO_PROFILE_TAG_START — Fase F1: Save current dialog state as a
        // MultiPass-only Surface Effect Profile (colormix payload stays absent).
        // The profile becomes available to the 3D Painter, where painting it
        // overrides the preset's MultiPass for the painted area.
        {
            wxBoxSizer* prof_row = new wxBoxSizer(wxHORIZONTAL);
            wxButton* btn_save = new wxButton(this, wxID_ANY, _L("Save as profile…"));
            prof_row->Add(btn_save, 0);
            vs->Add(prof_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);
            btn_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                wxTextEntryDialog ned(this, _L("Profile name:"),
                                      _L("Save MultiPass profile"));
                if (ned.ShowModal() != wxID_OK) return;
                const std::string name = ned.GetValue().ToStdString();
                if (name.empty()) return;

                // NEOTKO_PROFILE_TAG — Fase F1 fix: write straight into the
                // SurfaceEffectPayload kv map. The earlier
                // `new_from_defaults_keys(multipass_keys())` path sometimes
                // produced an empty config (per-region keys not surfaced from
                // FullPrintConfig defaults on every build), in which case the
                // typed writers silently no-op'd and `snapshot_keys` returned
                // present=false → painter mode read "no MP payload" and fell
                // back to CM. Serialising manually here matches how
                // ConfigOption*::serialize emits values (bool→"1"/"0",
                // int→%d, float→%g, string→raw) so the inverse parse in
                // `multipass_from_profile_payload` round-trips exactly.
                Slic3r::SurfaceEffectPayload mp_payload;
                mp_payload.present = true;
                auto wi = [&](const char* k, int v) {
                    mp_payload.kv[k] = std::to_string(v);
                };
                auto wb = [&](const char* k, bool v) {
                    mp_payload.kv[k] = v ? "1" : "0";
                };
                auto wf = [&](const char* k, double v) {
                    std::ostringstream os;
                    os.imbue(std::locale::classic());
                    os << v;
                    mp_payload.kv[k] = os.str();
                };
                auto ws = [&](const char* k, const std::string& v) {
                    mp_payload.kv[k] = v;
                };
                // Top role — saved profile enables MP on top by default.
                wb("multipass_enabled",          true);
                wi("multipass_surface",          get_surface());
                wi("multipass_num_passes",       get_passes());
                wi("multipass_tool_1",           get_tool1());
                wi("multipass_tool_2",           get_tool2());
                wi("multipass_tool_3",           get_tool3());
                wf("multipass_width_ratio_1",    get_ratio1());
                wf("multipass_width_ratio_2",    get_ratio2());
                wf("multipass_width_ratio_3",    get_ratio3());
                wb("multipass_vary_pattern",     get_vary());
                wi("multipass_angle_1",          get_angle1());
                wi("multipass_angle_2",          get_angle2());
                wi("multipass_angle_3",          get_angle3());
                wi("multipass_pa_mode",          get_pa_mode());
                wf("multipass_pa_value",         get_pa_value());
                wi("multipass_fan_1",            get_fan1());
                wi("multipass_fan_2",            get_fan2());
                wi("multipass_fan_3",            get_fan3());
                wi("multipass_speed_pct_1",      get_speed1());
                wi("multipass_speed_pct_2",      get_speed2());
                wi("multipass_speed_pct_3",      get_speed3());
                ws("multipass_gcode_start_1",    get_gcode_start1());
                ws("multipass_gcode_start_2",    get_gcode_start2());
                ws("multipass_gcode_start_3",    get_gcode_start3());
                ws("multipass_gcode_end_1",      get_gcode_end1());
                ws("multipass_gcode_end_2",      get_gcode_end2());
                ws("multipass_gcode_end_3",      get_gcode_end3());
                // multipass_perimeter_override + multipass_path_gradient live in
                // SCM dialog widgets, not here — keep defaults from new_from_defaults_keys.
                // Penultimate role
                wb("penultimate_multipass_enabled",     get_penu_enabled());
                wi("penultimate_multipass_num_passes",  get_penu_passes());
                wi("penultimate_multipass_tool_1",      get_penu_tool1());
                wi("penultimate_multipass_tool_2",      get_penu_tool2());
                wi("penultimate_multipass_tool_3",      get_penu_tool3());
                wf("penultimate_multipass_width_ratio_1", get_penu_ratio1());
                wf("penultimate_multipass_width_ratio_2", get_penu_ratio2());
                wf("penultimate_multipass_width_ratio_3", get_penu_ratio3());
                wb("penultimate_multipass_vary_pattern", get_penu_vary());
                wi("penultimate_multipass_angle_1",     get_penu_angle1());
                wi("penultimate_multipass_angle_2",     get_penu_angle2());
                wi("penultimate_multipass_angle_3",     get_penu_angle3());
                wi("penultimate_multipass_fan_1",       get_penu_fan1());
                wi("penultimate_multipass_fan_2",       get_penu_fan2());
                wi("penultimate_multipass_fan_3",       get_penu_fan3());
                wi("penultimate_multipass_speed_pct_1", get_penu_speed1());
                wi("penultimate_multipass_speed_pct_2", get_penu_speed2());
                wi("penultimate_multipass_speed_pct_3", get_penu_speed3());
                ws("penultimate_multipass_gcode_start_1", get_penu_gcode_start1());
                ws("penultimate_multipass_gcode_start_2", get_penu_gcode_start2());
                ws("penultimate_multipass_gcode_start_3", get_penu_gcode_start3());
                ws("penultimate_multipass_gcode_end_1",   get_penu_gcode_end1());
                ws("penultimate_multipass_gcode_end_2",   get_penu_gcode_end2());
                ws("penultimate_multipass_gcode_end_3",   get_penu_gcode_end3());

                Slic3r::SurfaceEffectProfile prof;
                prof.name = name;
                prof.multipass = std::move(mp_payload);
                // colormix / pathblend payloads stay present=false.
                const int new_id = Slic3r::SurfaceEffectProfileManager::get().add(std::move(prof));
                wxMessageBox(wxString::Format(_L("Saved MultiPass profile #%d (%zu keys)."),
                                              new_id,
                                              (size_t)Slic3r::SurfaceEffectProfileManager::get()
                                                  .find(new_id)->multipass.kv.size()),
                             _L("Surface Effect Profile"),
                             wxOK | wxICON_INFORMATION, this);
            });
        }
        // NEOTKO_PROFILE_TAG_END

        vs->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL),
                0, wxALL|wxALIGN_RIGHT, PAD);
        SetSizerAndFit(vs);
    }
};
// NEOTKO_MULTIPASS_TAG_END
// NEOTKO_SANDWICH_TAG — Fase 5 s73: PathBlendDialog class is dead code.
// PathBlend is now edited inline in the SandwichDialog row (kind selector
// Half/Full + floor_mm spinner + mid_end_mm field + ease cycler + chip
// tool pickers). Wrapped in #if 0 to remove it from the build without
// the bulk-delete risk; remove physically in a follow-up cleanup.
#if 0
class PathBlendDialog : public wxDialog
{
public:
    PathBlendDialog(wxWindow* parent,
                    const Slic3r::PathBlendPassConfig& cur,
                    double layer_height_mm,
                    const std::vector<std::string>& fcolors = {})
        : wxDialog(parent, wxID_ANY, _L("PathBlend Settings"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_layer_height_mm(std::max(0.04, layer_height_mm))
    {
        m_ease_mode = std::clamp(cur.ease_mode, 0, 3);
        const int PAD = 8;
        auto* vs = new wxBoxSizer(wxVERTICAL);

        // --- Mode (Half / Full) ---
        {
            auto* row_mode = new wxBoxSizer(wxHORIZONTAL);
            wxString choices[2] = { _L("Half (ramp only, semi-fill)"),
                                    _L("Full (ramp + cap)") };
            m_rb_mode = new wxRadioBox(this, wxID_ANY, _L("Mode"),
                                       wxDefaultPosition, wxDefaultSize,
                                       2, choices, 1, wxRA_SPECIFY_ROWS);
            m_rb_mode->SetSelection(
                (cur.mode == Slic3r::PathBlendPassConfig::Mode::Full) ? 1 : 0);
            row_mode->Add(m_rb_mode, 0);
            vs->Add(row_mode, 0, wxALL, PAD);
        }

        // --- Tools (bottom + top, Fase 5 s72) ---
        // Stored config values are 0-based; displayed here as 1-based Fn.
        const int init_tools[2] = { cur.tool_bottom,
                                    (cur.tool_top < 0 ? 0 : cur.tool_top) };

        // Helper: tool index -> filament color
        auto pb_hex_col = [&fcolors](int t) -> wxColour {
            if (t >= 0 && t < (int)fcolors.size() && !fcolors[t].empty()) {
                unsigned long rgb = 0;
                wxString s = wxString::FromUTF8(fcolors[t]);
                if (s.StartsWith("#")) s = s.Mid(1);
                s.ToULong(&rgb, 16);
                return wxColour((rgb>>16)&0xFF,(rgb>>8)&0xFF,rgb&0xFF);
            }
            return wxColour(128, 128, 128);
        };

        // 3 columns: label | spinctrl (F1-based) | color swatch (updates on change)
        // F-notation: F1 = T0, F2 = T1, ... (1-based Fn).
        auto* grid = new wxFlexGridSizer(2, 3, 4, 8);
        const int pb_max_tool = std::max(4, (int)fcolors.size()); // F1..FN
        const wxString tool_lbl[2] = { _L("Ramp tool (bottom):"),
                                       _L("Cap tool (top):") };

        for (int i = 0; i < 2; ++i) {
            grid->Add(new wxStaticText(this, wxID_ANY, tool_lbl[i]),
                      0, wxALIGN_CENTER_VERTICAL);

            // Convert stored 0-based index to F-notation (1-based) for display.
            const int init_t = std::clamp(init_tools[i] + 1, 1, pb_max_tool);
            m_sc_tool[i] = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                           wxDefaultPosition, wxSize(60,-1),
                                           wxSP_ARROW_KEYS, 1, pb_max_tool, init_t);
            m_sc_tool[i]->SetToolTip(_L("Filament number (F1 = T0, F2 = T1, ...). Matches MixedColor F-notation."));
            grid->Add(m_sc_tool[i], 0, wxALIGN_CENTER_VERTICAL);

            // Swatch: pb_hex_col takes 0-based; init_t is 1-based → subtract 1.
            m_swatch_pb[i] = new ColorSwatch(this, pb_hex_col(init_t - 1));
            grid->Add(m_swatch_pb[i], 0, wxALIGN_CENTER_VERTICAL);

            // Update swatch when the filament number changes
            m_sc_tool[i]->Bind(wxEVT_SPINCTRL,
                [this, i, fcolors_cap = fcolors](wxSpinEvent& e) {
                    if (!m_swatch_pb[i]) return;
                    const int t = e.GetValue() - 1; // F-notation → 0-based for color lookup
                    if (t >= 0 && t < (int)fcolors_cap.size() && !fcolors_cap[t].empty()) {
                        unsigned long rgb = 0;
                        wxString s = wxString::FromUTF8(fcolors_cap[t]);
                        if (s.StartsWith("#")) s = s.Mid(1);
                        s.ToULong(&rgb, 16);
                        m_swatch_pb[i]->set_color(wxColour((rgb>>16)&0xFF,(rgb>>8)&0xFF,rgb&0xFF));
                    } else {
                        m_swatch_pb[i]->set_color(wxColour(128, 128, 128));
                    }
                });
        }
        vs->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);

        // --- Heights (mm) - explicit geometry replaces min_ratio/max_ratio ---
        // floor: Z of the ramp at t=0 (low end), >= 0.01.
        // mid_end: Z of the ramp at t=1 (high end), >= floor. In Full mode it
        // also satisfies mid_end <= H - 0.04 (cap thickness >= 0.04 mm).
        const double H = m_layer_height_mm;
        const double mid_max_full = std::max(0.01, H - 0.04);
        const double mid_max_half = H;
        const double init_floor   = std::clamp((double)cur.floor_mm,   0.01, H);
        const double init_mid     = std::clamp((double)cur.mid_end_mm,
                                              init_floor,
                                              (cur.mode == Slic3r::PathBlendPassConfig::Mode::Full)
                                                  ? mid_max_full : mid_max_half);

        auto* h_grid = new wxFlexGridSizer(2, 2, 4, 8);
        h_grid->Add(new wxStaticText(this, wxID_ANY,
            wxString::Format(_L("Floor mm (ramp at t=0, layer H=%.2f):"), H)),
            0, wxALIGN_CENTER_VERTICAL);
        m_sc_floor = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                                          wxDefaultPosition, wxSize(80, -1),
                                          wxSP_ARROW_KEYS, 0.01, H, init_floor, 0.005);
        m_sc_floor->SetDigits(3);
        h_grid->Add(m_sc_floor, 0, wxALIGN_CENTER_VERTICAL);
        h_grid->Add(new wxStaticText(this, wxID_ANY, _L("Mid-end mm (ramp at t=1):")),
                    0, wxALIGN_CENTER_VERTICAL);
        m_sc_mid_end = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString,
                                            wxDefaultPosition, wxSize(80, -1),
                                            wxSP_ARROW_KEYS, init_floor, H, init_mid, 0.005);
        m_sc_mid_end->SetDigits(3);
        h_grid->Add(m_sc_mid_end, 0, wxALIGN_CENTER_VERTICAL);
        vs->Add(h_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);

        // Cross-control: floor and mid_end never cross; mid_end ceiling tightens
        // to H-0.04 when Full (cap >= 0.04). Tool-top is meaningful only in Full.
        auto reclamp_heights = [this, H]() {
            const bool full = (m_rb_mode && m_rb_mode->GetSelection() == 1);
            const double mid_max = full ? std::max(0.01, H - 0.04) : H;
            const double floor_v = m_sc_floor->GetValue();
            if (m_sc_mid_end->GetValue() < floor_v) m_sc_mid_end->SetValue(floor_v);
            if (m_sc_mid_end->GetValue() > mid_max) m_sc_mid_end->SetValue(mid_max);
            m_sc_mid_end->SetRange(floor_v, mid_max);
            if (m_sc_tool[1])   m_sc_tool[1]->Enable(full);
            if (m_swatch_pb[1]) m_swatch_pb[1]->Enable(full);
        };
        m_sc_floor->Bind(wxEVT_SPINCTRLDOUBLE,
            [reclamp_heights](wxSpinDoubleEvent&) { reclamp_heights(); });
        m_sc_mid_end->Bind(wxEVT_SPINCTRLDOUBLE,
            [reclamp_heights](wxSpinDoubleEvent&) { reclamp_heights(); });
        m_rb_mode->Bind(wxEVT_RADIOBOX,
            [reclamp_heights](wxCommandEvent&) { reclamp_heights(); });
        reclamp_heights();

        // --- Gradient easing buttons ---
        {
            vs->Add(new wxStaticText(this, wxID_ANY, _L("Gradient curve:")),
                    0, wxLEFT | wxRIGHT | wxTOP, PAD);
            auto* row_ease = new wxBoxSizer(wxHORIZONTAL);
            const wxString ease_labels[4] = {
                _L("Linear"), _L("Ease In"), _L("Ease Out"), _L("Ease In/Out")
            };
            const wxString ease_tips[4] = {
                _L("Linear: uniform transition."),
                _L("Ease In: slow start, fast end (t\u00b2)."),
                _L("Ease Out: fast start, slow end (1-(1-t)\u00b2)."),
                _L("Ease In/Out: S-curve (smoothstep).")
            };
            for (int i = 0; i < 4; ++i) {
                m_btn_ease[i] = new wxButton(this, wxID_ANY, ease_labels[i],
                                              wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                m_btn_ease[i]->SetToolTip(ease_tips[i]);
                row_ease->Add(m_btn_ease[i], 0, wxRIGHT, 3);
            }
            vs->Add(row_ease, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);

            // Highlight active button
            auto refresh_ease = [this]() {
                const wxColour active_bg(60, 120, 220);
                const wxColour active_fg(*wxWHITE);
                const wxColour normal_bg(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
                const wxColour normal_fg(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT));
                for (int i = 0; i < 4; ++i) {
                    if (!m_btn_ease[i]) continue;
                    const bool on = (i == m_ease_mode);
                    m_btn_ease[i]->SetBackgroundColour(on ? active_bg : normal_bg);
                    m_btn_ease[i]->SetForegroundColour(on ? active_fg : normal_fg);
                    m_btn_ease[i]->Refresh();
                }
            };
            for (int i = 0; i < 4; ++i) {
                m_btn_ease[i]->Bind(wxEVT_BUTTON, [this, i, refresh_ease](wxCommandEvent&) {
                    m_ease_mode = i;
                    refresh_ease();
                });
            }
            refresh_ease();
        }

        // --- Fill angle override ---
        // (invert_gradient dropped in Fase 5: heights are explicit, so the
        // ramp direction is fixed by the floor/mid_end signs — no toggle.)
        {
            auto* row_ang = new wxBoxSizer(wxHORIZONTAL);
            row_ang->Add(new wxStaticText(this, wxID_ANY, _L("Fill angle (-1 = follow top surface):")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            const int safe_ang = (cur.fill_angle >= -1 && cur.fill_angle <= 359) ? cur.fill_angle : -1;
            m_sc_angle_pb = new wxSpinCtrl(this, wxID_ANY, wxEmptyString,
                                            wxDefaultPosition, wxSize(70,-1),
                                            wxSP_ARROW_KEYS, -1, 359, safe_ang);
            m_sc_angle_pb->SetToolTip(_L(
                "-1 = follow the top surface fill angle setting.\n"
                "0-359 = custom fill angle for PathBlend lines only."));
            row_ang->Add(m_sc_angle_pb, 0, wxALIGN_CENTER_VERTICAL);
            vs->Add(row_ang, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);
        }

        // note
        auto* note = new wxStaticText(this, wxID_ANY,
            _L("Gradient is automatic. Surface filter (top/penu) is set in the Surface Mixer combos."));
        note->SetForegroundColour(wxColour(90, 90, 90));
        note->Wrap(320);
        vs->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);

        // NEOTKO_PROFILE_TAG_START — Fase G1: Save current dialog state as a
        // PathBlend-only Surface Effect Profile (colormix / multipass payloads
        // stay absent). The profile becomes available to the 3D Painter, where
        // painting it overrides the preset's PathBlend on the painted area.
        {
            wxBoxSizer* prof_row = new wxBoxSizer(wxHORIZONTAL);
            wxButton* btn_save = new wxButton(this, wxID_ANY, _L("Save as profile…"));
            prof_row->Add(btn_save, 0);
            vs->Add(prof_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);
            btn_save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                wxTextEntryDialog ned(this, _L("Profile name:"),
                                      _L("Save PathBlend profile"));
                if (ned.ShowModal() != wxID_OK) return;
                const std::string name = ned.GetValue().ToStdString();
                if (name.empty()) return;

                Slic3r::SurfaceEffectPayload pb_payload;
                pb_payload.present = true;
                auto wi = [&](const char* k, int v) {
                    pb_payload.kv[k] = std::to_string(v);
                };
                auto wb = [&](const char* k, bool v) {
                    pb_payload.kv[k] = v ? "1" : "0";
                };
                auto wf = [&](const char* k, double v) {
                    std::ostringstream os;
                    os.imbue(std::locale::classic());
                    os << v;
                    pb_payload.kv[k] = os.str();
                };
                // Saved profile activates PathBlend by default (mirrors MP
                // dialog save semantics: dialog open → user intends to use it).
                wb("multipass_path_gradient",  true);
                wi("pathblend_num_passes",     get_num_passes());
                wi("pathblend_tool_1",         get_tool(0));
                wi("pathblend_tool_2",         get_tool(1));
                wi("pathblend_tool_3",         get_tool(2));
                wi("pathblend_tool_4",         get_tool(3));
                wf("pathblend_min_ratio",      get_min_ratio());
                wf("pathblend_max_ratio",      get_max_ratio());
                wi("pathblend_ease_mode",      get_ease_mode());
                wb("pathblend_invert_gradient", get_invert());
                wi("pathblend_fill_angle",     get_fill_angle());
                // pathblend_surface / pathblend_layer_ratio_* not exposed in
                // this dialog — kept out of the payload so the painter-mode
                // builder falls back to struct defaults.

                Slic3r::SurfaceEffectProfile prof;
                prof.name = name;
                prof.pathblend = std::move(pb_payload);
                const int new_id = Slic3r::SurfaceEffectProfileManager::get().add(std::move(prof));
                wxMessageBox(wxString::Format(_L("Saved PathBlend profile #%d (%zu keys)."),
                                              new_id,
                                              (size_t)Slic3r::SurfaceEffectProfileManager::get()
                                                  .find(new_id)->pathblend.kv.size()),
                             _L("Surface Effect Profile"),
                             wxOK | wxICON_INFORMATION, this);
            });
        }
        // NEOTKO_PROFILE_TAG_END

        vs->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxALIGN_RIGHT, PAD);
        SetSizerAndFit(vs);
    }

    // ---- Fase 5 (s72) accessors over the new geometry model ----
    Slic3r::PathBlendPassConfig get_config() const
    {
        Slic3r::PathBlendPassConfig out;
        out.mode        = (m_rb_mode && m_rb_mode->GetSelection() == 1)
                          ? Slic3r::PathBlendPassConfig::Mode::Full
                          : Slic3r::PathBlendPassConfig::Mode::Half;
        out.floor_mm    = static_cast<float>(m_sc_floor   ? m_sc_floor->GetValue()   : 0.01);
        out.mid_end_mm  = static_cast<float>(m_sc_mid_end ? m_sc_mid_end->GetValue() : 0.05);
        // Spinners display F-notation (F1=1, F2=2, ...); stored 0-based.
        out.tool_bottom = m_sc_tool[0] ? (m_sc_tool[0]->GetValue() - 1) : 0;
        out.tool_top    = (out.mode == Slic3r::PathBlendPassConfig::Mode::Full && m_sc_tool[1])
                          ? (m_sc_tool[1]->GetValue() - 1) : -1;
        out.ease_mode   = m_ease_mode;
        out.fill_angle  = m_sc_angle_pb ? m_sc_angle_pb->GetValue() : -1;
        out.sync_legacy_view();
        return out;
    }

    // Legacy getters kept for the "Save as profile..." button below, which
    // still writes the painter's legacy kv payload (back-compat with Fase G
    // profiles saved before Fase 5; painter reads via from_payload helper that
    // converts on read).
    int   get_num_passes()  const { return (m_rb_mode && m_rb_mode->GetSelection() == 1) ? 2 : 1; }
    int   get_tool(int i)   const {
        if (i == 0 && m_sc_tool[0]) return m_sc_tool[0]->GetValue() - 1;
        if (i == 1 && m_sc_tool[1] && get_num_passes() >= 2)
            return m_sc_tool[1]->GetValue() - 1;
        return -1;
    }
    float get_min_ratio()   const {
        const double H = std::max(0.04, m_layer_height_mm);
        return static_cast<float>(std::clamp((m_sc_floor ? m_sc_floor->GetValue() : 0.01) / H,
                                             0.01, 0.49));
    }
    float get_max_ratio()   const {
        const double H = std::max(0.04, m_layer_height_mm);
        return static_cast<float>(std::clamp((m_sc_mid_end ? m_sc_mid_end->GetValue() : 0.05) / H,
                                             0.51, 1.0));
    }
    int   get_ease_mode()   const { return m_ease_mode; }
    bool  get_invert()      const { return true; }   // dropped in Fase 5; safe default
    int   get_fill_angle()  const { return m_sc_angle_pb ? m_sc_angle_pb->GetValue() : -1; }

private:
    double            m_layer_height_mm = 0.20;
    wxRadioBox*       m_rb_mode         = nullptr;
    wxSpinCtrl*       m_sc_tool[2]      = {nullptr, nullptr};
    ColorSwatch*      m_swatch_pb[2]    = {nullptr, nullptr};
    wxSpinCtrlDouble* m_sc_floor        = nullptr;
    wxSpinCtrlDouble* m_sc_mid_end      = nullptr;
    wxButton*         m_btn_ease[4]     = {nullptr, nullptr, nullptr, nullptr};
    int               m_ease_mode       = 0;
    wxSpinCtrl*       m_sc_angle_pb     = nullptr;
};
#endif // NEOTKO_SANDWICH_TAG Fase 5 s73 — PathBlendDialog dead code wrap
// NEOTKO_PATHBLEND_TAG_END

// NEOTKO_SURFACE_MIXER_TAG_START
// Unified Surface Color Mixer dialog.
// Two independent surface boxes (Top Layer / Penultimate Layer) each with an
// effect selector (None / Color Mix / Multi-Pass / Path Blend) and an "Edit…"
// button that opens the respective sub-dialog.  A TD (Transmittance Depth)
// section with per-filament sliders drives a Beer-Lambert blend preview swatch.
// TD values are persisted in app_config under neotko_td_1 … neotko_td_4.
// Effect assignments are written back to the existing config keys on OK.
class SurfaceColorMixerDialog : public wxDialog
{
public:
    static constexpr int EFF_NONE = 0;
    static constexpr int EFF_CM   = 1;
    static constexpr int EFF_MP   = 2;
    static constexpr int EFF_PB   = 3;

    SurfaceColorMixerDialog(wxWindow* parent,
                            DynamicPrintConfig* config,
                            std::function<void(const std::string&)> on_change_cb)
        : wxDialog(parent, wxID_ANY, _L("Surface Color Mixer"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_config(config)
        , m_on_change(std::move(on_change_cb))
    {
        // Filament colors from project config
        if (auto* o = wxGetApp().preset_bundle->project_config
                          .option<ConfigOptionStrings>("filament_colour"))
            m_fcolors = o->values;
        while (m_fcolors.size() < 4) m_fcolors.push_back("#808080");

        // TD values from app_config
        auto* ac = wxGetApp().app_config;
        for (int i = 0; i < 4; ++i) {
            const std::string key = "neotko_td_" + std::to_string(i + 1);
            const std::string val = ac ? ac->get(key) : "";
            float v = 0.f;
            try { if (!val.empty()) v = std::stof(val); } catch (...) {}
            m_td[i] = std::max(0.01f, std::min(10.f, v));
        }

        infer_effects();
        build_ui();
    }

private:
    DynamicPrintConfig*                      m_config    = nullptr;
    std::function<void(const std::string&)>  m_on_change;
    std::vector<std::string>                 m_fcolors;
    std::array<float, 4>                     m_td        = {};
    int                                      m_top_eff   = EFF_NONE;
    int                                      m_penu_eff  = EFF_NONE;

    // Zone card widgets — pills replace wxChoice combos
    wxButton*      m_pill_top[4]    = {nullptr, nullptr, nullptr, nullptr};
    wxButton*      m_pill_penu[4]   = {nullptr, nullptr, nullptr, nullptr};
    wxButton*      m_btn_adv_top    = nullptr;
    wxButton*      m_btn_adv_penu   = nullptr;
    wxStaticText*  m_lbl_top_sum    = nullptr;
    wxStaticText*  m_lbl_penu_sum   = nullptr;
    wxPanel*       m_prev_top       = nullptr;  // 120×68 mini-preview
    wxPanel*       m_prev_penu      = nullptr;
    // Filament section
    std::array<wxSlider*,     4>  m_sl_td  = {};
    std::array<wxStaticText*, 4>  m_lbl_td = {};
    // NEOTKO_COLORMIX_TAG_START — zone + filament filter + min_length controls
    wxChoice*         m_choice_top_zone      = nullptr;
    wxChoice*         m_choice_penu_zone     = nullptr;
    wxSpinCtrl*       m_sc_filament_filter   = nullptr;
    wxSpinCtrlDouble* m_sc_min_length        = nullptr;
    wxCheckBox*       m_chk_use_virtual      = nullptr;
    // NEOTKO_COLORMIX_TAG — s58: lane distribution mode (global, affects ColorMix + PathBlend).
    wxChoice*         m_choice_lane_mode     = nullptr;
    // NEOTKO_COLORMIX_TAG — s60: gradient UI moved to ColorMixPatternDialog.
    // NEOTKO_MULTIPASS_SURFACES_TAG — stacked preview swatches (Top × Penu Beer-Lambert)
    wxPanel*       m_stacked_sw_top  = nullptr;
    wxPanel*       m_stacked_sw_penu = nullptr;
    wxPanel*       m_stacked_swatch  = nullptr;
    wxStaticText*  m_lbl_opacity_top = nullptr;
    // NEOTKO_MULTIPASS_TAG_START — Blend Suggestion (Beer-Lambert joint optimizer)
    wxComboBox*    m_bs_combo_target  = nullptr; // virtual MixedColor target picker
    wxPanel*       m_bs_swatch_target = nullptr; // colour of selected target MixedColor
    wxRadioButton* m_bs_rb_top       = nullptr; // "Top layer only" mode
    wxRadioButton* m_bs_rb_joint     = nullptr; // "Top + Penultimate" mode
    wxPanel*       m_bs_swatch       = nullptr; // simulated result colour
    wxStaticText*  m_bs_lbl_score    = nullptr; // "ΔE: X.X" inline result
    std::vector<Slic3r::ColorMixOption> m_bs_mix_opts; // virtual-only options cache
    wxCheckBox*    m_mp_perim_override = nullptr; // NEOTKO_MULTIPASS_TAG: perimeter override checkbox
    // NEOTKO_MULTIPASS_TAG_END

    // ------------------------------------------------------------------ helpers

    // Re-read m_fcolors + mixed_filament_definitions and repopulate the Target combo.
    // Called by the ↺ refresh button next to the dropdown.
    void refresh_mix_opts()
    {
        // Re-read filament colours from project_config (always up-to-date)
        if (auto* o = wxGetApp().preset_bundle->project_config
                          .option<ConfigOptionStrings>("filament_colour"))
            m_fcolors = o->values;
        while (m_fcolors.size() < 4) m_fcolors.push_back("#808080");

        // Prefer project_config for mixed_filament_definitions: it reflects live UI
        // changes (add/delete MixedColors) without requiring a re-slice.
        // m_config holds the snapshot from the last slice and is always stale here.
        m_bs_mix_opts.clear();
        std::string mixed_defs;
        if (auto* o = wxGetApp().preset_bundle->project_config
                          .option<ConfigOptionString>("mixed_filament_definitions"))
            mixed_defs = o->value;
        if (mixed_defs.empty()) {
            if (auto* o = m_config->option<ConfigOptionString>("mixed_filament_definitions"))
                mixed_defs = o->value;
        }

        if (!mixed_defs.empty()) {
            for (auto& opt : Slic3r::SurfaceColorMix::get_mix_options(mixed_defs, m_fcolors))
                if (!opt.is_physical)
                    m_bs_mix_opts.push_back(opt);
        }

        if (!m_bs_combo_target) return;
        m_bs_combo_target->Clear();
        if (m_bs_mix_opts.empty()) {
            m_bs_combo_target->Append(_L("(no MixedColor defined)"));
            m_bs_combo_target->SetSelection(0);
            m_bs_combo_target->Enable(false);
        } else {
            for (auto& opt : m_bs_mix_opts)
                m_bs_combo_target->Append(wxString::FromUTF8(opt.label));
            m_bs_combo_target->SetSelection(0);
            m_bs_combo_target->Enable(true);
        }

        // Sync target swatch to new selection
        if (m_bs_swatch_target) {
            wxColour c(128,128,128);
            if (!m_bs_mix_opts.empty()) {
                const std::string& dc = m_bs_mix_opts[0].display_color;
                if (dc.size() >= 7 && dc[0] == '#') {
                    unsigned long rgb = 0;
                    if (wxString::FromUTF8(dc.substr(1)).ToULong(&rgb, 16))
                        c = wxColour((rgb>>16)&0xFF, (rgb>>8)&0xFF, rgb&0xFF);
                }
            }
            m_bs_swatch_target->SetBackgroundColour(c);
            m_bs_swatch_target->Refresh();
        }

        // Refresh TD sliders in case filament list changed
        update_ui();
    }

    static wxColour hex_to_col(const std::string& hex)
    {
        unsigned long rgb = 0;
        wxString s = wxString::FromUTF8(hex);
        if (s.StartsWith("#")) s = s.Mid(1);
        if (!s.ToULong(&rgb, 16)) return wxColour(128, 128, 128);
        return wxColour((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    }

    void infer_effects()
    {
        const bool cm_on     = m_config->opt_bool("interlayer_colormix_enabled");
        const int  cm_srf    = m_config->opt_int ("interlayer_colormix_surface");
        const bool mp_on     = m_config->opt_bool("multipass_enabled");         // top only
        const int  mp_srf    = m_config->opt_int ("multipass_surface");
        // NEOTKO_MULTIPASS_SURFACES_TAG: Penultimate MultiPass now has its own enabled key.
        const bool penu_mp_on = m_config->opt_bool("penultimate_multipass_enabled");
        // PathBlend is now independent of MultiPass — uses its own surface key.
        const bool pb_on  = m_config->opt_bool("multipass_path_gradient");
        const int  pb_srf = m_config->opt_int ("pathblend_surface");

        // surface encoding: 0=both, 1=top only, 2=penu only
        auto for_top  = [](int s) { return s == 0 || s == 1; };
        auto for_penu = [](int s) { return s == 0 || s == 2; };

        m_top_eff  = EFF_NONE;
        m_penu_eff = EFF_NONE;

        if      (pb_on  && for_top(pb_srf))  m_top_eff  = EFF_PB;
        else if (mp_on  && for_top(mp_srf))  m_top_eff  = EFF_MP;
        else if (cm_on  && for_top(cm_srf))  m_top_eff  = EFF_CM;

        // NEOTKO_PROFILE_TAG — s68: PB on Penu re-enabled (engine refactor of
        // PathBlendEngine::apply_path made it role-aware).
        if      (pb_on && for_penu(pb_srf))     m_penu_eff = EFF_PB;
        else if (penu_mp_on)                    m_penu_eff = EFF_MP;
        else if (cm_on     && for_penu(cm_srf)) m_penu_eff = EFF_CM;
    }

    wxString summary_for(int eff, int surface_id) const
    {
        if (eff == EFF_NONE) return _L("No effect");
        if (eff == EFF_CM) {
            const std::string& pat = (surface_id == 0)
                ? m_config->opt_string("interlayer_colormix_pattern_top")
                : m_config->opt_string("interlayer_colormix_pattern_penultimate");
            return wxString::Format(_L("Pattern: %s"),
                wxString::FromUTF8(pat.empty() ? "(none)" : pat));
        }
        if (eff == EFF_MP) {
            int n = 2;
            // NEOTKO_MULTIPASS_SURFACES_TAG: penultimate reads its own num_passes key
            const char* np_key = (surface_id == 0)
                ? "multipass_num_passes" : "penultimate_multipass_num_passes";
            if (auto* o = m_config->option<ConfigOptionInt>(np_key)) n = o->value;
            return wxString::Format(_L("%d passes"), n);
        }
        if (eff == EFF_PB) {
            int n = 2;
            if (auto* o = m_config->option<ConfigOptionInt>("pathblend_num_passes")) n = o->value;
            return wxString::Format(_L("PathBlend: %d passes"), n);
        }
        return "";
    }

    wxColour blend_preview(int surface_id, float* out_weight = nullptr) const
    {
        const int eff = (surface_id == 0) ? m_top_eff : m_penu_eff;
        if (eff == EFF_NONE) return GetBackgroundColour();

        struct Pass { int tool_0; float ratio; };
        std::vector<Pass> passes;

        if (eff == EFF_CM) {
            const std::string& pat = (surface_id == 0)
                ? m_config->opt_string("interlayer_colormix_pattern_top")
                : m_config->opt_string("interlayer_colormix_pattern_penultimate");
            if (pat.empty()) return wxColour(180, 180, 180);
            std::map<int, int> cnt;
            int total = 0;
            for (char c : pat) {
                int t = static_cast<int>(c - '1');
                if (t >= 0 && t < 4) { cnt[t]++; total++; }
            }
            if (total == 0) return wxColour(180, 180, 180);
            for (auto& [t, n] : cnt)
                passes.push_back({t, static_cast<float>(n) / total});
        } else if (eff == EFF_MP) {
            // NEOTKO_MULTIPASS_SURFACES_TAG: use correct prefix per surface
            const bool is_penu = (surface_id != 0);
            const char* np_key = is_penu ? "penultimate_multipass_num_passes" : "multipass_num_passes";
            int n = 2;
            if (auto* o = m_config->option<ConfigOptionInt>(np_key)) n = o->value;
            const char* rk_top[3]  = {"multipass_width_ratio_1","multipass_width_ratio_2","multipass_width_ratio_3"};
            const char* tk_top[3]  = {"multipass_tool_1","multipass_tool_2","multipass_tool_3"};
            const char* rk_penu[3] = {"penultimate_multipass_width_ratio_1","penultimate_multipass_width_ratio_2","penultimate_multipass_width_ratio_3"};
            const char* tk_penu[3] = {"penultimate_multipass_tool_1","penultimate_multipass_tool_2","penultimate_multipass_tool_3"};
            const char** rk = is_penu ? rk_penu : rk_top;
            const char** tk = is_penu ? tk_penu : tk_top;
            const float def_r[3] = {0.5f, 0.5f, 0.34f};
            for (int i = 0; i < std::min(n, 3); ++i) {
                int   t = 0;        if (auto* o = m_config->option<ConfigOptionInt>  (tk[i])) t = o->value;
                float r = def_r[i]; if (auto* o = m_config->option<ConfigOptionFloat>(rk[i])) r = o->value;
                passes.push_back({t, r});
            }
        } else if (eff == EFF_PB) {
            // PathBlend: equal-weight preview (actual ratios are dynamic at runtime).
            int n = 2;
            if (auto* o = m_config->option<ConfigOptionInt>("pathblend_num_passes")) n = o->value;
            n = std::clamp(n, 1, 4);
            const char* tk[4] = {"pathblend_tool_1","pathblend_tool_2","pathblend_tool_3","pathblend_tool_4"};
            const float equal_r = 1.0f / static_cast<float>(n);
            for (int i = 0; i < n; ++i) {
                int t = i; if (auto* o = m_config->option<ConfigOptionInt>(tk[i])) t = o->value;
                passes.push_back({t, equal_r});
            }
        }

        // Beer-Lambert weighted blend: weight_i = opacity_i = 1 - 0.1^(ratio_i/TD_i)
        // Opaque filament (low TD) → high opacity → dominates the blend (physically correct).
        float tr = 0, tg = 0, tb = 0, tw = 0;
        for (auto& p : passes) {
            int t = std::max(0, std::min(3, p.tool_0));
            float td = m_td[t];
            wxColour col = t < (int)m_fcolors.size() ? hex_to_col(m_fcolors[t]) : wxColour(128, 128, 128);
            const float opacity = (td < 1e-6f)
                ? 1.0f
                : 1.0f - static_cast<float>(std::pow(0.1, p.ratio / td));
            float w = opacity;
            tr += col.Red()   * w;
            tg += col.Green() * w;
            tb += col.Blue()  * w;
            tw += w;
        }
        if (tw < 1e-6f) {
            if (out_weight) *out_weight = 0.f;
            return wxColour(180, 180, 180);
        }
        if (out_weight) *out_weight = tw;
        return wxColour(
            static_cast<unsigned char>(std::min(255.f, tr / tw)),
            static_cast<unsigned char>(std::min(255.f, tg / tw)),
            static_cast<unsigned char>(std::min(255.f, tb / tw)));
    }

    // NEOTKO_MULTIPASS_SURFACES_TAG — Beer-Lambert stacked preview (Top over Penultimate)
    // opacity_top = Σ opacity_i where opacity_i = 1 - 0.1^(ratio_i/TD_i)
    // opacity_top=1 → top fully opaque (only top visible)
    // opacity_top=0 → top fully translucent (only penu visible)
    wxColour stacked_preview() const
    {
        float opacity_top = 0.f;
        const wxColour c_top  = blend_preview(0, &opacity_top);
        const wxColour c_penu = blend_preview(1);
        const float op  = std::clamp(opacity_top, 0.f, 1.f);
        const float tr  = 1.f - op;
        return wxColour(
            static_cast<unsigned char>(c_top.Red()   * op + c_penu.Red()   * tr),
            static_cast<unsigned char>(c_top.Green() * op + c_penu.Green() * tr),
            static_cast<unsigned char>(c_top.Blue()  * op + c_penu.Blue()  * tr));
    }

    // ------------------------------------------------------------------ UI update

    void style_pills(wxButton* pills[4], int eff)
    {
        const wxColour active_bg(60, 120, 220);
        const wxColour active_fg(*wxWHITE);
        const wxColour normal_bg(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE));
        const wxColour normal_fg(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNTEXT));
        for (int i = 0; i < 4; ++i) {
            if (!pills[i]) continue;
            const bool on = (i == eff);
            pills[i]->SetBackgroundColour(on ? active_bg : normal_bg);
            pills[i]->SetForegroundColour(on ? active_fg : normal_fg);
            pills[i]->Refresh();
        }
    }

    void update_ui()
    {
        style_pills(m_pill_top,  m_top_eff);
        style_pills(m_pill_penu, m_penu_eff);

        if (m_btn_adv_top)  m_btn_adv_top ->Enable(m_top_eff  != EFF_NONE);
        if (m_btn_adv_penu) m_btn_adv_penu->Enable(m_penu_eff != EFF_NONE);

        if (m_lbl_top_sum)  { m_lbl_top_sum ->SetLabel(summary_for(m_top_eff,  0)); m_lbl_top_sum ->Wrap(220); }
        if (m_lbl_penu_sum) { m_lbl_penu_sum->SetLabel(summary_for(m_penu_eff, 1)); m_lbl_penu_sum->Wrap(220); }

        if (m_prev_top)  m_prev_top ->Refresh();
        if (m_prev_penu) m_prev_penu->Refresh();
        // NEOTKO_MULTIPASS_SURFACES_TAG — refresh stacked preview
        {
            float opacity_top = 0.f;
            blend_preview(0, &opacity_top);
            if (m_stacked_sw_top)  m_stacked_sw_top ->Refresh();
            if (m_stacked_sw_penu) m_stacked_sw_penu->Refresh();
            if (m_stacked_swatch)  m_stacked_swatch ->Refresh();
            if (m_lbl_opacity_top)
                m_lbl_opacity_top->SetLabel(
                    wxString::Format(_L("opacity=%.2f"), std::clamp(opacity_top, 0.f, 1.f)));
        }

        Layout();
    }

    // ------------------------------------------------------------------ sub-dialog launchers

    void open_edit_for(int surface_id)
    {
        const int eff = (surface_id == 0) ? m_top_eff : m_penu_eff;

        if (eff == EFF_CM) {
            std::string mixed_defs;
            if (auto* o = m_config->option<ConfigOptionString>("mixed_filament_definitions"))
                mixed_defs = o->value;
            const auto options = Slic3r::SurfaceColorMix::get_mix_options(mixed_defs, m_fcolors);
            // Each surface has its own independent pattern.
            // surface_id 0 = Top, 1 = Penultimate.
            const char* pat_key = (surface_id == 0)
                ? "interlayer_colormix_pattern_top"
                : "interlayer_colormix_pattern_penultimate";
            const std::string cur_pat = m_config->opt_string(pat_key);
            bool use_virtual = false;
            if (auto* o = m_config->option<ConfigOptionBool>("interlayer_colormix_use_virtual"))
                use_virtual = o->value;
            ColorMixPatternDialog dlg(this, options, m_fcolors, cur_pat, use_virtual,
                                       m_config, surface_id);
            if (dlg.ShowModal() == wxID_OK) {
                if (auto* o = m_config->option<ConfigOptionString>(pat_key))
                    o->value = dlg.get_pattern();
                m_on_change(pat_key);

                // NEOTKO_COLORMIX_TAG — s61: per-role gradient write-back.
                // surface_id 0 (Top) → original "interlayer_colormix_*" keys.
                // surface_id 1 (Penu) → "interlayer_colormix_penu_*" mirrors.
                // The dialog already read+edited the right set via grad_key();
                // here we write back using the same prefix so Top and Pen never
                // bleed into each other.
                const std::string grad_prefix = (surface_id == 1)
                    ? std::string("interlayer_colormix_penu_")
                    : std::string("interlayer_colormix_");
                auto wi = [&](const std::string& k, int v) {
                    if (auto* o = m_config->option<ConfigOptionInt>(k)) o->value = v;
                    m_on_change(k);
                };
                auto wf = [&](const std::string& k, double v) {
                    if (auto* o = m_config->option<ConfigOptionFloat>(k)) o->value = v;
                    m_on_change(k);
                };
                wi(grad_prefix + "mode",              dlg.get_grad_mode());
                wi(grad_prefix + "pct_a",             dlg.get_grad_pct_a());
                // NEOTKO_COLORMIX_TAG — s90: global key (no prefix). Both
                // Top and Penu Advanced dialogs write the same value here.
                wf("interlayer_colormix_min_length",  dlg.get_grad_min_length());
                wi(grad_prefix + "pct_b",             dlg.get_grad_pct_b());
                wi(grad_prefix + "easing",            dlg.get_grad_easing());
                wf(grad_prefix + "gamma",             dlg.get_grad_gamma());
                wi(grad_prefix + "min_surface_lines", dlg.get_grad_min_lines());
                wf(grad_prefix + "overlap",           dlg.get_grad_overlap());
                {
                    const std::string k_inv = grad_prefix + "invert";
                    if (auto* o = m_config->option<ConfigOptionBool>(k_inv))
                        o->value = dlg.get_grad_invert();
                    m_on_change(k_inv);
                }
                wi(grad_prefix + "band_count_a",      dlg.get_grad_band_a());
                wi(grad_prefix + "band_count_b",      dlg.get_grad_band_b());
                wi(grad_prefix + "band_count_c",      dlg.get_grad_band_c());
                wi(grad_prefix + "band_count_d",      dlg.get_grad_band_d());
                wi(grad_prefix + "tool_a",            dlg.get_tool_a());
                wi(grad_prefix + "tool_b",            dlg.get_tool_b());
                wi(grad_prefix + "tool_c",            dlg.get_tool_c());
                wi(grad_prefix + "tool_d",            dlg.get_tool_d());
                wi(grad_prefix + "angle",             dlg.get_grad_angle());
                // interlayer_colormix_surface is set by the outer dialog combos, not here.
            }
        } else if (eff == EFF_MP) {
            int    cur_passes=2, cur_surface=0;
            int    cur_tool1=0, cur_tool2=1, cur_tool3=-1;
            double cur_r1=0.5, cur_r2=0.5, cur_r3=0.34;
            bool   cur_vary=false;
            int    cur_a1=-1, cur_a2=-1, cur_a3=-1;
            int    cur_f1=-1, cur_f2=-1, cur_f3=-1;
            int    cur_s1=100, cur_s2=100, cur_s3=100;
            std::string gs1,gs2,gs3,ge1,ge2,ge3;
            int    pa_mode=0; double pa_val=0.1;

            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_num_passes"))    cur_passes  =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_surface"))       cur_surface =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_tool_1"))        cur_tool1   =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_tool_2"))        cur_tool2   =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_tool_3"))        cur_tool3   =o->value;
            if (auto* o=m_config->option<ConfigOptionFloat>("multipass_width_ratio_1")) cur_r1=(double)o->value;
            if (auto* o=m_config->option<ConfigOptionFloat>("multipass_width_ratio_2")) cur_r2=(double)o->value;
            if (auto* o=m_config->option<ConfigOptionFloat>("multipass_width_ratio_3")) cur_r3=(double)o->value;
            if (auto* o=m_config->option<ConfigOptionBool> ("multipass_vary_pattern"))  cur_vary    =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_angle_1"))       cur_a1      =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_angle_2"))       cur_a2      =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_angle_3"))       cur_a3      =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_fan_1"))         cur_f1      =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_fan_2"))         cur_f2      =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_fan_3"))         cur_f3      =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_speed_pct_1"))   cur_s1      =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_speed_pct_2"))   cur_s2      =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_speed_pct_3"))   cur_s3      =o->value;
            if (auto* o=m_config->option<ConfigOptionString>("multipass_gcode_start_1")) gs1        =o->value;
            if (auto* o=m_config->option<ConfigOptionString>("multipass_gcode_start_2")) gs2        =o->value;
            if (auto* o=m_config->option<ConfigOptionString>("multipass_gcode_start_3")) gs3        =o->value;
            if (auto* o=m_config->option<ConfigOptionString>("multipass_gcode_end_1"))   ge1        =o->value;
            if (auto* o=m_config->option<ConfigOptionString>("multipass_gcode_end_2"))   ge2        =o->value;
            if (auto* o=m_config->option<ConfigOptionString>("multipass_gcode_end_3"))   ge3        =o->value;
            if (auto* o=m_config->option<ConfigOptionInt>  ("multipass_pa_mode"))        pa_mode    =o->value;
            if (auto* o=m_config->option<ConfigOptionFloat>("multipass_pa_value"))       pa_val=(double)o->value;
            double cur_prime_vol = 0.0; // NEOTKO_MULTIPASS_PRIME_TAG
            if (auto* o=m_config->option<ConfigOptionFloat>("multipass_prime_volume"))   cur_prime_vol=(double)o->value;
            double cur_layer_height = 0.0; // NEOTKO_MULTIPASS_MINLAYER_TAG
            if (auto* o=m_config->option<ConfigOptionFloat>("layer_height"))             cur_layer_height=(double)o->value;

            // NEOTKO_MULTIPASS_TAG_START — build virtual-only ColorMixOption list for "Normalize to MixedColor %"
            std::vector<Slic3r::ColorMixOption> mp_mix_opts;
            {
                std::string mp_mixed_defs;
                if (auto* o = m_config->option<ConfigOptionString>("mixed_filament_definitions"))
                    mp_mixed_defs = o->value;
                if (!mp_mixed_defs.empty()) {
                    auto all = Slic3r::SurfaceColorMix::get_mix_options(mp_mixed_defs, m_fcolors);
                    for (auto& opt : all)
                        if (!opt.is_physical)
                            mp_mix_opts.push_back(opt);
                }
            }
            // NEOTKO_MULTIPASS_TAG_END

            // NEOTKO_MULTIPASS_SURFACES_TAG — read penultimate independent config
            // NEOTKO_MULTIPASS_SURFACES_TAG — read penultimate independent config
            bool   penu_enabled_cur = false;
            int    penu_passes_cur  = 2;
            int    penu_t1_cur=0, penu_t2_cur=1, penu_t3_cur=-1;
            double penu_r1_cur=0.5, penu_r2_cur=0.5, penu_r3_cur=0.34;
            int    penu_a1_cur=-1, penu_a2_cur=-1, penu_a3_cur=-1;
            int    penu_f1_cur=-1, penu_f2_cur=-1, penu_f3_cur=-1;
            int    penu_s1_cur=100, penu_s2_cur=100, penu_s3_cur=100;
            bool   penu_vary_cur=false;
            std::string penu_gs1_cur,penu_gs2_cur,penu_gs3_cur;
            std::string penu_ge1_cur,penu_ge2_cur,penu_ge3_cur;
            double penu_prime_cur=0.0;
            if (auto*o=m_config->option<ConfigOptionBool>  ("penultimate_multipass_enabled"))         penu_enabled_cur=o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_num_passes"))      penu_passes_cur =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_tool_1"))          penu_t1_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_tool_2"))          penu_t2_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_tool_3"))          penu_t3_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionFloat> ("penultimate_multipass_width_ratio_1"))   penu_r1_cur=(double)o->value;
            if (auto*o=m_config->option<ConfigOptionFloat> ("penultimate_multipass_width_ratio_2"))   penu_r2_cur=(double)o->value;
            if (auto*o=m_config->option<ConfigOptionFloat> ("penultimate_multipass_width_ratio_3"))   penu_r3_cur=(double)o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_angle_1"))         penu_a1_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_angle_2"))         penu_a2_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_angle_3"))         penu_a3_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_fan_1"))           penu_f1_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_fan_2"))           penu_f2_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_fan_3"))           penu_f3_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_speed_pct_1"))     penu_s1_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_speed_pct_2"))     penu_s2_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionInt>   ("penultimate_multipass_speed_pct_3"))     penu_s3_cur     =o->value;
            if (auto*o=m_config->option<ConfigOptionBool>  ("penultimate_multipass_vary_pattern"))    penu_vary_cur   =o->value;
            if (auto*o=m_config->option<ConfigOptionString>("penultimate_multipass_gcode_start_1"))   penu_gs1_cur    =o->value;
            if (auto*o=m_config->option<ConfigOptionString>("penultimate_multipass_gcode_start_2"))   penu_gs2_cur    =o->value;
            if (auto*o=m_config->option<ConfigOptionString>("penultimate_multipass_gcode_start_3"))   penu_gs3_cur    =o->value;
            if (auto*o=m_config->option<ConfigOptionString>("penultimate_multipass_gcode_end_1"))     penu_ge1_cur    =o->value;
            if (auto*o=m_config->option<ConfigOptionString>("penultimate_multipass_gcode_end_2"))     penu_ge2_cur    =o->value;
            if (auto*o=m_config->option<ConfigOptionString>("penultimate_multipass_gcode_end_3"))     penu_ge3_cur    =o->value;
            // NEOTKO_MULTIPASS_PRIME_TAG — s58: penultimate_multipass_prime_volume removed.
            // The unified `multipass_prime_volume` is now read into both cur_prime_vol and
            // penu_prime_cur so the (vestigial) dialog parameter still has a sensible value
            // until the dialog's prime field is fully cleaned up.
            penu_prime_cur = cur_prime_vol;

            const SurfaceZone zone = (surface_id == 0) ? SurfaceZone::TOP : SurfaceZone::PENULTIMATE;
            MultiPassConfigDialog dlg(this,
                zone,
                cur_passes, cur_surface,
                cur_tool1, cur_tool2, cur_tool3,
                cur_r1, cur_r2, cur_r3, cur_vary,
                cur_a1, cur_a2, cur_a3,
                cur_f1, cur_f2, cur_f3,
                cur_s1, cur_s2, cur_s3,
                gs1, gs2, gs3, ge1, ge2, ge3,
                m_fcolors, mp_mix_opts, pa_mode, pa_val,
                cur_prime_vol, cur_layer_height,
                penu_enabled_cur, penu_passes_cur,
                penu_t1_cur, penu_t2_cur, penu_t3_cur,
                penu_r1_cur, penu_r2_cur, penu_r3_cur,
                penu_a1_cur, penu_a2_cur, penu_a3_cur,
                penu_f1_cur, penu_f2_cur, penu_f3_cur,
                penu_s1_cur, penu_s2_cur, penu_s3_cur,
                penu_vary_cur,
                penu_gs1_cur, penu_gs2_cur, penu_gs3_cur,
                penu_ge1_cur, penu_ge2_cur, penu_ge3_cur,
                penu_prime_cur);
            if (dlg.ShowModal() == wxID_OK) {
                auto wi = [&](const char* k, int v)  { if(auto*o=m_config->option<ConfigOptionInt>  (k))o->value=v;   m_on_change(k); };
                auto wb = [&](const char* k, bool v) { if(auto*o=m_config->option<ConfigOptionBool> (k))o->value=v;   m_on_change(k); };
                auto wf = [&](const char* k, float v){ if(auto*o=m_config->option<ConfigOptionFloat>(k))o->value=v;   m_on_change(k); };
                auto ws = [&](const char* k, const std::string& v){ if(auto*o=m_config->option<ConfigOptionString>(k))o->value=v; m_on_change(k); };
                if (surface_id == 0) {
                    // TOP zone: write top surface keys only
                    wi("multipass_num_passes",  dlg.get_passes());
                    wi("multipass_surface",     dlg.get_surface());
                    wi("multipass_tool_1",      dlg.get_tool1());
                    wi("multipass_tool_2",      dlg.get_tool2());
                    wi("multipass_tool_3",      dlg.get_tool3());
                    wf("multipass_width_ratio_1", (float)dlg.get_ratio1());
                    wf("multipass_width_ratio_2", (float)dlg.get_ratio2());
                    wf("multipass_width_ratio_3", (float)dlg.get_ratio3());
                    wb("multipass_vary_pattern",dlg.get_vary());
                    wi("multipass_angle_1",     dlg.get_angle1());
                    wi("multipass_angle_2",     dlg.get_angle2());
                    wi("multipass_angle_3",     dlg.get_angle3());
                    wi("multipass_fan_1",       dlg.get_fan1());
                    wi("multipass_fan_2",       dlg.get_fan2());
                    wi("multipass_fan_3",       dlg.get_fan3());
                    wi("multipass_speed_pct_1", dlg.get_speed1());
                    wi("multipass_speed_pct_2", dlg.get_speed2());
                    wi("multipass_speed_pct_3", dlg.get_speed3());
                    ws("multipass_gcode_start_1", dlg.get_gcode_start1());
                    ws("multipass_gcode_start_2", dlg.get_gcode_start2());
                    ws("multipass_gcode_start_3", dlg.get_gcode_start3());
                    ws("multipass_gcode_end_1",   dlg.get_gcode_end1());
                    ws("multipass_gcode_end_2",   dlg.get_gcode_end2());
                    ws("multipass_gcode_end_3",   dlg.get_gcode_end3());
                    wi("multipass_pa_mode",     dlg.get_pa_mode());
                    wf("multipass_pa_value",    (float)dlg.get_pa_value());
                    // NEOTKO_MULTIPASS_PRIME_TAG — s58: prime is managed by the global
                    // optgroup setting (multipass_prime_volume) below `neotko_wipe_tower`,
                    // not from this dialog. Dialog's get_prime_volume() returns 0 (nullptr
                    // guard), writing it here would clobber the user's global setting.
                } else {
                    // NEOTKO_MULTIPASS_SURFACES_TAG — PENULTIMATE zone: write penu keys only
                    wb("penultimate_multipass_enabled",       dlg.get_penu_enabled());
                    wi("penultimate_multipass_num_passes",    dlg.get_penu_passes());
                    wi("penultimate_multipass_tool_1",        dlg.get_penu_tool1());
                    wi("penultimate_multipass_tool_2",        dlg.get_penu_tool2());
                    wi("penultimate_multipass_tool_3",        dlg.get_penu_tool3());
                    wf("penultimate_multipass_width_ratio_1", (float)dlg.get_penu_ratio1());
                    wf("penultimate_multipass_width_ratio_2", (float)dlg.get_penu_ratio2());
                    wf("penultimate_multipass_width_ratio_3", (float)dlg.get_penu_ratio3());
                    wi("penultimate_multipass_angle_1",       dlg.get_penu_angle1());
                    wi("penultimate_multipass_angle_2",       dlg.get_penu_angle2());
                    wi("penultimate_multipass_angle_3",       dlg.get_penu_angle3());
                    wi("penultimate_multipass_fan_1",         dlg.get_penu_fan1());
                    wi("penultimate_multipass_fan_2",         dlg.get_penu_fan2());
                    wi("penultimate_multipass_fan_3",         dlg.get_penu_fan3());
                    wi("penultimate_multipass_speed_pct_1",   dlg.get_penu_speed1());
                    wi("penultimate_multipass_speed_pct_2",   dlg.get_penu_speed2());
                    wi("penultimate_multipass_speed_pct_3",   dlg.get_penu_speed3());
                    wb("penultimate_multipass_vary_pattern",  dlg.get_penu_vary());
                    ws("penultimate_multipass_gcode_start_1", dlg.get_penu_gcode_start1());
                    ws("penultimate_multipass_gcode_start_2", dlg.get_penu_gcode_start2());
                    ws("penultimate_multipass_gcode_start_3", dlg.get_penu_gcode_start3());
                    ws("penultimate_multipass_gcode_end_1",   dlg.get_penu_gcode_end1());
                    ws("penultimate_multipass_gcode_end_2",   dlg.get_penu_gcode_end2());
                    ws("penultimate_multipass_gcode_end_3",   dlg.get_penu_gcode_end3());
                    // NEOTKO_MULTIPASS_PRIME_TAG — s58: penultimate_multipass_prime_volume
                    // removed; the global `multipass_prime_volume` covers Top + Penu.
                }
            }
        } else if (eff == EFF_PB) {
            // NEOTKO_SANDWICH_TAG — Fase 5 (s72): the PathBlend popup
            // (PathBlendDialog) is gone. PathBlend is now edited through the
            // Sandwich editor with draggable floor/mid_end heights in mm and
            // Half / Full mode selection on the row. The pill stays here for
            // back-compat of the old SurfaceColorMixerDialog: clicking it just
            // redirects the user to the new editor.
            wxMessageBox(_L("PathBlend is now edited from the Sandwich editor.\n\n"
                            "Open the Sandwich editor and choose 'PathBlend Half' "
                            "or 'PathBlend Full' as the pass kind."),
                         _L("PathBlend moved"),
                         wxOK | wxICON_INFORMATION, this);
        }
        update_ui();
    }

    // ------------------------------------------------------------------ apply

    void apply_effect_config()
    {
        // m_top_eff and m_penu_eff are set by pill click handlers — read directly.

        // ColorMix
        const bool cm_en = (m_top_eff == EFF_CM || m_penu_eff == EFF_CM);
        int cm_srf = 0;
        if (m_top_eff == EFF_CM && m_penu_eff != EFF_CM) cm_srf = 1;
        else if (m_top_eff != EFF_CM && m_penu_eff == EFF_CM) cm_srf = 2;

        if (auto* o = m_config->option<ConfigOptionBool>("interlayer_colormix_enabled")) o->value = cm_en;
        m_on_change("interlayer_colormix_enabled");
        if (auto* o = m_config->option<ConfigOptionInt>("interlayer_colormix_surface")) o->value = cm_srf;
        m_on_change("interlayer_colormix_surface");

        // MultiPass — NEOTKO_MULTIPASS_SURFACES_TAG: Top and Penultimate are now independent.
        // multipass_enabled controls Top Surface only.
        // penultimate_multipass_enabled controls Penultimate independently.
        const bool top_mp  = (m_top_eff  == EFF_MP);
        const bool penu_mp = (m_penu_eff == EFF_MP);

        if (auto* o = m_config->option<ConfigOptionBool>("multipass_enabled"))          o->value = top_mp;
        m_on_change("multipass_enabled");
        // multipass_surface is always 1 (top only) since penu has its own key
        if (top_mp) {
            if (auto* o = m_config->option<ConfigOptionInt>("multipass_surface"))       o->value = 1;
            m_on_change("multipass_surface");
        }
        if (auto* o = m_config->option<ConfigOptionBool>("penultimate_multipass_enabled")) o->value = penu_mp;
        m_on_change("penultimate_multipass_enabled");
        // NEOTKO_MULTIPASS_TAG — s120: el checkbox legacy "MultiPass Perimeter
        // Override" se retiró junto al panel Blend Suggestion. El perim override
        // ahora es el unificado de s118 (m_ui[z].perim_chk → stack → wb en la
        // escritura normalizada). GUARD: si el checkbox ya no existe, NO se
        // escribe aquí — de lo contrario clobbearía a `false` el valor canónico
        // que deriva del stack.
        if (m_mp_perim_override) {
            if (auto* o = m_config->option<ConfigOptionBool>("multipass_perimeter_override"))
                o->value = m_mp_perim_override->GetValue();
            m_on_change("multipass_perimeter_override");
        }

        // PathBlend (EFF_PB only — independent surface key, independent enable)
        // NEOTKO_PROFILE_TAG — s68: PB on Penu re-enabled.
        const bool top_pb  = (m_top_eff  == EFF_PB);
        const bool penu_pb = (m_penu_eff == EFF_PB);
        const bool pb_en   = top_pb || penu_pb;
        int pb_srf = 0;
        if (top_pb && !penu_pb) pb_srf = 1;
        else if (!top_pb && penu_pb) pb_srf = 2;

        if (auto* o = m_config->option<ConfigOptionBool>("multipass_path_gradient")) o->value = pb_en;
        m_on_change("multipass_path_gradient");
        if (auto* o = m_config->option<ConfigOptionInt> ("pathblend_surface"))       o->value = pb_srf;
        m_on_change("pathblend_surface");

        // NEOTKO_COLORMIX_TAG_START — write zone, filament filter, min_length
        {
            auto wi_z = [&](const char* k, int v) {
                if (auto* o = m_config->option<ConfigOptionInt>(k)) o->value = v;
                m_on_change(k);
            };
            auto wf_z = [&](const char* k, double v) {
                if (auto* o = m_config->option<ConfigOptionFloat>(k)) o->value = v;
                m_on_change(k);
            };
            wi_z("interlayer_colormix_top_zone",
                 m_choice_top_zone  ? m_choice_top_zone->GetSelection()  : 0);
            wi_z("interlayer_colormix_penu_zone",
                 m_choice_penu_zone ? m_choice_penu_zone->GetSelection() : 0);
            wi_z("interlayer_colormix_filament_filter",
                 m_sc_filament_filter ? m_sc_filament_filter->GetValue() : 0);
            wf_z("interlayer_colormix_min_length",
                 m_sc_min_length ? m_sc_min_length->GetValue() : 1.0);
            // NEOTKO_COLORMIX_TAG_START — use_virtual write-back
            if (auto* o = m_config->option<ConfigOptionBool>("interlayer_colormix_use_virtual"))
                o->value = m_chk_use_virtual && m_chk_use_virtual->GetValue();
            m_on_change("interlayer_colormix_use_virtual");
            // NEOTKO_COLORMIX_TAG_END

            // NEOTKO_COLORMIX_TAG — s60: numeric gradient write-back was here.
            // Now handled inside ColorMixPatternDialog (sub-dialog opened by
            // the Advanced button per role) where the controls live.
        }
        // NEOTKO_COLORMIX_TAG_END

        // TD values → app_config
        auto* ac = wxGetApp().app_config;
        if (ac) {
            for (int i = 0; i < 4; ++i) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.3f", m_td[i]);
                ac->set("neotko_td_" + std::to_string(i + 1), buf);
            }
        }
    }

    // ------------------------------------------------------------------ mini-preview draw helpers

    void draw_none_preview(wxPaintDC& dc, wxSize sz)
    {
        dc.SetBackground(wxBrush(wxColour(48, 48, 48)));
        dc.Clear();
        dc.SetPen(wxPen(wxColour(72, 72, 72), 1));
        for (int x = -sz.y; x < sz.x + sz.y; x += 8)
            dc.DrawLine(x, 0, x + sz.y, sz.y);
    }

    void draw_colormix_preview(wxPaintDC& dc, int surface_id, wxSize sz)
    {
        dc.SetBackground(wxBrush(wxColour(52, 52, 52)));
        dc.Clear();
        const std::string pat = (surface_id == 0)
            ? m_config->opt_string("interlayer_colormix_pattern_top")
            : m_config->opt_string("interlayer_colormix_pattern_penultimate");
        if (pat.empty()) return;
        const int n = static_cast<int>(pat.size());
        const int bw = std::max(4, sz.x / n);
        int x = 0;
        for (char c : pat) {
            const int t = static_cast<int>(c - '1');
            wxColour col(120, 120, 120);
            if (t >= 0 && t < (int)m_fcolors.size() && !m_fcolors[t].empty())
                col = hex_to_col(m_fcolors[t]);
            dc.SetBrush(wxBrush(col));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(x, 0, bw, sz.y);
            x += bw;
            if (x >= sz.x) break;
        }
    }

    void draw_multipass_preview(wxPaintDC& dc, int surface_id, wxSize sz)
    {
        dc.SetBackground(wxBrush(wxColour(48, 48, 48)));
        dc.Clear();
        const bool is_penu = (surface_id != 0);
        const char* np_key = is_penu ? "penultimate_multipass_num_passes" : "multipass_num_passes";
        int n = 2;
        if (auto* o = m_config->option<ConfigOptionInt>(np_key)) n = o->value;
        n = std::clamp(n, 1, 3);

        const char* tk_top[3]  = {"multipass_tool_1","multipass_tool_2","multipass_tool_3"};
        const char* rk_top[3]  = {"multipass_width_ratio_1","multipass_width_ratio_2","multipass_width_ratio_3"};
        const char* tk_penu[3] = {"penultimate_multipass_tool_1","penultimate_multipass_tool_2","penultimate_multipass_tool_3"};
        const char* rk_penu[3] = {"penultimate_multipass_width_ratio_1","penultimate_multipass_width_ratio_2","penultimate_multipass_width_ratio_3"};
        const char** tk = is_penu ? tk_penu : tk_top;
        const char** rk = is_penu ? rk_penu : rk_top;
        const float def_r[3] = {0.5f, 0.5f, 0.34f};

        float ratios[3] = {};
        float sum = 0.f;
        for (int i = 0; i < n; ++i) {
            ratios[i] = def_r[i];
            if (auto* o = m_config->option<ConfigOptionFloat>(rk[i])) ratios[i] = o->value;
            sum += ratios[i];
        }
        if (sum < 1e-6f) sum = 1.f;

        // NEOTKO_MULTIPASS_TAG — s58 UX fix:
        // Draw passes in reverse so pass 0 (printed FIRST, physically at the
        // bottom of the layer) appears at the BOTTOM of the preview, and the
        // last pass (printed last, physically on top) appears at the TOP.
        // Previously the preview was inverted vs reality.
        int y = 0;
        for (int i = n - 1; i >= 0; --i) {
            int t = 0;
            if (auto* o = m_config->option<ConfigOptionInt>(tk[i])) t = o->value;
            wxColour col(120, 120, 120);
            if (t >= 0 && t < (int)m_fcolors.size() && !m_fcolors[t].empty())
                col = hex_to_col(m_fcolors[t]);
            // Last drawn (i==0, bottom-most band) fills the remaining height.
            const int band_h = (i > 0)
                ? static_cast<int>(ratios[i] / sum * sz.y + 0.5f)
                : sz.y - y;
            dc.SetBrush(wxBrush(col));
            dc.SetPen(wxPen(wxColour(28, 28, 28), 1));
            dc.DrawRectangle(0, y, sz.x, std::max(1, band_h));
            y += band_h;
        }
    }

    void draw_pathblend_preview(wxPaintDC& dc, wxSize sz)
    {
        dc.SetBackground(wxBrush(wxColour(48, 48, 48)));
        dc.Clear();
        int t0 = 0, t1 = 1;
        if (auto* o = m_config->option<ConfigOptionInt>("pathblend_tool_1")) t0 = o->value;
        if (auto* o = m_config->option<ConfigOptionInt>("pathblend_tool_2")) t1 = o->value;
        wxColour col0(80, 80, 160), col1(160, 80, 80);
        if (t0 >= 0 && t0 < (int)m_fcolors.size() && !m_fcolors[t0].empty())
            col0 = hex_to_col(m_fcolors[t0]);
        if (t1 >= 0 && t1 < (int)m_fcolors.size() && !m_fcolors[t1].empty())
            col1 = hex_to_col(m_fcolors[t1]);

        const int N = 8;
        const int col_w = (sz.x + N - 1) / N;
        for (int i = 0; i < N; ++i) {
            const double t = static_cast<double>(i) / (N - 1);
            const int x   = i * col_w;
            const int h1  = static_cast<int>(t * sz.y + 0.5);
            const int h0  = sz.y - h1;
            dc.SetPen(*wxTRANSPARENT_PEN);
            if (h1 > 0) {
                dc.SetBrush(wxBrush(col1));
                dc.DrawRectangle(x, 0, col_w, h1);
            }
            if (h0 > 0) {
                dc.SetBrush(wxBrush(col0));
                dc.DrawRectangle(x, h1, col_w, h0);
            }
        }
    }

    void draw_effect_preview(wxPaintDC& dc, int surface_id, int eff, wxSize sz)
    {
        switch (eff) {
            case EFF_CM: draw_colormix_preview(dc, surface_id, sz); break;
            case EFF_MP: draw_multipass_preview(dc, surface_id, sz); break;
            case EFF_PB: draw_pathblend_preview(dc, sz); break;
            default:     draw_none_preview(dc, sz); break;
        }
    }

    // ------------------------------------------------------------------ zone card factory

    wxStaticBoxSizer* create_zone_card(int surface_id)
    {
        const int PAD = 6;
        const wxString title = (surface_id == 0) ? _L("Top layer") : _L("Penultimate layer");
        auto* sb = new wxStaticBoxSizer(wxVERTICAL, this, title);

        // --- Pills row ---
        const wxString pill_labels[4] = {_L("None"), _L("ColorStitch"), _L("MultiPass"), _L("PathBlend")};
        wxButton** pills = (surface_id == 0) ? m_pill_top : m_pill_penu;
        const int cur_eff = (surface_id == 0) ? m_top_eff : m_penu_eff;

        auto* pill_row = new wxBoxSizer(wxHORIZONTAL);
        for (int i = 0; i < 4; ++i) {
            pills[i] = new wxButton(this, wxID_ANY, pill_labels[i],
                                    wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
            pills[i]->Bind(wxEVT_BUTTON, [this, surface_id, i](wxCommandEvent&) {
                if (surface_id == 0) m_top_eff  = i;
                else                  m_penu_eff = i;
                update_ui();
            });
            // NEOTKO_PROFILE_TAG — s68: Penu PB pill re-enabled. The engine
            // (PathBlendEngine::apply_path) is now role-aware so penu PB+MP
            // stacks on the penultimate sub-layers, not the top stack.
            pill_row->Add(pills[i], 0, wxRIGHT, 3);
        }
        sb->Add(pill_row, 0, wxALL, PAD);
        style_pills(pills, cur_eff);

        // --- Content row: mini-preview + right column ---
        auto* content_row = new wxBoxSizer(wxHORIZONTAL);

        // Mini-preview panel 120×68
        auto* mini = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(120, 68));
        mini->SetBackgroundStyle(wxBG_STYLE_PAINT);
        mini->Bind(wxEVT_PAINT, [this, mini, surface_id](wxPaintEvent&) {
            wxPaintDC dc(mini);
            const int eff = (surface_id == 0) ? m_top_eff : m_penu_eff;
            draw_effect_preview(dc, surface_id, eff, mini->GetSize());
        });
        if (surface_id == 0) m_prev_top  = mini;
        else                  m_prev_penu = mini;
        content_row->Add(mini, 0, wxRIGHT, PAD);

        // Right column: summary + zone selector + Advanced button
        auto* right_col = new wxBoxSizer(wxVERTICAL);

        auto* lbl_sum = new wxStaticText(this, wxID_ANY, summary_for(cur_eff, surface_id),
                                          wxDefaultPosition, wxSize(200, -1));
        lbl_sum->SetForegroundColour(wxColour(90, 90, 90));
        lbl_sum->Wrap(200);
        if (surface_id == 0) m_lbl_top_sum  = lbl_sum;
        else                  m_lbl_penu_sum = lbl_sum;
        right_col->Add(lbl_sum, 1, wxEXPAND | wxBOTTOM, 4);

        // NEOTKO_COLORMIX_TAG_START — zone selector per surface
        {
            const char* zone_key = (surface_id == 0)
                ? "interlayer_colormix_top_zone"
                : "interlayer_colormix_penu_zone";
            int cur_zone = 0;
            if (auto* o = m_config->option<ConfigOptionInt>(zone_key)) cur_zone = o->value;
            cur_zone = std::clamp(cur_zone, 0, 1);

            auto* zone_row = new wxBoxSizer(wxHORIZONTAL);
            zone_row->Add(new wxStaticText(this, wxID_ANY, _L("Zone:")),
                          0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            auto* zone_ch = new wxChoice(this, wxID_ANY);
            zone_ch->Append(_L("All surfaces"));
            zone_ch->Append(_L("Topmost only"));
            zone_ch->SetSelection(cur_zone);
            zone_ch->SetToolTip(surface_id == 0
                ? _L("All top surfaces — or only the single topmost top surface of the object.")
                : _L("All penultimate surfaces — or only the topmost penultimate surface of the object."));
            zone_row->Add(zone_ch, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
            right_col->Add(zone_row, 0, wxEXPAND | wxBOTTOM, 4);

            if (surface_id == 0) m_choice_top_zone  = zone_ch;
            else                 m_choice_penu_zone = zone_ch;
        }
        // NEOTKO_COLORMIX_TAG_END

        auto* btn_adv = new wxButton(this, wxID_ANY, _L("Advanced\u2026"),
                                      wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        btn_adv->Enable(cur_eff != EFF_NONE);
        btn_adv->Bind(wxEVT_BUTTON, [this, surface_id](wxCommandEvent&) {
            open_edit_for(surface_id);
        });
        if (surface_id == 0) m_btn_adv_top  = btn_adv;
        else                  m_btn_adv_penu = btn_adv;
        right_col->Add(btn_adv, 0, wxALIGN_RIGHT);

        content_row->Add(right_col, 1, wxEXPAND);
        sb->Add(content_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);

        return sb;
    }

    // ------------------------------------------------------------------ filament section factory

    wxStaticBoxSizer* create_filament_section()
    {
        const int PAD = 6;
        auto* sb = new wxStaticBoxSizer(wxVERTICAL, this, _L("Filament"));

        // TD note
        auto* td_note = new wxStaticText(this, wxID_ANY,
            _L("TD (Tinting Density): low=opaque (0.1-0.5), mid=semi-opaque (0.5-3), high=translucent (3-10+). Used for blend preview only."));
        td_note->SetForegroundColour(wxColour(90, 90, 90));
        sb->Add(td_note, 0, wxALL, PAD);

        // TD sliders grid
        auto* td_grid = new wxFlexGridSizer(4, 4, 4, 8);
        td_grid->AddGrowableCol(2, 1);
        for (int i = 0; i < 4; ++i) {
            auto* sw = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(18, 18));
            sw->SetBackgroundColour(i < (int)m_fcolors.size()
                ? hex_to_col(m_fcolors[i]) : wxColour(128, 128, 128));
            td_grid->Add(sw, 0, wxALIGN_CENTER_VERTICAL);

            td_grid->Add(new wxStaticText(this, wxID_ANY, wxString::Format("T%d", i + 1)),
                         0, wxALIGN_CENTER_VERTICAL);

            const int iv = static_cast<int>(m_td[i] * 100.f + 0.5f);
            auto* sl = new wxSlider(this, wxID_ANY, iv, 1, 1000,
                                     wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
            m_sl_td[i] = sl;
            td_grid->Add(sl, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

            auto* lbl = new wxStaticText(this, wxID_ANY,
                wxString::Format("%.2f", m_td[i]),
                wxDefaultPosition, wxSize(38, -1));
            m_lbl_td[i] = lbl;
            td_grid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);

            sl->Bind(wxEVT_SLIDER, [this, i](wxCommandEvent&) {
                m_td[i] = m_sl_td[i]->GetValue() / 100.f;
                m_lbl_td[i]->SetLabel(wxString::Format("%.2f", m_td[i]));
                if (m_prev_top)  m_prev_top ->Refresh();
                if (m_prev_penu) m_prev_penu->Refresh();
                // NEOTKO_MULTIPASS_SURFACES_TAG — refresh stacked preview
                if (m_stacked_swatch) {
                    float transmit_top = 0.f;
                    blend_preview(0, &transmit_top);
                    m_stacked_swatch->Refresh();
                    if (m_stacked_sw_top)  m_stacked_sw_top ->Refresh();
                    if (m_stacked_sw_penu) m_stacked_sw_penu->Refresh();
                    if (m_lbl_opacity_top)
                        m_lbl_opacity_top->SetLabel(
                            wxString::Format(_L("transmit=%.2f"), std::clamp(transmit_top, 0.f, 1.f)));
                }
            });
        }
        sb->Add(td_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);

        // NEOTKO_MULTIPASS_SURFACES_TAG — stacked preview row (Top × Penu Beer-Lambert)
        {
            auto* row_st = new wxBoxSizer(wxHORIZONTAL);
            auto* lbl_t = new wxStaticText(this, wxID_ANY, _L("Top:"));
            m_stacked_sw_top = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(28, 18));
            m_stacked_sw_top->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_stacked_sw_top->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxPaintDC dc(m_stacked_sw_top);
                dc.SetBackground(wxBrush(blend_preview(0)));
                dc.Clear();
            });
            auto* lbl_p = new wxStaticText(this, wxID_ANY, _L("  Penu:"));
            m_stacked_sw_penu = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(28, 18));
            m_stacked_sw_penu->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_stacked_sw_penu->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxPaintDC dc(m_stacked_sw_penu);
                dc.SetBackground(wxBrush(blend_preview(1)));
                dc.Clear();
            });
            auto* lbl_r = new wxStaticText(this, wxID_ANY, _L("  Result:"));
            m_stacked_swatch = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(40, 18));
            m_stacked_swatch->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_stacked_swatch->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxPaintDC dc(m_stacked_swatch);
                dc.SetBackground(wxBrush(stacked_preview()));
                dc.Clear();
            });
            m_lbl_opacity_top = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(90, -1));
            m_lbl_opacity_top->SetForegroundColour(wxColour(90, 90, 90));
            row_st->Add(lbl_t,          0, wxALIGN_CENTER_VERTICAL);
            row_st->Add(m_stacked_sw_top,  0, wxALIGN_CENTER_VERTICAL|wxLEFT, 4);
            row_st->Add(lbl_p,          0, wxALIGN_CENTER_VERTICAL);
            row_st->Add(m_stacked_sw_penu, 0, wxALIGN_CENTER_VERTICAL|wxLEFT, 4);
            row_st->Add(lbl_r,   0, wxALIGN_CENTER_VERTICAL);
            row_st->Add(m_stacked_swatch,    0, wxALIGN_CENTER_VERTICAL|wxLEFT, 4);
            row_st->Add(m_lbl_opacity_top,   0, wxALIGN_CENTER_VERTICAL|wxLEFT, 6);
            sb->Add(row_st, 0, wxLEFT|wxRIGHT|wxBOTTOM, PAD);
        }
        // NEOTKO_MULTIPASS_SURFACES_TAG_END

        // "More" toggle — shows filament filter / min_length / use_virtual
        auto* more_btn = new wxButton(this, wxID_ANY, _L("\u25b6 More"),
                                       wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        sb->Add(more_btn, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);

        auto* more_panel = new wxPanel(this, wxID_ANY);
        auto* more_vs    = new wxBoxSizer(wxVERTICAL);

        // NEOTKO_COLORMIX_TAG_START — filament filter + min_length + use_virtual
        {
            int cur_ff = 0;
            if (auto* o = m_config->option<ConfigOptionInt>("interlayer_colormix_filament_filter"))
                cur_ff = o->value;
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(more_panel, wxID_ANY, _L("Filament filter:")),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            m_sc_filament_filter = new wxSpinCtrl(more_panel, wxID_ANY, wxEmptyString,
                wxDefaultPosition, wxSize(65, -1),
                wxSP_ARROW_KEYS, 0, 16, std::clamp(cur_ff, 0, 16));
            m_sc_filament_filter->SetToolTip(
                _L("0 = apply to all filaments (no filter).\n"
                   "N = apply effect only to regions whose base solid-infill extruder is filament N.\n"
                   "Useful in Assembled multi-material objects to target a specific colour region."));
            row->Add(m_sc_filament_filter, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(new wxStaticText(more_panel, wxID_ANY, _L("(0 = all)")),
                     0, wxALIGN_CENTER_VERTICAL);
            more_vs->Add(row, 0, wxEXPAND | wxBOTTOM, 4);
        }
        {
            double cur_ml = 1.0;
            if (auto* o = m_config->option<ConfigOptionFloat>("interlayer_colormix_min_length"))
                cur_ml = o->value;
            cur_ml = std::max(0.0, std::min(50.0, cur_ml));
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(more_panel, wxID_ANY, _L("ColorStitch min. line length:")),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            m_sc_min_length = new wxSpinCtrlDouble(more_panel, wxID_ANY, wxEmptyString,
                wxDefaultPosition, wxSize(75, -1),
                wxSP_ARROW_KEYS, 0.0, 50.0, cur_ml, 0.5);
            m_sc_min_length->SetDigits(1);
            m_sc_min_length->SetToolTip(
                _L("ColorStitch skips lines shorter than this value — they keep the region's default tool.\n"
                   "Higher values = fewer tool changes but more uncoloured gaps near edges.\n"
                   "0 = colour every line regardless of length.\n"
                   "Tip: if you see empty zones, lower this value."));
            row->Add(m_sc_min_length, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            row->Add(new wxStaticText(more_panel, wxID_ANY, _L("mm")),
                     0, wxALIGN_CENTER_VERTICAL);
            more_vs->Add(row, 0, wxEXPAND | wxBOTTOM, 4);
        }
        {
            bool cur_uv = true;
            if (auto* o = m_config->option<ConfigOptionBool>("interlayer_colormix_use_virtual"))
                cur_uv = o->value;
            m_chk_use_virtual = new wxCheckBox(more_panel, wxID_ANY,
                _L("Use Mixed Filament colors in pattern"));
            m_chk_use_virtual->SetValue(cur_uv);
            m_chk_use_virtual->SetToolTip(
                _L("When enabled, the pattern editor shows Mixed Filament virtual colors\n"
                   "(e.g. 'F1+F2') as clickable buttons alongside physical filaments.\n"
                   "Pattern digits 5-9 reference these virtual colors.\n"
                   "Requires Mixed Filaments to be defined in the filament panel."));
            more_vs->Add(m_chk_use_virtual, 0, wxBOTTOM, 4);
        }
        // NEOTKO_COLORMIX_TAG_END

        more_panel->SetSizer(more_vs);
        more_panel->Hide();
        sb->Add(more_panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);

        more_btn->Bind(wxEVT_BUTTON, [this, more_btn, more_panel](wxCommandEvent&) {
            const bool show = !more_panel->IsShown();
            more_panel->Show(show);
            more_btn->SetLabel(show ? _L("\u25bc More") : _L("\u25b6 More"));
            Layout();
            Fit();
        });

        return sb;
    }

    // ------------------------------------------------------------------ UI build

    void build_ui()
    {
        const int PAD = 8;
        auto* vs = new wxBoxSizer(wxVERTICAL);

        // 1. Top zone card
        vs->Add(create_zone_card(0), 0, wxEXPAND | wxALL, PAD / 2);

        // 2. Penultimate zone card
        vs->Add(create_zone_card(1), 0, wxEXPAND | wxALL, PAD / 2);

        // 3. Filament section (TD sliders + More collapsible)
        vs->Add(create_filament_section(), 0, wxEXPAND | wxALL, PAD / 2);

        // NEOTKO_COLORMIX_TAG_START — s58 Line distribution mode (global setting).
        // Controls how SurfaceColorMix and PathBlend map pattern slots to extrusion
        // lines.  Default (0) is the legacy behaviour (slot = path_idx % n_slots).
        // Modes 1/2/3 are experimental geometric distribution strategies; see the
        // tooltips for details.  Affects all four combinations of
        // (ColorMix/PathBlend) × (Top/Penultimate).
        {
            auto* lm_sb = new wxStaticBoxSizer(wxVERTICAL, this,
                _L("Line distribution mode"));
            auto* lm_row = new wxBoxSizer(wxHORIZONTAL);

            wxArrayString lm_labels;
            lm_labels.Add(_L("Default — line index (safe, original)"));
            lm_labels.Add(_L("GeoSort (A) — sort by perpendicular position"));
            lm_labels.Add(_L("LaneQuant (B) — quantize to geometric lanes"));
            lm_labels.Add(_L("DirCluster (C) — cluster by direction, then quantize"));

            int cur_lm = 0;
            if (auto* o = m_config->option<ConfigOptionInt>("surface_color_mix_lane_mode"))
                cur_lm = std::clamp(o->value, 0, 3);

            m_choice_lane_mode = new wxChoice(this, wxID_ANY,
                wxDefaultPosition, wxSize(320, -1), lm_labels);
            m_choice_lane_mode->SetSelection(cur_lm);
            m_choice_lane_mode->SetToolTip(
                _L("How pattern slots are assigned to individual extrusion lines.\n\n"
                   "0 Default — slot = line_index mod n_slots.\n"
                   "  Legacy behaviour. Pattern can break visually on irregular shapes\n"
                   "  (corners, concavities, narrow regions) because line order isn't\n"
                   "  always proportional to geometric position.\n\n"
                   "1 GeoSort — sort lines by position perpendicular to fill direction.\n"
                   "  Rayas más ordenadas; fragmentos del mismo carril aún pueden recibir\n"
                   "  colores distintos.\n\n"
                   "2 LaneQuant — slot = quantized geometric lane.\n"
                   "  Fragmentos del mismo carril reciben el mismo color → rayas continuas.\n"
                   "  Más robusto para formas irregulares.\n\n"
                   "3 DirCluster — agrupa primero por dirección de fill, luego LaneQuant\n"
                   "  por cluster. Maneja casos donde el motor de fill rotó la dirección\n"
                   "  en sub-regiones del contorno.\n\n"
                   "Aplica a ColorMix + PathBlend en Top + Penultimate."));

            m_choice_lane_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
                if (!m_choice_lane_mode) return;
                const int v = std::clamp(m_choice_lane_mode->GetSelection(), 0, 3);
                if (auto* o = m_config->option<ConfigOptionInt>("surface_color_mix_lane_mode"))
                    o->value = v;
                m_on_change("surface_color_mix_lane_mode");
                // Update mini-previews — they don't change shape but it triggers a
                // redraw so the user gets immediate visual feedback.
                if (m_prev_top)  m_prev_top->Refresh();
                if (m_prev_penu) m_prev_penu->Refresh();
            });

            lm_row->Add(new wxStaticText(this, wxID_ANY, _L("Mode:")),
                        0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            lm_row->Add(m_choice_lane_mode, 1, wxALIGN_CENTER_VERTICAL);
            lm_sb->Add(lm_row, 0, wxEXPAND | wxALL, PAD / 2);

            auto* lm_note = new wxStaticText(this, wxID_ANY,
                _L("Experimental. Default = original behaviour (no risk).\n"
                   "Modes A/B/C improve pattern fidelity on irregular shapes."));
            lm_note->SetForegroundColour(wxColour(100, 100, 100));
            lm_sb->Add(lm_note, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD / 2);

            vs->Add(lm_sb, 0, wxEXPAND | wxALL, PAD / 2);
        }
        // NEOTKO_COLORMIX_TAG_END

        // NEOTKO_COLORMIX_TAG — s60: Gradient section MOVED to ColorMixPatternDialog (sub-dialog opened by Advanced button per role).

        // NEOTKO_MULTIPASS_TAG_START — Blend Suggestion (Beer-Lambert joint Top+Penultimate optimizer)
        {
            // s120: pool de MixedColors conservado para el Match del Studio.
            {
                std::string mixed_defs;
                if (auto* o = m_config->option<ConfigOptionString>("mixed_filament_definitions"))
                    mixed_defs = o->value;
                if (!mixed_defs.empty()) {
                    auto all = Slic3r::SurfaceColorMix::get_mix_options(mixed_defs, m_fcolors);
                    for (auto& opt : all)
                        if (!opt.is_physical)
                            m_bs_mix_opts.push_back(opt);
                }
            }
        }
        // NEOTKO_MULTIPASS_TAG_END

#if 0   // s120: UI del antiguo Blend Suggestion (sustituida por ColorStitch Studio → Match). Pendiente excisión dura.
        {
            auto* bs_sb = new wxStaticBoxSizer(wxVERTICAL, this,
                _L("Blend Suggestion \u2014 Beer-Lambert optimizer"));

            auto* note = new wxStaticText(this, wxID_ANY,
                _L("Calculates optimal pass ORDER and layer RATIOS to approximate a MixedColor target.\n"
                   "Top only: single layer blend.  Top + Penultimate: same passes on both layers\n"
                   "(doubles physical material \u2014 allows smaller per-pass ratios)."));
            note->SetForegroundColour(wxColour(80,80,80));
            bs_sb->Add(note, 0, wxALL, PAD/2);

            // ---- Target picker row ----
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(this, wxID_ANY, _L("Target colour:")),
                         0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 6);
                wxArrayString labels;
                for (auto& opt : m_bs_mix_opts)
                    labels.Add(wxString::FromUTF8(opt.label));
                if (labels.IsEmpty()) labels.Add(_L("(no MixedColor defined)"));
                m_bs_combo_target = new wxComboBox(this, wxID_ANY,
                    labels.IsEmpty() ? wxString() : labels[0],
                    wxDefaultPosition, wxSize(180,-1), labels, wxCB_READONLY);
                m_bs_combo_target->SetToolTip(_L("Select the virtual MixedColor whose display colour is the blend target."));
                m_bs_combo_target->Enable(!m_bs_mix_opts.empty());
                row->Add(m_bs_combo_target, 1, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);

                // Colour swatch for the selected target MixedColor
                m_bs_swatch_target = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(18,18));
                m_bs_swatch_target->SetToolTip(_L("Display colour of the selected MixedColor target."));
                auto update_bs_target_swatch = [this]() {
                    if (!m_bs_swatch_target) return;
                    const int sel = m_bs_combo_target ? m_bs_combo_target->GetSelection() : -1;
                    wxColour c(128,128,128);
                    if (sel >= 0 && sel < (int)m_bs_mix_opts.size()) {
                        const std::string& dc = m_bs_mix_opts[sel].display_color;
                        if (dc.size() >= 7 && dc[0] == '#') {
                            unsigned long rgb = 0;
                            if (wxString::FromUTF8(dc.substr(1)).ToULong(&rgb, 16))
                                c = wxColour((rgb>>16)&0xFF, (rgb>>8)&0xFF, rgb&0xFF);
                        }
                    }
                    m_bs_swatch_target->SetBackgroundColour(c);
                    m_bs_swatch_target->Refresh();
                };
                update_bs_target_swatch();
                m_bs_combo_target->Bind(wxEVT_COMBOBOX, [update_bs_target_swatch](wxCommandEvent&) {
                    update_bs_target_swatch();
                });
                row->Add(m_bs_swatch_target, 0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);

                auto* btn_refresh = new wxButton(this, wxID_ANY, L"\u21BA",
                    wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                btn_refresh->SetToolTip(_L("Refresh — re-read Mixed Filament definitions from the current project."));
                btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_mix_opts(); });
                row->Add(btn_refresh, 0, wxALIGN_CENTER_VERTICAL|wxLEFT, 4);
                bs_sb->Add(row, 0, wxEXPAND|wxALL, PAD/2);
            }

            // ---- Mode radio buttons ----
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(this, wxID_ANY, _L("Mode:")),
                         0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 8);
                m_bs_rb_top = new wxRadioButton(this, wxID_ANY, _L("Top layer only"),
                                                wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
                m_bs_rb_joint = new wxRadioButton(this, wxID_ANY,
                    _L("Top + Penultimate  (same passes on both \u2192 double material)"));
                m_bs_rb_joint->SetValue(true); // default: joint mode
                m_bs_rb_top->SetToolTip(_L("MultiPass applies to top surface only (multipass_surface=1)."));
                m_bs_rb_joint->SetToolTip(_L(
                    "MultiPass applies to both top AND penultimate (multipass_surface=0).\n"
                    "The same passes run twice \u2014 Beer-Lambert models this as a stacked double layer.\n"
                    "Enables smaller per-pass ratios while maintaining physical adhesion."));
                row->Add(m_bs_rb_top,   0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 12);
                row->Add(m_bs_rb_joint, 0, wxALIGN_CENTER_VERTICAL);
                bs_sb->Add(row, 0, wxALL, PAD/2);
            }

            // NEOTKO_MULTIPASS_TAG_START — Perimeter Override checkbox
            {
                const bool cur_perim_ov = m_config->opt_bool("multipass_perimeter_override");
                m_mp_perim_override = new wxCheckBox(this, wxID_ANY,
                    _L("MultiPass Perimeter Override"));
                m_mp_perim_override->SetValue(cur_perim_ov);
                m_mp_perim_override->SetToolTip(_L(
                    "When enabled, perimeters are suppressed at the real layer Z and "
                    "re-printed once per MultiPass sublayer using that sublayer's tool.\n"
                    "Gives perimeter walls the same per-pass color blending as the top surface fill."));
                bs_sb->Add(m_mp_perim_override, 0, wxALL, PAD/2);
            }
            // NEOTKO_MULTIPASS_TAG_END

            // ---- Calculate button + result display ----
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                auto* btn_calc = new wxButton(this, wxID_ANY, _L("Calculate \u25b6"),
                                              wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                btn_calc->Enable(!m_bs_mix_opts.empty());

                m_bs_swatch = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(22,22));
                m_bs_swatch->SetBackgroundColour(wxColour(128,128,128));
                m_bs_swatch->SetToolTip(_L("Simulated blend colour after Beer-Lambert optimisation."));

                m_bs_lbl_score = new wxStaticText(this, wxID_ANY, _L("  ΔE: ---"));
                m_bs_lbl_score->SetToolTip(_L("CIE76 colour distance between simulated result and target.\n"
                                              "<5 excellent, 5\u201310 good, >10 poor approximation."));

                // NEOTKO_MULTIPASS_SURFACES_TAG — recipe-split Calculate
                // Algorithm: sort recipe tools by weight desc, partition between penu (dominant)
                // and top (fine-tuning). Ratios = recipe_weight / TD so visual contribution
                // matches recipe proportions. Min ratio enforces 0.04mm extrusion floor.
                btn_calc->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                    if (m_bs_mix_opts.empty() || !m_bs_combo_target) return;
                    const int sel = m_bs_combo_target->GetSelection();
                    if (sel < 0 || sel >= (int)m_bs_mix_opts.size()) return;
                    const auto& opt = m_bs_mix_opts[sel];
                    if (opt.tool_weights.empty()) return;

                    // Target display color
                    const std::string& dc = opt.display_color;
                    if (dc.size() < 7 || dc[0] != '#') return;
                    unsigned long rgb_v = 0;
                    if (!wxString::FromUTF8(dc.substr(1)).ToULong(&rgb_v, 16)) return;
                    const double t_r = ((rgb_v>>16)&0xFF)/255.0;
                    const double t_g = ((rgb_v>> 8)&0xFF)/255.0;
                    const double t_b = ( rgb_v     &0xFF)/255.0;

                    // Normalize recipe weights → sorted by weight desc
                    double total_w = 0.0;
                    for (auto& [t, w] : opt.tool_weights) total_w += w;
                    if (total_w < 1e-9) return;
                    std::vector<std::pair<int,double>> recipe; // {tool_0based, norm_weight}
                    for (auto& [t, w] : opt.tool_weights)
                        recipe.push_back({t, (double)w / total_w});
                    std::sort(recipe.begin(), recipe.end(),
                              [](const auto& a, const auto& b){ return a.second > b.second; });
                    const int n_rec = static_cast<int>(recipe.size());

                    // Pass counts + min ratio from layer height
                    int top_passes = 2, penu_passes = 2;
                    if (auto* o = m_config->option<ConfigOptionInt>("multipass_num_passes"))
                        top_passes = std::clamp(o->value, 1, 3);
                    if (auto* o = m_config->option<ConfigOptionInt>("penultimate_multipass_num_passes"))
                        penu_passes = std::clamp(o->value, 1, 3);
                    double lh = 0.2;
                    if (auto* o = m_config->option<ConfigOptionFloat>("layer_height"))
                        lh = std::max(0.04, (double)o->value);
                    const double min_r = std::max(0.05, 0.04 / lh);

                    // TD for a tool (0-based)
                    auto* ac = wxGetApp().app_config;
                    auto get_td = [&](int t) -> double {
                        if (t >= 0 && t < 4) return std::max(0.01, (double)m_td[t]);
                        double v = 0.5;
                        if (t >= 0 && ac) {
                            const std::string s = ac->get("neotko_td_" + std::to_string(t+1));
                            try { if (!s.empty()) v = std::stod(s); } catch(...) {}
                        }
                        return std::max(0.01, v);
                    };

                    // Build N pass slots from recipe starting at offset (wraps around)
                    // Returns {tool, norm_weight} with weights renormalized within the set
                    auto make_passes = [&](int offset, int n) {
                        std::vector<std::pair<int,double>> v;
                        for (int i = 0; i < n; ++i)
                            v.push_back(recipe[(offset + i) % n_rec]);
                        double s = 0.0; for (auto& [t,w] : v) s += w;
                        if (s > 1e-9) for (auto& [t,w] : v) w /= s;
                        else          for (auto& [t,w] : v) w  = 1.0 / n;
                        return v;
                    };

                    // Ratios from (tool, norm_weight): ratio_i = w_i * TD_i
                    // Beer-Lambert inverse: given target visual weight w (from recipe),
                    // solve ratio such that opacity = w → ratio = -TD * log10(1 - w).
                    // Opaque (low TD) gets small ratio; translucent (high TD) gets large ratio.
                    auto make_ratios = [&](const std::vector<std::pair<int,double>>& passes) {
                        std::vector<double> r;
                        for (auto& [t,w] : passes) {
                            const double td = get_td(t);
                            double ratio;
                            if (w <= 0.0) {
                                ratio = min_r;
                            } else if (w >= 1.0) {
                                ratio = td * 5.0; // physical ceiling
                            } else {
                                ratio = -td * std::log10(1.0 - std::clamp(w, 0.0, 0.9999));
                            }
                            r.push_back(std::max(min_r, ratio));
                        }
                        double s = 0.0; for (double x : r) s += x;
                        if (s < 1e-9) s = 1.0;
                        for (double& x : r) x = std::max(min_r, x / s);
                        s = 0.0; for (double x : r) s += x;
                        for (double& x : r) x /= s; // re-normalize after floor clamp
                        return r;
                    };

                    auto wi = [&](const char* k, int v)   { if (auto* o=m_config->option<ConfigOptionInt>  (k)) o->value=v; m_on_change(k); };
                    auto wb = [&](const char* k, bool v)  { if (auto* o=m_config->option<ConfigOptionBool> (k)) o->value=v; m_on_change(k); };
                    auto wf = [&](const char* k, float v) { if (auto* o=m_config->option<ConfigOptionFloat>(k)) o->value=v; m_on_change(k); };

                    const bool joint = m_bs_rb_joint && m_bs_rb_joint->GetValue();

                    const char* top_tk[3]  = {"multipass_tool_1","multipass_tool_2","multipass_tool_3"};
                    const char* top_rk[3]  = {"multipass_width_ratio_1","multipass_width_ratio_2","multipass_width_ratio_3"};
                    const char* penu_tk[3] = {"penultimate_multipass_tool_1","penultimate_multipass_tool_2","penultimate_multipass_tool_3"};
                    const char* penu_rk[3] = {"penultimate_multipass_width_ratio_1","penultimate_multipass_width_ratio_2","penultimate_multipass_width_ratio_3"};

                    if (joint) {
                        // Penu: dominant tools (offset 0); Top: next tools (offset penu_passes)
                        const auto pv = make_passes(0,           penu_passes);
                        const auto tv = make_passes(penu_passes, top_passes);
                        const auto pr = make_ratios(pv);
                        const auto tr_v = make_ratios(tv);
                        for (int i = 0; i < top_passes;  ++i) { wi(top_tk[i],  tv[i].first); wf(top_rk[i],  (float)tr_v[i]); }
                        for (int i = 0; i < penu_passes; ++i) { wi(penu_tk[i], pv[i].first); wf(penu_rk[i], (float)pr[i]);   }
                        wi("multipass_num_passes",             top_passes);
                        wi("penultimate_multipass_num_passes", penu_passes);
                        wb("multipass_enabled",                true);
                        wi("multipass_surface",                1);
                        wb("penultimate_multipass_enabled",    true);
                        m_top_eff  = EFF_MP;
                        m_penu_eff = EFF_MP;
                    } else {
                        // Top-only: best tools from recipe for top_passes slots
                        const auto tv   = make_passes(0, top_passes);
                        const auto tr_v = make_ratios(tv);
                        for (int i = 0; i < top_passes; ++i) { wi(top_tk[i], tv[i].first); wf(top_rk[i], (float)tr_v[i]); }
                        wi("multipass_num_passes", top_passes);
                        wb("multipass_enabled",    true);
                        wi("multipass_surface",    1);
                        m_top_eff = EFF_MP;
                    }

                    // Predicted result color + ΔE vs target
                    const wxColour rc = joint ? stacked_preview() : blend_preview(0);
                    const double dr = rc.Red()  /255.0 - t_r;
                    const double dg = rc.Green()/255.0 - t_g;
                    const double db = rc.Blue() /255.0 - t_b;
                    const double de = std::sqrt(dr*dr + dg*dg + db*db) * 100.0 / std::sqrt(3.0);

                    if (m_bs_swatch) { m_bs_swatch->SetBackgroundColour(rc); m_bs_swatch->Refresh(); }
                    if (m_bs_lbl_score) {
                        m_bs_lbl_score->SetLabel(wxString::Format(_L("  \u0394E: %.1f"), de));
                        m_bs_lbl_score->SetForegroundColour(
                            de < 5.0  ? wxColour(30,140,30) :
                            de < 10.0 ? wxColour(190,130,0) : wxColour(180,40,40));
                        m_bs_lbl_score->Refresh();
                    }
                    update_ui();
                    Layout();
                });

                row->Add(btn_calc,       0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 6);
                row->Add(m_bs_swatch,    0, wxALIGN_CENTER_VERTICAL|wxRIGHT, 4);
                row->Add(m_bs_lbl_score, 1, wxALIGN_CENTER_VERTICAL);
                bs_sb->Add(row, 0, wxALL, PAD/2);
            }

            vs->Add(bs_sb, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);
        }
#endif  // s120: fin UI Blend Suggestion eliminada

        // NEOTKO_PROFILE_TAG_START — Fase A: Save/Manage Surface Effect Profiles.
        // Snapshot ONLY the ColorMix payload for now; the profile struct already
        // reserves PathBlend (Fase G) and MultiPass (Fase F) payloads.
        {
            wxBoxSizer* prof_row = new wxBoxSizer(wxHORIZONTAL);
            wxButton* btn_save_prof = new wxButton(this, wxID_ANY, _L("Save as profile…"));
            wxButton* btn_mgr_prof  = new wxButton(this, wxID_ANY, _L("Manage profiles…"));
            prof_row->Add(btn_save_prof, 0, wxRIGHT, PAD);
            prof_row->Add(btn_mgr_prof,  0);
            vs->Add(prof_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);

            btn_save_prof->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                wxTextEntryDialog dlg(this, _L("Profile name:"),
                                      _L("Save Surface Effect Profile"));
                if (dlg.ShowModal() != wxID_OK) return;
                const std::string name = dlg.GetValue().ToStdString();
                if (name.empty()) return;

                // NEOTKO_PROFILE_TAG — Fase F1: snapshot only effects that are
                // currently enabled. Otherwise a profile saved with MP pill OFF
                // would carry an MP payload with `multipass_enabled=0`, which
                // painter mode reads as "explicitly disabled" and suppresses MP
                // even when the user wanted ONLY ColorMix in that profile.
                // NEOTKO_PROFILE_TAG — Fase G fix: read pill state (live UI).
                // m_config only updates on OK, so Save before OK would otherwise
                // snapshot the LAST applied state, not what the user sees.
                // We also override the per-effect enable flags in the captured
                // kv so they reflect pill state, without mutating m_config
                // (which would persist across Cancel).
                const bool _cm_on = (m_top_eff == EFF_CM) || (m_penu_eff == EFF_CM);
                const bool _mp_on = (m_top_eff == EFF_MP) || (m_penu_eff == EFF_MP);
                // NEOTKO_PROFILE_TAG — s68: PB on Penu re-enabled.
                const bool _pb_on = (m_top_eff == EFF_PB) || (m_penu_eff == EFF_PB);
                Slic3r::SurfaceEffectProfile p;
                p.name = name;
                // NEOTKO_PROFILE_TAG — Fase G fix: also force the per-effect
                // surface enum (0=Both / 1=Top only / 2=Penu only) in the
                // captured kv. Otherwise a profile saved with top=PB+penu=None
                // would inherit whatever pathblend_surface was last in m_config
                // (often 0=Both), and Loading would phantom-fill the penu pill.
                auto surface_enum = [&](int eff) -> std::string {
                    const bool top = (m_top_eff  == eff);
                    const bool pen = (m_penu_eff == eff);
                    if (top && pen) return "0"; // Both
                    if (top)        return "1"; // Top only
                    return "2";                 // Penu only
                };
                if (_cm_on) {
                    p.colormix = Slic3r::SurfaceEffectProfileManager::snapshot_keys(
                        *m_config, Slic3r::SurfaceEffectProfileManager::colormix_keys());
                    p.colormix.kv["interlayer_colormix_enabled"] = "1";
                    p.colormix.kv["interlayer_colormix_surface"] = surface_enum(EFF_CM);
                }
                if (_mp_on) {
                    p.multipass = Slic3r::SurfaceEffectProfileManager::snapshot_keys(
                        *m_config, Slic3r::SurfaceEffectProfileManager::multipass_keys());
                    // Mark per-role enabled based on which pill is on.
                    p.multipass.kv["multipass_enabled"] =
                        (m_top_eff == EFF_MP) ? "1" : "0";
                    p.multipass.kv["penultimate_multipass_enabled"] =
                        (m_penu_eff == EFF_MP) ? "1" : "0";
                    // multipass_surface still kept for legacy reads — sync to pills.
                    p.multipass.kv["multipass_surface"] = surface_enum(EFF_MP);
                }
                if (_pb_on) {
                    p.pathblend = Slic3r::SurfaceEffectProfileManager::snapshot_keys(
                        *m_config, Slic3r::SurfaceEffectProfileManager::pathblend_keys());
                    p.pathblend.kv["multipass_path_gradient"] = "1";
                    p.pathblend.kv["pathblend_surface"] = surface_enum(EFF_PB);
                }
                // NEOTKO_PROFILE_TAG — Fase 6 (aditivo): snapshot the resolved
                // sandwich stacks for the 3D Painter mini-preview. PREVIEW-ONLY —
                // the slice engine keeps reading the payloads above. Derived from
                // the live config via the same resolver the SandwichDialog uses.
                // synthesize_from_legacy leaves ColorMix passes with empty kv
                // (engine falls back to region config); inject the snapshotted
                // ColorMix payload so the gizmo preview is self-contained.
                {
                    auto st_top  = Slic3r::SurfacePassStack::resolve_for_zone(*m_config, false);
                    auto st_penu = Slic3r::SurfacePassStack::resolve_for_zone(*m_config, true);
                    auto inject = [&](Slic3r::SurfacePassStack& st) {
                        if (!p.colormix.present) return;
                        for (auto& pass : st.passes)
                            if (pass.kind == Slic3r::SurfacePassKind::ColorMix && pass.colormix.kv.empty())
                                pass.colormix = p.colormix;
                    };
                    inject(st_top);
                    inject(st_penu);
                    p.stack_top_json  = st_top.to_json();
                    p.stack_penu_json = st_penu.to_json();
                }
                const int new_id = Slic3r::SurfaceEffectProfileManager::get().add(std::move(p));
                const auto* added = Slic3r::SurfaceEffectProfileManager::get().find(new_id);
                wxMessageBox(wxString::Format(_L("Saved profile #%d (CM:%zu, MP:%zu, PB:%zu keys)."),
                                              new_id,
                                              (size_t)added->colormix.kv.size(),
                                              (size_t)added->multipass.kv.size(),
                                              (size_t)added->pathblend.kv.size()),
                             _L("Surface Effect Profile"), wxOK | wxICON_INFORMATION, this);
            });

            btn_mgr_prof->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                auto& mgr = Slic3r::SurfaceEffectProfileManager::get();
                wxDialog mdlg(this, wxID_ANY, _L("Manage Surface Effect Profiles"),
                              wxDefaultPosition, wxSize(420, 320),
                              wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
                wxBoxSizer* ms = new wxBoxSizer(wxVERTICAL);
                wxListBox*  lb = new wxListBox(&mdlg, wxID_ANY);

                auto refill = [&]() {
                    lb->Clear();
                    for (const auto& p : mgr.list()) {
                        wxString label = wxString::Format("#%d  %s  [CM:%s PB:%s MP:%s]",
                            p.id,
                            wxString::FromUTF8(p.name),
                            p.colormix.present  ? "*" : "-",
                            p.pathblend.present ? "*" : "-",
                            p.multipass.present ? "*" : "-");
                        lb->Append(label, reinterpret_cast<void*>((intptr_t)p.id));
                    }
                };
                refill();

                // NEOTKO_PROFILE_TAG — two button rows: edit ops + actions.
                wxBoxSizer* br_edit = new wxBoxSizer(wxHORIZONTAL);
                wxButton* btn_load = new wxButton(&mdlg, wxID_ANY, _L("Load into dialog"));
                wxButton* btn_upd  = new wxButton(&mdlg, wxID_ANY, _L("Update from current"));
                br_edit->Add(btn_load, 0, wxRIGHT, 6);
                br_edit->Add(btn_upd,  0);

                wxBoxSizer* br = new wxBoxSizer(wxHORIZONTAL);
                wxButton* btn_ren = new wxButton(&mdlg, wxID_ANY, _L("Rename"));
                wxButton* btn_del = new wxButton(&mdlg, wxID_ANY, _L("Delete"));
                wxButton* btn_cls = new wxButton(&mdlg, wxID_CLOSE, _L("Close"));
                br->Add(btn_ren, 0, wxRIGHT, 6);
                br->Add(btn_del, 0, wxRIGHT, 6);
                br->AddStretchSpacer(1);
                br->Add(btn_cls, 0);

                ms->Add(lb,      1, wxEXPAND | wxALL, 8);
                ms->Add(br_edit, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
                ms->Add(br,      0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
                mdlg.SetSizer(ms);

                auto selected_id = [&]() -> int {
                    const int sel = lb->GetSelection();
                    if (sel == wxNOT_FOUND) return 0;
                    return (int)(intptr_t)lb->GetClientData(sel);
                };

                // NEOTKO_PROFILE_TAG — Load: restore the profile's ColorMix payload
                // into the active SCM config so the user can edit it in the dialog.
                // After this the user can tweak values and use "Update from current"
                // (below) to commit the changes back to this same profile, or
                // "Save as profile…" to fork it into a new one.
                btn_load->Bind(wxEVT_BUTTON, [&, this](wxCommandEvent&) {
                    const int id = selected_id();
                    if (id == 0) return;
                    const auto* p = mgr.find(id);
                    if (!p) return;
                    if (!p->colormix.present && !p->multipass.present && !p->pathblend.present) return;
                    if (p->colormix.present)
                        Slic3r::SurfaceEffectProfileManager::restore_keys(*m_config, p->colormix);
                    // NEOTKO_PROFILE_TAG — Fase F1: restore MultiPass keys too so the
                    // MP sub-dialog (re-opened later) sees the loaded values.
                    if (p->multipass.present)
                        Slic3r::SurfaceEffectProfileManager::restore_keys(*m_config, p->multipass);
                    // NEOTKO_PROFILE_TAG — Fase G1: restore PathBlend keys.
                    if (p->pathblend.present)
                        Slic3r::SurfaceEffectProfileManager::restore_keys(*m_config, p->pathblend);
                    // Notify config-listeners for every restored key so other panes
                    // refresh too (filament filter, surface combo, etc.).
                    if (p->colormix.present)
                        for (const auto& kv : p->colormix.kv)
                            m_on_change(kv.first);
                    if (p->multipass.present)
                        for (const auto& kv : p->multipass.kv)
                            m_on_change(kv.first);
                    if (p->pathblend.present)
                        for (const auto& kv : p->pathblend.kv)
                            m_on_change(kv.first);
                    // Push restored values back into the visible widgets — they
                    // were built from m_config at dialog creation and don't poll
                    // for external changes. The advanced sub-dialog
                    // (ColorMixPatternDialog) re-reads m_config when opened, so
                    // numeric gradient values appear correctly there.
                    if (m_choice_top_zone)
                        m_choice_top_zone->SetSelection(
                            m_config->opt_int("interlayer_colormix_top_zone"));
                    if (m_choice_penu_zone)
                        m_choice_penu_zone->SetSelection(
                            m_config->opt_int("interlayer_colormix_penu_zone"));
                    if (m_sc_filament_filter)
                        m_sc_filament_filter->SetValue(
                            m_config->opt_int("interlayer_colormix_filament_filter"));
                    if (m_sc_min_length)
                        m_sc_min_length->SetValue(
                            m_config->opt_float("interlayer_colormix_min_length"));
                    if (m_chk_use_virtual)
                        m_chk_use_virtual->SetValue(
                            m_config->opt_bool("interlayer_colormix_use_virtual"));
                    infer_effects();
                    update_ui();
                    Layout();
                });

                // NEOTKO_PROFILE_TAG — Update from current: re-snapshot the dialog's
                // live config and overwrite the selected profile's ColorMix payload.
                // Keeps the profile id/name; only the payload is replaced.
                btn_upd->Bind(wxEVT_BUTTON, [&, this](wxCommandEvent&) {
                    const int id = selected_id();
                    if (id == 0) return;
                    auto* p = mgr.find_mut(id);
                    if (!p) return;
                    if (wxMessageBox(
                            wxString::Format(_L("Overwrite profile '%s' with the "
                                                "current dialog settings?"),
                                             wxString::FromUTF8(p->name)),
                            _L("Update profile"),
                            wxYES_NO | wxICON_QUESTION, &mdlg) != wxYES)
                        return;
                    // NEOTKO_PROFILE_TAG — Fase F1: snapshot only effects that
                    // are currently enabled; effects toggled OFF clear their
                    // payload from the profile. Matches Save semantics: profile
                    // = bundle of currently-active settings.
                    // NEOTKO_PROFILE_TAG — Fase G fix: same pill-state read as
                    // SCM Save. Pre-OK Update must reflect what the user sees.
                    const bool _u_cm_on = (m_top_eff == EFF_CM) || (m_penu_eff == EFF_CM);
                    const bool _u_mp_on = (m_top_eff == EFF_MP) || (m_penu_eff == EFF_MP);
                    // NEOTKO_PROFILE_TAG — s68: PB on Penu re-enabled.
                    const bool _u_pb_on = (m_top_eff == EFF_PB) || (m_penu_eff == EFF_PB);
                    auto u_surface_enum = [&](int eff) -> std::string {
                        const bool top = (m_top_eff  == eff);
                        const bool pen = (m_penu_eff == eff);
                        if (top && pen) return "0";
                        if (top)        return "1";
                        return "2";
                    };
                    if (_u_cm_on) {
                        p->colormix = Slic3r::SurfaceEffectProfileManager::snapshot_keys(
                            *m_config, Slic3r::SurfaceEffectProfileManager::colormix_keys());
                        p->colormix.kv["interlayer_colormix_enabled"] = "1";
                        p->colormix.kv["interlayer_colormix_surface"] = u_surface_enum(EFF_CM);
                    } else {
                        p->colormix = Slic3r::SurfaceEffectPayload{};
                    }
                    if (_u_mp_on) {
                        p->multipass = Slic3r::SurfaceEffectProfileManager::snapshot_keys(
                            *m_config, Slic3r::SurfaceEffectProfileManager::multipass_keys());
                        p->multipass.kv["multipass_enabled"] =
                            (m_top_eff == EFF_MP) ? "1" : "0";
                        p->multipass.kv["penultimate_multipass_enabled"] =
                            (m_penu_eff == EFF_MP) ? "1" : "0";
                        p->multipass.kv["multipass_surface"] = u_surface_enum(EFF_MP);
                    } else {
                        p->multipass = Slic3r::SurfaceEffectPayload{};
                    }
                    if (_u_pb_on) {
                        p->pathblend = Slic3r::SurfaceEffectProfileManager::snapshot_keys(
                            *m_config, Slic3r::SurfaceEffectProfileManager::pathblend_keys());
                        p->pathblend.kv["multipass_path_gradient"] = "1";
                        p->pathblend.kv["pathblend_surface"] = u_surface_enum(EFF_PB);
                    } else {
                        p->pathblend = Slic3r::SurfaceEffectPayload{};
                    }
                    // NEOTKO_PROFILE_TAG — Fase 6 (aditivo): refresh preview stacks
                    // (self-contained: inject ColorMix payload, see SCM Save above).
                    {
                        auto st_top  = Slic3r::SurfacePassStack::resolve_for_zone(*m_config, false);
                        auto st_penu = Slic3r::SurfacePassStack::resolve_for_zone(*m_config, true);
                        auto inject = [&](Slic3r::SurfacePassStack& st) {
                            if (!p->colormix.present) return;
                            for (auto& pass : st.passes)
                                if (pass.kind == Slic3r::SurfacePassKind::ColorMix && pass.colormix.kv.empty())
                                    pass.colormix = p->colormix;
                        };
                        inject(st_top);
                        inject(st_penu);
                        p->stack_top_json  = st_top.to_json();
                        p->stack_penu_json = st_penu.to_json();
                    }
                    refill();
                });

                btn_ren->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
                    const int id = selected_id();
                    if (id == 0) return;
                    const auto* p = mgr.find(id);
                    wxTextEntryDialog td(&mdlg, _L("New name:"), _L("Rename profile"),
                                         wxString::FromUTF8(p ? p->name : ""));
                    if (td.ShowModal() == wxID_OK)
                        mgr.rename(id, td.GetValue().ToStdString());
                    refill();
                });
                btn_del->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
                    const int id = selected_id();
                    if (id == 0) return;
                    if (wxMessageBox(_L("Delete this profile?"), _L("Confirm"),
                                     wxYES_NO | wxICON_QUESTION, &mdlg) == wxYES) {
                        mgr.remove(id);
                        refill();
                    }
                });
                btn_cls->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { mdlg.EndModal(wxID_CLOSE); });

                mdlg.ShowModal();
            });
        }
        // NEOTKO_PROFILE_TAG_END

        // ---- OK / Cancel ----
        vs->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxALIGN_RIGHT, PAD);
        SetSizerAndFit(vs);

        // Wire OK to apply config
        Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
            if (e.GetId() == wxID_OK)
                apply_effect_config();
            e.Skip();
        });
    }
};
// NEOTKO_SURFACE_MIXER_TAG_END

// NEOTKO_SANDWICH_TAG_START — Fase 3: SandwichDialog
// ===========================================================================
// SandwichDialog — additive editor for the per-zone pass-stack ("sandwich").
//
// Independent of the ~4000-line SurfaceColorMixerDialog: it does NOT touch it.
// SurfaceColorMixerDialog keeps editing the legacy keys; this dialog edits the
// 2 blob keys `neotko_surface_passes_top` / `_penu`. The engine's resolve()
// reads the blob first and only falls back to synthesize_from_legacy() when the
// blob is empty — so writing "" here restores legacy behaviour.
//
// A zone (Top, and an independent Penultimate) is a stack of 1..3 passes; each
// pass has a Z-ratio and a kind (None/Solid/ColorMix/PathBlend). Slot rule:
//   1 slot    → ColorMix or PathBlend only
//   2-3 slots → any kind
// PathBlend collapses the zone to a single full-height pass (legacy engine).
// ===========================================================================
class SandwichDialog : public wxDialog
{
    using Kind = Slic3r::SurfacePassKind;
    static constexpr int TOOL_MENU_BASE = 1700;

public:
    SandwichDialog(wxWindow* parent,
                   DynamicPrintConfig* config,
                   std::function<void(const std::string&)> on_change_cb)
        : wxDialog(parent, wxID_ANY, _L("Sandwich Editor"),
                   wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_config(config)
        , m_on_change(std::move(on_change_cb))
    {
        if (auto* o = wxGetApp().preset_bundle->project_config
                          .option<ConfigOptionStrings>("filament_colour"))
            m_fcolors = o->values;
        while (m_fcolors.size() < 4) m_fcolors.push_back("#808080");

        // NEOTKO_PATHBLEND_TAG — s88. Seed PathBlend runtimes from app_config
        // so last-session toggles take effect immediately. Runtimes are split:
        //   • PathBlendDispatcherRuntime → chain_continuous, chain_max_xy_mm
        //   • PathBlendSchedulerRuntime  → chain_atomic
        // Both live in SurfaceColorMix.hpp (PathBlend ingredient section).
        if (auto* ac = wxGetApp().app_config) {
            Slic3r::PathBlendDispatcherRuntime& d = Slic3r::PathBlendDispatcherRuntime::mut();
            Slic3r::PathBlendSchedulerRuntime&  s = Slic3r::PathBlendSchedulerRuntime::mut();
            const std::string vc = ac->get("neotko_pb_chain_continuous");
            const std::string va = ac->get("neotko_pb_chain_atomic");
            const std::string vx = ac->get("neotko_pb_chain_max_xy_mm");
            const std::string vk = ac->get("neotko_pb_use_canon_scheduler");
            if (!vc.empty()) d.chain_continuous   = (vc == "1" || vc == "true");
            if (!va.empty()) s.chain_atomic       = (va == "1" || va == "true");
            if (!vk.empty()) s.use_canon_scheduler = (vk == "1" || vk == "true");
            if (!vx.empty()) { try { d.chain_max_xy_mm = std::stod(vx); } catch (...) {} }
        }

        // Load the current state of both zones (blob, or synthesized legacy).
        m_stack[0] = Slic3r::SurfacePassStack::resolve_for_zone(*m_config, false);
        m_stack[1] = Slic3r::SurfacePassStack::resolve_for_zone(*m_config, true);
        for (int z = 0; z < 2; ++z)
            sanitize_stack(z);

        // NEOTKO_SANDWICH_TAG — Fase 7 (s84): TD from app_config (same keys
        // as SurfaceColorMixerDialog: neotko_td_1..4 — single source of truth).
        {
            auto* ac = wxGetApp().app_config;
            for (int i = 0; i < 4; ++i) {
                const std::string key = "neotko_td_" + std::to_string(i + 1);
                const std::string val = ac ? ac->get(key) : "";
                float v = 0.f;
                try { if (!val.empty()) v = std::stof(val); } catch (...) {}
                m_td[i] = std::max(0.01f, std::min(10.f, v > 0.f ? v : 1.f));
            }
        }

        // NEOTKO_SANDWICH_TAG — Fase 7 (s84b): preload virtual MixedColor
        // options for Blend Suggestion target picker.
        load_mix_opts();

        build_ui();
    }

private:
    DynamicPrintConfig*                     m_config = nullptr;
    std::function<void(const std::string&)> m_on_change;
    std::vector<std::string>                m_fcolors;

    Slic3r::SurfacePassStack m_stack[2];     // 0 = Top, 1 = Penultimate
    int m_pending_load_id = 0;               // NEOTKO_SANDWICH_TAG — deferred Load (see on_manage_profiles)

    // Everything is inline per row — no shared "Advanced" panel. The fill angle
    // is a per-row field bound to that pass's own SurfacePass.angle. fan/speed/
    // gcode of SurfacePass are kept in the data model (round-trip, "forgotten
    // box") but have no widgets — they do nothing for a Solid pass.
    struct ZoneUI {
        wxCheckBox*   enable_chk = nullptr;
        wxCheckBox*   perim_chk  = nullptr;
        wxRadioBox*   slots_rb   = nullptr;
        wxPanel*      ratio_bar  = nullptr;   // stacked draggable Z-ratio bar
        wxPanel*      rows_host  = nullptr;
        wxBoxSizer*   rows_sizer = nullptr;
        wxStaticText* sum_lbl    = nullptr;
        wxButton*     norm_btn   = nullptr;
        std::vector<wxPanel*>          row_panel;
        std::vector<wxPanel*>          chips;
        std::vector<wxStaticText*>     badge;
        std::vector<wxChoice*>         kind_choice;
        std::vector<wxSpinCtrlDouble*> ratio_spin;
        std::vector<wxStaticText*>     angle_lbl;   // Solid only
        std::vector<wxTextCtrl*>       angle_txt;   // Solid only
        std::vector<wxPanel*>          preview;
        std::vector<wxButton*>         adv_btn;     // ColorMix / PathBlend
        // NEOTKO_PATHBLEND_TAG — s88. "Advanced ⚙" button shown next to the
        // adv_btn ONLY for PathBlend rows. Opens the PB Advanced toggles modal
        // (chain_continuous, chain_max_xy_mm, chain_atomic).
        std::vector<wxButton*>         pb_advanced_btn;
        std::vector<wxStaticText*>     minlbl;      // "< 0.04 mm" warning
        // NEOTKO_SANDWICH_TAG — Fase 7 (s84c): move-up / move-down per row.
        std::vector<wxButton*>         up_btn;
        std::vector<wxButton*>         down_btn;
        // NEOTKO_SANDWICH_TAG — Fase 5 s73: KindEntry encodes Kind + PB mode
        // so the kind selector can list "PathBlend Half" and "PathBlend Full"
        // as distinct entries while both map to Kind::PathBlend internally.
        struct KindEntry {
            Kind     kind;
            int      pb_mode;  // 0 = Half, 1 = Full; -1 for non-PB kinds
            wxString label;
        };
        std::vector<KindEntry>         kindlist;    // choice-index -> KindEntry
    };
    ZoneUI m_ui[2];

    // ratio-bar drag state: which zone / which internal boundary is held.
    int m_drag_zone  = -1;
    int m_drag_bound = -1;

    // NEOTKO_SANDWICH_TAG — Fase 7 (s84): TD panel + Lane mode + use_virtual
    // portados del SurfaceColorMixerDialog viejo. Decisión usuario s84:
    //   - Panel derecho fijo (BoxSizer horizontal, mockup s71).
    //   - El viejo diálogo NO se toca todavía (duplicación intencional;
    //     consolidación en una sesión futura de "retirar UX viejo").
    //   - Color Science Roadmap (docs/FUTURE/COLOR SCIENCE ROADMAP.md):
    //     este panel es el punto de entrada natural para N1.1 (RGB→Lab) y
    //     N1.3 (TD por canal). Por ahora se mantiene escalar + RGB.
    std::array<float, 4>          m_td       = {};
    std::array<wxSlider*, 4>      m_sl_td    = {};
    std::array<wxStaticText*, 4>  m_lbl_td   = {};
    std::array<wxPanel*, 4>       m_sw_td    = {};
    wxPanel*       m_stacked_sw_top  = nullptr;
    wxPanel*       m_stacked_sw_penu = nullptr;
    wxPanel*       m_stacked_swatch  = nullptr;
    wxStaticText*  m_lbl_opacity_top = nullptr;
    wxChoice*      m_choice_lane_mode = nullptr;
    wxCheckBox*    m_chk_use_virtual  = nullptr;

    // NEOTKO_SANDWICH_TAG — Fase 7 (s84b): Blend Suggestion (Beer-Lambert
    // joint optimizer) portado del SurfaceColorMixerDialog viejo. Escribe a
    // m_stack[] (autoritativo) en vez de las legacy multipass_* keys.
    wxComboBox*    m_bs_combo_target  = nullptr;
    wxPanel*       m_bs_swatch_target = nullptr;
    wxRadioButton* m_bs_rb_top        = nullptr;
    wxRadioButton* m_bs_rb_joint      = nullptr;
    wxPanel*       m_bs_swatch        = nullptr;
    wxStaticText*  m_bs_lbl_score     = nullptr;
    std::vector<Slic3r::ColorMixOption> m_bs_mix_opts;

    // NEOTKO_COLORSCI_TAG GD1+GD2+GD3 — Gradient Designer embebido (columna
    // derecha, bajo el panel TD). Engine puro en libslic3r/ColorSci/ — aquí
    // solo UI. Decisión UX usuario (2026-06-11): Line distribution baja a la
    // columna izquierda y este panel ocupa su hueco + el stretch.
    wxChoice*         m_gd_tool_a   = nullptr;
    wxChoice*         m_gd_tool_b   = nullptr;
    wxPanel*          m_gd_sw_a     = nullptr;   // swatch color tool A
    wxPanel*          m_gd_sw_b     = nullptr;   // swatch color tool B
    wxChoice*         m_gd_weave    = nullptr;
    wxTextCtrl*       m_gd_pattern  = nullptr;
    wxSpinCtrl*       m_gd_steps    = nullptr;
    wxSpinCtrlDouble* m_gd_min      = nullptr;
    wxSpinCtrlDouble* m_gd_max      = nullptr;
    wxTextCtrl*       m_gd_name     = nullptr;
    wxStaticText*     m_gd_warn     = nullptr;
    wxPanel*          m_gd_strip    = nullptr;   // tira de swatches clicables
    std::vector<Slic3r::ColorSci::GradientStep> m_gd_ramp;
    std::vector<bool>                           m_gd_selected;

    // NEOTKO_COLORSCI_TAG Predict — ronda Flat/Mixed (3 modos). Selector arriba
    // del panel: 0 Gradient (ramp manual), 1 Flat (predicción solo-solids),
    // 2 Mixed (predicción dither penu + top solid → gamut extendido).
    // En modos predicción la tira muestra `m_gd_recipes` y CLICK CARGA la
    // receta en el sandwich vivo (m_stack[] + reload_ui_from_stack) en vez de
    // togglear selección de export.
    wxChoice*         m_gd_mode        = nullptr;
    wxPanel*          m_gd_design_pnl  = nullptr;   // controles modo Gradient
    wxPanel*          m_gd_predict_pnl = nullptr;   // controles modos Flat/Mixed
    wxChoice*         m_gd_ptarget     = nullptr;   // target picker (= m_bs_mix_opts)
    wxStaticText*     m_gd_de          = nullptr;   // ΔE del match
    std::vector<Slic3r::ColorSci::ColorRecipe> m_gd_recipes;
    int               m_gd_match_idx   = -1;        // swatch resaltado tras Match
    // Debounce: al mover un slider TD se disparan muchos eventos; regeneramos
    // las tres tiras (gradient/flat/mixed) UNA vez ~150 ms tras el último.
    wxTimer           m_gd_td_timer;
    static constexpr int kGdTdTimerId = wxID_HIGHEST + 4201;

    // ----------------------------------------------------------- small helpers
    // Layer height (mm) — pass heights are shown as ratio × this.
    double layer_height_mm() const
    {
        if (auto* o = m_config->option<ConfigOptionFloat>("layer_height"))
            if (o->value > 0.001) return o->value;
        return 0.2;
    }
    static constexpr double kMinPassMM = 0.04;   // engine MinLayer rule

    wxColour tool_colour(int t) const
    {
        if (t >= 0 && t < (int)m_fcolors.size() && !m_fcolors[t].empty()) {
            unsigned long rgb = 0;
            wxString s = wxString::FromUTF8(m_fcolors[t]);
            if (s.StartsWith("#")) s = s.Mid(1);
            if (s.ToULong(&rgb, 16))
                return wxColour((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        }
        return wxColour(128, 128, 128);
    }

    static wxString kind_name(Kind k)
    {
        switch (k) {
            case Kind::None:      return _L("None");
            case Kind::Solid:     return _L("Solid");
            case Kind::ColorMix:  return _L("ColorStitch"); // NEOTKO_COLORSTITCH_TAG
            case Kind::PathBlend: return _L("PathBlend");
        }
        return wxEmptyString;
    }
    static wxString kind_badge(Kind k)
    {
        switch (k) {
            case Kind::None:      return "NONE";
            case Kind::Solid:     return "SOLID";
            case Kind::ColorMix:  return "COLORMIX";
            case Kind::PathBlend: return "PATHBLEND";
        }
        return wxEmptyString;
    }
    static wxColour kind_colour(Kind k)
    {
        switch (k) {
            case Kind::None:      return wxColour(110, 110, 110);
            case Kind::Solid:     return wxColour(214, 124, 48);
            case Kind::ColorMix:  return wxColour(72, 110, 200);
            case Kind::PathBlend: return wxColour(150, 88, 178);
        }
        return wxColour(110, 110, 110);
    }

    // NEOTKO_SANDWICH_TAG — Fase 5 s73: PB badge text reflects Half/Full mode.
    static wxString kind_badge_for(const Slic3r::SurfacePass& p)
    {
        if (p.kind != Kind::PathBlend) return kind_badge(p.kind);
        const auto it = p.pathblend.kv.find("blob");
        if (it == p.pathblend.kv.end() || it->second.empty()) return "PB FULL";
        const Slic3r::PathBlendPassConfig pbc =
            Slic3r::PathBlendPassConfig::from_blob_json(it->second);
        return (pbc.mode == Slic3r::PathBlendPassConfig::Mode::Half) ? "PB HALF" : "PB FULL";
    }

    // NEOTKO_SANDWICH_TAG — Fase 5 s73: kind entries with explicit Half/Full
    // PathBlend distinction. Selecting either PB entry collapses the stack to
    // a single PB pass with the chosen mode (handled in on_kind_change).
    static std::vector<ZoneUI::KindEntry> kind_entries_for_slots(int n)
    {
        using PBMode = Slic3r::PathBlendPassConfig::Mode;
        std::vector<ZoneUI::KindEntry> out;
        // NEOTKO_SANDWICH_TAG s119 (EMPTY model): None is NOT a per-row kind. Empty
        // is a zone-level, exclusive, single-pass state (the Enabled checkbox) — it
        // can never be one band among several. So a multi-pass stack only offers
        // Solid here; "no effect" is expressed by disabling the whole zone.
        if (n > 1) {
            out.push_back({ Kind::Solid,     -1, _L("Solid") });
        }
        out.push_back({ Kind::ColorMix,  -1,                _L("ColorStitch") }); // NEOTKO_COLORSTITCH_TAG
        out.push_back({ Kind::PathBlend, (int)PBMode::Half, _L("PathBlend Half") });
        out.push_back({ Kind::PathBlend, (int)PBMode::Full, _L("PathBlend Full") });
        return out;
    }

    // NEOTKO_SANDWICH_TAG — read a ColorMix gradient int key for a given pass:
    // the pass's per-lámina override (pass.colormix.kv) first, region config as
    // fallback. `full_key` is the full region-key name (e.g. interlayer_colormix_tool_a).
    int cm_pass_int(const Slic3r::SurfacePass* p, const std::string& full_key, int dflt) const
    {
        if (p && p->colormix.present) {
            auto it = p->colormix.kv.find(full_key);
            if (it != p->colormix.kv.end()) {
                try { return std::stoi(it->second); } catch (...) {}
            }
        }
        if (auto* o = m_config->option<ConfigOptionInt>(full_key)) return o->value;
        return dflt;
    }
    double cm_pass_dbl(const Slic3r::SurfacePass* p, const std::string& full_key, double dflt) const
    {
        if (p && p->colormix.present) {
            auto it = p->colormix.kv.find(full_key);
            if (it != p->colormix.kv.end()) {
                try { return std::stod(it->second); } catch (...) {}
            }
        }
        if (auto* o = m_config->option<ConfigOptionFloat>(full_key)) return o->value;
        return dflt;
    }
    std::string cm_pass_str(const Slic3r::SurfacePass* p, const std::string& full_key,
                            const std::string& dflt) const
    {
        if (p && p->colormix.present) {
            auto it = p->colormix.kv.find(full_key);
            if (it != p->colormix.kv.end()) return it->second;
        }
        if (auto* o = m_config->option<ConfigOptionString>(full_key)) return o->value;
        return dflt;
    }

    // NEOTKO_SANDWICH_TAG — s80: build the REAL per-line ColorMix tool sequence
    // for a pass preview. Mirrors SurfaceColorMix::assign_and_group_tools dither
    // + repetitions + invert, reading the pass's per-lámina override (region
    // fallback). Returns physical tool indices; -1 = unknown (mixed digit 5-9).
    std::vector<int> colormix_preview_seq(int z, const Slic3r::SurfacePass& p, int N) const
    {
        const std::string pre = (z == 1) ? "interlayer_colormix_penu_"
                                         : "interlayer_colormix_";
        const int    mode    = cm_pass_int(&p, pre + "mode", 0);
        const int    pct_a   = cm_pass_int(&p, pre + "pct_a", 50);
        const int    pct_b   = cm_pass_int(&p, pre + "pct_b", 33);
        const int    easing  = cm_pass_int(&p, pre + "easing", 0);
        const double gamma   = cm_pass_dbl(&p, pre + "gamma", 1.0);
        const double overlap = cm_pass_dbl(&p, pre + "overlap", 0.6);
        const int    reps    = std::max(1, cm_pass_int(&p, pre + "repetitions", 1));
        const int    ta      = cm_pass_int(&p, pre + "tool_a", 0);
        const int    tb      = cm_pass_int(&p, pre + "tool_b", 1);
        const int    tc      = cm_pass_int(&p, pre + "tool_c", 2);
        const int    td      = cm_pass_int(&p, pre + "tool_d", 3);
        const int    build_N = (reps > 1) ? std::max(2, N / reps) : N;
        std::vector<int> seq;
        if (mode == 1) {
            seq = Slic3r::SurfaceColorMix::build_dithered_tools_2color(
                build_N, ta, tb, pct_a, easing, gamma);
        } else if (mode == 2) {
            seq = Slic3r::SurfaceColorMix::build_dithered_tools_3color(
                build_N, ta, tb, tc, pct_a, pct_b, easing, gamma, overlap);
        } else if (mode == 3) {
            seq = Slic3r::SurfaceColorMix::build_custom_bands(
                build_N, ta, cm_pass_int(&p, pre + "band_count_a", 0),
                         tb, cm_pass_int(&p, pre + "band_count_b", 0),
                         tc, cm_pass_int(&p, pre + "band_count_c", 0),
                         td, cm_pass_int(&p, pre + "band_count_d", 0));
        } else {
            const char* pat_key = (z == 0) ? "interlayer_colormix_pattern_top"
                                           : "interlayer_colormix_pattern_penultimate";
            const std::string pat = cm_pass_str(&p, pat_key, "");
            for (char c : pat) {
                if      (c >= '1' && c <= '4') seq.push_back(c - '1');
                else if (c >= '5' && c <= '9') seq.push_back(-1);
            }
            if (seq.empty()) seq.push_back(-1);
        }
        if (reps > 1 && mode >= 1 && mode <= 3 && !seq.empty() && (int)seq.size() < N) {
            const int period = (int)seq.size();
            std::vector<int> tiled; tiled.reserve(N);
            for (int i = 0; i < N; ++i) tiled.push_back(seq[i % period]);
            seq.swap(tiled);
        }
        if (cm_pass_int(&p, pre + "invert", 0) != 0 && seq.size() > 1)
            std::reverse(seq.begin(), seq.end());
        return seq;
    }

    // Read the ColorMix gradient tools for chip previews. When `p` carries a
    // per-lámina override the tools come from it; otherwise the shared region config.
    std::vector<int> colormix_tools(int z, const Slic3r::SurfacePass* p = nullptr) const
    {
        const std::string pre = (z == 1) ? "interlayer_colormix_penu_"
                                         : "interlayer_colormix_";
        std::vector<int> out;
        for (const char* s : { "tool_a", "tool_b", "tool_c", "tool_d" }) {
            const int v = cm_pass_int(p, pre + s, -1);
            if (v >= 0) out.push_back(v);
        }
        if (out.empty()) out = { 0, 1 };
        return out;
    }
    std::vector<int> pathblend_tools(int z) const
    {
        const char* zkey = (z == 1) ? "pathblend_penu" : "pathblend_top";
        std::string blob;
        if (auto* o = m_config->option<ConfigOptionString>(zkey)) blob = o->value;
        std::vector<int> out;
        if (!blob.empty()) {
            const Slic3r::PathBlendPassConfig pb =
                Slic3r::PathBlendPassConfig::from_blob_json(blob);
            for (int i = 0; i < pb.num_passes && i < 4; ++i)
                if (pb.tool[i] >= 0) out.push_back(pb.tool[i]);
        }
        if (out.empty()) out = { 0, 1 };
        return out;
    }

    // Force a stack into a valid shape (1..3 passes, slot rule, PathBlend solo).
    void sanitize_stack(int z)
    {
        Slic3r::SurfacePassStack& st = m_stack[z];
        if ((int)st.passes.size() > Slic3r::SurfacePassStack::kMaxPasses)
            st.passes.resize(Slic3r::SurfacePassStack::kMaxPasses);
        // PathBlend is always a single full-height pass.
        bool has_pb = false;
        for (const auto& p : st.passes)
            if (p.kind == Kind::PathBlend) has_pb = true;
        if (has_pb && st.passes.size() > 1) {
            Slic3r::SurfacePass pb;
            pb.kind = Kind::PathBlend; pb.ratio = 1.0;
            st.passes.assign(1, pb);
        }
        // NEOTKO_SANDWICH_TAG s119 (EMPTY model): a lone None pass is the canonical
        // Empty zone (explicit passthrough loaded from a [None] blob). Keep it as a
        // DISABLED zone with its single None pass so commit()'s to_json() writes the
        // explicit [None] blob (not ""→synthesize, which is what captures the zone).
        // Do NOT promote it to ColorMix — Empty is the Enabled-checkbox-off state.
        if (st.passes.size() == 1 && st.passes[0].kind == Kind::None) {
            st.enabled = false;
        } else if (st.passes.size() == 1 && st.passes[0].kind == Kind::Solid) {
            st.passes[0].kind = Kind::ColorMix;   // lone Solid is meaningless
        }
        if (st.passes.empty())
            st.enabled = false;                   // nothing to show
    }

    // NEOTKO_SANDWICH_TAG — Fase 6b: re-sync the WHOLE dialog UI to the current
    // m_stack[] without destroying any window. Mirrors what the constructor's
    // build_ui() tail does (header widgets + refresh_rows + sync_zone_enabled),
    // but post-construction. This is the in-place refresh the s81 note said a
    // "Load into dialog" would need — refresh_rows only shows/hides the fixed row
    // panels (never frees them), so it is safe on macOS (no live-NSView free).
    void reload_ui_from_stack()
    {
        for (int z = 0; z < 2; ++z) {
            sanitize_stack(z);
            m_ui[z].enable_chk->SetValue(m_stack[z].enabled && !m_stack[z].passes.empty());
            const int n0 = std::max(1, (int)m_stack[z].passes.size());
            m_ui[z].slots_rb->SetSelection(std::min(3, n0) - 1);
            // s118: perim_chk único (sólo z==0), refleja el estado combinado.
            if (m_ui[z].perim_chk)
                m_ui[z].perim_chk->SetValue(m_stack[0].perimeter_override || m_stack[1].perimeter_override);
            refresh_rows(z);
            sync_zone_enabled(z);
        }
        Layout();
    }

    // NEOTKO_SANDWICH_TAG — Fase 6b: load a saved profile's stacks into the
    // dialog so it can be edited / re-saved (the tests need this to iterate on
    // debug profiles without losing them). Deferred-call safe: invoked AFTER the
    // Manage modal closes, so no parent UI is mutated while a child modal lives.
    void load_profile_into_dialog(int id)
    {
        const auto* p = Slic3r::SurfaceEffectProfileManager::get().find(id);
        if (!p) return;
        m_stack[0] = Slic3r::SurfacePassStack::from_json(p->stack_top_json);
        m_stack[1] = Slic3r::SurfacePassStack::from_json(p->stack_penu_json);
        reload_ui_from_stack();
    }

    // ------------------------------------------------------------------- build
    // ------------------------------------------------------------------ Blend Suggestion data
    // NEOTKO_SANDWICH_TAG — Fase 7 (s84b): mirror of refresh_mix_opts from
    // the old SurfaceColorMixerDialog, scoped to virtual-only options.
    void load_mix_opts()
    {
        m_bs_mix_opts.clear();
        std::string mixed_defs;
        if (auto* o = wxGetApp().preset_bundle->project_config
                          .option<ConfigOptionString>("mixed_filament_definitions"))
            mixed_defs = o->value;
        if (mixed_defs.empty()) {
            if (auto* o = m_config->option<ConfigOptionString>("mixed_filament_definitions"))
                mixed_defs = o->value;
        }
        if (!mixed_defs.empty()) {
            for (auto& opt : Slic3r::SurfaceColorMix::get_mix_options(mixed_defs, m_fcolors))
                if (!opt.is_physical)
                    m_bs_mix_opts.push_back(opt);
        }
    }

    void refresh_bs_target_swatch()
    {
        if (!m_bs_swatch_target) return;
        const int sel = m_bs_combo_target ? m_bs_combo_target->GetSelection() : -1;
        wxColour c(128,128,128);
        if (sel >= 0 && sel < (int)m_bs_mix_opts.size()) {
            const std::string& dc = m_bs_mix_opts[sel].display_color;
            if (dc.size() >= 7 && dc[0] == '#') {
                unsigned long rgb = 0;
                if (wxString::FromUTF8(dc.substr(1)).ToULong(&rgb, 16))
                    c = wxColour((rgb>>16)&0xFF, (rgb>>8)&0xFF, rgb&0xFF);
            }
        }
        m_bs_swatch_target->SetBackgroundColour(c);
        m_bs_swatch_target->Refresh();
    }

    // ------------------------------------------------------------------ TD preview helpers
    // NEOTKO_SANDWICH_TAG — Fase 7 (s84): SandwichDialog-native Beer-Lambert
    // blend preview. Walks m_stack[z].passes (authoritative) instead of the
    // legacy keys the old SurfaceColorMixerDialog reads. Solid → 1 tool at
    // pass.ratio; ColorMix → histogram of pattern digits (kv override or
    // region-config fallback); PathBlend → 50/50 cap+ramp from blob.
    // Single-channel TD (same maths as the old dialog) — Color Science N1.3
    // (TD por canal) entrará cuando se ataque la roadmap futura.
    // NEOTKO_COLORSCI_TAG A — CS-1 (parcial): la expansión de passes y el
    // blend paralelo viven ahora en libslic3r/ColorSci/StackFlatten (port 1:1
    // de la lógica que estaba aquí — borrada en el mismo cambio, regla N1.1).
    // Paridad: con TD escalar (r=g=b) ColorSci::blend_parallel es equivalente
    // al weighted-average original. stacked_preview_color() no se toca — ya
    // composita sobre esta función.
    wxColour blend_preview_zone(int z, float* out_w = nullptr) const
    {
        const auto& st = m_stack[z];
        if (!st.enabled || st.passes.empty()) {
            if (out_w) *out_w = 0.f;
            return GetBackgroundColour();
        }
        Slic3r::ColorSci::Material mats[4];
        for (int t = 0; t < 4; ++t)
            mats[t] = Slic3r::ColorSci::material_from_hex(
                t < (int)m_fcolors.size() ? m_fcolors[t] : std::string(), m_td[t]);
        // fallback de patrón para passes ColorMix sin kv (config viva del diálogo)
        std::string fallback;
        const char* k = (z == 1) ? "interlayer_colormix_pattern_penultimate"
                                 : "interlayer_colormix_pattern_top";
        if (auto* o = m_config->option<ConfigOptionString>(k)) fallback = o->value;

        float rgb[3], w = 0.f;
        if (!Slic3r::ColorSci::zone_colour(st, z == 1, fallback, mats, rgb, &w)) {
            if (out_w) *out_w = 0.f;
            return wxColour(180, 180, 180);   // mismo gris fallback que antes
        }
        if (out_w) *out_w = w;
        return wxColour((unsigned char)std::min(255.f, rgb[0] * 255.f),
                        (unsigned char)std::min(255.f, rgb[1] * 255.f),
                        (unsigned char)std::min(255.f, rgb[2] * 255.f));
    }

    wxColour stacked_preview_color() const
    {
        float op = 0.f;
        const wxColour c_top  = blend_preview_zone(0, &op);
        const wxColour c_penu = blend_preview_zone(1);
        const float a  = std::clamp(op, 0.f, 1.f);
        const float ia = 1.f - a;
        return wxColour(
            (unsigned char)(c_top.Red()   * a + c_penu.Red()   * ia),
            (unsigned char)(c_top.Green() * a + c_penu.Green() * ia),
            (unsigned char)(c_top.Blue()  * a + c_penu.Blue()  * ia));
    }

    void refresh_td_previews()
    {
        float transmit_top = 0.f;
        blend_preview_zone(0, &transmit_top);
        if (m_stacked_sw_top)  m_stacked_sw_top ->Refresh();
        if (m_stacked_sw_penu) m_stacked_sw_penu->Refresh();
        if (m_stacked_swatch)  m_stacked_swatch ->Refresh();
        if (m_lbl_opacity_top)
            m_lbl_opacity_top->SetLabel(
                wxString::Format(_L("transmit=%.2f"),
                                  std::clamp(transmit_top, 0.f, 1.f)));
        // NEOTKO_COLORSCI_TAG GD2 — la rampa del Gradient Designer depende de
        // los TD: repintar la tira (los stacks no cambian, solo su color).
        if (m_gd_strip) m_gd_strip->Refresh();
        // Per-pass previews (rows) are RGB-only swatches today; nothing to refresh.
    }

    // ------------------------------------------------------------------ TD panel builder
    wxSizer* build_td_panel()
    {
        const int PAD = 6;
        auto* sb = new wxStaticBoxSizer(wxVERTICAL, this, _L("Filament & TD"));
        wxWindow* host = sb->GetStaticBox();

        auto* note = new wxStaticText(host, wxID_ANY,
            _L("Transmission Distance (TD)"));
        note->SetForegroundColour(wxColour(90, 90, 90));
        sb->Add(note, 0, wxALL, PAD);

        auto* grid = new wxFlexGridSizer(4, 4, 4, 8);
        grid->AddGrowableCol(2, 1);
        for (int i = 0; i < 4; ++i) {
            auto* sw = new wxPanel(host, wxID_ANY, wxDefaultPosition, wxSize(18, 18));
            sw->SetBackgroundColour(tool_colour(i));
            m_sw_td[i] = sw;
            grid->Add(sw, 0, wxALIGN_CENTER_VERTICAL);

            grid->Add(new wxStaticText(host, wxID_ANY, wxString::Format("T%d", i + 1)),
                      0, wxALIGN_CENTER_VERTICAL);

            const int iv = (int)(m_td[i] * 100.f + 0.5f);
            auto* sl = new wxSlider(host, wxID_ANY, iv, 1, 1000,
                                    wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
            m_sl_td[i] = sl;
            grid->Add(sl, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);

            auto* lbl = new wxStaticText(host, wxID_ANY,
                wxString::Format("%.2f", m_td[i]),
                wxDefaultPosition, wxSize(38, -1));
            m_lbl_td[i] = lbl;
            grid->Add(lbl, 0, wxALIGN_CENTER_VERTICAL);

            sl->Bind(wxEVT_SLIDER, [this, i](wxCommandEvent&) {
                m_td[i] = m_sl_td[i]->GetValue() / 100.f;
                m_lbl_td[i]->SetLabel(wxString::Format("%.2f", m_td[i]));
                refresh_td_previews();
                gd_schedule_recalc();   // regenera gradient/flat/mixed (debounced)
            });
        }
        sb->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);

        // Stacked preview row (Top × Penu Beer-Lambert).
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(host, wxID_ANY, _L("Top:")),
                     0, wxALIGN_CENTER_VERTICAL);
            m_stacked_sw_top = new wxPanel(host, wxID_ANY, wxDefaultPosition, wxSize(28, 18));
            m_stacked_sw_top->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_stacked_sw_top->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxPaintDC dc(m_stacked_sw_top);
                dc.SetBackground(wxBrush(blend_preview_zone(0)));
                dc.Clear();
            });
            row->Add(m_stacked_sw_top, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
            row->Add(new wxStaticText(host, wxID_ANY, _L("  Penu:")),
                     0, wxALIGN_CENTER_VERTICAL);
            m_stacked_sw_penu = new wxPanel(host, wxID_ANY, wxDefaultPosition, wxSize(28, 18));
            m_stacked_sw_penu->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_stacked_sw_penu->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxPaintDC dc(m_stacked_sw_penu);
                dc.SetBackground(wxBrush(blend_preview_zone(1)));
                dc.Clear();
            });
            row->Add(m_stacked_sw_penu, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
            row->Add(new wxStaticText(host, wxID_ANY, _L("  Result:")),
                     0, wxALIGN_CENTER_VERTICAL);
            m_stacked_swatch = new wxPanel(host, wxID_ANY, wxDefaultPosition, wxSize(40, 18));
            m_stacked_swatch->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_stacked_swatch->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxPaintDC dc(m_stacked_swatch);
                dc.SetBackground(wxBrush(stacked_preview_color()));
                dc.Clear();
            });
            row->Add(m_stacked_swatch, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
            m_lbl_opacity_top = new wxStaticText(host, wxID_ANY, wxEmptyString,
                                                 wxDefaultPosition, wxSize(90, -1));
            m_lbl_opacity_top->SetForegroundColour(wxColour(90, 90, 90));
            row->Add(m_lbl_opacity_top, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
            sb->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);
        }

        // use_virtual (Mixed Filament digits 5-9 in ColorMix pattern editor).
        {
            bool cur_uv = true;
            if (auto* o = m_config->option<ConfigOptionBool>("interlayer_colormix_use_virtual"))
                cur_uv = o->value;
            m_chk_use_virtual = new wxCheckBox(host, wxID_ANY,
                _L("Use Mixed Filament colors in pattern"));
            m_chk_use_virtual->SetValue(cur_uv);
            m_chk_use_virtual->SetToolTip(
                _L("When enabled, the pattern editor shows Mixed Filament virtual colors\n"
                   "(e.g. 'F1+F2') as clickable buttons alongside physical filaments.\n"
                   "Pattern digits 5-9 reference these virtual colors.\n"
                   "Requires Mixed Filaments to be defined in the filament panel."));
            sb->Add(m_chk_use_virtual, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);
        }

        // NEOTKO_SANDWICH_TAG — Fase 7 (s84b): Blend Suggestion (Target +
        // Mode + Calculate + ΔE). Portado del SurfaceColorMixerDialog viejo;
        // Calculate escribe a m_stack[] (autoritativo) en vez de las legacy
        // multipass_* keys. La checkbox "MultiPass Perimeter Override" no se
        // porta — ya existe per-zone en SandwichDialog (m_ui[z].perim_chk).
#if 0   // s120: panel "Blend Suggestion" del SandwichDialog RETIRADO (legacy mp_suggest, superseded por ColorStitch Studio → Match, ΔE2000). El pool (load_mix_opts) y el Studio siguen vivos. Pendiente excisión dura.
        {
            sb->Add(new wxStaticLine(host), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, PAD);
            auto* bs_lbl = new wxStaticText(host, wxID_ANY,
                _L("Blend Suggestion — Beer-Lambert optimizer"));
            wxFont f = bs_lbl->GetFont(); f.MakeBold(); bs_lbl->SetFont(f);
            sb->Add(bs_lbl, 0, wxLEFT | wxRIGHT | wxTOP, PAD);

            // Target picker row
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(host, wxID_ANY, _L("Target colour:")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                wxArrayString labels;
                for (auto& opt : m_bs_mix_opts)
                    labels.Add(wxString::FromUTF8(opt.label));
                if (labels.IsEmpty()) labels.Add(_L("(no MixedColor defined)"));
                m_bs_combo_target = new wxComboBox(host, wxID_ANY,
                    labels.IsEmpty() ? wxString() : labels[0],
                    wxDefaultPosition, wxSize(170, -1), labels, wxCB_READONLY);
                m_bs_combo_target->SetToolTip(
                    _L("Select the virtual MixedColor whose display colour is the blend target."));
                m_bs_combo_target->Enable(!m_bs_mix_opts.empty());
                row->Add(m_bs_combo_target, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

                m_bs_swatch_target = new wxPanel(host, wxID_ANY,
                    wxDefaultPosition, wxSize(18, 18));
                m_bs_swatch_target->SetToolTip(
                    _L("Display colour of the selected MixedColor target."));
                refresh_bs_target_swatch();
                m_bs_combo_target->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
                    refresh_bs_target_swatch();
                });
                row->Add(m_bs_swatch_target, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

                auto* btn_refresh = new wxButton(host, wxID_ANY, L"↺",
                    wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                btn_refresh->SetToolTip(
                    _L("Refresh — re-read Mixed Filament definitions from the current project."));
                btn_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                    load_mix_opts();
                    if (!m_bs_combo_target) return;
                    m_bs_combo_target->Clear();
                    if (m_bs_mix_opts.empty()) {
                        m_bs_combo_target->Append(_L("(no MixedColor defined)"));
                        m_bs_combo_target->SetSelection(0);
                        m_bs_combo_target->Enable(false);
                    } else {
                        for (auto& opt : m_bs_mix_opts)
                            m_bs_combo_target->Append(wxString::FromUTF8(opt.label));
                        m_bs_combo_target->SetSelection(0);
                        m_bs_combo_target->Enable(true);
                    }
                    refresh_bs_target_swatch();
                });
                row->Add(btn_refresh, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
                sb->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);
            }

            // Mode radios
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(new wxStaticText(host, wxID_ANY, _L("Mode:")),
                         0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
                m_bs_rb_top = new wxRadioButton(host, wxID_ANY, _L("Top layer only"),
                    wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
                m_bs_rb_joint = new wxRadioButton(host, wxID_ANY,
                    _L("Top + Penultimate"));
                m_bs_rb_joint->SetValue(true);
                m_bs_rb_top->SetToolTip(
                    _L("Apply suggested passes to the Top zone only."));
                m_bs_rb_joint->SetToolTip(
                    _L("Apply the same passes to both Top and Penultimate zones.\n"
                       "Beer-Lambert models this as a stacked double layer — allows\n"
                       "smaller per-pass ratios while maintaining physical adhesion."));
                row->Add(m_bs_rb_top,   0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
                row->Add(m_bs_rb_joint, 0, wxALIGN_CENTER_VERTICAL);
                sb->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);
            }

            // Calculate + result row
            {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                auto* btn_calc = new wxButton(host, wxID_ANY, _L("Calculate ▶"),
                    wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                btn_calc->Enable(!m_bs_mix_opts.empty());

                m_bs_swatch = new wxPanel(host, wxID_ANY,
                    wxDefaultPosition, wxSize(22, 22));
                m_bs_swatch->SetBackgroundColour(wxColour(128, 128, 128));
                m_bs_swatch->SetToolTip(
                    _L("Simulated blend colour after Beer-Lambert optimisation."));

                m_bs_lbl_score = new wxStaticText(host, wxID_ANY, _L("  ΔE: ---"));
                m_bs_lbl_score->SetToolTip(
                    _L("CIE76 RGB-cube colour distance between simulated result and target.\n"
                       "<5 excellent, 5-10 good, >10 poor approximation.\n"
                       "Color Science N1.1 will upgrade this to CIEDE2000 in Lab space."));

                btn_calc->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                    if (m_bs_mix_opts.empty() || !m_bs_combo_target) return;
                    const int sel = m_bs_combo_target->GetSelection();
                    if (sel < 0 || sel >= (int)m_bs_mix_opts.size()) return;
                    const auto& opt = m_bs_mix_opts[sel];

                    // Target display color → RGB [0..255]
                    const std::string& dc = opt.display_color;
                    if (dc.size() < 7 || dc[0] != '#') return;
                    unsigned long rgb_v = 0;
                    if (!wxString::FromUTF8(dc.substr(1)).ToULong(&rgb_v, 16)) return;
                    const double t_r = (double)((rgb_v >> 16) & 0xFF);
                    const double t_g = (double)((rgb_v >>  8) & 0xFF);
                    const double t_b = (double)( rgb_v        & 0xFF);

                    auto cur_slots = [&](int z) {
                        const int n = (int)m_stack[z].passes.size();
                        return n > 0 ? std::clamp(n, 1, 3) : 2;
                    };
                    const int top_passes  = cur_slots(0);
                    const int penu_passes = cur_slots(1);

                    const double LH = layer_height_mm();
                    const double min_r = std::max(0.05, kMinPassMM / LH);

                    // NEOTKO_SANDWICH_TAG — Fase 7 (s84d) Calculate B: exhaustive
                    // search over (tool sequence) × (ratio composition), Beer-
                    // Lambert + ΔE_RGB. Pool = 4 physical filaments; tools may
                    // repeat. Ratios discretized on a step=0.1 grid summing to 1,
                    // with floor = min_r (enforced 0.04 mm extrusion). Joint mode
                    // is greedy: Penu best alone, then Top stacked over it.
                    // Color Science N1.1 (Lab/CIEDE2000) and N3.1 (NNLS) refinan
                    // este search — esta es la versión "barata" del N3.2.
                    constexpr int kPool   = 4;
                    constexpr int K       = 10;   // 0.1 step
                    const     int min_int = (int)std::ceil(min_r * K - 1e-6);

                    // Enumerate ratio compositions summing to K, each ≥ min_int.
                    std::vector<std::vector<double>> ratio_grids[4]; // by n=1..3
                    auto fill_grid = [&](int n) {
                        std::vector<std::vector<double>>& out = ratio_grids[n];
                        out.clear();
                        if (n == 1) { out.push_back({1.0}); return; }
                        std::vector<int> acc(n, 0);
                        std::function<void(int,int)> rec = [&](int i, int rem) {
                            if (i == n - 1) {
                                if (rem >= min_int) {
                                    acc[i] = rem;
                                    std::vector<double> r(n);
                                    for (int j = 0; j < n; ++j) r[j] = (double)acc[j] / K;
                                    out.push_back(std::move(r));
                                }
                                return;
                            }
                            for (int v = min_int; v <= rem - min_int*(n-1-i); ++v) {
                                acc[i] = v; rec(i+1, rem - v);
                            }
                        };
                        rec(0, K);
                    };
                    fill_grid(1); fill_grid(2); fill_grid(3);

                    // Beer-Lambert color of a (tool, ratio) stack. Returns
                    // {Rfg, Gfg, Bfg, opacity_total}. Same maths as
                    // blend_preview_zone (weighted average by per-pass opacity).
                    auto bl = [&](const std::vector<int>& tools,
                                  const std::vector<double>& ratios) {
                        double tr=0, tg=0, tb=0, tw=0;
                        for (size_t i = 0; i < tools.size(); ++i) {
                            const int t = std::clamp(tools[i], 0, 3);
                            const double td = std::max(0.01, (double)m_td[t]);
                            const wxColour col = tool_colour(t);
                            const double op = 1.0 - std::pow(0.1, ratios[i] / td);
                            tr += col.Red()  * op;
                            tg += col.Green()* op;
                            tb += col.Blue() * op;
                            tw += op;
                        }
                        std::array<double,4> out;
                        if (tw < 1e-9) { out = {180.0,180.0,180.0, 0.0}; }
                        else            { out = {tr/tw, tg/tw, tb/tw, std::clamp(tw, 0.0, 1.0)}; }
                        return out;
                    };

                    // Search best (tools, ratios) of length n minimising ΔE² to
                    // (tr, tg, tb), optionally composited Beer-Lambert over a
                    // background (br, bg, bb). base_alpha < 1 → fg shows over bg.
                    auto search = [&](int n, double tr_, double tg_, double tb_,
                                      double br, double bg, double bb,
                                      bool composite) {
                        struct R { std::vector<int> tools; std::vector<double> ratios; double de2; };
                        R best; best.de2 = 1e18;
                        std::vector<int> tools(n, 0);
                        const auto& rgrid = ratio_grids[n];
                        std::function<void(int)> rec = [&](int depth) {
                            if (depth == n) {
                                for (const auto& r : rgrid) {
                                    const auto c = bl(tools, r);
                                    double rr, gg, bb_;
                                    if (composite) {
                                        const double a = c[3], ia = 1.0 - a;
                                        rr = c[0]*a + br*ia;
                                        gg = c[1]*a + bg*ia;
                                        bb_= c[2]*a + bb*ia;
                                    } else {
                                        rr = c[0]; gg = c[1]; bb_ = c[2];
                                    }
                                    const double dr = rr - tr_;
                                    const double dg = gg - tg_;
                                    const double db = bb_- tb_;
                                    const double de2 = dr*dr + dg*dg + db*db;
                                    if (de2 < best.de2) {
                                        best.de2    = de2;
                                        best.tools  = tools;
                                        best.ratios = r;
                                    }
                                }
                                return;
                            }
                            for (int t = 0; t < kPool; ++t) {
                                tools[depth] = t;
                                rec(depth + 1);
                            }
                        };
                        rec(0);
                        return best;
                    };

                    auto write_zone = [&](int z, const std::vector<int>& tools,
                                          const std::vector<double>& ratios) {
                        Slic3r::SurfacePassStack& st = m_stack[z];
                        st.enabled = true;
                        st.passes.clear();
                        for (size_t i = 0; i < tools.size(); ++i) {
                            Slic3r::SurfacePass p;
                            p.kind       = Kind::Solid;
                            p.solid_tool = std::clamp(tools[i], 0, 3);
                            p.ratio      = ratios[i];
                            p.angle      = -1;
                            st.passes.push_back(p);
                        }
                        sanitize_stack(z);
                        refresh_rows(z);
                        sync_zone_enabled(z);
                    };

                    const bool joint = m_bs_rb_joint && m_bs_rb_joint->GetValue();
                    if (joint) {
                        // Penu first: best alone match to target.
                        auto bp = search(penu_passes, t_r, t_g, t_b,
                                         0,0,0, /*composite=*/false);
                        const auto penu_c = bl(bp.tools, bp.ratios);
                        // Top: stacked over Penu result (composite=true).
                        auto bt = search(top_passes, t_r, t_g, t_b,
                                         penu_c[0], penu_c[1], penu_c[2],
                                         /*composite=*/true);
                        write_zone(0, bt.tools, bt.ratios);
                        write_zone(1, bp.tools, bp.ratios);
                    } else {
                        // Top-only: best alone match.
                        auto bt = search(top_passes, t_r, t_g, t_b,
                                         0,0,0, /*composite=*/false);
                        write_zone(0, bt.tools, bt.ratios);
                    }

                    refresh_td_previews();

                    // Predicted result + ΔE for the display swatch (same maths
                    // used by the search, so this matches what the optimiser saw).
                    const wxColour rc = joint ? stacked_preview_color()
                                              : blend_preview_zone(0);
                    const double dr = rc.Red()   - t_r;
                    const double dg = rc.Green() - t_g;
                    const double db = rc.Blue()  - t_b;
                    const double de = std::sqrt(dr*dr + dg*dg + db*db) * 100.0
                                      / (255.0 * std::sqrt(3.0));
                    if (m_bs_swatch) {
                        m_bs_swatch->SetBackgroundColour(rc);
                        m_bs_swatch->Refresh();
                    }
                    if (m_bs_lbl_score) {
                        m_bs_lbl_score->SetLabel(wxString::Format(_L("  ΔE: %.1f"), de));
                        m_bs_lbl_score->SetForegroundColour(
                            de < 5.0  ? wxColour(30,140,30) :
                            de < 10.0 ? wxColour(190,130,0) : wxColour(180,40,40));
                        m_bs_lbl_score->Refresh();
                    }
                    Layout();
                });

                row->Add(btn_calc,       0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
                row->Add(m_bs_swatch,    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
                row->Add(m_bs_lbl_score, 1, wxALIGN_CENTER_VERTICAL);
                sb->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);
            }
        }
#endif  // s120: fin panel Blend Suggestion (SandwichDialog) eliminado

        return sb;
    }

    // ------------------------------------------------------------------ Lane mode builder
    wxSizer* build_lane_panel()
    {
        const int PAD = 6;
        auto* sb = new wxStaticBoxSizer(wxVERTICAL, this, _L("Line distribution mode"));
        wxWindow* host = sb->GetStaticBox();

        wxArrayString labels;
        labels.Add(_L("Default — line index (safe, original)"));
        labels.Add(_L("GeoSort (A) — sort by perpendicular position"));
        labels.Add(_L("LaneQuant (B) — quantize to geometric lanes"));
        labels.Add(_L("DirCluster (C) — cluster by direction, then quantize"));

        int cur_lm = 0;
        if (auto* o = m_config->option<ConfigOptionInt>("surface_color_mix_lane_mode"))
            cur_lm = std::clamp(o->value, 0, 3);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(host, wxID_ANY, _L("Mode:")),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_choice_lane_mode = new wxChoice(host, wxID_ANY,
            wxDefaultPosition, wxSize(280, -1), labels);
        m_choice_lane_mode->SetSelection(cur_lm);
        m_choice_lane_mode->SetToolTip(
            _L("How pattern slots are assigned to individual extrusion lines.\n\n"
               "0 Default — slot = line_index mod n_slots (legacy).\n"
               "1 GeoSort — sort lines by position perpendicular to fill direction.\n"
               "2 LaneQuant — slot = quantized geometric lane (continuous stripes).\n"
               "3 DirCluster — cluster by direction first, then LaneQuant per cluster.\n\n"
               "Applies to ColorMix + PathBlend in Top + Penultimate."));
        m_choice_lane_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            if (!m_choice_lane_mode) return;
            const int v = std::clamp(m_choice_lane_mode->GetSelection(), 0, 3);
            if (auto* o = m_config->option<ConfigOptionInt>("surface_color_mix_lane_mode"))
                o->value = v;
            m_on_change("surface_color_mix_lane_mode");
        });
        row->Add(m_choice_lane_mode, 1, wxALIGN_CENTER_VERTICAL);
        sb->Add(row, 0, wxEXPAND | wxALL, PAD);

        auto* note = new wxStaticText(host, wxID_ANY,
            _L("Experimental. Default = original behaviour (no risk).\n"
               "Modes A/B/C improve pattern fidelity on irregular shapes."));
        note->SetForegroundColour(wxColour(100, 100, 100));
        sb->Add(note, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);

        return sb;
    }

    // ------------------------------------------------------------- Gradient Designer
    // NEOTKO_COLORSCI_TAG GD1+GD2+GD3 — diseño de rampas A→penu→B (port in-app
    // de docs/MULTITEST/gen_gradient_grid.py). El usuario barre el split de
    // los dos SOLID top sobre un penu ColorStitch y exporta los swatches que
    // le gusten como perfiles del 3D Painter (mismo camino que on_save_profile:
    // solo blobs autoritativos + preview_argb).

    // ---- helpers de modo --------------------------------------------------
    // 0 = Gradient ramp (manual), 1 = Flat (predict solids), 2 = Mixed (predict
    // dither+top). En 1/2 la tira muestra m_gd_recipes (predicción).
    int gd_mode() const { return m_gd_mode ? m_gd_mode->GetSelection() : 0; }
    bool gd_is_predict() const { return gd_mode() != 0; }

    int gd_count() const
    {
        return gd_is_predict() ? (int)m_gd_recipes.size() : (int)m_gd_ramp.size();
    }

    void gd_materials(Slic3r::ColorSci::Material out[4]) const
    {
        for (int t = 0; t < 4; ++t)
            out[t] = Slic3r::ColorSci::material_from_hex(
                t < (int)m_fcolors.size() ? m_fcolors[t] : std::string(), m_td[t]);
    }

    // Color del swatch i según el modo activo (ramp = recompute en vivo con TD;
    // recipe = rgb ya predicho en gd_recalc).
    wxColour gd_colour_at(int i) const
    {
        if (gd_is_predict()) {
            if (i < 0 || i >= (int)m_gd_recipes.size()) return wxColour(180,180,180);
            const auto& c = m_gd_recipes[i].rgb;
            return wxColour((unsigned char)std::min(255.f, c[0]*255.f),
                            (unsigned char)std::min(255.f, c[1]*255.f),
                            (unsigned char)std::min(255.f, c[2]*255.f));
        }
        if (i < 0 || i >= (int)m_gd_ramp.size()) return wxColour(180,180,180);
        return gd_step_colour(m_gd_ramp[i]);
    }

    // Geometría de un swatch i de la tira (compartida por paint/hit-test).
    wxRect gd_swatch_rect(int i) const
    {
        const int N = gd_count();
        if (N <= 0 || !m_gd_strip) return wxRect();
        const wxSize sz = m_gd_strip->GetClientSize();
        const int gap = 3;
        int w = (sz.x - gap * (N + 1)) / std::max(1, N);
        w = std::clamp(w, 8, 64);
        const int h = std::max(28, sz.y - 2 * gap);
        return wxRect(gap + i * (w + gap), gap, w, h);
    }

    int gd_swatch_at(const wxPoint& p) const
    {
        for (int i = 0; i < gd_count(); ++i)
            if (gd_swatch_rect(i).Contains(p)) return i;
        return -1;
    }

    // Color previsto de un step: composición física apilada (penu abajo, top
    // B+A encima) con los TD del panel de la derecha — la tira reacciona en
    // vivo a los sliders TD vía refresh_td_previews().
    wxColour gd_step_colour(const Slic3r::ColorSci::GradientStep& g) const
    {
        Slic3r::ColorSci::Material mats[4];
        gd_materials(mats);
        const float bg[3] = { 0.f, 0.f, 0.f };
        float rgb[3];
        Slic3r::ColorSci::sandwich_colour_stacked(g.top, g.penu, mats, bg, rgb);
        return wxColour((unsigned char)std::min(255.f, rgb[0] * 255.f),
                        (unsigned char)std::min(255.f, rgb[1] * 255.f),
                        (unsigned char)std::min(255.f, rgb[2] * 255.f));
    }

    // Cargar una receta en el sandwich vivo (cambia las capas que el usuario
    // ve). Click en swatch de predicción → esto. reload_ui_from_stack repuebla
    // filas + checkboxes; refresh_td_previews actualiza el preview TD.
    void gd_load_recipe(const Slic3r::ColorSci::ColorRecipe& r)
    {
        m_stack[0] = r.top;
        m_stack[1] = r.penu;
        reload_ui_from_stack();
        refresh_td_previews();
    }

    Slic3r::ColorSci::PredictOptions gd_predict_opts() const
    {
        Slic3r::ColorSci::PredictOptions o;
        o.layer_height = layer_height_mm();
        return o;
    }

    // Debounce de regeneración tras cambios de TD: reinicia el one-shot timer;
    // al expirar (~150 ms sin más cambios) regenera la tira del modo activo.
    // En modos predicción esto recomputa m_gd_recipes con los nuevos TD (el
    // color va baked en la receta, un Refresh solo no basta). En Gradient el
    // color se recomputa en vivo al pintar, pero recalc tampoco molesta.
    void gd_schedule_recalc()
    {
        if (m_gd_td_timer.GetOwner() == this)
            m_gd_td_timer.StartOnce(150);
    }

    void gd_recalc()
    {
        if (!m_gd_strip) return;   // aún construyendo la página
        // Qualify explícito — `using namespace` aquí no inyecta nombres en el
        // ámbito de `Slic3r::GUI::<anon>` por reglas de lookup.
        namespace CS = Slic3r::ColorSci;
        const int mode = gd_mode();

        if (mode == 0) {
            // -------- modo Gradient: rampa manual --------
            CS::GradientSpec s;
            s.tool_a       = std::max(0, m_gd_tool_a->GetSelection());
            s.tool_b       = std::max(0, m_gd_tool_b->GetSelection());
            s.steps        = m_gd_steps->GetValue();
            s.split_min_mm = m_gd_min->GetValue();
            s.split_max_mm = m_gd_max->GetValue();
            s.layer_height = layer_height_mm();

            std::vector<std::string> warns;
            m_gd_ramp = CS::build_ramp(CS::sanitize(s, &warns));
            if (m_gd_selected.size() != m_gd_ramp.size())
                m_gd_selected.assign(m_gd_ramp.size(), true);

            if (m_gd_warn) {
                wxString w;
                for (const auto& msg : warns) {
                    if (!w.empty()) w += "  ·  ";
                    w += wxString::FromUTF8(msg);
                }
                m_gd_warn->SetLabel(w);
                const bool want_shown = !w.empty();
                if (m_gd_warn->IsShown() != want_shown) { m_gd_warn->Show(want_shown); Layout(); }
            }
            if (m_gd_sw_a) { m_gd_sw_a->SetBackgroundColour(tool_colour(s.tool_a)); m_gd_sw_a->Refresh(); }
            if (m_gd_sw_b) { m_gd_sw_b->SetBackgroundColour(tool_colour(s.tool_b)); m_gd_sw_b->Refresh(); }
        } else {
            // -------- modos Flat / Mixed: paleta-predicción --------
            CS::Material mats[4];
            gd_materials(mats);
            const CS::PredictOptions o = gd_predict_opts();
            // PR.1: frontera única — el dispatcher build_palette unifica las
            // tres familias. Flat/Mixed pasan por aquí; el ramp manual conserva
            // su path GradientStep (mismo build_ramp, vivo con los TD).
            m_gd_recipes = CS::build_palette(
                mode == 1 ? CS::PaletteKind::Flat : CS::PaletteKind::Mixed,
                mats, o);
            m_gd_match_idx = -1;
            if (m_gd_warn && m_gd_warn->IsShown()) { m_gd_warn->Show(false); Layout(); }
        }
        m_gd_strip->Refresh();
    }

    // Match inverso: target colour (mismo pool que Blend Suggestion) → mejor
    // receta flat/mixed por ΔE2000. Prepende el resultado a la paleta, lo
    // resalta y lo CARGA en el sandwich.
    void gd_on_match()
    {
        namespace CS = Slic3r::ColorSci;
        if (!m_gd_ptarget || m_bs_mix_opts.empty()) return;
        const int sel = m_gd_ptarget->GetSelection();
        if (sel < 0 || sel >= (int)m_bs_mix_opts.size()) return;
        const std::string& dc = m_bs_mix_opts[sel].display_color;
        if (dc.size() < 7 || dc[0] != '#') return;
        unsigned long v = 0;
        if (!wxString::FromUTF8(dc.substr(1)).ToULong(&v, 16)) return;
        const float target[3] = { ((v>>16)&0xFF)/255.f, ((v>>8)&0xFF)/255.f, (v&0xFF)/255.f };

        CS::Material mats[4];
        gd_materials(mats);
        const CS::PredictOptions o = gd_predict_opts();
        CS::ColorRecipe best = (gd_mode() == 1) ? CS::suggest_flat(target, mats, o)
                                                : CS::suggest_mixed(target, mats, o);
        // prepende al frente para que sea visible y resaltado
        m_gd_recipes.insert(m_gd_recipes.begin(), best);
        m_gd_match_idx = 0;
        if (m_gd_de)
            m_gd_de->SetLabel(wxString::Format(_L("best ΔE %.1f"), best.delta_e));
        gd_load_recipe(best);
        m_gd_strip->Refresh();
    }

    // Helper común: empaqueta un (top,penu,color) como SurfaceEffectProfile y
    // lo añade al manager. Mismo contrato que on_save_profile() — blobs
    // autoritativos, payloads legacy present=false (painter Fase 6b consume
    // los blobs), penu self-contained (no necesita bake).
    void gd_export_one(const wxString& name,
                       const Slic3r::SurfacePassStack& top,
                       const Slic3r::SurfacePassStack& penu,
                       const wxColour& c)
    {
        Slic3r::SurfaceEffectProfile p;
        p.name            = name.ToStdString();
        p.stack_top_json  = top.to_json();
        p.stack_penu_json = penu.to_json();
        p.preview_argb    = 0xFF000000u
                          | ((uint32_t)c.Red()   << 16)
                          | ((uint32_t)c.Green() <<  8)
                          |  (uint32_t)c.Blue();
        Slic3r::SurfaceEffectProfileManager::get().add(std::move(p));
    }

    void gd_on_export()
    {
        gd_recalc();
        const wxString base = m_gd_name->GetValue();
        int created = 0;

        if (!gd_is_predict()) {
            // -------- Gradient: exporta los swatches seleccionados de la rampa
            int n_sel = 0;
            for (bool b : m_gd_selected) n_sel += b ? 1 : 0;
            if (n_sel == 0) {
                wxMessageBox(_L("No swatches selected — click swatches in the ramp "
                                "to include them."),
                             _L("Export palette"), wxOK | wxICON_WARNING, this);
                return;
            }
            for (size_t i = 0; i < m_gd_ramp.size(); ++i) {
                if (!m_gd_selected[i]) continue;
                gd_export_one(
                    wxString::Format("%s %d/%d (A%.2f B%.2f)", base, (int)i + 1,
                                     (int)m_gd_ramp.size(), m_gd_ramp[i].a_mm, m_gd_ramp[i].b_mm),
                    m_gd_ramp[i].top, m_gd_ramp[i].penu, gd_step_colour(m_gd_ramp[i]));
                ++created;
            }
        } else {
            // -------- Flat/Mixed: exporta la paleta-predicción completa
            if (m_gd_recipes.empty()) {
                wxMessageBox(_L("Empty palette — adjust filaments/TD first."),
                             _L("Export palette"), wxOK | wxICON_WARNING, this);
                return;
            }
            const wxString kind = (gd_mode() == 1) ? "Flat" : "Mixed";
            for (size_t i = 0; i < m_gd_recipes.size(); ++i) {
                const auto& r = m_gd_recipes[i];
                gd_export_one(
                    wxString::Format("%s %s %d (%s)", base, kind, (int)i + 1,
                                     wxString::FromUTF8(r.desc)),
                    r.top, r.penu, gd_colour_at((int)i));
                ++created;
            }
        }
        wxMessageBox(wxString::Format(
                         _L("%d profiles created — available as a palette in "
                            "the 3D Painter (Sandwich)."), created),
                     _L("Export palette"), wxOK | wxICON_INFORMATION, this);
    }

    // Re-aplica visibilidad de sub-paneles + textos según el modo activo.
    void gd_apply_mode()
    {
        const int mode = gd_mode();
        if (m_gd_design_pnl)  m_gd_design_pnl ->Show(mode == 0);
        if (m_gd_predict_pnl) m_gd_predict_pnl->Show(mode != 0);
        if (m_gd_de) m_gd_de->SetLabel(wxEmptyString);
        Layout();
        gd_recalc();
    }

    wxSizer* build_gradient_panel()
    {
        const int PAD = 6;
        auto* sb = new wxStaticBoxSizer(wxVERTICAL, this,
            _L("ColorStitch Studio"));
        wxWindow* host = sb->GetStaticBox();

        // ---- selector de modo (siempre visible) -------------------------------
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(host, wxID_ANY, _L("Mode:")),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
            wxArrayString modes;
            modes.Add(_L("Gradient ramp"));
            modes.Add(_L("Flat color (predict)"));
            modes.Add(_L("Mixed approximation (predict)"));
            m_gd_mode = new wxChoice(host, wxID_ANY, wxDefaultPosition, wxDefaultSize, modes);
            m_gd_mode->SetSelection(0);
            m_gd_mode->SetToolTip(_L(
                "Gradient ramp — design a smooth A→penu→B ramp by hand.\n"
                "Flat color — predicted palette of colors reachable by stacking solids.\n"
                "Mixed approximation — predicted EXTENDED palette using a ColorStitch\n"
                "  dither base (e.g. pattern \"12\") + a translucent solid top. Reaches\n"
                "  colors no flat stack can (optical mix as a new primary)."));
            m_gd_mode->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { gd_apply_mode(); });
            row->Add(m_gd_mode, 1, wxALIGN_CENTER_VERTICAL);
            sb->Add(row, 0, wxEXPAND | wxALL, PAD);
        }

        // ============ sub-panel modo Gradient (controles manuales) =============
        m_gd_design_pnl = new wxPanel(host, wxID_ANY);
        {
            wxWindow* d = m_gd_design_pnl;
            auto* dz = new wxBoxSizer(wxVERTICAL);

            // fila: tools A/B con swatch + ligamento
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            wxArrayString tools;
            for (int i = 1; i <= 4; ++i) tools.Add(wxString::Format("T%d", i));
            m_gd_sw_a = new wxPanel(d, wxID_ANY, wxDefaultPosition, wxSize(14, 14));
            m_gd_sw_a->SetBackgroundColour(tool_colour(0));
            row->Add(m_gd_sw_a, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
            row->Add(new wxStaticText(d, wxID_ANY, _L("A")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
            m_gd_tool_a = new wxChoice(d, wxID_ANY, wxDefaultPosition, wxSize(60, -1), tools);
            m_gd_tool_a->SetSelection(0);
            m_gd_tool_a->SetToolTip(_L("Top tool (visible side) — also penu pattern tool A"));
            row->Add(m_gd_tool_a, 0, wxRIGHT, 10);
            m_gd_sw_b = new wxPanel(d, wxID_ANY, wxDefaultPosition, wxSize(14, 14));
            m_gd_sw_b->SetBackgroundColour(tool_colour(1));
            row->Add(m_gd_sw_b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
            row->Add(new wxStaticText(d, wxID_ANY, _L("B")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
            m_gd_tool_b = new wxChoice(d, wxID_ANY, wxDefaultPosition, wxSize(60, -1), tools);
            m_gd_tool_b->SetSelection(1);
            m_gd_tool_b->SetToolTip(_L("Contrast tool (lower top pass)"));
            row->Add(m_gd_tool_b, 0, wxRIGHT, 10);
            // s120: Weave/Pattern retirados — el gradiente es TOP-only (sin penu).
            // El dither de penu lo añade el usuario aparte si lo quiere.
            row->AddStretchSpacer(1);
            dz->Add(row, 0, wxEXPAND | wxBOTTOM, PAD);

            // fila: steps + split  (s120: Pattern retirado — gradiente top-only)
            auto* row2 = new wxBoxSizer(wxHORIZONTAL);
            row2->Add(new wxStaticText(d, wxID_ANY, _L("Steps:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_gd_steps = new wxSpinCtrl(d, wxID_ANY, "8", wxDefaultPosition, wxSize(56, -1),
                                        wxSP_ARROW_KEYS, 1, 30, 8);
            row2->Add(m_gd_steps, 0, wxRIGHT, 10);
            const double lo = Slic3r::ColorSci::kMinSweepMM;
            const double hi = std::max(lo, layer_height_mm() - lo);
            row2->Add(new wxStaticText(d, wxID_ANY, _L("Split:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_gd_min = new wxSpinCtrlDouble(d, wxID_ANY, "0.04", wxDefaultPosition, wxSize(72, -1),
                                            wxSP_ARROW_KEYS, lo, hi, 0.04, 0.01);
            m_gd_min->SetToolTip(_L("Thinnest top pass (mm). Floor 0.04 = engine MinLayer."));
            row2->Add(m_gd_min, 0, wxRIGHT, 2);
            row2->Add(new wxStaticText(d, wxID_ANY, "–"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 2);
            m_gd_max = new wxSpinCtrlDouble(d, wxID_ANY, "0.16", wxDefaultPosition, wxSize(72, -1),
                                            wxSP_ARROW_KEYS, lo, hi, 0.16, 0.01);
            m_gd_max->SetToolTip(_L("Thickest top pass (mm)"));
            row2->Add(m_gd_max, 0, wxRIGHT, 4);
            row2->Add(new wxStaticText(d, wxID_ANY,
                          wxString::Format("mm (LH %.2f)", layer_height_mm())),
                      0, wxALIGN_CENTER_VERTICAL);
            dz->Add(row2, 0, wxEXPAND);
            d->SetSizer(dz);
            sb->Add(m_gd_design_pnl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);
        }

        // ============ sub-panel modos Flat/Mixed (target match) ================
        m_gd_predict_pnl = new wxPanel(host, wxID_ANY);
        {
            wxWindow* pp = m_gd_predict_pnl;
            auto* pz = new wxBoxSizer(wxVERTICAL);
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(pp, wxID_ANY, _L("Target:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            wxArrayString tlabels;
            for (auto& opt : m_bs_mix_opts) tlabels.Add(wxString::FromUTF8(opt.label));
            if (tlabels.IsEmpty()) tlabels.Add(_L("(no MixedColor defined)"));
            m_gd_ptarget = new wxChoice(pp, wxID_ANY, wxDefaultPosition, wxDefaultSize, tlabels);
            m_gd_ptarget->SetSelection(0);
            m_gd_ptarget->SetToolTip(_L("Target colour (same pool as Blend Suggestion). "
                                        "Define MixedColors in the filament panel."));
            row->Add(m_gd_ptarget, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            auto* btn_match = new wxButton(pp, wxID_ANY, _L("Match ▸"));
            btn_match->SetToolTip(_L("Find the closest achievable recipe (ΔE2000) and load it "
                                     "into the sandwich above."));
            btn_match->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { gd_on_match(); });
            row->Add(btn_match, 0, wxRIGHT, 6);
            m_gd_de = new wxStaticText(pp, wxID_ANY, wxEmptyString,
                                       wxDefaultPosition, wxSize(90, -1));
            m_gd_de->SetForegroundColour(wxColour(90, 90, 90));
            row->Add(m_gd_de, 0, wxALIGN_CENTER_VERTICAL);
            pz->Add(row, 0, wxEXPAND);
            pp->SetSizer(pz);
            sb->Add(m_gd_predict_pnl, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);
            m_gd_predict_pnl->Hide();   // arranca en modo Gradient
        }

        // ============ tira de swatches (compartida, mode-aware) ================
        {
            m_gd_strip = new wxPanel(host, wxID_ANY, wxDefaultPosition, wxSize(-1, 64));
            m_gd_strip->SetMinSize(wxSize(-1, 56));
            m_gd_strip->SetBackgroundStyle(wxBG_STYLE_PAINT);
            m_gd_strip->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
                wxPaintDC dc(m_gd_strip);
                dc.SetBackground(wxBrush(m_gd_strip->GetParent()->GetBackgroundColour()));
                dc.Clear();
                const bool predict = gd_is_predict();
                for (int i = 0; i < gd_count(); ++i) {
                    const wxRect r = gd_swatch_rect(i);
                    dc.SetBrush(wxBrush(gd_colour_at(i)));
                    // Gradient: borde azul = incluido en export; hatch = excluido.
                    // Predict: borde verde = receta resaltada por Match.
                    const bool sel = predict ? (i == m_gd_match_idx)
                                             : (i < (int)m_gd_selected.size() && m_gd_selected[i]);
                    if (predict)
                        dc.SetPen(sel ? wxPen(wxColour(0, 180, 90), 2)
                                      : wxPen(wxColour(110, 110, 110), 1));
                    else
                        dc.SetPen(sel ? wxPen(wxColour(0, 120, 255), 2)
                                      : wxPen(wxColour(110, 110, 110), 1));
                    dc.DrawRectangle(r);
                    if (!predict && !sel) {
                        dc.SetBrush(wxBrush(wxColour(110, 110, 110), wxBRUSHSTYLE_BDIAGONAL_HATCH));
                        dc.SetPen(*wxTRANSPARENT_PEN);
                        dc.DrawRectangle(r);
                    }
                }
            });
            m_gd_strip->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
                const int i = gd_swatch_at(e.GetPosition());
                if (i < 0) return;
                if (gd_is_predict()) {
                    // CLICK CARGA la receta en el sandwich vivo (cambia las capas).
                    if (i < (int)m_gd_recipes.size()) {
                        m_gd_match_idx = i;
                        gd_load_recipe(m_gd_recipes[i]);
                        if (m_gd_de) m_gd_de->SetLabel(wxEmptyString);
                        m_gd_strip->Refresh();
                    }
                } else if (i < (int)m_gd_selected.size()) {
                    m_gd_selected[i] = !m_gd_selected[i];
                    m_gd_strip->Refresh();
                }
            });
            m_gd_strip->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
                const int i = gd_swatch_at(e.GetPosition());
                if (i < 0) { m_gd_strip->UnsetToolTip(); e.Skip(); return; }
                if (gd_is_predict()) {
                    if (i < (int)m_gd_recipes.size())
                        m_gd_strip->SetToolTip(wxString::FromUTF8(m_gd_recipes[i].desc));
                } else if (i < (int)m_gd_ramp.size()) {
                    m_gd_strip->SetToolTip(wxString::Format(
                        "A %.2f / B %.2f mm", m_gd_ramp[i].a_mm, m_gd_ramp[i].b_mm));
                }
                e.Skip();
            });
            m_gd_strip->Bind(wxEVT_SIZE, [this](wxSizeEvent& e) { m_gd_strip->Refresh(); e.Skip(); });
            sb->Add(m_gd_strip, 1, wxEXPAND | wxLEFT | wxRIGHT, PAD);

            auto* hint = new wxStaticText(host, wxID_ANY,
                _L("Gradient: click toggles export · Predict: click loads the recipe into the sandwich"));
            hint->SetForegroundColour(wxColour(100, 100, 100));
            sb->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);
        }

        // warnings de sanitize (oculto si no hay)
        m_gd_warn = new wxStaticText(host, wxID_ANY, wxEmptyString);
        m_gd_warn->SetForegroundColour(wxColour(200, 130, 0));
        m_gd_warn->Hide();
        sb->Add(m_gd_warn, 0, wxLEFT | wxRIGHT | wxBOTTOM, PAD);

        // fila final: nombre + export (compartida)
        {
            auto* row = new wxBoxSizer(wxHORIZONTAL);
            row->Add(new wxStaticText(host, wxID_ANY, _L("Name:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
            m_gd_name = new wxTextCtrl(host, wxID_ANY, _L("Palette"));
            row->Add(m_gd_name, 1, wxRIGHT, 8);
            auto* btn_export = new wxButton(host, wxID_ANY, _L("Export palette…"));
            btn_export->SetToolTip(_L("Gradient: exports selected ramp swatches.\n"
                                      "Predict: exports the whole predicted palette.\n"
                                      "Each becomes a 3D Painter profile."));
            btn_export->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { gd_on_export(); });
            row->Add(btn_export, 0);
            sb->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, PAD);
        }

        // reactividad modo Gradient
        m_gd_tool_a->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { gd_recalc(); });
        m_gd_tool_b->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { gd_recalc(); });
        m_gd_steps->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&)   { gd_recalc(); });
        m_gd_min->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { gd_recalc(); });
        m_gd_max->Bind(wxEVT_SPINCTRLDOUBLE, [this](wxSpinDoubleEvent&) { gd_recalc(); });

        // Timer de debounce TD → regenerar. Owner = este diálogo, id propio.
        m_gd_td_timer.SetOwner(this, kGdTdTimerId);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { gd_recalc(); }, kGdTdTimerId);

        gd_recalc();
        return sb;
    }

    void build_ui()
    {
        // NEOTKO_SANDWICH_TAG — Fase 7 (s84): layout horizontal. Izquierda
        // editor de pilas + profile manager; derecha panel TD + Lane mode +
        // use_virtual (mockup s71, portado del SurfaceColorMixerDialog).
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* hbox = new wxBoxSizer(wxHORIZONTAL);

        auto* left  = new wxBoxSizer(wxVERTICAL);
        auto* right = new wxBoxSizer(wxVERTICAL);

        left->Add(build_zone(0, _L("Top layer")),
                  0, wxEXPAND | wxALL, 6);
        left->Add(build_zone(1, _L("Penultimate layer")),
                  0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        // NEOTKO_COLORSCI_TAG GD1 — UX 2026-06-11: Line distribution baja
        // bajo los dos sandwich; su hueco de la derecha lo ocupa el Gradient
        // Designer (que absorbe también el stretch → la tira crece).
        left->Add(build_lane_panel(),
                  0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);
        left->AddStretchSpacer(1);

        right->Add(build_td_panel(),       0, wxEXPAND | wxALL, 6);
        right->Add(build_gradient_panel(), 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

        hbox->Add(left,  1, wxEXPAND);
        hbox->Add(right, 0, wxEXPAND | wxLEFT, 4);
        root->Add(hbox, 1, wxEXPAND);

        // NEOTKO_PROFILE_TAG — Fase 6: profile manager (3D Painter). Saves the
        // authoritative pass-stack blobs (stack_top/penu_json) into a
        // SurfaceEffectProfile so the painter can pick this sandwich. The legacy
        // 3-payloads stay empty (present=false) — the painter ENGINE migration to
        // consume the stack is Fase 6b; until then the gizmo previews it.
        root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
        auto* prof_row = new wxBoxSizer(wxHORIZONTAL);
        auto* btn_save_prof = new wxButton(this, wxID_ANY, _L("Save as profile…"));
        auto* btn_mgr_prof  = new wxButton(this, wxID_ANY, _L("Manage profiles…"));
        prof_row->Add(btn_save_prof, 0, wxRIGHT, 6);
        prof_row->Add(btn_mgr_prof,  0);
        root->Add(prof_row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);
        btn_save_prof->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_save_profile(); });
        btn_mgr_prof->Bind(wxEVT_BUTTON,  [this](wxCommandEvent&) { on_manage_profiles(); });

        root->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT, 8);
        auto* btns = CreateButtonSizer(wxOK | wxCANCEL);
        if (btns) root->Add(btns, 0, wxEXPAND | wxALL, 8);

        Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
            if (e.GetId() == wxID_OK) commit();
            e.Skip();
        });

        SetSizerAndFit(root);
        SetMinSize(wxSize(560, 420));
        for (int z = 0; z < 2; ++z) { refresh_rows(z); sync_zone_enabled(z); }
        Layout();
    }

    wxSizer* build_zone(int z, const wxString& title)
    {
        auto* box = new wxStaticBoxSizer(wxVERTICAL, this, title);
        wxWindow* boxw = box->GetStaticBox();

        // header — Enabled checkbox + 1/2/3 slot selector
        auto* hdr = new wxBoxSizer(wxHORIZONTAL);
        m_ui[z].enable_chk = new wxCheckBox(boxw, wxID_ANY, _L("Enabled"));
        m_ui[z].enable_chk->SetValue(m_stack[z].enabled && !m_stack[z].passes.empty());
        m_ui[z].enable_chk->Bind(wxEVT_CHECKBOX, [this, z](wxCommandEvent&) {
            const bool on = m_ui[z].enable_chk->GetValue();
            m_stack[z].enabled = on;
            if (on) {
                // NEOTKO_SANDWICH_TAG s119 (EMPTY model): enabling a zone means "I
                // want an effect here". An empty stack or a lone None (Empty) zone
                // becomes a real ColorMix pass — the per-row selector no longer
                // offers None, so a None row would otherwise show no kind.
                if (m_stack[z].passes.empty()) {
                    Slic3r::SurfacePass p;
                    p.kind = Kind::ColorMix; p.ratio = 1.0;
                    m_stack[z].passes.push_back(p);
                } else if (m_stack[z].passes.size() == 1 &&
                           m_stack[z].passes[0].kind == Kind::None) {
                    m_stack[z].passes[0].kind  = Kind::ColorMix;
                    m_stack[z].passes[0].ratio = 1.0;
                }
            }
            refresh_rows(z);
            sync_zone_enabled(z);
        });
        hdr->Add(m_ui[z].enable_chk, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);

        wxArrayString slots; slots.Add("1"); slots.Add("2"); slots.Add("3");
        m_ui[z].slots_rb = new wxRadioBox(boxw, wxID_ANY, _L("Passes"),
                                          wxDefaultPosition, wxDefaultSize,
                                          slots, 3, wxRA_SPECIFY_COLS);
        const int n0 = std::max(1, (int)m_stack[z].passes.size());
        m_ui[z].slots_rb->SetSelection(std::min(3, n0) - 1);
        m_ui[z].slots_rb->Bind(wxEVT_RADIOBOX, [this, z](wxCommandEvent&) {
            set_slot_count(z, m_ui[z].slots_rb->GetSelection() + 1);
        });
        hdr->Add(m_ui[z].slots_rb, 0, wxALIGN_CENTER_VERTICAL);

        // NEOTKO_COLORSTITCH_TAG — s118: Perimeter override es UNA sola opción del
        // color (no per-zona). Se crea sólo en la zona Top y aplica a top+penu; la
        // zona Penu no lo dibuja (m_ui[1].perim_chk queda null → refs guardadas).
        // El motor lo lee por-zona (Fill.cpp), así que escribimos ambos stacks.
        if (z == 0) {
            m_ui[z].perim_chk = new wxCheckBox(boxw, wxID_ANY, _L("Perimeter override"));
            m_ui[z].perim_chk->SetValue(m_stack[0].perimeter_override || m_stack[1].perimeter_override);
            m_ui[z].perim_chk->SetToolTip(_L("Clone the walls into every Solid pass "
                                             "(MultiPass perimeter override). Applies to "
                                             "Top and Penultimate."));
            m_ui[z].perim_chk->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
                const bool on = m_ui[0].perim_chk->GetValue();
                m_stack[0].perimeter_override = on;
                m_stack[1].perimeter_override = on;
            });
            hdr->Add(m_ui[z].perim_chk, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 16);
        }
        box->Add(hdr, 0, wxALL, 6);

        // rows host — kMaxPasses fixed row panels, shown/hidden by slot count.
        m_ui[z].rows_host  = new wxPanel(boxw);
        m_ui[z].rows_sizer = new wxBoxSizer(wxVERTICAL);
        m_ui[z].rows_host->SetSizer(m_ui[z].rows_sizer);
        const int kMax = Slic3r::SurfacePassStack::kMaxPasses;
        m_ui[z].row_panel  .assign(kMax, nullptr);
        m_ui[z].chips      .assign(kMax, nullptr);
        m_ui[z].badge      .assign(kMax, nullptr);
        m_ui[z].kind_choice.assign(kMax, nullptr);
        m_ui[z].ratio_spin .assign(kMax, nullptr);
        m_ui[z].angle_lbl  .assign(kMax, nullptr);
        m_ui[z].angle_txt  .assign(kMax, nullptr);
        m_ui[z].preview    .assign(kMax, nullptr);
        m_ui[z].adv_btn    .assign(kMax, nullptr);
        m_ui[z].pb_advanced_btn.assign(kMax, nullptr);
        m_ui[z].minlbl     .assign(kMax, nullptr);
        m_ui[z].up_btn     .assign(kMax, nullptr);
        m_ui[z].down_btn   .assign(kMax, nullptr);
        for (int idx = kMax - 1; idx >= 0; --idx)   // top of stack drawn first
            m_ui[z].rows_sizer->Add(build_row(z, idx), 0,
                                    wxEXPAND | wxTOP | wxBOTTOM, 3);

        // stacked draggable Z-ratio bar, left of the rows (drag the dividers).
        m_ui[z].ratio_bar = new wxPanel(boxw, wxID_ANY,
                                        wxDefaultPosition, wxSize(30, -1));
        m_ui[z].ratio_bar->SetToolTip(
            _L("Drag the dividers to split the layer height between passes."));
        m_ui[z].ratio_bar->Bind(wxEVT_PAINT,
            [this, z](wxPaintEvent&) { paint_ratio_bar(z); });
        m_ui[z].ratio_bar->Bind(wxEVT_LEFT_DOWN,
            [this, z](wxMouseEvent& e) { ratio_bar_down(z, e); });
        m_ui[z].ratio_bar->Bind(wxEVT_MOTION,
            [this, z](wxMouseEvent& e) { ratio_bar_motion(z, e); });
        m_ui[z].ratio_bar->Bind(wxEVT_LEFT_UP,
            [this, z](wxMouseEvent& e) { ratio_bar_up(z, e); });
        m_ui[z].ratio_bar->Bind(wxEVT_MOUSE_CAPTURE_LOST,
            [this](wxMouseCaptureLostEvent&) { m_drag_zone = -1; m_drag_bound = -1; });

        auto* mid = new wxBoxSizer(wxHORIZONTAL);
        mid->Add(m_ui[z].ratio_bar, 0, wxEXPAND | wxRIGHT, 5);
        mid->Add(m_ui[z].rows_host, 1, wxEXPAND);
        box->Add(mid, 0, wxEXPAND | wxLEFT | wxRIGHT, 6);

        // footer — ratio sum + normalize
        auto* foot = new wxBoxSizer(wxHORIZONTAL);
        m_ui[z].sum_lbl = new wxStaticText(boxw, wxID_ANY, wxEmptyString);
        foot->Add(m_ui[z].sum_lbl, 1, wxALIGN_CENTER_VERTICAL);
        m_ui[z].norm_btn = new wxButton(boxw, wxID_ANY, _L("Normalize"),
                                        wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        m_ui[z].norm_btn->Bind(wxEVT_BUTTON, [this, z](wxCommandEvent&) { normalize(z); });
        foot->Add(m_ui[z].norm_btn, 0, wxLEFT, 8);
        box->Add(foot, 0, wxEXPAND | wxALL, 6);
        return box;
    }

    // --------------------------------------------------------------- row build
    // The 3 row panels are built ONCE and never destroyed — only shown/hidden
    // and re-synced. Destroying child windows from inside an event handler
    // (the old rebuild approach) freed NSViews that AppKit still drew → crash.
    void refresh_rows(int z)
    {
        ZoneUI& u = m_ui[z];
        const int n = (int)m_stack[z].passes.size();
        u.kindlist = kind_entries_for_slots(n);

        for (int idx = 0; idx < (int)u.row_panel.size(); ++idx) {
            const bool vis = idx < n;
            if (u.row_panel[idx]) u.row_panel[idx]->Show(vis);
            if (!vis) continue;
            const Slic3r::SurfacePass& p = m_stack[z].passes[idx];

            // repopulate the kind choice for the current slot rule
            u.kind_choice[idx]->Clear();
            for (const auto& e : u.kindlist)
                u.kind_choice[idx]->Append(e.label);
            // Select the entry that matches kind + (for PB) mode in the blob.
            int sel = 0;
            int pb_mode_cur = (int)Slic3r::PathBlendPassConfig::Mode::Full;
            if (p.kind == Kind::PathBlend) {
                const auto it_blob = p.pathblend.kv.find("blob");
                const std::string blob = (it_blob != p.pathblend.kv.end())
                    ? it_blob->second : std::string();
                if (!blob.empty()) {
                    const Slic3r::PathBlendPassConfig pbc =
                        Slic3r::PathBlendPassConfig::from_blob_json(blob);
                    pb_mode_cur = (int)pbc.mode;
                }
            }
            for (size_t i = 0; i < u.kindlist.size(); ++i) {
                if (u.kindlist[i].kind != p.kind) continue;
                if (p.kind == Kind::PathBlend) {
                    if (u.kindlist[i].pb_mode == pb_mode_cur) { sel = (int)i; break; }
                } else {
                    sel = (int)i; break;
                }
            }
            u.kind_choice[idx]->SetSelection(sel);

            // NEOTKO_SANDWICH_TAG — Fase 5 s73: ratio_spin shows floor_mm for PB.
            if (p.kind == Kind::PathBlend) {
                Slic3r::PathBlendPassConfig pbc;
                const auto it_blob = p.pathblend.kv.find("blob");
                if (it_blob != p.pathblend.kv.end() && !it_blob->second.empty())
                    pbc = Slic3r::PathBlendPassConfig::from_blob_json(it_blob->second);
                u.ratio_spin[idx]->SetValue(pbc.floor_mm);
            } else {
                u.ratio_spin[idx]->SetValue(p.ratio * layer_height_mm());
            }
            sync_row_widgets(z, idx);
        }
        u.rows_host->Enable(m_stack[z].enabled);
        u.rows_host->Layout();
        update_sum(z);
        if (GetSizer()) { GetSizer()->Layout(); Fit(); }
    }

    // Per-row sync that does NOT touch the kind choice items, so it is safe to
    // call from inside the choice's own event handler. Updates badge, the
    // angle field (Solid only) and the advanced button (ColorMix / PathBlend).
    void sync_row_widgets(int z, int idx)
    {
        ZoneUI& u = m_ui[z];
        if (idx >= (int)m_stack[z].passes.size()) return;
        const Slic3r::SurfacePass& p = m_stack[z].passes[idx];

        u.badge[idx]->SetLabel(kind_badge_for(p));   // NEOTKO_SANDWICH_TAG s73
        u.badge[idx]->SetBackgroundColour(kind_colour(p.kind));
        u.badge[idx]->Refresh();

        // NEOTKO_SANDWICH_TAG — Fase 7 (s84c): up/down visibility + enable.
        // Hidden when slots < 2 (or PB collapses the stack to 1).
        {
            const int n = (int)m_stack[z].passes.size();
            const bool visible = n >= 2;
            if (u.up_btn[idx]) {
                u.up_btn[idx]->Show(visible);
                u.up_btn[idx]->Enable(visible && idx < n - 1);
            }
            if (u.down_btn[idx]) {
                u.down_btn[idx]->Show(visible);
                u.down_btn[idx]->Enable(visible && idx > 0);
            }
        }

        // NEOTKO_SANDWICH_TAG — Fase 5 s73: per-row inline PathBlend repurpose.
        // Solid -> spinner = Z mm (pass height), angle field = fill angle.
        // PathBlend -> spinner = floor_mm, angle field = mid_end_mm (label
        // changes from "angle:" to "mid:"). Ease + tools picked via adv_btn
        // and chip clicks. The kind selector decides Half vs Full.
        const bool is_solid = (p.kind == Kind::Solid);
        const bool is_pb    = (p.kind == Kind::PathBlend);

        // Angle/Mid field is visible for Solid and PathBlend.
        u.angle_lbl[idx]->Show(is_solid || is_pb);
        u.angle_txt[idx]->Show(is_solid || is_pb);
        // Default to enabled; the PB-Half branch below disables when needed.
        u.angle_lbl[idx]->Enable(true);
        u.angle_txt[idx]->Enable(true);
        if (is_solid) {
            u.angle_lbl[idx]->SetLabel(_L("angle:"));
            u.angle_txt[idx]->ChangeValue(wxString::Format("%d", p.angle));
        } else if (is_pb) {
            u.angle_lbl[idx]->SetLabel(_L("ramp end:"));
            Slic3r::PathBlendPassConfig pbc;
            const auto it_blob = p.pathblend.kv.find("blob");
            if (it_blob != p.pathblend.kv.end() && !it_blob->second.empty())
                pbc = Slic3r::PathBlendPassConfig::from_blob_json(it_blob->second);
            u.angle_txt[idx]->ChangeValue(wxString::Format("%.3f", pbc.mid_end_mm));
            // NEOTKO_SANDWICH_TAG — Fase 5 s73: in Half mode mid is forced to
            // the layer height (no cap), so the field is disabled to make the
            // constraint visible to the user.
            const bool half = (pbc.mode == Slic3r::PathBlendPassConfig::Mode::Half);
            u.angle_txt[idx]->Enable(!half);
            u.angle_lbl[idx]->Enable(!half);
        }

        // adv_btn: "Edit gradient..." for ColorMix; ease-mode cycler for PB;
        // hidden for Solid/None.
        wxButton* adv = u.adv_btn[idx];
        if (p.kind == Kind::ColorMix) {
            adv->SetLabel(_L("Edit gradient…")); adv->Show(true);
        } else if (is_pb) {
            static const wxString ease_names[4] = {
                _L("Mode: Linear"), _L("Mode: Ease In"),
                _L("Mode: Ease Out"), _L("Mode: Ease In/Out")
            };
            Slic3r::PathBlendPassConfig pbc;
            const auto it_blob = p.pathblend.kv.find("blob");
            if (it_blob != p.pathblend.kv.end() && !it_blob->second.empty())
                pbc = Slic3r::PathBlendPassConfig::from_blob_json(it_blob->second);
            const int em = std::clamp(pbc.ease_mode, 0, 3);
            adv->SetLabel(ease_names[em]);
            adv->Show(true);
        } else {
            adv->Show(false);
        }
        // NEOTKO_PATHBLEND_TAG — s88. Advanced ⚙ button is PB-only.
        if (u.pb_advanced_btn[idx]) {
            u.pb_advanced_btn[idx]->Show(is_pb);
            if (is_pb) {
                // Reflect current runtime state on the label so the user sees
                // at a glance whether any toggle is non-default.
                const auto& d = Slic3r::PathBlendDispatcherRuntime::get();
                const auto& s = Slic3r::PathBlendSchedulerRuntime::get();
                const bool all_default = d.chain_continuous && s.chain_atomic
                    && s.use_canon_scheduler
                    && std::abs(d.chain_max_xy_mm - 1.0) < 1e-6;
                u.pb_advanced_btn[idx]->SetLabel(
                    all_default ? _L("Advanced \xE2\x9A\x99")
                                : _L("Advanced \xE2\x9A\x99 *"));
            }
        }

        // Ratio spinner label changes: "Z mm:" normally, "floor mm:" for PB.
        // The spinner itself is at index 0 of its sizer (we just retitle via the
        // spinner's value, the static label is at row sizer position).
        // The label widget isn't stored separately; we keep the static text "Z mm:"
        // in build_row and instead use the tooltip + spinner range to disambiguate.
        // For PB, the spinner value shows floor_mm directly (refresh_rows handles).

        // min-layer warning: only meaningful for Solid/ColorMix sub-bands. PB
        // doesn't go through the sublayer MinLayer gate (its floor can be 0.01).
        const double mm = p.ratio * layer_height_mm();
        const bool thin = (!is_pb) && (mm < kMinPassMM - 1e-9);
        if (u.minlbl[idx]) {
            u.minlbl[idx]->SetLabel(thin
                ? _L("⚠ < 0,04 mm — pasada apagada (su altura se reparte al guardar)")
                : wxString());
            u.minlbl[idx]->Show(thin);
        }
        u.row_panel[idx]->SetBackgroundColour(
            thin ? wxColour(48, 40, 32) : wxColour(60, 60, 60));

        u.chips[idx]->Refresh();
        u.preview[idx]->Refresh();
        u.row_panel[idx]->Refresh();
        u.row_panel[idx]->Layout();
    }

    // Builds one fixed row panel (widgets only — values synced by refresh_rows).
    wxPanel* build_row(int z, int idx)
    {
        ZoneUI& u = m_ui[z];
        auto* row = new wxPanel(u.rows_host);
        row->SetBackgroundColour(wxColour(60, 60, 60));
        auto* rv = new wxBoxSizer(wxVERTICAL);
        auto* rh = new wxBoxSizer(wxHORIZONTAL);

        // NEOTKO_SANDWICH_TAG — Fase 7 (s84c): up/down reorder buttons.
        // Vector m_stack[z].passes is bottom→top, rows are drawn top-first
        // (kMax-1 .. 0). Up arrow (visual top direction) = swap with idx+1.
        // Down arrow = swap with idx-1. Shown only when slots >= 2; disabled
        // at the extreme positions. State managed in sync_row_widgets.
        auto* up = new wxButton(row, wxID_ANY, L"▲",
            wxDefaultPosition, wxSize(22, 22), wxBU_EXACTFIT);
        up->SetToolTip(_L("Move this pass up (toward the top of the layer)."));
        up->Bind(wxEVT_BUTTON, [this, z, idx](wxCommandEvent&) {
            move_pass(z, idx, idx + 1);
        });
        rh->Add(up, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 2);

        auto* down = new wxButton(row, wxID_ANY, L"▼",
            wxDefaultPosition, wxSize(22, 22), wxBU_EXACTFIT);
        down->SetToolTip(_L("Move this pass down (toward the bottom of the layer)."));
        down->Bind(wxEVT_BUTTON, [this, z, idx](wxCommandEvent&) {
            move_pass(z, idx, idx - 1);
        });
        rh->Add(down, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

        auto* chip = new wxPanel(row, wxID_ANY, wxDefaultPosition, wxSize(78, 22));
        chip->Bind(wxEVT_PAINT, [this, z, idx](wxPaintEvent&) { paint_chip(z, idx); });
        chip->Bind(wxEVT_LEFT_DOWN, [this, z, idx](wxMouseEvent& ev) {
            if (idx >= (int)m_stack[z].passes.size()) return;
            const Kind k = m_stack[z].passes[idx].kind;
            if (k == Kind::Solid) {
                pick_solid_tool(z, idx);
            } else if (k == Kind::PathBlend) {
                // NEOTKO_SANDWICH_TAG — Fase 5 s73: chip is painted in 2 halves
                // for PB Full. Click in upper half -> cap tool; lower half ->
                // ramp tool. Half mode: any click -> ramp (only one tool).
                wxPanel* w = m_ui[z].chips[idx];
                const int slot = (w && ev.GetY() < w->GetClientSize().y / 2) ? 1 : 0;
                pick_pathblend_tool(z, idx, slot);
            }
        });
        rh->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);

        auto* badge = new wxStaticText(row, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxSize(82, 20), wxALIGN_CENTRE_HORIZONTAL);
        badge->SetForegroundColour(*wxWHITE);
        rh->Add(badge, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

        auto* choice = new wxChoice(row, wxID_ANY);
        choice->Bind(wxEVT_CHOICE, [this, z, idx](wxCommandEvent&) {
            const int sel = m_ui[z].kind_choice[idx]->GetSelection();
            if (sel >= 0 && sel < (int)m_ui[z].kindlist.size())
                on_kind_change(z, idx, m_ui[z].kindlist[sel]);  // KindEntry
        });
        rh->Add(choice, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);

        // pass height in mm (= ratio × layer_height). Editing one rescales the
        // others so the stack always fills the layer.
        rh->Add(new wxStaticText(row, wxID_ANY, _L("Z mm:")),
                0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
        auto* ratio = new wxSpinCtrlDouble(row, wxID_ANY, wxEmptyString,
            wxDefaultPosition, wxSize(78, -1), wxSP_ARROW_KEYS,
            0.0, 5.0, 0.1, 0.02);
        ratio->SetDigits(2);
        ratio->Bind(wxEVT_SPINCTRLDOUBLE,
            [this, z, idx](wxSpinDoubleEvent&) { on_height_edit(z, idx); });
        rh->Add(ratio, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

        // fill angle — per-pass, Solid only. Editable + mouse-wheel rotation.
        auto* albl = new wxStaticText(row, wxID_ANY, _L("angle:"));
        rh->Add(albl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
        auto* atxt = new wxTextCtrl(row, wxID_ANY, "-1",
            wxDefaultPosition, wxSize(48, -1), wxTE_PROCESS_ENTER);
        atxt->SetToolTip(_L("-1 = auto. Scroll the wheel over the box below to rotate."));
        atxt->Bind(wxEVT_MOUSEWHEEL,
            [this, z, idx](wxMouseEvent& e) { on_angle_wheel(z, idx, e); });
        atxt->Bind(wxEVT_TEXT_ENTER,
            [this, z, idx](wxCommandEvent&) { store_angle(z, idx); });
        atxt->Bind(wxEVT_KILL_FOCUS,
            [this, z, idx](wxFocusEvent& e) { store_angle(z, idx); e.Skip(); });
        rh->Add(atxt, 0, wxALIGN_CENTER_VERTICAL);
        rv->Add(rh, 0, wxEXPAND | wxALL, 3);

        // preview box — Solid shows the fill-angle hatch (wheel rotates it here).
        auto* prev = new wxPanel(row, wxID_ANY, wxDefaultPosition, wxSize(-1, 20));
        prev->Bind(wxEVT_PAINT, [this, z, idx](wxPaintEvent&) { paint_preview(z, idx); });
        prev->Bind(wxEVT_MOUSEWHEEL,
            [this, z, idx](wxMouseEvent& e) { on_angle_wheel(z, idx, e); });
        rv->Add(prev, 0, wxEXPAND | wxLEFT | wxRIGHT, 3);

        // adv_btn: ColorMix -> open gradient dialog. PathBlend -> cycle ease_mode.
        // (Fase 5 s73: PB popup deleted; ease cycles inline on this button.)
        auto* adv = new wxButton(row, wxID_ANY, _L("Edit…"),
                                 wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        adv->Bind(wxEVT_BUTTON, [this, z, idx](wxCommandEvent&) {
            if (idx >= (int)m_stack[z].passes.size()) return;
            const Kind k = m_stack[z].passes[idx].kind;
            if (k == Kind::ColorMix) {
                open_colormix_for(z, idx);
            } else if (k == Kind::PathBlend) {
                cycle_pathblend_ease(z, idx);
            }
        });

        // NEOTKO_PATHBLEND_TAG — s88. Advanced ⚙ button placed RIGHT NEXT TO
        // the adv_btn ("Mode: Linear / Ease In / …"). Shown only on PB rows
        // (sync_row_widgets toggles visibility). Opens the PB Advanced
        // modal with the runtime toggles (chain_continuous, chain_max_xy_mm,
        // chain_atomic). The modal reads/writes the singletons in
        // SurfaceColorMix.hpp (PathBlend ingredient section) and persists
        // to app_config so choices survive across sessions.
        auto* pb_adv = new wxButton(row, wxID_ANY,
            _L("Advanced \xE2\x9A\x99"),
            wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        pb_adv->SetToolTip(_L(
            "PathBlend advanced behavior:\n"
            "  • Continuous chain — suppress retract+lift between adjacent\n"
            "    scanlines of the same tool.\n"
            "  • Atomic chain — complete one object's PathBlend before\n"
            "    starting the next (no cross-object travels during ramp).\n"
            "  • XY threshold — beyond this distance, treat scanlines as\n"
            "    disconnected islands and bring the lift back."));
        pb_adv->Bind(wxEVT_BUTTON, [this, pb_adv](wxCommandEvent&) {
            // Seed runtimes from app_config on open (defensive — main seed
            // happens at SandwichDialog ctor).
            auto* ac = wxGetApp().app_config;
            Slic3r::PathBlendDispatcherRuntime& d = Slic3r::PathBlendDispatcherRuntime::mut();
            Slic3r::PathBlendSchedulerRuntime&  s = Slic3r::PathBlendSchedulerRuntime::mut();
            if (ac) {
                const std::string vc = ac->get("neotko_pb_chain_continuous");
                const std::string vx = ac->get("neotko_pb_chain_max_xy_mm");
                const std::string va = ac->get("neotko_pb_chain_atomic");
                const std::string vk = ac->get("neotko_pb_use_canon_scheduler");
                if (!vc.empty()) d.chain_continuous    = (vc == "1" || vc == "true");
                if (!va.empty()) s.chain_atomic        = (va == "1" || va == "true");
                if (!vk.empty()) s.use_canon_scheduler = (vk == "1" || vk == "true");
                if (!vx.empty()) { try { d.chain_max_xy_mm = std::stod(vx); } catch (...) {} }
            }
            wxDialog dlg(pb_adv, wxID_ANY, _L("PathBlend Advanced"),
                         wxDefaultPosition, wxDefaultSize,
                         wxDEFAULT_DIALOG_STYLE);
            auto* vbox = new wxBoxSizer(wxVERTICAL);
            auto* chk_cont = new wxCheckBox(&dlg, wxID_ANY,
                _L("Continuous chain (suppress retract/lift between adjacent scanlines)"));
            chk_cont->SetValue(d.chain_continuous);
            chk_cont->SetToolTip(_L(
                "When ON, consecutive same-tool PathBlend sublayers within the\n"
                "XY threshold are emitted as one continuous extrusion (no\n"
                "retract, no wipe, no z-hop). Beyond the threshold the normal\n"
                "lift cycle returns — useful for disconnected islands."));
            vbox->Add(chk_cont, 0, wxALL, 8);

            auto* chk_atomic = new wxCheckBox(&dlg, wxID_ANY,
                _L("Atomic chain (complete each object's PathBlend before next)"));
            chk_atomic->SetValue(s.chain_atomic);
            chk_atomic->SetToolTip(_L(
                "When ON, the multi-object scheduler drains every same-tool\n"
                "sublayer of one chain before moving on. Eliminates cross-\n"
                "object micro-travels between scanlines when several objects\n"
                "with PathBlend print at the same Z.\n\n"
                "NOTE: must stay in sync between the GCode dispatcher and the\n"
                "wipe-tower planner — never toggle mid-print."));
            vbox->Add(chk_atomic, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

            // NEOTKO_PATHBLEND_TAG — s89. Canon scheduler toggle.
            auto* chk_canon = new wxCheckBox(&dlg, wxID_ANY,
                _L("Canon scheduler (NeoTower uses dispatcher's algorithm)"));
            chk_canon->SetValue(s.use_canon_scheduler);
            chk_canon->SetToolTip(_L(
                "When ON, NeoTower's sublayer scheduler uses the SAME algorithm\n"
                "the GCode dispatcher uses (order_sublayers_by_tool), so the\n"
                "wipe-tower plan matches actual emission by construction.\n\n"
                "Required for multi-object atomic PathBlend with distinct tools\n"
                "(test04 4-cube case). Safe for ColorMix and MultiPass — the\n"
                "algorithm reduces to natural ascending-z order when items per\n"
                "plane is 1.\n\n"
                "Turn OFF only if a regression appears in test01 / test05 /\n"
                "ColorMix; report logs before doing so."));
            vbox->Add(chk_canon, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

            auto* hxy = new wxBoxSizer(wxHORIZONTAL);
            hxy->Add(new wxStaticText(&dlg, wxID_ANY,
                    _L("Continuous-chain XY threshold (mm):")),
                    0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            auto* sc_xy = new wxSpinCtrlDouble(&dlg, wxID_ANY,
                wxEmptyString, wxDefaultPosition, wxSize(90, -1),
                wxSP_ARROW_KEYS, 0.0, 50.0, d.chain_max_xy_mm, 0.1);
            sc_xy->SetDigits(2);
            sc_xy->SetToolTip(_L(
                "Two consecutive PB sublayers are part of the same chain\n"
                "when their XY distance is ≤ this value. Default 1.0 mm\n"
                "is fine for most rectilinear fills (≥ 2 × spacing)."));
            hxy->Add(sc_xy, 0, wxALIGN_CENTER_VERTICAL);
            vbox->Add(hxy, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8);

            vbox->Add(dlg.CreateButtonSizer(wxOK | wxCANCEL),
                      0, wxEXPAND | wxALL, 8);
            dlg.SetSizerAndFit(vbox);
            if (dlg.ShowModal() == wxID_OK) {
                d.chain_continuous    = chk_cont->GetValue();
                s.chain_atomic        = chk_atomic->GetValue();
                s.use_canon_scheduler = chk_canon->GetValue();
                d.chain_max_xy_mm     = sc_xy->GetValue();
                if (ac) {
                    ac->set("neotko_pb_chain_continuous",    d.chain_continuous    ? "1" : "0");
                    ac->set("neotko_pb_chain_atomic",        s.chain_atomic        ? "1" : "0");
                    ac->set("neotko_pb_use_canon_scheduler", s.use_canon_scheduler ? "1" : "0");
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.4f", d.chain_max_xy_mm);
                    ac->set("neotko_pb_chain_max_xy_mm",  buf);
                    ac->save();
                }
                // Refresh the button label to reflect new state.
                pb_adv->SetLabel(
                    (d.chain_continuous && s.chain_atomic && s.use_canon_scheduler
                        && std::abs(d.chain_max_xy_mm - 1.0) < 1e-6)
                    ? _L("Advanced \xE2\x9A\x99")
                    : _L("Advanced \xE2\x9A\x99 *"));
            }
        });

        // Place adv_btn and pb_adv side by side in a horizontal sub-sizer.
        auto* adv_row = new wxBoxSizer(wxHORIZONTAL);
        adv_row->Add(adv,    0, wxALIGN_CENTER_VERTICAL);
        adv_row->Add(pb_adv, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        rv->Add(adv_row, 0, wxLEFT | wxTOP | wxBOTTOM, 3);

        // min-layer warning — shown when this pass's height < 0.04 mm.
        auto* minl = new wxStaticText(row, wxID_ANY, wxEmptyString);
        minl->SetForegroundColour(wxColour(220, 150, 60));
        rv->Add(minl, 0, wxLEFT | wxBOTTOM, 3);
        minl->Hide();

        row->SetSizer(rv);

        u.row_panel[idx]   = row;
        u.chips[idx]       = chip;
        u.badge[idx]       = badge;
        u.kind_choice[idx] = choice;
        u.ratio_spin[idx]  = ratio;
        u.angle_lbl[idx]   = albl;
        u.angle_txt[idx]   = atxt;
        u.preview[idx]     = prev;
        u.adv_btn[idx]     = adv;
        u.pb_advanced_btn[idx] = pb_adv;
        u.minlbl[idx]      = minl;
        u.up_btn[idx]      = up;
        u.down_btn[idx]    = down;
        return row;
    }

    // NEOTKO_SANDWICH_TAG — Fase 7 (s84c): reorder helper. Passes carry all
    // their state (kind, colormix.kv, pathblend.kv, ratio, angle) so a swap
    // is non-destructive — no gradients/PB blobs are lost.
    void move_pass(int z, int i, int j)
    {
        auto& ps = m_stack[z].passes;
        if (i < 0 || j < 0 || i == j) return;
        if (i >= (int)ps.size() || j >= (int)ps.size()) return;
        // PathBlend collapses the zone to 1 pass — nothing to reorder there;
        // the buttons are hidden in that case, but guard anyway.
        if (ps.size() < 2) return;
        std::swap(ps[i], ps[j]);
        refresh_rows(z);
    }

    // ------------------------------------------------------------- interaction
    void set_slot_count(int z, int n)
    {
        n = std::clamp(n, 1, Slic3r::SurfacePassStack::kMaxPasses);
        auto& ps = m_stack[z].passes;

        bool has_pb = false;
        for (const auto& p : ps) if (p.kind == Kind::PathBlend) has_pb = true;
        if (has_pb && n > 1)                       // PathBlend can't be multi-pass
            for (auto& p : ps) p.kind = Kind::Solid;

        if ((int)ps.size() < n) {
            while ((int)ps.size() < n) {
                Slic3r::SurfacePass p;
                p.kind = Kind::Solid;
                ps.push_back(p);
            }
        } else if ((int)ps.size() > n) {
            ps.resize(n);
        }
        if (n == 1 && (ps[0].kind == Kind::Solid || ps[0].kind == Kind::None))
            ps[0].kind = Kind::ColorMix;            // lone Solid/None is meaningless

        const double even = 1.0 / n;
        for (auto& p : ps) p.ratio = even;

        if (m_ui[z].slots_rb) m_ui[z].slots_rb->SetSelection(n - 1);
        refresh_rows(z);
    }

    void on_kind_change(int z, int idx, const ZoneUI::KindEntry& e)
    {
        if (e.kind == Kind::PathBlend) {
            // NEOTKO_SANDWICH_TAG — Fase 5 s73: Half/Full distinction lives in
            // the blob (pb.mode), not in the enum. Collapse the zone to a
            // single PB pass and write the blob with the chosen mode.
            // Deferred via CallAfter: refresh_rows() repopulates the wxChoice
            // that is firing this very event — safer done after dispatch.
            const int pb_mode = e.pb_mode;
            CallAfter([this, z, pb_mode]() {
                const char* zkey = (z == 0) ? "pathblend_top" : "pathblend_penu";
                // Preserve other PB fields (floor/mid_end/tools/ease) when
                // re-selecting; only override the mode.
                Slic3r::PathBlendPassConfig pbc;
                if (auto* o = m_config->option<ConfigOptionString>(zkey))
                    pbc = Slic3r::PathBlendPassConfig::from_blob_json(o->value);
                pbc.mode = static_cast<Slic3r::PathBlendPassConfig::Mode>(pb_mode);
                // NEOTKO_SANDWICH_TAG — Fase 5 s73: pb_apply_constraints
                // re-clamps floor/mid for the new mode (Half forces mid=H,
                // Full caps mid at H-0.04 and ensures mid > floor strictly).
                pb_apply_constraints(pbc, layer_height_mm());
                pbc.sync_legacy_view();
                const std::string blob = pbc.to_blob_json();
                if (auto* o = m_config->option<ConfigOptionString>(zkey))
                    o->value = blob;
                m_on_change(zkey);

                Slic3r::SurfacePass pb;
                pb.kind = Kind::PathBlend; pb.ratio = 1.0;
                pb.pathblend.present = true;
                pb.pathblend.kv["blob"] = blob;
                m_stack[z].passes.assign(1, pb);
                if (m_ui[z].slots_rb) m_ui[z].slots_rb->SetSelection(0);
                refresh_rows(z);
            });
            return;
        }
        // Non-PB: slot count unchanged → kind choice items stay valid; update
        // only this row's widgets without repopulating the choice that just
        // fired this event.
        m_stack[z].passes[idx].kind = e.kind;
        sync_row_widgets(z, idx);
        if (GetSizer()) { GetSizer()->Layout(); Fit(); }
    }

    void pick_solid_tool(int z, int idx)
    {
        wxMenu menu;
        const int n = std::max(1, (int)m_fcolors.size());
        for (int t = 0; t < n; ++t)
            menu.Append(TOOL_MENU_BASE + t, wxString::Format(_L("Tool T%d"), t + 1));
        const int sel = GetPopupMenuSelectionFromUser(menu);
        if (sel == wxID_NONE) return;
        m_stack[z].passes[idx].solid_tool = sel - TOOL_MENU_BASE;
        if (idx < (int)m_ui[z].chips.size())   m_ui[z].chips[idx]->Refresh();
        if (idx < (int)m_ui[z].preview.size()) m_ui[z].preview[idx]->Refresh();
    }

    // Edited one pass's height (mm) → set its ratio, rescale the others so the
    // stack still fills the layer, then re-sync every row.
    // NEOTKO_SANDWICH_TAG — Fase 5 s73: for PathBlend, the spinner means
    // floor_mm (not ratio) — delegate to on_pathblend_floor_edit.
    void on_height_edit(int z, int idx)
    {
        auto& ps = m_stack[z].passes;
        const int n = (int)ps.size();
        if (idx < 0 || idx >= n || !m_ui[z].ratio_spin[idx]) return;
        if (ps[idx].kind == Kind::PathBlend) {
            on_pathblend_floor_edit(z, idx);
            return;
        }
        const double LH = layer_height_mm();
        double newR = std::clamp(m_ui[z].ratio_spin[idx]->GetValue() / LH, 0.0, 1.0);

        double othersOld = 0;
        for (int j = 0; j < n; ++j)
            if (j != idx) othersOld += std::max(0.0, ps[j].ratio);
        ps[idx].ratio = newR;
        if (n > 1) {
            const double target = 1.0 - newR;
            if (othersOld > 1e-6) {
                const double k = target / othersOld;
                for (int j = 0; j < n; ++j)
                    if (j != idx) ps[j].ratio = std::max(0.0, ps[j].ratio) * k;
            } else {
                const double ev = target / (n - 1);
                for (int j = 0; j < n; ++j) if (j != idx) ps[j].ratio = ev;
            }
        }
        sync_heights(z);
    }

    // Push m_stack ratios back into every row's spin + warning + bar.
    void sync_heights(int z)
    {
        const double LH = layer_height_mm();
        const int n = (int)m_stack[z].passes.size();
        for (int i = 0; i < n; ++i) {
            if (m_ui[z].ratio_spin[i])
                m_ui[z].ratio_spin[i]->SetValue(m_stack[z].passes[i].ratio * LH);
            sync_row_widgets(z, i);
        }
        if (m_ui[z].ratio_bar) m_ui[z].ratio_bar->Refresh();
        update_sum(z);
        if (GetSizer()) { GetSizer()->Layout(); Fit(); }
    }

    void normalize(int z)
    {
        auto& ps = m_stack[z].passes;
        double sum = 0;
        for (const auto& p : ps) sum += std::max(0.0, p.ratio);
        if (sum < 1e-6) return;
        for (auto& p : ps) p.ratio = std::max(0.0, p.ratio) / sum;
        sync_heights(z);
    }

    void update_sum(int z)
    {
        ZoneUI& u = m_ui[z];
        if (!u.sum_lbl) return;
        const double LH = layer_height_mm();
        const int n = (int)m_stack[z].passes.size();
        double mm = 0;
        for (int i = 0; i < n; ++i) mm += std::max(0.0, m_stack[z].passes[i].ratio) * LH;
        u.sum_lbl->SetLabel(wxString::Format(_L("Layer height: %.2f mm   (Σ %.2f)"),
                                             LH, mm));
        const bool ok = std::abs(mm - LH) < 0.005;
        u.sum_lbl->SetForegroundColour(ok ? wxColour(150, 150, 150)
                                          : wxColour(220, 150, 60));
        u.sum_lbl->Refresh();
        if (u.ratio_bar) u.ratio_bar->Refresh();
    }

    void sync_zone_enabled(int z)
    {
        const bool on = m_stack[z].enabled;
        if (m_ui[z].slots_rb)  m_ui[z].slots_rb->Enable(on);
        if (m_ui[z].rows_host) m_ui[z].rows_host->Enable(on);
        if (m_ui[z].norm_btn)  m_ui[z].norm_btn->Enable(on);
        // s118: perim_chk es global (no se ata al enable de la zona Top).
        if (m_ui[z].ratio_bar) { m_ui[z].ratio_bar->Enable(on); m_ui[z].ratio_bar->Refresh(); }
    }

    // ---------------------------------------------------- per-pass fill angle
    // Each Solid pass owns its own SurfacePass.angle, edited inline on its row.
    // NEOTKO_SANDWICH_TAG — Fase 5 s73: for PathBlend, the same widget edits
    // mid_end_mm (label "mid mm:" in sync_row_widgets). Delegate accordingly.
    void store_angle(int z, int idx)
    {
        if (idx >= (int)m_stack[z].passes.size()) return;
        Slic3r::SurfacePass& p = m_stack[z].passes[idx];
        if (!m_ui[z].angle_txt[idx]) return;
        if (p.kind == Kind::PathBlend) {
            on_pathblend_mid_edit(z, idx);
            return;
        }
        if (p.kind != Kind::Solid) return;
        long v = -1;
        if (!m_ui[z].angle_txt[idx]->GetValue().Trim().Trim(false).ToLong(&v))
            v = -1;
        if (v < 0) v = -1; else if (v > 359) v = 359;
        p.angle = (int)v;
        m_ui[z].angle_txt[idx]->ChangeValue(wxString::Format("%d", (int)v));
        m_ui[z].preview[idx]->Refresh();
    }

    void on_angle_wheel(int z, int idx, wxMouseEvent& e)
    {
        if (idx >= (int)m_stack[z].passes.size()) { e.Skip(); return; }
        Slic3r::SurfacePass& p = m_stack[z].passes[idx];
        if (p.kind == Kind::PathBlend) {
            Slic3r::PathBlendPassConfig pbc = read_pb_blob(z);
            // Half: mid is locked to H — wheel does nothing on this field.
            if (pbc.mode == Slic3r::PathBlendPassConfig::Mode::Half) return;
            // Full: nudge mid_end_mm by 0.01. pb_apply_constraints clamps to
            // [floor + ε, H - 0.04].
            const double step = (e.GetWheelRotation() > 0) ? 0.01 : -0.01;
            pbc.mid_end_mm += (float)step;
            write_pb_blob(z, idx, pbc);
            return;
        }
        if (p.kind != Kind::Solid) { e.Skip(); return; }
        const int step = (e.GetWheelRotation() > 0) ? 5 : -5;
        int a = p.angle;
        if (a < 0) a = (step > 0) ? 0 : -1;          // leave / stay at auto
        else { a += step; if (a < 0) a = -1; else a %= 360; }
        p.angle = a;
        m_ui[z].angle_txt[idx]->ChangeValue(wxString::Format("%d", a));
        m_ui[z].preview[idx]->Refresh();
    }

    // ---------------------------------------------------- PathBlend helpers
    // NEOTKO_SANDWICH_TAG — Fase 5 s73: inline PathBlend editing helpers.
    // Read/write the per-zone PB blob (pathblend_top / pathblend_penu), update
    // both the live config (so the engine sees it) and the SurfacePass kv
    // (so the dialog round-trips on save).

    Slic3r::PathBlendPassConfig read_pb_blob(int z) const
    {
        const char* zkey = (z == 0) ? "pathblend_top" : "pathblend_penu";
        std::string blob;
        if (auto* o = m_config->option<ConfigOptionString>(zkey)) blob = o->value;
        return Slic3r::PathBlendPassConfig::from_blob_json(blob);
    }

    // NEOTKO_SANDWICH_TAG — Fase 5 s73 UX hardening: enforce physical sanity
    // on (floor_mm, mid_end_mm) before any write reaches the engine.
    //   Half: mid_end_mm is the layer top; there is no cap. Forced mid = H.
    //   Full: cap = top 0,04 mm of flow → mid_end ≤ H − 0,04. Ramp must exist
    //         (mid > floor strictly); equal values would mean two flat layers,
    //         which is the MultiPass use-case, not PathBlend.
    // NEOTKO_COLORSTITCH_TAG s108 — body promoted to the engine
    // (PathBlendPassConfig::apply_constraints) so the painter pro-mode tray
    // shares the exact same rules; this wrapper keeps the call sites intact.
    static void pb_apply_constraints(Slic3r::PathBlendPassConfig& pbc, double H)
    {
        pbc.apply_constraints(H);
    }

    void write_pb_blob(int z, int idx, const Slic3r::PathBlendPassConfig& pbc_in)
    {
        Slic3r::PathBlendPassConfig pbc = pbc_in;
        pb_apply_constraints(pbc, layer_height_mm());
        pbc.sync_legacy_view();
        const std::string blob = pbc.to_blob_json();
        const char* zkey = (z == 0) ? "pathblend_top" : "pathblend_penu";
        if (auto* o = m_config->option<ConfigOptionString>(zkey))
            o->value = blob;
        m_on_change(zkey);
        if (idx >= 0 && idx < (int)m_stack[z].passes.size()) {
            Slic3r::SurfacePass& p = m_stack[z].passes[idx];
            p.pathblend.present = true;
            p.pathblend.kv["blob"] = blob;
        }
        // Re-sync the row widgets so spinners / badge / preview reflect the change.
        if (idx >= 0 && idx < (int)m_ui[z].ratio_spin.size() && m_ui[z].ratio_spin[idx])
            m_ui[z].ratio_spin[idx]->SetValue(pbc.floor_mm);
        if (idx >= 0 && idx < (int)m_ui[z].angle_txt.size() && m_ui[z].angle_txt[idx])
            m_ui[z].angle_txt[idx]->ChangeValue(wxString::Format("%.3f", pbc.mid_end_mm));
        if (idx >= 0 && idx < (int)m_ui[z].chips.size())   m_ui[z].chips[idx]->Refresh();
        if (idx >= 0 && idx < (int)m_ui[z].preview.size()) m_ui[z].preview[idx]->Refresh();
        if (idx >= 0 && idx < (int)m_ui[z].badge.size())   sync_row_widgets(z, idx);
    }

    // The ratio_spin for a PB row carries floor_mm. pb_apply_constraints
    // (in write_pb_blob) enforces the per-mode clamp and bumps mid_end if the
    // new floor crosses it.
    void on_pathblend_floor_edit(int z, int idx)
    {
        if (idx < 0 || idx >= (int)m_stack[z].passes.size()) return;
        if (!m_ui[z].ratio_spin[idx]) return;
        Slic3r::PathBlendPassConfig pbc = read_pb_blob(z);
        pbc.floor_mm = (float)m_ui[z].ratio_spin[idx]->GetValue();
        // If user pushed floor above current mid, bump mid up so the ramp
        // survives the strict mid>floor rule in pb_apply_constraints.
        if (pbc.mid_end_mm <= pbc.floor_mm) pbc.mid_end_mm = pbc.floor_mm + 0.001f;
        write_pb_blob(z, idx, pbc);
    }

    // The angle_txt for a PB row carries mid_end_mm. In Half mode the field is
    // read-only (mid is forced to H) — ignore edits and just refresh.
    void on_pathblend_mid_edit(int z, int idx)
    {
        if (idx < 0 || idx >= (int)m_stack[z].passes.size()) return;
        if (!m_ui[z].angle_txt[idx]) return;
        Slic3r::PathBlendPassConfig pbc = read_pb_blob(z);
        if (pbc.mode == Slic3r::PathBlendPassConfig::Mode::Half) {
            write_pb_blob(z, idx, pbc);  // re-syncs widget to forced mid=H
            return;
        }
        double v = 0;
        wxString txt = m_ui[z].angle_txt[idx]->GetValue().Trim().Trim(false);
        if (!txt.ToDouble(&v)) v = pbc.mid_end_mm;
        pbc.mid_end_mm = (float)v;
        write_pb_blob(z, idx, pbc);
    }

    // adv_btn click on a PB row cycles the ease_mode (Linear -> EaseIn ->
    // EaseOut -> EaseInOut -> Linear ...).
    void cycle_pathblend_ease(int z, int idx)
    {
        if (idx < 0 || idx >= (int)m_stack[z].passes.size()) return;
        Slic3r::PathBlendPassConfig pbc = read_pb_blob(z);
        pbc.ease_mode = (pbc.ease_mode + 1) % 4;
        write_pb_blob(z, idx, pbc);
    }

    // chip click on a PB row picks the tool for slot 0 (ramp/bottom) or slot 1
    // (cap/top). Only slot 0 is meaningful in Half mode.
    void pick_pathblend_tool(int z, int idx, int slot)
    {
        if (idx < 0 || idx >= (int)m_stack[z].passes.size()) return;
        Slic3r::PathBlendPassConfig pbc = read_pb_blob(z);
        if (slot == 1 && pbc.mode != Slic3r::PathBlendPassConfig::Mode::Full) return;
        wxMenu menu;
        const int n = std::max(1, (int)m_fcolors.size());
        for (int t = 0; t < n; ++t)
            menu.Append(TOOL_MENU_BASE + t, wxString::Format(_L("Tool T%d"), t + 1));
        const int sel = GetPopupMenuSelectionFromUser(menu);
        if (sel == wxID_NONE) return;
        const int tool = sel - TOOL_MENU_BASE;
        if (slot == 0) pbc.tool_bottom = tool;
        else           pbc.tool_top    = tool;
        write_pb_blob(z, idx, pbc);
    }

    // NEOTKO_SANDWICH_TAG — per-lámina ColorMix gradient editor.
    // Each ColorMix pass owns its gradient via pass.colormix.kv (colormix_keys
    // of the role + angle, full region-key names). The shared region keys
    // (interlayer_colormix_*) are NEVER mutated here — they remain the legacy /
    // synthesize_from_legacy fallback. The ColorMixPatternDialog reads/writes the
    // DynamicPrintConfig, so we transiently load the pass override into m_config
    // to drive the dialog, snapshot the result back into the pass, then restore
    // the shared config untouched. Engine side: Fill.cpp FASE 2 applies the same
    // kv over a copy of the region config (single source → wipe-tower stays in sync).
    void open_colormix_for(int z, int idx)
    {
        if (idx < 0 || idx >= (int)m_stack[z].passes.size()) return;
        Slic3r::SurfacePass& pass = m_stack[z].passes[idx];

        const char* pat_key = (z == 0) ? "interlayer_colormix_pattern_top"
                                       : "interlayer_colormix_pattern_penultimate";
        const std::string gp = (z == 1) ? std::string("interlayer_colormix_penu_")
                                        : std::string("interlayer_colormix_");
        // Role gradient keys this editor owns (colormix_keys of the role + angle).
        std::vector<std::string> role_keys;
        role_keys.push_back(pat_key);
        for (const char* s : { "mode", "pct_a", "pct_b", "easing", "gamma",
                               "min_surface_lines", "overlap", "invert", "repetitions",
                               "band_count_a", "band_count_b", "band_count_c",
                               "band_count_d", "tool_a", "tool_b", "tool_c",
                               "tool_d", "angle" })
            role_keys.push_back(gp + s);

        using SEPM = Slic3r::SurfaceEffectProfileManager;
        // Save the shared region values so we can restore them afterwards, then
        // load this pass's override (no-op when the pass has none → the dialog
        // shows the shared fallback, exactly what the engine would use).
        Slic3r::SurfaceEffectPayload saved = SEPM::snapshot_keys(*m_config, role_keys);
        SEPM::restore_keys(*m_config, pass.colormix);

        std::string mixed_defs;
        if (auto* o = m_config->option<ConfigOptionString>("mixed_filament_definitions"))
            mixed_defs = o->value;
        const auto options =
            Slic3r::SurfaceColorMix::get_mix_options(mixed_defs, m_fcolors);
        const std::string cur_pat = m_config->opt_string(pat_key);
        bool use_virtual = false;
        if (auto* o = m_config->option<ConfigOptionBool>("interlayer_colormix_use_virtual"))
            use_virtual = o->value;
        ColorMixPatternDialog dlg(this, options, m_fcolors, cur_pat,
                                  use_virtual, m_config, z);
        if (dlg.ShowModal() != wxID_OK) {
            SEPM::restore_keys(*m_config, saved);   // undo the transient load
            return;
        }

        // Apply the dialog outputs into m_config transiently (no m_on_change —
        // the shared config is reverted below; persistence is via the stack blob).
        auto si = [&](const std::string& k, int v) {
            if (auto* o = m_config->option<ConfigOptionInt>(k)) o->value = v;
        };
        auto sf = [&](const std::string& k, double v) {
            if (auto* o = m_config->option<ConfigOptionFloat>(k)) o->value = v;
        };
        if (auto* o = m_config->option<ConfigOptionString>(pat_key))
            o->value = dlg.get_pattern();
        si(gp + "mode",              dlg.get_grad_mode());
        si(gp + "pct_a",             dlg.get_grad_pct_a());
        si(gp + "pct_b",             dlg.get_grad_pct_b());
        si(gp + "easing",            dlg.get_grad_easing());
        sf(gp + "gamma",             dlg.get_grad_gamma());
        si(gp + "min_surface_lines", dlg.get_grad_min_lines());
        sf(gp + "overlap",           dlg.get_grad_overlap());
        if (auto* o = m_config->option<ConfigOptionBool>(gp + "invert"))
            o->value = dlg.get_grad_invert();
        si(gp + "band_count_a", dlg.get_grad_band_a());
        si(gp + "band_count_b", dlg.get_grad_band_b());
        si(gp + "band_count_c", dlg.get_grad_band_c());
        si(gp + "band_count_d", dlg.get_grad_band_d());
        si(gp + "tool_a",       dlg.get_tool_a());
        si(gp + "tool_b",       dlg.get_tool_b());
        si(gp + "tool_c",       dlg.get_tool_c());
        si(gp + "tool_d",       dlg.get_tool_d());
        si(gp + "angle",        dlg.get_grad_angle());
        si(gp + "repetitions",  dlg.get_grad_repetitions());
        // NEOTKO_COLORMIX_TAG — s90: global key (no prefix). NOT included in
        // role_keys/snapshot — it's intentionally outside the per-pass override
        // because there's a single value shared by all CM passes.
        sf("interlayer_colormix_min_length", dlg.get_grad_min_length());

        // Snapshot the edited keys into this pass's override, then restore shared.
        pass.colormix = SEPM::snapshot_keys(*m_config, role_keys);
        pass.colormix.present = true;
        SEPM::restore_keys(*m_config, saved);

        if (idx >= 0 && idx < (int)m_ui[z].chips.size()) {
            m_ui[z].chips[idx]->Refresh();
            m_ui[z].preview[idx]->Refresh();
        }
    }

    // NEOTKO_SANDWICH_TAG — Fase 5 s73: open_pathblend_for removed.
    // Everything is edited inline on the row (kind selector Half/Full,
    // floor_mm spinner, mid_end_mm field, ease cycler button, chip click
    // tool pickers). The PathBlendDialog popup class is also unused (kept
    // in the file as dead code; safe to delete in a follow-up cleanup).

    // ---------------------------------------------------------------- painting
    void paint_chip(int z, int idx)
    {
        wxPanel* w = m_ui[z].chips[idx];
        if (!w) return;
        wxPaintDC dc(w);
        if (idx >= (int)m_stack[z].passes.size()) return;  // hidden row
        const wxSize sz = w->GetClientSize();
        dc.SetBrush(wxBrush(wxColour(45, 45, 45)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);

        const Slic3r::SurfacePass& p = m_stack[z].passes[idx];

        // NEOTKO_SANDWICH_TAG — Fase 5 s73: PathBlend chip = 2 stacked halves
        // (top half = cap tool, bottom half = ramp tool) so the click handler
        // can pick the slot from the Y coordinate. Half mode hides the cap
        // half (only ramp visible).
        if (p.kind == Kind::PathBlend) {
            Slic3r::PathBlendPassConfig pbc;
            const auto it_blob = p.pathblend.kv.find("blob");
            if (it_blob != p.pathblend.kv.end() && !it_blob->second.empty())
                pbc = Slic3r::PathBlendPassConfig::from_blob_json(it_blob->second);
            const int half_h = sz.y / 2;
            dc.SetPen(wxPen(wxColour(20, 20, 20)));
            // Bottom half = ramp (always present).
            dc.SetBrush(wxBrush(tool_colour(pbc.tool_bottom)));
            dc.DrawRectangle(1, half_h, sz.x - 2, sz.y - half_h - 1);
            // Top half = cap (Full only).
            if (pbc.mode == Slic3r::PathBlendPassConfig::Mode::Full) {
                dc.SetBrush(wxBrush(tool_colour(pbc.tool_top)));
                dc.DrawRectangle(1, 1, sz.x - 2, half_h - 1);
            }
            // Labels: T# in white on each half.
            dc.SetTextForeground(*wxWHITE);
            wxFont f = dc.GetFont(); f.SetPointSize(7); dc.SetFont(f);
            dc.DrawText(wxString::Format("T%d", pbc.tool_bottom + 1), 3, half_h + 1);
            if (pbc.mode == Slic3r::PathBlendPassConfig::Mode::Full)
                dc.DrawText(wxString::Format("T%d", pbc.tool_top + 1), 3, 2);
            return;
        }

        std::vector<int> tools;
        if      (p.kind == Kind::Solid)     tools = { p.solid_tool };
        else if (p.kind == Kind::ColorMix)  tools = colormix_tools(z, &p);
        if (tools.empty()) return;

        const int cw = std::min(20, sz.x / (int)tools.size());
        int x = 1;
        for (size_t i = 0; i < tools.size() && x + cw <= sz.x; ++i) {
            dc.SetBrush(wxBrush(tool_colour(tools[i])));
            dc.SetPen(wxPen(wxColour(20, 20, 20)));
            dc.DrawRectangle(x, 2, cw - 2, sz.y - 4);
            if (p.kind == Kind::Solid) {
                dc.SetTextForeground(*wxWHITE);
                wxFont f = dc.GetFont(); f.SetPointSize(7); dc.SetFont(f);
                dc.DrawText(wxString::Format("T%d", tools[i] + 1), x + 2, 4);
            }
            x += cw;
        }
    }

    void paint_preview(int z, int idx)
    {
        wxPanel* w = m_ui[z].preview[idx];
        if (!w) return;
        wxPaintDC dc(w);
        if (idx >= (int)m_stack[z].passes.size()) return;  // hidden row
        const wxSize sz = w->GetClientSize();
        const Slic3r::SurfacePass& p = m_stack[z].passes[idx];
        dc.SetPen(*wxTRANSPARENT_PEN);

        if (p.kind == Kind::Solid) {
            // tool colour + hatch lines showing the fill angle (auto = 45°)
            const wxColour base = tool_colour(p.solid_tool);
            dc.SetBrush(wxBrush(base));
            dc.DrawRectangle(0, 0, sz.x, sz.y);
            const bool   autom = (p.angle < 0);
            const double PI    = 3.14159265358979323846;
            const double rad   = (autom ? 45.0 : (double)p.angle) * PI / 180.0;
            const double dx = std::cos(rad), dy = -std::sin(rad);
            const double nx = -dy,           ny = dx;
            const bool   light_bg = (base.Red() + base.Green() + base.Blue() > 384);
            dc.SetPen(wxPen(light_bg ? wxColour(40, 40, 40)
                                     : wxColour(225, 225, 225), 1));
            const int cx = sz.x / 2, cy = sz.y / 2;
            const int R  = sz.x + sz.y;
            for (int off = -R; off <= R; off += 6) {
                const double ox = cx + off * nx, oy = cy + off * ny;
                dc.DrawLine((int)(ox - dx * R), (int)(oy - dy * R),
                            (int)(ox + dx * R), (int)(oy + dy * R));
            }
        } else if (p.kind == Kind::ColorMix) {
            // NEOTKO_SANDWICH_TAG — s80: render the REAL per-line tool sequence
            // the engine will produce (mode-aware dither + repetitions + invert),
            // reading this pass's per-lámina override. Each sequence slot is one
            // vertical stripe — same representation as the ColorMix dialog strip.
            const std::vector<int> seq = colormix_preview_seq(z, p, std::max(2, sz.x));
            const int NS = (int)seq.size();
            if (NS <= 0) {
                dc.SetBrush(wxBrush(wxColour(90, 90, 90)));
                dc.DrawRectangle(0, 0, sz.x, sz.y);
            } else {
                for (int x = 0; x < sz.x; ++x) {
                    const int si = (sz.x > 1) ? (int)((long long)x * NS / sz.x) : 0;
                    const int t  = seq[std::min(NS - 1, std::max(0, si))];
                    const wxColour c = (t < 0) ? wxColour(150, 150, 150)
                                               : tool_colour(t);
                    dc.SetPen(wxPen(c));
                    dc.DrawLine(x, 0, x, sz.y);
                }
            }
        } else if (p.kind == Kind::PathBlend) {
            // NEOTKO_SANDWICH_TAG — Fase 5 s73: preview reflects the actual
            // v=2 geometry (floor_mm, mid_end_mm, mode, ease_mode).
            //   - Dark grey background = the whole layer [0..H].
            //   - For Full: a flat cap rectangle from mid_end up to H, tool_top color.
            //   - The ramp: filled polygon from (t=0, floor) up to (t=1, mid_end),
            //     with vertices following the easing curve (visible steps).
            //   - Half: no cap rectangle; the area above the ramp stays dark.
            //   - Horizontal indicator lines at floor and mid_end.
            //
            // Coordinate convention: y=0 is top of the panel = nominal_z. y=sz.y
            // is bottom = bottom_z. Z increases upward visually.
            Slic3r::PathBlendPassConfig pbc;
            const auto it_blob = p.pathblend.kv.find("blob");
            if (it_blob != p.pathblend.kv.end() && !it_blob->second.empty())
                pbc = Slic3r::PathBlendPassConfig::from_blob_json(it_blob->second);

            const double H = std::max(0.04, layer_height_mm());
            const double floor_frac   = std::clamp((double)pbc.floor_mm   / H, 0.0, 1.0);
            const double mid_end_frac = std::clamp((double)pbc.mid_end_mm / H, 0.0, 1.0);
            auto y_from_frac = [&](double f) -> int {
                return (int)std::round((1.0 - f) * (double)sz.y);
            };

            // Background = dark grey (= unfilled / above-ramp area in Half).
            dc.SetBrush(wxBrush(wxColour(40, 40, 40)));
            dc.DrawRectangle(0, 0, sz.x, sz.y);

            // Cap (Full only): flat band from mid_end up to nominal.
            if (pbc.mode == Slic3r::PathBlendPassConfig::Mode::Full) {
                const int y_mid = y_from_frac(mid_end_frac);
                const int y_top = y_from_frac(1.0);
                if (y_mid > y_top) {
                    dc.SetBrush(wxBrush(tool_colour(pbc.tool_top)));
                    dc.DrawRectangle(0, y_top, sz.x, y_mid - y_top);
                }
            }

            // Ramp: polygon following the easing curve from (t=0, floor) to (t=1, mid_end).
            // Steps = 16 (enough to make the easing curve visible without aliasing).
            const int steps = 16;
            std::vector<wxPoint> poly;
            poly.reserve(steps + 3);
            poly.push_back(wxPoint(0, sz.y));      // bottom-left corner
            for (int i = 0; i <= steps; ++i) {
                const double t_raw = (double)i / (double)steps;
                double t = t_raw;
                switch (pbc.ease_mode) {
                    case 1: t = t * t;                       break; // EaseIn
                    case 2: t = 1.0 - (1.0 - t) * (1.0 - t); break; // EaseOut
                    case 3: t = t * t * (3.0 - 2.0 * t);     break; // EaseInOut
                    default: break;                                  // Linear
                }
                const double z_frac = floor_frac + t * (mid_end_frac - floor_frac);
                const int x = (int)std::round(t_raw * (double)sz.x);
                poly.push_back(wxPoint(x, y_from_frac(z_frac)));
            }
            poly.push_back(wxPoint(sz.x, sz.y));   // bottom-right corner
            dc.SetBrush(wxBrush(tool_colour(pbc.tool_bottom)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawPolygon((int)poly.size(), poly.data());

            // Horizontal indicator lines + mm labels.
            dc.SetPen(wxPen(wxColour(200, 200, 200), 1, wxPENSTYLE_DOT));
            dc.SetTextForeground(wxColour(220, 220, 220));
            wxFont f = dc.GetFont(); f.SetPointSize(6); dc.SetFont(f);
            const int y_floor = y_from_frac(floor_frac);
            dc.DrawLine(0, y_floor, sz.x, y_floor);
            dc.DrawText(wxString::Format("%.2f", pbc.floor_mm), 2, std::min(y_floor, sz.y - 8));
            if (pbc.mode == Slic3r::PathBlendPassConfig::Mode::Full) {
                const int y_mid = y_from_frac(mid_end_frac);
                dc.DrawLine(0, y_mid, sz.x, y_mid);
                dc.DrawText(wxString::Format("%.2f", pbc.mid_end_mm),
                            2, std::max(y_mid - 8, 0));
            }
        } else { // None — diagonal grey hatch
            dc.SetBrush(wxBrush(wxColour(70, 70, 70)));
            dc.DrawRectangle(0, 0, sz.x, sz.y);
            dc.SetPen(wxPen(wxColour(110, 110, 110)));
            for (int x = -sz.y; x < sz.x; x += 7)
                dc.DrawLine(x, sz.y, x + sz.y, 0);
        }
    }

    // -------------------------------------------------- stacked Z-ratio bar
    // Representative colour of a pass for the stacked bar.
    wxColour seg_colour(int z, int idx) const
    {
        const Slic3r::SurfacePass& p = m_stack[z].passes[idx];
        if (p.kind == Kind::Solid)     return tool_colour(p.solid_tool);
        if (p.kind == Kind::ColorMix)  return tool_colour(colormix_tools(z, &p).front());
        if (p.kind == Kind::PathBlend) return tool_colour(pathblend_tools(z).front());
        return wxColour(90, 90, 90);                       // None
    }

    void paint_ratio_bar(int z)
    {
        wxPanel* w = m_ui[z].ratio_bar;
        if (!w) return;
        wxPaintDC dc(w);
        const wxSize sz = w->GetClientSize();
        dc.SetBrush(wxBrush(wxColour(30, 30, 30)));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, 0, sz.x, sz.y);

        const auto& ps = m_stack[z].passes;
        const int n = (int)ps.size();
        if (n == 0 || !m_stack[z].enabled) return;
        double sum = 0;
        for (const auto& p : ps) sum += std::max(0.0, p.ratio);
        if (sum < 1e-6) sum = 1.0;

        wxFont f = dc.GetFont(); f.SetPointSize(7); dc.SetFont(f);
        double acc = 0.0;
        for (int dp = 0; dp < n; ++dp) {            // dp 0 = top of stack
            const int idx  = n - 1 - dp;
            const double fr = std::max(0.0, ps[idx].ratio) / sum;
            const int y0 = (int)std::round(acc * sz.y);
            acc += fr;
            const int y1 = (dp == n - 1) ? sz.y : (int)std::round(acc * sz.y);
            dc.SetBrush(wxBrush(seg_colour(z, idx)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRectangle(0, y0, sz.x, y1 - y0);
            dc.SetTextForeground(*wxWHITE);
            wxFont sf = dc.GetFont(); sf.SetPointSize(7); dc.SetFont(sf);
            dc.DrawText(wxString::Format("%.2f", fr * layer_height_mm()),
                        2, y0 + 2);
            if (dp != n - 1) {                      // draggable divider
                dc.SetPen(wxPen(wxColour(235, 235, 235), 2));
                dc.DrawLine(0, y1, sz.x, y1);
            }
        }
    }

    void ratio_bar_down(int z, wxMouseEvent& e)
    {
        const auto& ps = m_stack[z].passes;
        const int n = (int)ps.size();
        if (n < 2 || !m_stack[z].enabled) return;
        const int h = m_ui[z].ratio_bar->GetClientSize().y;
        double sum = 0;
        for (const auto& p : ps) sum += std::max(0.0, p.ratio);
        if (sum < 1e-6 || h <= 0) return;
        double acc = 0;
        for (int k = 0; k < n - 1; ++k) {           // boundary k = below display pos k
            acc += std::max(0.0, ps[n - 1 - k].ratio) / sum;
            if (std::abs(e.GetY() - (int)std::round(acc * h)) <= 6) {
                m_drag_zone = z; m_drag_bound = k;
                m_ui[z].ratio_bar->CaptureMouse();
                return;
            }
        }
    }

    void ratio_bar_motion(int z, wxMouseEvent& e)
    {
        if (m_drag_zone != z || m_drag_bound < 0 || !e.Dragging()) return;
        auto& ps = m_stack[z].passes;
        const int n = (int)ps.size();
        const int k = m_drag_bound;
        if (k < 0 || k >= n - 1) return;
        const int h = m_ui[z].ratio_bar->GetClientSize().y;
        if (h <= 0) return;
        double sum = 0;
        for (const auto& p : ps) sum += std::max(0.0, p.ratio);
        if (sum < 1e-6) return;

        const int idxA = n - 1 - k;                 // pass above the divider
        const int idxB = n - 2 - k;                 // pass below
        double topAcc = 0;                          // fraction above pass A
        for (int kk = 0; kk < k; ++kk)
            topAcc += std::max(0.0, ps[n - 1 - kk].ratio) / sum;
        const double comb = (std::max(0.0, ps[idxA].ratio)
                           + std::max(0.0, ps[idxB].ratio)) / sum;
        // minimum drag fraction = 0.04 mm so the bar can't create a sub-min pass.
        const double minF = std::min(0.45, kMinPassMM / layer_height_mm());
        double aFrac = (double)e.GetY() / h - topAcc;
        aFrac = std::clamp(aFrac, minF, std::max(minF, comb - minF));
        ps[idxA].ratio = aFrac * sum;               // keeps Σ unchanged
        ps[idxB].ratio = (comb - aFrac) * sum;

        const double LH = layer_height_mm();
        if (m_ui[z].ratio_spin[idxA])
            m_ui[z].ratio_spin[idxA]->SetValue(ps[idxA].ratio / sum * LH);
        if (m_ui[z].ratio_spin[idxB])
            m_ui[z].ratio_spin[idxB]->SetValue(ps[idxB].ratio / sum * LH);
        sync_row_widgets(z, idxA);
        sync_row_widgets(z, idxB);
        m_ui[z].ratio_bar->Refresh();
        update_sum(z);
    }

    void ratio_bar_up(int z, wxMouseEvent&)
    {
        if (m_drag_zone == z && m_ui[z].ratio_bar
            && m_ui[z].ratio_bar->HasCapture())
            m_ui[z].ratio_bar->ReleaseMouse();
        m_drag_zone = -1; m_drag_bound = -1;
    }

    // NEOTKO_SANDWICH_TAG — Fase 6b/Plan1: snapshot the role-prefixed ColorMix
    // gradient keys of zone z from the live config (same key set the per-pass
    // gradient editor `open_colormix_for` owns). Used to bake a self-contained
    // gradient into a ColorMix pass that has no per-pass override.
    Slic3r::SurfaceEffectPayload zone_colormix_snapshot(int z) const
    {
        const char* pat_key = (z == 0) ? "interlayer_colormix_pattern_top"
                                       : "interlayer_colormix_pattern_penultimate";
        const std::string gp = (z == 1) ? std::string("interlayer_colormix_penu_")
                                        : std::string("interlayer_colormix_");
        std::vector<std::string> role_keys;
        role_keys.push_back(pat_key);
        for (const char* s : { "mode", "pct_a", "pct_b", "easing", "gamma",
                               "min_surface_lines", "overlap", "invert", "repetitions",
                               "band_count_a", "band_count_b", "band_count_c",
                               "band_count_d", "tool_a", "tool_b", "tool_c",
                               "tool_d", "angle" })
            role_keys.push_back(gp + s);
        auto pl = Slic3r::SurfaceEffectProfileManager::snapshot_keys(*m_config, role_keys);
        pl.present = true;
        return pl;
    }

    // NEOTKO_SANDWICH_TAG — Fase 6: normalize one zone's stack EXACTLY as commit()
    // does (flush typed angles → read ratio spins → fold sub-0.04 mm passes →
    // Σ=1) but on a COPY, returning its to_json(). Used by "Save as profile…" so
    // the profile captures the same blob commit() would write, without mutating
    // config or closing the dialog.
    std::string normalized_zone_json(int z)
    {
        // flush typed angles into the live model first (same as commit()).
        for (int idx = 0; idx < (int)m_stack[z].passes.size(); ++idx)
            if (m_stack[z].passes[idx].kind == Kind::Solid)
                store_angle(z, idx);

        const double LH = layer_height_mm();
        Slic3r::SurfacePassStack st = m_stack[z];   // copy
        for (size_t i = 0; i < m_ui[z].ratio_spin.size() && i < st.passes.size(); ++i)
            st.passes[i].ratio = m_ui[z].ratio_spin[i]->GetValue() / LH;

        double lost = 0, keepSum = 0;
        for (auto& p : st.passes) {
            if (p.ratio * LH < kMinPassMM - 1e-9) { lost += std::max(0.0, p.ratio); p.ratio = 0.0; }
            else keepSum += std::max(0.0, p.ratio);
        }
        if (lost > 0 && keepSum > 1e-6)
            for (auto& p : st.passes)
                if (p.ratio > 0) p.ratio += lost * (p.ratio / keepSum);

        double sum = 0;
        for (const auto& p : st.passes) sum += std::max(0.0, p.ratio);
        if (sum > 1e-6)
            for (auto& p : st.passes) p.ratio = std::max(0.0, p.ratio) / sum;
        else if (!st.passes.empty())
            for (auto& p : st.passes) p.ratio = 1.0 / st.passes.size();

        // NEOTKO_SANDWICH_TAG — Fase 6b/Plan1: a ColorMix pass with no per-pass
        // gradient (the user never opened "Edit gradient…") serializes with an
        // empty kv. In PAINTER mode the engine would then fall back to the
        // SLICED OBJECT's preset (default T0/T1), not this profile — so the saved
        // sandwich is not self-contained and the colors come out wrong. Bake the
        // current zone gradient snapshot into those passes. Passes that DO carry
        // a per-pass gradient (kv non-empty, set by open_colormix_for) are left
        // untouched, so an explicit gradient always wins.
        for (auto& p : st.passes)
            if (p.kind == Kind::ColorMix && p.colormix.kv.empty())
                p.colormix = zone_colormix_snapshot(z);

        return st.to_json();   // "" when disabled/empty
    }

    // NEOTKO_PROFILE_TAG — Fase 6: save the current sandwich as a 3D-Painter
    // profile. Stores ONLY the authoritative stack blobs; the legacy 3 payloads
    // stay empty (engine migration to consume the stack = Fase 6b).
    void on_save_profile()
    {
        wxTextEntryDialog dlg(this, _L("Profile name:"), _L("Save Sandwich Profile"));
        if (dlg.ShowModal() != wxID_OK) return;
        const std::string name = dlg.GetValue().ToStdString();
        if (name.empty()) return;
        Slic3r::SurfaceEffectProfile p;
        p.name            = name;
        p.stack_top_json  = normalized_zone_json(0);
        p.stack_penu_json = normalized_zone_json(1);
        if (p.stack_top_json.empty() && p.stack_penu_json.empty()) {
            wxMessageBox(_L("Nothing to save: both zones are empty or disabled."),
                         _L("Sandwich Profile"), wxOK | wxICON_WARNING, this);
            return;
        }
        const int new_id = Slic3r::SurfaceEffectProfileManager::get().add(std::move(p));
        wxMessageBox(wxString::Format(_L("Saved sandwich profile #%d."), new_id),
                     _L("Sandwich Profile"), wxOK | wxICON_INFORMATION, this);
    }

    // Short per-zone description for the manage list (e.g. "CM+T2").
    wxString stack_desc(const std::string& js) const
    {
        Slic3r::SurfacePassStack st = Slic3r::SurfacePassStack::from_json(js);
        if (st.passes.empty()) return "—";
        wxString s;
        for (const auto& p : st.passes) {
            if (!s.empty()) s += "+";
            switch (p.kind) {
                case Kind::Solid:     s += wxString::Format("T%d", p.solid_tool + 1); break;
                case Kind::ColorMix:  s += "CM"; break;
                case Kind::PathBlend: s += "PB"; break;
                default:              s += "·"; break;
            }
        }
        return s;
    }

    // NEOTKO_PROFILE_TAG — Fase 6 / 6b: manage Sandwich profiles.
    // Load into dialog / Update / Rename / Delete. "Load into dialog" repopulates
    // the rows via reload_ui_from_stack() (in-place refresh_rows — never frees a
    // window), and is deferred until after this modal closes so AppKit can't free
    // a live NSView (the crash that originally kept Load out).
    void on_manage_profiles()
    {
        auto& mgr = Slic3r::SurfaceEffectProfileManager::get();
        wxDialog mdlg(this, wxID_ANY, _L("Manage Sandwich Profiles"),
                      wxDefaultPosition, wxSize(440, 320),
                      wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* ms = new wxBoxSizer(wxVERTICAL);
        auto* lb = new wxListBox(&mdlg, wxID_ANY);
        auto refill = [&]() {
            lb->Clear();
            for (const auto& p : mgr.list())
                lb->Append(wxString::Format("#%d  %s   T:%s  P:%s",
                               p.id, wxString::FromUTF8(p.name),
                               stack_desc(p.stack_top_json),
                               stack_desc(p.stack_penu_json)),
                           reinterpret_cast<void*>((intptr_t)p.id));
        };
        refill();

        auto* br = new wxBoxSizer(wxHORIZONTAL);
        auto* btn_load = new wxButton(&mdlg, wxID_ANY, _L("Load into dialog"));
        auto* btn_upd = new wxButton(&mdlg, wxID_ANY, _L("Update from current"));
        auto* btn_ren = new wxButton(&mdlg, wxID_ANY, _L("Rename"));
        auto* btn_del = new wxButton(&mdlg, wxID_ANY, _L("Delete"));
        auto* btn_cls = new wxButton(&mdlg, wxID_CLOSE, _L("Close"));
        br->Add(btn_load, 0, wxRIGHT, 6);
        br->Add(btn_upd, 0, wxRIGHT, 6);
        br->Add(btn_ren, 0, wxRIGHT, 6);
        br->Add(btn_del, 0, wxRIGHT, 6);
        br->AddStretchSpacer(1);
        br->Add(btn_cls, 0);
        ms->Add(lb, 1, wxEXPAND | wxALL, 8);
        ms->Add(br, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        mdlg.SetSizer(ms);

        auto selected_id = [&]() -> int {
            const int sel = lb->GetSelection();
            if (sel == wxNOT_FOUND) return 0;
            return (int)(intptr_t)lb->GetClientData(sel);
        };

        // NEOTKO_SANDWICH_TAG — Fase 6b: Load into dialog. Defer the actual load
        // until AFTER the modal closes (don't mutate the parent dialog's widgets
        // while this child modal is alive — that is what crashed AppKit before).
        btn_load->Bind(wxEVT_BUTTON, [&, this](wxCommandEvent&) {
            const int id = selected_id();
            if (id == 0) return;
            m_pending_load_id = id;
            mdlg.EndModal(wxID_OK);
        });
        btn_upd->Bind(wxEVT_BUTTON, [&, this](wxCommandEvent&) {
            const int id = selected_id();
            if (id == 0) return;
            auto* p = mgr.find_mut(id);
            if (!p) return;
            if (wxMessageBox(wxString::Format(
                    _L("Overwrite profile '%s' with the current sandwich?"),
                    wxString::FromUTF8(p->name)),
                    _L("Update profile"), wxYES_NO | wxICON_QUESTION, &mdlg) != wxYES)
                return;
            p->stack_top_json  = normalized_zone_json(0);
            p->stack_penu_json = normalized_zone_json(1);
            refill();
        });
        btn_ren->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
            const int id = selected_id();
            if (id == 0) return;
            const auto* p = mgr.find(id);
            wxTextEntryDialog td(&mdlg, _L("New name:"), _L("Rename profile"),
                                 wxString::FromUTF8(p ? p->name : ""));
            if (td.ShowModal() == wxID_OK) { mgr.rename(id, td.GetValue().ToStdString()); refill(); }
        });
        btn_del->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) {
            const int id = selected_id();
            if (id == 0) return;
            mgr.remove(id);
            refill();
        });
        btn_cls->Bind(wxEVT_BUTTON, [&](wxCommandEvent&) { mdlg.EndModal(wxID_CLOSE); });
        mdlg.ShowModal();

        // Deferred Load: the modal is gone now, so refreshing the parent rows
        // in place is safe.
        if (m_pending_load_id != 0) {
            const int id = m_pending_load_id;
            m_pending_load_id = 0;
            load_profile_into_dialog(id);
        }
    }

    // ----------------------------------------------------------------- commit
    void commit()
    {
        // flush any angle field still being typed into its pass
        for (int z = 0; z < 2; ++z)
            for (int idx = 0; idx < (int)m_stack[z].passes.size(); ++idx)
                if (m_stack[z].passes[idx].kind == Kind::Solid)
                    store_angle(z, idx);
        const double LH = layer_height_mm();
        for (int z = 0; z < 2; ++z) {
            Slic3r::SurfacePassStack& st = m_stack[z];
            // read heights (mm) from spins back into ratios
            for (size_t i = 0; i < m_ui[z].ratio_spin.size() &&
                               i < st.passes.size(); ++i)
                st.passes[i].ratio = m_ui[z].ratio_spin[i]->GetValue() / LH;

            // fold sub-0.04 mm passes into the rest: set them to ratio 0 (the
            // engine skips a 0-ratio pass cleanly, no Z gap) and redistribute
            // their share to the passes that ARE ≥ 0.04 mm.
            double lost = 0, keepSum = 0;
            for (auto& p : st.passes) {
                if (p.ratio * LH < kMinPassMM - 1e-9) { lost += std::max(0.0, p.ratio); p.ratio = 0.0; }
                else keepSum += std::max(0.0, p.ratio);
            }
            if (lost > 0 && keepSum > 1e-6)
                for (auto& p : st.passes)
                    if (p.ratio > 0) p.ratio += lost * (p.ratio / keepSum);

            // normalize to Σ = 1.0
            double sum = 0;
            for (const auto& p : st.passes) sum += std::max(0.0, p.ratio);
            if (sum > 1e-6)
                for (auto& p : st.passes) p.ratio = std::max(0.0, p.ratio) / sum;
            else if (!st.passes.empty())
                for (auto& p : st.passes) p.ratio = 1.0 / st.passes.size();

            const char* key = (z == 0) ? "neotko_surface_passes_top"
                                       : "neotko_surface_passes_penu";
            const std::string json = st.to_json();   // "" when disabled/empty
            if (auto* o = m_config->option<ConfigOptionString>(key))
                o->value = json;
            m_on_change(key);
        }

        // Sync the legacy enable gates the engine still needs:
        //  - a ColorMix pass needs interlayer_colormix_enabled (bucketing gate)
        //  - a PathBlend pass routes through the legacy GCode engine
        // Disabled zones also clear their MultiPass enable so a stale legacy key
        // can't resurrect the zone via synthesize_from_legacy().
        auto has = [&](int z, Kind k) {
            if (!m_stack[z].enabled) return false;
            for (const auto& p : m_stack[z].passes)
                if (p.kind == k) return true;
            return false;
        };
        const bool cm_t = has(0, Kind::ColorMix),  cm_p = has(1, Kind::ColorMix);
        const bool pb_t = has(0, Kind::PathBlend), pb_p = has(1, Kind::PathBlend);
        auto wb = [&](const char* k, bool v) {
            if (auto* o = m_config->option<ConfigOptionBool>(k)) o->value = v;
            m_on_change(k);
        };
        auto wi = [&](const char* k, int v) {
            if (auto* o = m_config->option<ConfigOptionInt>(k)) o->value = v;
            m_on_change(k);
        };
        wb("interlayer_colormix_enabled", cm_t || cm_p);
        if (cm_t || cm_p)
            wi("interlayer_colormix_surface", (cm_t && cm_p) ? 0 : (cm_t ? 1 : 2));
        wb("multipass_path_gradient", pb_t || pb_p);
        if (pb_t || pb_p)
            wi("pathblend_surface", (pb_t && pb_p) ? 0 : (pb_t ? 1 : 2));
        if (!m_stack[0].enabled) wb("multipass_enabled", false);
        if (!m_stack[1].enabled) wb("penultimate_multipass_enabled", false);

        // Perimeter override: FASE 2 reads the legacy region key
        // `multipass_perimeter_override` (shared, not the per-stack blob flag),
        // so mirror it here or the Sandwich checkbox does nothing.
        const bool perim = (m_stack[0].enabled && m_stack[0].perimeter_override)
                        || (m_stack[1].enabled && m_stack[1].perimeter_override);
        wb("multipass_perimeter_override", perim);

        // NEOTKO_SANDWICH_TAG — Fase 7 (s84): TD + use_virtual write-back.
        // Lane mode is committed live on its wxChoice handler (no extra work).
        if (auto* ac = wxGetApp().app_config) {
            char buf[32];
            for (int i = 0; i < 4; ++i) {
                std::snprintf(buf, sizeof(buf), "%.3f", m_td[i]);
                ac->set("neotko_td_" + std::to_string(i + 1), buf);
            }
        }
        if (m_chk_use_virtual) {
            if (auto* o = m_config->option<ConfigOptionBool>("interlayer_colormix_use_virtual"))
                o->value = m_chk_use_virtual->GetValue();
            m_on_change("interlayer_colormix_use_virtual");
        }
    }
};
// NEOTKO_SANDWICH_TAG_END

} // anonymous namespace
// NEOTKO_COLORMIX_TAG_END

// NEOTKO_COLORSTITCH_TAG — puente público (decl. en ColorStitchPatternLauncher.hpp)
// para abrir el editor avanzado de patrón desde el 3D Painter gizmo, que no puede
// ver la clase ColorMixPatternDialog (file-private, anon ns). Definido aquí, FUERA
// del anon ns → linkage externo; la clase sigue siendo reachable porque el anon ns
// inyecta sus nombres en Slic3r::GUI. Reusa la misma construcción de `options` que
// los call-sites del SandwichDialog. A diferencia de open_edit_for(), NO reescribe
// los knobs gradiente a DynamicPrintConfig: solo devuelve el patrón (+ ángulo), para
// que varios ColorMix por nivel no se contaminen entre sí.
bool open_colorstitch_pattern_dialog(
    wxWindow*                                 parent,
    const std::vector<std::string>&           fcolors,
    bool                                      penu,
    Slic3r::DynamicPrintConfig*               base_cfg,
    const std::map<std::string, std::string>& cur_kv,
    std::map<std::string, std::string>&       out_kv)
{
    using SEPM = Slic3r::SurfaceEffectProfileManager;
    if (!base_cfg) return false;

    // Trabajar sobre una COPIA — el preset vivo NUNCA se ensucia (mismo motivo por
    // el que open_colormix_for hace save/restore: el diálogo lee/escribe el cfg).
    DynamicPrintConfig cfg = *base_cfg;

    const char* pat_key = penu ? "interlayer_colormix_pattern_penultimate"
                               : "interlayer_colormix_pattern_top";
    const std::string gp = penu ? std::string("interlayer_colormix_penu_")
                                : std::string("interlayer_colormix_");
    // role_keys = idéntico a open_colormix_for (pattern + knobs del gradiente role).
    std::vector<std::string> role_keys;
    role_keys.push_back(pat_key);
    for (const char* s : { "mode", "pct_a", "pct_b", "easing", "gamma",
                           "min_surface_lines", "overlap", "invert", "repetitions",
                           "band_count_a", "band_count_b", "band_count_c",
                           "band_count_d", "tool_a", "tool_b", "tool_c",
                           "tool_d", "angle" })
        role_keys.push_back(gp + s);

    // Sembrar el diálogo desde el override actual del pase (vacío → fallback región).
    if (!cur_kv.empty()) {
        Slic3r::SurfaceEffectPayload seed; seed.present = true; seed.kv = cur_kv;
        SEPM::restore_keys(cfg, seed);
    }

    std::string mixed_defs;
    if (auto* o = cfg.option<ConfigOptionString>("mixed_filament_definitions"))
        mixed_defs = o->value;
    const auto options = Slic3r::SurfaceColorMix::get_mix_options(mixed_defs, fcolors);
    const std::string cur_pat = cfg.opt_string(pat_key);
    bool use_virtual = false;
    if (auto* o = cfg.option<ConfigOptionBool>("interlayer_colormix_use_virtual"))
        use_virtual = o->value;

    ColorMixPatternDialog dlg(parent, options, fcolors, cur_pat,
                              use_virtual, &cfg, penu ? 1 : 0);
    if (dlg.ShowModal() != wxID_OK)
        return false;

    // Volcar TODOS los getters al cfg copia (mismo orden que open_colormix_for).
    auto si = [&](const std::string& k, int v) {
        if (auto* o = cfg.option<ConfigOptionInt>(k)) o->value = v; };
    auto sf = [&](const std::string& k, double v) {
        if (auto* o = cfg.option<ConfigOptionFloat>(k)) o->value = v; };
    if (auto* o = cfg.option<ConfigOptionString>(pat_key)) o->value = dlg.get_pattern();
    si(gp + "mode",              dlg.get_grad_mode());
    si(gp + "pct_a",             dlg.get_grad_pct_a());
    si(gp + "pct_b",             dlg.get_grad_pct_b());
    si(gp + "easing",            dlg.get_grad_easing());
    sf(gp + "gamma",             dlg.get_grad_gamma());
    si(gp + "min_surface_lines", dlg.get_grad_min_lines());
    sf(gp + "overlap",           dlg.get_grad_overlap());
    if (auto* o = cfg.option<ConfigOptionBool>(gp + "invert")) o->value = dlg.get_grad_invert();
    si(gp + "band_count_a", dlg.get_grad_band_a());
    si(gp + "band_count_b", dlg.get_grad_band_b());
    si(gp + "band_count_c", dlg.get_grad_band_c());
    si(gp + "band_count_d", dlg.get_grad_band_d());
    si(gp + "tool_a",       dlg.get_tool_a());
    si(gp + "tool_b",       dlg.get_tool_b());
    si(gp + "tool_c",       dlg.get_tool_c());
    si(gp + "tool_d",       dlg.get_tool_d());
    si(gp + "angle",        dlg.get_grad_angle());
    si(gp + "repetitions",  dlg.get_grad_repetitions());
    // min_length es GLOBAL (no role-prefijado) → fuera del per-pase, igual que
    // open_colormix_for lo deja fuera de role_keys.

    // Snapshot del payload completo del pase (string + todos los knobs).
    Slic3r::SurfaceEffectPayload snap = SEPM::snapshot_keys(cfg, role_keys);
    out_kv = std::move(snap.kv);
    return true;
}

#define DISABLE_UNDO_SYS

static const std::vector<std::string> plate_keys = { "curr_bed_type", "skirt_start_angle", "first_layer_print_sequence", "first_layer_sequence_choice", "other_layers_print_sequence", "other_layers_sequence_choice", "print_sequence", "spiral_mode"};

void Tab::Highlighter::set_timer_owner(wxEvtHandler* owner, int timerid/* = wxID_ANY*/)
{
    m_timer.SetOwner(owner, timerid);
}

void Tab::Highlighter::init(std::pair<OG_CustomCtrl*, bool*> params)
{
    if (m_timer.IsRunning())
        invalidate();
    if (!params.first || !params.second)
        return;

    m_timer.Start(300, false);

    m_custom_ctrl = params.first;
    m_show_blink_ptr = params.second;

    *m_show_blink_ptr = true;
    m_custom_ctrl->Refresh();
}

void Tab::Highlighter::invalidate()
{
    m_timer.Stop();

    if (m_custom_ctrl && m_show_blink_ptr) {
        *m_show_blink_ptr = false;
        m_custom_ctrl->Refresh();
        m_show_blink_ptr = nullptr;
        m_custom_ctrl = nullptr;
    }

    m_blink_counter = 0;
}

void Tab::Highlighter::blink()
{
    if (m_custom_ctrl && m_show_blink_ptr) {
        *m_show_blink_ptr = !*m_show_blink_ptr;
        m_custom_ctrl->Refresh();
    }
    else
        return;

    if ((++m_blink_counter) == 11)
        invalidate();
}

//BBS: GUI refactor
Tab::Tab(ParamsPanel* parent, const wxString& title, Preset::Type type) :
    m_parent(parent), m_title(title), m_type(type)
{
    Create(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL/*, name*/);
    this->SetFont(Slic3r::GUI::wxGetApp().normal_font());

    wxGetApp().UpdateDarkUI(this);
    SetBackgroundColour(*wxWHITE);

    m_compatible_printers.type			= Preset::TYPE_PRINTER;
    m_compatible_printers.key_list		= "compatible_printers";
    m_compatible_printers.key_condition	= "compatible_printers_condition";
    //m_compatible_printers.dialog_title  = _L("Compatible printers");
    //m_compatible_printers.dialog_label  = _L("Select the printers this profile is compatible with.");

    m_compatible_prints.type			= Preset::TYPE_PRINT;
    m_compatible_prints.key_list 		= "compatible_prints";
    m_compatible_prints.key_condition	= "compatible_prints_condition";
    //m_compatible_prints.dialog_title 	= _L("Compatible print profiles");
    //m_compatible_prints.dialog_label 	= _L("Select the print profiles this profile is compatible with.");

    wxGetApp().tabs_list.push_back(this);

    m_em_unit = em_unit(m_parent); //wxGetApp().em_unit();

    m_config_manipulation = get_config_manipulation();

    Bind(wxEVT_SIZE, ([](wxSizeEvent &evt) {
        //for (auto page : m_pages)
        //    if (! page.get()->IsShown())
        //        page->layout_valid = false;
        evt.Skip();
    }));

    m_highlighter.set_timer_owner(this, 0);
    this->Bind(wxEVT_TIMER, [this](wxTimerEvent&)
    {
        m_highlighter.blink();
    });
}

void Tab::set_type()
{
    if (m_name == PRESET_PRINT_NAME)              { m_type = Slic3r::Preset::TYPE_PRINT; }
    else if (m_name == "sla_print")     { m_type = Slic3r::Preset::TYPE_SLA_PRINT; }
    else if (m_name == PRESET_FILAMENT_NAME)      { m_type = Slic3r::Preset::TYPE_FILAMENT; }
    else if (m_name == "sla_material")  { m_type = Slic3r::Preset::TYPE_SLA_MATERIAL; }
    else if (m_name == PRESET_PRINTER_NAME)       { m_type = Slic3r::Preset::TYPE_PRINTER; }
    else                                { m_type = Slic3r::Preset::TYPE_INVALID; assert(false); }
}

// sub new
//BBS: GUI refactor, change tab to fit into ParamsPanel
void Tab::create_preset_tab()
{
//move to ParamsPanel
/*#ifdef __WINDOWS__
    SetDoubleBuffered(true);
#endif //__WINDOWS__*/
    auto panel = this;

    m_preset_bundle = wxGetApp().preset_bundle;

    // Vertical sizer to hold the choice menu and the rest of the page.
/*#ifdef __WXOSX__
    auto  *main_sizer = new wxBoxSizer(wxVERTICAL);
    main_sizer->SetSizeHints(this);
    this->SetSizer(main_sizer);

    // Create additional panel to Fit() it from OnActivate()
    // It's needed for tooltip showing on OSX
    m_tmp_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBK_LEFT | wxTAB_TRAVERSAL);
    auto panel = m_tmp_panel;
    auto  sizer = new wxBoxSizer(wxVERTICAL);
    m_tmp_panel->SetSizer(sizer);
    m_tmp_panel->Layout();

    main_sizer->Add(m_tmp_panel, 1, wxEXPAND | wxALL, 0);
#else
    Tab *panel = this;
    auto  *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->SetSizeHints(panel);
    panel->SetSizer(sizer);
#endif //__WXOSX__*/

    // BBS: model config
    if (m_type < Preset::TYPE_COUNT) {
        // preset chooser
        m_presets_choice = new TabPresetComboBox(panel, m_type);
        // m_presets_choice->SetFont(Label::Body_10); // BBS
        m_presets_choice->set_selection_changed_function([this](int selection) {
            if (!m_presets_choice->selection_is_changed_according_to_physical_printers())
            {
                if (m_type == Preset::TYPE_PRINTER && !m_presets_choice->is_selected_physical_printer())
                    m_preset_bundle->physical_printers.unselect_printer();

                // select preset
                std::string preset_name = m_presets_choice->GetString(selection).ToUTF8().data();
                select_preset(Preset::remove_suffix_modified(preset_name));
            }
        });
    }

    auto color = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);

    //buttons
    m_scaled_buttons.reserve(6);
    m_scaled_bitmaps.reserve(4);

    m_top_panel = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    // BBS: open this tab by select first
    m_top_panel->SetBackgroundColour(*wxWHITE);
    m_top_panel->Bind(wxEVT_LEFT_UP, [this](auto & e) {
        restore_last_select_item();
    });

    //add_scaled_button(panel, &m_btn_compare_preset, "compare");
    add_scaled_button(m_top_panel, &m_btn_save_preset, "save");
    add_scaled_button(m_top_panel, &m_btn_delete_preset, "cross");
    //if (m_type == Preset::Type::TYPE_PRINTER)
    //    add_scaled_button(panel, &m_btn_edit_ph_printer, "cog");

    m_show_incompatible_presets = false;
    add_scaled_bitmap(this, m_bmp_show_incompatible_presets, "flag_red");
    add_scaled_bitmap(this, m_bmp_hide_incompatible_presets, "flag_green");

    //add_scaled_button(panel, &m_btn_hide_incompatible_presets, m_bmp_hide_incompatible_presets.name());

    //m_btn_compare_preset->SetToolTip(_L("Compare presets"));
    // TRN "Save current Settings"
    m_btn_save_preset->SetToolTip(wxString::Format(_L("Save current %s"), m_title));
    m_btn_delete_preset->SetToolTip(_(L("Delete this preset")));
    m_btn_delete_preset->Hide();

    /*add_scaled_button(panel, &m_question_btn, "question");
    m_question_btn->SetToolTip(_(L("Hover the cursor over buttons to find more information \n"
                                   "or click this button.")));

    add_scaled_button(panel, &m_search_btn, "search");
    m_search_btn->SetToolTip(format_wxstr(_L("Search in settings [%1%]"), _L("Ctrl+") + "F"));*/

    // Bitmaps to be shown on the "Revert to system" aka "Lock to system" button next to each input field.
    add_scaled_bitmap(this, m_bmp_value_lock  , "unlock_normal");
    add_scaled_bitmap(this, m_bmp_value_unlock, "lock_normal");
    m_bmp_non_system = &m_bmp_white_bullet;
    // Bitmaps to be shown on the "Undo user changes" button next to each input field.
    add_scaled_bitmap(this, m_bmp_value_revert, "undo");
    add_scaled_bitmap(this, m_bmp_white_bullet, "dot");
    // Bitmap to be shown on the "edit" button before to each editable input field.
    add_scaled_bitmap(this, m_bmp_edit_value, "edit");

    set_tooltips_text();

    add_scaled_button(m_top_panel, &m_undo_btn,        m_bmp_white_bullet.name());
    //add_scaled_button(m_top_panel, &m_undo_to_sys_btn, m_bmp_white_bullet.name());
    add_scaled_button(m_top_panel, &m_btn_search,      "search");
    m_btn_search->SetToolTip(_L("Search in preset"));

    //search input
    m_search_item = new StaticBox(m_top_panel);
    StateColor box_colour(std::pair<wxColour, int>(*wxWHITE, StateColor::Normal));
    StateColor box_border_colour(std::pair<wxColour, int>(wxColour("#009688"), StateColor::Normal)); // ORCA match border color with other input/combo boxes

    m_search_item->SetBackgroundColor(box_colour);
    m_search_item->SetBorderColor(box_border_colour);
    m_search_item->SetCornerRadius(5);


    //StateColor::darkModeColorFor(wxColour(238, 238, 238)), wxDefaultPosition, wxSize(m_top_panel->GetSize().GetWidth(), 3 * wxGetApp().em_unit()), 8);
    auto search_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_search_input = new TextInput(m_search_item, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0 | wxBORDER_NONE);
    m_search_input->SetBackgroundColour(wxColour(238, 238, 238));
    m_search_input->SetForegroundColour(wxColour(43, 52, 54));
    m_search_input->SetFont(wxGetApp().bold_font());
    m_search_input->SetIcon(*BitmapCache().load_svg("search", FromDIP(16), FromDIP(16)));
    m_search_input->GetTextCtrl()->SetHint(_L("Search in preset") + dots);
    search_sizer->Add(new wxWindow(m_search_item, wxID_ANY, wxDefaultPosition, wxSize(0, 0)), 0, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(2));
    search_sizer->Add(m_search_input, 1, wxEXPAND | wxALL, FromDIP(2));
    //bbl for linux
    //search_sizer->Add(new wxWindow(m_search_input, wxID_ANY, wxDefaultPosition, wxSize(0, 0)), 0, wxEXPAND | wxLEFT, 16);


     m_search_item->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        m_search_input->SetFocus();
    });

    m_search_input->Bind(wxCUSTOMEVT_EXIT_SEARCH, [this](wxCommandEvent &) {
         Freeze();
        if (m_presets_choice) m_presets_choice->Show();

        m_btn_save_preset->Show();
        m_btn_delete_preset->Show(); // ORCA: fixes delete preset button visible while search box focused
        m_undo_btn->Show();          // ORCA: fixes revert preset button visible while search box focused
        m_btn_search->Show();
        m_search_item->Hide();

        m_search_item->Refresh();
        m_search_item->Update();
        m_search_item->Layout();

        this->GetParent()->Refresh();
        this->GetParent()->Update();
        this->GetParent()->Layout();
        Thaw();
    });

    m_search_item->SetSizer(search_sizer);
    m_search_item->Layout();
    search_sizer->Fit(m_search_item);

    m_search_item->Hide();
    //m_btn_search->SetId(wxID_FIND_PROCESS);

    m_btn_search->Bind(
        wxEVT_BUTTON,
        [this](wxCommandEvent &) {
         Freeze();
         if (m_presets_choice)
             m_presets_choice->Hide();

         m_btn_save_preset->Hide();
         m_btn_delete_preset->Hide(); // ORCA: fixes delete preset button visible while search box focused
         m_undo_btn->Hide();          // ORCA: fixes revert preset button visible while search box focused
         m_btn_search->Hide();
         m_search_item->Show();

         this->GetParent()->Refresh();
         this->GetParent()->Update();
         this->GetParent()->Layout();

         wxGetApp().plater()->search(false, m_type, m_top_panel->GetParent(), m_search_input, m_btn_search);
         Thaw();

        });

    m_undo_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent) { on_roll_back_value(); }));
    //m_undo_to_sys_btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent) { on_roll_back_value(true); }));
    /* m_search_btn->Bind(wxEVT_BUTTON, [](wxCommandEvent) { wxGetApp().plater()->search(false); });*/

    // Colors for ui "decoration"
    m_sys_label_clr			= wxGetApp().get_label_clr_sys();
    m_modified_label_clr	= wxGetApp().get_label_clr_modified();
    m_default_text_clr      = wxGetApp().get_label_clr_default();

    m_main_sizer = new wxBoxSizer( wxVERTICAL );
    m_top_sizer = new wxBoxSizer( wxHORIZONTAL );

    m_top_sizer->Add(m_undo_btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(SidebarProps::ContentMargin()));
    // BBS: model config
    if (m_presets_choice) {
        m_presets_choice->Reparent(m_top_panel);
        m_top_sizer->Add(m_presets_choice, 1, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(SidebarProps::ElementSpacing()));
    } else {
        m_top_sizer->AddSpacer(FromDIP(SidebarProps::ElementSpacing()));
        m_top_sizer->AddStretchSpacer(1);
    }

    const float scale_factor = /*wxGetApp().*/em_unit(this)*0.1;// GetContentScaleFactor();
#ifndef DISABLE_UNDO_SYS
    m_top_sizer->Add(m_undo_to_sys_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_top_sizer->AddSpacer(FromDIP(SidebarProps::IconSpacing()));
#endif
    m_top_sizer->Add(m_btn_save_preset, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    m_top_sizer->Add(m_btn_delete_preset, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    m_top_sizer->Add(m_btn_search, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
    m_top_sizer->Add(m_search_item, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::ContentMargin()));

    if (dynamic_cast<TabPrint*>(this) == nullptr) {
        m_static_title = new Label(m_top_panel, Label::Body_12, _L("Advance"));
        m_static_title->Wrap( -1 );
        // BBS: open this tab by select first
        m_static_title->Bind(wxEVT_LEFT_UP, [this](auto& e) {
            restore_last_select_item();
        });
        m_top_sizer->Add(m_static_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(SidebarProps::IconSpacing()));
        m_mode_view = new SwitchButton(m_top_panel, wxID_ABOUT);
        m_top_sizer->AddSpacer(FromDIP(SidebarProps::ElementSpacing()));
        m_top_sizer->Add( m_mode_view, 0, wxALIGN_CENTER_VERTICAL);
    }

    m_top_sizer->AddSpacer(FromDIP(SidebarProps::ContentMargin()));

    m_top_sizer->SetMinSize(-1, 3 * m_em_unit);
    m_top_panel->SetSizer(m_top_sizer);
    if (m_presets_choice)
        m_main_sizer->Add(m_top_panel, 0, wxEXPAND | wxUP | wxDOWN, m_em_unit);
    else
        m_top_panel->Hide();

#if 0
#ifdef _MSW_DARK_MODE
    // Sizer with buttons for mode changing
    if (wxGetApp().tabs_as_menu())
#endif
        m_mode_sizer = new ModeSizer(panel, int (0.5*em_unit(this)));

    const float scale_factor = /*wxGetApp().*/em_unit(this)*0.1;// GetContentScaleFactor();
    m_hsizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_hsizer, 0, wxEXPAND | wxBOTTOM, 3);
    m_hsizer->Add(m_presets_choice, 0, wxLEFT | wxRIGHT | wxTOP | wxALIGN_CENTER_VERTICAL, 3);
    m_hsizer->AddSpacer(int(4*scale_factor));
    m_hsizer->Add(m_btn_save_preset, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(4 * scale_factor));
    m_hsizer->Add(m_btn_delete_preset, 0, wxALIGN_CENTER_VERTICAL);
    if (m_btn_edit_ph_printer) {
        m_hsizer->AddSpacer(int(4 * scale_factor));
        m_hsizer->Add(m_btn_edit_ph_printer, 0, wxALIGN_CENTER_VERTICAL);
    }
    m_hsizer->AddSpacer(int(/*16*/8 * scale_factor));
    m_hsizer->Add(m_btn_hide_incompatible_presets, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(8 * scale_factor));
    m_hsizer->Add(m_question_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(32 * scale_factor));
    m_hsizer->Add(m_undo_to_sys_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->Add(m_undo_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(32 * scale_factor));
    m_hsizer->Add(m_search_btn, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(8*scale_factor));
    m_hsizer->Add(m_btn_compare_preset, 0, wxALIGN_CENTER_VERTICAL);
    m_hsizer->AddSpacer(int(16*scale_factor));
    // m_hsizer->AddStretchSpacer(32);
    // StretchSpacer has a strange behavior under OSX, so
    // There is used just additional sizer for m_mode_sizer with right alignment
    if (m_mode_sizer) {
        auto mode_sizer = new wxBoxSizer(wxVERTICAL);
        // Don't set the 2nd parameter to 1, making the sizer rubbery scalable in Y axis may lead
        // to wrong vertical size assigned to wxBitmapComboBoxes, see GH issue #7176.
        mode_sizer->Add(m_mode_sizer, 0, wxALIGN_RIGHT);
        m_hsizer->Add(mode_sizer, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, wxOSX ? 15 : 10);
    }

    //Horizontal sizer to hold the tree and the selected page.
    m_hsizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_hsizer, 1, wxEXPAND, 0);

    //left vertical sizer
    m_left_sizer = new wxBoxSizer(wxVERTICAL);
    m_hsizer->Add(m_left_sizer, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, 3);
#endif
    // tree
    m_tabctrl = new TabCtrl(panel, wxID_ANY, wxDefaultPosition, wxSize(20 * m_em_unit, -1),
        wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES | wxBORDER_NONE | wxWANTS_CHARS | wxTR_FULL_ROW_HIGHLIGHT);
    m_tabctrl->Bind(wxEVT_RIGHT_DOWN, [this](auto &e) {}); // disable right select
    m_tabctrl->SetFont(Label::Body_14);
    //m_left_sizer->Add(m_tabctrl, 1, wxEXPAND);
    const int img_sz = int(32 * scale_factor + 0.5f);
    m_icons = new wxImageList(img_sz, img_sz, false, 1);
    // Index of the last icon inserted into $self->{icons}.
    m_icon_count = -1;
    m_tabctrl->AssignImageList(m_icons);
    wxGetApp().UpdateDarkUI(m_tabctrl);

    // Delay processing of the following handler until the message queue is flushed.
    // This helps to process all the cursor key events on Windows in the tree control,
    // so that the cursor jumps to the last item.
    // BBS: bold selection
    m_tabctrl->Bind(wxEVT_TAB_SEL_CHANGING, [this](wxCommandEvent& event) {
        const auto sel_item = m_tabctrl->GetSelection();
        m_tabctrl->SetItemBold(sel_item, false);
        });
    m_tabctrl->Bind(wxEVT_TAB_SEL_CHANGED, [this](wxCommandEvent& event) {
#ifdef __linux__
        // Events queue is opposite On Linux. wxEVT_SET_FOCUS invokes after wxEVT_TAB_SEL_CHANGED,
        // and a result wxEVT_KILL_FOCUS doesn't invoke for the TextCtrls.
        // So, call SetFocus explicitly for this control before changing of the selection
        m_tabctrl->SetFocus();
#endif
            if (!m_disable_tree_sel_changed_event && !m_pages.empty()) {
                if (m_page_switch_running)
                    m_page_switch_planned = true;
                else {
                    m_page_switch_running = true;
                    do {
                        m_page_switch_planned = false;
                        m_tabctrl->Update();
                    } while (this->tree_sel_change_delayed(event));
                    m_page_switch_running = false;
                }
            }
        });

    m_tabctrl->Bind(wxEVT_KEY_DOWN, &Tab::OnKeyDown, this);

    m_main_sizer->Add(m_tabctrl, 1, wxEXPAND | wxALL, 0 );

    this->SetSizer(m_main_sizer);
    //this->Layout();
    m_page_view = m_parent->get_paged_view();

    // Initialize the page.
/*#ifdef __WXOSX__
    auto page_parent = m_tmp_panel;
#else
    auto page_parent = this;
#endif

    m_page_view = new wxScrolledWindow(page_parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL);
    m_page_sizer = new wxBoxSizer(wxVERTICAL);
    m_page_view->SetSizer(m_page_sizer);
    m_page_view->SetScrollbars(1, 20, 1, 2);
    m_hsizer->Add(m_page_view, 1, wxEXPAND | wxLEFT, 5);*/

    //m_btn_compare_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { compare_preset(); }));
    m_btn_save_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { save_preset(); }));
    m_btn_delete_preset->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) { delete_preset(); }));
    /*m_btn_hide_incompatible_presets->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) {
        toggle_show_hide_incompatible();
    }));

    if (m_btn_edit_ph_printer)
        m_btn_edit_ph_printer->Bind(wxEVT_BUTTON, [this](wxCommandEvent e) {
            if (m_preset_bundle->physical_printers.has_selection())
                m_presets_choice->edit_physical_printer();
            else
                m_presets_choice->add_physical_printer();
        });*/

    // Initialize the DynamicPrintConfig by default keys/values.
    build();

    // ys_FIXME: Following should not be needed, the function will be called later
    // (update_mode->update_visibility->rebuild_page_tree). This does not work, during the
    // second call of rebuild_page_tree m_tabctrl->GetFirstVisibleItem(); returns zero
    // for some unknown reason (and the page is not refreshed until user does a selection).
    rebuild_page_tree();

    m_completed = true;
}

void Tab::add_scaled_button(wxWindow* parent,
                            ScalableButton** btn,
                            const std::string& icon_name,
                            const wxString& label/* = wxEmptyString*/,
                            long style /*= wxBU_EXACTFIT | wxNO_BORDER*/)
{
    *btn = new ScalableButton(parent, wxID_ANY, icon_name, label, wxDefaultSize, wxDefaultPosition, style, true);
    (*btn)->SetBackgroundColour(parent->GetBackgroundColour());
    m_scaled_buttons.push_back(*btn);
}

void Tab::add_scaled_bitmap(wxWindow* parent,
                            ScalableBitmap& bmp,
                            const std::string& icon_name)
{
    bmp = ScalableBitmap(parent, icon_name);
    m_scaled_bitmaps.push_back(&bmp);
}

void Tab::load_initial_data()
{
    m_config = &m_presets->get_edited_preset().config;
    bool has_parent = m_presets->get_selected_preset_parent() != nullptr;
    m_bmp_non_system = has_parent ? &m_bmp_value_unlock : &m_bmp_white_bullet;
    m_ttg_non_system = has_parent ? &m_ttg_value_unlock : &m_ttg_white_bullet_ns;
    m_tt_non_system  = has_parent ? &m_tt_value_unlock  : &m_ttg_white_bullet_ns;
}

Slic3r::GUI::PageShp Tab::add_options_page(const wxString& title, const std::string& icon, bool is_extruder_pages /*= false*/)
{
    // Index of icon in an icon list $self->{icons}.
    auto icon_idx = 0;
    if (!icon.empty()) {
        icon_idx = (m_icon_index.find(icon) == m_icon_index.end()) ? -1 : m_icon_index.at(icon);
        if (icon_idx == -1) {
            // Add a new icon to the icon list.
            m_scaled_icons_list.push_back(ScalableBitmap(this, icon, 32, false, true));
            //m_icons->Add(m_scaled_icons_list.back().bmp());
            icon_idx = ++m_icon_count;
            m_icon_index[icon] = icon_idx;
        }

        if (m_category_icon.find(title) == m_category_icon.end()) {
            // Add new category to the category_to_icon list.
            m_category_icon[title] = icon;
        }
    }
    // Initialize the page.
    //BBS: GUI refactor
    PageShp page = std::make_shared<Page>(m_page_view, title, icon_idx, this);
//	page->SetBackgroundStyle(wxBG_STYLE_SYSTEM);
#ifdef __WINDOWS__
//	page->SetDoubleBuffered(true);
#endif //__WINDOWS__

    if (dynamic_cast<TabPrint*>(this)) {
        page->m_split_multi_line = true;
        page->m_option_label_at_right = true;
    }

    if (!is_extruder_pages)
        m_pages.push_back(page);

    page->set_config(m_config);
    return page;
}

// Names of categories is save in English always. We translate them only for UI.
// But category "Extruder n" can't be translated regularly (using _()), so
// just for this category we should splite the title and translate "Extruder" word separately
wxString Tab::translate_category(const wxString& title, Preset::Type preset_type)
{
    if (preset_type == Preset::TYPE_PRINTER && title.Contains("Extruder ")) {
        return _("Extruder") + title.SubString(8, title.Last());
    }
    return _(title);
}

void Tab::OnActivate()
{
    //BBS: GUI refactor
    //noUpdates seems not working
    //wxWindowUpdateLocker noUpdates(this);
/*#ifdef __WXOSX__
//    wxWindowUpdateLocker noUpdates(this);
    auto size = GetSizer()->GetSize();
    m_tmp_panel->GetSizer()->SetMinSize(size.x + m_size_move, size.y);
    Fit();
    m_size_move *= -1;
#endif // __WXOSX__*/

#ifdef __WXMSW__
    // Workaround for tooltips over Tree Controls displayed over excessively long
    // tree control items, stealing the window focus.
    //
    // In case the Tab was reparented from the MainFrame to the floating dialog,
    // the tooltip created by the Tree Control before reparenting is not reparented,
    // but it still points to the MainFrame. If the tooltip pops up, the MainFrame
    // is incorrectly focussed, stealing focus from the floating dialog.
    //
    // The workaround is to delete the tooltip control.
    // Vojtech tried to reparent the tooltip control, but it did not work,
    // and if the Tab was later reparented back to MainFrame, the tooltip was displayed
    // at an incorrect position, therefore it is safer to just discard the tooltip control
    // altogether.
    HWND hwnd_tt = TreeView_GetToolTips(m_tabctrl->GetHandle());
    if (hwnd_tt) {
	    HWND hwnd_toplevel 	= find_toplevel_parent(m_tabctrl)->GetHandle();
	    HWND hwnd_parent 	= ::GetParent(hwnd_tt);
	    if (hwnd_parent != hwnd_toplevel) {
	    	::DestroyWindow(hwnd_tt);
			TreeView_SetToolTips(m_tabctrl->GetHandle(), nullptr);
	    }
    }
#endif

    // BBS: select on first active
    if (!m_active_page)
        restore_last_select_item();

    //BBS: GUI refactor
    m_page_view->Freeze();

    // create controls on active page
    activate_selected_page([](){});
    //BBS: GUI refactor
    //m_main_sizer->Layout();
    m_parent->Layout();

#ifdef _MSW_DARK_MODE
    // Because of DarkMode we use our own Notebook (inherited from wxSiplebook) instead of wxNotebook
    // And it looks like first Layout of the page doesn't update a size of the m_presets_choice
    // So we have to set correct size explicitely
   /* if (wxSize ok_sz = wxSize(35 * m_em_unit, m_presets_choice->GetBestSize().y);
        ok_sz != m_presets_choice->GetSize()) {
        m_presets_choice->SetMinSize(ok_sz);
        m_presets_choice->SetSize(ok_sz);
        GetSizer()->GetItem(size_t(0))->GetSizer()->Layout();
        if (wxGetApp().tabs_as_menu())
            m_presets_choice->update();
    }*/
#endif // _MSW_DARK_MODE
    Refresh();

    //BBS: GUI refactor
    m_page_view->Thaw();
}

void Tab::update_label_colours()
{
    m_default_text_clr = wxGetApp().get_label_clr_default();
    if (m_sys_label_clr == wxGetApp().get_label_clr_sys() && m_modified_label_clr == wxGetApp().get_label_clr_modified())
        return;
    m_sys_label_clr = wxGetApp().get_label_clr_sys();
    m_modified_label_clr = wxGetApp().get_label_clr_modified();

    //update options "decoration"
    for (const auto& opt : m_options_list)
    {
        const wxColour *color = &m_sys_label_clr;

        // value isn't equal to system value
        if ((opt.second & osSystemValue) == 0) {
            // value is equal to last saved
            if ((opt.second & osInitValue) != 0)
                color = &m_default_text_clr;
            // value is modified
            else
                color = &m_modified_label_clr;
        }
        if (opt.first == "printable_area"            ||
            opt.first == "compatible_prints"    || opt.first == "compatible_printers"           ) {
            if (Line* line = get_line(opt.first))
                line->set_label_colour(color);
            continue;
        }

        Field* field = get_field(opt.first);
        if (field == nullptr) continue;
        field->set_label_colour(color);
    }

    auto cur_item = m_tabctrl->GetFirstVisibleItem();
    if (cur_item < 0 || !m_tabctrl->IsVisible(cur_item))
        return;
    while (cur_item >= 0) {
        auto title = m_tabctrl->GetItemText(cur_item);
        for (auto page : m_pages)
        {
            if (translate_category(page->title(), m_type) != title)
                continue;

            const wxColor *clr = !page->m_is_nonsys_values ? &m_sys_label_clr :
                page->m_is_modified_values ? &m_modified_label_clr :
                (m_type < Preset::TYPE_COUNT ? &m_default_text_clr : &m_modified_label_clr);

            m_tabctrl->SetItemTextColour(cur_item, clr == &m_modified_label_clr ? *clr : StateColor(
                        std::make_pair(0x6B6B6C, (int) StateColor::NotChecked),
                        std::make_pair(*clr, (int) StateColor::Normal)));
            break;
        }
        cur_item = m_tabctrl->GetNextVisible(cur_item);
    }

    decorate();
}

void Tab::decorate()
{
    for (const auto& opt : m_options_list)
    {
        Field*      field = nullptr;
        bool        option_without_field = false;

        if (opt.first == "printable_area" ||
            opt.first == "compatible_prints" || opt.first == "compatible_printers")
            option_without_field = true;

        if (!option_without_field) {
            field = get_field(opt.first);
            if (!field)
                continue;
        }

        bool is_nonsys_value = false;
        bool is_modified_value = true;
        const ScalableBitmap* sys_icon  = &m_bmp_value_lock;
        const ScalableBitmap* icon      = &m_bmp_value_revert;

        const wxColour* color = m_is_default_preset ? &m_default_text_clr : &m_sys_label_clr;

        const wxString* sys_tt  = &m_tt_value_lock;
        const wxString* tt      = &m_tt_value_revert;

        // value isn't equal to system value
        if ((opt.second & osSystemValue) == 0) {
            is_nonsys_value = true;
            sys_icon = m_bmp_non_system;
            sys_tt = m_tt_non_system;
            // value is equal to last saved
            if ((opt.second & osInitValue) != 0)
                color = &m_default_text_clr;
            // value is modified
            else
                color = &m_modified_label_clr;
        }
        if ((opt.second & osInitValue) != 0)
        {
            is_modified_value = false;
            icon = &m_bmp_white_bullet;
            tt = &m_tt_white_bullet;
        }

        if (option_without_field) {
            if (Line* line = get_line(opt.first)) {
                line->set_undo_bitmap(icon);
                line->set_undo_to_sys_bitmap(sys_icon);
                line->set_undo_tooltip(tt);
                line->set_undo_to_sys_tooltip(sys_tt);
                line->set_label_colour(color);
            }
            continue;
        }

        field->m_is_nonsys_value = is_nonsys_value;
        field->m_is_modified_value = is_modified_value;
        field->set_undo_bitmap(icon);
        //BBS: GUI refactor
        field->set_undo_to_sys_bitmap(sys_icon);
        field->set_undo_tooltip(tt);
        field->set_undo_to_sys_tooltip(sys_tt);
        field->set_label_colour(color);

        if (field->has_edit_ui())
            field->set_edit_bitmap(&m_bmp_edit_value);

    }

    if (m_active_page)
        m_active_page->refresh();
}

// Update UI according to changes
void Tab::update_changed_ui()
{
    if (m_postpone_update_ui)
        return;

    const bool deep_compare = (m_type == Slic3r::Preset::TYPE_PRINTER || m_type == Slic3r::Preset::TYPE_SLA_MATERIAL);
    auto dirty_options = m_presets->current_dirty_options(deep_compare);
    auto nonsys_options = m_presets->current_different_from_parent_options(deep_compare);
    if (m_type == Preset::TYPE_PRINTER && static_cast<TabPrinter*>(this)->m_printer_technology == ptFFF) {
        TabPrinter* tab = static_cast<TabPrinter*>(this);
        if (tab->m_initial_extruders_count != tab->m_extruders_count)
            dirty_options.emplace_back("extruders_count");
        if (tab->m_sys_extruders_count != tab->m_extruders_count)
            nonsys_options.emplace_back("extruders_count");
    }

    for (auto& it : m_options_list)
        it.second = m_opt_status_value;

    for (auto opt_key : dirty_options)	m_options_list[opt_key] &= ~osInitValue;
    for (auto opt_key : nonsys_options)	m_options_list[opt_key] &= ~osSystemValue;

    update_custom_dirty();

    decorate();

    wxTheApp->CallAfter([this]() {
        if (parent()) //To avoid a crash, parent should be exist for a moment of a tree updating
            update_changed_tree_ui();
    });
    // BBS:
    update_undo_buttons();
}

void Tab::init_options_list()
{
    if (!m_options_list.empty())
        m_options_list.clear();

    for (const std::string& opt_key : m_config->keys())
        m_options_list.emplace(opt_key, m_opt_status_value);
}

template<class T>
void add_correct_opts_to_options_list(const std::string &opt_key, std::map<std::string, int>& map, Tab *tab, const int& value)
{
    T *opt_cur = static_cast<T*>(tab->m_config->option(opt_key));
    for (size_t i = 0; i < opt_cur->values.size(); i++)
        map.emplace(opt_key + "#" + std::to_string(i), value);
}

void TabPrinter::init_options_list()
{
    if (!m_options_list.empty())
        m_options_list.clear();

    for (const std::string& opt_key : m_config->keys())
    {
        if (opt_key == "printable_area" || opt_key == "bed_exclude_area" || opt_key == "thumbnails") {
            m_options_list.emplace(opt_key, m_opt_status_value);
            continue;
        }
        switch (m_config->option(opt_key)->type())
        {
        case coInts:	add_correct_opts_to_options_list<ConfigOptionInts		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coBools:	add_correct_opts_to_options_list<ConfigOptionBools		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coFloats:	add_correct_opts_to_options_list<ConfigOptionFloats		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coStrings:	add_correct_opts_to_options_list<ConfigOptionStrings	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coPercents:add_correct_opts_to_options_list<ConfigOptionPercents	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coPoints:	add_correct_opts_to_options_list<ConfigOptionPoints		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        // BBS
        case coEnums:   add_correct_opts_to_options_list<ConfigOptionInts       >(opt_key, m_options_list, this, m_opt_status_value);   break;
        default:		m_options_list.emplace(opt_key, m_opt_status_value);		break;
        }
    }
    if (m_printer_technology == ptFFF)
        m_options_list.emplace("extruders_count", m_opt_status_value);
}

void TabPrinter::msw_rescale()
{
    Tab::msw_rescale();

    if (m_reset_to_filament_color)
        m_reset_to_filament_color->msw_rescale();

    //BBS: GUI refactor
    //Layout();
    m_parent->Layout();
}

void TabSLAMaterial::init_options_list()
{
    if (!m_options_list.empty())
        m_options_list.clear();

    for (const std::string& opt_key : m_config->keys())
    {
        if (opt_key == "compatible_prints" || opt_key == "compatible_printers") {
            m_options_list.emplace(opt_key, m_opt_status_value);
            continue;
        }
        switch (m_config->option(opt_key)->type())
        {
        case coInts:	add_correct_opts_to_options_list<ConfigOptionInts		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coBools:	add_correct_opts_to_options_list<ConfigOptionBools		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coFloats:	add_correct_opts_to_options_list<ConfigOptionFloats		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coStrings:	add_correct_opts_to_options_list<ConfigOptionStrings	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coPercents:add_correct_opts_to_options_list<ConfigOptionPercents	>(opt_key, m_options_list, this, m_opt_status_value);	break;
        case coPoints:	add_correct_opts_to_options_list<ConfigOptionPoints		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        // BBS
        case coEnums:	add_correct_opts_to_options_list<ConfigOptionInts		>(opt_key, m_options_list, this, m_opt_status_value);	break;
        default:		m_options_list.emplace(opt_key, m_opt_status_value);		break;
        }
    }
}

void Tab::get_sys_and_mod_flags(const std::string& opt_key, bool& sys_page, bool& modified_page)
{
    auto opt = m_options_list.find(opt_key);
    if (opt == m_options_list.end())
        return;
    // If the value is empty, clear the system flag
    if (opt_key == "compatible_printers" || opt_key == "compatible_prints") {
        auto* compatible_values = m_config->option<ConfigOptionStrings>(opt_key);
        if (compatible_values && compatible_values->values.empty()) {
            sys_page = false; // Empty value should NOT be treated as a system value
        }
    } else if (sys_page) {
        sys_page = (opt->second & osSystemValue) != 0;
    }

    modified_page |= (opt->second & osInitValue) == 0;

    //if (sys_page) sys_page = (opt->second & osSystemValue) != 0;
    //modified_page |= (opt->second & osInitValue) == 0;
}

void Tab::update_changed_tree_ui()
{
    if (m_options_list.empty()) {
        if (m_type == Preset::Type::TYPE_PLATE) {
            for (auto page : m_pages) {
                page->m_is_nonsys_values = false;
            }
        }
        return;
    }
    auto cur_item = m_tabctrl->GetFirstVisibleItem();
    if (cur_item < 0 || !m_tabctrl->IsVisible(cur_item))
        return;

    auto selected_item = m_tabctrl->GetSelection();
    auto selection = selected_item >= 0 ? m_tabctrl->GetItemText(selected_item) : "";

    while (cur_item >= 0) {
        auto title = m_tabctrl->GetItemText(cur_item);
        for (auto page : m_pages)
        {
            if (translate_category(page->title(), m_type) != title)
                continue;
            bool sys_page = true;
            bool modified_page = false;
            if (page->title() == "General") {
                std::initializer_list<const char*> optional_keys{ "extruders_count", "printable_area" };
                for (auto &opt_key : optional_keys) {
                    get_sys_and_mod_flags(opt_key, sys_page, modified_page);
                }
            }
            if (page->title() == "Dependencies") {
                if (m_type == Slic3r::Preset::TYPE_PRINTER) {
                    sys_page = m_presets->get_selected_preset_parent() != nullptr;
                    modified_page = false;
                } else {
                    if (m_type == Slic3r::Preset::TYPE_FILAMENT || m_type == Slic3r::Preset::TYPE_SLA_MATERIAL)
                        get_sys_and_mod_flags("compatible_prints", sys_page, modified_page);
                    get_sys_and_mod_flags("compatible_printers", sys_page, modified_page);
                }
            }
            for (auto group : page->m_optgroups)
            {
                if (!sys_page && modified_page)
                    break;
                for (const auto &kvp : group->opt_map()) {
                    const std::string& opt_key = kvp.first;
                    get_sys_and_mod_flags(opt_key, sys_page, modified_page);
                }
            }

            const wxColor *clr = sys_page ? (m_is_default_preset ? &m_default_text_clr : &m_sys_label_clr) :
                                 (modified_page || m_type >= Preset::TYPE_COUNT) ? &m_modified_label_clr : &m_default_text_clr;

            if (page->set_item_colour(clr))
                m_tabctrl->SetItemTextColour(cur_item, clr == &m_modified_label_clr ? *clr : StateColor(
                        std::make_pair(0x6B6B6C, (int) StateColor::NotChecked),
                        std::make_pair(*clr, (int) StateColor::Normal)));

            page->m_is_nonsys_values = !sys_page;
            page->m_is_modified_values = modified_page;

            if (selection == title) {
                m_is_nonsys_values = page->m_is_nonsys_values;
                m_is_modified_values = page->m_is_modified_values;
            }
            break;
        }
        auto next_item = m_tabctrl->GetNextVisible(cur_item);
        cur_item = next_item;
    }
}

void Tab::update_undo_buttons()
{
    // BBS: restore all pages in preset
    m_undo_btn->        SetBitmap_(m_presets->get_edited_preset().is_dirty ? m_bmp_value_revert: m_bmp_white_bullet);
    //m_undo_to_sys_btn-> SetBitmap_(m_is_nonsys_values   ? *m_bmp_non_system : m_bmp_value_lock);

    m_undo_btn->SetToolTip(m_presets->get_edited_preset().is_dirty ? _L("Click to reset all settings to the last saved preset.") : m_ttg_white_bullet);
    //m_undo_to_sys_btn->SetToolTip(m_is_nonsys_values ? *m_ttg_non_system : m_ttg_value_lock);
}

void Tab::on_roll_back_value(const bool to_sys /*= true*/)
{
    // BBS: restore all pages in preset
    // if (!m_active_page) return;

    int os;
    if (to_sys)	{
        if (!m_is_nonsys_values) return;
        os = osSystemValue;
    }
    else {
        // BBS: restore all pages in preset
        if (!m_presets->get_edited_preset().is_dirty) return;
        os = osInitValue;
    }

    m_postpone_update_ui = true;

    // BBS: restore all preset
    for (auto page : m_pages)
    for (auto group : page->m_optgroups) {
        if (group->title == "Capabilities") {
            if ((m_options_list["extruders_count"] & os) == 0)
                to_sys ? group->back_to_sys_value("extruders_count") : group->back_to_initial_value("extruders_count");
        }
        if (group->title == "Size and coordinates") {
            if ((m_options_list["printable_area"] & os) == 0) {
                to_sys ? group->back_to_sys_value("printable_area") : group->back_to_initial_value("printable_area");
                load_key_value("printable_area", true/*some value*/, true);
            }
        }
        if (group->title == "Profile dependencies") {
            if (m_type != Preset::TYPE_PRINTER && (m_options_list["compatible_printers"] & os) == 0) {
                to_sys ? group->back_to_sys_value("compatible_printers") : group->back_to_initial_value("compatible_printers");
                load_key_value("compatible_printers", true/*some value*/, true);
            }
            if ((m_type == Preset::TYPE_FILAMENT || m_type == Preset::TYPE_SLA_MATERIAL) && (m_options_list["compatible_prints"] & os) == 0) {
                to_sys ? group->back_to_sys_value("compatible_prints") : group->back_to_initial_value("compatible_prints");
                load_key_value("compatible_prints", true/*some value*/, true);
            }
        }
        for (const auto &kvp : group->opt_map()) {
            const std::string& opt_key = kvp.first;
            if ((m_options_list[opt_key] & os) == 0)
                to_sys ? group->back_to_sys_value(opt_key) : group->back_to_initial_value(opt_key);
        }
    }

    // BBS: restore all pages in preset
    m_presets->discard_current_changes();

    m_postpone_update_ui = false;

    // When all values are rolled, then we have to update whole tab in respect to the reverted values
    update();
    if (m_active_page)
        m_active_page->update_visibility(m_mode, true);

    // BBS: restore all pages in preset, update_dirty also update combobox
    update_dirty();

    if (m_compatible_printers.checkbox) {
        bool is_empty = m_config->option<ConfigOptionStrings>("compatible_printers")->values.empty();
        m_compatible_printers.checkbox->SetValue(is_empty);
        is_empty ? m_compatible_printers.btn->Disable() : m_compatible_printers.btn->Enable();
    }
    if (m_compatible_prints.checkbox) {
        bool is_empty = m_config->option<ConfigOptionStrings>("compatible_prints")->values.empty();
        m_compatible_prints.checkbox->SetValue(is_empty);
        is_empty ? m_compatible_prints.btn->Disable() : m_compatible_prints.btn->Enable();
    }

    m_page_view->GetParent()->Layout();
}

// Update the combo box label of the selected preset based on its "dirty" state,
// comparing the selected preset config with $self->{config}.
void Tab::update_dirty()
{
    if (m_postpone_update_ui)
        return;

    if (m_presets_choice) {
        m_presets_choice->update_dirty();
        on_presets_changed();
    } else {
        m_presets->update_dirty();
    }
    update_changed_ui();
}

void Tab::update_tab_ui(bool update_plater_presets)
{
    if (m_presets_choice) {
        m_presets_choice->update();
        if (update_plater_presets)
            on_presets_changed();
    }
}

// Load a provied DynamicConfig into the tab, modifying the active preset.
// This could be used for example by setting a Wipe Tower position by interactive manipulation in the 3D view.
void Tab::load_config(const DynamicPrintConfig& config)
{
    bool modified = 0;
    for(auto opt_key : m_config->diff(config)) {
        m_config->set_key_value(opt_key, config.option(opt_key)->clone());
        modified = 1;
    }
    if (modified) {
        update_dirty();
        //# Initialize UI components with the config values.
        reload_config();
        update();
    }
}

// Reload current $self->{config} (aka $self->{presets}->edited_preset->config) into the UI fields.
void Tab::reload_config()
{
    if (m_active_page)
        m_active_page->reload_config();
}

void Tab::update_mode()
{
    m_mode = wxGetApp().get_mode();

    //BBS: GUI refactor
    // update mode for ModeSizer
    //if (m_mode_sizer)
    //    m_mode_sizer->SetMode(m_mode);

    update_visibility();

    update_changed_tree_ui();
}

void Tab::update_visibility()
{
    Freeze(); // There is needed Freeze/Thaw to avoid a flashing after Show/Layout

    for (auto page : m_pages)
        page->update_visibility(m_mode, page.get() == m_active_page);
    rebuild_page_tree();

    if (m_type == Preset::TYPE_SLA_PRINT)
        update_description_lines();

    //BBS: GUI refactor
    //Layout();
    m_parent->Layout();
    Thaw();
}

// NEOTKO_LIBRE_TAG_START — Less Used Toggle
// Reads "less_used_opt_<key>" from app_config and sets toggle_visible = false on
// matching Lines across all pages. Call at end of build() to initialize flags
// before pages are activated. The flags are applied by OG_CustomCtrl::update_visibility()
// when each page is first activated (init_ctrl_lines + update_visibility call chain).
// The OG_CustomCtrl right-click handler manages its own per-optgroup refresh at runtime.
void Tab::apply_less_used_visibility()
{
    auto* ac = wxGetApp().app_config;
    if (!ac) return;

    for (auto& page : m_pages) {
        for (auto& group : page->m_optgroups) {
            auto& lines = const_cast<std::vector<Line>&>(group->get_lines());
            for (auto& l : lines) {
                if (l.get_options().empty()) continue;
                const std::string& key = l.get_options()[0].opt_id;
                if (ac->get("less_used_opt_" + key) == "1")
                    l.toggle_visible = false;
            }
        }
    }
}
// NEOTKO_LIBRE_TAG_END

void Tab::msw_rescale()
{
    m_em_unit = em_unit(m_parent);

    m_top_sizer->SetMinSize(-1, 3 * m_em_unit);

    //BBS: GUI refactor
    //if (m_mode_sizer)
    //    m_mode_sizer->msw_rescale();
    if (m_presets_choice)
        m_presets_choice->msw_rescale();

    m_tabctrl->SetMinSize(wxSize(20 * m_em_unit, -1));

    // rescale buttons and cached bitmaps
    for (const auto btn : m_scaled_buttons)
        btn->msw_rescale();
    for (const auto bmp : m_scaled_bitmaps)
        bmp->msw_rescale();

    if (m_mode_view)
        m_mode_view->Rescale();

    if (m_detach_preset_btn)
        m_detach_preset_btn->msw_rescale();

    // rescale icons for tree_ctrl
    for (ScalableBitmap& bmp : m_scaled_icons_list)
        bmp.msw_rescale();
    // recreate and set new ImageList for tree_ctrl
    m_icons->RemoveAll();
    m_icons = new wxImageList(m_scaled_icons_list.front().bmp().GetWidth(), m_scaled_icons_list.front().bmp().GetHeight(), false);
    for (ScalableBitmap& bmp : m_scaled_icons_list)
        //m_icons->Add(bmp.bmp());
    m_tabctrl->AssignImageList(m_icons);

    // rescale options_groups
    if (m_active_page)
        m_active_page->msw_rescale();

    m_tabctrl->Rescale();

    //BBS: GUI refactor
    //Layout();
    m_parent->Layout();
}

void Tab::sys_color_changed()
{
    if (m_presets_choice)
        m_presets_choice->sys_color_changed();

    // update buttons and cached bitmaps
    for (const auto btn : m_scaled_buttons)
        btn->msw_rescale();
    for (const auto bmp : m_scaled_bitmaps)
        bmp->msw_rescale();
    if (m_detach_preset_btn)
        m_detach_preset_btn->msw_rescale();

    // update icons for tree_ctrl
    for (ScalableBitmap& bmp : m_scaled_icons_list)
        bmp.msw_rescale();
    // recreate and set new ImageList for tree_ctrl
    m_icons->RemoveAll();
    m_icons = new wxImageList(m_scaled_icons_list.front().bmp().GetWidth(), m_scaled_icons_list.front().bmp().GetHeight(), false);
    for (ScalableBitmap& bmp : m_scaled_icons_list)
        //m_icons->Add(bmp.bmp());
    m_tabctrl->AssignImageList(m_icons);

    // Colors for ui "decoration"
    update_label_colours();
#ifdef _WIN32
    wxWindowUpdateLocker noUpdates(this);
    //BBS: GUI refactor
    //if (m_mode_sizer)
    //    m_mode_sizer->msw_rescale();
    wxGetApp().UpdateDarkUI(this);
    wxGetApp().UpdateDarkUI(m_tabctrl);
#endif
    update_changed_tree_ui();

    // update options_groups
    if (m_active_page)
        m_active_page->sys_color_changed();

    //BBS: GUI refactor
    //Layout();
    m_parent->Layout();
}

Field* Tab::get_field(const t_config_option_key& opt_key, int opt_index/* = -1*/) const
{
    return m_active_page ? m_active_page->get_field(opt_key, opt_index) : nullptr;
}

Line* Tab::get_line(const t_config_option_key& opt_key)
{
    return m_active_page ? m_active_page->get_line(opt_key) : nullptr;
}

std::pair<OG_CustomCtrl*, bool*> Tab::get_custom_ctrl_with_blinking_ptr(const t_config_option_key& opt_key, int opt_index/* = -1*/)
{
    if (!m_active_page)
        return {nullptr, nullptr};

    std::pair<OG_CustomCtrl*, bool*> ret = {nullptr, nullptr};

    for (auto opt_group : m_active_page->m_optgroups) {
        ret = opt_group->get_custom_ctrl_with_blinking_ptr(opt_key, opt_index);
        if (ret.first && ret.second)
            break;
    }
    return ret;
}

Field* Tab::get_field(const t_config_option_key& opt_key, Page** selected_page, int opt_index/* = -1*/)
{
    Field* field = nullptr;
    for (auto page : m_pages) {
        field = page->get_field(opt_key, opt_index);
        if (field != nullptr) {
            *selected_page = page.get();
            return field;
        }
    }
    return field;
}

void Tab::toggle_option(const std::string& opt_key, bool toggle, int opt_index/* = -1*/)
{
    if (!m_active_page)
        return;
    Field* field = m_active_page->get_field(opt_key, opt_index);
    if (field)
        field->toggle(toggle);
}

void Tab::toggle_line(const std::string &opt_key, bool toggle)
{
    if (!m_active_page) return;
    Line *line = m_active_page->get_line(opt_key);
    if (line) line->toggle_visible = toggle;
};

// To be called by custom widgets, load a value into a config,
// update the preset selection boxes (the dirty flags)
// If value is saved before calling this function, put saved_value = true,
// and value can be some random value because in this case it will not been used
void Tab::load_key_value(const std::string& opt_key, const boost::any& value, bool saved_value /*= false*/)
{
    if (!saved_value) change_opt_value(*m_config, opt_key, value);
    // Mark the print & filament enabled if they are compatible with the currently selected preset.
    if (opt_key == "compatible_printers" || opt_key == "compatible_prints") {
        // Don't select another profile if this profile happens to become incompatible.
        m_preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
    }
    if (m_presets_choice)
        m_presets_choice->update_dirty();
    on_presets_changed();
    update();
}

static wxString support_combo_value_for_config(const DynamicPrintConfig &config, bool is_fff)
{
    const std::string support         = is_fff ? "enable_support"                 : "supports_enable";
    const std::string buildplate_only = is_fff ? "support_on_build_plate_only" : "support_buildplate_only";

    // BBS
#if 0
    return
        ! config.opt_bool(support) ?
            _("None") :
            (is_fff && !config.opt_bool("support_material_auto")) ?
                _("For support enforcers only") :
                (config.opt_bool(buildplate_only) ? _("Support on build plate only") :
                                                    _("Everywhere"));
#else
    if (config.opt_bool(support)) {
         return (config.opt_bool(buildplate_only) ? _("Support on build plate only") : _("Everywhere"));
    } else {
        return _("For support enforcers only");
    }
#endif
}

static wxString pad_combo_value_for_config(const DynamicPrintConfig &config)
{
    return config.opt_bool("pad_enable") ? (config.opt_bool("pad_around_object") ? _("Around object") : _("Below object")) : _("None");
}

void Tab::on_value_change(const std::string& opt_key, const boost::any& value)
{
    if (wxGetApp().plater() == nullptr) {
        return;
    }
    if (m_config == nullptr) {
        return;
    }

    if (opt_key == "compatible_prints")
        this->compatible_widget_reload(m_compatible_prints);
    if (opt_key == "compatible_printers")
        this->compatible_widget_reload(m_compatible_printers);

    const bool is_fff = supports_printer_technology(ptFFF);
    ConfigOptionsGroup* og_freq_chng_params = wxGetApp().sidebar().og_freq_chng_params(is_fff);
    //BBS: GUI refactor
    if (og_freq_chng_params) {
        if (opt_key == "sparse_infill_density" || opt_key == "pad_enable")
        {
            boost::any val = og_freq_chng_params->get_config_value(*m_config, opt_key);
            og_freq_chng_params->set_value(opt_key, val);
        }

        if (opt_key == "pad_around_object") {
            for (PageShp& pg : m_pages) {
                Field* fld = pg->get_field(opt_key); /// !!! ysFIXME ????
                if (fld) fld->set_value(value, false);
            }
        }

        if (is_fff ?
            (opt_key == "enable_support" || opt_key == "support_type" || opt_key == "support_on_build_plate_only") :
            (opt_key == "supports_enable" || opt_key == "support_buildplate_only"))
            og_freq_chng_params->set_value("support", support_combo_value_for_config(*m_config, is_fff));

        if (!is_fff && (opt_key == "pad_enable" || opt_key == "pad_around_object"))
            og_freq_chng_params->set_value("pad", pad_combo_value_for_config(*m_config));

        if (opt_key == "brim_width")
        {
            bool val = m_config->opt_float("brim_width") > 0.0 ? true : false;
            og_freq_chng_params->set_value("brim", val);
        }
    }

    if (opt_key == "single_extruder_multi_material" || opt_key == "extruders_count" )
        update_wiping_button_visibility();


    if (opt_key == "pellet_flow_coefficient")
    {
        double double_value = Preset::convert_pellet_flow_to_filament_diameter(boost::any_cast<double>(value));
        m_config->set_key_value("filament_diameter", new ConfigOptionFloats{double_value});
	}

    if (opt_key == "filament_diameter") {
        double double_value = Preset::convert_filament_diameter_to_pellet_flow(boost::any_cast<double>(value));
        m_config->set_key_value("pellet_flow_coefficient", new ConfigOptionFloats{double_value});
    }


    if (opt_key == "single_extruder_multi_material"  ){
        const auto bSEMM = m_config->opt_bool("single_extruder_multi_material");
        wxGetApp().sidebar().show_SEMM_buttons(bSEMM);
        wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
    }

    if(opt_key == "purge_in_prime_tower")
        wxGetApp().get_tab(Preset::TYPE_PRINT)->update();


    if (opt_key == "enable_prime_tower") {
        auto timelapse_type = m_config->option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
        bool timelapse_enabled = timelapse_type->value == TimelapseType::tlSmooth;
        if (!boost::any_cast<bool>(value) && timelapse_enabled) {
            MessageDialog dlg(wxGetApp().plater(), _L("A prime tower is required for smooth timelapse. There may be flaws on the model without prime tower. Are you sure you want to disable prime tower?"),
                              _L("Warning"), wxICON_WARNING | wxYES | wxNO);
            if (dlg.ShowModal() == wxID_NO) {
                DynamicPrintConfig new_conf = *m_config;
                new_conf.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
                m_config_manipulation.apply(m_config, &new_conf);
            }
            wxGetApp().plater()->update();
        }
        bool is_precise_z_height = m_config->option<ConfigOptionBool>("precise_z_height")->value;
        if (boost::any_cast<bool>(value) && is_precise_z_height) {
            MessageDialog dlg(wxGetApp().plater(), _L("Enabling both precise Z height and the prime tower may cause the size of prime tower to increase. Do you still want to enable?"),
                _L("Warning"), wxICON_WARNING | wxYES | wxNO);
            if (dlg.ShowModal() == wxID_NO) {
                DynamicPrintConfig new_conf = *m_config;
                new_conf.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
                m_config_manipulation.apply(m_config, &new_conf);
            }
            wxGetApp().plater()->update();
        }
        update_wiping_button_visibility();
    }

    if (opt_key == "dithering_local_z_mode") {
        const bool local_z_enabled = boost::any_cast<bool>(value);
        if (local_z_enabled &&
            (!m_config->has("mixed_filament_region_collapse") ||
             m_config->option("mixed_filament_region_collapse") == nullptr ||
             m_config->opt_bool("mixed_filament_region_collapse"))) {
            change_opt_value(*m_config, "mixed_filament_region_collapse", boost::any(false));
            if (m_type == Preset::TYPE_PRINT) {
                DynamicPrintConfig &project_cfg = wxGetApp().preset_bundle->project_config;
                project_cfg.set_key_value("mixed_filament_region_collapse", new ConfigOptionBool(false));
            }
            if (Field *field = this->get_field("mixed_filament_region_collapse"))
                field->set_value(boost::any(false), false);
            update_dirty();
        }
        if (!local_z_enabled &&
            m_config->has("dithering_local_z_whole_objects") &&
            m_config->option("dithering_local_z_whole_objects") != nullptr &&
            m_config->opt_bool("dithering_local_z_whole_objects")) {
            change_opt_value(*m_config, "dithering_local_z_whole_objects", boost::any(false));
            if (m_type == Preset::TYPE_PRINT) {
                DynamicPrintConfig &project_cfg = wxGetApp().preset_bundle->project_config;
                project_cfg.set_key_value("dithering_local_z_whole_objects", new ConfigOptionBool(false));
            }
            if (Field *field = this->get_field("dithering_local_z_whole_objects"))
                field->set_value(boost::any(false), false);
            update_dirty();
        }
        if (!local_z_enabled &&
            m_config->has("dithering_local_z_direct_multicolor") &&
            m_config->option("dithering_local_z_direct_multicolor") != nullptr &&
            m_config->opt_bool("dithering_local_z_direct_multicolor")) {
            change_opt_value(*m_config, "dithering_local_z_direct_multicolor", boost::any(false));
            if (m_type == Preset::TYPE_PRINT) {
                DynamicPrintConfig &project_cfg = wxGetApp().preset_bundle->project_config;
                project_cfg.set_key_value("dithering_local_z_direct_multicolor", new ConfigOptionBool(false));
            }
            if (Field *field = this->get_field("dithering_local_z_direct_multicolor"))
                field->set_value(boost::any(false), false);
            update_dirty();
        }
    }


    if (opt_key == "single_extruder_multi_material"  ){
        const auto bSEMM = m_config->opt_bool("single_extruder_multi_material");
        wxGetApp().sidebar().show_SEMM_buttons(bSEMM);
        wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
    }

    if(opt_key == "purge_in_prime_tower")
        wxGetApp().get_tab(Preset::TYPE_PRINT)->update();


    if (opt_key == "enable_prime_tower") {
        auto timelapse_type = m_config->option<ConfigOptionEnum<TimelapseType>>("timelapse_type");
        bool timelapse_enabled = timelapse_type->value == TimelapseType::tlSmooth;
        if (!boost::any_cast<bool>(value) && timelapse_enabled) {
            MessageDialog dlg(wxGetApp().plater(), _L("A prime tower is required for smooth timelapse. There may be flaws on the model without prime tower. Are you sure you want to disable prime tower?"),
                              _L("Warning"), wxICON_WARNING | wxYES | wxNO);
            if (dlg.ShowModal() == wxID_NO) {
                DynamicPrintConfig new_conf = *m_config;
                new_conf.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
                m_config_manipulation.apply(m_config, &new_conf);
            }
            wxGetApp().plater()->update();
        }
        update_wiping_button_visibility();
    }

    // reload scene to update timelapse wipe tower
    if (opt_key == "timelapse_type") {
        bool wipe_tower_enabled = m_config->option<ConfigOptionBool>("enable_prime_tower")->value;
        if (!wipe_tower_enabled && boost::any_cast<int>(value) == (int)TimelapseType::tlSmooth) {
            MessageDialog dlg(wxGetApp().plater(), _L("A prime tower is required for smooth timelapse. There may be flaws on the model without prime tower. Do you want to enable prime tower?"),
                              _L("Warning"), wxICON_WARNING | wxYES | wxNO);
            if (dlg.ShowModal() == wxID_YES) {
                DynamicPrintConfig new_conf = *m_config;
                new_conf.set_key_value("enable_prime_tower", new ConfigOptionBool(true));
                m_config_manipulation.apply(m_config, &new_conf);
                wxGetApp().plater()->update();
            }
        } else {
            wxGetApp().plater()->update();
        }
    }

    if (opt_key == "print_sequence" && m_config->opt_enum<PrintSequence>("print_sequence") == PrintSequence::ByObject) {
        auto printer_structure_opt = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionEnum<PrinterStructure>>("printer_structure");
        if (printer_structure_opt && printer_structure_opt->value == PrinterStructure::psI3) {
            wxString msg_text = _(L("Timelapse is not supported because Print sequence is set to \"By object\"."));
            msg_text += "\n\n" + _(L("Still print by object?"));

            MessageDialog dialog(wxGetApp().plater(), msg_text, "", wxICON_WARNING | wxYES | wxNO);
            auto          answer = dialog.ShowModal();
            if (answer == wxID_NO) {
                DynamicPrintConfig new_conf = *m_config;
                new_conf.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(PrintSequence::ByLayer));
                m_config_manipulation.apply(m_config, &new_conf);
                wxGetApp().plater()->update();
            }
        }
    }

    // BBS: Add warning notification for Snapmaker U1 + Print by Object
    if (opt_key == "print_sequence") {
        PrintSequence print_seq = m_config->opt_enum<PrintSequence>("print_sequence");

        if (print_seq == PrintSequence::ByObject) {
            // Get current printer model
            auto printer_model_opt = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionString>("printer_model");

            if (printer_model_opt && !printer_model_opt->value.empty()) {
                std::string printer_model = printer_model_opt->value;

                // Check if this is Snapmaker U1 printer
                bool is_snapmaker_u1 = boost::icontains(printer_model, "Snapmaker") &&
                                       boost::icontains(printer_model, "U1");

                if (is_snapmaker_u1) {
                    // Show red warning notification
                    if (wxGetApp().plater() && wxGetApp().plater()->get_notification_manager()) {
                        wxString warning_text = _L("Printing by object with caution. This function may cause the print head to collide with printed parts during switching.");
                        wxGetApp().plater()->get_notification_manager()->push_plater_error_notification(warning_text.ToStdString());
                    }
                }
            }
        } else {
            // Clear warning when switching away from ByObject
            if (wxGetApp().plater() && wxGetApp().plater()->get_notification_manager()) {
                wxString warning_text = _L("Printing by object with caution. This function may cause the print head to collide with printed parts during switching.");
                wxGetApp().plater()->get_notification_manager()->close_plater_error_notification(warning_text.ToStdString());
            }
        }
    }

    // BBS set support style to default when support type changes
    // Orca: do this only in simple mode
    if (opt_key == "support_type" && m_mode == comSimple) {
        DynamicPrintConfig new_conf = *m_config;
        new_conf.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(smsDefault));
        m_config_manipulation.apply(m_config, &new_conf);
    }

    // BBS popup a message to ask the user to set optimum parameters for support interface if support materials are used
    if (opt_key == "support_interface_filament") {
        int interface_filament_id = m_config->opt_int("support_interface_filament") - 1; // the displayed id is based from 1, while internal id is based from 0
        if (is_support_filament(interface_filament_id) && !(m_config->opt_float("support_top_z_distance") == 0 && m_config->opt_float("support_interface_spacing") == 0 &&
                                                            m_config->opt_enum<SupportMaterialInterfacePattern>("support_interface_pattern") == SupportMaterialInterfacePattern::smipRectilinearInterlaced)) {
            wxString msg_text = _L("When using support material for the support interface, we recommend the following settings:\n"
                                   "0 top Z distance, 0 interface spacing, interlaced rectilinear pattern and disable independent support layer height");
            msg_text += "\n\n" + _L("Change these settings automatically?\n"
                                    "Yes - Change these settings automatically\n"
                                    "No  - Do not change these settings for me");
            MessageDialog      dialog(wxGetApp().plater(), msg_text, "Suggestion", wxICON_WARNING | wxYES | wxNO);
            DynamicPrintConfig new_conf = *m_config;
            if (dialog.ShowModal() == wxID_YES) {
                new_conf.set_key_value("support_top_z_distance", new ConfigOptionFloat(0));
                new_conf.set_key_value("support_interface_spacing", new ConfigOptionFloat(0));
                new_conf.set_key_value("support_interface_pattern", new ConfigOptionEnum<SupportMaterialInterfacePattern>(SupportMaterialInterfacePattern::smipRectilinearInterlaced));
                new_conf.set_key_value("independent_support_layer_height", new ConfigOptionBool(false));
                m_config_manipulation.apply(m_config, &new_conf);
            }
            wxGetApp().plater()->update();
        }
    }

    if(opt_key == "make_overhang_printable"){
        if(m_config->opt_bool("make_overhang_printable")){
            wxString msg_text = _(
                L("Enabling this option will modify the model's shape. If your print requires precise dimensions or is part of an "
                  "assembly, it's important to double-check whether this change in geometry impacts the functionality of your print."));
            msg_text += "\n\n" + _(L("Are you sure you want to enable this option?"));
            MessageDialog dialog(wxGetApp().plater(), msg_text, "", wxICON_WARNING | wxYES | wxNO);
            dialog.SetButtonLabel(wxID_YES, _L("Enable"));
            dialog.SetButtonLabel(wxID_NO, _L("Cancel"));
            if (dialog.ShowModal() == wxID_NO) {
                DynamicPrintConfig new_conf = *m_config;
                new_conf.set_key_value("make_overhang_printable", new ConfigOptionBool(false));
                m_config_manipulation.apply(m_config, &new_conf);
                wxGetApp().plater()->update();
            }
        }
    }

    if (opt_key == "sparse_infill_rotate_template") {
        // Orca: show warning dialog if rotate template for solid infill if not support
        const auto _sparse_infill_pattern = m_config->option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value;
        bool       is_safe_to_rotate      = _sparse_infill_pattern == ipRectilinear || _sparse_infill_pattern == ipLine ||
                                 _sparse_infill_pattern == ipZigZag || _sparse_infill_pattern == ipCrossZag ||
                                 _sparse_infill_pattern == ipLockedZag;
        
        auto new_value = boost::any_cast<std::string>(value);
        is_safe_to_rotate = is_safe_to_rotate || new_value.empty();

        if (!is_safe_to_rotate) {
            wxString msg_text = _(
                L("Infill patterns are typically designed to handle rotation automatically to ensure proper printing and achieve their "
                  "intended effects (e.g., Gyroid, Cubic). Rotating the current sparse infill pattern may lead to insufficient support. "
                  "Please proceed with caution and thoroughly check for any potential printing issues."
                  "Are you sure you want to enable this option?"));
            msg_text += "\n\n" + _(L("Are you sure you want to enable this option?"));
            MessageDialog dialog(wxGetApp().plater(), msg_text, "", wxICON_WARNING | wxYES | wxNO);
            dialog.SetButtonLabel(wxID_YES, _L("Enable"));
            dialog.SetButtonLabel(wxID_NO, _L("Cancel"));
            if (dialog.ShowModal() == wxID_NO) {
                DynamicPrintConfig new_conf = *m_config;
                new_conf.set_key_value("sparse_infill_rotate_template", new ConfigOptionString(""));
                m_config_manipulation.apply(m_config, &new_conf);
                wxGetApp().plater()->update();
            }
        }
    }

    if(opt_key=="layer_height"){
        auto min_layer_height_from_nozzle=wxGetApp().preset_bundle->full_config().option<ConfigOptionFloats>("min_layer_height")->values;
        auto max_layer_height_from_nozzle=wxGetApp().preset_bundle->full_config().option<ConfigOptionFloats>("max_layer_height")->values;
        auto layer_height_floor = *std::min_element(min_layer_height_from_nozzle.begin(), min_layer_height_from_nozzle.end());
        auto layer_height_ceil  = *std::max_element(max_layer_height_from_nozzle.begin(), max_layer_height_from_nozzle.end());
        const auto lh = m_config->opt_float("layer_height");
        bool exceed_minimum_flag = lh < layer_height_floor;
        bool exceed_maximum_flag = lh > layer_height_ceil;

        if (exceed_maximum_flag || exceed_minimum_flag) {
            if (lh < EPSILON) {
                auto          msg_text = _(L("Layer height is too small.\nIt will set to min_layer_height\n"));
                MessageDialog dialog(wxGetApp().plater(), msg_text, "", wxICON_WARNING | wxOK);
                dialog.SetButtonLabel(wxID_OK, _L("OK"));
                dialog.ShowModal();
                auto new_conf = *m_config;
                new_conf.set_key_value("layer_height", new ConfigOptionFloat(layer_height_floor));
                m_config_manipulation.apply(m_config, &new_conf);
            } else {
                wxString msg_text = _(L("Layer height exceeds the limit in Printer Settings -> Extruder -> Layer height limits, "
                                        "this may cause printing quality issues."));
                msg_text += "\n\n" + _(L("Adjust to the set range automatically?\n"));
                MessageDialog dialog(wxGetApp().plater(), msg_text, "", wxICON_WARNING | wxYES | wxNO);
                dialog.SetButtonLabel(wxID_YES, _L("Adjust"));
                dialog.SetButtonLabel(wxID_NO, _L("Ignore"));
                auto answer   = dialog.ShowModal();
                auto new_conf = *m_config;
                if (answer == wxID_YES) {
                    if (exceed_maximum_flag)
                        new_conf.set_key_value("layer_height", new ConfigOptionFloat(layer_height_ceil));
                    if (exceed_minimum_flag)
                        new_conf.set_key_value("layer_height", new ConfigOptionFloat(layer_height_floor));
                    m_config_manipulation.apply(m_config, &new_conf);
                }
            }
            wxGetApp().plater()->update();
        }
    }


    // -1 means caculate all
    auto update_flush_volume = [](int idx = -1) {
        if (idx < 0) {
            size_t filament_size = wxGetApp().plater()->get_extruder_colors_from_plater_config(nullptr, false).size();
            for (size_t i = 0; i < filament_size; ++i)
                wxGetApp().plater()->sidebar().auto_calc_flushing_volumes(i);
        }
        else
            wxGetApp().plater()->sidebar().auto_calc_flushing_volumes(idx);
        };


    string opt_key_without_idx = opt_key.substr(0, opt_key.find('#'));

    if (opt_key_without_idx == "long_retractions_when_cut") {
        unsigned char activate = boost::any_cast<unsigned char>(value);
        if (activate == 1) {
            MessageDialog dialog(wxGetApp().plater(),
                _L("Experimental feature: Retracting and cutting off the filament at a greater distance during filament changes to minimize flush. "
                    "Although it can notably reduce flush, it may also elevate the risk of nozzle clogs or other printing complications."), "", wxICON_WARNING | wxOK);
            dialog.ShowModal();
        }
    }

    if (opt_key == "filament_long_retractions_when_cut"){
        unsigned char activate = boost::any_cast<unsigned char>(value);
        if (activate == 1) {
            MessageDialog dialog(wxGetApp().plater(),
                _L("Experimental feature: Retracting and cutting off the filament at a greater distance during filament changes to minimize flush. "
                   "Although it can notably reduce flush, it may also elevate the risk of nozzle clogs or other printing complications. "
                   "Please use with the latest printer firmware."), "", wxICON_WARNING | wxOK);
            dialog.ShowModal();
        }
    }


    //Orca: sync filament num if it's a multi tool printer
    if (opt_key == "extruders_count" && !m_config->opt_bool("single_extruder_multi_material")){
        auto num_extruder = boost::any_cast<size_t>(value);
        int         old_filament_size = wxGetApp().preset_bundle->filament_presets.size();
        std::vector<std::string> new_colors;
        for (int i = old_filament_size; i < num_extruder; ++i) {
            wxColour    new_col   = Plater::get_next_color_for_filament();
            std::string new_color = new_col.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
            new_colors.push_back(new_color);
        }
        if (wxGetApp().preset_bundle->printers.get_edited_preset().name.find("Snapmaker U1") != std::string::npos) {
            if (old_filament_size > num_extruder) {
                num_extruder = old_filament_size;
                new_colors.clear();
                for (int i = 0; i < old_filament_size; ++i) {
                    wxColour    new_col   = Plater::get_next_color_for_filament();
                    std::string new_color = new_col.GetAsString(wxC2S_HTML_SYNTAX).ToStdString();
                    new_colors.push_back(new_color);
                }
            }
        } 
        wxGetApp().preset_bundle->set_num_filaments(num_extruder, new_colors);
        wxGetApp().plater()->on_filaments_change(num_extruder);
        wxGetApp().get_tab(Preset::TYPE_PRINT)->update();
        wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
    }

    //Orca: disable purge_in_prime_tower if single_extruder_multi_material is disabled
    if (opt_key == "single_extruder_multi_material" && m_config->opt_bool("single_extruder_multi_material") == false){
        DynamicPrintConfig new_conf = *m_config;
        new_conf.set_key_value("purge_in_prime_tower", new ConfigOptionBool(false));
        m_config_manipulation.apply(m_config, &new_conf);
    }

    if (m_postpone_update_ui) {
        // It means that not all values are rolled to the system/last saved values jet.
        // And call of the update() can causes a redundant check of the config values,
        return;
    }

    const bool refresh_mixed_filament_panel =
        m_type == Preset::TYPE_PRINT && opt_key == "mixed_filament_component_bias_enabled";

    // Keep Mixed Filaments global settings in sync with project_config. In
    // full_fff_config(), project_config is applied last and would otherwise
    // override the edited print preset value from the Others panel.
    if (m_type == Preset::TYPE_PRINT &&
        (opt_key == "mixed_filament_gradient_mode" ||
         opt_key == "mixed_filament_height_lower_bound" ||
         opt_key == "mixed_filament_height_upper_bound" ||
         opt_key == "mixed_color_layer_height_a" ||
         opt_key == "mixed_color_layer_height_b" ||
         opt_key == "mixed_filament_advanced_dithering" ||
         opt_key == "mixed_filament_pointillism_pixel_size" ||
         opt_key == "mixed_filament_pointillism_line_gap" ||
         opt_key == "mixed_filament_component_bias_enabled" ||
         opt_key == "mixed_filament_surface_indentation" ||
         opt_key == "mixed_filament_region_collapse" ||
         opt_key == "dithering_z_step_size" ||
         opt_key == "dithering_local_z_mode" ||
         opt_key == "dithering_local_z_whole_objects" ||
         opt_key == "dithering_local_z_direct_multicolor" ||
         opt_key == "dithering_step_painted_zones_only" ||
         opt_key == "mixed_filament_definitions")) {
        DynamicPrintConfig &project_cfg = wxGetApp().preset_bundle->project_config;
        if (const ConfigOption *opt = m_config->option(opt_key))
            project_cfg.set_key_value(opt_key, opt->clone());
    }

    update();
    if(m_active_page)
        m_active_page->update_visibility(m_mode, true);
    m_page_view->GetParent()->Layout();

    if (refresh_mixed_filament_panel && wxGetApp().plater() != nullptr)
        wxGetApp().sidebar().update_mixed_filament_panel(false);
}

void Tab::show_timelapse_warning_dialog() {
    if (!m_is_timelapse_wipe_tower_already_prompted) {
        wxString      msg_text = _(L("When recording timelapse without toolhead, it is recommended to add a \"Timelapse Wipe Tower\" \n"
                                "by right-click the empty position of build plate and choose \"Add Primitive\"->\"Timelapse Wipe Tower\"."));
        msg_text += "\n";
        MessageDialog dialog(nullptr, msg_text, "", wxICON_WARNING | wxOK);
        dialog.ShowModal();
        m_is_timelapse_wipe_tower_already_prompted = true;
    }
}

// Show/hide the 'purging volumes' button
void Tab::update_wiping_button_visibility() {
    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptSLA)
        return; // ys_FIXME
    // Orca: it's not used
    //
    // bool wipe_tower_enabled = dynamic_cast<ConfigOptionBool*>(  (m_preset_bundle->prints.get_edited_preset().config  ).option("enable_prime_tower"))->value;
    // bool multiple_extruders = dynamic_cast<ConfigOptionFloats*>((m_preset_bundle->printers.get_edited_preset().config).option("nozzle_diameter"))->values.size() > 1;
    // auto wiping_dialog_button = wxGetApp().sidebar().get_wiping_dialog_button();
    // if (wiping_dialog_button) {
    //     wiping_dialog_button->Show(wipe_tower_enabled && multiple_extruders);
    //     wiping_dialog_button->GetParent()->Layout();
    // }

}

void Tab::activate_option(const std::string& opt_key, const wxString& category)
{
    wxString page_title = translate_category(category, m_type);

    auto cur_item = m_tabctrl->GetFirstVisibleItem();
    if (cur_item < 0)
        return;

    // We should to activate a tab with searched option, if it doesn't.
    // And do it before finding of the cur_item to avoid a case when Tab isn't activated jet and all treeItems are invisible
    //BBS: GUI refactor
    //wxGetApp().mainframe->select_tab(this);
    wxGetApp().mainframe->select_tab((wxPanel*)m_parent);

    while (cur_item >= 0) {
        if (page_title.empty()) {
            bool has = false;
            for (auto &g : m_pages[cur_item]->m_optgroups) {
                for (auto &l : g->get_lines()) {
                    for (auto &o : l.get_options()) { if (o.opt.opt_key == opt_key) { has = true; break; } }
                    if (has) break;
                }
                if (has) break;
            }
            if (!has) {
                cur_item = m_tabctrl->GetNextVisible(cur_item);
                continue;
            }
        } else {
            auto title = m_tabctrl->GetItemText(cur_item);
            if (page_title != title) {
                cur_item = m_tabctrl->GetNextVisible(cur_item);
                continue;
            }
        }

        m_tabctrl->SelectItem(cur_item);
        break;
    }

    auto set_focus = [](wxWindow* win) {
        win->SetFocus();
#ifdef WIN32
        if (wxTextCtrl* text = dynamic_cast<wxTextCtrl*>(win))
            text->SetSelection(-1, -1);
        else if (wxSpinCtrl* spin = dynamic_cast<wxSpinCtrl*>(win))
            spin->SetSelection(-1, -1);
#endif // WIN32
    };

    Field* field = get_field(opt_key);

    // focused selected field
    if (field) {
        set_focus(field->getWindow());
        if (!field->getWindow()->HasFocus()) {
            wxScrollEvent evt(wxEVT_SCROLL_CHANGED);
            evt.SetEventObject(field->getWindow());
            wxPostEvent(m_page_view, evt);
        }
    }
    else if (category == "Single extruder MM setup") {
       // When we show and hide "Single extruder MM setup" page,
       // related options are still in the search list
       // So, let's hightlighte a "single_extruder_multi_material" option,
       // as a "way" to show hidden page again
       field = get_field("single_extruder_multi_material");
       if (field)
           set_focus(field->getWindow());
    }

    m_highlighter.init(get_custom_ctrl_with_blinking_ptr(opt_key));
}

void Tab::apply_searcher()
{
    wxGetApp().sidebar().get_searcher().apply(m_config, m_type, m_mode);
}

void Tab::cache_config_diff(const std::vector<std::string>& selected_options, const DynamicPrintConfig* config/* = nullptr*/)
{
    m_cache_config.apply_only(config ? *config : m_presets->get_edited_preset().config, selected_options);
}

void Tab::apply_config_from_cache()
{
    bool was_applied = false;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<boost::format(": enter");
    // check and apply extruders count for printer preset
    if (m_type == Preset::TYPE_PRINTER)
        was_applied = static_cast<TabPrinter*>(this)->apply_extruder_cnt_from_cache();

    if (!m_cache_config.empty()) {
        // Apply to edited preset (官方版本的简单实现)
        m_presets->get_edited_preset().config.apply(m_cache_config);
        m_cache_config.clear();
        was_applied = true;
    }

    if (was_applied) {
        update_dirty();
        // 标记为 dirty 以保留修改
        m_presets->get_edited_preset().is_dirty = true;
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<boost::format(": exit, was_applied=%1%")%was_applied;
}

// Call a callback to update the selection of presets on the plater:
// To update the content of the selection boxes,
// to update the filament colors of the selection boxes,
// to update the "dirty" flags of the selection boxes,
// to update number of "filament" selection boxes when the number of extruders change.
void Tab::on_presets_changed()
{
    if (wxGetApp().plater() == nullptr)
        return;

    // Instead of PostEvent (EVT_TAB_PRESETS_CHANGED) just call update_presets
    wxGetApp().plater()->sidebar().update_presets(m_type);

    bool is_bbl_vendor_preset = wxGetApp().preset_bundle->is_bbl_vendor();
    if (is_bbl_vendor_preset) {
        wxGetApp().plater()->get_partplate_list().set_render_option(true, true);
        if (wxGetApp().preset_bundle->printers.get_edited_preset().has_cali_lines(wxGetApp().preset_bundle)) {
            wxGetApp().plater()->get_partplate_list().set_render_cali(true);
        } else {
            wxGetApp().plater()->get_partplate_list().set_render_cali(false);
        }
    } else {
        wxGetApp().plater()->get_partplate_list().set_render_option(false, true);
        wxGetApp().plater()->get_partplate_list().set_render_cali(false);
    }

    // Printer selected at the Printer tab, update "compatible" marks at the print and filament selectors.
    for (auto t: m_dependent_tabs)
    {
        Tab* tab = wxGetApp().get_tab(t);
        // If the printer tells us that the print or filament/sla_material preset has been switched or invalidated,
        // refresh the print or filament/sla_material tab page.
        // But if there are options, moved from the previously selected preset, update them to edited preset
        tab->apply_config_from_cache();
        tab->load_current_preset();
    }
    // clear m_dependent_tabs after first update from select_preset()
    // to avoid needless preset loading from update() function
    m_dependent_tabs.clear();

    wxGetApp().plater()->update_project_dirty_from_presets();
}

void Tab::build_preset_description_line(ConfigOptionsGroup* optgroup)
{
    auto description_line = [this](wxWindow* parent) {
        return description_line_widget(parent, &m_parent_preset_description_line);
    };

    auto detach_preset_btn = [this](wxWindow* parent) {
        m_detach_preset_btn = new ScalableButton(parent, wxID_ANY, "lock_normal", "",
                                                 wxDefaultSize, wxDefaultPosition, wxBU_LEFT | wxBU_EXACTFIT, true);
        ScalableButton* btn = m_detach_preset_btn;
        btn->SetFont(Slic3r::GUI::wxGetApp().normal_font());

        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(btn);

        btn->Bind(wxEVT_BUTTON, [this, parent](wxCommandEvent&)
        {
        	bool system = m_presets->get_edited_preset().is_system;
        	bool dirty  = m_presets->get_edited_preset().is_dirty;
            wxString msg_text = system ?
            	_(L("A copy of the current system preset will be created, which will be detached from the system preset.")) :
                _(L("The current custom preset will be detached from the parent system preset."));
            if (dirty) {
	            msg_text += "\n\n";
            	msg_text += _(L("Modifications to the current profile will be saved."));
            }
            msg_text += "\n\n";
            msg_text += _(L("This action is not revertible.\nDo you want to proceed?"));

            //wxMessageDialog dialog(parent, msg_text, _(L("Detach preset")), wxICON_WARNING | wxYES_NO | wxCANCEL);
            MessageDialog dialog(parent, msg_text, _(L("Detach preset")), wxICON_WARNING | wxYES_NO | wxCANCEL);
            if (dialog.ShowModal() == wxID_YES)
                save_preset(m_presets->get_edited_preset().is_system ? std::string() : m_presets->get_edited_preset().name, true);
        });

        btn->Hide();

        return sizer;
    };

    Line line = Line{ "", "" };
    line.full_width = 1;

    line.append_widget(description_line);
    line.append_widget(detach_preset_btn);
    optgroup->append_line(line);
}

void Tab::update_preset_description_line()
{
    const Preset* parent = m_presets->get_selected_preset_parent();
    const Preset& preset = m_presets->get_edited_preset();

    wxString description_line;

    if (preset.is_default) {
        description_line = _(L("This is a default preset."));
    } else if (preset.is_system) {
        description_line = _(L("This is a system preset."));
    } else if (parent == nullptr) {
        description_line = _(L("Current preset is inherited from the default preset."));
    } else {
        std::string name = parent->name;
        boost::replace_all(name, "&", "&&");
        description_line = _(L("Current preset is inherited from")) + ":\n\t" + from_u8(name);
    }

    if (preset.is_default || preset.is_system)
        description_line += "\n\t" + _(L("It can't be deleted or modified.")) + "\n\t" +
                            _(L("Any modifications should be saved as a new preset inherited from this one.")) + "\n\t" +
                            _(L("To do that please specify a new name for the preset."));

    if (parent && parent->vendor) {
        description_line += "\n\n" + _(L("Additional information:")) + "\n";
        description_line += "\t" + _(L("vendor")) + ": " + (m_type == Slic3r::Preset::TYPE_PRINTER ? "\n\t\t" : "") + parent->vendor->name +
                            ", ver: " + parent->vendor->config_version.to_string();
        if (m_type == Slic3r::Preset::TYPE_PRINTER) {
            const std::string& printer_model = preset.config.opt_string("printer_model");
            if (!printer_model.empty())
                description_line += "\n\n\t" + _(L("printer model")) + ": \n\t\t" + printer_model;
            switch (preset.printer_technology()) {
            case ptFFF: {
                // FIXME add prefered_sla_material_profile for SLA
                const std::string&              default_print_profile = preset.config.opt_string("default_print_profile");
                const std::vector<std::string>& default_filament_profiles =
                    preset.config.option<ConfigOptionStrings>("default_filament_profile")->values;
                if (!default_print_profile.empty())
                    description_line += "\n\n\t" + _(L("default print profile")) + ": \n\t\t" + default_print_profile;
                if (!default_filament_profiles.empty()) {
                    description_line += "\n\n\t" + _(L("default filament profile")) + ": \n\t\t";
                    for (auto& profile : default_filament_profiles) {
                        if (&profile != &*default_filament_profiles.begin())
                            description_line += ", ";
                        description_line += profile;
                    }
                }
                break;
            }
            case ptSLA: {
                // FIXME add prefered_sla_material_profile for SLA
                const std::string& default_sla_material_profile = preset.config.opt_string("default_sla_material_profile");
                if (!default_sla_material_profile.empty())
                    description_line += "\n\n\t" + _(L("default SLA material profile")) + ": \n\t\t" + default_sla_material_profile;

                const std::string& default_sla_print_profile = preset.config.opt_string("default_sla_print_profile");
                if (!default_sla_print_profile.empty())
                    description_line += "\n\n\t" + _(L("default SLA print profile")) + ": \n\t\t" + default_sla_print_profile;
                break;
            }
            default: break;
            }
        } else if (!preset.alias.empty()) {
            description_line += "\n\n\t" + _(L("full profile name")) + ": \n\t\t" + preset.name;
            description_line += "\n\t" + _(L("symbolic profile name")) + ": \n\t\t" + preset.alias;
        }
    }

    m_parent_preset_description_line->SetText(description_line, false);

    if (m_detach_preset_btn)
        m_detach_preset_btn->Show(parent && parent->is_system && !preset.is_default);
    // BBS: GUI refactor
    // Layout();
    m_parent->Layout();
}

void Tab::update_frequently_changed_parameters()
{
    const bool is_fff = supports_printer_technology(ptFFF);
    auto og_freq_chng_params = wxGetApp().sidebar().og_freq_chng_params(is_fff);
    if (!og_freq_chng_params) return;

    og_freq_chng_params->set_value("support", support_combo_value_for_config(*m_config, is_fff));
    if (! is_fff)
        og_freq_chng_params->set_value("pad", pad_combo_value_for_config(*m_config));

    const std::string updated_value_key = is_fff ? "sparse_infill_density" : "pad_enable";

    const boost::any val = og_freq_chng_params->get_config_value(*m_config, updated_value_key);
    og_freq_chng_params->set_value(updated_value_key, val);

    if (is_fff)
    {
        og_freq_chng_params->set_value("brim", bool(m_config->opt_float("brim_width") > 0.0));
        update_wiping_button_visibility();
    }
}

//BBS: BBS new parameter list
void TabPrint::build()
{
    if (m_presets == nullptr)
        m_presets = &m_preset_bundle->prints;
    load_initial_data();

    auto page = add_options_page(L("Quality"), "custom-gcode_quality"); // ORCA: icon only visible on placeholders
        auto optgroup = page->new_optgroup(L("Layer height"), L"param_layer_height");
        optgroup->append_single_option_line("layer_height","quality_settings_layer_height");
        optgroup->append_single_option_line("initial_layer_print_height","quality_settings_layer_height");

        optgroup = page->new_optgroup(L("Line width"), L"param_line_width");
        optgroup->append_single_option_line("line_width","quality_settings_line_width");
        optgroup->append_single_option_line("initial_layer_line_width","quality_settings_line_width");
        optgroup->append_single_option_line("outer_wall_line_width","quality_settings_line_width");
        optgroup->append_single_option_line("inner_wall_line_width","quality_settings_line_width");
        optgroup->append_single_option_line("top_surface_line_width","quality_settings_line_width");
        optgroup->append_single_option_line("sparse_infill_line_width","quality_settings_line_width");
        optgroup->append_single_option_line("internal_solid_infill_line_width","quality_settings_line_width");
        optgroup->append_single_option_line("support_line_width","quality_settings_line_width");

        optgroup = page->new_optgroup(L("Seam"), L"param_seam");
        optgroup->append_single_option_line("seam_position", "quality_settings_seam#seam-position");
        optgroup->append_single_option_line("staggered_inner_seams", "quality_settings_seam#staggered-inner-seams");
        optgroup->append_single_option_line("seam_gap","quality_settings_seam#seam-gap");
        optgroup->append_single_option_line("seam_slope_type", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("seam_slope_conditional", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("scarf_angle_threshold", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("scarf_overhang_threshold", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("scarf_joint_speed", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("seam_slope_start_height", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("seam_slope_entire_loop", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("seam_slope_min_length", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("seam_slope_steps", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("scarf_joint_flow_ratio", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("seam_slope_inner_walls", "quality_settings_seam#scarf-joint-seam");
        optgroup->append_single_option_line("role_based_wipe_speed","quality_settings_seam#role-based-wipe-speed");
        optgroup->append_single_option_line("wipe_speed", "quality_settings_seam#wipe-speed");
        optgroup->append_single_option_line("wipe_on_loops","quality_settings_seam#wipe-on-loop-inward-movement");
        optgroup->append_single_option_line("wipe_before_external_loop","quality_settings_seam#wipe-before-external");


        optgroup = page->new_optgroup(L("Precision"), L"param_precision");
        optgroup->append_single_option_line("slice_closing_radius", "quality_settings_precision#slice-gap-closing-radius");
        optgroup->append_single_option_line("resolution", "quality_settings_precision#resolution");
        optgroup->append_single_option_line("enable_arc_fitting", "quality_settings_precision#arc-fitting");
        optgroup->append_single_option_line("xy_hole_compensation", "quality_settings_precision#x-y-compensation");
        optgroup->append_single_option_line("xy_contour_compensation", "quality_settings_precision#x-y-compensation");
        optgroup->append_single_option_line("elefant_foot_compensation", "quality_settings_precision#elephant-foot-compensation");
        optgroup->append_single_option_line("elefant_foot_compensation_layers", "quality_settings_precision#elephant-foot-compensation");
        optgroup->append_single_option_line("precise_outer_wall", "quality_settings_precision#precise-wall");
        optgroup->append_single_option_line("precise_z_height", "quality_settings_precision#precise-z-height");
        optgroup->append_single_option_line("hole_to_polyhole", "quality_settings_precision#polyholes");
        optgroup->append_single_option_line("hole_to_polyhole_threshold", "quality_settings_precision#polyholes");
        optgroup->append_single_option_line("hole_to_polyhole_twisted", "quality_settings_precision#polyholes");

        optgroup = page->new_optgroup(L("Ironing"), L"param_ironing");
        optgroup->append_single_option_line("ironing_type", "quality_settings_ironing#type");
        optgroup->append_single_option_line("ironing_pattern", "quality_settings_ironing#pattern");
        optgroup->append_single_option_line("ironing_flow", "quality_settings_ironing#flow");
        optgroup->append_single_option_line("ironing_spacing", "quality_settings_ironing#line-spacing");
        optgroup->append_single_option_line("ironing_inset", "quality_settings_ironing#inset");
        optgroup->append_single_option_line("ironing_angle", "quality_settings_ironing#angle");

        optgroup = page->new_optgroup(L("Wall generator"), L"param_wall_generator");
        optgroup->append_single_option_line("wall_generator", "quality_settings_wall_generator");
        // Arachne keys — shown when wall_generator == Arachne (toggle_line in ConfigManipulation).
        optgroup->append_single_option_line("wall_transition_angle", "quality_settings_wall_generator#arachne");
        optgroup->append_single_option_line("wall_transition_filter_deviation", "quality_settings_wall_generator#arachne");
        optgroup->append_single_option_line("wall_transition_length", "quality_settings_wall_generator#arachne");
        optgroup->append_single_option_line("wall_distribution_count", "quality_settings_wall_generator#arachne");
        optgroup->append_single_option_line("initial_layer_min_bead_width", "quality_settings_wall_generator#arachne");
        optgroup->append_single_option_line("min_bead_width", "quality_settings_wall_generator#arachne");
        optgroup->append_single_option_line("min_feature_size", "quality_settings_wall_generator#arachne");
        optgroup->append_single_option_line("min_length_factor", "quality_settings_wall_generator#arachne");
        // NEOTKO_NEOARACHNE_TAG fase2+fase3.0 — same optgroup as Arachne for visual
        // consistency: keys appear inline below the wall_generator dropdown when
        // NeoArachne is selected, mirroring how Arachne's keys show below it.
        // toggle_line gating lives in ConfigManipulation (have_neoarachne).
        optgroup->append_single_option_line("neoarachne_outer_wall",           "quality_settings_wall_generator#neoarachne");
        optgroup->append_single_option_line("neoarachne_inner_walls",          "quality_settings_wall_generator#neoarachne");
        optgroup->append_single_option_line("neoarachne_gap_fill",             "quality_settings_wall_generator#neoarachne");
        optgroup->append_single_option_line("neoarachne_allowed_overlap_pct",  "quality_settings_wall_generator#neoarachne-edge-closure");
        optgroup->append_single_option_line("neoarachne_min_bead_width_pct",   "quality_settings_wall_generator#neoarachne-edge-closure");
        optgroup->append_single_option_line("neoarachne_max_bead_width_pct",   "quality_settings_wall_generator#neoarachne-edge-closure");
        optgroup->append_single_option_line("neoarachne_min_feature_size_pct", "quality_settings_wall_generator#neoarachne-edge-closure");
        optgroup->append_single_option_line("neoarachne_keep_short_tails",     "quality_settings_wall_generator#neoarachne-edge-closure");
        // NEOTKO_NEOARACHNE_TAG fase3
        optgroup->append_single_option_line("neoarachne_bead_count_hysteresis_pct", "quality_settings_wall_generator#neoarachne-neotkoedge");
        // NEOTKO_NEOARACHNE_TAG fase4
        optgroup->append_single_option_line("neoarachne_transition_filter_dist_mm",  "quality_settings_wall_generator#neoarachne-neotkoedge");

        // NEOTKO_NEOARACHNE_TAG preview-lab PL.2+PL.3+PL.6 — reactive inline canvas.
        // Renders the W test geometry every time the user touches an Edge Closure
        // slider so the effect is visible without re-slicing the whole bed.
        // Pass `this` so the panel reads the LIVE Tab config (includes per-object
        // and per-plate overrides), not just the bundle's saved preset.
        {
            Line preview_line(_L("Edge Closure preview"), wxEmptyString);
            preview_line.full_width = 1;
            Tab* tab_self = this;
            preview_line.widget = [tab_self](wxWindow* parent) -> wxSizer* {
                auto* sizer = new wxBoxSizer(wxHORIZONTAL);
                auto* canvas = new NeoArachnePreviewPanel(parent, tab_self);
                sizer->Add(canvas, 1, wxEXPAND | wxALL, 4);
                return sizer;
            };
            optgroup->append_line(preview_line);
        }

        optgroup = page->new_optgroup(L("Walls and surfaces"), L"param_wall_surface");
        optgroup->append_single_option_line("wall_sequence", "quality_settings_wall_and_surfaces#walls-printing-order");
        optgroup->append_single_option_line("is_infill_first", "quality_settings_wall_and_surfaces#print-infill-first");
        optgroup->append_single_option_line("wall_direction", "quality_settings_wall_and_surfaces#wall-loop-direction");
        optgroup->append_single_option_line("print_flow_ratio", "quality_settings_wall_and_surfaces#surface-flow-ratio");
        optgroup->append_single_option_line("top_solid_infill_flow_ratio", "quality_settings_wall_and_surfaces#surface-flow-ratio");
        optgroup->append_single_option_line("bottom_solid_infill_flow_ratio", "quality_settings_wall_and_surfaces#surface-flow-ratio");
        optgroup->append_single_option_line("only_one_wall_top", "quality_settings_wall_and_surfaces#only-one-wall");
        optgroup->append_single_option_line("min_width_top_surface", "quality_settings_wall_and_surfaces#threshold");
        optgroup->append_single_option_line("only_one_wall_first_layer", "quality_settings_wall_and_surfaces#only-one-wall");
        optgroup->append_single_option_line("reduce_crossing_wall", "quality_settings_wall_and_surfaces#avoid-crossing-walls");
        optgroup->append_single_option_line("max_travel_detour_distance", "quality_settings_wall_and_surfaces#max-detour-length");

        optgroup->append_single_option_line("small_area_infill_flow_compensation", "quality_settings_wall_and_surfaces#small-area-flow-compensation");
        Option option = optgroup->get_option("small_area_infill_flow_compensation_model");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = 15;
        optgroup->append_single_option_line(option, "quality_settings_wall_and_surfaces#small-area-flow-compensation");

        optgroup = page->new_optgroup(L("Bridging"), L"param_bridge");
        optgroup->append_single_option_line("bridge_flow", "quality_settings_bridging#flow-ratio");
	    optgroup->append_single_option_line("internal_bridge_flow", "quality_settings_bridging#flow-ratio");
        optgroup->append_single_option_line("bridge_density", "quality_settings_bridging#bridge-density");
        optgroup->append_single_option_line("internal_bridge_density", "quality_settings_bridging#bridge-density");
        optgroup->append_single_option_line("thick_bridges", "quality_settings_bridging#thick-bridges");
        optgroup->append_single_option_line("thick_internal_bridges", "quality_settings_bridging#thick-bridges");
        optgroup->append_single_option_line("enable_extra_bridge_layer", "quality_settings_bridging#extra-bridge-layers");
        optgroup->append_single_option_line("dont_filter_internal_bridges", "quality_settings_bridging#filter-out-small-internal-bridges");
        optgroup->append_single_option_line("counterbore_hole_bridging", "quality_settings_bridging#bridge-counterbore-hole");

        // NEOTKO_NEOWEAVING_TAG_START
        optgroup = page->new_optgroup(L("Neotko Neoweaving"));
        optgroup->append_single_option_line("interlayer_neoweave_enabled");
        optgroup->append_single_option_line("interlayer_neoweave_mode");
        optgroup->append_single_option_line("neoweave_filter");
        optgroup->append_single_option_line("interlayer_neoweave_amplitude");
        optgroup->append_single_option_line("interlayer_neoweave_period");
        optgroup->append_single_option_line("interlayer_neoweave_max_z_speed");
        optgroup->append_single_option_line("interlayer_neoweave_min_length");
        optgroup->append_single_option_line("neoweave_penultimate_layers");
        optgroup->append_single_option_line("neoweave_speed_pct");
        // NEOTKO_NEOWEAVING_TAG_START — Feature 14: Monotonic Interlayer Nesting
        optgroup->append_single_option_line("neotko_interlayer_nesting_enabled");
        // NEOTKO_NEOWEAVING_TAG_END

        // NEOTKO_SURFACE_MIXER_TAG_START - Surface Color Mixer (replaces separate ColorMix/MultiPass/PathBlend controls)
        optgroup = page->new_optgroup(L("Surface ColorStitch")); // NEOTKO_COLORSTITCH_TAG — rebrand UI (keys intactas)

        // NEOTKO_SURFACE_MIXER_TAG — s97: el checkbox "Enable Surface Color Mixer"
        // se retira. Tras s96+s97 la única fuente de verdad del enable es el stack
        // del SandwichDialog (blobs neotko_surface_passes_top/_penu + sus toggles
        // per-zone). El checkbox externo se auto-revertía cuando había stack JSON
        // poblado (apagar = no persiste, on→off→on de un solo refresh), y duplicaba
        // controles. La entrada al editor vive en la fila "Sandwich editor…" abajo.

        // Row 2 (legacy "Edit Surface Color Mixer…" launcher) — DESHABILITADO en s90.
        // El SandwichDialog (Row 3 abajo) absorbe toda la funcionalidad; el último
        // bit que faltaba ("ColorMix min. line length") está ahora en el Advanced
        // per-pass de ColorMix (ColorMixPatternDialog). La clase SurfaceColorMixerDialog
        // se conserva intacta como referencia hasta la retirada formal en Fase 7
        // (ROADMAP_UNIFIED.md s82 punto 3).
#if 0
        // Row 2: Edit button — separate line below the Enable checkbox.
        create_line_with_widget(optgroup.get(), "interlayer_colormix_surface", "",
            [this](wxWindow* parent) -> wxSizer* {
                auto* btn = new wxButton(parent, wxID_ANY,
                                         _L("Edit Surface Color Mixer\u2026"),
                                         wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                    SurfaceColorMixerDialog dlg(
                        wxGetApp().mainframe,
                        m_config,
                        [this](const std::string& key) {
                            on_value_change(key, boost::any());
                        });
                    dlg.ShowModal();
                });
                auto* sz = new wxBoxSizer(wxHORIZONTAL);
                sz->Add(btn, 0, wxALL, 3);
                return sz;
            });
#endif // s90: legacy SurfaceColorMixerDialog launcher disabled
        // NEOTKO_SANDWICH_TAG — Fase 3: launcher for the pass-stack editor,
        // on its own row below the legacy dialog button.
        create_line_with_widget(optgroup.get(), "interlayer_colormix_surface", "",
            [this](wxWindow* parent) -> wxSizer* {
                auto* sw_btn = new wxButton(parent, wxID_ANY,
                                            _L("Sandwich editor…"),
                                            wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
                sw_btn->SetToolTip(_L("Edit the per-layer effect stack (Top and "
                                      "Penultimate) as a sandwich of passes."));
                sw_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                    SandwichDialog dlg(
                        wxGetApp().mainframe,
                        m_config,
                        [this](const std::string& key) {
                            on_value_change(key, boost::any());
                        });
                    dlg.ShowModal();
                });
                auto* sz = new wxBoxSizer(wxHORIZONTAL);
                sz->Add(sw_btn, 0, wxALL, 3);
                return sz;
            });
        // NEOTKO_COLORMIX_TAG s99 — surface interlayer_colormix_min_length here
        // as "Min. Segment" so it stops being buried in the SandwichDialog Linear
        // panel / retired "More" toggle. Gates which lines participate in the
        // ColorMix per-line split (lines shorter than this fall back to the
        // region's default extruder).
        optgroup->append_single_option_line("interlayer_colormix_min_length");
        // NEOTKO_COLORMIX_TAG — s60: numeric gradient controls live INSIDE the
        // Surface Color Mixer dialog (Gradient section). The optgroup is only
        // for the master enable + dialog launcher.
        // NEOTKO_SURFACE_MIXER_TAG_END

        optgroup = page->new_optgroup(L("Overhangs"), L"param_overhang");
        optgroup->append_single_option_line("detect_overhang_wall", "quality_settings_overhangs#detect-overhang-wall");
        optgroup->append_single_option_line("make_overhang_printable", "quality_settings_overhangs#make-overhang-printable");
        optgroup->append_single_option_line("make_overhang_printable_angle", "quality_settings_overhangs#maximum-angle");
        optgroup->append_single_option_line("make_overhang_printable_hole_size", "quality_settings_overhangs#hole-area");
        optgroup->append_single_option_line("extra_perimeters_on_overhangs", "quality_settings_overhangs#extra-perimeters-on-overhangs");
        optgroup->append_single_option_line("overhang_reverse", "quality_settings_overhangs#reverse-on-even");
        optgroup->append_single_option_line("overhang_reverse_internal_only", "quality_settings_overhangs#reverse-internal-only");
        optgroup->append_single_option_line("overhang_reverse_threshold", "quality_settings_overhangs#reverse-threshold");

    page = add_options_page(L("Strength"), "custom-gcode_strength"); // ORCA: icon only visible on placeholders
        optgroup = page->new_optgroup(L("Walls"), L"param_wall");
    optgroup->append_single_option_line("wall_loops", "strength_settings_walls#wall-loops");
        optgroup->append_single_option_line("alternate_extra_wall", "strength_settings_walls#alternate-extra-wall");
        optgroup->append_single_option_line("detect_thin_wall", "strength_settings_walls#detect-thin-wall");

        optgroup = page->new_optgroup(L("Top/bottom shells"), L"param_shell");
        optgroup->append_single_option_line("top_surface_pattern", "fill-patterns#Infill of the top surface and bottom surface");
        optgroup->append_single_option_line("top_shell_layers");
        optgroup->append_single_option_line("top_shell_thickness");
        // NEOTKO_LIBRE_TAG_START — Feature 4: Top/Bottom Surface Density UI
        optgroup->append_single_option_line("top_surface_density");
        // NEOTKO_LIBRE_TAG_END
        // NEOTKO_COLORMIX_TAG_START — Penultimate Top Layers Control
        optgroup->append_single_option_line("penultimate_top_layers");
        optgroup->append_single_option_line("penultimate_solid_infill_pattern");
        optgroup->append_single_option_line("penultimate_solid_infill_density");
        // NEOTKO_COLORMIX_TAG_END
        optgroup->append_single_option_line("bottom_surface_pattern", "fill-patterns#Infill of the top surface and bottom surface");
        optgroup->append_single_option_line("bottom_shell_layers");
        optgroup->append_single_option_line("bottom_shell_thickness");
        // NEOTKO_LIBRE_TAG_START — Feature 4: Bottom Surface Density UI
        optgroup->append_single_option_line("bottom_surface_density");
        // NEOTKO_LIBRE_TAG_END
        optgroup->append_single_option_line("top_bottom_infill_wall_overlap");

        optgroup = page->new_optgroup(L("Infill"), L"param_infill");
        optgroup->append_single_option_line("sparse_infill_density", "strength_settings_infill#sparse-infill-density");
        optgroup->append_single_option_line("fill_multiline", "strength_settings_infill#fill-multiline");
        optgroup->append_single_option_line("sparse_infill_pattern", "strength_settings_infill#sparse-infill-pattern");
        if (m_type >= Preset::TYPE_COUNT) {
            optgroup->append_single_option_line("enable_infill_filament_override");
            optgroup->append_single_option_line("infill_filament_use_base_first_layers");
            optgroup->append_single_option_line("infill_filament_use_base_last_layers");
            optgroup->append_single_option_line("sparse_infill_filament", "multimaterial_settings_filament_for_features#infill");
        }
        optgroup->append_single_option_line("infill_direction", "strength_settings_infill#direction");
        optgroup->append_single_option_line("sparse_infill_rotate_template", "strength_settings_infill_rotation_template_metalanguage");
        optgroup->append_single_option_line("skin_infill_density", "strength_settings_patterns#locked-zag");
        optgroup->append_single_option_line("skeleton_infill_density", "strength_settings_patterns#locked-zag");
        optgroup->append_single_option_line("infill_lock_depth", "strength_settings_patterns#locked-zag");
        optgroup->append_single_option_line("skin_infill_depth", "strength_settings_patterns#locked-zag");
        optgroup->append_single_option_line("skin_infill_line_width", "strength_settings_patterns#locked-zag");
        optgroup->append_single_option_line("skeleton_infill_line_width", "strength_settings_patterns#locked-zag");
        optgroup->append_single_option_line("symmetric_infill_y_axis", "strength_settings_patterns#zig-zag");
        optgroup->append_single_option_line("infill_shift_step", "strength_settings_patterns#cross-hatch");
        optgroup->append_single_option_line("lateral_lattice_angle_1", "strength_settings_patterns#lateral-lattice");
        optgroup->append_single_option_line("lateral_lattice_angle_2", "strength_settings_patterns#lateral-lattice");
        optgroup->append_single_option_line("infill_overhang_angle", "strength_settings_patterns#lateral-honeycomb");
        optgroup->append_single_option_line("infill_anchor_max", "strength_settings_infill#anchor");
        optgroup->append_single_option_line("infill_anchor", "strength_settings_infill#anchor");
        optgroup->append_single_option_line("internal_solid_infill_pattern", "strength_settings_infill#internal-solid-infill");
        optgroup->append_single_option_line("solid_infill_direction", "strength_settings_infill#direction");
        optgroup->append_single_option_line("solid_infill_rotate_template", "strength_settings_infill_rotation_template_metalanguage");
        optgroup->append_single_option_line("gap_fill_target", "strength_settings_infill#apply-gap-fill");
        optgroup->append_single_option_line("filter_out_gap_fill", "strength_settings_infill#filter-out-tiny-gaps");
        optgroup->append_single_option_line("infill_wall_overlap", "strength_settings_infill#infill-wall-overlap");

        optgroup = page->new_optgroup(L("Advanced"), L"param_advanced");
        optgroup->append_single_option_line("align_infill_direction_to_model", "strength_settings_advanced#align-infill-direction-to-model");
        optgroup->append_single_option_line("extra_solid_infills", "strength_settings_infill#extra-solid-infill");
        optgroup->append_single_option_line("bridge_angle", "strength_settings_advanced#bridge-infill-direction");
        optgroup->append_single_option_line("internal_bridge_angle", "strength_settings_advanced#bridge-infill-direction"); // ORCA: Internal bridge angle override
        optgroup->append_single_option_line("minimum_sparse_infill_area", "strength_settings_advanced#minimum-sparse-infill-threshold");
        optgroup->append_single_option_line("infill_combination", "strength_settings_advanced#infill-combination");
        optgroup->append_single_option_line("infill_combination_max_layer_height", "strength_settings_advanced#max-layer-height");
        optgroup->append_single_option_line("detect_narrow_internal_solid_infill", "strength_settings_advanced#detect-narrow-internal-solid-infill");
        optgroup->append_single_option_line("ensure_vertical_shell_thickness", "strength_settings_advanced#ensure-vertical-shell-thickness");

    page = add_options_page(L("Speed"), "custom-gcode_speed"); // ORCA: icon only visible on placeholders
        optgroup = page->new_optgroup(L("Initial layer speed"), L"param_speed_first", 15);
    optgroup->append_single_option_line("initial_layer_speed", "speed_settings_initial_layer_speed#initial-layer");
        optgroup->append_single_option_line("initial_layer_infill_speed", "speed_settings_initial_layer_speed#initial-layer-infill");
        optgroup->append_single_option_line("initial_layer_travel_speed", "speed_settings_initial_layer_speed#initial-layer-travel-speed");
        optgroup->append_single_option_line("slow_down_layers", "speed_settings_initial_layer_speed#number-of-slow-layers");
        optgroup = page->new_optgroup(L("Other layers speed"), L"param_speed", 15);
        optgroup->append_single_option_line("outer_wall_speed", "speed_settings_other_layers_speed#outer-wall");
        optgroup->append_single_option_line("inner_wall_speed", "speed_settings_other_layers_speed#inner-wall");
        optgroup->append_single_option_line("small_perimeter_speed", "speed_settings_other_layers_speed#small-perimeters");
        optgroup->append_single_option_line("small_perimeter_threshold", "speed_settings_other_layers_speed#small-perimeters-threshold");
        optgroup->append_single_option_line("sparse_infill_speed", "speed_settings_other_layers_speed#sparse-infill");
        optgroup->append_single_option_line("internal_solid_infill_speed", "speed_settings_other_layers_speed#internal-solid-infill");
        optgroup->append_single_option_line("top_surface_speed", "speed_settings_other_layers_speed#top-surface");
        // NEOTKO_COLORMIX_TAG_START — penultimate_infill_speed: key exists but UI hidden
        // Speed not working correctly — penultimate uses top_surface_speed for now. FASE 2.
        // optgroup->append_single_option_line("penultimate_infill_speed");
        // NEOTKO_COLORMIX_TAG_END
        optgroup->append_single_option_line("gap_infill_speed", "speed_settings_other_layers_speed#gap-infill");
        optgroup->append_single_option_line("ironing_speed", "speed_settings_other_layers_speed#ironing-speed");
        optgroup->append_single_option_line("support_speed", "speed_settings_other_layers_speed#support");
        optgroup->append_single_option_line("support_interface_speed", "speed_settings_other_layers_speed#support-interface");
        optgroup = page->new_optgroup(L("Overhang speed"), L"param_overhang_speed", 15);
        optgroup->append_single_option_line("enable_overhang_speed", "speed_settings_overhang_speed#slow-down-for-overhang");
        optgroup->append_single_option_line("slowdown_for_curled_perimeters", "speed_settings_overhang_speed#slow-down-for-curled-perimeters");
        Line line = { L("Overhang speed"), L("This is the speed for various overhang degrees. Overhang degrees are expressed as a percentage of line width. 0 speed means no slowing down for the overhang degree range and wall speed is used") };
        line.label_path = "slow-down-for-overhang";
        line.append_option(optgroup->get_option("overhang_1_4_speed"));
        line.append_option(optgroup->get_option("overhang_2_4_speed"));
        line.append_option(optgroup->get_option("overhang_3_4_speed"));
        line.append_option(optgroup->get_option("overhang_4_4_speed"));
        optgroup->append_line(line);
        optgroup->append_separator();
        line = { L("Bridge"), L("Set speed for external and internal bridges") };
        line.append_option(optgroup->get_option("bridge_speed"));
        line.append_option(optgroup->get_option("internal_bridge_speed"));
        optgroup->append_line(line);

        optgroup = page->new_optgroup(L("Travel speed"), L"param_travel_speed", 15);
        optgroup->append_single_option_line("travel_speed", "speed_settings_travel");

        optgroup = page->new_optgroup(L("Acceleration"), L"param_acceleration", 15);
        optgroup->append_single_option_line("default_acceleration", "speed_settings_acceleration#normal-printing");
        optgroup->append_single_option_line("outer_wall_acceleration", "speed_settings_acceleration#outer-wall");
        optgroup->append_single_option_line("inner_wall_acceleration", "speed_settings_acceleration#inner-wall");
        optgroup->append_single_option_line("bridge_acceleration", "speed_settings_acceleration#bridge");
        optgroup->append_single_option_line("sparse_infill_acceleration", "speed_settings_acceleration#sparse-infill");
        optgroup->append_single_option_line("internal_solid_infill_acceleration", "speed_settings_acceleration#internal-solid-infill");
        optgroup->append_single_option_line("initial_layer_acceleration", "speed_settings_acceleration#initial-layer");
        optgroup->append_single_option_line("top_surface_acceleration", "speed_settings_acceleration#top-surface");
        optgroup->append_single_option_line("travel_acceleration", "speed_settings_acceleration#travel");
        optgroup->append_single_option_line("accel_to_decel_enable", "speed_settings_acceleration");
        optgroup->append_single_option_line("accel_to_decel_factor", "speed_settings_acceleration");

        optgroup = page->new_optgroup(L("Jerk(XY)"), L"param_jerk", 15);
        optgroup->append_single_option_line("default_jerk", "speed_settings_jerk_xy#default");
        optgroup->append_single_option_line("outer_wall_jerk", "speed_settings_jerk_xy#outer-wall");
        optgroup->append_single_option_line("inner_wall_jerk", "speed_settings_jerk_xy#inner-wall");
        optgroup->append_single_option_line("infill_jerk", "speed_settings_jerk_xy#infill");
        optgroup->append_single_option_line("top_surface_jerk", "speed_settings_jerk_xy#top-surface");
        optgroup->append_single_option_line("initial_layer_jerk", "speed_settings_jerk_xy#initial-layer");
        optgroup->append_single_option_line("travel_jerk", "speed_settings_jerk_xy#travel");
        optgroup->append_single_option_line("default_junction_deviation", "speed_settings_jerk_xy#junction-deviation");

        optgroup = page->new_optgroup(L("Advanced"), L"param_advanced", 15);
        optgroup->append_single_option_line("max_volumetric_extrusion_rate_slope", "speed_settings_advanced");
        optgroup->append_single_option_line("max_volumetric_extrusion_rate_slope_segment_length", "speed_settings_advanced");
        optgroup->append_single_option_line("extrusion_rate_smoothing_external_perimeter_only", "speed_settings_advanced");

    page = add_options_page(L("Support"), "custom-gcode_support"); // ORCA: icon only visible on placeholders
        optgroup = page->new_optgroup(L("Support"), L"param_support");
    optgroup->append_single_option_line("enable_support", "support_settings_support");
        optgroup->append_single_option_line("support_type", "support_settings_support#type");
        optgroup->append_single_option_line("support_style", "support_settings_support#style");
        optgroup->append_single_option_line("support_threshold_angle", "support_settings_support#threshold-angle");
        optgroup->append_single_option_line("support_threshold_overlap", "support_settings_support#threshold-overlap");
        optgroup->append_single_option_line("raft_first_layer_density", "support_settings_support#initial-layer-density");
        optgroup->append_single_option_line("raft_first_layer_expansion", "support_settings_support#initial-layer-expansion");
        optgroup->append_single_option_line("support_on_build_plate_only", "support_settings_support#on-build-plate-only");
        optgroup->append_single_option_line("support_critical_regions_only", "support_settings_support#support-critical-regions-only");
        optgroup->append_single_option_line("support_remove_small_overhang", "support_settings_support#remove-small-overhangs");
        //optgroup->append_single_option_line("enforce_support_layers", "support_settings_support");

        optgroup = page->new_optgroup(L("Raft"), L"param_raft");
        optgroup->append_single_option_line("raft_layers", "support_settings_raft");
        optgroup->append_single_option_line("raft_contact_distance", "support_settings_raft");

        optgroup = page->new_optgroup(L("Support filament"), L"param_support_filament");
        optgroup->append_single_option_line("support_filament", "support_settings_filament#base");
        optgroup->append_single_option_line("support_interface_filament", "support_settings_filament#interface");
        optgroup->append_single_option_line("support_interface_not_for_body", "support_settings_filament#avoid-interface-filament-for-base");

        optgroup = page->new_optgroup(L("Support ironing"), L"param_ironing");
        optgroup->append_single_option_line("support_ironing", "support_settings_ironing");
        optgroup->append_single_option_line("support_ironing_pattern", "support_settings_ironing#pattern");
        optgroup->append_single_option_line("support_ironing_flow", "support_settings_ironing#flow");
        optgroup->append_single_option_line("support_ironing_spacing", "support_settings_ironing#line-spacing");

        //optgroup = page->new_optgroup(L("Options for support material and raft"));

        // Support
        optgroup = page->new_optgroup(L("Advanced"), L"param_advanced");
        optgroup->append_single_option_line("support_top_z_distance", "support_settings_advanced#z-distance");
        optgroup->append_single_option_line("support_bottom_z_distance", "support_settings_advanced#z-distance");
        optgroup->append_single_option_line("tree_support_wall_count", "support_settings_advanced#support-wall-loops");
        optgroup->append_single_option_line("support_base_pattern", "support_settings_advanced#base-pattern");
        optgroup->append_single_option_line("support_base_pattern_spacing", "support_settings_advanced#base-pattern-spacing");
        optgroup->append_single_option_line("support_angle", "support_settings_advanced#pattern-angle");
        optgroup->append_single_option_line("support_interface_top_layers", "support_settings_advanced#interface-layers");
        optgroup->append_single_option_line("support_interface_bottom_layers", "support_settings_advanced#interface-layers");
        optgroup->append_single_option_line("support_interface_pattern", "support_settings_advanced#interface-pattern");
        optgroup->append_single_option_line("support_interface_spacing", "support_settings_advanced#interface-spacing");
        optgroup->append_single_option_line("support_bottom_interface_spacing", "support_settings_advanced#interface-spacing");
        optgroup->append_single_option_line("support_expansion", "support_settings_advanced#normal-support-expansion");
        //optgroup->append_single_option_line("support_interface_loop_pattern", "support_settings_advanced");

        optgroup->append_single_option_line("support_object_xy_distance", "support_settings_advanced#supportobject-xy-distance");
        optgroup->append_single_option_line("support_object_first_layer_gap", "support_settings_advanced#supportobject-first-layer-gap");
        optgroup->append_single_option_line("bridge_no_support", "support_settings_advanced#dont-support-bridges");
        optgroup->append_single_option_line("max_bridge_length", "support_settings_advanced");
        optgroup->append_single_option_line("independent_support_layer_height", "support_settings_advanced#independent-support-layer-height");

        optgroup = page->new_optgroup(L("Tree supports"), L"param_support_tree");
        optgroup->append_single_option_line("tree_support_tip_diameter", "support_settings_tree#tip-diameter");
        optgroup->append_single_option_line("tree_support_branch_distance", "support_settings_tree#branch-distance");
        optgroup->append_single_option_line("tree_support_branch_distance_organic", "support_settings_tree#branch-distance");
        optgroup->append_single_option_line("tree_support_top_rate", "support_settings_tree#branch-density");
        optgroup->append_single_option_line("tree_support_branch_diameter", "support_settings_tree#branch-diameter");
        optgroup->append_single_option_line("tree_support_branch_diameter_organic", "support_settings_tree#branch-diameter");
        optgroup->append_single_option_line("tree_support_branch_diameter_angle", "support_settings_tree#branch-diameter-angle");
        optgroup->append_single_option_line("tree_support_branch_angle", "support_settings_tree#branch-angle");
        optgroup->append_single_option_line("tree_support_branch_angle_organic", "support_settings_tree#branch-angle");
        optgroup->append_single_option_line("tree_support_angle_slow", "support_settings_tree#preferred-branch-angle");
        optgroup->append_single_option_line("tree_support_adaptive_layer_height", "support_settings_tree");
        optgroup->append_single_option_line("tree_support_auto_brim", "support_settings_tree");
        optgroup->append_single_option_line("tree_support_brim_width", "support_settings_tree");

    page = add_options_page(L("Multimaterial"), "custom-gcode_multi_material"); // ORCA: icon only visible on placeholders
        optgroup = page->new_optgroup(L("Prime tower"), L"param_tower");
        optgroup->append_single_option_line("enable_prime_tower", "multimaterial_settings_prime_tower");
        optgroup->append_single_option_line("prime_tower_width", "multimaterial_settings_prime_tower#width");
        optgroup->append_single_option_line("prime_volume", "multimaterial_settings_prime_tower");
        optgroup->append_single_option_line("prime_tower_brim_width", "multimaterial_settings_prime_tower#brim-width");
        optgroup->append_single_option_line("prime_tower_brim_chamfer", "multimaterial_settings_prime_tower#brim-chamfer");
        optgroup->append_single_option_line("prime_tower_brim_chamfer_max_width", "multimaterial_settings_prime_tower#brim-chamfer-max-width");
        optgroup->append_single_option_line("wipe_tower_rotation_angle", "multimaterial_settings_prime_tower#wipe-tower-rotation-angle");
        optgroup->append_single_option_line("wipe_tower_bridging", "multimaterial_settings_prime_tower#maximal-bridging-distance");
        optgroup->append_single_option_line("wipe_tower_extra_spacing", "multimaterial_settings_prime_tower#wipe-tower-purge-lines-spacing");
        optgroup->append_single_option_line("wipe_tower_extra_flow", "multimaterial_settings_prime_tower#extra-flow-for-purge");
        optgroup->append_single_option_line("local_z_wipe_tower_purge_lines", "multimaterial_settings_prime_tower");
        optgroup->append_single_option_line("wipe_tower_max_purge_speed", "multimaterial_settings_prime_tower#maximum-wipe-tower-print-speed");
        optgroup->append_single_option_line("wipe_tower_wall_type", "multimaterial_settings_prime_tower#wall-type");
        optgroup->append_single_option_line("wipe_tower_cone_angle", "multimaterial_settings_prime_tower#stabilization-cone-apex-angle");
        optgroup->append_single_option_line("wipe_tower_extra_rib_length", "multimaterial_settings_prime_tower#extra-rib-length");
        optgroup->append_single_option_line("wipe_tower_rib_width", "multimaterial_settings_prime_tower#rib-width");
        optgroup->append_single_option_line("wipe_tower_fillet_wall", "multimaterial_settings_prime_tower#fillet-wall");
        optgroup->append_single_option_line("wipe_tower_no_sparse_layers", "multimaterial_settings_prime_tower#no-sparse-layers");
        optgroup->append_single_option_line("single_extruder_multi_material_priming", "multimaterial_settings_prime_tower");
        // NEOTKO_NEOTOWER_TAG_START
        // NEOTKO_NEOTOWER_TAG s104 — tower type selector replaces the legacy
        // neotko_wipe_tower checkbox (decisión s103: enum estilo wall_type).
        // The bool key stays registered as a hidden fallback for old profiles.
        optgroup->append_single_option_line("neotko_tower_type");
        optgroup->append_single_option_line("neotower_zigurat");
        optgroup->append_single_option_line("neotower_purge_compaction");
        // NEOTKO_MULTIPASS_PRIME_TAG — s58: SurfaceColorMix wipe reserve (unified).
        // Was previously buried inside MultiPassConfigDialog as two separate fields
        // (multipass_prime_volume + penultimate_multipass_prime_volume).  Now a single
        // global setting under NeoTower.
        optgroup->append_single_option_line("multipass_prime_volume");
        // NEOTKO_NEOTOWER_TAG_END

        optgroup = page->new_optgroup(L("Filament for Features"), L"param_filament_for_features");
        optgroup->append_single_option_line("wall_filament", "multimaterial_settings_filament_for_features#walls");
        optgroup->append_single_option_line("solid_infill_filament", "multimaterial_settings_filament_for_features#solid-infill");
        optgroup->append_single_option_line("wipe_tower_filament", "multimaterial_settings_filament_for_features#wipe-tower");

        optgroup = page->new_optgroup(L("Ooze prevention"), L"param_ooze_prevention");
        optgroup->append_single_option_line("ooze_prevention", "multimaterial_settings_ooze_prevention");
        optgroup->append_single_option_line("standby_temperature_delta", "multimaterial_settings_ooze_prevention#temperature-variation");
        optgroup->append_single_option_line("preheat_time", "multimaterial_settings_ooze_prevention#preheat-time");
        optgroup->append_single_option_line("preheat_steps", "multimaterial_settings_ooze_prevention#preheat-steps");
        optgroup->append_single_option_line("delta_temperature", "multimaterial_settings_ooze_prevention#delta-temperature");

        optgroup = page->new_optgroup(L("Flush options"), L"param_flush");
        optgroup->append_single_option_line("flush_into_infill", "multimaterial_settings_flush_options#flush-into-objects-infill");
        optgroup->append_single_option_line("flush_into_objects", "multimaterial_settings_flush_options");
        optgroup->append_single_option_line("flush_into_support", "multimaterial_settings_flush_options#flush-into-objects-support");
        optgroup = page->new_optgroup(L("Advanced"), L"advanced");
        optgroup->append_single_option_line("interlocking_beam", "multimaterial_settings_advanced#interlocking-beam");
        optgroup->append_single_option_line("interface_shells", "multimaterial_settings_advanced#interface-shells");
        optgroup->append_single_option_line("mmu_segmented_region_max_width", "multimaterial_settings_advanced#maximum-width-of-segmented-region");
        optgroup->append_single_option_line("mmu_segmented_region_interlocking_depth", "multimaterial_settings_advanced#interlocking-depth-of-segmented-region");
        optgroup->append_single_option_line("interlocking_beam_width", "multimaterial_settings_advanced#interlocking-beam-width");
        optgroup->append_single_option_line("interlocking_orientation", "multimaterial_settings_advanced#interlocking-direction");
        optgroup->append_single_option_line("interlocking_beam_layer_count", "multimaterial_settings_advanced#interlocking-beam-layers");
        optgroup->append_single_option_line("interlocking_depth", "multimaterial_settings_advanced#interlocking-depth");
        optgroup->append_single_option_line("interlocking_boundary_avoidance", "multimaterial_settings_advanced#interlocking-boundary-avoidance");

page = add_options_page(L("Others"), "custom-gcode_other"); // ORCA: icon only visible on placeholders
        optgroup = page->new_optgroup(L("Skirt"), L"param_skirt");
optgroup->append_single_option_line("skirt_loops", "others_settings_skirt#loops");
        optgroup->append_single_option_line("skirt_type", "others_settings_skirt#type");
        optgroup->append_single_option_line("min_skirt_length", "others_settings_skirt#minimum-extrusion-length");
        optgroup->append_single_option_line("skirt_distance", "others_settings_skirt#distance");
        optgroup->append_single_option_line("skirt_start_angle", "others_settings_skirt#start-point");
        optgroup->append_single_option_line("skirt_speed", "others_settings_skirt#speed");
        optgroup->append_single_option_line("skirt_height", "others_settings_skirt#height");
        optgroup->append_single_option_line("draft_shield", "others_settings_skirt#shield");
        optgroup->append_single_option_line("single_loop_draft_shield", "others_settings_skirt#single-loop-after-first-layer");

        optgroup = page->new_optgroup(L("Brim"), L"param_adhension");
        optgroup->append_single_option_line("brim_type", "others_settings_brim#type");
        optgroup->append_single_option_line("brim_width", "others_settings_brim#width");
        optgroup->append_single_option_line("brim_object_gap", "others_settings_brim#brim-object-gap");
        optgroup->append_single_option_line("brim_ears_max_angle", "others_settings_brim#ear-max-angle");
        optgroup->append_single_option_line("brim_ears_detection_length", "others_settings_brim#ear-detection-radius");

        optgroup = page->new_optgroup(L("Special mode"), L"param_special");
        optgroup->append_single_option_line("slicing_mode", "others_settings_special_mode#slicing-mode");
        optgroup->append_single_option_line("print_sequence", "others_settings_special_mode#print-sequence");
        optgroup->append_single_option_line("print_order", "others_settings_special_mode#intra-layer-order");
        optgroup->append_single_option_line("spiral_mode", "others_settings_special_mode#spiral-vase");
        optgroup->append_single_option_line("spiral_mode_smooth", "others_settings_special_mode#smooth-spiral");
        optgroup->append_single_option_line("spiral_mode_max_xy_smoothing", "others_settings_special_mode#max-xy-smoothing");
        optgroup->append_single_option_line("spiral_starting_flow_ratio", "others_settings_special_mode#spiral-starting-flow-ratio");
        optgroup->append_single_option_line("spiral_finishing_flow_ratio", "others_settings_special_mode#spiral-finishing-flow-ratio");

        optgroup->append_single_option_line("timelapse_type", "others_settings_special_mode#timelapse");

        // Use default (no icon) here to avoid runtime bitmap load failures.
        optgroup = page->new_optgroup(L("Mixed Filaments"));
        // Height-weighted cadence is no longer exposed in the Others tab.
        // optgroup->append_single_option_line("mixed_filament_gradient_mode");
        optgroup->append_single_option_line("mixed_filament_height_lower_bound");
        optgroup->append_single_option_line("mixed_filament_height_upper_bound");
        optgroup->append_single_option_line("mixed_filament_advanced_dithering");
        // Pointillisme controls are retired; keep config keys for compatibility only.
        // optgroup->append_single_option_line("mixed_filament_pointillism_pixel_size");
        // optgroup->append_single_option_line("mixed_filament_pointillism_line_gap");
        optgroup->append_single_option_line("mixed_filament_component_bias_enabled");
        optgroup->append_single_option_line("mixed_filament_surface_indentation");
        optgroup->append_single_option_line("mixed_filament_region_collapse");
        optgroup->append_single_option_line("dithering_z_step_size");
        optgroup->append_single_option_line("dithering_local_z_mode");
        optgroup->append_single_option_line("dithering_local_z_whole_objects");
        optgroup->append_single_option_line("dithering_local_z_direct_multicolor");
        optgroup->append_single_option_line("dithering_step_painted_zones_only");

        optgroup = page->new_optgroup(L("Fuzzy Skin"), L"fuzzy_skin");
        optgroup->append_single_option_line("fuzzy_skin", "others_settings_fuzzy_skin");
        optgroup->append_single_option_line("fuzzy_skin_mode", "others_settings_fuzzy_skin#fuzzy-skin-mode");
        optgroup->append_single_option_line("fuzzy_skin_noise_type", "others_settings_fuzzy_skin#noise-type");
        optgroup->append_single_option_line("fuzzy_skin_point_distance", "others_settings_fuzzy_skin#point-distance");
        optgroup->append_single_option_line("fuzzy_skin_thickness", "others_settings_fuzzy_skin#skin-thickness");
        optgroup->append_single_option_line("fuzzy_skin_scale", "others_settings_fuzzy_skin#skin-feature-size");
        optgroup->append_single_option_line("fuzzy_skin_octaves", "others_settings_fuzzy_skin#skin-noise-octaves");
        optgroup->append_single_option_line("fuzzy_skin_persistence", "others_settings_fuzzy_skin#skin-noise-persistence");
        optgroup->append_single_option_line("fuzzy_skin_first_layer", "others_settings_fuzzy_skin#apply-fuzzy-skin-to-first-layer");

        optgroup = page->new_optgroup(L("G-code output"), L"param_gcode");
        optgroup->append_single_option_line("reduce_infill_retraction", "others_settings_g_code_output#reduce-infill-retraction");
        optgroup->append_single_option_line("gcode_add_line_number", "others_settings_g_code_output#add-line-number");
        optgroup->append_single_option_line("gcode_comments", "others_settings_g_code_output#verbose-g-code");
        optgroup->append_single_option_line("gcode_label_objects", "others_settings_g_code_output#label-objects");
        optgroup->append_single_option_line("exclude_object", "others_settings_g_code_output#exclude-objects");
        option = optgroup->get_option("filename_format");
        // option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.multiline = true;
        // option.opt.height = 5;
        optgroup->append_single_option_line(option, "others_settings_g_code_output#filename-format");

        optgroup = page->new_optgroup(L("Post-processing Scripts"), L"param_gcode", 0);
        option = optgroup->get_option("post_process");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = 15;
        optgroup->append_single_option_line(option, "others_settings_post_processing_scripts");

        optgroup = page->new_optgroup(L("Notes"), "note", 0);
        option = optgroup->get_option("notes");
        option.opt.full_width = true;
        option.opt.height = 25;//250;
        optgroup->append_single_option_line(option, "others_settings_notes");

    // Orca: hide the dependencies tab for process for now. The UI is not ready yet.
    // page = add_options_page(L("Dependencies"), "param_profile_dependencies"); // icons ready
    //     optgroup = page->new_optgroup(L("Profile dependencies"), "param_profile_dependencies"); // icons ready

    //     create_line_with_widget(optgroup.get(), "compatible_printers", "", [this](wxWindow* parent) {
    //         return compatible_widget_create(parent, m_compatible_printers);
    //     });

    //     option = optgroup->get_option("compatible_printers_condition");
    //     option.opt.full_width = true;
    //     optgroup->append_single_option_line(option);

    //     build_preset_description_line(optgroup.get());

    // NEOTKO_LIBRE_TAG_START — Less Used Toggle: apply saved flags at build time
    apply_less_used_visibility();
    // NEOTKO_LIBRE_TAG_END
}

// Reload current config (aka presets->edited_preset->config) into the UI fields.
void TabPrint::reload_config()
{
    this->compatible_widget_reload(m_compatible_printers);
    Tab::reload_config();
}

void TabPrint::update_description_lines()
{
    Tab::update_description_lines();

    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptSLA)
        return;

    if (m_active_page && m_active_page->title() == "Layers and perimeters" &&
        m_recommended_thin_wall_thickness_description_line && m_top_bottom_shell_thickness_explanation)
    {
        m_recommended_thin_wall_thickness_description_line->SetText(
            from_u8(PresetHints::recommended_thin_wall_thickness(*m_preset_bundle)));
        m_top_bottom_shell_thickness_explanation->SetText(
            from_u8(PresetHints::top_bottom_shell_thickness_explanation(*m_preset_bundle)));
    }

}

void TabPrint::toggle_options()
{
    if (!m_active_page) return;
    // BBS: whether the preset is Bambu Lab printer
    if (m_preset_bundle) {
        bool is_BBL_printer = wxGetApp().preset_bundle->is_bbl_vendor();
        m_config_manipulation.set_is_BBL_Printer(is_BBL_printer);
    }

    m_config_manipulation.toggle_print_fff_options(m_config, m_type < Preset::TYPE_COUNT);

    Field *field = m_active_page->get_field("support_style");
    auto   support_type = m_config->opt_enum<SupportType>("support_type");
    if (auto choice = dynamic_cast<Choice*>(field)) {
        auto def = print_config_def.get("support_style");
        std::vector<int> enum_set_normal = {smsDefault, smsGrid, smsSnug };
        std::vector<int> enum_set_tree   = { smsDefault, smsTreeSlim, smsTreeStrong, smsTreeHybrid, smsTreeOrganic };
        auto &           set             = is_tree(support_type) ? enum_set_tree : enum_set_normal;
        auto &           opt             = const_cast<ConfigOptionDef &>(field->m_opt);
        auto             cb              = dynamic_cast<ComboBox *>(choice->window);
        auto             n               = cb->GetValue();
        opt.enum_values.clear();
        opt.enum_labels.clear();
        cb->Clear();
        for (auto i : set) {
            opt.enum_values.push_back(def->enum_values[i]);
            opt.enum_labels.push_back(def->enum_labels[i]);
            cb->Append(_(def->enum_labels[i]));
        }
        cb->SetValue(n);
    }

    // NEOTKO_LIBRE_TAG_START — Support options always accessible in Libre Mode
    // ConfigManipulation gates these on have_support_interface / have_prime_tower.
    // In LM we want them always visible and editable regardless of those conditions,
    // so we override here AFTER toggle_print_fff_options() has already run.
    {
        auto* ac = wxGetApp().app_config;
        if (ac && ac->get_bool("neotko_libre_mode")) {
            toggle_line("support_top_z_distance",          true);
            toggle_option("support_top_z_distance",        true);
            toggle_line("support_interface_spacing",       true);
            toggle_option("support_interface_spacing",     true);
            toggle_line("independent_support_layer_height", true);
            toggle_option("independent_support_layer_height", true);
        }
    }
    // NEOTKO_LIBRE_TAG_END

    // NEOTKO_SURFACE_MIXER_TAG: Effect assignment is managed inside SurfaceColorMixerDialog.
}

void TabPrint::update()
{
    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptSLA)
        return; // ys_FIXME

    m_update_cnt++;

    // ysFIXME: It's temporary workaround and should be clewer reworked:
    // Note: This workaround works till "enable_support" and "overhangs" is exclusive sets of mutually no-exclusive parameters.
    // But it should be corrected when we will have more such sets.
    // Disable check of the compatibility of the "enable_support" and "overhangs" options for saved user profile
    // NOTE: Initialization of the support_material_overhangs_queried value have to be processed just ones
    if (!m_config_manipulation.is_initialized_support_material_overhangs_queried())
    {
        const Preset& selected_preset = m_preset_bundle->prints.get_selected_preset();
        bool is_user_and_saved_preset = !selected_preset.is_system && !selected_preset.is_dirty;
        bool support_material_overhangs_queried = m_config->opt_bool("enable_support") && !m_config->opt_bool("detect_overhang_wall");
        m_config_manipulation.initialize_support_material_overhangs_queried(is_user_and_saved_preset && support_material_overhangs_queried);
    }

    m_config_manipulation.update_print_fff_config(m_config, m_type < Preset::TYPE_COUNT, m_type == Preset::TYPE_PLATE);

    update_description_lines();
    //BBS: GUI refactor
    //Layout();
    m_parent->Layout();

    m_update_cnt--;

    if (m_update_cnt==0) {
        if (m_active_page && !(m_active_page->title() == "Dependencies"))
            toggle_options();
        // update() could be called during undo/redo execution
        // Update of objectList can cause a crash in this case (because m_objects doesn't match ObjectList)
        if (m_type != Preset::TYPE_MODEL && !wxGetApp().plater()->inside_snapshot_capture())
            wxGetApp().obj_list()->update_and_show_object_settings_item();

        wxGetApp().mainframe->on_config_changed(m_config);
    }
}

void TabPrint::clear_pages()
{
    Tab::clear_pages();

    m_recommended_thin_wall_thickness_description_line = nullptr;
    m_top_bottom_shell_thickness_explanation = nullptr;
}


//BBS: GUI refactor

static std::vector<std::string> intersect(std::vector<std::string> const& l, std::vector<std::string> const& r)
{
    std::vector<std::string> t;
    std::copy_if(r.begin(), r.end(), std::back_inserter(t), [&l](auto & e) { return std::find(l.begin(), l.end(), e) != l.end(); });
    return t;
}

static std::vector<std::string> concat(std::vector<std::string> const& l, std::vector<std::string> const& r)
{
    std::vector<std::string> t;
    std::set_union(l.begin(), l.end(), r.begin(), r.end(), std::back_inserter(t));
    return t;
}

static std::vector<std::string> substruct(std::vector<std::string> const& l, std::vector<std::string> const& r)
{
    std::vector<std::string> t;
    std::copy_if(l.begin(), l.end(), std::back_inserter(t), [&r](auto & e) { return std::find(r.begin(), r.end(), e) == r.end(); });
    return t;
}

static DynamicPrintConfig resolved_model_config_for_tab(const DynamicPrintConfig& config)
{
    DynamicPrintConfig resolved(config);
    const auto*        infill_override_opt = config.option<ConfigOptionBool>("enable_infill_filament_override");
    const bool         infill_override_enabled = infill_override_opt != nullptr && infill_override_opt->value;

    if (!infill_override_enabled && resolved.has("sparse_infill_filament"))
        resolved.erase("sparse_infill_filament");

    if (const auto* extruder_opt = config.option<ConfigOptionInt>("extruder"); extruder_opt != nullptr && extruder_opt->value > 0) {
        const int extruder = extruder_opt->value;
        if (!resolved.has("wall_filament"))
            resolved.set_key_value("wall_filament", new ConfigOptionInt(extruder));
        if (!resolved.has("sparse_infill_filament"))
            resolved.set_key_value("sparse_infill_filament", new ConfigOptionInt(extruder));
        if (!resolved.has("solid_infill_filament"))
            resolved.set_key_value("solid_infill_filament", new ConfigOptionInt(extruder));
    }

    if (!resolved.has("solid_infill_filament") && resolved.has("sparse_infill_filament"))
        resolved.set_key_value("solid_infill_filament", new ConfigOptionInt(resolved.opt_int("sparse_infill_filament")));

    return resolved;
}

TabPrintModel::TabPrintModel(ParamsPanel* parent, std::vector<std::string> const & keys)
    : TabPrint(parent, Preset::TYPE_MODEL)
    , m_keys(intersect(Preset::print_options(), keys))
    , m_prints(Preset::TYPE_MODEL, Preset::print_options(), static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults()))
{
    m_opt_status_value = osInitValue | osSystemValue;
    m_is_default_preset = true;
}

void TabPrintModel::build()
{
    m_presets = &m_prints;
    TabPrint::build();
    init_options_list();

    auto page = add_options_page(L("Frequent"), "empty");
        auto optgroup = page->new_optgroup("");
            optgroup->append_single_option_line("layer_height", "quality_settings_layer_height");
            optgroup->append_single_option_line("sparse_infill_density", "strength_settings_infill#sparse-infill-density");
            optgroup->append_single_option_line("wall_loops", "strength_settings_walls");
            optgroup->append_single_option_line("enable_support", "support_settings_support");
    m_pages.pop_back();
    m_pages.insert(m_pages.begin(), page);

    for (auto p : m_pages) {
        for (auto g : p->m_optgroups) {
            auto & lines = const_cast<std::vector<Line>&>(g->get_lines());
            for (auto & l : lines) {
                auto & opts = const_cast<std::vector<Option>&>(l.get_options());
                opts.erase(std::remove_if(opts.begin(), opts.end(), [this](auto & o) {
                    return !has_key(o.opt.opt_key);
                }), opts.end());
                l.undo_to_sys = true;
            }
            lines.erase(std::remove_if(lines.begin(), lines.end(), [](auto & l) {
                // NEOTKO_NEOARACHNE_TAG preview-lab — widget-only lines (e.g. the
                // NeoArachne preview canvas) have no options but ARE meaningful.
                // Keeping them lets the same preview render under per-object
                // overrides, where it reflects the merged config.
                return l.get_options().empty() && l.widget == nullptr;
            }), lines.end());
            // TODO: remove items from g->m_options;
            g->have_sys_config = [this] { m_back_to_sys = true; return true; };
        }
        p->m_optgroups.erase(std::remove_if(p->m_optgroups.begin(), p->m_optgroups.end(), [](auto & g) {
            return g->get_lines().empty();
        }), p->m_optgroups.end());
    }
    m_pages.erase(std::remove_if(m_pages.begin(), m_pages.end(), [](auto & p) {
        return p->m_optgroups.empty();
    }), m_pages.end());

    // NEOTKO_LIBRE_TAG_START — Less Used Toggle: apply after key filtering
    apply_less_used_visibility();
    // NEOTKO_LIBRE_TAG_END
}

void TabPrintModel::set_model_config(std::map<ObjectBase *, ModelConfig *> const & object_configs)
{
    m_object_configs = object_configs;
    m_prints.get_selected_preset().config.apply(*m_parent_tab->m_config);
    update_model_config();
}

void TabPrintModel::update_model_config()
{
    if (m_config_manipulation.is_applying()) {
        return;
    }
    m_config->apply(*m_parent_tab->m_config);
    if (m_type != Preset::TYPE_PLATE) {
        m_config->apply_only(*wxGetApp().plate_tab->get_config(), plate_keys);
    }
    m_null_keys.clear();
    if (!m_object_configs.empty()) {
        DynamicPrintConfig const & global_config= *m_config;
        const DynamicPrintConfig local_config = resolved_model_config_for_tab(m_object_configs.begin()->second->get());
        DynamicPrintConfig diff_config;
        std::vector<std::string> all_keys = local_config.keys(); // at least one has these keys
        std::vector<std::string> local_keys = intersect(m_keys, all_keys); // all equal on these keys
        if (m_object_configs.size() > 1) {
            std::vector<std::string> global_keys = m_keys; // all equal with global on these keys
            for (auto & config : m_object_configs) {
                const DynamicPrintConfig resolved_config = resolved_model_config_for_tab(config.second->get());
                auto equals = global_config.equal(resolved_config);
                global_keys = intersect(global_keys, equals);
                diff_config.apply_only(resolved_config, substruct(resolved_config.keys(), equals));
                all_keys = concat(all_keys, resolved_config.keys());
                local_keys = intersect(local_keys, local_config.equal(resolved_config));
            }
            all_keys = intersect(all_keys, m_keys);
            m_null_keys = substruct(substruct(all_keys, global_keys), local_keys);
            m_config->apply(diff_config);
        }
        m_all_keys = intersect(all_keys, m_keys);
        // except those than all equal on
        m_config->apply_only(local_config, local_keys);
        m_config_manipulation.apply_null_fff_config(m_config, m_null_keys, m_object_configs);

        if (m_type == Preset::Type::TYPE_PLATE) {
            // Reset m_config manually because there's no corresponding config in m_parent_tab->m_config
            for (auto plate_item : m_object_configs) {
                const DynamicPrintConfig& plate_config = plate_item.second->get();
                BedType plate_bed_type = (BedType)0;
                PrintSequence plate_print_seq = (PrintSequence)0;
                if (!plate_config.has("curr_bed_type")) {
                    // same as global
                    DynamicConfig& global_cfg = wxGetApp().preset_bundle->project_config;
                    if (global_cfg.has("curr_bed_type")) {
                        BedType global_bed_type = global_cfg.opt_enum<BedType>("curr_bed_type");
                        m_config->set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(global_bed_type));
                    }
                }
                if (!plate_config.has("first_layer_print_sequence")) {
                    m_config->set_key_value("first_layer_sequence_choice", new ConfigOptionEnum<LayerSeq>(flsAuto));
                }
                else {
                    replace(m_all_keys.begin(), m_all_keys.end(), std::string("first_layer_print_sequence"), std::string("first_layer_sequence_choice"));
                    m_config->set_key_value("first_layer_sequence_choice", new ConfigOptionEnum<LayerSeq>(flsCustomize));
                }
                if (!plate_config.has("other_layers_print_sequence")) {
                    m_config->set_key_value("other_layers_sequence_choice", new ConfigOptionEnum<LayerSeq>(flsAuto));
                }
                else {
                    replace(m_all_keys.begin(), m_all_keys.end(), std::string("other_layers_print_sequence"), std::string("other_layers_sequence_choice"));
                    m_config->set_key_value("other_layers_sequence_choice", new ConfigOptionEnum<LayerSeq>(flsCustomize));
                }
                notify_changed(plate_item.first);
            }
        }

    }
    toggle_options();
    if (m_active_page)
        m_active_page->update_visibility(m_mode, true); // for taggle line
    update_dirty();
    TabPrint::reload_config();
    //update();
    if (!m_null_keys.empty()) {
        if (m_active_page) {
            for (auto k : m_null_keys) {
                auto f = m_active_page->get_field(k);
                if (f)
                    f->set_value(boost::any(), false);
            }
        }
    }
}

void TabPrintModel::reset_model_config()
{
    if (m_object_configs.empty()) return;
    wxGetApp().plater()->take_snapshot(std::string("Reset Options"));
    for (auto config : m_object_configs) {
        auto rmkeys = intersect(m_keys, config.second->keys());
        for (auto& k : rmkeys) {
            config.second->erase(k);
        }
        notify_changed(config.first);
    }
    update_model_config();
    wxGetApp().mainframe->on_config_changed(m_config);
}

bool TabPrintModel::has_key(std::string const& key)
{
    return std::find(m_keys.begin(), m_keys.end(), key) != m_keys.end();
}

void TabPrintModel::activate_selected_page(std::function<void()> throw_if_canceled)
{
    TabPrint::activate_selected_page(throw_if_canceled);
    if (m_active_page) {
        for (auto k : m_null_keys) {
            auto f = m_active_page->get_field(k);
            if (f)
                f->set_value(boost::any(), false);
        }
    }
}

void TabPrintModel::on_value_change(const std::string& opt_key, const boost::any& value)
{
    // TODO: support opt_index, translate by OptionsGroup's m_opt_map
    auto k = opt_key;
    if (m_config_manipulation.is_applying()) {
        TabPrint::on_value_change(opt_key, value);
        return;
    }
    if (!has_key(k))
        return;
    if (!m_object_configs.empty())
        wxGetApp().plater()->take_snapshot((boost::format("Change Option %s") % k).str());
    auto inull = std::find(m_null_keys.begin(), m_null_keys.end(), k);
    // always add object config
    bool set   = true; // *m_config->option(k) != *m_prints.get_selected_preset().config.option(k) || inull != m_null_keys.end();
    if (m_back_to_sys) {
        for (auto config : m_object_configs)
            config.second->erase(k);
        m_all_keys.erase(std::remove(m_all_keys.begin(), m_all_keys.end(), k), m_all_keys.end());
    } else if (set) {
        for (auto config : m_object_configs)
            config.second->apply_only(*m_config, {k});
        m_all_keys = concat(m_all_keys, {k});
    }
    if (inull != m_null_keys.end())
        m_null_keys.erase(inull);
    if (m_back_to_sys || set) update_changed_ui();
    m_back_to_sys = false;
    TabPrint::on_value_change(k, value);
    for (auto config : m_object_configs) {
        config.second->touch();
        notify_changed(config.first);
    }
    wxGetApp().params_panel()->notify_object_config_changed();
}

void TabPrintModel::reload_config()
{
    TabPrint::reload_config();
    auto keys = m_config_manipulation.applying_keys();
    bool super_changed = false;
    for (auto & k : keys) {
        if (has_key(k)) {
            auto inull = std::find(m_null_keys.begin(), m_null_keys.end(), k);
            bool set   = *m_config->option(k) != *m_prints.get_selected_preset().config.option(k) || inull != m_null_keys.end();
            if (set) {
                for (auto config : m_object_configs)
                    config.second->apply_only(*m_config, {k});
                m_all_keys = concat(m_all_keys, {k});
            }
            if (inull != m_null_keys.end()) m_null_keys.erase(inull);
        } else {
            m_parent_tab->m_config->apply_only(*m_config, {k});
            super_changed = true;
        }
    }
    if (super_changed) {
        m_parent_tab->update_dirty();
        m_parent_tab->reload_config();
        m_parent_tab->update();
    }
}

void TabPrintModel::update_custom_dirty()
{
    for (auto k : m_null_keys) m_options_list[k] = 0;
    for (auto k : m_all_keys) m_options_list[k] &= ~osSystemValue;
}

//BBS: GUI refactor
TabPrintPlate::TabPrintPlate(ParamsPanel* parent) :
    TabPrintModel(parent, plate_keys)
{
    m_parent_tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
    m_type = Preset::TYPE_PLATE;
    m_keys = concat(m_keys, plate_keys);
}

void TabPrintPlate::build()
{
    m_presets = &m_prints;
    load_initial_data();

    m_config->option("curr_bed_type", true);
    if (m_preset_bundle->project_config.has("curr_bed_type")) {
        BedType global_bed_type = m_preset_bundle->project_config.opt_enum<BedType>("curr_bed_type");
        global_bed_type = BedType(global_bed_type - 1);
        m_config->set_key_value("curr_bed_type", new ConfigOptionEnum<BedType>(global_bed_type));
    }
    m_config->option("first_layer_sequence_choice", true);
    m_config->option("first_layer_print_sequence", true);
    m_config->option("other_layers_print_sequence", true);
    m_config->option("other_layers_sequence_choice", true);

    auto page = add_options_page(L("Plate Settings"), "empty");
    auto optgroup = page->new_optgroup("");
    optgroup->append_single_option_line("curr_bed_type");
    optgroup->append_single_option_line("skirt_start_angle");
    optgroup->append_single_option_line("print_sequence");
    optgroup->append_single_option_line("spiral_mode");
    optgroup->append_single_option_line("first_layer_sequence_choice");
    optgroup->append_single_option_line("other_layers_sequence_choice");

    for (auto& line : const_cast<std::vector<Line>&>(optgroup->get_lines())) {
        line.undo_to_sys = true;
    }
    optgroup->have_sys_config = [this] { m_back_to_sys = true; return true; };
}

void TabPrintPlate::reset_model_config()
{
    if (m_object_configs.empty()) return;
    wxGetApp().plater()->take_snapshot(std::string("Reset Options"));
    for (auto plate_item : m_object_configs) {
        auto rmkeys = intersect(m_keys, plate_item.second->keys());
        for (auto& k : rmkeys) {
            plate_item.second->erase(k);
        }
        auto plate = dynamic_cast<PartPlate*>(plate_item.first);
        plate->reset_bed_type();
        plate->reset_skirt_start_angle();
        plate->set_print_seq(PrintSequence::ByDefault);
        plate->set_first_layer_print_sequence({});
        plate->set_other_layers_print_sequence({});
        plate->set_spiral_vase_mode(false, true);
        notify_changed(plate_item.first);
    }
    update_model_config();
    wxGetApp().mainframe->on_config_changed(m_config);
}

void TabPrintPlate::on_value_change(const std::string& opt_key, const boost::any& value)
{
    auto k = opt_key;
    if (m_config_manipulation.is_applying()) {
        return;
    }
    if (!has_key(k))
        return;
    if (!m_object_configs.empty())
        wxGetApp().plater()->take_snapshot((boost::format("Change Option %s") % k).str());
    bool set = true;
    if (m_back_to_sys) {
        for (auto plate_item : m_object_configs) {
            plate_item.second->erase(k);
            auto plate = dynamic_cast<PartPlate*>(plate_item.first);
            if (k == "curr_bed_type")
                plate->reset_bed_type();
            if (k == "skirt_start_angle")
                plate->config()->erase("skirt_start_angle");
            if (k == "print_sequence")
                plate->set_print_seq(PrintSequence::ByDefault);
            if (k == "first_layer_sequence_choice")
                plate->set_first_layer_print_sequence({});
            if (k == "other_layers_sequence_choice")
                plate->set_other_layers_print_sequence({});
            if (k == "spiral_mode")
                plate->set_spiral_vase_mode(false, true);
        }
        m_all_keys.erase(std::remove(m_all_keys.begin(), m_all_keys.end(), k), m_all_keys.end());
    }
    else if (set) {
        for (auto plate_item : m_object_configs) {
            plate_item.second->apply_only(*m_config, { k });
            auto plate = dynamic_cast<PartPlate*>(plate_item.first);
            BedType bed_type;
            PrintSequence print_seq;
            LayerSeq first_layer_seq_choice;
            LayerSeq other_layer_seq_choice;
            if (k == "curr_bed_type") {
                bed_type = m_config->opt_enum<BedType>("curr_bed_type");
                plate->set_bed_type(BedType(bed_type));
            }
            if (k == "skirt_start_angle") {
                float angle = m_config->opt_float("skirt_start_angle");
                plate->config()->set_key_value("skirt_start_angle", new ConfigOptionFloat(angle));
            }
            if (k == "print_sequence") {
                print_seq = m_config->opt_enum<PrintSequence>("print_sequence");
                plate->set_print_seq(print_seq);
            }
            if (k == "first_layer_sequence_choice") {
                first_layer_seq_choice = m_config->opt_enum<LayerSeq>("first_layer_sequence_choice");
                if (first_layer_seq_choice == LayerSeq::flsAuto) {
                    plate->set_first_layer_print_sequence({});
                }
                else if (first_layer_seq_choice == LayerSeq::flsCustomize) {
                    const DynamicPrintConfig& plate_config = plate_item.second->get();
                    if (!plate_config.has("first_layer_print_sequence")) {
                        std::vector<int> initial_sequence;
                        for (int i = 0; i < wxGetApp().filaments_cnt(); i++) {
                            initial_sequence.push_back(i + 1);
                        }
                        plate->set_first_layer_print_sequence(initial_sequence);
                    }
                    wxCommandEvent evt(EVT_OPEN_PLATESETTINGSDIALOG);
                    evt.SetInt(plate->get_index());
                    evt.SetString("only_layer_sequence");
                    evt.SetEventObject(wxGetApp().plater());
                    wxPostEvent(wxGetApp().plater(), evt);
                }
            }
            if (k == "other_layers_sequence_choice") {
                other_layer_seq_choice = m_config->opt_enum<LayerSeq>("other_layers_sequence_choice");
                if (other_layer_seq_choice == LayerSeq::flsAuto) {
                    plate->set_other_layers_print_sequence({});
                }
                else if (other_layer_seq_choice == LayerSeq::flsCustomize) {
                    const DynamicPrintConfig& plate_config = plate_item.second->get();
                    if (!plate_config.has("other_layers_print_sequence")) {
                        std::vector<int> initial_sequence;
                        for (int i = 0; i < wxGetApp().filaments_cnt(); i++) {
                            initial_sequence.push_back(i + 1);
                        }
                        std::vector<LayerPrintSequence> initial_layer_sequence{ std::make_pair(std::make_pair(2, INT_MAX), initial_sequence) };
                        plate->set_other_layers_print_sequence(initial_layer_sequence);
                    }
                    wxCommandEvent evt(EVT_OPEN_PLATESETTINGSDIALOG);
                    evt.SetInt(plate->get_index());
                    evt.SetString("only_layer_sequence");
                    evt.SetEventObject(wxGetApp().plater());
                    wxPostEvent(wxGetApp().plater(), evt);
                }
            }
            if (k == "spiral_mode") {
                plate->set_spiral_vase_mode(m_config->opt_bool("spiral_mode"), false);
            }
        }
        m_all_keys = concat(m_all_keys, { k });
    }
    if (m_back_to_sys || set) update_changed_ui();
    m_back_to_sys = false;
    for (auto plate_item : m_object_configs) {
        plate_item.second->touch();
        notify_changed(plate_item.first);
    }

    wxGetApp().params_panel()->notify_object_config_changed();
    update();
}

void TabPrintPlate::notify_changed(ObjectBase* object)
{
    auto plate = dynamic_cast<PartPlate*>(object);
    auto objects_list = wxGetApp().obj_list();
    wxDataViewItemArray items;
    objects_list->GetSelections(items);
    for (auto item : items) {
        if (objects_list->GetModel()->GetItemType(item) == itPlate) {
            ObjectDataViewModelNode* node = static_cast<ObjectDataViewModelNode*>(item.GetID());
            if (node)
                node->set_action_icon(!m_all_keys.empty());
        }
    }
}

void TabPrintPlate::update_custom_dirty()
{
    for (auto k : m_null_keys)
        m_options_list[k] = 0;
    for (auto k : m_all_keys) {
        if (k == "first_layer_sequence_choice" || k == "other_layers_sequence_choice") {
            if (m_config->opt_enum<LayerSeq>("first_layer_sequence_choice") != LayerSeq::flsAuto) {
                m_options_list[k] &= ~osInitValue;
            }
            if (m_config->opt_enum<LayerSeq>("other_layers_sequence_choice") != LayerSeq::flsAuto) {
                m_options_list[k] &= ~osInitValue;
            }
        }
        if (k == "curr_bed_type") {
            DynamicConfig& global_cfg = wxGetApp().preset_bundle->project_config;
            if (global_cfg.has("curr_bed_type")) {
                BedType global_bed_type = global_cfg.opt_enum<BedType>("curr_bed_type");
                if (m_config->opt_enum<BedType>("curr_bed_type") != global_bed_type) {
                    m_options_list[k] &= ~osInitValue;
                }
            }
        }
        m_options_list[k] &= ~osSystemValue;
    }
}

TabPrintObject::TabPrintObject(ParamsPanel* parent) :
    TabPrintModel(parent, concat(PrintObjectConfig().keys(), PrintRegionConfig().keys()))
{
    m_parent_tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
}

void TabPrintObject::notify_changed(ObjectBase * object)
{
    auto obj = dynamic_cast<ModelObject*>(object);
    wxGetApp().obj_list()->object_config_options_changed({obj, nullptr});
}

//BBS: GUI refactor

TabPrintPart::TabPrintPart(ParamsPanel* parent) :
    // NEOTKO_LIBRE_TAG_START — Assembled Parts Full Options
    // In Libre Mode: expose the same keys as TabPrintObject (ObjectConfig + RegionConfig).
    // NOTE: The tab is built once at startup. If Libre Mode is toggled at runtime,
    // a restart is required for this change to take effect.
    // wipe_tower_* are in PrintConfig (global) — not in PrintObjectConfig, never shown here.
    TabPrintModel(parent, ([](){
        auto* ac = wxGetApp().app_config;
        if (ac && ac->get_bool("neotko_libre_mode"))
            return concat(PrintObjectConfig().keys(), PrintRegionConfig().keys());
        return PrintRegionConfig().keys();
    })())
    // NEOTKO_LIBRE_TAG_END
{
    m_parent_tab = wxGetApp().get_model_tab();
}

void TabPrintPart::notify_changed(ObjectBase * object)
{
    auto vol = dynamic_cast<ModelVolume*>(object);
    wxGetApp().obj_list()->object_config_options_changed({vol->get_object(), vol});
}

// NEOTKO_LIBRE_TAG_START — Assembled Parts Full Options
// Copies ObjectConfig overrides from the Part's ModelVolume config → parent ModelObject config,
// then schedules a reslice. Only ObjectConfig keys that are explicitly set in the volume config
// are copied — RegionConfig keys are handled natively per-volume by the slicer already.
void TabPrintPart::refresh_part_object_config()
{
    const auto obj_key_vec = PrintObjectConfig().keys();
    const auto is_obj_key = [&obj_key_vec](const std::string& k) {
        return std::find(obj_key_vec.begin(), obj_key_vec.end(), k) != obj_key_vec.end();
    };

    bool any_copied = false;
    for (auto& [obj_base, model_config] : m_object_configs) {
        auto* vol = dynamic_cast<ModelVolume*>(obj_base);
        if (!vol) continue;
        ModelObject* obj = vol->get_object();
        const DynamicPrintConfig& vol_cfg = model_config->get();

        for (const std::string& key : vol_cfg.keys()) {
            if (!is_obj_key(key)) continue;
            const ConfigOption* opt = vol_cfg.option(key);
            if (!opt) continue;
            obj->config.set_key_value(key, opt->clone());
            any_copied = true;

            if (std::getenv("ORCA_DEBUG_LIBRE")) {
                FILE* f = fopen("/tmp/libre_debug.log", "a");
                if (f) {
                    fprintf(f, "[REFRESH_SETTINGS] vol→obj key=%s\n", key.c_str());
                    fclose(f);
                }
            }
        }
    }

    if (any_copied)
        wxGetApp().plater()->schedule_background_process();
}
// NEOTKO_LIBRE_TAG_END

static std::string layer_height = "layer_height";
TabPrintLayer::TabPrintLayer(ParamsPanel* parent) :
    TabPrintModel(parent, concat({ layer_height }, PrintRegionConfig().keys()))
{
    m_parent_tab = wxGetApp().get_model_tab();
}

void TabPrintLayer::notify_changed(ObjectBase * object)
{
    for (auto config : m_object_configs) {
        if (!config.second->has(layer_height)) {
            auto option = m_parent_tab->get_config()->option(layer_height);
            config.second->set_key_value(layer_height, option->clone());
        }
        auto objects_list = wxGetApp().obj_list();
        wxDataViewItemArray items;
        objects_list->GetSelections(items);
        for (auto item : items)
            objects_list->add_settings_item(item, &config.second->get());
    }
}

void TabPrintLayer::update_custom_dirty()
{
    for (auto k : m_null_keys) m_options_list[k] = 0;
    for (auto k : m_all_keys) m_options_list[k] &= ~osSystemValue;

    auto option = m_parent_tab->get_config()->option(layer_height);
    for (auto config : m_object_configs) {
        if (!config.second->has(layer_height)) {
            config.second->set_key_value(layer_height, option->clone());
            m_options_list[layer_height] = osInitValue | osSystemValue;
        }
        else if (config.second->opt_float(layer_height) == option->getFloat())
            m_options_list[layer_height] = osInitValue | osSystemValue;
    }
}

bool Tab::validate_custom_gcode(const wxString& title, const std::string& gcode)
{
    std::vector<std::string> tags;
    bool invalid = GCodeProcessor::contains_reserved_tags(gcode, 5, tags);
    if (invalid) {
        std::string lines = ":\n";
        for (const std::string& keyword : tags)
            lines += ";" + keyword + "\n";
        wxString reports = format_wxstr(
            _L_PLURAL("Following line %s contains reserved keywords.\nPlease remove it, or will beat G-code visualization and printing time estimation.",
                      "Following lines %s contain reserved keywords.\nPlease remove them, or will beat G-code visualization and printing time estimation.",
                      tags.size()),
            lines);
        //wxMessageDialog dialog(wxGetApp().mainframe, reports, _L("Found reserved keywords in") + " " + _(title), wxICON_WARNING | wxOK);
        MessageDialog dialog(wxGetApp().mainframe, reports, _L("Reserved keywords found") + " " + _(title), wxICON_WARNING | wxOK);
        dialog.ShowModal();
    }
    return !invalid;
}

static void validate_custom_gcode_cb(Tab* tab, const wxString& title, const t_config_option_key& opt_key, const boost::any& value) {
    tab->validate_custom_gcodes_was_shown = !Tab::validate_custom_gcode(title, boost::any_cast<std::string>(value));
    tab->update_dirty();
    tab->on_value_change(opt_key, value);
}

static void validate_custom_gcode_cb(Tab* tab, ConfigOptionsGroupShp opt_group, const t_config_option_key& opt_key, const boost::any& value) {
    tab->validate_custom_gcodes_was_shown = !Tab::validate_custom_gcode(opt_group->title, boost::any_cast<std::string>(value));
    tab->update_dirty();
    tab->on_value_change(opt_key, value);
}

void Tab::edit_custom_gcode(const t_config_option_key& opt_key)
{
    EditGCodeDialog dlg = EditGCodeDialog(this, opt_key, get_custom_gcode(opt_key));
    if (dlg.ShowModal() == wxID_OK) {
        set_custom_gcode(opt_key, dlg.get_edited_gcode());
        update_dirty();
        update();
    }
}

const std::string& Tab::get_custom_gcode(const t_config_option_key& opt_key)
{
    return m_config->opt_string(opt_key);
}

void Tab::set_custom_gcode(const t_config_option_key& opt_key, const std::string& value)
{
    DynamicPrintConfig new_conf = *m_config;
    new_conf.set_key_value(opt_key, new ConfigOptionString(value));
    load_config(new_conf);
}

const std::string& TabFilament::get_custom_gcode(const t_config_option_key& opt_key)
{
    return m_config->opt_string(opt_key, unsigned(0));
}

void TabFilament::set_custom_gcode(const t_config_option_key& opt_key, const std::string& value)
{
    std::vector<std::string> gcodes = static_cast<const ConfigOptionStrings*>(m_config->option(opt_key))->values;
    gcodes[0] = value;

    DynamicPrintConfig new_conf = *m_config;
    new_conf.set_key_value(opt_key, new ConfigOptionStrings(gcodes));
    load_config(new_conf);
}

void TabFilament::add_filament_overrides_page()
{
    //BBS
    PageShp page = add_options_page(L("Setting Overrides"), "custom-gcode_setting_override"); // ORCA: icon only visible on placeholders
    ConfigOptionsGroupShp optgroup = page->new_optgroup(L("Retraction"), L"param_retraction");

    auto append_single_option_line = [optgroup, this](const std::string& opt_key, int opt_index)
    {
        Line line {"",""};
        //BBS
        line = optgroup->create_single_option_line(optgroup->get_option(opt_key));

        line.near_label_widget = [this, optgroup_wk = ConfigOptionsGroupWkp(optgroup), opt_key, opt_index](wxWindow* parent) {
            auto check_box = new ::CheckBox(parent); // ORCA modernize checkboxes
            check_box->Bind(wxEVT_TOGGLEBUTTON, [this, optgroup_wk, opt_key, opt_index](wxCommandEvent& evt) {
                const bool is_checked = evt.IsChecked();
                if (auto optgroup_sh = optgroup_wk.lock(); optgroup_sh) {
                    if (Field *field = optgroup_sh->get_fieldc(opt_key, opt_index); field != nullptr) {
                        field->toggle(is_checked);

                        if (is_checked) {
                            field->update_na_value(_(L("N/A")));
                            field->set_last_meaningful_value();
                        }
                        else {
                            const std::string printer_opt_key = opt_key.substr(strlen("filament_"));
                            const auto printer_config = m_preset_bundle->printers.get_edited_preset().config;
                            const boost::any printer_config_value = optgroup_sh->get_config_value(printer_config, printer_opt_key, opt_index);
                            field->update_na_value(printer_config_value);
                            field->set_na_value();
                        }
                    }
                }
            }, check_box->GetId());

            m_overrides_options[opt_key] = check_box;
            return check_box;
        };

        optgroup->append_line(line);
    };

    const int extruder_idx = 0; // #ys_FIXME

    for (const std::string opt_key : {  "filament_retraction_length",
                                        "filament_z_hop",
                                        "filament_z_hop_types",
                                        "filament_retract_lift_above",
                                        "filament_retract_lift_below",
                                        "filament_retract_lift_enforce",
                                        "filament_retraction_speed",
                                        "filament_deretraction_speed",
                                        "filament_retract_restart_extra",
                                        "filament_retraction_minimum_travel",
                                        "filament_retract_when_changing_layer",
                                        "filament_wipe",
                                        //BBS
                                        "filament_wipe_distance",
                                        "filament_retract_before_wipe",
                                        "filament_long_retractions_when_cut",
                                        "filament_retraction_distances_when_cut",
                                        "filament_retract_length_toolchange",
                                        "filament_retract_restart_extra_toolchange"
                                        //SoftFever
                                        // "filament_seam_gap"
                                     })
        append_single_option_line(opt_key, extruder_idx);
}

void TabFilament::update_filament_overrides_page(const DynamicPrintConfig* printers_config)
{
    if (!m_active_page || m_active_page->title() != "Setting Overrides")
        return;

    //BBS: GUI refactor
    if (m_overrides_options.size() <= 0)
        return;

    Page* page = m_active_page;

    const auto og_it = std::find_if(page->m_optgroups.begin(), page->m_optgroups.end(), [](const ConfigOptionsGroupShp og) { return og->title == "Retraction"; });
    if (og_it == page->m_optgroups.end())
        return;
    ConfigOptionsGroupShp optgroup = *og_it;

    std::vector<std::string> opt_keys = {   "filament_retraction_length",
                                            "filament_z_hop",
                                            "filament_z_hop_types",
                                            "filament_retract_lift_above",
                                            "filament_retract_lift_below",
                                            "filament_retract_lift_enforce",
                                            "filament_retraction_speed",
                                            "filament_deretraction_speed",
                                            "filament_retract_restart_extra",
                                            "filament_retraction_minimum_travel",
                                            "filament_retract_when_changing_layer",
                                            "filament_wipe",
                                            //BBS
                                            "filament_wipe_distance",
                                            "filament_retract_before_wipe",
                                            "filament_long_retractions_when_cut",
                                            "filament_retraction_distances_when_cut",
                                            "filament_retract_length_toolchange",
                                            "filament_retract_restart_extra_toolchange"
                                            //SoftFever
                                            // "filament_seam_gap"
                                        };

    const int extruder_idx = 0; // #ys_FIXME

    const bool have_retract_length = m_config->option("filament_retraction_length")->is_nil() ||
                                     m_config->opt_float("filament_retraction_length", extruder_idx) > 0;

    for (const std::string& opt_key : opt_keys)
    {
        bool is_checked = opt_key=="filament_retraction_length" ? true : have_retract_length;
        m_overrides_options[opt_key]->Enable(is_checked);

        is_checked &= !m_config->option(opt_key)->is_nil();
        m_overrides_options[opt_key]->SetValue(is_checked);

        Field* field = optgroup->get_fieldc(opt_key, extruder_idx);
        if (field == nullptr) continue;

        if (opt_key == "filament_long_retractions_when_cut") {
            int machine_enabled_level = printers_config->option<ConfigOptionInt>(
                "enable_long_retraction_when_cut")->value;
            bool machine_enabled = machine_enabled_level == LongRectrationLevel::EnableFilament;
            toggle_line(opt_key, machine_enabled);
            field->toggle(is_checked && machine_enabled);
        } else if (opt_key == "filament_retraction_distances_when_cut") {
            int machine_enabled_level = printers_config->option<ConfigOptionInt>(
                "enable_long_retraction_when_cut")->value;
            bool machine_enabled = machine_enabled_level == LongRectrationLevel::EnableFilament;
            bool filament_enabled = m_config->option<ConfigOptionBools>("filament_long_retractions_when_cut")->values[extruder_idx] == 1;
            toggle_line(opt_key, filament_enabled && machine_enabled);
            field->toggle(is_checked && filament_enabled && machine_enabled);
        } else {
            if (!is_checked) {
                const std::string printer_opt_key = opt_key.substr(strlen("filament_"));
                boost::any printer_config_value = optgroup->get_config_value(*printers_config, printer_opt_key, extruder_idx);
                field->update_na_value(printer_config_value);
                field->set_value(printer_config_value, false);
            }

            field->toggle(is_checked);
        }
    }
}

void TabFilament::build()
{
    m_presets = &m_preset_bundle->filaments;
    load_initial_data();

    auto page = add_options_page(L("Filament"), "custom-gcode_filament"); // ORCA: icon only visible on placeholders
        //BBS
        auto optgroup = page->new_optgroup(L("Basic information"), L"param_information");
        optgroup->append_single_option_line("filament_type"); // ORCA use same width with other elements
        optgroup->append_single_option_line("filament_vendor");
        optgroup->append_single_option_line("filament_soluble");
        // BBS
        optgroup->append_single_option_line("filament_is_support");
        //optgroup->append_single_option_line("filament_colour");
        optgroup->append_single_option_line("required_nozzle_HRC");
        optgroup->append_single_option_line("default_filament_colour");
        optgroup->append_single_option_line("filament_diameter");

        optgroup->append_single_option_line("filament_density");
        optgroup->append_single_option_line("filament_shrink");
        optgroup->append_single_option_line("filament_shrinkage_compensation_z");
        optgroup->append_single_option_line("filament_cost");
        //BBS
        optgroup->append_single_option_line("temperature_vitrification");
        optgroup->append_single_option_line("idle_temperature");
        Line line = { L("Recommended nozzle temperature"), L("Recommended nozzle temperature range of this filament. 0 means no set") };
        line.append_option(optgroup->get_option("nozzle_temperature_range_low"));
        line.append_option(optgroup->get_option("nozzle_temperature_range_high"));
        optgroup->append_line(line);

        optgroup->m_on_change = [this, optgroup](t_config_option_key opt_key, boost::any value) {
            DynamicPrintConfig &filament_config = wxGetApp().preset_bundle->filaments.get_edited_preset().config;

            update_dirty();
            if (!m_postpone_update_ui && (opt_key == "nozzle_temperature_range_low" || opt_key == "nozzle_temperature_range_high")) {
                m_config_manipulation.check_nozzle_recommended_temperature_range(&filament_config);
            }
            on_value_change(opt_key, value);
        };

        // Orca: New section to focus on flow rate and PA to declutter general section
        optgroup = page->new_optgroup(L("Flow ratio and Pressure Advance"), L"param_flow_ratio_and_pressure_advance");
        optgroup->append_single_option_line("pellet_flow_coefficient", "pellet-flow-coefficient");
        optgroup->append_single_option_line("filament_flow_ratio");

        optgroup->append_single_option_line("enable_pressure_advance", "pressure-advance-calib");
        optgroup->append_single_option_line("pressure_advance", "pressure-advance-calib");

        // Orca: adaptive pressure advance and calibration model
        optgroup->append_single_option_line("adaptive_pressure_advance", "adaptive-pressure-advance-calib");
        optgroup->append_single_option_line("adaptive_pressure_advance_overhangs", "adaptive-pressure-advance-calib");
        optgroup->append_single_option_line("adaptive_pressure_advance_bridges", "adaptive-pressure-advance-calib");

        Option option = optgroup->get_option("adaptive_pressure_advance_model");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = 15;
        optgroup->append_single_option_line(option);
        //

        optgroup = page->new_optgroup(L("Print chamber temperature"), L"param_chamber_temp");
        optgroup->append_single_option_line("chamber_temperature", "chamber-temperature");
        optgroup->append_single_option_line("activate_chamber_temp_control", "chamber-temperature");

        optgroup->append_separator();


        optgroup = page->new_optgroup(L("Print temperature"), L"param_extruder_temp");
        line = { L("Nozzle"), L("Nozzle temperature when printing") };
        line.append_option(optgroup->get_option("nozzle_temperature_initial_layer"));
        line.append_option(optgroup->get_option("nozzle_temperature"));
        optgroup->append_line(line);

        optgroup = page->new_optgroup(L("Bed temperature"), L"param_bed_temp");
        line = { L("Cool Plate (SuperTack)"),
                 L("Bed temperature when the Cool Plate SuperTack is installed. A value of 0 means the filament does not support printing on the Cool Plate SuperTack.") };
        line.append_option(optgroup->get_option("supertack_plate_temp_initial_layer"));
        line.append_option(optgroup->get_option("supertack_plate_temp"));
        optgroup->append_line(line);

        line = { L("Cool Plate"),
                 L("Bed temperature when the Cool Plate is installed. A value of 0 means the filament does not support printing on the Cool Plate.") };
        line.append_option(optgroup->get_option("cool_plate_temp_initial_layer"));
        line.append_option(optgroup->get_option("cool_plate_temp"));
        optgroup->append_line(line);

        line = { L("Textured Cool Plate"),
                 L("Bed temperature when the Textured Cool Plate is installed. A value of 0 means the filament does not support printing on the Textured Cool Plate.") };
        line.append_option(optgroup->get_option("textured_cool_plate_temp_initial_layer"));
        line.append_option(optgroup->get_option("textured_cool_plate_temp"));
        optgroup->append_line(line);

        line = { L("Engineering Plate"),
                 L("Bed temperature when the Engineering Plate is installed. A value of 0 means the filament does not support printing on the Engineering Plate.") };
        line.append_option(optgroup->get_option("eng_plate_temp_initial_layer"));
        line.append_option(optgroup->get_option("eng_plate_temp"));
        optgroup->append_line(line);

        line = { L("Smooth PEI Plate / High Temp Plate"),
                 L("Bed temperature when the Smooth PEI Plate/High Temperature Plate is installed. A value of 0 means the filament does not support printing on the Smooth PEI Plate/High Temp Plate.") };
        line.append_option(optgroup->get_option("hot_plate_temp_initial_layer"));
        line.append_option(optgroup->get_option("hot_plate_temp"));
        optgroup->append_line(line);

        line = { L("Textured PEI Plate"),
                 L("Bed temperature when the Textured PEI Plate is installed. A value of 0 means the filament does not support printing on the Textured PEI Plate.") };
        line.append_option(optgroup->get_option("textured_plate_temp_initial_layer"));
        line.append_option(optgroup->get_option("textured_plate_temp"));
        optgroup->append_line(line);

        optgroup->m_on_change = [this](t_config_option_key opt_key, boost::any value)
        {
            DynamicPrintConfig& filament_config = wxGetApp().preset_bundle->filaments.get_edited_preset().config;

            update_dirty();
            /*if (opt_key == "cool_plate_temp" || opt_key == "cool_plate_temp_initial_layer") {
                m_config_manipulation.check_bed_temperature_difference(BedType::btPC, &filament_config);
            }
            else if (opt_key == "eng_plate_temp" || opt_key == "eng_plate_temp_initial_layer") {
                m_config_manipulation.check_bed_temperature_difference(BedType::btEP, &filament_config);
            }
            else if (opt_key == "hot_plate_temp" || opt_key == "hot_plate_temp_initial_layer") {
                m_config_manipulation.check_bed_temperature_difference(BedType::btPEI, &filament_config);
            }
            else if (opt_key == "textured_plate_temp" || opt_key == "textured_plate_temp_initial_layer") {
                m_config_manipulation.check_bed_temperature_difference(BedType::btPTE, &filament_config);
            }
            else */if (opt_key == "nozzle_temperature") {
                m_config_manipulation.check_nozzle_temperature_range(&filament_config);
            }
            else if (opt_key == "nozzle_temperature_initial_layer") {
                m_config_manipulation.check_nozzle_temperature_initial_layer_range(&filament_config);
            }
            else if (opt_key == "chamber_temperatures") {
                m_config_manipulation.check_chamber_temperature(&filament_config);
            }

            on_value_change(opt_key, value);
        };

        //BBS
        optgroup = page->new_optgroup(L("Volumetric speed limitation"), L"param_volumetric_speed");
        optgroup->append_single_option_line("filament_max_volumetric_speed");

        //line = { "", "" };
        //line.full_width = 1;
        //line.widget = [this](wxWindow* parent) {
        //    return description_line_widget(parent, &m_volumetric_speed_description_line);
        //};
        //optgroup->append_line(line);

    page = add_options_page(L("Cooling"), "custom-gcode_cooling_fan"); // ORCA: icon only visible on placeholders

        //line = { "", "" };
        //line.full_width = 1;
        //line.widget = [this](wxWindow* parent) {
        //    return description_line_widget(parent, &m_cooling_description_line);
        //};
        //optgroup->append_line(line);
        optgroup = page->new_optgroup(L("Cooling for specific layer"), L"param_cooling_specific_layer");
        optgroup->append_single_option_line("close_fan_the_first_x_layers");
        optgroup->append_single_option_line("full_fan_speed_layer");

        optgroup = page->new_optgroup(L("Part cooling fan"), L"param_cooling_part_fan");
        line = { L("Min fan speed threshold"), L("Part cooling fan speed will start to run at min speed when the estimated layer time is no longer than the layer time in setting. When layer time is shorter than threshold, fan speed is interpolated between the minimum and maximum fan speed according to layer printing time") };
        line.label_path = "auto-cooling";
        line.append_option(optgroup->get_option("fan_min_speed"));
        line.append_option(optgroup->get_option("fan_cooling_layer_time"));
        optgroup->append_line(line);
        line = { L("Max fan speed threshold"), L("Part cooling fan speed will be max when the estimated layer time is shorter than the setting value") };
        line.label_path = "auto-cooling";
        line.append_option(optgroup->get_option("fan_max_speed"));
        line.append_option(optgroup->get_option("slow_down_layer_time"));
        optgroup->append_line(line);
        optgroup->append_single_option_line("reduce_fan_stop_start_freq");
        optgroup->append_single_option_line("slow_down_for_layer_cooling");
        optgroup->append_single_option_line("dont_slow_down_outer_wall");
        optgroup->append_single_option_line("slow_down_min_speed");

        optgroup->append_single_option_line("enable_overhang_bridge_fan");
        optgroup->append_single_option_line("overhang_fan_threshold");
        optgroup->append_single_option_line("overhang_fan_speed");
        optgroup->append_single_option_line("internal_bridge_fan_speed"); // ORCA: Add support for separate internal bridge fan speed control
        optgroup->append_single_option_line("support_material_interface_fan_speed");
        optgroup->append_single_option_line("ironing_fan_speed"); // ORCA: Add support for ironing fan speed control

        optgroup = page->new_optgroup(L("Auxiliary part cooling fan"), L"param_cooling_aux_fan");
        optgroup->append_single_option_line("additional_cooling_fan_speed", "auxiliary-fan");

        optgroup = page->new_optgroup(L("Exhaust fan"),L"param_cooling_exhaust");

        optgroup->append_single_option_line("activate_air_filtration", "air-filtration");

        line = {L("During print"), ""};
        line.append_option(optgroup->get_option("during_print_exhaust_fan_speed"));
        optgroup->append_line(line);


        line = {L("Complete print"), ""};
        line.append_option(optgroup->get_option("complete_print_exhaust_fan_speed"));
        optgroup->append_line(line);
        //BBS
        add_filament_overrides_page();
        const int gcode_field_height = 15; // 150
        const int notes_field_height = 25; // 250

        auto edit_custom_gcode_fn = [this](const t_config_option_key& opt_key) { edit_custom_gcode(opt_key); };

    page = add_options_page(L("Advanced"), "custom-gcode_advanced"); // ORCA: icon only visible on placeholders
        optgroup = page->new_optgroup(L("Filament start G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("filament_start_gcode");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;// 150;
        optgroup->append_single_option_line(option);

        optgroup = page->new_optgroup(L("Filament end G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("filament_end_gcode");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;// 150;
        optgroup->append_single_option_line(option);

    page = add_options_page(L("Multimaterial"), "custom-gcode_multi_material"); // ORCA: icon only visible on placeholders
        optgroup = page->new_optgroup(L("Wipe tower parameters"), "param_tower");
        optgroup->append_single_option_line("filament_minimal_purge_on_wipe_tower");

        optgroup = page->new_optgroup(L("Tool change parameters with single extruder MM printers"), "param_toolchange");
        optgroup->append_single_option_line("filament_loading_speed_start", "semm");
        optgroup->append_single_option_line("filament_loading_speed", "semm");
        optgroup->append_single_option_line("filament_unloading_speed_start", "semm");
        optgroup->append_single_option_line("filament_unloading_speed", "semm");
        optgroup->append_single_option_line("filament_toolchange_delay", "semm");
        optgroup->append_single_option_line("filament_cooling_moves", "semm");
        optgroup->append_single_option_line("filament_cooling_initial_speed", "semm");
        optgroup->append_single_option_line("filament_cooling_final_speed", "semm");
        optgroup->append_single_option_line("filament_stamping_loading_speed");
        optgroup->append_single_option_line("filament_stamping_distance");
        create_line_with_widget(optgroup.get(), "filament_ramming_parameters", "", [this](wxWindow* parent) {

            // ORCA modernize button style
            Button* btn = new Button(parent, _(L("Set")) + " " + dots);
            btn->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);

            auto sizer = new wxBoxSizer(wxHORIZONTAL);
            sizer->Add(btn);

            btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent& e) {
                RammingDialog dlg(this,(m_config->option<ConfigOptionStrings>("filament_ramming_parameters"))->get_at(0));
                if (dlg.ShowModal() == wxID_OK) {
                    load_key_value("filament_ramming_parameters", dlg.get_parameters());
                    update_changed_ui();
                }
            });
            return sizer;
        });

        optgroup = page->new_optgroup(L("Tool change parameters with multi extruder MM printers"), "param_toolchange_multi_extruder");
        optgroup->append_single_option_line("filament_multitool_ramming");
        optgroup->append_single_option_line("filament_multitool_ramming_volume");
        optgroup->append_single_option_line("filament_multitool_ramming_flow");

    page = add_options_page(L("Dependencies"), "advanced");
        optgroup = page->new_optgroup(L("Compatible printers"), "param_dependencies_printers");
        create_line_with_widget(optgroup.get(), "compatible_printers", "", [this](wxWindow* parent) {
            return compatible_widget_create(parent, m_compatible_printers);
        });

        option = optgroup->get_option("compatible_printers_condition");
        option.opt.full_width = true;
        optgroup->append_single_option_line(option);

        optgroup = page->new_optgroup(L("Compatible process profiles"), "param_dependencies_presets");
        create_line_with_widget(optgroup.get(), "compatible_prints", "", [this](wxWindow* parent) {
            return compatible_widget_create(parent, m_compatible_prints);
        });

        option = optgroup->get_option("compatible_prints_condition");
        option.opt.full_width = true;
        optgroup->append_single_option_line(option);

    page = add_options_page(L("Notes"), "custom-gcode_note"); // ORCA: icon only visible on placeholders
        optgroup = page->new_optgroup(L("Notes"),"note", 0);
        optgroup->label_width = 0;
        option = optgroup->get_option("filament_notes");
        option.opt.full_width = true;
        option.opt.height = notes_field_height;// 250;
        optgroup->append_single_option_line(option);

        //build_preset_description_line(optgroup.get());
}

// Reload current config (aka presets->edited_preset->config) into the UI fields.
void TabFilament::reload_config()
{
    this->compatible_widget_reload(m_compatible_printers);
    this->compatible_widget_reload(m_compatible_prints);
    Tab::reload_config();
}

//void TabFilament::update_volumetric_flow_preset_hints()
//{
//    wxString text;
//    try {
//        text = from_u8(PresetHints::maximum_volumetric_flow_description(*m_preset_bundle));
//    } catch (std::exception &ex) {
//        text = _(L("Volumetric flow hints not available")) + "\n\n" + from_u8(ex.what());
//    }
//    m_volumetric_speed_description_line->SetText(text);
//}

void TabFilament::update_description_lines()
{
    Tab::update_description_lines();

    if (!m_active_page)
        return;

    if (m_active_page->title() == "Cooling" && m_cooling_description_line)
        m_cooling_description_line->SetText(from_u8(PresetHints::cooling_description(m_presets->get_edited_preset())));
    //BBS
    //if (m_active_page->title() == "Filament" && m_volumetric_speed_description_line)
    //    this->update_volumetric_flow_preset_hints();
}

void TabFilament::toggle_options()
{
    if (!m_active_page)
        return;
    bool is_BBL_printer = false;
    if (m_preset_bundle) {
      is_BBL_printer =
          wxGetApp().preset_bundle->is_bbl_vendor();
    }

    auto cfg = m_preset_bundle->printers.get_edited_preset().config;
    if (m_active_page->title() == L("Cooling")) {
      bool has_enable_overhang_bridge_fan = m_config->opt_bool("enable_overhang_bridge_fan", 0);
      for (auto el : {"overhang_fan_speed", "overhang_fan_threshold", "internal_bridge_fan_speed"}) // ORCA: Add support for separate internal bridge fan speed control
            toggle_option(el, has_enable_overhang_bridge_fan);

      toggle_option("additional_cooling_fan_speed", cfg.opt_bool("auxiliary_fan"));

      // Orca: toggle dont slow down for external perimeters if
      bool has_slow_down_for_layer_cooling = m_config->opt_bool("slow_down_for_layer_cooling", 0);
      toggle_option("dont_slow_down_outer_wall", has_slow_down_for_layer_cooling);
    }
    if (m_active_page->title() == L("Filament"))
    {
        bool pa = m_config->opt_bool("enable_pressure_advance", 0);
        toggle_option("pressure_advance", pa);
        
        // BBS: 控制床温选项的显示
        auto support_multi_bed_types = is_BBL_printer || cfg.opt_bool("support_multi_bed_types");
        if (support_multi_bed_types) {
            // 支持多床型：显示所有床温选项
            toggle_line("supertack_plate_temp_initial_layer", true);
            toggle_line("cool_plate_temp", true);
            toggle_line("cool_plate_temp_initial_layer", true);
            toggle_line("cool_plate_temp", true);
            toggle_line("textured_cool_plate_temp_initial_layer", true);
            toggle_line("textured_cool_plate_temp", true);
            toggle_line("eng_plate_temp_initial_layer", true);
            toggle_line("eng_plate_temp", true);
            toggle_line("hot_plate_temp_initial_layer", true);
            toggle_line("hot_plate_temp", true);
            toggle_line("textured_plate_temp_initial_layer", true);
            toggle_line("textured_plate_temp", true);
        } else {
            // 不支持多床型：只显示当前选择的床型
            
            BedType curr_bed_type = m_preset_bundle->printers.get_edited_preset().get_default_bed_type(m_preset_bundle);
           
            toggle_line("supertack_plate_temp_initial_layer", curr_bed_type == btSuperTack);
            toggle_line("supertack_plate_temp", curr_bed_type == btSuperTack);
            toggle_line("cool_plate_temp_initial_layer", curr_bed_type == btPC);
            toggle_line("cool_plate_temp", curr_bed_type == btPC);
            toggle_line("textured_cool_plate_temp_initial_layer", curr_bed_type == btPCT);
            toggle_line("textured_cool_plate_temp", curr_bed_type == btPCT);
            toggle_line("eng_plate_temp_initial_layer", curr_bed_type == btEP);
            toggle_line("eng_plate_temp", curr_bed_type == btEP);
            toggle_line("hot_plate_temp_initial_layer", curr_bed_type == btPEI);
            toggle_line("hot_plate_temp", curr_bed_type == btPEI);
            toggle_line("textured_plate_temp_initial_layer", curr_bed_type == btPTE);
            toggle_line("textured_plate_temp", curr_bed_type == btPTE);
        }



        // Orca: adaptive pressure advance and calibration model
        // If PA is not enabled, disable adaptive pressure advance and hide the model section
        // If adaptive PA is not enabled, hide the adaptive PA model section
        toggle_option("adaptive_pressure_advance", pa);
        toggle_option("adaptive_pressure_advance_overhangs", pa);
        bool has_adaptive_pa = m_config->opt_bool("adaptive_pressure_advance", 0);
        toggle_line("adaptive_pressure_advance_overhangs", has_adaptive_pa && pa);
        toggle_line("adaptive_pressure_advance_model", has_adaptive_pa && pa);
        toggle_line("adaptive_pressure_advance_bridges", has_adaptive_pa && pa);

        bool is_pellet_printer = cfg.opt_bool("pellet_modded_printer");
        toggle_line("pellet_flow_coefficient", is_pellet_printer);
        toggle_line("filament_diameter", !is_pellet_printer);

        bool support_chamber_temp_control = this->m_preset_bundle->printers.get_edited_preset().config.opt_bool("support_chamber_temp_control");
        toggle_line("chamber_temperatures", support_chamber_temp_control);
    }
    if (m_active_page->title() == L("Setting Overrides"))
        update_filament_overrides_page(&cfg);

    if (m_active_page->title() == L("Multimaterial")) {
        // Orca: hide specific settings for BBL printers
        for (auto el : {"filament_minimal_purge_on_wipe_tower", "filament_loading_speed_start", "filament_loading_speed",
                        "filament_unloading_speed_start", "filament_unloading_speed", "filament_toolchange_delay", "filament_cooling_moves",
                        "filament_cooling_initial_speed", "filament_cooling_final_speed"})
            toggle_option(el, !is_BBL_printer);

        bool multitool_ramming = m_config->opt_bool("filament_multitool_ramming", 0);
        toggle_option("filament_multitool_ramming_volume", multitool_ramming);
        toggle_option("filament_multitool_ramming_flow", multitool_ramming);
    }
}

void TabFilament::update()
{
    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptSLA)
        return; // ys_FIXME

    m_config_manipulation.check_filament_max_volumetric_speed(m_config);

    m_update_cnt++;

    update_description_lines();
    //BBS: GUI refactor
    //Layout();
    m_parent->Layout();

    toggle_options();

    m_update_cnt--;

    if (m_update_cnt == 0)
        wxGetApp().mainframe->on_config_changed(m_config);
}

void TabFilament::clear_pages()
{
    Tab::clear_pages();

    m_volumetric_speed_description_line = nullptr;
	m_cooling_description_line = nullptr;

    //BBS: GUI refactor
    m_overrides_options.clear();
}

wxSizer* Tab::description_line_widget(wxWindow* parent, ogStaticText* *StaticText, wxString text /*= wxEmptyString*/)
{
    *StaticText = new ogStaticText(parent, text);

//	auto font = (new wxSystemSettings)->GetFont(wxSYS_DEFAULT_GUI_FONT);
    (*StaticText)->SetFont(wxGetApp().normal_font());

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(*StaticText, 1, wxEXPAND|wxALL, 0);
    return sizer;
}

bool Tab::saved_preset_is_dirty() const { return m_presets->saved_is_dirty(); }
void Tab::update_saved_preset_from_current_preset() { m_presets->update_saved_preset_from_current_preset(); }
bool Tab::current_preset_is_dirty() const { return m_presets->current_is_dirty(); }

void TabPrinter::build()
{
    m_presets = &m_preset_bundle->printers;
    m_printer_technology = m_presets->get_selected_preset().printer_technology();

    // For DiffPresetDialog we use options list which is saved in Searcher class.
    // Options for the Searcher is added in the moment of pages creation.
    // So, build first of all printer pages for non-selected printer technology...
    //std::string def_preset_name = "- default " + std::string(m_printer_technology == ptSLA ? "FFF" : "SLA") + " -";
    std::string def_preset_name = "Default Printer";
    m_config = &m_presets->find_preset(def_preset_name)->config;
    m_printer_technology == ptSLA ? build_fff() : build_sla();
    if (m_printer_technology == ptSLA)
        m_extruders_count_old = 0;// revert this value

    // ... and than for selected printer technology
    load_initial_data();
    m_printer_technology == ptSLA ? build_sla() : build_fff();
}

void TabPrinter::build_fff()
{
    if (!m_pages.empty())
        m_pages.resize(0);
    // to avoid redundant memory allocation / deallocation during extruders count changing
    m_pages.reserve(30);

    auto   *nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(m_config->option("nozzle_diameter"));
    m_initial_extruders_count = m_extruders_count = nozzle_diameter->values.size();
    // BBS
    //wxGetApp().obj_list()->update_objects_list_filament_column(m_initial_extruders_count);

    const Preset* parent_preset = m_printer_technology == ptSLA ? nullptr // just for first build, if SLA printer preset is selected
                                  : m_presets->get_selected_preset_parent();
    m_sys_extruders_count = parent_preset == nullptr ? 0 :
            static_cast<const ConfigOptionFloats*>(parent_preset->config.option("nozzle_diameter"))->values.size();

    auto page = add_options_page(L("Basic information"), "custom-gcode_object-info"); // ORCA: icon only visible on placeholders
    auto optgroup = page->new_optgroup(L("Printable space"), "param_printable_space");

        create_line_with_widget(optgroup.get(), "printable_area", "custom-svg-and-png-bed-textures_124612", [this](wxWindow* parent) {
           return 	create_bed_shape_widget(parent);
        });
        Option option = optgroup->get_option("bed_exclude_area");
        option.opt.full_width = true;
        optgroup->append_single_option_line(option);
        // optgroup->append_single_option_line("printable_area");
        optgroup->append_single_option_line("printable_height");
        optgroup->append_single_option_line("support_multi_bed_types","bed-types");
        optgroup->append_single_option_line("nozzle_volume");
        optgroup->append_single_option_line("best_object_pos");
        optgroup->append_single_option_line("z_offset");
        optgroup->append_single_option_line("preferred_orientation");

        optgroup = page->new_optgroup(L("Advanced"), L"param_advanced");
        optgroup->append_single_option_line("printer_structure");
        optgroup->append_single_option_line("gcode_flavor");
        optgroup->append_single_option_line("pellet_modded_printer", "pellet-flow-coefficient");
        optgroup->append_single_option_line("bbl_use_printhost");
        optgroup->append_single_option_line("disable_m73");
        option = optgroup->get_option("thumbnails");
        option.opt.full_width = true;
        optgroup->append_single_option_line(option);
        // optgroup->append_single_option_line("thumbnails_format");
        optgroup->m_on_change = [this](t_config_option_key opt_key, boost::any value) {
            wxTheApp->CallAfter([this, opt_key, value]() {
                if (opt_key == "thumbnails" && m_config->has("thumbnails_format")) {
                    // to backward compatibility we need to update "thumbnails_format" from new "thumbnails"
                    const std::string val = boost::any_cast<std::string>(value);
                    if (!value.empty()) {
                        auto [thumbnails_list, errors] = GCodeThumbnails::make_and_check_thumbnail_list(val);

                        if (errors != enum_bitmask<ThumbnailError>()) {
                            // TRN: The first argument is the parameter's name; the second argument is its value.
                            std::string error_str = format(_u8L("Invalid value provided for parameter %1%: %2%"), "thumbnails", val);
                            error_str += GCodeThumbnails::get_error_string(errors);
                            InfoDialog(parent(), _L("G-code flavor is switched"), from_u8(error_str)).ShowModal();
                        }

                        if (!thumbnails_list.empty()) {
                            GCodeThumbnailsFormat old_format = GCodeThumbnailsFormat(m_config->option("thumbnails_format")->getInt());
                            GCodeThumbnailsFormat new_format = thumbnails_list.begin()->first;
                            if (old_format != new_format) {
                                DynamicPrintConfig new_conf = *m_config;

                                auto* opt = m_config->option("thumbnails_format")->clone();
                                opt->setInt(int(new_format));
                                new_conf.set_key_value("thumbnails_format", opt);

                                load_config(new_conf);
                            }
                        }
                    }
                }

                update_dirty();
                on_value_change(opt_key, value);
            });
        };

        optgroup->append_single_option_line("use_relative_e_distances");
        optgroup->append_single_option_line("use_firmware_retraction");
        // optgroup->append_single_option_line("spaghetti_detector");
        optgroup->append_single_option_line("time_cost");

        optgroup  = page->new_optgroup(L("Cooling Fan"), "param_cooling_fan");
        Line line = Line{ L("Fan speed-up time"), optgroup->get_option("fan_speedup_time").opt.tooltip };
        line.append_option(optgroup->get_option("fan_speedup_time"));
        line.append_option(optgroup->get_option("fan_speedup_overhangs"));
        optgroup->append_line(line);
        optgroup->append_single_option_line("fan_kickstart");

        optgroup = page->new_optgroup(L("Extruder Clearance"), "param_extruder_clearence");
        optgroup->append_single_option_line("extruder_clearance_radius");
        optgroup->append_single_option_line("extruder_clearance_height_to_rod");
        optgroup->append_single_option_line("extruder_clearance_height_to_lid");

        optgroup = page->new_optgroup(L("Adaptive bed mesh"), "param_adaptive_mesh");
        optgroup->append_single_option_line("bed_mesh_min", "adaptive-bed-mesh");
        optgroup->append_single_option_line("bed_mesh_max", "adaptive-bed-mesh");
        optgroup->append_single_option_line("bed_mesh_probe_distance", "adaptive-bed-mesh");
        optgroup->append_single_option_line("adaptive_bed_mesh_margin", "adaptive-bed-mesh");

        optgroup = page->new_optgroup(L("Accessory"), "param_accessory");
        optgroup->append_single_option_line("nozzle_type");
        optgroup->append_single_option_line("nozzle_hrc");
        optgroup->append_single_option_line("auxiliary_fan", "auxiliary-fan");
        optgroup->append_single_option_line("support_chamber_temp_control", "chamber-temperature");
        optgroup->append_single_option_line("support_air_filtration", "air-filtration");

        auto edit_custom_gcode_fn = [this](const t_config_option_key& opt_key) { edit_custom_gcode(opt_key); };

    const int gcode_field_height = 15; // 150
    const int notes_field_height = 25; // 250
    page = add_options_page(L("Machine G-code"), "custom-gcode_gcode"); // ORCA: icon only visible on placeholders
        optgroup = page->new_optgroup(L("Machine start G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("machine_start_gcode");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;//150;
        optgroup->append_single_option_line(option);

        optgroup = page->new_optgroup(L("Machine end G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("machine_end_gcode");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;//150;
        optgroup->append_single_option_line(option);

        optgroup              = page->new_optgroup(L("Printing by object G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, optgroup](const t_config_option_key &opt_key, const boost::any &value) {
            validate_custom_gcode_cb(this, optgroup, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option                = optgroup->get_option("printing_by_object_gcode");
        option.opt.full_width = true;
        option.opt.is_code    = true;
        option.opt.height     = gcode_field_height; // 150;
        optgroup->append_single_option_line(option);


        optgroup = page->new_optgroup(L("Before layer change G-code"),"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("before_layer_change_gcode");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;//150;
        optgroup->append_single_option_line(option);

        optgroup = page->new_optgroup(L("Layer change G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("layer_change_gcode");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;//150;
        optgroup->append_single_option_line(option);

        optgroup = page->new_optgroup(L("Timelapse G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("time_lapse_gcode");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;//150;
        optgroup->append_single_option_line(option);

        optgroup = page->new_optgroup(L("Change filament G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("change_filament_gcode");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;//150;
        optgroup->append_single_option_line(option);

        optgroup = page->new_optgroup(L("Change extrusion role G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key &opt_key, const boost::any &value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("change_extrusion_role_gcode");
        option.opt.full_width = true;
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;//150;
        optgroup->append_single_option_line(option);

        optgroup = page->new_optgroup(L("Pause G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("machine_pause_gcode");
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;//150;
        optgroup->append_single_option_line(option);

        optgroup = page->new_optgroup(L("Template Custom G-code"), L"param_gcode", 0);
        optgroup->m_on_change = [this, &optgroup_title = optgroup->title](const t_config_option_key& opt_key, const boost::any& value) {
            validate_custom_gcode_cb(this, optgroup_title, opt_key, value);
        };
        optgroup->edit_custom_gcode = edit_custom_gcode_fn;
        option = optgroup->get_option("template_custom_gcode");
        option.opt.is_code = true;
        option.opt.height = gcode_field_height;//150;
        optgroup->append_single_option_line(option);

    page = add_options_page(L("Notes"), "custom-gcode_note"); // ORCA: icon only visible on placeholders
        optgroup = page->new_optgroup(L("Notes"), "note", 0);
        option = optgroup->get_option("printer_notes");
        option.opt.full_width = true;
        option.opt.height = notes_field_height;//250;
        optgroup->append_single_option_line(option);
#if 0
    //page = add_options_page(L("Dependencies"), "advanced");
    //    optgroup = page->new_optgroup(L("Profile dependencies"));

    //    build_preset_description_line(optgroup.get());
#endif
    build_unregular_pages(true);
}

void TabPrinter::build_sla()
{
    //if (!m_pages.empty())
    //    m_pages.resize(0);
    //auto page = add_options_page(L("General"), "printer");
    //auto optgroup = page->new_optgroup(L("Size and coordinates"));

    //create_line_with_widget(optgroup.get(), "printable_area", "custom-svg-and-png-bed-textures_124612", [this](wxWindow* parent) {
    //    return 	create_bed_shape_widget(parent);
    //});
    //optgroup->append_single_option_line("printable_height");

    //optgroup = page->new_optgroup(L("Display"));
    //optgroup->append_single_option_line("display_width");
    //optgroup->append_single_option_line("display_height");

    //auto option = optgroup->get_option("display_pixels_x");
    //Line line = { option.opt.full_label, "" };
    //line.append_option(option);
    //line.append_option(optgroup->get_option("display_pixels_y"));
    //optgroup->append_line(line);
    //optgroup->append_single_option_line("display_orientation");

    //// FIXME: This should be on one line in the UI
    //optgroup->append_single_option_line("display_mirror_x");
    //optgroup->append_single_option_line("display_mirror_y");

    //optgroup = page->new_optgroup(L("Tilt"));
    //line = { L("Tilt time"), "" };
    //line.append_option(optgroup->get_option("fast_tilt_time"));
    //line.append_option(optgroup->get_option("slow_tilt_time"));
    //optgroup->append_line(line);
    //optgroup->append_single_option_line("area_fill");

    //optgroup = page->new_optgroup(L("Corrections"));
    //line = Line{ m_config->def()->get("relative_correction")->full_label, "" };
    //for (auto& axis : { "X", "Y", "Z" }) {
    //    auto opt = optgroup->get_option(std::string("relative_correction_") + char(std::tolower(axis[0])));
    //    opt.opt.label = axis;
    //    line.append_option(opt);
    //}
    //optgroup->append_line(line);
    //optgroup->append_single_option_line("absolute_correction");
    //optgroup->append_single_option_line("elefant_foot_compensation");
    //optgroup->append_single_option_line("elefant_foot_min_width");
    //optgroup->append_single_option_line("gamma_correction");
    //
    //optgroup = page->new_optgroup(L("Exposure"));
    //optgroup->append_single_option_line("min_exposure_time");
    //optgroup->append_single_option_line("max_exposure_time");
    //optgroup->append_single_option_line("min_initial_exposure_time");
    //optgroup->append_single_option_line("max_initial_exposure_time");

    //page = add_options_page(L("Dependencies"), "wrench.png");
    //optgroup = page->new_optgroup(L("Profile dependencies"));

    //build_preset_description_line(optgroup.get());
}

void TabPrinter::extruders_count_changed(size_t extruders_count)
{
    bool is_count_changed = false;
    if (m_extruders_count != extruders_count) {
        m_extruders_count = extruders_count;
        m_preset_bundle->printers.get_edited_preset().set_num_extruders(extruders_count);
        m_preset_bundle->update_multi_material_filament_presets();
        is_count_changed = true;
    }
    // Orca: support multi tool
    else if (m_extruders_count == 1 &&
             m_preset_bundle->project_config.option<ConfigOptionFloats>("flush_volumes_matrix")->values.size()>1)
        m_preset_bundle->update_multi_material_filament_presets();

    /* This function should be call in any case because of correct updating/rebuilding
     * of unregular pages of a Printer Settings
     */
    build_unregular_pages();

    if (is_count_changed) {
        on_value_change("extruders_count", extruders_count);
        // BBS
        //wxGetApp().obj_list()->update_objects_list_filament_column(extruders_count);
    }
}

void TabPrinter::append_option_line(ConfigOptionsGroupShp optgroup, const std::string opt_key)
{
    auto option = optgroup->get_option(opt_key, 0);
    auto line = Line{ option.opt.full_label, "" };
    line.append_option(option);
    if (m_use_silent_mode
        || m_printer_technology == ptSLA // just for first build, if SLA printer preset is selected
        )
        line.append_option(optgroup->get_option(opt_key, 1));
    optgroup->append_line(line);
}

PageShp TabPrinter::build_kinematics_page()
{
    auto page = add_options_page(L("Motion ability"), "custom-gcode_motion", true); // ORCA: icon only visible on placeholders

    if (m_use_silent_mode) {
        // Legend for OptionsGroups
        auto optgroup = page->new_optgroup("");
        auto line = Line{ "", "" };

        ConfigOptionDef def;
        def.type = coString;
        def.width = Field::def_width();
        def.gui_type = ConfigOptionDef::GUIType::legend;
        def.mode = comDevelop;
        //def.tooltip = L("Values in this column are for Normal mode");
        def.set_default_value(new ConfigOptionString{ _(L("Normal")).ToUTF8().data() });

        auto option = Option(def, "full_power_legend");
        line.append_option(option);

        //def.tooltip = L("Values in this column are for Stealth mode");
        def.set_default_value(new ConfigOptionString{ _(L("Silent")).ToUTF8().data() });
        option = Option(def, "silent_legend");
        line.append_option(option);

        optgroup->append_line(line);
    }
    auto optgroup = page->new_optgroup(L("Advanced"), "param_advanced");
    optgroup->append_single_option_line("emit_machine_limits_to_gcode");

    // resonance avoidance ported over from qidi slicer
    optgroup = page->new_optgroup(L("Resonance Avoidance"), "param_resonance_avoidance");
    optgroup->append_single_option_line("resonance_avoidance");
    // Resonance‑avoidance speed inputs
    {
        Line resonance_line = {L("Resonance Avoidance Speed"), L""};
        resonance_line.append_option(optgroup->get_option("min_resonance_avoidance_speed"));
        resonance_line.append_option(optgroup->get_option("max_resonance_avoidance_speed"));
        optgroup->append_line(resonance_line);
    }

    const std::vector<std::string> speed_axes{
        "machine_max_speed_x",
        "machine_max_speed_y",
        "machine_max_speed_z",
        "machine_max_speed_e"
    };
    optgroup = page->new_optgroup(L("Speed limitation"), "param_speed");
        for (const std::string &speed_axis : speed_axes)	{
            append_option_line(optgroup, speed_axis);
        }

    const std::vector<std::string> axes{ "x", "y", "z", "e" };
        optgroup = page->new_optgroup(L("Acceleration limitation"), "param_acceleration");
        for (const std::string &axis : axes)	{
            append_option_line(optgroup, "machine_max_acceleration_" + axis);
        }
        append_option_line(optgroup, "machine_max_acceleration_extruding");
        append_option_line(optgroup, "machine_max_acceleration_retracting");
        append_option_line(optgroup, "machine_max_acceleration_travel");

        optgroup = page->new_optgroup(L("Jerk limitation"), "param_jerk");
        for (const std::string &axis : axes)	{
            append_option_line(optgroup, "machine_max_jerk_" + axis);
        }

        // machine max junction deviation
         append_option_line(optgroup, "machine_max_junction_deviation");
    //optgroup = page->new_optgroup(L("Minimum feedrates"));
    //    append_option_line(optgroup, "machine_min_extruding_rate");
    //    append_option_line(optgroup, "machine_min_travel_rate");

    return page;
}

/* Previous name build_extruder_pages().
 *
 * This function was renamed because of now it implements not just an extruder pages building,
 * but "Motion ability" and "Single extruder MM setup" too
 * (These pages can changes according to the another values of a current preset)
 * */
void TabPrinter::build_unregular_pages(bool from_initial_build/* = false*/)
{
    size_t		n_before_extruders = 2;			//	Count of pages before Extruder pages
    auto        flavor = m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    bool		is_marlin_flavor = (flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware || flavor == gcfKlipper || flavor == gcfRepRapFirmware);

    /* ! Freeze/Thaw in this function is needed to avoid call OnPaint() for erased pages
     * and be cause of application crash, when try to change Preset in moment,
     * when one of unregular pages is selected.
     *  */
    Freeze();

    // Add/delete Kinematics page according to is_marlin_flavor
    size_t existed_page = 0;
    for (size_t i = n_before_extruders; i < m_pages.size(); ++i) // first make sure it's not there already
        if (m_pages[i]->title().find(L("Motion ability")) != std::string::npos) {
            if (m_rebuild_kinematics_page)
                m_pages.erase(m_pages.begin() + i);
            else
                existed_page = i;
            break;
        }

    if (existed_page < n_before_extruders && (is_marlin_flavor || from_initial_build)) {
        auto page = build_kinematics_page();
        if (from_initial_build && !is_marlin_flavor)
            page->clear();
        else
            m_pages.insert(m_pages.begin() + n_before_extruders, page);
    }

if (is_marlin_flavor)
    n_before_extruders++;
    size_t		n_after_single_extruder_MM = 2; //	Count of pages after single_extruder_multi_material page

    if (from_initial_build) {
        // create a page, but pretend it's an extruder page, so we can add it to m_pages ourselves
        auto page     = add_options_page(L("Multimaterial"), "custom-gcode_multi_material", true); // ORCA: icon only visible on placeholders
        auto optgroup = page->new_optgroup(L("Single extruder multi-material setup"), "param_multi_material");
        optgroup->append_single_option_line("single_extruder_multi_material", "semm");
        ConfigOptionDef def;
        def.type    = coInt, def.set_default_value(new ConfigOptionInt((int) m_extruders_count));
        def.label   = L("Extruders");
        def.tooltip = L("Number of extruders of the printer.");
        def.min     = 1;
        def.max     = MAXIMUM_EXTRUDER_NUMBER;
        def.mode    = comAdvanced;
        Option option(def, "extruders_count");
        optgroup->append_single_option_line(option);

        // Orca: rebuild missed extruder pages
        optgroup->m_on_change = [this, optgroup_wk = ConfigOptionsGroupWkp(optgroup)](t_config_option_key opt_key, boost::any value) {
            auto optgroup_sh = optgroup_wk.lock();
            if (!optgroup_sh)
                return;

            // optgroup->get_value() return int for def.type == coInt,
            // Thus, there should be boost::any_cast<int> !
            // Otherwise, boost::any_cast<size_t> causes an "unhandled unknown exception"
            const auto v = optgroup_sh->get_value("extruders_count");
            if (v.empty()) return;
            size_t extruders_count = size_t(boost::any_cast<int>(v));
            wxTheApp->CallAfter([this, opt_key, value, extruders_count]() {
                if (opt_key == "extruders_count" || opt_key == "single_extruder_multi_material") {
                    extruders_count_changed(extruders_count);
                    init_options_list(); // m_options_list should be updated before UI updating
                    update_dirty();
                    if (opt_key == "single_extruder_multi_material") { // the single_extruder_multimaterial was added to force pages
                        on_value_change(opt_key, value);                      // rebuild - let's make sure the on_value_change is not skipped

                        if (boost::any_cast<bool>(value) && m_extruders_count > 1) {
                            SuppressBackgroundProcessingUpdate sbpu;

// Orca: we use a different logic here. If SEMM is enabled, we set extruder count to 1.
#if 1
                            extruders_count_changed(1);
#else

                            std::vector<double> nozzle_diameters =
                                static_cast<const ConfigOptionFloats*>(m_config->option("nozzle_diameter"))->values;
                            const double frst_diam = nozzle_diameters[0];

                            for (auto cur_diam : nozzle_diameters) {
                                // if value is differs from first nozzle diameter value
                                if (fabs(cur_diam - frst_diam) > EPSILON) {
                                    const wxString msg_text = _(
                                        L("Single Extruder Multi Material is selected, \n"
                                          "and all extruders must have the same diameter.\n"
                                          "Do you want to change the diameter for all extruders to first extruder nozzle diameter value?"));
                                    MessageDialog dialog(parent(), msg_text, _(L("Nozzle diameter")), wxICON_WARNING | wxYES_NO);

                                    DynamicPrintConfig new_conf = *m_config;
                                    if (dialog.ShowModal() == wxID_YES) {
                                        for (size_t i = 1; i < nozzle_diameters.size(); i++)
                                            nozzle_diameters[i] = frst_diam;

                                        new_conf.set_key_value("nozzle_diameter", new ConfigOptionFloats(nozzle_diameters));
                                    } else
                                        new_conf.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));

                                    load_config(new_conf);
                                    break;
                                }
                            }
#endif
                        }

                        m_preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
                        // Upadte related comboboxes on Sidebar and Tabs
                        Sidebar& sidebar = wxGetApp().plater()->sidebar();
                        for (const Preset::Type& type : {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT}) {
                            sidebar.update_presets(type);
                            wxGetApp().get_tab(type)->update_tab_ui();
                        }
                    }
                }
                else {
                    update_dirty();
                    on_value_change(opt_key, value);
                }
            });
        };
        optgroup->append_single_option_line("manual_filament_change", "semm#manual-filament-change");

        optgroup = page->new_optgroup(L("Wipe tower"), "param_tower");
        optgroup->append_single_option_line("purge_in_prime_tower", "semm");
        optgroup->append_single_option_line("enable_filament_ramming", "semm");
        optgroup->append_single_option_line("ramming_line_width_ratio");
        optgroup->append_single_option_line("enable_change_pressure_when_wiping");
        optgroup->append_single_option_line("ramming_pressure_advance_value");

         // 添加依赖关系处理
        optgroup->m_on_change = [this, optgroup](t_config_option_key opt_key, boost::any value) {
            if (opt_key == "enable_change_pressure_when_wiping") {
                bool enable_pressure = boost::any_cast<bool>(value);
                // 根据enable_change_pressure_when_wiping的值来启用/禁用ramming_pressure_advance_value
                optgroup->enable_field("ramming_pressure_advance_value", enable_pressure);
            }
            
            // 调用原有的更新逻辑
            update_dirty();
            on_value_change(opt_key, value);
        };


        optgroup = page->new_optgroup(L("Single extruder multi-material parameters"), "param_settings");
        optgroup->append_single_option_line("cooling_tube_retraction", "semm");
        optgroup->append_single_option_line("cooling_tube_length", "semm");
        optgroup->append_single_option_line("parking_pos_retraction", "semm");
        optgroup->append_single_option_line("extra_loading_move", "semm");
        optgroup->append_single_option_line("high_current_on_filament_swap", "semm");

        optgroup = page->new_optgroup(L("Advanced"), L"param_advanced");
        optgroup->append_single_option_line("machine_load_filament_time");
        optgroup->append_single_option_line("machine_unload_filament_time");
        optgroup->append_single_option_line("machine_tool_change_time");
        optgroup->append_single_option_line("tool_change_temprature_wait");

        m_pages.insert(m_pages.end() - n_after_single_extruder_MM, page);
    }

    // Orca: build missed extruder pages
    for (auto extruder_idx = m_extruders_count_old; extruder_idx < m_extruders_count; ++extruder_idx) {
        // auto extruder_idx = 0;
        const wxString& page_name = wxString::Format(_L("Extruder %d"), int(extruder_idx + 1));
        bool page_exist = false;
        for (auto page_temp : m_pages) {
            if (page_temp->title() == page_name) {
                page_exist = true;
                break;
            }
        }

        if (!page_exist)
        {
            //# build page
            //const wxString& page_name = wxString::Format(_L("Extruder %d"), int(extruder_idx + 1));
            auto page = add_options_page(page_name, "custom-gcode_extruder", true); // ORCA: icon only visible on placeholders
            m_pages.insert(m_pages.begin() + n_before_extruders + extruder_idx, page);

                auto optgroup = page->new_optgroup(L("Size"), L"param_extruder_size");
                optgroup->append_single_option_line("nozzle_diameter", "", extruder_idx);

                optgroup->m_on_change = [this, extruder_idx](const t_config_option_key& opt_key, boost::any value)
                {
                    bool is_SEMM = m_config->opt_bool("single_extruder_multi_material");
                    if (is_SEMM && m_extruders_count > 1 && opt_key.find_first_of("nozzle_diameter") != std::string::npos)
                    {
                        SuppressBackgroundProcessingUpdate sbpu;
                        const double new_nd = boost::any_cast<double>(value);
                        std::vector<double> nozzle_diameters = static_cast<const ConfigOptionFloats*>(m_config->option("nozzle_diameter"))->values;

                        // if value was changed
                        if (fabs(nozzle_diameters[extruder_idx == 0 ? 1 : 0] - new_nd) > EPSILON)
                        {
                            const wxString msg_text = _(L("This is a single extruder multi-material printer, diameters of all extruders "
                                "will be set to the new value. Do you want to proceed?"));
                            //wxMessageDialog dialog(parent(), msg_text, _(L("Nozzle diameter")), wxICON_WARNING | wxYES_NO);
                            MessageDialog dialog(parent(), msg_text, _(L("Nozzle diameter")), wxICON_WARNING | wxYES_NO);

                            DynamicPrintConfig new_conf = *m_config;
                            if (dialog.ShowModal() == wxID_YES) {
                                for (size_t i = 0; i < nozzle_diameters.size(); i++) {
                                    if (i == extruder_idx)
                                        continue;
                                    nozzle_diameters[i] = new_nd;
                                }
                            }
                            else
                                nozzle_diameters[extruder_idx] = nozzle_diameters[extruder_idx == 0 ? 1 : 0];

                            new_conf.set_key_value("nozzle_diameter", new ConfigOptionFloats(nozzle_diameters));
                            load_config(new_conf);
                        }
                    }

                    update_dirty();
                    update();
                };

                optgroup = page->new_optgroup(L("Layer height limits"), L"param_layer_height");
                optgroup->append_single_option_line("min_layer_height", "", extruder_idx);
                optgroup->append_single_option_line("max_layer_height", "", extruder_idx);

                optgroup = page->new_optgroup(L("Position"), L"param_position");
                optgroup->append_single_option_line("extruder_offset", "", extruder_idx);

                //BBS: don't show retract related config menu in machine page
                optgroup = page->new_optgroup(L("Retraction"), L"param_retraction");
                optgroup->append_single_option_line("retraction_length", "", extruder_idx);
                optgroup->append_single_option_line("retract_restart_extra", "", extruder_idx);
                optgroup->append_single_option_line("retraction_speed", "", extruder_idx);
                optgroup->append_single_option_line("deretraction_speed", "", extruder_idx);
                optgroup->append_single_option_line("retraction_minimum_travel", "", extruder_idx);
                optgroup->append_single_option_line("retract_when_changing_layer", "", extruder_idx);
                optgroup->append_single_option_line("wipe", "", extruder_idx);
                optgroup->append_single_option_line("wipe_distance", "", extruder_idx);
                optgroup->append_single_option_line("retract_before_wipe", "", extruder_idx);

                optgroup = page->new_optgroup(L("Z-Hop"), L"param_extruder_lift_enforcement");
                optgroup->append_single_option_line("retract_lift_enforce", "", extruder_idx);
                optgroup->append_single_option_line("z_hop_types", "", extruder_idx);
                optgroup->append_single_option_line("z_hop", "", extruder_idx);
                optgroup->append_single_option_line("z_hop_when_prime", "", extruder_idx);
                optgroup->append_single_option_line("travel_slope", "", extruder_idx);
                optgroup->append_single_option_line("retract_lift_above", "", extruder_idx);
                optgroup->append_single_option_line("retract_lift_below", "", extruder_idx);

                optgroup = page->new_optgroup(L("Retraction when switching material"), L"param_retraction_material_change");
                optgroup->append_single_option_line("retract_length_toolchange", "", extruder_idx);
                optgroup->append_single_option_line("retract_restart_extra_toolchange", "", extruder_idx);
                // do not display this params now
                optgroup->append_single_option_line("long_retractions_when_cut", "", extruder_idx);
                optgroup->append_single_option_line("retraction_distances_when_cut", "", extruder_idx);

    #if 0
                //optgroup = page->new_optgroup(L("Preview"), -1, true);

                //auto reset_to_filament_color = [this, extruder_idx](wxWindow* parent) {
                //    m_reset_to_filament_color = new ScalableButton(parent, wxID_ANY, "undo", _L("Reset to Filament Color"),
                //                                                   wxDefaultSize, wxDefaultPosition, wxBU_LEFT | wxBU_EXACTFIT, true);
                //    ScalableButton* btn = m_reset_to_filament_color;
                //    btn->SetFont(Slic3r::GUI::wxGetApp().normal_font());
                //    btn->SetSize(btn->GetBestSize());
                //    auto sizer = new wxBoxSizer(wxHORIZONTAL);
                //    sizer->Add(btn);

                //    btn->Bind(wxEVT_BUTTON, [this, extruder_idx](wxCommandEvent& e)
                //    {
                //        std::vector<std::string> colors = static_cast<const ConfigOptionStrings*>(m_config->option("extruder_colour"))->values;
                //        colors[extruder_idx] = "";

                //        DynamicPrintConfig new_conf = *m_config;
                //        new_conf.set_key_value("extruder_colour", new ConfigOptionStrings(colors));
                //        load_config(new_conf);

                //        update_dirty();
                //        update();
                //    });

                //    return sizer;
                //};
                ////BBS
                //Line line = optgroup->create_single_option_line("extruder_colour", "", extruder_idx);
                //line.append_widget(reset_to_filament_color);
                //optgroup->append_line(line);
    #endif
        }
}
    // BBS. No extra extruder page for single physical extruder machine
    // # remove extra pages
    if (m_extruders_count < m_extruders_count_old)
        m_pages.erase(	m_pages.begin() + n_before_extruders + m_extruders_count,
                        m_pages.begin() + n_before_extruders + m_extruders_count_old);

    Thaw();

    m_extruders_count_old = m_extruders_count;

    if (from_initial_build && m_printer_technology == ptSLA)
        return; // next part of code is no needed to execute at this moment

    rebuild_page_tree();

    // Reload preset pages with current configuration values
    reload_config();

    // apply searcher with current configuration
    apply_searcher();
}

// this gets executed after preset is loaded and before GUI fields are updated
void TabPrinter::on_preset_loaded()
{
    // Orca
    // update the extruders count field
    auto   *nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(m_config->option("nozzle_diameter"));
    size_t extruders_count = nozzle_diameter->values.size();
    // update the GUI field according to the number of nozzle diameters supplied
    extruders_count_changed(extruders_count);
}

void TabPrinter::update_pages()
{
    // update m_pages ONLY if printer technology is changed
    const PrinterTechnology new_printer_technology = m_presets->get_edited_preset().printer_technology();
    if (new_printer_technology == m_printer_technology)
        return;

    //clear all active pages before switching
    clear_pages();

    // set m_pages to m_pages_(technology before changing)
    m_printer_technology == ptFFF ? m_pages.swap(m_pages_fff) : m_pages.swap(m_pages_sla);

    // build Tab according to the technology, if it's not exist jet OR
    // set m_pages_(technology after changing) to m_pages
    // m_printer_technology will be set by Tab::load_current_preset()
    if (new_printer_technology == ptFFF)
    {
        if (m_pages_fff.empty())
        {
            build_fff();
            if (m_extruders_count > 1)
            {
                m_preset_bundle->update_multi_material_filament_presets();
                on_value_change("extruders_count", m_extruders_count);
            }
        }
        else
            m_pages.swap(m_pages_fff);

         wxGetApp().obj_list()->update_objects_list_filament_column(m_extruders_count);
    }
    else
        m_pages_sla.empty() ? build_sla() : m_pages.swap(m_pages_sla);

    rebuild_page_tree();
}

void TabPrinter::reload_config()
{
    Tab::reload_config();

    // "extruders_count" doesn't update from the update_config(),
    // so update it implicitly
    if (m_active_page && m_active_page->title() == "Multimaterial")
        m_active_page->set_value("extruders_count", int(m_extruders_count));
}

void TabPrinter::activate_selected_page(std::function<void()> throw_if_canceled)
{
    Tab::activate_selected_page(throw_if_canceled);

    // "extruders_count" doesn't update from the update_config(),
    // so update it implicitly
    if (m_active_page && m_active_page->title() == "Multimaterial")
        m_active_page->set_value("extruders_count", int(m_extruders_count));
}

void TabPrinter::clear_pages()
{
    Tab::clear_pages();
    m_reset_to_filament_color = nullptr;
}

void TabPrinter::toggle_options()
{
    if (!m_active_page || m_presets->get_edited_preset().printer_technology() == ptSLA)
        return;

    //BBS: whether the preset is Bambu Lab printer
    bool is_BBL_printer = false;
    if (m_preset_bundle) {
       is_BBL_printer = wxGetApp().preset_bundle->is_bbl_vendor();
    }

    bool have_multiple_extruders = true;
    //m_extruders_count > 1;
    //if (m_active_page->title() == "Custom G-code") {
    //    toggle_option("change_filament_gcode", have_multiple_extruders);
    //}
    if (m_active_page->title() == L("Basic information")) {

        // SoftFever: hide BBL specific settings
        for (auto el : {"scan_first_layer", "bbl_calib_mark_logo", "bbl_use_printhost"})
            toggle_line(el, is_BBL_printer);

        // SoftFever: hide non-BBL settings
        for (auto el : {"use_firmware_retraction", "use_relative_e_distances", "support_multi_bed_types", "pellet_modded_printer", "bed_mesh_max", "bed_mesh_min", "bed_mesh_probe_distance", "adaptive_bed_mesh_margin", "thumbnails"})
          toggle_line(el, !is_BBL_printer);
    }

    if (m_active_page->title() == L("Multimaterial")) {
        // SoftFever: hide specific settings for BBL printer
        for (auto el : {
                 "enable_filament_ramming",
                 "cooling_tube_retraction",
                 "cooling_tube_length",
                 "parking_pos_retraction",
                 "extra_loading_move",
                 "high_current_on_filament_swap",
             })
            toggle_option(el, !is_BBL_printer);

        auto bSEMM = m_config->opt_bool("single_extruder_multi_material");
        if (!bSEMM && m_config->opt_bool("manual_filament_change")) {
            DynamicPrintConfig new_conf = *m_config;
            new_conf.set_key_value("manual_filament_change", new ConfigOptionBool(false));
            load_config(new_conf);
        }
        toggle_option("extruders_count", !bSEMM);
        toggle_option("manual_filament_change", bSEMM);
        toggle_option("purge_in_prime_tower", bSEMM && !is_BBL_printer);
    }
    wxString extruder_number;
    long val = 1;
    if ( m_active_page->title().IsSameAs(L("Extruder")) ||
        (m_active_page->title().StartsWith("Extruder ", &extruder_number) && extruder_number.ToLong(&val) &&
        val > 0 && (size_t)val <= m_extruders_count))
    {
        size_t i = size_t(val - 1);
        bool have_retract_length = m_config->opt_float("retraction_length", i) > 0;

        // when using firmware retraction, firmware decides retraction length
        bool use_firmware_retraction = m_config->opt_bool("use_firmware_retraction");
        toggle_option("retract_length", !use_firmware_retraction, i);

        // user can customize travel length if we have retraction length or we"re using
        // firmware retraction
        toggle_option("retraction_minimum_travel", have_retract_length || use_firmware_retraction, i);

        // user can customize other retraction options if retraction is enabled
        //BBS
        bool retraction = have_retract_length || use_firmware_retraction;
        std::vector<std::string> vec = {"z_hop", "retract_when_changing_layer"};
        for (auto el : vec)
            toggle_option(el, retraction, i);

        // retract lift above / below + enforce only applies if using retract lift
        vec.resize(0);
        vec = {"retract_lift_above", "retract_lift_below", "retract_lift_enforce"};
        for (auto el : vec)
          toggle_option(el, retraction && (m_config->opt_float("z_hop", i) > 0), i);

        // some options only apply when not using firmware retraction
        vec.resize(0);
        vec = {"retraction_speed", "deretraction_speed",    "retract_before_wipe",
               "retract_length",   "retract_restart_extra", "wipe",
               "wipe_distance"};
        for (auto el : vec)
            //BBS
            toggle_option(el, retraction && !use_firmware_retraction, i);

        bool wipe = retraction && m_config->opt_bool("wipe", i);
        toggle_option("retract_before_wipe", wipe, i);
        if (use_firmware_retraction && wipe) {
            //wxMessageDialog dialog(parent(),
            MessageDialog dialog(parent(),
                _(L("The Wipe option is not available when using the Firmware Retraction mode.\n"
                    "\nShall I disable it in order to enable Firmware Retraction?")),
                _(L("Firmware Retraction")), wxICON_WARNING | wxYES | wxNO);

            DynamicPrintConfig new_conf = *m_config;
            if (dialog.ShowModal() == wxID_YES) {
                auto wipe = static_cast<ConfigOptionBools*>(m_config->option("wipe")->clone());
                for (size_t w = 0; w < wipe->values.size(); w++)
                    wipe->values[w] = false;
                new_conf.set_key_value("wipe", wipe);
            }
            else {
                new_conf.set_key_value("use_firmware_retraction", new ConfigOptionBool(false));
            }
            load_config(new_conf);
        }
        // BBS
        toggle_option("wipe_distance", wipe, i);

        toggle_option("retract_length_toolchange", have_multiple_extruders, i);

        bool toolchange_retraction = m_config->opt_float("retract_length_toolchange", i) > 0;
        toggle_option("retract_restart_extra_toolchange", have_multiple_extruders && toolchange_retraction, i);

        toggle_option("long_retractions_when_cut", !use_firmware_retraction && m_config->opt_int("enable_long_retraction_when_cut"),i);
        toggle_line("retraction_distances_when_cut#0", m_config->opt_bool("long_retractions_when_cut", i));
        //toggle_option("retraction_distances_when_cut", m_config->opt_bool("long_retractions_when_cut",i),i);

        toggle_option("travel_slope", m_config->opt_enum("z_hop_types", i) != ZHopType::zhtNormal, i);
    }

    if (m_active_page->title() == L("Motion ability")) {
        auto gcf = m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
        bool silent_mode = m_config->opt_bool("silent_mode");
        int  max_field   = silent_mode ? 2 : 1;
        for (int i = 0; i < max_field; ++i)
            toggle_option("machine_max_acceleration_travel", gcf != gcfMarlinLegacy && gcf != gcfKlipper, i);
        toggle_line("machine_max_acceleration_travel", gcf != gcfMarlinLegacy && gcf != gcfKlipper);
        for (int i = 0; i < max_field; ++i)
            toggle_option("machine_max_junction_deviation", gcf == gcfMarlinFirmware, i);
        toggle_line("machine_max_junction_deviation", gcf == gcfMarlinFirmware);

        bool resonance_avoidance = m_config->opt_bool("resonance_avoidance");
        toggle_option("min_resonance_avoidance_speed", resonance_avoidance);
        toggle_option("max_resonance_avoidance_speed", resonance_avoidance);
    }
}

void TabPrinter::update()
{
    m_update_cnt++;
    m_presets->get_edited_preset().printer_technology() == ptFFF ? update_fff() : update_sla();
    m_update_cnt--;

    update_description_lines();
    //BBS: GUI refactor
    //Layout();
    m_parent->Layout();

    if (m_update_cnt == 0)
        wxGetApp().mainframe->on_config_changed(m_config);
}

void TabPrinter::update_fff()
{
    if (m_use_silent_mode != m_config->opt_bool("silent_mode"))	{
        m_rebuild_kinematics_page = true;
        m_use_silent_mode = m_config->opt_bool("silent_mode");
    }

    toggle_options();
}

void TabPrinter::update_sla()
{ ; }

void Tab::update_ui_items_related_on_parent_preset(const Preset* selected_preset_parent)
{
    m_is_default_preset = selected_preset_parent != nullptr && selected_preset_parent->is_default;

    m_bmp_non_system = selected_preset_parent ? &m_bmp_value_unlock : &m_bmp_white_bullet;
    m_ttg_non_system = selected_preset_parent ? &m_ttg_value_unlock : &m_ttg_white_bullet_ns;
    m_tt_non_system  = selected_preset_parent ? &m_tt_value_unlock  : &m_ttg_white_bullet_ns;
}

//BBS: reactive the preset combo box
void Tab::reactive_preset_combo_box()
{
    if (!m_presets_choice) return;
    //BBS: add workaround to fix the issue caused by wxwidget 3.15 upgrading
    m_presets_choice->Enable(false);
    m_presets_choice->Enable(true);
}

// Initialize the UI from the current preset
void Tab::load_current_preset()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<boost::format(": enter");
    const Preset& preset = m_presets->get_edited_preset();

    update_btns_enabling();

    update();
    if (m_type == Slic3r::Preset::TYPE_PRINTER) {
        // For the printer profile, generate the extruder pages.
        if (preset.printer_technology() == ptFFF)
            on_preset_loaded();
        else
            wxGetApp().obj_list()->update_objects_list_filament_column(1);
    }

    // Reload preset pages with the new configuration values.
    reload_config();

    update_ui_items_related_on_parent_preset(m_presets->get_selected_preset_parent());

//	m_undo_to_sys_btn->Enable(!preset.is_default);

#if 0
    // use CallAfter because some field triggers schedule on_change calls using CallAfter,
    // and we don't want them to be called after this update_dirty() as they would mark the
    // preset dirty again
    // (not sure this is true anymore now that update_dirty is idempotent)
    wxTheApp->CallAfter([this]
#endif
    {
        // checking out if this Tab exists till this moment
        if (!wxGetApp().checked_tab(this))
            return;
        update_tab_ui();

        // update show/hide tabs
        if (m_type == Slic3r::Preset::TYPE_PRINTER) {
            const PrinterTechnology printer_technology = m_presets->get_edited_preset().printer_technology();
            if (printer_technology != static_cast<TabPrinter*>(this)->m_printer_technology)
            {
                // The change of the technology requires to remove some of unrelated Tabs
                // During this action, wxNoteBook::RemovePage invoke wxEVT_NOTEBOOK_PAGE_CHANGED
                // and as a result a function select_active_page() is called fron Tab::OnActive()
                // But we don't need it. So, to avoid activation of the page, set m_active_page to NULL
                // till unusable Tabs will be deleted
                Page* tmp_page = m_active_page;
                m_active_page = nullptr;
                for (auto tab : wxGetApp().tabs_list) {
                    if (tab->type() == Preset::TYPE_PRINTER) { // Printer tab is shown every time
                        int cur_selection = wxGetApp().tab_panel()->GetSelection();
                        if (cur_selection != 0)
                            wxGetApp().tab_panel()->SetSelection(wxGetApp().tab_panel()->GetPageCount() - 1);
                        continue;
                    }
                    if (tab->supports_printer_technology(printer_technology))
                    {
#ifdef _MSW_DARK_MODE
                        if (!wxGetApp().tabs_as_menu()) {
                            std::string bmp_name = tab->type() == Slic3r::Preset::TYPE_FILAMENT      ? "spool" :
                                                   tab->type() == Slic3r::Preset::TYPE_SLA_MATERIAL  ? "" : "cog";
                            tab->Hide(); // #ys_WORKAROUND : Hide tab before inserting to avoid unwanted rendering of the tab
                            dynamic_cast<Notebook*>(wxGetApp().tab_panel())->InsertPage(wxGetApp().tab_panel()->FindPage(this), tab, tab->title(), bmp_name);
                        }
                        else
#endif
                            wxGetApp().tab_panel()->InsertPage(wxGetApp().tab_panel()->FindPage(this), tab, tab->title(), "");
                        #ifdef __linux__ // the tabs apparently need to be explicitly shown on Linux (pull request #1563)
                            int page_id = wxGetApp().tab_panel()->FindPage(tab);
                            wxGetApp().tab_panel()->GetPage(page_id)->Show(true);
                        #endif // __linux__
                    }
                    else {
                        int page_id = wxGetApp().tab_panel()->FindPage(tab);
                        wxGetApp().tab_panel()->GetPage(page_id)->Show(false);
                        wxGetApp().tab_panel()->RemovePage(page_id);
                    }
                }
                static_cast<TabPrinter*>(this)->m_printer_technology = printer_technology;
                m_active_page = tmp_page;
#ifdef _MSW_DARK_MODE
                if (!wxGetApp().tabs_as_menu())
                    dynamic_cast<Notebook*>(wxGetApp().tab_panel())->SetPageImage(wxGetApp().tab_panel()->FindPage(this), printer_technology == ptFFF ? "printer" : "sla_printer");
#endif
            }
            on_presets_changed();
            if (printer_technology == ptFFF) {
                static_cast<TabPrinter*>(this)->m_initial_extruders_count = static_cast<const ConfigOptionFloats*>(m_presets->get_selected_preset().config.option("nozzle_diameter"))->values.size(); //static_cast<TabPrinter*>(this)->m_extruders_count;
                const Preset* parent_preset = m_presets->get_selected_preset_parent();
                static_cast<TabPrinter*>(this)->m_sys_extruders_count = parent_preset == nullptr ? 0 :
                    static_cast<const ConfigOptionFloats*>(parent_preset->config.option("nozzle_diameter"))->values.size();
            }
        }
        else {
            on_presets_changed();
            if (m_type == Preset::TYPE_SLA_PRINT || m_type == Preset::TYPE_PRINT)
                update_frequently_changed_parameters();
        }
        m_opt_status_value = (m_presets->get_selected_preset_parent() ? osSystemValue : 0) | osInitValue;
        init_options_list();
        update_visibility();
        update_changed_ui();
    }
#if 0
    );
#endif
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<<boost::format(": exit");
}

//Regerenerate content of the page tree.
void Tab::rebuild_page_tree()
{
    // get label of the currently selected item
    auto sel_item = m_tabctrl->GetSelection();
    // BBS: fix new layout, record last select
    if (sel_item < 0)
        sel_item = m_last_select_item;
    const auto selected = sel_item >= 0 ? m_tabctrl->GetItemText(sel_item) : "";

    int item = -1;

    // Delete/Append events invoke wxEVT_TAB_SEL_CHANGED event.
    // To avoid redundant clear/activate functions call
    // suppress activate page before page_tree rebuilding
    m_disable_tree_sel_changed_event = true;

    int curr_item = 0;
    for (auto p : m_pages)
    {
        if (!p->get_show())
            continue;
        if (m_tabctrl->GetCount() <= curr_item) {
            m_tabctrl->AppendItem(translate_category(p->title(), m_type), p->iconID());
        } else {
            m_tabctrl->SetItemText(curr_item, translate_category(p->title(), m_type));
        }
        m_tabctrl->SetItemTextColour(curr_item, p->get_item_colour() == m_modified_label_clr ? p->get_item_colour() : StateColor(
                        std::make_pair(0x6B6B6C, (int) StateColor::NotChecked),
                        std::make_pair(p->get_item_colour(), (int) StateColor::Normal)));
        if (translate_category(p->title(), m_type) == selected)
            item = curr_item;
        curr_item++;
    }
    while (m_tabctrl->GetCount() > curr_item) {
        m_tabctrl->DeleteItem(m_tabctrl->GetCount() - 1);
    }

    // BBS: on mac, root is selected, this fix it
    m_tabctrl->Unselect();
    // BBS: not select on hide tab
    if (item == -1 && m_parent->is_active_and_shown_tab(this)) {
        // this is triggered on first load, so we don't disable the sel change event
        item = m_tabctrl->GetFirstVisibleItem();
    }
    // BBS: fix new layout, record last select
    if (sel_item == m_last_select_item)
        m_last_select_item = item;
    else
        m_last_select_item = NULL;

    // allow activate page before selection of a page_tree item
    m_disable_tree_sel_changed_event = false;
    //BBS: GUI refactor
    if (item >= 0)
    {
        bool ret = update_current_page_in_background(item);
        //if m_active_page is changed in update_current_page_in_background
        //will just update the selected item of the treectrl
         if (m_parent->is_active_and_shown_tab(this)) // FIX: modify state not update
            m_tabctrl->SelectItem(item);
    }
}

void Tab::update_btns_enabling()
{
    // we can delete any preset from the physical printer
    // and any user preset
    const Preset& preset = m_presets->get_edited_preset();
    m_btn_delete_preset->Show((m_type == Preset::TYPE_PRINTER && m_preset_bundle->physical_printers.has_selection())
                              || (!preset.is_default && !preset.is_system));

    //if (m_btn_edit_ph_printer)
    //    m_btn_edit_ph_printer->SetToolTip( m_preset_bundle->physical_printers.has_selection() ?
    //                                       _L("Edit physical printer") : _L("Add physical printer"));
}

void Tab::update_preset_choice()
{
    if (m_presets_choice)
        m_presets_choice->update();
    update_btns_enabling();
}

// Called by the UI combo box when the user switches profiles, and also to delete the current profile.
// Select a preset by a name.If !defined(name), then the default preset is selected.
// If the current profile is modified, user is asked to save the changes.
bool Tab::select_preset(std::string preset_name, bool delete_current /*=false*/, const std::string& last_selected_ph_printer_name/* =""*/, bool force_select)
{
    BOOST_LOG_TRIVIAL(info) << boost::format("select preset, name %1%, delete_current %2%")
        %preset_name %delete_current;
    if (preset_name.empty()) {
        if (delete_current) {
            // Find an alternate preset to be selected after the current preset is deleted.
            const std::deque<Preset> &presets 		= m_presets->get_presets();
            size_t    				  idx_current   = m_presets->get_idx_selected();
            // Find the next visible preset.
            size_t 				      idx_new       = idx_current + 1;
            if (idx_new < presets.size())
                for (; idx_new < presets.size() && ! presets[idx_new].is_visible; ++ idx_new) ;
            if (idx_new == presets.size())
                for (idx_new = idx_current - 1; idx_new > 0 && ! presets[idx_new].is_visible; -- idx_new);
            preset_name = presets[idx_new].name;
            BOOST_LOG_TRIVIAL(info) << boost::format("cause by delete current ,choose the next visible, idx %1%, name %2%")
                                        %idx_new %preset_name;
        } else {
            //BBS select first visible item first
            const std::deque<Preset> &presets 		= this->m_presets->get_presets();
            size_t 				      idx_new = 0;
            if (idx_new < presets.size())
                for (; idx_new < presets.size() && ! presets[idx_new].is_visible; ++ idx_new) ;
            preset_name = presets[idx_new].name;
            if (idx_new == presets.size()) {
                // If no name is provided, select the "-- default --" preset.
                preset_name = m_presets->default_preset().name;
            }
            BOOST_LOG_TRIVIAL(info) << boost::format("not cause by delete current ,choose the first visible, idx %1%, name %2%")
                                        %idx_new %preset_name;
        }
    }
    //BBS: add project embedded preset logic and refine is_external
    assert(! delete_current || (m_presets->get_edited_preset().name != preset_name && (m_presets->get_edited_preset().is_user() || m_presets->get_edited_preset().is_project_embedded)));
    //assert(! delete_current || (m_presets->get_edited_preset().name != preset_name && m_presets->get_edited_preset().is_user()));
    bool current_dirty = ! delete_current && m_presets->current_is_dirty();
    bool print_tab     = m_presets->type() == Preset::TYPE_PRINT || m_presets->type() == Preset::TYPE_SLA_PRINT;
    bool printer_tab   = m_presets->type() == Preset::TYPE_PRINTER;
    bool canceled      = false;
    bool no_transfer = false;
    bool technology_changed = false;
    m_dependent_tabs.clear();
    if ((m_presets->type() == Preset::TYPE_FILAMENT) && !preset_name.empty())
    {
        Preset *to_be_selected = m_presets->find_preset(preset_name, false, true);
        if (to_be_selected) {
            std::string current_type, to_select_type;
            ConfigOptionStrings* cur_opt = dynamic_cast <ConfigOptionStrings *>(m_presets->get_edited_preset().config.option("filament_type"));
            ConfigOptionStrings* to_select_opt = dynamic_cast <ConfigOptionStrings *>(to_be_selected->config.option("filament_type"));
            if (cur_opt && (cur_opt->values.size() > 0)) {
                current_type =  cur_opt->values[0];
            }
            if (to_select_opt && (to_select_opt->values.size() > 0)) {
                to_select_type =  to_select_opt->values[0];
            }
            if (current_type != to_select_type)
                no_transfer = true;
        }
    }
    else if (printer_tab)
        no_transfer = true;
    if (current_dirty && ! may_discard_current_dirty_preset(nullptr, preset_name, no_transfer) && !force_select) {
        canceled = true;
        BOOST_LOG_TRIVIAL(info) << boost::format("current dirty and cancelled");
    } else if (print_tab) {
        // Before switching the print profile to a new one, verify, whether the currently active filament or SLA material
        // are compatible with the new print.
        // If it is not compatible and the current filament or SLA material are dirty, let user decide
        // whether to discard the changes or keep the current print selection.
        PresetWithVendorProfile printer_profile = m_preset_bundle->printers.get_edited_preset_with_vendor_profile();
        PrinterTechnology  printer_technology = printer_profile.preset.printer_technology();
        PresetCollection  &dependent = (printer_technology == ptFFF) ? m_preset_bundle->filaments : m_preset_bundle->sla_materials;
        bool 			   old_preset_dirty = dependent.current_is_dirty();
        bool 			   new_preset_compatible = is_compatible_with_print(dependent.get_edited_preset_with_vendor_profile(),
        	m_presets->get_preset_with_vendor_profile(*m_presets->find_preset(preset_name, true)), printer_profile);
        if (! canceled)
            canceled = old_preset_dirty && ! may_discard_current_dirty_preset(&dependent, preset_name) && ! new_preset_compatible && !force_select;
        if (! canceled) {
            // The preset will be switched to a different, compatible preset, or the '-- default --'.
            m_dependent_tabs.emplace_back((printer_technology == ptFFF) ? Preset::Type::TYPE_FILAMENT : Preset::Type::TYPE_SLA_MATERIAL);
            if (old_preset_dirty && ! new_preset_compatible)
                dependent.discard_current_changes();
        }
        BOOST_LOG_TRIVIAL(info) << boost::format("select process, new_preset_compatible %1%, old_preset_dirty %2%, cancelled %3%")
            %new_preset_compatible %old_preset_dirty % canceled;
    } else if (printer_tab) {
        // Before switching the printer to a new one, verify, whether the currently active print and filament
        // are compatible with the new printer.
        // If they are not compatible and the current print or filament are dirty, let user decide
        // whether to discard the changes or keep the current printer selection.
        //
        // With the introduction of the SLA printer types, we need to support switching between
        // the FFF and SLA printers.
        const Preset 		&new_printer_preset     = *m_presets->find_preset(preset_name, true);
		const PresetWithVendorProfile new_printer_preset_with_vendor_profile = m_presets->get_preset_with_vendor_profile(new_printer_preset);
        PrinterTechnology    old_printer_technology = m_presets->get_edited_preset().printer_technology();
        PrinterTechnology    new_printer_technology = new_printer_preset.printer_technology();
        if (new_printer_technology == ptSLA && old_printer_technology == ptFFF && !wxGetApp().may_switch_to_SLA_preset(_omitL("New printer preset selected")))
            canceled = true;
        else {
            struct PresetUpdate {
                Preset::Type         tab_type;
                PresetCollection 	*presets;
                PrinterTechnology    technology;
                bool    	         old_preset_dirty;
                bool         	     new_preset_compatible;
            };
            std::vector<PresetUpdate> updates = {
                { Preset::Type::TYPE_PRINT,         &m_preset_bundle->prints,       ptFFF },
                //{ Preset::Type::TYPE_SLA_PRINT,     &m_preset_bundle->sla_prints,   ptSLA },
                { Preset::Type::TYPE_FILAMENT,      &m_preset_bundle->filaments,    ptFFF },
                //{ Preset::Type::TYPE_SLA_MATERIAL,  &m_preset_bundle->sla_materials,ptSLA }
            };
            for (PresetUpdate &pu : updates) {
                pu.old_preset_dirty = (old_printer_technology == pu.technology) && pu.presets->current_is_dirty();
                pu.new_preset_compatible = (new_printer_technology == pu.technology) && is_compatible_with_printer(pu.presets->get_edited_preset_with_vendor_profile(), new_printer_preset_with_vendor_profile);
                if (!canceled)
                    canceled = pu.old_preset_dirty && !may_discard_current_dirty_preset(pu.presets, preset_name) && !pu.new_preset_compatible && !force_select;
            }
            if (!canceled) {
                for (PresetUpdate &pu : updates) {
                    // The preset will be switched to a different, compatible preset, or the '-- default --'.
                    if (pu.technology == new_printer_technology)
                        m_dependent_tabs.emplace_back(pu.tab_type);
                    if (pu.old_preset_dirty && !pu.new_preset_compatible)
                        pu.presets->discard_current_changes();
                }
            }
        }
        if (! canceled)
        	technology_changed = old_printer_technology != new_printer_technology;

        BOOST_LOG_TRIVIAL(info) << boost::format("select machine, technology_changed %1%, canceled %2%")
                %technology_changed  % canceled;
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("before delete action, canceled %1%, delete_current %2%") %canceled %delete_current;
    bool        delete_third_printer = false;
    std::deque<Preset> filament_presets;
    std::deque<Preset> process_presets;
    if (! canceled && delete_current) {
        // Delete the file and select some other reasonable preset.
        // It does not matter which preset will be made active as the preset will be re-selected from the preset_name variable.
        // The 'external' presets will only be removed from the preset list, their files will not be deleted.
        try {
            //BBS delete preset
            Preset &current_preset = m_presets->get_selected_preset();

            // Obtain compatible filament and process presets for printers
            if (m_preset_bundle && m_presets->get_preset_base(current_preset) == &current_preset && printer_tab && !current_preset.is_system) {
                delete_third_printer = true;
                for (const Preset &preset : m_preset_bundle->filaments.get_presets()) {
                    if (preset.is_compatible && !preset.is_default) {
                        if (preset.inherits() != "")
                            filament_presets.push_front(preset);
                        else
                            filament_presets.push_back(preset);
                        if (!preset.setting_id.empty()) { m_preset_bundle->filaments.set_sync_info_and_save(preset.name, preset.setting_id, "delete", 0); }
                    }
                }
                for (const Preset &preset : m_preset_bundle->prints.get_presets()) {
                    if (preset.is_compatible && !preset.is_default) {
                        if (preset.inherits() != "")
                            process_presets.push_front(preset);
                        else
                            process_presets.push_back(preset);
                        if (!preset.setting_id.empty()) { m_preset_bundle->filaments.set_sync_info_and_save(preset.name, preset.setting_id, "delete", 0); }
                    }
                }
            }
            if (!current_preset.setting_id.empty()) {
                m_presets->set_sync_info_and_save(current_preset.name, current_preset.setting_id, "delete", 0);
                wxGetApp().delete_preset_from_cloud(current_preset.setting_id);
            }
            BOOST_LOG_TRIVIAL(info) << "delete preset = " << current_preset.name << ", setting_id = " << current_preset.setting_id;
            BOOST_LOG_TRIVIAL(info) << boost::format("will delete current preset...");
            m_presets->delete_current_preset();
        } catch (const std::exception & ex) {
            //FIXME add some error reporting!
            canceled = true;
            BOOST_LOG_TRIVIAL(info) << boost::format("found exception when delete: %1%") %ex.what();
        }
    }

    if (canceled) {
        BOOST_LOG_TRIVIAL(info) << boost::format("canceled delete, update ui...");
        if (m_type == Preset::TYPE_PRINTER) {
            if (!last_selected_ph_printer_name.empty() &&
                m_presets->get_edited_preset().name == PhysicalPrinter::get_preset_name(last_selected_ph_printer_name)) {
                // If preset selection was canceled and previously was selected physical printer, we should select it back
                m_preset_bundle->physical_printers.select_printer(last_selected_ph_printer_name);
            }
            if (m_preset_bundle->physical_printers.has_selection()) {
                // If preset selection was canceled and physical printer was selected
                // we must disable selection marker for the physical printers
                m_preset_bundle->physical_printers.unselect_printer();
            }
        }

        update_tab_ui();

        // Trigger the on_presets_changed event so that we also restore the previous value in the plater selector,
        // if this action was initiated from the plater.
        on_presets_changed();
    } else {
        BOOST_LOG_TRIVIAL(info) << boost::format("successfully delete, will update compatibility");
        if (current_dirty)
            m_presets->discard_current_changes();

        const bool is_selected = m_presets->select_preset_by_name(preset_name, false) || delete_current;
        assert(m_presets->get_edited_preset().name == preset_name || ! is_selected);
        // Mark the print & filament enabled if they are compatible with the currently selected preset.
        // The following method should not discard changes of current print or filament presets on change of a printer profile,
        // if they are compatible with the current printer.
        auto update_compatible_type = [delete_current](bool technology_changed, bool on_page, bool show_incompatible_presets) {
        	return (delete_current || technology_changed) ? PresetSelectCompatibleType::Always :
        	       on_page                                ? PresetSelectCompatibleType::Never  :
        	       show_incompatible_presets              ? PresetSelectCompatibleType::OnlyIfWasCompatible : PresetSelectCompatibleType::Always;
        };
        if (current_dirty || delete_current || print_tab || printer_tab)
            m_preset_bundle->update_compatible(
            	update_compatible_type(technology_changed, print_tab,   (print_tab ? this : wxGetApp().get_tab(Preset::TYPE_PRINT))->m_show_incompatible_presets),
            	update_compatible_type(technology_changed, false, 		wxGetApp().get_tab(Preset::TYPE_FILAMENT)->m_show_incompatible_presets));
        // Initialize the UI from the current preset.
        if (printer_tab)
            static_cast<TabPrinter*>(this)->update_pages();

        if (! is_selected && printer_tab)
        {
            /* There is a case, when :
             * after Config Wizard applying we try to select previously selected preset, but
             * in a current configuration this one:
             *  1. doesn't exist now,
             *  2. have another printer_technology
             * So, it is necessary to update list of dependent tabs
             * to the corresponding printer_technology
             */
            const PrinterTechnology printer_technology = m_presets->get_edited_preset().printer_technology();
            if (printer_technology == ptFFF && m_dependent_tabs.front() != Preset::Type::TYPE_PRINT)
                m_dependent_tabs = { Preset::Type::TYPE_PRINT, Preset::Type::TYPE_FILAMENT };
            else if (printer_technology == ptSLA && m_dependent_tabs.front() != Preset::Type::TYPE_SLA_PRINT)
                m_dependent_tabs = { Preset::Type::TYPE_SLA_PRINT, Preset::Type::TYPE_SLA_MATERIAL };
        }

        // check if there is something in the cache to move to the new selected preset
        apply_config_from_cache();

        // Orca: update presets for the selected printer
        if (m_type == Preset::TYPE_PRINTER && wxGetApp().app_config->get_bool("remember_printer_config")) {
            if (preset_name.find("Snapmaker U1") != std::string::npos) {
                // 在 update_selections() 改变耗材数量之前先保存旧数量和颜色
                std::vector<std::string> old_filament_colors = wxGetApp().plater()->get_extruder_colors_from_plater_config(nullptr, false);
                std::vector<std::string> old_filament_presets = m_preset_bundle->filament_presets;

                m_preset_bundle->update_selections(*wxGetApp().app_config);

                // 恢复耗材预设到原来的数量和预设名称（保持类型自适应）
                m_preset_bundle->filament_presets = old_filament_presets;

                // 恢复原来的颜色，保持用户设置的耗材颜色不变
                wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour")->values = old_filament_colors;

                // 重要：立即保存颜色到配置文件，这样下次切换时也会保持
                std::string filament_colors_str = boost::algorithm::join(old_filament_colors, ",");
                wxGetApp().app_config->set_printer_setting(preset_name, "filament_colors", filament_colors_str);

                wxGetApp().plater()->sidebar().on_filaments_change(m_preset_bundle->filament_presets.size());
            } else {
                // 非 U1 机型：使用默认行为
                m_preset_bundle->update_selections(*wxGetApp().app_config);
                wxGetApp().plater()->sidebar().on_filaments_change(m_preset_bundle->filament_presets.size());
            }
        }
        load_current_preset();

        if (delete_third_printer) {
            wxGetApp().CallAfter([filament_presets, process_presets]() {
                PresetBundle *preset_bundle     = wxGetApp().preset_bundle;
                std::string   old_filament_name = preset_bundle->filaments.get_edited_preset().name;
                std::string   old_process_name  = preset_bundle->prints.get_edited_preset().name;

                for (const Preset &preset : filament_presets) {
                    if (!preset.setting_id.empty()) {
                        wxGetApp().delete_preset_from_cloud(preset.setting_id);
                    }
                    BOOST_LOG_TRIVIAL(info) << "delete filament preset = " << preset.name << ", setting_id = " << preset.setting_id;
                    preset_bundle->filaments.delete_preset(preset.name);
                }

                for (const Preset &preset : process_presets) {
                    if (!preset.setting_id.empty()) {
                        wxGetApp().delete_preset_from_cloud(preset.setting_id);
                    }
                    BOOST_LOG_TRIVIAL(info) << "delete print preset = " << preset.name << ", setting_id = " << preset.setting_id;
                    preset_bundle->prints.delete_preset(preset.name);
                }

                preset_bundle->update_compatible(PresetSelectCompatibleType::Always);
                preset_bundle->filaments.select_preset_by_name(old_filament_name, true);
                preset_bundle->prints.select_preset_by_name(old_process_name, true);
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " old filament name is:" << old_filament_name << " old process name is: " << old_process_name;

            });
        }

        // Trigger the on_presets_changed event to apply cached config for dependent tabs
        on_presets_changed();
    }

    if (technology_changed)
        wxGetApp().mainframe->technology_changed();
    BOOST_LOG_TRIVIAL(info) << boost::format("select preset, exit");

    return !canceled;
}

// If the current preset is dirty, the user is asked whether the changes may be discarded.
// if the current preset was not dirty, or the user agreed to discard the changes, 1 is returned.
bool Tab::may_discard_current_dirty_preset(PresetCollection* presets /*= nullptr*/, const std::string& new_printer_name /*= ""*/, bool no_transfer)
{
    if (presets == nullptr) presets = m_presets;

    UnsavedChangesDialog dlg(m_type, presets, new_printer_name, no_transfer);

    if (dlg.ShowModal() == wxID_CANCEL)
        return false;

    if (dlg.save_preset())  // save selected changes
    {
        const std::vector<std::string>& unselected_options = dlg.get_unselected_options(presets->type());
        const std::string& name = dlg.get_preset_name();
        //BBS: add project embedded preset relate logic
        bool save_to_project = dlg.get_save_to_project_option();

        if (m_type == presets->type()) // save changes for the current preset from this tab
        {
            // revert unselected options to the old values
            presets->get_edited_preset().config.apply_only(presets->get_selected_preset().config, unselected_options);
            //BBS: add project embedded preset relate logic
            save_preset(name, false, save_to_project);
            //save_preset(name);
        }
        else
        {
            //BBS: add project embedded preset relate logic
            m_preset_bundle->save_changes_for_preset(name, presets->type(), unselected_options, save_to_project);
            //m_preset_bundle->save_changes_for_preset(name, presets->type(), unselected_options);

            // If filament preset is saved for multi-material printer preset,
            // there are cases when filament comboboxs are updated for old (non-modified) colors,
            // but in full_config a filament_colors option aren't.
            if (presets->type() == Preset::TYPE_FILAMENT && wxGetApp().extruders_edited_cnt() > 1)
                wxGetApp().plater()->force_filament_colors_update();
        }
    }
    else if (dlg.transfer_changes()) // move selected changes
    {
        std::vector<std::string> selected_options = dlg.get_selected_options();
        if (m_type == presets->type()) // move changes for the current preset from this tab
        {
            if (m_type == Preset::TYPE_PRINTER) {
                auto it = std::find(selected_options.begin(), selected_options.end(), "extruders_count");
                if (it != selected_options.end()) {
                    // erase "extruders_count" option from the list
                    selected_options.erase(it);
                    // cache the extruders count
                    static_cast<TabPrinter*>(this)->cache_extruder_cnt();
                }
            }

            // copy selected options to the cache from edited preset
            cache_config_diff(selected_options);
        }
        else
            wxGetApp().get_tab(presets->type())->cache_config_diff(selected_options);
    }

    return true;
}

void Tab::clear_pages()
{
    // invalidated highlighter, if any exists
    m_highlighter.invalidate();
    // clear pages from the controlls
    for (auto p : m_pages)
        p->clear();
    //BBS: clear page in Parent
    //m_page_sizer->Clear(true);
    m_parent->clear_page();

    // nulling pointers
    m_parent_preset_description_line = nullptr;
    m_detach_preset_btn = nullptr;

    m_compatible_printers.checkbox  = nullptr;
    m_compatible_printers.btn       = nullptr;

    m_compatible_prints.checkbox    = nullptr;
    m_compatible_prints.btn         = nullptr;
}

//BBS: GUI refactor: unselect current item
void Tab::unselect_tree_item()
{
    // BBS: bold selection
    const auto sel_item = m_tabctrl->GetSelection();
    m_last_select_item = sel_item;
    m_tabctrl->SetItemBold(sel_item, false);
    m_tabctrl->Unselect();
    m_active_page = nullptr;
}

// BBS: open/close this tab
void Tab::set_expanded(bool value)
{
    if (value) {
        if (m_presets_choice)
            m_main_sizer->Show(m_presets_choice);
        m_main_sizer->Show(m_tabctrl);
    }
    else {
        m_active_page = NULL;
        if (m_presets_choice)
            m_main_sizer->Hide(m_presets_choice);
        m_main_sizer->Hide(m_tabctrl);
    }
}

// BBS: new layout
void Tab::restore_last_select_item()
{
    auto item = m_last_select_item;
    if (item == -1)
        item = m_tabctrl->GetFirstVisibleItem();
    m_tabctrl->SelectItem(item);
}

void Tab::update_description_lines()
{
    if (m_active_page && m_active_page->title() == "Dependencies" && m_parent_preset_description_line)
        update_preset_description_line();
}

void Tab::activate_selected_page(std::function<void()> throw_if_canceled)
{
    if (!m_active_page)
        return;

    m_active_page->activate(m_mode, throw_if_canceled);
    update_changed_ui();
    update_description_lines();
    if (m_active_page && !(m_active_page->title() == "Dependencies"))
        toggle_options();
    m_active_page->update_visibility(m_mode, true); // for taggle line
}

//BBS: GUI refactor
bool Tab::update_current_page_in_background(int& item)
{
    Page* page = nullptr;

    const auto selection = item >= 0 ? m_tabctrl->GetItemText(item) : "";
    for (auto p : m_pages)
        if (translate_category(p->title(), m_type) == selection)
        {
            page = p.get();
            break;
        }

    if (page == nullptr || m_active_page == page)
        return false;

    bool active_tab = false;
    if (wxGetApp().mainframe != nullptr && wxGetApp().mainframe->is_active_and_shown_tab(m_parent))
        active_tab = true;

    if (!active_tab || (!m_parent->is_active_and_shown_tab((wxPanel*)this)))
    {
        m_is_nonsys_values = page->m_is_nonsys_values;
        m_is_modified_values = page->m_is_modified_values;
        // BBS: not need active
        // m_active_page = page;

        // invalidated highlighter, if any exists
        m_highlighter.invalidate();

        // clear pages from the controlls
        // BBS: fix after new layout, clear page in backgroud
        for (auto p : m_pages)
            p->clear();
        if (m_parent->is_active_and_shown_tab((wxPanel*)this))
            m_parent->clear_page();

        update_undo_buttons();

        // BBS: this is not used, because we not SelectItem in background
        //todo: update selected item of tree_ctrl
        // wxTreeItemData item_data;
        // m_tabctrl->SetItemData(item, &item_data);

        return false;
    }

    return true;
}

//BBS: GUI refactor
bool Tab::tree_sel_change_delayed(wxCommandEvent& event)
{
    // The issue apparently manifests when Show()ing a window with overlay scrollbars while the UI is frozen. For this reason,
    // we will Thaw the UI prematurely on Linux. This means destroing the no_updates object prematurely.
#ifdef __linux__
    std::unique_ptr<wxWindowUpdateLocker> no_updates(new wxWindowUpdateLocker(this));
#else
    /* On Windows we use DoubleBuffering during rendering,
     * so on Window is no needed to call a Freeze/Thaw functions.
     * But under OSX (builds compiled with MacOSX10.14.sdk) wxStaticBitmap rendering is broken without Freeze/Thaw call.
     */
//#ifdef __WXOSX__  // Use Freeze/Thaw to avoid flickering during clear/activate new page
//    wxWindowUpdateLocker noUpdates(this);
//#endif
#endif

    //BBS: GUI refactor
    Page* page = nullptr;
    const auto sel_item = m_tabctrl->GetSelection();
    // BBS: bold selection
    //OutputDebugStringA("tree_sel_change_delayed ");
    //OutputDebugStringA(m_title.c_str());
    m_tabctrl->SetItemBold(sel_item, true);
    const auto selection = sel_item >= 0 ? m_tabctrl->GetItemText(sel_item) : "";
    //OutputDebugString(selection);
    //OutputDebugStringA("\n");
    for (auto p : m_pages)
        if (translate_category(p->title(), m_type) == selection)
        {
            page = p.get();
            m_is_nonsys_values = page->m_is_nonsys_values;
            m_is_modified_values = page->m_is_modified_values;
            break;
        }

    //BBS: GUI refactor
    if (page == nullptr)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("can not find page with current selection %1%\n") % selection;
        return false;
    }
    void* item_data = m_tabctrl->GetItemData(sel_item);
    if (item_data)
    {
        //from update_current_page_in_background in not active tab
        m_tabctrl->SetItemData(sel_item, NULL);
        return false;
    }

    if (!m_parent->is_active_and_shown_tab((wxPanel*)this))
    {
        Tab* current_tab = dynamic_cast<Tab*>(m_parent->get_current_tab());

        m_page_view->Freeze();

        if (current_tab)
        {
            current_tab->clear_pages();
            current_tab->unselect_tree_item();
        }
        m_active_page = page;
        // BBS: not changed
        // update_undo_buttons();
        this->OnActivate();
        m_parent->set_active_tab(this);

        m_page_view->Thaw();
        return false;
    }

    //process logic in the same tab when select treeCtrlItem
    if (m_active_page == page)
        return false;

    m_active_page = page;

    auto throw_if_canceled = std::function<void()>([this](){
#ifdef WIN32
            //BBS: GUI refactor
            //TODO: remove this call currently, after refactor, there is Paint event in the queue
            //this call will cause OnPaint immediately, which will cause crash
            //wxCheckForInterrupt(m_tabctrl);
            if (m_page_switch_planned)
                throw UIBuildCanceled();
#else // WIN32
            (void)this; // silence warning
#endif
        });

    try {
        m_page_view->Freeze();
        // clear pages from the controls
        clear_pages();
        throw_if_canceled();

        //BBS: GUI refactor
        if (wxGetApp().mainframe!=nullptr && wxGetApp().mainframe->is_active_and_shown_tab(m_parent))
            activate_selected_page(throw_if_canceled);

        #ifdef __linux__
            no_updates.reset(nullptr);
        #endif

        // BBS: not changed
        // update_undo_buttons();
        throw_if_canceled();

        //BBS: GUI refactor
        //m_hsizer->Layout();
        m_parent->Layout();
        throw_if_canceled();
        // Refresh();

        m_page_view->Thaw();
    } catch (const UIBuildCanceled&) {
	    if (m_active_page)
		    m_active_page->clear();
        m_page_view->Thaw();
        return true;
    }

    return false;
}

void Tab::OnKeyDown(wxKeyEvent& event)
{
    if (event.GetKeyCode() == WXK_TAB)
        m_tabctrl->Navigate(event.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
    else
        event.Skip();
}

void Tab::compare_preset()
{
    wxGetApp().mainframe->diff_dialog.show(m_type);
}

void Tab::transfer_options(const std::string &name_from, const std::string &name_to, std::vector<std::string> options)
{
    if (options.empty())
        return;

    Preset* preset_from = m_presets->find_preset(name_from);
    Preset* preset_to = m_presets->find_preset(name_to);

    if (m_type == Preset::TYPE_PRINTER) {
         auto it = std::find(options.begin(), options.end(), "extruders_count");
         if (it != options.end()) {
             // erase "extruders_count" option from the list
             options.erase(it);
             // cache the extruders count
             static_cast<TabPrinter*>(this)->cache_extruder_cnt(&preset_from->config);
         }
    }
    cache_config_diff(options, &preset_from->config);

    if (name_to != m_presets->get_edited_preset().name )
        select_preset(preset_to->name);

    apply_config_from_cache();
    load_current_preset();
}

// Save the current preset into file.
// This removes the "dirty" flag of the preset, possibly creates a new preset under a new name,
// and activates the new preset.
// Wizard calls save_preset with a name "My Settings", otherwise no name is provided and this method
// opens a Slic3r::GUI::SavePresetDialog dialog.
//BBS: add project embedded preset relate logic
void Tab::save_preset(std::string name /*= ""*/, bool detach, bool save_to_project, bool from_input, std::string input_name )
{
    // since buttons(and choices too) don't get focus on Mac, we set focus manually
    // to the treectrl so that the EVT_* events are fired for the input field having
    // focus currently.is there anything better than this ?
//!	m_tabctrl->OnSetFocus();
    if (from_input) {
        SavePresetDialog dlg(m_parent, m_type, detach ? _u8L("Detached") : "");
        dlg.Show(false);
        dlg.input_name_from_other(input_name);
        wxCommandEvent evt(wxEVT_TEXT, GetId());
        dlg.GetEventHandler()->ProcessEvent(evt);
        dlg.confirm_from_other();
        name = input_name;
    }

    if (name.empty()) {
        SavePresetDialog dlg(m_parent, m_type, detach ? _u8L("Detached") : "");
        if (!m_just_edit) {
            if (dlg.ShowModal() != wxID_OK)
                return;
        }
        name = dlg.get_name();
        //BBS: add project embedded preset relate logic
        save_to_project = dlg.get_save_to_project_selection(m_type);
    }

    //BBS record current preset name
    std::string curr_preset_name = m_presets->get_edited_preset().name;

    bool exist_preset = false;
    Preset* new_preset = m_presets->find_preset(name, false);
    if (new_preset) {
        exist_preset = true;
    }

    Preset* _current_printer = nullptr;
    if (m_presets->type() == Preset::TYPE_FILAMENT) {
        _current_printer = const_cast<Preset*>(&wxGetApp().preset_bundle->printers.get_selected_preset_base());
    }
    // Save the preset into Slic3r::data_dir / presets / section_name / preset_name.json
    m_presets->save_current_preset(name, detach, save_to_project, nullptr, _current_printer);

    //BBS create new settings
    new_preset = m_presets->find_preset(name, false, true);
    //Preset* preset = &m_presets.preset(it - m_presets.begin(), true);
    if (!new_preset) {
        BOOST_LOG_TRIVIAL(info) << "create new preset failed";
        return;
    }

    // set sync_info for sync service
    if (exist_preset) {
        new_preset->sync_info = "update";
        BOOST_LOG_TRIVIAL(info) << "sync_preset: update preset = " << new_preset->name;
    }
    else {
        new_preset->sync_info = "create";
        if (wxGetApp().is_user_login())
            new_preset->user_id = wxGetApp().getAgent()->get_user_id();
        BOOST_LOG_TRIVIAL(info) << "sync_preset: create preset = " << new_preset->name;
    }
    new_preset->save_info();

    // Mark the print & filament enabled if they are compatible with the currently selected preset.
    // If saving the preset changes compatibility with other presets, keep the now incompatible dependent presets selected, however with a "red flag" icon showing that they are no more compatible.
    m_preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
    // Add the new item into the UI component, remove dirty flags and activate the saved item.
    update_tab_ui();

    // Update the selection boxes at the plater.
    on_presets_changed();

    //BBS if create a new prset name, preset changed from preset name to new preset name
    if (!exist_preset) {
        wxGetApp().plater()->sidebar().update_presets_from_to(m_type, curr_preset_name, new_preset->name);
    }

    // If current profile is saved, "delete preset" button have to be enabled
    m_btn_delete_preset->Show();
    m_btn_delete_preset->GetParent()->Layout();

    if (m_type == Preset::TYPE_PRINTER)
        static_cast<TabPrinter*>(this)->m_initial_extruders_count = static_cast<TabPrinter*>(this)->m_extruders_count;

    // Parent preset is "default" after detaching, so we should to update UI values, related on parent preset
    if (detach)
        update_ui_items_related_on_parent_preset(m_presets->get_selected_preset_parent());

    update_changed_ui();

    /* If filament preset is saved for multi-material printer preset,
     * there are cases when filament comboboxs are updated for old (non-modified) colors,
     * but in full_config a filament_colors option aren't.*/
    if (m_type == Preset::TYPE_FILAMENT && wxGetApp().extruders_edited_cnt() > 1)
        wxGetApp().plater()->force_filament_colors_update();

    {
        // Profile compatiblity is updated first when the profile is saved.
        // Update profile selection combo boxes at the depending tabs to reflect modifications in profile compatibility.
        std::vector<Preset::Type> dependent;
        switch (m_type) {
        case Preset::TYPE_PRINT:
            dependent = { Preset::TYPE_FILAMENT };
            break;
        case Preset::TYPE_SLA_PRINT:
            dependent = { Preset::TYPE_SLA_MATERIAL };
            break;
        case Preset::TYPE_PRINTER:
            if (static_cast<const TabPrinter*>(this)->m_printer_technology == ptFFF)
                dependent = { Preset::TYPE_PRINT, Preset::TYPE_FILAMENT };
            else
                dependent = { Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL };
            break;
        default:
            break;
        }
        for (Preset::Type preset_type : dependent)
            wxGetApp().get_tab(preset_type)->update_tab_ui();
    }

    // update preset comboboxes in DiffPresetDlg
    wxGetApp().mainframe->diff_dialog.update_presets(m_type);
}

// Called for a currently selected preset.
void Tab::delete_preset()
{
    auto current_preset = m_presets->get_selected_preset();
    // Don't let the user delete the ' - default - ' configuration.
    //BBS: add project embedded preset logic and refine is_external
    std::string action =  _utf8(L("Delete"));
    //std::string action = current_preset.is_external ? _utf8(L("remove")) : _utf8(L("delete"));
    // TRN  remove/delete
    wxString msg;
    bool     confirm_delete_third_party_printer = false;
    bool     is_base_preset                 = false;
    if (m_presets->get_preset_base(current_preset) == &current_preset) { //root preset
        is_base_preset = true;
        if (current_preset.type == Preset::Type::TYPE_PRINTER && !current_preset.is_system) { //Customize third-party printers
            Preset &current_preset = m_presets->get_selected_preset();
            int filament_preset_num    = 0;
            int process_preset_num     = 0;
            for (const Preset &preset : m_preset_bundle->filaments.get_presets()) {
                if (preset.is_compatible && !preset.is_default) { filament_preset_num++; }
            }
            for (const Preset &preset : m_preset_bundle->prints.get_presets()) {
                if (preset.is_compatible && !preset.is_default) { process_preset_num++; }
            }

            DeleteConfirmDialog
                dlg(parent(), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Delete"),
                    wxString::Format(_L("%d Filament Preset and %d Process Preset is attached to this printer. Those presets would be deleted if the printer is deleted."),
                                     filament_preset_num, process_preset_num));
            int res = dlg.ShowModal();
            if (res != wxID_OK) return;
            confirm_delete_third_party_printer = true;
        }
        int count = 0;
        wxString presets;
        for (auto &preset2 : *m_presets)
            if (preset2.inherits() == current_preset.name) {
                ++count;
                presets += "\n - " + preset2.name;
            }
        if (count > 0) {
            msg = _L("Presets inherited by other presets cannot be deleted!");
            msg += "\n";
            msg += _L_PLURAL("The following presets inherit this preset.",
                            "The following preset inherits this preset.", count);
            wxString title = from_u8((boost::format(_utf8(L("%1% Preset"))) % action).str()); // action + _(L(" Preset"));
            MessageDialog(parent(), msg + presets, title, wxOK | wxICON_ERROR).ShowModal();
            return;
        }
    }

    BOOST_LOG_TRIVIAL(info) << boost::format("delete preset %1%, setting_id %2%, user_id %3%, base_id %4%, sync_info %5%, type %6%")
        %current_preset.name%current_preset.setting_id%current_preset.user_id%current_preset.base_id%current_preset.sync_info
        %Preset::get_type_string(m_type);
    PhysicalPrinterCollection& physical_printers = m_preset_bundle->physical_printers;

    if (m_type == Preset::TYPE_PRINTER && !physical_printers.empty())
    {
        // Check preset for delete in physical printers
        // Ask a customer about next action, if there is a printer with just one preset and this preset is equal to delete
        std::vector<std::string> ph_printers        = physical_printers.get_printers_with_preset(current_preset.name);
        std::vector<std::string> ph_printers_only   = physical_printers.get_printers_with_only_preset(current_preset.name);

        //if (!ph_printers.empty()) {
        //    msg += _L_PLURAL("The physical printer below is based on the preset, you are going to delete.",
        //                        "The physical printers below are based on the preset, you are going to delete.", ph_printers.size());
        //    for (const std::string& printer : ph_printers)
        //        msg += "\n    \"" + from_u8(printer) + "\",";
        //    msg.RemoveLast();
        //    msg += "\n" + _L_PLURAL("Note, that the selected preset will be deleted from this printer too.",
        //                            "Note, that the selected preset will be deleted from these printers too.", ph_printers.size()) + "\n\n";
        //}

        //if (!ph_printers_only.empty()) {
        //    msg += _L_PLURAL("The physical printer below is based only on the preset, you are going to delete.",
        //                        "The physical printers below are based only on the preset, you are going to delete.", ph_printers_only.size());
        //    for (const std::string& printer : ph_printers_only)
        //        msg += "\n    \"" + from_u8(printer) + "\",";
        //    msg.RemoveLast();
        //    msg += "\n" + _L_PLURAL("Note, that this printer will be deleted after deleting the selected preset.",
        //                            "Note, that these printers will be deleted after deleting the selected preset.", ph_printers_only.size()) + "\n\n";
        //}
        if (!ph_printers.empty() || !ph_printers_only.empty()) {
            msg += _L_PLURAL("Following preset will be deleted too.", "Following presets will be deleted too.", ph_printers.size() + ph_printers_only.size());
            for (const std::string &printer : ph_printers) msg += "\n    \"" + from_u8(printer) + "\",";
            for (const std::string &printer : ph_printers_only) msg += "\n    \"" + from_u8(printer) + "\",";
            msg.RemoveLast();
            // msg += "\n" + _L_PLURAL("Note, that the selected preset will be deleted from this printer too.",
            //                        "Note, that the selected preset will be deleted from these printers too.", ph_printers.size()) + "\n\n";
        }
    }

    if (is_base_preset && (current_preset.type == Preset::Type::TYPE_FILAMENT) && action == _utf8(L("Delete"))) {
        msg += from_u8(_u8L("Are you sure to delete the selected preset? \nIf the preset corresponds to a filament currently in use on your printer, please reset the filament information for that slot."));
    } else {
        msg += from_u8((boost::format(_u8L("Are you sure to %1% the selected preset?")) % action).str());
    }

    //BBS: add project embedded preset logic and refine is_external
    action =  _utf8(L("Delete"));
    //action = current_preset.is_external ? _utf8(L("Remove")) : _utf8(L("Delete"));
    // TRN  Remove/Delete
    wxString title = from_u8((boost::format(_utf8(L("%1% Preset"))) % action).str());  //action + _(L(" Preset"));
    if (current_preset.is_default || !(confirm_delete_third_party_printer ||
        //wxID_YES != wxMessageDialog(parent(), msg, title, wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal())
        wxID_YES == MessageDialog(parent(), msg, title, wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal()))
        return;

    // if we just delete preset from the physical printer
    if (m_presets_choice->is_selected_physical_printer()) {
        PhysicalPrinter& printer = physical_printers.get_selected_printer();

        // just delete this preset from the current physical printer
        printer.delete_preset(m_presets->get_edited_preset().name);
        // select first from the possible presets for this printer
        physical_printers.select_printer(printer);

        this->select_preset(physical_printers.get_selected_printer_preset_name());
        return;
    }

    // delete selected preset from printers and printer, if it's needed
    if (m_type == Preset::TYPE_PRINTER && !physical_printers.empty())
        physical_printers.delete_preset_from_printers(current_preset.name);

    // Select will handle of the preset dependencies, of saving & closing the depending profiles, and
    // finally of deleting the preset.
    this->select_preset("", true);

    BOOST_LOG_TRIVIAL(info) << boost::format("delete preset finished");
}

void Tab::toggle_show_hide_incompatible()
{
    m_show_incompatible_presets = !m_show_incompatible_presets;
    if (m_presets_choice)
        m_presets_choice->set_show_incompatible_presets(m_show_incompatible_presets);
    update_show_hide_incompatible_button();
    update_tab_ui();
}

void Tab::update_show_hide_incompatible_button()
{
    //BBS: GUI refactor
    /*m_btn_hide_incompatible_presets->SetBitmap_(m_show_incompatible_presets ?
        m_bmp_show_incompatible_presets : m_bmp_hide_incompatible_presets);
    m_btn_hide_incompatible_presets->SetToolTip(m_show_incompatible_presets ?
        "Both compatible an incompatible presets are shown. Click to hide presets not compatible with the current printer." :
        "Only compatible presets are shown. Click to show both the presets compatible and not compatible with the current printer.");*/
}

void Tab::update_ui_from_settings()
{
    // Show the 'show / hide presets' button only for the print and filament tabs, and only if enabled
    // in application preferences.
    m_show_btn_incompatible_presets = true;
    bool show = m_show_btn_incompatible_presets && m_type != Slic3r::Preset::TYPE_PRINTER;
    //BBS: GUI refactor
    //Layout();
    m_parent->Layout();
    //show ? m_btn_hide_incompatible_presets->Show() :  m_btn_hide_incompatible_presets->Hide();
    // If the 'show / hide presets' button is hidden, hide the incompatible presets.
    if (show) {
        update_show_hide_incompatible_button();
    }
    else {
        if (m_show_incompatible_presets) {
            m_show_incompatible_presets = false;
            update_tab_ui();
        }
    }
}

void Tab::create_line_with_widget(ConfigOptionsGroup* optgroup, const std::string& opt_key, const std::string& path, widget_t widget)
{
    Line line = optgroup->create_single_option_line(opt_key);
    line.widget = widget;
    line.label_path = path;

    // set default undo ui
    line.set_undo_bitmap(&m_bmp_white_bullet);
    line.set_undo_to_sys_bitmap(&m_bmp_white_bullet);
    line.set_undo_tooltip(&m_tt_white_bullet);
    line.set_undo_to_sys_tooltip(&m_tt_white_bullet);
    line.set_label_colour(&m_default_text_clr);

    optgroup->append_line(line);
}

// Return a callback to create a Tab widget to mark the preferences as compatible / incompatible to the current printer.
wxSizer* Tab::compatible_widget_create(wxWindow* parent, PresetDependencies &deps)
{
    deps.checkbox = new ::CheckBox(parent, wxID_ANY);
    wxGetApp().UpdateDarkUI(deps.checkbox, false, true);

    deps.checkbox_title = new wxStaticText(parent, wxID_ANY, _L("All"));
    deps.checkbox_title->SetFont(Label::Body_14);
    deps.checkbox_title->SetForegroundColour(wxColour("#363636"));
    wxGetApp().UpdateDarkUI(deps.checkbox_title, false, true);

    // ORCA modernize button style
    Button* btn = new Button(parent, _(L("Set")) + " " + dots);
    btn->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    deps.btn = btn;

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add((deps.checkbox), 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add((deps.checkbox_title), 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(new wxStaticText(parent, wxID_ANY, "  ")); // weirdly didnt apply AddSpacer or wxRIGHT border
    sizer->Add((deps.btn), 0, wxALIGN_CENTER_VERTICAL);

    auto on_toggle = [this, &deps](const bool &state){
        deps.checkbox->SetValue(state);
        deps.btn->Enable(!state);
        // All printers have been made compatible with this preset.
        if (state)
            this->load_key_value(deps.key_list, std::vector<std::string> {});
        this->get_field(deps.key_condition)->toggle(state);
        this->update_changed_ui();
    };

    deps.checkbox_title->Bind(wxEVT_LEFT_DOWN,([this, &deps, on_toggle](wxMouseEvent e) {
        if (e.GetEventType() == wxEVT_LEFT_DCLICK) return;
        on_toggle(!deps.checkbox->GetValue());
        e.Skip();
    }));

    deps.checkbox_title->Bind(wxEVT_LEFT_DCLICK,([this, &deps, on_toggle](wxMouseEvent e) {
        on_toggle(!deps.checkbox->GetValue());
        e.Skip();
    }));

    deps.checkbox->Bind(wxEVT_TOGGLEBUTTON, ([this, on_toggle](wxCommandEvent e) {
        on_toggle(e.IsChecked());
        e.Skip();
    }), deps.checkbox->GetId());

    if (deps.checkbox){
        bool is_empty = m_config->option<ConfigOptionStrings>(deps.key_list)->values.empty();
        deps.checkbox->SetValue(is_empty);
        deps.btn->Enable(!is_empty);
    }

    /*
    if (m_compatible_printers.checkbox) {
        bool is_empty = m_config->option<ConfigOptionStrings>("compatible_printers")->values.empty();
        m_compatible_printers.checkbox->SetValue(is_empty);
        is_empty ? m_compatible_printers.btn->Disable() : m_compatible_printers.btn->Enable();
    }

    if (m_compatible_prints.checkbox) {
        bool is_empty = m_config->option<ConfigOptionStrings>("compatible_prints")->values.empty();
        m_compatible_prints.checkbox->SetValue(is_empty);
        is_empty ? m_compatible_prints.btn->Disable() : m_compatible_prints.btn->Enable();
    }
    */

    deps.btn->Bind(wxEVT_BUTTON, ([this, parent, &deps](wxCommandEvent e)
    {
        // Collect names of non-default non-external profiles.
        PrinterTechnology printer_technology = m_preset_bundle->printers.get_edited_preset().printer_technology();
        PresetCollection &depending_presets  = (deps.type == Preset::TYPE_PRINTER) ? m_preset_bundle->printers :
                (printer_technology == ptFFF) ? m_preset_bundle->prints : m_preset_bundle->sla_prints;
        wxArrayString presets;
        for (size_t idx = 0; idx < depending_presets.size(); ++ idx)
        {
            Preset& preset = depending_presets.preset(idx);
            //BBS: add project embedded preset logic and refine is_external
            bool add = ! preset.is_default;
            //bool add = ! preset.is_default && ! preset.is_external;
            if (add && deps.type == Preset::TYPE_PRINTER)
                // Only add printers with the same technology as the active printer.
                add &= preset.printer_technology() == printer_technology;
            if (add)
                presets.Add(from_u8(preset.name));
        }

        wxMultiChoiceDialog dlg(parent, deps.dialog_title, deps.dialog_label, presets);
        wxGetApp().UpdateDlgDarkUI(&dlg);
        // Collect and set indices of depending_presets marked as compatible.
        wxArrayInt selections;
        auto *compatible_printers = dynamic_cast<const ConfigOptionStrings*>(m_config->option(deps.key_list));
        if (compatible_printers != nullptr || !compatible_printers->values.empty())
            for (auto preset_name : compatible_printers->values)
                for (size_t idx = 0; idx < presets.GetCount(); ++idx)
                    if (presets[idx] == preset_name) {
                        selections.Add(idx);
                        break;
                    }
        dlg.SetSelections(selections);
        std::vector<std::string> value;
        // Show the dialog.
        if (dlg.ShowModal() == wxID_OK) {
            selections.Clear();
            selections = dlg.GetSelections();
            for (auto idx : selections)
                value.push_back(presets[idx].ToUTF8().data());
            if (value.empty()) {
                deps.checkbox->SetValue(1);
                deps.btn->Disable();
            }
            // All depending_presets have been made compatible with this preset.
            this->load_key_value(deps.key_list, value);
            this->update_changed_ui();
        }
    }));

    return sizer;
}

// Return a callback to create a TabPrinter widget to edit bed shape
wxSizer* TabPrinter::create_bed_shape_widget(wxWindow* parent)
{
    // ORCA modernize button style
    Button* btn = new Button(parent, _(L("Set")) + " " + dots);
    btn->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);

    auto sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(btn, 0, wxALIGN_CENTER_VERTICAL);

    btn->Bind(wxEVT_BUTTON, ([this](wxCommandEvent e) {
            bool  is_configed_by_BBL = PresetUtils::system_printer_bed_model(m_preset_bundle->printers.get_edited_preset()).size() > 0;
            BedShapeDialog dlg(this);
            dlg.build_dialog(m_config->option<ConfigOptionPoints>("printable_area")->values,
                *m_config->option<ConfigOptionString>("bed_custom_texture"),
                *m_config->option<ConfigOptionString>("bed_custom_model"));
            if (dlg.ShowModal() == wxID_OK) {
                const std::vector<Vec2d>& shape = dlg.get_shape();
                const std::string& custom_texture = dlg.get_custom_texture();
                const std::string& custom_model = dlg.get_custom_model();
                if (!shape.empty())
                {
                    load_key_value("printable_area", shape);
                    load_key_value("bed_custom_texture", custom_texture);
                    load_key_value("bed_custom_model", custom_model);
                    update_changed_ui();
                }
            on_presets_changed();

            }
        }));

    {
        Search::OptionsSearcher& searcher = wxGetApp().sidebar().get_searcher();
        const Search::GroupAndCategory& gc = searcher.get_group_and_category("printable_area");
        searcher.add_key("bed_custom_texture", m_type, gc.group, gc.category);
        searcher.add_key("bed_custom_model", m_type, gc.group, gc.category);
    }

    return sizer;
}

void TabPrinter::cache_extruder_cnt(const DynamicPrintConfig* config/* = nullptr*/)
{
    const DynamicPrintConfig& cached_config = config ? *config : m_presets->get_edited_preset().config;
    if (Preset::printer_technology(cached_config) == ptSLA)
        return;

    // get extruders count
    auto* nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(cached_config.option("nozzle_diameter"));
    m_cache_extruder_count = nozzle_diameter->values.size(); //m_extruders_count;
}

bool TabPrinter::apply_extruder_cnt_from_cache()
{
    if (m_presets->get_edited_preset().printer_technology() == ptSLA)
        return false;

    if (m_cache_extruder_count > 0) {
        m_presets->get_edited_preset().set_num_extruders(m_cache_extruder_count);
        m_cache_extruder_count = 0;
        return true;
    }
    return false;
}

bool Tab::validate_custom_gcodes()
{
    if (m_type != Preset::TYPE_FILAMENT &&
        (m_type != Preset::TYPE_PRINTER || static_cast<TabPrinter*>(this)->m_printer_technology != ptFFF))
        return true;
    if (m_active_page->title() != L("Custom G-code"))
        return true;

    // When we switch Settings tab after editing of the custom g-code, then warning message could ba already shown after KillFocus event
    // and then it's no need to show it again
    if (validate_custom_gcodes_was_shown) {
        validate_custom_gcodes_was_shown = false;
        return true;
    }

    bool valid = true;
    for (auto opt_group : m_active_page->m_optgroups) {
        assert(opt_group->opt_map().size() == 1);
        if (!opt_group->is_activated())
            break;
        std::string key = opt_group->opt_map().begin()->first;
        valid &= validate_custom_gcode(opt_group->title, boost::any_cast<std::string>(opt_group->get_value(key)));
        if (!valid)
            break;
    }
    return valid;
}

void Tab::set_just_edit(bool just_edit)
{
    m_just_edit = just_edit;
    if (just_edit) {
        m_presets_choice->Disable();
        m_btn_delete_preset->Disable();
    } else {
        m_presets_choice->Enable();
        m_btn_delete_preset->Enable();
    }
}

void Tab::compatible_widget_reload(PresetDependencies &deps)
{
    Field* field = this->get_field(deps.key_condition);
    if (!field)
        return;

    bool has_any = ! m_config->option<ConfigOptionStrings>(deps.key_list)->values.empty();
    has_any ? deps.btn->Enable() : deps.btn->Disable();
    deps.checkbox->SetValue(! has_any);

    field->toggle(! has_any);
}

void Tab::set_tooltips_text()
{
    // --- Tooltip text for reset buttons (for whole options group)
    // Text to be shown on the "Revert to system" aka "Lock to system" button next to each input field.
    //m_ttg_value_lock =		_(L("LOCKED LOCK icon indicates that the settings are the same as the system (or default) values "
    //                            "for the current option group"));
    //m_ttg_value_unlock =	_(L("UNLOCKED LOCK icon indicates that some settings were changed and are not equal "
    //                            "to the system (or default) values for the current option group.\n"
    //                            "Click to reset all settings for current option group to the system (or default) values."));
    //m_ttg_white_bullet_ns =	_(L("WHITE BULLET icon indicates a non system (or non default) preset."));
    //m_ttg_non_system =		&m_ttg_white_bullet_ns;
    // Text to be shown on the "Undo user changes" button next to each input field.
    //m_ttg_white_bullet =	_(L("WHITE BULLET icon indicates that the settings are the same as in the last saved "
    //                            "preset for the current option group."));
    //m_ttg_value_revert =	_(L("BACK ARROW icon indicates that the settings were changed and are not equal to "
    //                            "the last saved preset for the current option group.\n"
    //                            "Click to reset all settings for the current option group to the last saved preset."));

    // --- Tooltip text for reset buttons (for each option in group)
    // Text to be shown on the "Revert to system" aka "Lock to system" button next to each input field.
    //m_tt_value_lock =		_(L("LOCKED LOCK icon indicates that the value is the same as the system (or default) value."));
    m_tt_value_unlock =		_(L("Click to reset current value and attach to the global value."));
    // 	m_tt_white_bullet_ns=	_(L("WHITE BULLET icon indicates a non system preset."));
    //m_tt_non_system =		&m_ttg_white_bullet_ns;
    // Text to be shown on the "Undo user changes" button next to each input field.
    //m_tt_white_bullet =		_(L("WHITE BULLET icon indicates that the value is the same as in the last saved preset."));
    m_tt_value_revert =		_(L("Click to drop current modify and reset to saved value."));
}

//BBS: GUI refactor
Page::Page(wxWindow* parent, const wxString& title, int iconID, wxPanel* tab_owner) :
        m_tab_owner(tab_owner),
        m_parent(parent),
        m_title(title),
        m_iconID(iconID)
{
    m_vsizer = (wxBoxSizer*)parent->GetSizer();
    m_page_title = NULL;
    m_item_color = &wxGetApp().get_label_clr_default();
}

void Page::reload_config()
{
    for (auto group : m_optgroups)
        group->reload_config();
}

void Page::update_visibility(ConfigOptionMode mode, bool update_contolls_visibility)
{
    bool ret_val = false;
#if HIDE_FIRST_SPLIT_LINE
    // BBS: no line spliter for first group
    bool first = true;
#endif
    for (auto group : m_optgroups) {
        ret_val = (update_contolls_visibility     ?
                   group->update_visibility(mode) :  // update visibility for all controlls in group
                   group->is_visible(mode)           // just detect visibility for the group
                   ) || ret_val;
#if HIDE_FIRST_SPLIT_LINE
        // BBS: no line spliter for first group
        if (update_contolls_visibility && ret_val && first) {
            if (group->stb) group->stb->Hide();
            first = false;
        }
#endif
    }

    m_show = ret_val;
#ifdef __WXMSW__
    if (!m_show) return;
    // BBS: fix field control position
    auto groups = this->m_optgroups;
    wxTheApp->CallAfter([groups]() {
        for (auto group : groups) {
            if (group->custom_ctrl) group->custom_ctrl->fixup_items_positions();
        }
    });
#endif
}

void Page::activate(ConfigOptionMode mode, std::function<void()> throw_if_canceled)
{
#if 0 // BBS: page title
    if (m_page_title == NULL) {
        m_page_title = new Label(Label::Head_18, _(m_title), m_parent);
        m_vsizer->AddSpacer(30);
        m_vsizer->Add(m_page_title, 0, wxALIGN_CENTER);
        m_vsizer->AddSpacer(20);
    }
#else
    //m_vsizer->AddSpacer(10);
#endif
#if HIDE_FIRST_SPLIT_LINE
    // BBS: no line spliter for first group
    bool first = true;
#endif
    for (auto group : m_optgroups) {
        if (!group->activate(throw_if_canceled))
            continue;
        m_vsizer->Add(group->sizer, 0, wxEXPAND | (group->is_legend_line() ? (wxLEFT|wxTOP) : wxALL), 5);
        group->update_visibility(mode);
#if HIDE_FIRST_SPLIT_LINE
        if (first) group->stb->Hide();
        first = false;
#endif
        group->reload_config();
        throw_if_canceled();
    }

#ifdef __WXMSW__
    // BBS: fix field control position
    wxTheApp->CallAfter([this]() {
        for (auto group : m_optgroups) {
            if (group->custom_ctrl)
                group->custom_ctrl->fixup_items_positions();
        }
    });
#endif
}

void Page::clear()
{
    for (auto group : m_optgroups)
        group->clear();
    m_page_title = NULL;
}

void Page::msw_rescale()
{
    for (auto group : m_optgroups)
        group->msw_rescale();
}

void Page::sys_color_changed()
{
    for (auto group : m_optgroups)
        group->sys_color_changed();
}

void Page::refresh()
{
    for (auto group : m_optgroups)
        group->refresh();
}

Field *Page::get_field(const t_config_option_key &opt_key, int opt_index /*= -1*/) const
{
    Field *field = nullptr;
    for (auto opt : m_optgroups) {
        field = opt->get_fieldc(opt_key, opt_index);
        if (field != nullptr) return field;
    }
    return field;
}

Line *Page::get_line(const t_config_option_key &opt_key)
{
    for (auto opt : m_optgroups)
        if (Line* line = opt->get_line(opt_key))
            return line;
    return nullptr;
}

bool Page::set_value(const t_config_option_key &opt_key, const boost::any &value)
{
    bool changed = false;
    for(auto optgroup: m_optgroups) {
        if (optgroup->set_value(opt_key, value))
            changed = true ;
    }
    return changed;
}

// package Slic3r::GUI::Tab::Page;
ConfigOptionsGroupShp Page::new_optgroup(const wxString &title, const wxString &icon, int noncommon_label_width /*= -1*/, bool is_extruder_og /* false */)
{
    //! config_ have to be "right"
    ConfigOptionsGroupShp optgroup  = is_extruder_og ? std::make_shared<ExtruderOptionsGroup>(m_parent, title, icon, m_config, true) // ORCA: add support for icons
        : std::make_shared<ConfigOptionsGroup>(m_parent, title, icon, m_config, true);
    optgroup->split_multi_line     = this->m_split_multi_line;
    optgroup->option_label_at_right = this->m_option_label_at_right;
    if (noncommon_label_width >= 0)
        optgroup->label_width = noncommon_label_width;

//BBS: GUI refactor
/*#ifdef __WXOSX__
    auto tab = parent()->GetParent()->GetParent();// GetParent()->GetParent();
#else
    auto tab = parent()->GetParent();// GetParent();
#endif*/
    auto tab = m_tab_owner;
    optgroup->set_config_category_and_type(m_title, static_cast<Tab*>(tab)->type());
    optgroup->m_on_change = [tab](t_config_option_key opt_key, boost::any value) {
        //! This function will be called from OptionGroup.
        //! Using of CallAfter is redundant.
        //! And in some cases it causes update() function to be recalled again
//!        wxTheApp->CallAfter([this, opt_key, value]() {
            static_cast<Tab*>(tab)->update_dirty();
            static_cast<Tab*>(tab)->on_value_change(opt_key, value);
//!        });
    };

    optgroup->m_get_initial_config = [tab]() {
        DynamicPrintConfig config = static_cast<Tab*>(tab)->m_presets->get_selected_preset().config;
        return config;
    };

    optgroup->m_get_sys_config = [tab]() {
        DynamicPrintConfig config = static_cast<Tab*>(tab)->m_presets->get_selected_preset_parent()->config;
        return config;
    };

    optgroup->have_sys_config = [tab]() {
        return static_cast<Tab*>(tab)->m_presets->get_selected_preset_parent() != nullptr;
    };

    optgroup->rescale_extra_column_item = [](wxWindow* win) {
        auto *ctrl = dynamic_cast<wxStaticBitmap*>(win);
        if (ctrl == nullptr)
            return;

        ctrl->SetBitmap(reinterpret_cast<ScalableBitmap*>(ctrl->GetClientData())->bmp());
    };

    m_optgroups.push_back(optgroup);

    return optgroup;
}

const ConfigOptionsGroupShp Page::get_optgroup(const wxString& title) const
{
    for (ConfigOptionsGroupShp optgroup : m_optgroups) {
        if (optgroup->title == title)
            return optgroup;
    }

    return nullptr;
}

void TabSLAMaterial::build()
{
    m_presets = &m_preset_bundle->sla_materials;
    load_initial_data();

    //auto page = add_options_page(L("Material"), "");

    //auto optgroup = page->new_optgroup(L("Material"));
    //optgroup->append_single_option_line("material_colour");
    //optgroup->append_single_option_line("bottle_cost");
    //optgroup->append_single_option_line("bottle_volume");
    //optgroup->append_single_option_line("bottle_weight");
    //optgroup->append_single_option_line("material_density");

    //optgroup->m_on_change = [this, optgroup](t_config_option_key opt_key, boost::any value)
    //{
    //    if (opt_key == "material_colour") {
    //        update_dirty();
    //        on_value_change(opt_key, value);
    //        return;
    //    }

    //    DynamicPrintConfig new_conf = *m_config;

    //    if (opt_key == "bottle_volume") {
    //        double new_bottle_weight =  boost::any_cast<double>(value)*(new_conf.option("material_density")->getFloat() / 1000);
    //        new_conf.set_key_value("bottle_weight", new ConfigOptionFloat(new_bottle_weight));
    //    }
    //    if (opt_key == "bottle_weight") {
    //        double new_bottle_volume =  boost::any_cast<double>(value)/new_conf.option("material_density")->getFloat() * 1000;
    //        new_conf.set_key_value("bottle_volume", new ConfigOptionFloat(new_bottle_volume));
    //    }
    //    if (opt_key == "material_density") {
    //        double new_bottle_volume = new_conf.option("bottle_weight")->getFloat() / boost::any_cast<double>(value) * 1000;
    //        new_conf.set_key_value("bottle_volume", new ConfigOptionFloat(new_bottle_volume));
    //    }

    //    load_config(new_conf);

    //    update_dirty();

    //    // BBS
    //    // Change of any from those options influences for an update of "Sliced Info"
    //    //wxGetApp().sidebar().Layout();
    //};

    //optgroup = page->new_optgroup(L("Layers"));
    //optgroup->append_single_option_line("initial_layer_height");

    //optgroup = page->new_optgroup(L("Exposure"));
    //optgroup->append_single_option_line("exposure_time");
    //optgroup->append_single_option_line("initial_exposure_time");

    //optgroup = page->new_optgroup(L("Corrections"));
    //auto line = Line{ m_config->def()->get("material_correction")->full_label, "" };
    //for (auto& axis : { "X", "Y", "Z" }) {
    //    auto opt = optgroup->get_option(std::string("material_correction_") + char(std::tolower(axis[0])));
    //    opt.opt.label = axis;
    //    line.append_option(opt);
    //}

    //optgroup->append_line(line);

    //page = add_options_page(L("Dependencies"), "wrench.png");
    //optgroup = page->new_optgroup(L("Profile dependencies"));

    //create_line_with_widget(optgroup.get(), "compatible_printers", "", [this](wxWindow* parent) {
    //    return compatible_widget_create(parent, m_compatible_printers);
    //});
    //
    //Option option = optgroup->get_option("compatible_printers_condition");
    //option.opt.full_width = true;
    //optgroup->append_single_option_line(option);

    //create_line_with_widget(optgroup.get(), "compatible_prints", "", [this](wxWindow* parent) {
    //    return compatible_widget_create(parent, m_compatible_prints);
    //});

    //option = optgroup->get_option("compatible_prints_condition");
    //option.opt.full_width = true;
    //optgroup->append_single_option_line(option);

    //build_preset_description_line(optgroup.get());

    //page = add_options_page(L("Material printing profile"), "printer.png");
    //optgroup = page->new_optgroup(L("Material printing profile"));
    //option = optgroup->get_option("material_print_speed");
    //optgroup->append_single_option_line(option);
}

// Reload current config (aka presets->edited_preset->config) into the UI fields.
void TabSLAMaterial::reload_config()
{
    this->compatible_widget_reload(m_compatible_printers);
    this->compatible_widget_reload(m_compatible_prints);
    Tab::reload_config();
}

void TabSLAMaterial::toggle_options()
{
    const Preset &current_printer = wxGetApp().preset_bundle->printers.get_edited_preset();
    std::string model = current_printer.config.opt_string("printer_model");
    m_config_manipulation.toggle_field("material_print_speed", model != "SL1");
}

void TabSLAMaterial::update()
{
    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptFFF)
        return;

    update_description_lines();
    Layout();

// #ys_FIXME. Just a template for this function
//     m_update_cnt++;
//     ! something to update
//     m_update_cnt--;
//
//     if (m_update_cnt == 0)
        wxGetApp().mainframe->on_config_changed(m_config);
}

void TabSLAPrint::build()
{
    m_presets = &m_preset_bundle->sla_prints;
    load_initial_data();

//    auto page = add_options_page(L("Layers and perimeters"), "layers");
//
//    auto optgroup = page->new_optgroup(L("Layers"));
//    optgroup->append_single_option_line("layer_height");
//    optgroup->append_single_option_line("faded_layers");
//
//    page = add_options_page(L("Supports"), "support"/*"sla_supports"*/);
//    optgroup = page->new_optgroup(L("Supports"));
//    optgroup->append_single_option_line("supports_enable");
//
//    optgroup = page->new_optgroup(L("Support head"));
//    optgroup->append_single_option_line("support_head_front_diameter");
//    optgroup->append_single_option_line("support_head_penetration");
//    optgroup->append_single_option_line("support_head_width");
//
//    optgroup = page->new_optgroup(L("Support pillar"));
//    optgroup->append_single_option_line("support_pillar_diameter");
//    optgroup->append_single_option_line("support_small_pillar_diameter_percent");
//    optgroup->append_single_option_line("support_max_bridges_on_pillar");
//
//    optgroup->append_single_option_line("support_pillar_connection_mode");
//    optgroup->append_single_option_line("support_buildplate_only");
//    // TODO: This parameter is not used at the moment.
//    // optgroup->append_single_option_line("support_pillar_widening_factor");
//    optgroup->append_single_option_line("support_base_diameter");
//    optgroup->append_single_option_line("support_base_height");
//    optgroup->append_single_option_line("support_base_safety_distance");
//
//    // Mirrored parameter from Pad page for toggling elevation on the same page
//    optgroup->append_single_option_line("support_object_elevation");
//
//    Line line{ "", "" };
//    line.full_width = 1;
//    line.widget = [this](wxWindow* parent) {
//        return description_line_widget(parent, &m_support_object_elevation_description_line);
//    };
//    optgroup->append_line(line);
//
//    optgroup = page->new_optgroup(L("Connection of the support sticks and junctions"));
//    optgroup->append_single_option_line("support_critical_angle");
//    optgroup->append_single_option_line("support_max_bridge_length");
//    optgroup->append_single_option_line("support_max_pillar_link_distance");
//
//    optgroup = page->new_optgroup(L("Automatic generation"));
//    optgroup->append_single_option_line("support_points_density_relative");
//    optgroup->append_single_option_line("support_points_minimal_distance");
//
//    page = add_options_page(L("Pad"), "");
//    optgroup = page->new_optgroup(L("Pad"));
//    optgroup->append_single_option_line("pad_enable");
//    optgroup->append_single_option_line("pad_wall_thickness");
//    optgroup->append_single_option_line("pad_wall_height");
//    optgroup->append_single_option_line("pad_brim_size");
//    optgroup->append_single_option_line("pad_max_merge_distance");
//    // TODO: Disabling this parameter for the beta release
////    optgroup->append_single_option_line("pad_edge_radius");
//    optgroup->append_single_option_line("pad_wall_slope");
//
//    optgroup->append_single_option_line("pad_around_object");
//    optgroup->append_single_option_line("pad_around_object_everywhere");
//    optgroup->append_single_option_line("pad_object_gap");
//    optgroup->append_single_option_line("pad_object_connector_stride");
//    optgroup->append_single_option_line("pad_object_connector_width");
//    optgroup->append_single_option_line("pad_object_connector_penetration");
//
//    page = add_options_page(L("Hollowing"), "hollowing");
//    optgroup = page->new_optgroup(L("Hollowing"));
//    optgroup->append_single_option_line("hollowing_enable");
//    optgroup->append_single_option_line("hollowing_min_thickness");
//    optgroup->append_single_option_line("hollowing_quality");
//    optgroup->append_single_option_line("hollowing_closing_distance");
//
//    page = add_options_page(L("Advanced"), "advanced");
//    optgroup = page->new_optgroup(L("Slicing"));
//    optgroup->append_single_option_line("slice_closing_radius");
//    optgroup->append_single_option_line("slicing_mode");
//
//    page = add_options_page(L("Output options"), "output+page_white");
//    optgroup = page->new_optgroup(L("Output file"));
//    Option option = optgroup->get_option("filename_format");
//    option.opt.full_width = true;
//    optgroup->append_single_option_line(option);
//
//    page = add_options_page(L("Dependencies"), "advanced");
//    optgroup = page->new_optgroup(L("Profile dependencies"));
//
//    create_line_with_widget(optgroup.get(), "compatible_printers", "", [this](wxWindow* parent) {
//        return compatible_widget_create(parent, m_compatible_printers);
//    });
//
//    option = optgroup->get_option("compatible_printers_condition");
//    option.opt.full_width = true;
//    optgroup->append_single_option_line(option);
//
//    build_preset_description_line(optgroup.get());
}

// Reload current config (aka presets->edited_preset->config) into the UI fields.
void TabSLAPrint::reload_config()
{
    this->compatible_widget_reload(m_compatible_printers);
    Tab::reload_config();
}

void TabSLAPrint::update_description_lines()
{
    Tab::update_description_lines();

    //if (m_active_page && m_active_page->title() == "Supports")
    //{
    //    bool is_visible = m_config->def()->get("support_object_elevation")->mode <= m_mode;
    //    if (m_support_object_elevation_description_line)
    //    {
    //        m_support_object_elevation_description_line->Show(is_visible);
    //        if (is_visible)
    //        {
    //            bool elev = !m_config->opt_bool("pad_enable") || !m_config->opt_bool("pad_around_object");
    //            m_support_object_elevation_description_line->SetText(elev ? "" :
    //                from_u8((boost::format(_u8L("\"%1%\" is disabled because \"%2%\" is on in \"%3%\" category.\n"
    //                    "To enable \"%1%\", please switch off \"%2%\""))
    //                    % _L("Object elevation") % _L("Pad around object") % _L("Pad")).str()));
    //        }
    //    }
    //}
}

void TabSLAPrint::toggle_options()
{
    if (m_active_page)
        m_config_manipulation.toggle_print_sla_options(m_config);
}

void TabSLAPrint::update()
{
    if (m_preset_bundle->printers.get_selected_preset().printer_technology() == ptFFF)
        return;

    m_update_cnt++;

    m_config_manipulation.update_print_sla_config(m_config, true);

    update_description_lines();
    //BBS: GUI refactor
    //Layout();
    m_parent->Layout();

    m_update_cnt--;

    if (m_update_cnt == 0) {
        toggle_options();

        // update() could be called during undo/redo execution
        // Update of objectList can cause a crash in this case (because m_objects doesn't match ObjectList)
        if (!wxGetApp().plater()->inside_snapshot_capture())
            wxGetApp().obj_list()->update_and_show_object_settings_item();

        wxGetApp().mainframe->on_config_changed(m_config);
    }
}

void TabSLAPrint::clear_pages()
{
    Tab::clear_pages();

    m_support_object_elevation_description_line = nullptr;
}

ConfigManipulation Tab::get_config_manipulation()
{
    auto load_config = [this]()
    {
        update_dirty();
        // Initialize UI components with the config values.
        reload_config();
        update();
    };

    auto cb_toggle_field = [this](const t_config_option_key& opt_key, bool toggle, int opt_index) {
        return toggle_option(opt_key, toggle, opt_index);
    };

    auto cb_toggle_line = [this](const t_config_option_key& opt_key, bool toggle) {
        return toggle_line(opt_key, toggle);
    };

    auto cb_value_change = [this](const std::string& opt_key, const boost::any& value) {
        return on_value_change(opt_key, value);
    };

    return ConfigManipulation(load_config, cb_toggle_field, cb_toggle_line, cb_value_change, nullptr, this);
}


} // GUI
} // Slic3r
