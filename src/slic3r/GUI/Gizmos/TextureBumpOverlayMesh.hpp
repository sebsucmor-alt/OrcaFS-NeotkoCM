#ifndef slic3r_TextureBumpOverlayMesh_hpp_
#define slic3r_TextureBumpOverlayMesh_hpp_

#include "slic3r/GUI/GLModel.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/BoundingBox.hpp"

struct indexed_triangle_set;

// NEOTKO_TEXTUREBUMP_TAG — Fase 4.1 (overlay con textura real). Shared between GLGizmoTextureBump
// (object-wide overlay) and GLGizmoTextureBumpPainter (per-zone overlay) so both gizmos preview the
// exact same U/V mapping the slicing engine will sample, without either depending on the other's
// GLGizmoBase lifecycle -- pure geometry function, no shared state.

namespace Slic3r::GUI {

// Builds a P3T2 (position + texture coord) GLModel::Geometry for the Texture Bump overlay shape.
// `its` supplies the shape for Planar/Cylindrical/Spherical (built by the caller via
// its_make_cube/cylinder/sphere, same as before this UV mesh existed); for Cubic it is ignored --
// the 4 lateral faces are generated directly here, since compute_u()'s Cubic branch dispatches on a
// caller-supplied face normal that its_make_cube's shared-corner vertices can't carry unambiguously
// per-face.
//
// `local_to_bounds` maps `its`' local vertex space (or, for Cubic, the object's own local box
// space in [0,size]) into the SAME frame `object_bounds_mm` is expressed in -- i.e. the caller's
// `overlay_local_transform` ALONE, NOT `box_trafo * overlay_local_transform` (the combined
// transform used for RENDERING, stored separately by the caller for the shader's
// view_model_matrix). Bug fix (reported after Fase 4.1 shipped): passing the box_trafo-inclusive
// transform here meant this function computed U/V from a point in WORLD/bed space while
// `object_bounds_mm` (the caller's pre-box_trafo `box`) stayed in local/selection space -- for any
// object not sitting exactly at the bed origin, subtracting object_bounds_mm.center() (~local,
// near zero) from a world-space point (offset by the object's actual bed position) produced a
// U/V dominated by that constant offset, collapsing the whole surface to ~1 row/column of the
// image stretched across it ("solo aplica una fila, estrechada x1000"). Both sides of every
// compute_u() call MUST be in the same frame -- local_to_bounds/object_bounds_mm now both are.
// `object_bounds_mm` must be the same bounding box passed to compute_u() at slice
// time (object_bounds_mm.center() anchors Planar/Cylindrical/Spherical; .size() anchors Cubic).
// `plane_transform` (Fase 4.2) is the same yaw+pivot transform threaded through
// TextureBumpConfig::plane_transform/compute_u() -- defaults to Identity (legacy behavior) so
// callers that don't yet expose the new plane controls don't need to change.
GLModel::Geometry build_texture_bump_overlay_geometry(const indexed_triangle_set& its,
                                                       const Transform3d&         local_to_bounds,
                                                       TextureProjectionMode      mode,
                                                       TextureProjectionAxis      axis,
                                                       const BoundingBoxf3&       object_bounds_mm,
                                                       double                     scale_mm,
                                                       int                        repeat_u,
                                                       const Transform3d&         plane_transform = Transform3d::Identity());

} // namespace Slic3r::GUI

#endif // slic3r_TextureBumpOverlayMesh_hpp_
