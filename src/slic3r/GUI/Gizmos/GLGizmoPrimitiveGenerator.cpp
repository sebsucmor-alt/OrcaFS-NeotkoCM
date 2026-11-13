// NEOTKO_PRIMGEN_TAG_START — 5-nov-2026, see GLGizmoPrimitiveGenerator.hpp
#include "GLGizmoPrimitiveGenerator.hpp"
#include "GizmoNeotkoStyle.hpp" // house window look
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <wx/filedlg.h>
#include <wx/utils.h>
// Windows: wx defines pid_t as a macro, which breaks boost::process (same fix as MediaPlayCtrl.cpp).
#undef pid_t
#include <boost/process.hpp>

#include <imgui/imgui.h>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>

namespace Slic3r {
namespace GUI {

using namespace NeoPrim;

static Model& pg_model() { return wxGetApp().plater()->model(); }

static std::string presets_path() { return data_dir() + "/primitives/presets.txt"; }

// Fewer facets for the sketch and the icons: they are a few hundred pixels wide.
static Values sketch_values(const Values& in, int cap)
{
    Values v = in;
    for (const char* k : {"seg", "res", "sides"}) {
        auto it = v.find(k);
        if (it != v.end())
            it->second = std::min(it->second, double(cap));
    }
    return v;
}

GLGizmoPrimitiveGenerator::GLGizmoPrimitiveGenerator(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

bool GLGizmoPrimitiveGenerator::on_init()
{
    m_shortcut_key = 0;
    m_values       = defaults(shapes().front());
    return true;
}

std::string GLGizmoPrimitiveGenerator::on_get_name() const { return _u8L("Primitive Generator"); }

void GLGizmoPrimitiveGenerator::on_set_state()
{
    if (get_state() == On) {
        m_target   = nullptr;
        m_detached = false;
        load_presets();
    } else if (get_state() == Off) {
        m_target = nullptr;
    }
}

void GLGizmoPrimitiveGenerator::data_changed(bool /*is_serializing*/)
{
    // Nothing to drop: find_target() rebuilds the target from the selection every frame and never
    // reads m_target before finding it in the model (undo, delete and reload are covered there).
}

const Shape& GLGizmoPrimitiveGenerator::shape() const
{
    const auto& all = shapes();
    return all[std::clamp(m_shape, 0, int(all.size()) - 1)];
}

void GLGizmoPrimitiveGenerator::select_shape(int idx)
{
    m_shape  = std::clamp(idx, 0, int(shapes().size()) - 1);
    m_values = defaults(shape());
    if (m_target != nullptr)
        m_detached = true; // the selected one stays as it is; this is a new shape
}

// -----------------------------------------------------------------------------
// Target: the selected volume that was made here
// -----------------------------------------------------------------------------

void GLGizmoPrimitiveGenerator::find_target()
{
    const Selection& sel = m_parent.get_selection();
    Model&           m   = pg_model();
    ModelVolume*     t   = nullptr;
    const int        oi  = sel.get_object_idx();
    if (oi >= 0 && oi < (int) m.objects.size() && !sel.is_empty()) {
        ModelObject* mo = m.objects[oi];
        if (sel.is_single_volume_or_modifier()) {
            const GLVolume* gv = sel.get_first_volume();
            const int       vi = gv != nullptr ? gv->volume_idx() : -1;
            if (vi >= 0 && vi < (int) mo->volumes.size() && !mo->volumes[vi]->neotko_primitive_recipe.empty())
                t = mo->volumes[vi];
        } else if (sel.is_single_full_instance() && mo->volumes.size() == 1 &&
                   !mo->volumes.front()->neotko_primitive_recipe.empty()) {
            t = mo->volumes.front();
        } else if (sel.is_single_full_instance() && mo->volumes.size() > 1 &&
                   NeoScad::is_recipe(mo->volumes.front()->neotko_primitive_recipe)) {
            // a multi-part OpenSCAD shape: every part carries the same recipe
            bool same = true;
            for (const ModelVolume* v : mo->volumes)
                same &= v->neotko_primitive_recipe == mo->volumes.front()->neotko_primitive_recipe;
            if (same)
                t = mo->volumes.front();
        }
    }
    if (t == m_target) {
        // Same volume, recipe changed from outside (undo / redo): follow it, unless a drag is on.
        if (t != nullptr && !m_detached && NeoScad::is_recipe(t->neotko_primitive_recipe)) {
            if (!m_drag_snapshot && !m_scad_pending && t->neotko_primitive_recipe != m_scad_mesh_recipe &&
                m_runner.state() != NeoScad::Runner::State::Running) {
                m_target = nullptr; // re-attach below: reads the script and the numbers again
                find_target();
            }
            return;
        }
        if (t != nullptr && !m_detached && !m_drag_snapshot && t->neotko_primitive_recipe != encode(shape().id, m_values)) {
            std::string id;
            Values      v;
            if (decode(t->neotko_primitive_recipe, id, v))
                for (int i = 0; i < (int) shapes().size(); ++i)
                    if (shapes()[i].id == id) { m_shape = i; m_values = v; }
        }
        return;
    }
    m_target       = t;
    m_target_multi = false;
    m_target_obj   = t != nullptr ? oi : -1;
    m_detached   = false;
    if (t != nullptr && NeoScad::is_recipe(t->neotko_primitive_recipe)) {
        std::string                        file;
        std::map<std::string, std::string> vals;
        if (NeoScad::decode(t->neotko_primitive_recipe, file, vals)) {
            m_scad_mode = true;
            // The 3mf carries the script. If our copy of that file is missing or different, the
            // project's own text wins: it goes to the projects folder and asks before running.
            std::string path = NeoScad::find_script(file);
            const std::string& carried = t->neotko_primitive_script;
            if (!carried.empty() && (path.empty() || NeoScad::read_file(path) != carried))
                path = NeoScad::materialize(file, carried);
            scad_reload_list();
            int idx = -1;
            for (int i = 0; i < (int) m_scripts.size(); ++i)
                if (m_scripts[i].path == path) idx = i;
            if (idx >= 0) {
                scad_select(idx, &vals);
                // show what is on the bed, no run needed (all its parts, where they sit)
                m_target_multi = t->get_object()->volumes.size() > 1;
                m_scad_mesh.clear();
                for (const ModelVolume* v : t->get_object()->volumes) {
                    if (v->neotko_primitive_recipe != t->neotko_primitive_recipe) continue;
                    indexed_triangle_set p = v->mesh().its;
                    its_transform(p, v->get_matrix());
                    its_merge(m_scad_mesh, p);
                }
                m_scad_parts.clear();
                m_scad_mesh_recipe = t->neotko_primitive_recipe;
                ++m_scad_mesh_serial;
                m_scad_pending     = false;
                m_scad_error.clear();
            } else {
                m_script = -1;
                m_scad_params.clear();
                m_scad_error = _u8L("The script of this shape is not in your scripts folder:") + " " + file;
            }
        }
    } else if (t != nullptr) {
        std::string id;
        Values      v;
        m_scad_mode = false;
        if (decode(t->neotko_primitive_recipe, id, v)) {
            const auto& all = shapes();
            for (int i = 0; i < (int) all.size(); ++i)
                if (all[i].id == id) {
                    m_shape  = i;
                    m_values = v;
                }
        }
    }
}

void GLGizmoPrimitiveGenerator::apply_to_target()
{
    if (m_target == nullptr || m_detached)
        return;
    Model& m = pg_model();
    if (m_target_obj < 0 || m_target_obj >= (int) m.objects.size())
        return;
    ModelObject* mo = m.objects[m_target_obj];
    if (std::find(mo->volumes.begin(), mo->volumes.end(), m_target) == mo->volumes.end())
        return;

    apply_mesh_to_target(build(shape().id, m_values), encode(shape().id, m_values));
}

void GLGizmoPrimitiveGenerator::apply_mesh_to_target(indexed_triangle_set its, const std::string& recipe)
{
    if (m_target == nullptr || m_detached || its.indices.empty())
        return;
    Model& m = pg_model();
    if (m_target_obj < 0 || m_target_obj >= (int) m.objects.size())
        return;
    ModelObject* mo = m.objects[m_target_obj];
    if (std::find(mo->volumes.begin(), mo->volumes.end(), m_target) == mo->volumes.end())
        return;
    // Same local frame as add_volume(): geometry centred on its bounding box, so the part grows
    // around where it is instead of jumping.
    Vec3f mn = Vec3f::Constant(1e30f), mx = Vec3f::Constant(-1e30f);
    for (const Vec3f& p : its.vertices) { mn = mn.cwiseMin(p); mx = mx.cwiseMax(p); }
    const Vec3f c = 0.5f * (mn + mx);
    for (Vec3f& p : its.vertices) p -= c;

    if (!m_drag_snapshot) {
        wxGetApp().plater()->take_snapshot("Primitive Generator: edit");
        m_drag_snapshot = true;
    }
    ModelVolume* mv = m_target;
    mv->set_mesh(std::move(its));
    // Painting is stored per triangle of the old mesh: it can't follow a new one.
    mv->supported_facets.reset();
    mv->seam_facets.reset();
    mv->mmu_segmentation_facets.reset();
    mv->fuzzy_skin_facets.reset();
    mv->colorstitch_paint_facets.reset();
    mv->texture_bump_paint_facets.reset();
    mv->calculate_convex_hull();
    mv->invalidate_convex_hull_2d();
    mv->set_new_unique_id();
    mv->neotko_primitive_recipe = recipe;
    mo->invalidate_bounding_box();
    if (mv->is_model_part() && !gravity_allow_free_z())
        mo->ensure_on_bed();
    wxGetApp().plater()->changed_mesh(m_target_obj);
}

// The loaded filament whose colour is closest to `hex` ("#rrggbb"), 1-based; 0 if no colour.
static int nearest_filament(const std::string& hex)
{
    auto rgb = [](const std::string& h, int& r, int& g, int& b) {
        if (h.size() < 7 || h[0] != '#') return false;
        r = std::strtol(h.substr(1, 2).c_str(), nullptr, 16);
        g = std::strtol(h.substr(3, 2).c_str(), nullptr, 16);
        b = std::strtol(h.substr(5, 2).c_str(), nullptr, 16);
        return true;
    };
    int r, g, b;
    if (!rgb(hex, r, g, b) || wxGetApp().preset_bundle == nullptr)
        return 0;
    const auto* cols = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (cols == nullptr || cols->values.empty())
        return 0;
    int    best = 0;
    double best_d = 1e30;
    for (size_t i = 0; i < cols->values.size(); ++i) {
        int fr, fg, fb;
        if (!rgb(cols->values[i], fr, fg, fb)) continue;
        // "redmean" distance: cheap and close enough to how we see colour
        const double rm = 0.5 * (r + fr), dr = r - fr, dg = g - fg, db = b - fb;
        const double d  = (2. + rm / 256.) * dr * dr + 4. * dg * dg + (2. + (255. - rm) / 256.) * db * db;
        if (d < best_d) { best_d = d; best = int(i) + 1; }
    }
    return best;
}

// A multi-part shape got new pieces: each part takes the piece with its name, placed in the
// script's frame the same way neotko_add_primitive_parts() placed them. Filaments stay as they are.
void GLGizmoPrimitiveGenerator::apply_parts_to_target(const std::vector<NeoScad::Part>& parts, const std::string& recipe)
{
    if (m_target == nullptr || m_detached || parts.empty())
        return;
    Model& m = pg_model();
    if (m_target_obj < 0 || m_target_obj >= (int) m.objects.size())
        return;
    ModelObject* mo = m.objects[m_target_obj];
    if (std::find(mo->volumes.begin(), mo->volumes.end(), m_target) == mo->volumes.end())
        return;
    BoundingBoxf3 all;
    for (const NeoScad::Part& p : parts)
        for (const Vec3f& v : p.its.vertices) all.merge(v.cast<double>());
    const Vec3d shift(-all.center().x(), -all.center().y(), -all.min.z());

    if (!m_drag_snapshot) {
        wxGetApp().plater()->take_snapshot("Primitive Generator: edit");
        m_drag_snapshot = true;
    }
    const std::string old_recipe = m_target->neotko_primitive_recipe;
    for (const NeoScad::Part& p : parts) {
        ModelVolume* mv = nullptr;
        for (ModelVolume* v : mo->volumes)
            if (v->neotko_primitive_recipe == old_recipe && v->name == p.label) mv = v;
        if (mv == nullptr)
            continue; // a piece the object doesn't have: the script changed its parts
        indexed_triangle_set its = p.its;
        Vec3f mn = Vec3f::Constant(1e30f), mx = Vec3f::Constant(-1e30f);
        for (Vec3f& v : its.vertices) { v += shift.cast<float>(); mn = mn.cwiseMin(v); mx = mx.cwiseMax(v); }
        const Vec3f c = 0.5f * (mn + mx);
        for (Vec3f& v : its.vertices) v -= c;
        mv->set_mesh(std::move(its));
        mv->supported_facets.reset();
        mv->seam_facets.reset();
        mv->mmu_segmentation_facets.reset();
        mv->fuzzy_skin_facets.reset();
        mv->colorstitch_paint_facets.reset();
        mv->texture_bump_paint_facets.reset();
        mv->calculate_convex_hull();
        mv->invalidate_convex_hull_2d();
        mv->set_new_unique_id();
        mv->set_offset(c.cast<double>());
    }
    for (ModelVolume* v : mo->volumes)
        if (v->neotko_primitive_recipe == old_recipe) {
            v->neotko_primitive_recipe = recipe;
            v->neotko_primitive_script = m_scad_source;
        }
    mo->invalidate_bounding_box();
    if (!gravity_allow_free_z())
        mo->ensure_on_bed();
    wxGetApp().plater()->changed_mesh(m_target_obj);
}

void GLGizmoPrimitiveGenerator::add_new()
{
    if (m_scad_mode) {
        if (m_script < 0 || m_scad_mesh.indices.empty() || m_scad_mesh_recipe != scad_recipe())
            return;
        if (m_scad_parts.size() > 1 && m_add_as == 0) {
            // one object, one part per piece, each on the filament closest to its colour
            std::vector<ObjectList::NeoPrimPart> parts;
            for (const NeoScad::Part& p : m_scad_parts)
                parts.push_back({p.label, TriangleMesh(p.its), nearest_filament(p.color)});
            if (ObjectList* ol = wxGetApp().obj_list())
                ol->neotko_add_primitive_parts(std::move(parts), m_scripts[m_script].name, scad_recipe(), m_scad_source);
            m_target   = nullptr;
            m_detached = false;
            return;
        }
        indexed_triangle_set its = m_scad_mesh;
        // sit on the bed, centred, like the built-in shapes
        Vec3f mn = Vec3f::Constant(1e30f), mx = Vec3f::Constant(-1e30f);
        for (const Vec3f& p : its.vertices) { mn = mn.cwiseMin(p); mx = mx.cwiseMax(p); }
        const Vec3f off(-0.5f * (mn.x() + mx.x()), -0.5f * (mn.y() + mx.y()), -mn.z());
        for (Vec3f& p : its.vertices) p += off;
        const ModelVolumeType t = m_add_as == 1 ? ModelVolumeType::MODEL_PART
                                : m_add_as == 2 ? ModelVolumeType::NEGATIVE_VOLUME
                                : m_add_as == 3 ? ModelVolumeType::PARAMETER_MODIFIER
                                                : ModelVolumeType::INVALID;
        if (ObjectList* ol = wxGetApp().obj_list())
            if (ModelVolume* mv = ol->neotko_add_primitive(TriangleMesh(std::move(its)), m_scripts[m_script].name, t, scad_recipe())) {
                mv->neotko_primitive_script = m_scad_source; // the 3mf carries its own factory
                const int ex = m_scad_parts.size() == 1 ? nearest_filament(m_scad_parts.front().color) : 0;
                if (ex > 0 && t == ModelVolumeType::INVALID)
                    mv->get_object()->config.set_key_value("extruder", new ConfigOptionInt(ex));
                else if (ex > 0 && t == ModelVolumeType::MODEL_PART)
                    mv->config.set_key_value("extruder", new ConfigOptionInt(ex));
                if (ex > 0)
                    wxGetApp().plater()->changed_object(*mv->get_object());
            }
        m_target   = nullptr;
        m_detached = false;
        return;
    }
    indexed_triangle_set its = build(shape().id, m_values);
    if (its.indices.empty())
        return;
    const ModelVolumeType t = m_add_as == 1 ? ModelVolumeType::MODEL_PART
                            : m_add_as == 2 ? ModelVolumeType::NEGATIVE_VOLUME
                            : m_add_as == 3 ? ModelVolumeType::PARAMETER_MODIFIER
                                            : ModelVolumeType::INVALID;
    if (ObjectList* ol = wxGetApp().obj_list())
        ol->neotko_add_primitive(TriangleMesh(std::move(its)), _u8L(shape().name.c_str()), t, encode(shape().id, m_values));
    m_target   = nullptr; // picked up again from the new selection
    m_detached = false;
}

void GLGizmoPrimitiveGenerator::vary()
{
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<double> u(-0.5, 0.5);
    for (const Param& p : shape().params) {
        if (p.key == "seg" || p.key == "res")
            continue;
        double& x = m_values[p.key];
        if (!p.choices.empty())
            x = double(std::uniform_int_distribution<int>(0, int(p.choices.size()) - 1)(rng));
        else
            x += u(rng) * 0.35 * (p.mx - p.mn);
    }
    clamp_values(shape(), m_values);
}

// -----------------------------------------------------------------------------
// Presets: data_dir()/primitives/presets.txt, one "name|recipe" per line
// -----------------------------------------------------------------------------

void GLGizmoPrimitiveGenerator::load_presets()
{
    m_presets.clear();
    boost::nowide::ifstream f(presets_path());
    std::string line;
    while (std::getline(f, line)) {
        const size_t bar = line.find('|');
        if (bar == std::string::npos || bar == 0)
            continue;
        std::string recipe = line.substr(bar + 1);
        while (!recipe.empty() && (recipe.back() == '\r' || recipe.back() == ' '))
            recipe.pop_back();
        std::string id;
        Values      v;
        if (decode(recipe, id, v))
            m_presets.emplace_back(line.substr(0, bar), recipe);
    }
}

void GLGizmoPrimitiveGenerator::save_presets() const
{
    boost::system::error_code ec;
    boost::filesystem::create_directories(boost::filesystem::path(presets_path()).parent_path(), ec);
    boost::nowide::ofstream f(presets_path(), std::ios::trunc);
    for (const auto& p : m_presets)
        f << p.first << '|' << p.second << '\n';
}

// -----------------------------------------------------------------------------
// Sketch: the mesh drawn with ImGui, flat shaded, back to front
// -----------------------------------------------------------------------------

// Flat-shaded projection of `its` for the ImGui sketch (out.size must be set: it centres the height).
static void project_sketch_impl(const indexed_triangle_set& its, float yaw, std::vector<std::array<float, 2>>& pts,
                                std::vector<std::array<int, 3>>& tris_out, std::vector<float>& shade_out,
                                float& x0, float& x1, float& y0, float& y1, float zc)
{
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float pitch = 0.55f, cp = std::cos(pitch), sp = std::sin(pitch);
    std::vector<Vec3f> r(its.vertices.size());
    for (size_t i = 0; i < its.vertices.size(); ++i) {
        const Vec3f& p = its.vertices[i];
        r[i] = Vec3f(p.x() * cy - p.y() * sy, p.x() * sy + p.y() * cy, p.z() - zc);
    }
    const Vec3f view(0.f, cp, -sp); // looking forward and down
    const Vec3f light = Vec3f(-0.45f, -0.55f, 0.7f).normalized();
    pts.resize(r.size());
    x0 = y0 = 1e30f;
    x1 = y1 = -1e30f;
    for (size_t i = 0; i < r.size(); ++i) {
        const float X = r[i].x(), Y = -(r[i].z() * cp + r[i].y() * sp);
        pts[i] = {X, Y};
        x0 = std::min(x0, X); x1 = std::max(x1, X);
        y0 = std::min(y0, Y); y1 = std::max(y1, Y);
    }
    std::vector<std::pair<float, int>> order;
    std::vector<float>                 shade(its.indices.size(), 0.f);
    for (int f = 0; f < (int) its.indices.size(); ++f) {
        const auto& t = its.indices[f];
        const Vec3f a = r[t[0]], b = r[t[1]], c = r[t[2]];
        Vec3f n = (b - a).cross(c - a);
        const float len = n.norm();
        if (len < 1e-12f)
            continue;
        n /= len;
        if (n.dot(view) >= 0.f) // facing away
            continue;
        shade[f] = 0.22f + 0.78f * std::max(0.f, n.dot(light));
        order.emplace_back(((a + b + c) / 3.f).dot(view), f);
    }
    std::sort(order.begin(), order.end(), [](const auto& l, const auto& rr) { return l.first > rr.first; });
    tris_out.clear();
    shade_out.clear();
    for (const auto& o : order) {
        const auto& t = its.indices[o.second];
        tris_out.push_back({int(t[0]), int(t[1]), int(t[2])});
        shade_out.push_back(shade[o.second]);
    }
}

#define project_sketch(ITS, YAW, OUT) \
    project_sketch_impl((ITS), (YAW), (OUT).pts, (OUT).tris, (OUT).shade, (OUT).x0, (OUT).x1, (OUT).y0, (OUT).y1, 0.5f * float((OUT).size.z()))

void GLGizmoPrimitiveGenerator::sketch_from_mesh(const indexed_triangle_set& its, const std::string& key, float yaw, Sketch& out)
{
    if (out.recipe == key && std::abs(out.yaw - yaw) < 1e-4f)
        return;
    const bool same = out.recipe == key;
    out.recipe = key;
    out.yaw    = yaw;
    // the mesh as it is, moved to sit on z = 0 and centred, like the built-in shapes
    indexed_triangle_set m = its;
    Vec3f mn = Vec3f::Constant(1e30f), mx = Vec3f::Constant(-1e30f);
    for (const Vec3f& p : m.vertices) { mn = mn.cwiseMin(p); mx = mx.cwiseMax(p); }
    if (m.vertices.empty()) { out.tris.clear(); out.shade.clear(); out.pts.clear(); out.triangles = 0; out.volume = 0.; out.size = Vec3d::Zero(); return; }
    const Vec3f off(-0.5f * (mn.x() + mx.x()), -0.5f * (mn.y() + mx.y()), -mn.z());
    for (Vec3f& p : m.vertices) p += off;
    if (!same) {
        out.triangles = m.indices.size();
        out.volume    = double(its_volume(m));
        out.size      = Vec3d((mx - mn).cast<double>());
    }
    project_sketch(m, yaw, out);
}

void GLGizmoPrimitiveGenerator::build_sketch(const std::string& id, const Values& v, float yaw, Sketch& out)
{
    const std::string recipe = encode(id, v);
    if (out.recipe == recipe && std::abs(out.yaw - yaw) < 1e-4f)
        return;
    const bool same_shape = out.recipe == recipe;
    out.recipe = recipe;
    out.yaw    = yaw;
    if (!same_shape) {
        const indexed_triangle_set full = build(id, v);
        out.triangles = full.indices.size();
        out.volume    = full.indices.empty() ? 0. : double(its_volume(full));
        Vec3f mn = Vec3f::Constant(1e30f), mx = Vec3f::Constant(-1e30f);
        for (const Vec3f& p : full.vertices) { mn = mn.cwiseMin(p); mx = mx.cwiseMax(p); }
        out.size = full.vertices.empty() ? Vec3d::Zero() : Vec3d((mx - mn).cast<double>());
    }
    const indexed_triangle_set its = build(id, sketch_values(v, 48));
    project_sketch(its, yaw, out);
}

void GLGizmoPrimitiveGenerator::draw_sketch(ImDrawList* dl, const Sketch& s, float ax, float ay, float bx, float by, bool lit)
{
    if (s.tris.empty() || s.x1 <= s.x0 || s.y1 <= s.y0)
        return;
    const float  k  = 0.86f * std::min((bx - ax) / (s.x1 - s.x0), (by - ay) / (s.y1 - s.y0));
    const float  ox = 0.5f * (ax + bx) - k * 0.5f * (s.x0 + s.x1);
    const float  oy = 0.5f * (ay + by) - k * 0.5f * (s.y0 + s.y1);
    const ImVec4 base = neo_col(lit ? NeoCol::AccentBright : NeoCol::TextDim);
    for (size_t i = 0; i < s.tris.size(); ++i) {
        const float  l = s.shade[i];
        const ImU32  col = ImGui::ColorConvertFloat4ToU32(ImVec4(std::min(1.f, base.x * l + 0.04f), std::min(1.f, base.y * l + 0.04f),
                                                                 std::min(1.f, base.z * l + 0.04f), 1.f));
        const auto& t = s.tris[i];
        const ImVec2 a(ox + k * s.pts[t[0]][0], oy + k * s.pts[t[0]][1]);
        const ImVec2 b(ox + k * s.pts[t[1]][0], oy + k * s.pts[t[1]][1]);
        const ImVec2 c(ox + k * s.pts[t[2]][0], oy + k * s.pts[t[2]][1]);
        dl->AddTriangleFilled(a, b, c, col);
    }
}

// -----------------------------------------------------------------------------
// Panel
// -----------------------------------------------------------------------------

static std::string fmt_num(double x, bool integer)
{
    char buf[32];
    if (integer)
        std::snprintf(buf, sizeof(buf), "%d", int(std::lround(x)));
    else
        std::snprintf(buf, sizeof(buf), "%.2f", x);
    std::string s = buf;
    if (!integer && s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

void GLGizmoPrimitiveGenerator::on_render_input_window(float x, float y, float bottom_limit)
{
    find_target();

    const float u     = neo_u();
    const float win_w = 21.f * u;
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    ImGui::SetNextWindowSizeConstraints(ImVec2(win_w, 0.f), ImVec2(win_w, FLT_MAX));
    neo_push_window_style();
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);
    ImGui::PushTextWrapPos(win_w - 16.f);
    neo_push_panel_style();

    ImDrawList* dl      = ImGui::GetWindowDrawList();
    const auto& all     = shapes();
    bool        changed = false;

    // ---- shape icons ---------------------------------------------------------------------------
    if (m_icons.size() != all.size()) {
        m_icons.assign(all.size(), Sketch());
        for (size_t i = 0; i < all.size(); ++i)
            build_sketch(all[i].id, sketch_values(defaults(all[i]), 20), 0.6f, m_icons[i]);
    }
    const float icon = 2.05f * u, gap = 0.25f * u;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int   per_row = std::max(1, int((avail + gap) / (icon + gap)));
    for (int i = 0; i < (int) all.size(); ++i) {
        ImGui::PushID(i);
        if (i % per_row != 0)
            ImGui::SameLine(0.f, gap);
        if (ImGui::InvisibleButton("##shape", ImVec2(icon, icon)) && (i != m_shape || m_scad_mode)) {
            if (m_scad_mode && i == m_shape) { // back from OpenSCAD to the same shape: keep its numbers
                m_scad_mode = false;
                if (m_target != nullptr) m_detached = true;
            } else
                select_shape(i);
            m_scad_mode = false;
        }
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool   hv = ImGui::IsItemHovered();
        const bool   on = !m_scad_mode && i == m_shape;
        dl->AddRectFilled(a, b, on ? neo_fade(NeoCol::Accent, 0.30f) : neo_col_u32(hv ? NeoCol::SurfaceHi : NeoCol::Surface), 5.f);
        if (on)
            dl->AddRect(a, b, neo_col_u32(NeoCol::AccentBright), 5.f, 0, 1.2f);
        draw_sketch(dl, m_icons[i], a.x + 3.f, a.y + 3.f, b.x - 3.f, b.y - 3.f, on || hv);
        if (hv)
            ImGui::SetTooltip("%s\n%s", _u8L(all[i].name.c_str()).c_str(), _u8L(all[i].tip.c_str()).c_str());
        ImGui::PopID();
    }
    { // the OpenSCAD tile, last in the row
        const int i = int(all.size());
        if (i % per_row != 0)
            ImGui::SameLine(0.f, gap);
        if (ImGui::InvisibleButton("##scad", ImVec2(icon, icon)) && !m_scad_mode) {
            m_scad_mode = true;
            if (m_target != nullptr && !NeoScad::is_recipe(m_target->neotko_primitive_recipe))
                m_detached = true;
            scad_reload_list();
        }
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool   hv = ImGui::IsItemHovered();
        dl->AddRectFilled(a, b, m_scad_mode ? neo_fade(NeoCol::Accent, 0.30f) : neo_col_u32(hv ? NeoCol::SurfaceHi : NeoCol::Surface), 5.f);
        if (m_scad_mode)
            dl->AddRect(a, b, neo_col_u32(NeoCol::AccentBright), 5.f, 0, 1.2f);
        const char*  code = "</>";
        const ImVec2 ts   = ImGui::CalcTextSize(code);
        dl->AddText(ImVec2(0.5f * (a.x + b.x - ts.x), 0.5f * (a.y + b.y - ts.y)),
                    neo_col_u32(m_scad_mode || hv ? NeoCol::AccentBright : NeoCol::TextDim), code);
        if (hv)
            ImGui::SetTooltip("%s", _u8L("OpenSCAD scripts\nYour own .scad files, with their Customizer numbers as sliders.").c_str());
    }
    if (m_scad_mode)
        scad_poll();

    // ---- sketch ----------------------------------------------------------------------------------
    ImGui::Spacing();
    if (m_scad_mode)
        sketch_from_mesh(m_scad_mesh, m_scad_mesh_recipe + "#" + std::to_string(m_scad_mesh_serial), m_yaw, m_scad_sketch);
    else
        build_sketch(shape().id, m_values, m_yaw, m_sketch);
    const Sketch& sk = m_scad_mode ? m_scad_sketch : m_sketch;
    {
        const float  w  = ImGui::GetContentRegionAvail().x, h = 8.5f * u;
        ImGui::InvisibleButton("##sketch", ImVec2(w, h));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        neo_draw_canvas(dl, a, w, h);
        if (ImGui::IsItemActive())
            m_yaw += ImGui::GetIO().MouseDelta.x * 0.012f;
        draw_sketch(dl, sk, a.x + 0.4f * u, a.y + 0.4f * u, b.x - 0.4f * u, b.y - (m_scad_mode ? 0.4f : 1.6f) * u,
                    !m_scad_mode || m_scad_mesh_recipe == scad_recipe());
        if (m_scad_mode && (m_scad_pending || m_runner.state() == NeoScad::Runner::State::Running)) {
            const std::string busy = _u8L("OpenSCAD is working…");
            const ImVec2      ts   = ImGui::CalcTextSize(busy.c_str());
            dl->AddText(ImVec2(b.x - ts.x - 0.5f * u, a.y + 0.4f * u), neo_col_u32(NeoCol::AccentBright), busy.c_str());
        }
        // numbered sizes, in the order of their sliders
        float tx = a.x + 0.5f * u;
        const float ty = b.y - 1.25f * u;
        for (const Param& p : shape().params) {
            if (p.mark <= 0 || m_scad_mode)
                continue;
            char num[4];
            std::snprintf(num, sizeof(num), "%d", p.mark);
            const ImVec2 c(tx + 0.45f * u, ty + 0.45f * u);
            dl->AddCircleFilled(c, 0.45f * u, neo_col_u32(NeoCol::Surface));
            dl->AddCircle(c, 0.45f * u, neo_col_u32(NeoCol::TextDim), 0, 1.f);
            const float  fs = 0.62f * u;
            const ImVec2 ns = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.f, num);
            dl->AddText(ImGui::GetFont(), fs, ImVec2(c.x - 0.5f * ns.x, c.y - 0.5f * ns.y), neo_col_u32(NeoCol::Ink), num);
            const std::string val = fmt_num(m_values[p.key], p.integer) + (p.unit.empty() ? "" : " " + p.unit);
            dl->AddText(ImVec2(c.x + 0.6f * u, ty + 0.1f * u), neo_col_u32(NeoCol::TextDim), val.c_str());
            tx += 1.4f * u + ImGui::CalcTextSize(val.c_str()).x + 0.5f * u;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Drag to turn it.").c_str());
    }
    {
        char info[160];
        std::snprintf(info, sizeof(info), "%.1f x %.1f x %.1f mm  ·  %zu %s  ·  ~%.1f g",
                      sk.size.x(), sk.size.y(), sk.size.z(), sk.triangles,
                      _u8L("triangles").c_str(), sk.volume / 1000. * 1.24);
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextUnformatted(info);
        ImGui::PopStyleColor();
    }

    // ---- where the numbers go ----------------------------------------------------------------------
    const bool editing = m_target != nullptr && !m_detached;
    if (editing)
        neo_status_chip(fit_text(_u8L("Editing") + ": " + m_target->name, win_w - 3.f * u).c_str(), neo_col_u32(NeoCol::AccentBright), true);
    else {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextWrapped("%s", _u8L("Set the numbers, then add it. Select a shape made here to change it again.").c_str());
        ImGui::PopStyleColor();
    }

    bool scad_add = false;
    if (m_scad_mode)
        render_scad(win_w, scad_add);
    if (!m_scad_mode) {
    // ---- parameters ----------------------------------------------------------------------------------
    neo_section(_u8L(shape().name.c_str()).c_str());
    const float label_col = 8.8f * u;
    for (const Param& p : shape().params) {
        ImGui::PushID(p.key.c_str());
        const std::string caption = (p.mark > 0 ? std::to_string(p.mark) + " · " : std::string()) + _u8L(p.label.c_str());
        const char*       tip     = p.tip.empty() ? nullptr : p.tip.c_str();
        if (!p.choices.empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(caption.c_str());
            ImGui::PopStyleColor();
            const int cur = int(std::lround(m_values[p.key]));
            const float row_x = ImGui::GetItemRectMin().x + label_col;
            float x_next = row_x;
            for (int c = 0; c < (int) p.choices.size(); ++c) {
                const std::string lbl = _u8L(p.choices[c].c_str()) + "##c" + std::to_string(c);
                const float bw = ImGui::CalcTextSize(_u8L(p.choices[c].c_str()).c_str()).x + 1.2f * u;
                if (c > 0 && x_next + bw > ImGui::GetWindowPos().x + win_w - 0.6f * u) { // wrap under the first chip
                    x_next = row_x;
                    ImGui::SetCursorScreenPos(ImVec2(row_x, ImGui::GetItemRectMax().y + 0.25f * u));
                } else
                    neo_same_line_at(x_next);
                if (neo_text_button(lbl.c_str(), c == cur ? NeoBtn::Accent : NeoBtn::Ghost) && c != cur) {
                    m_values[p.key] = c;
                    changed         = true;
                }
                x_next = ImGui::GetItemRectMax().x + 0.25f * u;
            }
        } else {
            float v = float(m_values[p.key]);
            std::string fmt = p.integer ? "%.0f" : "%.2f";
            if (!p.unit.empty())
                fmt += " " + p.unit;
            if (neo_row_slider("##s", caption.c_str(), &v, float(p.mn), float(p.mx), fmt.c_str(), tip, label_col)) {
                m_values[p.key] = p.integer ? std::round(v) : double(v);
                changed         = true;
            }
        }
        ImGui::PopID();
    }
    if (changed) {
        clamp_values(shape(), m_values);
        apply_to_target();
    }

    ImGui::Spacing();
    if (neo_text_button(_u8L("Vary").c_str(), NeoBtn::Ghost)) {
        vary();
        apply_to_target();
        m_drag_snapshot = false; // one undo step per click
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Random numbers around the current ones. Click again for more.").c_str());
    ImGui::SameLine();
    if (neo_text_button(_u8L("Defaults").c_str(), NeoBtn::Ghost)) {
        m_values = defaults(shape());
        apply_to_target();
        m_drag_snapshot = false;
    }

    } // !m_scad_mode

    // ---- add as ----------------------------------------------------------------------------------
    neo_section(_u8L("Add as").c_str());
    const bool has_object = m_parent.get_selection().get_object_idx() >= 0;
    {
        const char* names[4] = {"object", "part", "negative", "modifier"};
        for (int i = 0; i < 4; ++i) {
            if (i > 0)
                ImGui::SameLine(0.f, 0.25f * u);
            const std::string lbl = _u8L(names[i]) + "##as" + std::to_string(i);
            if (neo_text_button(lbl.c_str(), i == m_add_as ? NeoBtn::Accent : NeoBtn::Ghost))
                m_add_as = i;
        }
    }
    if (m_add_as != 0 && !has_object)
        neo_warn_row("##pg_noobj", _u8L("Select an object first: a part goes inside it.").c_str(),
                     _u8L("Without a selected object it is added as a new object.").c_str());

    if (!m_scad_mode) {
    // ---- presets ---------------------------------------------------------------------------------
    neo_section(_u8L("My presets").c_str());
    int load_idx = -1, del_idx = -1;
    for (int i = 0; i < (int) m_presets.size(); ++i) {
        ImGui::PushID(i);
        const float  row_w = ImGui::GetContentRegionAvail().x, row_h = 1.5f * u;
        const ImVec2 p0    = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##pr", ImVec2(row_w - 1.6f * u, row_h)))
            load_idx = i;
        const bool hv = ImGui::IsItemHovered();
        if (hv)
            dl->AddRectFilled(p0, ImVec2(p0.x + row_w, p0.y + row_h), neo_col_u32(NeoCol::Surface), 4.f);
        std::string sid;
        Values      sv;
        decode(m_presets[i].second, sid, sv);
        const Shape* ps = find_shape(sid);
        dl->AddText(ImVec2(p0.x + 0.4f * u, p0.y + 0.2f * u), neo_col_u32(hv ? NeoCol::AccentBright : NeoCol::Ink),
                    fit_text(m_presets[i].first, row_w - 8.f * u).c_str());
        if (ps != nullptr) {
            const std::string kind = _u8L(ps->name.c_str());
            const float       kw   = ImGui::CalcTextSize(kind.c_str()).x;
            dl->AddText(ImVec2(p0.x + row_w - 2.f * u - kw, p0.y + 0.2f * u), neo_col_u32(NeoCol::TextDim), kind.c_str());
        }
        if (hv)
            ImGui::SetTooltip("%s", _u8L("Click to load it.").c_str());
        ImGui::SameLine(0.f, 0.2f * u);
        if (neo_glyph_button("##del", 1.25f * u, Glyph::Trash, true, _u8L("Delete this preset.").c_str()))
            del_idx = i;
        ImGui::PopID();
    }
    if (load_idx >= 0) {
        std::string id;
        Values      v;
        if (decode(m_presets[load_idx].second, id, v))
            for (int i = 0; i < (int) all.size(); ++i)
                if (all[i].id == id) {
                    if (i != m_shape && m_target != nullptr)
                        m_detached = true;
                    m_shape  = i;
                    m_values = v;
                    apply_to_target();
                    m_drag_snapshot = false;
                }
    }
    if (del_idx >= 0) {
        m_presets.erase(m_presets.begin() + del_idx);
        save_presets();
    }
    const std::string save_lbl = _u8L("Save preset");
    const float save_w = ImGui::CalcTextSize(save_lbl.c_str()).x + 0.95f * u + 0.35f * u + 1.2f * u; // as neo_text_button
    ImGui::SetNextItemWidth(std::max(4.f * u, ImGui::GetContentRegionAvail().x - save_w - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##pname", _u8L("Preset name").c_str(), m_preset_name, sizeof(m_preset_name));
    ImGui::SameLine();
    if (neo_text_button(save_lbl.c_str(), NeoBtn::Ghost, true, Glyph::Save)) {
        std::string name = m_preset_name;
        std::replace(name.begin(), name.end(), '|', '/');
        if (name.empty())
            name = _u8L(shape().name.c_str()) + " " + std::to_string(m_presets.size() + 1);
        m_presets.emplace_back(name, encode(shape().id, m_values));
        save_presets();
        m_preset_name[0] = 0;
    }

    } // !m_scad_mode

    // ---- add ---------------------------------------------------------------------------------------
    ImGui::Spacing();
    const std::string add_lbl = editing ? _u8L("Add a copy to the bed") : _u8L("Add to the bed");
    const bool scad_ready = !m_scad_mode || (m_script >= 0 && !m_scad_mesh.indices.empty() && m_scad_mesh_recipe == scad_recipe());
    if (neo_text_button(add_lbl.c_str(), scad_ready ? NeoBtn::Accent : NeoBtn::Ghost, true, Glyph::Plus, ImGui::GetContentRegionAvail().x) && scad_ready)
        add_new();
    if (!scad_ready && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Wait for OpenSCAD to finish the shape.").c_str());

    if (!ImGui::IsAnyItemActive())
        m_drag_snapshot = false; // the drag ended: the next change is a new undo step

    neo_pop_panel_style();
    ImGui::PopTextWrapPos();
    GizmoImguiEnd();
    neo_pop_window_style();
}


// -----------------------------------------------------------------------------
// OpenSCAD scripts (phase 3)
// -----------------------------------------------------------------------------

static long long file_mtime(const std::string& path)
{
    boost::system::error_code ec;
    const auto t = boost::filesystem::last_write_time(path, ec);
    return ec ? 0 : (long long) t;
}

static std::string read_text(const std::string& path)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    std::string             s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

std::string GLGizmoPrimitiveGenerator::scad_recipe() const
{
    if (m_script < 0 || m_script >= (int) m_scripts.size())
        return {};
    return NeoScad::encode(boost::filesystem::path(m_scripts[m_script].path).filename().string(), m_scad_params);
}

void GLGizmoPrimitiveGenerator::scad_reload_list()
{
    const std::string keep = (m_script >= 0 && m_script < (int) m_scripts.size()) ? m_scripts[m_script].path : std::string();
    m_scripts = NeoScad::list_scripts();
    m_script  = -1;
    for (int i = 0; i < (int) m_scripts.size(); ++i)
        if (m_scripts[i].path == keep) m_script = i;
    if (m_scad_binary.empty())
        m_scad_binary = NeoScad::find_openscad();
}

void GLGizmoPrimitiveGenerator::scad_select(int idx, const std::map<std::string, std::string>* values)
{
    if (idx < 0 || idx >= (int) m_scripts.size())
        return;
    if (values == nullptr && m_target != nullptr)
        m_detached = true; // a script picked by hand is a new shape; the selected one stays as it is
    m_script         = idx;
    m_scad_source    = read_text(m_scripts[idx].path);
    m_scad_untrusted = m_scripts[idx].from_project && !NeoScad::is_trusted(m_scad_source);
    m_scad_params    = NeoScad::parse(m_scad_source);
    if (values != nullptr)
        NeoScad::apply(m_scad_params, *values);
    m_scad_mtime = file_mtime(m_scripts[idx].path);
    m_scad_error.clear();
    m_scad_pending   = true;
    m_scad_pending_t = 0.; // run now
}

void GLGizmoPrimitiveGenerator::scad_request()
{
    m_scad_pending   = true;
    m_scad_pending_t = ImGui::GetTime();
}

void GLGizmoPrimitiveGenerator::scad_poll()
{
    const double now = ImGui::GetTime();
    // The script was saved from OpenSCAD (or any editor): read it again, keep the numbers, run.
    if (m_script >= 0 && m_script < (int) m_scripts.size() && now - m_scad_mtime_check > 1.0) {
        m_scad_mtime_check = now;
        const long long t  = file_mtime(m_scripts[m_script].path);
        if (t != 0 && t != m_scad_mtime) {
            std::map<std::string, std::string> keep;
            for (const NeoScad::Param& p : m_scad_params)
                keep[p.name] = NeoScad::value_literal(p);
            const std::string path = m_scripts[m_script].path;
            m_scad_source = read_text(path);
            m_scad_params = NeoScad::parse(m_scad_source);
            NeoScad::apply(m_scad_params, keep);
            m_scad_mtime = t;
            scad_request();
            m_scad_pending_t = 0.;
        }
    }
    // a finished run
    std::vector<NeoScad::Part> parts;
    int                        gen = -1;
    if (m_runner.take(parts, gen) && gen == m_scad_wanted) {
        indexed_triangle_set its;
        for (const NeoScad::Part& p : parts)
            its_merge(its, p.its);
        m_scad_parts       = parts;
        m_scad_mesh        = its;
        m_scad_mesh_recipe = m_scad_run_recipe;
        ++m_scad_mesh_serial;
        m_scad_error.clear();
        if (m_target != nullptr && !m_detached && NeoScad::is_recipe(m_target->neotko_primitive_recipe)) {
            m_drag_snapshot = false; // one undo step per run
            if (m_target_multi)
                apply_parts_to_target(parts, m_scad_run_recipe);
            else
                apply_mesh_to_target(std::move(its), m_scad_run_recipe);
            if (m_target != nullptr)
                m_target->neotko_primitive_script = m_scad_source;
            m_drag_snapshot = false;
        }
    }
    if (m_runner.state() == NeoScad::Runner::State::Failed && m_scad_wanted == m_runner.generation() && !m_scad_pending)
        m_scad_error = m_runner.error();
    // a pending run: after a short pause, and never while a slider is held
    if (m_scad_pending && !m_scad_untrusted && !m_scad_binary.empty() && m_script >= 0 && now - m_scad_pending_t > 0.35 &&
        !ImGui::IsAnyItemActive()) {
        m_scad_pending    = false;
        m_scad_run_recipe = scad_recipe();
        m_runner.start(m_scad_binary, m_scripts[m_script].path, m_scad_params);
        m_scad_wanted = m_runner.generation();
        m_scad_error.clear();
    }
    if ((m_scad_pending && !m_scad_untrusted) || m_runner.state() == NeoScad::Runner::State::Running)
        m_parent.schedule_extra_frame(120);
}

void GLGizmoPrimitiveGenerator::render_scad(float win_w, bool& /*add_clicked*/)
{
    namespace fs   = boost::filesystem;
    const float u  = neo_u();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- the program ----
    neo_section("OpenSCAD");
    if (m_scad_binary.empty()) {
        static double last_look = -10.;
        if (ImGui::GetTime() - last_look > 2.) { // it may have just been installed
            last_look     = ImGui::GetTime();
            m_scad_binary = NeoScad::find_openscad();
        }
    }
    auto choose_binary = [this]() {
        wxFileDialog dlg(wxGetApp().mainframe, _L("Where is OpenSCAD?"), "", "",
#ifdef _WIN32
                         "openscad.exe|openscad.exe|*.*|*.*",
#else
                         "*",
#endif
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK)
            return;
        std::string path = into_u8(dlg.GetPath());
        if (path.size() > 4 && path.substr(path.size() - 4) == ".app")
            path += "/Contents/MacOS/OpenSCAD";
        NeoScad::set_openscad_path(path);
        m_scad_binary = path;
        if (m_script >= 0) { scad_request(); m_scad_pending_t = 0.; }
    };
    if (m_scad_binary.empty()) {
        neo_warn_row("##pg_noscad", _u8L("OpenSCAD is not installed, or not where I can find it.").c_str(),
                     _u8L("OpenSCAD is free. Install it, or show me where it is.\n"
                          "Version 2025 or newer is much faster (Manifold engine).").c_str());
        if (neo_text_button(_u8L("Choose OpenSCAD…").c_str(), NeoBtn::Normal))
            choose_binary();
        ImGui::SameLine();
        if (neo_text_button(_u8L("Get OpenSCAD").c_str(), NeoBtn::Ghost))
            wxLaunchDefaultBrowser("https://openscad.org/downloads.html");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(fit_text(_u8L("Using") + " " + m_scad_binary, win_w - 6.5f * u).c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", m_scad_binary.c_str());
        neo_same_line_at(ImGui::GetWindowPos().x + win_w - 4.6f * u);
        if (neo_text_button(_u8L("Change").c_str(), NeoBtn::Ghost))
            choose_binary();
    }

    // ---- the scripts ----
    neo_section(_u8L("Scripts").c_str());
    int pick = -1;
    for (int i = 0; i < (int) m_scripts.size(); ++i) {
        ImGui::PushID(1000 + i);
        const float  row_w = ImGui::GetContentRegionAvail().x, row_h = 1.45f * u;
        const ImVec2 p0    = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##sc", ImVec2(row_w, row_h)))
            pick = i;
        const bool hv = ImGui::IsItemHovered(), on = i == m_script;
        if (on)
            dl->AddRectFilled(p0, ImVec2(p0.x + row_w, p0.y + row_h), neo_fade(NeoCol::Accent, 0.30f), 4.f);
        else if (hv)
            dl->AddRectFilled(p0, ImVec2(p0.x + row_w, p0.y + row_h), neo_col_u32(NeoCol::Surface), 4.f);
        dl->AddText(ImVec2(p0.x + 0.4f * u, p0.y + 0.2f * u), neo_col_u32(on || hv ? NeoCol::AccentBright : NeoCol::Ink),
                    fit_text(m_scripts[i].name, row_w - 5.f * u).c_str());
        if (m_scripts[i].example || m_scripts[i].from_project) {
            const std::string tag = m_scripts[i].example ? _u8L("example") : _u8L("from a project");
            const float       tw  = ImGui::CalcTextSize(tag.c_str()).x;
            dl->AddText(ImVec2(p0.x + row_w - tw - 0.4f * u, p0.y + 0.2f * u), neo_col_u32(NeoCol::TextDim), tag.c_str());
        }
        ImGui::PopID();
    }
    if (m_scripts.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextWrapped("%s", _u8L("No scripts yet. Import a .scad file.").c_str());
        ImGui::PopStyleColor();
    }
    if (pick >= 0 && pick != m_script)
        scad_select(pick);

    if (neo_text_button(_u8L("Import .scad…").c_str(), NeoBtn::Ghost, true, Glyph::Plus)) {
        wxFileDialog dlg(wxGetApp().mainframe, _L("Import an OpenSCAD script"), "", "", "OpenSCAD (*.scad)|*.scad",
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK) {
            const fs::path src(into_u8(dlg.GetPath()));
            const fs::path dst = fs::path(NeoScad::user_dir()) / src.filename();
            boost::system::error_code ec;
            if (src != dst)
                fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            scad_reload_list();
            for (int i = 0; i < (int) m_scripts.size(); ++i)
                if (m_scripts[i].path == dst.string()) scad_select(i);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Copies the file into your scripts folder.\nScripts that use libraries (BOSL2...) need them installed in OpenSCAD.").c_str());
    ImGui::SameLine();
    if (neo_text_button(_u8L("Folder").c_str(), NeoBtn::Ghost))
        desktop_open_any_folder(NeoScad::user_dir());
    ImGui::SameLine();
    if (neo_text_button(_u8L("Refresh").c_str(), NeoBtn::Ghost))
        scad_reload_list();
    if (m_script >= 0 && !m_scad_binary.empty()) {
        ImGui::SameLine();
        if (neo_text_button(_u8L("Edit").c_str(), NeoBtn::Ghost)) {
            std::string path = m_scripts[m_script].path;
            if (m_scripts[m_script].example) {
                // examples belong to the app: edit a copy in your folder, with its own name so the
                // two never get mixed up (a 3mf finds its script by file name)
                const fs::path dst = fs::path(NeoScad::user_dir()) / (fs::path(path).stem().string() + "_mine.scad");
                boost::system::error_code ec;
                if (!fs::exists(dst, ec))
                    fs::copy_file(path, dst, ec);
                std::map<std::string, std::string> keep;
                for (const NeoScad::Param& p : m_scad_params) keep[p.name] = NeoScad::value_literal(p);
                scad_reload_list();
                for (int i = 0; i < (int) m_scripts.size(); ++i)
                    if (m_scripts[i].path == dst.string()) scad_select(i, &keep);
                path = dst.string();
            }
            try {
                boost::process::spawn(m_scad_binary, path);
            } catch (const std::exception&) {
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Opens the script in OpenSCAD. Save it there and the shape here updates.").c_str());
    }

    if (m_script < 0 || m_script >= (int) m_scripts.size())
        return;

    // ---- its numbers ----
    neo_section(m_scripts[m_script].name.c_str());
    if (m_scad_untrusted) {
        neo_warn_row("##pg_trust", _u8L("This script came inside a project. It won't run until you trust it.").c_str(),
                     _u8L("OpenSCAD scripts can't run programs or change your files,\n"
                          "but only run scripts from people you trust.\n"
                          "Edit opens it in OpenSCAD so you can read it first.").c_str());
        if (neo_text_button(_u8L("Trust and run").c_str(), NeoBtn::Accent)) {
            NeoScad::trust(m_scad_source);
            m_scad_untrusted = false;
            scad_request();
            m_scad_pending_t = 0.;
        }
    }
    if (m_scad_params.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextWrapped("%s", _u8L("This script has no Customizer numbers. Write them at the top as  name = 10; // [1:50]").c_str());
        ImGui::PopStyleColor();
    }
    const float label_col = 8.8f * u;
    bool        changed   = false;
    std::string group;
    const int   part_idx  = NeoScad::part_param(m_scad_params);
    if (part_idx >= 0) {
        // the pieces this script draws, each as its own part, with the filament it will get
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextUnformatted(_u8L("parts").c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("This script draws its pieces one by one (its \"part\" choice).\n"
                                         "Each piece becomes a part of the object, on the filament\n"
                                         "closest to the colour the script gives it. Change it later in the object list.").c_str());
        const auto  pieces = NeoScad::part_choices(m_scad_params[part_idx]);
        float       x      = ImGui::GetItemRectMin().x + label_col;
        const float sw     = 0.8f * u;
        for (size_t i = 0; i < pieces.size(); ++i) {
            std::string col;
            for (const NeoScad::Part& q : m_scad_parts)
                if (q.label == pieces[i].second) col = q.color;
            const int ex = nearest_filament(col);
            std::string txt = pieces[i].second + (ex > 0 ? " T" + std::to_string(ex) : "");
            const float tw  = ImGui::CalcTextSize(txt.c_str()).x;
            if (i > 0 && x + sw + tw + 1.f * u > ImGui::GetWindowPos().x + win_w - 0.6f * u) {
                x = ImGui::GetItemRectMin().x + label_col;
                ImGui::NewLine();
            }
            neo_same_line_at(x);
            ImGui::InvisibleButton(("##pc" + std::to_string(i)).c_str(), ImVec2(sw + 0.3f * u + tw, ImGui::GetFrameHeight()));
            const ImVec2 a = ImGui::GetItemRectMin();
            const float  cy = a.y + 0.5f * ImGui::GetFrameHeight();
            ImU32 fill = neo_col_u32(NeoCol::SurfaceHi);
            if (col.size() == 7)
                fill = IM_COL32(std::strtol(col.substr(1, 2).c_str(), nullptr, 16), std::strtol(col.substr(3, 2).c_str(), nullptr, 16),
                                std::strtol(col.substr(5, 2).c_str(), nullptr, 16), 255);
            dl->AddRectFilled(ImVec2(a.x, cy - 0.5f * sw), ImVec2(a.x + sw, cy + 0.5f * sw), fill, 3.f);
            dl->AddRect(ImVec2(a.x, cy - 0.5f * sw), ImVec2(a.x + sw, cy + 0.5f * sw), neo_col_u32(NeoCol::TextDim), 3.f);
            dl->AddText(ImVec2(a.x + sw + 0.3f * u, cy - 0.5f * ImGui::GetTextLineHeight()), neo_col_u32(NeoCol::Ink), txt.c_str());
            x = ImGui::GetItemRectMax().x + 0.7f * u;
        }
    }
    for (size_t k = 0; k < m_scad_params.size(); ++k) {
        if ((int) k == part_idx)
            continue; // shown above as the parts row
        NeoScad::Param& p = m_scad_params[k];
        if (!p.group.empty() && p.group != group) {
            group = p.group;
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::AccentBright));
            ImGui::TextUnformatted(group.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PushID(int(2000 + k));
        const std::string first = p.description.substr(0, p.description.find('\n'));
        const std::string label = (!first.empty() && first.size() <= 28) ? first : p.name;
        const std::string tip   = p.name + (p.description.empty() ? "" : "\n" + p.description);
        switch (p.type) {
        case NeoScad::Param::Bool: {
            bool b = p.num != 0.;
            if (neo_row_toggle("##b", label.c_str(), &b, tip.c_str(), label_col)) { p.num = b ? 1. : 0.; changed = true; }
            break;
        }
        case NeoScad::Param::Text: {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(fit_text(label, label_col - 0.4f * u).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip.c_str());
            ImGui::SameLine(label_col);
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", p.text.c_str());
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputText("##t", buf, sizeof(buf))) { p.text = buf; changed = true; }
            break;
        }
        case NeoScad::Param::Choice: {
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(fit_text(label, label_col - 0.4f * u).c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip.c_str());
            const int   cur   = int(std::lround(p.num));
            const float row_x = ImGui::GetItemRectMin().x + label_col;
            float       x_next = row_x;
            for (int c = 0; c < (int) p.choices.size(); ++c) {
                const std::string& name = p.choices[c].second;
                const std::string  lbl  = name + "##c" + std::to_string(c);
                const float        bw   = ImGui::CalcTextSize(name.c_str()).x + 1.2f * u;
                if (c > 0 && x_next + bw > ImGui::GetWindowPos().x + win_w - 0.6f * u) {
                    x_next = row_x;
                    ImGui::SetCursorScreenPos(ImVec2(row_x, ImGui::GetItemRectMax().y + 0.25f * u));
                } else
                    neo_same_line_at(x_next);
                if (neo_text_button(lbl.c_str(), c == cur ? NeoBtn::Accent : NeoBtn::Ghost) && c != cur) { p.num = c; changed = true; }
                x_next = ImGui::GetItemRectMax().x + 0.25f * u;
            }
            break;
        }
        default: {
            float       v   = float(p.num);
            std::string fmt = p.integer ? "%.0f" : (p.step > 0. && p.step >= 0.1 ? "%.1f" : "%.2f");
            if (neo_row_slider("##n", label.c_str(), &v, float(p.mn), float(p.mx), fmt.c_str(), tip.c_str(), label_col)) {
                double x = v;
                if (p.step > 0.) x = p.mn + std::round((x - p.mn) / p.step) * p.step;
                if (p.integer) x = std::round(x);
                p.num   = x;
                changed = true;
            }
        }
        }
        ImGui::PopID();
    }
    if (changed)
        scad_request();
    if (!m_scad_error.empty()) {
        const std::string first = m_scad_error.substr(0, m_scad_error.find('\n'));
        neo_warn_row("##pg_scaderr", first.c_str(), m_scad_error.c_str());
    }
}

} // namespace GUI
} // namespace Slic3r
// NEOTKO_PRIMGEN_TAG_END
