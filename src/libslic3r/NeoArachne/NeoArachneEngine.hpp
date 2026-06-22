// NEOTKO_NEOARACHNE_TAG fase0
// Facade dispatch for the hybrid Classic/Arachne wall generator.
// Phase 0: pure passthrough to PerimeterGenerator::process_classic(),
// bit-identical to selecting Classic. Phases 1+ replace this with the
// real hybrid logic. Never calls process_classic/process_arachne with
// a modified PerimeterGenerator — always delegates as-is.
#ifndef slic3r_NeoArachneEngine_hpp_
#define slic3r_NeoArachneEngine_hpp_

namespace Slic3r {
class PerimeterGenerator;
namespace NeoArachne {

void run(PerimeterGenerator& g);

}} // namespace Slic3r::NeoArachne

#endif
