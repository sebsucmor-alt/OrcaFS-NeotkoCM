#ifndef slic3r_SvgPaint_hpp_
#define slic3r_SvgPaint_hpp_

// SVG shapes for painting (multi-material painter, Vector mode). The SVG is read with nanosvg
// in mm, cleaned (CSS classes inlined, background dropped, transparencies resolved), and split
// two ways: into colour ZONES (what the user sees as one colour of the SVG) and into its single
// SHAPES. Masks are occlusion-resolved like a viewer draws the SVG: later shapes cover earlier ones.

#include "libslic3r.h"
#include "ExPolygon.hpp"
#include "Polygon.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace SvgPaint {

// One colour of the SVG: a flat colour (nearly equal ones, ΔE2000 < 2, go together) or one linear
// gradient. First-appearance order; the index is the stable identity a decal stores.
struct Zone { uint32_t rgb = 0; bool grad = false; int shapes = 0; };
// One shape of the SVG (a fill or a stroke), its rings in the target frame.
struct Shape { uint32_t rgb = 0; bool is_stroke = false; Polygons rings;
               bool grad = false; Vec2d g0 = Vec2d::Zero(), g1 = Vec2d::Zero(); };

std::vector<Zone>  zones(const std::string& svg_data);
std::vector<int>   shape_zones(const std::string& svg_data);   // zone of each shape
std::vector<Shape> shapes(const std::string& svg_data, const Transform3d& to_target);

// The area of each group, occlusion resolved. shape_group[k] = group of shape k, -1 = transparent
// (it does not cover what is under it); groups >= n_groups are dropped.
std::vector<ExPolygons> group_masks(const std::string& svg_data, const Transform3d& to_target,
                                    const std::vector<int>& shape_group, size_t n_groups);
// group_masks() with the groups = the zones; zone_hidden[z] makes zone z transparent.
std::vector<ExPolygons> zone_masks(const std::string& svg_data, const Transform3d& to_target,
                                   const std::vector<bool>& zone_hidden = {});

// nanosvg does not read <style>: `.class { ... }` rules are inlined into each element's style.
std::string inline_css_classes(const std::string& svg);
// What prints is what a viewer shows: a background rectangle covering >= 95 % of the drawing is
// dropped (unless keep_background), semi-transparent shapes are blended over what is under them,
// everything is clipped to the viewBox, and each colour becomes one region. Returns a new SVG of
// flat, opaque colours in mm, or the text as it was when there is nothing to flatten.
struct FlattenInfo { bool changed = false; bool background_dropped = false;
                     int translucent = 0; int colors_out = 0; int clipped = 0; };
std::string flatten(const std::string& svg, bool keep_background, FlattenInfo* info = nullptr);

} // namespace SvgPaint
} // namespace Slic3r

#endif // slic3r_SvgPaint_hpp_
