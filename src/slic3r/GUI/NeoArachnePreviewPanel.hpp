// NEOTKO_NEOARACHNE_TAG preview-lab PL.2+PL.3+PL.6+PL.8
//
// PL.8 (s94) restructured the original single-canvas panel into a vertical
// container:
//   ┌─ NeoArachnePreviewPanel ────────┐
//   │  NeoArachnePreviewCanvas (paint)│
//   │  ──────────────────────────────  │
//   │  layer Z slider                  │
//   │  radio: W / Wedge / Selected     │
//   │  [ Use current selection ] btn   │
//   └─────────────────────────────────┘
// The canvas is a child wxPanel that owns the buffered+GCDC paint, so the
// outer panel doesn't need wxBG_STYLE_PAINT and won't conflict with child
// widget rendering. The outer panel keeps all reactive state — debounce,
// worker dispatch, snapshot of the selected ModelVolume, and the current
// PreviewGeometrySource that feeds the slicer.
#ifndef slic3r_GUI_NeoArachnePreviewPanel_hpp_
#define slic3r_GUI_NeoArachnePreviewPanel_hpp_

#include <wx/panel.h>
#include <wx/timer.h>

#include <atomic>
#include <memory>

#include "../../libslic3r/NeoArachne/Preview/PreviewResult.hpp"
#include "../../libslic3r/NeoArachne/Preview/PreviewGeometrySource.hpp"

class wxSlider;
class wxRadioBox;
class wxButton;
class wxStaticText;
class wxCheckBox;

namespace Slic3r {
class DynamicPrintConfig;
class TriangleMesh;
namespace NeoArachne { namespace Preview { struct ConfigSnapshot; }}
}

namespace Slic3r { namespace GUI {

class Tab;
class NeoArachnePreviewCanvas;

class NeoArachnePreviewPanel : public wxPanel
{
public:
    // `tab` is the live Tab that owns the optgroup. We read its `m_config`
    // (the merged edited config including any per-plate/per-object overrides)
    // so the canvas reflects whatever the user is editing right now — not the
    // bundle's saved preset, which lags behind per-object overrides.
    NeoArachnePreviewPanel(wxWindow* parent, Tab* tab);
    ~NeoArachnePreviewPanel() override;

    // Schedules a re-slice (debounced). Safe to call from any GUI-thread event
    // handler. The actual compute happens on a worker thread; the result is
    // delivered back to this panel via a CallAfter and triggers a canvas
    // Refresh().
    void schedule_refresh();

private:
    void on_poll_timer(wxTimerEvent&);
    void on_debounce_timer(wxTimerEvent&);

    void on_layer_slider(wxCommandEvent&);
    void on_geom_radio(wxCommandEvent&);
    void on_use_selected(wxCommandEvent&);
    void on_translucent_toggle(wxCommandEvent&);
    void on_borders_toggle(wxCommandEvent&);
    void on_fit_clicked(wxCommandEvent&);
    // v3 — G-code preview controls
    void on_play_clicked(wxCommandEvent&);
    void on_speed_slider(wxCommandEvent&);
    void on_show_seams_toggle(wxCommandEvent&);
    void on_show_travels_toggle(wxCommandEvent&);
    void on_anim_timer(wxTimerEvent&);
    void on_dump_clicked(wxCommandEvent&);
    void on_build_mode_toggle(wxCommandEvent&);

    void launch_async_slice();
    size_t hash_current_relevant_config() const;

    // Snapshots whatever ModelVolume is currently selected in the Plater
    // (mesh + instance/volume transform baked in) and stores the result on
    // the panel. Returns empty string on success, an error message
    // otherwise (no selection / too many / wrong type / etc.).
    std::string snapshot_selected_model_volume();

    // Updates the slider min/max to match the current snapshot's Z bbox.
    // No-op if there's no snapshot or kind != FromMesh.
    void refresh_slider_range_from_snapshot();

    // Resolves the current source (kind from radio, mesh from snapshot,
    // slice_z from slider) into a PreviewGeometrySource the worker can
    // consume.
    NeoArachne::Preview::PreviewGeometrySource current_source() const;

    struct AliveFlag { std::atomic<bool> alive{true}; };

    // ── owners / live config source ──────────────────────────────────────
    Tab*                                        m_tab = nullptr;

    // ── child widgets ────────────────────────────────────────────────────
    NeoArachnePreviewCanvas*                    m_canvas         = nullptr;
    wxSlider*                                   m_layer_slider   = nullptr;
    wxStaticText*                               m_layer_label    = nullptr;
    wxRadioBox*                                 m_geom_radio     = nullptr;
    wxButton*                                   m_use_btn        = nullptr;
    wxCheckBox*                                 m_translucent_cb = nullptr;
    wxCheckBox*                                 m_borders_cb     = nullptr;
    wxButton*                                   m_fit_btn        = nullptr;
    // v3 — G-code real preview controls
    wxButton*                                   m_play_btn       = nullptr;
    wxSlider*                                   m_speed_slider   = nullptr;
    wxStaticText*                               m_speed_label    = nullptr;
    wxCheckBox*                                 m_show_seams_cb  = nullptr;
    wxCheckBox*                                 m_show_travels_cb= nullptr;
    wxCheckBox*                                 m_build_mode_cb  = nullptr;
    wxButton*                                   m_dump_btn       = nullptr;

    // ── reactive plumbing ────────────────────────────────────────────────
    wxTimer                                     m_poll_timer;       // 250 ms cadence — detects config edits via hash
    wxTimer                                     m_debounce_timer;   // one-shot 300 ms
    wxTimer                                     m_anim_timer;       // 33 ms (~30 FPS) when playing — drives head animation
    std::atomic<std::uint64_t>                  m_request_id{0};
    size_t                                      m_last_config_hash = 0;
    std::shared_ptr<AliveFlag>                  m_alive;            // worker checks before CallAfter

    // ── snapshot state ───────────────────────────────────────────────────
    // Mesh snapshot frozen at the moment the user clicked "Use selection".
    // Subsequent moves/rotations of the source object on the bed do NOT
    // propagate — the user must click again to refresh.
    std::shared_ptr<const TriangleMesh>         m_mesh_snapshot;
    double                                      m_snapshot_z_min   = 0.0;
    double                                      m_snapshot_z_max   = 0.0;

    // ── last result ──────────────────────────────────────────────────────
    std::unique_ptr<NeoArachne::Preview::PreviewResult> m_result;   // last successful slice
    bool                                        m_pending = false;  // a slice is in flight

    // ── v3 animation state ───────────────────────────────────────────────
    // m_anim_pos_scaled walks the cumulative chain length (mix of extrusion
    // + travel in scaled coord units). Reset to 0 on every new result. The
    // head dot interpolates inside the segment that contains it. Playing is
    // a toggle; when off the head stays parked at its last position.
    bool                                        m_playing          = false;
    double                                      m_anim_pos_scaled  = 0.0;
    double                                      m_anim_speed_mult  = 0.1;   // multiplier on base speed — matches slider tick 20 default

    // ── fix #4A (s96): Z-snap trace ──────────────────────────────────────
    // Recorded on every launch_async_slice when src.kind == FromMesh. The
    // dump surfaces these so the user can confirm the preview is slicing at
    // the same mesh-Z the real slicer's layer plane lands on (mid-layer
    // convention, adjusted for initial_layer_print_height). −1 = N/A.
    double                                      m_last_slice_z_raw  = -1.0;
    double                                      m_last_slice_z_used = -1.0;
    int                                         m_last_slice_layer_n = -1;
    int                                         m_last_slice_total_n = -1;
};

}} // namespace Slic3r::GUI

#endif
