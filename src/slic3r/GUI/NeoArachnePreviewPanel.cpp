// NEOTKO_NEOARACHNE_TAG preview-lab PL.2+PL.3+PL.6+PL.8
#include "NeoArachnePreviewPanel.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "GLCanvas3D.hpp"
#include "Tab.hpp"
#include "../../libslic3r/Model.hpp"
#include "../../libslic3r/PresetBundle.hpp"
#include "../../libslic3r/PrintConfig.hpp"
#include "../../libslic3r/Flow.hpp"
#include "../../libslic3r/ExtrusionEntity.hpp"
#include "../../libslic3r/ExtrusionEntityCollection.hpp"
#include "../../libslic3r/ExPolygon.hpp"
#include "../../libslic3r/TriangleMesh.hpp"
#include "../../libslic3r/libslic3r.h"

#include "../../libslic3r/NeoArachne/Preview/PreviewConfigSnapshot.hpp"
#include "../../libslic3r/NeoArachne/Preview/PreviewGeometrySource.hpp"
#include "../../libslic3r/NeoArachne/Preview/PreviewSlicer.hpp"

namespace Slic3r { namespace GUI {

namespace NPrev = NeoArachne::Preview;

namespace {

constexpr int kCanvasMinW = 220;
constexpr int kCanvasMinH = 220;

// 30% opacity for extrusion strokes and the fill region when the user has
// the Translucent toggle on. Orca's main G-code preview paints these as
// opaque solids, which hides the overlap zones where adjacent beads /
// outer↔inner walls collide. Painting at alpha 76 (~30%) lets those overlap
// regions show through as colour blends — a debugging aid the main viewport
// doesn't provide.
constexpr unsigned char kStrokeAlphaTranslucent = 76;
constexpr unsigned char kStrokeAlphaOpaque      = 255;
constexpr unsigned char kFillAlphaTranslucent   = 76;
constexpr unsigned char kFillAlphaOpaque        = 230;
// Border draw: a 2 px-wider opaque outline drawn underneath each capsule.
// Mimics the slicer's main view "rotated squares give shadow" feel without
// abandoning the translucent fill, so adjacent extrusions still blend.
constexpr int           kBorderExtraPx          = 2;
constexpr unsigned char kBorderAlpha            = 220;
constexpr double        kBorderDarkenFactor     = 0.45;

// Zoom limits. 0.25× lets the user back out if they overshoot; 20× is plenty
// for inspecting a single bead transition without the renderer going crazy
// with sub-pixel jitter.
constexpr double kZoomMin  = 0.25;
constexpr double kZoomMax  = 20.0;
constexpr double kZoomStep = 1.15;   // multiplicative per wheel tick

// Slider precision: we expose layer Z in 0.05 mm increments. That's 1/4 of
// the default layer height (0.2 mm) — fine enough to step through layers
// individually, coarse enough that the slider doesn't feel sluggish.
constexpr int    kSliderTicksPerMm = 20;
constexpr double kSliderStepMm     = 1.0 / kSliderTicksPerMm;

// Keys that meaningfully affect the W preview. Changes to other config keys
// don't kick off a re-slice — keeps the hash check trivially cheap and avoids
// thrashing the worker on unrelated edits.
const std::vector<std::string>& relevant_config_keys()
{
    static const std::vector<std::string> keys = {
        "wall_generator",
        "neoarachne_outer_wall",
        "neoarachne_inner_walls",
        "neoarachne_gap_fill",
        "neoarachne_allowed_overlap_pct",
        "neoarachne_min_bead_width_pct",
        "neoarachne_max_bead_width_pct",
        "neoarachne_min_feature_size_pct",
        "neoarachne_keep_short_tails",
        "neoarachne_bead_count_hysteresis_pct",
        "neoarachne_transition_filter_dist_mm",
        "wall_loops",
        "outer_wall_line_width",
        "inner_wall_line_width",
        "line_width",
        "nozzle_diameter",
        "layer_height",
        "wall_filament",
        "bridge_flow",
        "resolution",
        "xy_contour_compensation",
        "xy_hole_compensation",
    };
    return keys;
}

// Builds the live merged config used by both the wall-generator probe and the
// snapshot capture. Starts from the bundle's full_config() (nozzle/filament
// keys + saved print preset) and overlays the Tab's live m_config on top — so
// per-object or per-plate overrides edited in this very Tab are reflected
// immediately, without waiting for the user to save the preset.
DynamicPrintConfig live_merged_config(Tab* tab)
{
    DynamicPrintConfig out;
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb != nullptr)
        out = pb->full_config();
    if (tab != nullptr) {
        if (DynamicPrintConfig* live = tab->get_config())
            out.apply(*live, /*ignore_nonexistent=*/true);
    }
    return out;
}

NPrev::ConfigSnapshot capture_snapshot_from_tab(Tab* tab)
{
    NPrev::ConfigSnapshot snap;
    DynamicPrintConfig dyn = live_merged_config(tab);
    if (dyn.keys().empty())
        return snap;

    FullPrintConfig full;
    full.apply(dyn, /*ignore_unknown_keys=*/true);

    snap.region = static_cast<const PrintRegionConfig&>(full);
    snap.object = static_cast<const PrintObjectConfig&>(full);
    snap.print  = static_cast<const PrintConfig&>(full);

    snap.layer_height = snap.object.layer_height.value > 0.0 ? snap.object.layer_height.value : 0.2;

    snap.force_isolated_layer_defaults();

    const int   extruder_idx  = std::max(0, snap.region.wall_filament.value - 1);
    const float nozzle_d      = float(snap.print.nozzle_diameter.get_at(extruder_idx));
    const float h             = float(snap.layer_height);

    const ConfigOptionFloatOrPercent& ext_w = snap.region.outer_wall_line_width.value > 0
        ? snap.region.outer_wall_line_width : snap.object.line_width;
    const ConfigOptionFloatOrPercent& inn_w = snap.region.inner_wall_line_width.value > 0
        ? snap.region.inner_wall_line_width : snap.object.line_width;
    const ConfigOptionFloatOrPercent& sol_w = snap.region.internal_solid_infill_line_width.value > 0
        ? snap.region.internal_solid_infill_line_width : snap.object.line_width;

    snap.ext_perimeter_flow         = Flow::new_from_config_width(frExternalPerimeter, ext_w, nozzle_d, h);
    snap.perimeter_flow             = Flow::new_from_config_width(frPerimeter,         inn_w, nozzle_d, h);
    snap.solid_infill_flow          = Flow::new_from_config_width(frSolidInfill,       sol_w, nozzle_d, h);
    snap.overhang_flow              = snap.perimeter_flow.with_flow_ratio(snap.region.bridge_flow);
    snap.smaller_ext_perimeter_flow = snap.ext_perimeter_flow;

    return snap;
}

// Diagnostic-friendly probe — returns a short tag describing what we read
// from the live merged config. Empty string means "NeoArachne is active and
// preview should render".
std::string probe_wall_generator_state(Tab* tab)
{
    DynamicPrintConfig dyn = live_merged_config(tab);
    if (dyn.keys().empty()) return "config empty";
    const ConfigOption* opt = dyn.option("wall_generator");
    if (opt == nullptr) return "wall_generator missing";
    const int raw = opt->getInt();
    if (raw == int(PerimeterGeneratorType::NeoArachne)) return "";  // OK
    switch (raw) {
        case int(PerimeterGeneratorType::Classic): return "wall_gen=Classic";
        case int(PerimeterGeneratorType::Arachne): return "wall_gen=Arachne";
        default: break;
    }
    return std::string("wall_gen=raw:") + std::to_string(raw);
}

// Recursive renderer — walks every ExtrusionPath inside loops/multipath/collections.
//
// Color convention mirrors how the main viewport visualises NeoArachne output:
//   * external perimeter  → orange   (Classic outer)
//   * closed inner loops  → yellow   (Arachne even-pass beads)
//   * ExtrusionMultiPath  → purple   (Arachne odd-pass / gap-equivalent beads)
//   * dedicated gap_fill  → red      (only present in legacy 3-pass combo)
enum class DrawPass { Border, Fill };

wxColour role_base_colour_for(ExtrusionRole role, bool from_multipath)
{
    if (from_multipath) return wxColour(180,  80, 255);   // purple — odd Arachne bead
    switch (role) {
        case erExternalPerimeter: return wxColour(255,  90,  20);
        case erPerimeter:         return wxColour(255, 215,  35);
        case erOverhangPerimeter: return wxColour(180,  80, 255);
        case erGapFill:           return wxColour(225,  40,  40);
        case erInternalInfill:    return wxColour(140, 140, 140);
        case erSolidInfill:       return wxColour(190, 190, 190);
        case erTopSolidInfill:    return wxColour(220, 220, 220);
        default:                  return wxColour(255, 200,   0);
    }
}

wxColour role_base_colour(const ExtrusionPath& path, bool is_odd_carrier)
{
    return role_base_colour_for(path.role(), is_odd_carrier);
}

inline wxColour darken(const wxColour& c, double factor, unsigned char alpha)
{
    return wxColour(
        static_cast<unsigned char>(std::max(0, std::min(255, int(c.Red()   * factor)))),
        static_cast<unsigned char>(std::max(0, std::min(255, int(c.Green() * factor)))),
        static_cast<unsigned char>(std::max(0, std::min(255, int(c.Blue()  * factor)))),
        alpha);
}

void render_entity(wxDC& dc, const ExtrusionEntity& e, double scale, const BoundingBox& bbox,
                   int screen_h, int margin_x, int margin_y,
                   DrawPass pass, unsigned char fill_alpha, bool from_multipath = false)
{
    if (const auto* sub = dynamic_cast<const ExtrusionEntityCollection*>(&e)) {
        for (const ExtrusionEntity* c : sub->entities)
            if (c) render_entity(dc, *c, scale, bbox, screen_h, margin_x, margin_y,
                                 pass, fill_alpha, from_multipath);
        return;
    }

    auto draw_path = [&](const ExtrusionPath& path, bool is_odd_carrier) {
        const wxColour base = role_base_colour(path, is_odd_carrier);
        // path.width is in mm, scale is in pixels per *scaled* coord unit
        // (because bbox uses scaled units). To get pixels we have to scale
        // the mm width up to the same unit system first. Without this the
        // thickness floors to ~0 and the round pen draws a 1 px stroke
        // instead of the real extrusion volume.
        const int fill_thickness   = std::max(1, int(scaled<double>(double(path.width)) * scale));
        const int border_thickness = fill_thickness + kBorderExtraPx;

        wxColour col;
        int      thickness;
        if (pass == DrawPass::Border) {
            col       = darken(base, kBorderDarkenFactor, kBorderAlpha);
            thickness = border_thickness;
        } else {
            col       = wxColour(base.Red(), base.Green(), base.Blue(), fill_alpha);
            thickness = fill_thickness;
        }

        wxPen pen(col, thickness, wxPENSTYLE_SOLID);
        pen.SetCap(wxCAP_ROUND);
        pen.SetJoin(wxJOIN_ROUND);
        dc.SetPen(pen);
        const Points& pts = path.polyline.points;
        for (size_t i = 1; i < pts.size(); ++i) {
            const wxPoint p0(margin_x + int((pts[i-1].x() - bbox.min.x()) * scale),
                             screen_h - margin_y - int((pts[i-1].y() - bbox.min.y()) * scale));
            const wxPoint p1(margin_x + int((pts[i].x()   - bbox.min.x()) * scale),
                             screen_h - margin_y - int((pts[i].y()   - bbox.min.y()) * scale));
            dc.DrawLine(p0, p1);
        }
    };

    if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&e)) {
        for (const ExtrusionPath& p : loop->paths) draw_path(p, /*is_odd_carrier=*/false);
    } else if (const auto* mp = dynamic_cast<const ExtrusionMultiPath*>(&e)) {
        for (const ExtrusionPath& p : mp->paths) draw_path(p, /*is_odd_carrier=*/true);
    } else if (const auto* p = dynamic_cast<const ExtrusionPath*>(&e)) {
        draw_path(*p, from_multipath);
    }
}

void render_fill_region(wxDC& dc, const SurfaceCollection& sc, double scale,
                        const BoundingBox& bbox, int screen_h, int margin_x, int margin_y,
                        unsigned char fill_alpha)
{
    dc.SetBrush(wxBrush(wxColour(70, 70, 80, fill_alpha)));
    dc.SetPen(*wxTRANSPARENT_PEN);
    for (const Surface& s : sc.surfaces) {
        const Points& pts = s.expolygon.contour.points;
        if (pts.size() < 3) continue;
        std::vector<wxPoint> wpts;
        wpts.reserve(pts.size());
        for (const Point& p : pts) {
            wpts.emplace_back(
                margin_x + int((p.x() - bbox.min.x()) * scale),
                screen_h - margin_y - int((p.y() - bbox.min.y()) * scale));
        }
        dc.DrawPolygon(int(wpts.size()), wpts.data());
    }
}

} // namespace

// ────────────────────────────────────────────────────────────────────────────
// Inner canvas widget. Pure paint surface — owns no state beyond the result
// pointer it's told to render. The outer panel pushes new results via
// set_result() and triggers a Refresh().
// ────────────────────────────────────────────────────────────────────────────

class NeoArachnePreviewCanvas : public wxPanel
{
public:
    NeoArachnePreviewCanvas(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(kCanvasMinW, kCanvasMinH),
                  wxBORDER_THEME)
    {
        SetMinSize(wxSize(kCanvasMinW, kCanvasMinH));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(20, 20, 24));
        Bind(wxEVT_PAINT,       &NeoArachnePreviewCanvas::on_paint,      this);
        Bind(wxEVT_MOUSEWHEEL,  &NeoArachnePreviewCanvas::on_mouse_wheel, this);
        Bind(wxEVT_LEFT_DOWN,   &NeoArachnePreviewCanvas::on_left_down,   this);
        Bind(wxEVT_LEFT_UP,     &NeoArachnePreviewCanvas::on_left_up,     this);
        Bind(wxEVT_MOTION,      &NeoArachnePreviewCanvas::on_motion,      this);
        Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&){ m_panning = false; });
    }

    void set_result(const NPrev::PreviewResult* r) { m_result = r; Refresh(false); }
    void set_pending(bool v)                       { m_pending = v; Refresh(false); }
    void set_status(const wxString& s)             { m_status_override = s; Refresh(false); }
    void clear_status()                            { m_status_override.clear(); Refresh(false); }

    void set_translucent(bool v)                   { m_translucent = v; Refresh(false); }
    void set_borders(bool v)                       { m_borders = v;     Refresh(false); }
    void reset_view()                              { m_zoom = 1.0; m_pan_x = 0; m_pan_y = 0; Refresh(false); }
    // v3 — toggles + animation position (scaled coord units along the chain)
    void set_show_seams(bool v)                    { m_show_seams = v;   Refresh(false); }
    void set_show_travels(bool v)                  { m_show_travels = v; Refresh(false); }
    void set_anim_pos(double scaled_pos, bool playing) { m_anim_pos_scaled = scaled_pos; m_playing = playing; Refresh(false); }
    void set_build_mode(bool v)                    { m_build_mode = v;   Refresh(false); }

private:
    void on_paint(wxPaintEvent&)
    {
        // Two-layer DC: the buffered paint DC handles flicker-free background
        // clearing; wxGCDC wraps it so all subsequent drawing flows through a
        // wxGraphicsContext (Quartz on macOS / Cairo on GTK / GDI+ on MSW)
        // which is the only path that respects the alpha channel on wxColour.
        // Plain wxDC discards alpha — that's why Orca's main viewport renders
        // solid.
        wxAutoBufferedPaintDC bdc(this);
        bdc.SetBackground(wxBrush(wxColour(20, 20, 24)));
        bdc.Clear();
        wxGCDC dc(bdc);
        if (wxGraphicsContext* gc = dc.GetGraphicsContext())
            gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

        const wxSize sz = GetClientSize();
        const int    W  = sz.GetWidth();
        const int    H  = sz.GetHeight();
        constexpr int margin = 12;

        auto draw_status = [&](const wxString& msg, const wxColour& col) {
            dc.SetTextForeground(col);
            dc.SetFont(GetFont());
            const wxSize ts = dc.GetTextExtent(msg);
            dc.DrawText(msg, std::max(4, (W - ts.GetWidth()) / 2),
                              std::max(4, (H - ts.GetHeight()) / 2));
        };

        if (!m_status_override.empty()) {
            draw_status(m_status_override, wxColour(220, 110, 110));
            return;
        }
        if (m_result == nullptr) {
            draw_status(m_pending ? _("Slicing preview…") : _("Preparing preview…"),
                        wxColour(170, 170, 180));
            return;
        }
        if (!m_result->ok) {
            draw_status(wxString::FromUTF8(m_result->error.c_str()), wxColour(220, 110, 110));
            return;
        }

        const BoundingBox& bbox = m_result->bbox;
        const double world_w   = std::max<coord_t>(1, bbox.size().x());
        const double world_h   = std::max<coord_t>(1, bbox.size().y());
        // The fit-everything scale, then multiplied by the user's zoom factor.
        // Pan adds an absolute pixel offset on top of the centred position so
        // dragging stays predictable across zoom levels.
        const double fit_scale = std::min((W - 2 * margin) / world_w,
                                          (H - 2 * margin) / world_h);
        const double scale     = fit_scale * m_zoom;
        const double scaled_w  = world_w * scale;
        const double scaled_h  = world_h * scale;
        const int    margin_x  = int((W - scaled_w) / 2) + m_pan_x;
        const int    margin_y  = int((H - scaled_h) / 2) + m_pan_y;

        const unsigned char fill_alpha_base   = m_translucent ? kFillAlphaTranslucent   : kFillAlphaOpaque;
        const unsigned char stroke_alpha_base = m_translucent ? kStrokeAlphaTranslucent : kStrokeAlphaOpaque;

        // Build mode: when active animation is in progress, fade the static
        // structural view to ~25% so it reads as a "ghost / plan" layer; the
        // printed segments will be re-painted on top at full opacity below.
        // The threshold (anim_pos > 0 OR playing) means the user pausing
        // mid-print keeps the ghost+built composition visible for inspection.
        const bool ghost_phase = m_build_mode && m_result->total_chain_scaled > 0.0 &&
                                 (m_playing || m_anim_pos_scaled > 0.0);
        const unsigned char fill_alpha   = ghost_phase
            ? static_cast<unsigned char>(std::max(20, int(fill_alpha_base)   / 4))
            : fill_alpha_base;
        const unsigned char stroke_alpha = ghost_phase
            ? static_cast<unsigned char>(std::max(20, int(stroke_alpha_base) / 4))
            : stroke_alpha_base;

        render_fill_region(dc, m_result->fill_surfaces, scale, bbox, H, margin_x, margin_y, fill_alpha);
        if (m_borders) {
            render_entity(dc, m_result->loops,    scale, bbox, H, margin_x, margin_y, DrawPass::Border, stroke_alpha);
            render_entity(dc, m_result->gap_fill, scale, bbox, H, margin_x, margin_y, DrawPass::Border, stroke_alpha);
        }
        render_entity(dc, m_result->loops,    scale, bbox, H, margin_x, margin_y, DrawPass::Fill, stroke_alpha);
        render_entity(dc, m_result->gap_fill, scale, bbox, H, margin_x, margin_y, DrawPass::Fill, stroke_alpha);

        // ── v3 overlays: travels, seam dots, animated head ──────────────
        // Coordinate transform: scaled-world (Point) → screen (wxPoint).
        auto w2s = [&](const Point& p) -> wxPoint {
            return wxPoint(
                margin_x + int((p.x() - bbox.min.x()) * scale),
                H - margin_y - int((p.y() - bbox.min.y()) * scale));
        };

        // ── Build mode: re-paint printed segments at full opacity ────────
        // Walks ordered_segments and draws each extrusion segment whose end
        // falls at or before the head position. The segment containing the
        // head gets clipped — its tail is rendered from `from` up to the
        // interpolated point. Travels are skipped (they get their own dashed
        // render below). Painted ON TOP of the ghost static, BENEATH the
        // seam/travel/head overlays so those decorations remain readable.
        if (ghost_phase) {
            for (const auto& seg : m_result->ordered_segments) {
                if (seg.is_travel) continue;
                if (seg.cum_start >= m_anim_pos_scaled) break;  // future — chain order is monotonic by cum_start

                Point end_pt;
                const double seg_end = seg.cum_start + seg.length_scaled;
                if (seg_end <= m_anim_pos_scaled) {
                    end_pt = seg.to;  // fully printed
                } else {
                    // Partial — interpolate
                    const double t = seg.length_scaled > 0.0
                        ? (m_anim_pos_scaled - seg.cum_start) / seg.length_scaled
                        : 1.0;
                    end_pt = Point(
                        seg.from.x() + coord_t((seg.to.x() - seg.from.x()) * t),
                        seg.from.y() + coord_t((seg.to.y() - seg.from.y()) * t));
                }

                const wxColour base = role_base_colour_for(seg.role, seg.from_multipath);
                // Full alpha for the printed view — this is the "just laid down"
                // appearance. Width matches the path's real width (mm → pixels
                // through the same scaled<> conversion render_entity uses).
                const int thickness = std::max(1, int(scaled<double>(double(seg.path_width)) * scale));
                wxPen pen(wxColour(base.Red(), base.Green(), base.Blue(), 255),
                          thickness, wxPENSTYLE_SOLID);
                pen.SetCap(wxCAP_ROUND);
                pen.SetJoin(wxJOIN_ROUND);
                dc.SetPen(pen);
                dc.DrawLine(w2s(seg.from), w2s(end_pt));
            }
        }

        // Travels — dashed gray lines in the chain visit order. Drawn under
        // the seam dots so the dots remain visible at travel endpoints.
        if (m_show_travels && !m_result->ordered_segments.empty()) {
            wxPen travel_pen(wxColour(120, 120, 140, 160), 1, wxPENSTYLE_SHORT_DASH);
            dc.SetPen(travel_pen);
            for (const auto& seg : m_result->ordered_segments) {
                if (!seg.is_travel) continue;
                dc.DrawLine(w2s(seg.from), w2s(seg.to));
            }
        }

        // Seam dots — small red circles at the start of each loop in chain
        // order. These mark where each closed perimeter STARTS post-emit
        // (not what SeamPlacer would do, which we don't run in preview).
        if (m_show_seams && !m_result->seam_points.empty()) {
            dc.SetPen(wxPen(wxColour(255, 240, 240), 1));
            dc.SetBrush(wxBrush(wxColour(230, 40, 40)));
            for (const Point& p : m_result->seam_points) {
                const wxPoint sp = w2s(p);
                dc.DrawCircle(sp, 4);
            }
        }

        // Head dot — only meaningful when we have a non-trivial chain. The
        // head is the white dot the user sees travel through the part as the
        // animation plays; we show it whenever playing OR the animation has
        // been advanced (so a paused position stays visible for inspection).
        if (m_result->total_chain_scaled > 0.0 &&
            (m_playing || m_anim_pos_scaled > 0.0))
        {
            // Find the segment containing m_anim_pos_scaled. Linear scan is
            // fine — even multi-island slices stay under a few hundred segments.
            const auto& segs = m_result->ordered_segments;
            wxPoint head_screen(-1, -1);
            for (const auto& seg : segs) {
                const double seg_end = seg.cum_start + seg.length_scaled;
                if (m_anim_pos_scaled >= seg.cum_start && m_anim_pos_scaled <= seg_end) {
                    const double t = seg.length_scaled > 0.0
                        ? (m_anim_pos_scaled - seg.cum_start) / seg.length_scaled
                        : 0.0;
                    const Point interp(
                        seg.from.x() + coord_t((seg.to.x() - seg.from.x()) * t),
                        seg.from.y() + coord_t((seg.to.y() - seg.from.y()) * t));
                    head_screen = w2s(interp);
                    break;
                }
            }
            // Fallback (shouldn't happen, but bound the position to last point)
            if (head_screen == wxPoint(-1, -1) && !segs.empty())
                head_screen = w2s(segs.back().to);

            if (head_screen != wxPoint(-1, -1)) {
                // White-cyan head with a subtle dark halo so it pops on both
                // light and dark extrusion roles underneath.
                dc.SetPen(wxPen(wxColour(0, 0, 0, 200), 1));
                dc.SetBrush(wxBrush(wxColour(40, 40, 50, 200)));
                dc.DrawCircle(head_screen, 8);
                dc.SetPen(wxPen(wxColour(200, 240, 255), 1));
                dc.SetBrush(wxBrush(wxColour(220, 255, 255)));
                dc.DrawCircle(head_screen, 5);
            }
        }

        // Metrics overlay (top-left).
        dc.SetTextForeground(wxColour(230, 230, 235));
        dc.SetFont(GetFont().Smaller());
        const auto& m = m_result->metrics;
        wxString lines[] = {
            wxString::Format("beads:    %.1f",  m.bead_count_avg),
            wxString::Format("closures: %zu",   m.closures_count),
            wxString::Format("walls:    %.2f mm", m.total_wall_mm),
            wxString::Format("fill:     %.2f mm²", m.total_fill_mm2),
        };
        int y = 6;
        for (const wxString& line : lines) {
            dc.DrawText(line, 8, y);
            y += dc.GetCharHeight() + 1;
        }

        if (m_pending) {
            dc.SetTextForeground(wxColour(140, 200, 255));
            dc.DrawText(_("…"), W - 18, 6);
        }
        if (m_zoom != 1.0) {
            dc.SetTextForeground(wxColour(180, 180, 200));
            const wxString z = wxString::Format("%.2fx", m_zoom);
            const wxSize   ts = dc.GetTextExtent(z);
            dc.DrawText(z, W - ts.GetWidth() - 6, H - ts.GetHeight() - 4);
        }
    }

    // ── interaction handlers ────────────────────────────────────────────

    void on_mouse_wheel(wxMouseEvent& e)
    {
        // Plain wheel must propagate to the parent (the Tab is a scroll
        // window). Alt/Option (macOS) / Alt (Win/Linux) hijacks the wheel
        // for zoom — switched from Cmd because Cmd+wheel triggers macOS's
        // system-wide accessibility zoom (System Settings → Accessibility
        // → Zoom → "Use scroll gesture with modifier keys to zoom"), making
        // the canvas zoom unusable on Mac. Alt+wheel mirrors the CAD
        // convention (Blender, FreeCAD, Fusion) and doesn't collide with
        // any common system shortcut.
        if (!e.AltDown()) { e.Skip(); return; }

        const int rotation = e.GetWheelRotation();
        if (rotation == 0) return;
        const double factor   = (rotation > 0) ? kZoomStep : 1.0 / kZoomStep;
        const double new_zoom = std::max(kZoomMin, std::min(kZoomMax, m_zoom * factor));
        if (new_zoom == m_zoom) return;

        // Zoom-toward-cursor: keep the world point under the mouse fixed.
        // We achieve that by adjusting m_pan_{x,y} so the new effective
        // (center + pan) puts the same world point at the same pixel.
        const wxPoint cursor    = e.GetPosition();
        const wxSize  sz        = GetClientSize();
        const int     W         = sz.GetWidth();
        const int     H         = sz.GetHeight();

        const double ratio = new_zoom / m_zoom;
        // Treat (center + m_pan) as the "anchor" offset. After zooming by
        // ratio around the cursor, the new pan that preserves the cursor's
        // world position is:
        //   new_pan = cursor - new_center - (cursor - old_center - old_pan) * ratio
        // Worked out from: pixel = center + pan + (world - bbox.min) * scale.
        if (m_result != nullptr) {
            const BoundingBox& bbox  = m_result->bbox;
            const double world_w     = std::max<coord_t>(1, bbox.size().x());
            const double world_h     = std::max<coord_t>(1, bbox.size().y());
            constexpr int margin     = 12;
            const double fit_scale   = std::min((W - 2 * margin) / world_w,
                                                (H - 2 * margin) / world_h);
            const double old_scale   = fit_scale * m_zoom;
            const double new_scale   = fit_scale * new_zoom;
            const double old_scaled_w = world_w * old_scale;
            const double old_scaled_h = world_h * old_scale;
            const double new_scaled_w = world_w * new_scale;
            const double new_scaled_h = world_h * new_scale;
            const int    old_center_x = int((W - old_scaled_w) / 2);
            const int    old_center_y = int((H - old_scaled_h) / 2);
            const int    new_center_x = int((W - new_scaled_w) / 2);
            const int    new_center_y = int((H - new_scaled_h) / 2);

            m_pan_x = int(cursor.x - new_center_x - (cursor.x - old_center_x - m_pan_x) * ratio);
            m_pan_y = int(cursor.y - new_center_y - (cursor.y - old_center_y - m_pan_y) * ratio);
        }
        m_zoom = new_zoom;
        Refresh(false);
    }

    void on_left_down(wxMouseEvent& e)
    {
        m_panning      = true;
        m_drag_start   = e.GetPosition();
        m_pan_at_start = wxPoint(m_pan_x, m_pan_y);
        if (!HasCapture()) CaptureMouse();
    }

    void on_left_up(wxMouseEvent&)
    {
        if (m_panning) {
            m_panning = false;
            if (HasCapture()) ReleaseMouse();
        }
    }

    void on_motion(wxMouseEvent& e)
    {
        if (!m_panning || !e.LeftIsDown()) return;
        const wxPoint p = e.GetPosition();
        m_pan_x = m_pan_at_start.x + (p.x - m_drag_start.x);
        m_pan_y = m_pan_at_start.y + (p.y - m_drag_start.y);
        Refresh(false);
    }

    // ── state ───────────────────────────────────────────────────────────

    const NPrev::PreviewResult* m_result          = nullptr;  // not owned
    bool                        m_pending         = false;
    wxString                    m_status_override;            // when set, displaces normal paint

    bool                        m_translucent = true;
    bool                        m_borders     = true;

    // v3 — G-code preview overlay state (set by outer panel via setters)
    bool                        m_show_seams      = true;
    bool                        m_show_travels    = true;
    bool                        m_playing         = false;
    double                      m_anim_pos_scaled = 0.0;
    // Build mode: fade the static structural view to ghost alpha and re-paint
    // each printed segment at full opacity as the head passes. Off by default
    // so existing UX (head-on-static-drawing) is preserved.
    bool                        m_build_mode      = false;

    double                      m_zoom        = 1.0;
    int                         m_pan_x       = 0;
    int                         m_pan_y       = 0;
    bool                        m_panning     = false;
    wxPoint                     m_drag_start;
    wxPoint                     m_pan_at_start;
};

// ────────────────────────────────────────────────────────────────────────────
// Outer panel.
// ────────────────────────────────────────────────────────────────────────────

NeoArachnePreviewPanel::NeoArachnePreviewPanel(wxWindow* parent, Tab* tab)
    : wxPanel(parent, wxID_ANY)
    , m_tab(tab)
    , m_alive(std::make_shared<AliveFlag>())
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_canvas = new NeoArachnePreviewCanvas(this);
    sizer->Add(m_canvas, 1, wxEXPAND | wxALL, 2);

    // ─ layer slider row (label + slider) ────────────────────────────────
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_layer_label  = new wxStaticText(this, wxID_ANY, _("Layer Z: —"));
        m_layer_slider = new wxSlider(this, wxID_ANY, 0, 0, 1,
                                      wxDefaultPosition, wxDefaultSize,
                                      wxSL_HORIZONTAL);
        m_layer_slider->Enable(false);  // disabled until a FromMesh snapshot exists
        row->Add(m_layer_label,  0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        row->Add(m_layer_slider, 1, wxEXPAND | wxLEFT | wxRIGHT, 6);
        sizer->Add(row, 0, wxEXPAND | wxTOP, 4);
    }

    // ─ geometry radio + use-selected button ─────────────────────────────
    {
        const wxString choices[] = { _("W"), _("Wedge"), _("Selected") };
        m_geom_radio = new wxRadioBox(this, wxID_ANY, _("Geometry"),
                                      wxDefaultPosition, wxDefaultSize,
                                      WXSIZEOF(choices), choices,
                                      3, wxRA_SPECIFY_COLS);
        m_geom_radio->SetSelection(0);  // W default
        m_geom_radio->Enable(2, /*enable=*/false);  // "Selected" until snapshot exists
        sizer->Add(m_geom_radio, 0, wxEXPAND | wxALL, 4);

        m_use_btn = new wxButton(this, wxID_ANY, _("Use current selection"));
        sizer->Add(m_use_btn, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 4);
    }

    // ─ render options row (translucent + borders + fit) ────────────────
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_translucent_cb = new wxCheckBox(this, wxID_ANY, _("Translucent"));
        m_translucent_cb->SetValue(true);
        m_translucent_cb->SetToolTip(_("Show extrusions at 30% opacity so overlapping beads blend visibly"));
        m_borders_cb     = new wxCheckBox(this, wxID_ANY, _("Borders"));
        m_borders_cb->SetValue(true);
        m_borders_cb->SetToolTip(_("Outline each extrusion to mimic the shadow effect of the main viewport"));
        m_fit_btn        = new wxButton(this, wxID_ANY, _("Fit"), wxDefaultPosition, wxSize(48, -1));
        m_fit_btn->SetToolTip(_("Reset zoom and pan (Alt/Option+wheel to zoom, drag to pan)"));
        row->Add(m_translucent_cb, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        row->Add(m_borders_cb,     0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        row->AddStretchSpacer();
        row->Add(m_fit_btn,        0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        sizer->Add(row, 0, wxEXPAND | wxBOTTOM, 4);
    }

    // ─ v3 G-code preview controls ───────────────────────────────────────
    // Two rows: row1 = play/pause + speed slider; row2 = show seams / show travels.
    // Kept compact because the canvas above is the visual centerpiece.
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_play_btn = new wxButton(this, wxID_ANY, _("▶ Play"), wxDefaultPosition, wxSize(70, -1));
        m_play_btn->SetToolTip(_("Animate the print head along the real chain visit order"));
        m_speed_label  = new wxStaticText(this, wxID_ANY, _("0.1x"));
        // Speed maps to log scale: tick 0..40 → 0.02x..0.5x via 0.02*pow(25,t/40).
        // Slow end (0.02x ≈ 1 mm/s) lets the user inspect every move of a fine
        // bead; fast end (0.5x ≈ 25 mm/s) skims through the chain for a quick
        // overview. Default tick 20 → ~0.1x (≈5 mm/s) — calibrated for "review
        // build" use cases. Old range 0.5–10x was overshooting the high end on
        // every interaction.
        m_speed_slider = new wxSlider(this, wxID_ANY, 20, 0, 40,
                                      wxDefaultPosition, wxDefaultSize, wxSL_HORIZONTAL);
        m_speed_slider->SetToolTip(_("Animation speed (0.02×..0.5×). Doesn't affect slicing — only the head playback. Default 0.1× is comfortable for reviewing build order."));
        m_dump_btn = new wxButton(this, wxID_ANY, _("Dump"), wxDefaultPosition, wxSize(56, -1));
        m_dump_btn->SetToolTip(_("Save the preview's internal data + pseudo-G-code to a text file for diffing against the real slicer output."));
        row->Add(m_play_btn,     0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        row->Add(m_speed_label,  0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        row->Add(m_speed_slider, 1, wxEXPAND | wxLEFT | wxRIGHT, 6);
        row->Add(m_dump_btn,     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        sizer->Add(row, 0, wxEXPAND | wxBOTTOM, 2);
    }
    {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        m_show_seams_cb   = new wxCheckBox(this, wxID_ANY, _("Show seams"));
        m_show_seams_cb->SetValue(true);
        m_show_seams_cb->SetToolTip(_("Red dots at the start of each closed perimeter (approximation — real seam_placer not invoked)"));
        m_show_travels_cb = new wxCheckBox(this, wxID_ANY, _("Show travels"));
        m_show_travels_cb->SetValue(true);
        m_show_travels_cb->SetToolTip(_("Dashed lines for non-extrusion moves between paths, in the real chain visit order"));
        m_build_mode_cb   = new wxCheckBox(this, wxID_ANY, _("Build mode"));
        m_build_mode_cb->SetValue(false);
        m_build_mode_cb->SetToolTip(_("During play, fade the structural view to ~25% and re-paint each printed segment at full opacity as the head passes — gives a sense of physical construction instead of a dot moving on a static drawing."));
        row->Add(m_show_seams_cb,   0, wxALIGN_CENTER_VERTICAL | wxLEFT, 4);
        row->Add(m_show_travels_cb, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        row->Add(m_build_mode_cb,   0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        sizer->Add(row, 0, wxEXPAND | wxBOTTOM, 4);
    }

    SetSizer(sizer);

    // ─ bindings ─────────────────────────────────────────────────────────
    m_layer_slider  ->Bind(wxEVT_SLIDER,    &NeoArachnePreviewPanel::on_layer_slider,       this);
    m_geom_radio    ->Bind(wxEVT_RADIOBOX,  &NeoArachnePreviewPanel::on_geom_radio,         this);
    m_use_btn       ->Bind(wxEVT_BUTTON,    &NeoArachnePreviewPanel::on_use_selected,       this);
    m_translucent_cb->Bind(wxEVT_CHECKBOX,  &NeoArachnePreviewPanel::on_translucent_toggle, this);
    m_borders_cb    ->Bind(wxEVT_CHECKBOX,  &NeoArachnePreviewPanel::on_borders_toggle,     this);
    m_fit_btn       ->Bind(wxEVT_BUTTON,    &NeoArachnePreviewPanel::on_fit_clicked,        this);
    // v3 bindings
    m_play_btn      ->Bind(wxEVT_BUTTON,    &NeoArachnePreviewPanel::on_play_clicked,       this);
    m_speed_slider  ->Bind(wxEVT_SLIDER,    &NeoArachnePreviewPanel::on_speed_slider,       this);
    m_show_seams_cb ->Bind(wxEVT_CHECKBOX,  &NeoArachnePreviewPanel::on_show_seams_toggle,  this);
    m_show_travels_cb->Bind(wxEVT_CHECKBOX, &NeoArachnePreviewPanel::on_show_travels_toggle,this);
    m_build_mode_cb ->Bind(wxEVT_CHECKBOX,  &NeoArachnePreviewPanel::on_build_mode_toggle,  this);
    m_dump_btn      ->Bind(wxEVT_BUTTON,    &NeoArachnePreviewPanel::on_dump_clicked,       this);

    m_poll_timer.SetOwner(this);
    m_debounce_timer.SetOwner(this);
    m_anim_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent& e) {
        if (e.GetTimer().GetId() == m_poll_timer.GetId())
            on_poll_timer(e);
        else if (e.GetTimer().GetId() == m_debounce_timer.GetId())
            on_debounce_timer(e);
        else if (e.GetTimer().GetId() == m_anim_timer.GetId())
            on_anim_timer(e);
    });

    m_last_config_hash = hash_current_relevant_config();
    schedule_refresh();
    m_poll_timer.Start(250);
}

NeoArachnePreviewPanel::~NeoArachnePreviewPanel()
{
    m_poll_timer.Stop();
    m_debounce_timer.Stop();
    m_anim_timer.Stop();
    // Detached workers may still be running. Two guards: bump m_request_id so
    // any pending CallAfter is dropped at the id check, and clear m_alive so
    // even the CallAfter is skipped (avoids touching `this` after dtor).
    ++m_request_id;
    m_alive->alive.store(false);
}

void NeoArachnePreviewPanel::schedule_refresh()
{
    m_debounce_timer.StartOnce(300);
}

size_t NeoArachnePreviewPanel::hash_current_relevant_config() const
{
    DynamicPrintConfig dyn = live_merged_config(m_tab);
    size_t seed = 0;
    for (const std::string& key : relevant_config_keys()) {
        const ConfigOption* opt = dyn.option(key);
        if (opt == nullptr) continue;
        const size_t h = opt->hash();
        seed ^= h + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    }
    return seed;
}

void NeoArachnePreviewPanel::on_poll_timer(wxTimerEvent&)
{
    const size_t h = hash_current_relevant_config();
    if (h != m_last_config_hash) {
        m_last_config_hash = h;
        schedule_refresh();
    }
}

void NeoArachnePreviewPanel::on_debounce_timer(wxTimerEvent&)
{
    launch_async_slice();
}

void NeoArachnePreviewPanel::on_layer_slider(wxCommandEvent&)
{
    if (m_layer_slider != nullptr) {
        const double z_mm = double(m_layer_slider->GetValue()) * kSliderStepMm;
        m_layer_label->SetLabel(wxString::Format(_("Layer Z: %.2f mm"), z_mm));
    }
    schedule_refresh();
}

void NeoArachnePreviewPanel::on_geom_radio(wxCommandEvent&)
{
    // Layer slider is meaningful only for FromMesh sources — W and Wedge are
    // 2D contours so Z is irrelevant. Reflect that in the widget state so
    // the user doesn't wonder why the slider does nothing.
    const bool from_mesh = (m_geom_radio->GetSelection() == 2);
    m_layer_slider->Enable(from_mesh && m_mesh_snapshot != nullptr);
    schedule_refresh();
}

void NeoArachnePreviewPanel::on_translucent_toggle(wxCommandEvent&)
{
    if (m_canvas != nullptr && m_translucent_cb != nullptr)
        m_canvas->set_translucent(m_translucent_cb->GetValue());
}

void NeoArachnePreviewPanel::on_borders_toggle(wxCommandEvent&)
{
    if (m_canvas != nullptr && m_borders_cb != nullptr)
        m_canvas->set_borders(m_borders_cb->GetValue());
}

void NeoArachnePreviewPanel::on_fit_clicked(wxCommandEvent&)
{
    if (m_canvas != nullptr) m_canvas->reset_view();
}

// ── v3 G-code preview controls ─────────────────────────────────────────

void NeoArachnePreviewPanel::on_play_clicked(wxCommandEvent&)
{
    m_playing = !m_playing;
    if (m_play_btn != nullptr)
        m_play_btn->SetLabel(m_playing ? _("⏸ Pause") : _("▶ Play"));
    if (m_playing) {
        // Wrap to 0 if we're already at the end so a click resumes from start.
        if (m_result && m_anim_pos_scaled >= m_result->total_chain_scaled)
            m_anim_pos_scaled = 0.0;
        m_anim_timer.Start(33);  // ~30 FPS
    } else {
        m_anim_timer.Stop();
        // Push the paused state so the head stays visible.
        if (m_canvas != nullptr) m_canvas->set_anim_pos(m_anim_pos_scaled, false);
    }
}

void NeoArachnePreviewPanel::on_speed_slider(wxCommandEvent&)
{
    if (m_speed_slider == nullptr) return;
    // Tick 0..40 → 0.02x..0.5x via 0.02 * pow(25, tick/40).
    // Old range was 0.5x..10x — user feedback: 0.5x was already too fast for
    // build-mode review use cases. New range bottoms out at 0.02x (~1 mm/s,
    // segment-by-segment inspection) and tops out at 0.5x (~25 mm/s, fast
    // skim). Default tick 20 → 0.1x (~5 mm/s) sits in a comfortable review
    // sweet spot.
    const int    tick = m_speed_slider->GetValue();
    const double mult = 0.02 * std::pow(25.0, double(tick) / 40.0);
    m_anim_speed_mult = mult;
    if (m_speed_label != nullptr) {
        // Use %.2fx for the slow end (precision matters more there),
        // %.1fx for the rest.
        m_speed_label->SetLabel(mult < 0.1
            ? wxString::Format("%.2fx", mult)
            : wxString::Format("%.1fx", mult));
    }
}

void NeoArachnePreviewPanel::on_build_mode_toggle(wxCommandEvent&)
{
    if (m_canvas != nullptr && m_build_mode_cb != nullptr)
        m_canvas->set_build_mode(m_build_mode_cb->GetValue());
}

void NeoArachnePreviewPanel::on_show_seams_toggle(wxCommandEvent&)
{
    if (m_canvas != nullptr && m_show_seams_cb != nullptr)
        m_canvas->set_show_seams(m_show_seams_cb->GetValue());
}

void NeoArachnePreviewPanel::on_show_travels_toggle(wxCommandEvent&)
{
    if (m_canvas != nullptr && m_show_travels_cb != nullptr)
        m_canvas->set_show_travels(m_show_travels_cb->GetValue());
}

namespace {
// Local-only utilities for the dump handler. Anonymous-namespace placement
// keeps them out of the public surface.

const char* role_label(ExtrusionRole r)
{
    switch (r) {
        case erNone:                  return "erNone";
        case erPerimeter:             return "erPerimeter";
        case erExternalPerimeter:     return "erExternalPerimeter";
        case erOverhangPerimeter:     return "erOverhangPerimeter";
        case erInternalInfill:        return "erInternalInfill";
        case erSolidInfill:           return "erSolidInfill";
        case erTopSolidInfill:        return "erTopSolidInfill";
        case erBottomSurface:         return "erBottomSurface";
        case erIroning:               return "erIroning";
        case erBridgeInfill:          return "erBridgeInfill";
        case erInternalBridgeInfill:  return "erInternalBridgeInfill";
        case erGapFill:               return "erGapFill";
        case erSkirt:                 return "erSkirt";
        case erBrim:                  return "erBrim";
        case erSupportMaterial:       return "erSupportMaterial";
        case erSupportMaterialInterface: return "erSupportMaterialInterface";
        case erWipeTower:             return "erWipeTower";
        case erMixed:                 return "erMixed";
        default:                      return "erUnknown";
    }
}

void dump_entity_tree(std::ostream& os, const ExtrusionEntity& e, const std::string& prefix)
{
    if (const auto* coll = dynamic_cast<const ExtrusionEntityCollection*>(&e)) {
        os << prefix << "ExtrusionEntityCollection no_sort=" << (coll->no_sort ? "true" : "false")
           << " role=" << role_label(coll->role())
           << " inset_idx=" << e.inset_idx
           << " children=" << coll->entities.size() << "\n";
        for (size_t i = 0; i < coll->entities.size(); ++i) {
            const ExtrusionEntity* c = coll->entities[i];
            if (!c) continue;
            dump_entity_tree(os, *c, prefix + "  [" + std::to_string(i) + "] ");
        }
        return;
    }
    if (const auto* loop = dynamic_cast<const ExtrusionLoop*>(&e)) {
        os << prefix << "ExtrusionLoop role=" << role_label(loop->role())
           << " inset_idx=" << e.inset_idx
           << " paths=" << loop->paths.size()
           << " length_mm=" << std::fixed << std::setprecision(3)
           << unscale<double>(loop->length()) << "\n";
        for (size_t i = 0; i < loop->paths.size(); ++i) {
            const ExtrusionPath& p = loop->paths[i];
            os << prefix << "  [" << i << "] ExtrusionPath role=" << role_label(p.role())
               << " width=" << std::fixed << std::setprecision(3) << p.width
               << " pts=" << p.polyline.points.size()
               << " length_mm=" << unscale<double>(p.length()) << "\n";
        }
        return;
    }
    if (const auto* mp = dynamic_cast<const ExtrusionMultiPath*>(&e)) {
        os << prefix << "ExtrusionMultiPath role=" << role_label(mp->role())
           << " inset_idx=" << e.inset_idx
           << " paths=" << mp->paths.size()
           << " length_mm=" << std::fixed << std::setprecision(3)
           << unscale<double>(mp->length()) << "\n";
        for (size_t i = 0; i < mp->paths.size(); ++i) {
            const ExtrusionPath& p = mp->paths[i];
            os << prefix << "  [" << i << "] ExtrusionPath role=" << role_label(p.role())
               << " width=" << std::fixed << std::setprecision(3) << p.width
               << " pts=" << p.polyline.points.size()
               << " length_mm=" << unscale<double>(p.length()) << "\n";
        }
        return;
    }
    if (const auto* path = dynamic_cast<const ExtrusionPath*>(&e)) {
        os << prefix << "ExtrusionPath role=" << role_label(path->role())
           << " inset_idx=" << e.inset_idx
           << " width=" << std::fixed << std::setprecision(3) << path->width
           << " pts=" << path->polyline.points.size()
           << " length_mm=" << unscale<double>(path->length()) << "\n";
        return;
    }
    os << prefix << "(unknown ExtrusionEntity subclass)\n";
}
} // namespace

void NeoArachnePreviewPanel::on_dump_clicked(wxCommandEvent&)
{
    if (!m_result || !m_result->ok) {
        m_canvas->set_status(_("No preview to dump yet"));
        return;
    }

    // Default filename includes timestamp so successive dumps don't overwrite.
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream stamp;
    stamp << std::put_time(&tm, "%Y%m%d_%H%M%S");
    const wxString default_name = wxString::Format("neoarachne_preview_%s.txt", stamp.str());
    const wxString default_dir  = wxStandardPaths::Get().GetDocumentsDir();

    wxFileDialog dlg(this, _("Save NeoArachne preview dump"),
                     default_dir, default_name,
                     "Text dump (*.txt)|*.txt|All files (*.*)|*.*",
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;

    const std::string path = dlg.GetPath().ToStdString();
    std::ofstream f(path);
    if (!f.is_open()) {
        m_canvas->set_status(wxString::Format(_("Dump failed: cannot open %s"), dlg.GetPath()));
        return;
    }

    // ── Header ──────────────────────────────────────────────────────────
    f << "; ════════════════════════════════════════════════════════════\n"
      << "; NeoArachne Preview Lab — debug dump\n"
      << "; Generated: " << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "\n"
      << "; ════════════════════════════════════════════════════════════\n";

    // ── Geometry source ─────────────────────────────────────────────────
    const int sel = m_geom_radio != nullptr ? m_geom_radio->GetSelection() : 0;
    const char* src_name = (sel == 0) ? "W (built-in)" :
                           (sel == 1) ? "Wedge (built-in)" :
                                        "Selected (mesh snapshot)";
    f << "; Geometry: " << src_name << "\n";
    if (sel == 2) {
        f << "; Mesh snapshot Z range: " << std::fixed << std::setprecision(3)
          << m_snapshot_z_min << " .. " << m_snapshot_z_max << " mm\n";
        if (m_layer_slider != nullptr) {
            const double z_mm = m_snapshot_z_min + double(m_layer_slider->GetValue()) * kSliderStepMm;
            f << "; Slice Z: " << std::fixed << std::setprecision(3) << z_mm << " mm\n";
        }
    }
    const BoundingBox& bb = m_result->bbox;
    f << "; Slice bbox: ("
      << std::fixed << std::setprecision(3)
      << unscale<double>(bb.min.x()) << ", " << unscale<double>(bb.min.y()) << ") .. ("
      << unscale<double>(bb.max.x()) << ", " << unscale<double>(bb.max.y()) << ") mm\n";
    // fix #2 + #4A (s96): show the layer mapping AND the Z snap so the user
    // can confirm the preview is simulating the same plane the real slicer
    // does. Trace recorded at the last launch_async_slice; -1 means N/A
    // (built-in W/Wedge sources, where there's no real layer concept).
    if (m_last_slice_layer_n > 0) {
        const bool top = m_last_slice_layer_n >= m_last_slice_total_n;
        const bool bot = m_last_slice_layer_n == 1;
        f << "; Layer snap: layer " << m_last_slice_layer_n
          << "/" << m_last_slice_total_n
          << (top ? " [TOP layer]" : "")
          << (bot ? " [BOTTOM layer]" : "")
          << "\n";
        f << "; Slice Z requested (slider): " << std::fixed << std::setprecision(3)
          << m_last_slice_z_raw << " mm\n";
        f << "; Slice Z used (snapped to layer mid-Z): " << std::fixed << std::setprecision(3)
          << m_last_slice_z_used << " mm\n";
    } else {
        f << "; Layer snap: layer_id=5 (built-in W/Wedge default — no real layer concept)\n";
    }

    // ── Relevant config keys ────────────────────────────────────────────
    f << ";\n; ──── Config (live merged) ────\n";
    DynamicPrintConfig dyn = live_merged_config(m_tab);
    for (const std::string& key : relevant_config_keys()) {
        const ConfigOption* opt = dyn.option(key);
        if (opt == nullptr) {
            f << "; " << key << " = <missing>\n";
            continue;
        }
        f << "; " << key << " = " << opt->serialize() << "\n";
    }

    // ── Metrics ─────────────────────────────────────────────────────────
    const auto& m = m_result->metrics;
    f << ";\n; ──── Metrics ────\n"
      << "; bead_count_avg = " << std::fixed << std::setprecision(2) << m.bead_count_avg << "\n"
      << "; closures_count = " << m.closures_count << "\n"
      << "; total_wall_mm  = " << m.total_wall_mm << "\n"
      << "; total_fill_mm² = " << m.total_fill_mm2 << "\n";

    // ── Input geometry (post-XY-compensation) ───────────────────────────
    // fix #3 (s96). These are the ExPolygons handed to NeoArachne::Plan::run.
    // Diff against the real slicer's layer.lslices (export 3MF + inspect) to
    // isolate upstream (slice prep) vs downstream (wall emit) divergence.
    f << ";\n; ════ Input geometry (post-XY compensation) ════\n";
    f << "; input_slices count: " << m_result->input_slices.size() << "\n";
    for (size_t i = 0; i < m_result->input_slices.size(); ++i) {
        const ExPolygon& ex = m_result->input_slices[i];
        f << "; [slice " << i << "] contour pts=" << ex.contour.points.size()
          << " area_mm² = " << std::fixed << std::setprecision(4)
          << unscale<double>(unscale<double>(std::abs(ex.contour.area())))
          << " holes=" << ex.holes.size() << "\n";
        // Contour points (full dump — never more than a few hundred for typical
        // single-layer slices; gives exact diffability against real slicer).
        f << "; [slice " << i << "] contour:\n";
        for (size_t j = 0; j < ex.contour.points.size(); ++j) {
            const Point& p = ex.contour.points[j];
            f << ";   c" << j << " "
              << std::fixed << std::setprecision(4)
              << unscale<double>(p.x()) << " " << unscale<double>(p.y()) << "\n";
        }
        for (size_t h = 0; h < ex.holes.size(); ++h) {
            const Polygon& hp = ex.holes[h];
            f << "; [slice " << i << "] hole " << h << " pts=" << hp.points.size()
              << " area_mm² = " << std::fixed << std::setprecision(4)
              << unscale<double>(unscale<double>(std::abs(hp.area()))) << "\n";
            for (size_t j = 0; j < hp.points.size(); ++j) {
                const Point& p = hp.points[j];
                f << ";   h" << h << "_" << j << " "
                  << std::fixed << std::setprecision(4)
                  << unscale<double>(p.x()) << " " << unscale<double>(p.y()) << "\n";
            }
        }
    }

    // ── Structural tree ─────────────────────────────────────────────────
    f << ";\n; ════ Structural tree (loops) ════\n";
    f << "; top-level entities: " << m_result->loops.entities.size() << "\n";
    for (size_t i = 0; i < m_result->loops.entities.size(); ++i) {
        const ExtrusionEntity* e = m_result->loops.entities[i];
        if (!e) continue;
        f << "; [loops/" << i << "]\n";
        dump_entity_tree(f, *e, "; ");
    }
    f << ";\n; ════ Structural tree (gap_fill) ════\n";
    f << "; top-level entities: " << m_result->gap_fill.entities.size() << "\n";
    for (size_t i = 0; i < m_result->gap_fill.entities.size(); ++i) {
        const ExtrusionEntity* e = m_result->gap_fill.entities[i];
        if (!e) continue;
        f << "; [gap_fill/" << i << "]\n";
        dump_entity_tree(f, *e, "; ");
    }

    // ── Chain order — pseudo-G-code ─────────────────────────────────────
    // G0 = travel, G1 = extrusion. Coordinates in mm with 3 decimals.
    // `; SEAM` markers tag each loop start. `; segment N/M role` per segment.
    // No E/F values — this is a diff target, not a printable file.
    f << ";\n; ════ Pseudo-G-code (chain order) ════\n";
    f << "; segments total: " << m_result->ordered_segments.size() << "\n";
    f << "; extrusion total: "
      << std::fixed << std::setprecision(3) << unscale<double>(m_result->total_length_scaled) << " mm\n";
    f << "; chain total (extrusion + travel): "
      << unscale<double>(m_result->total_chain_scaled) << " mm\n";
    f << "; seam dots count: " << m_result->seam_points.size() << "\n;\n";

    // Track which seam points have already been reported as we walk segments
    // — emit `; SEAM` whenever the next extrusion begins on a seam point.
    std::set<std::pair<coord_t, coord_t>> seam_lookup;
    for (const Point& p : m_result->seam_points)
        seam_lookup.emplace(p.x(), p.y());

    auto write_mm = [&](const Point& p) {
        f << std::fixed << std::setprecision(3)
          << unscale<double>(p.x()) << " Y" << unscale<double>(p.y());
    };

    Point cursor(0, 0);
    f << "G0 X" ; write_mm(cursor); f << "  ; origin\n";
    for (size_t i = 0; i < m_result->ordered_segments.size(); ++i) {
        const auto& seg = m_result->ordered_segments[i];
        const bool starts_loop = seam_lookup.count({seg.from.x(), seg.from.y()}) > 0;

        if (seg.is_travel) {
            f << "; segment " << (i+1) << "/" << m_result->ordered_segments.size()
              << " [travel]\n";
            f << "G0 X"; write_mm(seg.to); f << "  ; len " << std::fixed << std::setprecision(3)
              << unscale<double>(seg.length_scaled) << "\n";
        } else {
            if (starts_loop)
                f << "; SEAM at X" << std::fixed << std::setprecision(3)
                  << unscale<double>(seg.from.x()) << " Y" << unscale<double>(seg.from.y()) << "\n";
            f << "; segment " << (i+1) << "/" << m_result->ordered_segments.size()
              << " [extrusion " << role_label(seg.role)
              << " w=" << std::fixed << std::setprecision(3) << seg.path_width << "mm"
              << (seg.from_multipath ? " from_multipath" : "") << "]\n";
            f << "G1 X"; write_mm(seg.to); f << "  ; len " << std::fixed << std::setprecision(3)
              << unscale<double>(seg.length_scaled) << "\n";
        }
        cursor = seg.to;
    }

    f << ";\n; ════ End of dump ════\n";
    f.close();

    m_canvas->set_status(wxString::Format(_("Dump saved → %s"), dlg.GetPath()));
}

void NeoArachnePreviewPanel::on_anim_timer(wxTimerEvent&)
{
    if (!m_playing || !m_result || m_result->total_chain_scaled <= 0.0) return;
    // Base head speed = 50 mm/s of "real print" time at 1x. Tick = 33ms = 0.033s.
    // Advance in scaled coord units: scaled<double>(mm) converts mm → coord_t units.
    constexpr double base_mm_per_s = 50.0;
    constexpr double dt_s          = 0.033;
    const double advance_mm     = base_mm_per_s * m_anim_speed_mult * dt_s;
    const double advance_scaled = scaled<double>(advance_mm);
    m_anim_pos_scaled += advance_scaled;
    // Loop at end so the user can watch the same chain repeatedly without
    // having to click Play again. Pause auto-fires nothing — the toggle is
    // the only way to stop the loop.
    if (m_anim_pos_scaled >= m_result->total_chain_scaled)
        m_anim_pos_scaled = 0.0;
    if (m_canvas != nullptr)
        m_canvas->set_anim_pos(m_anim_pos_scaled, /*playing=*/true);
}

void NeoArachnePreviewPanel::on_use_selected(wxCommandEvent&)
{
    std::string err = snapshot_selected_model_volume();
    if (!err.empty()) {
        // Surface the error directly on the canvas so it's visible without
        // a modal. Cleared on next successful slice.
        m_canvas->set_status(wxString::FromUTF8(err.c_str()));
        return;
    }
    m_canvas->clear_status();
    m_geom_radio->Enable(2, true);
    m_geom_radio->SetSelection(2);
    refresh_slider_range_from_snapshot();
    m_layer_slider->Enable(true);
    schedule_refresh();
}

std::string NeoArachnePreviewPanel::snapshot_selected_model_volume()
{
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr) return "no plater (preview unavailable)";
    GLCanvas3D* c3d = plater->canvas3D();
    if (c3d == nullptr) return "no 3D canvas";

    Selection& sel = c3d->get_selection();
    int obj_idx = -1, vol_idx = -1;
    ModelVolume* mv = sel.get_selected_single_volume(obj_idx, vol_idx);

    // Fallback: if the user has selected a full instance (the common case
    // when clicking once on an object), pick its first model part volume.
    if (mv == nullptr) {
        ModelObject* mo = sel.get_selected_single_object(obj_idx);
        if (mo != nullptr) {
            for (ModelVolume* v : mo->volumes) {
                if (v != nullptr && v->is_model_part()) { mv = v; break; }
            }
        }
    }
    if (mv == nullptr || mv->get_object() == nullptr)
        return "select a single object on the bed first";

    const ModelObject* mo = mv->get_object();
    const ModelInstance* inst = mo->instances.empty() ? nullptr : mo->instances.front();
    // Bake instance × volume transform into a copy so subsequent
    // moves/rotations of the source object in the bed do NOT invalidate the
    // snapshot (snapshot freezing per the s93 plan). The mesh is copied
    // first, then transformed in place; the original ModelVolume's shared
    // mesh_ptr() is untouched.
    const Transform3d trafo = (inst != nullptr ? inst->get_matrix() : Transform3d::Identity())
                            * mv->get_matrix();

    TriangleMesh m = mv->mesh();
    if (m.empty()) return "selected volume has no geometry";
    m.transform(trafo);

    const BoundingBoxf3 bb = m.bounding_box();
    m_snapshot_z_min = bb.min.z();
    m_snapshot_z_max = bb.max.z();
    m_mesh_snapshot  = std::make_shared<const TriangleMesh>(std::move(m));
    return std::string{};
}

void NeoArachnePreviewPanel::refresh_slider_range_from_snapshot()
{
    if (!m_mesh_snapshot || m_layer_slider == nullptr) return;
    const double range_mm = std::max(0.0, m_snapshot_z_max - m_snapshot_z_min);
    const int    ticks    = std::max(1, int(range_mm * kSliderTicksPerMm));
    m_layer_slider->SetRange(0, ticks);
    // Default to mid-Z, which matches build_from_mesh's mid-Z fallback when
    // the requested Z is out of range.
    const int mid = ticks / 2;
    m_layer_slider->SetValue(mid);
    const double mid_z = m_snapshot_z_min + double(mid) * kSliderStepMm;
    m_layer_label->SetLabel(wxString::Format(_("Layer Z: %.2f mm"), mid_z));
}

NPrev::PreviewGeometrySource NeoArachnePreviewPanel::current_source() const
{
    const int sel = m_geom_radio != nullptr ? m_geom_radio->GetSelection() : 0;
    switch (sel) {
        case 1: return NPrev::PreviewGeometrySource::wedge();
        case 2:
            if (m_mesh_snapshot) {
                // Slider value 0..ticks → mesh-frame Z. The mesh was baked
                // with the instance transform so its bbox is already in
                // world coords; the slider range matches that bbox.
                const double slider_z_offset = double(m_layer_slider->GetValue()) * kSliderStepMm;
                const double slice_z_world   = m_snapshot_z_min + slider_z_offset;
                return NPrev::PreviewGeometrySource::from_mesh(m_mesh_snapshot, slice_z_world);
            }
            // Fallthrough — no snapshot yet, fall back to W so the canvas
            // still shows something useful.
            return NPrev::PreviewGeometrySource::w();
        default: return NPrev::PreviewGeometrySource::w();
    }
}

void NeoArachnePreviewPanel::launch_async_slice()
{
    // If the wall generator isn't NeoArachne, short-circuit with a status
    // message instead of dispatching a useless slice. Avoids the panel
    // showing "Slicing preview…" forever when the user is editing Classic.
    const std::string gen_state = probe_wall_generator_state(m_tab);
    if (!gen_state.empty()) {
        m_canvas->set_status(wxString::Format(_("Preview idle (%s) — pick NeoArachne"),
                                              wxString::FromUTF8(gen_state.c_str())));
        return;
    }
    m_canvas->clear_status();

    const std::uint64_t my_id = ++m_request_id;
    m_pending = true;
    m_canvas->set_pending(true);

    NPrev::ConfigSnapshot         snap = capture_snapshot_from_tab(m_tab);
    NPrev::PreviewGeometrySource  src  = current_source();

    // fix #2 (s96): derive layer_id from the slice_z + mesh bbox so the
    // preview reproduces top/bottom-specific slicer behaviour. Only meaningful
    // for FromMesh; W/Wedge keep the snapshot's default "safe middle" value.
    //
    // fix #4A (s96): SNAP slice_z to the mid-Z of the real layer plane. The
    // slicer uses initial_layer_print_height for layer 1 (e.g. 0.28mm for
    // adhesion) and layer_height for layers 2+, then slices at the MIDDLE of
    // each layer. Without snapping, the user's raw slider Z falls between
    // real layer planes — the preview sees a slightly different cross-section
    // than what the real slicer's nearest layer sees, which is enough to
    // change Arachne's SkeletalTrapezoidation graph for thin features.
    m_last_slice_z_raw   = -1.0;
    m_last_slice_z_used  = -1.0;
    m_last_slice_layer_n = -1;
    m_last_slice_total_n = -1;
    if (src.kind == NeoArachne::Preview::GeometryKind::FromMesh && m_mesh_snapshot) {
        const double lh    = snap.layer_height > 0.0 ? snap.layer_height : 0.2;
        const double flh   = snap.print.initial_layer_print_height.value > 0.0
                                ? snap.print.initial_layer_print_height.value
                                : lh;
        const double raw_z = (src.slice_z_mm > 0.0)
                                ? src.slice_z_mm
                                : 0.5 * (m_snapshot_z_min + m_snapshot_z_max);

        // Identify the layer N that contains raw_z, then compute its mid-Z.
        // Layer 1 covers [0, flh]; layer N>=2 covers [flh+(N-2)*lh, flh+(N-1)*lh].
        int layer_n;
        double mid_z;
        if (raw_z <= flh) {
            layer_n = 1;
            mid_z   = flh * 0.5;
        } else {
            layer_n = 2 + int(std::floor((raw_z - flh) / lh));
            mid_z   = flh + (double(layer_n) - 1.5) * lh;
        }
        // Total layers across the mesh height (for label display).
        const int total_n = (m_snapshot_z_max <= flh)
            ? 1
            : 1 + int(std::ceil((m_snapshot_z_max - flh) / lh));

        // Clamp layer_n to total so a slider past the mesh top still maps to
        // the actual last printable layer.
        layer_n = std::min(layer_n, std::max(1, total_n));

        // Recompute mid_z after clamp.
        mid_z = (layer_n == 1) ? flh * 0.5 : flh + (double(layer_n) - 1.5) * lh;

        // Push snapped value back into the source so the worker slices there.
        src.slice_z_mm = mid_z;

        // Trace for the dump.
        m_last_slice_z_raw   = raw_z;
        m_last_slice_z_used  = mid_z;
        m_last_slice_layer_n = layer_n;
        m_last_slice_total_n = total_n;

        snap.effective_layer_id = std::max(0, layer_n - 1);  // 0-indexed in the rest of the slicer
        snap.is_top_layer       = (layer_n >= total_n);
        snap.is_bottom_layer    = (layer_n == 1);
    }

    // Detach so the previous worker's future destructor doesn't synchronize
    // on this thread. Worker captures the alive flag — if the panel is
    // destroyed before the worker finishes, the CallAfter is skipped and
    // `this` is never touched.
    std::shared_ptr<AliveFlag> alive = m_alive;
    std::thread([snap = std::move(snap), src = std::move(src), my_id, this, alive]() mutable {
        NPrev::PreviewResult r = NPrev::preview_slice(snap, src);
        if (!alive->alive.load()) return;
        CallAfter([this, my_id, r = std::move(r), alive]() mutable {
            if (!alive->alive.load())            return;
            if (my_id != m_request_id.load())    return;
            m_pending = false;
            m_result  = std::make_unique<NPrev::PreviewResult>(std::move(r));
            m_canvas->set_pending(false);
            m_canvas->set_result(m_result.get());
            // v3 — reset animation cursor whenever fresh chain arrives so the
            // head doesn't point at a stale offset (the chain length may have
            // shrunk). Keep playing state if the user was already playing.
            m_anim_pos_scaled = 0.0;
            m_canvas->set_anim_pos(0.0, m_playing);
        });
    }).detach();
}

}} // namespace Slic3r::GUI
