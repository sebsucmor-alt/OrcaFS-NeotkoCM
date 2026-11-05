// Primitive Generator 
#ifndef slic3r_GLGizmoPrimitiveGenerator_hpp_
#define slic3r_GLGizmoPrimitiveGenerator_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/NeotkoPrimitives.hpp"
#include "slic3r/GUI/NeotkoScad.hpp"

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct ImDrawList;

namespace Slic3r {
class ModelVolume;
namespace GUI {

// Panel with the shapes of libslic3r/NeotkoPrimitives: a row of shape icons, a live 3D sketch with
// numbered sizes, the sliders, "Add as" (object / part / negative / modifier), presets and the add
// button. NOT LibreMode-gated.
//
// A shape added from here keeps its recipe on the ModelVolume. Select it again (or keep it selected
// right after adding) and the sliders change it live, one undo step per drag.
class GLGizmoPrimitiveGenerator : public GLGizmoBase
{
public:
    GLGizmoPrimitiveGenerator(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent&) override { return false; }
    void data_changed(bool is_serializing) override;

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override { return true; }
    void        on_set_state() override;
    void        on_render() override {}
    void        on_render_input_window(float x, float y, float bottom_limit) override;

private:
    // Projected mesh for the ImGui sketch: triangles back to front, with a shade per triangle.
    struct Sketch
    {
        std::string                     recipe;
        float                           yaw = 0.f;
        std::vector<std::array<float, 2>> pts;
        std::vector<std::array<int, 3>> tris;
        std::vector<float>              shade;
        float                           x0 = 0.f, x1 = 0.f, y0 = 0.f, y1 = 0.f;
        size_t                          triangles = 0;
        double                          volume    = 0.;   // mm³
        Vec3d                           size      = Vec3d::Zero();
    };
    static void build_sketch(const std::string& id, const NeoPrim::Values& v, float yaw, Sketch& out);
    static void sketch_from_mesh(const indexed_triangle_set& its, const std::string& key, float yaw, Sketch& out);
    static void draw_sketch(ImDrawList* dl, const Sketch& s, float ax, float ay, float bx, float by, bool lit);

    const NeoPrim::Shape& shape() const;
    void select_shape(int idx);
    void find_target();          // the selected volume made here, or nullptr
    void apply_to_target();      // regenerate the target's mesh with the current numbers
    void apply_mesh_to_target(indexed_triangle_set its, const std::string& recipe);
    void apply_parts_to_target(const std::vector<NeoScad::Part>& parts, const std::string& recipe);

    // ---- OpenSCAD (phase 3) ----
    void render_scad(float win_w, bool& add_clicked);
    void scad_select(int idx, const std::map<std::string, std::string>* values = nullptr);
    void scad_reload_list();
    void scad_poll();              // picks up a finished run, starts a pending one
    void scad_request();           // the numbers changed: run again after a short pause
    std::string scad_recipe() const;
    void add_new();
    void vary();

    void load_presets();
    void save_presets() const;

    int             m_shape  = 0;
    NeoPrim::Values m_values;
    int             m_add_as = 0;  // 0 object, 1 part, 2 negative, 3 modifier
    float           m_yaw    = 0.6f;

    ModelVolume* m_target     = nullptr; // selected volume with a recipe
    int          m_target_obj = -1;
    bool         m_detached   = false;   // a new shape was picked while the target stays selected
    bool         m_drag_snapshot = false;

    bool                         m_scad_mode = false;
    std::vector<NeoScad::Script> m_scripts;
    int                          m_script = -1;
    std::vector<NeoScad::Param>  m_scad_params;
    std::string                  m_scad_binary;
    std::string                  m_scad_error;
    NeoScad::Runner              m_runner;
    int                          m_scad_wanted = -1;   // generation the panel waits for
    bool                         m_scad_pending = false;
    double                       m_scad_pending_t = 0.;
    double                       m_scad_mtime_check = 0.;
    long long                    m_scad_mtime = 0;
    indexed_triangle_set         m_scad_mesh;
    std::string                  m_scad_mesh_recipe; // recipe the mesh was made with
    std::string                  m_scad_run_recipe;  // recipe of the run being waited for
    std::string                  m_scad_source;      // text of the selected script
    int                          m_scad_mesh_serial = 0; // bumps on every new mesh: the sketch key
    std::vector<NeoScad::Part>   m_scad_parts;           // the pieces of the last run (one if not multi-part)
    bool                         m_target_multi = false; // target is a whole multi-part object
    bool                         m_scad_untrusted = false; // came inside a project: ask before running
    Sketch                       m_scad_sketch;

    Sketch                                           m_sketch;
    std::vector<Sketch>                              m_icons;
    std::vector<std::pair<std::string, std::string>> m_presets; // name, recipe
    char                                             m_preset_name[64] = {0};
};

} // namespace GUI
} // namespace Slic3r

#endif
