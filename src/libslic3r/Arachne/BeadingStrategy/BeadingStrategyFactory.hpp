// Copyright (c) 2022 Ultimaker B.V.
// CuraEngine is released under the terms of the AGPLv3 or higher.

#ifndef BEADING_STRATEGY_FACTORY_H
#define BEADING_STRATEGY_FACTORY_H

#include <math.h>
#include <cmath>

#include "BeadingStrategy.hpp"
#include "../../Point.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r::Arachne
{

class BeadingStrategyFactory
{
public:
    static BeadingStrategyPtr makeStrategy
    (
        coord_t preferred_bead_width_outer = scaled<coord_t>(0.0005),
        coord_t preferred_bead_width_inner = scaled<coord_t>(0.0005),
        coord_t preferred_transition_length = scaled<coord_t>(0.0004),
        float transitioning_angle = M_PI / 4.0,
        bool print_thin_walls = false,
        coord_t min_bead_width = 0,
        coord_t min_feature_size = 0,
        double wall_split_middle_threshold = 0.5,
        double wall_add_middle_threshold = 0.5,
        coord_t max_bead_count = 0,
        coord_t outer_wall_offset = 0,
        int inward_distributed_center_wall_count = 2,
        double minimum_variable_line_width = 0.5,
        // NEOTKO_NEOARACHNE_TAG fase3 — when neotko_edge_enabled is true, wrap
        // the meta-chain (after Redistribute / Widening / OuterInset, BEFORE
        // Limited's 0-width marker injection) with NeoArachneBeadingStrategy.
        // Default false → upstream behaviour preserved.
        bool   neotko_edge_enabled = false,
        bool   neotko_edge_pin_outer = true,
        bool   neotko_edge_cap_widening = false,
        double neotko_edge_hysteresis_pct = 0.0
    );
};

} // namespace Slic3r::Arachne
#endif // BEADING_STRATEGY_FACTORY_H
