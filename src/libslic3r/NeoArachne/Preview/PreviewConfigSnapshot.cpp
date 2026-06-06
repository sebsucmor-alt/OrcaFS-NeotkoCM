// NEOTKO_NEOARACHNE_TAG preview-lab PL.5
#include "PreviewConfigSnapshot.hpp"

namespace Slic3r { namespace NeoArachne { namespace Preview {

void ConfigSnapshot::force_isolated_layer_defaults()
{
    // Disable every check that consults upper/lower slices. PL.1 scope doc
    // documents the rationale; comments here mirror the field names that
    // PerimeterGenerator::process_classic reads.
    region.only_one_wall_top.value         = false;
    region.only_one_wall_first_layer.value = false;
    region.detect_overhang_wall.value      = false;
    region.detect_thin_wall.value          = false;
    region.extra_perimeters_on_overhangs.value = false;
    region.alternate_extra_wall.value      = false;
    region.fuzzy_skin.value                = FuzzySkinType::None;
}

}}} // namespace Slic3r::NeoArachne::Preview
