#include "TextureBumpOverlayMesh.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Feature/TextureBump/TextureBump.hpp"
#include "libslic3r/NeoDebug.hpp"
#include <algorithm>
#include <array>
#include <sstream>

// NEOTKO_TEXTUREBUMP_TAG -- debug system for this overlay's U/V math (requested explicitly after
// the "one row of the image stretched x1000" report): gated behind the SAME ORCA_DEBUG_TEXTUREBUMP
// channel the rest of the feature already uses (NeoDebug.hpp), logs to the SAME log file
// (/tmp/neotko_texturebump.log). Prints once per geometry rebuild (not per-vertex/per-frame) --
// the bounds this call was given, plus the first sampled point's local/bounds-frame position and
// resulting u/v, so a frame mismatch like the one fixed here (local_to_bounds vs object_bounds_mm
// disagreeing about which space they're in) shows up immediately as a bounds-relative point far
// outside [-size, +size] or a u/v wildly outside [0, num_periods]/[0, a few] -- both are the
// tell-tale signature of subtracting a center in the wrong frame.
#define TEXTUREBUMP_OVERLAY_LOG(body) do { if (Slic3r::NeoDebug::enabled(Slic3r::NeoDebug::TEXTUREBUMP)) { \
    std::ostringstream _tbovl_; _tbovl_ << body;                                                          \
    Slic3r::NeoDebug::write(Slic3r::NeoDebug::TEXTUREBUMP, _tbovl_.str()); } } while (0)

namespace Slic3r::GUI {

namespace {

// Mirrors TextureBumpTable::build()'s num_periods formula exactly (TextureBump.cpp) so the overlay
// repeats the same number of times the engine will actually tile the image.
int texture_bump_num_periods(TextureProjectionMode mode, int repeat_u)
{
    const int base_periods = (mode == TextureProjectionMode::Cubic) ? 4 : 1;
    return base_periods * std::max(repeat_u, 1);
}

void add_uv_quad(GLModel::Geometry& data, const std::array<Vec3d, 4>& local_corners, const Transform3d& local_to_bounds,
                  const Vec2d& perp_dir, TextureProjectionMode mode, TextureProjectionAxis axis,
                  const BoundingBoxf3& object_bounds_mm, double scale_mm, int num_periods, const Transform3d& plane_transform,
                  bool log_first_only, bool& logged)
{
    const unsigned int base_index = static_cast<unsigned int>(data.vertices_count());
    for (const Vec3d& local : local_corners) {
        const Vec3d  bounds_pt    = local_to_bounds * local;
        const double u_canonical  = Slic3r::Feature::TextureBump::compute_u(bounds_pt, perp_dir, mode, axis, object_bounds_mm, plane_transform);
        const double u            = u_canonical * double(num_periods);
        // NEOTKO_TEXTUREBUMP_TAG -- bug fix (same root cause as ZBump's overlay, GLGizmoTextureBump.cpp:
        // GLTexture::load_from_png() uploads wxImage's row-major top-to-bottom pixel data straight
        // into glTexImage2D with no vertical flip, so GL's v=0 ends up sampling the SOURCE IMAGE's
        // top row, not its bottom -- confirmed live, an asymmetric texture (readable text/logo)
        // rendered upside-down/mirrored on this overlay just like ZBump's. Not fixed at
        // load_from_png() itself -- shared by the plate logo, toolbar backgrounds, environment
        // texture, etc., see that fix's comment for why touching it there is out of scope.
        // Compensated here, local to this consumption, same as ZBump's uv_at().
        const double v            = -(bounds_pt.z() / std::max(scale_mm, 1e-6));
        if (log_first_only && !logged) {
            TEXTUREBUMP_OVERLAY_LOG("overlay_quad_vertex local=(" << local.x() << "," << local.y() << "," << local.z()
                << ") bounds_pt=(" << bounds_pt.x() << "," << bounds_pt.y() << "," << bounds_pt.z()
                << ") u_canonical=" << u_canonical << " u=" << u << " v=" << v);
            logged = true;
        }
        const Vec3f local_f = local.cast<float>();
        data.add_vertex(local_f, Vec2f(float(u), float(v)));
    }
    data.add_triangle(base_index + 0, base_index + 1, base_index + 2);
    data.add_triangle(base_index + 0, base_index + 2, base_index + 3);
}

} // anonymous namespace

GLModel::Geometry build_texture_bump_overlay_geometry(const indexed_triangle_set& its, const Transform3d& local_to_bounds,
                                                       TextureProjectionMode mode, TextureProjectionAxis axis,
                                                       const BoundingBoxf3& object_bounds_mm, double scale_mm, int repeat_u,
                                                       const Transform3d& plane_transform)
{
    GLModel::Geometry data;
    data.format.type          = GLModel::Geometry::EPrimitiveType::Triangles;
    data.format.vertex_layout = GLModel::Geometry::EVertexLayout::P3T2;

    const int num_periods = texture_bump_num_periods(mode, repeat_u);

    TEXTUREBUMP_OVERLAY_LOG("overlay_build mode=" << int(mode) << " axis=" << int(axis) << " repeat_u=" << repeat_u
        << " num_periods=" << num_periods << " scale_mm=" << scale_mm
        << " bounds_min=(" << object_bounds_mm.min.x() << "," << object_bounds_mm.min.y() << "," << object_bounds_mm.min.z() << ")"
        << " bounds_max=(" << object_bounds_mm.max.x() << "," << object_bounds_mm.max.y() << "," << object_bounds_mm.max.z() << ")"
        << " bounds_center=(" << object_bounds_mm.center().x() << "," << object_bounds_mm.center().y() << "," << object_bounds_mm.center().z() << ")");

    bool logged_sample = false;

    if (mode == TextureProjectionMode::Cubic) {
        // Cubic's per-face UV depends on which of the 4 lateral faces a vertex belongs to --
        // compute_u()'s Cubic branch dispatches on a caller-supplied face normal, not on geometry it
        // infers itself -- so the shared-corner vertices its_make_cube() would produce can't carry 2
        // different UVs at once. Build the 4 lateral quads directly instead, in the object's own
        // local box space [0,size] (same convention update_overlay_and_grabbers() already uses for
        // Cubic's overlay_local_transform = translation(box.min)).
        const Vec3d  size = object_bounds_mm.size();
        const double sx = std::max(size.x(), 1.0), sy = std::max(size.y(), 1.0), sz = std::max(size.z(), 1.0);
        // NEOTKO_TEXTUREBUMP_TAG -- bug fix (z-fighting, reported after Fase 4.1 shipped a
        // textured overlay): these quads used to sit exactly at x/y in {0,sx}/{0,sy} -- i.e.
        // exactly coincident with the real object's own faces whenever the object's actual
        // surface touches its bounding box (e.g. a plain cuboid). Inflated outward by `m` on
        // every side (including the two dimensions each quad DOESN'T vary along, so adjacent
        // inflated quads still meet edge-to-edge with no gap) so the overlay is strictly outside
        // the real surface everywhere. `local_to_bounds` (== translation(box.min), see the
        // GLGizmoTextureBump*.cpp callers) is intentionally left un-inflated -- all the margin
        // logic lives here so the two can't drift out of sync.
        const double m = std::max(0.01 * std::max({ sx, sy, sz }), 0.2);
        data.reserve_vertices(16);
        data.reserve_indices(24);
        // East (+X)
        add_uv_quad(data, { Vec3d(sx + m, -m, -m), Vec3d(sx + m, sy + m, -m), Vec3d(sx + m, sy + m, sz + m), Vec3d(sx + m, -m, sz + m) },
                    local_to_bounds, Vec2d(1, 0), mode, axis, object_bounds_mm, scale_mm, num_periods, plane_transform, true, logged_sample);
        // North (+Y)
        add_uv_quad(data, { Vec3d(-m, sy + m, -m), Vec3d(sx + m, sy + m, -m), Vec3d(sx + m, sy + m, sz + m), Vec3d(-m, sy + m, sz + m) },
                    local_to_bounds, Vec2d(0, 1), mode, axis, object_bounds_mm, scale_mm, num_periods, plane_transform, false, logged_sample);
        // West (-X)
        add_uv_quad(data, { Vec3d(-m, -m, -m), Vec3d(-m, sy + m, -m), Vec3d(-m, sy + m, sz + m), Vec3d(-m, -m, sz + m) },
                    local_to_bounds, Vec2d(-1, 0), mode, axis, object_bounds_mm, scale_mm, num_periods, plane_transform, false, logged_sample);
        // South (-Y)
        add_uv_quad(data, { Vec3d(-m, -m, -m), Vec3d(sx + m, -m, -m), Vec3d(sx + m, -m, sz + m), Vec3d(-m, -m, sz + m) },
                    local_to_bounds, Vec2d(0, -1), mode, axis, object_bounds_mm, scale_mm, num_periods, plane_transform, false, logged_sample);
        return data;
    }

    // Planar/Cylindrical/Spherical: U is a pure function of position within object_bounds_mm's own
    // frame (atan2 or axis-projected linear position, see compute_u()), no face disambiguation
    // needed -- reuse `its` as-is (built by the caller via
    // its_make_cylinder/its_make_sphere/its_make_cube-as-thin-slab). perp_dir is only consumed by
    // the Cubic branch above, so any value is fine here.
    const Vec2d perp_dir_unused(1.0, 0.0);
    data.reserve_vertices(its.vertices.size());
    data.reserve_indices(its.indices.size() * 3);
    for (const stl_vertex& v : its.vertices) {
        const Vec3d  local        = v.cast<double>();
        const Vec3d  bounds_pt    = local_to_bounds * local;
        const double u_canonical  = Slic3r::Feature::TextureBump::compute_u(bounds_pt, perp_dir_unused, mode, axis, object_bounds_mm, plane_transform);
        const double u            = u_canonical * double(num_periods);
        // NEOTKO_TEXTUREBUMP_TAG -- bug fix, same as add_uv_quad() above (Cubic branch) -- see its
        // comment. Covers Planar/Cylindrical/Spherical, the 3 modes that go through this loop.
        const double v_coord      = -(bounds_pt.z() / std::max(scale_mm, 1e-6));
        if (!logged_sample) {
            TEXTUREBUMP_OVERLAY_LOG("overlay_vertex local=(" << local.x() << "," << local.y() << "," << local.z()
                << ") bounds_pt=(" << bounds_pt.x() << "," << bounds_pt.y() << "," << bounds_pt.z()
                << ") u_canonical=" << u_canonical << " u=" << u << " v=" << v_coord);
            logged_sample = true;
        }
        const Vec3f  local_f       = local.cast<float>();
        data.add_vertex(local_f, Vec2f(float(u), float(v_coord)));
    }
    for (const stl_triangle_vertex_indices& tri : its.indices)
        data.add_triangle(static_cast<unsigned int>(tri(0)), static_cast<unsigned int>(tri(1)), static_cast<unsigned int>(tri(2)));

    return data;
}

} // namespace Slic3r::GUI
