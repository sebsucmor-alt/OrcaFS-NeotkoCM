#ifndef slic3r_GLGizmoPainterBase_hpp_
#define slic3r_GLGizmoPainterBase_hpp_

#include "GLGizmoBase.hpp"

#include "slic3r/GUI/GLModel.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Model.hpp"

#include <cereal/types/vector.hpp>
#include <GL/glew.h>

#include <memory>
#include <map>
#include <set>   // selected SVG parts
#include <functional>
#include <unordered_map>


namespace Slic3r::GUI {

enum class SLAGizmoEventType : unsigned char;
class ClippingPlane;
struct Camera;
class GLGizmoMmuSegmentation;
class Selection;

enum class PainterGizmoType {
    FDM_SUPPORTS,
    SEAM,
    MM_SEGMENTATION,
    FUZZY_SKIN
};

class TriangleSelectorGUI : public TriangleSelector {
public:
    explicit TriangleSelectorGUI(const TriangleMesh& mesh, float edge_limit = 0.6f)
        : TriangleSelector(mesh, edge_limit) {}
    virtual ~TriangleSelectorGUI() = default;

    virtual void render(ImGuiWrapper* imgui, const Transform3d& matrix);
    //void         render(const Transform3d& matrix) { this->render(nullptr, matrix); }
    void         set_wireframe_needed(bool need_wireframe) { m_need_wireframe = need_wireframe; }
    bool         get_wireframe_needed() { return m_need_wireframe; }

    // BBS
    void request_update_render_data(bool paint_changed = false)
    {
        m_update_render_data = true;
        m_paint_changed |= paint_changed;
    }

    // BBS
    static ColorRGBA enforcers_color;
    static ColorRGBA blockers_color;

#ifdef PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
    void render_debug(ImGuiWrapper* imgui);
    bool m_show_triangles{false};
    bool m_show_invalid{false};
#endif

protected:
    bool m_update_render_data = false;
    // BBS
    bool m_paint_changed = true;

    static ColorRGBA get_seed_fill_color(const ColorRGBA &base_color);

private:
    void update_render_data();

    GLModel                m_iva_enforcers;
    GLModel                m_iva_blockers;
    std::array<GLModel, 3> m_iva_seed_fills;
#ifdef PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
    std::array<GLModel, 3> m_varrays;
#endif // PRUSASLICER_TRIANGLE_SELECTOR_DEBUG

protected:
    GLModel                      m_paint_contour;

    void update_paint_contour();
    void render_paint_contour(const Transform3d& matrix);

    bool                                m_need_wireframe {false};
};

// BBS
struct TrianglePatch {
    std::vector<float>  patch_vertices;
    std::vector<int> triangle_indices;
    std::vector<int> facet_indices;
    EnforcerBlockerType type = EnforcerBlockerType::NONE;
    std::set<EnforcerBlockerType> neighbor_types;
    // if area is larger than GapAreaMax, stop accumulate left triangle areas to improve performance
    float area = 0.f;

    bool is_fragment() const;
};

class TriangleSelectorPatch : public TriangleSelectorGUI {
public:
    explicit TriangleSelectorPatch(const TriangleMesh& mesh, const std::vector<ColorRGBA> ebt_colors, float edge_limit = 0.6f)
        : TriangleSelectorGUI(mesh, edge_limit), m_ebt_colors(ebt_colors) {}
    virtual ~TriangleSelectorPatch() = default;

    // Render current selection. Transformation matrices are supposed
    // to be already set.
    void render(ImGuiWrapper* imgui, const Transform3d& matrix) override;
    // TriangleSelector.m_triangles => m_gizmo_scene.triangle_patches
    void update_triangles_per_type();
    // m_gizmo_scene.triangle_patches => TriangleSelector.m_triangles
    void update_selector_triangles();
    void update_triangles_per_patch();

    void set_ebt_colors(const std::vector<ColorRGBA> ebt_colors) { m_ebt_colors = ebt_colors; }
    void set_filter_state(bool is_filter_state);

    constexpr static float GapAreaMin = 0.f;
    constexpr static float GapAreaMax = 5.f;
    constexpr static float GapAreaStep = 0.2f;

    // BBS: fix me
    static float gap_area;

protected:
    // Release the geometry data, release OpenGL VBOs.
    void release_geometry();
    // Finalize the initialization of the geometry, upload the geometry to OpenGL VBO objects
    // and possibly releasing it if it has been loaded into the VBOs.
    void finalize_vertices();
    // Finalize the initialization of the indices, upload the indices to OpenGL VBO objects
    // and possibly releasing it if it has been loaded into the VBOs.
    void finalize_triangle_indices();

    void clear()
    {
        // BBS
        this->m_vertices_VBO_ids.clear();
        this->m_triangle_indices_VBO_ids.clear();
        this->m_triangle_indices_sizes.clear();

        for (TrianglePatch& patch : this->m_triangle_patches)
        {
            patch.patch_vertices.clear();
            patch.triangle_indices.clear();
        }
        this->m_triangle_patches.clear();
    }

    [[nodiscard]] inline bool has_VBOs(size_t triangle_indices_idx) const
    {
        assert(triangle_indices_idx < this->m_triangle_patches.size());
        return this->m_triangle_indices_VBO_ids[triangle_indices_idx] != 0;
    }

    //std::vector<float>          m_patch_vertices;
    std::vector<TrianglePatch>  m_triangle_patches;

    // When the triangle indices are loaded into the graphics card as Vertex Buffer Objects,
    // the above mentioned std::vectors are cleared and the following variables keep their original length.
    std::vector<size_t>         m_triangle_indices_sizes;

    // IDs of the Vertex Array Objects, into which the geometry has been loaded.
    // Zero if the VBOs are not sent to GPU yet.
    //unsigned int                m_vertices_VBO_id{ 0 };
    std::vector<unsigned int>   m_vertices_VBO_ids;
    std::vector<unsigned int>   m_triangle_indices_VBO_ids;

    std::vector<ColorRGBA> m_ebt_colors;

    bool                        m_filter_state = false;

private:
    void update_render_data();
    void render(int buffer_idx, bool show_wireframe=false);
};


// Following class is a base class for a gizmo with ability to paint on mesh
// using circular blush (such as FDM supports gizmo and seam painting gizmo).
// The purpose is not to duplicate code related to mesh painting.
class GLGizmoPainterBase : public GLGizmoBase
{
private:
    ObjectID m_old_mo_id;
    size_t m_old_volumes_size = 0;
    void on_render() override {}

public:
    GLGizmoPainterBase(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoPainterBase() override;
    void data_changed(bool is_serializing) override;
    virtual bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down);

    // Following function renders the triangles and cursor. Having this separated
    // from usual on_render method allows to render them before transparent
    // objects, so they can be seen inside them. The usual on_render is called
    // after all volumes (including transparent ones) are rendered.
    virtual void render_painter_gizmo() = 0;

    virtual const float get_cursor_radius_min() const { return CursorRadiusMin; }
    virtual const float get_cursor_radius_max() const { return CursorRadiusMax; }
    virtual const float get_cursor_radius_step() const { return CursorRadiusStep; }

    // BBS: just for CursorType::HeightRange
    virtual const float get_cursor_height_min() const { return CursorHeightMin; }
    virtual const float get_cursor_height_max() const { return CursorHeightMax; }
    virtual const float get_cursor_height_step() const { return CursorHeightStep; }

    /// <summary>
    /// Implement when want to process mouse events in gizmo
    /// Click, Right click, move, drag, ...
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>Return True when use the information and don't want to
    /// propagate it otherwise False.</returns>
    bool on_mouse(const wxMouseEvent &mouse_event) override;

protected:
    virtual void render_triangles(const Selection& selection) const;
    void render_cursor();
    void render_cursor_circle();
    void render_cursor_sphere(const Transform3d& trafo) const;
    // BBS
    void render_cursor_height_range(const Transform3d& trafo) const;
    void render_cursor_rectangle();
    void render_cursor_polygon();
    //BBS: add logic to distinguish the first_time_update and later_update
    virtual void update_model_object() = 0;
    virtual void update_from_model_object(bool first_update) = 0;

    virtual ColorRGBA get_cursor_sphere_left_button_color() const  { return { 0.0f, 0.0f, 1.0f, 0.25f }; }
    virtual ColorRGBA get_cursor_sphere_right_button_color() const { return { 1.0f, 0.0f, 0.0f, 0.25f }; }
    // BBS
    virtual ColorRGBA get_cursor_hover_color() const { return { 0.f, 0.f, 0.f, 0.25f }; }

    virtual EnforcerBlockerType get_left_button_state_type() const { return EnforcerBlockerType::ENFORCER; }
    virtual EnforcerBlockerType get_right_button_state_type() const { return EnforcerBlockerType::BLOCKER; }

    float m_cursor_radius = 1.f;
    // BBS
    float m_cursor_height = 0.2f;
    static constexpr float CursorRadiusMin  = 0.4f; // cannot be zero
    static constexpr float CursorRadiusMax  = 8.f;
    static constexpr float CursorRadiusStep = 0.2f;
    float m_precision_factor = 1.f;
    static constexpr float PrecisionFactorMin = 1.f;
    static constexpr float PrecisionFactorMax = 8.f;
    static constexpr float CursorHeightMin = 0.1f; // cannot be zero
    static constexpr float CursorHeightMax = 8.f;
    static constexpr float CursorHeightStep = 0.2f;

    // For each model-part volume, store status and division of the triangles.
    std::vector<std::unique_ptr<TriangleSelectorGUI>> m_triangle_selectors;

    TriangleSelector::CursorType m_cursor_type = TriangleSelector::SPHERE;

    enum class ToolType {
        BRUSH,
        BUCKET_FILL,
        SMART_FILL,
        // BBS
        GAP_FILL,
        RECTANGLE,
        POLYGON,
    };

    struct ProjectedMousePosition
    {
        Vec3f  mesh_hit;
        int    mesh_idx;
        size_t facet_idx;
    };

    // BBS: projected result of mouse height range for a mesh
    struct ProjectedHeightRange
    {
        float   z_world;
        int     mesh_idx;
        size_t  first_facet_idx;
    };

    bool     m_triangle_splitting_enabled = true;
    ToolType m_tool_type                  = ToolType::BRUSH;
    float    m_smart_fill_angle           = 30.f;

    Vec2d m_rect_start_corner = Vec2d::Zero();
    Vec2d m_rect_end_corner   = Vec2d::Zero();
    bool  m_rect_dragging     = false;

    // POLYGON tool state, a closed shape
    // made of nodes with optional Bezier handles (screen-space, canvas-local coords). Click adds a
    // corner node, click+drag adds a curved one. Closing the shape (click the first node with 3+
    // placed) no longer paints: the shape stays editable until shape_apply() (panel Apply), and
    // right-click / Cancel clears it. Curves are flattened to a polyline right before painting,
    // so TriangleSelector::PolygonProjectionCursor is fed a plain polygon exactly like before.
    struct ShapeNode
    {
        Vec2d p   = Vec2d::Zero();
        // Handle offsets relative to p. Both zero = sharp corner.
        Vec2d in  = Vec2d::Zero();
        Vec2d out = Vec2d::Zero();
        // Smooth node: dragging one handle mirrors the other (Alt-drag breaks it).
        bool  smooth = false;
        bool  is_curved() const { return !in.isZero() || !out.isZero(); }
    };
    enum class ShapeDrag { None, NewNodeHandle, Node, HandleIn, HandleOut, Move, Corner };
    // Last mouse position while dragging a whole closed shape (ShapeDrag::Move).
    Vec2d m_shape_move_last = Vec2d::Zero();
    std::vector<ShapeNode> m_shape_nodes;
    bool      m_shape_closed           = false;
    bool      m_shape_view_lock        = true;
    bool      m_shape_keep_after_apply = false;
    // Set when the camera moved under an unlocked, unfinished shape and it had to be dropped.
    bool      m_shape_lost_to_camera   = false;
    ShapeDrag m_shape_drag             = ShapeDrag::None;
    int       m_shape_drag_idx         = -1;
    // Camera view*projection when the first node was placed: the nodes are screen-space, so they
    // only mean something for this exact view.
    Matrix4d  m_shape_view_projection  = Matrix4d::Identity();
    static constexpr float  PolygonCloseRadiusPx       = 8.f;
    static constexpr double ShapeFlattenTolerancePx    = 0.25;
    static constexpr int    ShapeFlattenMaxSegments    = 64;
    static constexpr double ShapeCurveDragThresholdPx  = 3.0;

    void shape_clear();
    // Shape as a polyline (curves subdivided until flat within ShapeFlattenTolerancePx).
    std::vector<Vec2d> shape_flatten(bool closed) const;
    // Camera input is swallowed while drawing a shape with Lock view on.
    bool shape_view_locked() const { return m_tool_type == ToolType::POLYGON && m_shape_view_lock; }
    // Where the right button went down: a right CLICK cancels a drawn shape, a right DRAG pans.
    Vec2d m_shape_right_down_pos = Vec2d::Zero();
    // Paints the closed shape with new_state as a single undo step.
    void shape_apply(EnforcerBlockerType new_state, const std::string &snapshot_name);
    // SVG shapes. The SVG is cleaned (CSS classes, background, transparencies, SvgPaint), split into its colour ZONES, and each zone paints with the
    // filament the user picks for it. Moved, scaled and rotated as a whole, painted with
    // Apply/Erase; points are not edited. Returns false and fills m_shape_svg_error when the
    // file gives no shape.
    bool shape_load_svg(const std::string &path);
    std::string m_shape_svg_error;
    // One outline of the SVG: an outer contour or a hole of one zone's area.
    struct SvgRing
    {
        std::vector<Vec2d> pts;
        bool               hole = false;
        int                zone = -1;
    };
    // One colour of the SVG (SvgPaint::zones) and the filament it paints with.
    struct SvgZone
    {
        uint32_t rgb    = 0;      // 0xRRGGBB as in the SVG (mean colour for a gradient)
        bool     grad   = false;
        int      state  = 1;      // filament / Mixed Filament id = EnforcerBlockerType value
        bool     hidden = false;  // eye off: transparent, not painted, shows what is under it
    };
    std::string          m_svg_data;          // the cleaned SVG text
    std::vector<SvgZone> m_svg_zones;
    // Parts: the shapes set apart from their colour (own filament or off),
    // stored as in the decal, and the paint groups they give (paint_decal_groups: the colours,
    // then one group per part with its own filament). SvgRing::zone is a GROUP index.
    std::vector<PaintDecalPart> m_svg_parts;
    std::vector<PaintDecalZone> m_svg_groups;
    // Every shape of the SVG in its own frame (rings, mm, Y up) and its colour, for picking parts
    // on screen and outlining them. Built with the masks.
    struct SvgShape { std::vector<std::vector<Vec2d>> rings; int zone = -1; Vec2d lo = Vec2d::Zero(), hi = Vec2d::Zero(); };
    std::vector<SvgShape> m_svg_shapes;
    std::set<int>         m_svg_part_sel;          // selected parts (shape indices)
    int                   m_svg_part_hover  = -1;  // part under the mouse in the list, outlined
    bool                  m_svg_pick_parts  = false;   // a click on the SVG picks a part
    // The part override of shape k (nullptr if it follows its colour); `create` adds one.
    PaintDecalPart *svg_part(int shape, bool create);
    void            svg_prune_parts();         // drops overrides that change nothing
    // Topmost shape under a screen point (hidden parts included, so they can be turned back on).
    int             svg_pick_part(const Vec2d &screen) const;
    // A hand-drawn shape saved as a decal turned back into points to edit
    // (only SVGs written by shape_to_svg_session). The SVG session keeps its decal, filament and
    // mirror, so saving the shape updates the same decal.
    bool            svg_is_drawn_shape() const;
    bool            svg_to_shape_nodes();
    int             m_shape_from_decal       = -1;   // decal the shape on screen came from
    int             m_shape_from_state       = -1;   // its filament
    int             m_shape_from_mirror      = 0;
    bool            m_shape_from_readable    = false;
    // The decal being edited on screen (as an SVG or as points): its card is not drawn.
    int             editing_decal() const { return m_svg_edit_decal >= 0 ? m_svg_edit_decal : m_shape_from_decal; }
    GLModel         m_svg_part_halo_model;
    GLModel         m_svg_part_core_model;
    GLModel         m_svg_hover_halo_model;
    GLModel         m_svg_hover_core_model;
    // Zone areas in the SVG's own frame (mm, Y up) and the 2D placement that puts them on screen:
    // screen = m_svg_lin * local + m_svg_off. Hiding a zone rebuilds the areas, the placement stays.
    std::vector<SvgRing> m_svg_local_rings;
    Matrix2d             m_svg_lin = Matrix2d::Identity();
    Vec2d                m_svg_off = Vec2d::Zero();
    // Corner deform, a homography in the SVG frame applied before the
    // placement: screen = m_svg_lin * warp(local) + m_svg_off. Identity = not deformed.
    Matrix3d             m_svg_warp   = Matrix3d::Identity();
    bool                 m_svg_deform = false;   // corner handles shown and grabbed
    Vec2d svg_local_to_screen(const Vec2d &local) const;
    // Puts corner k (0..3 = the SVG box's lo, (hi.x, lo.y), hi, (lo.x, hi.y)) at a screen point;
    // refused (false) when the box would stop being convex.
    bool  svg_set_corner(int k, const Vec2d &screen);
    void  svg_reset_corners() { m_svg_warp = Matrix3d::Identity(); svg_update_screen_rings(); }
    bool  svg_is_deformed() const { return !m_svg_warp.isIdentity(1e-12); }
    std::vector<SvgRing> m_svg_rings;          // the same rings on screen (pixels, Y down)
    // Each zone's area as triangles in the SVG's own frame, for the preview fill (mapped to the
    // screen through the placement, so it never depends on how the rings turn on screen).
    std::vector<std::vector<Vec2d>> m_svg_local_tris;
    // Box of the SVG's areas in its own frame: grabbing anywhere inside it moves the SVG (thin or
    // holed drawings had almost nothing to click on), and it is drawn as a frame with a move cross.
    Vec2d                m_svg_local_lo = Vec2d::Zero();
    Vec2d                m_svg_local_hi = Vec2d::Zero();
    // The box's corners on screen, in order (bottom-left, bottom-right, top-right, top-left).
    std::array<Vec2d, 4> svg_screen_box() const;
    std::vector<GLModel> m_svg_fill_models;    // one preview fill per zone
    void svg_rebuild_masks();
    void svg_update_screen_rings();
    std::string          m_svg_name;           // file stem, becomes the decal's name

    // Paint decals (ModelObject::paint_decals). The SVG on screen is the
    // editing session: "Pin to this view" turns it into a decal card anchored to the object, and
    // "Edit" brings a decal back to the screen from its own view. m_svg_edit_decal = the decal
    // being edited (-1: a new one); m_svg_pending_edit = a decal to load on the next frame, once
    // the camera matrices of its view are up to date.
    int  m_svg_edit_decal   = -1;
    int  m_svg_pending_edit = -1;
    // Instance transform of the selected object, and every model part's transform (and without
    // translation), as the masks use them.
    Transform3d model_part_trafos(std::vector<Transform3d> &trafos, std::vector<Transform3d> &trafos_no_translate) const;
    bool svg_pin_to_view();
    // The decal the SVG on screen would become if pinned now (current camera); false if it can't.
    bool svg_build_decal(PaintDecal &decal);
    int  m_svg_mirror = 0;
    bool m_svg_mirror_readable = false;
    // Vector mode: a click only starts a shape after "Draw shape", so a
    // plain left drag (on the object or the empty canvas) orbits the camera as usual.
    bool m_shape_draw_armed = false;
    virtual bool shape_draw_needs_arming() const { return false; }
    void svg_edit_decal(int idx);
    void svg_finish_pending_edit();
    // Paints decals idxs (in that order) onto the object; erase = remove their paint instead.
    void decal_apply(const std::vector<size_t> &idxs, bool erase);
    // Zone areas of a decal in its SVG frame (hidden zones transparent), cached per decal.
    const std::vector<SvgRing> &decal_rings(size_t idx);
    // centre of the object's box in the object frame, where the decal mirror planes cross.
    Vec3d decal_mirror_centre() const;
    // "Pick centre" armed: the next left click on the object sets the mirror centre there.
    bool  m_mirror_pick_armed = false;
    // Decal cards: floating on a sphere, or laid on the surface as they will paint.
    bool m_decal_on_surface = true;
    void render_decal_cards(const PaintDecal *on_screen);
    void render_decal_cards_body(const ModelObject *decal_mo, const PaintDecal *on_screen,
                                 const std::function<Vec2f(const Vec2d &)> &to_ndc, float cnv_width, float cnv_height);
    void render_mirror_marker();
    // Brush symmetry (ModelObject::paint_symmetry.brush): every brush
    // stroke repeated through the mirror centre, on the surface point nearest to the mirrored one
    // (skipped where the object has nothing there). Ghost cursors show where the copies land.
    void brush_paint_mirrored(const std::vector<ProjectedMousePosition> &positions, const Transform3d &src_trafo,
                              EnforcerBlockerType new_state, const std::vector<Transform3d> &trafos,
                              const std::vector<Transform3d> &trafos_not_translate);
    // `eye_obj` (object frame, may be null): the mirrored camera. The point is first looked for along
    // the mirrored ray, like the stroke itself was found (a clean line on a near-symmetric surface);
    // the nearest surface point only if that ray misses.
    bool symmetry_surface_point(const Vec3d &obj_pt, const std::vector<Transform3d> &trafos, const Transform3d &inst,
                                double max_dist, int &mesh, int &facet, Vec3f &hit_mesh, const Vec3d *eye_obj = nullptr) const;
    void render_brush_symmetry(const std::vector<Transform3d> &trafos, bool hovering);
    GLModel m_brush_ghost_model;
    virtual bool show_decal_cards_while_brushing() const { return false; }
    // "Move on object" armed: a left drag on the object slides the selected decal over it,
    // turning it around the object's centre so the grabbed point follows the mouse.
    bool       m_decal_move_armed = false;
    int        m_decal_drag       = -1;     // decal being slid, -1 = none
    PaintDecal m_decal_drag_start;          // its state when the drag began
    Vec3d      m_decal_drag_anchor = Vec3d::Zero();   // grabbed point, object frame, minus the pivot
    virtual int selected_decal_index() const { return -1; }
    // Turn (degrees) and scale a stored decal on its own card, around the centre of its SVG:
    // d = start turned and scaled (start = the decal before the gesture began).
    void decal_turn_scale(size_t idx, const PaintDecal &start, double degrees, double factor);
    bool mouse_on_object(const Vec2d &mouse, Vec3d &obj_pt);   // object-frame hit under the mouse
    struct SurfaceCard { std::vector<std::vector<Vec3d>> rings; std::vector<std::vector<Vec3d>> tris; };
    std::map<std::string, SurfaceCard> m_surface_cache;
    const SurfaceCard &decal_surface(const PaintDecal &d, const std::vector<SvgRing> &rings,
                                     const std::vector<std::vector<Vec2d>> *tris, bool live);
    GLModel m_mirror_centre_model;
    // tris[zone]: that zone's area as triangles (3 points each) in the SVG frame, for the preview fill.
    struct DecalRingsCache { std::string key; std::vector<SvgRing> rings; std::vector<std::vector<Vec2d>> tris; std::vector<PaintDecalZone> groups; };
    std::vector<DecalRingsCache> m_decal_rings_cache;
    GLModel m_decal_halo_model;
    GLModel m_shape_halo_model;   // dark halo under the drawn shape's outline and handles
    // turns the closed drawn shape into an SVG on screen (one colour, the
    // selected filament), exactly where it is, so it can be pinned as a decal like any SVG.
    bool shape_to_svg_session();
    GLModel m_decal_core_model;
    GLModel m_decal_back_halo_model;   // cards facing away from the camera, fainter
    GLModel m_decal_back_core_model;
    // the cards filled with their filaments' colours, translucent, to judge
    // the result before Apply. One model per (card, colour), rebuilt every frame.
    bool                 m_decal_preview_colours = true;
    std::vector<GLModel> m_decal_fill_models;
    // Scale (percent) and rotation (degrees) of the SVG set, as set by the panel sliders.
    float m_shape_scale_pct  = 100.f;
    // width / height of the SVG on screen, along its own axes.
    float m_svg_width_pct    = 100.f;
    float m_svg_height_pct   = 100.f;
    void  svg_stretch(int axis, double factor);
    float m_shape_rotate_deg = 0.f;
    bool shape_contains(const Vec2d &p) const;
    void shape_move(const Vec2d &delta);
    void shape_scale(double factor);
    void shape_rotate(double degrees);
    // Translucent preview fill of a closed shape; the MMU gizmo uses the selected filament colour.
    virtual ColorRGBA get_shape_fill_color() const { return { 0.0f, 1.0f, 0.3f, 0.25f }; }
    // Colour of a paint state (filament id) for the SVG zone previews; MMU gizmo: the filament's.
    virtual ColorRGBA get_state_color(int /*state*/) const { return get_shape_fill_color(); }
    // Filament an SVG colour starts with when loaded; MMU gizmo: the closest filament colour.
    virtual int svg_default_state(uint32_t /*rgb*/) const { return int(get_left_button_state_type()); }

    bool     m_paint_on_overhangs_only          = false;
    float    m_highlight_by_angle_threshold_deg = 0.f;

    GLModel m_circle;
    Vec2d m_old_center{ Vec2d::Zero() };
    float m_old_cursor_radius{ 0.0f };
    GLModel m_rect_overlay;
    Vec2d   m_old_rect_start_corner = Vec2d::Zero();
    Vec2d   m_old_rect_end_corner   = Vec2d::Zero();
    // Polygon overlay is rebuilt unconditionally each frame: point count is tiny (a handful of
    // clicks) and it changes on almost every frame anyway (rubber-band segment follows the mouse).
    GLModel m_polygon_overlay;
    GLModel m_polygon_fill_overlay;
    GLModel m_polygon_handles_overlay;

    static constexpr float SmartFillAngleMin  = 0.0f;
    static constexpr float SmartFillAngleMax  = 90.f;
    static constexpr float SmartFillAngleStep = 1.f;

    // Orca: paint behavior enchancement
    bool m_vertical_only = false;
    bool m_horizontal_only = false;

    // It stores the value of the previous mesh_id to which the seed fill was applied.
    // It is used to detect when the mouse has moved from one volume to another one.
    int      m_seed_fill_last_mesh_id     = -1;

    enum class Button {
        None,
        Left,
        Right
    };

    struct ClippingPlaneDataWrapper
    {
        std::array<float, 4> clp_dataf;
        std::array<float, 2> z_range;
    };

    ClippingPlaneDataWrapper get_clipping_plane_data() const;

    TriangleSelector::ClippingPlane get_clipping_plane_in_volume_coordinates(const Transform3d &trafo) const;

private:
    std::vector<std::vector<ProjectedMousePosition>> get_projected_mouse_positions(const Vec2d &mouse_position, double resolution, const std::vector<Transform3d> &trafo_matrices) const;

    std::vector<ProjectedHeightRange> get_projected_height_range(const Vec2d& mouse_position, double resolution, const std::vector<const ModelVolume*>& part_volumes, const std::vector<Transform3d>& trafo_matrices) const;

    void apply_rectangle_mask(EnforcerBlockerType new_state);
    // Applies the POLYGON mask (m_shape_nodes, flattened) to every mesh, called from shape_apply().
    void apply_polygon_mask(EnforcerBlockerType new_state, const std::vector<Vec2d> &flat);
    // All rings of an SVG (outers + holes) in one pass per mesh.
    void apply_polygon_set_mask(EnforcerBlockerType new_state, const std::vector<std::vector<Vec2d>> &rings, const std::vector<bool> &is_hole);
    // Builds the cursor for one model part: mesh-local camera points, view*projection, viewport,
    // part transform and clipping plane.
    using AreaCursorFactory = std::function<std::unique_ptr<TriangleSelector::Cursor>(
        const Vec3f &, const Vec3f &, const Matrix4d &, const std::array<int, 4> &, const Transform3d &, const TriangleSelector::ClippingPlane &)>;
    // One select_patch() per model part of the object, with the cursor make_cursor builds.
    void apply_area_cursor(EnforcerBlockerType new_state, const AreaCursorFactory &make_cursor);
    // Occlusion test for the rectangle / polygon masks on mesh mesh_idx (mesh coords): a ray from the point towards the camera must
    // not hit any model part of the object. Results are cached per point.
    TriangleSelector::Cursor::VisibilityTest make_visibility_test(size_t mesh_idx, const std::vector<Transform3d> &trafo_matrices) const;
    // The same test towards any viewer: rays to eye_world (perspective)
    // or along to_eye_world (ortho). The camera version above calls it with the camera.
    TriangleSelector::Cursor::VisibilityTest make_visibility_test_toward(size_t mesh_idx, const std::vector<Transform3d> &trafo_matrices,
        bool perspective, const Vec3d &eye_world, const Vec3d &to_eye_world) const;

    bool is_mesh_point_clipped(const Vec3d& point, const Transform3d& trafo) const;
    void update_raycast_cache(const Vec2d& mouse_position,
                              const Camera& camera,
                              const std::vector<Transform3d>& trafo_matrices) const;

    static std::shared_ptr<GLModel> s_sphere;

    bool m_internal_stack_active = false;
    bool m_schedule_update = false;
    Vec2d m_last_mouse_click = Vec2d::Zero();

    Button m_button_down = Button::None;
    EState m_old_state = Off; // to be able to see that the gizmo has just been closed (see on_set_state)

    // Following cache holds result of a raycast query. The queries are asked
    // during rendering the sphere cursor and painting, this saves repeated
    // raycasts when the mouse position is the same as before.
    struct RaycastResult {
        Vec2d mouse_position;
        int mesh_id;
        Vec3f hit;
        size_t facet;
    };
    mutable RaycastResult m_rr = {Vec2d::Zero(), -1, Vec3f::Zero(), 0};

    // BBS
    struct CutContours
    {
        TriangleMesh mesh;
        GLModel contours;
        double cut_z{ 0.0 };
        Vec3d position{ Vec3d::Zero() };
        Vec3d shift{ Vec3d::Zero() };
        ObjectID object_id;
        int instance_idx{ -1 };
    };
    mutable std::vector<CutContours> m_cut_contours;
    mutable int                      m_volumes_index = 0;
    mutable float       m_cursor_z{0};
    mutable double      m_height_start_z_in_imgui{0};
    mutable bool        m_is_set_height_start_z_by_imgui{false};
    mutable Vec2i32       m_height_start_pos{0, 0};
    mutable bool        m_is_cursor_in_imgui{false};
    BoundingBoxf3 bounding_box() const;
    void update_contours(int i, const TriangleMesh& vol_mesh, float cursor_z, float max_z, float min_z) const;

protected:
    void on_set_state() override;
    virtual void on_opening() = 0;
    virtual void on_shutdown() = 0;
    virtual PainterGizmoType get_painter_type() const = 0;

    bool on_is_activable() const override;
    bool on_is_selectable() const override;
    void on_load(cereal::BinaryInputArchive& ar) override;
    void on_save(cereal::BinaryOutputArchive& ar) const override {}
    CommonGizmosDataID on_get_requirements() const override;
    bool wants_enter_leave_snapshots() const override { return true; }

    virtual wxString handle_snapshot_action_name(bool shift_down, Button button_down) const = 0;

    friend class ::Slic3r::GUI::GLGizmoMmuSegmentation;
};


} // namespace Slic3r::GUI

#endif // slic3r_GLGizmoPainterBase_hpp_
