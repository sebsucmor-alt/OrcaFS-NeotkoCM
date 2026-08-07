#include "Layer.hpp"
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Exception.hpp"
#include "Geometry.hpp"
#include "PerimeterGenerator.hpp"
#include "Print.hpp"
#include "Feature/TextureBump/TextureBump.hpp"
#include "Surface.hpp"
#include "NeoArachne/NeoArachneEngine.hpp" // NEOTKO_NEOARACHNE_TAG Inc2e (port s134)
#include "BoundingBox.hpp"
#include "SVG.hpp"
#include "Algorithm/RegionExpansion.hpp"
#include "NeoDebug.hpp"   // NEOTKO_BRIDGE_TAG s230 — canal BOTTOM para la traza de expansión
#include "Feature/HeightAdaptive/HeightCurve.hpp" // NEOTKO_HAE_TAG s247 — sparse infill width by height

#include <algorithm>
#include <cmath>
#include <string>
#include <sstream>   // NEOTKO_BRIDGE_TAG s230 — traza BRIDGE_EXPAND
#include <map>

#include <boost/log/trivial.hpp>
#include <boost/algorithm/clamp.hpp>

namespace Slic3r {

namespace {

unsigned int effective_layer_filament_id(const Layer &layer, unsigned int filament_id)
{
    if (filament_id == 0)
        return filament_id;

    const PrintObject *object = layer.object();
    const Print       *print  = object ? object->print() : nullptr;
    if (print == nullptr)
        return filament_id;

    const size_t num_physical = print->config().filament_diameter.size();
    if (num_physical == 0)
        return filament_id;

    // Ordinary mixed rows still print with one physical filament per layer even
    // when region collapse is disabled, so geometry / flow decisions must use
    // that effective physical filament to avoid per-layer thin-feature drift.
    return print->mixed_filament_manager().effective_painted_region_filament_id(filament_id,
                                                                                num_physical,
                                                                                int(layer.id()),
                                                                                float(layer.print_z),
                                                                                float(layer.height));
}

unsigned int effective_infill_filament_id(const Layer &layer, const PrintRegionConfig &config, unsigned int filament_id)
{
    const unsigned int effective = effective_layer_filament_id(layer, filament_id);
    if (effective != filament_id || filament_id == 0)
        return effective;

    const PrintObject *object = layer.object();
    const Print       *print  = object ? object->print() : nullptr;
    if (print == nullptr)
        return filament_id;

    const size_t num_physical = print->config().filament_diameter.size();
    if (num_physical == 0)
        return filament_id;

    const MixedFilamentManager &mixed_mgr = print->mixed_filament_manager();
    const MixedFilament *mixed_row = mixed_mgr.mixed_filament_from_id(filament_id, num_physical);
    if (mixed_row == nullptr)
        return filament_id;

    const std::string normalized_pattern = MixedFilamentManager::normalize_manual_pattern(mixed_row->manual_pattern);
    if (normalized_pattern.find(',') == std::string::npos)
        return filament_id;

    const int innermost_perimeter_index = std::max(0, config.wall_loops.value - 1);
    return mixed_mgr.resolve_perimeter(filament_id,
                                       num_physical,
                                       int(layer.id()),
                                       innermost_perimeter_index,
                                       float(layer.print_z),
                                       float(layer.height),
                                       false,
                                       object);
}

} // namespace

unsigned int LayerRegion::extruder(FlowRole role) const
{
    const PrintRegionConfig &config = this->region().config();
    unsigned int             filament_id = 0;
    if (role == frInfill)
        filament_id = config.sparse_infill_filament.value;
    else if (role == frSolidInfill && std::abs(config.sparse_infill_density.value - 100.) < EPSILON)
        filament_id = config.sparse_infill_filament.value;
    else
        filament_id = this->region().extruder(role);

    return (role == frInfill || role == frSolidInfill) ?
        effective_infill_filament_id(*m_layer, config, filament_id) :
        effective_layer_filament_id(*m_layer, filament_id);
}

Flow LayerRegion::flow(FlowRole role) const
{
    return this->flow(role, m_layer->height);
}

Flow LayerRegion::flow(FlowRole role, double layer_height) const
{
    return this->flow(role, layer_height, m_layer->id() == 0);
}

Flow LayerRegion::flow(FlowRole role, double layer_height, bool use_initial_layer_width) const
{
    const PrintConfig          &print_config = m_layer->object()->print()->config();
    ConfigOptionFloatOrPercent config_width;
    if (use_initial_layer_width && print_config.initial_layer_line_width.value > 0) {
        config_width = print_config.initial_layer_line_width;
    } else if (role == frExternalPerimeter) {
        config_width = m_region->config().outer_wall_line_width;
    } else if (role == frPerimeter) {
        config_width = m_region->config().inner_wall_line_width;
    } else if (role == frInfill) {
        config_width = m_region->config().sparse_infill_line_width;
    } else if (role == frSolidInfill) {
        config_width = m_region->config().internal_solid_infill_line_width;
    } else if (role == frTopSolidInfill) {
        config_width = m_region->config().top_surface_line_width;
    } else {
        BOOST_LOG_TRIVIAL(error) << "Unknown role in LayerRegion::flow: " << int(role);
        assert(false);
        return Flow();
    }

    if (config_width.value == 0)
        config_width = m_layer->object()->config().line_width;

    const auto nozzle_diameter = float(print_config.nozzle_diameter.get_at(this->extruder(role) - 1));

    // NEOTKO_HAE_TAG s247 — Height Adaptive Effects, effect B: sparse infill line width by height
    // (docs/FUTURE/HEIGHT_ADAPTIVE_EFFECTS_PLAN.md §6). Hooking HERE and not in Fill.cpp is
    // deliberate: this one function feeds BOTH sparse-infill consumers — params.flow and the
    // deliberately layer-height-independent params.spacing of Fill.cpp:957 — so they can never
    // disagree about the width of the same layer.
    //
    // Stepped is the DEFAULT, not a decree (s247 revision of plan §6.2). Sparse infill stands on
    // itself — the spacing is held constant across layers on purpose — so a width that drifts
    // walks every line off the one below it. But what breaks the stacking is the drift between
    // CONSECUTIVE layers, which is proportional to how steep the curve is, not to it being a
    // curve: a slow ramp moves each layer by a fraction of a micron. The curve therefore carries
    // its own mode; the argument below is only the fallback for curves stored before the mode
    // token existed, which is exactly how those were evaluated.
    //
    // The curve is a per-OBJECT key overriding a per-REGION value at the point of consumption; the
    // PrintRegion itself stays immutable. Skipped when the initial-layer width is in force: layer 0
    // has its own width for adhesion reasons that have nothing to do with the curve.
    // NEOTKO_HAE_TAG s248 — Adaptive Effector LEVEL 0 (ADAPTIVE_EFFECTOR_PLAN.md §4). The block
    // below was written for frInfill alone; it is now the same override for all FIVE roles,
    // because this function is where every one of them is resolved. That is the whole finding of
    // the plan: one hook, five line widths, and no way for two of them to disagree about a layer.
    //
    // 🔑 And this covers PerimeterGenerator for free: it does not read the wall widths out of the
    // config, it is HANDED perimeter_flow / ext_perimeter_flow / solid_infill_flow built right
    // here (LayerRegion.cpp:248 and :295-297). The plan's "outer_wall_line_width has a second
    // consumer" warning was a mis-generalisation of the sparse-infill case, where
    // PerimeterGenerator genuinely does read the region key raw to size gap fill.
    const bool initial_layer_width_wins = use_initial_layer_width && print_config.initial_layer_line_width.value > 0;
    if (! initial_layer_width_wins) {
        const PrintObjectConfig &oc = m_layer->object()->config();
        const std::string *curve_str = nullptr;
        switch (role) {
        case frInfill:            curve_str = &oc.neotko_hae_infill_width.value;       break;
        case frPerimeter:         curve_str = &oc.neotko_hae_inner_wall_width.value;   break;
        case frExternalPerimeter: curve_str = &oc.neotko_hae_outer_wall_width.value;   break;
        default:                  break;
        }
        // Empty string is the overwhelmingly common case: cost is one length check, and the flow is
        // then computed exactly as stock (golden rule of plan §3).
        if (curve_str != nullptr && ! curve_str->empty()) {
            const auto curve = HeightAdaptive::HeightCurve::parse(*curve_str, HeightAdaptive::Interp::Stepped);
            if (! curve.empty()) {
                double base_mm = config_width.get_abs_value(nozzle_diameter);
                if (base_mm <= 0.)
                    base_mm = Flow::auto_extrusion_width(role, nozzle_diameter);
                // Curve values are absolute mm (the gizmo edits mm over the real layer bands).
                const double w = curve.at(m_layer->slice_z, base_mm);
                if (w > 0.)
                    config_width = ConfigOptionFloatOrPercent(w, false);
            }
        }
    }

    return Flow::new_from_config_width(role, config_width, nozzle_diameter, float(layer_height));
}

Flow LayerRegion::bridging_flow(FlowRole role, bool thick_bridge) const
{
    const PrintRegion       &region         = this->region();
    const PrintRegionConfig &region_config  = region.config();
    const PrintObject       &print_object   = *this->layer()->object();
    Flow bridge_flow;
    auto nozzle_diameter = float(print_object.print()->config().nozzle_diameter.get_at(this->extruder(role) - 1));
    if (thick_bridge) {
        // The old Slic3r way (different from all other slicers): Use rounded extrusions.
        // Get the configured nozzle_diameter for the extruder associated to the flow role requested.
        // Here this->extruder(role) - 1 may underflow to MAX_INT, but then the get_at() will follback to zero'th element, so everything is all right.
        // Applies default bridge spacing.
        bridge_flow = Flow::bridging_flow(float(sqrt(region_config.bridge_flow)) * nozzle_diameter, nozzle_diameter);
    } else {
        // The same way as other slicers: Use normal extrusions. Apply bridge_flow while maintaining the original spacing.
        bridge_flow = this->flow(role).with_flow_ratio(region_config.bridge_flow);
    }
    return bridge_flow;

}

// Fill in layerm->fill_surfaces by trimming the layerm->slices by the cummulative layerm->fill_surfaces.
void LayerRegion::slices_to_fill_surfaces_clipped()
{
    // Note: this method should be idempotent, but fill_surfaces gets modified 
    // in place. However we're now only using its boundaries (which are invariant)
    // so we're safe. This guarantees idempotence of prepare_infill() also in case
    // that combine_infill() turns some fill_surface into VOID surfaces.
    // Collect polygons per surface type.
    std::array<SurfacesPtr, size_t(stCount)> by_surface;
    for (Surface &surface : this->slices.surfaces)
        by_surface[size_t(surface.surface_type)].emplace_back(&surface);
    // Trim surfaces by the fill_boundaries.
    this->fill_surfaces.surfaces.clear();
    for (size_t surface_type = 0; surface_type < size_t(stCount); ++ surface_type) {
        const SurfacesPtr &this_surfaces = by_surface[surface_type];
        if (! this_surfaces.empty())
            this->fill_surfaces.append(intersection_ex(this_surfaces, this->fill_expolygons), SurfaceType(surface_type));
    }
}

void LayerRegion::make_perimeters(const SurfaceCollection &slices, const LayerRegionPtrs &compatible_regions, SurfaceCollection* fill_surfaces, ExPolygons* fill_no_overlap)
{
    this->perimeters.clear();
    this->thin_fills.clear();

    const PrintConfig       &print_config  = this->layer()->object()->print()->config();
    const PrintRegionConfig &region_config = this->region().config();
    const PrintObjectConfig& object_config = this->layer()->object()->config();
    PrintRegionConfig        perimeter_config = region_config;
    perimeter_config.wall_filament.value = int(effective_layer_filament_id(*this->layer(), unsigned(std::max(0, region_config.wall_filament.value))));
    perimeter_config.sparse_infill_filament.value = int(effective_layer_filament_id(*this->layer(), unsigned(std::max(0, region_config.sparse_infill_filament.value))));
    perimeter_config.solid_infill_filament.value = int(effective_layer_filament_id(*this->layer(), unsigned(std::max(0, region_config.solid_infill_filament.value))));
    // This needs to be in sync with PrintObject::_slice() slicing_mode_normal_below_layer!
    bool spiral_mode = print_config.spiral_mode &&
        //FIXME account for raft layers.
        (this->layer()->id() >= size_t(region_config.bottom_shell_layers.value) &&
         this->layer()->print_z >= region_config.bottom_shell_thickness - EPSILON);

    PerimeterGenerator g(
        // input:
        &slices,
        &compatible_regions,
        this->layer()->height,
        this->layer()->slice_z,
        this->flow(frPerimeter),
        &perimeter_config,
        &this->layer()->object()->config(),
        &print_config,
        spiral_mode,
        
        // output:
        &this->perimeters,
        &this->thin_fills,
        fill_surfaces,
        //BBS
        fill_no_overlap
    );
    
    if (this->layer()->lower_layer != nullptr)
        // Cummulative sum of polygons over all the regions.
        g.lower_slices = &this->layer()->lower_layer->lslices;
    // NEOTKO_GRAVITY_TAG s226 — Fase 3: overhang is measured against the REAL floor, not just this
    // object's own previous layer. Widen lower_slices with the foreign floor under this layer (top
    // of any neighbour), so a wall resting on another piece is not flagged overhang; and at layer 0
    // (lower_layer null) a non-empty floor gives PerimeterGenerator something to rest on — the part
    // over air stays overhang, the part on the neighbour does not. The floor was built ONCE in
    // PrintObject::make_perimeters(); this local must outlive process_arachne()/process_classic()
    // below, same lifetime rule as painted_texture_bump_zones. Empty floor → stock pointer untouched
    // (byte-identical when Gravity is off).
    ExPolygons gravity_lower_slices;
    {
        const std::vector<Polygons> &floor = this->layer()->object()->gravity_floor();
        const size_t                 li    = this->layer()->id();
        if (li < floor.size() && ! floor[li].empty()) {
            Polygons combined;
            if (this->layer()->lower_layer != nullptr)
                append(combined, to_polygons(this->layer()->lower_layer->lslices));
            append(combined, floor[li]);
            gravity_lower_slices = union_ex(combined);
            g.lower_slices       = &gravity_lower_slices;
            g.has_gravity_floor  = true;
        }
    }
    if (this->layer()->upper_layer != NULL)
        g.upper_slices = &this->layer()->upper_layer->lslices;

    int region_id = this->region().print_object_region_id();
    if (this->layer()->upper_layer != NULL)
        g.upper_slices_same_region = &this->layer()->upper_layer->get_region(region_id)->slices;

    g.layer_id              = (int)this->layer()->id();
    g.ext_perimeter_flow    = this->flow(frExternalPerimeter);
    g.overhang_flow         = this->bridging_flow(frPerimeter, object_config.thick_bridges);
    g.solid_infill_flow     = this->flow(frSolidInfill);
    // NEOTKO_TEXTUREBUMP_TAG — Fase 3: nullptr-safe/empty-safe in all the same ways the v1 single
    // table was (find() against an empty map is just as harmless as empty() was).
    g.texture_bump_tables    = &this->layer()->object()->texture_bump_tables();
    // NEOTKO_TEXTUREBUMP_TAG -- fix (2026-07-08): see PerimeterGenerator.hpp's own comment on this
    // field -- the object-level plane transform has to reach group_region_by_texture_bump()'s cfg
    // reconstruction the same way it already reaches PrintObject::make_perimeters()'s table build.
    g.texture_bump_plane_transform = this->layer()->object()->model_object()->texture_bump_plane_transform;
    // NEOTKO_TEXTUREBUMP_TAG — Fase 3: painted zones resolved for THIS layer specifically (own
    // canvas, not ColorMix's). `painted_zones` must outlive g.process_arachne()/process_classic()
    // below, hence the local (not a temporary) -- PerimeterGenerator only stores a pointer to it.
    std::vector<Slic3r::Feature::TextureBump::PaintedTextureBumpZone> painted_texture_bump_zones =
        Slic3r::Feature::TextureBump::painted_texture_bump_zones_in_layer(
            this->layer()->object(), this->layer()->slice_z, this->layer()->height);
    g.painted_texture_bump_zones = &painted_texture_bump_zones;

    // NEOTKO_NEOSTITCH_TAG — Z-Stitch Interlock (docs/FUTURE/NEOSTITCH_PLAN.md): object-centered mm
    // bounds, same formula as TextureBump's own tb_bounds (PrintObject.cpp's build_tables_for_configs
    // call site) so NeoStitch's compute_u() call shares the exact same world-space convention.
    {
        const Vec3crd& obj_size = this->layer()->object()->size();
        g.neostitch_bounds.min = Vec3d(-unscale_(obj_size.x()) / 2.0, -unscale_(obj_size.y()) / 2.0, 0.0);
        g.neostitch_bounds.max = Vec3d( unscale_(obj_size.x()) / 2.0,  unscale_(obj_size.y()) / 2.0,  unscale_(obj_size.z()));
    }

    if (this->layer()->object()->config().wall_generator.value == PerimeterGeneratorType::Arachne && !spiral_mode)
        g.process_arachne();
    // NEOTKO_NEOARACHNE_TAG Inc2e (port s134) — NeoArachne hybrid wall generator. The real engine
    // runs only when LibreMode is active (Tier-B gate, slice-time flag neotko_libre_mode injected at
    // background_process.apply). If a preset carries wall_generator=neoarachne but LibreMode is off,
    // fall back to stock Arachne (NeoArachne is Arachne-based; closest intent). Spiral mode → Classic.
    else if (this->layer()->object()->config().wall_generator.value == PerimeterGeneratorType::NeoArachne && !spiral_mode) {
        if (this->layer()->object()->config().neotko_libre_mode.value)
            Slic3r::NeoArachne::run(g);
        else
            g.process_arachne();
    }
    else
        g.process_classic();
}

#if 1

// Extract surfaces of given type from surfaces, extract fill (layer) thickness of one of the surfaces.
static ExPolygons fill_surfaces_extract_expolygons(Surfaces &surfaces, std::initializer_list<SurfaceType> surface_types, double &thickness)
{
    size_t cnt = 0;
    for (const Surface &surface : surfaces)
        if (std::find(surface_types.begin(), surface_types.end(), surface.surface_type) != surface_types.end()) {
            ++cnt;
            thickness = surface.thickness;
        }
    if (cnt == 0)
        return {};

    ExPolygons out;
    out.reserve(cnt);
    for (Surface &surface : surfaces)
        if (std::find(surface_types.begin(), surface_types.end(), surface.surface_type) != surface_types.end())
            out.emplace_back(std::move(surface.expolygon));
    return out;
}

struct ExpansionZone
{
    ExPolygons                           expolygons;
    Algorithm::RegionExpansionParameters parameters;
    bool                                 expanded_into = false;
};

// Cache for detecting bridge orientation and merging regions with overlapping expansions.
struct Bridge {
    ExPolygon expolygon;
    uint32_t group_id;
    std::vector<Algorithm::RegionExpansionEx>::const_iterator bridge_expansion_begin;
    std::optional<double> angle{std::nullopt};
};

// Group the bridge surfaces by overlaps.
uint32_t group_id(std::vector<Bridge> &bridges, uint32_t src_id) {
    uint32_t group_id = bridges[src_id].group_id;
    while (group_id != src_id) {
        src_id = group_id;
        group_id = bridges[src_id].group_id;
    }
    bridges[src_id].group_id = group_id;
    return group_id;
};

std::vector<Bridge> get_grouped_bridges(
    ExPolygons&& bridge_expolygons,
    const std::vector<Algorithm::RegionExpansionEx>& bridge_expansions
) {
    using namespace Algorithm;

    std::vector<Bridge> result;
    {
        result.reserve(bridge_expansions.size());
        uint32_t group_id = 0;
        using std::move_iterator;
        for (ExPolygon& expolygon : bridge_expolygons)
            result.push_back({ std::move(expolygon), group_id ++, bridge_expansions.end() });
    }


    // Detect overlaps of bridge anchors inside their respective shell regions.
    // bridge_expansions are sorted by boundary id and source id.
    for (auto expansion_iterator = bridge_expansions.begin(); expansion_iterator != bridge_expansions.end();) {
        auto boundary_region_begin = expansion_iterator;
        auto boundary_region_end = std::find_if(
            next(expansion_iterator),
            bridge_expansions.end(),
            [&](const RegionExpansionEx& expansion){
                return expansion.boundary_id != expansion_iterator->boundary_id;
            }
        );

        // Cache of bboxes per expansion boundary.
        std::vector<BoundingBox> bounding_boxes;
        bounding_boxes.reserve(std::distance(boundary_region_begin, boundary_region_end));
        std::transform(
            boundary_region_begin,
            boundary_region_end,
            std::back_inserter(bounding_boxes),
            [](const RegionExpansionEx& expansion){
                return get_extents(expansion.expolygon.contour);
            }
        );

        // For each bridge anchor of the current source:
        for (;expansion_iterator != boundary_region_end; ++expansion_iterator) {
            auto candidate_iterator = std::next(expansion_iterator);
            for (;candidate_iterator != boundary_region_end; ++candidate_iterator) {
                const BoundingBox& current_bounding_box{
                    bounding_boxes[expansion_iterator - boundary_region_begin]
                };
                const BoundingBox& candidate_bounding_box{
                    bounding_boxes[candidate_iterator - boundary_region_begin]
                };
                if (
                    expansion_iterator->src_id != candidate_iterator->src_id
                    && current_bounding_box.overlap(candidate_bounding_box)
                    // One may ignore holes, they are irrelevant for intersection test.
                    && !intersection(expansion_iterator->expolygon.contour, candidate_iterator->expolygon.contour).empty()
                ) {
                    // The two bridge regions intersect. Give them the same (lower) group id.
                    uint32_t id  = group_id(result, expansion_iterator->src_id);
                    uint32_t id2 = group_id(result, candidate_iterator->src_id);
                    if (id < id2)
                        result[id2].group_id = id;
                    else
                        result[id].group_id = id2;
                }
            }
        }
    }
    return result;
}

void detect_bridge_directions(
    const Algorithm::WaveSeeds& bridge_anchors,
    std::vector<Bridge>& bridges,
    const std::vector<ExpansionZone>& expansion_zones
) {
    if (expansion_zones.empty()) {
        throw std::runtime_error("At least one expansion zone must exist!");
    }
    auto it_bridge_anchor = bridge_anchors.begin();
    for (uint32_t bridge_id = 0; bridge_id < uint32_t(bridges.size()); ++ bridge_id) {
        Bridge &bridge = bridges[bridge_id];
        Polygons anchor_areas;
        int32_t last_anchor_id = -1;
        for (; it_bridge_anchor != bridge_anchors.end() && it_bridge_anchor->src == bridge_id; ++ it_bridge_anchor) {
            if (last_anchor_id != int(it_bridge_anchor->boundary)) {
                last_anchor_id = int(it_bridge_anchor->boundary);

                unsigned start_index{};
                unsigned end_index{};
                for (const ExpansionZone& expansion_zone: expansion_zones) {
                    end_index += expansion_zone.expolygons.size();
                    if (last_anchor_id < static_cast<int64_t>(end_index)) {
                        append(anchor_areas, to_polygons(expansion_zone.expolygons[last_anchor_id - start_index]));
                        break;
                    }
                    start_index += expansion_zone.expolygons.size();
                }
            }
        }
        Lines lines{to_lines(diff_pl(to_polylines(bridge.expolygon), expand(anchor_areas, float(SCALED_EPSILON))))};
        auto [bridging_dir, unsupported_dist] = detect_bridging_direction(lines, to_polygons(bridge.expolygon));
        bridge.angle = M_PI + std::atan2(bridging_dir.y(), bridging_dir.x());

        if constexpr (false) {
            coordf_t    stroke_width = scale_(0.06);
            BoundingBox bbox         = get_extents(anchor_areas);
            bbox.merge(get_extents(bridge.expolygon));
            bbox.offset(scale_(1.));
            ::Slic3r::SVG
                svg(debug_out_path(("bridge" + std::to_string(*bridge.angle) + "_" /* + std::to_string(this->layer()->bottom_z())*/).c_str()),
                bbox);
            svg.draw(bridge.expolygon, "cyan");
            svg.draw(lines, "green", stroke_width);
            svg.draw(anchor_areas, "red");
        }
    }
}

Surfaces merge_bridges(
    std::vector<Bridge>& bridges,
    const std::vector<Algorithm::RegionExpansionEx>& bridge_expansions,
    const float closing_radius
) {
    for (auto it = bridge_expansions.begin(); it != bridge_expansions.end(); ) {
        bridges[it->src_id].bridge_expansion_begin = it;
        uint32_t src_id = it->src_id;
        for (++ it; it != bridge_expansions.end() && it->src_id == src_id; ++ it) ;
    }

    Surfaces result;
    for (uint32_t bridge_id = 0; bridge_id < uint32_t(bridges.size()); ++ bridge_id) {
        if (group_id(bridges, bridge_id) == bridge_id) {
            // Head of the group.
            Polygons acc;
            for (uint32_t bridge_id2 = bridge_id; bridge_id2 < uint32_t(bridges.size()); ++ bridge_id2)
                if (group_id(bridges, bridge_id2) == bridge_id) {
                    append(acc, to_polygons(std::move(bridges[bridge_id2].expolygon)));
                    auto it_bridge_expansion = bridges[bridge_id2].bridge_expansion_begin;
                    assert(it_bridge_expansion == bridge_expansions.end() || it_bridge_expansion->src_id == bridge_id2);
                    for (; it_bridge_expansion != bridge_expansions.end() && it_bridge_expansion->src_id == bridge_id2; ++ it_bridge_expansion)
                        append(acc, to_polygons(it_bridge_expansion->expolygon));
                }
            //FIXME try to be smart and pick the best bridging angle for all?
            if (!bridges[bridge_id].angle) {
                assert(false && "Bridge angle must be pre-calculated!");
            }
            Surface templ{ stBottomBridge, {} };
            templ.bridge_angle = bridges[bridge_id].angle ? *bridges[bridge_id].angle : -1;
            //NOTE: The current regularization of the shells can create small unasigned regions in the object (E.G. benchy)
            // without the following closing operation, those regions will stay unfilled and cause small holes in the expanded surface.
            // look for narrow_ensure_vertical_wall_thickness_region_radius filter.
            ExPolygons final = closing_ex(acc, closing_radius);
            // without safety offset, artifacts are generated (GH #2494)
            // union_safety_offset_ex(acc)
            for (ExPolygon &ex : final)
                result.emplace_back(templ, std::move(ex));
        }
    }
    return result;
}

struct ExpansionResult {
    Algorithm::WaveSeeds anchors;
    std::vector<Algorithm::RegionExpansionEx> expansions;
};

ExpansionResult expand_expolygons(
    const ExPolygons& expolygons,
    std::vector<ExpansionZone>& expansion_zones
) {
    using namespace Algorithm;
    WaveSeeds bridge_anchors;
    std::vector<RegionExpansionEx> bridge_expansions;

    unsigned processed_bridges_count = 0;
    for (ExpansionZone& expansion_zone : expansion_zones) {
        WaveSeeds seeds{wave_seeds(
            expolygons,
            expansion_zone.expolygons,
            expansion_zone.parameters.tiny_expansion,
            true
        )};
        std::vector<RegionExpansionEx> expansions{propagate_waves_ex(
            seeds,
            expansion_zone.expolygons,
            expansion_zone.parameters
        )};

        for (WaveSeed &seed : seeds)
            seed.boundary += processed_bridges_count;
        for (RegionExpansionEx &expansion : expansions)
            expansion.boundary_id += processed_bridges_count;

        expansion_zone.expanded_into = ! expansions.empty();

        append(bridge_anchors, std::move(seeds));
        append(bridge_expansions, std::move(expansions));

        processed_bridges_count += expansion_zone.expolygons.size();
    }
    return {bridge_anchors, bridge_expansions};
}

// Extract bridging surfaces from "surfaces", expand them into "shells" using expansion_params,
// detect bridges.
// Trim "shells" by the expanded bridges.
Surfaces expand_bridges_detect_orientations(
    Surfaces &surfaces,
    std::vector<ExpansionZone>& expansion_zones,
    const float closing_radius
)
{
    using namespace Slic3r::Algorithm;

    double thickness;
    ExPolygons bridge_expolygons = fill_surfaces_extract_expolygons(surfaces, {stBottomBridge}, thickness);
    if (bridge_expolygons.empty())
        return {};

    // Calculate bridge anchors and their expansions in their respective shell region.
    ExpansionResult expansion_result{expand_expolygons(
        bridge_expolygons,
        expansion_zones
    )};

    std::vector<Bridge> bridges{get_grouped_bridges(
        std::move(bridge_expolygons),
        expansion_result.expansions
    )};
    bridge_expolygons.clear();

    std::sort(expansion_result.anchors.begin(), expansion_result.anchors.end(), Algorithm::lower_by_src_and_boundary);
    detect_bridge_directions(expansion_result.anchors, bridges, expansion_zones);

    // Merge the groups with the same group id, produce surfaces by merging source overhangs with their newly expanded anchors.
    std::sort(expansion_result.expansions.begin(), expansion_result.expansions.end(), [](auto &l, auto &r) {
        return l.src_id < r.src_id || (l.src_id == r.src_id && l.boundary_id < r.boundary_id);
    });
    Surfaces out{merge_bridges(bridges, expansion_result.expansions, closing_radius)};

    // Clip by the expanded bridges.
    for (ExpansionZone& expansion_zone : expansion_zones)
        if (expansion_zone.expanded_into)
            expansion_zone.expolygons = diff_ex(expansion_zone.expolygons, out);
    return out;
}

Surfaces expand_merge_surfaces(
    Surfaces &surfaces,
    SurfaceType surface_type,
    std::vector<ExpansionZone>& expansion_zones,
    const float closing_radius,
    const double bridge_angle = -1
)
{
    using namespace Slic3r::Algorithm;

    double thickness;
    ExPolygons src = fill_surfaces_extract_expolygons(surfaces, {surface_type}, thickness);
    if (src.empty())
        return {};

    unsigned processed_expolygons_count = 0;
    std::vector<RegionExpansion> expansions;
    for (ExpansionZone& expansion_zone : expansion_zones) {
        std::vector<RegionExpansion> zone_expansions = propagate_waves(src, expansion_zone.expolygons, expansion_zone.parameters);
        expansion_zone.expanded_into = !zone_expansions.empty();

        for (RegionExpansion &expansion : zone_expansions)
            expansion.boundary_id += processed_expolygons_count;

        processed_expolygons_count += expansion_zone.expolygons.size();
        append(expansions, std::move(zone_expansions));
    }

    std::vector<ExPolygon> expanded = merge_expansions_into_expolygons(std::move(src), std::move(expansions));
    //NOTE: The current regularization of the shells can create small unasigned regions in the object (E.G. benchy)
    // without the following closing operation, those regions will stay unfilled and cause small holes in the expanded surface.
    // look for narrow_ensure_vertical_wall_thickness_region_radius filter.
    expanded = closing_ex(expanded, closing_radius);
    // Trim the zones by the expanded expolygons.
    for (ExpansionZone& expansion_zone : expansion_zones)
        if (expansion_zone.expanded_into)
            expansion_zone.expolygons = diff_ex(expansion_zone.expolygons, expanded);

    Surface templ{ surface_type, {} };
    templ.bridge_angle = bridge_angle;
    Surfaces out;
    out.reserve(expanded.size());
    for (auto &expoly : expanded)
        out.emplace_back(templ, std::move(expoly));
    return out;
}

void LayerRegion::process_external_surfaces(const Layer *lower_layer, const Polygons *lower_layer_covered)
{
    using namespace Slic3r::Algorithm;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // Width of the perimeters.
    float shell_width = 0;
    float expansion_min = 0;
    if (int num_perimeters = this->region().config().wall_loops; num_perimeters > 0) {
        Flow external_perimeter_flow = this->flow(frExternalPerimeter);
        Flow perimeter_flow          = this->flow(frPerimeter);
        shell_width  = 0.5f * external_perimeter_flow.scaled_width() + external_perimeter_flow.scaled_spacing();
        shell_width += perimeter_flow.scaled_spacing() * (num_perimeters - 1);
        expansion_min = perimeter_flow.scaled_spacing();
    } else {
        // TODO: Maybe there is better solution when printing with zero perimeters, but this works reasonably well, given the situation
        shell_width   = float(SCALED_EPSILON);
        expansion_min = float(SCALED_EPSILON);;
    }

    // Scaled expansions of the respective external surfaces.
    float                           expansion_top           = shell_width * sqrt(2.);
    float                           expansion_bottom        = expansion_top;
    // NEOTKO_BRIDGE_TAG — s230: "Bridging infill extra expansion". La expansión automática
    // de arriba nace de shell_width, o sea del NÚMERO DE PAREDES — subir wall_loops por
    // rigidez cambiaba de rebote el anclaje de todos los puentes. Esta clave suma milímetros
    // encima para desacoplar ambas cosas y permitir más agarre sin tocar las paredes.
    // Solo afecta al bridge: top y bottom normales se quedan como estaban.
    // Aditiva — con el default 0 la expresión es idéntica a la anterior.
    float                           expansion_bottom_bridge = expansion_top
        + scaled<float>(std::max(0., this->region().config().bridge_expansion_extra.value));
    // Expand by waves of expansion_step size (expansion_step is scaled), but with no more steps than max_nr_expansion_steps.
    const float                     expansion_step          = scaled<float>(0.1);
    // Don't take more than max_nr_steps for small expansion_step.
    static constexpr const size_t   max_nr_expansion_steps  = 5;
    // Radius (with added epsilon) to absorb empty regions emering from regularization of ensuring, viz  const float narrow_ensure_vertical_wall_thickness_region_radius = 0.5f * 0.65f * min_perimeter_infill_spacing;
    const float closing_radius = 0.55f * 0.65f * 1.05f * this->flow(frSolidInfill).scaled_spacing();

    // Expand the top / bottom / bridge surfaces into the shell thickness solid infills.
    double     layer_thickness;
    // NEOTKO_MULTIPASS_TAG_START: save penultimate surfaces before extraction/clear cycle —
    // fill_surfaces.clear() below wipes everything and only stInternalSolid/stInternal/bridges/
    // stBottom/stTop are rebuilt. stPenultimateInternalSolid must be preserved separately.
    ExPolygons penultimate_saved;
    for (Surface &s : this->fill_surfaces.surfaces)
        if (s.surface_type == stPenultimateInternalSolid)
            penultimate_saved.emplace_back(std::move(s.expolygon));
    // NEOTKO_MULTIPASS_TAG_END
    ExPolygons shells = union_ex(fill_surfaces_extract_expolygons(this->fill_surfaces.surfaces, { stInternalSolid }, layer_thickness));
    ExPolygons sparse = union_ex(fill_surfaces_extract_expolygons(this->fill_surfaces.surfaces, {stInternal}, layer_thickness));
    ExPolygons top_expolygons = union_ex(fill_surfaces_extract_expolygons(this->fill_surfaces.surfaces, {stTop}, layer_thickness));
    const auto expansion_params_into_sparse_infill = RegionExpansionParameters::build(expansion_min, expansion_step, max_nr_expansion_steps);
    const auto expansion_params_into_solid_infill  = RegionExpansionParameters::build(expansion_bottom_bridge, expansion_step, max_nr_expansion_steps);

    std::vector<ExpansionZone> expansion_zones{
        ExpansionZone{std::move(shells), expansion_params_into_solid_infill},
        ExpansionZone{std::move(sparse), expansion_params_into_sparse_infill},
        ExpansionZone{std::move(top_expolygons), expansion_params_into_solid_infill},
    };

    // NEOTKO_BRIDGE_TAG s230 — 4ª zona: stBottom, SOLO para la expansión de bridges y SOLO
    // cuando el usuario pidió expansión extra.
    //
    // Por qué hacía falta: las 3 zonas de arriba (sólido interno / disperso / top) no incluyen
    // stBottom, así que en el caso clásico "viga apoyada en dos bloques" (dolmen) el bridge NO
    // podía crecer sobre la zona apoyada de al lado — que es justo el mejor vecino posible,
    // sólido y con material debajo. Diagnóstico s230 con la traza BRIDGE_EXPAND:
    // zone_solid_mm2=0 en la capa del bridge pese a haber sólido a la vista → el vecino era
    // stBottom, un tipo que no estaba en la lista. Es exactamente lo que hace el "Bridging
    // Infill Extra Expansion" de Simplify3D: apropiarse del área contigua de la propia pieza,
    // de modo que la capa entera pueda imprimirse como un único bridge continuo (el usuario
    // lo usa así a propósito, con valores grandes, para eliminar el artefacto de la junta
    // entre la zona puenteada y la apoyada).
    //
    // Se añade solo con extra > 0 para mantener la promesa de "aditivo puro": con el default 0
    // la lista de zonas es la de siempre y el gcode es byte-idéntico.
    //
    // SIN TOPE, deliberadamente (S3D admite hasta 999 y ese es el uso real): el objetivo
    // legítimo es forzar la capa completa a bridge.
    //
    // La contabilidad la hace expand_merge_surfaces por su cuenta: recorta cada zona con lo
    // expandido (diff_ex(zone.expolygons, expanded)), así que el área que el bridge se lleve
    // desaparece del remanente. Ese remanente se devuelve a fill_surfaces como stBottom más
    // abajo — mismo patrón extraer→zona→devolver que el código ya usaba para stTop, así que
    // NO puede quedar stBottom y stBottomBridge solapados en la misma capa.
    const bool _bridge_extra_on = this->region().config().bridge_expansion_extra.value > 0.;
    if (_bridge_extra_on) {
        ExPolygons bottom_expolygons = union_ex(
            fill_surfaces_extract_expolygons(this->fill_surfaces.surfaces, { stBottom }, layer_thickness));
        expansion_zones.push_back(ExpansionZone{ std::move(bottom_expolygons), expansion_params_into_solid_infill });
    }

    SurfaceCollection bridges;
    {
        BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges. layer" << this->layer()->print_z;
        const double custom_angle = this->region().config().bridge_angle.value;

        // NEOTKO_BRIDGE_TAG s230 — traza de la expansión de bridges (canal BOTTOM,
        // ORCA_DEBUG_BOTTOM=1 → /tmp/neotko_bottom.log). Responde, en este orden, a las tres
        // preguntas que hay que hacerse cuando bridge_expansion_extra "no hace nada":
        //   1) extra_cfg  → ¿llegó el valor de la clave hasta aquí? (si es 0, es invalidación
        //                   o el preset, no el motor)
        //   2) exp_solid vs exp_sparse → cuánto se expande a CADA zona vecina. exp_sparse usa
        //                   expansion_min y NO lo toca la clave: si el puente solo linda con
        //                   relleno disperso, la expansión no cambia por diseño.
        //   3) zone_*_mm2 → cuánta área hay de cada vecino. Si solid=0, no hay a dónde crecer.
        //   4) bridges_before/after + área → el efecto real sobre la superficie de bridge.
        // NOTA: se usa NeoDebug::write directo, NO la macro NEOTKO_LOG — esa vive en
        // SurfaceColorMix.hpp, una cabecera pesada que este fichero no incluye ni necesita.
        int    _n_bridge_before = 0;
        double _a_bridge_before = 0.;
        const bool _dbg = NeoDebug::enabled(NeoDebug::BOTTOM);
        if (_dbg) {
            for (const Surface& s : this->fill_surfaces.surfaces)
                if (s.surface_type == stBottomBridge) { ++_n_bridge_before; _a_bridge_before += s.area(); }
            auto _mm2 = [](const ExPolygons& e) {
                double a = 0.; for (const auto& x : e) a += x.area();
                return unscale<double>(unscale<double>(a));
            };
            std::ostringstream _o;
            _o << "BRIDGE_EXPAND z=" << this->layer()->print_z
               << " extra_cfg=" << this->region().config().bridge_expansion_extra.value
               << " exp_solid_mm=" << unscale<double>(expansion_bottom_bridge)
               << " exp_sparse_mm=" << unscale<double>(expansion_min)
               << " zone_solid_mm2=" << _mm2(expansion_zones[0].expolygons)
               << " zone_sparse_mm2=" << _mm2(expansion_zones[1].expolygons)
               << " zone_top_mm2=" << _mm2(expansion_zones[2].expolygons)
               // s230 — 4ª zona (stBottom), solo presente con extra > 0. Es LA zona que importa
               // en el caso dolmen: si sale >0 y delta_mm2 se mueve, la expansión funciona.
               << " zone_bottom_mm2=" << (expansion_zones.size() > 3 ? _mm2(expansion_zones[3].expolygons) : -1.)
               << " bridges_before=" << _n_bridge_before
               << " area_before_mm2=" << unscale<double>(unscale<double>(_a_bridge_before));
            NeoDebug::write(NeoDebug::BOTTOM, _o.str());
        }

        bridges.surfaces = custom_angle > 0 ?
            expand_merge_surfaces(this->fill_surfaces.surfaces, stBottomBridge, expansion_zones, closing_radius, Geometry::deg2rad(custom_angle)) :
            expand_bridges_detect_orientations(this->fill_surfaces.surfaces, expansion_zones, closing_radius);

        if (_dbg) {
            double _a_after = 0.;
            for (const Surface& s : bridges.surfaces) _a_after += s.area();
            std::ostringstream _o;
            _o << "BRIDGE_EXPAND_DONE z=" << this->layer()->print_z
               << " bridges_after=" << bridges.surfaces.size()
               << " area_after_mm2=" << unscale<double>(unscale<double>(_a_after))
               << " delta_mm2=" << unscale<double>(unscale<double>(_a_after - _a_bridge_before));
            NeoDebug::write(NeoDebug::BOTTOM, _o.str());
        }
        BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges - done";
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        {
            static int iRun = 0;
            bridges.export_to_svg(debug_out_path("bridges-after-grouping-%d.svg", iRun++).c_str(), true);
        }
#endif
    }

    // NEOTKO_BRIDGE_TAG s230 — devolver PRIMERO el remanente de la zona stBottom (es la última
    // que se apiló, así que sale antes que la de stTop). Ya viene recortada por
    // expand_merge_surfaces con lo que el bridge se llevó, de modo que lo que se reinserta es
    // exactamente el bottom que NO pasó a ser puente. Sin este bloque el área extraída se
    // perdería (quedaría sin rellenar) — el mismo motivo por el que stTop tiene su bloque gemelo
    // justo debajo. Solo existe cuando la 4ª zona se creó (extra > 0).
    if (_bridge_extra_on) {
        this->fill_surfaces.remove_types({ stBottom });
        Surface bottom_templ(stBottom, {});
        bottom_templ.thickness = layer_thickness;
        this->fill_surfaces.append(std::move(expansion_zones.back().expolygons), bottom_templ);
        expansion_zones.pop_back();
    }

    this->fill_surfaces.remove_types({stTop});
    {
        Surface top_templ(stTop, {});
        top_templ.thickness = layer_thickness;
        this->fill_surfaces.append(std::move(expansion_zones.back().expolygons), top_templ);
    }

    expansion_zones.pop_back();

    expansion_zones.at(0).parameters = RegionExpansionParameters::build(expansion_bottom, expansion_step, max_nr_expansion_steps);
    Surfaces bottoms = expand_merge_surfaces(this->fill_surfaces.surfaces, stBottom, expansion_zones, closing_radius);

    expansion_zones.at(0).parameters = RegionExpansionParameters::build(expansion_top, expansion_step, max_nr_expansion_steps);
    Surfaces tops = expand_merge_surfaces(this->fill_surfaces.surfaces, stTop, expansion_zones, closing_radius);

    // turn too small internal regions into solid regions according to the user setting
    if (!this->layer()->object()->print()->config().spiral_mode && this->region().config().sparse_infill_density.value > 0) {
        // scaling an area requires two calls!
        double min_area = scale_(scale_(this->region().config().minimum_sparse_infill_area.value));
        ExPolygons small_regions{};
        expansion_zones[1].expolygons.erase(std::remove_if(expansion_zones[1].expolygons.begin(), expansion_zones[1].expolygons.end(), [min_area, &small_regions](ExPolygon& ex_polygon) {
            if (ex_polygon.area() <= min_area) {
                small_regions.push_back(ex_polygon);
                return true;
            }
            return false;
        }), expansion_zones[1].expolygons.end());

        if (!small_regions.empty()) {
            expansion_zones[0].expolygons = union_ex(expansion_zones[0].expolygons, small_regions);
        }
    }

//    this->fill_surfaces.remove_types({ stBottomBridge, stBottom, stTop, stInternal, stInternalSolid });
    this->fill_surfaces.clear();
    unsigned zones_expolygons_count = 0;
    for (const ExpansionZone& zone : expansion_zones)
        zones_expolygons_count += zone.expolygons.size();
    reserve_more(this->fill_surfaces.surfaces, zones_expolygons_count + bridges.size() + bottoms.size() + tops.size());
    {
        Surface solid_templ(stInternalSolid, {});
        solid_templ.thickness = layer_thickness;
        this->fill_surfaces.append(std::move(expansion_zones[0].expolygons), solid_templ);
    }
    {
        Surface sparse_templ(stInternal, {});
        sparse_templ.thickness = layer_thickness;
        this->fill_surfaces.append(std::move(expansion_zones[1].expolygons), sparse_templ);
    }
    this->fill_surfaces.append(std::move(bridges.surfaces));
    this->fill_surfaces.append(std::move(bottoms));
    this->fill_surfaces.append(std::move(tops));
    // NEOTKO_MULTIPASS_TAG_START: restore penultimate surfaces wiped by fill_surfaces.clear()
    if (!penultimate_saved.empty()) {
        Surface penultimate_templ(stPenultimateInternalSolid, {});
        // thickness=-1 means "use layer.height" — safe default used throughout the pipeline.
        // layer_thickness may be uninitialized if layer 5 has no stInternalSolid/stInternal/stTop.
        penultimate_templ.thickness = -1;
        this->fill_surfaces.append(std::move(penultimate_saved), penultimate_templ);
    }
    // NEOTKO_MULTIPASS_TAG_END

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}
#else

//#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 3.
//#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 1.5
#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

void LayerRegion::process_external_surfaces(const Layer *lower_layer, const Polygons *lower_layer_covered)
{
    const bool      has_infill = this->region().config().sparse_infill_density.value > 0.;
    //BBS
    auto nozzle_diameter = this->region().nozzle_dmr_avg(this->layer()->object()->print()->config());
    const float margin = float(scale_(EXTERNAL_INFILL_MARGIN));
    const float bridge_margin = std::min(float(scale_(BRIDGE_INFILL_MARGIN)), float(scale_(nozzle_diameter * BRIDGE_INFILL_MARGIN / 0.4)));

    // BBS
    const PrintObjectConfig& object_config = this->layer()->object()->config();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // 1) Collect bottom and bridge surfaces, each of them grown by a fixed 3mm offset
    // for better anchoring.
    // Bottom surfaces, grown.
    Surfaces                    bottom;
    // Bridge surfaces, initialy not grown.
    Surfaces                    bridges;
    // Top surfaces, grown.
    Surfaces                    top;
    // Internal surfaces, not grown.
    Surfaces                    internal;
    // Areas, where an infill of various types (top, bottom, bottom bride, sparse, void) could be placed.
    Polygons                    fill_boundaries = to_polygons(this->fill_expolygons);
    Polygons  					lower_layer_covered_tmp;

    // Collect top surfaces and internal surfaces.
    // Collect fill_boundaries: If we're slicing with no infill, we can't extend external surfaces over non-existent infill.
    // This loop destroys the surfaces (aliasing this->fill_surfaces.surfaces) by moving into top/internal/fill_boundaries!

    {
        // Voids are sparse infills if infill rate is zero.
        Polygons voids;

        double max_grid_area = -1;
        if (this->layer()->lower_layer != nullptr)
            max_grid_area = this->layer()->lower_layer->get_sparse_infill_max_void_area();
        for (const Surface &surface : this->fill_surfaces.surfaces) {
            if (surface.is_top()) {
                // Collect the top surfaces, inflate them and trim them by the bottom surfaces.
                // This gives the priority to bottom surfaces.
                if (max_grid_area < 0 || surface.expolygon.area() < max_grid_area)
                    surfaces_append(top, offset_ex(surface.expolygon, margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS), surface);
                else
                    //BBS: Don't need to expand too much in this situation. Expand 3mm to eliminate hole and 1mm for contour
                    surfaces_append(top, intersection_ex(offset(surface.expolygon.contour, margin / 3.0, EXTERNAL_SURFACES_OFFSET_PARAMETERS),
                                                         offset_ex(surface.expolygon, margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS)), surface);
            } else if (surface.surface_type == stBottom || (surface.surface_type == stBottomBridge && lower_layer == nullptr)) {
                // Grown by 3mm.
                surfaces_append(bottom, offset_ex(surface.expolygon, margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS), surface);
            } else if (surface.surface_type == stBottomBridge) {
                if (! surface.empty())
                    bridges.emplace_back(surface);
            }
            if (surface.is_internal()) {
            	assert(surface.surface_type == stInternal || surface.surface_type == stInternalSolid);
            	if (! has_infill && lower_layer != nullptr)
            		polygons_append(voids, surface.expolygon);
            	internal.emplace_back(std::move(surface));
            }
        }
        if (! has_infill && lower_layer != nullptr && ! voids.empty()) {
        	// Remove voids from fill_boundaries, that are not supported by the layer below.
            if (lower_layer_covered == nullptr) {
            	lower_layer_covered = &lower_layer_covered_tmp;
            	lower_layer_covered_tmp = to_polygons(lower_layer->lslices);
            }
            if (! lower_layer_covered->empty())
            	voids = diff(voids, *lower_layer_covered);
            fill_boundaries = diff(fill_boundaries, voids);
        }
    }

#if 0
    {
        static int iRun = 0;
        bridges.export_to_svg(debug_out_path("bridges-before-grouping-%d.svg", iRun ++), true);
    }
#endif

    if (bridges.empty())
    {
        fill_boundaries = union_safety_offset(fill_boundaries);
    } else
    {
        // 1) Calculate the inflated bridge regions, each constrained to its island.
        ExPolygons               fill_boundaries_ex = union_safety_offset_ex(fill_boundaries);
        std::vector<Polygons>    bridges_grown;
        std::vector<BoundingBox> bridge_bboxes;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        {
            static int iRun = 0;
            SVG svg(debug_out_path("3_process_external_surfaces-fill_regions-%d.svg", iRun ++).c_str(), get_extents(fill_boundaries_ex));
            svg.draw(fill_boundaries_ex);
            svg.draw_outline(fill_boundaries_ex, "black", "blue", scale_(0.05)); 
            svg.Close();
        }

//        export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
 
        {
            // Bridge expolygons, grown, to be tested for intersection with other bridge regions.
            std::vector<BoundingBox> fill_boundaries_ex_bboxes = get_extents_vector(fill_boundaries_ex);
            bridges_grown.reserve(bridges.size());
            bridge_bboxes.reserve(bridges.size());
            for (size_t i = 0; i < bridges.size(); ++ i) {
                // Find the island of this bridge.
                const Point pt = bridges[i].expolygon.contour.points.front();
                int idx_island = -1;
                for (int j = 0; j < int(fill_boundaries_ex.size()); ++ j)
                    if (fill_boundaries_ex_bboxes[j].contains(pt) && 
                        fill_boundaries_ex[j].contains(pt)) {
                        idx_island = j;
                        break;
                    }
                // Grown by 3mm.
                //BBS: eliminate too narrow area to avoid generating bridge on top layer when wall loop is 1
                //Polygons polys = offset(bridges[i].expolygon, bridge_margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                Polygons polys = offset2({ bridges[i].expolygon }, -scale_(nozzle_diameter * 0.1), bridge_margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                if (idx_island == -1) {
				    BOOST_LOG_TRIVIAL(trace) << "Bridge did not fall into the source region!";
                } else {
                    // Found an island, to which this bridge region belongs. Trim it,
                    polys = intersection(polys, fill_boundaries_ex[idx_island]);
                }
                bridge_bboxes.push_back(get_extents(polys));
                bridges_grown.push_back(std::move(polys));
            }
        }

        // 2) Group the bridge surfaces by overlaps.
        std::vector<size_t> bridge_group(bridges.size(), (size_t)-1);
        size_t n_groups = 0; 
        for (size_t i = 0; i < bridges.size(); ++ i) {
            // A grup id for this bridge.
            size_t group_id = (bridge_group[i] == size_t(-1)) ? (n_groups ++) : bridge_group[i];
            bridge_group[i] = group_id;
            // For all possibly overlaping bridges:
            for (size_t j = i + 1; j < bridges.size(); ++ j) {
                if (! bridge_bboxes[i].overlap(bridge_bboxes[j]))
                    continue;
                if (intersection(bridges_grown[i], bridges_grown[j]).empty())
                    continue;
                // The two bridge regions intersect. Give them the same group id.
                if (bridge_group[j] != size_t(-1)) {
                    // The j'th bridge has been merged with some other bridge before.
                    size_t group_id_new = bridge_group[j];
                    for (size_t k = 0; k < j; ++ k)
                        if (bridge_group[k] == group_id)
                            bridge_group[k] = group_id_new;
                    group_id = group_id_new;
                }
                bridge_group[j] = group_id;
            }
        }

        // 3) Merge the groups with the same group id, detect bridges.
        {
			BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges. layer" << this->layer()->print_z << ", bridge groups: " << n_groups;
            for (size_t group_id = 0; group_id < n_groups; ++ group_id) {
                size_t n_bridges_merged = 0;
                size_t idx_last = (size_t)-1;
                for (size_t i = 0; i < bridges.size(); ++ i) {
                    if (bridge_group[i] == group_id) {
                        ++ n_bridges_merged;
                        idx_last = i;
                    }
                }
                if (n_bridges_merged == 0)
                    // This group has no regions assigned as these were moved into another group.
                    continue;
                // Collect the initial ungrown regions and the grown polygons.
                ExPolygons  initial;
                Polygons    grown;
                for (size_t i = 0; i < bridges.size(); ++ i) {
                    if (bridge_group[i] != group_id)
                        continue;
                    initial.push_back(std::move(bridges[i].expolygon));
                    polygons_append(grown, bridges_grown[i]);
                }
                // detect bridge direction before merging grown surfaces otherwise adjacent bridges
                // would get merged into a single one while they need different directions
                // also, supply the original expolygon instead of the grown one, because in case
                // of very thin (but still working) anchors, the grown expolygon would go beyond them
                double custom_angle = Geometry::deg2rad(this->region().config().bridge_angle.value);
                if (custom_angle > 0.0) {
                    bridges[idx_last].bridge_angle = custom_angle;
                } else {
                    auto [bridging_dir, unsupported_dist] = detect_bridging_direction(to_polygons(initial), to_polygons(lower_layer->lslices));
                    bridges[idx_last].bridge_angle = PI + std::atan2(bridging_dir.y(), bridging_dir.x());
                }

                /*
                BridgeDetector bd(initial, lower_layer->lslices, this->bridging_flow(frInfill, object_config.thick_bridges).scaled_width());
                #ifdef SLIC3R_DEBUG
                printf("Processing bridge at layer %zu:\n", this->layer()->id());
                #endif
                //BBS: use 0 as custom angle to enable auto detection all the time
                double custom_angle = Geometry::deg2rad(this->region().config().bridge_angle.value);
                if(custom_angle > 0)
                        bridges[idx_last].bridge_angle = custom_angle;
				else if (bd.detect_angle(custom_angle)) {
                    bridges[idx_last].bridge_angle = bd.angle;
                    if (this->layer()->object()->has_support()) {
//                        polygons_append(this->bridged, bd.coverage());
                        append(this->unsupported_bridge_edges, bd.unsupported_edges());
                    }
				} else if (custom_angle > 0) {
					// Bridge was not detected (likely it is only supported at one side). Still it is a surface filled in
					// using a bridging flow, therefore it makes sense to respect the custom bridging direction.
					bridges[idx_last].bridge_angle = custom_angle;
				}
                */
                // without safety offset, artifacts are generated (GH #2494)
                surfaces_append(bottom, union_safety_offset_ex(grown), bridges[idx_last]);
            }

            fill_boundaries = to_polygons(fill_boundaries_ex);
			BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges - done";
		}

    #if 0
        {
            static int iRun = 0;
            bridges.export_to_svg(debug_out_path("bridges-after-grouping-%d.svg", iRun ++), true);
        }
    #endif
    }

    Surfaces new_surfaces;
    {
        // Merge top and bottom in a single collection.
        surfaces_append(top, std::move(bottom));
        // Intersect the grown surfaces with the actual fill boundaries.
        Polygons bottom_polygons = to_polygons(bottom);
        for (size_t i = 0; i < top.size(); ++ i) {
            Surface &s1 = top[i];
            if (s1.empty())
                continue;
            Polygons polys;
            polygons_append(polys, to_polygons(std::move(s1)));
            for (size_t j = i + 1; j < top.size(); ++ j) {
                Surface &s2 = top[j];
                if (! s2.empty() && surfaces_could_merge(s1, s2)) {
                    polygons_append(polys, to_polygons(std::move(s2)));
                    s2.clear();
                }
            }
            if (s1.is_top())
                // Trim the top surfaces by the bottom surfaces. This gives the priority to the bottom surfaces.
                polys = diff(polys, bottom_polygons);
            surfaces_append(
                new_surfaces,
                // Don't use a safety offset as fill_boundaries were already united using the safety offset.
                intersection_ex(polys, fill_boundaries),
                s1);
        }
    }
    
    // Subtract the new top surfaces from the other non-top surfaces and re-add them.
    Polygons new_polygons = to_polygons(new_surfaces);
    for (size_t i = 0; i < internal.size(); ++ i) {
        Surface &s1 = internal[i];
        if (s1.empty())
            continue;
        Polygons polys;
        polygons_append(polys, to_polygons(std::move(s1)));
        for (size_t j = i + 1; j < internal.size(); ++ j) {
            Surface &s2 = internal[j];
            if (! s2.empty() && surfaces_could_merge(s1, s2)) {
                polygons_append(polys, to_polygons(std::move(s2)));
                s2.clear();
            }
        }
        ExPolygons new_expolys = diff_ex(polys, new_polygons);
        polygons_append(new_polygons, to_polygons(new_expolys));
        surfaces_append(new_surfaces, std::move(new_expolys), s1);
    }
    
    this->fill_surfaces.surfaces = std::move(new_surfaces);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}
#endif

void LayerRegion::prepare_fill_surfaces()
{
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-initial");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */ 

    /*  Note: in order to make the psPrepareInfill step idempotent, we should never
        alter fill_surfaces boundaries on which our idempotency relies since that's
        the only meaningful information returned by psPerimeters. */
    
    bool spiral_mode = this->layer()->object()->print()->config().spiral_mode;

    // if no solid layers are requested, turn top/bottom surfaces to internal
    if (! spiral_mode && this->region().config().top_shell_layers == 0) {
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.is_top())
                //BBS
                //surface.surface_type = this->layer()->object()->config().infill_only_where_needed ? stInternalVoid : stInternal;
                surface.surface_type = PrintObject::infill_only_where_needed ? stInternalVoid : stInternal;
    }
    if (this->region().config().bottom_shell_layers == 0) {
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.is_bottom()) // (surface.surface_type == stBottom)
                surface.surface_type = stInternal;
    }

    if (!spiral_mode && fabs(this->region().config().sparse_infill_density.value - 100.) < EPSILON) {
        // Turn all internal sparse infill into solid infill, if sparse_infill_density is 100%
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.surface_type == stInternal)
                surface.surface_type = stInternalSolid;
    }

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-final");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}

double LayerRegion::infill_area_threshold() const
{
    double ss = this->flow(frSolidInfill).scaled_spacing();
    return ss*ss;
}

void LayerRegion::trim_surfaces(const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices.surfaces)
        assert(surface.surface_type == stInternal);
#endif /* NDEBUG */
	this->slices.set(intersection_ex(this->slices.surfaces, trimming_polygons), stInternal);
}

void LayerRegion::elephant_foot_compensation_step(const float elephant_foot_compensation_perimeter_step, const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices.surfaces)
        assert(surface.surface_type == stInternal);
#endif /* NDEBUG */
    Polygons tmp = intersection(this->slices.surfaces, trimming_polygons);
    append(tmp, diff(this->slices.surfaces, opening(this->slices.surfaces, elephant_foot_compensation_perimeter_step)));
    this->slices.set(union_ex(tmp), stInternal);
}

void LayerRegion::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (Surfaces::const_iterator surface = this->slices.surfaces.begin(); surface != this->slices.surfaces.end(); ++surface)
        bbox.merge(get_extents(surface->expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (Surfaces::const_iterator surface = this->slices.surfaces.begin(); surface != this->slices.surfaces.end(); ++surface)
        svg.draw(surface->expolygon, surface_type_to_color_name(surface->surface_type), transparency);
    for (Surfaces::const_iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
        svg.draw(surface->expolygon.lines(), surface_type_to_color_name(surface->surface_type));
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void LayerRegion::export_region_slices_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_slices_to_svg(debug_out_path("LayerRegion-slices-%s-%d.svg", name, idx ++).c_str());
}

void LayerRegion::export_region_fill_surfaces_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (Surfaces::const_iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
        bbox.merge(get_extents(surface->expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const Surface &surface : this->fill_surfaces.surfaces) {
        svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
        svg.draw_outline(surface.expolygon, "black", "blue", scale_(0.05)); 
    }
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void LayerRegion::export_region_fill_surfaces_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_fill_surfaces_to_svg(debug_out_path("LayerRegion-fill_surfaces-%s-%d.svg", name, idx ++).c_str());
}

void LayerRegion::simplify_entity_collection(ExtrusionEntityCollection* entity_collection)
{
    for (size_t i = 0; i < entity_collection->entities.size(); i++) {
        if (ExtrusionEntityCollection* collection = dynamic_cast<ExtrusionEntityCollection*>(entity_collection->entities[i]))
            this->simplify_entity_collection(collection);
        else if (ExtrusionPath* path = dynamic_cast<ExtrusionPath*>(entity_collection->entities[i]))
            this->simplify_path(path);
        else if (ExtrusionMultiPath* multipath = dynamic_cast<ExtrusionMultiPath*>(entity_collection->entities[i]))
            this->simplify_multi_path(multipath);
        else if (ExtrusionLoop* loop = dynamic_cast<ExtrusionLoop*>(entity_collection->entities[i]))
            this->simplify_loop(loop);
        else
            throw Slic3r::InvalidArgument("Invalid extrusion entity supplied to simplify_entity_collection()");
    }
}

void LayerRegion::simplify_path(ExtrusionPath* path)
{
    // NEOTKO_ZBUMP_TAG — Douglas-Peucker/arc-fitting only look at XY; the relief's per-vertex
    // points are often near-collinear in XY by construction (inserted along an originally
    // straight scanline), so either would silently collapse them back to a flat line, discarding
    // zbump_z_offset's Z data. See docs/WIP/ZBUMP_TOP_SURFACE_PLAN.md §7.3.
    if (!path->zbump_z_offset.empty())
        return;

    const auto print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    if (enable_arc_fitting &&
        !spiral_mode) {
        if (path->role() == erInternalInfill)
            path->simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
        else
            path->simplify_by_fitting_arc(scaled_resolution);
    } else {
        path->simplify(scaled_resolution);
    }
}

void LayerRegion::simplify_multi_path(ExtrusionMultiPath* multipath)
{
    const auto print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < multipath->paths.size(); ++i) {
        // NEOTKO_ZBUMP_TAG — see simplify_path() above for why relief-bumped sub-paths must
        // skip both simplification modes.
        if (!multipath->paths[i].zbump_z_offset.empty())
            continue;
        if (enable_arc_fitting &&
            !spiral_mode) {
            if (multipath->paths[i].role() == erInternalInfill)
                multipath->paths[i].simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
            else
                multipath->paths[i].simplify_by_fitting_arc(scaled_resolution);
        } else {
            multipath->paths[i].simplify(scaled_resolution);
        }
    }
}

void LayerRegion::simplify_loop(ExtrusionLoop* loop)
{
    const auto print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < loop->paths.size(); ++i) {
        // NEOTKO_ZBUMP_TAG — see simplify_path() above for why relief-bumped sub-paths must
        // skip both simplification modes.
        if (!loop->paths[i].zbump_z_offset.empty())
            continue;
        if (enable_arc_fitting &&
            !spiral_mode) {
            if (loop->paths[i].role() == erInternalInfill)
                loop->paths[i].simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
            else
                loop->paths[i].simplify_by_fitting_arc(scaled_resolution);
        } else {
            loop->paths[i].simplify(scaled_resolution);
        }
    }
}

}
 
