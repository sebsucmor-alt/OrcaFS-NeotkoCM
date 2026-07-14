///|/ Wave-Huygens support-roof fill (FillWaveRoof) — WAVESUPPORT_PLAN.md Fase 3.
///|/
///|/ Core algorithm ported/adapted from:
///|/   Wave overhangs algorithm: Janis A. Andersons (andersonsjanis).
///|/   Builds on arc-overhang algorithm by Steven McCulloch (stmcculloch).
///|/   PrusaSlicer integration: Steven McCulloch.
///|/   OrcaSlicer port: Dennis Klappe (dennisklappe) — OrcaSlicer-WaveOverhangs (AGPL-3.0).
///|/   Roof adaptation: Neotko (OrcaFS NeotkoCM fork).
///|/
///|/ Released under the terms of the AGPLv3 or higher.
///|/
#include "FillWaveRoof.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "../ClipperUtils.hpp"
#include "../ExPolygon.hpp"
#include "../ExtrusionEntity.hpp"
#include "../NeoDebug.hpp"
#include "../Polygon.hpp"
#include "../Polyline.hpp"
#include "../libslic3r.h"

// NEOTKO_WAVEROOF_TAG — local log macro (same shape as WaveSupport.cpp's WAVESUPPORT_LOG),
// gated on ORCA_DEBUG_WAVEROOF via the NeoDebug WAVEROOF channel (allocated in Fase 0).
#define WAVEROOF_LOG(body)                                                    \
    do {                                                                      \
        if (Slic3r::NeoDebug::enabled(Slic3r::NeoDebug::WAVEROOF)) {          \
            std::ostringstream _ndbg_;                                        \
            _ndbg_ << body;                                                   \
            Slic3r::NeoDebug::write(Slic3r::NeoDebug::WAVEROOF, _ndbg_.str());\
        }                                                                     \
    } while (0)

namespace Slic3r {
namespace {

// ---------------------------------------------------------------------------
// Helpers ported verbatim from OrcaSlicer-WaveOverhangs (Klappe), trimmed to what
// the roof case needs. Corner-taper, ZigZag/Monotonic patterns, shell perimeters,
// bridge detection and anchor/overhang seeding are intentionally NOT ported — the
// roof sits fully on the pillar below, so there is no overhang/anchor duality.
// ---------------------------------------------------------------------------

// Opportunistically chain polylines whose endpoints are within `limit_distance`,
// so consecutive fronts print as fewer, longer paths instead of many short loops.
Polylines reconnect_polylines(const Polylines &polylines, double limit_distance)
{
    if (polylines.empty())
        return polylines;

    std::unordered_map<size_t, Polyline> connected;
    connected.reserve(polylines.size());
    for (size_t i = 0; i < polylines.size(); ++i) {
        if (! polylines[i].empty())
            connected.emplace(i, polylines[i]);
    }

    for (size_t a = 0; a < polylines.size(); ++a) {
        auto base_it = connected.find(a);
        if (base_it == connected.end())
            continue;

        Polyline &base = base_it->second;
        for (size_t b = a + 1; b < polylines.size(); ++b) {
            auto next_it = connected.find(b);
            if (next_it == connected.end())
                continue;

            Polyline &next = next_it->second;
            if ((base.last_point() - next.first_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.append(std::move(next));
                connected.erase(next_it);
            } else if ((base.last_point() - next.last_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.points.insert(base.points.end(), next.points.rbegin(), next.points.rend());
                connected.erase(next_it);
            } else if ((base.first_point() - next.last_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                next.append(std::move(base));
                base = std::move(next);
                base.reverse();
                connected.erase(next_it);
            } else if ((base.first_point() - next.first_point()).cast<double>().squaredNorm() < limit_distance * limit_distance) {
                base.reverse();
                base.append(std::move(next));
                base.reverse();
                connected.erase(next_it);
            }
        }
    }

    Polylines result;
    result.reserve(connected.size());
    for (auto &entry : connected)
        result.push_back(std::move(entry.second));
    return result;
}

template <class Fn>
void for_each_boundary_point(const ExPolygon &expoly, Fn &&fn)
{
    for (const Point &pt : expoly.contour.points)
        fn(pt);
    for (const Polygon &hole : expoly.holes)
        for (const Point &pt : hole.points)
            fn(pt);
}

struct ClosestBoundaryPair {
    Point  a;
    Point  b;
    double distance_sq{ std::numeric_limits<double>::infinity() };
    bool   valid{ false };
};

struct NarrowSplitCandidate {
    Point   a;
    Point   b;
    Point   midpoint;
    double  distance_sq{ std::numeric_limits<double>::infinity() };
    Polygon slit;
};

ClosestBoundaryPair find_closest_boundary_pair(const ExPolygon &a, const ExPolygon &b, const ExPolygon &container)
{
    ClosestBoundaryPair best;

    auto try_pair = [&](const Point &src, const ExPolygon &other, bool src_is_a) {
        Point projected = other.point_projection(src);
        const double distance_sq = (projected - src).cast<double>().squaredNorm();
        if (distance_sq >= best.distance_sq)
            return;

        const Point midpoint = (0.5 * (src.cast<double>() + projected.cast<double>())).cast<coord_t>();
        if (! container.contains(midpoint))
            return;

        best.distance_sq = distance_sq;
        best.valid = true;
        if (src_is_a) {
            best.a = src;
            best.b = projected;
        } else {
            best.a = projected;
            best.b = src;
        }
    };

    for_each_boundary_point(a, [&](const Point &pt) { try_pair(pt, b, true); });
    for_each_boundary_point(b, [&](const Point &pt) { try_pair(pt, a, false); });

    return best;
}

Polygon make_split_slit(const Point &a, const Point &b, coord_t extension, coord_t half_width)
{
    const Vec2d start = a.cast<double>();
    const Vec2d end   = b.cast<double>();
    const Vec2d delta = end - start;
    const double length = delta.norm();
    if (length <= 0.)
        return {};

    const Vec2d dir = delta / length;
    const Vec2d normal(-dir.y(), dir.x());
    const Vec2d extended_start = start - dir * double(extension);
    const Vec2d extended_end   = end + dir * double(extension);
    const Vec2d offset         = normal * double(std::max<coord_t>(1, half_width));

    Polygon slit;
    slit.points = {
        Point((extended_start + offset).cast<coord_t>()),
        Point((extended_end   + offset).cast<coord_t>()),
        Point((extended_end   - offset).cast<coord_t>()),
        Point((extended_start - offset).cast<coord_t>())
    };
    return slit;
}

size_t total_hole_count(const ExPolygons &expolygons)
{
    size_t count = 0;
    for (const ExPolygon &expolygon : expolygons)
        count += expolygon.holes.size();
    return count;
}

bool slit_changes_topology(const ExPolygon &wave_cover, const Polygon &slit)
{
    if (! slit.is_valid())
        return false;

    const ExPolygons split_result = union_ex(diff_ex(ExPolygons{ wave_cover }, Polygons{ slit }));
    return split_result.size() != 1 || total_hole_count(split_result) != wave_cover.holes.size();
}

Polygon make_effective_split_slit(const ExPolygon &wave_cover, const Point &a, const Point &b, coord_t extension, coord_t initial_half_width, coord_t wave_spacing)
{
    coord_t half_width = std::max<coord_t>(std::max<coord_t>(1, initial_half_width), wave_spacing / 2 + 1);
    for (int attempt = 0; attempt < 6; ++attempt) {
        Polygon slit = make_split_slit(a, b, extension, half_width);
        if (slit_changes_topology(wave_cover, slit))
            return slit;

        half_width = std::max<coord_t>(half_width + 1, half_width * 2);
    }

    return {};
}

// Cut the wave cover across necks narrower than `minimum_wave_width` so the
// wavefront collapses into clean separate regions instead of pinching.
Polygons generate_narrow_split_slits(const ExPolygon &wave_cover, coord_t wave_spacing, coord_t minimum_wave_width)
{
    const coord_t effective_minimum_width = std::max<coord_t>(0, minimum_wave_width);
    if (effective_minimum_width <= 0)
        return {};

    const double max_gap_sq = std::pow(double(effective_minimum_width), 2);
    const coord_t slit_half_width = std::max<coord_t>(1, wave_spacing / 20);
    const coord_t slit_extension  = std::max<coord_t>(slit_half_width, effective_minimum_width);
    const std::array<double, 4> inset_fractions{{ 0.25, 0.5, 0.75, 1.0 }};
    const double duplicate_radius_sq = std::pow(0.5 * double(wave_spacing), 2);
    const size_t original_hole_count = wave_cover.holes.size();

    std::vector<NarrowSplitCandidate> candidates;
    auto append_candidate = [&](const ClosestBoundaryPair &pair) {
        if (! pair.valid || pair.distance_sq > max_gap_sq)
            return;

        NarrowSplitCandidate candidate;
        candidate.a = pair.a;
        candidate.b = pair.b;
        candidate.distance_sq = pair.distance_sq;
        candidate.midpoint = (0.5 * (pair.a.cast<double>() + pair.b.cast<double>())).cast<coord_t>();
        candidate.slit = make_effective_split_slit(wave_cover, pair.a, pair.b, wave_spacing + slit_extension, slit_half_width, wave_spacing);
        if (! candidate.slit.is_valid())
            return;

        candidates.push_back(std::move(candidate));
    };

    for (double inset_fraction : inset_fractions) {
        const coord_t inset_depth = std::max<coord_t>(1, coord_t(std::round(inset_fraction * double(wave_spacing))));
        const ExPolygons inset_components = offset_ex(wave_cover, -float(inset_depth), jtRound, 0.);
        const bool component_count_changed = inset_components.size() > 1;
        const bool hole_count_changed = total_hole_count(inset_components) != original_hole_count;
        if (! component_count_changed && ! hole_count_changed)
            continue;

        if (component_count_changed) {
            for (size_t i = 0; i < inset_components.size(); ++i) {
                for (size_t j = i + 1; j < inset_components.size(); ++j) {
                    ClosestBoundaryPair pair = find_closest_boundary_pair(inset_components[i], inset_components[j], wave_cover);
                    append_candidate(pair);
                }
            }
        }

        if (hole_count_changed && ! wave_cover.holes.empty()) {
            ExPolygon outer_boundary;
            outer_boundary.contour = wave_cover.contour;

            for (size_t hole_idx = 0; hole_idx < wave_cover.holes.size(); ++hole_idx) {
                ExPolygon hole_boundary;
                hole_boundary.contour = wave_cover.holes[hole_idx];
                append_candidate(find_closest_boundary_pair(outer_boundary, hole_boundary, wave_cover));

                for (size_t other_hole_idx = hole_idx + 1; other_hole_idx < wave_cover.holes.size(); ++other_hole_idx) {
                    ExPolygon other_hole_boundary;
                    other_hole_boundary.contour = wave_cover.holes[other_hole_idx];
                    append_candidate(find_closest_boundary_pair(hole_boundary, other_hole_boundary, wave_cover));
                }
            }
        }
    }

    if (candidates.empty())
        return {};

    std::sort(candidates.begin(), candidates.end(), [](const NarrowSplitCandidate &lhs, const NarrowSplitCandidate &rhs) {
        return lhs.distance_sq < rhs.distance_sq;
    });

    Polygons slits;
    std::vector<Point> kept_midpoints;
    for (NarrowSplitCandidate &candidate : candidates) {
        bool duplicate = false;
        for (const Point &kept_midpoint : kept_midpoints) {
            if ((candidate.midpoint - kept_midpoint).cast<double>().squaredNorm() <= duplicate_radius_sq) {
                duplicate = true;
                break;
            }
        }

        if (duplicate)
            continue;

        kept_midpoints.push_back(candidate.midpoint);
        slits.push_back(std::move(candidate.slit));
    }

    return slits.empty() ? Polygons{} : union_(slits);
}

ExtrusionPath make_wave_path(const Polyline &polyline, const Flow &flow, ExtrusionRole role)
{
    ExtrusionPath path(role, flow.mm3_per_mm(), flow.width(), flow.height());
    path.polyline = polyline;
    return path;
}

ExtrusionPath make_wave_path(Polyline &&polyline, const Flow &flow, ExtrusionRole role)
{
    ExtrusionPath path(role, flow.mm3_per_mm(), flow.width(), flow.height());
    path.polyline = std::move(polyline);
    return path;
}

// Emit fronts in an order/orientation that keeps the print head continuous:
// for each front, pick the endpoint that lands nearest to already-placed paths
// (support_score), so travels between fronts stay short. Ported from Klappe's
// default (non-ZigZag/Monotonic) branch of append_wave_fronts.
void append_wave_fronts(ExtrusionPaths &roof_region,
                        const Polylines &fronts,
                        const Flow      &wave_flow,
                        coord_t          connector_limit,
                        ExtrusionRole    role)
{
    if (fronts.empty())
        return;

    auto point_at_distance = [](const Polyline &line, double distance) {
        if (line.points.empty())
            return Point{};
        if (distance <= 0. || line.points.size() == 1)
            return line.first_point();

        double walked = 0.;
        for (size_t i = 1; i < line.points.size(); ++i) {
            const Vec2d a = line.points[i - 1].cast<double>();
            const Vec2d b = line.points[i].cast<double>();
            const Vec2d segment = b - a;
            const double segment_length = segment.norm();
            if (segment_length <= 0.)
                continue;
            if (walked + segment_length >= distance) {
                const double t = (distance - walked) / segment_length;
                return Point((a + t * segment).cast<coord_t>());
            }
            walked += segment_length;
        }
        return line.last_point();
    };

    auto support_score = [&point_at_distance](const Polyline &candidate, const ExtrusionPaths &support_paths, coord_t support_reach, coord_t prefix_length) {
        if (support_paths.empty() || candidate.points.size() < 2)
            return -1.;

        const double candidate_length = candidate.length();
        if (candidate_length <= 0.)
            return -1.;

        const double sample_length = std::min(candidate_length, double(std::max<coord_t>(1, prefix_length)));
        const std::array<std::pair<double, double>, 3> samples = {{
            { 0.0,                 3.0 },
            { 0.5 * sample_length, 2.0 },
            { sample_length,       1.0 }
        }};

        double best_score = -1.;
        for (auto it = support_paths.rbegin(); it != support_paths.rend(); ++it) {
            if (it->polyline.points.size() < 2)
                continue;

            double score = 0.;
            for (const auto &[distance_along, weight] : samples) {
                Point sample = point_at_distance(candidate, distance_along);
                std::pair<int, Point> foot = foot_pt(it->polyline.points, sample);
                int seg_idx = foot.first;
                if (seg_idx < 0 || size_t(seg_idx + 1) >= it->polyline.points.size())
                    continue;

                const Point &a = it->polyline.points[size_t(seg_idx)];
                const Point &b = it->polyline.points[size_t(seg_idx + 1)];
                const bool interior_projection = foot.second != a && foot.second != b;
                const double distance_to_support = (sample - foot.second).cast<double>().norm();
                const double normalized_support = std::max(0.0, 1.0 - distance_to_support / double(std::max<coord_t>(1, support_reach)));

                score += weight * (3.0 * normalized_support + (interior_projection ? 1.5 : 0.2));
            }

            best_score = std::max(best_score, score);
        }

        return best_score;
    };

    ExtrusionPaths support_paths = roof_region;
    const coord_t support_reach = std::max<coord_t>(wave_flow.scaled_width(), connector_limit);
    const coord_t prefix_length = std::max<coord_t>(wave_flow.scaled_width(), connector_limit / 2);

    for (const Polyline &source_front : fronts) {
        Polyline front = source_front;
        if (front.points.size() < 2)
            continue;

        Polyline reversed = front;
        reversed.reverse();
        const double forward_score = support_score(front, support_paths, support_reach, prefix_length);
        const double reverse_score = support_score(reversed, support_paths, support_reach, prefix_length);
        if (reverse_score > forward_score)
            front.reverse();

        roof_region.push_back(make_wave_path(front, wave_flow, role));
        support_paths.push_back(make_wave_path(std::move(front), wave_flow, role));
    }
}

// NEOTKO_WAVESUPPORT_TAG_VARIANTS — Fase 4c. Monotonic: emit each front as its own path, in the
// given order, no reordering or stitching. The simplest strategy — predictable, more travels.
void append_wave_fronts_monotonic(ExtrusionPaths &roof_region, const Polylines &fronts, const Flow &wave_flow, ExtrusionRole role)
{
    for (const Polyline &front : fronts)
        if (front.points.size() >= 2)
            roof_region.push_back(make_wave_path(front, wave_flow, role));
}

// NEOTKO_WAVESUPPORT_TAG_VARIANTS — Fase 4c. ZigZag: walk the front levels outer→inner, greedily
// chaining each front to the previous one's endpoint (flipping when closer) so the whole region
// prints as few continuous boustrophedon polylines as possible. Ported verbatim from Klappe's
// append_zig_zag_front_levels, adapted to our 3-arg make_wave_path (role passed through).
void append_zig_zag_front_levels(ExtrusionPaths &roof_region, const std::vector<Polylines> &front_levels, const Flow &wave_flow, coord_t connector_limit, ExtrusionRole role)
{
    if (front_levels.empty())
        return;

    std::vector<std::vector<bool>> used;
    used.reserve(front_levels.size());
    for (const Polylines &level : front_levels)
        used.emplace_back(level.size(), false);

    const double max_connector_distance_sq = double(connector_limit) * double(connector_limit);

    auto append_or_start = [&](Polyline &&front) {
        if (roof_region.empty()) {
            roof_region.push_back(make_wave_path(std::move(front), wave_flow, role));
            return;
        }
        ExtrusionPath &current = roof_region.back();
        const double d_keep = (current.last_point() - front.first_point()).cast<double>().squaredNorm();
        const double d_flip = (current.last_point() - front.last_point()).cast<double>().squaredNorm();
        if (std::min(d_keep, d_flip) > max_connector_distance_sq) {
            roof_region.push_back(make_wave_path(std::move(front), wave_flow, role));
            return;
        }
        if (d_flip < d_keep)
            front.reverse();
        if (current.last_point() == front.first_point())
            current.polyline.append(front.points.begin() + 1, front.points.end());
        else
            current.polyline.append(std::move(front));
    };

    std::function<void(size_t, size_t, bool)> follow_branch = [&](size_t level_idx, size_t front_idx, bool reverse_front) {
        used[level_idx][front_idx] = true;
        Polyline current = front_levels[level_idx][front_idx];
        if (current.points.size() < 2)
            return;
        if (reverse_front)
            current.reverse();

        append_or_start(std::move(current));

        for (size_t next_level = level_idx + 1; next_level < front_levels.size(); ++next_level) {
            size_t best_idx = size_t(-1);
            bool   reverse_child = false;
            double best_d = max_connector_distance_sq;

            const Point anchor = roof_region.back().last_point();
            for (size_t candidate_idx = 0; candidate_idx < front_levels[next_level].size(); ++candidate_idx) {
                if (used[next_level][candidate_idx])
                    continue;
                const Polyline &candidate = front_levels[next_level][candidate_idx];
                if (candidate.points.size() < 2)
                    continue;
                const double d_keep = (anchor - candidate.first_point()).cast<double>().squaredNorm();
                if (d_keep <= best_d) { best_d = d_keep; best_idx = candidate_idx; reverse_child = false; }
                const double d_flip = (anchor - candidate.last_point()).cast<double>().squaredNorm();
                if (d_flip <= best_d) { best_d = d_flip; best_idx = candidate_idx; reverse_child = true; }
            }

            if (best_idx == size_t(-1) || best_d > max_connector_distance_sq)
                break;
            follow_branch(next_level, best_idx, reverse_child);
            return;
        }
    };

    for (size_t level_idx = 0; level_idx < front_levels.size(); ++level_idx)
        for (size_t front_idx = 0; front_idx < front_levels[level_idx].size(); ++front_idx)
            if (! used[level_idx][front_idx])
                follow_branch(level_idx, front_idx, false);
}

} // namespace

ExtrusionPaths FillWaveRoof::generate(const ExPolygons &region, const FillWaveRoofParams &params) const
{
    ExtrusionPaths result;
    if (region.empty())
        return result;

    const coord_t base_spacing      = params.flow.scaled_spacing();
    const Flow    wave_flow         = params.line_width_mm > 0. ? params.flow.with_width(float(params.line_width_mm)) : params.flow;
    const coord_t wave_spacing      = std::max<coord_t>(1, params.line_spacing_mm > 0. ? coord_t(scale_(params.line_spacing_mm)) : base_spacing);
    const coord_t perimeter_overlap = std::max<coord_t>(0, params.perimeter_overlap_mm > 0. ? coord_t(scale_(params.perimeter_overlap_mm)) : 0);
    const coord_t min_wave_width    = std::max<coord_t>(0, params.minimum_wave_width_mm > 0. ? coord_t(scale_(params.minimum_wave_width_mm)) : 0);
    const coord_t half_line_width   = std::max<coord_t>(1, wave_flow.scaled_width() / 2);
    const double  simplify_tol      = params.scaled_resolution > 0. ? params.scaled_resolution : double(scale_(0.05));
    const coord_t connector_limit   = std::max<coord_t>(wave_spacing, wave_flow.scaled_width()) + perimeter_overlap;
    // Map min_new_area (mm^2) into Clipper's scaled area units; fall back to the
    // legacy 0.05 * spacing^2 heuristic when the caller leaves it at 0.
    const double  min_area          = params.min_new_area_mm2 > 0.
                                      ? scale_(1.) * scale_(1.) * params.min_new_area_mm2
                                      : 0.05 * double(wave_spacing) * double(wave_spacing);

    for (const ExPolygon &roof : union_ex(region)) {
        ExtrusionPaths roof_region;
        int region_fronts = 0, region_iterations = 0;

        // Split the cover at necks narrower than the line, so the collapsing
        // wavefront yields clean separate loops rather than pinching.
        ExPolygons split_covers = { roof };
        if (Polygons slits = generate_narrow_split_slits(roof, wave_spacing, min_wave_width); ! slits.empty())
            split_covers = union_ex(diff_ex(ExPolygons{ roof }, slits));

        for (const ExPolygon &split_cover : split_covers) {
            std::vector<Polylines> front_levels;
            const Polygons cover_polygons = to_polygons(split_cover);
            int iteration = 0;

            if (params.shape == WaveRoofShape::Concentric) {
                // NEOTKO_WAVESUPPORT_TAG_VARIANTS — Fase 4d, CONCENTRIC shape (original Fase 3
                // behaviour, restored as an explicit option): seed the full closed boundary and
                // collapse it inward, one wave_spacing per step → nested rings. Radial — natural for
                // bridging inward over a hollow pillar. reverse_order flips outer-first ↔ inner-first.
                Polygons current = shrink(cover_polygons, half_line_width, jtRound, 0.);
                if (current.empty())
                    current = cover_polygons;
                while (! current.empty()) {
                    if (params.max_iterations > 0 && iteration >= params.max_iterations)
                        break;
                    ++iteration;

                    Polylines fronts = to_polylines(current);
                    for (Polyline &front : fronts)
                        front.simplify(std::min(0.05 * double(wave_spacing), simplify_tol));
                    fronts.erase(
                        std::remove_if(fronts.begin(), fronts.end(), [](const Polyline &front) { return front.points.size() < 2; }),
                        fronts.end());
                    fronts = reconnect_polylines(fronts, wave_spacing);
                    if (! fronts.empty())
                        front_levels.emplace_back(std::move(fronts));

                    Polygons next = shrink(current, wave_spacing, jtRound, 0.);
                    if (next.empty() || area(next) < min_area)
                        break;
                    current = std::move(next);
                }
                if (params.reverse_order)
                    std::reverse(front_levels.begin(), front_levels.end());
            } else {
                // NEOTKO_WAVESUPPORT_TAG_VARIANTS — Fase 4d, WAVE shape (Camino 2): TRUE directional
                // propagation (open arcs), NOT rings. Seed a thin sliver at one edge and GROW the
                // accumulated area outward one wave_spacing at a time; each front is the LEADING edge
                // of the accumulated region, clipped to the interior (trim_boundary). On irregular
                // regions this sweeps as one continuous diffracting front instead of fragmenting.
                // reverse_order picks the opposite seed edge → sweeps the other way.
                //
                // trim_boundary = cover inset half a line: clipping the accumulated boundary to it
                // drops the edges running along the region outline, keeping only the advancing front.
                Polygons trim_boundary = shrink(cover_polygons, half_line_width, jtRound, 0.);
                if (trim_boundary.empty())
                    trim_boundary = cover_polygons;

                // Seed sliver: a strip ~half a line wide at the min (or max, if reversed) end of the
                // region's LONGER axis, so the wave sweeps along that axis (longest continuous fronts).
                const BoundingBox cover_bb = get_extents(cover_polygons);
                const bool    sweep_x    = cover_bb.size().x() >= cover_bb.size().y();
                const coord_t seed_thick = std::max<coord_t>(half_line_width, wave_spacing / 2);
                Polygon seed_band;
                {
                    const coord_t pad = seed_thick + SCALED_EPSILON;
                    Point lo = cover_bb.min - Point(pad, pad);
                    Point hi = cover_bb.max + Point(pad, pad);
                    if (sweep_x) {
                        lo.x() = params.reverse_order ? cover_bb.max.x() - seed_thick : lo.x();
                        hi.x() = params.reverse_order ? hi.x()                        : cover_bb.min.x() + seed_thick;
                    } else {
                        lo.y() = params.reverse_order ? cover_bb.max.y() - seed_thick : lo.y();
                        hi.y() = params.reverse_order ? hi.y()                        : cover_bb.min.y() + seed_thick;
                    }
                    seed_band.points = { lo, Point(hi.x(), lo.y()), hi, Point(lo.x(), hi.y()) };
                }
                Polygons accumulated = intersection(cover_polygons, Polygons{ seed_band });
                // Degenerate region (sliver empty): fall back to the whole cover so we still emit
                // something (the caller also has a normal-fill safety net).
                if (accumulated.empty())
                    accumulated = shrink(cover_polygons, half_line_width, jtRound, 0.);
                if (accumulated.empty())
                    accumulated = cover_polygons;

                double accumulated_area = area(accumulated);
                for (;;) {
                    if (params.max_iterations > 0 && iteration >= params.max_iterations)
                        break;
                    ++iteration;

                    // Leading edge of the current accumulated region = one wavefront.
                    Polylines fronts = intersection_pl(to_polylines(accumulated), trim_boundary);
                    for (Polyline &front : fronts)
                        front.simplify(std::min(0.05 * double(wave_spacing), simplify_tol));
                    fronts.erase(
                        std::remove_if(fronts.begin(), fronts.end(), [](const Polyline &front) { return front.points.size() < 2; }),
                        fronts.end());
                    fronts = reconnect_polylines(fronts, wave_spacing);
                    if (! fronts.empty())
                        front_levels.emplace_back(std::move(fronts));

                    Polygons next = intersection(expand(accumulated, float(wave_spacing), jtRound, 0.), cover_polygons);
                    if (next.empty())
                        break;
                    const double next_area = area(next);
                    if (next_area <= accumulated_area + min_area) // converged: no new territory to cover
                        break;
                    accumulated      = std::move(next);
                    accumulated_area = next_area;
                }
            }
            region_iterations += iteration;

            if (! front_levels.empty()) {

                for (const Polylines &level : front_levels)
                    region_fronts += int(level.size());

                switch (params.pattern) {
                case WaveRoofPattern::ZigZag:
                    append_zig_zag_front_levels(roof_region, front_levels, wave_flow, connector_limit, params.role);
                    break;
                case WaveRoofPattern::Monotonic: {
                    Polylines collected;
                    for (const Polylines &level : front_levels)
                        collected.insert(collected.end(), level.begin(), level.end());
                    append_wave_fronts_monotonic(roof_region, collected, wave_flow, params.role);
                    break;
                }
                case WaveRoofPattern::Smart:
                default: {
                    Polylines collected;
                    for (const Polylines &level : front_levels)
                        collected.insert(collected.end(), level.begin(), level.end());
                    append_wave_fronts(roof_region, collected, wave_flow, connector_limit, params.role);
                    break;
                }
                }
            }
        }

        roof_region.erase(
            std::remove_if(roof_region.begin(), roof_region.end(), [](const ExtrusionPath &path) { return path.empty(); }),
            roof_region.end());

        // NEOTKO_WAVESUPPORT_TAG_VARIANTS — Camino 2 diagnostic. With the directional sweep, the
        // 3 patterns now differ in path count (ZigZag chains fronts → paths << fronts) and reverse
        // flips the SEED edge (spatial sweep direction), so first_path_r vs last_path_r should swap
        // between reverse OFF/ON. Echoes pattern+reverse to confirm the config reached the engine.
        const char *shape_name = params.shape == WaveRoofShape::Concentric ? "Concentric" : "Wave";
        const char *pat_name = params.pattern == WaveRoofPattern::ZigZag ? "ZigZag" :
                               params.pattern == WaveRoofPattern::Monotonic ? "Monotonic" : "Smart";
        double first_r = -1., last_r = -1.;
        if (! roof_region.empty()) {
            const Point c = roof.contour.bounding_box().center();
            first_r = unscale<double>((roof_region.front().first_point() - c).cast<double>().norm());
            last_r  = unscale<double>((roof_region.back().first_point()  - c).cast<double>().norm());
        }
        WAVEROOF_LOG("FillWaveRoof: region area=" << unscale<double>(unscale<double>(roof.area()))
            << "mm2 splits=" << split_covers.size() << " iterations=" << region_iterations
            << " fronts=" << region_fronts << " paths=" << roof_region.size()
            << " shape=" << shape_name << " order=" << pat_name << " reverse=" << (params.reverse_order ? 1 : 0)
            << " first_path_r=" << first_r << "mm last_path_r=" << last_r << "mm");

        append(result, std::move(roof_region));
    }

    return result;
}

} // namespace Slic3r
