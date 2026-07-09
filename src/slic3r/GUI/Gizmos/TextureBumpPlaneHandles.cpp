#include "TextureBumpPlaneHandles.hpp"
#include <cmath>

namespace Slic3r::GUI {

Vec3d texture_bump_plane_normal(TextureProjectionAxis axis)
{
    switch (axis) {
        case TextureProjectionAxis::X: return Vec3d::UnitX();
        case TextureProjectionAxis::Y: return Vec3d::UnitY();
        case TextureProjectionAxis::Z:
        default:                       return Vec3d::UnitZ();
    }
}

Vec2d texture_bump_world_to_ab(TextureProjectionAxis axis, const Vec3d& center, const Vec3d& world)
{
    // Mirrors plane_components() (TextureBump.cpp, private to that .cpp) exactly.
    switch (axis) {
        case TextureProjectionAxis::X: return Vec2d(world.y() - center.y(), world.z() - center.z());
        case TextureProjectionAxis::Y: return Vec2d(world.x() - center.x(), world.z() - center.z());
        case TextureProjectionAxis::Z:
        default:                       return Vec2d(world.x() - center.x(), world.y() - center.y());
    }
}

Vec3d texture_bump_ab_to_world(TextureProjectionAxis axis, const Vec3d& center, double a, double b, double plane_coord)
{
    switch (axis) {
        case TextureProjectionAxis::X: return Vec3d(plane_coord, center.y() + a, center.z() + b);
        case TextureProjectionAxis::Y: return Vec3d(center.x() + a, plane_coord, center.z() + b);
        case TextureProjectionAxis::Z:
        default:                       return Vec3d(center.x() + a, center.y() + b, plane_coord);
    }
}

Vec3d texture_bump_ray_plane_intersection(const Vec3d& ray_origin, const Vec3d& ray_dir, const Vec3d& plane_point,
                                          const Vec3d& plane_normal)
{
    const double denom = ray_dir.dot(plane_normal);
    if (std::abs(denom) < 1e-9)
        return plane_point;
    const double t = (plane_point - ray_origin).dot(plane_normal) / denom;
    return ray_origin + t * ray_dir;
}

double texture_bump_plane_transform_yaw(const Transform3d& t)
{
    return std::atan2(t.linear()(1, 0), t.linear()(0, 0));
}

Vec2d texture_bump_plane_transform_pivot(const Transform3d& t)
{
    return Vec2d(t.translation().x(), t.translation().y());
}

Transform3d texture_bump_make_plane_transform(double yaw, const Vec2d& pivot)
{
    Transform3d t = Transform3d::Identity();
    t.translation() = Vec3d(pivot.x(), pivot.y(), 0.0);
    t.linear() = Eigen::AngleAxisd(yaw, Vec3d::UnitZ()).toRotationMatrix();
    return t;
}

} // namespace Slic3r::GUI
