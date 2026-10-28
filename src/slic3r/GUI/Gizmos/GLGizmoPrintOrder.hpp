// NEOTKO_PRINTORDER_TAG_START — 2_49
#ifndef slic3r_GLGizmoPrintOrder_hpp_
#define slic3r_GLGizmoPrintOrder_hpp_

#include "GLGizmoBase.hpp"
#include "libslic3r/Color.hpp"

#include <map>
#include <tuple>
#include <vector>

namespace Slic3r {
class ModelObject;
namespace GUI {

// «Print order (Experimental, handle with care)» — LibreMode only.
// Two levels, never both on screen at once:
//  - Objects: each object gets a number (1, 2, 3...) or stays Free. In every layer, and in every
//    filament pass, the numbered objects print in that order; Free ones go after them, as Orca does.
//  - Parts (an object of several parts, e.g. an Assembly): its parts get 2.1, 2.2... and its
//    islands follow them inside the object's turn. Entered with another click on a numbered
//    object of several parts (the Align & Construct scheme: no double click, macOS sends a quick
//    second click as LeftDown + LeftDClick), or with «Parts» in its row. «Back», or a click on
//    empty space, goes back to objects.
// Mouse: click = next number; right click / ⌥ + click = back to Free. What is under the mouse
// lights up BEFORE the click; the rest of the scene goes see-through.
// Numbers live on ModelObject / ModelVolume::neotko_print_rank; Plater mirrors them into plate keys
// that only redo the G-code (docs/FUTURE/PRINT_ORDER_GIZMO_2_49.md).
class GLGizmoPrintOrder : public GLGizmoBase
{
public:
    GLGizmoPrintOrder(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    bool on_mouse(const wxMouseEvent& mouse_event) override;
    void data_changed(bool is_serializing) override;

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_set_state() override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;

private:
    // ---- objects ----
    std::vector<int> numbered_objects() const; // Model::objects indices, by number
    std::vector<int> free_objects() const;
    int              next_object_rank() const;
    void             set_object_rank(int obj_idx, int rank, const char* snapshot);
    void             swap_objects(int a, int b);
    void             start_over();

    // ---- parts of m_edit_obj ----
    static int       model_part_count(const ModelObject* mo);
    std::vector<int> numbered_parts() const; // ModelObject::volumes indices, by number
    std::vector<int> free_parts() const;
    int              next_part_rank() const;
    void             set_part_rank(int vol_idx, int rank, const char* snapshot);
    void             swap_parts(int a, int b);
    void             enter_parts(int obj_idx);
    void             leave_parts();

    void compact();  // numbers back to 1..n (ties stay tied), objects and every object's parts
    void commit();   // compact + redo the G-code only

    // ---- look ----
    void apply_highlight();   // see-through scene, lit hover, part mode glass
    void restore_highlight();
    void render_objects_layer();
    void render_parts_layer();

    // what the highlight shows: the panel row under the mouse wins over the scene
    int  hl_obj() const { return m_panel_hover_obj >= 0 ? m_panel_hover_obj : m_hover_obj; }
    int  hl_vol() const { return m_panel_hover_obj >= 0 ? m_panel_hover_vol : m_hover_vol; }

    int  m_edit_obj        = -1; // object whose parts are being numbered, -1 = objects level
    int  m_hover_obj       = -1; // under the mouse in the scene
    int  m_hover_vol       = -1;
    int  m_panel_hover_obj = -1; // under the mouse in the panel list
    int  m_panel_hover_vol = -1;
    // Lock: clicks in the scene stop numbering and stop leaving the parts level, so a stray click
    // on a crowded plate costs nothing. The panel keeps working; the camera keeps working.
    bool m_locked          = false;
    std::map<std::tuple<int, int, int>, std::pair<ColorRGBA, bool>> m_saved_colors;
};

} // namespace GUI
} // namespace Slic3r

#endif
// NEOTKO_PRINTORDER_TAG_END
