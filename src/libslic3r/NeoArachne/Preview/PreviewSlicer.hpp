// NEOTKO_NEOARACHNE_TAG preview-lab PL.5
// Backend entry point for the NeoArachne Preview Lab. Builds a mocked
// PerimeterGenerator over the hardcoded W slice and dispatches it through
// NeoArachne::Plan::run with the user's live Edge Closure parameters. The
// returned PreviewResult is bit-identical to what the real slicer would
// produce for the same region on a 1-layer print of the W.
#ifndef slic3r_NeoArachne_Preview_PreviewSlicer_hpp_
#define slic3r_NeoArachne_Preview_PreviewSlicer_hpp_

#include "PreviewResult.hpp"
#include "PreviewGeometrySource.hpp"

namespace Slic3r { namespace NeoArachne { namespace Preview {

struct ConfigSnapshot;

// Pure compute. Safe to call from a worker thread (std::async). Never throws
// — internal exceptions are captured into PreviewResult::error. The geometry
// source decides what gets sliced (hardcoded W / Wedge / frozen ModelVolume
// snapshot, see PreviewGeometrySource).
PreviewResult preview_slice(const ConfigSnapshot& snap, const PreviewGeometrySource& src);

// Convenience wrapper preserved for the legacy call site; equivalent to
// preview_slice(snap, PreviewGeometrySource::w()).
inline PreviewResult preview_slice_w(const ConfigSnapshot& snap)
{
    return preview_slice(snap, PreviewGeometrySource::w());
}

}}} // namespace Slic3r::NeoArachne::Preview

#endif
