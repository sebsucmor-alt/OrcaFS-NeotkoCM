#include <assert.h>
#include <stdio.h>
#include <memory>

#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../Surface.hpp"

#include "ExtrusionEntity.hpp"
#include "FillBase.hpp"
#include "FillRectilinear.hpp"
#include "FillLightning.hpp"
#include "FillConcentricInternal.hpp"
#include "FillTpmsD.hpp"
#include "FillTpmsFK.hpp"
#include "FillConcentric.hpp"
#include "libslic3r.h"
// NEOTKO_COLORMIX_TAG_START
#include "../SurfaceColorMix.hpp"
#include "../SurfaceEffectProfile.hpp" // NEOTKO_PROFILE_TAG — Fase F painter-mode MP override
// NEOTKO_COLORMIX_TAG_END

namespace Slic3r {

// Calculate infill rotation angle (in radians) for a given layer from a rotation template.
// Grammar subset handled (rotation only):
//   [±]α[*Z or !][joint][-][N|B|T][length][* or !]
//   [±]α*                    sets an initial angle only (no layer processed)
// Where:
// - α: angle in degrees. Without a sign it's absolute; with +/− it's relative. α% means a percentage of 360°.
// - Runtime: *Z repeats the instruction Z times; bare * is a no-op used for initialization; ! runs once globally and then stops.
// - Solid signs (D,S,O,M,R) are not processed here; if present they are treated as invalid/non-rotation characters.
// - Joint signs (shape of the turn across a range):
//     / linear;
//     N,n vertical sinus (n = lazy/half amplitude);
//     Z,z horizontal sinus (z = lazy/half amplitude);
//     $ arcsin; L quarter circle H→V; l quarter circle V→H;
//     U,u squared; Q,q cubic; ~ random; ^ pseudorandom; | middle step; # vertical step at end.
// - Counting / range length:
//     After the joint (or after α) a count determines duration of the turn:
//       N = layer count, B = bottom_shell_layers, T = top_shell_layers.
//     Prefix '-' flips the joint (swap initial/final orientation).
// - Length modifiers convert the count to a Z range instead of a pure layer count:
//     mm, cm, m, ' (feet), " (inches), # (standard height of N layers), % (percent of model height).
//
// Behavior:
// - The template string is tokenized by commas/whitespace and evaluated cyclically with one or more "ranges" per token.
// - Absolute α resets the accumulated angle at the start of its range; relative α accumulates.
// - *Z and ! control repetition and one-time execution of tokens across layers.
// - If the template contains no metalanguage symbols, it is treated as a simple comma-separated list of angles repeated by modulo.
// - Returns angle in radians for the requested layer_id. 0° aligns with +X; fillers may internally rotate as needed.
double calculate_infill_rotation_angle(const PrintObject* object,
                                       size_t             layer_id,
                                       const double&      fixed_infill_angle,
                                       const std::string& template_string)
{
    if (template_string.empty()) {
        return Geometry::deg2rad(fixed_infill_angle);
    }
    double             angle = 0.0;
    ConfigOptionFloats rotate_angles;
    const std::string  search_string = "/NnZz$LlUuQq~^|#";
    if (regex_search(template_string, std::regex("[+\\-%*@\'\"cm" + search_string + "]"))) { // template metalanguage of rotating infill
        std::regex                 del("[\\s,]+");
        std::sregex_token_iterator it(template_string.begin(), template_string.end(), del, -1);
        std::vector<std::string>   tk;
        std::sregex_token_iterator end;
        while (it != end) {
            tk.push_back(*it++);
        }
        int    t            = 0;
        int    repeats      = 0;
        double angle_add    = 0;
        double angle_steps  = 1;
        double angle_start  = 0;
        double limit_fill_z = object->get_layer(0)->bottom_z();
        double start_fill_z = limit_fill_z;
        bool   _noop        = false;
        auto              fill_form = std::string::npos;
        bool              _absolute = false;
        bool              _negative = false;
        std::vector<bool> stop(tk.size(), false);

        for (int i = 0; i <= layer_id; i++) {
            double fill_z = object->get_layer(i)->bottom_z();

            if (limit_fill_z < object->get_layer(i)->slice_z) {
                if (repeats) { // if repeats >0 then restore parameters for new iteration
                    limit_fill_z += limit_fill_z - start_fill_z;
                    start_fill_z = fill_z;
                    repeats--;
                } else {
                    start_fill_z = fill_z;
                    limit_fill_z = object->get_layer(i)->print_z;
                    // Solid handling removed: this function only computes rotation.
                    fill_form    = std::string::npos;
                    do {
                        if (!stop[t]) {
                            _noop     = false;
                            _absolute = false;
                            _negative = false;
                            angle_start += angle_add;
                            angle_add   = 0;
                            angle_steps = 1;
                            repeats     = 1;
                            if (tk[t].find('!') != std::string::npos) // this is an one-time instruction
                                stop[t] = true;

                            char* cs = &tk[t][0];

                            if ((cs[0] >= '0' && cs[0] <= '9') && !(cs[0] == '+' || cs[0] == '-')) // absolute/relative
                                _absolute = true;

                            angle_add = strtod(cs, &cs); // read angle parameter

                            if (cs[0] == '%') { // percentage of angles
                                angle_add *= 3.6;
                                cs = &cs[1];
                            }

                            int tit = tk[t].find('*');
                            if (tit != std::string::npos) // overall angle_cycles
                                repeats = strtol(&tk[t][tit + 1], &cs, 0);

                            if (repeats) {                                // run if overall cycles greater than 0
                                // Solid signs (D,S,O,M,R) are not handled here; if present they behave as invalid characters.

                                if (cs[0] == 'B') {
                                    angle_steps = object->print()->default_region_config().bottom_shell_layers.value;
                                } else if (cs[0] == 'T') {
                                    angle_steps = object->print()->default_region_config().top_shell_layers.value;
                                } else {
                                    fill_form = search_string.find(cs[0]);
                                    if (fill_form != std::string::npos)
                                        cs = &cs[1];

                                    _negative   = (cs[0] == '-'); // negative parameter
                                    angle_steps = abs(strtod(cs, &cs));

                                    if (angle_steps && cs[0] != '\0' && cs[0] != '!') {
                                        if (cs[0] == '%') // value in the percents of fill_z
                                            limit_fill_z = angle_steps * object->height() * 1e-8;
                                        else if (cs[0] == '#') // value in the feet
                                            limit_fill_z = angle_steps * object->config().layer_height;
                                        else if (cs[0] == '\'') // value in the feet
                                            limit_fill_z = angle_steps * 12 * 25.4;
                                        else if (cs[0] == '\"') // value in the inches
                                            limit_fill_z = angle_steps * 25.4;
                                        else if (cs[0] == 'c') // value in centimeters
                                            limit_fill_z = angle_steps * 10.;
                                        else if (cs[0] == 'm') {
                                            if (cs[1] == 'm') { // value in the millimeters
                                                limit_fill_z = angle_steps * 1.;
                                            } else{
                                                limit_fill_z = angle_steps * 1000.;
                                            }
                                        }
                                        limit_fill_z += fill_z;
                                        angle_steps = 0; // limit_fill_z has already count
                                    }
                                }
                                if (angle_steps) { // if limit_fill_z does not setting by lenght method. Get count the layer id above model height
                                    if (fill_form == std::string::npos && !_absolute)
                                        angle_add *= (int) angle_steps;
                                    int idx      = i + std::max(angle_steps - 1, 0.);
                                    int sdx      = std::max(0, idx - (int) object->layers().size());
                                    idx          = std::min(idx, (int) object->layers().size() - 1);
                                    limit_fill_z = object->get_layer(idx)->print_z + sdx * object->config().layer_height;
                                }
                                repeats = std::max(--repeats, 0);
                            } else
                                _noop = true; // set the dumb cycle
                            if (_absolute) {  // is absolute
                                angle_start = angle_add;
                                angle_add   = 0;
                            }
                        }
                        if (++t >= tk.size())
                            t = 0;
                    } while (std::all_of(stop.begin(), stop.end(), [](bool v) { return v; }) ?
                                 false :
                                 (t ? _noop : false) || stop[t]); // if this is a dumb instruction which never reaprated twice
                }
            }
            double top_z    = object->get_layer(i)->print_z;
            double negvalue = (_negative ? limit_fill_z - top_z : top_z - start_fill_z) / (limit_fill_z - start_fill_z);

            switch (fill_form) {
            case 0: break;                                                  // /-joint, linear
            case 1: negvalue -= sin(negvalue * PI * 2.) / (PI * 2.); break; // N-joint, sinus, vertical start
            case 2: negvalue -= sin(negvalue * PI * 2.) / (PI * 4.); break; // n-joint, sinus, vertical start, lazy
            case 3: negvalue += sin(negvalue * PI * 2.) / (PI * 2.); break; // Z-joint, sinus, horizontal start
            case 4: negvalue += sin(negvalue * PI * 2.) / (PI * 4.); break; // z-joint, sinus, horizontal start, lazy
            case 5: negvalue = asin(negvalue * 2. - 1.) / PI + 0.5; break;  // $-joint, arcsin
            case 6: negvalue = sin(negvalue * PI / 2.); break;              // L-joint, quarter of circle, horizontal start
            case 7: negvalue = 1. - cos(negvalue * PI / 2.); break;         // l-joint, quarter of circle, vertical start
            case 8: negvalue = 1. - pow(1. - negvalue, 2); break;           // U-joint, squared, x2
            case 9: negvalue = pow(1 - negvalue, 2); break;                 // u-joint, squared, x2 inverse
            case 10: negvalue = 1. - pow(1. - negvalue, 3); break;          // Q-joint, cubic, x3
            case 11: negvalue = pow(1. - negvalue, 3); break;               // q-joint, cubic, x3 inverse
            case 12: negvalue = (double) rand() / RAND_MAX; break;          // ~-joint, random, fill the whole angle
            case 13: negvalue += (double) rand() / RAND_MAX - 0.5; break;   // ^-joint, pseudorandom, disperse at middle line
            case 14: negvalue = 0.5; break;                                 // |-joint, like #-joint but placed at middle angle
            case 15: negvalue = _negative ? 0. : 1.; break;                 // #-joint, vertical at the end angle
            }
            angle = Geometry::deg2rad(angle_start + angle_add * negvalue);
        }
    } else {
        rotate_angles.deserialize(template_string);
        auto rotate_angle_idx = layer_id % rotate_angles.size();
        angle                 = Geometry::deg2rad(rotate_angles.values[rotate_angle_idx]);
    }
    return angle;
}

struct SurfaceFillParams
{
	// Zero based extruder ID.
    unsigned int 	extruder = 0;
	// Infill pattern, adjusted for the density etc.
    InfillPattern  	pattern = InfillPattern(0);

    // FillBase
    // in unscaled coordinates
    coordf_t    	spacing = 0.;
    // infill / perimeter overlap, in unscaled coordinates
    coordf_t    	overlap = 0.;
    // Angle as provided by the region config, in radians.
    float       	angle = 0.f;
    // Orca: is_using_template_angle
    bool        is_using_template_angle = false;
    // Is bridging used for this fill? Bridging parameters may be used even if this->flow.bridge() is not set.
    bool 			bridge;
    // Non-negative for a bridge.
    float 			bridge_angle = 0.f;

    // FillParams
    float       	density = 0.f;
    // Infill line multiplier count.
    int   multiline = 1;
    // Don't adjust spacing to fill the space evenly.
//    bool        	dont_adjust = false;
    // Length of the infill anchor along the perimeter line.
    // 1000mm is roughly the maximum length line that fits into a 32bit coord_t.
    float 			anchor_length     = 1000.f;
    float 			anchor_length_max = 1000.f;

    // width, height of extrusion, nozzle diameter, is bridge
    // For the output, for fill generator.
    Flow 			flow;

	// For the output
    ExtrusionRole	extrusion_role = ExtrusionRole(0);

	// Various print settings?

	// Index of this entry in a linear vector.
    size_t 			idx = 0;
	// infill speed settings
	float			sparse_infill_speed = 0;
	float			top_surface_speed = 0;
	float			solid_infill_speed = 0;

    // Params for lattice infill angles
    float lateral_lattice_angle_1 = 0.f;
    float lateral_lattice_angle_2 = 0.f;
    float infill_lock_depth          = 0;
    float skin_infill_depth          = 0;
    bool symmetric_infill_y_axis = false;

    // Params for Lateral honeycomb
    float infill_overhang_angle = 60.f;

	bool operator<(const SurfaceFillParams &rhs) const {
#define RETURN_COMPARE_NON_EQUAL(KEY) if (this->KEY < rhs.KEY) return true; if (this->KEY > rhs.KEY) return false;
#define RETURN_COMPARE_NON_EQUAL_TYPED(TYPE, KEY) if (TYPE(this->KEY) < TYPE(rhs.KEY)) return true; if (TYPE(this->KEY) > TYPE(rhs.KEY)) return false;

		// Sort first by decreasing bridging angle, so that the bridges are processed with priority when trimming one layer by the other.
		if (this->bridge_angle > rhs.bridge_angle) return true;
		if (this->bridge_angle < rhs.bridge_angle) return false;

		RETURN_COMPARE_NON_EQUAL(extruder);
		RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, pattern);
		RETURN_COMPARE_NON_EQUAL(spacing);
		RETURN_COMPARE_NON_EQUAL(overlap);
		RETURN_COMPARE_NON_EQUAL(angle);
		RETURN_COMPARE_NON_EQUAL(is_using_template_angle);
		RETURN_COMPARE_NON_EQUAL(density);
		RETURN_COMPARE_NON_EQUAL(multiline);
//		RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, dont_adjust);
		RETURN_COMPARE_NON_EQUAL(anchor_length);
		RETURN_COMPARE_NON_EQUAL(anchor_length_max);
		RETURN_COMPARE_NON_EQUAL(flow.width());
		RETURN_COMPARE_NON_EQUAL(flow.height());
		RETURN_COMPARE_NON_EQUAL(flow.nozzle_diameter());
		RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, bridge);
		RETURN_COMPARE_NON_EQUAL_TYPED(unsigned, extrusion_role);
		RETURN_COMPARE_NON_EQUAL(sparse_infill_speed);
		RETURN_COMPARE_NON_EQUAL(top_surface_speed);
		RETURN_COMPARE_NON_EQUAL(solid_infill_speed);
        RETURN_COMPARE_NON_EQUAL(lateral_lattice_angle_1);
		RETURN_COMPARE_NON_EQUAL(lateral_lattice_angle_2);
		RETURN_COMPARE_NON_EQUAL(symmetric_infill_y_axis);
		RETURN_COMPARE_NON_EQUAL(infill_lock_depth);
		RETURN_COMPARE_NON_EQUAL(skin_infill_depth);		RETURN_COMPARE_NON_EQUAL(infill_overhang_angle);

		return false;
	}

	bool operator==(const SurfaceFillParams &rhs) const {
		return  this->extruder 			== rhs.extruder 		&&
				this->pattern 			== rhs.pattern 			&&
				this->spacing 			== rhs.spacing 			&&
				this->overlap 			== rhs.overlap 			&&
				this->angle   			== rhs.angle   			&&
				this->is_using_template_angle == rhs.is_using_template_angle &&
				this->bridge   			== rhs.bridge   		&&
				this->bridge_angle 		== rhs.bridge_angle		&&
				this->density   		== rhs.density   		&&
				this->multiline             == rhs.multiline    &&
//				this->dont_adjust   	== rhs.dont_adjust 		&&
				this->anchor_length  	== rhs.anchor_length    &&
				this->anchor_length_max == rhs.anchor_length_max &&
				this->flow 				== rhs.flow 			&&
				this->extrusion_role	== rhs.extrusion_role	&&
				this->sparse_infill_speed	== rhs.sparse_infill_speed &&
				this->top_surface_speed		== rhs.top_surface_speed &&
				this->solid_infill_speed	== rhs.solid_infill_speed &&
                this->lateral_lattice_angle_1		== rhs.lateral_lattice_angle_1 &&
				this->lateral_lattice_angle_2	    == rhs.lateral_lattice_angle_2 &&
				this->infill_lock_depth      ==  rhs.infill_lock_depth &&
				this->skin_infill_depth      ==  rhs.skin_infill_depth &&
                this->infill_overhang_angle == rhs.infill_overhang_angle;
	}
};

struct SurfaceFill {
	SurfaceFill(const SurfaceFillParams& params) : region_id(size_t(-1)), surface(stCount, ExPolygon()), params(params) {}

	size_t 				region_id;
	Surface 			surface;
	ExPolygons       	expolygons;
	SurfaceFillParams	params;
    // BBS
    std::vector<size_t> region_id_group;
    ExPolygons          no_overlap_expolygons;
};


// Detect narrow infill regions
// Based on the anti-vibration algorithm from PrusaSlicer:
// https://github.com/prusa3d/PrusaSlicer/blob/5dc04b4e8f14f65bbcc5377d62cad3e86c2aea36/src/libslic3r/Fill/FillEnsuring.cpp#L37-L273

static coord_t _MAX_LINE_LENGTH_TO_FILTER() // 4 mm.
{
    return scaled<coord_t>(4.);
}
const constexpr size_t  MAX_SKIPS_ALLOWED           = 2; // Skip means propagation through long line.
const constexpr size_t  MIN_DEPTH_FOR_LINE_REMOVING = 5;

struct LineNode
{
    struct State
    {
        // The total number of long lines visited before this node was reached.
        // We just need the minimum number of all possible paths to decide whether we can remove the line or not.
        int min_skips_taken             = 0;
        // The total number of short lines visited before this node was reached.
        int total_short_lines           = 0;
        // Some initial line is touching some long line. This information is propagated to neighbors.
        bool initial_touches_long_lines = false;
        bool initialized                = false;

        void reset() {
            this->min_skips_taken            = 0;
            this->total_short_lines          = 0;
            this->initial_touches_long_lines = false;
            this->initialized                = false;
        }
    };

    explicit LineNode(const Line &line) : line(line) {}

    Line                   line;
    // Pointers to line nodes in the previous and the next section that overlap with this line.
    std::vector<LineNode*> next_section_overlapping_lines;
    std::vector<LineNode*> prev_section_overlapping_lines;

    bool                   is_removed = false;

    State                  state;

    // Return true if some initial line is touching some long line and this information was propagated into the current line.
    bool is_initial_line_touching_long_lines() const {
        if (prev_section_overlapping_lines.empty())
            return false;

        for (LineNode *line_node : prev_section_overlapping_lines) {
            if (line_node->state.initial_touches_long_lines)
                return true;
        }

        return false;
    }

    // Return true if the current line overlaps with some long line in the previous section.
    bool is_touching_long_lines_in_previous_layer() const {
        if (prev_section_overlapping_lines.empty())
            return false;

        const auto MAX_LINE_LENGTH_TO_FILTER = _MAX_LINE_LENGTH_TO_FILTER();
        for (LineNode *line_node : prev_section_overlapping_lines) {
            if (!line_node->is_removed && line_node->line.length() >= MAX_LINE_LENGTH_TO_FILTER)
                return true;
        }

        return false;
    }

    // Return true if the current line overlaps with some line in the next section.
    bool has_next_layer_neighbours() const {
        if (next_section_overlapping_lines.empty())
            return false;

        for (LineNode *line_node : next_section_overlapping_lines) {
            if (!line_node->is_removed)
                return true;
        }

        return false;
    }
};

using LineNodes = std::vector<LineNode>;

inline bool are_lines_overlapping_in_y_axes(const Line &first_line, const Line &second_line) {
    return (second_line.a.y() <= first_line.a.y() && first_line.a.y() <= second_line.b.y())
        || (second_line.a.y() <= first_line.b.y() && first_line.b.y() <= second_line.b.y())
        || (first_line.a.y() <= second_line.a.y() && second_line.a.y() <= first_line.b.y())
        || (first_line.a.y() <= second_line.b.y() && second_line.b.y() <= first_line.b.y());
}

bool can_line_note_be_removed(const LineNode &line_node) {
    const auto MAX_LINE_LENGTH_TO_FILTER = _MAX_LINE_LENGTH_TO_FILTER();
    return (line_node.line.length() < MAX_LINE_LENGTH_TO_FILTER)
        && (line_node.state.total_short_lines > int(MIN_DEPTH_FOR_LINE_REMOVING)
            || (!line_node.is_initial_line_touching_long_lines() && !line_node.has_next_layer_neighbours()));
}

// Remove the node and propagate its removal to the previous sections.
void propagate_line_node_remove(const LineNode &line_node) {
    std::queue<LineNode *> line_node_queue;
    for (LineNode *prev_line : line_node.prev_section_overlapping_lines) {
        if (prev_line->is_removed)
            continue;

        line_node_queue.emplace(prev_line);
    }

    for (; !line_node_queue.empty(); line_node_queue.pop()) {
        LineNode &line_to_check = *line_node_queue.front();

        if (can_line_note_be_removed(line_to_check)) {
            line_to_check.is_removed = true;

            for (LineNode *prev_line : line_to_check.prev_section_overlapping_lines) {
                if (prev_line->is_removed)
                    continue;

                line_node_queue.emplace(prev_line);
            }
        }
    }
}

// Filter out short extrusions that could create vibrations.
static std::vector<Lines> filter_vibrating_extrusions(const std::vector<Lines> &lines_sections) {
    // Initialize all line nodes.
    std::vector<LineNodes> line_nodes_sections(lines_sections.size());
    for (const Lines &lines_section : lines_sections) {
        const size_t section_idx = &lines_section - lines_sections.data();

        line_nodes_sections[section_idx].reserve(lines_section.size());
        for (const Line &line : lines_section) {
            line_nodes_sections[section_idx].emplace_back(line);
        }
    }

    // Precalculate for each line node which line nodes in the previous and next section this line node overlaps.
    for (auto curr_lines_section_it = line_nodes_sections.begin(); curr_lines_section_it != line_nodes_sections.end(); ++curr_lines_section_it) {
        if (curr_lines_section_it != line_nodes_sections.begin()) {
            const auto prev_lines_section_it = std::prev(curr_lines_section_it);
            for (LineNode &curr_line : *curr_lines_section_it) {
                for (LineNode &prev_line : *prev_lines_section_it) {
                    if (are_lines_overlapping_in_y_axes(curr_line.line, prev_line.line)) {
                        curr_line.prev_section_overlapping_lines.emplace_back(&prev_line);
                    }
                }
            }
        }

        if (std::next(curr_lines_section_it) != line_nodes_sections.end()) {
            const auto next_lines_section_it = std::next(curr_lines_section_it);
            for (LineNode &curr_line : *curr_lines_section_it) {
                for (LineNode &next_line : *next_lines_section_it) {
                    if (are_lines_overlapping_in_y_axes(curr_line.line, next_line.line)) {
                        curr_line.next_section_overlapping_lines.emplace_back(&next_line);
                    }
                }
            }
        }
    }

    const auto MAX_LINE_LENGTH_TO_FILTER = _MAX_LINE_LENGTH_TO_FILTER();
    // Select each section as the initial lines section and propagate line node states from this initial lines section to the last lines section.
    // During this propagation, we remove those lines that meet the conditions for its removal.
    // When some line is removed, we propagate this removal to previous layers.
    for (size_t initial_line_section_idx = 0; initial_line_section_idx < line_nodes_sections.size(); ++initial_line_section_idx) {
        // Stars from non-removed short lines.
        for (LineNode &initial_line : line_nodes_sections[initial_line_section_idx]) {
            if (initial_line.is_removed || initial_line.line.length() >= MAX_LINE_LENGTH_TO_FILTER)
                continue;

            initial_line.state.reset();
            initial_line.state.total_short_lines          = 1;
            initial_line.state.initial_touches_long_lines = initial_line.is_touching_long_lines_in_previous_layer();
            initial_line.state.initialized                = true;
        }

        // Iterate from the initial lines section until the last lines section.
        for (size_t propagation_line_section_idx = initial_line_section_idx; propagation_line_section_idx < line_nodes_sections.size(); ++propagation_line_section_idx) {
            // Before we propagate node states into next lines sections, we reset the state of all line nodes in the next line section.
            if (propagation_line_section_idx + 1 < line_nodes_sections.size()) {
                for (LineNode &propagation_line : line_nodes_sections[propagation_line_section_idx + 1]) {
                    propagation_line.state.reset();
                }
            }

            for (LineNode &propagation_line : line_nodes_sections[propagation_line_section_idx]) {
                if (propagation_line.is_removed || !propagation_line.state.initialized)
                    continue;

                for (LineNode *neighbour_line : propagation_line.next_section_overlapping_lines) {
                    if (neighbour_line->is_removed)
                        continue;

                    const bool is_short_line   = neighbour_line->line.length() < MAX_LINE_LENGTH_TO_FILTER;
                    const bool is_skip_allowed = propagation_line.state.min_skips_taken < int(MAX_SKIPS_ALLOWED);

                    if (!is_short_line && !is_skip_allowed)
                        continue;

                    const int neighbour_total_short_lines = propagation_line.state.total_short_lines + int(is_short_line);
                    const int neighbour_min_skips_taken   = propagation_line.state.min_skips_taken + int(!is_short_line);

                    if (neighbour_line->state.initialized) {
                        // When the state of the node was previously filled, then we need to update data in such a way
                        // that will maximize the possibility of removing this node.
                        neighbour_line->state.min_skips_taken = std::max(neighbour_line->state.min_skips_taken, neighbour_total_short_lines);
                        neighbour_line->state.min_skips_taken = std::min(neighbour_line->state.min_skips_taken, neighbour_min_skips_taken);

                        // We will keep updating neighbor initial_touches_long_lines until it is equal to false.
                        if (neighbour_line->state.initial_touches_long_lines) {
                            neighbour_line->state.initial_touches_long_lines = propagation_line.state.initial_touches_long_lines;
                        }
                    } else {
                        neighbour_line->state.total_short_lines          = neighbour_total_short_lines;
                        neighbour_line->state.min_skips_taken            = neighbour_min_skips_taken;
                        neighbour_line->state.initial_touches_long_lines = propagation_line.state.initial_touches_long_lines;
                        neighbour_line->state.initialized                = true;
                    }
                }

                if (can_line_note_be_removed(propagation_line)) {
                    // Remove the current node and propagate its removal to the previous sections.
                    propagation_line.is_removed = true;
                    propagate_line_node_remove(propagation_line);
                }
            }
        }
    }

    // Create lines sections without filtered-out lines.
    std::vector<Lines> lines_sections_out(line_nodes_sections.size());
    for (const std::vector<LineNode> &line_nodes_section : line_nodes_sections) {
        const size_t section_idx = &line_nodes_section - line_nodes_sections.data();

        for (const LineNode &line_node : line_nodes_section) {
            if (!line_node.is_removed) {
                lines_sections_out[section_idx].emplace_back(line_node.line);
            }
        }
    }

    return lines_sections_out;
}

void split_solid_surface(size_t layer_id, const SurfaceFill &fill, ExPolygons &normal_infill, ExPolygons &narrow_infill)
{
    assert(fill.surface.surface_type == stInternalSolid);

	switch (fill.params.pattern) {
    case ipRectilinear:
    case ipMonotonic:
    case ipMonotonicLine:
    case ipAlignedRectilinear:
        // Only support straight line based infill
        break;

    default:
        // For all other types, don't split
        return;
    }

    Polygons normal_fill_areas;  // Areas that filled with normal infill

    constexpr double connect_extrusions = true;

    const coord_t scaled_spacing                      = scaled<coord_t>(fill.params.spacing);
    double        distance_limit_reconnection         = 2.0 * double(scaled_spacing);
    double        squared_distance_limit_reconnection = distance_limit_reconnection * distance_limit_reconnection;
    // Calculate infill direction, see Fill::_infill_direction
    double        base_angle                          = fill.params.angle + float(M_PI / 2.);
    // For pattern other than ipAlignedRectilinear, the angle are alternated
    if (fill.params.pattern != ipAlignedRectilinear) {
        size_t idx = layer_id / fill.surface.thickness_layers;
        base_angle += (idx & 1) ? float(M_PI / 2.) : 0;
    }
    const double aligning_angle = -base_angle + PI;

	for (const ExPolygon &expolygon : fill.expolygons) {
        Polygons filled_area = to_polygons(expolygon);
        polygons_rotate(filled_area, aligning_angle);
        BoundingBox bb = get_extents(filled_area);

        Polygons inner_area = intersection(filled_area, opening(filled_area, 2 * scaled_spacing, 3 * scaled_spacing));

        inner_area = shrink(inner_area, scaled_spacing * 0.5 - scaled<double>(fill.params.overlap));

        AABBTreeLines::LinesDistancer<Line> area_walls{to_lines(inner_area)};

        const size_t  n_vlines = (bb.max.x() - bb.min.x() + scaled_spacing - 1) / scaled_spacing;
        const coord_t y_min    = bb.min.y();
        const coord_t y_max    = bb.max.y();
        Lines         vertical_lines(n_vlines);
        for (size_t i = 0; i < n_vlines; i++) {
            coord_t x           = bb.min.x() + i * double(scaled_spacing);
            vertical_lines[i].a = Point{x, y_min};
            vertical_lines[i].b = Point{x, y_max};
        }

        if (!vertical_lines.empty()) {
            vertical_lines.push_back(vertical_lines.back());
            vertical_lines.back().a = Point{coord_t(bb.min.x() + n_vlines * double(scaled_spacing) + scaled_spacing * 0.5), y_min};
            vertical_lines.back().b = Point{vertical_lines.back().a.x(), y_max};
        }

        std::vector<Lines> polygon_sections(n_vlines);

        for (size_t i = 0; i < n_vlines; i++) {
            const auto intersections = area_walls.intersections_with_line<true>(vertical_lines[i]);

            for (int intersection_idx = 0; intersection_idx < int(intersections.size()) - 1; intersection_idx++) {
                const auto &a = intersections[intersection_idx];
                const auto &b = intersections[intersection_idx + 1];
                if (area_walls.outside((a.first + b.first) / 2) < 0) {
                    if (std::abs(a.first.y() - b.first.y()) > scaled_spacing) {
                        polygon_sections[i].emplace_back(a.first, b.first);
                    }
                }
            }
        }

        polygon_sections = filter_vibrating_extrusions(polygon_sections);

        Polygons reconstructed_area{};
        // reconstruct polygon from polygon sections
        {
            struct TracedPoly
            {
                Points lows;
                Points highs;
            };

            std::vector<std::vector<Line>> polygon_sections_w_width = polygon_sections;
            for (auto &slice : polygon_sections_w_width) {
                for (Line &l : slice) {
                    l.a -= Point{0.0, 0.5 * scaled_spacing};
                    l.b += Point{0.0, 0.5 * scaled_spacing};
                }
            }

            std::vector<TracedPoly> current_traced_polys;
            for (const auto &polygon_slice : polygon_sections_w_width) {
                std::unordered_set<const Line *> used_segments;
                for (TracedPoly &traced_poly : current_traced_polys) {
                    auto candidates_begin = std::upper_bound(polygon_slice.begin(), polygon_slice.end(), traced_poly.lows.back(),
                                                             [](const Point &low, const Line &seg) { return seg.b.y() > low.y(); });
                    auto candidates_end   = std::upper_bound(polygon_slice.begin(), polygon_slice.end(), traced_poly.highs.back(),
                                                             [](const Point &high, const Line &seg) { return seg.a.y() > high.y(); });

                    bool segment_added = false;
                    for (auto candidate = candidates_begin; candidate != candidates_end && !segment_added; candidate++) {
                        if (used_segments.find(&(*candidate)) != used_segments.end()) {
                            continue;
                        }
                        if (connect_extrusions && (traced_poly.lows.back() - candidates_begin->a).cast<double>().squaredNorm() <
                                                      squared_distance_limit_reconnection) {
                            traced_poly.lows.push_back(candidates_begin->a);
                        } else {
                            traced_poly.lows.push_back(traced_poly.lows.back() + Point{scaled_spacing / 2, coord_t(0)});
                            traced_poly.lows.push_back(candidates_begin->a - Point{scaled_spacing / 2, 0});
                            traced_poly.lows.push_back(candidates_begin->a);
                        }

                        if (connect_extrusions && (traced_poly.highs.back() - candidates_begin->b).cast<double>().squaredNorm() <
                                                      squared_distance_limit_reconnection) {
                            traced_poly.highs.push_back(candidates_begin->b);
                        } else {
                            traced_poly.highs.push_back(traced_poly.highs.back() + Point{scaled_spacing / 2, 0});
                            traced_poly.highs.push_back(candidates_begin->b - Point{scaled_spacing / 2, 0});
                            traced_poly.highs.push_back(candidates_begin->b);
                        }
                        segment_added = true;
                        used_segments.insert(&(*candidates_begin));
                    }

                    if (!segment_added) {
                        // Zero or multiple overlapping segments. Resolving this is nontrivial,
                        // so we just close this polygon and maybe open several new. This will hopefully happen much less often
                        traced_poly.lows.push_back(traced_poly.lows.back() + Point{scaled_spacing / 2, 0});
                        traced_poly.highs.push_back(traced_poly.highs.back() + Point{scaled_spacing / 2, 0});
                        Polygon &new_poly = reconstructed_area.emplace_back(std::move(traced_poly.lows));
                        new_poly.points.insert(new_poly.points.end(), traced_poly.highs.rbegin(), traced_poly.highs.rend());
                        traced_poly.lows.clear();
                        traced_poly.highs.clear();
                    }
                }

                current_traced_polys.erase(std::remove_if(current_traced_polys.begin(), current_traced_polys.end(),
                                                          [](const TracedPoly &tp) { return tp.lows.empty(); }),
                                           current_traced_polys.end());

                for (const auto &segment : polygon_slice) {
                    if (used_segments.find(&segment) == used_segments.end()) {
                        TracedPoly &new_tp = current_traced_polys.emplace_back();
                        new_tp.lows.push_back(segment.a - Point{scaled_spacing / 2, 0});
                        new_tp.lows.push_back(segment.a);
                        new_tp.highs.push_back(segment.b - Point{scaled_spacing / 2, 0});
                        new_tp.highs.push_back(segment.b);
                    }
                }
            }

            // add not closed polys
            for (TracedPoly &traced_poly : current_traced_polys) {
                Polygon &new_poly = reconstructed_area.emplace_back(std::move(traced_poly.lows));
                new_poly.points.insert(new_poly.points.end(), traced_poly.highs.rbegin(), traced_poly.highs.rend());
            }
        }

        polygons_append(normal_fill_areas, reconstructed_area);
    }

    polygons_rotate(normal_fill_areas, -aligning_angle);

    // Do the split
    ExPolygons normal_fill_areas_ex = union_safety_offset_ex(normal_fill_areas);
    ExPolygons narrow_fill_areas    = diff_ex(fill.expolygons, normal_fill_areas_ex);

    // Merge very small areas that is smaller than a single line width to the normal infill if they touches
    for (auto iter = narrow_fill_areas.begin(); iter != narrow_fill_areas.end();) {
        auto shrinked_expoly = offset_ex(*iter, -scaled_spacing * 0.5);
        if (shrinked_expoly.empty()) {
            // Too small! Check if it touches any normal infills
            auto     expanede_exploy          = offset_ex(*iter, scaled_spacing * 0.3);
            Polygons normal_fill_area_clipped = ClipperUtils::clip_clipper_polygons_with_subject_bbox(normal_fill_areas_ex, get_extents(expanede_exploy));
            auto     touch_check              = intersection_ex(normal_fill_area_clipped, expanede_exploy);
            if (!touch_check.empty()) {
                normal_fill_areas_ex.emplace_back(*iter);
                iter = narrow_fill_areas.erase(iter);
                continue;
            }
        }
        iter++;
    }

    if (narrow_fill_areas.empty()) {
        // No split needed
        return;
    }

    // Expand the normal infills a little bit to avoid gaps between normal and narrow infills
    normal_infill = intersection_ex(offset_ex(normal_fill_areas_ex, scaled_spacing * 0.1), fill.expolygons);
    narrow_infill = narrow_fill_areas;

#ifdef DEBUG_SURFACE_SPLIT
    {
        BoundingBox bbox   = get_extents(fill.expolygons);
        bbox.offset(scale_(1.));
        ::Slic3r::SVG svg(debug_out_path("surface_split_%d.svg", layer_id), bbox);
        svg.draw(to_lines(fill.expolygons), "red", scale_(0.1));
        svg.draw(normal_infill, "blue", 0.5);
        svg.draw(narrow_infill, "green", 0.5);
        svg.Close();
    }
#endif
}

std::vector<SurfaceFill> group_fills(const Layer &layer, LockRegionParam &lock_param)
{
	std::vector<SurfaceFill> surface_fills;
	// Fill in a map of a region & surface to SurfaceFillParams.
	std::set<SurfaceFillParams> 						set_surface_params;
	std::vector<std::vector<const SurfaceFillParams*>> 	region_to_surface_params(layer.regions().size(), std::vector<const SurfaceFillParams*>());
    SurfaceFillParams									params;
    bool 												has_internal_voids = false;
	const PrintObjectConfig&							object_config = layer.object()->config();

	auto append_flow_param = [](std::map<Flow, ExPolygons> &flow_params, Flow flow, const ExPolygon &exp) {
        auto it = flow_params.find(flow);
        if (it == flow_params.end())
            flow_params.insert({flow, {exp}});
        else
            it->second.push_back(exp);
    };

	auto append_density_param = [](std::map<float, ExPolygons> &density_params, float density, const ExPolygon &exp) {
        auto it = density_params.find(density);
        if (it == density_params.end())
            density_params.insert({density, {exp}});
        else
            it->second.push_back(exp);
    };

	for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id) {
		const LayerRegion  &layerm = *layer.regions()[region_id];
		region_to_surface_params[region_id].assign(layerm.fill_surfaces.size(), nullptr);
	    for (const Surface &surface : layerm.fill_surfaces.surfaces)
	        if (surface.surface_type == stInternalVoid)
	        	has_internal_voids = true;
	        else {
		        const PrintRegionConfig &region_config = layerm.region().config();
		        FlowRole extrusion_role = surface.is_top() ? frTopSolidInfill : (surface.is_solid() ? frSolidInfill : frInfill);
		        bool     is_bridge 	    = layer.id() > 0 && surface.is_bridge();
                const unsigned int effective_extruder = layerm.extruder(extrusion_role);
		        params.extruder 	 = effective_extruder;
		        params.pattern 		 = region_config.sparse_infill_pattern.value;
		        params.density       = float(region_config.sparse_infill_density);
                params.lateral_lattice_angle_1 = region_config.lateral_lattice_angle_1;
                params.lateral_lattice_angle_2 = region_config.lateral_lattice_angle_2;
                params.infill_overhang_angle = region_config.infill_overhang_angle;
                if (params.pattern == ipLockedZag) {
                    params.infill_lock_depth = scale_(region_config.infill_lock_depth);
                    params.skin_infill_depth = scale_(region_config.skin_infill_depth);
                }
                if (params.pattern == ipCrossZag || params.pattern == ipLockedZag) {
                    params.symmetric_infill_y_axis = region_config.symmetric_infill_y_axis;
                } else if (params.pattern == ipZigZag) {
                    params.symmetric_infill_y_axis = region_config.symmetric_infill_y_axis;
                }

                if (surface.is_solid()) {
                    if (surface.is_external() && !is_bridge) {
                        if (surface.is_top()) {
                            params.pattern = region_config.top_surface_pattern.value;
                            params.density = float(region_config.top_surface_density);
                        } else { // Surface is bottom
                            params.pattern = region_config.bottom_surface_pattern.value;
                            params.density = float(region_config.bottom_surface_density);
                        }
                    // NEOTKO_MULTIPASS_TAG (port s129) — penultimate solid surface has its own pattern
                    // (default Monotonic Line, required by PathBlend/ColorStitch) and its own density.
                    // internal_solid_infill_pattern then governs only the remaining internal solid.
                    } else if (surface.surface_type == stPenultimateInternalSolid) {
                        // penu keys live in PrintObjectConfig (not region) → object_config.
                        params.pattern = object_config.penultimate_solid_infill_pattern.value;
                        params.density = float(object_config.penultimate_solid_infill_density);
                    } else if (surface.is_solid_infill()) {
                        params.pattern = region_config.internal_solid_infill_pattern.value;
                        params.density = 100.f;
                    } else {
                        if (region_config.top_surface_pattern == ipMonotonic || region_config.top_surface_pattern == ipMonotonicLine)
                            params.pattern = ipMonotonic;
                        else
                            params.pattern = ipRectilinear;
                        params.density = 100.f;
                    }
                } else if (params.density <= 0)
                    continue;

				params.extrusion_role = erInternalInfill;
                if (is_bridge) {
                    if (surface.is_internal_bridge())
                        params.extrusion_role = erInternalBridgeInfill;
                    else
                        params.extrusion_role = erBridgeInfill;
                } else if (surface.is_solid()) {
                    if (surface.is_top()) {
                        params.extrusion_role = erTopSolidInfill;
                    } else if (surface.is_bottom()) {
                        params.extrusion_role = erBottomSurface;
                    // NEOTKO_MULTIPASS_TAG (port s129): penultimate layers get dedicated role.
                    } else if (surface.surface_type == stPenultimateInternalSolid) {
                        params.extrusion_role = erPenultimateInfill;
                    } else {
                        params.extrusion_role = erSolidInfill;
                    }
                }
                // Orca: apply fill multiline only for sparse infill
                params.multiline = params.extrusion_role == erInternalInfill ? int(region_config.fill_multiline) : 1;

                if (params.extrusion_role == erInternalInfill) {
                    params.angle = calculate_infill_rotation_angle(layer.object(), layer.id(), region_config.infill_direction.value,
                                                                   region_config.sparse_infill_rotate_template.value);
                    params.is_using_template_angle = !region_config.sparse_infill_rotate_template.value.empty();
                } else {
                    params.angle = calculate_infill_rotation_angle(layer.object(), layer.id(), region_config.solid_infill_direction.value,
                                                                   region_config.solid_infill_rotate_template.value);
                    params.is_using_template_angle = !region_config.solid_infill_rotate_template.value.empty();
                }
                params.bridge_angle = float(surface.bridge_angle);
                
                if (region_config.align_infill_direction_to_model) {
                    auto m = layer.object()->trafo().matrix();
                    params.angle += atan2((float) m(1, 0), (float) m(0, 0));
                }

                // Calculate the actual flow we'll be using for this infill.
		        params.bridge = is_bridge || Fill::use_bridge_flow(params.pattern);
                const bool is_thick_bridge = surface.is_bridge() && (surface.is_internal_bridge() ? object_config.thick_internal_bridges : object_config.thick_bridges);
				params.flow   = params.bridge ?
					//Orca: enable thick bridge based on config
					layerm.bridging_flow(extrusion_role, is_thick_bridge) :
					layerm.flow(extrusion_role, (surface.thickness == -1) ? layer.height : surface.thickness);
				// record speed params
                if (!params.bridge) {
                    if (params.extrusion_role == erInternalInfill)
                        params.sparse_infill_speed = region_config.sparse_infill_speed;
                    else if (params.extrusion_role == erTopSolidInfill) {
                        params.top_surface_speed = region_config.top_surface_speed;
                    } else if (params.extrusion_role == erSolidInfill)
                        params.solid_infill_speed = region_config.internal_solid_infill_speed;
                }
				// Calculate flow spacing for infill pattern generation.
		        if (surface.is_solid() || is_bridge) {
		            params.spacing = params.flow.spacing();
		            // Don't limit anchor length for solid or bridging infill.
		            params.anchor_length = 1000.f;
					params.anchor_length_max = 1000.f;
		        } else {
					// Internal infill pattern spacing must stay independent of the current layer height and first-layer line width,
					// so sparse infill stays aligned over all layers of the current region.
		            params.spacing = layerm.flow(frInfill, layer.object()->config().layer_height, false).spacing();
		            // Anchor a sparse infill to inner perimeters with the following anchor length:
		        	params.anchor_length = float(region_config.infill_anchor);
					if (region_config.infill_anchor.percent)
						params.anchor_length = float(params.anchor_length * 0.01 * params.spacing);
					params.anchor_length_max = float(region_config.infill_anchor_max);
					if (region_config.infill_anchor_max.percent)
						params.anchor_length_max = float(params.anchor_length_max * 0.01 * params.spacing);
					params.anchor_length = std::min(params.anchor_length, params.anchor_length_max);
				}

				//get locked region param
				if (params.pattern == ipLockedZag){
					const PrintObject *object = layerm.layer()->object();
					auto nozzle_diameter = float(object->print()->config().nozzle_diameter.get_at(effective_extruder - 1));
					Flow skin_flow = params.bridge ? params.flow : Flow::new_from_config_width(extrusion_role, region_config.skin_infill_line_width, nozzle_diameter, float((surface.thickness == -1) ? layer.height : surface.thickness));
					//add skin flow
					append_flow_param(lock_param.skin_flow_params, skin_flow, surface.expolygon);

					Flow skeleton_flow = params.bridge ? params.flow : Flow::new_from_config_width(extrusion_role, region_config.skeleton_infill_line_width, nozzle_diameter, float((surface.thickness == -1) ? layer.height : surface.thickness)) ;
					// add skeleton flow
					append_flow_param(lock_param.skeleton_flow_params, skeleton_flow, surface.expolygon);

					// add skin density
					append_density_param(lock_param.skin_density_params, float(0.01 * region_config.skin_infill_density), surface.expolygon);

					// add skin density
					append_density_param(lock_param.skeleton_density_params, float(0.01 * region_config.skeleton_infill_density), surface.expolygon);

				}

                auto it_params = set_surface_params.find(params);

		        if (it_params == set_surface_params.end())
		        	it_params = set_surface_params.insert(it_params, params);
		        region_to_surface_params[region_id][&surface - &layerm.fill_surfaces.surfaces.front()] = &(*it_params);
		    }
	}

	surface_fills.reserve(set_surface_params.size());
	for (const SurfaceFillParams &params : set_surface_params) {
		const_cast<SurfaceFillParams&>(params).idx = surface_fills.size();
		surface_fills.emplace_back(params);
	}

	for (size_t region_id = 0; region_id < layer.regions().size(); ++ region_id) {
		const LayerRegion &layerm = *layer.regions()[region_id];
	    for (const Surface &surface : layerm.fill_surfaces.surfaces)
	        if (surface.surface_type != stInternalVoid) {
	        	const SurfaceFillParams *params = region_to_surface_params[region_id][&surface - &layerm.fill_surfaces.surfaces.front()];
				if (params != nullptr) {
	        		SurfaceFill &fill = surface_fills[params->idx];
                    if (fill.region_id == size_t(-1)) {
	        			fill.region_id = region_id;
	        			fill.surface = surface;
	        			fill.expolygons.emplace_back(std::move(fill.surface.expolygon));
						//BBS
						fill.region_id_group.push_back(region_id);
						fill.no_overlap_expolygons = layerm.fill_no_overlap_expolygons;
					} else {
						fill.expolygons.emplace_back(surface.expolygon);
						//BBS
						auto t = find(fill.region_id_group.begin(), fill.region_id_group.end(), region_id);
						if (t == fill.region_id_group.end()) {
							fill.region_id_group.push_back(region_id);
							fill.no_overlap_expolygons = union_ex(fill.no_overlap_expolygons, layerm.fill_no_overlap_expolygons);
						}
					}
				}
	        }
	}

	{
		Polygons all_polygons;
		for (SurfaceFill &fill : surface_fills)
			if (! fill.expolygons.empty()) {
				if (fill.expolygons.size() > 1 || ! all_polygons.empty()) {
					Polygons polys = to_polygons(std::move(fill.expolygons));
		            // Make a union of polygons, use a safety offset, subtract the preceding polygons.
				    // Bridges are processed first (see SurfaceFill::operator<())
		            fill.expolygons = all_polygons.empty() ? union_safety_offset_ex(polys) : diff_ex(polys, all_polygons, ApplySafetyOffset::Yes);
					append(all_polygons, std::move(polys));
				} else if (&fill != &surface_fills.back())
					append(all_polygons, to_polygons(fill.expolygons));
	        }
	}

    // we need to detect any narrow surfaces that might collapse
    // when adding spacing below
    // such narrow surfaces are often generated in sloping walls
    // by bridge_over_infill() and combine_infill() as a result of the
    // subtraction of the combinable area from the layer infill area,
    // which leaves small areas near the perimeters
    // we are going to grow such regions by overlapping them with the void (if any)
    // TODO: detect and investigate whether there could be narrow regions without
    // any void neighbors
    if (has_internal_voids) {
    	// Internal voids are generated only if "infill_only_where_needed" or "infill_every_layers" are active.
        coord_t  distance_between_surfaces = 0;
        Polygons surfaces_polygons;
        Polygons voids;
		int      region_internal_infill = -1;
		int		 region_solid_infill = -1;
		int		 region_some_infill = -1;
    	for (SurfaceFill &surface_fill : surface_fills)
			if (! surface_fill.expolygons.empty()) {
    			distance_between_surfaces = std::max(distance_between_surfaces, surface_fill.params.flow.scaled_spacing());
				append((surface_fill.surface.surface_type == stInternalVoid) ? voids : surfaces_polygons, to_polygons(surface_fill.expolygons));
				if (surface_fill.surface.surface_type == stInternalSolid)
					region_internal_infill = (int)surface_fill.region_id;
				if (surface_fill.surface.is_solid())
					region_solid_infill = (int)surface_fill.region_id;
				if (surface_fill.surface.surface_type != stInternalVoid)
					region_some_infill = (int)surface_fill.region_id;
			}
    	if (! voids.empty() && ! surfaces_polygons.empty()) {
    		// First clip voids by the printing polygons, as the voids were ignored by the loop above during mutual clipping.
    		voids = diff(voids, surfaces_polygons);
	        // Corners of infill regions, which would not be filled with an extrusion path with a radius of distance_between_surfaces/2
	        Polygons collapsed = diff(
	            surfaces_polygons,
				opening(surfaces_polygons, float(distance_between_surfaces /2), float(distance_between_surfaces / 2 + ClipperSafetyOffset)));
	        //FIXME why the voids are added to collapsed here? First it is expensive, second the result may lead to some unwanted regions being
	        // added if two offsetted void regions merge.
	        // polygons_append(voids, collapsed);
	        ExPolygons extensions = intersection_ex(expand(collapsed, float(distance_between_surfaces)), voids, ApplySafetyOffset::Yes);
	        // Now find an internal infill SurfaceFill to add these extrusions to.
	        SurfaceFill *internal_solid_fill = nullptr;
			unsigned int region_id = 0;
			if (region_internal_infill != -1)
				region_id = region_internal_infill;
			else if (region_solid_infill != -1)
				region_id = region_solid_infill;
			else if (region_some_infill != -1)
				region_id = region_some_infill;
			const LayerRegion& layerm = *layer.regions()[region_id];
	        for (SurfaceFill &surface_fill : surface_fills)
	        	if (surface_fill.surface.surface_type == stInternalSolid && std::abs(layer.height - surface_fill.params.flow.height()) < EPSILON) {
	        		internal_solid_fill = &surface_fill;
	        		break;
	        	}
	        if (internal_solid_fill == nullptr) {
	        	// Produce another solid fill.
		        params.extruder 	 = layerm.extruder(frSolidInfill);
                const auto top_pattern = layerm.region().config().top_surface_pattern;
                if(top_pattern == ipMonotonic || top_pattern == ipMonotonicLine)
                    params.pattern = top_pattern;
                else
                    params.pattern 		 = ipRectilinear;
	            params.density 		 = 100.f;
		        params.extrusion_role = erSolidInfill;
		        const PrintRegionConfig &region_config = layerm.region().config();
                params.angle = calculate_infill_rotation_angle(layer.object(), layer.id(), region_config.solid_infill_direction.value,
                                                               region_config.solid_infill_rotate_template.value);
                params.is_using_template_angle = !region_config.solid_infill_rotate_template.value.empty();

                // calculate the actual flow we'll be using for this infill
				params.flow = layerm.flow(frSolidInfill);
		        params.spacing = params.flow.spacing();
				surface_fills.emplace_back(params);
				surface_fills.back().surface.surface_type = stInternalSolid;
				surface_fills.back().surface.thickness = layer.height;
				surface_fills.back().expolygons = std::move(extensions);
	        } else {
	        	append(extensions, std::move(internal_solid_fill->expolygons));
	        	internal_solid_fill->expolygons = union_ex(extensions);
	        }
		}
    }

	// BBS: detect narrow internal solid infill area and use ipConcentricInternal pattern instead
	if (layer.object()->config().detect_narrow_internal_solid_infill) {
		size_t surface_fills_size = surface_fills.size();
		for (size_t i = 0; i < surface_fills_size; i++) {
			if (surface_fills[i].surface.surface_type != stInternalSolid)
				continue;

			ExPolygons normal_infill;
            ExPolygons narrow_infill;
            split_solid_surface(layer.id(), surface_fills[i], normal_infill, narrow_infill);

			if (narrow_infill.empty()) {
				// BBS: has no narrow expolygon
				continue;
			} else if (normal_infill.empty()) {
				// BBS: all expolygons are narrow, directly change the fill pattern
				surface_fills[i].params.pattern = ipConcentricInternal;
			}
			else {
				// BBS: some expolygons are narrow, spilit surface_fills[i] and rearrange the expolygons
				params = surface_fills[i].params;
				params.pattern = ipConcentricInternal;
				surface_fills.emplace_back(params);
				surface_fills.back().region_id = surface_fills[i].region_id;
				surface_fills.back().surface.surface_type = stInternalSolid;
				surface_fills.back().surface.thickness = surface_fills[i].surface.thickness;
                surface_fills.back().region_id_group       = surface_fills[i].region_id_group;
                surface_fills.back().no_overlap_expolygons = surface_fills[i].no_overlap_expolygons;
			    // BBS: move the narrow expolygons to new surface_fills.back();
			    surface_fills.back().expolygons = std::move(narrow_infill);
			    // BBS: delete the narrow expolygons from old surface_fills
                surface_fills[i].expolygons = std::move(normal_infill);
			}
		}
	}

	return surface_fills;
}

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
void export_group_fills_to_svg(const char *path, const std::vector<SurfaceFill> &fills)
{
    BoundingBox bbox;
    for (const auto &fill : fills)
        for (const auto &expoly : fill.expolygons)
            bbox.merge(get_extents(expoly));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const auto &fill : fills)
        for (const auto &expoly : fill.expolygons)
            svg.draw(expoly, surface_type_to_color_name(fill.surface.surface_type), transparency);
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}
#endif

// NEOTKO_MULTIPASS_TAG_START — helper: recursively scale mm3_per_mm + height by ratio
// Used by perimeter override to match the Beer-Lambert stadium correction applied to fills.
static void neotko_mp_scale_perim(ExtrusionEntity* e, double ratio, double k_mp)
{
    if (auto* path = dynamic_cast<ExtrusionPath*>(e)) {
        const double W = path->width, H = path->height, H_sub = H * ratio;
        const double A_orig = H     * (W - k_mp * H);
        const double A_sub  = H_sub * (W - k_mp * H_sub);
        path->mm3_per_mm = float(path->mm3_per_mm *
            ((A_orig > 1e-9 && W > H + 1e-6) ? (A_sub / A_orig) : ratio));
        path->height = float(H_sub);
    } else if (auto* loop = dynamic_cast<ExtrusionLoop*>(e)) {
        for (ExtrusionPath& p : loop->paths) {
            const double W = p.width, H = p.height, H_sub = H * ratio;
            const double A_orig = H     * (W - k_mp * H);
            const double A_sub  = H_sub * (W - k_mp * H_sub);
            p.mm3_per_mm = float(p.mm3_per_mm *
                ((A_orig > 1e-9 && W > H + 1e-6) ? (A_sub / A_orig) : ratio));
            p.height = float(H_sub);
        }
    } else if (auto* coll = dynamic_cast<ExtrusionEntityCollection*>(e)) {
        for (ExtrusionEntity* ee : coll->entities)
            neotko_mp_scale_perim(ee, ratio, k_mp);
    }
}
// NEOTKO_MULTIPASS_TAG_END

// friend to Layer
bool Layer::make_fills(FillAdaptive::Octree* adaptive_fill_octree, FillAdaptive::Octree* support_fill_octree, FillLightning::Generator* lightning_generator)
{
	for (LayerRegion *layerm : m_regions)
		layerm->fills.clear();


#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
//	this->export_region_fill_surfaces_to_svg_debug("10_fill-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
    LockRegionParam lock_param;
    std::vector<SurfaceFill>     surface_fills = group_fills(*this, lock_param);
	const Slic3r::BoundingBox bbox 			= this->object()->bounding_box();
	const auto                resolution 	= this->object()->print()->config().resolution.value;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
	{
		static int iRun = 0;
		export_group_fills_to_svg(debug_out_path("Layer-fill_surfaces-10_fill-final-%d.svg", iRun ++).c_str(), surface_fills);
	}
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    for (SurfaceFill &surface_fill : surface_fills) {
        // Create the filler object.
        std::unique_ptr<Fill> f = std::unique_ptr<Fill>(Fill::new_from_type(surface_fill.params.pattern));
        f->set_bounding_box(bbox);
        f->layer_id = this->id();
        f->z 		= this->print_z;
        f->angle 	= surface_fill.params.angle;
        f->is_using_template_angle = surface_fill.params.is_using_template_angle;
        f->adapt_fill_octree   = (surface_fill.params.pattern == ipSupportCubic) ? support_fill_octree : adaptive_fill_octree;
        f->print_config        = &this->object()->print()->config();
        f->print_object_config = &this->object()->config();
        // NEOTKO_NEOWEAVING_TAG_START — provide region config for angle lock in _infill_direction
        // SurfaceFillParams has no .config member in this codebase; retrieve via region_id.
        f->print_region_config  = (surface_fill.region_id != size_t(-1))
            ? &this->regions()[surface_fill.region_id]->region().config()
            : nullptr;
        // NEOTKO_NEOWEAVING_TAG_END
		if (surface_fill.params.pattern == ipConcentricInternal) {
            FillConcentricInternal *fill_concentric = dynamic_cast<FillConcentricInternal *>(f.get());
            assert(fill_concentric != nullptr);
            fill_concentric->print_config        = &this->object()->print()->config();
            fill_concentric->print_object_config = &this->object()->config();
        } else if (surface_fill.params.pattern == ipConcentric) {
            FillConcentric *fill_concentric = dynamic_cast<FillConcentric *>(f.get());
            assert(fill_concentric != nullptr);
            fill_concentric->print_config = &this->object()->print()->config();
            fill_concentric->print_object_config = &this->object()->config();
        } else if (surface_fill.params.pattern == ipLightning)
            dynamic_cast<FillLightning::Filler*>(f.get())->generator = lightning_generator;
        // calculate flow spacing for infill pattern generation
        bool using_internal_flow = ! surface_fill.surface.is_solid() && ! surface_fill.params.bridge;
        double link_max_length = 0.;
        if (! surface_fill.params.bridge) {
#if 0
            link_max_length = layerm.region()->config().get_abs_value(surface.is_external() ? "external_fill_link_max_length" : "fill_link_max_length", flow.spacing());
//            printf("flow spacing: %f,  is_external: %d, link_max_length: %lf\n", flow.spacing(), int(surface.is_external()), link_max_length);
#else
            if (surface_fill.params.density > 80.) // 80%
                link_max_length = 3. * f->spacing;
#endif
        }

        LayerRegion* layerm = this->m_regions[surface_fill.region_id];

        // Maximum length of the perimeter segment linking two infill lines.
        f->link_max_length = (coord_t)scale_(link_max_length);
        // Used by the concentric infill pattern to clip the loops to create extrusion paths.
        f->loop_clipping = coord_t(scale_(layerm->region().config().seam_gap.get_abs_value(surface_fill.params.flow.nozzle_diameter())));

        // apply half spacing using this flow's own spacing and generate infill
        FillParams params;
        params.density 		     = float(0.01 * surface_fill.params.density);
        params.multiline         = surface_fill.params.multiline;
		params.dont_adjust		 = false; //  surface_fill.params.dont_adjust;
        params.anchor_length     = surface_fill.params.anchor_length;
		params.anchor_length_max = surface_fill.params.anchor_length_max;
		params.resolution        = resolution;
        params.use_arachne       = surface_fill.params.pattern == ipConcentric || surface_fill.params.pattern == ipConcentricInternal;
        params.layer_height      = layerm->layer()->height;
        params.lateral_lattice_angle_1   = surface_fill.params.lateral_lattice_angle_1;
        params.lateral_lattice_angle_2   = surface_fill.params.lateral_lattice_angle_2;
        params.infill_overhang_angle   = surface_fill.params.infill_overhang_angle;

		// BBS
		params.flow = surface_fill.params.flow;
		params.extrusion_role = surface_fill.params.extrusion_role;
		params.using_internal_flow = using_internal_flow;
		params.no_extrusion_overlap = surface_fill.params.overlap;
        auto &region_config = layerm->region().config();
        params.config               = &region_config;
        params.pattern              = surface_fill.params.pattern;

        if( surface_fill.params.pattern == ipLockedZag ) {
			params.locked_zag = true;
            params.infill_lock_depth = surface_fill.params.infill_lock_depth;
            params.skin_infill_depth = surface_fill.params.skin_infill_depth;
            f->set_lock_region_param(lock_param);
		}
        if (surface_fill.params.pattern == ipCrossZag || surface_fill.params.pattern == ipLockedZag) {
            if (f->layer_id % 2 == 0) {
                params.horiz_move -= scale_(region_config.infill_shift_step) * (f->layer_id / 2);
            } else {
                params.horiz_move += scale_(region_config.infill_shift_step) * (f->layer_id / 2);
            }

            params.symmetric_infill_y_axis = surface_fill.params.symmetric_infill_y_axis;

        }
		if (surface_fill.params.pattern == ipGrid)
			params.can_reverse = false;

        // NEOTKO_PROFILE_TAG_START — Fase 6c v2: painter footprint masking,
        // MULTI-SLOT. A painted profile must apply ONLY to the XY area the user
        // actually painted; AND when multiple painted profiles share the same Z
        // (twin islands of a staircase painted with different profiles), EACH
        // slot's footprint gets its own sandwich. v1 used dominant_slot → only
        // one side got the effect, the other fell through to natural — that's
        // the test case the user hit. v2 enumerates every painted slot in the
        // band, builds its mask, and tags each piece with its slot id (0 =
        // natural remainder). The wipe tower is untouched: sublayer tools still
        // come from sub.tool_id, just more sublayers per layer.
        std::vector<int> _fp_tag;   // slot id per expoly (0 = natural, -1 = no pre-split)
        {
            const ModelObject* _mo6c = (this->object() != nullptr)
                ? this->object()->model_object() : nullptr;
            const ExtrusionRole _role6c = surface_fill.params.extrusion_role;
            const bool _is_tp = (_role6c == erTopSolidInfill ||
                                 _role6c == erPenultimateInfill);
            // NEOTKO_PAINT_COEXIST_TAG s91 — removed `!is_mm_painted()` gate.
            // Allow painter pre-split even on MMU-painted objects: the per-piece
            // MMU governance check downstream skips pieces whose XY is owned by
            // MMU, while colormix-painter pieces on non-MMU regions emit normally.
            if (_is_tp && _mo6c
                && SurfaceColorMix::object_has_any_colormix_paint(_mo6c)
                && !surface_fill.expolygons.empty()) {
                const PrintObject* _po6c = this->object();
                // NEOTKO_COLORSTITCH_TAG — s112 fix: el tope REAL de la malla puede
                // quedar hasta ~1 altura de capa POR ENCIMA del print_z de la última
                // rebanada (cuando la altura del objeto no es múltiplo limpio de la
                // capa). La pintura del top vive ahí, así que extendemos el límite
                // superior +1 capa (top) / +2 capas (penu, que ancla 1 capa más abajo)
                // para alcanzarla. Sin esto, objetos cuyo tope cae entre capas no
                // resuelven slot y no slicean (duplicar+escalar lo "arreglaba" por
                // alineación accidental).
                const double _zlo = (_role6c == erTopSolidInfill)
                    ? this->print_z - this->height : this->print_z;
                const double _zhi = (_role6c == erTopSolidInfill)
                    ? this->print_z + this->height
                    : this->print_z + 2.0 * this->height;
                // Enumerate every painted slot present in the Z band, then keep
                // only the ones whose profile has a non-empty stack for this role.
                std::vector<int> _slots = SurfaceColorMix::enumerate_painted_slots_in_z_range(
                    _po6c, _zlo, _zhi);
                std::vector<int>        _useful_slots;
                std::vector<ExPolygons> _useful_masks;
                for (int _slot6c : _slots) {
                    const int _pid = SurfaceColorMix::profile_id_for_slot(_po6c, _slot6c);
                    const auto* _p = Slic3r::SurfaceEffectProfileManager::get().find(_pid);
                    if (!_p) continue;
                    const std::string& _js = (_role6c == erTopSolidInfill)
                        ? _p->stack_top_json : _p->stack_penu_json;
                    if (SurfacePassStack::from_json(_js).passes.empty()) continue;
                    ExPolygons _mask = SurfaceColorMix::painted_footprint_in_z_range(
                        _po6c, _slot6c, _zlo, _zhi);
                    if (_mask.empty()) continue;
                    _useful_slots.push_back(_slot6c);
                    _useful_masks.push_back(std::move(_mask));
                }
                if (!_useful_slots.empty()) {
                    // Split: per-slot painted pieces + natural remainder = surface
                    // minus the union of all painted masks. Iteration order of
                    // surface_fill.expolygons after the split: slot1 pieces,
                    // slot2 pieces, ..., natural pieces. _fp_tag aligns 1:1.
                    ExPolygons _all_masks_union;
                    for (const auto& m : _useful_masks)
                        for (const auto& e : m) _all_masks_union.push_back(e);
                    _all_masks_union = union_ex(_all_masks_union);

                    ExPolygons _orig = std::move(surface_fill.expolygons);
                    surface_fill.expolygons.clear();
                    std::ostringstream _clip_log;
                    _clip_log << "FOOTPRINT_CLIP z=" << this->print_z
                              << " role=" << (int)_role6c << " slots=[";
                    for (size_t k = 0; k < _useful_slots.size(); ++k) {
                        ExPolygons _painted = intersection_ex(_orig, _useful_masks[k]);
                        const size_t _np = _painted.size();
                        for (auto& e : _painted) {
                            surface_fill.expolygons.push_back(std::move(e));
                            _fp_tag.push_back(_useful_slots[k]);
                        }
                        if (k) _clip_log << ",";
                        _clip_log << _useful_slots[k] << ":" << _np;
                    }
                    ExPolygons _natural = diff_ex(_orig, _all_masks_union);
                    const size_t _nn = _natural.size();
                    for (auto& e : _natural) {
                        surface_fill.expolygons.push_back(std::move(e));
                        _fp_tag.push_back(0);
                    }
                    _clip_log << "] natural=" << _nn;
                    NEOTKO_LOG(PROFILE, _clip_log.str());
                }
            }
        }
        size_t _fp_idx = 0;
        // NEOTKO_PROFILE_TAG_END

		for (ExPolygon& expoly : surface_fill.expolygons) {
            // NEOTKO_PROFILE_TAG — Fase 6c v2: slot id of this piece (-1 = no
            // pre-split ran, 0 = natural remainder, >0 = painted with that slot).
            const int _fp_slot_tag = (_fp_idx < _fp_tag.size()) ? _fp_tag[_fp_idx] : -1;
            ++_fp_idx;

      f->no_overlap_expolygons = intersection_ex(surface_fill.no_overlap_expolygons, ExPolygons() = {expoly}, ApplySafetyOffset::Yes);
            if (params.symmetric_infill_y_axis) {
                params.symmetric_y_axis = f->extended_object_bounding_box().center().x();
                expoly.symmetric_y(params.symmetric_y_axis);
            }

			// Spacing is modified by the filler to indicate adjustments. Reset it for each expolygon.
			f->spacing = surface_fill.params.spacing;
			surface_fill.surface.expolygon = std::move(expoly);

			if(surface_fill.params.bridge && surface_fill.surface.is_external() && surface_fill.params.density > 99.0){
				params.density = layerm->region().config().bridge_density.get_abs_value(1.0);
				params.dont_adjust = true;
			}
            if(surface_fill.surface.is_internal_bridge()){
                params.density = f->print_object_config->internal_bridge_density.get_abs_value(1.0);
                params.dont_adjust = true;
            }
			// NEOTKO_MULTIPASS_TAG_START — FASE 2: per-pass fill generation (Z stacking)
            // For MultiPass top/penultimate surfaces: call fill_surface_extrusion() N times,
            // once per active pass, each with its own fill angle (spacing unchanged).
            {
                const auto& mp_cfg   = layerm->region().config();
                MultiPassConfig     mp;
                SurfacePassStack    mp_stack;  // NEOTKO_SANDWICH_TAG — drives the FASE 2 loop
                bool                is_mp_fill = false;
                // NEOTKO_PROFILE_TAG_START — Fase F: painter-mode MP override.
                // If any model_part volume of this object has painted facets,
                // the painter is authoritative (Q1=A absoluto): preset MP is
                // suppressed and only profile.multipass payloads apply, per
                // dominant painted slot at this Z range.
                const ModelObject* _mp_mo = (this->object() != nullptr)
                    ? this->object()->model_object() : nullptr;
                const bool _mp_painter_mode = SurfaceColorMix::object_has_any_colormix_paint(_mp_mo);
                // NEOTKO_PAINT_COEXIST_TAG s91 — per-piece MMU governance.
                // Was: global is_mm_painted() short-circuit on the whole object.
                // Now: object-flag fast path + per-surface_piece XY check against
                // the MMU footprint at the role's vertical slab. Each piece in
                // surface_fill.expolygons (already pre-split per painted slot by
                // Fase 6c v2 via _fp_tag) is evaluated independently. MMU-owned
                // pieces are skipped; sandwich-eligible pieces emit normally.
                const bool _mp_mm_painted_obj =
                    (_mp_mo != nullptr && _mp_mo->is_mm_painted());
                bool _mp_mm_painted = false; // recomputed PER piece below
                if (_mp_mm_painted_obj &&
                    (surface_fill.params.extrusion_role == erTopSolidInfill ||
                     surface_fill.params.extrusion_role == erPenultimateInfill)) {
                    const double _z_lo =
                        (surface_fill.params.extrusion_role == erTopSolidInfill)
                            ? this->print_z - this->height : this->print_z;
                    const double _z_hi =
                        (surface_fill.params.extrusion_role == erTopSolidInfill)
                            ? this->print_z : this->print_z + this->height;
                    ExPolygons _one; _one.push_back(surface_fill.surface.expolygon);
                    _mp_mm_painted = SurfaceColorMix::mmu_governs_xy(
                        this->object(), _one, _z_lo, _z_hi);
                    if (_mp_mm_painted)
                        NEOTKO_LOG(MULTIPASS, "s91/coexist MMU_GOVERNS site=FILL"
                            << " z=" << this->print_z
                            << " role=" << (int)surface_fill.params.extrusion_role
                            << " fp_slot_tag=" << _fp_slot_tag
                            << " z_slab=[" << _z_lo << "," << _z_hi << "]"
                            << " → suppress sandwich for this piece (MMU owns)");
                    else
                        NEOTKO_LOG(MULTIPASS, "s91/coexist MMU_FREE site=FILL"
                            << " z=" << this->print_z
                            << " role=" << (int)surface_fill.params.extrusion_role
                            << " fp_slot_tag=" << _fp_slot_tag
                            << " → sandwich keeps applying (no MMU on this XY)");
                }
                // NEOTKO_PROFILE_TAG_END
                // NEOTKO_MULTIPASS_SURFACES_TAG — bifurcate enabled check by role:
                // Top surface uses multipass_enabled; Penultimate uses its own key.
                if (!surface_fill.params.bridge && !_mp_mm_painted &&
                    (surface_fill.params.extrusion_role == erTopSolidInfill ||
                     surface_fill.params.extrusion_role == erPenultimateInfill)) {
                    const ExtrusionRole _mp_role = surface_fill.params.extrusion_role;
                    // NEOTKO_COLORSTITCH_TAG — s112 diagnóstico: ¿llega un objeto
                    // pintado a la rama FASE-2 painter? Vuelca mo + painter_mode + rol.
                    NEOTKO_LOG(PROFILE, "FILL_MP z=" << this->print_z
                        << " role=" << (int)_mp_role
                        << " painter_mode=" << (_mp_painter_mode ? "1" : "0")
                        << " mo=" << (const void*)_mp_mo
                        << " obj='" << (_mp_mo ? _mp_mo->name : "<null>") << "'"
                        << " fp_slot_tag=" << _fp_slot_tag);
                    // NEOTKO_PROFILE_TAG_START — Fase 6b: painter = pure applicator
                    // of the authoritative SurfacePassStack. The painter no longer
                    // reads the legacy 3 payloads (multipass/pathblend/colormix);
                    // it resolves the stack the SandwichDialog/SCM dialog saved
                    // (stack_top_json / stack_penu_json) and feeds it to the SAME
                    // FASE-2 sublayer engine as preset mode. So a painted profile
                    // now applies a full multi-pass sandwich (Solid/ColorMix/
                    // PathBlend, N passes, per-pass gradient via pass.colormix.kv),
                    // not just a single effect. CLEAN CUT: profiles with an empty
                    // stack (v1 3mf, or saved from a standalone MP/PB dialog that
                    // does not snapshot a stack) no longer apply until re-saved.
                    if (_mp_painter_mode) {
                        const PrintObject* _po = this->object();
                        // NEOTKO_PROFILE_TAG — Fase 6c v2: prefer the slot from the
                        // pre-split tag (multi-slot aware: each painted piece carries
                        // its OWN slot id). When pre-split didn't run (no paint in
                        // this surface's band), fall back to dominant_slot — and a
                        // 0 tag means natural remainder, no effect.
                        const int _slot = (_fp_slot_tag > 0)
                            ? _fp_slot_tag
                            : (_fp_slot_tag == 0 ? 0
                                : ((_mp_role == erTopSolidInfill)
                                    // NEOTKO_COLORSTITCH_TAG — s112 fix: +1 capa arriba
                                    // (top) para alcanzar el tope de malla entre capas.
                                    ? SurfaceColorMix::dominant_painted_slot_in_z_range(
                                          _po, this->print_z - this->height, this->print_z + this->height)
                                    // penu ancla 1 capa más abajo → +2 capas arriba.
                                    : SurfaceColorMix::dominant_painted_slot_in_z_range(
                                          _po, this->print_z, this->print_z + 2.0 * this->height)));
                        if (_slot > 0) {
                            const int _pid = SurfaceColorMix::profile_id_for_slot(_po, _slot);
                            const auto* _p = Slic3r::SurfaceEffectProfileManager::get().find(_pid);
                            if (_p) {
                                const std::string& _js = (_mp_role == erTopSolidInfill)
                                    ? _p->stack_top_json : _p->stack_penu_json;
                                SurfacePassStack _st = SurfacePassStack::from_json(_js);
                                // NEOTKO_SANDWICH_TAG s119 (EMPTY model): the painted
                                // CONTENT is the only source of truth. Engage iff the
                                // zone has a real effect (any non-None pass) — NOT the
                                // legacy `enabled` gate. A [None] (Empty) zone suppresses
                                // → vanilla. This removes the gate that made two regions
                                // painted identically diverge (one enabled, one not).
                                if (_st.any_effect()) {
                                    mp_stack    = std::move(_st);
                                    is_mp_fill  = true;
                                    NEOTKO_LOG(PROFILE, "STACK_OVERRIDE layer=" << f->layer_id
                                        << " z=" << this->print_z
                                        << " role=" << (int)_mp_role
                                        << " profile='" << _p->name << "'"
                                        << " passes=" << mp_stack.passes.size());
                                } else {
                                    NEOTKO_LOG(PROFILE, "STACK_SUPPRESS layer=" << f->layer_id
                                        << " role=" << (int)_mp_role
                                        << " profile='" << _p->name << "'"
                                        << " (empty/disabled stack for this role)");
                                }
                            }
                        }
                        // else: unpainted area of a painted object → painter
                        // absoluto says no effect here. is_mp_fill stays false.
                    } else {
                        // NEOTKO_SANDWICH_TAG_START — preset mode: resolve the
                        // pass stack (blob, or synthesize_from_legacy if empty).
                        // Fase 2: a Solid/ColorMix/PathBlend stack drives FASE-2
                        // sublayers (the loop dispatches per pass kind).
                        // NEOTKO_PATHBLEND_TAG — Fase 5 s77 migración: PathBlend
                        // passes NO longer fall through to the legacy GCode engine.
                        // A PathBlend pass is compiled into ramp(+cap) sublayers in
                        // the band loop below, exactly like Solid/ColorMix passes —
                        // so its scheduling joins the proven single-tool-per-sublayer
                        // model and the legacy ToolOrdering/extrude_path PB special
                        // cases are deleted (no more "append_tcr unexpected" crashes).
                        const SurfacePassStack _stack =
                            SurfacePassStack::resolve(mp_cfg, _mp_role);
                        // NEOTKO_SANDWICH_TAG s119 (EMPTY model): drive by content, not
                        // the legacy `enabled` gate. A [None] (Empty) zone → vanilla.
                        if (_stack.any_effect()) {
                            mp_stack   = _stack;
                            is_mp_fill = true;
                        }
                        // NEOTKO_SANDWICH_TAG_END
                    }
                    // NEOTKO_PROFILE_TAG_END
                }
                // NEOTKO_COLORMIX_TAG_START — Zone + filament filter (FASE2 MultiPass)
                // NEOTKO_PROFILE_TAG — Fase F: zone+filter gates are preset-mode only.
                // In painter mode the painted slot lookup already encodes "which area"
                // (Q1=A absoluto) so preset zone/filter must not double-gate.
                if (is_mp_fill && !_mp_painter_mode) {
                    const ExtrusionRole _role = surface_fill.params.extrusion_role;
                    const int _zone = (_role == erTopSolidInfill)
                        ? mp_cfg.interlayer_colormix_top_zone.value
                        : mp_cfg.interlayer_colormix_penu_zone.value;
                    if (_zone == 1) {
                        const bool _in_zone = (_role == erTopSolidInfill)
                            ? (this->upper_layer == nullptr)
                            : (this->upper_layer != nullptr && this->upper_layer->upper_layer == nullptr);
                        if (!_in_zone) is_mp_fill = false;
                    }
                    if (is_mp_fill) {
                        const int _ff = mp_cfg.interlayer_colormix_filament_filter.value;
                        if (_ff > 0 && mp_cfg.solid_infill_filament.value != _ff)
                            is_mp_fill = false;
                    }
                }
                // NEOTKO_COLORMIX_TAG_END

                // NEOTKO_MULTIPASS_MINLAYER_TAG — MultiPass forbidden on first object layer.
                // Guarantees at least one real layer has called next_layer() before sublayer
                // handler runs → m_layer_idx is valid for Local-Z prime slot lookup.
                if (is_mp_fill) {
                    const size_t layer_idx_in_object =
                        this->id() - this->object()->get_layer(0)->id();
                    if (layer_idx_in_object == 0) {
                        is_mp_fill = false;
                        BOOST_LOG_TRIVIAL(warning)
                            << "[MultiPass] Disabled on first object layer (layer_height="
                            << this->height << " mm). MultiPass starts from layer 2.";
                        NEOTKO_LOG(MULTIPASS, "FASE2_CHECK: is_mp_fill forced=false (first layer hardlock)");
                    }
                }

                // NEOTKO_SANDWICH_TAG — the painter branch (legacy bridge) builds
                // a MultiPassConfig; convert it to an all-Solid stack so the loop
                // below always iterates a SurfacePassStack.
                if (is_mp_fill && mp_stack.passes.empty())
                    mp_stack = SurfacePassStack::from_multipass_config(mp);

                NEOTKO_LOG(MULTIPASS, "FASE2_CHECK layer=" << f->layer_id
                    << " print_z=" << this->print_z
                    << " role=" << (int)surface_fill.params.extrusion_role
                    << " bridge=" << surface_fill.params.bridge
                    << " mp_enabled=" << mp_cfg.multipass_enabled.value
                    << " is_mp_fill=" << is_mp_fill);

                if (is_mp_fill) {
                    // NEOTKO_SANDWICH_TAG_START — Fase 2: the FASE 2 loop iterates
                    // the SurfacePassStack and compiles each band into sublayers:
                    //   Solid / None → one mono-tool sublayer (None = passthrough,
                    //                  prints with the object's natural surface tool)
                    //   ColorMix     → assign_and_group_tools encodes tools, then
                    //                  eec_to_tool_buckets splits the lámina into N
                    //                  per-tool buckets — each emitted as its own
                    //                  single-tool sublayer (shaped like a MultiPass
                    //                  pass: distinct print_z, monotonic pass_idx) so
                    //                  NeoTower / ToolOrdering / the GCode handler
                    //                  schedule them with zero changes.
                    //   PathBlend    → never reaches here (preset gate excludes it).
                    const int   n          = std::min<int>(3, (int)mp_stack.passes.size());
                    const float base_angle = surface_fill.params.angle;
                    // Beer-Lambert stadium model: A(W,H) = H*(W - H*(1-π/4)) [matches Flow.cpp]
                    constexpr double k_mp  = 1.0 - 0.25 * M_PI;
                    auto& sublayer_slot    = this->object()->multipass_sublayers()[this->id()];
                    const ExtrusionRole _sub_role = surface_fill.params.extrusion_role;
                    // Natural surface tool (0-based): used by None passes (passthrough)
                    // and as the ColorMix unencoded-path fallback bucket.
                    const int natural_tool = std::max(0, mp_cfg.solid_infill_filament.value - 1);
                    // NEOTKO_SANDWICH_TAG — monotonic sublayer index across the
                    // whole layer. NeoTower chains sublayers by (obj,layer,pass_idx);
                    // a ColorMix band emits several sublayers, so pass_idx must be
                    // a running counter, not the band index.
                    int global_pass = 0;

                    for (int i = 0; i < n; ++i) {
                        const SurfacePass&    pass = mp_stack.passes[i];
                        const SurfacePassKind kind = pass.kind;

                        // NEOTKO_PATHBLEND_TAG — Fase 5 s77 migración: compile a
                        // PathBlend pass into ramp(+cap) single-tool sublayers.
                        // Geometry is UNCHANGED — the GCode handler routes each
                        // sublayer's fills through PathBlendEngine::apply_path
                        // (variable-Z ramp / flat complementary cap) exactly as the
                        // legacy engine did. Only the SCHEDULING changes: one tool
                        // per sublayer with its own print_z → wipe-tower sync is
                        // structural (no has_pathblend_chain / body_equals_cap /
                        // body_needs_return / std::rotate special cases).
                        if (kind == SurfacePassKind::PathBlend) {
                            const std::string _blob = pass.pathblend.kv.count("blob")
                                ? pass.pathblend.kv.at("blob") : std::string();
                            const PathBlendPassConfig pb = PathBlendPassConfig::from_blob_json(_blob);
                            // NEOTKO_PATHBLEND_TAG — Fase 5 s78 fix: do NOT gate on
                            // pb.enabled here. `enabled` is a shared scope key
                            // (multipass_path_gradient) intentionally NOT serialized
                            // into the per-zone blob (see to_blob_json /
                            // from_region_config). from_blob_json() therefore always
                            // returns enabled=false, so `!pb.enabled` was ALWAYS true
                            // → every PathBlend pass skipped → empty top layer → empty
                            // wipe tower → EXC_BAD_ACCESS in
                            // update_print_stats_and_format_filament_stats
                            // (used_filament[id] on an empty vector). Reaching this
                            // branch with kind==PathBlend already proves PB is enabled
                            // (synthesize_from_legacy / the painter only emit a
                            // PathBlend pass when the enable flag is set).
                            if (pb.num_passes < 1 || pb.tool_bottom < 0) continue;

                            // Fill angle: PathBlend override or the natural surface angle.
                            f->angle = (pb.fill_angle >= 0)
                                ? Geometry::deg2rad(static_cast<float>(pb.fill_angle))
                                : base_angle;

                            // NEOTKO_PATHBLEND_TAG_START — s87 B-bands gate.
                            // When ON, discretise the PB pass into K real
                            // micro-layers (ramp) + matching cap-bands. Each
                            // band is masked by its t-strip in Y world coords
                            // (matches the surface_t Y-bbox convention used by
                            // extrude_path in GCode.cpp), regenerated with a
                            // Flow.with_height(h_step) — its own respaced infill
                            // — and emitted as a regular single-tool sublayer
                            // (pathblend_pass = -1 → dispatcher takes the plain
                            // Solid branch, no apply_path scaling). When OFF or
                            // when compute_pb_bands returns empty (band thinner
                            // than min_band_h) the LEGACY single-Fill ramp/cap
                            // path below runs unchanged.
                            static constexpr bool kEnablePBBands = true;
                            // Minimum band height — safety floor for the bead geometry.
                            // Lower values open the door to more bands (finer staircase),
                            // but anything under ~0.04mm is at the edge of printable for a
                            // 0.4mm nozzle. Setting it to 0.003 to allow K up to ~46 with
                            // floor=0.01, mid_end=0.15 — the actual K is then capped by
                            // kTargetK below for testing.
                            static constexpr double kMinBandH    = 0.01;
                            // Target band count. 0 = let compute_pb_bands decide from
                            // min_band_h. >0 = explicit number of staircase steps for
                            // visual fidelity vs printability tests. Set 2-3 for "real"
                            // staircase, 32 to approximate the legacy 32-path look.
                            static constexpr int    kTargetK     = 32;
                            std::vector<PBBand> _bands;
                            if (kEnablePBBands) {
                                _bands = compute_pb_bands(
                                    pb, this->bottom_z(), double(this->height),
                                    kMinBandH, /*want_cap=*/(pb.tool_top >= 0 && pb.num_passes >= 2),
                                    /*target_k=*/kTargetK);
                            }
                            // NEOTKO_PATHBLEND_TAG — s87 OPCIÓN B: partición por Y-centroide.
                            //
                            // Arquitectura del modelo B (final):
                            //
                            // RAMPA:
                            //   - UN único Fill global sobre el ExPolygon ENTERO de la
                            //     superficie, con Flow.with_height(h_step). El infill se
                            //     genera coherente (un solo bbox, una sola fase).
                            //   - Los paths resultantes se PARTICIONAN por su Y-centroide
                            //     en K subconjuntos. Cada subconjunto pertenece a UNA
                            //     banda rampa.
                            //   - Cada banda → un sublayer real a su z_top con sus paths.
                            //     Sin masking, sin sub-Fills independientes, sin artefactos
                            //     de fase (que es lo que el viejo strip-masking producía y
                            //     ahora vive separado como FillMicroStitch).
                            //
                            // TAPA (Full mode):
                            //   - UN único Fill global con la Flow nominal (W,H).
                            //   - Por cada path, calcular h_cap_at_t = H - ramp(t) según
                            //     su Y-centroide, y escalar path.mm3_per_mm * (h_cap/H).
                            //   - Todos los paths → UN solo sublayer a Z=nominal con tool
                            //     pb.tool_top. La "cuña residual" se cubre con flow por path
                            //     que varía con la posición.
                            //
                            // El resultado: K escalones reales ascendentes (rampa) + una
                            // tapa única que rellena el residual variable. Sin gaps por
                            // mask-induced phase mismatch.
                            if (!_bands.empty()) {
                                // NEOTKO_MULTIPASS_TAG — Perimeter Override anchor: record
                                // the sublayer count before this PB band so the cloned
                                // perimeter can attach to the last PB sublayer pushed here
                                // (cap for Full, topmost ramp for Half). Additive only —
                                // does NOT touch the ramp/cap staircase geometry.
                                const size_t _pb_slot_base = sublayer_slot.size();
                                const ExPolygon  _src_exp = surface_fill.surface.expolygon;
                                const BoundingBox _src_bb = _src_exp.contour.bounding_box();
                                const coord_t _ymin = _src_bb.min.y();
                                const coord_t _ymax = _src_bb.max.y();
                                const double  _yspan = double(_ymax - _ymin);
                                const Flow    _nominal_flow = params.flow;
                                const double  _pb_band_top_sched = this->bottom_z() + this->height - 2.0 * EPSILON;
                                int _band_local_idx = 0;

                                // Helper: extrae Y-centroide de un path como t∈[0,1] del bbox.
                                auto _t_of = [&](const ExtrusionEntity* e) -> double {
                                    const auto* p = dynamic_cast<const ExtrusionPath*>(e);
                                    if (!p || p->polyline.points.empty() || _yspan <= 0.0) return 0.5;
                                    double sum = 0.0;
                                    for (const auto& pt : p->polyline.points) sum += double(pt.y());
                                    const double cy = sum / double(p->polyline.points.size());
                                    return std::clamp((cy - double(_ymin)) / _yspan, 0.0, 1.0);
                                };
                                // Helper: walk nested EECs, calling visit() on each leaf ExtrusionPath.
                                std::function<void(const ExtrusionEntity*, const std::function<void(const ExtrusionPath*)>&)>
                                    _walk_paths = [&](const ExtrusionEntity* e,
                                                      const std::function<void(const ExtrusionPath*)>& visit) {
                                    if (!e) return;
                                    if (const auto* coll = dynamic_cast<const ExtrusionEntityCollection*>(e)) {
                                        for (const ExtrusionEntity* ee : coll->entities) _walk_paths(ee, visit);
                                    } else if (const auto* p = dynamic_cast<const ExtrusionPath*>(e)) {
                                        visit(p);
                                    }
                                };

                                // Separa bandas rampa de bandas tapa (preservando orden de emisión).
                                std::vector<const PBBand*> _ramp_bands, _cap_bands;
                                for (const PBBand& b : _bands)
                                    (b.is_cap ? _cap_bands : _ramp_bands).push_back(&b);

                                // ============ RAMPA (per-scanline staircase) ============
                                // NEOTKO_PATHBLEND_TAG — s88 rewrite. Modelo simétrico a la
                                // tapa: en lugar de UN Z y per-path flow variable (tapa),
                                // aquí UN bead Z propio por scanline y per-path flow
                                // variable. Cada path es su propio sublayer (o agrupado con
                                // sus simétricos al mismo Z):
                                //   h_p = floor + t_p · (mid_end − floor)   espesor propio
                                //   z_p = bottom_z + h_p                    nozzle Z = techo
                                //   mm3_per_mm *= h_p / H                   flow proporcional
                                // El bead se apoya en la capa previa (bottom_z) y crece hasta
                                // z_p; los beads adyacentes en Y forman un staircase real.
                                // Volumen rampa(h_p)+tapa(H−h_p)=H ⇒ conservación per Y.
                                // Orden: ascendente por t → empieza en el punto más bajo y
                                // termina en el más alto, como pidió el usuario.
                                if (pb.tool_bottom >= 0) {
                                    FillParams _ramp_params = params;
                                    _ramp_params.flow = _nominal_flow;
                                    f->spacing = _nominal_flow.spacing();

                                    ExtrusionEntityCollection _ramp_global;
                                    Surface _ms = surface_fill.surface;
                                    _ms.expolygon = _src_exp;
                                    f->fill_surface_extrusion(&_ms, _ramp_params, _ramp_global.entities);

                                    const double _H_d = double(this->height);
                                    const float _floor_pb = std::max(0.01f, pb.floor_mm);
                                    // mid_end_mm <0 ⇒ auto: resolve to the tallest legal ramp
                                    // before the min-clamp (else a default PB yields a negative
                                    // range and the staircase collapses flat).
                                    const float _mid_pref_pb = (pb.mid_end_mm < 0.f)
                                        ? ((pb.mode == PathBlendPassConfig::Mode::Full)
                                               ? static_cast<float>(_H_d - 0.04) : static_cast<float>(_H_d))
                                        : pb.mid_end_mm;
                                    const float _mid_end_pb = (pb.mode == PathBlendPassConfig::Mode::Full)
                                        ? std::min(_mid_pref_pb, static_cast<float>(_H_d - 0.04))
                                        : std::min(_mid_pref_pb, static_cast<float>(_H_d));
                                    const double _range_pb = double(_mid_end_pb) - double(_floor_pb);
                                    const double _base_z = this->bottom_z();

                                    struct ScanPath { double t; double z; double h; ExtrusionPath* p; };
                                    std::vector<ScanPath> _scans;
                                    auto _collect_ramp = [&](const ExtrusionPath* p) {
                                        const double t   = _t_of(p);
                                        const double h_p = double(_floor_pb) + t * _range_pb;
                                        const double z_p = _base_z + h_p;
                                        const double ratio = (_H_d > 0.0) ? (h_p / _H_d) : 1.0;
                                        ExtrusionPath* cl = dynamic_cast<ExtrusionPath*>(p->clone());
                                        if (!cl) return;
                                        cl->mm3_per_mm = float(cl->mm3_per_mm * ratio);
                                        cl->height     = float(h_p);
                                        _scans.push_back({t, z_p, h_p, cl});
                                    };
                                    for (const ExtrusionEntity* e : _ramp_global.entities) _walk_paths(e, _collect_ramp);

                                    std::sort(_scans.begin(), _scans.end(),
                                              [](const ScanPath& a, const ScanPath& b){ return a.t < b.t; });

                                    // Agrupa paths con Z prácticamente idéntico (par ida/vuelta
                                    // del rectilinear cae al mismo t) en un solo sublayer.
                                    constexpr double Z_QUANT = 1e-4; // 0.1 µm
                                    const size_t _total = _scans.size();
                                    size_t i = 0;
                                    while (i < _total) {
                                        size_t j = i + 1;
                                        const double z_ref = _scans[i].z;
                                        while (j < _total && std::abs(_scans[j].z - z_ref) < Z_QUANT) ++j;

                                        MultiPassSubLayer _sub;
                                        // print_z monotonic ascendente — wipe tower/ToolOrdering
                                        // necesita orden estricto; los sublayers reales se
                                        // distinguen por real_extrude_z (Z física del nozzle).
                                        // NEOTKO_PATHBLEND_TAG — s88. Step compressed from 1e-6
                                        // to 1e-7 so that 32 scanlines + cap span < NT_WT_EPS
                                        // (1e-5). Otherwise NeoTower splits the PB chain into
                                        // multiple wt2_li groups and generate() can't match
                                        // the cap's plan slot to raw_result. Span at i=0 is
                                        // (_total + 1) * 1e-7 ≈ 3.3e-6 mm, well below EPS.
                                        _sub.print_z        = _pb_band_top_sched - double(_total - i) * 1e-7;
                                        _sub.height         = _scans[i].h;
                                        _sub.real_extrude_z = z_ref;
                                        _sub.pass_idx       = global_pass++;
                                        _sub.role           = _sub_role;
                                        _sub.effect         = SurfacePassKind::PathBlend;
                                        _sub.tool_id        = pb.tool_bottom;
                                        _sub.speed_pct      = 100;
                                        _sub.pathblend_pass = -1;
                                        _sub.pathblend_blob.clear();
                                        for (size_t k = i; k < j; ++k)
                                            _sub.fills.entities.push_back(_scans[k].p);
                                        sublayer_slot.push_back(std::move(_sub));
                                        ++_band_local_idx;
                                        NEOTKO_LOG(MULTIPASS, "  EMIT_PB_RAMP_SCAN layer=" << f->layer_id
                                            << " idx=" << i << "-" << (j-1)
                                            << " t=" << _scans[i].t
                                            << " real_z=" << z_ref
                                            << " h=" << _scans[i].h
                                            << " tool=T" << pb.tool_bottom
                                            << " pass=" << (global_pass - 1)
                                            << " paths=" << (j - i));
                                        i = j;
                                    }
                                }

                                // ============ TAPA (Full mode) ============
                                if (!_cap_bands.empty() && pb.tool_top >= 0) {
                                    FillParams _cap_params = params;
                                    _cap_params.flow = _nominal_flow;
                                    f->spacing = _nominal_flow.spacing();

                                    ExtrusionEntityCollection _cap_global;
                                    Surface _ms = surface_fill.surface;
                                    _ms.expolygon = _src_exp;
                                    f->fill_surface_extrusion(&_ms, _cap_params, _cap_global.entities);

                                    const double _H_d = double(this->height);
                                    const float _floor_pb   = std::max(0.01f, pb.floor_mm);
                                    // mid_end_mm <0 ⇒ auto: resolve to tallest legal ramp first.
                                    const float _mid_pref_pb = (pb.mid_end_mm < 0.f)
                                        ? ((pb.mode == PathBlendPassConfig::Mode::Full)
                                               ? static_cast<float>(_H_d - 0.04) : static_cast<float>(_H_d))
                                        : pb.mid_end_mm;
                                    const float _mid_end_pb = (pb.mode == PathBlendPassConfig::Mode::Full)
                                        ? std::min(_mid_pref_pb, static_cast<float>(_H_d - 0.04))
                                        : std::min(_mid_pref_pb, static_cast<float>(_H_d));

                                    ExtrusionEntityCollection _cap_collected;
                                    auto _scale_and_collect = [&](const ExtrusionPath* p) {
                                        const double t = _t_of(p);
                                        const double ramp_t = double(_floor_pb) + t * double(_mid_end_pb - _floor_pb);
                                        const double h_cap  = std::max(0.04, _H_d - ramp_t);
                                        const double ratio  = h_cap / _H_d;
                                        ExtrusionPath* cl = dynamic_cast<ExtrusionPath*>(p->clone());
                                        if (!cl) return;
                                        cl->mm3_per_mm = float(cl->mm3_per_mm * ratio);
                                        cl->height     = float(h_cap);
                                        _cap_collected.entities.push_back(cl);
                                    };
                                    for (const ExtrusionEntity* e : _cap_global.entities) _walk_paths(e, _scale_and_collect);

                                    if (!_cap_collected.entities.empty()) {
                                        MultiPassSubLayer _sub;
                                        // Cap is always the LAST sublayer of the PB pass —
                                        // anchor at _pb_band_top_sched, ramp sublayers fall
                                        // strictly below (their print_z uses _total-i > 0).
                                        _sub.print_z        = _pb_band_top_sched;
                                        _sub.height         = double(this->height);
                                        _sub.real_extrude_z = this->bottom_z() + _H_d; // Z=nominal
                                        _sub.pass_idx       = global_pass++;
                                        _sub.role           = _sub_role;
                                        _sub.effect         = SurfacePassKind::PathBlend;
                                        _sub.tool_id        = pb.tool_top;
                                        _sub.speed_pct      = 100;
                                        _sub.pathblend_pass = -1;
                                        _sub.pathblend_blob.clear();
                                        _sub.fills          = std::move(_cap_collected);
                                        sublayer_slot.push_back(std::move(_sub));
                                        ++_band_local_idx;
                                        NEOTKO_LOG(MULTIPASS, "  EMIT_PB_CAP layer=" << f->layer_id
                                            << " real_z=" << _sub.real_extrude_z
                                            << " tool=T" << pb.tool_top
                                            << " pass=" << (global_pass - 1)
                                            << " spacing=" << _nominal_flow.spacing()
                                            << " paths=" << _sub.fills.entities.size());
                                    }
                                }

                                // NEOTKO_MULTIPASS_TAG — Perimeter Override for PathBlend.
                                // perimeter_override suppresses the real-layer perimeter
                                // (GCode MP_PERIM_SUPPRESS); without a replacement the PB
                                // surface loses its contour. The perimeter is a structural
                                // wall → debe construirse a la altura REAL de la zona (Z
                                // NOMINAL, full height), NO a la altura del path-half.
                                //   Full: el cap ya imprime a Z nominal → adjuntar ahí.
                                //   Half: NO hay cap; el último sublayer es el tope de rampa
                                //   (real_extrude_z = ramp_end, sub-nominal). Adjuntar ahí
                                //   dejaría el perímetro corto. → sintetizamos un sublayer
                                //   PORTADOR de SOLO perímetro a Z nominal (tool_bottom),
                                //   sin fills, para que el contorno cierre la capa.
                                const bool _pb_perim = _mp_painter_mode
                                    ? mp_stack.perimeter_override
                                    : mp_cfg.multipass_perimeter_override.value;
                                if (_pb_perim && i == n - 1 && sublayer_slot.size() > _pb_slot_base) {
                                    const double _nominal_z = this->bottom_z() + double(this->height);
                                    const bool _back_at_nominal =
                                        sublayer_slot.back().real_extrude_z >= _nominal_z - 1e-4;
                                    if (_back_at_nominal) {
                                        // Full (cap a Z nominal) — adjuntar al cap.
                                        MultiPassSubLayer& _anchor = sublayer_slot.back();
                                        for (const LayerRegion* lr : this->regions())
                                            for (const ExtrusionEntity* pe : lr->perimeters.entities)
                                                _anchor.perimeters.entities.push_back(pe->clone());
                                        NEOTKO_LOG(MULTIPASS, "  SUBLAYER PB perimeters cloned (cap@nominal): "
                                            << _anchor.perimeters.entities.size()
                                            << " → tool T" << _anchor.tool_id << " z=" << _anchor.print_z);
                                    } else {
                                        // Half (sin cap) — sublayer portador a Z nominal.
                                        MultiPassSubLayer _psub;
                                        _psub.print_z        = _pb_band_top_sched; // tras todas las rampas
                                        _psub.height         = double(this->height);
                                        _psub.real_extrude_z = _nominal_z;         // perímetro a altura real
                                        _psub.pass_idx       = global_pass++;
                                        _psub.role           = _sub_role;
                                        _psub.effect         = SurfacePassKind::PathBlend;
                                        _psub.tool_id        = pb.tool_bottom;     // color de la superficie
                                        _psub.speed_pct      = 100;
                                        _psub.pathblend_pass = -1;                 // plain dispatch, sin apply_path
                                        _psub.pathblend_blob.clear();
                                        for (const LayerRegion* lr : this->regions())
                                            for (const ExtrusionEntity* pe : lr->perimeters.entities)
                                                _psub.perimeters.entities.push_back(pe->clone());
                                        NEOTKO_LOG(MULTIPASS, "  SUBLAYER PB perimeters carrier (half@nominal): "
                                            << _psub.perimeters.entities.size()
                                            << " → tool T" << _psub.tool_id << " real_z=" << _psub.real_extrude_z);
                                        sublayer_slot.push_back(std::move(_psub));
                                        ++_band_local_idx;
                                    }
                                }

                                // Restore surface_fill.surface state.
                                surface_fill.surface.expolygon = _src_exp;
                                continue;
                            }
                            // NEOTKO_PATHBLEND_TAG_END — fall through to legacy.

                            // Generate the surface fill once (NO Beer-Lambert ratio
                            // scaling — apply_path derives flow from ramp_thickness/H,
                            // overriding the path's mm3_per_mm).
                            ExtrusionEntityCollection pb_src;
                            f->fill_surface_extrusion(&surface_fill.surface, params, pb_src.entities);
                            if (pb_src.entities.empty()) continue;

                            auto _clone_into = [](const ExtrusionEntityCollection& s,
                                                  ExtrusionEntityCollection& d) {
                                for (const ExtrusionEntity* e : s.entities)
                                    if (e) d.entities.push_back(e->clone());
                            };

                            // Scheduling Z: ramp/cap as two co-planar buckets just
                            // below nominal (mirror the ColorMix 1 µm bucket step).
                            // apply_path sets the REAL extrusion Z (ramp variable, cap
                            // nominal); print_z here is only the LayerTools key + the
                            // ramp-before-cap ordering.
                            const double pb_band_top = this->bottom_z() + this->height - 2.0 * EPSILON;
                            const float  pb_height   = float(this->height);
                            const int    pb_npasses  = (pb.tool_top >= 0 && pb.num_passes >= 2) ? 2 : 1;

                            for (int pp = 0; pp < pb_npasses; ++pp) {
                                const int pb_tool = (pp == 0) ? pb.tool_bottom : pb.tool_top;
                                if (pb_tool < 0) continue;
                                MultiPassSubLayer sub;
                                sub.print_z        = (pp == 0) ? (pb_band_top - 1e-3) : pb_band_top;
                                sub.height         = pb_height;
                                sub.pass_idx       = global_pass++;
                                sub.role           = _sub_role;
                                sub.effect         = SurfacePassKind::PathBlend;
                                sub.tool_id        = pb_tool;
                                sub.speed_pct      = 100;
                                sub.pathblend_pass = pp;     // 0 = ramp, 1 = cap
                                sub.pathblend_blob = _blob;  // decoded back into pb at dispatch
                                if (pp == pb_npasses - 1)
                                    sub.fills = std::move(pb_src);   // last takes ownership
                                else
                                    _clone_into(pb_src, sub.fills);
                                sublayer_slot.push_back(std::move(sub));
                            }
                            // NEOTKO_MULTIPASS_TAG — Perimeter Override (legacy PB path):
                            // attach the suppressed perimeter to the last pass pushed
                            // (cap for Full, ramp for Half). Mirrors the staircase block.
                            {
                                const bool _pb_perim = _mp_painter_mode
                                    ? mp_stack.perimeter_override
                                    : mp_cfg.multipass_perimeter_override.value;
                                if (_pb_perim && i == n - 1 && !sublayer_slot.empty()
                                    && sublayer_slot.back().effect == SurfacePassKind::PathBlend) {
                                    MultiPassSubLayer& _anchor = sublayer_slot.back();
                                    for (const LayerRegion* lr : this->regions())
                                        for (const ExtrusionEntity* pe : lr->perimeters.entities) {
                                            ExtrusionEntity* cloned = pe->clone();
                                            neotko_mp_scale_perim(cloned, 1.0, k_mp);
                                            _anchor.perimeters.entities.push_back(cloned);
                                        }
                                }
                            }
                            NEOTKO_LOG(MULTIPASS, "  SANDWICH PathBlend z=" << pb_band_top
                                << " mode=" << (pb_npasses == 2 ? "full" : "half")
                                << " tools=[T" << pb.tool_bottom
                                << (pb_npasses == 2 ? ",T" + std::to_string(pb.tool_top) : "")
                                << "] pass_idx=" << (global_pass - pb_npasses)
                                << ".." << (global_pass - 1)
                                << " slot_tag=" << _fp_slot_tag);
                            // NEOTKO_SANDWICH_DEBUG — per-sublayer trace (s79j+): one line per
                            // pushed sublayer so we can see EXACTLY what landed in the queue
                            // before the coalescer. Greppable: "EMIT_SUB".
                            for (int _pp = 0; _pp < pb_npasses; ++_pp) {
                                const MultiPassSubLayer& _ss = sublayer_slot[sublayer_slot.size() - pb_npasses + _pp];
                                NEOTKO_LOG(MULTIPASS, "  EMIT_SUB layer=" << f->layer_id
                                    << " z=" << _ss.print_z
                                    << " tool=T" << _ss.tool_id
                                    << " pass=" << _ss.pass_idx
                                    << " role=" << (int)_ss.role
                                    << " effect=PB pb_pass=" << _ss.pathblend_pass
                                    << " slot_tag=" << _fp_slot_tag
                                    << " fills=" << _ss.fills.entities.size());
                            }
                            continue;
                        }

                        const bool is_cm   = (kind == SurfacePassKind::ColorMix);
                        const bool is_none = (kind == SurfacePassKind::None);
                        // None prints with the object's natural surface tool.
                        const int  solid_tool = is_none ? natural_tool : pass.solid_tool;
                        // Disabled Solid pass (tool<0): skip generation — its ratio
                        // still counts in the cumulative Z sum (byte-equivalent to
                        // a classic MultiPass run with a disabled middle pass).
                        if (!is_cm && solid_tool < 0) continue;

                        const double ratio = pass.ratio;

                        // NEOTKO_MULTIPASS_MINLAYER_TAG — skip passes thinner than minimum printable height.
                        {
                            const float sub_height_mm = float(this->height * ratio);
                            if (sub_height_mm < 0.04f) {
                                BOOST_LOG_TRIVIAL(warning) << "NEOTKO_MULTIPASS_MINLAYER: pass " << i
                                    << " sub_height=" << sub_height_mm << "mm < 0.04mm — skipping";
                                continue;
                            }
                        }
                        // NEOTKO_MULTIPASS_MINLAYER_TAG

                        // NEOTKO_SANDWICH_TAG_START — per-lámina ColorMix gradient.
                        // Each ColorMix pass may carry its own gradient config in
                        // pass.colormix.kv (interlayer_colormix_* keys, same names
                        // as the region config). When present we apply it over a
                        // COPY of the region config so two ColorMix láminas in the
                        // same zone can differ. Empty kv → fall back to the shared
                        // region config (byte-equivalent to the old behaviour).
                        // The override only affects this band's bucketing/angle;
                        // the buckets it yields become sub.tool_id, which is the
                        // single source ToolOrdering/NeoTower read → wipe-tower
                        // stays in sync with zero extra plumbing.
                        PrintRegionConfig          cm_cfg_override;
                        const PrintRegionConfig*   cm_eff = &mp_cfg;
                        if (is_cm && pass.colormix.present && !pass.colormix.kv.empty()) {
                            cm_cfg_override = mp_cfg;  // copy the region config
                            int _ok = 0, _skip = 0;
                            for (const auto& _kvp : pass.colormix.kv) {
                                try {
                                    cm_cfg_override.set_deserialize_strict(_kvp.first, _kvp.second);
                                    ++_ok;
                                } catch (const std::exception& _e) {
                                    ++_skip;
                                    NEOTKO_LOG(MULTIPASS, "  CM_OVERRIDE_SKIP band" << i
                                        << " key=" << _kvp.first << " val=" << _kvp.second
                                        << " err=" << _e.what());
                                }
                            }
                            // A ColorMix pass is enabled by definition (it sits in
                            // the stack as ColorMix); ensure assign_and_group_tools
                            // doesn't early-return on a stale legacy enable flag.
                            cm_cfg_override.interlayer_colormix_enabled.value = true;
                            cm_eff = &cm_cfg_override;
                            NEOTKO_LOG(MULTIPASS, "  CM_OVERRIDE band" << i
                                << " role=" << (int)_sub_role
                                << " applied=" << _ok << " skipped=" << _skip);
                        }
                        // NEOTKO_SANDWICH_TAG_END

                        // Fill angle. ColorMix needs a consistent direction (its
                        // dither runs perpendicular to the lines, MonotonicLine);
                        // Solid alternates perpendicular per pass for bonding.
                        if (is_cm) {
                            const int cm_angle = (_sub_role == erTopSolidInfill)
                                ? cm_eff->interlayer_colormix_angle.value
                                : cm_eff->interlayer_colormix_penu_angle.value;
                            f->angle = (cm_angle >= 0)
                                ? Geometry::deg2rad(static_cast<float>(cm_angle))
                                : base_angle;
                        } else {
                            f->angle = (pass.angle >= 0)
                                ? Geometry::deg2rad(static_cast<float>(pass.angle))
                                : base_angle + float(i % 2) * float(M_PI / 2);
                        }

                        ExtrusionEntityCollection temp;
                        f->fill_surface_extrusion(&surface_fill.surface, params, temp.entities);
                        // Beer-Lambert stadium correction to the sublayer height.
                        for (auto* e : temp.entities) {
                            auto* coll = dynamic_cast<ExtrusionEntityCollection*>(e);
                            if (!coll) continue;
                            for (auto* ee : coll->entities) {
                                if (auto* path = dynamic_cast<ExtrusionPath*>(ee)) {
                                    const double W      = path->width;
                                    const double H      = path->height;
                                    const double H_sub  = H * ratio;
                                    const double A_orig = H     * (W - k_mp * H);
                                    const double A_sub  = H_sub * (W - k_mp * H_sub);
                                    if (A_orig > 1e-9 && W > H + 1e-6)
                                        path->mm3_per_mm = float(path->mm3_per_mm * (A_sub / A_orig));
                                    else
                                        path->mm3_per_mm = float(path->mm3_per_mm * ratio);
                                    path->height = float(H_sub);
                                }
                            }
                        }

                        // Z: the band top — where the nozzle prints a band of
                        // height ratio*H whose bottom is the cumulative Z below.
                        double cumsum = 0.0;
                        for (int pi = 0; pi <= i; ++pi) cumsum += mp_stack.passes[pi].ratio;
                        const bool   is_last_band = (i == n - 1);
                        // Float rounding in ratios (e.g. 0.396+0.417+0.188=1.001) makes
                        // cumsum slightly >1 → bottom_z + cumsum*h - 2ε = nominal_z exactly.
                        // Fix: last band always uses nominal_z-2ε directly, bypassing cumsum.
                        const double band_top_z = is_last_band
                            ? (this->bottom_z() + this->height - 2.0 * EPSILON)
                            : (this->bottom_z() + cumsum * this->height);
                        const float  band_height = float(this->height * ratio);

                        if (is_cm) {
                            // NEOTKO_SANDWICH_TAG — ColorMix lámina: encode the tools,
                            // decode into per-tool buckets, and emit ONE single-tool
                            // sublayer per bucket. Each bucket is shaped exactly like
                            // a MultiPass pass → NeoTower / ToolOrdering / the GCode
                            // handler schedule them with zero changes (their pass-chain
                            // + cross-product machinery already plans the toolchanges).
                            SurfaceColorMix::assign_and_group_tools(
                                temp, *cm_eff, _sub_role, int(f->layer_id),
                                /*allow_top=*/true, /*allow_penu=*/true,
                                // NEOTKO_SANDWICH_TAG s119 — ROOT FIX for "painted penu
                                // colormix doesn't slice unless the SandwichDialog penu
                                // 'Enabled' is checked". This FASE2 call omitted
                                // print_object + Z, so assign_and_group_tools could NOT
                                // detect PAINTER MODE and fell into PRESET mode, where
                                // should_process_role(Penultimate, surface) gates on the
                                // preset's interlayer_colormix_surface — which the penu
                                // 'Enabled' checkbox writes (top-only when disabled) →
                                // penu skipped → 1 bucket (no mix). Passing these (as the
                                // LayerRegion call site already does) enables painter
                                // mode → the painted profile drives the penu, ignoring
                                // the legacy preset surface gate.
                                /*mgr=*/nullptr, /*num_physical=*/0,
                                this->object(), this->print_z, this->height,
                                // NEOTKO_COLORSTITCH_TAG — cm_eff ya lleva el override
                                // per-pase (cuando se construyó cm_cfg_override); avisa a
                                // assign_and_group_tools para que NO lo pise con el payload
                                // colapsado del profile (tools del último pase del rol).
                                /*config_has_pass_override=*/(cm_eff == &cm_cfg_override));
                            auto buckets =
                                SurfaceColorMix::eec_to_tool_buckets(temp, natural_tool);
                            const int NB = (int)buckets.size();
                            // Buckets are co-planar (one lámina). Step print_z by 1 µm
                            // so each gets a distinct LayerTools entry / z_um key; the
                            // last bucket sits at the band top.
                            constexpr double kBucketZStep = 1e-3; // 1 µm
                            const int _pass0 = global_pass;
                            for (int k = 0; k < NB; ++k) {
                                MultiPassSubLayer sub;
                                sub.print_z   = band_top_z
                                    - double(NB - 1 - k) * kBucketZStep;
                                sub.height    = band_height;
                                sub.pass_idx  = global_pass++;
                                sub.role      = _sub_role;
                                sub.effect    = SurfacePassKind::ColorMix;
                                sub.tool_id   = buckets[k].first;
                                sub.speed_pct = 100;
                                sub.fills     = std::move(buckets[k].second);
                                sublayer_slot.push_back(std::move(sub));
                            }

                            // NEOTKO_MULTIPASS_TAG — Perimeter Override for ColorMix.
                            // s109 fix: emit the perimeter ONCE at FULL layer height on
                            // the topmost band's last bucket (≈ nominal Z) so the wall
                            // closes the layer. Scaling it to the band height (old bug)
                            // left Penultimate/partial bands short → open perimeter.
                            // Only the colour (the last bucket's tool) is overridden.
                            {
                                const bool _cm_perim = _mp_painter_mode
                                    ? mp_stack.perimeter_override
                                    : mp_cfg.multipass_perimeter_override.value;
                                if (_cm_perim && i == n - 1 && !sublayer_slot.empty()) {
                                    MultiPassSubLayer& _anchor = sublayer_slot.back();
                                    for (const LayerRegion* lr : this->regions())
                                        for (const ExtrusionEntity* pe : lr->perimeters.entities)
                                            _anchor.perimeters.entities.push_back(pe->clone());
                                    NEOTKO_LOG(MULTIPASS, "  SUBLAYER CM perimeters cloned (full-h): "
                                        << _anchor.perimeters.entities.size()
                                        << " entities → tool T" << _anchor.tool_id << " top band " << i);
                                }
                            }
                            NEOTKO_LOG(MULTIPASS, "  SANDWICH ColorMix band" << i
                                << " z=" << band_top_z << " buckets=" << NB
                                << " pass_idx=" << _pass0 << ".." << (global_pass - 1)
                                << " slot_tag=" << _fp_slot_tag);
                            // NEOTKO_SANDWICH_DEBUG — per-bucket trace (s79j+). Greppable: "EMIT_SUB".
                            for (int _kk = 0; _kk < NB; ++_kk) {
                                const MultiPassSubLayer& _ss = sublayer_slot[sublayer_slot.size() - NB + _kk];
                                NEOTKO_LOG(MULTIPASS, "  EMIT_SUB layer=" << f->layer_id
                                    << " z=" << _ss.print_z
                                    << " tool=T" << _ss.tool_id
                                    << " pass=" << _ss.pass_idx
                                    << " role=" << (int)_ss.role
                                    << " effect=CM bucket=" << _kk << "/" << NB
                                    << " slot_tag=" << _fp_slot_tag
                                    << " fills=" << _ss.fills.entities.size());
                            }
                        } else {
                            // Solid / None — one mono-tool sublayer at the band top.
                            MultiPassSubLayer sub;
                            sub.print_z     = band_top_z;
                            sub.height      = band_height;
                            sub.pass_idx    = global_pass++;
                            sub.role        = _sub_role;
                            sub.effect      = SurfacePassKind::Solid;
                            sub.tool_id     = solid_tool;
                            sub.speed_pct   = is_none ? 100 : pass.speed_pct;
                            sub.gcode_start = is_none ? std::string() : pass.gcode_start;
                            sub.gcode_end   = is_none ? std::string() : pass.gcode_end;
                            sub.fills       = std::move(temp);

                            // NEOTKO_MULTIPASS_TAG — Perimeter Override (Solid only).
                            // NEOTKO_PROFILE_TAG — Fase 6b: in painter mode the flag
                            // travels in the resolved stack (mp_stack.perimeter_override),
                            // not the legacy MP payload (no longer populated).
                            const bool _mp_perim = _mp_painter_mode
                                ? mp_stack.perimeter_override
                                : mp_cfg.multipass_perimeter_override.value;
                            if (_mp_perim) {
                                for (const LayerRegion* lr : this->regions()) {
                                    for (const ExtrusionEntity* pe : lr->perimeters.entities) {
                                        ExtrusionEntity* cloned = pe->clone();
                                        neotko_mp_scale_perim(cloned, ratio, k_mp);
                                        sub.perimeters.entities.push_back(cloned);
                                    }
                                }
                                NEOTKO_LOG(MULTIPASS, "  SUBLAYER perimeters cloned: "
                                    << sub.perimeters.entities.size() << " entities for band " << i);
                            }

                            sublayer_slot.push_back(std::move(sub));

                            NEOTKO_LOG(MULTIPASS, "  SUBLAYER band" << i
                                << " kind=" << (int)kind << " print_z=" << band_top_z
                                << " T" << solid_tool
                                << " pass_idx=" << (global_pass - 1)
                                << " slot_tag=" << _fp_slot_tag);
                            // NEOTKO_SANDWICH_DEBUG — per-sublayer trace (s79j+). Greppable: "EMIT_SUB".
                            {
                                const MultiPassSubLayer& _ss = sublayer_slot.back();
                                NEOTKO_LOG(MULTIPASS, "  EMIT_SUB layer=" << f->layer_id
                                    << " z=" << _ss.print_z
                                    << " tool=T" << _ss.tool_id
                                    << " pass=" << _ss.pass_idx
                                    << " role=" << (int)_ss.role
                                    << " effect=" << (is_none ? "None" : "Solid")
                                    << " slot_tag=" << _fp_slot_tag
                                    << " fills=" << _ss.fills.entities.size()
                                    << " perims=" << _ss.perimeters.entities.size());
                            }
                        }
                    }
                    // NEOTKO_SANDWICH_TAG_END

                    f->angle = base_angle;
                } else {
                    // NEOTKO_PATHBLEND_TAG_START — angle override for PathBlend surfaces
                    // If PathBlend is active and pathblend_fill_angle >= 0, use that angle
                    // instead of the top surface fill angle.
                    // NEOTKO_PROFILE_TAG — Fase G: in painter mode, read the
                    // fill_angle from the painted profile's PathBlend payload
                    // (preset's multipass_path_gradient flag is suppressed).
                    if (!surface_fill.params.bridge && !_mp_mm_painted &&
                        (surface_fill.params.extrusion_role == erTopSolidInfill ||
                         surface_fill.params.extrusion_role == erPenultimateInfill)) {
                        if (_mp_painter_mode && _fp_slot_tag != 0) {   // NEOTKO_PROFILE_TAG — Fase 6c v2: natural remainder (tag==0) keeps natural angle
                            const ExtrusionRole _role = surface_fill.params.extrusion_role;
                            const PrintObject* _po = this->object();
                            const int _slot = (_role == erTopSolidInfill)
                                ? SurfaceColorMix::dominant_painted_slot_in_z_range(
                                      _po, this->print_z - this->height, this->print_z)
                                : SurfaceColorMix::dominant_painted_slot_in_z_range(
                                      _po, this->print_z, this->print_z + this->height);
                            if (_slot > 0) {
                                const int _pid = SurfaceColorMix::profile_id_for_slot(_po, _slot);
                                const auto* _p = Slic3r::SurfaceEffectProfileManager::get().find(_pid);
                                if (_p && _p->pathblend.present) {
                                    const PathBlendPassConfig pb_ang =
                                        SurfaceColorMix::pathblend_from_profile_payload(_p->pathblend);
                                    if (pb_ang.enabled && pb_ang.fill_angle >= 0)
                                        f->angle = Geometry::deg2rad(static_cast<float>(pb_ang.fill_angle));
                                }
                            }
                        } else if (layerm->region().config().multipass_path_gradient.value) {
                            const PathBlendPassConfig pb_ang =
                                PathBlendPassConfig::from_region_config(
                                    layerm->region().config(),
                                    surface_fill.params.extrusion_role);
                            if (SurfaceColorMix::should_process_role(
                                    surface_fill.params.extrusion_role, pb_ang.surface) &&
                                pb_ang.fill_angle >= 0) {
                                f->angle = Geometry::deg2rad(static_cast<float>(pb_ang.fill_angle));
                            }
                        }
                    }
                    // NEOTKO_PATHBLEND_TAG_END
                    // NEOTKO_COLORMIX_TAG_START — angle override for ColorMix surfaces
                    // When interlayer_colormix_angle (Top) or interlayer_colormix_penu_angle
                    // (Penultimate) is >= 0 AND ColorMix is active for this layer/role,
                    // override f->angle completely — bypassing solid_infill_direction and
                    // solid_infill_rotate_template on these layers only.
                    if (!surface_fill.params.bridge && !_mp_mm_painted &&
                        (surface_fill.params.extrusion_role == erTopSolidInfill ||
                         surface_fill.params.extrusion_role == erPenultimateInfill)) {
                        const auto& _cm_cfg_angle = layerm->region().config();
                        bool _cm_angle_active = false;
                        if (_mp_painter_mode && _fp_slot_tag != 0) {   // NEOTKO_PROFILE_TAG — Fase 6c v2: natural remainder (tag==0) keeps natural angle
                            // Painter mode: active if a painted profile resolves at this layer/role
                            const ExtrusionRole _role = surface_fill.params.extrusion_role;
                            const PrintObject* _po = this->object();
                            const int _slot = (_role == erTopSolidInfill)
                                ? SurfaceColorMix::dominant_painted_slot_in_z_range(
                                      _po, this->print_z - this->height, this->print_z)
                                : SurfaceColorMix::dominant_painted_slot_in_z_range(
                                      _po, this->print_z, this->print_z + this->height);
                            if (_slot > 0) {
                                const int _pid = SurfaceColorMix::profile_id_for_slot(_po, _slot);
                                _cm_angle_active = (_pid > 0);
                            }
                        } else if (_cm_cfg_angle.interlayer_colormix_enabled.value) {
                            // Preset mode: apply zone + filament filter identical to assign_and_group_tools
                            const int _ff = _cm_cfg_angle.interlayer_colormix_filament_filter.value;
                            const bool _ff_ok = (_ff <= 0) || (_cm_cfg_angle.solid_infill_filament.value == _ff);
                            if (_ff_ok) {
                                if (surface_fill.params.extrusion_role == erTopSolidInfill) {
                                    _cm_angle_active = (_cm_cfg_angle.interlayer_colormix_top_zone.value == 0) ||
                                                       (this->upper_layer == nullptr);
                                } else {
                                    _cm_angle_active = (_cm_cfg_angle.interlayer_colormix_penu_zone.value == 0) ||
                                                       (this->upper_layer != nullptr &&
                                                        this->upper_layer->upper_layer == nullptr);
                                }
                            }
                        }
                        if (_cm_angle_active) {
                            const int _cm_angle = (surface_fill.params.extrusion_role == erTopSolidInfill)
                                ? _cm_cfg_angle.interlayer_colormix_angle.value
                                : _cm_cfg_angle.interlayer_colormix_penu_angle.value;
                            if (_cm_angle >= 0)
                                f->angle = Geometry::deg2rad(static_cast<float>(_cm_angle));
                        }
                    }
                    // NEOTKO_COLORMIX_TAG_END
                    // BBS: make fill
                    f->fill_surface_extrusion(&surface_fill.surface,
                        params,
                        m_regions[surface_fill.region_id]->fills.entities);
                }
            }
            // NEOTKO_MULTIPASS_TAG_END
		}
    }

    // NEOTKO_PATHBLEND_TAG — Fase 5 s77 migración: the legacy PathBlend FASE2
    // "path duplication" block (clone-per-pass + mm3 tool encoding, decoded by
    // COLORMIX_HOOK at GCode time) was DELETED. PathBlend now compiles into
    // ramp(+cap) single-tool sublayers in the FASE 2 band loop above, scheduled
    // exactly like MultiPass/ColorMix. See the SurfacePassKind::PathBlend branch.


    // NEOTKO_COLORMIX_TAG_START — Apply Surface ColorMix to top/penultimate layers
    // assign_and_group_tools() splits each zig-zag path into individual lines,
    // assigns tools cyclically, and reorders by tool group.
    // allow_top / allow_penu carry the zone filter so each role is gated independently.
    // Returns COLORMIX_FLAG_UNSPLITTABLE if monotonic pattern prevents line splitting.
    bool any_unsplittable = false;
    // NEOTKO_PROFILE_TAG — Fase 6c: the legacy real-layer ColorMix route must NOT
    // run for painter-mode objects. The painter applies ColorMix through FASE-2
    // sublayers (painted footprint only); the unpainted remainder is now a normal
    // top fill sitting in layerm->fills, and this route's painted_override path
    // would re-dither it — flooding exactly the area 6c masking just protected.
    // Painter objects are fully handled above; skip them here.
    const ModelObject* _cm_mo = (this->object() != nullptr)
        ? this->object()->model_object() : nullptr;
    const bool _cm_painter_obj = SurfaceColorMix::object_has_any_colormix_paint(_cm_mo);
    for (LayerRegion *layerm : m_regions) {
        if (_cm_painter_obj) break;   // painter mode → real-layer route disabled
        if (layerm->fills.entities.empty()) continue;
        const auto& _cm_cfg = layerm->region().config();
        // Filament filter
        const int _ff_cm = _cm_cfg.interlayer_colormix_filament_filter.value;
        if (_ff_cm > 0 && _cm_cfg.solid_infill_filament.value != _ff_cm) continue;
        // Zone filter per role
        const bool _cm_top_ok  = (_cm_cfg.interlayer_colormix_top_zone.value  == 0) || (this->upper_layer == nullptr);
        const bool _cm_penu_ok = (_cm_cfg.interlayer_colormix_penu_zone.value == 0) ||
            (this->upper_layer != nullptr && this->upper_layer->upper_layer == nullptr);
        if (!_cm_top_ok && !_cm_penu_ok) continue;
        // Pass MixedFilamentManager for virtual-digit recipe expansion (use_virtual gate).
        // When OFF, mgr stays null → build_tool_list_from_pattern uses only '1'-'4'.
        const MixedFilamentManager* _cm_mgr = nullptr;
        size_t _cm_num_phys = 0;
        if (_cm_cfg.interlayer_colormix_use_virtual.value) {
            const Print* _print = this->object()->print();
            _cm_mgr      = &_print->mixed_filament_manager();
            _cm_num_phys = _print->config().filament_colour.size();
        }
        int flags = SurfaceColorMix::assign_and_group_tools(
            layerm->fills, _cm_cfg, erNone, (int)this->id(), _cm_top_ok, _cm_penu_ok,
            _cm_mgr, _cm_num_phys,
            // NEOTKO_PROFILE_TAG — Fase D: enable painted-profile lookup.
            this->object(), this->print_z, this->height);
        if (flags & COLORMIX_FLAG_UNSPLITTABLE)
            any_unsplittable = true;
    }
    // NEOTKO_COLORMIX_TAG_END

    // add thin fill regions
    // Unpacks the collection, creates multiple collections per path.
    // The path type could be ExtrusionPath, ExtrusionLoop or ExtrusionEntityCollection.
    // Why the paths are unpacked?
	for (LayerRegion *layerm : m_regions)
	    for (const ExtrusionEntity *thin_fill : layerm->thin_fills.entities) {
	        ExtrusionEntityCollection &collection = *(new ExtrusionEntityCollection());
	        layerm->fills.entities.push_back(&collection);
	        collection.entities.push_back(thin_fill->clone());
	    }

#ifndef NDEBUG
	for (LayerRegion *layerm : m_regions)
	    for (size_t i = 0; i < layerm->fills.entities.size(); ++ i)
    	    assert(dynamic_cast<ExtrusionEntityCollection*>(layerm->fills.entities[i]) != nullptr);
#endif

    // NEOTKO_SANDWICH_TAG_START — coalesce twin islands at the same Z.
    // A single object with two top/penu surfaces at the EXACT same print_z
    // (e.g. the twin steps of a double staircase modeled as ONE object) emits a
    // separate sublayer per island. The per-fill pass counter resets for each
    // island, so the twins end up with an IDENTICAL scheduling key
    // (print_z, pass_idx, tool_id, role, effect, pathblend_pass).
    // collect_layers_to_print() (GCode.cpp) keeps only ONE LayerToPrint per
    // (object, print_z), so the second island silently loses its sandwich
    // (visible as only one of the twin steps getting the effect). Two SEPARATE
    // objects don't hit this — each object has its own slot in that merge.
    // Fix: merge sublayers identical in that key by concatenating their fills/
    // perimeters → the wipe tower sees the same sublayer count as a single
    // island would (no extra toolchanges, no plan divergence).
    // LIMITATION: twins resolving to DIFFERENT tools at the same Z (different
    // per-island config / painter profiles) are NOT merged — that needs
    // per-sublayer unique keys (deferred; see docs/GIZMO_SANDWICH_PAINTER.md).
    if (this->object() != nullptr &&
        this->id() < this->object()->multipass_sublayers().size()) {
        auto& subs = this->object()->multipass_sublayers()[this->id()];
        for (size_t a = 0; a < subs.size(); ++a) {
            for (size_t b = subs.size(); b-- > a + 1; ) {
                if (subs[a].tool_id        == subs[b].tool_id        &&
                    subs[a].pass_idx       == subs[b].pass_idx       &&
                    subs[a].role           == subs[b].role           &&
                    subs[a].effect         == subs[b].effect         &&
                    subs[a].pathblend_pass == subs[b].pathblend_pass &&
                    std::abs(subs[a].print_z - subs[b].print_z) < EPSILON) {
                    MultiPassSubLayer& A = subs[a];
                    MultiPassSubLayer& B = subs[b];
                    A.fills.entities.insert(A.fills.entities.end(),
                        B.fills.entities.begin(), B.fills.entities.end());
                    B.fills.entities.clear(); // ownership moved to A — no double free
                    A.perimeters.entities.insert(A.perimeters.entities.end(),
                        B.perimeters.entities.begin(), B.perimeters.entities.end());
                    B.perimeters.entities.clear();
                    subs.erase(subs.begin() + b);
                }
            }
        }
        // NEOTKO_SANDWICH_DEBUG — survivors snapshot (s79j+). One line per
        // sublayer that made it past the coalescer, in stored order. Greppable:
        // "POST_COALESCE_SUB". If your "missing letter" sublayer doesn't appear
        // here but did EMIT_SUB upstream, the coalescer ate it. If it appears
        // here but never DISPATCH_SUB downstream, the GCode dispatcher dropped it.
        {
            const auto& _subs_final = this->object()->multipass_sublayers()[this->id()];
            NEOTKO_LOG(MULTIPASS, "POST_COALESCE layer=" << this->id()
                << " print_z=" << this->print_z
                << " survivors=" << _subs_final.size());
            for (size_t _q = 0; _q < _subs_final.size(); ++_q) {
                const MultiPassSubLayer& _ss = _subs_final[_q];
                NEOTKO_LOG(MULTIPASS, "  POST_COALESCE_SUB idx=" << _q
                    << " z=" << _ss.print_z
                    << " tool=T" << _ss.tool_id
                    << " pass=" << _ss.pass_idx
                    << " role=" << (int)_ss.role
                    << " effect=" << (int)_ss.effect
                    << " pb_pass=" << _ss.pathblend_pass
                    << " fills=" << _ss.fills.entities.size()
                    << " perims=" << _ss.perimeters.entities.size());
            }
        }
    }
    // NEOTKO_SANDWICH_TAG_END

    // NEOTKO_COLORMIX_TAG_START
    return any_unsplittable;
    // NEOTKO_COLORMIX_TAG_END
}
/**
 * Generate sparse-infill polylines for anchoring/analysis purposes.
 *
 * This produces the geometric polylines of internal sparse infill for the current
 * layer (using the same infill pattern, angle, rotation template, and spacing that
 * normal slicing would use), but it does not create extrusion entities.
 *
 * The returned polylines are consumed by internal-bridge detection on the next
 * layer to derive anchor lines and compute the bridge direction over sparse infill.
 *
 * Notes:
 * - Only `stInternal` surfaces are considered.
 * - Rotation templates (e.g. `sparse_infill_rotate_template`) are applied so the
 *   anchors reflect the actual infill orientation.
 * - For lightning/adaptive patterns, the respective generators are wired so their
 *   polylines match the final infill layout.
 */
Polylines Layer::generate_sparse_infill_polylines_for_anchoring(FillAdaptive::Octree* adaptive_fill_octree, FillAdaptive::Octree* support_fill_octree,  FillLightning::Generator* lightning_generator) const
{
    LockRegionParam skin_inner_param;
    std::vector<SurfaceFill> surface_fills = group_fills(*this, skin_inner_param);
	const Slic3r::BoundingBox bbox = this->object()->bounding_box();
	const auto                resolution = this->object()->print()->config().resolution.value;

    Polylines sparse_infill_polylines{};

    for (SurfaceFill &surface_fill : surface_fills) {
		if (surface_fill.surface.surface_type != stInternal) {
			continue;
		}

        switch (surface_fill.params.pattern) {
        case ipCount: continue; break;
        case ipSupportBase: continue; break;
        case ipConcentricInternal: continue; break;
        case ipLightning:
		case ipAdaptiveCubic:
        case ipSupportCubic:
        case ipRectilinear:
        case ipMonotonic:
        case ipMonotonicLine:
        case ipAlignedRectilinear:
        case ipGrid:
        case ipLateralLattice:
        case ipTriangles:
        case ipStars:
        case ipCubic:
        case ipLine:
        case ipConcentric:
        case ipHoneycomb:
        case ipLateralHoneycomb:
        case ip3DHoneycomb:
        case ipGyroid:
        case ipTpmsD:
        case ipTpmsFK:
        case ipHilbertCurve:
        case ipArchimedeanChords:
        case ipOctagramSpiral:
        case ipZigZag:
        case ipCrossZag:
		case ipLockedZag: break;
        }

        // Create the filler object.
        std::unique_ptr<Fill> f = std::unique_ptr<Fill>(Fill::new_from_type(surface_fill.params.pattern));
        f->set_bounding_box(bbox);
        f->layer_id = this->id() - this->object()->get_layer(0)->id(); // We need to subtract raft layers.
        f->z        = this->print_z;
        f->angle    = surface_fill.params.angle;
        f->is_using_template_angle = surface_fill.params.is_using_template_angle;
        f->adapt_fill_octree   = (surface_fill.params.pattern == ipSupportCubic) ? support_fill_octree : adaptive_fill_octree;
        f->print_config        = &this->object()->print()->config();
        f->print_object_config = &this->object()->config();

        if (surface_fill.params.pattern == ipLightning)
            dynamic_cast<FillLightning::Filler *>(f.get())->generator = lightning_generator;

        // calculate flow spacing for infill pattern generation
        double link_max_length = 0.;
        if (!surface_fill.params.bridge) {
#if 0
            link_max_length = layerm.region()->config().get_abs_value(surface.is_external() ? "external_fill_link_max_length" : "fill_link_max_length", flow.spacing());
//            printf("flow spacing: %f,  is_external: %d, link_max_length: %lf\n", flow.spacing(), int(surface.is_external()), link_max_length);
#else
            if (surface_fill.params.density > 80.) // 80%
                link_max_length = 3. * f->spacing;
#endif
        }

        LayerRegion &layerm = *m_regions[surface_fill.region_id];

        // Maximum length of the perimeter segment linking two infill lines.
        f->link_max_length = (coord_t) scale_(link_max_length);
        // Used by the concentric infill pattern to clip the loops to create extrusion paths.
        f->loop_clipping = coord_t(scale_(layerm.region().config().seam_gap.get_abs_value(surface_fill.params.flow.nozzle_diameter())));

        // apply half spacing using this flow's own spacing and generate infill
        FillParams params;
        params.density           = float(0.01 * surface_fill.params.density);
        params.dont_adjust       = false; //  surface_fill.params.dont_adjust;
        params.anchor_length     = surface_fill.params.anchor_length;
        params.anchor_length_max = surface_fill.params.anchor_length_max;
        params.resolution        = resolution;
        params.use_arachne       = false;
        params.layer_height      = layerm.layer()->height;
        params.lateral_lattice_angle_1   = surface_fill.params.lateral_lattice_angle_1;
        params.lateral_lattice_angle_2   = surface_fill.params.lateral_lattice_angle_2;
        params.infill_overhang_angle   = surface_fill.params.infill_overhang_angle;
        params.multiline         = surface_fill.params.multiline;

        for (ExPolygon &expoly : surface_fill.expolygons) {
            // Spacing is modified by the filler to indicate adjustments. Reset it for each expolygon.
            f->spacing                     = surface_fill.params.spacing;
            surface_fill.surface.expolygon = std::move(expoly);
            try {
                Polylines polylines = f->fill_surface(&surface_fill.surface, params);
                sparse_infill_polylines.insert(sparse_infill_polylines.end(), polylines.begin(), polylines.end());
            } catch (InfillFailedException &) {}
        }
    }

    return sparse_infill_polylines;
}

// Create ironing extrusions over top surfaces.
void Layer::make_ironing()
{
	// LayerRegion::slices contains surfaces marked with SurfaceType.
	// Here we want to collect top surfaces extruded with the same extruder.
	// A surface will be ironed with the same extruder to not contaminate the print with another material leaking from the nozzle.

	// First classify regions based on the extruder used.
	struct IroningParams {
		InfillPattern pattern;
		int 		extruder 	= -1;
		bool 		just_infill = false;
		// Spacing of the ironing lines, also to calculate the extrusion flow from.
		double 		line_spacing;
		// Height of the extrusion, to calculate the extrusion flow from.
		double 		height;
		double 		speed;
		double 		angle;
        double 		inset;

		bool operator<(const IroningParams &rhs) const {
			if (this->extruder < rhs.extruder)
				return true;
			if (this->extruder > rhs.extruder)
				return false;
			if (int(this->just_infill) < int(rhs.just_infill))
				return true;
			if (int(this->just_infill) > int(rhs.just_infill))
				return false;
			if (this->line_spacing < rhs.line_spacing)
				return true;
			if (this->line_spacing > rhs.line_spacing)
				return false;
			if (this->height < rhs.height)
				return true;
			if (this->height > rhs.height)
				return false;
			if (this->speed < rhs.speed)
				return true;
			if (this->speed > rhs.speed)
				return false;
			if (this->angle < rhs.angle)
				return true;
			if (this->angle > rhs.angle)
				return false;
            if (this->inset < rhs.inset)
                return true;
            if (this->inset > rhs.inset)
                return false;
			return false;
		}

		bool operator==(const IroningParams &rhs) const {
			return this->extruder == rhs.extruder && this->just_infill == rhs.just_infill &&
				   this->line_spacing == rhs.line_spacing && this->height == rhs.height && this->speed == rhs.speed && this->angle == rhs.angle && this->pattern == rhs.pattern && this->inset == rhs.inset;
		}

		LayerRegion *layerm		= nullptr;

		// IdeaMaker: ironing
		// ironing flowrate (5% percent)
		// ironing speed (10 mm/sec)

		// Kisslicer:
		// iron off, Sweep, Group
		// ironing speed: 15 mm/sec

		// Cura:
		// Pattern (zig-zag / concentric)
		// line spacing (0.1mm)
		// flow: from normal layer height. 10%
		// speed: 20 mm/sec
	};

	std::vector<IroningParams> by_extruder;
    double default_layer_height = this->object()->config().layer_height;

	for (LayerRegion *layerm : m_regions)
		if (! layerm->slices.empty()) {
			IroningParams ironing_params;
			const PrintRegionConfig &config = layerm->region().config();
			if (config.ironing_type != IroningType::NoIroning &&
			    (config.ironing_type == IroningType::AllSolid ||
				    ((config.top_shell_layers > 0 || (this->object()->print()->config().spiral_mode && config.bottom_shell_layers > 1)) &&
					    (config.ironing_type == IroningType::TopSurfaces ||
					        (config.ironing_type == IroningType::TopmostOnly && layerm->layer()->upper_layer == nullptr))))) {
				if (config.wall_filament == config.solid_infill_filament || config.wall_loops == 0) {
					// Iron the whole face.
					ironing_params.extruder = config.solid_infill_filament;
				} else {
					// Iron just the infill.
					ironing_params.extruder = config.solid_infill_filament;
				}
			}
			if (ironing_params.extruder != -1) {
				//TODO just_infill is currently not used.
				ironing_params.just_infill 	= false;
				ironing_params.line_spacing = config.ironing_spacing;
                ironing_params.inset 		= config.ironing_inset;
				ironing_params.height 		= default_layer_height * 0.01 * config.ironing_flow;
				ironing_params.speed 		= config.ironing_speed;
                ironing_params.angle        = (config.ironing_angle >= 0 ? config.ironing_angle : config.infill_direction) * M_PI / 180.;
				ironing_params.pattern      = config.ironing_pattern;
				ironing_params.layerm 		= layerm;
				by_extruder.emplace_back(ironing_params);
			}
		}
	std::sort(by_extruder.begin(), by_extruder.end());

    FillParams 			fill_params;
    fill_params.density 	 = 1.;
    fill_params.monotonic    = true;
    InfillPattern         f_pattern = ipRectilinear;
    std::unique_ptr<Fill> f         = std::unique_ptr<Fill>(Fill::new_from_type(f_pattern));
    f->set_bounding_box(this->object()->bounding_box());
    f->layer_id = this->id();
    f->z        = this->print_z;
    f->overlap  = 0;
	for (size_t i = 0; i < by_extruder.size();) {
		// Find span of regions equivalent to the ironing operation.
		IroningParams &ironing_params = by_extruder[i];
		// Create the filler object.
		if( f_pattern != ironing_params.pattern )
		{
            f_pattern               = ironing_params.pattern;
            f = std::unique_ptr<Fill>(Fill::new_from_type(f_pattern));
            f->set_bounding_box(this->object()->bounding_box());
            f->layer_id = this->id();
            f->z        = this->print_z;
            f->overlap  = 0;
		}

		size_t j = i;
		for (++ j; j < by_extruder.size() && ironing_params == by_extruder[j]; ++ j) ;

		// Create the ironing extrusions for regions <i, j)
		ExPolygons ironing_areas;
		double nozzle_dmr = this->object()->print()->config().nozzle_diameter.get_at(ironing_params.extruder - 1);
		if (ironing_params.just_infill) {
			//TODO just_infill is currently not used.
			// Just infill.
		} else {
			// Infill and perimeter.
			// Merge top surfaces with the same ironing parameters.
			Polygons polys;
			Polygons infills;
			for (size_t k = i; k < j; ++ k) {
				const IroningParams		 &ironing_params  = by_extruder[k];
				const PrintRegionConfig  &region_config   = ironing_params.layerm->region().config();
				bool					  iron_everything = region_config.ironing_type == IroningType::AllSolid;
				bool					  iron_completely = iron_everything;
				if (iron_everything) {
					// Check whether there is any non-solid hole in the regions.
					bool internal_infill_solid = region_config.sparse_infill_density.value > 95.;
					for (const Surface &surface : ironing_params.layerm->fill_surfaces.surfaces)
						if ((!internal_infill_solid && surface.surface_type == stInternal) || surface.surface_type == stInternalBridge || surface.surface_type == stInternalVoid) {
							// Some fill region is not quite solid. Don't iron over the whole surface.
							iron_completely = false;
							break;
						}
				}
				if (iron_completely) {
					// Iron everything. This is likely only good for solid transparent objects.
					for (const Surface &surface : ironing_params.layerm->slices.surfaces)
						polygons_append(polys, surface.expolygon);
				} else {
					for (const Surface &surface : ironing_params.layerm->slices.surfaces)
						if ((surface.surface_type == stTop && (region_config.top_shell_layers > 0 || this->object()->print()->config().spiral_mode)) || (iron_everything && surface.surface_type == stBottom && region_config.bottom_shell_layers > 0))
							// stBottomBridge is not being ironed on purpose, as it would likely destroy the bridges.
							polygons_append(polys, surface.expolygon);
				}
				if (iron_everything && ! iron_completely) {
					// Add solid fill surfaces. This may not be ideal, as one will not iron perimeters touching these
					// solid fill surfaces, but it is likely better than nothing.
					for (const Surface &surface : ironing_params.layerm->fill_surfaces.surfaces)
						if (surface.surface_type == stInternalSolid)
							polygons_append(infills, surface.expolygon);
				}
			}

			if (! infills.empty() || j > i + 1) {
				// Ironing over more than a single region or over solid internal infill.
				if (! infills.empty())
					// For IroningType::AllSolid only:
					// Add solid infill areas for layers, that contain some non-ironable infil (sparse infill, bridge infill).
					append(polys, std::move(infills));
				polys = union_safety_offset(polys);
			}
			// Trim the top surfaces with half the nozzle diameter.
            // BBS: ironing inset
            double ironing_areas_offset = ironing_params.inset == 0 ? float(scale_(0.5 * nozzle_dmr)) : scale_(ironing_params.inset);
			ironing_areas = intersection_ex(polys, offset(this->lslices, - ironing_areas_offset));
		}

        // Create the filler object.
        f->spacing = ironing_params.line_spacing;
        f->angle = float(ironing_params.angle);
        f->link_max_length = (coord_t) scale_(3. * f->spacing);
		double  extrusion_height = ironing_params.height * f->spacing / nozzle_dmr;
		float  extrusion_width  = Flow::rounded_rectangle_extrusion_width_from_spacing(float(nozzle_dmr), float(extrusion_height));
		double flow_mm3_per_mm = nozzle_dmr * extrusion_height;
        Surface surface_fill(stTop, ExPolygon());
        for (ExPolygon &expoly : ironing_areas) {
			surface_fill.expolygon = std::move(expoly);
			Polylines polylines;
			try {
				polylines = f->fill_surface(&surface_fill, fill_params);
			} catch (InfillFailedException &) {
			}
	        if (! polylines.empty()) {
		        // Save into layer.
				ExtrusionEntityCollection *eec = nullptr;
		        ironing_params.layerm->fills.entities.push_back(eec = new ExtrusionEntityCollection());
		        // Don't sort the ironing infill lines as they are monotonicly ordered.
				eec->no_sort = true;
		        extrusion_entities_append_paths(
		            eec->entities, std::move(polylines),
		            erIroning,
		            flow_mm3_per_mm, extrusion_width, float(extrusion_height));
		    }
		}

		// Regions up to j were processed.
		i = j;
	}
}

} // namespace Slic3r
