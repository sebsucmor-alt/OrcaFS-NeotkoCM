#include <algorithm>
#include <cmath>
#include <sstream>

#include "libslic3r/Arachne/utils/ExtrusionJunction.hpp"
#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/Feature/FuzzySkin/FuzzySkin.hpp"
#include "libslic3r/Feature/TextureBump/TextureBump.hpp"
#include "libslic3r/NeoDebug.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "NeoStitch.hpp"

// NEOTKO_NEOSTITCH_TAG — Z-Stitch Interlock, F1+F2+F4 (notch + fill + real config/UI). See NeoStitch.hpp and
// docs/FUTURE/NEOSTITCH_PLAN.md for the model this implements. Original mechanism/design of this
// fork — see plan §0 for the explicit distinction from "brick layers" (which shift Z; this never
// does).

using namespace Slic3r;

// Local mirror of TextureBump.cpp's TEXTUREBUMP_LOG macro (same pattern, own channel) so this file
// doesn't need to pull in an unrelated translation unit just for logging.
#define NEOSTITCH_LOG(body) do { if (Slic3r::NeoDebug::enabled(Slic3r::NeoDebug::NEOSTITCH)) { \
    std::ostringstream _nsdbg_; _nsdbg_ << body;                                               \
    Slic3r::NeoDebug::write(Slic3r::NeoDebug::NEOSTITCH, _nsdbg_.str()); } } while (0)

namespace Slic3r::Feature::NeoStitch {

NeoStitchConfig config_from_region(const PrintRegionConfig& region_config)
{
    NeoStitchConfig c;
    c.target         = region_config.neostitch.value;
    c.depth_mm       = region_config.neostitch_depth.value;
    c.flat_length_mm = region_config.neostitch_flat_length.value;
    c.ramp_length_mm = region_config.neostitch_ramp_length.value;
    c.period_mm      = region_config.neostitch_period.value;
    c.flow_pct       = region_config.neostitch_flow.value;
    c.skip_layers    = region_config.neostitch_skip_layers.value;
    c.fill_margin_mm = region_config.neostitch_fill_margin.value;
    return c;
}

namespace {

// Plan §3.2 — which inset_idx this target resolves to for a given wall stack. Returns false (no
// target this stack) rather than forcing a wall count the user didn't ask for (plan §3.2/§7 point
// 2: NEVER copy TextureBump's `loop_number = max(loop_number, 1)` trick here).
bool resolve_target_inset_idx(NeoStitchTarget target, int total_loops, size_t& out_inset_idx)
{
    switch (target) {
        case NeoStitchTarget::Outermost:
            out_inset_idx = 0; // always exists -- any wall stack has at least inset_idx 0
            return true;
        case NeoStitchTarget::SecondWall:
            if (total_loops < 1) return false;
            out_inset_idx = 1;
            return true;
        case NeoStitchTarget::ThirdWall:
            if (total_loops < 2) return false;
            out_inset_idx = 2;
            return true;
        case NeoStitchTarget::Innermost:
            if (total_loops < 1) return false;
            out_inset_idx = size_t(total_loops);
            return true;
        case NeoStitchTarget::Disabled:
        default:
            return false;
    }
}

// Plan §4 guard #9 -- mutual exclusion with fuzzy skin / texture bump on the SAME loop (not the
// same region: a region can have fuzzy on the skin (inset_idx 0) and NeoStitch on the 2nd wall at
// the same time -- that's the intended "estrella" use case, not a conflict). Reuses the sibling
// modules' own per-loop predicates instead of re-deriving anything -- both are already exactly
// "does this config apply to this layer/loop_idx/contour-or-hole" tests.
bool neostitch_conflicts(const PerimeterGenerator& pg, size_t inset_idx, bool is_contour)
{
    for (const auto& region : pg.regions_by_fuzzify)
        if (Slic3r::Feature::FuzzySkin::should_fuzzify(region.first, pg.layer_id, inset_idx, is_contour))
            return true;
    if (pg.texture_bump_tables)
        for (const auto& kv : *pg.texture_bump_tables)
            if (Slic3r::Feature::TextureBump::should_apply_texture_bump(kv.first, pg.layer_id, inset_idx, is_contour))
                return true;
    return false;
}

// Plan §2.1 — N (periods) is FIXED per object from an approximate silhouette perimeter (bbox
// rectangle perimeter, a cheap stand-in — see plan §2.4 for the known Cylindrical-u distortion on
// non-circular shapes and the Cubic/arc-length escalation path if it matters in practice).
// Rounded to an integer so the wave closes with no seam discontinuity (same reasoning TextureBump
// applies to its own num_periods, TextureBump.cpp:374-376) -- NEVER derive this per-loop/per-layer
// (plan §7 point 1: the #1 predictable way to break vertical registration).
int compute_num_periods(const BoundingBoxf3& bounds, double period_mm)
{
    const double approx_perimeter_mm = 2.0 * (bounds.size().x() + bounds.size().y());
    return std::max(4, int(std::lround(approx_perimeter_mm / std::max(period_mm, 1e-3))));
}

// Plan §2.1/§2.2 — the notch/fill envelope. `u_canonical` in [0,1) (world-stable angular
// coordinate around the object), `num_periods` fixed per object (see above), `layer_parity` =
// layer_id % 2. Returns s in [-1, 1]: negative = notch depth fraction, positive = fill fraction,
// 0 = neutral (between events, line left at nominal position/width).
//
// One period unit tau = fract(u_canonical * num_periods + layer_parity * 0.5) is split into two
// halves: [0, 0.5) = NOTCH region, [0.5, 1.0) = FILL region. The half-period phase shift on odd
// layers swaps which physical world position falls in which half between consecutive layers --
// that swap IS the registration (plan §2.1): whatever was NOTCH on layer N is FILL on layer N+1.
double neostitch_signal(double u_canonical, int num_periods, int layer_parity, const NeoStitchConfig& cfg, double period_mm_eff)
{
    const double u    = u_canonical * double(num_periods) + double(layer_parity) * 0.5;
    const double tau  = u - std::floor(u); // fract, in [0,1)
    const bool   is_notch = tau < 0.5;
    const double r    = is_notch ? (tau / 0.5) : ((tau - 0.5) / 0.5); // position within half-period, in [0,1)

    const double half_region_mm = period_mm_eff / 2.0;
    // Plan §5.3 (F2b, "real drop" recipe): the fill event is shrunk vs. the notch it plugs, by
    // fill_margin_mm per side, so the deposited bead is CONTAINED inside the notch void instead of
    // bridging across its shoulders (PathBlend Full precedent: contained extrusion drops into an
    // exact gap; a same-length fill would instead rest on the notch's own rim). ramp_length_mm
    // itself is untouched -- only the plateau shrinks, so the fill keeps a real flat top until
    // fill_margin_mm exceeds flat_length_mm/2, at which point peak amplitude starts tapering below
    // 1.0 (an F6 tuning question, not something to guard against here).
    const double notch_event_mm = cfg.flat_length_mm + 2.0 * cfg.ramp_length_mm;
    const double fill_event_mm  = std::max(notch_event_mm - 2.0 * cfg.fill_margin_mm, 0.0);
    const double event_mm       = is_notch ? notch_event_mm : fill_event_mm;
    const double half_event_mm  = event_mm / 2.0;

    const double pos_mm             = r * half_region_mm;
    const double dist_from_center   = std::abs(pos_mm - half_region_mm / 2.0);

    double envelope = 0.0; // neutral (between events) by default
    if (dist_from_center <= half_event_mm) {
        const double d = half_event_mm - dist_from_center; // distance inward from the event's own edge
        envelope = (d < cfg.ramp_length_mm) ? std::clamp(d / std::max(cfg.ramp_length_mm, 1e-6), 0.0, 1.0) : 1.0;
    }

    // Consumed by neostitch_walk_extrusion_line() below: negative -> position displacement
    // (notch), positive -> width modulation (fill), plan §2.2/§2.3.
    return is_notch ? -envelope : envelope;
}

// Fill-branch width/anchor constants -- deliberately the SAME values TextureBump's own
// Combined-style width blend uses (TextureBump.cpp:869-870, print-validated s178), reused here for
// consistency rather than re-derived: 0.05mm is a physical floor no nozzle can usefully go below,
// 2.0x is a sane ceiling before a "wide line" stops behaving like a wall bead.
constexpr double kMinExtrusionWidthMm = 0.05;
constexpr double kMaxWidthMultiplier  = 2.0;

// Same interpolated-point walk as TextureBump::texture_bump_extrusion_line() (TextureBump.cpp:894-932)
// -- points are inserted along each segment at a fixed spacing so the notch/fill ramps actually
// have vertices to bend, then displaced/widened along the segment's own perpendicular. Plan §7
// point 3: without this subdivision, long straight walls have too few Arachne junctions for the
// wave to exist at all.
void neostitch_walk_extrusion_line(Arachne::ExtrusionJunctions& ext_lines, const PerimeterGenerator& pg,
                                    const NeoStitchConfig& cfg, int num_periods, double period_mm_eff,
                                    const Polygons& lower_polys)
{
    // Sample spacing: dense enough to approximate the ramp, floor to avoid pathological vertex
    // counts on a very short ramp_length_mm. NEOTKO_NEOSTITCH_TAG -- real bug found on first print
    // test (s206): p0p1_size below is a norm of scaled coord_t deltas (SCALING_FACTOR ~= 1e-6mm,
    // so ~1e6 units/mm), matching TextureBump's own min_dist_between_points (coord_t-typed
    // cfg.point_distance, TextureBump.cpp:891). This must be scale_()'d into the same coord_t-scale
    // units -- left as raw mm it made the inner walk loop need ~4e6 iterations per mm of wall
    // (hundreds of millions per loop), which read as a hang/freeze rather than a crash.
    const double sample_distance_mm      = std::max(cfg.ramp_length_mm / 4.0, 0.2);
    const double min_dist_between_points = scale_(sample_distance_mm);
    double       dist_left_over          = min_dist_between_points / 2.0;

    const int layer_parity = pg.layer_id % 2;

    auto* p0 = &ext_lines.front();
    Arachne::ExtrusionJunctions out;
    out.reserve(ext_lines.size());

    double max_notch_mm = 0.0;
    double max_fill_mm  = 0.0;
    // Plan §4 guard #8 (per-tramo lower-slices support test): tested once per notch/fill EVENT (on
    // the neutral->active transition of `s`), not once per sample -- Slic3r::contains() is
    // O(vertices in lower_polys), and there can be dozens of samples per event at default spacing.
    // Stays correct automatically under a shrunk fill_event_mm (§5.3): it doesn't assume anything
    // about event length, it just watches s's own transitions live, whatever shape they have.
    bool prev_sample_active = false;
    bool event_supported    = true;
    for (auto& p1 : ext_lines) {
        if (p0->p == p1.p) { // Connect endpoints. p0 intentionally NOT advanced here -- matches
            // TextureBump::texture_bump_extrusion_line() exactly (TextureBump.cpp:904-907): p0 only
            // advances in the non-degenerate branch below, so a duplicate-point run collapses onto
            // whichever real point last anchored p0 instead of chaining onto itself.
            out.emplace_back(p1.p, p1.w, p1.perimeter_index);
            continue;
        }

        Vec2d  p0p1      = (p1.p - p0->p).cast<double>();
        double p0p1_size = p0p1.norm();
        double p0pa_dist = dist_left_over;
        for (; p0pa_dist < p0p1_size; p0pa_dist += min_dist_between_points) {
            Point       pa       = p0->p + (p0p1 * (p0pa_dist / p0p1_size)).cast<coord_t>();
            const Vec2d perp_dir = perp(p0p1).cast<double>().normalized();
            const Vec3d point_mm(unscale_(pa.x()), unscale_(pa.y()), pg.slice_z);

            // Cylindrical/Z projection reused verbatim from TextureBump (compute_u() is a public,
            // stateless function -- no TextureBump behavior is touched by calling it). Identity
            // plane_transform: NeoStitch has no orientable-plane concept in F1.
            const double u = Slic3r::Feature::TextureBump::compute_u(
                point_mm, perp_dir, Slic3r::TextureProjectionMode::Cylindrical, Slic3r::TextureProjectionAxis::Z,
                pg.neostitch_bounds, Transform3d::Identity());

            double s = neostitch_signal(u, num_periods, layer_parity, cfg, period_mm_eff);

            // Guard #8: on the neutral->active transition, test the NOMINAL point (before any
            // shift is applied) against the lower layer's real slices -- once per event, cached
            // until the next transition. Unsupported -> force this whole event neutral (nominal
            // position/width): resolves bottom surfaces, overhangs and bridges in one guard.
            const bool sample_active = (s != 0.0);
            if (sample_active && !prev_sample_active)
                event_supported = Slic3r::contains(lower_polys, pa);
            prev_sample_active = sample_active;
            if (sample_active && !event_supported)
                s = 0.0;

            double center_shift_mm = 0.0;
            coord_t new_w = p1.w;
            if (s < 0.0) {
                // Notch: inward displacement, width untouched. TextureBump's convention
                // (TextureBump.cpp:386-391, print-validated) is that +perp_dir is OUTWARD for this
                // ExtrusionLine winding, so inward is -perp_dir -- `s` is already negative here,
                // giving that sign directly.
                center_shift_mm = s * cfg.depth_mm;
                max_notch_mm    = std::max(max_notch_mm, -center_shift_mm);
            } else if (s > 0.0) {
                // Fill: width grows to plug the notch left by the SAME physical position one layer
                // below (registration is the phase math in neostitch_signal(), not searched for
                // here). Plan §2.3: at full flow the added cross-section per unit length must equal
                // the notch's removed cross-section (depth*h == width_delta*h), so width_delta_mm
                // at s=1/flow_pct=100% is exactly cfg.depth_mm -- independent of the line's own
                // nominal width. Anchor: the OUTER edge (the visible/previous-wall-facing side,
                // +perp_dir) must NOT move -- all the extra width goes inward, toward infill, which
                // is deliberately the OPPOSITE anchor from TextureBump's own Combined blend (that
                // one anchors the INFILL-facing edge instead, TextureBump.cpp:876) because here the
                // notch that needs plugging is on the infill side, not the skin side.
                const double w_mm         = unscale_(p1.w);
                const double width_delta_mm_raw = cfg.depth_mm * (cfg.flow_pct / 100.0) * s;
                const double max_rad_mm   = w_mm * kMaxWidthMultiplier;
                const double rad_mm       = std::clamp(w_mm + width_delta_mm_raw, kMinExtrusionWidthMm, std::max(max_rad_mm, kMinExtrusionWidthMm));
                center_shift_mm = -(rad_mm - w_mm) / 2.0; // inward (-perp_dir), post-clamp delta so the anchor stays exact even when clamped
                new_w = coord_t(scale_(rad_mm));
                max_fill_mm = std::max(max_fill_mm, rad_mm - w_mm);
            }

            out.emplace_back(pa + (perp_dir * scale_(center_shift_mm)).cast<coord_t>(), new_w, p1.perimeter_index);
        }
        dist_left_over = p0pa_dist - p0p1_size;
        p0 = &p1;
    }

    NEOSTITCH_LOG("walk_extrusion_line layer=" << pg.layer_id << " layer_parity=" << layer_parity
        << " num_periods=" << num_periods << " period_mm_eff=" << period_mm_eff
        << " max_notch_mm=" << max_notch_mm << " max_fill_mm=" << max_fill_mm << " points=" << out.size());

    // Same fallback/endpoint-reconnect pattern as TextureBump (TextureBump.cpp:939-950).
    while (out.size() < 3) {
        size_t point_idx = ext_lines.size() - 2;
        out.emplace_back(ext_lines[point_idx].p, ext_lines[point_idx].w, ext_lines[point_idx].perimeter_index);
        if (point_idx == 0)
            break;
        --point_idx;
    }

    if (ext_lines.back().p == ext_lines.front().p) {
        out.front().p = out.back().p;
        out.front().w = out.back().w;
    }

    if (out.size() >= 3)
        ext_lines = std::move(out);
}

} // anonymous namespace

bool apply_neostitch(Arachne::ExtrusionLine* extrusion, const PerimeterGenerator& perimeter_generator, bool is_contour, int total_loops)
{
    if (perimeter_generator.config->neostitch.value == NeoStitchTarget::Disabled)
        return false; // fast-path skip before even building the config struct (mirrors TextureBump/FuzzySkin's own None fast-path)
    NeoStitchConfig cfg = config_from_region(*perimeter_generator.config);
    if (cfg.depth_mm <= 0.0)
        cfg.depth_mm = double(perimeter_generator.perimeter_flow.width()); // "0 = auto" -> this object's own Inner wall line width, already fully resolved (mm/%% -> mm)

    if (!extrusion->is_closed || extrusion->is_odd)
        return false; // gap-fill/odd centre beads never touched (plan §4 point 3)

    size_t target_inset_idx = 0;
    if (!resolve_target_inset_idx(cfg.target, total_loops, target_inset_idx))
        return false; // target wall doesn't exist in this stack -- skip silently, never force wall_loops
    if (extrusion->inset_idx != target_inset_idx)
        return false;

    // Plan §4 guard #9: yield to fuzzy skin / texture bump on this SAME loop rather than stacking
    // width/position modulators -- evaluated right after the cheap inset_idx match (fail-fast),
    // ahead of the pricier lower-slices/loop-length checks below.
    if (neostitch_conflicts(perimeter_generator, extrusion->inset_idx, is_contour)) {
        NEOSTITCH_LOG("apply_neostitch skip_conflict layer=" << perimeter_generator.layer_id
            << " inset_idx=" << extrusion->inset_idx << " (fuzzy_skin/texture_bump already active on this loop)");
        return false;
    }

    if (perimeter_generator.lower_slices == nullptr)
        return false; // first layer -- never notch straight into the bed (plan §4 point 5)
    if (perimeter_generator.layer_id < perimeter_generator.object_config->raft_layers.value + 1 + cfg.skip_layers)
        return false; // plan §3.1 neostitch_skip_layers + always-skip-first-layer
    if (perimeter_generator.upper_slices == nullptr)
        return false; // topmost layer -- a notch with nothing printed above it never gets filled (plan §4 point 6)

    const int    num_periods    = compute_num_periods(perimeter_generator.neostitch_bounds, cfg.period_mm);
    const double approx_perimeter_mm = 2.0 * (perimeter_generator.neostitch_bounds.size().x() + perimeter_generator.neostitch_bounds.size().y());
    const double period_mm_eff  = approx_perimeter_mm / double(num_periods);

    // Loop too short for even one full period -- skip (plan §4 point 7). Compares against the
    // ACTUAL loop length (getLength() is scaled units), not the approximate silhouette above.
    const double loop_length_mm = unscale_(double(extrusion->getLength()));
    if (loop_length_mm < 2.0 * cfg.period_mm) {
        NEOSTITCH_LOG("apply_neostitch skip_short_loop layer=" << perimeter_generator.layer_id
            << " inset_idx=" << extrusion->inset_idx << " loop_length_mm=" << loop_length_mm);
        return false;
    }

    // Plan §5.3 caveat: warn (once per call, not per sample -- see neostitch_signal()'s own
    // comment on why logging there would be spam) if fill_margin_mm has eaten the whole plateau --
    // peak fill amplitude starts tapering below 1.0 past this point, an F6 tuning question.
    if (cfg.flat_length_mm - 2.0 * cfg.fill_margin_mm < 0.0)
        NEOSTITCH_LOG("apply_neostitch fill_margin_attenuated layer=" << perimeter_generator.layer_id
            << " flat_length_mm=" << cfg.flat_length_mm << " fill_margin_mm=" << cfg.fill_margin_mm);

    NEOSTITCH_LOG("apply_neostitch layer=" << perimeter_generator.layer_id
        << " inset_idx=" << extrusion->inset_idx << " total_loops=" << total_loops
        << " target=" << int(cfg.target) << " num_periods=" << num_periods
        << " period_mm_eff=" << period_mm_eff << " loop_length_mm=" << loop_length_mm);

    // Guard #8's per-event support test (inside the walk) needs this once, not per-sample --
    // lower_slices_polygons() returns Polygons BY VALUE (a full copy each call).
    const Polygons lower_polys = perimeter_generator.lower_slices_polygons();
    neostitch_walk_extrusion_line(extrusion->junctions, perimeter_generator, cfg, num_periods, period_mm_eff, lower_polys);
    return true;
}

// Plan §5.3/§7: tags the fill sub-paths for GCode.cpp's speed selection by WIDTH COMPARISON, not by
// re-running neostitch_signal() at emission time. Reasoning (see also NeoStitch.hpp's own comment
// on this function): in v1 only the fill branch of neostitch_walk_extrusion_line() ever widens a
// junction (the notch branch only shifts position, leaving width untouched) -- and Arachne's
// generic ExtrusionLine->ExtrusionPath conversion (thick_polyline_to_multi_path(), VariableWidth.cpp)
// already splits into a new ExtrusionPath at essentially every width change, so every fill
// ramp/plateau is already isolated into its own small path(s) for free by the time this runs. A
// width comparison against this loop's own nominal wall width also automatically reflects guard #8
// neutralizing a period back to nominal width -- a parallel re-evaluation of the pure signal would
// NOT know that guard fired and would mislabel a neutralized-but-signal-positive stretch as fill.
void apply_neostitch_fill_speed(ExtrusionPaths& paths, const PerimeterGenerator& perimeter_generator, const Arachne::ExtrusionLine& extrusion)
{
    const bool   is_external      = extrusion.inset_idx == 0;
    const double nominal_width_mm = double((is_external ? perimeter_generator.ext_perimeter_flow : perimeter_generator.perimeter_flow).width());

    size_t tagged = 0;
    for (ExtrusionPath& path : paths) {
        const double width_delta_mm = double(path.width) - nominal_width_mm;
        if (width_delta_mm > 0.001) {
            path.neostitch_fill_event = true;
            // Visualization (s207, GCode.cpp's ;HEIGHT: re-declaration, plan §5.3/§6): width_delta_mm
            // here is ALREADY, by the same m=1+d/w conservation this fill width came from (§2.3),
            // numerically equal to the notch depth being plugged at this exact point -- reused
            // directly as the displayed height bump, no extra scaling or re-derivation needed. Never
            // touches path.height itself (the real per-mm flow is already fixed by this point) or
            // the real toolpath Z -- see GCode.cpp's own comment at the ;HEIGHT: tag site.
            path.neostitch_visual_height_mm = float(width_delta_mm);
            ++tagged;
        }
    }
    NEOSTITCH_LOG("apply_neostitch_fill_speed layer=" << perimeter_generator.layer_id
        << " inset_idx=" << extrusion.inset_idx << " nominal_width_mm=" << nominal_width_mm
        << " paths=" << paths.size() << " tagged=" << tagged);
}

} // namespace Slic3r::Feature::NeoStitch
