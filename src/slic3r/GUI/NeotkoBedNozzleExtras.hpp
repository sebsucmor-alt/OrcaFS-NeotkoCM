// NEOTKO_TOOLSLEEP_TAG s294 — "Bed and Nozzle Extras", the little panel that hangs under the Bed
// type combo in the sidebar's Printer section.
//
// Why it exists as its own home instead of a checkbox in a Tab: everything in here is a house
// preference, not a slicing parameter. It lives in app_config, exactly like the "Bed type" combo
// right above it (Plater.cpp reads curr_bed_type from app_config too), so it survives a Snapmaker
// profile update, never marks a preset as modified, and never collides when new profiles land.
//
// First tenant: "Turn off unused hotends fully". See docs/FUTURE/IDLE_TOOL_POWER_DOWN.md.
#ifndef slic3r_GUI_NeotkoBedNozzleExtras_hpp_
#define slic3r_GUI_NeotkoBedNozzleExtras_hpp_

#include "GUI_Utils.hpp"

#include <vector>

class CheckBox;
class wxStaticText;

namespace Slic3r { namespace GUI {

class NeotkoBedNozzleExtrasDialog : public DPIDialog
{
public:
    explicit NeotkoBedNozzleExtrasDialog(wxWindow* parent);

    // True if anything in here changed, so the caller can invalidate the slice result.
    bool changed() const { return m_changed; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build();
    void apply();

    CheckBox*     m_toolsleep_cb{ nullptr };
    CheckBox*     m_deepsleep_cb{ nullptr };
    // Title plus body text of Extra Energy Save, greyed out together with its checkbox.
    std::vector<wxWindow*> m_deepsleep_dim;
    // Extra Energy Save does nothing on its own, so it follows its parent's state.
    void sync_enabled();
    wxStaticText* m_toolsleep_note{ nullptr };
    bool          m_changed{ false };
};

// Returns true if the user changed something (so the caller can invalidate the slice).
bool neotko_show_bed_nozzle_extras(wxWindow* parent);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_NeotkoBedNozzleExtras_hpp_
