#ifndef slic3r_TextureBumpPlaneHandles_hpp_
#define slic3r_TextureBumpPlaneHandles_hpp_

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Point.hpp"

// NEOTKO_TEXTUREBUMP_TAG — Fase 4.2 (docs/ATTRIBUTION_TEXTURE_BUMP.md §5 point 4): geometry helpers
// for the yaw-ring + XY-pivot-equivalent drag handles, shared between GLGizmoTextureBump (base
// object) and GLGizmoTextureBumpPainter (per-zone) so the two never disagree on where a handle
// sits or what a drag means. Deliberately NOT GL-aware (no GLModel/shader here) -- pure geometry,
// mirrors plane_components()'s axis dispatch (TextureBump.cpp, itself private to that .cpp) so the
// handle's world position always matches what compute_u() will actually sample.

namespace Slic3r::GUI {

// The world-space normal of the 2D wrap-plane `axis` selects (Z for axis==Z -> XY plane, etc.) --
// this is the plane a drag ray gets intersected against, and the axis plane_transform's yaw
// rotates within.
Vec3d texture_bump_plane_normal(TextureProjectionAxis axis);

// Maps a world-space point into the (a,b) component space plane_components() (TextureBump.cpp)
// uses for the given axis -- e.g. axis==Z gives (x-center.x(), y-center.y()), matching
// compute_u()'s Cylindrical/Planar branches exactly.
Vec2d texture_bump_world_to_ab(TextureProjectionAxis axis, const Vec3d& center, const Vec3d& world);

// Inverse of texture_bump_world_to_ab(): places (a,b) back into world space, using `plane_coord`
// for whichever single world dimension the (a,b) pair doesn't cover (e.g. world Z for axis==Z).
Vec3d texture_bump_ab_to_world(TextureProjectionAxis axis, const Vec3d& center, double a, double b, double plane_coord);

// Ray/plane intersection (plane given by a point on it + its normal). Returns `plane_point` itself
// if the ray is (near-)parallel to the plane, a degenerate but harmless fallback for a UI drag.
Vec3d texture_bump_ray_plane_intersection(const Vec3d& ray_origin, const Vec3d& ray_dir, const Vec3d& plane_point,
                                          const Vec3d& plane_normal);

// Yaw/pivot extraction+construction for a TextureBumpConfig::plane_transform. MUST stay in
// lockstep with apply_plane_transform() (TextureBump.cpp, private to that .cpp): that function
// reads plane_transform.linear()(1,0)/(0,0) as the yaw and .translation().x()/y() as the pivot,
// regardless of which axis is selected -- these helpers are the single place both gizmos
// (GLGizmoTextureBump's 3D drag handles, GLGizmoTextureBumpPainter's numeric zone-editor fields)
// read/write that same convention, so they can never drift apart from each other or from the
// engine.
double      texture_bump_plane_transform_yaw(const Transform3d& t);
Vec2d       texture_bump_plane_transform_pivot(const Transform3d& t);
Transform3d texture_bump_make_plane_transform(double yaw, const Vec2d& pivot);

} // namespace Slic3r::GUI

#endif // slic3r_TextureBumpPlaneHandles_hpp_
