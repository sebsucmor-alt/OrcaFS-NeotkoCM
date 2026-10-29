#include "GLGizmoPainterBase.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"

#include <GL/glew.h>

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/OpenGLManager.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ClipperUtils.hpp" // NEOTKO_PAINTERPRO_TAG — shape mask fill
#include "libslic3r/NSVGUtils.hpp"    // NEOTKO_PAINTERPRO_TAG — SVG shape load
#include "libslic3r/NeoDebug.hpp"     // NEOTKO_SVGDECAL_TAG — F0 Apply timing
#include "libslic3r/ColorStitch.hpp"  // NEOTKO_SVGDECAL_TAG — F2 SVG zones (Sticker engine)
#include <fstream>
#include <boost/filesystem/path.hpp>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstring>  // NEOTKO_SVGDECAL_TAG — std::strlen (drawn-shape SVG)
#include <locale>   // NEOTKO_SVGDECAL_TAG — classic locale to read the drawn-shape path
#include "libslic3r/Tesselate.hpp"

#include <memory>
#include <optional>
#include <cstdio>   // NEOTKO_COLORSTITCH_TAG — std::snprintf for weave uniform names
#include <map>      // NEOTKO_COLORSTITCH_TAG — (state, weave_idx) → patch batching
#include <utility>
#include <unordered_map> // NEOTKO_PAINTERPRO_TAG — mask occlusion cache

namespace Slic3r::GUI {

std::shared_ptr<GLModel> GLGizmoPainterBase::s_sphere = nullptr;

GLGizmoPainterBase::GLGizmoPainterBase(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
    m_vertical_only = false;
    m_horizontal_only = false;
}

GLGizmoPainterBase::~GLGizmoPainterBase()
{
    if (s_sphere != nullptr)
        s_sphere.reset();
}

void GLGizmoPainterBase::data_changed(bool is_serializing)
{
    if (m_state != On)
        return;

    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    const Selection &  selection = m_parent.get_selection();
    if (mo && selection.is_from_single_instance()
     && (m_schedule_update || mo->id() != m_old_mo_id || mo->volumes.size() != m_old_volumes_size))
    {
        //BBS: add logic to distinguish the first_time_update and later_update
        update_from_model_object(!m_schedule_update);

        m_old_mo_id = mo->id();
        m_old_volumes_size = mo->volumes.size();
        m_schedule_update = false;
    }
}

GLGizmoPainterBase::ClippingPlaneDataWrapper GLGizmoPainterBase::get_clipping_plane_data() const
{
    ClippingPlaneDataWrapper clp_data_out{{0.f, 0.f, 1.f, FLT_MAX}, {-FLT_MAX, FLT_MAX}};
    // Take care of the clipping plane. The normal of the clipping plane is
    // saved with opposite sign than we need to pass to OpenGL (FIXME)
    if (bool clipping_plane_active = m_c->object_clipper()->get_position() != 0.; clipping_plane_active) {
        const ClippingPlane *clp = m_c->object_clipper()->get_clipping_plane();
        for (size_t i = 0; i < 3; ++i)
            clp_data_out.clp_dataf[i] = -1.f * float(clp->get_data()[i]);
        clp_data_out.clp_dataf[3] = float(clp->get_data()[3]);
    }

    // z_range is calculated in the same way as in GLCanvas3D::_render_objects(GLVolumeCollection::ERenderType type)
    if (m_c->get_canvas()->get_use_clipping_planes()) {
        const std::array<ClippingPlane, 2> &clps = m_c->get_canvas()->get_clipping_planes();
        clp_data_out.z_range                     = {float(-clps[0].get_data()[3]), float(clps[1].get_data()[3])};
    }

    return clp_data_out;
}

void GLGizmoPainterBase::render_triangles(const Selection& selection) const
{
    auto* shader = wxGetApp().get_shader("mm_gouraud");
    if (!shader)
        return;
    shader->start_using();
    ScopeGuard guard([shader]() { if (shader) shader->stop_using(); });

    ClippingPlaneDataWrapper clp_data = this->get_clipping_plane_data();
    shader->set_uniform("clipping_plane", clp_data.clp_dataf);
    shader->set_uniform("z_range", clp_data.z_range);

    // BBS: to improve the random white pixel issue
    glsafe(::glDisable(GL_CULL_FACE));

    const ModelObject* mo = m_c->selection_info()->model_object();
    int                mesh_id = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        ++mesh_id;

        Transform3d trafo_matrix;
        if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
            trafo_matrix = mo->instances[selection.get_instance_idx()]->get_assemble_transformation().get_matrix() * mv->get_matrix();
            trafo_matrix.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) + mo->instances[selection.get_instance_idx()]->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
        }
        else {
            trafo_matrix = mo->instances[selection.get_instance_idx()]->get_transformation().get_matrix()* mv->get_matrix();
        }

        bool is_left_handed = trafo_matrix.matrix().determinant() < 0.;
        if (is_left_handed)
            glsafe(::glFrontFace(GL_CW));

        const Camera& camera = wxGetApp().plater()->get_camera();
        const Transform3d& view_matrix = camera.get_view_matrix();
        shader->set_uniform("view_model_matrix", view_matrix * trafo_matrix);
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);

        float normal_z = -::cos(Geometry::deg2rad(m_highlight_by_angle_threshold_deg));
        Matrix3f normal_matrix = static_cast<Matrix3f>(trafo_matrix.matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>());

        shader->set_uniform("volume_world_matrix", trafo_matrix);
        shader->set_uniform("volume_mirrored", is_left_handed);
        shader->set_uniform("slope.actived", m_parent.is_using_slope());
        shader->set_uniform("slope.volume_world_normal_matrix", normal_matrix);
        shader->set_uniform("slope.normal_z", normal_z);
        m_triangle_selectors[mesh_id]->render(m_imgui, trafo_matrix);

        if (is_left_handed)
            glsafe(::glFrontFace(GL_CCW));
    }
}

void GLGizmoPainterBase::render_cursor()
{
    // NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F4: RECTANGLE/POLYGON are pure screen-space
    // overlays, independent of whatever the raycast below finds under the mouse (the user can
    // drag/click past the object's silhouette while masking), so handle them before it.
    // NEOTKO_SVGDECAL_TAG — the decal cards as a reference while painting by hand.
    if (m_tool_type != ToolType::POLYGON && show_decal_cards_while_brushing())
        render_decal_cards(nullptr);
    if (m_tool_type == ToolType::RECTANGLE) {
        render_cursor_rectangle();
        return;
    }
    if (m_tool_type == ToolType::POLYGON) {
        render_cursor_polygon();
        return;
    }

    // First check that the mouse pointer is on an object.
    const ModelObject* mo = m_c->selection_info()->model_object();
    const Selection& selection = m_parent.get_selection();
    const ModelInstance* mi = mo->instances[selection.get_instance_idx()];
    const Camera& camera = wxGetApp().plater()->get_camera();

    // Precalculate transformations of individual meshes.
    std::vector<Transform3d> trafo_matrices;
    for (const ModelVolume* mv : mo->volumes) {
        if (mv->is_model_part())
        {
            if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
                Transform3d temp = mi->get_assemble_transformation().get_matrix() * mv->get_matrix();
                temp.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) + mi->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
                trafo_matrices.emplace_back(temp);
            }
            else {
                trafo_matrices.emplace_back(mi->get_transformation().get_matrix() * mv->get_matrix());
            }
        }
    }
    // Raycast and return if there's no hit.
    update_raycast_cache(m_parent.get_local_mouse_position(), camera, trafo_matrices);
    // NEOTKO_SVGDECAL_TAG — F5b: the mirror centre always, the ghost cursors over the object.
    if (mo->paint_symmetry.brush != 0 && m_tool_type == ToolType::BRUSH)
        render_brush_symmetry(trafo_matrices, m_rr.mesh_id != -1
            && (m_cursor_type == TriangleSelector::SPHERE || m_cursor_type == TriangleSelector::CIRCLE));
    if (m_rr.mesh_id == -1)
        return;

    if (m_tool_type == ToolType::BRUSH) {
        if (m_cursor_type == TriangleSelector::SPHERE)
            render_cursor_sphere(trafo_matrices[m_rr.mesh_id]);
        else if (m_cursor_type == TriangleSelector::CIRCLE)
            render_cursor_circle();
        else if (m_cursor_type == TriangleSelector::HEIGHT_RANGE)
            render_cursor_height_range(trafo_matrices[m_rr.mesh_id]);
    }
}

void GLGizmoPainterBase::render_cursor_circle()
{
    const Size cnv_size = m_parent.get_canvas_size();
    const float cnv_width  = float(cnv_size.get_width());
    const float cnv_height = float(cnv_size.get_height());
    if (cnv_width == 0.0f || cnv_height == 0.0f)
        return;

    const float cnv_inv_width  = 1.0f / cnv_width;
    const float cnv_inv_height = 1.0f / cnv_height;

    const Vec2d center = m_parent.get_local_mouse_position();
    const float radius = m_cursor_radius * float(wxGetApp().plater()->get_camera().get_zoom());

    glsafe(::glLineWidth(1.5f));
    glsafe(::glDisable(GL_DEPTH_TEST));

    glsafe(::glPushAttrib(GL_ENABLE_BIT));
    glsafe(::glLineStipple(4, 0xAAAA));
    glsafe(::glEnable(GL_LINE_STIPPLE));

    if (!m_circle.is_initialized() || !m_old_center.isApprox(center) || std::abs(m_old_cursor_radius - radius) > EPSILON) {
        m_old_cursor_radius = radius;
        m_old_center = center;
        m_circle.reset();

        GLModel::Geometry init_data;
        static const unsigned int StepsCount = 32;
        static const float StepSize = 2.0f * float(PI) / float(StepsCount);
        init_data.format = { GLModel::Geometry::EPrimitiveType::LineLoop, GLModel::Geometry::EVertexLayout::P2 };
        init_data.color  = { 0.0f, 1.0f, 0.3f, 1.0f };
        init_data.reserve_vertices(StepsCount);
        init_data.reserve_indices(StepsCount);

        // vertices + indices
        for (unsigned int i = 0; i < StepsCount; ++i) {
            const float angle = float(i * StepSize);
            init_data.add_vertex(Vec2f(2.0f * ((center.x() + ::cos(angle) * radius) * cnv_inv_width - 0.5f),
                                       -2.0f * ((center.y() + ::sin(angle) * radius) * cnv_inv_height - 0.5f)));
            init_data.add_index(i);
        }

        m_circle.init_from(std::move(init_data));
    }

    // BBS
    ColorRGBA render_color = this->get_cursor_hover_color();
    if (m_button_down == Button::Left)
        render_color = this->get_cursor_sphere_left_button_color();
    else if (m_button_down == Button::Right)
        render_color = this->get_cursor_sphere_right_button_color();

    m_circle.set_color(render_color);

    GLShaderProgram* shader = GUI::wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        shader->set_uniform("view_model_matrix", Transform3d::Identity());
        shader->set_uniform("projection_matrix", Transform3d::Identity());
        m_circle.render();
        shader->stop_using();
    }

    glsafe(::glPopAttrib());
    glsafe(::glEnable(GL_DEPTH_TEST));
}


void GLGizmoPainterBase::render_cursor_sphere(const Transform3d& trafo) const
{
    if (s_sphere == nullptr) {
        s_sphere = std::make_shared<GLModel>();
        s_sphere->init_from(its_make_sphere(1.0, double(PI) / 12.0));
    }

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Transform3d complete_scaling_matrix_inverse = Geometry::Transformation(trafo).get_scaling_factor_matrix().inverse();

    // BBS
    ColorRGBA render_color = this->get_cursor_hover_color();
    if (m_button_down == Button::Left)
        render_color = this->get_cursor_sphere_left_button_color();
    else if (m_button_down == Button::Right)
        render_color = this->get_cursor_sphere_right_button_color();

    shader->start_using();

    const Camera& camera = wxGetApp().plater()->get_camera();
    Transform3d view_model_matrix = camera.get_view_matrix() * trafo *
        Geometry::assemble_transform(m_rr.hit.cast<double>()) * complete_scaling_matrix_inverse *
        Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), m_cursor_radius * Vec3d::Ones());

    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    const bool is_left_handed = Geometry::Transformation(view_model_matrix).is_left_handed();
    if (is_left_handed)
        glsafe(::glFrontFace(GL_CW));

    assert(s_sphere != nullptr);
    s_sphere->set_color(render_color);
    s_sphere->render();

    if (is_left_handed)
        glsafe(::glFrontFace(GL_CCW));

    shader->stop_using();
}

// BBS
void GLGizmoPainterBase::render_cursor_height_range(const Transform3d& trafo) const
{
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    shader->start_using();

    const BoundingBoxf3 box = bounding_box();
    Vec3d hit_world = trafo * Vec3d(m_rr.hit(0), m_rr.hit(1), m_rr.hit(2));
    float max_z = (float)box.max.z();
    float min_z = (float)box.min.z();

    float cursor_z = std::clamp((float)hit_world.z(), min_z, max_z);
    std::array<float, 2> zs = { cursor_z, std::clamp(cursor_z + m_cursor_height, min_z, max_z) };

    const Selection& selection = m_parent.get_selection();
    const ModelObject* model_object = wxGetApp().model().objects[selection.get_object_idx()];
    const ModelInstance* mi = model_object->instances[selection.get_instance_idx()];

    int volumes_count = model_object->volumes.size();
    if (m_cut_contours.size() != volumes_count * 2) {
        m_cut_contours.resize(volumes_count * 2);
    }
    m_volumes_index = 0;
    for (const ModelVolume* mv : model_object->volumes) {
        TriangleMesh vol_mesh = mv->mesh();
        if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
            Transform3d temp = mi->get_assemble_transformation().get_matrix() * mv->get_matrix();
            temp.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) + mi->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
            vol_mesh.transform(temp);
        }
        else {
            vol_mesh.transform(mi->get_transformation().get_matrix() * mv->get_matrix());
        }

        for (int i = 0; i < zs.size(); i++) {
            update_contours(m_volumes_index, vol_mesh, zs[i], max_z, min_z);

            const Camera& camera = wxGetApp().plater()->get_camera();
            Transform3d view_model_matrix = camera.get_view_matrix()  * Geometry::assemble_transform(m_cut_contours[m_volumes_index].shift);

            shader->set_uniform("view_model_matrix", view_model_matrix);
            shader->set_uniform("projection_matrix", camera.get_projection_matrix());
            glsafe(::glLineWidth(2.0f));
            m_cut_contours[m_volumes_index].contours.render();
            m_volumes_index++;
        }

    }

    shader->stop_using();
}

// NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F4, Sesion A (deuda pendiente de esta sesión: no
// se había implementado el overlay). Screen-space quad outline while dragging, same NDC-from-
// pixels/"flat" shader pattern as render_cursor_circle() above and GLSelectionRectangle.
void GLGizmoPainterBase::render_cursor_rectangle()
{
    if (!m_rect_dragging)
        return;

    const Size cnv_size = m_parent.get_canvas_size();
    const float cnv_width  = float(cnv_size.get_width());
    const float cnv_height = float(cnv_size.get_height());
    if (cnv_width == 0.0f || cnv_height == 0.0f)
        return;
    const float cnv_inv_width  = 1.0f / cnv_width;
    const float cnv_inv_height = 1.0f / cnv_height;

    glsafe(::glLineWidth(1.5f));
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glPushAttrib(GL_ENABLE_BIT));
    glsafe(::glLineStipple(4, 0xAAAA));
    glsafe(::glEnable(GL_LINE_STIPPLE));

    if (!m_rect_overlay.is_initialized() || !m_old_rect_start_corner.isApprox(m_rect_start_corner)
     || !m_old_rect_end_corner.isApprox(m_rect_end_corner)) {
        m_old_rect_start_corner = m_rect_start_corner;
        m_old_rect_end_corner   = m_rect_end_corner;
        m_rect_overlay.reset();

        const float left   = float(std::min(m_rect_start_corner.x(), m_rect_end_corner.x()));
        const float right  = float(std::max(m_rect_start_corner.x(), m_rect_end_corner.x()));
        const float top    = float(std::min(m_rect_start_corner.y(), m_rect_end_corner.y()));
        const float bottom = float(std::max(m_rect_start_corner.y(), m_rect_end_corner.y()));
        auto to_ndc = [cnv_inv_width, cnv_inv_height](float x, float y) {
            return Vec2f(2.0f * (x * cnv_inv_width - 0.5f), -2.0f * (y * cnv_inv_height - 0.5f));
        };

        GLModel::Geometry init_data;
        init_data.format = { GLModel::Geometry::EPrimitiveType::LineLoop, GLModel::Geometry::EVertexLayout::P2 };
        init_data.color  = { 0.0f, 1.0f, 0.3f, 1.0f };
        init_data.reserve_vertices(4);
        init_data.reserve_indices(4);
        init_data.add_vertex(to_ndc(left, top));
        init_data.add_vertex(to_ndc(right, top));
        init_data.add_vertex(to_ndc(right, bottom));
        init_data.add_vertex(to_ndc(left, bottom));
        for (unsigned int i = 0; i < 4; ++i)
            init_data.add_index(i);

        m_rect_overlay.init_from(std::move(init_data));
    }

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();
        shader->set_uniform("view_model_matrix", Transform3d::Identity());
        shader->set_uniform("projection_matrix", Transform3d::Identity());
        m_rect_overlay.render();
        shader->stop_using();
    }

    glsafe(::glPopAttrib());
    glsafe(::glEnable(GL_DEPTH_TEST));
}

// NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F5 (commit 4). Draws the shape mask: the flattened
// outline (exactly the polygon that Apply will paint), a rubber-band segment to the mouse while
// the shape is open, node squares and handle lines, and a translucent fill in the selected
// filament colour once it's closed. Also drops an unfinished shape if the camera moved under it
// (nodes are screen-space, they'd no longer match the object). Rebuilt every frame (see
// m_polygon_overlay's declaration for why that's fine here).
// NEOTKO_SVGDECAL_TAG — F3/F5: the decal cards (and their mirrored copies, and the copies of the SVG
// being placed, on_screen). Floating on a sphere around the object, or laid on the surface as they
// will paint (m_decal_on_surface). Draws with its own GL state; the caller's shader is left stopped.
void GLGizmoPainterBase::render_decal_cards(const PaintDecal *on_screen)
{
    const ModelObject *decal_mo = m_c->selection_info()->model_object();
    if (decal_mo == nullptr)
        return;
    const Size  cnv_size   = m_parent.get_canvas_size();
    const float cnv_width  = float(cnv_size.get_width());
    const float cnv_height = float(cnv_size.get_height());
    if (cnv_width == 0.0f || cnv_height == 0.0f)
        return;
    auto to_ndc = [cnv_width, cnv_height](const Vec2d &p) {
        return Vec2f(2.0f * (float(p.x()) / cnv_width - 0.5f), -2.0f * (float(p.y()) / cnv_height - 0.5f));
    };
    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;
    glsafe(::glPushAttrib(GL_ENABLE_BIT));
    glsafe(::glDisable(GL_DEPTH_TEST));
    shader->start_using();
    shader->set_uniform("view_model_matrix", Transform3d::Identity());
    shader->set_uniform("projection_matrix", Transform3d::Identity());
    render_decal_cards_body(decal_mo, on_screen, to_ndc, cnv_width, cnv_height);
    shader->stop_using();
    glsafe(::glPopAttrib());
}

void GLGizmoPainterBase::decal_turn_scale(size_t idx, const PaintDecal &start, double degrees, double factor)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || idx >= mo->paint_decals.size() || factor <= 0.)
        return;
    // Centre of the SVG's box, placed on the card (warp included): the point that stays put.
    Vec2d lo = Vec2d::Constant(std::numeric_limits<double>::max()), hi = Vec2d::Constant(std::numeric_limits<double>::lowest());
    for (const SvgRing &r : decal_rings(idx))
        for (const Vec2d &p : r.pts) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
    if (lo.x() > hi.x())
        return;
    const Vec2d c_svg = 0.5 * (lo + hi);
    const Vec3d q     = start.warp_matrix() * Vec3d(c_svg.x(), c_svg.y(), 1.);
    Matrix2d A;
    A << start.lin[0], start.lin[1], start.lin[2], start.lin[3];
    const Vec2d off(start.off[0], start.off[1]);
    const Vec2d uv_c = A * Vec2d(q.x() / q.z(), q.y() / q.z()) + off;
    const double rad = degrees * M_PI / 180.;
    Matrix2d     T;
    T << std::cos(rad), -std::sin(rad), std::sin(rad), std::cos(rad);
    T *= factor;
    const Matrix2d A2   = T * A;
    const Vec2d    off2 = uv_c + T * (off - uv_c);
    PaintDecal d = start;
    d.lin = { A2(0, 0), A2(0, 1), A2(1, 0), A2(1, 1) };
    d.off = { off2.x(), off2.y() };
    mo->paint_decals[idx] = d;
}

bool GLGizmoPainterBase::mouse_on_object(const Vec2d &mouse, Vec3d &obj_pt)
{
    if (m_c->selection_info()->model_object() == nullptr)
        return false;
    std::vector<Transform3d> trafos, trafos_no_translate;
    const Transform3d inst = model_part_trafos(trafos, trafos_no_translate);
    update_raycast_cache(mouse, wxGetApp().plater()->get_camera(), trafos);
    if (m_rr.mesh_id < 0 || size_t(m_rr.mesh_id) >= trafos.size())
        return false;
    obj_pt = inst.inverse() * (trafos[size_t(m_rr.mesh_id)] * m_rr.hit.cast<double>());
    return true;
}

// NEOTKO_SVGDECAL_TAG — F5b brush symmetry. Every combination of the planes in `bits`, as the
// diagonal of the reflection (one plane: one copy; two: three; three: seven), like the decals.
static std::vector<Vec3d> symmetry_reflections(int bits)
{
    std::vector<Vec3d> out;
    for (int combo = 1; combo < 8; ++combo)
        if ((combo & ~bits) == 0)
            out.emplace_back((combo & 1) ? -1. : 1., (combo & 2) ? -1. : 1., (combo & 4) ? -1. : 1.);
    return out;
}

bool GLGizmoPainterBase::symmetry_surface_point(const Vec3d &obj_pt, const std::vector<Transform3d> &trafos, const Transform3d &inst,
                                                double max_dist, int &mesh, int &facet, Vec3f &hit_mesh, const Vec3d *eye_obj) const
{
    const std::vector<const MeshRaycaster *> raycasters = m_c->raycaster()->raycasters();
    const Vec3d world = inst * obj_pt;
    double best = max_dist * max_dist;
    mesh = -1;
    if (eye_obj != nullptr) {
        // The mirrored ray, from the mirrored camera through the mirrored point: first hit wins.
        const Vec3d eye_w = inst * *eye_obj;
        const Vec3d dir_w = (world - eye_w).normalized();
        double best_t = std::numeric_limits<double>::max();
        for (size_t i = 0; i < trafos.size() && i < raycasters.size(); ++i) {
            const Transform3d inv = trafos[i].inverse();
            const AABBMesh::hit_result h = raycasters[i]->get_aabb_mesh().query_ray_hit(inv * eye_w, Vec3d(inv.linear() * dir_w).normalized());
            if (!h.is_hit())
                continue;
            const Vec3d hw = trafos[i] * h.position();
            const double t = (hw - eye_w).dot(dir_w);
            if (t < best_t && (hw - world).squaredNorm() <= best) {
                best_t   = t;
                mesh     = int(i);
                facet    = h.face();
                hit_mesh = h.position().cast<float>();
            }
        }
        if (mesh >= 0)
            return true;
    }
    for (size_t i = 0; i < trafos.size() && i < raycasters.size(); ++i) {
        const Vec3d q = trafos[i].inverse() * world;
        int    f = -1;
        Vec3d  c;
        raycasters[i]->get_aabb_mesh().squared_distance(q, f, c);
        if (f < 0)
            continue;
        const double d2 = (trafos[i] * c - world).squaredNorm();   // in mm, whatever the part's scale
        if (d2 <= best) {
            best     = d2;
            mesh     = int(i);
            facet    = f;
            hit_mesh = c.cast<float>();
        }
    }
    return mesh >= 0;
}

void GLGizmoPainterBase::brush_paint_mirrored(const std::vector<ProjectedMousePosition> &positions, const Transform3d &src_trafo,
                                              EnforcerBlockerType new_state, const std::vector<Transform3d> &trafos,
                                              const std::vector<Transform3d> &trafos_not_translate)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || mo->paint_symmetry.brush == 0 || positions.empty())
        return;
    std::vector<Transform3d> part_trafos, part_trafos_nt;
    const Transform3d inst     = model_part_trafos(part_trafos, part_trafos_nt);
    const Transform3d inst_inv = inst.inverse();
    const Vec3d       centre   = decal_mirror_centre();
    const Vec3d       cam_obj  = inst_inv * wxGetApp().plater()->get_camera().get_position();
    // A mirrored point farther than this from the surface: the object is not symmetric there.
    const double      max_dist = std::max(2.0 * double(m_cursor_radius), 1.0);
    struct Hit { int mesh = -1; int facet = -1; Vec3f p = Vec3f::Zero(); };
    for (const Vec3d &s : symmetry_reflections(mo->paint_symmetry.brush)) {
        auto reflect = [&](const Vec3d &p) { return Vec3d(centre + s.cwiseProduct(p - centre)); };
        const Vec3d cam_mirror = reflect(cam_obj);
        std::vector<Hit> hits(positions.size());
        for (size_t k = 0; k < positions.size(); ++k) {
            const Vec3d obj = inst_inv * (src_trafo * positions[k].mesh_hit.cast<double>());
            symmetry_surface_point(reflect(obj), trafos, inst, max_dist, hits[k].mesh, hits[k].facet, hits[k].p, &cam_mirror);
        }
        const Vec3d cam_world = inst * cam_mirror;
        std::vector<bool> touched(trafos.size(), false);
        auto paint = [&](const Hit &a, const Hit *b) {
            const Transform3d &trafo = trafos[size_t(a.mesh)];
            const Vec3f cam = (trafo.inverse() * cam_world).cast<float>();
            const TriangleSelector::ClippingPlane &clp = this->get_clipping_plane_in_volume_coordinates(trafo);
            std::unique_ptr<TriangleSelector::Cursor> cursor = b == nullptr
                ? TriangleSelector::SinglePointCursor::cursor_factory(a.p, cam, m_cursor_radius, m_cursor_type, trafo, clp)
                : TriangleSelector::DoublePointCursor::cursor_factory(a.p, b->p, cam, m_cursor_radius, m_cursor_type, trafo, clp);
            m_triangle_selectors[size_t(a.mesh)]->set_precision_factor(m_precision_factor);
            m_triangle_selectors[size_t(a.mesh)]->select_patch(a.facet, std::move(cursor), new_state, trafos_not_translate[size_t(a.mesh)],
                m_triangle_splitting_enabled, m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);
            touched[size_t(a.mesh)] = true;
        };
        if (hits.size() == 1) {
            if (hits.front().mesh >= 0 && size_t(hits.front().mesh) < m_triangle_selectors.size())
                paint(hits.front(), nullptr);
        } else {
            for (size_t k = 0; k + 1 < hits.size(); ++k) {
                const Hit &a = hits[k], &b = hits[k + 1];
                if (a.mesh < 0 || size_t(a.mesh) >= m_triangle_selectors.size())
                    continue;
                if (b.mesh == a.mesh)
                    paint(a, &b);
                else
                    paint(a, nullptr);   // the copy jumps to another part (or off the object) here
            }
        }
        for (size_t i = 0; i < touched.size(); ++i)
            if (touched[i] && i < m_triangle_selectors.size())
                m_triangle_selectors[i]->request_update_render_data(true);
    }
}

void GLGizmoPainterBase::render_brush_symmetry(const std::vector<Transform3d> &trafos, bool hovering)
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    const Size         cnv = m_parent.get_canvas_size();
    const double       cw = cnv.get_width(), ch = cnv.get_height();
    GLShaderProgram   *shader = wxGetApp().get_shader("flat");
    if (mo == nullptr || shader == nullptr || cw <= 0. || ch <= 0.)
        return;
    std::vector<Transform3d> part_trafos, part_trafos_nt;
    const Transform3d inst   = model_part_trafos(part_trafos, part_trafos_nt);
    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Matrix4d    vpm    = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    auto to_px = [&](const Vec3d &w, Vec2d &px) {
        const Vec4d clip = vpm * Vec4d(w.x(), w.y(), w.z(), 1.0);
        if (clip.w() <= 0.0)
            return false;
        px = Vec2d((clip.x() / clip.w() * 0.5 + 0.5) * cw, (0.5 - clip.y() / clip.w() * 0.5) * ch);
        return true;
    };
    GLModel::Geometry ghost;
    ghost.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
    ghost.color  = { 1.0f, 0.55f, 0.1f, 0.9f };
    unsigned int idx = 0;
    auto quad = [&](const Vec2d &a, const Vec2d &b, double hw) {
        Vec2d d = b - a;
        const double len = d.norm();
        if (len < 1e-9)
            return;
        d /= len;
        const Vec2d n(-d.y() * hw, d.x() * hw);
        for (const Vec2d &p : { Vec2d(a + n), Vec2d(a - n), Vec2d(b + n), Vec2d(a - n), Vec2d(b - n), Vec2d(b + n) }) {
            ghost.add_vertex(Vec2f(float(2. * (p.x() / cw - 0.5)), float(-2. * (p.y() / ch - 0.5))));
            ghost.add_index(idx++);
        }
    };
    if (hovering && m_rr.mesh_id >= 0 && size_t(m_rr.mesh_id) < trafos.size()) {
        const Transform3d inst_inv = inst.inverse();
        const Vec3d       centre   = decal_mirror_centre();
        const Vec3d       obj      = inst_inv * (trafos[size_t(m_rr.mesh_id)] * m_rr.hit.cast<double>());
        const Vec3d       right    = camera.get_dir_right();
        const Vec3d       cam_obj  = inst_inv * camera.get_position();
        const double      max_dist = std::max(2.0 * double(m_cursor_radius), 1.0);
        for (const Vec3d &s : symmetry_reflections(mo->paint_symmetry.brush)) {
            int   mesh = -1, facet = -1;
            Vec3f hit;
            const Vec3d cam_mirror = centre + s.cwiseProduct(cam_obj - centre);
            if (!symmetry_surface_point(Vec3d(centre + s.cwiseProduct(obj - centre)), trafos, inst, max_dist, mesh, facet, hit, &cam_mirror))
                continue;
            const Vec3d w = trafos[size_t(mesh)] * hit.cast<double>();
            Vec2d c, e;
            if (!to_px(w, c) || !to_px(Vec3d(w + right * double(m_cursor_radius)), e))
                continue;
            const double r = std::max((e - c).norm(), 3.0);
            // A dashed ring: every other piece of 24.
            for (int k = 0; k < 24; k += 2) {
                const double a0 = 2. * M_PI * k / 24., a1 = 2. * M_PI * (k + 1) / 24.;
                quad(c + r * Vec2d(std::cos(a0), std::sin(a0)), c + r * Vec2d(std::cos(a1), std::sin(a1)), 1.0);
            }
        }
    }
    shader->start_using();
    shader->set_uniform("view_model_matrix", Transform3d::Identity());
    shader->set_uniform("projection_matrix", Transform3d::Identity());
    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    m_brush_ghost_model.reset();
    if (idx > 0) {
        m_brush_ghost_model.init_from(std::move(ghost));
        m_brush_ghost_model.render();
    }
    render_mirror_marker();
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));
    shader->stop_using();
}

// F5: where the mirror planes cross, an orange ring and cross, while any mirror is on.
void GLGizmoPainterBase::render_mirror_marker()
{
    const ModelObject *decal_mo = m_c->selection_info()->model_object();
    if (decal_mo == nullptr)
        return;
    bool any_mirror = (m_svg_mirror != 0 && !m_svg_local_rings.empty()) || decal_mo->paint_symmetry.brush != 0;
    for (const PaintDecal &d : decal_mo->paint_decals)
        any_mirror = any_mirror || (!d.hidden && d.mirror != 0);
    m_mirror_centre_model.reset();
    if (!any_mirror)
        return;
    std::vector<Transform3d> trafos, trafos_no_translate;
    const Transform3d inst   = model_part_trafos(trafos, trafos_no_translate);
    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Matrix4d    vpm    = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const Size        cnv    = m_parent.get_canvas_size();
    const double      cw = cnv.get_width(), ch = cnv.get_height();
    const Vec3d       w    = inst * decal_mirror_centre();
    const Vec4d       clip = vpm * Vec4d(w.x(), w.y(), w.z(), 1.0);
    if (clip.w() <= 0.0 || cw <= 0. || ch <= 0.)
        return;
    const Vec2d c((clip.x() / clip.w() * 0.5 + 0.5) * cw, (0.5 - clip.y() / clip.w() * 0.5) * ch);
    GLModel::Geometry mark;
    mark.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
    mark.color  = { 1.0f, 0.55f, 0.1f, 1.0f };
    unsigned int idx = 0;
    auto quad = [&](const Vec2d &a, const Vec2d &b, double hw) {
        Vec2d d = b - a;
        const double len = d.norm();
        if (len < 1e-9)
            return;
        d /= len;
        const Vec2d n(-d.y() * hw, d.x() * hw);
        for (const Vec2d &p : { Vec2d(a + n), Vec2d(a - n), Vec2d(b + n), Vec2d(a - n), Vec2d(b - n), Vec2d(b + n) }) {
            mark.add_vertex(Vec2f(float(2. * (p.x() / cw - 0.5)), float(-2. * (p.y() / ch - 0.5))));
            mark.add_index(idx++);
        }
    };
    const double arm = 10.0, rad = 6.0;
    quad(c - Vec2d(arm, 0.), c + Vec2d(arm, 0.), 1.0);
    quad(c - Vec2d(0., arm), c + Vec2d(0., arm), 1.0);
    for (int k = 0; k < 16; ++k) {
        const double a0 = 2. * M_PI * k / 16., a1 = 2. * M_PI * (k + 1) / 16.;
        quad(c + rad * Vec2d(std::cos(a0), std::sin(a0)), c + rad * Vec2d(std::cos(a1), std::sin(a1)), 1.0);
    }
    m_mirror_centre_model.init_from(std::move(mark));
    m_mirror_centre_model.render();
}

void GLGizmoPainterBase::render_decal_cards_body(const ModelObject *decal_mo, const PaintDecal *on_screen,
    const std::function<Vec2f(const Vec2d &)> &to_ndc, float cnv_width, float cnv_height)
{
    std::vector<Transform3d> trafos, trafos_no_translate;
    const Transform3d inst   = model_part_trafos(trafos, trafos_no_translate);
    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Matrix4d    vpm    = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const Vec3d       mirror_centre = decal_mirror_centre();   // F5: mirrored copies drawn like the decal
    // Cards are drawn on a sphere around the object (its box's bounding sphere), each one
    // slid out along its own direction, so a card and its mirrored copies sit on the side
    // they paint. Only the drawing moves: sliding along the rays (towards the eye in
    // perspective, along dir in parallel) changes nothing of what is painted, and from the
    // view a card was pinned from it looks exactly the same.
    const BoundingBoxf3 obj_box       = decal_mo->raw_mesh_bounding_box();
    const Vec3d         sphere_centre = obj_box.center();
    const double        sphere_radius = 0.5 * obj_box.size().norm();
    auto on_sphere = [&](const PaintDecal &d, const Vec3d &p) {
        const Vec3d  c(d.centre[0], d.centre[1], d.centre[2]);
        const Vec3d  dn = Vec3d(d.dir[0], d.dir[1], d.dir[2]).normalized();
        const double t  = dn.dot(sphere_centre - c) + sphere_radius;
        if (d.perspective && d.eye_dist > 0.) {
            const Vec3d  E = c + dn * d.eye_dist;
            const double k = (d.eye_dist - t) / d.eye_dist;
            return k > 0.05 ? Vec3d(E + (p - E) * k) : p;   // eye inside the sphere: left where it is
        }
        return Vec3d(p + dn * t);
    };
    auto to_px = [&](const PaintDecal &d, const Vec2d &svg_pt, Vec2d &px) {
        const Vec3d w    = inst * on_sphere(d, d.svg_to_object(svg_pt));
        const Vec4d clip = vpm * Vec4d(w.x(), w.y(), w.z(), 1.0);
        if (clip.w() <= 0.0)
            return false;
        px = Vec2d((clip.x() / clip.w() * 0.5 + 0.5) * cnv_width, (0.5 - clip.y() / clip.w() * 0.5) * cnv_height);
        return true;
    };
    // Every card to draw: each visible decal with its mirrored copies, and the copies of the
    // SVG on screen (the SVG itself is drawn on screen further down).
    struct CardItem { PaintDecal decal; const std::vector<SvgRing> *rings; const std::vector<std::vector<Vec2d>> *tris; const std::vector<PaintDecalZone> *groups; bool back = false; bool live = false; };
    std::vector<CardItem> cards;
    // The cache is filled first: it may grow, and pointers into it are taken only after.
    for (size_t i = 0; i < decal_mo->paint_decals.size(); ++i)
        if (!decal_mo->paint_decals[i].hidden && int(i) != editing_decal())
            decal_rings(i);   // builds the cache, triangles included
    for (size_t i = 0; i < decal_mo->paint_decals.size(); ++i) {
        const PaintDecal &d = decal_mo->paint_decals[i];
        if (d.hidden || int(i) == editing_decal())
            continue;
        for (PaintDecal &dc : d.with_mirror_copies(mirror_centre))
            cards.push_back({ std::move(dc), &m_decal_rings_cache[i].rings, &m_decal_rings_cache[i].tris, &m_decal_rings_cache[i].groups, false, int(i) == m_decal_drag });
    }
    if ((on_screen != nullptr)) {
        std::vector<PaintDecal> copies = on_screen->with_mirror_copies(mirror_centre);
        for (size_t k = 1; k < copies.size(); ++k)
            cards.push_back({ std::move(copies[k]), &m_svg_local_rings, &m_svg_local_tris, &m_svg_groups });
    }
    // A card whose painting side faces away from the camera is on the far side: drawn fainter.
    const Vec3d to_viewer = -camera.get_dir_forward();
    for (CardItem &card : cards)
        card.back = Vec3d(inst.linear() * Vec3d(card.decal.dir[0], card.decal.dir[1], card.decal.dir[2])).dot(to_viewer) < 0.;

    // On surface: every card as it will paint, its outline and colours laid on the mesh by the same
    // rays Apply uses, hidden where the object covers them (depth test against the drawn model).
    if (m_decal_on_surface) {
        const Vec3d cam_pos   = camera.get_position();
        const bool  cam_persp = camera.get_type() == Camera::EType::Perspective;
        // A surface point in NDC (with depth), pulled a little towards the camera so it is not
        // hidden by the very face it lies on.
        auto to_ndc3 = [&](const Vec3d &obj, Vec3f &out) {
            Vec3d w = inst * obj;
            const Vec3d to_cam = cam_persp ? Vec3d((cam_pos - w).normalized()) : to_viewer;
            w += to_cam * 0.3;
            const Vec4d clip = vpm * Vec4d(w.x(), w.y(), w.z(), 1.0);
            if (clip.w() <= 0.0)
                return false;
            out = Vec3f(float(clip.x() / clip.w()), float(clip.y() / clip.w()), float(clip.z() / clip.w()));
            return true;
        };
        const float sx = 0.5f * cnv_width, sy = 0.5f * cnv_height;
        auto add_quad3 = [&](GLModel::Geometry &data, unsigned int &idx, const Vec3f &a, const Vec3f &b, double hw) {
            // a, b in NDC; the half width hw is in pixels.
            Vec2d d((b.x() - a.x()) * sx, (b.y() - a.y()) * sy);
            const double len = d.norm();
            if (len < 1e-9)
                return;
            d /= len;
            const Vec2f n(float(-d.y() * hw / sx), float(d.x() * hw / sy));
            const Vec3f an(n.x(), n.y(), 0.f);
            for (const Vec3f &p : { Vec3f(a + an), Vec3f(a - an), Vec3f(b + an), Vec3f(a - an), Vec3f(b - an), Vec3f(b + an) }) {
                data.add_vertex(p);
                data.add_index(idx++);
            }
        };
        glsafe(::glEnable(GL_DEPTH_TEST));
        glsafe(::glDepthFunc(GL_LEQUAL));
        glsafe(::glDepthMask(GL_FALSE));
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        GLModel::Geometry s_halo, s_core;
        s_halo.format = s_core.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        s_halo.color  = { 0.0f, 0.0f, 0.0f, 0.6f };
        s_core.color  = { 0.35f, 0.75f, 1.0f, 1.0f };
        unsigned int s_halo_idx = 0, s_core_idx = 0;
        size_t fill_count = 0;
        for (const CardItem &card : cards) {
            // Rebuilt every frame while it moves (the SVG being placed, a decal being slid): outline only.
            const bool live = card.live || card.rings == &m_svg_local_rings;
            const SurfaceCard &sc = decal_surface(card.decal, *card.rings, live ? nullptr : card.tris, live);
            if (m_decal_preview_colours)
                for (const PaintDecalZone &zone : *card.groups) {
                    if (zone.hidden || zone.zone < 0 || size_t(zone.zone) >= sc.tris.size() || sc.tris[size_t(zone.zone)].empty())
                        continue;
                    GLModel::Geometry fill_data;
                    fill_data.format = s_core.format;
                    fill_data.color  = get_state_color(zone.state);
                    fill_data.color.a(0.55f);
                    unsigned int idx = 0;
                    const std::vector<Vec3d> &t = sc.tris[size_t(zone.zone)];
                    for (size_t k = 0; k + 2 < t.size(); k += 3) {
                        Vec3f a, b, c;
                        if (!to_ndc3(t[k], a) || !to_ndc3(t[k + 1], b) || !to_ndc3(t[k + 2], c))
                            continue;
                        for (const Vec3f &p : { a, b, c }) {
                            fill_data.add_vertex(p);
                            fill_data.add_index(idx++);
                        }
                    }
                    if (idx == 0)
                        continue;
                    if (m_decal_fill_models.size() <= fill_count)
                        m_decal_fill_models.resize(fill_count + 1);
                    GLModel &model = m_decal_fill_models[fill_count++];
                    model.reset();
                    model.init_from(std::move(fill_data));
                    glsafe(::glEnable(GL_POLYGON_OFFSET_FILL));
                    glsafe(::glPolygonOffset(-2.0f, -8.0f));
                    model.render();
                    glsafe(::glPolygonOffset(0.0f, 0.0f));
                    glsafe(::glDisable(GL_POLYGON_OFFSET_FILL));
                }
            for (const std::vector<Vec3d> &ring : sc.rings)
                for (size_t k = 0; k < ring.size(); ++k) {
                    const Vec3d &pa = ring[k], &pb = ring[(k + 1) % ring.size()];
                    if (!pa.allFinite() || !pb.allFinite())
                        continue;   // a ray that missed, or a jump over a silhouette
                    Vec3f a, b;
                    if (!to_ndc3(pa, a) || !to_ndc3(pb, b))
                        continue;
                    add_quad3(s_halo, s_halo_idx, a, b, 2.0);
                    add_quad3(s_core, s_core_idx, a, b, 0.8);
                }
        }
        for (size_t k = fill_count; k < m_decal_fill_models.size(); ++k)
            m_decal_fill_models[k].reset();
        m_decal_halo_model.reset();
        m_decal_core_model.reset();
        m_decal_back_halo_model.reset();
        m_decal_back_core_model.reset();
        if (s_halo_idx > 0) {
            m_decal_halo_model.init_from(std::move(s_halo));
            m_decal_halo_model.render();
        }
        if (s_core_idx > 0) {
            m_decal_core_model.init_from(std::move(s_core));
            m_decal_core_model.render();
        }
        glsafe(::glDepthMask(GL_TRUE));
        glsafe(::glDepthFunc(GL_LESS));
        glsafe(::glDisable(GL_DEPTH_TEST));
        render_mirror_marker();
        glsafe(::glDisable(GL_BLEND));
        return;
    }
    GLModel::Geometry halo_data;
    halo_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
    halo_data.color  = { 0.0f, 0.0f, 0.0f, 0.6f };
    GLModel::Geometry core_data;
    core_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
    core_data.color  = { 0.35f, 0.75f, 1.0f, 1.0f };
    unsigned int halo_idx = 0, core_idx = 0;
    auto add_quad = [&to_ndc](GLModel::Geometry &data, unsigned int &idx, const Vec2d &a, const Vec2d &b, double hw) {
        Vec2d d = b - a;
        const double len = d.norm();
        if (len < 1e-9)
            return;
        d /= len;
        const Vec2d n(-d.y() * hw, d.x() * hw);
        for (const Vec2d &p : { Vec2d(a + n), Vec2d(a - n), Vec2d(b + n), Vec2d(a - n), Vec2d(b - n), Vec2d(b + n) }) {
            data.add_vertex(to_ndc(p));
            data.add_index(idx++);
        }
    };
    // Colour preview: each zone's area in its filament's colour, see-through.
    size_t fill_count = 0;
    if (m_decal_preview_colours) {
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        for (const CardItem &card : cards) {
            const PaintDecal                      &dc   = card.decal;
            const std::vector<std::vector<Vec2d>> &tris = *card.tris;
            for (const PaintDecalZone &zone : *card.groups) {
                if (zone.hidden || zone.zone < 0 || size_t(zone.zone) >= tris.size() || tris[size_t(zone.zone)].empty())
                    continue;
                GLModel::Geometry fill_data;
                fill_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
                fill_data.color  = get_state_color(zone.state);
                fill_data.color.a(card.back ? 0.15f : 0.45f);
                unsigned int idx = 0;
                const std::vector<Vec2d> &t = tris[size_t(zone.zone)];
                for (size_t k = 0; k + 2 < t.size(); k += 3) {
                    Vec2d a, b, c;
                    if (!to_px(dc, t[k], a) || !to_px(dc, t[k + 1], b) || !to_px(dc, t[k + 2], c))
                        continue;
                    for (const Vec2d &p : { a, b, c }) {
                        fill_data.add_vertex(to_ndc(p));
                        fill_data.add_index(idx++);
                    }
                }
                if (idx == 0)
                    continue;
                if (m_decal_fill_models.size() <= fill_count)
                    m_decal_fill_models.resize(fill_count + 1);
                GLModel &model = m_decal_fill_models[fill_count++];
                model.reset();
                model.init_from(std::move(fill_data));
                model.render();
            }
        }
        glsafe(::glDisable(GL_BLEND));
    }
    for (size_t k = fill_count; k < m_decal_fill_models.size(); ++k)
        m_decal_fill_models[k].reset();

    GLModel::Geometry back_halo_data, back_core_data;
    back_halo_data.format = back_core_data.format = halo_data.format;
    back_halo_data.color  = { 0.0f, 0.0f, 0.0f, 0.25f };
    back_core_data.color  = { 0.35f, 0.75f, 1.0f, 0.35f };
    unsigned int back_halo_idx = 0, back_core_idx = 0;
    for (const CardItem &card : cards)
        for (const SvgRing &ring : *card.rings)
            for (size_t k = 0; k < ring.pts.size(); ++k) {
                Vec2d a, b;
                if (!to_px(card.decal, ring.pts[k], a) || !to_px(card.decal, ring.pts[(k + 1) % ring.pts.size()], b))
                    continue;
                if (card.back) {
                    add_quad(back_halo_data, back_halo_idx, a, b, 1.6);
                    add_quad(back_core_data, back_core_idx, a, b, 0.6);
                } else {
                    add_quad(halo_data, halo_idx, a, b, 2.0);
                    add_quad(core_data, core_idx, a, b, 0.8);
                }
            }
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    m_decal_halo_model.reset();
    m_decal_core_model.reset();
    m_decal_back_halo_model.reset();
    m_decal_back_core_model.reset();
    if (back_halo_idx > 0) {
        m_decal_back_halo_model.init_from(std::move(back_halo_data));
        m_decal_back_halo_model.render();
    }
    if (back_core_idx > 0) {
        m_decal_back_core_model.init_from(std::move(back_core_data));
        m_decal_back_core_model.render();
    }
    render_mirror_marker();
    if (halo_idx > 0) {
        m_decal_halo_model.init_from(std::move(halo_data));
        m_decal_halo_model.render();
    }
    if (core_idx > 0) {
        m_decal_core_model.init_from(std::move(core_data));
        m_decal_core_model.render();
    }
    glsafe(::glDisable(GL_BLEND));
}

// The card's outline (and, unless live, its zone fills cut finer) carried onto the mesh by the
// rays Apply casts: from the stored eye (perspective) or along the card's direction (parallel),
// first face hit. Cached by everything that moves a ray; a ray that misses, or a segment that
// jumps over a silhouette, is left out (NaN). live = the copies of the SVG being placed, rebuilt as
// it moves: outline only, thinned out.
const GLGizmoPainterBase::SurfaceCard &GLGizmoPainterBase::decal_surface(const PaintDecal &d, const std::vector<SvgRing> &rings,
    const std::vector<std::vector<Vec2d>> *tris, bool live)
{
    std::vector<Transform3d> trafos, trafos_no_translate;
    const Transform3d inst     = model_part_trafos(trafos, trafos_no_translate);
    const Transform3d inst_inv = inst.inverse();
    std::ostringstream key;
    key.precision(9);
    key << (live ? 'L' : 'S') << (const void *) &rings << ':' << rings.size() << ':' << (tris ? tris->size() : 0);
    for (double v : d.centre) key << ',' << v;
    for (double v : d.dir) key << ',' << v;
    for (double v : d.up) key << ',' << v;
    for (double v : d.lin) key << ',' << v;
    for (double v : d.off) key << ',' << v;
    for (double v : d.warp) key << ',' << v;
    key << ',' << d.perspective << ',' << d.eye_dist;
    for (const Transform3d &t : trafos)
        for (int k = 0; k < 16; ++k)
            key << ',' << t.matrix()(k);
    size_t pts = 0;
    size_t zones_hash = 0;   // parts: the same outlines can fall in other groups
    for (const SvgRing &r : rings) {
        pts += r.pts.size();
        zones_hash = zones_hash * 31 + size_t(r.zone + 1);
    }
    key << ':' << pts << ':' << zones_hash;
    auto it = m_surface_cache.find(key.str());
    if (it != m_surface_cache.end())
        return it->second;
    if (m_surface_cache.size() > 64)
        m_surface_cache.clear();
    SurfaceCard &sc = m_surface_cache[key.str()];

    const std::vector<const MeshRaycaster *> raycasters = m_c->raycaster()->raycasters();
    std::vector<Transform3d> mesh_to_obj, obj_to_mesh;
    for (size_t i = 0; i < trafos.size() && i < raycasters.size(); ++i) {
        mesh_to_obj.push_back(inst_inv * trafos[i]);
        obj_to_mesh.push_back(mesh_to_obj.back().inverse());
    }
    const BoundingBoxf3 box = m_c->selection_info()->model_object()->raw_mesh_bounding_box();
    const double        far_off = box.size().norm() * 2. + 10.;
    const Vec3d         dn  = Vec3d(d.dir[0], d.dir[1], d.dir[2]).normalized();
    const bool          persp = d.perspective && d.eye_dist > 0.;
    const Vec3d         eye   = d.eye_object();
    const Vec3d         nan3  = Vec3d::Constant(std::numeric_limits<double>::quiet_NaN());
    auto hit = [&](const Vec2d &svg) {
        const Vec3d P   = d.svg_to_object(svg);
        const Vec3d o   = persp ? eye : Vec3d(P + dn * (dn.dot(box.center() - P) + far_off));
        const Vec3d dir = persp ? Vec3d((P - eye).normalized()) : Vec3d(-dn);
        double best = std::numeric_limits<double>::max();
        Vec3d  out  = nan3;
        for (size_t i = 0; i < mesh_to_obj.size(); ++i) {
            const Vec3d om = obj_to_mesh[i] * o;
            const Vec3d dm = Vec3d(obj_to_mesh[i].linear() * dir).normalized();
            const AABBMesh::hit_result h = raycasters[i]->get_aabb_mesh().query_ray_hit(om, dm);
            if (!h.is_hit())
                continue;
            const Vec3d p    = mesh_to_obj[i] * h.position();
            const double dst = (p - o).norm();
            if (dst < best) {
                best = dst;
                out  = p;
            }
        }
        return out;
    };
    // A segment that jumps far more on the mesh than on the card crosses a silhouette.
    auto jump = [&](const Vec2d &sa, const Vec2d &sb, const Vec3d &pa, const Vec3d &pb) {
        const double on_card = (d.svg_to_object(sa) - d.svg_to_object(sb)).norm();
        return (pa - pb).norm() > 2.5 * on_card + 0.3;
    };

    size_t stride = 1;
    if (live && pts > 3000)
        stride = (pts + 2999) / 3000;
    for (const SvgRing &r : rings) {
        std::vector<Vec2d> src;
        for (size_t k = 0; k < r.pts.size(); k += stride)
            src.push_back(r.pts[k]);
        if (src.size() < 2)
            continue;
        std::vector<Vec3d> out(src.size());
        for (size_t k = 0; k < src.size(); ++k)
            out[k] = hit(src[k]);
        // A jump over a silhouette blanks the segment's start point (which also drops the segment
        // before it: one short gap, never a line across the object).
        std::vector<Vec3d> ring(out);
        for (size_t k = 0; k < src.size(); ++k) {
            const size_t n = (k + 1) % src.size();
            if (out[k].allFinite() && out[n].allFinite() && jump(src[k], src[n], out[k], out[n]))
                ring[k] = nan3;
        }
        sc.rings.push_back(std::move(ring));
    }

    if (tris != nullptr) {
        Matrix2d A;
        A << d.lin[0], d.lin[1], d.lin[2], d.lin[3];
        const double scale = std::sqrt(std::max(std::abs(A.determinant()), 1e-12));
        // Fine enough that a flat triangle stays close to a curved surface (it is drawn just above it).
        const double max_edge_svg = std::max(0.4, 0.5 * box.size().norm() / 120.) / scale;
        size_t budget = 60000;   // fill vertices at most, the outline is what matters
        sc.tris.resize(tris->size());
        for (size_t z = 0; z < tris->size(); ++z) {
            std::vector<Vec2d> fine;
            std::function<void(const Vec2d &, const Vec2d &, const Vec2d &, int)> split = [&](const Vec2d &a, const Vec2d &b, const Vec2d &c, int depth) {
                const double ab = (a - b).norm(), bc = (b - c).norm(), ca = (c - a).norm();
                const double m  = std::max({ ab, bc, ca });
                if (m <= max_edge_svg || depth > 8 || fine.size() + sc.tris[z].size() > budget) {
                    fine.insert(fine.end(), { a, b, c });
                    return;
                }
                if (m == ab) { const Vec2d h = 0.5 * (a + b); split(a, h, c, depth + 1); split(h, b, c, depth + 1); }
                else if (m == bc) { const Vec2d h = 0.5 * (b + c); split(a, b, h, depth + 1); split(a, h, c, depth + 1); }
                else { const Vec2d h = 0.5 * (c + a); split(a, b, h, depth + 1); split(h, b, c, depth + 1); }
            };
            const std::vector<Vec2d> &t = (*tris)[z];
            for (size_t k = 0; k + 2 < t.size(); k += 3)
                split(t[k], t[k + 1], t[k + 2], 0);
            for (size_t k = 0; k + 2 < fine.size(); k += 3) {
                const Vec3d a = hit(fine[k]), b = hit(fine[k + 1]), c = hit(fine[k + 2]);
                if (!a.allFinite() || !b.allFinite() || !c.allFinite())
                    continue;
                if (jump(fine[k], fine[k + 1], a, b) || jump(fine[k + 1], fine[k + 2], b, c) || jump(fine[k + 2], fine[k], c, a))
                    continue;
                sc.tris[z].insert(sc.tris[z].end(), { a, b, c });
            }
            budget = budget > sc.tris[z].size() ? budget - sc.tris[z].size() : 0;
        }
    }
    return sc;
}

void GLGizmoPainterBase::render_cursor_polygon()
{
    svg_finish_pending_edit();   // NEOTKO_SVGDECAL_TAG — F3: "Edit" lands here, with the new view's matrices
    const ModelObject *decal_mo = m_c->selection_info()->model_object();
    bool has_cards = false;
    if (decal_mo != nullptr)
        for (size_t i = 0; i < decal_mo->paint_decals.size() && !has_cards; ++i)
            has_cards = !decal_mo->paint_decals[i].hidden && int(i) != editing_decal();
    // F5: the SVG on screen shows its mirrored copies live, where they will land once saved.
    PaintDecal on_screen_decal;
    const bool on_screen_mirror = m_svg_mirror != 0 && !m_svg_local_rings.empty() && svg_build_decal(on_screen_decal);
    has_cards = has_cards || on_screen_mirror;
    if (m_shape_nodes.empty() && m_svg_rings.empty() && !has_cards)
        return;

    // NEOTKO — the shape is kept when the camera moves: it stays where it is on screen and
    // can be moved, scaled or edited again (it no longer follows the object).

    const Size cnv_size = m_parent.get_canvas_size();
    const float cnv_width  = float(cnv_size.get_width());
    const float cnv_height = float(cnv_size.get_height());
    if (cnv_width == 0.0f || cnv_height == 0.0f)
        return;
    const float cnv_inv_width  = 1.0f / cnv_width;
    const float cnv_inv_height = 1.0f / cnv_height;
    auto to_ndc = [cnv_inv_width, cnv_inv_height](const Vec2d &p) {
        return Vec2f(2.0f * (float(p.x()) * cnv_inv_width - 0.5f), -2.0f * (float(p.y()) * cnv_inv_height - 0.5f));
    };

    if (has_cards)
        render_decal_cards(on_screen_mirror ? &on_screen_decal : nullptr);

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    glsafe(::glDisable(GL_DEPTH_TEST));
    glsafe(::glPushAttrib(GL_ENABLE_BIT));
    shader->start_using();
    shader->set_uniform("view_model_matrix", Transform3d::Identity());
    shader->set_uniform("projection_matrix", Transform3d::Identity());

    const std::vector<Vec2d> flat = shape_flatten(m_shape_closed);

    // SVG shapes: each zone's area filled with the colour of its filament, and every ring outlined.
    // NEOTKO_SVGDECAL_TAG — the outline is built from triangles (a dark halo under a light core):
    // glLineWidth() above 1 px is ignored on macOS core profiles, and a 1 px dashed line got lost
    // on dark or busy models.
    if (!m_svg_rings.empty()) {
        GLModel::Geometry halo_data;
        halo_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
        halo_data.color  = { 0.0f, 0.0f, 0.0f, 0.85f };
        GLModel::Geometry core_data;
        core_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
        core_data.color  = { 1.0f, 1.0f, 1.0f, 1.0f };
        unsigned int halo_idx = 0;
        unsigned int core_idx = 0;
        // A segment as a quad of half-width hw pixels, stretched by hw at both ends to close the joins.
        auto add_thick_segment = [&to_ndc](GLModel::Geometry &data, unsigned int &idx, const Vec2d &a, const Vec2d &b, double hw) {
            Vec2d d = b - a;
            const double len = d.norm();
            if (len < 1e-9)
                return;
            d /= len;
            const Vec2d n(-d.y() * hw, d.x() * hw);
            const Vec2d a2 = a - d * hw;
            const Vec2d b2 = b + d * hw;
            for (const Vec2d &p : { Vec2d(a2 + n), Vec2d(a2 - n), Vec2d(b2 + n), Vec2d(a2 - n), Vec2d(b2 - n), Vec2d(b2 + n) }) {
                data.add_vertex(to_ndc(p));
                data.add_index(idx++);
            }
        };
        // The SVG's box (where it can be grabbed) and a move cross at its centre.
        {
            const std::array<Vec2d, 4> box = svg_screen_box();
            for (size_t i = 0; i < 4; ++i) {
                add_thick_segment(halo_data, halo_idx, box[i], box[(i + 1) % 4], 1.6);
                add_thick_segment(core_data, core_idx, box[i], box[(i + 1) % 4], 0.5);
            }
            const Vec2d c = 0.25 * (box[0] + box[1] + box[2] + box[3]);
            const double arm = 9.0;
            for (const Vec2d &d : { Vec2d(arm, 0.), Vec2d(0., arm) }) {
                add_thick_segment(halo_data, halo_idx, c - d, c + d, 2.5);
                add_thick_segment(core_data, core_idx, c - d, c + d, 1.0);
            }
            // F4: corner handles, squares at the corners of the (deformed) box.
            if (m_svg_deform) {
                const double h = double(PolygonCloseRadiusPx) * 0.6;
                for (const Vec2d &p : box) {
                    const std::array<Vec2d, 4> sq = { Vec2d(p + Vec2d(-h, -h)), Vec2d(p + Vec2d(h, -h)), Vec2d(p + Vec2d(h, h)), Vec2d(p + Vec2d(-h, h)) };
                    for (size_t k = 0; k < 4; ++k) {
                        add_thick_segment(halo_data, halo_idx, sq[k], sq[(k + 1) % 4], 2.5);
                        add_thick_segment(core_data, core_idx, sq[k], sq[(k + 1) % 4], 1.2);
                    }
                }
            }
        }
        for (const SvgRing &ring : m_svg_rings) {
            for (size_t i = 0; i < ring.pts.size(); ++i) {
                const Vec2d &p = ring.pts[i];
                const Vec2d &q = ring.pts[(i + 1) % ring.pts.size()];
                add_thick_segment(halo_data, halo_idx, p, q, 2.5);
                add_thick_segment(core_data, core_idx, p, q, 1.0);
            }
        }
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        m_svg_fill_models.resize(m_svg_groups.size());
        for (size_t z = 0; z < m_svg_groups.size(); ++z) {
            GLModel &model = m_svg_fill_models[z];
            model.reset();
            if (m_svg_groups[z].hidden || z >= m_svg_local_tris.size() || m_svg_local_tris[z].empty())
                continue;
            GLModel::Geometry fill_data;
            fill_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
            fill_data.color  = get_state_color(m_svg_groups[z].state);
            fill_data.color.a(0.55f);
            unsigned int fill_idx = 0;
            for (const Vec2d &t : m_svg_local_tris[z]) {
                fill_data.add_vertex(to_ndc(svg_local_to_screen(t)));
                fill_data.add_index(fill_idx++);
            }
            if (fill_idx > 0) {
                model.init_from(std::move(fill_data));
                model.render();
            }
        }
        // Parts: the selected ones outlined in orange, the one under the mouse in the list in white,
        // drawn over everything else of the SVG.
        auto outline_part = [&](GLModel &halo_model, GLModel &core_model, const std::vector<int> &ks, const ColorRGBA &colour) {
            GLModel::Geometry part_halo, part_core;
            part_halo.format = part_core.format = halo_data.format;
            part_halo.color  = { 0.0f, 0.0f, 0.0f, 0.85f };
            part_core.color  = colour;
            unsigned int ph_idx = 0, pc_idx = 0;
            for (int k : ks) {
                if (k < 0 || size_t(k) >= m_svg_shapes.size())
                    continue;
                for (const std::vector<Vec2d> &ring : m_svg_shapes[size_t(k)].rings)
                    for (size_t i = 0; i < ring.size(); ++i) {
                        const Vec2d a = svg_local_to_screen(ring[i]), b = svg_local_to_screen(ring[(i + 1) % ring.size()]);
                        add_thick_segment(part_halo, ph_idx, a, b, 2.9);
                        add_thick_segment(part_core, pc_idx, a, b, 1.4);
                    }
            }
            halo_model.reset();
            core_model.reset();
            if (ph_idx > 0) {
                halo_model.init_from(std::move(part_halo));
                core_model.init_from(std::move(part_core));
            }
        };
        outline_part(m_svg_part_halo_model, m_svg_part_core_model, std::vector<int>(m_svg_part_sel.begin(), m_svg_part_sel.end()),
                     ColorRGBA(1.0f, 0.55f, 0.1f, 1.0f));
        outline_part(m_svg_hover_halo_model, m_svg_hover_core_model, { m_svg_part_hover }, ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f));
        // The handles model is free in SVG mode (no nodes), so it carries the halo.
        m_polygon_handles_overlay.reset();
        if (halo_idx > 0) {
            m_polygon_handles_overlay.init_from(std::move(halo_data));
            m_polygon_handles_overlay.render();
        }
        m_polygon_overlay.reset();
        if (core_idx > 0) {
            m_polygon_overlay.init_from(std::move(core_data));
            m_polygon_overlay.render();
        }
        for (GLModel *model : { &m_svg_part_halo_model, &m_svg_part_core_model, &m_svg_hover_halo_model, &m_svg_hover_core_model })
            if (model->is_initialized())
                model->render();
        glsafe(::glDisable(GL_BLEND));
    }

    // Fill (closed only). Nonzero union, same fill rule as PolygonProjectionCursor.
    m_polygon_fill_overlay.reset();
    if (m_shape_closed && flat.size() >= 3) {
        Polygon poly;
        poly.points.reserve(flat.size());
        for (const Vec2d &p : flat)
            poly.points.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
        GLModel::Geometry fill_data;
        fill_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
        fill_data.color  = get_shape_fill_color();
        unsigned int idx = 0;
        for (const ExPolygon &ex : union_ex(Polygons{ poly }))
            for (const Vec2d &t : triangulate_expolygon_2d(ex)) {
                fill_data.add_vertex(to_ndc(t));
                fill_data.add_index(idx++);
            }
        if (idx > 0) {
            m_polygon_fill_overlay.init_from(std::move(fill_data));
            glsafe(::glEnable(GL_BLEND));
            glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
            m_polygon_fill_overlay.render();
            glsafe(::glDisable(GL_BLEND));
        }
    }

    // NEOTKO_SVGDECAL_TAG — outline and handles built from triangles (dark halo under a light core),
    // like the SVG: a 1 px dashed line got lost on the model. Green outline, white handles.
    {
        GLModel::Geometry halo_data, line_data, handle_data;
        for (GLModel::Geometry *g : { &halo_data, &line_data, &handle_data })
            g->format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P2 };
        halo_data.color   = { 0.0f, 0.0f, 0.0f, 0.85f };
        line_data.color   = { 0.2f, 1.0f, 0.45f, 1.0f };
        handle_data.color = { 1.0f, 1.0f, 1.0f, 1.0f };
        unsigned int halo_idx = 0, line_idx = 0, handle_idx = 0;
        auto quad = [&to_ndc](GLModel::Geometry &data, unsigned int &idx, const Vec2d &a, const Vec2d &b, double hw) {
            Vec2d d = b - a;
            const double len = d.norm();
            if (len < 1e-9)
                return;
            d /= len;
            const Vec2d n(-d.y() * hw, d.x() * hw);
            const Vec2d a2 = a - d * hw, b2 = b + d * hw;
            for (const Vec2d &p : { Vec2d(a2 + n), Vec2d(a2 - n), Vec2d(b2 + n), Vec2d(a2 - n), Vec2d(b2 - n), Vec2d(b2 + n) }) {
                data.add_vertex(to_ndc(p));
                data.add_index(idx++);
            }
        };
        auto segment = [&](GLModel::Geometry &core, unsigned int &core_idx, const Vec2d &a, const Vec2d &b, double hw) {
            quad(halo_data, halo_idx, a, b, hw + 1.5);
            quad(core, core_idx, a, b, hw);
        };
        auto square = [&](const Vec2d &c, double h) {
            const Vec2d p0 = c + Vec2d(-h, -h), p1 = c + Vec2d(h, -h), p2 = c + Vec2d(h, h), p3 = c + Vec2d(-h, h);
            segment(handle_data, handle_idx, p0, p1, 0.8); segment(handle_data, handle_idx, p1, p2, 0.8);
            segment(handle_data, handle_idx, p2, p3, 0.8); segment(handle_data, handle_idx, p3, p0, 0.8);
        };
        std::vector<Vec2d> outline = flat;
        if (!m_shape_closed && !outline.empty())
            outline.push_back(m_parent.get_local_mouse_position());   // rubber band to the mouse
        const size_t n_seg = m_shape_closed ? outline.size() : (outline.empty() ? 0 : outline.size() - 1);
        for (size_t i = 0; i < n_seg && outline.size() >= 2; ++i)
            segment(line_data, line_idx, outline[i], outline[(i + 1) % outline.size()], 1.0);
        for (size_t i = 0; i < m_shape_nodes.size(); ++i) {
            const ShapeNode &node = m_shape_nodes[i];
            // The first node is drawn bigger: clicking it closes the shape.
            square(node.p, (i == 0 && !m_shape_closed) ? 6. : 4.);
            if (node.is_curved()) {
                segment(handle_data, handle_idx, node.p, node.p + node.in, 0.6);
                segment(handle_data, handle_idx, node.p, node.p + node.out, 0.6);
                square(node.p + node.in, 2.5);
                square(node.p + node.out, 2.5);
            }
        }
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        m_shape_halo_model.reset();
        m_polygon_overlay.reset();
        m_polygon_handles_overlay.reset();
        if (halo_idx > 0) {
            m_shape_halo_model.init_from(std::move(halo_data));
            m_shape_halo_model.render();
        }
        if (line_idx > 0) {
            m_polygon_overlay.init_from(std::move(line_data));
            m_polygon_overlay.render();
        }
        if (handle_idx > 0) {
            m_polygon_handles_overlay.init_from(std::move(handle_data));
            m_polygon_handles_overlay.render();
        }
        glsafe(::glDisable(GL_BLEND));
    }

    shader->stop_using();
    glsafe(::glPopAttrib());
    glsafe(::glEnable(GL_DEPTH_TEST));
}

BoundingBoxf3 GLGizmoPainterBase::bounding_box() const
{
    BoundingBoxf3 ret;
    const Selection& selection = m_parent.get_selection();
    const Selection::IndicesList& idxs = selection.get_volume_idxs();
    for (unsigned int i : idxs) {
        const GLVolume* volume = selection.get_volume(i);
        if (!volume->is_modifier)
            ret.merge(volume->transformed_convex_hull_bounding_box());
    }
    return ret;
}

void GLGizmoPainterBase::update_contours(int i, const TriangleMesh& vol_mesh, float cursor_z, float max_z, float min_z) const
{
    const Selection& selection = m_parent.get_selection();
    const GLVolume* first_glvolume = selection.get_first_volume();
    const BoundingBoxf3& box = first_glvolume->transformed_convex_hull_bounding_box();

    const ModelObject* model_object = wxGetApp().model().objects[selection.get_object_idx()];
    const int instance_idx = selection.get_instance_idx();

        if (min_z < cursor_z && cursor_z < max_z) {
            if (m_cut_contours[i].cut_z != cursor_z || m_cut_contours[i].object_id != model_object->id() || m_cut_contours[i].instance_idx != instance_idx) {
                m_cut_contours[i].cut_z = cursor_z;

                m_cut_contours[i].mesh = vol_mesh;

                m_cut_contours[i].position = box.center();
                m_cut_contours[i].shift = Vec3d::Zero();
                m_cut_contours[i].object_id = model_object->id();
                m_cut_contours[i].instance_idx = instance_idx;
                m_cut_contours[i].contours.reset();

                MeshSlicingParams slicing_params;
                slicing_params.trafo = Transform3d::Identity().matrix();
                const Polygons polys = slice_mesh(m_cut_contours[i].mesh.its, cursor_z, slicing_params);
                if (!polys.empty()) {
                    m_cut_contours[i].contours.init_from(polys, static_cast<float>(cursor_z));
                    m_cut_contours[i].contours.set_color({ 1.0f, 1.0f, 1.0f, 1.0f });
                }
            }
            else if (box.center() != m_cut_contours[i].position) {
                m_cut_contours[i].shift = box.center() - m_cut_contours[i].position;
            }
        }
        else
            m_cut_contours[i].contours.reset();
}

bool GLGizmoPainterBase::is_mesh_point_clipped(const Vec3d& point, const Transform3d& trafo) const
{
    if (m_c->object_clipper()->get_position() == 0.)
        return false;

    auto sel_info = m_c->selection_info();
    Vec3d transformed_point =  trafo * point;
    transformed_point(2) += sel_info->get_sla_shift();
    return m_c->object_clipper()->get_clipping_plane()->is_point_clipped(transformed_point);
}

// Interpolate points between the previous and current mouse positions, which are then projected onto the object.
// Returned projected mouse positions are grouped by mesh_idx. It may contain multiple std::vector<GLGizmoPainterBase::ProjectedMousePosition>
// with the same mesh_idx, but all items in std::vector<GLGizmoPainterBase::ProjectedMousePosition> always have the same mesh_idx.
std::vector<std::vector<GLGizmoPainterBase::ProjectedMousePosition>> GLGizmoPainterBase::get_projected_mouse_positions(const Vec2d &mouse_position, const double resolution, const std::vector<Transform3d> &trafo_matrices) const
{
    // List of mouse positions that will be used as seeds for painting.
    std::vector<Vec2d> mouse_positions{mouse_position};
    if (m_last_mouse_click != Vec2d::Zero()) {
        // In case current mouse position is far from the last one,
        // add several positions from between into the list, so there
        // are no gaps in the painted region.
        if (size_t patches_in_between = size_t((mouse_position - m_last_mouse_click).norm() / resolution); patches_in_between > 0) {
            const Vec2d diff = (m_last_mouse_click - mouse_position) / (patches_in_between + 1);
            for (size_t patch_idx = 1; patch_idx <= patches_in_between; ++patch_idx)
                mouse_positions.emplace_back(mouse_position + patch_idx * diff);
            mouse_positions.emplace_back(m_last_mouse_click);
        }
    }

    const Camera                       &camera = wxGetApp().plater()->get_camera();
    std::vector<ProjectedMousePosition> mesh_hit_points;
    mesh_hit_points.reserve(mouse_positions.size());

    // In mesh_hit_points only the last item could have mesh_id == -1, any other items mustn't.
    for (const Vec2d &mp : mouse_positions) {
        update_raycast_cache(mp, camera, trafo_matrices);
        mesh_hit_points.push_back({m_rr.hit, m_rr.mesh_id, m_rr.facet});
        if (m_rr.mesh_id == -1)
            break;
    }

    // Divide mesh_hit_points into groups with the same mesh_idx. It may contain multiple groups with the same mesh_idx.
    std::vector<std::vector<ProjectedMousePosition>> mesh_hit_points_by_mesh;
    for (size_t prev_mesh_hit_point = 0, curr_mesh_hit_point = 0; curr_mesh_hit_point < mesh_hit_points.size(); ++curr_mesh_hit_point) {
        size_t next_mesh_hit_point = curr_mesh_hit_point + 1;
        if (next_mesh_hit_point >= mesh_hit_points.size() || mesh_hit_points[curr_mesh_hit_point].mesh_idx != mesh_hit_points[next_mesh_hit_point].mesh_idx) {
            mesh_hit_points_by_mesh.emplace_back();
            mesh_hit_points_by_mesh.back().insert(mesh_hit_points_by_mesh.back().end(), mesh_hit_points.begin() + int(prev_mesh_hit_point), mesh_hit_points.begin() + int(next_mesh_hit_point));
            prev_mesh_hit_point = next_mesh_hit_point;
        }
    }

    auto on_same_facet = [](std::vector<ProjectedMousePosition> &hit_points) -> bool {
        for (const ProjectedMousePosition &mesh_hit_point : hit_points)
            if (mesh_hit_point.facet_idx != hit_points.front().facet_idx)
                return false;
        return true;
    };

    struct Plane
    {
        Vec3d origin;
        Vec3d first_axis;
        Vec3d second_axis;
    };
    auto find_plane = [](std::vector<ProjectedMousePosition> &hit_points) -> std::optional<Plane> {
        assert(hit_points.size() >= 3);
        for (size_t third_idx = 2; third_idx < hit_points.size(); ++third_idx) {
            const Vec3d &first_point  = hit_points[third_idx - 2].mesh_hit.cast<double>();
            const Vec3d &second_point = hit_points[third_idx - 1].mesh_hit.cast<double>();
            const Vec3d &third_point  = hit_points[third_idx].mesh_hit.cast<double>();

            const Vec3d  first_vec    = first_point - second_point;
            const Vec3d  second_vec   = third_point - second_point;

            // If three points aren't collinear, then there exists only one plane going through all points.
            if (first_vec.cross(second_vec).squaredNorm() > sqr(EPSILON)) {
                const Vec3d first_axis_vec_n = first_vec.normalized();
                // Make second_vec perpendicular to first_axis_vec_n using Gram–Schmidt orthogonalization process
                const Vec3d second_axis_vec_n = (second_vec - (first_vec.dot(second_vec) / first_vec.dot(first_vec)) * first_vec).normalized();
                return Plane{second_point, first_axis_vec_n, second_axis_vec_n};
            }
        }

        return std::nullopt;
    };

    for(std::vector<ProjectedMousePosition> &hit_points : mesh_hit_points_by_mesh) {
        assert(!hit_points.empty());
        if (hit_points.back().mesh_idx == -1)
            break;

        if (hit_points.size() <= 2)
            continue;

        if (on_same_facet(hit_points)) {
            hit_points = {hit_points.front(), hit_points.back()};
        } else if (std::optional<Plane> plane = find_plane(hit_points); plane) {
            Polyline polyline;
            polyline.points.reserve(hit_points.size());
            // Project hit_points into its plane to simplified them in the next step.
            for (auto &hit_point : hit_points) {
                const Vec3d &point  = hit_point.mesh_hit.cast<double>();
                const double x_cord = plane->first_axis.dot(point - plane->origin);
                const double y_cord = plane->second_axis.dot(point - plane->origin);
                polyline.points.emplace_back(scale_(x_cord), scale_(y_cord));
            }

            polyline.simplify(scale_(m_cursor_radius) / 10.);

            const int                           mesh_idx = hit_points.front().mesh_idx;
            std::vector<ProjectedMousePosition> new_hit_points;
            new_hit_points.reserve(polyline.points.size());
            // Project 2D simplified hit_points beck to 3D.
            for (const Point &point : polyline.points) {
                const double x_cord        = unscale<double>(point.x());
                const double y_cord        = unscale<double>(point.y());
                const Vec3d  new_hit_point = plane->origin + x_cord * plane->first_axis + y_cord * plane->second_axis;
                const int    facet_idx     = m_c->raycaster()->raycasters()[mesh_idx]->get_closest_facet(new_hit_point.cast<float>());
                new_hit_points.push_back({new_hit_point.cast<float>(), mesh_idx, size_t(facet_idx)});
            }

            hit_points = new_hit_points;
        } else {
            hit_points = {hit_points.front(), hit_points.back()};
        }
    }

    return mesh_hit_points_by_mesh;
}

// BBS
std::vector<GLGizmoPainterBase::ProjectedHeightRange> GLGizmoPainterBase::get_projected_height_range(
    const Vec2d& mouse_position,
    double resolution,
    const std::vector<const ModelVolume*>& part_volumes,
    const std::vector<Transform3d>& trafo_matrices) const
{
    std::vector<GLGizmoPainterBase::ProjectedHeightRange> hit_triangles_by_mesh;

    const Camera& camera = wxGetApp().plater()->get_camera();

    // In mesh_hit_points only the last item could have mesh_id == -1, any other items mustn't.
    update_raycast_cache(mouse_position, camera, trafo_matrices);
    if (m_rr.mesh_id == -1)
        return hit_triangles_by_mesh;

    ProjectedMousePosition mesh_hit_point = { m_rr.hit, m_rr.mesh_id, m_rr.facet };
    float z_bot_world= (trafo_matrices[m_rr.mesh_id] * Vec3d(m_rr.hit(0), m_rr.hit(1), m_rr.hit(2))).z();
    float z_top_world = z_bot_world+ m_cursor_height;
    hit_triangles_by_mesh.push_back({ z_bot_world, m_rr.mesh_id, size_t(m_rr.facet) });

    const Selection& selection = m_parent.get_selection();
    const ModelObject* mo = m_c->selection_info()->model_object();
    const ModelInstance* mi = mo->instances[selection.get_instance_idx()];
    const Transform3d   instance_trafo = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
        mi->get_assemble_transformation().get_matrix() :
        mi->get_transformation().get_matrix();
    const Transform3d   instance_trafo_not_translate = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
        mi->get_assemble_transformation().get_matrix_no_offset() :
        mi->get_transformation().get_matrix_no_offset();

    for (int mesh_idx = 0; mesh_idx < part_volumes.size(); mesh_idx++) {
        if (mesh_idx == m_rr.mesh_id)
            continue;

        const Transform3d& trafo = trafo_matrices[mesh_idx];
        const indexed_triangle_set& its = part_volumes[mesh_idx]->mesh().its;

        int first_hit_facet_idx = -1;
        for (int facet_idx = 0; facet_idx < its.indices.size(); facet_idx++) {
            stl_vertex v0 = its.vertices[its.indices[facet_idx].x()];
            stl_vertex v1 = its.vertices[its.indices[facet_idx].y()];
            stl_vertex v2 = its.vertices[its.indices[facet_idx].z()];

            float v0_z = (trafo * Vec3d(v0(0), v0(1), v0(2))).z();
            float v1_z = (trafo * Vec3d(v1(0), v1(1), v1(2))).z();
            float v2_z = (trafo * Vec3d(v2(0), v2(1), v2(2))).z();
            bool outside_range = (v0_z < z_bot_world&& v1_z < z_bot_world&& v2_z < z_bot_world) ||
                                 (v0_z > z_top_world && v1_z > z_top_world && v2_z > z_top_world);
            if (!outside_range) {
                first_hit_facet_idx = facet_idx;
                break;
            }
        }

        if (first_hit_facet_idx != -1) {
            hit_triangles_by_mesh.push_back({ z_bot_world, mesh_idx, (size_t)first_hit_facet_idx });
        }
    }

    return hit_triangles_by_mesh;
}

// Following function is called from GLCanvas3D to inform the gizmo about a mouse/keyboard event.
// The gizmo has an opportunity to react - if it does, it should return true so that the Canvas3D is
// aware that the event was reacted to and stops trying to make different sense of it. If the gizmo
// concludes that the event was not intended for it, it should return false.
// NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F5 (commit 4): cubic Bezier helpers for the shape mask.
static Vec2d shape_bezier_point(const std::array<Vec2d, 4> &c, double t)
{
    const double u = 1. - t;
    return u * u * u * c[0] + 3. * u * u * t * c[1] + 3. * u * t * t * c[2] + t * t * t * c[3];
}

// Appends the curve c (without its first point) to out, subdividing until each piece is flat
// within tol pixels: the distance of both control points to the chord.
static void shape_flatten_bezier(const std::array<Vec2d, 4> &c, double tol, int depth, std::vector<Vec2d> &out)
{
    auto dist_to_chord = [&c](const Vec2d &p) {
        const Vec2d  d   = c[3] - c[0];
        const double len = d.norm();
        if (len < EPSILON)
            return (p - c[0]).norm();
        return std::abs(d.x() * (p.y() - c[0].y()) - d.y() * (p.x() - c[0].x())) / len;
    };
    if (depth <= 0 || std::max(dist_to_chord(c[1]), dist_to_chord(c[2])) <= tol) {
        out.push_back(c[3]);
        return;
    }
    const Vec2d p01 = 0.5 * (c[0] + c[1]), p12 = 0.5 * (c[1] + c[2]), p23 = 0.5 * (c[2] + c[3]);
    const Vec2d p012 = 0.5 * (p01 + p12), p123 = 0.5 * (p12 + p23);
    const Vec2d mid = 0.5 * (p012 + p123);
    shape_flatten_bezier({ c[0], p01, p012, mid }, tol, depth - 1, out);
    shape_flatten_bezier({ mid, p123, p23, c[3] }, tol, depth - 1, out);
}

// Crossing-number test on a closed polyline (screen space).
static bool point_in_shape(const std::vector<Vec2d> &poly, const Vec2d &p)
{
    bool inside = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Vec2d &a = poly[i];
        const Vec2d &b = poly[j];
        if ((a.y() > p.y()) != (b.y() > p.y()) && p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x())
            inside = !inside;
    }
    return inside;
}

void GLGizmoPainterBase::shape_clear()
{
    m_svg_rings.clear();
    m_svg_local_rings.clear();
    m_svg_local_tris.clear();
    m_svg_zones.clear();
    m_svg_parts.clear();
    m_svg_groups.clear();
    m_svg_shapes.clear();
    m_svg_part_sel.clear();
    m_svg_part_hover = -1;
    m_svg_pick_parts = false;
    m_shape_from_decal = -1;
    m_shape_from_state = -1;
    m_shape_from_mirror = 0;
    m_shape_from_readable = false;
    m_svg_data.clear();
    m_svg_name.clear();
    m_svg_edit_decal = -1;
    m_svg_warp       = Matrix3d::Identity();
    m_svg_mirror     = 0;
    m_svg_mirror_readable = false;
    m_shape_draw_armed = false;
    m_decal_move_armed = false;
    m_shape_nodes.clear();
    m_shape_closed   = false;
    m_shape_drag     = ShapeDrag::None;
    m_shape_drag_idx = -1;
    m_button_down    = Button::None;
}

std::vector<Vec2d> GLGizmoPainterBase::shape_flatten(bool closed) const
{
    std::vector<Vec2d> out;
    const int n = int(m_shape_nodes.size());
    if (n == 0)
        return out;
    // 2^6 = 64 pieces per segment at most (ShapeFlattenMaxSegments).
    const int max_depth = 6;
    static_assert(ShapeFlattenMaxSegments == 64, "max_depth must match ShapeFlattenMaxSegments");
    out.push_back(m_shape_nodes.front().p);
    const int segments = closed ? n : n - 1;
    for (int i = 0; i < segments; ++i) {
        const ShapeNode &a = m_shape_nodes[i];
        const ShapeNode &b = m_shape_nodes[(i + 1) % n];
        if (a.out.isZero() && b.in.isZero())
            out.push_back(b.p);
        else
            shape_flatten_bezier({ a.p, a.p + a.out, b.p + b.in, b.p }, ShapeFlattenTolerancePx, max_depth, out);
    }
    // A closed shape ends where it started; the polygon cursor closes it implicitly.
    if (closed && out.size() > 1)
        out.pop_back();
    return out;
}

void GLGizmoPainterBase::shape_apply(EnforcerBlockerType new_state, const std::string &snapshot_name)
{
    if (!m_shape_closed || (m_shape_nodes.size() < 3 && m_svg_rings.empty()) || m_triangle_selectors.empty())
        return;
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), snapshot_name, UndoRedo::SnapshotType::GizmoAction);
    if (m_svg_rings.empty()) {
        apply_polygon_mask(new_state, shape_flatten(true));
    } else {
        // NEOTKO_SVGDECAL_TAG — F0: time of the SVG Apply in the PROFILE log, for both paths.
        const auto t0 = std::chrono::steady_clock::now();
        size_t holes = 0, points = 0;
        for (const SvgRing &ring : m_svg_rings) {
            holes  += ring.hole ? 1 : 0;
            points += ring.pts.size();
        }
        // F2: one pass per group (colour, or part with its own filament) with its filament (Erase:
        // every visible group with NONE). Groups are disjoint, so the order does not matter.
        for (size_t z = 0; z < m_svg_groups.size(); ++z) {
            const PaintDecalZone &zone = m_svg_groups[z];
            if (zone.hidden)
                continue;
            std::vector<std::vector<Vec2d>> rings;
            std::vector<bool>               is_hole;
            for (const SvgRing &ring : m_svg_rings)
                if (ring.zone == int(z)) {
                    rings.push_back(ring.pts);
                    is_hole.push_back(ring.hole);
                }
            if (rings.empty())
                continue;
            const EnforcerBlockerType state = new_state == EnforcerBlockerType::NONE ? EnforcerBlockerType::NONE
                                                                                      : EnforcerBlockerType(zone.state);
            apply_polygon_set_mask(state, rings, is_hole);
        }
        if (NeoDebug::enabled(NeoDebug::PROFILE)) {
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            size_t facets = 0, parts = 0;
            if (const ModelObject *mo = m_c->selection_info()->model_object())
                for (const ModelVolume *mv : mo->volumes)
                    if (mv->is_model_part()) {
                        facets += mv->mesh().facets_count();
                        ++parts;
                    }
            std::ostringstream os;
            os << "[SVGAPPLY] zones=" << m_svg_zones.size() << " rings=" << m_svg_rings.size()
               << " holes=" << holes << " points=" << points << " parts=" << parts << " facets=" << facets
               << " state=" << int(new_state) << " ms=" << std::fixed << std::setprecision(1) << ms;
            NeoDebug::write(NeoDebug::PROFILE, os.str());
        }
    }
    update_model_object();
    if (!m_shape_keep_after_apply)
        shape_clear();
    m_parent.set_as_dirty();
}

bool GLGizmoPainterBase::shape_load_svg(const std::string &path)
{
    m_shape_svg_error.clear();
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    if (file)
        text << file.rdbuf();
    if (text.str().empty()) {
        m_shape_svg_error = _u8L("The SVG file could not be read.");
        return false;
    }

    // Same cleaning as a Sandwich Sticker: colours set by CSS classes, background rectangle
    // dropped, transparencies resolved, so what paints is what the SVG shows.
    const std::string svg = ColorStitch::svg_flatten_for_sticker(ColorStitch::svg_inline_css_classes(text.str()), false);
    const std::vector<ColorStitch::StickerZone> zones = ColorStitch::sticker_svg_zones(svg);
    if (zones.empty()) {
        m_shape_svg_error = _u8L("The SVG has no filled shape to paint.");
        return false;
    }

    shape_clear();
    m_svg_data = svg;
    m_svg_name = boost::filesystem::path(path).stem().string();
    for (const ColorStitch::StickerZone &zone : zones)
        m_svg_zones.push_back({ zone.rgb, zone.grad, svg_default_state(zone.rgb), false });
    svg_rebuild_masks();
    if (m_svg_local_rings.empty()) {
        shape_clear();
        m_shape_svg_error = _u8L("The SVG has no filled shape to paint.");
        return false;
    }

    // Fitted to the middle 60% of the canvas. The SVG frame has Y up, the screen Y down.
    Vec2d lo = Vec2d::Constant(std::numeric_limits<double>::max());
    Vec2d hi = Vec2d::Constant(std::numeric_limits<double>::lowest());
    for (const SvgRing &ring : m_svg_local_rings)
        for (const Vec2d &p : ring.pts) {
            lo = lo.cwiseMin(p);
            hi = hi.cwiseMax(p);
        }
    const Vec2d size = hi - lo;
    if (size.x() <= 0. || size.y() <= 0.) {
        shape_clear();
        m_shape_svg_error = _u8L("The SVG shape has no area.");
        return false;
    }
    const Size   cnv_size = m_parent.get_canvas_size();
    const double cnv_w    = cnv_size.get_width();
    const double cnv_h    = cnv_size.get_height();
    const double scale    = 0.6 * std::min(cnv_w / size.x(), cnv_h / size.y());
    m_svg_lin << scale, 0., 0., -scale;
    const Vec2d local_centre = 0.5 * (lo + hi);
    m_svg_off = Vec2d(0.5 * cnv_w, 0.5 * cnv_h) - Vec2d(m_svg_lin * local_centre);
    svg_update_screen_rings();

    m_shape_closed     = true;
    m_shape_scale_pct  = 100.f;
    m_svg_width_pct    = 100.f;
    m_svg_height_pct   = 100.f;
    m_shape_rotate_deg = 0.f;
    m_parent.set_as_dirty();
    return true;
}

// Zone areas from the Sticker engine, with the hidden zones transparent.
void GLGizmoPainterBase::svg_rebuild_masks()
{
    m_svg_local_rings.clear();
    std::vector<PaintDecalZone> zones;
    for (size_t z = 0; z < m_svg_zones.size(); ++z)
        zones.push_back({ int(z), m_svg_zones[z].rgb, m_svg_zones[z].state, m_svg_zones[z].hidden });
    PaintDecalGroups groups = paint_decal_groups(m_svg_data, zones, m_svg_parts);
    m_svg_groups = std::move(groups.groups);
    const std::vector<ExPolygons> masks = ColorStitch::svg_group_masks(m_svg_data, Transform3d::Identity(), groups.shape_group, m_svg_groups.size());
    if (m_svg_shapes.empty()) {
        const std::vector<ColorStitch::StickerShape> shapes    = ColorStitch::sticker_shapes_in_transform(m_svg_data, Transform3d::Identity());
        const std::vector<int>                       shape_zone = ColorStitch::sticker_shape_zones(m_svg_data);
        m_svg_shapes.resize(shapes.size());
        for (size_t k = 0; k < shapes.size(); ++k) {
            SvgShape &sh = m_svg_shapes[k];
            sh.zone = k < shape_zone.size() ? shape_zone[k] : -1;
            sh.lo   = Vec2d::Constant(std::numeric_limits<double>::max());
            sh.hi   = Vec2d::Constant(std::numeric_limits<double>::lowest());
            for (const Polygon &poly : shapes[k].rings) {
                std::vector<Vec2d> ring;
                ring.reserve(poly.points.size());
                for (const Point &pt : poly.points) {
                    ring.push_back(pt.cast<double>() * SCALING_FACTOR);
                    sh.lo = sh.lo.cwiseMin(ring.back());
                    sh.hi = sh.hi.cwiseMax(ring.back());
                }
                sh.rings.push_back(std::move(ring));
            }
        }
    }
    auto add_ring = [this](const Polygon &poly, bool hole, int zone) {
        if (poly.points.size() < 3)
            return;
        SvgRing ring;
        ring.hole = hole;
        ring.zone = zone;
        ring.pts.reserve(poly.points.size());
        for (const Point &pt : poly.points)
            ring.pts.push_back(pt.cast<double>() * SCALING_FACTOR);
        m_svg_local_rings.push_back(std::move(ring));
    };
    m_svg_local_tris.assign(masks.size(), {});
    for (size_t z = 0; z < masks.size(); ++z)
        for (const ExPolygon &ex : masks[z]) {
            add_ring(ex.contour, false, int(z));
            for (const Polygon &hole : ex.holes)
                add_ring(hole, true, int(z));
            for (const Vec2d &t : triangulate_expolygon_2d(ex))
                m_svg_local_tris[z].push_back(t);
        }
    m_svg_local_lo = Vec2d::Constant(std::numeric_limits<double>::max());
    m_svg_local_hi = Vec2d::Constant(std::numeric_limits<double>::lowest());
    for (const SvgRing &ring : m_svg_local_rings)
        for (const Vec2d &p : ring.pts) {
            m_svg_local_lo = m_svg_local_lo.cwiseMin(p);
            m_svg_local_hi = m_svg_local_hi.cwiseMax(p);
        }
    if (m_svg_local_rings.empty())
        m_svg_local_lo = m_svg_local_hi = Vec2d::Zero();
}

// NEOTKO_SVGDECAL_TAG — parts.
PaintDecalPart *GLGizmoPainterBase::svg_part(int shape, bool create)
{
    for (PaintDecalPart &p : m_svg_parts)
        if (p.shape == shape)
            return &p;
    if (!create || shape < 0 || size_t(shape) >= m_svg_shapes.size())
        return nullptr;
    PaintDecalPart p;
    p.shape = shape;
    m_svg_parts.push_back(p);
    return &m_svg_parts.back();
}

void GLGizmoPainterBase::svg_prune_parts()
{
    m_svg_parts.erase(std::remove_if(m_svg_parts.begin(), m_svg_parts.end(),
                                     [](const PaintDecalPart &p) { return p.state < 0 && !p.hidden; }),
                      m_svg_parts.end());
    std::sort(m_svg_parts.begin(), m_svg_parts.end(), [](const PaintDecalPart &a, const PaintDecalPart &b) { return a.shape < b.shape; });
}

int GLGizmoPainterBase::svg_pick_part(const Vec2d &screen) const
{
    // Screen to the SVG frame: undo the placement, then the corner deform.
    const Matrix2d &lin = m_svg_lin;
    if (std::abs(lin.determinant()) < 1e-12)
        return -1;
    const Vec2d u = lin.inverse() * (screen - m_svg_off);
    const Vec3d q = m_svg_warp.inverse() * Vec3d(u.x(), u.y(), 1.);
    if (std::abs(q.z()) < 1e-12)
        return -1;
    const Vec2d p(q.x() / q.z(), q.y() / q.z());
    for (size_t k = m_svg_shapes.size(); k-- > 0; ) {
        const SvgShape &sh = m_svg_shapes[k];
        if (p.x() < sh.lo.x() || p.y() < sh.lo.y() || p.x() > sh.hi.x() || p.y() > sh.hi.y())
            continue;
        bool inside = false;   // even-odd over all its rings: holes included
        for (const std::vector<Vec2d> &ring : sh.rings)
            for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
                const Vec2d &a = ring[i], &b = ring[j];
                if ((a.y() > p.y()) != (b.y() > p.y()) && p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x())
                    inside = !inside;
            }
        if (inside)
            return int(k);
    }
    return -1;
}

// The SVG shape_to_svg_session() writes: one path, M then L / C segments, Z.
static const char *const DrawnShapeSvgPrefix = "<svg xmlns=\"http://www.w3.org/2000/svg\"><path fill=\"#";

bool GLGizmoPainterBase::svg_is_drawn_shape() const
{
    return m_svg_data.compare(0, std::strlen(DrawnShapeSvgPrefix), DrawnShapeSvgPrefix) == 0 && m_svg_shapes.size() == 1;
}

bool GLGizmoPainterBase::svg_to_shape_nodes()
{
    if (!svg_is_drawn_shape())
        return false;
    const size_t d0 = m_svg_data.find(" d=\"");
    const size_t d1 = d0 == std::string::npos ? std::string::npos : m_svg_data.find('"', d0 + 4);
    if (d1 == std::string::npos)
        return false;
    std::istringstream in(m_svg_data.substr(d0 + 4, d1 - d0 - 4));
    in.imbue(std::locale::classic());
    // Nodes in the path's own units (screen pixels when it was drawn).
    std::vector<ShapeNode> nodes;
    std::string cmd;
    Vec2d       first;
    if (!(in >> cmd) || cmd != "M" || !(in >> first.x() >> first.y()))
        return false;
    nodes.push_back({ first, Vec2d::Zero(), Vec2d::Zero() });
    while (in >> cmd && cmd != "Z") {
        ShapeNode &a = nodes.back();
        Vec2d c1, c2, b;
        if (cmd == "L") {
            if (!(in >> b.x() >> b.y()))
                return false;
            c1 = a.p;
            c2 = b;
        } else if (cmd == "C") {
            if (!(in >> c1.x() >> c1.y() >> c2.x() >> c2.y() >> b.x() >> b.y()))
                return false;
        } else
            return false;
        a.out = c1 - a.p;
        if ((b - first).norm() < 1e-6)
            nodes.front().in = c2 - first;   // the closing segment
        else
            nodes.push_back({ b, c2 - b, Vec2d::Zero() });
    }
    if (nodes.size() < 3)
        return false;

    // Path units to the SVG frame: 25.4/96 mm per unit, Y flipped, and the Sticker engine centres
    // the drawing, so the centres of the two boxes match.
    const double k = 25.4 / 96.0;
    std::vector<ShapeNode> saved_nodes = std::move(m_shape_nodes);
    m_shape_nodes = nodes;
    const std::vector<Vec2d> flat = shape_flatten(true);
    m_shape_nodes = std::move(saved_nodes);
    Vec2d plo = Vec2d::Constant(std::numeric_limits<double>::max()), phi = Vec2d::Constant(std::numeric_limits<double>::lowest());
    for (const Vec2d &p : flat) {
        plo = plo.cwiseMin(p);
        phi = phi.cwiseMax(p);
    }
    const Vec2d pmid = 0.5 * (plo + phi);
    const Vec2d lmid = 0.5 * (m_svg_shapes.front().lo + m_svg_shapes.front().hi);
    auto to_screen = [&](const Vec2d &p) { return svg_local_to_screen(Vec2d(lmid.x() + k * (p.x() - pmid.x()), lmid.y() - k * (p.y() - pmid.y()))); };
    for (ShapeNode &n : nodes) {
        const Vec2d p = to_screen(n.p);
        n.in  = n.in.isZero() ? Vec2d::Zero() : Vec2d(to_screen(n.p + n.in) - p);
        n.out = n.out.isZero() ? Vec2d::Zero() : Vec2d(to_screen(n.p + n.out) - p);
        n.p   = p;
    }

    const int  decal    = m_svg_edit_decal;
    const int  state    = m_svg_zones.empty() ? -1 : m_svg_zones.front().state;
    const int  mirror   = m_svg_mirror;
    const bool readable = m_svg_mirror_readable;
    shape_clear();
    m_shape_nodes         = std::move(nodes);
    m_shape_closed        = true;
    m_shape_from_decal    = decal;
    m_shape_from_state    = state;
    m_shape_from_mirror   = mirror;
    m_shape_from_readable = readable;
    m_parent.set_as_dirty();
    return true;
}

std::array<Vec2d, 4> GLGizmoPainterBase::svg_screen_box() const
{
    const Vec2d &lo = m_svg_local_lo, &hi = m_svg_local_hi;
    return { svg_local_to_screen(lo), svg_local_to_screen(Vec2d(hi.x(), lo.y())),
             svg_local_to_screen(hi), svg_local_to_screen(Vec2d(lo.x(), hi.y())) };
}

Vec2d GLGizmoPainterBase::svg_local_to_screen(const Vec2d &local) const
{
    const Vec3d q      = m_svg_warp * Vec3d(local.x(), local.y(), 1.);
    const Vec2d placed = std::abs(q.z()) > 1e-12 ? Vec2d(q.x() / q.z(), q.y() / q.z()) : local;
    return Vec2d(m_svg_lin * placed) + m_svg_off;
}

bool GLGizmoPainterBase::svg_set_corner(int k, const Vec2d &screen)
{
    if (k < 0 || k > 3 || m_svg_local_rings.empty() || std::abs(m_svg_lin.determinant()) < 1e-12)
        return false;
    const Vec2d &lo = m_svg_local_lo, &hi = m_svg_local_hi;
    const std::array<Vec2d, 4> src = { lo, Vec2d(hi.x(), lo.y()), hi, Vec2d(lo.x(), hi.y()) };
    std::array<Vec2d, 4> dst;
    for (int i = 0; i < 4; ++i) {
        const Vec3d q = m_svg_warp * Vec3d(src[i].x(), src[i].y(), 1.);
        dst[i] = Vec2d(q.x() / q.z(), q.y() / q.z());
    }
    dst[k] = Vec2d(m_svg_lin.inverse() * (screen - m_svg_off));
    // Convex and turning the same way as the undeformed box (no fold, no flip).
    double turn = 0.;
    for (int i = 0; i < 4; ++i) {
        const Vec2d  a = dst[(i + 1) % 4] - dst[i], b = dst[(i + 2) % 4] - dst[(i + 1) % 4];
        const double c = a.x() * b.y() - a.y() * b.x();
        if (std::abs(c) < 1e-9 || (turn != 0. && (c > 0.) != (turn > 0.)))
            return false;
        turn = c;
    }
    if (turn < 0.)
        return false;
    m_svg_warp = homography_from_quads(src, dst);
    svg_update_screen_rings();
    return true;
}

void GLGizmoPainterBase::svg_update_screen_rings()
{
    m_svg_rings = m_svg_local_rings;
    for (SvgRing &ring : m_svg_rings)
        for (Vec2d &p : ring.pts)
            p = svg_local_to_screen(p);
}

bool GLGizmoPainterBase::shape_contains(const Vec2d &p) const
{
    if (m_svg_rings.empty())
        return m_shape_closed && !m_shape_nodes.empty() && point_in_shape(shape_flatten(true), p);
    // NEOTKO_SVGDECAL_TAG — anywhere inside the SVG's box, transparent parts included.
    const std::array<Vec2d, 4> box = svg_screen_box();
    return point_in_shape(std::vector<Vec2d>(box.begin(), box.end()), p);
}

void GLGizmoPainterBase::shape_move(const Vec2d &delta)
{
    for (ShapeNode &node : m_shape_nodes)
        node.p += delta;
    if (!m_svg_local_rings.empty()) {
        m_svg_off += delta;
        svg_update_screen_rings();
    }
}

// Centre of the bounding box of every point of the shape on screen.
static Vec2d shape_bbox_centre(const std::vector<Vec2d> &points)
{
    Vec2d lo = Vec2d::Constant(std::numeric_limits<double>::max());
    Vec2d hi = Vec2d::Constant(std::numeric_limits<double>::lowest());
    for (const Vec2d &p : points) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    return 0.5 * (lo + hi);
}

void GLGizmoPainterBase::shape_scale(double factor)
{
    std::vector<Vec2d> all;
    for (const ShapeNode &node : m_shape_nodes)
        all.push_back(node.p);
    for (const SvgRing &ring : m_svg_rings)
        all.insert(all.end(), ring.pts.begin(), ring.pts.end());
    if (all.empty())
        return;
    const Vec2d centre = shape_bbox_centre(all);
    for (ShapeNode &node : m_shape_nodes) {
        node.p    = centre + factor * (node.p - centre);
        node.out *= factor;
        node.in  *= factor;
    }
    if (!m_svg_local_rings.empty()) {
        m_svg_lin *= factor;
        m_svg_off = centre + factor * (m_svg_off - centre);
        svg_update_screen_rings();
    }
}

// F4b: stretch along the SVG's own X (axis 0) or Y (1) about the centre of its box. Without a
// corner deform it goes into the placement (exact); with one, into the deform (Reset corners
// then also drops the stretch).
void GLGizmoPainterBase::svg_stretch(int axis, double factor)
{
    if (m_svg_local_rings.empty() || factor <= 0.)
        return;
    const Vec2d c = 0.5 * (m_svg_local_lo + m_svg_local_hi);
    if (svg_is_deformed()) {
        Matrix3d d = Matrix3d::Identity();
        d(axis, axis) = factor;
        d(axis, 2)    = c[axis] * (1. - factor);
        m_svg_warp = m_svg_warp * d;
    } else {
        Matrix2d d = Matrix2d::Identity();
        d(axis, axis) = factor;
        m_svg_off = m_svg_off + m_svg_lin * Vec2d((Matrix2d::Identity() - d) * c);
        m_svg_lin = m_svg_lin * d;
    }
    svg_update_screen_rings();
}

void GLGizmoPainterBase::shape_rotate(double degrees)
{
    std::vector<Vec2d> all;
    for (const ShapeNode &node : m_shape_nodes)
        all.push_back(node.p);
    for (const SvgRing &ring : m_svg_rings)
        all.insert(all.end(), ring.pts.begin(), ring.pts.end());
    if (all.empty())
        return;
    const Vec2d    centre = shape_bbox_centre(all);
    const double   rad    = degrees * std::acos(-1.) / 180.;
    const double   c = std::cos(rad), s = std::sin(rad);
    Matrix2d       rot;
    rot << c, -s, s, c;
    for (ShapeNode &node : m_shape_nodes) {
        node.p   = centre + Vec2d(rot * (node.p - centre));
        node.out = Vec2d(rot * node.out);
        node.in  = Vec2d(rot * node.in);
    }
    if (!m_svg_local_rings.empty()) {
        m_svg_lin = Matrix2d(rot * m_svg_lin);
        m_svg_off = centre + Vec2d(rot * (m_svg_off - centre));
        svg_update_screen_rings();
    }
}

// NEOTKO_SVGDECAL_TAG — F3. The SVG on screen becomes a decal card anchored to the object, seen
// from the current camera. The card plane is perpendicular to the view: through the object's
// centre (ortho), or at the depth of that centre on the camera axis (perspective, so the stored eye
// IS the camera). Three points of the SVG are taken from the screen to that plane, which gives the
// SVG -> card mapping (exact: a plane facing the camera maps to the screen without distortion).
bool GLGizmoPainterBase::svg_build_decal(PaintDecal &decal)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || m_svg_local_rings.empty() || m_svg_data.empty())
        return false;
    std::vector<Transform3d> trafos, trafos_no_translate;
    const Transform3d inst     = model_part_trafos(trafos, trafos_no_translate);
    const Transform3d inst_inv = inst.inverse();
    const Camera     &camera   = wxGetApp().plater()->get_camera();

    const Vec3d fwd = Vec3d(inst_inv.linear() * camera.get_dir_forward()).normalized();
    Vec3d       up  = inst_inv.linear() * camera.get_dir_up();
    up = Vec3d(up - fwd * up.dot(fwd)).normalized();
    const Vec3d eye         = inst_inv * camera.get_position();
    const Vec3d bbox_centre = mo->raw_mesh_bounding_box().center();
    const bool  perspective = camera.get_type() == Camera::EType::Perspective;
    double      eye_dist    = fwd.dot(bbox_centre - eye);
    Vec3d       centre      = bbox_centre;
    if (perspective) {
        eye_dist = std::max(eye_dist, 1.);
        centre   = eye + fwd * eye_dist;
    }
    const Vec3d right = Vec3d(up.cross(-fwd)).normalized();

    const Matrix4d inv_vp = Matrix4d(camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix()).inverse();
    const std::array<int, 4> &vp = camera.get_viewport();
    auto card_uv = [&](const Vec2d &svg_pt, Vec2d &uv) {
        const Vec2d px = Vec2d(m_svg_lin * svg_pt) + m_svg_off;
        const double nx = 2.0 * (px.x() - vp[0]) / double(vp[2]) - 1.0;
        const double ny = 1.0 - 2.0 * (px.y() + vp[1]) / double(vp[3]);
        Vec4d a = inv_vp * Vec4d(nx, ny, -1.0, 1.0);
        Vec4d b = inv_vp * Vec4d(nx, ny, 1.0, 1.0);
        if (std::abs(a.w()) < 1e-12 || std::abs(b.w()) < 1e-12)
            return false;
        a /= a.w();
        b /= b.w();
        const Vec3d  A   = inst_inv * Vec3d(a.head<3>());
        const Vec3d  B   = inst_inv * Vec3d(b.head<3>());
        const Vec3d  D   = B - A;
        const double den = fwd.dot(D);
        if (std::abs(den) < 1e-12)
            return false;
        const Vec3d H = A + D * (fwd.dot(centre - A) / den);
        uv = Vec2d(right.dot(H - centre), up.dot(H - centre));
        return true;
    };
    Vec2d uv0, uv1, uv2;
    if (!card_uv(Vec2d(0., 0.), uv0) || !card_uv(Vec2d(1., 0.), uv1) || !card_uv(Vec2d(0., 1.), uv2))
        return false;

    decal = PaintDecal();
    const bool editing = m_svg_edit_decal >= 0 && size_t(m_svg_edit_decal) < mo->paint_decals.size();
    if (editing)
        decal = mo->paint_decals[size_t(m_svg_edit_decal)];   // keeps eye, lock, through
    decal.name        = m_svg_name.empty() ? decal.name : m_svg_name;
    decal.svg_data    = m_svg_data;
    decal.centre      = { centre.x(), centre.y(), centre.z() };
    decal.dir         = { -fwd.x(), -fwd.y(), -fwd.z() };
    decal.up          = { up.x(), up.y(), up.z() };
    decal.perspective = perspective;
    if (perspective)
        decal.eye_dist = eye_dist;
    const Vec2d c1 = uv1 - uv0, c2 = uv2 - uv0;
    decal.lin = { c1.x(), c2.x(), c1.y(), c2.y() };
    decal.off = { uv0.x(), uv0.y() };
    for (int i = 0; i < 9; ++i)
        decal.warp[size_t(i)] = m_svg_warp(i / 3, i % 3);
    decal.zones.clear();
    for (size_t z = 0; z < m_svg_zones.size(); ++z)
        decal.zones.push_back({ int(z), m_svg_zones[z].rgb, m_svg_zones[z].state, m_svg_zones[z].hidden });
    decal.parts           = m_svg_parts;
    decal.mirror          = m_svg_mirror;
    decal.mirror_readable = m_svg_mirror_readable;
    return true;
}

bool GLGizmoPainterBase::svg_pin_to_view()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    PaintDecal   decal;
    if (mo == nullptr || !svg_build_decal(decal))
        return false;
    const bool editing = m_svg_edit_decal >= 0 && size_t(m_svg_edit_decal) < mo->paint_decals.size();
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), editing ? _u8L("Update SVG decal") : _u8L("Pin SVG decal"), UndoRedo::SnapshotType::GizmoAction);
    if (editing)
        mo->paint_decals[size_t(m_svg_edit_decal)] = std::move(decal);
    else
        mo->paint_decals.push_back(std::move(decal));
    shape_clear();
    m_parent.set_as_dirty();
    return true;
}

// Puts the camera where decal idx looks from, and loads it on screen on the next frame (the
// projection matrix of the new view is only rebuilt when the frame is drawn).
void GLGizmoPainterBase::svg_edit_decal(int idx)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || idx < 0 || size_t(idx) >= mo->paint_decals.size() || mo->paint_decals[size_t(idx)].locked)
        return;
    const PaintDecal &d = mo->paint_decals[size_t(idx)];
    std::vector<Transform3d> trafos, trafos_no_translate;
    const Transform3d inst     = model_part_trafos(trafos, trafos_no_translate);
    Camera           &camera   = wxGetApp().plater()->get_camera();
    const Vec3d centre_w = inst * Vec3d(d.centre[0], d.centre[1], d.centre[2]);
    const Vec3d dir_w    = Vec3d(inst.linear() * Vec3d(d.dir[0], d.dir[1], d.dir[2])).normalized();
    const Vec3d up_w     = Vec3d(inst.linear() * Vec3d(d.up[0], d.up[1], d.up[2])).normalized();
    Vec3d eye_w;
    if (d.perspective && d.eye_dist > 0.) {
        camera.set_type(Camera::EType::Perspective);
        eye_w = inst * d.eye_object();
    } else {
        camera.set_type(Camera::EType::Ortho);
        eye_w = centre_w + dir_w * std::max(camera.get_distance(), 10.);
    }
    camera.look_at(eye_w, centre_w, up_w);
    shape_clear();
    m_svg_pending_edit = idx;
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void GLGizmoPainterBase::svg_finish_pending_edit()
{
    if (m_svg_pending_edit < 0)
        return;
    const int idx = m_svg_pending_edit;
    m_svg_pending_edit = -1;
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || size_t(idx) >= mo->paint_decals.size())
        return;
    const PaintDecal &d = mo->paint_decals[size_t(idx)];
    PaintDecal unwarped = d;   // the placement alone; the deform comes back as m_svg_warp
    unwarped.warp = PaintDecal().warp;
    std::vector<Transform3d> trafos, trafos_no_translate;
    const Transform3d inst   = model_part_trafos(trafos, trafos_no_translate);
    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Matrix4d    vpm    = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const std::array<int, 4> &vp = camera.get_viewport();
    auto to_px = [&](const Vec2d &svg_pt, Vec2d &px) {
        const Vec3d w    = inst * unwarped.svg_to_object(svg_pt);
        const Vec4d clip = vpm * Vec4d(w.x(), w.y(), w.z(), 1.0);
        if (clip.w() <= 0.0)
            return false;
        const Vec3d ndc = Vec3d(clip.head<3>()) / clip.w();
        px = Vec2d(vp[0] + (ndc.x() * 0.5 + 0.5) * vp[2], vp[3] - (vp[1] + (ndc.y() * 0.5 + 0.5) * vp[3]));
        return true;
    };
    Vec2d p0, p1, p2;
    if (!to_px(Vec2d(0., 0.), p0) || !to_px(Vec2d(1., 0.), p1) || !to_px(Vec2d(0., 1.), p2))
        return;

    m_svg_data = d.svg_data;
    m_svg_name = d.name;
    m_svg_zones.clear();
    for (const ColorStitch::StickerZone &zone : ColorStitch::sticker_svg_zones(d.svg_data))
        m_svg_zones.push_back({ zone.rgb, zone.grad, svg_default_state(zone.rgb), false });
    for (const PaintDecalZone &dz : d.zones)
        if (dz.zone >= 0 && size_t(dz.zone) < m_svg_zones.size()) {
            m_svg_zones[size_t(dz.zone)].state  = dz.state;
            m_svg_zones[size_t(dz.zone)].hidden = dz.hidden;
        }
    m_svg_lin << (p1 - p0).x(), (p2 - p0).x(), (p1 - p0).y(), (p2 - p0).y();
    m_svg_off = p0;
    m_svg_warp = d.warp_matrix();
    m_svg_mirror = d.mirror;
    m_svg_mirror_readable = d.mirror_readable;
    m_svg_parts  = d.parts;
    svg_rebuild_masks();
    svg_update_screen_rings();
    m_shape_closed     = true;
    m_shape_scale_pct  = 100.f;
    m_svg_width_pct    = 100.f;
    m_svg_height_pct   = 100.f;
    m_shape_rotate_deg = 0.f;
    m_svg_edit_decal   = idx;
}

const std::vector<GLGizmoPainterBase::SvgRing> &GLGizmoPainterBase::decal_rings(size_t idx)
{
    static const std::vector<SvgRing> none;
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || idx >= mo->paint_decals.size())
        return none;
    const PaintDecal &d = mo->paint_decals[idx];
    std::string key = std::to_string(d.svg_data.size()) + ':' + std::to_string(std::hash<std::string>{}(d.svg_data)) + ':';
    for (const PaintDecalZone &z : d.zones)
        key += std::to_string(z.zone) + (z.hidden ? "h" : "") + '/' + std::to_string(z.state) + ',';
    key += '|';
    for (const PaintDecalPart &p : d.parts)
        key += std::to_string(p.shape) + (p.hidden ? "h" : "") + '/' + std::to_string(p.state) + ',';
    if (m_decal_rings_cache.size() <= idx)
        m_decal_rings_cache.resize(idx + 1);
    DecalRingsCache &cache = m_decal_rings_cache[idx];
    if (cache.key != key) {
        cache.key = key;
        cache.rings.clear();
        PaintDecalGroups groups = paint_decal_groups(d.svg_data, d.zones, d.parts);
        cache.groups = std::move(groups.groups);
        const std::vector<ExPolygons> masks = ColorStitch::svg_group_masks(d.svg_data, Transform3d::Identity(), groups.shape_group, cache.groups.size());
        auto add_ring = [&cache](const Polygon &poly, bool hole, int zone) {
            if (poly.points.size() < 3)
                return;
            SvgRing ring;
            ring.hole = hole;
            ring.zone = zone;
            for (const Point &pt : poly.points)
                ring.pts.push_back(pt.cast<double>() * SCALING_FACTOR);
            cache.rings.push_back(std::move(ring));
        };
        cache.tris.assign(masks.size(), {});
        for (size_t z = 0; z < masks.size(); ++z)
            for (const ExPolygon &ex : masks[z]) {
                add_ring(ex.contour, false, int(z));
                for (const Polygon &hole : ex.holes)
                    add_ring(hole, true, int(z));
                for (const Vec2d &t : triangulate_expolygon_2d(ex))
                    cache.tris[z].push_back(t);
            }
    }
    return cache.rings;
}

Vec3d GLGizmoPainterBase::decal_mirror_centre() const
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return Vec3d::Zero();
    if (mo->paint_symmetry.custom_centre)
        return Vec3d(mo->paint_symmetry.centre[0], mo->paint_symmetry.centre[1], mo->paint_symmetry.centre[2]);
    return mo->raw_mesh_bounding_box().center();
}

// One pass per decal and zone. The cursor is the screen-mask one, fed with a "view-projection"
// built from the decal (PaintDecal::object_to_svg) and a 2x2 viewport, so its "screen" is the
// SVG frame itself: the zone masks are tested in SVG millimetres, wherever the card is.
void GLGizmoPainterBase::decal_apply(const std::vector<size_t> &idxs, bool erase)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr || m_triangle_selectors.empty())
        return;
    std::vector<Transform3d> trafos, trafos_no_translate;
    const Transform3d inst     = model_part_trafos(trafos, trafos_no_translate);
    const Transform3d inst_inv = inst.inverse();
    const std::array<int, 4> viewport = { 0, 0, 2, 2 };

    for (size_t idx : idxs) {
        if (idx >= mo->paint_decals.size())
            continue;
        const std::vector<SvgRing>        &rings  = decal_rings(idx);
        const std::vector<PaintDecalZone> groups = m_decal_rings_cache[idx].groups;   // copied: the cache may move
        // F5: the decal and its mirrored copies, each painted like a decal of its own.
        for (const PaintDecal &d : mo->paint_decals[idx].with_mirror_copies(decal_mirror_centre())) {
            const Matrix4d    M = d.object_to_svg() * inst_inv.matrix();
            // project_mesh_point_to_screen() with viewport (0, 0, 2, 2) gives x = ndc.x + 1, y = 1 - ndc.y.
            Matrix4d vp_matrix = Matrix4d::Zero();
            vp_matrix.row(0) = M.row(0) - M.row(3);
            vp_matrix.row(1) = M.row(3) - M.row(1);
            vp_matrix.row(3) = M.row(3);
            const bool  perspective = d.perspective && d.eye_dist > 0.;
            const Vec3d eye_w       = inst * d.eye_object();
            const Vec3d centre_w    = inst * Vec3d(d.centre[0], d.centre[1], d.centre[2]);
            const Vec3d to_eye_w    = Vec3d(inst.linear() * Vec3d(d.dir[0], d.dir[1], d.dir[2])).normalized();

            for (const PaintDecalZone &zone : groups) {
                if (zone.hidden)
                    continue;
                std::vector<std::vector<Vec2d>> zone_rings;
                std::vector<bool>               is_hole;
                for (const SvgRing &ring : rings)
                    if (ring.zone == zone.zone) {
                        zone_rings.push_back(ring.pts);
                        is_hole.push_back(ring.hole);
                    }
                auto mask = std::make_shared<PolygonMask2D>();
                mask->build(zone_rings, is_hole);
                if (mask->empty())
                    continue;
                const EnforcerBlockerType state = erase ? EnforcerBlockerType::NONE : EnforcerBlockerType(zone.state);
                for (size_t mesh_idx = 0; mesh_idx < trafos.size() && mesh_idx < m_triangle_selectors.size(); ++mesh_idx) {
                    const Transform3d &trafo     = trafos[mesh_idx];
                    const Transform3d  trafo_inv = trafo.inverse();
                    const Vec3f eye_mesh    = Vec3d(trafo_inv * eye_w).cast<float>();
                    const Vec3f centre_mesh = Vec3d(trafo_inv * centre_w).cast<float>();
                    auto cursor = std::make_unique<TriangleSelector::PolygonSetProjectionCursor>(eye_mesh, centre_mesh, vp_matrix, viewport,
                        mask, trafo, this->get_clipping_plane_in_volume_coordinates(trafo));
                    if (d.through)
                        cursor->set_any_facing(true);
                    else
                        cursor->set_visibility_test(make_visibility_test_toward(mesh_idx, trafos, perspective, eye_w, to_eye_w));
                    std::unique_ptr<TriangleSelector::Cursor> base_cursor = std::move(cursor);
                    m_triangle_selectors[mesh_idx]->set_precision_factor(m_precision_factor);
                    m_triangle_selectors[mesh_idx]->select_patch(0, std::move(base_cursor), state, trafos_no_translate[mesh_idx],
                        m_triangle_splitting_enabled, m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);
                    m_triangle_selectors[mesh_idx]->request_update_render_data(true);
                }
            }
        }
    }
}

// NEOTKO_SVGDECAL_TAG — the drawn shape as an SVG path in screen pixels (curves kept as cubics),
// filled with the selected filament's colour. NanoSVG reads px as mm at 96 dpi and the Sticker
// engine centres the drawing and flips Y, so screen = centre + diag(1/k, -1/k) * svg_mm, k = 25.4/96.
bool GLGizmoPainterBase::shape_to_svg_session()
{
    if (!m_shape_closed || m_shape_nodes.size() < 3)
        return false;
    const std::vector<Vec2d> flat = shape_flatten(true);
    if (flat.size() < 3)
        return false;
    const int       state = m_shape_from_state >= 0 ? m_shape_from_state : int(get_left_button_state_type());
    const ColorRGBA col   = get_state_color(state);
    const int  from_decal    = m_shape_from_decal;
    const int  from_mirror   = m_shape_from_mirror;
    const bool from_readable = m_shape_from_readable;
    char hex[8];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", int(col.r() * 255.f + 0.5f), int(col.g() * 255.f + 0.5f), int(col.b() * 255.f + 0.5f));
    std::ostringstream path;
    path.precision(6);
    path << std::fixed;
    const int n = int(m_shape_nodes.size());
    path << "M " << m_shape_nodes[0].p.x() << " " << m_shape_nodes[0].p.y();
    for (int i = 0; i < n; ++i) {
        const ShapeNode &a = m_shape_nodes[i];
        const ShapeNode &b = m_shape_nodes[(i + 1) % n];
        if (a.out.isZero() && b.in.isZero())
            path << " L " << b.p.x() << " " << b.p.y();
        else
            path << " C " << (a.p + a.out).x() << " " << (a.p + a.out).y() << " " << (b.p + b.in).x() << " " << (b.p + b.in).y()
                 << " " << b.p.x() << " " << b.p.y();
    }
    path << " Z";
    const std::string svg = std::string("<svg xmlns=\"http://www.w3.org/2000/svg\"><path fill=\"") + hex + "\" d=\"" + path.str() + "\"/></svg>";

    Vec2d lo = Vec2d::Constant(std::numeric_limits<double>::max());
    Vec2d hi = Vec2d::Constant(std::numeric_limits<double>::lowest());
    for (const Vec2d &p : flat) {
        lo = lo.cwiseMin(p);
        hi = hi.cwiseMax(p);
    }
    const Vec2d screen_centre = 0.5 * (lo + hi);

    const std::vector<ColorStitch::StickerZone> zones = ColorStitch::sticker_svg_zones(svg);
    if (zones.empty())
        return false;
    shape_clear();
    m_svg_data = svg;
    m_svg_name = _u8L("Shape");
    for (const ColorStitch::StickerZone &zone : zones)
        m_svg_zones.push_back({ zone.rgb, zone.grad, state, false });
    if (from_decal >= 0) {   // points of a saved decal: saving updates that decal
        const ModelObject *mo = m_c->selection_info()->model_object();
        if (mo != nullptr && size_t(from_decal) < mo->paint_decals.size())
            m_svg_name = mo->paint_decals[size_t(from_decal)].name;
        m_svg_edit_decal      = from_decal;
        m_svg_mirror          = from_mirror;
        m_svg_mirror_readable = from_readable;
    }
    svg_rebuild_masks();
    if (m_svg_local_rings.empty()) {
        shape_clear();
        return false;
    }
    const double k = 25.4 / 96.0;
    m_svg_lin << 1. / k, 0., 0., -1. / k;
    m_svg_off = screen_centre - Vec2d(m_svg_lin * (0.5 * (m_svg_local_lo + m_svg_local_hi)));
    svg_update_screen_rings();
    m_shape_closed     = true;
    m_shape_scale_pct  = 100.f;
    m_svg_width_pct    = 100.f;
    m_svg_height_pct   = 100.f;
    m_shape_rotate_deg = 0.f;
    m_parent.set_as_dirty();
    return true;
}

bool GLGizmoPainterBase::gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down)
{
    // NEOTKO_SVGDECAL_TAG — F4b: with "Move on object" on, the wheel turns the selected decal and
    // Shift + wheel scales it (5 degrees / 5 % a notch), around the centre of its SVG.
    if (m_decal_move_armed && (action == SLAGizmoEventType::MouseWheelUp || action == SLAGizmoEventType::MouseWheelDown)) {
        ModelObject *mo  = m_c->selection_info()->model_object();
        const int    idx = selected_decal_index();
        if (mo != nullptr && idx >= 0 && size_t(idx) < mo->paint_decals.size() && !mo->paint_decals[size_t(idx)].locked) {
            const double sign = action == SLAGizmoEventType::MouseWheelUp ? 1. : -1.;
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Turn or scale SVG decal"), UndoRedo::SnapshotType::GizmoAction);
            const PaintDecal start = mo->paint_decals[size_t(idx)];
            if (shift_down)
                decal_turn_scale(size_t(idx), start, 0., sign > 0. ? 1.05 : 1. / 1.05);
            else
                decal_turn_scale(size_t(idx), start, 5. * sign, 1.);
            m_parent.set_as_dirty();
            return true;
        }
    }

    // NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F5: Lock view swallows wheel zoom while shaping.
    if (shape_view_locked()
     && (action == SLAGizmoEventType::MouseWheelUp || action == SLAGizmoEventType::MouseWheelDown))
        return true;

    Vec2d _mouse_position = mouse_position;
    if (action == SLAGizmoEventType::MouseWheelUp
     || action == SLAGizmoEventType::MouseWheelDown) {
        if (control_down) {
            //BBS
            if (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::HEIGHT_RANGE) {
                m_cursor_height = action == SLAGizmoEventType::MouseWheelDown ? std::max(m_cursor_height - this->get_cursor_height_step(), this->get_cursor_height_min()) :
                                                                                std::min(m_cursor_height + this->get_cursor_height_step(), this->get_cursor_height_max());
                m_parent.set_as_dirty();
                return true;
            }

            if (m_tool_type == ToolType::BRUSH && (m_cursor_type == TriangleSelector::CursorType::SPHERE || m_cursor_type == TriangleSelector::CursorType::CIRCLE)) {
                m_cursor_radius = action == SLAGizmoEventType::MouseWheelDown ? std::max(m_cursor_radius - this->get_cursor_radius_step(), this->get_cursor_radius_min()) :
                                                                                std::min(m_cursor_radius + this->get_cursor_radius_step(), this->get_cursor_radius_max());
                m_parent.set_as_dirty();
                return true;
            }

            if (m_tool_type == ToolType::BUCKET_FILL || m_tool_type == ToolType::SMART_FILL) {
                m_smart_fill_angle = action == SLAGizmoEventType::MouseWheelDown ? std::max(m_smart_fill_angle - SmartFillAngleStep, SmartFillAngleMin)
                                                                                : std::min(m_smart_fill_angle + SmartFillAngleStep, SmartFillAngleMax);
                m_parent.set_as_dirty();
                if (m_rr.mesh_id != -1) {
                    const Selection     &selection                 = m_parent.get_selection();
                    const ModelObject   *mo                        = m_c->selection_info()->model_object();
                    const ModelInstance *mi                        = mo->instances[selection.get_instance_idx()];
                    const Transform3d   trafo_matrix_not_translate = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
                        mi->get_assemble_transformation().get_matrix_no_offset() * mo->volumes[m_rr.mesh_id]->get_matrix_no_offset() :
                        mi->get_transformation().get_matrix_no_offset() * mo->volumes[m_rr.mesh_id]->get_matrix_no_offset();
                    const Transform3d   trafo_matrix = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
                        mi->get_assemble_transformation().get_matrix() * mo->volumes[m_rr.mesh_id]->get_matrix() :
                        mi->get_transformation().get_matrix() * mo->volumes[m_rr.mesh_id]->get_matrix();
                    m_triangle_selectors[m_rr.mesh_id]->seed_fill_select_triangles(m_rr.hit, int(m_rr.facet), trafo_matrix_not_translate, this->get_clipping_plane_in_volume_coordinates(trafo_matrix), m_smart_fill_angle,
                                                                                   m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f, true);
                    m_triangle_selectors[m_rr.mesh_id]->request_update_render_data();
                    m_seed_fill_last_mesh_id = m_rr.mesh_id;
                }
                return true;
            }

            if (m_tool_type == ToolType::GAP_FILL) {
                TriangleSelectorPatch::gap_area = action == SLAGizmoEventType::MouseWheelDown ?
                    std::max(TriangleSelectorPatch::gap_area - TriangleSelectorPatch::GapAreaStep, TriangleSelectorPatch::GapAreaMin) :
                    std::min(TriangleSelectorPatch::gap_area + TriangleSelectorPatch::GapAreaStep, TriangleSelectorPatch::GapAreaMax);
                m_parent.set_as_dirty();
                return true;
            }
        }
        else if (alt_down) {
            // BBS
            double pos = m_c->object_clipper()->get_position();
            pos = action == SLAGizmoEventType::MouseWheelDown
                      ? std::max(0., pos - 0.01)
                      : std::min(1., pos + 0.01);
            m_c->object_clipper()->set_position_by_ratio(pos, true);
            return true;
        }
    }

    if (action == SLAGizmoEventType::ResetClippingPlane) {
        m_c->object_clipper()->set_position_by_ratio(-1., false);
        return true;
    }

    // NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F4, Sesion A: RECTANGLE doesn't paint on every
    // Dragging event like BRUSH/BUCKET_FILL/SMART_FILL do below — it only tracks the
    // screen-space rectangle corners while the button is held. The mask is applied once, on
    // release, in the LeftUp/RightUp handling further down (apply_rectangle_mask()).
    if (m_tool_type == ToolType::RECTANGLE
     && (action == SLAGizmoEventType::LeftDown || action == SLAGizmoEventType::RightDown
      || (action == SLAGizmoEventType::Dragging && m_button_down != Button::None))) {
        if (m_triangle_selectors.empty())
            return false;

        if (action == SLAGizmoEventType::LeftDown || action == SLAGizmoEventType::RightDown) {
            m_button_down       = (action == SLAGizmoEventType::LeftDown) ? Button::Left : Button::Right;
            m_rect_start_corner = mouse_position;
            m_rect_dragging     = true;
        }
        m_rect_end_corner = mouse_position;
        m_parent.set_as_dirty();
        return true;
    }

    // NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F5 (commit 4): POLYGON is now a shape of nodes
    // with optional Bezier handles. Like before it claims every Dragging event between a LeftDown
    // and its LeftUp, so incidental mouse movement while clicking can't orbit the camera (s201).
    // LeftDown hit-tests, in priority order: a handle (drag it, Alt breaks the mirror), a node
    // (first node with 3+ placed closes the shape, Alt toggles corner/curve, otherwise drag it),
    // a segment of a closed shape (insert a node there without changing the curve), and finally
    // empty space (append a node to an open shape; dragging right away pulls out its handles).
    // Closing does NOT paint any more: painting happens on the panel's Apply (shape_apply()).
    // RightDown cancels the whole shape.
    if (m_tool_type == ToolType::POLYGON
     && (action == SLAGizmoEventType::LeftDown || action == SLAGizmoEventType::RightDown
      || action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::RightUp
      || (action == SLAGizmoEventType::Dragging && m_button_down != Button::None))) {
        if (m_triangle_selectors.empty())
            return false;

        // NEOTKO_SVGDECAL_TAG — right button: it used to cancel the shape on press, so starting a
        // pan with it wiped the shape (and the SVG). Now the press is left to the camera, and only
        // a click (released where it was pressed) cancels a drawn shape; an SVG goes with Cancel.
        if (action == SLAGizmoEventType::RightDown) {
            m_shape_right_down_pos = mouse_position;
            return false;
        }
        if (action == SLAGizmoEventType::RightUp) {
            if (m_svg_rings.empty() && (mouse_position - m_shape_right_down_pos).norm() < 3.0) {
                shape_clear();
                m_parent.set_as_dirty();
            }
            return true;
        }

        if (action == SLAGizmoEventType::LeftUp && m_decal_drag >= 0) {
            m_decal_drag  = -1;
            m_button_down = Button::None;
            m_parent.set_as_dirty();
            return true;
        }
        if (action == SLAGizmoEventType::Dragging && m_decal_drag >= 0) {
            // F4b: turn the decal around the object's centre so the grabbed point goes under the mouse.
            ModelObject *mo = m_c->selection_info()->model_object();
            Vec3d hit;
            if (mo != nullptr && size_t(m_decal_drag) < mo->paint_decals.size() && mouse_on_object(mouse_position, hit)) {
                const Vec3d pivot = mo->raw_mesh_bounding_box().center();
                const Vec3d to    = hit - pivot;
                if (to.norm() > 1e-6 && m_decal_drag_anchor.norm() > 1e-6) {
                    const Eigen::Matrix3d R = Eigen::Quaterniond::FromTwoVectors(m_decal_drag_anchor, to).toRotationMatrix();
                    PaintDecal d = m_decal_drag_start;
                    const Vec3d c  = pivot + R * (Vec3d(d.centre[0], d.centre[1], d.centre[2]) - pivot);
                    const Vec3d dn = R * Vec3d(d.dir[0], d.dir[1], d.dir[2]);
                    const Vec3d up = R * Vec3d(d.up[0], d.up[1], d.up[2]);
                    d.centre = { c.x(), c.y(), c.z() };
                    d.dir    = { dn.x(), dn.y(), dn.z() };
                    d.up     = { up.x(), up.y(), up.z() };
                    mo->paint_decals[size_t(m_decal_drag)] = d;
                }
            }
            m_parent.set_as_dirty();
            return true;
        }
        if (action == SLAGizmoEventType::LeftUp) {
            if (m_button_down == Button::None)
                return false;   // the press went to the camera
            m_shape_drag     = ShapeDrag::None;
            m_shape_drag_idx = -1;
            m_button_down    = Button::None;
            m_parent.set_as_dirty();
            return true;
        }

        if (action == SLAGizmoEventType::Dragging) {
            if (m_shape_drag == ShapeDrag::Corner) {
                svg_set_corner(m_shape_drag_idx, mouse_position);   // a fold is refused, the corner stays
                m_parent.set_as_dirty();
                return true;
            }
            if (m_shape_drag == ShapeDrag::Move) {
                // Whole shape: every node and handle moves with the mouse.
                shape_move(mouse_position - m_shape_move_last);
                m_shape_move_last = mouse_position;
                m_parent.set_as_dirty();
                return true;
            }
            if (m_shape_drag_idx >= 0 && m_shape_drag_idx < int(m_shape_nodes.size())) {
                ShapeNode &node = m_shape_nodes[m_shape_drag_idx];
                switch (m_shape_drag) {
                case ShapeDrag::NewNodeHandle: {
                    const Vec2d d = mouse_position - node.p;
                    if (node.is_curved() || d.norm() >= ShapeCurveDragThresholdPx) {
                        node.out    = d;
                        node.in     = -d;
                        node.smooth = true;
                    }
                    break;
                }
                case ShapeDrag::Node: node.p = mouse_position; break;
                case ShapeDrag::HandleOut:
                case ShapeDrag::HandleIn: {
                    const bool out = m_shape_drag == ShapeDrag::HandleOut;
                    Vec2d &dragged = out ? node.out : node.in;
                    Vec2d &other   = out ? node.in : node.out;
                    dragged = mouse_position - node.p;
                    if (alt_down)
                        node.smooth = false;
                    if (node.smooth && dragged.norm() > EPSILON) {
                        // Keep the opposite handle's own length, only mirror its direction.
                        const double len = other.norm() > EPSILON ? other.norm() : dragged.norm();
                        other = -dragged.normalized() * len;
                    }
                    break;
                }
                default: break;
                }
            }
            m_parent.set_as_dirty();
            return true;
        }

        // F4b: "Move on object": a press on the object grabs the selected decal there.
        if (m_decal_move_armed) {
            ModelObject *mo  = m_c->selection_info()->model_object();
            const int    idx = selected_decal_index();
            Vec3d        hit;
            if (mo != nullptr && idx >= 0 && size_t(idx) < mo->paint_decals.size() && !mo->paint_decals[size_t(idx)].locked
             && mouse_on_object(mouse_position, hit)) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Move SVG decal"), UndoRedo::SnapshotType::GizmoAction);
                m_decal_drag        = idx;
                m_decal_drag_start  = mo->paint_decals[size_t(idx)];
                m_decal_drag_anchor = hit - mo->raw_mesh_bounding_box().center();
                m_button_down       = Button::Left;
                m_parent.set_as_dirty();
                return true;
            }
            m_button_down = Button::None;
            return false;   // off the object: the camera's
        }

        // F5: "Pick centre" takes this click: the mirror centre goes where it hits the object.
        if (m_mirror_pick_armed) {
            m_mirror_pick_armed = false;
            ModelObject *mo = m_c->selection_info()->model_object();
            std::vector<Transform3d> trafos, trafos_no_translate;
            if (mo != nullptr) {
                const Transform3d inst = model_part_trafos(trafos, trafos_no_translate);
                update_raycast_cache(mouse_position, wxGetApp().plater()->get_camera(), trafos);
                if (m_rr.mesh_id >= 0 && size_t(m_rr.mesh_id) < trafos.size()) {
                    const Vec3d p = inst.inverse() * (trafos[size_t(m_rr.mesh_id)] * m_rr.hit.cast<double>());
                    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Mirror centre"), UndoRedo::SnapshotType::GizmoAction);
                    mo->paint_symmetry.custom_centre = true;
                    mo->paint_symmetry.centre        = { p.x(), p.y(), p.z() };
                }
            }
            m_parent.set_as_dirty();
            return true;
        }

        // LeftDown from here on: claim the button for the whole click gesture.
        m_button_down    = Button::Left;
        m_shape_drag     = ShapeDrag::None;
        m_shape_drag_idx = -1;
        m_shape_lost_to_camera = false;
        const double r = double(PolygonCloseRadiusPx);

        // SVG shapes: drag inside moves the whole set, no point editing.
        if (!m_svg_rings.empty()) {
            // Parts: a click picks the topmost part under it (Shift adds or removes it).
            if (m_svg_pick_parts) {
                const int k = svg_pick_part(mouse_position);
                if (k < 0 && !shape_contains(mouse_position)) {
                    m_button_down = Button::None;   // outside the SVG: orbit the camera
                    return false;
                }
                if (!shift_down)
                    m_svg_part_sel.clear();
                if (k >= 0 && shift_down && m_svg_part_sel.count(k))
                    m_svg_part_sel.erase(k);
                else if (k >= 0)
                    m_svg_part_sel.insert(k);
                m_parent.set_as_dirty();
                return true;
            }
            // F4: a corner handle first, then the box.
            if (m_svg_deform) {
                const std::array<Vec2d, 4> box = svg_screen_box();
                for (int k = 0; k < 4 && m_shape_drag == ShapeDrag::None; ++k)
                    if ((box[size_t(k)] - mouse_position).norm() <= r) {
                        m_shape_drag     = ShapeDrag::Corner;
                        m_shape_drag_idx = k;
                    }
                if (m_shape_drag == ShapeDrag::Corner) {
                    m_parent.set_as_dirty();
                    return true;
                }
            }
            if (shape_contains(mouse_position)) {
                m_shape_drag      = ShapeDrag::Move;
                m_shape_move_last = mouse_position;
            } else if (shape_draw_needs_arming()) {
                m_button_down = Button::None;   // outside the SVG: orbit the camera
                return false;
            }
            m_parent.set_as_dirty();
            return true;
        }

        // Vector mode: no shape until "Draw shape"; the click is the camera's.
        if (m_shape_nodes.empty() && shape_draw_needs_arming() && !m_shape_draw_armed) {
            m_button_down = Button::None;
            return false;
        }

        // 1) Handles.
        for (int i = 0; i < int(m_shape_nodes.size()); ++i) {
            ShapeNode &node = m_shape_nodes[i];
            if (!node.is_curved())
                continue;
            if ((node.p + node.out - mouse_position).norm() <= r) { m_shape_drag = ShapeDrag::HandleOut; m_shape_drag_idx = i; break; }
            if ((node.p + node.in  - mouse_position).norm() <= r) { m_shape_drag = ShapeDrag::HandleIn;  m_shape_drag_idx = i; break; }
        }
        if (m_shape_drag != ShapeDrag::None) {
            if (alt_down)
                m_shape_nodes[m_shape_drag_idx].smooth = false;
            m_parent.set_as_dirty();
            return true;
        }

        // 2) Nodes.
        int hit_node = -1;
        for (int i = 0; i < int(m_shape_nodes.size()); ++i)
            if ((m_shape_nodes[i].p - mouse_position).norm() <= r) {
                hit_node = i;
                break;
            }
        if (hit_node >= 0) {
            ShapeNode &node = m_shape_nodes[hit_node];
            if (hit_node == 0 && !m_shape_closed && m_shape_nodes.size() >= 3) {
                m_shape_closed = true;
            } else if (alt_down) {
                if (node.is_curved()) {
                    node.in = node.out = Vec2d::Zero();
                    node.smooth = false;
                } else {
                    // Corner -> smooth curve: handles along the neighbours' direction.
                    const int   n    = int(m_shape_nodes.size());
                    const Vec2d prev = m_shape_nodes[(hit_node + n - 1) % n].p;
                    const Vec2d next = m_shape_nodes[(hit_node + 1) % n].p;
                    Vec2d dir = next - prev;
                    if (dir.norm() < EPSILON)
                        dir = Vec2d(4. * r, 0.);
                    node.out    = 0.25 * dir;
                    node.in     = -node.out;
                    node.smooth = true;
                }
            } else {
                m_shape_drag     = ShapeDrag::Node;
                m_shape_drag_idx = hit_node;
            }
            m_parent.set_as_dirty();
            return true;
        }

        // 3) Segment of a closed shape: split it there (De Casteljau keeps the curve as is).
        if (m_shape_closed) {
            const int n = int(m_shape_nodes.size());
            int    best_seg = -1;
            double best_t   = 0.;
            double best_d   = r;
            for (int i = 0; i < n; ++i) {
                const ShapeNode &a = m_shape_nodes[i];
                const ShapeNode &b = m_shape_nodes[(i + 1) % n];
                const std::array<Vec2d, 4> c = { a.p, a.p + a.out, b.p + b.in, b.p };
                for (int k = 1; k < 64; ++k) {
                    const double t = double(k) / 64.;
                    const double d = (shape_bezier_point(c, t) - mouse_position).norm();
                    if (d < best_d) { best_d = d; best_seg = i; best_t = t; }
                }
            }
            if (best_seg >= 0) {
                ShapeNode &a = m_shape_nodes[best_seg];
                ShapeNode &b = m_shape_nodes[(best_seg + 1) % n];
                const double t = best_t;
                const Vec2d p0 = a.p, p1 = a.p + a.out, p2 = b.p + b.in, p3 = b.p;
                const Vec2d p01 = p0 + t * (p1 - p0), p12 = p1 + t * (p2 - p1), p23 = p2 + t * (p3 - p2);
                const Vec2d p012 = p01 + t * (p12 - p01), p123 = p12 + t * (p23 - p12);
                const Vec2d mid = p012 + t * (p123 - p012);
                ShapeNode nn;
                nn.p = mid;
                const bool curved = a.is_curved() || b.is_curved();
                if (curved) {
                    a.out     = p01 - p0;
                    b.in      = p23 - p3;
                    nn.in     = p012 - mid;
                    nn.out    = p123 - mid;
                    nn.smooth = true;
                }
                m_shape_nodes.insert(m_shape_nodes.begin() + best_seg + 1, nn);
                m_shape_drag     = ShapeDrag::Node;
                m_shape_drag_idx = best_seg + 1;
                m_parent.set_as_dirty();
                return true;
            }
            // Inside a closed shape (not on its outline): drag moves the whole shape.
            if (shape_contains(mouse_position)) {
                m_shape_drag      = ShapeDrag::Move;
                m_shape_move_last = mouse_position;
            } else if (shape_draw_needs_arming()) {
                m_button_down = Button::None;   // away from the shape: orbit the camera
                return false;
            }
            m_parent.set_as_dirty();
            return true;
        }

        // 4) Empty space on an open shape: append a node; dragging now pulls out its handles.
        if (m_shape_nodes.empty()) {
            const Camera &camera = wxGetApp().plater()->get_camera();
            m_shape_view_projection = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
        }
        ShapeNode nn;
        nn.p = mouse_position;
        m_shape_nodes.push_back(nn);
        m_shape_drag     = ShapeDrag::NewNodeHandle;
        m_shape_drag_idx = int(m_shape_nodes.size()) - 1;
        m_parent.set_as_dirty();
        return true;
    }

    if (action == SLAGizmoEventType::LeftDown
     || action == SLAGizmoEventType::RightDown
    || (action == SLAGizmoEventType::Dragging && m_button_down != Button::None)) {

        if (m_triangle_selectors.empty())
            return false;

        // NEOTKO_SVGDECAL_TAG — F5b: "Pick on object" in Brush mode takes this click.
        if (action == SLAGizmoEventType::LeftDown && m_mirror_pick_armed) {
            m_mirror_pick_armed = false;
            ModelObject *pick_mo = m_c->selection_info()->model_object();
            Vec3d        p;
            if (pick_mo != nullptr && mouse_on_object(mouse_position, p)) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Mirror centre"), UndoRedo::SnapshotType::GizmoAction);
                pick_mo->paint_symmetry.custom_centre = true;
                pick_mo->paint_symmetry.centre        = { p.x(), p.y(), p.z() };
            }
            m_parent.set_as_dirty();
            return true;
        }

        EnforcerBlockerType new_state = EnforcerBlockerType::NONE;
        // BBS
        if (action == SLAGizmoEventType::Dragging) {
            if (m_button_down == Button::Right && this->get_right_button_state_type() == EnforcerBlockerType(-1))
                return false;
        }
        else {
            if (action == SLAGizmoEventType::RightDown && this->get_right_button_state_type() == EnforcerBlockerType(-1))
                return false;
        }

        if (! shift_down) {
            if (action == SLAGizmoEventType::Dragging)
                new_state = m_button_down == Button::Left ? this->get_left_button_state_type() : this->get_right_button_state_type();
            else
                new_state = action == SLAGizmoEventType::LeftDown ? this->get_left_button_state_type() : this->get_right_button_state_type();
        }

        const Camera        &camera                      = wxGetApp().plater()->get_camera();
        const Selection     &selection                   = m_parent.get_selection();
        const ModelObject   *mo                          = m_c->selection_info()->model_object();
        const ModelInstance *mi                          = mo->instances[selection.get_instance_idx()];
        Transform3d   instance_trafo = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
            mi->get_assemble_transformation().get_matrix() :
            mi->get_transformation().get_matrix();
        Transform3d   instance_trafo_not_translate = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
            mi->get_assemble_transformation().get_matrix_no_offset() :
            mi->get_transformation().get_matrix_no_offset();
        std::vector<const ModelVolume*> part_volumes;

        // Precalculate transformations of individual meshes.
        std::vector<Transform3d> trafo_matrices;
        std::vector<Transform3d> trafo_matrices_not_translate;
        for (const ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
                    Transform3d temp = instance_trafo * mv->get_matrix();
                    temp.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) + mi->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
                    trafo_matrices.emplace_back(temp);
                }
                else {
                    trafo_matrices.emplace_back(instance_trafo* mv->get_matrix());
                }
                trafo_matrices_not_translate.emplace_back(instance_trafo_not_translate * mv->get_matrix_no_offset());
                part_volumes.push_back(mv);
            }

        // BBS
        if (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::HEIGHT_RANGE)
        {
            std::vector<ProjectedHeightRange> projected_height_range_by_mesh = get_projected_height_range(_mouse_position, 1., part_volumes, trafo_matrices);
            m_last_mouse_click = Vec2d::Zero();

            for (int i = 0; i < projected_height_range_by_mesh.size(); i++) {
                const ProjectedHeightRange& phr = projected_height_range_by_mesh[i];
                int mesh_idx = phr.mesh_idx;

                // The mouse button click detection is enabled when there is a valid hit.
                // Missing the object entirely
                // shall not capture the mouse.
                const bool dragging_while_painting = (action == SLAGizmoEventType::Dragging && m_button_down != Button::None);
                if (mesh_idx != -1 && m_button_down == Button::None)
                    m_button_down = ((action == SLAGizmoEventType::LeftDown) ? Button::Left : Button::Right);

                const Transform3d& trafo_matrix = trafo_matrices[mesh_idx];
                const Transform3d& trafo_matrix_not_translate = trafo_matrices_not_translate[mesh_idx];

                // Calculate direction from camera to the hit (in mesh coords):
                Vec3f camera_pos = (trafo_matrix.inverse() * camera.get_position()).cast<float>();
                const TriangleSelector::ClippingPlane& clp = this->get_clipping_plane_in_volume_coordinates(trafo_matrix);

                std::unique_ptr<TriangleSelector::Cursor> cursor = TriangleSelector::SinglePointCursor::cursor_factory(phr.z_world,
                    camera_pos, m_cursor_height, trafo_matrix, clp);
                m_triangle_selectors[mesh_idx]->set_precision_factor(m_precision_factor);
                m_triangle_selectors[mesh_idx]->select_patch(int(phr.first_facet_idx), std::move(cursor), new_state, trafo_matrix_not_translate,
                    m_triangle_splitting_enabled, m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);

                m_triangle_selectors[mesh_idx]->request_update_render_data(true);
                m_last_mouse_click = _mouse_position;
            }

            return true;
        }

        if (action == SLAGizmoEventType::Dragging && m_tool_type == ToolType::BRUSH ){
            if(m_vertical_only)
                _mouse_position.x() = m_last_mouse_click.x();
            else if(m_horizontal_only)
                _mouse_position.y() = m_last_mouse_click.y();
        }
        
        std::vector<std::vector<ProjectedMousePosition>> projected_mouse_positions_by_mesh = get_projected_mouse_positions(_mouse_position, 1., trafo_matrices);
        m_last_mouse_click = Vec2d::Zero(); // only actual hits should be saved

        for (const std::vector<ProjectedMousePosition> &projected_mouse_positions : projected_mouse_positions_by_mesh) {
            assert(!projected_mouse_positions.empty());
            const int  mesh_idx                = projected_mouse_positions.front().mesh_idx;
            const bool dragging_while_painting = (action == SLAGizmoEventType::Dragging && m_button_down != Button::None);

            // The mouse button click detection is enabled when there is a valid hit.
            // Missing the object entirely
            // shall not capture the mouse.
            if (mesh_idx != -1)
                if (m_button_down == Button::None)
                    m_button_down = ((action == SLAGizmoEventType::LeftDown) ? Button::Left : Button::Right);

            // In case we have no valid hit, we can return. The event will be stopped when
            // dragging while painting (to prevent scene rotations and moving the object)
            if (mesh_idx == -1)
                return dragging_while_painting;

            const Transform3d &trafo_matrix               = trafo_matrices[mesh_idx];
            const Transform3d &trafo_matrix_not_translate = trafo_matrices_not_translate[mesh_idx];

            // Calculate direction from camera to the hit (in mesh coords):
            Vec3f camera_pos = (trafo_matrix.inverse() * camera.get_position()).cast<float>();

            assert(mesh_idx < int(m_triangle_selectors.size()));
            const TriangleSelector::ClippingPlane &clp = this->get_clipping_plane_in_volume_coordinates(trafo_matrix);
            if (m_tool_type == ToolType::SMART_FILL || m_tool_type == ToolType::BUCKET_FILL || (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER)) {
                for(const ProjectedMousePosition &projected_mouse_position : projected_mouse_positions) {
                    assert(projected_mouse_position.mesh_idx == mesh_idx);
                    const Vec3f mesh_hit = projected_mouse_position.mesh_hit;
                    const int facet_idx = int(projected_mouse_position.facet_idx);
                    m_triangle_selectors[mesh_idx]->seed_fill_apply_on_triangles(new_state);
                    if (m_tool_type == ToolType::SMART_FILL)
                        m_triangle_selectors[mesh_idx]->seed_fill_select_triangles(mesh_hit, facet_idx, trafo_matrix_not_translate, clp, m_smart_fill_angle,
                                                                                       m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f, true);
                    else if (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER)
                        // BBS: add infill_angle parameter
                        m_triangle_selectors[mesh_idx]->bucket_fill_select_triangles(mesh_hit, facet_idx, clp, -1.f, false, true);
                    else if (m_tool_type == ToolType::BUCKET_FILL)
                        // BBS: add infill_angle parameter
                        m_triangle_selectors[mesh_idx]->bucket_fill_select_triangles(mesh_hit, facet_idx, clp, m_smart_fill_angle, true, true);

                    m_seed_fill_last_mesh_id = -1;
                }
            } else if (m_tool_type == ToolType::BRUSH) {
                assert(m_cursor_type == TriangleSelector::CursorType::CIRCLE || m_cursor_type == TriangleSelector::CursorType::SPHERE);

                if (projected_mouse_positions.size() == 1) {
                    const ProjectedMousePosition             &first_position = projected_mouse_positions.front();
                    std::unique_ptr<TriangleSelector::Cursor> cursor         = TriangleSelector::SinglePointCursor::cursor_factory(first_position.mesh_hit,
                                                                                                                                   camera_pos, m_cursor_radius,
                                                                                                                                   m_cursor_type, trafo_matrix, clp);
                    m_triangle_selectors[mesh_idx]->set_precision_factor(m_precision_factor);
                    m_triangle_selectors[mesh_idx]->select_patch(int(first_position.facet_idx), std::move(cursor), new_state, trafo_matrix_not_translate,
                                                                 m_triangle_splitting_enabled, m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);
                } else {
                    for (auto first_position_it = projected_mouse_positions.cbegin(); first_position_it != projected_mouse_positions.cend() - 1; ++first_position_it) {
                        auto second_position_it = first_position_it + 1;
                        std::unique_ptr<TriangleSelector::Cursor> cursor = TriangleSelector::DoublePointCursor::cursor_factory(first_position_it->mesh_hit, second_position_it->mesh_hit, camera_pos, m_cursor_radius, m_cursor_type, trafo_matrix, clp);
                        m_triangle_selectors[mesh_idx]->set_precision_factor(m_precision_factor);
                        m_triangle_selectors[mesh_idx]->select_patch(int(first_position_it->facet_idx), std::move(cursor), new_state, trafo_matrix_not_translate, m_triangle_splitting_enabled, m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);
                    }
                }
                // NEOTKO_SVGDECAL_TAG — F5b: the same stroke on the mirrored side(s).
                brush_paint_mirrored(projected_mouse_positions, trafo_matrix, new_state, trafo_matrices, trafo_matrices_not_translate);
            }

            m_triangle_selectors[mesh_idx]->request_update_render_data(true);

            m_last_mouse_click = _mouse_position;
        }

        return true;
    }

    if (action == SLAGizmoEventType::Moving && (m_tool_type == ToolType::SMART_FILL || m_tool_type == ToolType::BUCKET_FILL || (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER))) {
        if (m_triangle_selectors.empty())
            return false;

        const Camera        &camera                       = wxGetApp().plater()->get_camera();
        const Selection     &selection                    = m_parent.get_selection();
        const ModelObject   *mo                           = m_c->selection_info()->model_object();
        const ModelInstance *mi                           = mo->instances[selection.get_instance_idx()];
        const Transform3d    instance_trafo = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
            mi->get_assemble_transformation().get_matrix() :
            mi->get_transformation().get_matrix();
        const Transform3d    instance_trafo_not_translate = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
            mi->get_assemble_transformation().get_matrix_no_offset() :
            mi->get_transformation().get_matrix_no_offset();

        // Precalculate transformations of individual meshes.
        std::vector<Transform3d> trafo_matrices;
        std::vector<Transform3d> trafo_matrices_not_translate;
        for (const ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView) {
                    Transform3d temp = instance_trafo * mv->get_matrix();
                    temp.translate(mv->get_transformation().get_offset() * (GLVolume::explosion_ratio - 1.0) + mi->get_offset_to_assembly() * (GLVolume::explosion_ratio - 1.0));
                    trafo_matrices.emplace_back(temp);
                }
                else {
                    trafo_matrices.emplace_back(instance_trafo * mv->get_matrix());
                }
                trafo_matrices_not_translate.emplace_back(instance_trafo_not_translate * mv->get_matrix_no_offset());
            }

        // Now "click" into all the prepared points and spill paint around them.
        update_raycast_cache(_mouse_position, camera, trafo_matrices);

        auto seed_fill_unselect_all = [this]() {
            for (auto &triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        };

        if (m_rr.mesh_id == -1) {
            // Clean selected by seed fill for all triangles in all meshes when a mouse isn't pointing on any mesh.
            seed_fill_unselect_all();
            m_seed_fill_last_mesh_id = -1;

            // In case we have no valid hit, we can return.
            return false;
        }

        // The mouse moved from one object's volume to another one. So it is needed to unselect all triangles selected by seed fill.
        if(m_rr.mesh_id != m_seed_fill_last_mesh_id)
            seed_fill_unselect_all();

        const Transform3d &trafo_matrix = trafo_matrices[m_rr.mesh_id];
        const Transform3d &trafo_matrix_not_translate = trafo_matrices_not_translate[m_rr.mesh_id];

        assert(m_rr.mesh_id < int(m_triangle_selectors.size()));
        const TriangleSelector::ClippingPlane &clp = this->get_clipping_plane_in_volume_coordinates(trafo_matrix);
        if (m_tool_type == ToolType::SMART_FILL)
            m_triangle_selectors[m_rr.mesh_id]->seed_fill_select_triangles(m_rr.hit, int(m_rr.facet), trafo_matrix_not_translate, clp, m_smart_fill_angle,
                                                                           m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);
        else if (m_tool_type == ToolType::BRUSH && m_cursor_type == TriangleSelector::CursorType::POINTER)
            // BBS: add infill_angle parameter
            m_triangle_selectors[m_rr.mesh_id]->bucket_fill_select_triangles(m_rr.hit, int(m_rr.facet), clp, -1.f, false);
        else if (m_tool_type == ToolType::BUCKET_FILL)
            // BBS: add infill_angle parameter
            m_triangle_selectors[m_rr.mesh_id]->bucket_fill_select_triangles(m_rr.hit, int(m_rr.facet), clp, m_smart_fill_angle, true);
        m_triangle_selectors[m_rr.mesh_id]->request_update_render_data();
        m_seed_fill_last_mesh_id = m_rr.mesh_id;
        return true;
    }

    if ((action == SLAGizmoEventType::LeftUp || action == SLAGizmoEventType::RightUp)
      && m_button_down != Button::None) {

        // NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F4, Sesion A: this is where RECTANGLE
        // actually applies its mask (BRUSH/BUCKET_FILL/SMART_FILL already mutated
        // m_triangle_selectors during the preceding Dragging events; this just brings
        // RECTANGLE up to the same point before the shared snapshot/update_model_object below).
        if (m_tool_type == ToolType::RECTANGLE && m_rect_dragging) {
            EnforcerBlockerType new_state = shift_down ? EnforcerBlockerType::NONE
                : (m_button_down == Button::Left ? this->get_left_button_state_type() : this->get_right_button_state_type());
            apply_rectangle_mask(new_state);
            m_rect_dragging = false;
        }

        // Take snapshot and update ModelVolume data.
        wxString action_name = this->handle_snapshot_action_name(shift_down, m_button_down);
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), std::string(action_name.ToUTF8().data()), UndoRedo::SnapshotType::GizmoAction);
        update_model_object();

        m_button_down = Button::None;
        m_last_mouse_click = Vec2d::Zero();
        return true;
    }

    return false;
}

// NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F5 (commit 4). Fix: the masks painted "through" the
// object. is_facet_visible() only rejects back faces, so front faces hidden behind another part
// of the object (the inside of a ring, a wall behind a handle) were painted too. Now each sampled
// point casts a ray towards the camera (to its position in perspective, along -forward in ortho)
// against every model part; any hit in between means it's hidden. Hits on the clipped-away side
// of the clipping plane don't count. Cached per point: the selector asks for the same vertices
// over and over while subdividing.
TriangleSelector::Cursor::VisibilityTest GLGizmoPainterBase::make_visibility_test(size_t mesh_idx, const std::vector<Transform3d> &trafo_matrices) const
{
    const Camera &camera = wxGetApp().plater()->get_camera();
    return make_visibility_test_toward(mesh_idx, trafo_matrices, camera.get_type() == Camera::EType::Perspective,
                                       camera.get_position(), -camera.get_dir_forward());
}

// NEOTKO_SVGDECAL_TAG — F3: body of make_visibility_test(), with the viewer as parameters so a
// decal can test visibility from its own card instead of from the camera.
TriangleSelector::Cursor::VisibilityTest GLGizmoPainterBase::make_visibility_test_toward(size_t mesh_idx, const std::vector<Transform3d> &trafo_matrices,
    bool perspective, const Vec3d &eye_world, const Vec3d &to_eye_world) const
{
    const Vec3d camera_pos = eye_world;
    const Vec3d to_camera  = to_eye_world.normalized();

    std::vector<const MeshRaycaster*> raycasters = m_c->raycaster()->raycasters();
    std::vector<Transform3d>          inverses;
    inverses.reserve(trafo_matrices.size());
    for (const Transform3d &t : trafo_matrices)
        inverses.emplace_back(t.inverse());

    struct KeyHash {
        size_t operator()(const std::array<float, 3> &k) const {
            return std::hash<float>()(k[0]) ^ (std::hash<float>()(k[1]) << 1) ^ (std::hash<float>()(k[2]) << 2);
        }
    };
    auto cache = std::make_shared<std::unordered_map<std::array<float, 3>, bool, KeyHash>>();

    return [this, mesh_idx, perspective, camera_pos, to_camera, raycasters, trafo_matrices, inverses, cache](const Vec3f &point) -> bool {
        const std::array<float, 3> key = { point.x(), point.y(), point.z() };
        if (auto it = cache->find(key); it != cache->end())
            return it->second;

        // Start a hair off the surface so the point's own facets don't count as occluders.
        constexpr double offset = 0.01; // mm
        const Vec3d world    = trafo_matrices[mesh_idx] * point.cast<double>();
        Vec3d       dir      = perspective ? Vec3d(camera_pos - world) : to_camera;
        const double max_dist = perspective ? dir.norm() : std::numeric_limits<double>::max();
        dir.normalize();
        const Vec3d origin = world + dir * offset;

        bool seen = true;
        for (size_t k = 0; seen && k < raycasters.size() && k < trafo_matrices.size(); ++k) {
            if (raycasters[k] == nullptr)
                continue;
            const Vec3d origin_m = inverses[k] * origin;
            const Vec3d dir_m    = (inverses[k].linear() * dir).normalized();
            for (const AABBMesh::hit_result &hit : raycasters[k]->get_aabb_mesh().query_ray_hits(origin_m, dir_m)) {
                if (!hit.is_hit())
                    continue;
                const Vec3d hit_world = trafo_matrices[k] * hit.position();
                if ((hit_world - origin).norm() >= max_dist)
                    continue;
                if (this->is_mesh_point_clipped(hit.position(), trafo_matrices[k]))
                    continue;
                seen = false;
                break;
            }
        }
        cache->emplace(key, seen);
        return seen;
    };
}

// NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F4, Sesion A (docs/FUTURE/PAINTER_PROMODE_PLAN.md).
// Applies the screen-space rectangle (m_rect_start_corner..m_rect_end_corner) as a paint mask on
// every model-part mesh. Mirrors the trafo/camera precompute the BRUSH/BUCKET_FILL path does in
// gizmo_event() above, but projects via the real camera view*projection matrix (TriangleSelector::
// RectangleProjectionCursor) instead of the small-brush "distance to a ray" approximation Circle/
// Sphere use, since a rectangle can span most of the screen.
void GLGizmoPainterBase::apply_rectangle_mask(EnforcerBlockerType new_state)
{
    if (m_triangle_selectors.empty())
        return;

    const Vec2f rect_min(float(std::min(m_rect_start_corner.x(), m_rect_end_corner.x())),
                          float(std::min(m_rect_start_corner.y(), m_rect_end_corner.y())));
    const Vec2f rect_max(float(std::max(m_rect_start_corner.x(), m_rect_end_corner.x())),
                          float(std::max(m_rect_start_corner.y(), m_rect_end_corner.y())));
    // Ignore a plain click (no real drag) - nothing to mask.
    if (rect_max.x() - rect_min.x() < 2.f || rect_max.y() - rect_min.y() < 2.f)
        return;

    const Camera        &camera    = wxGetApp().plater()->get_camera();
    const Selection      &selection = m_parent.get_selection();
    const ModelObject    *mo        = m_c->selection_info()->model_object();
    const ModelInstance  *mi        = mo->instances[selection.get_instance_idx()];
    const Transform3d     instance_trafo = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
        mi->get_assemble_transformation().get_matrix() :
        mi->get_transformation().get_matrix();

    std::vector<Transform3d> trafo_matrices;
    std::vector<Transform3d> trafo_matrices_not_translate;
    for (const ModelVolume *mv : mo->volumes)
        if (mv->is_model_part()) {
            trafo_matrices.emplace_back(instance_trafo * mv->get_matrix());
            trafo_matrices_not_translate.emplace_back(
                (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView ?
                    mi->get_assemble_transformation().get_matrix_no_offset() :
                    mi->get_transformation().get_matrix_no_offset())
                * mv->get_matrix_no_offset());
        }

    const Matrix4d view_projection_matrix = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const std::array<int, 4> viewport = camera.get_viewport();
    const Vec3d camera_pos_world    = camera.get_position();
    const Vec3d camera_target_world = camera_pos_world + camera.get_dir_forward();

    for (size_t mesh_idx = 0; mesh_idx < trafo_matrices.size(); ++mesh_idx) {
        const Transform3d &trafo_matrix               = trafo_matrices[mesh_idx];
        const Transform3d &trafo_matrix_not_translate  = trafo_matrices_not_translate[mesh_idx];
        const Transform3d  trafo_matrix_inv            = trafo_matrix.inverse();

        const Vec3f camera_pos_mesh    = (trafo_matrix_inv * camera_pos_world).cast<float>();
        const Vec3f camera_target_mesh = (trafo_matrix_inv * camera_target_world).cast<float>();
        const TriangleSelector::ClippingPlane &clp = this->get_clipping_plane_in_volume_coordinates(trafo_matrix);

        std::unique_ptr<TriangleSelector::Cursor> cursor = TriangleSelector::RectangleProjectionCursor::cursor_factory(
            camera_pos_mesh, camera_target_mesh, view_projection_matrix, viewport, rect_min, rect_max, trafo_matrix, clp);
        cursor->set_visibility_test(make_visibility_test(mesh_idx, trafo_matrices));

        m_triangle_selectors[mesh_idx]->set_precision_factor(m_precision_factor);
        m_triangle_selectors[mesh_idx]->select_patch(0, std::move(cursor), new_state, trafo_matrix_not_translate,
            m_triangle_splitting_enabled, m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);
        m_triangle_selectors[mesh_idx]->request_update_render_data(true);
    }
}

// NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F4, Sesion B. Same idea as apply_rectangle_mask()
// above (see its comment for the camera/trafo precompute), but with an arbitrary closed polygon
// (TriangleSelector::PolygonProjectionCursor) instead of a 4-corner box. Called once the polygon
// is closed (see gizmo_event()'s POLYGON handling).
void GLGizmoPainterBase::apply_polygon_mask(EnforcerBlockerType new_state, const std::vector<Vec2d> &flat)
{
    if (m_triangle_selectors.empty() || flat.size() < 3)
        return;
    std::vector<Vec2f> polygon;
    polygon.reserve(flat.size());
    for (const Vec2d &p : flat)
        polygon.emplace_back(float(p.x()), float(p.y()));

    apply_area_cursor(new_state, [&polygon](const Vec3f &camera_pos_mesh, const Vec3f &camera_target_mesh, const Matrix4d &view_projection_matrix,
                                            const std::array<int, 4> &viewport, const Transform3d &trafo_matrix, const TriangleSelector::ClippingPlane &clp) {
        return TriangleSelector::PolygonProjectionCursor::cursor_factory(
            camera_pos_mesh, camera_target_mesh, view_projection_matrix, viewport, polygon, trafo_matrix, clp);
    });
}

// NEOTKO_SVGDECAL_TAG — F1: every ring of the SVG (outer contours and holes) in one pass per mesh,
// through one PolygonMask2D shared by all meshes.
void GLGizmoPainterBase::apply_polygon_set_mask(EnforcerBlockerType new_state, const std::vector<std::vector<Vec2d>> &rings, const std::vector<bool> &is_hole)
{
    if (m_triangle_selectors.empty())
        return;
    auto mask = std::make_shared<PolygonMask2D>();
    mask->build(rings, is_hole);
    if (mask->empty())
        return;

    apply_area_cursor(new_state, [&mask](const Vec3f &camera_pos_mesh, const Vec3f &camera_target_mesh, const Matrix4d &view_projection_matrix,
                                         const std::array<int, 4> &viewport, const Transform3d &trafo_matrix, const TriangleSelector::ClippingPlane &clp) {
        return TriangleSelector::PolygonSetProjectionCursor::cursor_factory(
            camera_pos_mesh, camera_target_mesh, view_projection_matrix, viewport, mask, trafo_matrix, clp);
    });
}

// Shared by the screen-space masks: one select_patch() per model part, with the cursor that
// make_cursor builds for it (mesh-local camera points, current view, part transform, clipping).
Transform3d GLGizmoPainterBase::model_part_trafos(std::vector<Transform3d> &trafos, std::vector<Transform3d> &trafos_no_translate) const
{
    const Selection      &selection = m_parent.get_selection();
    const ModelObject    *mo        = m_c->selection_info()->model_object();
    const ModelInstance  *mi        = mo->instances[selection.get_instance_idx()];
    const bool            assemble  = m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView;
    const Transform3d     instance_trafo = assemble ? mi->get_assemble_transformation().get_matrix() : mi->get_transformation().get_matrix();
    trafos.clear();
    trafos_no_translate.clear();
    for (const ModelVolume *mv : mo->volumes)
        if (mv->is_model_part()) {
            trafos.emplace_back(instance_trafo * mv->get_matrix());
            trafos_no_translate.emplace_back(
                (assemble ? mi->get_assemble_transformation().get_matrix_no_offset() : mi->get_transformation().get_matrix_no_offset())
                * mv->get_matrix_no_offset());
        }
    return instance_trafo;
}

void GLGizmoPainterBase::apply_area_cursor(EnforcerBlockerType new_state, const AreaCursorFactory &make_cursor)
{
    const Camera &camera = wxGetApp().plater()->get_camera();
    std::vector<Transform3d> trafo_matrices;
    std::vector<Transform3d> trafo_matrices_not_translate;
    model_part_trafos(trafo_matrices, trafo_matrices_not_translate);

    const Matrix4d view_projection_matrix = camera.get_projection_matrix().matrix() * camera.get_view_matrix().matrix();
    const std::array<int, 4> viewport = camera.get_viewport();
    const Vec3d camera_pos_world    = camera.get_position();
    const Vec3d camera_target_world = camera_pos_world + camera.get_dir_forward();

    for (size_t mesh_idx = 0; mesh_idx < trafo_matrices.size(); ++mesh_idx) {
        const Transform3d &trafo_matrix               = trafo_matrices[mesh_idx];
        const Transform3d &trafo_matrix_not_translate  = trafo_matrices_not_translate[mesh_idx];
        const Transform3d  trafo_matrix_inv            = trafo_matrix.inverse();

        const Vec3f camera_pos_mesh    = (trafo_matrix_inv * camera_pos_world).cast<float>();
        const Vec3f camera_target_mesh = (trafo_matrix_inv * camera_target_world).cast<float>();
        const TriangleSelector::ClippingPlane &clp = this->get_clipping_plane_in_volume_coordinates(trafo_matrix);

        std::unique_ptr<TriangleSelector::Cursor> cursor = make_cursor(
            camera_pos_mesh, camera_target_mesh, view_projection_matrix, viewport, trafo_matrix, clp);
        cursor->set_visibility_test(make_visibility_test(mesh_idx, trafo_matrices));

        m_triangle_selectors[mesh_idx]->set_precision_factor(m_precision_factor);
        m_triangle_selectors[mesh_idx]->select_patch(0, std::move(cursor), new_state, trafo_matrix_not_translate,
            m_triangle_splitting_enabled, m_paint_on_overhangs_only ? m_highlight_by_angle_threshold_deg : 0.f);
        m_triangle_selectors[mesh_idx]->request_update_render_data(true);
    }
}

bool GLGizmoPainterBase::on_mouse(const wxMouseEvent &mouse_event)
{
    // wxCoord == int --> wx/types.h
    Vec2i32 mouse_coord(mouse_event.GetX(), mouse_event.GetY());
    Vec2d mouse_pos = mouse_coord.cast<double>();

    if (mouse_event.Moving()) {
        gizmo_event(SLAGizmoEventType::Moving, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false);
        return false;
    }

    // when control is down we allow scene pan and rotation even when clicking
    // over some object
    bool control_down           = mouse_event.CmdDown();
    bool grabber_contains_mouse = (get_hover_id() != -1);

    const Selection &selection = m_parent.get_selection();
    int selected_object_idx = selection.get_object_idx();
    if (mouse_event.LeftDown()) {
        if ((!control_down || grabber_contains_mouse) &&            
            gizmo_event(SLAGizmoEventType::LeftDown, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), false))
            // the gizmo got the event and took some action, there is no need
            // to do anything more
            return true;
    } else if (mouse_event.RightDown()){
        if (!control_down && selected_object_idx != -1 &&
            gizmo_event(SLAGizmoEventType::RightDown, mouse_pos, false, false, false)) 
            // event was taken care of
            return true;
    } else if (mouse_event.Dragging()) {
        if (m_parent.get_move_volume_id() != -1)
            // don't allow dragging objects with the Sla gizmo on
            return true;
        if (!control_down && gizmo_event(SLAGizmoEventType::Dragging,
                                         mouse_pos, mouse_event.ShiftDown(),
                                         mouse_event.AltDown(), false)) {
            // the gizmo got the event and took some action, no need to do
            // anything more here
            m_parent.set_as_dirty();
            return true;
        }
        if(control_down && (mouse_event.LeftIsDown() || mouse_event.RightIsDown()))
        {
            // CTRL has been pressed while already dragging -> stop current action
            if (mouse_event.LeftIsDown())
                gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), true);
            else if (mouse_event.RightIsDown())
                gizmo_event(SLAGizmoEventType::RightUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), true);
            return shape_view_locked();
        }
    } else if (mouse_event.LeftUp()) {
        if (!m_parent.is_mouse_dragging()) {
            // in case SLA/FDM gizmo is selected, we just pass the LeftUp
            // event and stop processing - neither object moving or selecting
            // is suppressed in that case
            gizmo_event(SLAGizmoEventType::LeftUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), control_down);
            return true;
        }
    } else if (mouse_event.RightUp()) {
        if (!m_parent.is_mouse_dragging()) {
            gizmo_event(SLAGizmoEventType::RightUp, mouse_pos, mouse_event.ShiftDown(), mouse_event.AltDown(), control_down);
            return true;
        }
    }
    // NEOTKO_PAINTERPRO_TAG — Painter Pro Mode F5: with Lock view on, nothing the shape tool
    // didn't use (middle/right/Ctrl drags...) may reach the camera. ImGui gets its events before
    // the gizmo, so the panel keeps working.
    return shape_view_locked();
}

void GLGizmoPainterBase::update_raycast_cache(const Vec2d& mouse_position,
                                              const Camera& camera,
                                              const std::vector<Transform3d>& trafo_matrices) const
{
    if (m_rr.mouse_position == mouse_position) {
        // Same query as last time - the answer is already in the cache.
        return;
    }

    Vec3f normal =  Vec3f::Zero();
    Vec3f hit = Vec3f::Zero();
    size_t facet = 0;
    Vec3f closest_hit = Vec3f::Zero();
    double closest_hit_squared_distance = std::numeric_limits<double>::max();
    size_t closest_facet = 0;
    int closest_hit_mesh_id = -1;

    // Cast a ray on all meshes, pick the closest hit and save it for the respective mesh
    for (int mesh_id = 0; mesh_id < int(trafo_matrices.size()); ++mesh_id) {

        if (m_c->raycaster()->raycasters()[mesh_id]->unproject_on_mesh(
                   mouse_position,
                   trafo_matrices[mesh_id],
                   camera,
                   hit,
                   normal,
                   m_c->object_clipper()->get_clipping_plane(),
                   &facet,
                   m_parent.get_canvas_type() != GLCanvas3D::CanvasAssembleView))
        {
            // In case this hit is clipped, skip it.
            if (is_mesh_point_clipped(hit.cast<double>(), trafo_matrices[mesh_id]))
                continue;

            // Is this hit the closest to the camera so far?
            double hit_squared_distance = (camera.get_position()-trafo_matrices[mesh_id]*hit.cast<double>()).squaredNorm();
            if (hit_squared_distance < closest_hit_squared_distance) {
                closest_hit_squared_distance = hit_squared_distance;
                closest_facet = facet;
                closest_hit_mesh_id = mesh_id;
                closest_hit = hit;
            }
        }
    }

    m_rr = {mouse_position, closest_hit_mesh_id, closest_hit, closest_facet};
}

bool GLGizmoPainterBase::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();

    if (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF
        || !selection.is_single_full_instance()/* || wxGetApp().get_mode() == comSimple*/)
        return false;

    // BBS
#if 0
    // Check that none of the selected volumes is outside. Only SLA auxiliaries (supports) are allowed outside.
    const Selection::IndicesList& list = selection.get_volume_idxs();
    return std::all_of(list.cbegin(), list.cend(), [&selection](unsigned int idx) { return !selection.get_volume(idx)->is_outside; });
#else
    return true;
#endif
}

bool GLGizmoPainterBase::on_is_selectable() const
{
    //BBS
    /*return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF
         && wxGetApp().get_mode() != comSimple );*/
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF);
}


CommonGizmosDataID GLGizmoPainterBase::on_get_requirements() const
{
    return CommonGizmosDataID(
                int(CommonGizmosDataID::SelectionInfo)
              | int(CommonGizmosDataID::InstancesHider)
              | int(CommonGizmosDataID::Raycaster)
              | int(CommonGizmosDataID::ObjectClipper));
}


void GLGizmoPainterBase::on_set_state()
{
    if (m_state == m_old_state)
        return;

    if (m_state == On && m_old_state != On) { // the gizmo was just turned on
        m_parent.enable_picking(false);
        on_opening();

        const Selection& selection = m_parent.get_selection();
        //Camera& camera = wxGetApp().plater()->get_camera();
        //Vec3d rotate_target = selection.get_bounding_box().center();
        //rotate_target(2) = 0.f;
        //Vec3d position = camera.get_position();
        //camera.set_target(rotate_target);
        //camera.look_at(position, rotate_target, Vec3d::UnitZ());
    }
    if (m_state == Off && m_old_state != Off) { // the gizmo was just turned Off
        m_parent.enable_picking(true);
        // we are actually shutting down
        on_shutdown();
        shape_clear(); // NEOTKO_PAINTERPRO_TAG — never leave a half-drawn shape behind
        m_old_mo_id = -1;
        //m_iva.release_geometry();
        m_triangle_selectors.clear();

        //Camera& camera = wxGetApp().plater()->get_camera();
        //camera.look_at(camera.get_position(), m_previous_target, Vec3d::UnitZ());
        //camera.set_target(m_previous_target);
        //camera.recover_from_free_camera();
    }
    m_old_state = m_state;
}



void GLGizmoPainterBase::on_load(cereal::BinaryInputArchive&)
{
    // We should update the gizmo from current ModelObject, but it is not
    // possible at this point. That would require having updated selection and
    // common gizmos data, which is not done at this point. Instead, save
    // a flag to do the update in set_painter_gizmo_data, which will be called
    // soon after.
    m_schedule_update = true;
}

TriangleSelector::ClippingPlane GLGizmoPainterBase::get_clipping_plane_in_volume_coordinates(const Transform3d &trafo) const {
    const ::Slic3r::GUI::ClippingPlane *const clipping_plane = m_c->object_clipper()->get_clipping_plane();
    if (clipping_plane == nullptr || !clipping_plane->is_active())
        return {};

    const Vec3d  clp_normal = clipping_plane->get_normal();
    const double clp_offset = clipping_plane->get_offset();

    const Transform3d trafo_normal = Transform3d(trafo.linear().transpose());
    const Transform3d trafo_inv    = trafo.inverse();

    Vec3d point_on_plane             = clp_normal * clp_offset;
    Vec3d point_on_plane_transformed = trafo_inv * point_on_plane;
    Vec3d normal_transformed         = trafo_normal * clp_normal;
    auto offset_transformed          = float(point_on_plane_transformed.dot(normal_transformed));

    return TriangleSelector::ClippingPlane({float(normal_transformed.x()), float(normal_transformed.y()), float(normal_transformed.z()), offset_transformed});
}

ColorRGBA TriangleSelectorGUI::enforcers_color = {0.5f, 1.f, 0.5f, 1.f};
ColorRGBA TriangleSelectorGUI::blockers_color  = {1.f, 0.5f, 0.5f, 1.f};

ColorRGBA TriangleSelectorGUI::get_seed_fill_color(const ColorRGBA& base_color)
{
    // BBS
    return {
        base_color.r() * 1.25f < 1.f ? base_color.r() * 1.25f : 1.f,
        base_color.g() * 1.25f < 1.f ? base_color.g() * 1.25f : 1.f,
        base_color.b() * 1.25f < 1.f ? base_color.b() * 1.25f : 1.f,
        1.f};
}

void TriangleSelectorGUI::render(ImGuiWrapper* imgui, const Transform3d& matrix)
{
    if (m_update_render_data) {
        update_render_data();
        m_update_render_data = false;
    }

    auto* shader = wxGetApp().get_current_shader();
    if (! shader)
        return;
    assert(shader->get_name() == "gouraud" || shader->get_name() == "mm_gouraud");

    for (auto iva : {std::make_pair(&m_iva_enforcers, enforcers_color),
                     std::make_pair(&m_iva_blockers, blockers_color)}) {
        iva.first->set_color(iva.second);
        iva.first->render();
    }

    for (auto& iva : m_iva_seed_fills) {
        size_t           color_idx = &iva - &m_iva_seed_fills.front();
        const ColorRGBA& color     = TriangleSelectorGUI::get_seed_fill_color(color_idx == 1 ? enforcers_color :
            color_idx == 2 ? blockers_color :
            GLVolume::NEUTRAL_COLOR);
        iva.set_color(color);
        iva.render();
    }

    render_paint_contour(matrix);

#ifdef PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
    if (imgui)
        render_debug(imgui);
    else
        assert(false); // If you want debug output, pass ptr to ImGuiWrapper.
#endif
}

void TriangleSelectorGUI::update_render_data()
{
    int              enf_cnt = 0;
    int              blc_cnt = 0;
    std::vector<int> seed_fill_cnt(m_iva_seed_fills.size(), 0);

    for (auto* iva : { &m_iva_enforcers, &m_iva_blockers }) {
        iva->reset();
    }

    for (auto& iva : m_iva_seed_fills) {
        iva.reset();
    }

    GLModel::Geometry iva_enforcers_data;
    iva_enforcers_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    GLModel::Geometry iva_blockers_data;
    iva_blockers_data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    std::array<GLModel::Geometry, 3> iva_seed_fills_data;
    for (auto& data : iva_seed_fills_data)
        data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };

    // small value used to offset triangles along their normal to avoid z-fighting
    static const float offset = 0.001f;

    for (const Triangle &tr : m_triangles) {
        if (!tr.valid() || tr.is_split() || (tr.get_state() == EnforcerBlockerType::NONE && !tr.is_selected_by_seed_fill()))
            continue;

        int tr_state = int(tr.get_state());
        GLModel::Geometry &iva = tr.is_selected_by_seed_fill()                   ? iva_seed_fills_data[tr_state] :
                                 tr.get_state() == EnforcerBlockerType::ENFORCER ? iva_enforcers_data :
                                                                                   iva_blockers_data;
        int                  &cnt = tr.is_selected_by_seed_fill()                   ? seed_fill_cnt[tr_state] :
                                    tr.get_state() == EnforcerBlockerType::ENFORCER ? enf_cnt :
                                                                                      blc_cnt;
        const Vec3f          &v0  = m_vertices[tr.verts_idxs[0]].v;
        const Vec3f          &v1  = m_vertices[tr.verts_idxs[1]].v;
        const Vec3f          &v2  = m_vertices[tr.verts_idxs[2]].v;
        //FIXME the normal may likely be pulled from m_triangle_selectors, but it may not be worth the effort
        // or the current implementation may be more cache friendly.
        const Vec3f           n   = (v1 - v0).cross(v2 - v1).normalized();
        // small value used to offset triangles along their normal to avoid z-fighting
        const Vec3f    offset_n   = offset * n;
        iva.add_vertex(v0 + offset_n, n);
        iva.add_vertex(v1 + offset_n, n);
        iva.add_vertex(v2 + offset_n, n);
        iva.add_triangle((unsigned int)cnt, (unsigned int)cnt + 1, (unsigned int)cnt + 2);
        cnt += 3;
    }

    if (!iva_enforcers_data.is_empty())
        m_iva_enforcers.init_from(std::move(iva_enforcers_data));
    if (!iva_blockers_data.is_empty())
        m_iva_blockers.init_from(std::move(iva_blockers_data));
    for (size_t i = 0; i < m_iva_seed_fills.size(); ++i) {
        if (!iva_seed_fills_data[i].is_empty())
            m_iva_seed_fills[i].init_from(std::move(iva_seed_fills_data[i]));
    }

    update_paint_contour();
}

// BBS
bool TrianglePatch::is_fragment() const
{
    return this->area < TriangleSelectorPatch::gap_area;
}

float TriangleSelectorPatch::gap_area = TriangleSelectorPatch::GapAreaMin;

void TriangleSelectorPatch::render(ImGuiWrapper* imgui, const Transform3d& matrix)
{
    static bool last_show_wireframe = false;
    if (last_show_wireframe != wxGetApp().plater()->is_show_wireframe()) {
        last_show_wireframe  = wxGetApp().plater()->is_show_wireframe();
        m_update_render_data = true;
        m_paint_changed      = true;
    }
    if (m_update_render_data) {
        update_render_data();
        m_update_render_data = false;
    }

    auto* shader = wxGetApp().get_current_shader();
    if (!shader)
        return;
    assert(shader->get_name() == "gouraud" || shader->get_name() == "mm_gouraud");
    bool  show_wireframe = false;
    if (wxGetApp().plater()->is_wireframe_enabled()) {
        if (m_need_wireframe && wxGetApp().plater()->is_show_wireframe()) {
            //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", show_wireframe on");
            shader->set_uniform("show_wireframe", true);
            show_wireframe = true;
        }
        else {
            //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", show_wireframe off");
            shader->set_uniform("show_wireframe", false);
        }
    }

    // NEOTKO_COLORSTITCH_TAG — push this slot's weave params into the mm_gouraud
    // uniforms (or disable). Always called so a previous patch's weave never leaks
    // into the next draw call. Inert on the MMU shader if the uniform is absent.
    auto apply_weave = [&shader](const TriangleSelectorPatch::WeaveParams* w) {
        const bool on = w && w->on && !w->cols.empty();
        shader->set_uniform("u_weave_on", on);
        if (!on) return;
        const int n = std::min<int>(64, (int)w->cols.size());
        shader->set_uniform("u_weave_n", n);
        shader->set_uniform("u_weave_tile", w->tile);   // NEOTKO_COLORSTITCH_TAG — wrap vs span
        // NEOTKO_COLORSTITCH_TAG — s280d: el ángulo se manda tal cual. Una banda en
        // AUTO ya no parpadea; su aviso es el contorno violeta del painter.
        shader->set_uniform("u_weave_angle", w->angle_rad);
        shader->set_uniform("u_weave_pitch", w->pitch);
        shader->set_uniform("u_weave_p0", w->p0);
        float ax[3]; w->shader_axis(ax);   // s318 F3 — ver ColorStitchPaintPreview::weave_frame
        shader->set_uniform("u_weave_axis", Vec3f(ax[0], ax[1], ax[2]));
        // s318 F3 — opción A: segundo pase compuesto por fragmento (make_zone_weave).
        shader->set_uniform("u_weave_dual", w->dual);
        if (w->dual) {
            const int n2 = std::min<int>(64, (int)w->cols2.size());
            shader->set_uniform("u_weave2_n", n2);
            shader->set_uniform("u_weave2_tile", w->tile2);
            shader->set_uniform("u_weave2_pitch", w->pitch2);
            shader->set_uniform("u_weave2_p0", w->p0_2);
            shader->set_uniform("u_weave2_axis", Vec3f(w->axis2[0], w->axis2[1], w->axis2[2]));
            for (int i = 0; i < n2; ++i) {
                char nm[24];
                std::snprintf(nm, sizeof(nm), "u_weave2_cols[%d]", i);
                const ColorRGBA& c = w->cols2[i];
                shader->set_uniform(nm, Vec3f(c.r(), c.g(), c.b()));
            }
            for (int i = 0; i < n && i < (int)w->dual_a.size(); ++i) {
                char nm[24];
                std::snprintf(nm, sizeof(nm), "u_weave_a[%d]", i);
                const ColorRGBA& c = w->dual_a[i];
                shader->set_uniform(nm, Vec3f(c.r(), c.g(), c.b()));
            }
        }
        for (int i = 0; i < n; ++i) {
            char nm[24];
            std::snprintf(nm, sizeof(nm), "u_weave_cols[%d]", i);
            const ColorRGBA& c = w->cols[i];
            shader->set_uniform(nm, Vec3f(c.r(), c.g(), c.b()));
        }
    };

    for (size_t buffer_idx = 0; buffer_idx < m_triangle_patches.size(); ++buffer_idx) {
        if (this->has_VBOs(buffer_idx)) {
            const TrianglePatch& patch = m_triangle_patches[buffer_idx];
            ColorRGBA color;
            size_t color_idx;
            if (patch.is_fragment() && !patch.neighbor_types.empty()) {
                color_idx = (size_t)*patch.neighbor_types.begin();
                if (color_idx >= m_ebt_colors.size())
                    continue;
                color = m_ebt_colors[color_idx];
                color.a(0.85);
            }
            else {
                color_idx = (size_t)patch.type;
                if (color_idx >= m_ebt_colors.size())
                    continue;
                color = m_ebt_colors[color_idx];
            }
            // NEOTKO_PROFILE_TAG s235 F5b — ver set_skip_base_patch(). Se decide sobre el
            // color_idx YA resuelto, no sobre patch.type: un fragmento hereda el estado del
            // vecino, y saltarse un fragmento cuyo vecino sí está pintado dejaría agujeros.
            if (m_skip_base_patch && color_idx == 0)
                continue;
            //to make black not too hard too see
            ColorRGBA new_color = adjust_color_for_rendering(color);
            shader->set_uniform("uniform_color", new_color);
            // NEOTKO_COLORSTITCH_TAG — per-ISLAND weave override; -1 ⇒ per-slot fallback.
            const TriangleSelectorPatch::WeaveParams* wparam =
                (patch.weave_idx >= 0 && patch.weave_idx < (int)m_weave_list.size())
                    ? &m_weave_list[patch.weave_idx]
                    : (color_idx < m_ebt_weave.size() ? &m_ebt_weave[color_idx] : nullptr);
            apply_weave(wparam);
            this->render(buffer_idx, show_wireframe);
        }
    }
    apply_weave(nullptr);   // leave the shader in flat mode for the contour pass

    render_paint_contour(matrix);
}

void TriangleSelectorPatch::update_triangles_per_type()
{
    //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", enter");
    // NEOTKO_COLORSTITCH_TAG — patches are batched by (state, weave_idx) so each painted
    // ISLAND can carry its own gradient/pattern params. Without a per-island map this
    // collapses to one patch per state = the original behaviour (weave_idx = -1).
    m_triangle_patches.clear();
    std::map<std::pair<int,int>, int> patch_of;   // (state, weave_idx) → m_triangle_patches index

    bool using_wireframe = (m_need_wireframe && wxGetApp().plater()->is_wireframe_enabled() && wxGetApp().plater()->is_show_wireframe()) ? true : false;

    int facet_idx = -1;
    for (auto& triangle : m_triangles) {
        ++facet_idx;
        if (!triangle.valid() || triangle.is_split())
            continue;

        int state = (int)triangle.get_state();
        if (state < 0 || state >= int(m_ebt_colors.size()))
            continue;
        int weave_idx = -1;
        if (!m_facet_weave_idx.empty()) {
            auto wit = m_facet_weave_idx.find(facet_idx);
            if (wit != m_facet_weave_idx.end()) weave_idx = wit->second;
        }
        const std::pair<int,int> key{state, weave_idx};
        int patch_index;
        auto pit = patch_of.find(key);
        if (pit == patch_of.end()) {
            patch_index = int(m_triangle_patches.size());
            patch_of.emplace(key, patch_index);
            m_triangle_patches.emplace_back();
            TrianglePatch& np = m_triangle_patches.back();
            np.type      = (EnforcerBlockerType)state;
            np.weave_idx = weave_idx;
            np.triangle_indices.reserve(m_triangles.size() / 3);
        } else {
            patch_index = pit->second;
        }
        auto& patch = m_triangle_patches[patch_index];
        //patch.triangle_indices.insert(patch.triangle_indices.end(), triangle.verts_idxs.begin(), triangle.verts_idxs.end());
        for (int i = 0; i < 3; ++i) {
            int j = triangle.verts_idxs[i];
            int index = using_wireframe?int(patch.patch_vertices.size()/6) : int(patch.patch_vertices.size()/3);
            //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: i=%2%, j=%3%, index=%4%, v[%5%,%6%,%7%]")%__LINE__%i%j%index%m_vertices[j].v(0)%m_vertices[j].v(1)%m_vertices[j].v(2);
            patch.patch_vertices.emplace_back(m_vertices[j].v(0));
            patch.patch_vertices.emplace_back(m_vertices[j].v(1));
            patch.patch_vertices.emplace_back(m_vertices[j].v(2));
            if (using_wireframe) {
                if (i == 0) {
                    patch.patch_vertices.emplace_back(1.0);
                    patch.patch_vertices.emplace_back(0.0);
                    patch.patch_vertices.emplace_back(0.0);
                }
                else if (i == 1) {
                    patch.patch_vertices.emplace_back(0.0);
                    patch.patch_vertices.emplace_back(1.0);
                    patch.patch_vertices.emplace_back(0.0);
                }
                else {
                    patch.patch_vertices.emplace_back(0.0);
                    patch.patch_vertices.emplace_back(0.0);
                    patch.patch_vertices.emplace_back(1.0);
                }
            }
            patch.triangle_indices.emplace_back( index);
        }
        //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: state=%2%, vertice size=%3%, triangle size %4%")%__LINE__%state%patch.patch_vertices.size()%patch.triangle_indices.size();
    }
    //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("exit");
}

void TriangleSelectorPatch::update_selector_triangles()
{
    for (TrianglePatch& patch : m_triangle_patches) {
        if (!patch.is_fragment() || patch.neighbor_types.empty())
            continue;

        EnforcerBlockerType type = *patch.neighbor_types.begin();
        for (int facet_idx : patch.facet_indices) {
            m_triangles[facet_idx].set_state(type);
        }
    }
}

void TriangleSelectorPatch::update_triangles_per_patch()
{
    auto [neighbors, neighbors_propagated] = this->precompute_all_neighbors();
    std::vector<bool>  visited(m_triangles.size(), false);

    bool using_wireframe = (m_need_wireframe && wxGetApp().plater()->is_wireframe_enabled() && wxGetApp().plater()->is_show_wireframe()) ? true : false; 

    auto get_all_touching_triangles = [this](int facet_idx, const Vec3i32& neighbors, const Vec3i32& neighbors_propagated) -> std::vector<int> {
        assert(facet_idx != -1 && facet_idx < int(m_triangles.size()));
        assert(this->verify_triangle_neighbors(m_triangles[facet_idx], neighbors));
        std::vector<int> touching_triangles;
        Vec3i32            vertices = { m_triangles[facet_idx].verts_idxs[0], m_triangles[facet_idx].verts_idxs[1], m_triangles[facet_idx].verts_idxs[2] };
        append_touching_subtriangles(neighbors(0), vertices(1), vertices(0), touching_triangles);
        append_touching_subtriangles(neighbors(1), vertices(2), vertices(1), touching_triangles);
        append_touching_subtriangles(neighbors(2), vertices(0), vertices(2), touching_triangles);

        for (int neighbor_idx : neighbors_propagated)
            if (neighbor_idx != -1 && !m_triangles[neighbor_idx].is_split())
                touching_triangles.emplace_back(neighbor_idx);

        return touching_triangles;
    };

    auto calc_fragment_area = [this](const TrianglePatch& patch, float max_limit_area, int stride) {
        double total_area = 0.f;
        const std::vector<int>& ti = patch.triangle_indices;
        /*for (int i = 0; i < ti.size() / 3; i++) {
            total_area += std::abs((m_vertices[ti[i]].v - m_vertices[ti[i + 1]].v)
                .cross(m_vertices[ti[i]].v - m_vertices[ti[i + 2]].v).norm()) / 2;
            if (total_area >= max_limit_area)
                break;
        }*/
        const std::vector<float>& vertices = patch.patch_vertices;
        for (int i = 0; i < ti.size(); i+=3) {
            stl_vertex v0(vertices[ti[i] * stride], vertices[ti[i] * stride + 1], vertices[ti[i] * stride + 2]);
            stl_vertex v1(vertices[ti[i + 1] * stride], vertices[ti[i + 1] * stride + 1], vertices[ti[i + 1] * stride + 2]);
            stl_vertex v2(vertices[ti[i + 2] * stride], vertices[ti[i + 2] * stride + 1], vertices[ti[i + 2] * stride + 2]);
            total_area += std::abs((v0 - v1).cross(v0 - v2).norm()) / 2;
            if (total_area >= max_limit_area)
                break;
        }

        return total_area;
    };

    int start_facet_idx = 0;
    while (1) {
        for (; start_facet_idx < visited.size(); start_facet_idx++) {
            if (!visited[start_facet_idx] && m_triangles[start_facet_idx].valid() && !m_triangles[start_facet_idx].is_split())
                break;
        }

        if (start_facet_idx >= m_triangles.size())
            break;

        EnforcerBlockerType start_facet_state = m_triangles[start_facet_idx].get_state();
        TrianglePatch patch;
        std::queue<int> facet_queue;
        facet_queue.push(start_facet_idx);
        while (!facet_queue.empty()) {
            int current_facet = facet_queue.front();
            facet_queue.pop();
            assert(!m_triangles[current_facet].is_split());

            if (!visited[current_facet]) {
                Triangle& triangle = m_triangles[current_facet];
                for (int i = 0; i < 3; ++i) {
                    int j = triangle.verts_idxs[i];
                    int index = using_wireframe?int(patch.patch_vertices.size()/6) : int(patch.patch_vertices.size()/3);
                    patch.patch_vertices.emplace_back(m_vertices[j].v(0));
                    patch.patch_vertices.emplace_back(m_vertices[j].v(1));
                    patch.patch_vertices.emplace_back(m_vertices[j].v(2));
                    if (using_wireframe) {
                        if (i == 0) {
                            patch.patch_vertices.emplace_back(1.0);
                            patch.patch_vertices.emplace_back(0.0);
                            patch.patch_vertices.emplace_back(0.0);
                        }
                        else if (i == 1) {
                            patch.patch_vertices.emplace_back(0.0);
                            patch.patch_vertices.emplace_back(1.0);
                            patch.patch_vertices.emplace_back(0.0);
                        }
                        else {
                            patch.patch_vertices.emplace_back(0.0);
                            patch.patch_vertices.emplace_back(0.0);
                            patch.patch_vertices.emplace_back(1.0);
                        }
                    }
                    patch.triangle_indices.emplace_back( index);
                }
                //patch.triangle_indices.insert(patch.triangle_indices.end(), triangle.verts_idxs.begin(), triangle.verts_idxs.end());
                patch.facet_indices.push_back(current_facet);

                std::vector<int> touching_triangles = get_all_touching_triangles(current_facet, neighbors[current_facet], neighbors_propagated[current_facet]);
                for (const int tr_idx : touching_triangles) {
                    if (tr_idx < 0)
                        continue;

                    if (m_triangles[tr_idx].get_state() != start_facet_state) {
                        patch.neighbor_types.insert(m_triangles[tr_idx].get_state());
                        continue;
                    }

                    // should check visited state after color for neight types
                    if (visited[tr_idx])
                        continue;

                    assert(!m_triangles[tr_idx].is_split());
                    facet_queue.push(tr_idx);
                }
            }

            visited[current_facet] = true;
        }

        patch.area = calc_fragment_area(patch, GapAreaMax, using_wireframe?6:3);
        patch.type = start_facet_state;
        m_triangle_patches.emplace_back(std::move(patch));
     }
}

void TriangleSelectorPatch::set_filter_state(bool is_filter_state)
{
    if (!m_filter_state && is_filter_state) {
        m_filter_state = is_filter_state;
        this->release_geometry();
        update_render_data();
    }

    m_filter_state = is_filter_state;
}

void TriangleSelectorPatch::update_render_data()
{
    //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", m_paint_changed=%1%, m_triangle_patches.size %2%")%m_paint_changed%m_triangle_patches.size();
    if (m_paint_changed || (m_triangle_patches.size() == 0)) {
        this->release_geometry();

        /*m_patch_vertices.reserve(m_vertices.size() * 3);
        for (const Vertex& vr : m_vertices) {
            m_patch_vertices.emplace_back(vr.v.x());
            m_patch_vertices.emplace_back(vr.v.y());
            m_patch_vertices.emplace_back(vr.v.z());
        }
        this->finalize_vertices();*/

        if (m_filter_state)
            update_triangles_per_patch();
        else
            update_triangles_per_type();
        this->finalize_triangle_indices();

        m_paint_changed = false;
    }

    //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", before paint_contour");
    update_paint_contour();
    //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", exit");
}

void TriangleSelectorPatch::render(int triangle_indices_idx, bool show_wireframe)
{
    assert(triangle_indices_idx < this->m_triangle_indices_VBO_ids.size());
    assert(this->m_triangle_patches.size() == this->m_triangle_indices_VBO_ids.size());
    //assert(this->m_vertices_VBO_id != 0);
    assert(this->m_triangle_patches.size() == this->m_vertices_VBO_ids.size());
    assert(this->m_vertices_VBO_ids[triangle_indices_idx] != 0);
    assert(this->m_triangle_indices_VBO_ids[triangle_indices_idx] != 0);

    GLShaderProgram *shader = wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    // the following binding is needed to set the vertex attributes
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->m_vertices_VBO_ids[triangle_indices_idx]));
    const GLint position_id = shader->get_attrib_location("v_position");
    if (position_id != -1) {
        if (show_wireframe) {
            glsafe(::glVertexAttribPointer((GLint) position_id, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (const void *) 0));
        } else {
            glsafe(::glVertexAttribPointer((GLint) position_id, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr));
        }
        glsafe(::glEnableVertexAttribArray((GLint)position_id));
    }
    GLint barycentric_id = -1;
    // Orca: This is required even if wireframe is not displayed, otherwise on AMD Vega GPUs the painter gizmo won't render properly
    /*if (show_wireframe)*/ {
        barycentric_id = shader->get_attrib_location("v_barycentric");
        if (barycentric_id != -1) {
            glsafe(::glVertexAttribPointer((GLint) barycentric_id, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (const void *) (3 * sizeof(float))));
            glsafe(::glEnableVertexAttribArray((GLint) barycentric_id));
        }
    }

    // Render using the Vertex Buffer Objects.
    if (this->m_triangle_indices_sizes[triangle_indices_idx] > 0) {
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->m_triangle_indices_VBO_ids[triangle_indices_idx]));
        glsafe(::glDrawElements(GL_TRIANGLES, GLsizei(this->m_triangle_indices_sizes[triangle_indices_idx]), GL_UNSIGNED_INT, nullptr));
        glsafe(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
        //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: triangle_indices_idx %2%, bind indices vbo, buffer id %3%")%__LINE__%triangle_indices_idx%this->m_triangle_indices_VBO_ids[triangle_indices_idx];
    }

    if (position_id != -1)
        glsafe(::glDisableVertexAttribArray(position_id));
    if (barycentric_id != -1)
        glsafe(::glDisableVertexAttribArray(barycentric_id));

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
}

void TriangleSelectorPatch::release_geometry()
{
    /*if (m_vertices_VBO_id) {
        glsafe(::glDeleteBuffers(1, &m_vertices_VBO_id));
        m_vertices_VBO_id = 0;
    }*/
    for (auto& vertice_VBO_id : m_vertices_VBO_ids) {
        glsafe(::glDeleteBuffers(1, &vertice_VBO_id));
        vertice_VBO_id = 0;
    }
    for (auto& triangle_indices_VBO_id : m_triangle_indices_VBO_ids) {
        glsafe(::glDeleteBuffers(1, &triangle_indices_VBO_id));
        triangle_indices_VBO_id = 0;
    }
    this->clear();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: released geometry")%__LINE__;
}

void TriangleSelectorPatch::finalize_vertices()
{
    /*assert(m_vertices_VBO_id == 0);
    if (!this->m_patch_vertices.empty()) {
        glsafe(::glGenBuffers(1, &this->m_vertices_VBO_id));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->m_vertices_VBO_id));
        glsafe(::glBufferData(GL_ARRAY_BUFFER, this->m_patch_vertices.size() * sizeof(float), this->m_patch_vertices.data(), GL_STATIC_DRAW));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
        this->m_patch_vertices.clear();
    }*/
}

void TriangleSelectorPatch::finalize_triangle_indices()
{
    m_vertices_VBO_ids.resize(m_triangle_patches.size());
    m_triangle_indices_VBO_ids.resize(m_triangle_patches.size());
    m_triangle_indices_sizes.resize(m_triangle_patches.size());
    assert(std::all_of(m_triangle_indices_VBO_ids.cbegin(), m_triangle_indices_VBO_ids.cend(), [](const auto& ti_VBO_id) { return ti_VBO_id == 0; }));

    for (size_t buffer_idx = 0; buffer_idx < m_triangle_patches.size(); ++buffer_idx) {
        std::vector<float>& patch_vertices = m_triangle_patches[buffer_idx].patch_vertices;
        if (!patch_vertices.empty()) {
            glsafe(::glGenBuffers(1, &m_vertices_VBO_ids[buffer_idx]));
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, m_vertices_VBO_ids[buffer_idx]));
            glsafe(::glBufferData(GL_ARRAY_BUFFER, patch_vertices.size() * sizeof(float), patch_vertices.data(), GL_STATIC_DRAW));
            glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
            //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: buffer_idx %2%, vertices size %3%, buffer id %4%")%__LINE__%buffer_idx%patch_vertices.size()%m_vertices_VBO_ids[buffer_idx];
            patch_vertices.clear();
        }

        std::vector<int>& triangle_indices = m_triangle_patches[buffer_idx].triangle_indices;
        m_triangle_indices_sizes[buffer_idx] = triangle_indices.size();
        if (!triangle_indices.empty()) {
            glsafe(::glGenBuffers(1, &m_triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, triangle_indices.size() * sizeof(int), triangle_indices.data(), GL_STATIC_DRAW));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
            //BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", Line %1%: buffer_idx %2%, vertices size %3%, buffer id %4%")%__LINE__%buffer_idx%triangle_indices.size()%m_triangle_indices_VBO_ids[buffer_idx];
            triangle_indices.clear();
        }
    }
}

#ifdef PRUSASLICER_TRIANGLE_SELECTOR_DEBUG
void TriangleSelectorGUI::render_debug(ImGuiWrapper* imgui)
{
    imgui->begin(std::string("TriangleSelector dialog (DEV ONLY)"),
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    static float edge_limit = 1.f;
    imgui->text("Edge limit (mm): ");
    imgui->slider_float("", &edge_limit, 0.1f, 8.f);
    set_edge_limit(edge_limit);
    imgui->checkbox("Show split triangles: ", m_show_triangles);
    imgui->checkbox("Show invalid triangles: ", m_show_invalid);

    int valid_triangles = m_triangles.size() - m_invalid_triangles;
    imgui->text("Valid triangles: " + std::to_string(valid_triangles) +
                  "/" + std::to_string(m_triangles.size()));
    imgui->text("Vertices: " + std::to_string(m_vertices.size()));
    if (imgui->button("Force garbage collection"))
        garbage_collect();

    if (imgui->button("Serialize - deserialize")) {
        auto map = serialize();
        deserialize(map);
    }

    imgui->end();

    if (! m_show_triangles)
        return;

    enum vtype {
        ORIGINAL = 0,
        SPLIT,
        INVALID
    };

    for (auto& va : m_varrays)
        va.reset();

    std::array<int, 3> cnts;

    ::glScalef(1.01f, 1.01f, 1.01f);

    std::array<GLModel::Geometry, 3> varrays_data;
    for (auto& data : varrays_data)
        data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3, GLModel::Geometry::EIndexType::UINT };

    for (int tr_id=0; tr_id<int(m_triangles.size()); ++tr_id) {
        const Triangle& tr = m_triangles[tr_id];
        GLModel::Geometry* va = nullptr;
        int* cnt = nullptr;
        if (tr_id < m_orig_size_indices) {
            va = &varrays_data[ORIGINAL];
            cnt = &cnts[ORIGINAL];
        }
        else if (tr.valid()) {
            va = &varrays_data[SPLIT];
            cnt = &cnts[SPLIT];
        }
        else {
            if (! m_show_invalid)
                continue;
            va = &varrays_data[INVALID];
            cnt = &cnts[INVALID];
        }

        for (int i = 0; i < 3; ++i) {
            va->add_vertex(m_vertices[tr.verts_idxs[i]].v, Vec3f(0.0f, 0.0f, 1.0f));
        }
        va->add_uint_triangle((unsigned int)*cnt, (unsigned int)*cnt + 1, (unsigned int)*cnt + 2);
        *cnt += 3;
    }

    for (int i = 0; i < 3; ++i) {
        if (!varrays_data[i].is_empty())
            m_varrays[i].init_from(std::move(varrays_data[i]));
    }

    GLShaderProgram* curr_shader = wxGetApp().get_current_shader();
    if (curr_shader != nullptr)
        curr_shader->stop_using();

    GLShaderProgram* shader = wxGetApp().get_shader("flat");
    if (shader != nullptr) {
        shader->start_using();

        const Camera& camera = wxGetApp().plater()->get_camera();
        shader->set_uniform("view_model_matrix", camera.get_view_matrix());
        shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    ::glPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
    for (vtype i : {ORIGINAL, SPLIT, INVALID}) {
        GLModel& va = m_varrays[i];
        switch (i) {
        case ORIGINAL: va.set_color({ 0.0f, 0.0f, 1.0f, 1.0f }); break;
        case SPLIT:    va.set_color({ 1.0f, 0.0f, 0.0f, 1.0f }); break;
        case INVALID:  va.set_color({ 1.0f, 1.0f, 0.0f, 1.0f }); break;
        }
        va.render();
    }
    ::glPolygonMode( GL_FRONT_AND_BACK, GL_FILL );

        shader->stop_using();
    }

    if (curr_shader != nullptr)
        curr_shader->start_using();
}
#endif // PRUSASLICER_TRIANGLE_SELECTOR_DEBUG

void TriangleSelectorGUI::update_paint_contour()
{
    m_paint_contour.reset();

    GLModel::Geometry init_data;
    const std::vector<Vec2i32> contour_edges = this->get_seed_fill_contour();
    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    init_data.reserve_vertices(2 * contour_edges.size());
    init_data.reserve_indices(2 * contour_edges.size());
    init_data.color = ColorRGBA::WHITE();
    // vertices + indices
    unsigned int vertices_count = 0;
    for (const Vec2i32& edge : contour_edges) {
        init_data.add_vertex(m_vertices[edge(0)].v);
        init_data.add_vertex(m_vertices[edge(1)].v);
        vertices_count += 2;
        init_data.add_line(vertices_count - 2, vertices_count - 1);
    }

    if (!init_data.is_empty())
        m_paint_contour.init_from(std::move(init_data));
}

void TriangleSelectorGUI::render_paint_contour(const Transform3d& matrix)
{
    auto* curr_shader = wxGetApp().get_current_shader();
    if (curr_shader != nullptr)
        curr_shader->stop_using();

    auto* contour_shader = wxGetApp().get_shader("mm_contour");
    if (contour_shader != nullptr) {
        contour_shader->start_using();

        contour_shader->set_uniform("offset", OpenGLManager::get_gl_info().is_mesa() ? 0.0005 : 0.00001);
        const Camera& camera = wxGetApp().plater()->get_camera();
        contour_shader->set_uniform("view_model_matrix", camera.get_view_matrix() * matrix);
        contour_shader->set_uniform("projection_matrix", camera.get_projection_matrix());

        m_paint_contour.render();

        contour_shader->stop_using();
    }

    if (curr_shader != nullptr)
        curr_shader->start_using();
}

} // namespace Slic3r::GUI
