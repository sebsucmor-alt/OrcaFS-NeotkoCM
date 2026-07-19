#ifndef slic3r_PerimeterGenerator_hpp_
#define slic3r_PerimeterGenerator_hpp_

#include "libslic3r.h"
#include <vector>
#include "Layer.hpp"
#include "Flow.hpp"
#include "Polygon.hpp"
#include "PrintConfig.hpp"
#include "SurfaceCollection.hpp"

// NEOTKO_TEXTUREBUMP_TAG — forward declaration only (no #include) to avoid a circular dependency:
// TextureBump.hpp includes this header, the same way FuzzySkin.hpp does.
namespace Slic3r::Feature::TextureBump {
class TextureBumpTable;
class TextureBumpTableMap;
struct PaintedTextureBumpZone;
}

namespace Slic3r {
struct FuzzySkinConfig
{
    FuzzySkinType type;
    coord_t       thickness;
    coord_t       point_distance;
    bool          fuzzy_first_layer;
    NoiseType     noise_type;
    double        noise_scale;
    int           noise_octaves;
    double        noise_persistence;
    FuzzySkinMode mode;

    bool operator==(const FuzzySkinConfig& r) const
    {
        return type == r.type
            && thickness == r.thickness
            && point_distance == r.point_distance
            && fuzzy_first_layer == r.fuzzy_first_layer
            && noise_type == r.noise_type
            && noise_scale == r.noise_scale
            && noise_octaves == r.noise_octaves
            && noise_persistence == r.noise_persistence
            && mode == r.mode;
    }

    bool operator!=(const FuzzySkinConfig& r) const { return !(*this == r); }
};

// NEOTKO_TEXTUREBUMP_TAG — same shape/role as FuzzySkinConfig above, kept as its own struct so
// Texture Bump never shares storage/behaviour with Fuzzy Skin. See docs/ATTRIBUTION_TEXTURE_BUMP.md.
struct TextureBumpConfig
{
    TextureBumpType       type;
    coord_t               thickness;
    coord_t               point_distance;
    bool                  first_layer;
    TextureProjectionMode projection_mode;
    TextureProjectionAxis axis;
    double                scale; // mm covered by the full image width/height
    int                   repeat_u; // horizontal repeat count, multiplies with Cubic's fixed 4-face period
    double                max_angle_rad;
    double                blur_strength;
    std::string           image_path;
    // NEOTKO_TEXTUREBUMP_TAG — Fase 4.2 (docs/ATTRIBUTION_TEXTURE_BUMP.md §5 point 4): orientable
    // projection plane, beyond the 3 fixed axes above. Restricted at the point of APPLICATION
    // (compute_u()/plane_components() in TextureBump.cpp) to yaw (rotation around world Z) + XY
    // pivot translation only -- stored as a full Transform3d (not just an angle+point) so a future
    // Fase 4b (full 3D tilt) only needs to widen what's ALLOWED here, not migrate storage again.
    // Identity == the 3 legacy fixed axes still behave bit-for-bit as before this field existed.
    // Populated from ModelObject::texture_bump_plane_transform for the base/object-wide config
    // (PrintObject::make_perimeters()), or from the zone's own copy for painted zones
    // (TextureBumpZoneProfile::config.plane_transform, TextureBumpZone.hpp) -- the two are
    // independent, same as every other field of this struct.
    Transform3d           plane_transform{ Transform3d::Identity() };

    bool operator==(const TextureBumpConfig& r) const
    {
        return type == r.type
            && thickness == r.thickness
            && point_distance == r.point_distance
            && first_layer == r.first_layer
            && projection_mode == r.projection_mode
            && axis == r.axis
            && scale == r.scale
            && repeat_u == r.repeat_u
            && max_angle_rad == r.max_angle_rad
            && blur_strength == r.blur_strength
            && image_path == r.image_path
            && plane_transform.matrix() == r.plane_transform.matrix();
    }

    bool operator!=(const TextureBumpConfig& r) const { return !(*this == r); }
};
}

namespace std {
template<> struct hash<Slic3r::FuzzySkinConfig>
{
    size_t operator()(const Slic3r::FuzzySkinConfig& c) const noexcept
    {
        std::size_t seed = std::hash<Slic3r::FuzzySkinType>{}(c.type);
        boost::hash_combine(seed, std::hash<coord_t>{}(c.thickness));
        boost::hash_combine(seed, std::hash<coord_t>{}(c.point_distance));
        boost::hash_combine(seed, std::hash<bool>{}(c.fuzzy_first_layer));
        boost::hash_combine(seed, std::hash<Slic3r::NoiseType>{}(c.noise_type));
        boost::hash_combine(seed, std::hash<double>{}(c.noise_scale));
        boost::hash_combine(seed, std::hash<int>{}(c.noise_octaves));
        boost::hash_combine(seed, std::hash<double>{}(c.noise_persistence));
        return seed;
    }
};

template<> struct hash<Slic3r::TextureBumpConfig>
{
    size_t operator()(const Slic3r::TextureBumpConfig& c) const noexcept
    {
        std::size_t seed = std::hash<Slic3r::TextureBumpType>{}(c.type);
        boost::hash_combine(seed, std::hash<coord_t>{}(c.thickness));
        boost::hash_combine(seed, std::hash<coord_t>{}(c.point_distance));
        boost::hash_combine(seed, std::hash<bool>{}(c.first_layer));
        boost::hash_combine(seed, std::hash<Slic3r::TextureProjectionMode>{}(c.projection_mode));
        boost::hash_combine(seed, std::hash<Slic3r::TextureProjectionAxis>{}(c.axis));
        boost::hash_combine(seed, std::hash<double>{}(c.scale));
        boost::hash_combine(seed, std::hash<int>{}(c.repeat_u));
        boost::hash_combine(seed, std::hash<double>{}(c.max_angle_rad));
        boost::hash_combine(seed, std::hash<double>{}(c.blur_strength));
        boost::hash_combine(seed, std::hash<std::string>{}(c.image_path));
        // NEOTKO_TEXTUREBUMP_TAG — Fase 4.2: hash all 16 matrix coefficients rather than deriving
        // yaw/pivot from them -- guarantees this stays exactly consistent with operator=='s own
        // full-matrix compare even if a future Fase 4b widens what plane_transform is allowed to
        // hold (see PerimeterGenerator.hpp's struct comment).
        {
            const auto& m = c.plane_transform.matrix();
            for (int row = 0; row < 4; ++row)
                for (int col = 0; col < 4; ++col)
                    boost::hash_combine(seed, std::hash<double>{}(m(row, col)));
        }
        return seed;
    }
};
} // namespace std

namespace Slic3r {

class PerimeterGenerator {
public:
    // Inputs:
    const SurfaceCollection     *slices;
    const LayerRegionPtrs       *compatible_regions;
    const ExPolygons            *upper_slices;
    const SurfaceCollection     *upper_slices_same_region;
    const ExPolygons            *lower_slices;
    double                       layer_height;
    int                          layer_id;
    coordf_t                     slice_z;
    Flow                         perimeter_flow;
    Flow                         ext_perimeter_flow;
    Flow                         overhang_flow;
    Flow                         solid_infill_flow;
    const PrintRegionConfig     *config;
    const PrintObjectConfig     *object_config;
    const PrintConfig           *print_config;
    // Outputs:
    ExtrusionEntityCollection   *loops;
    ExtrusionEntityCollection   *gap_fill;
    SurfaceCollection           *fill_surfaces;
    //BBS
    ExPolygons                  *fill_no_overlap;

    //BBS
    Flow                        smaller_ext_perimeter_flow;
    std::vector<Polygons>       m_lower_polygons_series;
    std::vector<Polygons>       m_external_lower_polygons_series;
    std::vector<Polygons>       m_smaller_external_lower_polygons_series;

    bool                                            has_fuzzy_skin = false;
    bool                                            has_fuzzy_hole = false;
    std::unordered_map<FuzzySkinConfig, ExPolygons> regions_by_fuzzify;

    // NEOTKO_TEXTUREBUMP_TAG — same role as the two fields above, kept separate from fuzzy skin.
    bool                                                has_texture_bump = false;
    bool                                                has_texture_bump_hole = false;
    std::unordered_map<TextureBumpConfig, ExPolygons>   regions_by_texture_bump;
    // NEOTKO_TEXTUREBUMP_TAG — Fase 3: one table per distinct TextureBumpConfig in use on the
    // object (was a single shared table built from region(0) only). Owned by PrintObject, set by
    // whoever constructs this PerimeterGenerator for a given layer; nullptr is a valid "no table
    // built" state (feature disabled for every region/zone of this object).
    const Feature::TextureBump::TextureBumpTableMap*   texture_bump_tables = nullptr;
    // NEOTKO_TEXTUREBUMP_TAG -- fix (2026-07-08, real bug found reading the code, not guessed):
    // group_region_by_texture_bump() rebuilds a TextureBumpConfig per region from PrintRegionConfig
    // alone (TextureBump.cpp), which has NO option to carry plane_transform (it lives on
    // ModelObject, not PrintRegionConfig -- see TextureBumpConfig::plane_transform's own comment).
    // Without this field that rebuilt cfg silently defaulted to Identity via the struct's in-class
    // initializer, so TextureBumpTableMap::find(cfg) (texture_bump_extrusion_line()) never matched
    // the table actually built with the real transform (PrintObject::make_perimeters(), which DOES
    // read it from the ModelObject) the moment the user rotated/moved the projection plane (any
    // non-zero yaw or pivot) -- the object-level ("All"/AllWalls) case then applied silently NO
    // effect at slice time, while painted zones (their own config copy, never rebuilt from
    // PrintRegionConfig) were unaffected and worked. Set alongside texture_bump_tables below.
    Transform3d                                         texture_bump_plane_transform{ Transform3d::Identity() };
    // NEOTKO_TEXTUREBUMP_TAG — Fase 3: painted zones already resolved for THIS layer (see
    // TextureBump::painted_texture_bump_zones_in_layer), owned by whoever constructs this
    // PerimeterGenerator (LayerRegion::make_perimeters keeps the vector alive for the call).
    // nullptr/empty is the common "nothing painted" case.
    const std::vector<Feature::TextureBump::PaintedTextureBumpZone>* painted_texture_bump_zones = nullptr;

    // NEOTKO_NEOSTITCH_TAG — Z-Stitch Interlock (docs/FUTURE/NEOSTITCH_PLAN.md): object-centered mm
    // bounds (same convention as TextureBump's per-table bounds, PrintObject.cpp's tb_bounds), fed
    // to TextureBump::compute_u() for the world-stable angular coordinate the notch/fill signal is
    // phased on. Set by LayerRegion::make_perimeters(); a default-constructed (empty) box is a
    // harmless no-op when NeoStitch is disabled (F1: no PrintRegionConfig gate yet, always set).
    BoundingBoxf3 neostitch_bounds;

    PerimeterGenerator(
        // Input:
        const SurfaceCollection*    slices,
        const LayerRegionPtrs       *compatible_regions,
        double                      layer_height,
        coordf_t                    slice_z,
        Flow                        flow,
        const PrintRegionConfig*    config,
        const PrintObjectConfig*    object_config,
        const PrintConfig*          print_config,
        const bool                  spiral_mode,
        // Output:
        // Loops with the external thin walls
        ExtrusionEntityCollection*  loops,
        // Gaps without the thin walls
        ExtrusionEntityCollection*  gap_fill,
        // Infills without the gap fills
        SurfaceCollection*          fill_surfaces,
        //BBS
        ExPolygons*                 fill_no_overlap,
        // NEOTKO_TEXTUREBUMP_TAG — optional, defaults to nullptr so every existing call site
        // keeps compiling unchanged.
        const Feature::TextureBump::TextureBumpTableMap* texture_bump_tables = nullptr)
        : slices(slices), compatible_regions(compatible_regions), upper_slices(nullptr), lower_slices(nullptr), layer_height(layer_height),
            slice_z(slice_z), layer_id(-1), perimeter_flow(flow), ext_perimeter_flow(flow),
            overhang_flow(flow), solid_infill_flow(flow),
            config(config), object_config(object_config), print_config(print_config),
            texture_bump_tables(texture_bump_tables),
            m_spiral_vase(spiral_mode),
            m_scaled_resolution(scaled<double>(print_config->resolution.value > EPSILON ? print_config->resolution.value : EPSILON)),
            loops(loops), gap_fill(gap_fill), fill_surfaces(fill_surfaces), fill_no_overlap(fill_no_overlap),
            m_ext_mm3_per_mm(-1), m_mm3_per_mm(-1), m_mm3_per_mm_overhang(-1), m_ext_mm3_per_mm_smaller_width(-1)
        {}

    void        process_classic();
    void        process_arachne();

    void        add_infill_contour_for_arachne( ExPolygons infill_contour, int loops, coord_t ext_perimeter_spacing, coord_t perimeter_spacing, coord_t min_perimeter_infill_spacing, coord_t spacing, bool is_inner_part );

    double      ext_mm3_per_mm()        const { return m_ext_mm3_per_mm; }
    double      mm3_per_mm()            const { return m_mm3_per_mm; }
    double      mm3_per_mm_overhang()   const { return m_mm3_per_mm_overhang; }
    //BBS
    double      smaller_width_ext_mm3_per_mm()   const { return m_ext_mm3_per_mm_smaller_width; }
    Polygons    lower_slices_polygons() const { return m_lower_slices_polygons; }

private:
    std::vector<Polygons>     generate_lower_polygons_series(float width);
    void split_top_surfaces(const ExPolygons &orig_polygons, ExPolygons &top_fills, ExPolygons &non_top_polygons, ExPolygons &fill_clip) const;
    void apply_extra_perimeters(ExPolygons& infill_area);
    void process_no_bridge(Surfaces& all_surfaces, coord_t perimeter_spacing, coord_t ext_perimeter_width);

private:
    bool        m_spiral_vase;
    double      m_scaled_resolution;
    double      m_ext_mm3_per_mm;
    double      m_mm3_per_mm;
    double      m_mm3_per_mm_overhang;
    //BBS
    double      m_ext_mm3_per_mm_smaller_width;
    Polygons    m_lower_slices_polygons;
};

}

#endif
