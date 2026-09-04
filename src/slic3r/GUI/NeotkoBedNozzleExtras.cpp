// NEOTKO_TOOLSLEEP_TAG s294 — see NeotkoBedNozzleExtras.hpp for why this lives outside the presets.
#include "NeotkoBedNozzleExtras.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r { namespace GUI {

static const char* TOOLSLEEP_KEY = "neotko_idle_tool_power_down";
static const char* DEEPSLEEP_KEY = "neotko_idle_tool_deep_sleep";

// Is a standby temperature command actually being emitted? Without one there is nothing for the
// post-processor to rewrite, so the toggle would silently do nothing. Checked here so the dialog can
// say so up front instead of letting the user find out from a warning after slicing.
static bool ooze_prevention_active()
{
    const DynamicPrintConfig& cfg = wxGetApp().preset_bundle->full_config();
    const ConfigOptionBool*   opt = cfg.option<ConfigOptionBool>("ooze_prevention");
    return opt != nullptr && opt->value;
}

NeotkoBedNozzleExtrasDialog::NeotkoBedNozzleExtrasDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Bed and Nozzle Extras"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
{
    build();
}

void NeotkoBedNozzleExtrasDialog::build()
{
    // No explicit background: UpdateDlgDarkUI below paints the dialog for the active theme, and
    // hard-coding white here is exactly what broke day/night mode elsewhere (s291).
    const int pad    = FromDIP(20);
    const int wrap   = FromDIP(360);
    const int indent = FromDIP(26);   // lines the body text up past the checkbox, under the title

    wxBoxSizer* root = new wxBoxSizer(wxVERTICAL);
    root->AddSpacer(pad);

    // ---- Turn off unused hotends fully ------------------------------------------------------
    wxBoxSizer* row = new wxBoxSizer(wxHORIZONTAL);
    m_toolsleep_cb  = new CheckBox(this);
    m_toolsleep_cb->SetValue(wxGetApp().app_config->get_bool(TOOLSLEEP_KEY));
    row->Add(m_toolsleep_cb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    row->Add(new Label(this, Label::Head_13, _L("Turn off unused hotends fully (0 °C)")), 0,
             wxALIGN_CENTER_VERTICAL);
    root->Add(row, 0, wxLEFT | wxRIGHT, pad);

    // Everything below the title hangs off its own sizer, indented once, so the copy reads as a
    // caption of the checkbox instead of as a second column of the dialog.
    wxBoxSizer* copy = new wxBoxSizer(wxVERTICAL);

    auto add_caption = [&](const wxString& text, const wxColour& colour) {
        Label* l = new Label(this, Label::Body_12, text);
        l->Wrap(wrap);
        l->SetForegroundColour(colour);
        copy->Add(l, 0, wxTOP, FromDIP(8));
        return l;
    };

    const wxColour dim("#6B6B6B");
    // Short on purpose: what it does, then why it is worth having on. The number behind the "3 W"
    // is in docs/FUTURE/IDLE_TOOL_POWER_DOWN.md §3.
    add_caption(_L("After a tool's last extrusion, its standby command is set to 0 °C instead of the "
                   "idle temperature, so it stops heating for the rest of the print."), dim);
    add_caption(_L("Saves around 3 W per idle hotend, and stops the filament parked in the nozzle "
                   "from cooking while it waits. A tool is only switched off when nothing uses it "
                   "again, so no print ever waits for a reheat."), dim);

    // The one real caveat, shown only when it applies.
    if (!ooze_prevention_active())
        m_toolsleep_note = add_caption(
            _L("Ooze prevention is off in this print profile, so there is no standby command to "
               "rewrite and nothing would be turned off."),
            wxColour(230, 130, 40));

    root->Add(copy, 0, wxLEFT | wxRIGHT, pad + indent);
    root->AddSpacer(FromDIP(18));

    // ---- Extra Energy Save (rides on the one above) -------------------------------------------
    wxBoxSizer* row2 = new wxBoxSizer(wxHORIZONTAL);
    m_deepsleep_cb   = new CheckBox(this);
    m_deepsleep_cb->SetValue(wxGetApp().app_config->get_bool(DEEPSLEEP_KEY));
    row2->Add(m_deepsleep_cb, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));
    Label* deep_title = new Label(this, Label::Head_13, _L("Extra Energy Save mode"));
    m_deepsleep_dim.push_back(deep_title);
    row2->Add(deep_title, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(row2, 0, wxLEFT | wxRIGHT, pad);

    wxBoxSizer* copy2 = new wxBoxSizer(wxVERTICAL);
    Label* deep_body  = new Label(
        this, Label::Body_12,
        _L("Also switches a tool off while it waits, not only when it has finished for good. A tool "
           "that prints on layer 2 and is not needed again until layer 200 sits at idle temperature "
           "for the whole gap; this puts it at 0 °C instead."));
    deep_body->Wrap(wrap);
    deep_body->SetForegroundColour(dim);
    copy2->Add(deep_body, 0, wxTOP, FromDIP(8));
    m_deepsleep_dim.push_back(deep_body);

    Label* deep_wait = new Label(
        this, Label::Body_12,
        _L("It costs no print time. A tool coming back too soon to be worth cooling keeps its heat: "
           "the preheat that Orca already schedules cancels the shutdown. Past that, preheating "
           "starts well before the tool is picked up, so it is at temperature when it is needed."));
    deep_wait->Wrap(wrap);
    deep_wait->SetForegroundColour(dim);
    copy2->Add(deep_wait, 0, wxTOP, FromDIP(8));
    m_deepsleep_dim.push_back(deep_wait);

    root->Add(copy2, 0, wxLEFT | wxRIGHT, pad + indent);
    root->AddSpacer(pad);

    m_toolsleep_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) { sync_enabled(); e.Skip(); });
    sync_enabled();

    DialogButtons* buttons = new DialogButtons(this, { "OK", "Cancel" });
    if (Button* ok = buttons->GetOK())
        ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            apply();
            EndModal(wxID_OK);
        });
    if (Button* cancel = buttons->GetCANCEL())
        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    root->Add(buttons, 0, wxEXPAND);

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });

    wxGetApp().UpdateDlgDarkUI(this);
    SetSizerAndFit(root);
    CenterOnParent();
}

// Extra Energy Save is meaningless with its parent off, so it greys out rather than sitting there
// ticked and doing nothing.
void NeotkoBedNozzleExtrasDialog::sync_enabled()
{
    const bool on = m_toolsleep_cb->GetValue();
    if (m_deepsleep_cb != nullptr)
        m_deepsleep_cb->Enable(on);
    for (wxWindow* w : m_deepsleep_dim)
        w->Enable(on);
}

void NeotkoBedNozzleExtrasDialog::apply()
{
    auto store = [this](const char* key, bool now) {
        if (wxGetApp().app_config->get_bool(key) != now) {
            wxGetApp().app_config->set_bool(key, now);
            m_changed = true;
        }
    };
    store(TOOLSLEEP_KEY, m_toolsleep_cb->GetValue());
    store(DEEPSLEEP_KEY, m_deepsleep_cb->GetValue());
}

void NeotkoBedNozzleExtrasDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Fit();
    Refresh();
}

bool neotko_show_bed_nozzle_extras(wxWindow* parent)
{
    NeotkoBedNozzleExtrasDialog dlg(parent);
    return dlg.ShowModal() == wxID_OK && dlg.changed();
}

}} // namespace Slic3r::GUI
