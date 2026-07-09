#ifndef slic3r_GCode_hpp_
#define slic3r_GCode_hpp_

#include "libslic3r.h"
#include "ExPolygon.hpp"
#include "GCodeWriter.hpp"
#include "Layer.hpp"
#include "Point.hpp"
#include "PlaceholderParser.hpp"
#include "PrintConfig.hpp"
#include "GCode/AvoidCrossingPerimeters.hpp"
#include "GCode/CoolingBuffer.hpp"
#include "GCode/FanMover.hpp"
#include "GCode/RetractWhenCrossingPerimeters.hpp"
#include "GCode/SpiralVase.hpp"
#include "GCode/ToolOrdering.hpp"
#include "GCode/WipeTower.hpp"
#include "NeoTower.hpp"        // NEOTKO_NEOTOWER_TAG
#include "NeoTowerZ.hpp"       // NEOTKO_NEOTOWER_TAG — Z epsilons for identity dispatch
#include "GCode/SeamPlacer.hpp"
#include "GCode/GCodeProcessor.hpp"
#include "EdgeGrid.hpp"
#include "GCode/ThumbnailData.hpp"
#include "libslic3r/ObjectID.hpp"
#include "GCode/ExtrusionProcessor.hpp"

#include "GCode/PressureEqualizer.hpp"
#include "GCode/SmallAreaInfillFlowCompensator.hpp"
// ORCA: post processor below used for Dynamic Pressure advance
#include "GCode/AdaptivePAProcessor.hpp"

#include <memory>
#include <map>
#include <set>
#include <string>
#include <cfloat>

namespace Slic3r {

// Forward declarations.
class GCode;

namespace { struct Item; }
struct PrintInstance;
class ConstPrintObjectPtrsAdaptor;
struct MultiPassSubLayer; // NEOTKO_MULTIPASS_TAG — defined in Print.hpp
struct PathBlendPassConfig; // NEOTKO_PATHBLEND_TAG — defined in SurfaceColorMix.hpp (pointer member only)

class OozePrevention {
public:
    bool enable;

    OozePrevention() : enable(false) {}
    std::string pre_toolchange(GCode &gcodegen);
    std::string post_toolchange(GCode &gcodegen);

private:
    int _get_temp(const GCode &gcodegen) const;
};

class Wipe {
public:
    bool enable;
    Polyline path;
    struct RetractionValues{
        double retractLengthBeforeWipe;
        double retractLengthDuringWipe;
    };

    Wipe() : enable(false) {}
    bool has_path() const { return !this->path.points.empty(); }
    void reset_path() { this->path = Polyline(); }
    std::string wipe(GCode &gcodegen, double length, bool toolchange = false, bool is_last = false);
    RetractionValues calculateWipeRetractionLengths(GCode& gcodegen, bool toolchange);
};

class WipeTowerIntegration {
public:
    WipeTowerIntegration(
        const PrintConfig                                           &print_config,
        // BBS: add partplate logic
        const int                                                    plate_idx,
        const Vec3d                                                  plate_origin,
        const std::vector<WipeTower::ToolChangeResult>              &priming,
        const std::vector<std::vector<WipeTower::ToolChangeResult>> &tool_changes,
        const std::vector<std::vector<WipeTower::ToolChangeResult>> &local_z_tool_changes,
        const std::vector<std::vector<WipeTower::box_coordinates>>  &local_z_reserve_boxes,
        const WipeTower::ToolChangeResult                           &final_purge) :
        m_left(/*float(print_config.wipe_tower_x.value)*/ 0.f),
        m_right(float(/*print_config.wipe_tower_x.value +*/ print_config.prime_tower_width.value)),
        m_wipe_tower_pos(float(print_config.wipe_tower_x.get_at(plate_idx)), float(print_config.wipe_tower_y.get_at(plate_idx))),
        m_wipe_tower_rotation(float(print_config.wipe_tower_rotation_angle)),
        m_extruder_offsets(print_config.extruder_offset.values),
        m_priming(priming),
        m_tool_changes(tool_changes),
        m_local_z_tool_changes(local_z_tool_changes),
        m_local_z_reserve_boxes(local_z_reserve_boxes),
        m_final_purge(final_purge),
        m_layer_idx(-1),
        m_tool_change_idx(0),
        m_local_z_tool_change_idx(local_z_tool_changes.size(), 0),
        m_local_z_reserve_slot_idx(local_z_reserve_boxes.size(), 0),
        m_plate_origin(plate_origin),
        m_single_extruder_multi_material(print_config.single_extruder_multi_material),
        m_enable_timelapse_print(print_config.timelapse_type.value == TimelapseType::tlSmooth),
        m_is_first_print(true)
    {}

    std::string prime(GCode &gcodegen);
    void next_layer() {
        ++ m_layer_idx;
        m_tool_change_idx = 0;
        if (m_layer_idx >= 0 && size_t(m_layer_idx) < m_local_z_tool_change_idx.size())
            m_local_z_tool_change_idx[size_t(m_layer_idx)] = 0;
        if (m_layer_idx >= 0 && size_t(m_layer_idx) < m_local_z_reserve_slot_idx.size())
            m_local_z_reserve_slot_idx[size_t(m_layer_idx)] = 0;
    }
    // NEOTKO_NEOTOWER_TAG_START — sync_to_z: z-aware plan layer lookup (replaces
    // next_layer() + suppress_finish_layer_if_future_layer() for NeoTower).
    // Root cause of structural desync: next_layer() increments m_layer_idx blindly,
    // but layers_to_print has more has_wipe_tower entries than wt2 plan entries
    // (sublayer/structural layers get has_wipe_tower=true via fill_wipe_tower_partitions
    // propagation). Fix: search m_tool_changes by z with Z_EPS_PLAN to find the correct
    // plan entry; when none matches, suppress finish_layer (structural-only layer).
    void sync_to_z(float nominal_print_z) {
        m_suppress_finish_layer = false;
        const int start = std::max(0, m_layer_idx);
        for (int i = start; i < (int)m_tool_changes.size(); ++i) {
            if (m_tool_changes[i].empty()) continue;
            const float pz = (float)m_tool_changes[i].front().print_z;
            if (std::abs(pz - nominal_print_z) <= NeoTowerZ::Z_EPS_PLAN) {
                m_layer_idx = i;
                m_tool_change_idx = 0;
                if (size_t(m_layer_idx) < m_local_z_tool_change_idx.size())
                    m_local_z_tool_change_idx[size_t(m_layer_idx)] = 0;
                if (size_t(m_layer_idx) < m_local_z_reserve_slot_idx.size())
                    m_local_z_reserve_slot_idx[size_t(m_layer_idx)] = 0;
                return;
            }
            if (pz > nominal_print_z + NeoTowerZ::Z_EPS_FUTURE_TC)
                break;  // no point scanning further
        }
        // No plan entry at this z → suppress finish_layer (empty structural layer).
        m_suppress_finish_layer = true;
    }
    // s79j — Bug 04 residual: emit orphan plan slots between the previously visited z
    // and `next_visited_z` (support air-gap layers exist in NeoTower's plan but are
    // absent from layers_to_print, so the normal pipeline never dispatches their TCRs).
    // Defined out-of-line in GCode.cpp to access append_tcr2.
    std::string emit_orphan_finish_layers_until_z(GCode& gcodegen, float next_visited_z);
    // NEOTKO_NEOTOWER_TAG_END

    // NEOTKO_MULTIPASS_TAG_START — hardening P4 (port s129).
    // Called when the MP group recovery block (P4) uses set_extruder() to
    // restore the printer to the expected initial tool of the current layer, bypassing
    // the normal WT toolchange path. If the WT plan's next pending TCR targets exactly
    // new_tool_id as its new_tool, advance m_tool_change_idx past it — otherwise the
    // next tool_change() call would attempt to re-do the same transition and mismatch.
    void skip_planned_toolchange_to(int new_tool_id) {
        if (m_layer_idx >= 0 && m_layer_idx < (int)m_tool_changes.size() &&
            (size_t)m_tool_change_idx < m_tool_changes[m_layer_idx].size()) {
            const auto& tc = m_tool_changes[m_layer_idx][m_tool_change_idx];
            // Only skip real tool transitions (initial→new where initial≠new).
            // Structural T→T fills must NOT be skipped — they carry NeoTower
            // structural finish_layer content and will be consumed by the
            // finish_layer call that follows the extruder_loop.
            if ((int)tc.new_tool == new_tool_id && (int)tc.initial_tool != (int)tc.new_tool)
                ++m_tool_change_idx;
        }
    }
    // NEOTKO_MULTIPASS_TAG_END

    std::string tool_change(GCode &gcodegen, int extruder_id, bool finish_layer,
                            bool local_z_unplanned = false,
                            double local_z_nominal_layer_z = -1.,
                            // NEOTKO_NEOTOWER_TAG s131 — sublayer disambiguation for the
                            // local_z_unplanned get_tcr() lookup. Sub-prime callers want the
                            // sublayer channel (true); real-layer recovery (BareRecover→PURGE)
                            // must use the real channel (false) so it picks the real-layer TCR
                            // that carries the structural drawer wall, not the µm-colliding
                            // sublayer purge.
                            bool local_z_sublayer_ctx = true);
    bool is_empty_wipe_tower_gcode(GCode &gcodegen, int extruder_id, bool finish_layer);
    std::string finalize(GCode &gcodegen);
    std::vector<float> used_filament_length() const;

    bool is_first_print() const { return m_is_first_print;}
    void set_is_first_print(bool is) { m_is_first_print = is; }

    bool enable_timelapse_print() const { return m_enable_timelapse_print; }

private:
    WipeTowerIntegration& operator=(const WipeTowerIntegration&);
    std::string append_tcr(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1.) const;
    std::string append_tcr2(GCode &gcodegen, const WipeTower::ToolChangeResult &tcr, int new_extruder_id, double z = -1.) const;

    // Postprocesses gcode: rotates and moves G1 extrusions and returns result
    std::string post_process_wipe_tower_moves(const WipeTower::ToolChangeResult& tcr, const Vec2f& translation, float angle) const;
    // Left / right edges of the wipe tower, for the planning of wipe moves.

    Vec2d extruder_offset_at(size_t extruder_id) const;

private:
    const float                                                  m_left;
    const float                                                  m_right;
    const Vec2f                                                  m_wipe_tower_pos;
    const float                                                  m_wipe_tower_rotation;
    const std::vector<Vec2d>                                     m_extruder_offsets;

    // Reference to cached values at the Printer class.
    const std::vector<WipeTower::ToolChangeResult>              &m_priming;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> &m_tool_changes;
    const std::vector<std::vector<WipeTower::ToolChangeResult>> &m_local_z_tool_changes;
    const std::vector<std::vector<WipeTower::box_coordinates>>  &m_local_z_reserve_boxes;
    const WipeTower::ToolChangeResult                           &m_final_purge;
    // Current layer index.
    int                                                          m_layer_idx;
    int                                                          m_tool_change_idx;
    std::vector<size_t>                                          m_local_z_tool_change_idx;
    std::vector<size_t>                                          m_local_z_reserve_slot_idx;
    double                                                       m_last_wipe_tower_print_z = 0.f;
    // NEOTKO_NEOTOWER_TAG — structural-layer sync (sync_to_z / orphan-emit). Inert
    // unless m_neo_tower is active (set at dispatch sites, increment 2).
    bool                                                         m_suppress_finish_layer = false;
    float                                                        m_orphan_floor_z = std::numeric_limits<float>::lowest();

    // BBS
    Vec3d                                                        m_plate_origin;
    bool                                                         m_single_extruder_multi_material;
    bool                                                         m_enable_timelapse_print;
    bool                                                         m_is_first_print;
};

class ColorPrintColors
{
    static const std::vector<std::string> Colors;
public:
    static const std::vector<std::string>& get() { return Colors; }
};

struct LayerResult {
    std::string gcode;
    size_t      layer_id;
    // Is spiral vase post processing enabled for this layer?
    bool        spiral_vase_enable { false };
    // Should the cooling buffer content be flushed at the end of this layer?
    bool        cooling_buffer_flush { false };
	// Is indicating if this LayerResult should be processed, or it is just inserted artificial LayerResult.
    // It is used for the pressure equalizer because it needs to buffer one layer back.
    bool        nop_layer_result { false };

    static LayerResult make_nop_layer_result() { return {"", std::numeric_limits<coord_t>::max(), false, false, true}; }
};

class GCode {

public:
    GCode() :
    	m_origin(Vec2d::Zero()),
        m_enable_loop_clipping(true),
        m_resonance_avoidance(true),
        m_enable_cooling_markers(false),
        m_enable_extrusion_role_markers(false),
        m_last_processor_extrusion_role(erNone),
        m_layer_count(0),
        m_layer_index(-1),
        m_layer(nullptr),
        m_object_layer_over_raft(false),
        //m_volumetric_speed(0),
        m_last_pos_defined(false),
        m_last_extrusion_role(erNone),
        m_last_width(0.0f),
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_last_mm3_per_mm(0.0),
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING
        m_brim_done(false),
        m_second_layer_things_done(false),
        m_silent_time_estimator_enabled(false),
        m_last_obj_copy(nullptr, Point(std::numeric_limits<coord_t>::max(), std::numeric_limits<coord_t>::max())),
        // BBS
        m_toolchange_count(0),
        m_nominal_z(0.)
        {}
    ~GCode() = default;

    // throws std::runtime_exception on error,
    // throws CanceledException through print->throw_if_canceled().
    void            do_export(Print* print, const char* path, GCodeProcessorResult* result = nullptr, ThumbnailsGeneratorCallback thumbnail_cb = nullptr);

    //BBS: set offset for gcode writer
    void set_gcode_offset(double x, double y) { m_writer.set_xy_offset(x, y); m_processor.set_xy_offset(x, y);}

    // Exported for the helper classes (OozePrevention, Wipe) and for the Perl binding for unit tests.
    const Vec2d&    origin() const { return m_origin; }
    void            set_origin(const Vec2d &pointf);
    void            set_origin(const coordf_t x, const coordf_t y) { this->set_origin(Vec2d(x, y)); }
    const Point&    last_pos() const { return m_last_pos; }
    Vec2d           point_to_gcode(const Point &point) const;
    Point           gcode_to_point(const Vec2d &point) const;
    Vec2d point_to_gcode_quantized(const Point& point) const;
    const FullPrintConfig &config() const { return m_config; }
    const Layer*    layer() const { return m_layer; }
    GCodeWriter&    writer() { return m_writer; }
    const GCodeWriter& writer() const { return m_writer; }
    PlaceholderParser& placeholder_parser() { return m_placeholder_parser_integration.parser; }
    const PlaceholderParser& placeholder_parser() const { return m_placeholder_parser_integration.parser; }
    // Process a template through the placeholder parser, collect error messages to be reported
    // inside the generated string and after the G-code export finishes.
    std::string     placeholder_parser_process(const std::string &name, const std::string &templ, unsigned int current_extruder_id, const DynamicConfig *config_override = nullptr);
    bool            enable_cooling_markers() const { return m_enable_cooling_markers; }
    // NEOTKO_NEOARACHNE_TAG Inc2a (port s134) — true when the most recently extruded path had
    // force_no_spiral_lift set (the marker NeoArachne puts on every emission). Read by
    // Wipe::calculateWipeRetractionLengths to rebalance retract_before_wipe for NeoArachne paths.
    bool            last_path_force_no_spiral_lift() const { return m_last_path_force_no_spiral_lift; }
    std::string     extrusion_role_to_string_for_parser(const ExtrusionRole &);

    // For Perl bindings, to be used exclusively by unit tests.
    unsigned int    layer_count() const { return m_layer_count; }
    void            set_layer_count(unsigned int value) { m_layer_count = value; }
    void            apply_print_config(const PrintConfig &print_config);

    std::string     travel_to(const Point& point, ExtrusionRole role, std::string comment, double z = DBL_MAX);
    bool            needs_retraction(const Polyline& travel, ExtrusionRole role, LiftType& lift_type);
    std::string     retract(bool toolchange = false, bool is_last_retraction = false, LiftType lift_type = LiftType::NormalLift, ExtrusionRole role = erNone);
    std::string     unretract() { return m_writer.unlift() + m_writer.unretract(); }
    std::string     set_extruder(unsigned int extruder_id, double print_z, bool by_object=false);
    bool is_BBL_Printer();

    // SoftFever
    std::string set_object_info(Print* print);

    // append full config to the given string
    static void append_full_config(const Print& print, std::string& str);

    // Object and support extrusions of the same PrintObject at the same print_z.
    // public, so that it could be accessed by free helper functions from GCode.cpp
    struct LayerToPrint
    {
        LayerToPrint() : object_layer(nullptr), support_layer(nullptr), original_object(nullptr),
                         mp_sublayer(nullptr), mp_object(nullptr), mp_layer_id(0), mp_print_z(0.) {}
        const Layer* 		object_layer;
        const SupportLayer* support_layer;
        const PrintObject*  original_object; //BBS: used for shared object logic
        // NEOTKO_MULTIPASS_TAG_START — virtual sublayer fields (MultiPassSubLayer forward-declared)
        const MultiPassSubLayer* mp_sublayer;  // non-null → this entry is a virtual MP sublayer
        const PrintObject*       mp_object;    // owning PrintObject for mp_sublayer
        size_t                   mp_layer_id;  // source layer index in multipass_sublayers()
        coordf_t                 mp_print_z;   // cached print_z (avoids dereferencing in header)
        // NEOTKO_MULTIPASS_TAG_END
        const Layer* 		layer()   const
        {
            if (object_layer != nullptr)
                return object_layer;

            if (support_layer != nullptr)
                return support_layer;

            return nullptr;
        }

        const PrintObject* 	object()   const
        {
            // NEOTKO_MULTIPASS_TAG_START
            if (mp_sublayer != nullptr) return mp_object;
            // NEOTKO_MULTIPASS_TAG_END
            return (this->layer() != nullptr) ? this->layer()->object() : nullptr;
        }
        coordf_t            print_z() const
        {
            // NEOTKO_MULTIPASS_TAG_START
            if (mp_sublayer != nullptr) return mp_print_z;  // cached, no dereference needed
            // NEOTKO_MULTIPASS_TAG_END
            coordf_t sum_z = 0.;
            size_t count = 0;
            if (object_layer != nullptr) {
                sum_z += object_layer->print_z;
                count++;
            }

            if (support_layer != nullptr) {
                sum_z += support_layer->print_z;
                count++;
            }

            return sum_z / count;
        }
    };

private:
    class GCodeOutputStream {
    public:
        GCodeOutputStream(FILE *f, GCodeProcessor &processor) : f(f), m_processor(processor) {}
        ~GCodeOutputStream() { this->close(); }

        bool is_open() const { return f; }
        bool is_error() const;

        void flush();
        void close();

        // Write a string into a file.
        void write(const std::string& what) { this->write(what.c_str()); }
        void write(const char* what);

        // Write a string into a file.
        // Add a newline, if the string does not end with a newline already.
        // Used to export a custom G-code section processed by the PlaceholderParser.
        void writeln(const std::string& what);

        // Formats and write into a file the given data.
        void write_format(const char* format, ...);

    private:
        FILE *f = nullptr;
        GCodeProcessor &m_processor;
    };
    void            _do_export(Print &print, GCodeOutputStream &file, ThumbnailsGeneratorCallback thumbnail_cb);

    static std::vector<LayerToPrint>        		                   collect_layers_to_print(const PrintObject &object);
    static std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>> collect_layers_to_print(const Print &print);

    std::string generate_skirt(const Print &print,
        const ExtrusionEntityCollection &skirt,
        const Point& offset,
        const float skirt_start_angle,
        const LayerTools &layer_tools,
        const Layer& layer,
        unsigned int extruder_id);

    LayerResult process_layer(
        const Print                     &print,
        // Set of object & print layers of the same PrintObject and with the same print_z.
        const std::vector<LayerToPrint> &layers,
        const LayerTools  				&layer_tools,
        const bool                       last_layer,
		// Pairs of PrintObject index and its instance index.
		const std::vector<const PrintInstance*> *ordering,
        // If set to size_t(-1), then print all copies of all objects.
        // Otherwise print a single copy of a single object.
        const size_t                     single_object_idx = size_t(-1),
        // BBS
        const bool                       prime_extruder = false);
    // Process all layers of all objects (non-sequential mode) with a parallel pipeline:
    // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
    // and export G-code into file.
    void process_layers(
        const Print                                                         &print,
        const ToolOrdering                                                  &tool_ordering,
        const std::vector<const PrintInstance*>                             &print_object_instances_ordering,
        const std::vector<std::pair<coordf_t, std::vector<LayerToPrint>>>   &layers_to_print,
        GCodeOutputStream                                                   &output_stream);
    // Process all layers of a single object instance (sequential mode) with a parallel pipeline:
    // Generate G-code, run the filters (vase mode, cooling buffer), run the G-code analyser
    // and export G-code into file.
    void process_layers(
        const Print                             &print,
        const ToolOrdering                      &tool_ordering,
        std::vector<LayerToPrint>                layers_to_print,
        const size_t                             single_object_idx,
        GCodeOutputStream                       &output_stream,
        // BBS
        const bool                               prime_extruder = false);

    //BBS
    void check_placeholder_parser_failed();

    void            set_last_pos(const Point &pos) { m_last_pos = pos; m_last_pos_defined = true; }
    bool            last_pos_defined() const { return m_last_pos_defined; }
    void            set_extruders(const std::vector<unsigned int> &extruder_ids);
    std::string     preamble();
    // BBS
    std::string     change_layer(coordf_t print_z);
    // Orca: pass the complete collection of region perimeters to the extrude loop to check whether the wipe before external loop
    // should be executed
    std::string     extrude_entity(const ExtrusionEntity &entity, std::string description = "", double speed = -1., const ExtrusionEntitiesPtr& region_perimeters = ExtrusionEntitiesPtr());
    // Orca: pass the complete collection of region perimeters to the extrude loop to check whether the wipe before external loop
    // should be executed
    std::string     extrude_loop(ExtrusionLoop loop, std::string description, double speed = -1., const ExtrusionEntitiesPtr& region_perimeters = ExtrusionEntitiesPtr(), const Point* start_point = nullptr);
    std::string     extrude_multi_path(ExtrusionMultiPath multipath, std::string description = "", double speed = -1.);
    std::string     extrude_path(ExtrusionPath path, std::string description = "", double speed = -1.);
    
    // Orca: Adaptive PA variables
    // Used for adaptive PA when extruding paths with multiple, varying flow segments.
    // This contains the sum of the mm3_per_mm values weighted by the length of each path segment.
    // The m_multi_flow_segment_path_pa_set constrains the PA change request to the first extrusion segment.
    // It sets the mm3_mm value for the adaptive PA post processor to be the average of that path
    // as calculated and stored in the m_multi_segment_path_average_mm3_per_mm value
    double          m_multi_flow_segment_path_average_mm3_per_mm = 0;
    bool            m_multi_flow_segment_path_pa_set = false;
    // Adaptive PA last set flow to enable issuing of PA change commands when adaptive PA for overhangs
    // is enabled
    double          m_last_mm3_mm = 0;
    // Orca: Adaptive PA code segment end

    // Extruding multiple objects with soluble / non-soluble / combined supports
    // on a multi-material printer, trying to minimize tool switches.
    // Following structures sort extrusions by the extruder ID, by an order of objects and object islands.
    struct ObjectByExtruder
    {
        ObjectByExtruder() : support(nullptr), support_extrusion_role(erNone) {}
        const ExtrusionEntityCollection  *support;
        // erSupportMaterial / erSupportMaterialInterface / erSupportTransition or erMixed.
        ExtrusionRole                     support_extrusion_role;

        struct Island
        {
            struct Region {
            	// Non-owned references to LayerRegion::perimeters::entities
            	// std::vector<const ExtrusionEntity*> would be better here, but there is no way in C++ to convert from std::vector<T*> std::vector<const T*> without copying.
                ExtrusionEntitiesPtr perimeters;
            	// Non-owned references to LayerRegion::fills::entities
                ExtrusionEntitiesPtr infills;

                std::vector<const WipingExtrusions::ExtruderPerCopy*> infills_overrides;
                std::vector<const WipingExtrusions::ExtruderPerCopy*> perimeters_overrides;

	            enum Type {
	            	PERIMETERS,
	            	INFILL,
	            };

                // Appends perimeter/infill entities and writes don't indices of those that are not to be extruder as part of perimeter/infill wiping
                void append(const Type type, const ExtrusionEntityCollection* eec, const WipingExtrusions::ExtruderPerCopy* copy_extruders);
            };


            std::vector<Region> by_region;                                    // all extrusions for this island, grouped by regions

            // Fills in by_region_per_copy_cache and returns its reference.
            const std::vector<Region>& by_region_per_copy(std::vector<Region> &by_region_per_copy_cache, unsigned int copy, unsigned int extruder, bool wiping_entities = false) const;
        };
        std::vector<Island>         islands;
    };

	struct InstanceToPrint
	{
		InstanceToPrint(ObjectByExtruder &object_by_extruder, size_t layer_id, const PrintObject &print_object, size_t instance_id, size_t label_object_id) :
			object_by_extruder(object_by_extruder), layer_id(layer_id), print_object(print_object), instance_id(instance_id), label_object_id(label_object_id) {}

		// Repository
		ObjectByExtruder		&object_by_extruder;
		// Index into std::vector<LayerToPrint>, which contains Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
		const size_t       		 layer_id;
		const PrintObject 		&print_object;
		// Instance idx of the copy of a print object.
		const size_t			 instance_id;
        //BBS: Unique id to label object to support skiping during printing
        const size_t             label_object_id;
	};

	std::vector<InstanceToPrint> sort_print_object_instances(
		std::vector<ObjectByExtruder> 					&objects_by_extruder,
		// Object and Support layers for the current print_z, collected for a single object, or for possibly multiple objects with multiple instances.
		const std::vector<LayerToPrint> 				&layers,
		// Ordering must be defined for normal (non-sequential print).
		const std::vector<const PrintInstance*>     	*ordering,
		// For sequential print, the instance of the object to be printing has to be defined.
		const size_t                     				 single_object_instance_idx);

    std::string     extrude_perimeters(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region, bool is_first_layer, bool is_infill_first);
    std::string     extrude_infill(const Print& print, const std::vector<ObjectByExtruder::Island::Region>& by_region, bool ironing);
    std::string     extrude_support(const ExtrusionEntityCollection& support_fills, const ExtrusionRole support_extrusion_role);

    // BBS
    LiftType to_lift_type(ZHopType z_hop_types);

    std::set<ObjectID>              m_objsWithBrim; // indicates the objs with brim
    std::set<ObjectID>              m_objSupportsWithBrim; // indicates the objs' supports with brim
    // Cache for custom seam enforcers/blockers for each layer.
    SeamPlacer                          m_seam_placer;

    ExtrusionQualityEstimator m_extrusion_quality_estimator;


    /* Origin of print coordinates expressed in unscaled G-code coordinates.
       This affects the input arguments supplied to the extrude*() and travel_to()
       methods. */
    Vec2d                               m_origin;
    FullPrintConfig                     m_config;
    DynamicConfig                       m_calib_config;
    // scaled G-code resolution
    double                              m_scaled_resolution;
    GCodeWriter                         m_writer;

    struct PlaceholderParserIntegration {
        void reset();
        void init(const GCodeWriter &config);
        void update_from_gcodewriter(const GCodeWriter &writer);
        void validate_output_vector_variables();

        PlaceholderParser                   parser;
        // For random number generator etc.
        PlaceholderParser::ContextData      context;
        // Collection of templates, on which the placeholder substitution failed.
        std::map<std::string, std::string>  failed_templates;
        // Input/output from/to custom G-code block, for returning position, retraction etc.
        DynamicConfig                       output_config;
        ConfigOptionFloats                 *opt_position { nullptr };
        ConfigOptionFloat                  *opt_zhop { nullptr };
        ConfigOptionFloats                 *opt_e_position { nullptr };
        ConfigOptionFloats                 *opt_e_retracted { nullptr };
        ConfigOptionFloats                 *opt_e_restart_extra { nullptr };
        ConfigOptionFloats                 *opt_extruded_volume { nullptr };
        ConfigOptionFloats                 *opt_extruded_weight { nullptr };
        ConfigOptionFloat                  *opt_extruded_volume_total { nullptr };
        ConfigOptionFloat                  *opt_extruded_weight_total { nullptr };
        // Caches of the data passed to the script.
        size_t                              num_extruders;
        std::vector<double>                 position;
        std::vector<double>                 e_position;
        std::vector<double>                 e_retracted;
        std::vector<double>                 e_restart_extra;
    } m_placeholder_parser_integration;

    OozePrevention                      m_ooze_prevention;
    Wipe                                m_wipe;
    AvoidCrossingPerimeters             m_avoid_crossing_perimeters;
    RetractWhenCrossingPerimeters       m_retract_when_crossing_perimeters;
    bool                                m_enable_loop_clipping;
    //resonance avoidance
    bool                                m_resonance_avoidance; 
    // If enabled, the G-code generator will put following comments at the ends
    // of the G-code lines: _EXTRUDE_SET_SPEED, _WIPE, _OVERHANG_FAN_START, _OVERHANG_FAN_END
    // Those comments are received and consumed (removed from the G-code) by the CoolingBuffer.pm Perl module.
    bool                                m_enable_cooling_markers;
    
    bool m_enable_exclude_object;
    std::vector<size_t> m_label_objects_ids;
    std::string _encode_label_ids_to_base64(std::vector<size_t> ids);
    // ORCA: Add support for role based fan speed control
    std::array<bool, ExtrusionRole::erCount> m_is_role_based_fan_on;
    // Markers for the Pressure Equalizer to recognize the extrusion type.
    // The Pressure Equalizer removes the markers from the final G-code.
    bool                                m_enable_extrusion_role_markers;
    // Keeps track of the last extrusion role passed to the processor
    ExtrusionRole                       m_last_processor_extrusion_role;
    // NEOTKO_NEOARACHNE_TAG Inc2a (port s134) — tracks ExtrusionPath::force_no_spiral_lift of the
    // most recently extruded path. Read in needs_retraction() to downgrade SpiralLift → LazyLift on
    // travel-after-NeoArachne, and in the Wipe pre-retract rebalance. Reset by any non-flagged path.
    bool                                m_last_path_force_no_spiral_lift = false;
    // How many times will change_layer() be called?
    // change_layer() will update the progress bar.
    unsigned int                        m_layer_count;
    // Progress bar indicator. Increments from -1 up to layer_count.
    int                                 m_layer_index;
    // Current layer processed. In sequential printing mode, only a single copy will be printed.
    // In non-sequential mode, all its copies will be printed.
    const Layer*                        m_layer;
    // m_layer is an object layer and it is being printed over raft surface.
    bool                                m_object_layer_over_raft;
    //double                              m_volumetric_speed;
    // Support for the extrusion role markers. Which marker is active?
    ExtrusionRole                       m_last_extrusion_role;
    // To ignore gapfill role for retract_lift_enforce
    ExtrusionRole                       m_last_notgapfill_extrusion_role;
    // Support for G-Code Processor
    float                               m_last_height{ 0.0f };
    float                               m_last_layer_z{ 0.0f };
    float                               m_max_layer_z{ 0.0f };
    float                               m_last_width{ 0.0f };

    // SM_Orca
    float                               m_next_wipe_x {0.0f};
    float                               m_next_wipe_y {0.0f};
#if ENABLE_GCODE_VIEWER_DATA_CHECKING
    double                              m_last_mm3_per_mm;
#endif // ENABLE_GCODE_VIEWER_DATA_CHECKING

    // Always check gcode placeholders when building in debug mode.
#if !defined(NDEBUG)
#define ORCA_CHECK_GCODE_PLACEHOLDERS 1
#endif
    
#if ORCA_CHECK_GCODE_PLACEHOLDERS
    std::map<std::string, std::vector<std::string>> m_placeholder_error_messages;
#endif

    Point                               m_last_pos;
    bool                                m_last_pos_defined;

    std::unique_ptr<CoolingBuffer>      m_cooling_buffer;
    std::unique_ptr<SpiralVase>         m_spiral_vase;

    std::unique_ptr<PressureEqualizer>  m_pressure_equalizer;
    
    std::unique_ptr<AdaptivePAProcessor>      m_pa_processor;

    std::unique_ptr<WipeTowerIntegration> m_wipe_tower;
    // NEOTKO_NEOTOWER_TAG — non-owning; NeoTower instance owned by Print::m_neo_tower.
    // Bound at the m_wipe_tower.reset() site; nullptr when neotko_tower_type==Classic
    // (all NeoTower dispatch branches gated on it → stock tower byte-identical).
    NeoTower*                             m_neo_tower = nullptr;

    std::unique_ptr<SmallAreaInfillFlowCompensator> m_small_area_infill_flow_compensator;
    
    // Heights (print_z) at which the skirt has already been extruded.
    std::vector<coordf_t>               m_skirt_done;
    // Has the brim been extruded already? Brim is being extruded only for the first object of a multi-object print.
    bool                                m_brim_done;
    // Flag indicating whether the nozzle temperature changes from 1st to 2nd layer were performed.
    bool                                m_second_layer_things_done;
    // Index of a last object copy extruded.
    std::pair<const PrintObject*, Point> m_last_obj_copy;

    int m_timelapse_warning_code = 0;
    bool m_support_traditional_timelapse = true;

    bool m_silent_time_estimator_enabled;

    // Processor
    GCodeProcessor m_processor;

    //some post-processing on the file, with their data class
    std::unique_ptr<FanMover> m_fan_mover;

    // BBS
    Print* m_curr_print = nullptr;
    unsigned int m_toolchange_count;
    coordf_t m_nominal_z;
    bool m_need_change_layer_lift_z = false;
    // NEOTKO_MULTIPASS_TAG_START — hardening P4
    // MultiPass group lifecycle. Replaces m_after_mp_sublayer + m_mp_sublayer_tools.
    //
    // Lifecycle per nominal layer with MP sublayers:
    //   mp_group.begin(z_nominal)
    //     ├── mp_group.add_pass(tool) × N
    //     └── mp_group.end(expected_initial, needs_tc) → Action
    struct MpGroupState {
        enum class Action { NoAction, BareRecover, WipeTowerPurge };

        bool active() const { return m_active; }
        bool was_active() const { return m_was_active_last_layer; }

        void begin(float z_nominal) {
            m_active = true;
            m_tools.clear();
            m_z_nominal = z_nominal;
        }

        void add_pass(unsigned int tool) {
            m_active = true;
            m_tools.insert(tool);
        }

        Action end(unsigned int expected_initial, bool writer_needs_toolchange) {
            Action result;
            if (!writer_needs_toolchange) {
                result = Action::NoAction;
            } else if (m_tools.count(expected_initial) > 0) {
                result = Action::BareRecover;
            } else {
                result = Action::WipeTowerPurge;
            }
            m_was_active_last_layer = m_active;
            m_active = false;
            return result;
        }

        const std::set<unsigned int>& tools() const { return m_tools; }
        float z_nominal() const { return m_z_nominal; }

    private:
        bool m_active = false;
        bool m_was_active_last_layer = false;
        std::set<unsigned int> m_tools;
        float m_z_nominal = 0.f;
    };

    MpGroupState m_mp_group;
    // NEOTKO_MULTIPASS_TAG_END
    // NEOTKO_PATHBLEND_TAG_START — per-surface Y bbox for correct surface_t normalisation.
    // Set in extrude_infill() before iterating each EEC; reset afterwards.
    // When defined, _extrude() uses this instead of the full layer bbox so that
    // objects placed anywhere on the build plate get the full [0..1] gradient range.
    BoundingBox m_pathblend_surface_bbox;

    // NEOTKO_COLORMIX_TAG — s58 per-path pre-computed surface_t for PathBlend.
    // Populated by extrude_infill() before iterating an EEC when
    // surface_color_mix_lane_mode != Default.  Each PathBlend-eligible path gets
    // a t in [0..1] computed according to the chosen lane mode.
    std::map<const ExtrusionPath*, double> m_pathblend_path_t;

    // NEOTKO_PATHBLEND_TAG — s59 path-pointer mismatch fix. Key the map by a stable
    // signature derived from the polyline values (first point + last point + size),
    // because extrude_path receives a LOCAL COPY of the path (address changes, values survive).
    std::map<uint64_t, double> m_pathblend_polyline_t;

    // NEOTKO_PATHBLEND_TAG — s58 Bug 2 safety: max-z reached per (layer, pass).
    // Clamp z so the nozzle never descends within a pass. Reset every real layer.
    std::map<int, double> m_pathblend_max_z_per_pass;

    // NEOTKO_PATHBLEND_TAG — Fase 5 s77: PathBlend-as-sublayer dispatch context.
    // Set by the sublayer dispatch (process_layer) before calling extrude_entity;
    // extrude_path's PB branch fires on m_pb_sub_pass >= 0. Cleared after each sub.
    const PathBlendPassConfig* m_pb_sub_cfg          = nullptr; // points to a dispatch-local pb
    int                        m_pb_sub_pass         = -1;      // 0 = ramp, 1 = cap
    ExtrusionRole              m_pb_sub_role         = erTopSolidInfill;
    double                     m_pb_sub_nominal_z    = 0.0;
    double                     m_pb_sub_layer_height = 0.0;
    // NEOTKO_PATHBLEND_TAG — s88 continuous chain: track prev PB sublayer tool + last XY.
    int                        m_pb_chain_prev_tool  = -1;
    Vec2d                      m_pb_chain_prev_xy    = Vec2d::Zero();

    // NEOTKO_ZBUMP_TAG — same sublayer-dispatch-context pattern as m_pb_sub_cfg above, own single
    // bool: ZBump reinforcement passes (Pass 2..N) reuse Pass 1's ORIGINAL ExtrusionPath's
    // .height/.mm3_per_mm verbatim (apply_zbump_reinforcement_passes(), ZBump.cpp) since they have
    // no "normal" top-surface content of their own -- every bit of material they deposit is on
    // top of what Pass 1 (and earlier reinforcement passes) already printed. _extrude()'s
    // has_zbump_relief branch needs to know this to pick a zero-baseline flow formula instead of
    // Pass 1's baseline-plus-extra one (real print bug: reinforcement passes over-extruded a full
    // nominal layer's worth at EVERY point, confirmed via gcode E values matching Pass 1's own
    // unbumped baseline rate even at z_diff=0, where a reinforcement point needs zero extra
    // material). Set by process_layer()'s sublayer dispatch before calling extrude_entity, same
    // as m_pb_sub_cfg; cleared after each sub.
    bool                       m_zbump_reinforcement_pass = false;
    // NEOTKO_PATHBLEND_TAG_END
    int m_start_gcode_filament = -1;

    std::set<unsigned int>                  m_initial_layer_extruders;
    // BBS
    int get_bed_temperature(const int extruder_id, const bool is_first_layer, const BedType bed_type) const;

    std::string _extrude(const ExtrusionPath &path, std::string description = "", double speed = -1);
    bool _needSAFC(const ExtrusionPath &path);
    void print_machine_envelope(GCodeOutputStream &file, Print &print);
    void _print_first_layer_bed_temperature(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    void _print_first_layer_extruder_temperatures(GCodeOutputStream &file, Print &print, const std::string &gcode, unsigned int first_printing_extruder_id, bool wait);
    // On the first printing layer. This flag triggers first layer speeds.
    //BBS
    bool    on_first_layer() const { return m_layer != nullptr && m_layer->id() == 0 && abs(m_layer->bottom_z()) < EPSILON; }
    int layer_id() const {
        if (m_layer == nullptr)
            return -1;
        return m_layer->id();
    }
    // To control print speed of 1st object layer over raft interface.
    bool                                object_layer_over_raft() const { return m_object_layer_over_raft; }

    friend ObjectByExtruder& object_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>> &by_extruder,
        unsigned int                                           extruder_id,
        size_t                                                 object_idx,
        size_t                                                 num_objects);
    friend std::vector<ObjectByExtruder::Island>& object_islands_by_extruder(
        std::map<unsigned int, std::vector<ObjectByExtruder>>  &by_extruder,
        unsigned int                                            extruder_id,
        size_t                                                  object_idx,
        size_t                                                  num_objects,
        size_t                                                  num_islands);

    // NEOTKO_MPSCHEDULER_TAG s79e (port s129) — suppression-aware print classifier.
    // Single source of truth for MP_RECOVERY_DIAG / BareRecover→PURGE / BUG02_FIX.
    // Static members so they can name ObjectByExtruder (private nested type).
    // obj_perim_suppressed is a per-LAYER aggregate done per-OBJECT: the per-layer
    // flag mp_perim_override_active is true if ANY object has sublayers, so applying
    // it to all objects would mis-classify non-sandwich objects' perimeters as
    // suppressed and drop their output in mixed scenes.
    static int mp_tool_emits(unsigned int                                                   tid,
                             const std::map<unsigned int, std::vector<ObjectByExtruder>>&   by_extruder,
                             const std::vector<bool>&                                       obj_perim_suppressed);
    static int mp_first_printing(const std::vector<unsigned int>&                           layer_extruders,
                                 const std::map<unsigned int, std::vector<ObjectByExtruder>>& by_extruder,
                                 const std::vector<bool>&                                   obj_perim_suppressed);

    friend class Wipe;
    friend class WipeTowerIntegration;
    friend class PressureEqualizer;
    friend class Print;
    friend class SmallAreaInfillFlowCompensator;
};

std::vector<const PrintInstance*> sort_object_instances_by_model_order(const Print& print, bool init_order = false);

}

#endif
