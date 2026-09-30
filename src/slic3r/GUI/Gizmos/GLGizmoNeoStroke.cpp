// NEOTKO_NEOSTROKE_TAG s337 — ver la cabecera.
#include "GLGizmoNeoStroke.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLShader.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "GizmoNeotkoStyle.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"   // LoadStrategy
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include <wx/filedlg.h>
#include <boost/filesystem/path.hpp>
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/NeoDebug.hpp"
#include "libslic3r/NeoArachne/Preview/PreviewGeometrySource.hpp"
#include "libslic3r/NeoArachne/Preview/PreviewSlicer.hpp"

#include <imgui/imgui.h>
#include <GL/glew.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

namespace Slic3r { namespace GUI {

namespace NSPrev = Slic3r::NeoArachne::Preview;

namespace {

// Topes del gizmo. El panel viejo tenía 8 islas y 2000 vértices porque relaminaba en cada tic de un
// deslizador wx; aquí se lamina cuando cambia algo y en otro hilo, así que caben placas enteras.
constexpr size_t kMaxIslands = 400;
constexpr size_t kMaxVerts   = 400000;
constexpr double kPollSec    = 0.4;

double now_sec()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

inline void mix(size_t& seed, size_t h) { seed ^= h + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2); }

// Los mandos, en los mismos grupos que la ventana Avanzado (`NeoStrokeAdvancedDialog.cpp`, s336g) más los
// tres de la pestaña. Si se añade un mando a NeoStroke, va aquí también.
struct ParamGroup { const char* title; std::vector<std::string> keys; };
const std::vector<ParamGroup>& param_groups()
{
    static const std::vector<ParamGroup> groups = {
        // NEOTKO_NEOSTROKE_TAG s338 — como la pestaña: los tres límites del planificador por campo arriba; fuera los
        // mandos que el campo no usa (tips, join, detail, fewer lines, corner hooks).
        { "Main", {
            "neostroke_max_bead_pct",
            "neostroke_max_width_pct",
            "neostroke_bead_min_pct",
        } },
        { "Line widths", {
            "neostroke_width_ref",
        } },
        { "Shape", {
            "neostroke_max_stroke_width",
            "neostroke_band_mm",            // s340 — 0 = auto
        } },
        { "Extra flow", {
            "neostroke_curve_overlap",
            "neostroke_lane_overlap",       // s339
            "neostroke_lead_in",            // s339b
        } },
        { "Path order and travel", {
            "neostroke_end_at_junctions",   // s339
            "neostroke_layer_jitter",
            "neostroke_skate",
            "neostroke_skate_detour",
        } },
        { "Classic wall", {
            "outer_wall_line_width",
            "inner_wall_line_width",
            "infill_wall_overlap",
        } },
    };
    return groups;
}

const ConfigOption* snap_option(const NSPrev::ConfigSnapshot& s, const std::string& key)
{
    if (const ConfigOption* o = s.region.option(key)) return o;
    if (const ConfigOption* o = s.object.option(key)) return o;
    return s.print.option(key);
}

// El umbral en mm que marca un mando de ancho, y hacia dónde manda (true = los tramos por DEBAJO del umbral
// son los que decide, false = los de por encima). Es la misma cuenta que `NeoStroke.cpp` (P.floor_w,
// P.detail_min, P.bead_min, P.ceiling_w, P.max_bead), sin los env var. Devuelve false si el mando no es de
// ancho: no hay nada que resaltar.
bool width_rule(const std::string& key, const NSPrev::ConfigSnapshot& s, double& thr_mm, bool& below)
{
    const double wref   = NSPrev::neostroke_width_ref_mm(s);
    const int    ext    = std::max(0, s.region.wall_filament.value - 1);
    const double nozzle = s.print.nozzle_diameter.get_at(size_t(ext));
    const PrintRegionConfig& r = s.region;
    if (key == "neostroke_min_width_pct")  { thr_mm = r.neostroke_min_width_pct.value  / 100. * wref;   below = true;  return true; }
    if (key == "neostroke_detail_min_pct") { thr_mm = r.neostroke_detail_min_pct.value / 100. * wref;   below = true;  return true; }
    if (key == "neostroke_bead_min_pct")   { thr_mm = r.neostroke_bead_min_pct.value   / 100. * nozzle; below = true;  return true; }
    if (key == "neostroke_max_width_pct")  { thr_mm = r.neostroke_max_width_pct.value  / 100. * wref;   below = false; return true; }
    if (key == "neostroke_max_bead_pct")   { thr_mm = r.neostroke_max_bead_pct.value   / 100. * wref;   below = false; return true; }
    return false;
}

// Lo que vale en mm un mando en %: la referencia de cada uno según `NeoStroke.cpp`. <0 = no aplica.
double pct_to_mm(const std::string& key, double pct, const NSPrev::ConfigSnapshot& s)
{
    const double wref   = NSPrev::neostroke_width_ref_mm(s);
    const int    ext    = std::max(0, s.region.wall_filament.value - 1);
    const double nozzle = s.print.nozzle_diameter.get_at(size_t(ext));
    if (key == "neostroke_bead_min_pct")
        return pct / 100. * nozzle;
    if (key == "neostroke_min_width_pct" || key == "neostroke_max_width_pct" ||
        key == "neostroke_detail_min_pct" || key == "neostroke_max_bead_pct")
        return pct / 100. * wref;
    if (key == "infill_wall_overlap")   // se come ese % del ancho del muro exterior (NeoStroke.cpp, P.outer_w)
        return pct / 100. * double(s.ext_perimeter_flow.width());
    return -1.0;
}

// Colores. Ancho: por la huella (sep), en cuatro bandas. La roja es la de s332: por debajo de 0.25 no es un
// cordón de verdad.
const ColorRGBA kClassic   (0.60f, 0.62f, 0.64f, 1.f);
const ColorRGBA kNsOwner   (1.00f, 0.55f, 0.15f, 1.f);
const ColorRGBA kBand[4] = { ColorRGBA(0.92f, 0.22f, 0.22f, 1.f),    // sep < 0.25
                             ColorRGBA(0.97f, 0.72f, 0.18f, 1.f),    // 0.25 – 0.40
                             ColorRGBA(0.12f, 0.72f, 0.62f, 1.f),    // 0.40 – 0.60
                             ColorRGBA(0.30f, 0.46f, 0.96f, 1.f) };  // ≥ 0.60
const char*     kBandName[4] = { "< 0.25 loose", "0.25-0.40", "0.40-0.60", ">= 0.60" };
// s337 — regla de vecinos (BEAD_MODEL_DATA §4): una fina ENTRE vecinos se llena; no es un fallo.
const ColorRGBA kSqueezed  (0.66f, 0.42f, 0.95f, 1.f);
// s337 — modo «Extra flow»: cuánto plástico de más mete el caudal extra (BEAD_MODEL_DATA §0.1: +13-15 % en
// cordones anchos en curva cerrada). Bandas: ≤1 %, 1-5 %, 5-10 %, >10 %.
const ColorRGBA kExtra[4] = { ColorRGBA(0.35f, 0.42f, 0.55f, 1.f), ColorRGBA(0.12f, 0.72f, 0.62f, 1.f),
                              ColorRGBA(0.97f, 0.72f, 0.18f, 1.f), ColorRGBA(0.92f, 0.22f, 0.22f, 1.f) };
const char*     kExtraName[4] = { "<= 1 %", "1-5 %", "5-10 %", "> 10 %" };
// s337b — modo «Risks»: lo que las macros del TEST22 enseñaron que falla (`docs/WIP/TEST22_frames/INDEX.md`).
const ColorRGBA kRisk[7] = { ColorRGBA(0.95f, 0.18f, 0.18f, 1.f),   // punta que se rompe
                             ColorRGBA(0.62f, 0.30f, 0.95f, 1.f),   // s338 — nudo de puntas (hoyo que se apila)
                             ColorRGBA(1.00f, 0.55f, 0.10f, 1.f),   // raja entre anchos
                             ColorRGBA(0.20f, 0.85f, 0.95f, 1.f),   // doble pasada
                             ColorRGBA(0.98f, 0.90f, 0.20f, 1.f),   // s339 — no se pisa con el vecino (surco)
                             ColorRGBA(1.00f, 0.45f, 0.75f, 1.f),   // fina suelta
                             ColorRGBA(0.42f, 0.45f, 0.48f, 1.f) }; // sin riesgo
const char*     kRiskName[7] = { "tip may break", "starts bunched (pit)", "seam (wide+wide)", "double pass", "no overlap (groove)", "thin, loose", "ok" };
int extra_band(double pct) { return pct <= 1.0 ? 0 : pct <= 5.0 ? 1 : pct <= 10.0 ? 2 : 3; }
const ColorRGBA kHighlight (1.00f, 0.25f, 0.95f, 1.f);
const ColorRGBA kTravel    (0.55f, 0.58f, 0.62f, 1.f);
const ColorRGBA kStart     (0.20f, 0.90f, 0.35f, 1.f);
const ColorRGBA kStop      (0.95f, 0.20f, 0.20f, 1.f);
const ColorRGBA kGap[NSPrev::kGapKinds] = {
    ColorRGBA(0.25f, 0.55f, 1.00f, 0.95f),   // sólo Classic
    ColorRGBA(1.00f, 0.15f, 0.80f, 0.95f),   // junta
    ColorRGBA(1.00f, 0.92f, 0.10f, 0.95f),   // sólo NeoStroke
    ColorRGBA(0.90f, 0.08f, 0.08f, 0.95f),   // s337b — agujero (> 0.3 mm², no es de nadie)
    ColorRGBA(0.55f, 0.20f, 0.85f, 0.95f) }; // s337b — bolsita cedida al relleno, que no la imprime (TEST23, la P)
const char*     kGapName[NSPrev::kGapKinds] = { "Classic only", "Joint", "NeoStroke only", "Hole", "Infill pocket" };

int band_of(double sep) { return sep < 0.25 ? 0 : sep < 0.40 ? 1 : sep < 0.60 ? 2 : 3; }

ColorRGBA run_color(size_t i)
{
    // Tono áureo, como `s336_capa_huella.py`, en 8 cubos para no tener un GLModel por recorrido.
    static const ColorRGBA c[8] = {
        ColorRGBA(0.95f, 0.35f, 0.30f, 1.f), ColorRGBA(0.30f, 0.80f, 0.95f, 1.f),
        ColorRGBA(0.95f, 0.80f, 0.25f, 1.f), ColorRGBA(0.55f, 0.35f, 0.95f, 1.f),
        ColorRGBA(0.35f, 0.90f, 0.45f, 1.f), ColorRGBA(0.95f, 0.45f, 0.80f, 1.f),
        ColorRGBA(0.25f, 0.50f, 0.95f, 1.f), ColorRGBA(0.95f, 0.60f, 0.20f, 1.f) };
    return c[i % 8];
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// ciclo de vida
// ═══════════════════════════════════════════════════════════════════════════════════════════════════

GLGizmoNeoStroke::GLGizmoNeoStroke(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
    , m_alive(std::make_shared<std::atomic<bool>>(true))
{}

GLGizmoNeoStroke::~GLGizmoNeoStroke()
{
    m_alive->store(false);
    m_cancel.store(true);
    if (m_worker.joinable())
        m_worker.join();
}

bool GLGizmoNeoStroke::on_init()
{
    m_shortcut_key = 0;
    return true;
}

std::string GLGizmoNeoStroke::on_get_name() const
{
    return _u8L("NeoStroke Preview");
}

bool GLGizmoNeoStroke::on_is_activable() const
{
    // El mismo candado que el motor (s336: una sola llave, la casilla de Preferencias o
    // ORCA_DEBUG_NEOSTROKE). Sin él NeoStroke no lamina, así que no hay nada que ver. El icono se queda
    // visible; sólo cambia si se puede pulsar. Y hace falta al menos un objeto seleccionado.
    if (!NeoDebug::enabled(NeoDebug::NEOSTROKE))
        return false;
    if (m_has_zone || m_zone_pick || m_ext_model)
        return true;   // B2/B4: con zona o modelo externo no hace falta selección
    const Selection& sel = m_parent.get_selection();
    return !sel.is_empty() && !sel.get_content().empty();
}

void GLGizmoNeoStroke::on_set_state()
{
    if (get_state() == On) {
        if (m_ext_model && !m_plate_hidden) {
            m_parent.toggle_model_objects_visibility(false);
            m_plate_hidden = true;
        }
        m_parent.set_neotko_selection_lock(m_locked);   // el candado se recuerda entre aperturas
        m_targets.clear();   // s342b — al abrir, siempre «objetos nuevos»: arranca en la última capa
        rebuild_targets();
        m_last_poll = 0.0;   // lamina en el primer frame
    } else if (get_state() == Off) {
        // 🚨 Esto se ejecuta también EN MEDIO de un borrado (memoria `lessons_code_traps`): nada de leer el
        //    modelo aquí. Sólo se cancela el hilo y se devuelve el corte del objeto.
        m_cancel.store(true);
        release_clipping();
        m_hover_key.clear();
        m_zone_pick = m_zone_dragging = false;
        m_island_pick = false;   // s342
        m_edit_island = -1;
        // 🚨 El candado vive en el lienzo: si no se suelta aquí, el lienzo se queda sin poder seleccionar nada.
        m_parent.set_neotko_selection_lock(false);
        // B4 — la placa vuelve a verse al cerrar. El modelo externo se conserva para la próxima vez.
        if (m_plate_hidden) {
            m_parent.toggle_model_objects_visibility(true);
            m_plate_hidden = false;
        }
    }
}

void GLGizmoNeoStroke::data_changed(bool is_serializing)
{
    if (get_state() != On)
        return;
    rebuild_targets();
    m_last_poll = 0.0;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// qué se mira
// ═══════════════════════════════════════════════════════════════════════════════════════════════════

void GLGizmoNeoStroke::rebuild_targets()
{
    const Selection& sel   = m_parent.get_selection();
    const Model*     model = sel.get_model();
    std::vector<Target> old = std::move(m_targets);
    m_targets.clear();
    if (model == nullptr)
        return;
    const DynamicPrintConfig full = wxGetApp().plater()->neotko_full_config();

    // Qué (objeto, instancia) se miran: los de la zona si la hay; si no, lo seleccionado (primera instancia).
    std::vector<std::pair<int, int>> picks;
    // B4 — con un modelo externo cargado, se miran sus objetos y nada más.
    const bool ext = m_ext_model != nullptr;
    const Model* src_model = ext ? m_ext_model.get() : model;
    if (ext) {
        for (size_t oi = 0; oi < m_ext_model->objects.size(); ++oi)
            for (size_t ii = 0; ii < m_ext_model->objects[oi]->instances.size(); ++ii)
                picks.emplace_back(int(oi), int(ii));
    } else if (m_has_zone) {
        const Vec2d lo(std::min(m_zone_a.x(), m_zone_b.x()), std::min(m_zone_a.y(), m_zone_b.y()));
        const Vec2d hi(std::max(m_zone_a.x(), m_zone_b.x()), std::max(m_zone_a.y(), m_zone_b.y()));
        for (size_t oi = 0; oi < model->objects.size(); ++oi) {
            const ModelObject* mo = model->objects[oi];
            if (mo == nullptr) continue;
            for (size_t ii = 0; ii < mo->instances.size(); ++ii) {
                const BoundingBoxf3 bb = mo->instance_bounding_box(ii);
                if (bb.max.x() >= lo.x() && bb.min.x() <= hi.x() && bb.max.y() >= lo.y() && bb.min.y() <= hi.y())
                    picks.emplace_back(int(oi), int(ii));
            }
        }
    } else {
        for (const auto& [obj_idx, insts] : sel.get_content())
            picks.emplace_back(obj_idx, insts.empty() ? 0 : std::max(0, *insts.begin()));
    }

    for (const auto& [obj_idx, inst_pick] : picks) {
        if (obj_idx < 0 || obj_idx >= int(src_model->objects.size()))
            continue;
        const ModelObject* mo = src_model->objects[size_t(obj_idx)];
        if (mo == nullptr || mo->instances.empty())
            continue;
        Target t;
        t.external  = ext;
        t.object_id = mo->id();
        t.obj_idx   = obj_idx;
        t.inst_idx  = inst_pick;
        if (t.inst_idx >= int(mo->instances.size()))
            t.inst_idx = 0;
        t.name = mo->name;
        if (mo->instances.size() > 1)
            t.name += " #" + std::to_string(t.inst_idx + 1);
        const Transform3d inst_m = mo->instances[size_t(t.inst_idx)]->get_matrix();
        double zmin = std::numeric_limits<double>::max();
        for (size_t vi = 0; vi < mo->volumes.size(); ++vi) {
            const ModelVolume* mv = mo->volumes[vi];
            if (mv == nullptr)
                continue;
            if (!mv->is_model_part()) {
                if (mv->is_modifier() || mv->is_negative_volume())
                    t.has_modifiers = true;
                continue;
            }
            TriangleMesh m = mv->mesh();
            if (m.empty())
                continue;
            m.transform(inst_m * mv->get_matrix());
            zmin = std::min(zmin, m.bounding_box().min.z());
            t.part_volume_idxs.push_back(int(vi));
            t.meshes.push_back(std::make_shared<const TriangleMesh>(std::move(m)));
        }
        if (t.meshes.empty())
            continue;
        t.world_min_z = zmin;
        // s342 — ajustes por isla: las anclas van en coordenadas del objeto y el gizmo corta en las de la placa.
        t.obj_to_world[0] = inst_m(0, 0); t.obj_to_world[1] = inst_m(0, 1); t.obj_to_world[2] = inst_m(0, 3);
        t.obj_to_world[3] = inst_m(1, 0); t.obj_to_world[4] = inst_m(1, 1); t.obj_to_world[5] = inst_m(1, 3);
        t.grid        = NSPrev::object_layer_grid(full, *mo);
        // La casilla de ocultar sobrevive a una selección nueva del mismo objeto.
        for (const Target& o : old)
            if (o.object_id == t.object_id && o.inst_idx == t.inst_idx) { t.visible = o.visible; break; }
        m_targets.push_back(std::move(t));
    }
    const int n = ref_layer_count();
    // s342b — objetos NUEVOS a la vista = se empieza por la ÚLTIMA capa (la cara de arriba es lo que se mira casi
    // siempre). Si son los mismos (un deshacer, un cambio de ajuste), la capa elegida se respeta.
    bool same = old.size() == m_targets.size();
    for (size_t i = 0; same && i < old.size(); ++i)
        same = old[i].object_id == m_targets[i].object_id && old[i].inst_idx == m_targets[i].inst_idx;
    if (n > 0)
        m_layer = same ? std::clamp(m_layer, 1, n) : std::max(1, top_filled_layer());
    if (!same) {
        m_edit_island  = -1;
        m_island_hover = -1;
    }
    if (m_edit_target >= int(m_targets.size()))
        m_edit_target = 0;
}

int GLGizmoNeoStroke::ref_target() const
{
    // s342c — la capa va por las capas del objeto ELEGIDO («Object» en Settings), no por el que más capas tiene
    // (Neotko: «elegir un objeto, ver su última capa y elegir cuál de SUS capas ver»). Los demás enseñan la capa
    // que cae a esa misma altura, o nada si ya se han acabado.
    if (m_edit_target >= 0 && m_edit_target < int(m_targets.size()) && m_targets[size_t(m_edit_target)].visible
        && !m_targets[size_t(m_edit_target)].grid.layers.empty())
        return m_edit_target;
    int best = -1;
    size_t best_n = 0;
    for (size_t i = 0; i < m_targets.size(); ++i)
        if (m_targets[i].visible && m_targets[i].grid.layers.size() > best_n) {
            best   = int(i);
            best_n = m_targets[i].grid.layers.size();
        }
    return best;
}

int GLGizmoNeoStroke::ref_layer_count() const
{
    const int r = ref_target();
    return r < 0 ? 0 : int(m_targets[size_t(r)].grid.layers.size());
}

int GLGizmoNeoStroke::top_filled_layer() const
{
    const int r = ref_target();
    if (r < 0)
        return 0;
    const Target& R = m_targets[size_t(r)];
    const int     n = int(R.grid.layers.size());
    MeshSlicingParamsEx msp;
    for (int li = n - 1; li >= 0; --li) {
        const double z = R.world_min_z + 0.5 * (R.grid.layers[size_t(li)].first + R.grid.layers[size_t(li)].second);
        for (const auto& m : R.meshes) {
            if (!m || m->bounding_box().max.z() < z)
                continue;
            const std::vector<ExPolygons> sl = slice_mesh_ex(m->its, std::vector<float>{ float(z) }, msp);
            if (!sl.empty() && !sl.front().empty())
                return li + 1;
        }
    }
    return n;
}

int GLGizmoNeoStroke::layer_for_target(size_t t, int ref_layer_1based) const
{
    const int r = ref_target();
    if (r < 0 || t >= m_targets.size())
        return -1;
    const Target& R = m_targets[size_t(r)];
    const int li = ref_layer_1based - 1;
    if (li < 0 || li >= int(R.grid.layers.size()))
        return -1;
    if (int(t) == r)
        return li;
    // Por la altura del plano medio en la PLACA: dos objetos con la misma rejilla dan la misma capa, y uno con
    // otra altura de capa da la suya, la que el laminado cortaría a esa altura.
    const double mid_world = R.world_min_z + 0.5 * (R.grid.layers[size_t(li)].first + R.grid.layers[size_t(li)].second);
    return m_targets[t].grid.layer_at(mid_world - m_targets[t].world_min_z);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// laminar
// ═══════════════════════════════════════════════════════════════════════════════════════════════════

std::vector<GLGizmoNeoStroke::Task> GLGizmoNeoStroke::build_tasks(size_t& key) const
{
    std::vector<Task> tasks;
    key = 0;
    const Model* model = m_parent.get_selection().get_model();
    if (model == nullptr)
        return tasks;
    // 🚨 La config CON las claves espejo, la misma que recibe Print::apply(). Con `full_config()` en crudo
    //    vuelve el divorcio visor/G-code de s335.
    FullPrintConfig full;
    full.apply(wxGetApp().plater()->neotko_full_config(), /*ignore_nonexistent=*/true);

    // La capa elegida es la de ARRIBA; la profundidad baja desde ella (nada por encima).
    for (int d = -m_span; d <= 0; ++d) {
        const int ref_layer = m_layer + d;
        for (size_t t = 0; t < m_targets.size(); ++t) {
            const Target& T = m_targets[t];
            if (!T.visible)
                continue;
            const ModelObject* mo = object_of(T);
            if (mo == nullptr)
                continue;   // la lista está vieja; data_changed la rehará
            const int li = layer_for_target(t, ref_layer);
            if (li < 0)
                continue;
            const auto& L = T.grid.layers[size_t(li)];
            Task task;
            task.target        = t;
            task.layer_idx     = li;
            task.slice_z_world = T.world_min_z + 0.5 * (L.first + L.second);
            task.top_z_world   = T.world_min_z + L.second;
            task.print_z       = L.second + T.grid.print_z_offset;
            task.warn_level    = double(m_warn_level_pct) / 100.0;
            task.closure_mm    = double(m_closure_mm);
            std::copy(std::begin(T.obj_to_world), std::end(T.obj_to_world), std::begin(task.obj_to_world));
            mix(key, std::hash<int>()(int(std::lround(m_warn_level_pct))));
            mix(key, std::hash<int>()(int(std::lround(m_closure_mm * 1000.f))));
            mix(key, std::hash<size_t>()(T.object_id.id));
            mix(key, std::hash<int>()(li));
            for (size_t k = 0; k < T.part_volume_idxs.size(); ++k) {
                const int vi = T.part_volume_idxs[k];
                if (vi < 0 || vi >= int(mo->volumes.size()))
                    continue;
                VolumeTask vt;
                vt.snap      = NSPrev::snapshot_for_volume(full, *mo, *mo->volumes[size_t(vi)], T.grid, li);
                vt.snap_hash = NSPrev::hash_snapshot(vt.snap);
                vt.mesh      = T.meshes[k];
                mix(key, vt.snap_hash);
                mix(key, std::hash<const void*>()(vt.mesh.get()));
                task.vols.push_back(std::move(vt));
            }
            if (!task.vols.empty())
                tasks.push_back(std::move(task));
        }
    }
    return tasks;
}

std::shared_ptr<GLGizmoNeoStroke::JobOut> GLGizmoNeoStroke::run_tasks(std::vector<Task> tasks, std::atomic<bool>& cancel)
{
    auto out = std::make_shared<JobOut>();
    const double t0 = now_sec();
    for (Task& task : tasks) {
        if (cancel.load()) { out->cancelled = true; break; }
        LayerOut lo;
        lo.target      = task.target;
        lo.layer_idx   = task.layer_idx;
        lo.top_z_world = task.top_z_world;
        lo.print_z     = task.print_z;
        if (!task.vols.empty()) {
            const NSPrev::ConfigSnapshot& s0 = task.vols.front().snap;
            lo.height    = s0.layer_height;
            lo.w_ref_mm  = NSPrev::neostroke_width_ref_mm(s0);
            const int ext = std::max(0, s0.region.wall_filament.value - 1);
            lo.nozzle_mm = s0.print.nozzle_diameter.get_at(size_t(ext));
        }
        // Piezas con la MISMA config van juntas (una región, como en el laminado); con config distinta, cada
        // una la suya. Donde se solapan, gana la pieza que va DESPUÉS en la lista.
        // ⚠️ Aproximación: el laminado real reparte los solapes con su propia regla (`PrintObjectSlice`).
        struct Group { size_t hash; const NSPrev::ConfigSnapshot* snap; ExPolygons slices; };
        std::vector<Group> groups;
        for (const VolumeTask& vt : task.vols) {
            if (!vt.mesh)
                continue;
            // 🚨 s340 — cortar como `PrintObjectSlice`: `slice_closing_radius` + `resolution`. Con
            //    `TriangleMesh::slice()` (cierre 0.0004 mm) los picos agudos de los agujeros no se aplanaban y
            //    el plan cambiaba (la A de NeoStroke-TEST: el gizmo pintaba una diagonal que no se imprime).
            MeshSlicingParamsEx msp;
            msp.closing_radius = float(std::max(0.0004, double(vt.snap.object.slice_closing_radius.value)));
            msp.resolution     = vt.snap.print.resolution.value <= 0.001 ? 0.0 : 0.0025;
            std::vector<ExPolygons> sl = slice_mesh_ex(vt.mesh->its, std::vector<float>{ float(task.slice_z_world) }, msp);
            if (sl.empty() || sl.front().empty())
                continue;
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.hash == vt.snap_hash; });
            if (it == groups.end())
                groups.push_back({ vt.snap_hash, &vt.snap, std::move(sl.front()) });
            else
                it->slices = union_ex(it->slices, sl.front());
        }
        for (size_t i = 0; i < groups.size(); ++i)
            for (size_t j = i + 1; j < groups.size(); ++j)
                if (!groups[i].slices.empty() && !groups[j].slices.empty())
                    groups[i].slices = diff_ex(groups[i].slices, to_polygons(groups[j].slices));
        for (Group& g : groups) {
            if (cancel.load()) { out->cancelled = true; break; }
            if (g.slices.empty())
                continue;
            NSPrev::PreviewGeometrySource src = NSPrev::PreviewGeometrySource::from_slices(std::move(g.slices), kMaxIslands, kMaxVerts);
            std::copy(std::begin(task.obj_to_world), std::end(task.obj_to_world), std::begin(src.obj_to_slice));
            const double t_sl0 = now_sec();   // s342f — sonda: laminar frente a las cifras
            NSPrev::PreviewResult r = NSPrev::preview_slice(*g.snap, src);
            const double t_sl = now_sec() - t_sl0;
            if (!r.ok) {
                if (!lo.error.empty()) lo.error += "; ";
                lo.error += r.error;
                continue;
            }
            lo.neostroke_active = lo.neostroke_active || r.neostroke_active;
            // s337b — la velocidad de NeoStroke (muro interior) de ESTE objeto: decide el riesgo de punta.
            NSPrev::MetricsOptions mopt;
            mopt.speed_mm_s = g.snap->region.inner_wall_speed.get_at(0); // Upstream Snapmaker #794: variante estándar
            mopt.level      = task.warn_level;
            mopt.closure_mm = task.closure_mm;
            const double t_me0 = now_sec();
            NSPrev::MetricsResult mr = NSPrev::compute_layer_metrics(r, mopt);
            if (NeoDebug::enabled(NeoDebug::NEOSTROKE)) {
                char tb[200];
                snprintf(tb, sizeof(tb), "[NS-GIZMO] capa %d: laminar %.0f ms, cifras %.0f ms (islas %zu)",
                         task.layer_idx, 1000.0 * t_sl, 1000.0 * (now_sec() - t_me0), r.input_slices.size());
                NeoDebug::write(NeoDebug::NEOSTROKE, tb);
            }
            lo.metrics.m.add(mr.m);
            for (NSPrev::Gap& gp : mr.gaps) lo.metrics.gaps.push_back(std::move(gp));
            for (NSPrev::Run& rn : mr.runs) lo.metrics.runs.push_back(rn);
            lo.metrics.excess_cells.insert(lo.metrics.excess_cells.end(), mr.excess_cells.begin(), mr.excess_cells.end());
            lo.metrics.sustained_cells.insert(lo.metrics.sustained_cells.end(), mr.sustained_cells.begin(), mr.sustained_cells.end());
            if (mr.excess_cell_mm > 0.0) lo.metrics.excess_cell_mm = mr.excess_cell_mm;
            lo.seg_flags.push_back(std::move(mr.seg_flags));
            lo.results.push_back(std::move(r));
        }
        out->layers.push_back(std::move(lo));
    }
    out->seconds = now_sec() - t0;
    return out;
}

void GLGizmoNeoStroke::launch()
{
    if (m_running) {
        // Ya hay uno: se le corta y se vuelve a lanzar al acabar, con lo que haya entonces.
        m_cancel.store(true);
        m_rerun = true;
        return;
    }
    if (m_worker.joinable())
        m_worker.join();
    size_t key = 0;
    std::vector<Task> tasks = build_tasks(key);
    m_wanted_key = key;
    if (tasks.empty()) {
        m_status = _u8L("Nothing to slice at this layer.");
        return;
    }
    m_cancel.store(false);
    m_running = true;
    m_rerun   = false;
    m_status  = _u8L("Slicing…");
    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    m_worker = std::thread([this, alive, key, tasks = std::move(tasks)]() mutable {
        std::shared_ptr<JobOut> out = run_tasks(std::move(tasks), m_cancel);
        out->key = key;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_worker_out = std::move(out);
        }
        wxGetApp().CallAfter([this, alive]() {
            if (!alive->load())
                return;
            worker_finished();
        });
    });
}

void GLGizmoNeoStroke::worker_finished()
{
    if (m_worker.joinable())
        m_worker.join();
    m_running = false;
    std::shared_ptr<JobOut> out;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        out = std::move(m_worker_out);
    }
    if (get_state() != On)
        return;
    if (out && !out->cancelled) {
        m_current = out;
        char buf[96];
        snprintf(buf, sizeof(buf), "%.2f s", out->seconds);
        m_status = _u8L("Sliced in") + " " + buf;
    }
    if (m_rerun)
        launch();
    m_parent.set_as_dirty();
    m_parent.request_extra_frame();
}

void GLGizmoNeoStroke::poll()
{
    const double t = now_sec();
    if (t - m_last_poll < kPollSec)
        return;
    m_last_poll = t;
    size_t key = 0;
    (void)build_tasks(key);
    m_wanted_key = key;
    const size_t have = m_current ? m_current->key : 0;
    if (m_auto && key != have && !m_running)
        launch();
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// dibujo
// ═══════════════════════════════════════════════════════════════════════════════════════════════════

const GLGizmoNeoStroke::JobOut* GLGizmoNeoStroke::shown() const
{
    if (m_show_a && m_frozen_a)
        return m_frozen_a.get();
    return m_current.get();
}

size_t GLGizmoNeoStroke::view_key() const
{
    size_t k = 0;
    mix(k, std::hash<int>()(m_color_mode));
    mix(k, std::hash<int>()((m_real_footprint ? 1 : 0) | (m_show_classic ? 2 : 0) | (m_show_travel ? 4 : 0) |
                            (m_show_marks ? 8 : 0) | (m_show_gaps ? 16 : 0) | (m_show_excess ? 32 : 0) |
                            (m_show_hills ? 64 : 0)));
    for (const Target& t : m_targets)
        mix(k, std::hash<int>()(t.visible ? 1 : 0));
    mix(k, std::hash<int>()(m_edit_island));   // s342 — el contorno de la isla que se edita
    mix(k, std::hash<int>()(m_edit_target));
    return k;
}

void GLGizmoNeoStroke::rebuild_models()
{
    m_buckets.clear();
    const JobOut* job = shown();
    m_models_for      = job;
    m_models_hover    = m_hover_key;
    m_models_view_key = view_key();
    if (job == nullptr)
        return;

    // Un cubo por color. Los tubos van en P3N3 (con luz); huecos, marcas y viajes en P3 plano.
    struct Acc {
        GLModel::Geometry geo;
        ColorRGBA         color;
        bool              lit, lines;
    };
    std::map<int, Acc> acc;
    auto bucket = [&](int id, const ColorRGBA& c, bool lit, bool lines) -> GLModel::Geometry& {
        auto it = acc.find(id);
        if (it == acc.end()) {
            Acc a;
            a.color = c; a.lit = lit; a.lines = lines;
            a.geo.format = { lines ? GLModel::Geometry::EPrimitiveType::Lines : GLModel::Geometry::EPrimitiveType::Triangles,
                             lit ? GLModel::Geometry::EVertexLayout::P3N3 : GLModel::Geometry::EVertexLayout::P3 };
            it = acc.emplace(id, std::move(a)).first;
        }
        return it->second.geo;
    };
    enum : int { idClassic = 0, idOwner = 1, idBand = 10, idSqueezed = 15, idRun = 20, idExtra = 30, idRisk = 35, idHi = 42, idTravel = 50, idStart = 51,
                 idStop = 52, idGap = 60, idExcess = 70, idHills = 71 };

    // El mando bajo el ratón: su umbral, por objeto.
    auto hover_rule = [&](const LayerOut& lo, double& thr, bool& below) -> bool {
        if (m_hover_key.empty())
            return false;
        // Umbral a partir de los datos del resultado: se usan los mismos snapshots que el hilo, reconstruidos
        // desde el modelo sólo para leer los % (barato: una config por objeto).
        if (lo.target >= m_targets.size())
            return false;
        const Target& T = m_targets[lo.target];
        const ModelObject* mo = object_of(T);
        if (mo == nullptr || T.part_volume_idxs.empty())
            return false;
        const int vi = T.part_volume_idxs.front();
        if (mo == nullptr || vi >= int(mo->volumes.size()))
            return false;
        const NSPrev::ConfigSnapshot s = NSPrev::snapshot_for_volume(wxGetApp().plater()->neotko_full_config(), *mo,
                                                                     *mo->volumes[size_t(vi)], T.grid, lo.layer_idx);
        return width_rule(m_hover_key, s, thr, below);
    };

    size_t run_counter = 0;
    for (const LayerOut& lo : job->layers) {
        if (lo.target >= m_targets.size() || !m_targets[lo.target].visible)
            continue;
        double thr = 0.0; bool below = true;
        const bool hl = hover_rule(lo, thr, below);
        const double top = lo.top_z_world;

        for (size_t ri = 0; ri < lo.results.size(); ++ri) {
            const NSPrev::PreviewResult& r = lo.results[ri];
            const std::vector<unsigned char>* flags = ri < lo.seg_flags.size() ? &lo.seg_flags[ri] : nullptr;
            // Tramos seguidos del mismo cubo = una polilínea (juntas limpias entre tramos).
            Lines lines; std::vector<double> widths, heights; int cur_id = -1; ColorRGBA cur_c;
            auto flush = [&]() {
                if (!lines.empty() && cur_id >= 0) {
                    GLModel::Geometry& g = bucket(cur_id, cur_c, true, false);
                    _3DScene::thick_lines_to_verts(lines, widths, heights, false, top, g);
                }
                lines.clear(); widths.clear(); heights.clear();
            };
            size_t run_i = run_counter;
            Point  last_end(0, 0);
            bool   have_last = false;
            for (size_t si = 0; si < r.ordered_segments.size(); ++si) {
                const NSPrev::OrderedSegment& s = r.ordered_segments[si];
                const unsigned char flag = (flags && si < flags->size()) ? (*flags)[si] : 0;
                if (s.is_travel || s.mm3_per_mm <= 0.0) {
                    flush();
                    have_last = false;
                    continue;
                }
                if (!s.neostroke && !m_show_classic) { flush(); have_last = false; continue; }
                const double h   = s.height > 0.f ? double(s.height) : lo.height;
                const double wf  = double(s.path_width_flow > 0.f ? s.path_width_flow : s.path_width);
                const double sep_draw = NSPrev::footprint_sep(wf, h);
                const double sep_nom  = NSPrev::footprint_sep(double(s.path_width), h);
                if (!have_last || !(last_end == s.from))
                    ++run_i;
                int id; ColorRGBA c;
                if (!s.neostroke) { id = idClassic; c = kClassic; }
                else if (hl && (below ? sep_nom <= thr * 1.02 : sep_nom >= thr * 0.98)) { id = idHi; c = kHighlight; }
                else if (m_color_mode == cmOwner) { id = idOwner; c = kNsOwner; }
                else if (m_color_mode == cmRuns)  { id = idRun + int(run_i % 8); c = run_color(run_i); }
                else if (m_color_mode == cmExtra) {
                    // Plástico de más frente al cordón nominal de Slic3r (h·w − h²(1 − π/4)).
                    const double nominal = h * double(s.path_width) - h * h * (1.0 - 0.25 * PI);
                    const double pct = nominal > 1e-9 ? 100.0 * (s.mm3_per_mm / nominal - 1.0) : 0.0;
                    const int b = extra_band(pct); id = idExtra + b; c = kExtra[b];
                }
                else if (m_color_mode == cmRisks) {
                    // Prioridad: lo que rompe la pieza primero.
                    const int k = (flag & NSPrev::sfTipRisk)    ? 0 : (flag & NSPrev::sfKnotRisk)  ? 1
                                : (flag & NSPrev::sfSeamRisk)   ? 2 : (flag & NSPrev::sfDoublePass) ? 3
                                : (flag & NSPrev::sfContact)    ? 4 : (flag & NSPrev::sfThinLoose)  ? 5 : 6;
                    id = idRisk + k; c = kRisk[k];
                }
                else if (flag & NSPrev::sfThinSqueezed) { id = idSqueezed; c = kSqueezed; }
                else if (flag & NSPrev::sfThinLoose)    { id = idBand; c = kBand[0]; }
                else { const int b = std::max(1, band_of(sep_nom)); id = idBand + b; c = kBand[b]; }
                if (id != cur_id || !have_last || !(last_end == s.from)) {
                    flush();
                    cur_id = id; cur_c = c;
                }
                lines.emplace_back(s.from, s.to);
                // Huella real: lo que de verdad ocupa sobre la capa. Nominal: el ancho de `;WIDTH:`.
                widths.push_back(std::max(0.01, m_real_footprint ? sep_draw : double(s.path_width)));
                heights.push_back(h);
                last_end  = s.to;
                have_last = true;
            }
            flush();
            run_counter = run_i + 1;

            if (m_show_travel) {
                GLModel::Geometry& g = bucket(idTravel, kTravel, false, true);
                for (const NSPrev::OrderedSegment& s : r.ordered_segments) {
                    if (!s.is_travel && s.mm3_per_mm > 0.0)
                        continue;
                    const unsigned int i0 = unsigned(g.vertices_count());
                    g.add_vertex(Vec3f(float(unscale<double>(s.from.x())), float(unscale<double>(s.from.y())), float(top + 0.02)));
                    g.add_vertex(Vec3f(float(unscale<double>(s.to.x())),   float(unscale<double>(s.to.y())),   float(top + 0.02)));
                    g.add_line(i0, i0 + 1);
                }
            }
        }

        // C4 — arranques (verde) y paradas (rojo) de NeoStroke: un rombo pequeño encima de la capa.
        if (m_show_marks) {
            auto diamond = [&](GLModel::Geometry& g, const Point& p, double rad) {
                const float x = float(unscale<double>(p.x())), y = float(unscale<double>(p.y())), z = float(top + 0.03);
                const float d = float(rad);
                const unsigned int i0 = unsigned(g.vertices_count());
                g.add_vertex(Vec3f(x + d, y, z)); g.add_vertex(Vec3f(x, y + d, z));
                g.add_vertex(Vec3f(x - d, y, z)); g.add_vertex(Vec3f(x, y - d, z));
                g.add_triangle(i0, i0 + 1, i0 + 2); g.add_triangle(i0, i0 + 2, i0 + 3);
            };
            for (const NSPrev::Run& rn : lo.metrics.runs) {
                if (!rn.neostroke)
                    continue;
                diamond(bucket(idStart, kStart, false, false), rn.start, 0.10);
                diamond(bucket(idStop,  kStop,  false, false), rn.end,   0.07);
            }
        }

        // s337b — el BATIBURRILLO: celdas donde NeoStroke pone más plástico del que cabe (densidad promediada
        // > 130 %). Cuadraditos color óxido encima de la capa.
        if (m_show_excess && lo.metrics.excess_cell_mm > 0.0) {
            static const ColorRGBA kExcess(0.78f, 0.36f, 0.10f, 0.95f);
            GLModel::Geometry& g = bucket(idExcess, kExcess, false, false);
            const float hc = float(0.5 * lo.metrics.excess_cell_mm), z = float(top + 0.015);
            for (const Point& pc : lo.metrics.excess_cells) {
                const float x = float(unscale<double>(pc.x())), y = float(unscale<double>(pc.y()));
                const unsigned int i0 = unsigned(g.vertices_count());
                g.add_vertex(Vec3f(x - hc, y - hc, z)); g.add_vertex(Vec3f(x + hc, y - hc, z));
                g.add_vertex(Vec3f(x + hc, y + hc, z)); g.add_vertex(Vec3f(x - hc, y + hc, z));
                g.add_triangle(i0, i0 + 1, i0 + 2); g.add_triangle(i0, i0 + 2, i0 + 3);
            }
        }

        // s339 — la MONTAÑA: sobra sostenida (> ~105 % en 1 mm), azul translúcido por debajo del batiburrillo.
        if (m_show_hills && lo.metrics.excess_cell_mm > 0.0 && !lo.metrics.sustained_cells.empty()) {
            static const ColorRGBA kHills(0.30f, 0.40f, 0.95f, 0.55f);
            GLModel::Geometry& g = bucket(idHills, kHills, false, false);
            const float hc = float(0.5 * lo.metrics.excess_cell_mm), z = float(top + 0.012);
            for (const Point& pc : lo.metrics.sustained_cells) {
                const float x = float(unscale<double>(pc.x())), y = float(unscale<double>(pc.y()));
                const unsigned int i0 = unsigned(g.vertices_count());
                g.add_vertex(Vec3f(x - hc, y - hc, z)); g.add_vertex(Vec3f(x + hc, y - hc, z));
                g.add_vertex(Vec3f(x + hc, y + hc, z)); g.add_vertex(Vec3f(x - hc, y + hc, z));
                g.add_triangle(i0, i0 + 1, i0 + 2); g.add_triangle(i0, i0 + 2, i0 + 3);
            }
        }

        // C3 — los huecos, rellenos, un pelo por encima de la capa para que no se peleen con los tubos.
        if (m_show_gaps) {
            for (const NSPrev::Gap& gp : lo.metrics.gaps) {
                const int k = int(gp.kind);
                GLModel::Geometry& g = bucket(idGap + k, kGap[k], false, false);
                const std::vector<Vec3d> tri = triangulate_expolygon_3d(gp.poly, top + 0.01);
                for (size_t i = 0; i + 2 < tri.size(); i += 3) {
                    const unsigned int i0 = unsigned(g.vertices_count());
                    // `triangulate_expolygon_3d` ya devuelve mm (desescala por dentro).
                    for (size_t j = 0; j < 3; ++j)
                        g.add_vertex(Vec3f(tri[i + j].cast<float>()));
                    g.add_triangle(i0, i0 + 1, i0 + 2);
                }
            }
        }
    }

    for (auto& [id, a] : acc) {
        if (a.geo.is_empty())
            continue;
        auto b = std::make_unique<Bucket>();
        b->color = a.color;
        b->lit   = a.lit;
        b->lines = a.lines;
        b->model.init_from(std::move(a.geo));
        b->model.set_color(a.color);
        m_buckets.push_back(std::move(b));
    }
}

void GLGizmoNeoStroke::update_clipping()
{
    // Corta la pieza por DEBAJO de la capa más baja que se enseña: así los caminos quedan encima del corte y
    // no dentro del sólido. Es el mismo mecanismo que usa el ocultador de instancias de los gizmos de pintura.
    const int r = ref_target();
    if (!m_cut_object || r < 0) {
        release_clipping();
        return;
    }
    const Target& R  = m_targets[size_t(r)];
    const int     li = std::clamp(m_layer - m_span, 1, int(R.grid.layers.size())) - 1;
    if (li < 0 || li >= int(R.grid.layers.size())) {
        release_clipping();
        return;
    }
    const double cut = R.world_min_z + R.grid.layers[size_t(li)].first;
    m_parent.set_use_clipping_planes(true);
    m_parent.set_clipping_plane(0, ClippingPlane(Vec3d::UnitZ(), 1e6));
    m_parent.set_clipping_plane(1, ClippingPlane(-Vec3d::UnitZ(), cut));
    m_clip_active = true;
}

void GLGizmoNeoStroke::release_clipping()
{
    if (!m_clip_active)
        return;
    m_parent.set_use_clipping_planes(false);
    m_clip_active = false;
    m_parent.set_as_dirty();
}

void GLGizmoNeoStroke::on_render()
{
    render_zone_rect();
    update_clipping();
    if (shown() != m_models_for || m_hover_key != m_models_hover || view_key() != m_models_view_key)
        rebuild_models();
    rebuild_glow();   // s342b — barato: sólo rehace si cambia isla, capa, ancla u objeto
    if (m_buckets.empty() && m_glow.empty())
        return;

    const Camera&      camera = wxGetApp().plater()->get_camera();
    const Transform3d& view   = camera.get_view_matrix();
    glsafe(::glEnable(GL_DEPTH_TEST));

    if (GLShaderProgram* sh = wxGetApp().get_shader("gouraud_light")) {
        sh->start_using();
        sh->set_uniform("view_model_matrix", view);   // los vértices ya están en coordenadas de placa
        sh->set_uniform("projection_matrix", camera.get_projection_matrix());
        const Matrix3d nm = view.matrix().block(0, 0, 3, 3);
        sh->set_uniform("view_normal_matrix", nm);
        sh->set_uniform("emission_factor", 0.15f);
        for (const auto& b : m_buckets)
            if (b->lit)
                b->model.render();
        sh->stop_using();
    }
    if (GLShaderProgram* sh = wxGetApp().get_shader("flat")) {
        sh->start_using();
        sh->set_uniform("view_model_matrix", view);
        sh->set_uniform("projection_matrix", camera.get_projection_matrix());
        glsafe(::glDisable(GL_CULL_FACE));
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
        for (const auto& b : m_buckets)
            if (!b->lit)
                b->model.render();
        // s342b — el resplandor, ENCIMA de todo (sin profundidad): tiene que leerse aunque lo tapen los tubos.
        if (!m_glow.empty()) {
            glsafe(::glDisable(GL_DEPTH_TEST));
            for (const auto& b : m_glow)
                b->model.render();
            glsafe(::glEnable(GL_DEPTH_TEST));
        }
        glsafe(::glDisable(GL_BLEND));
        glsafe(::glEnable(GL_CULL_FACE));
        sh->stop_using();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// panel — s337b, rediseño (boceto aprobado por Neotko): cabecera en una línea, de dónde salen los objetos,
// la capa, una FRANJA FIJA con las cifras de «qué tal vas» y cuatro pestañas (View, Numbers, Settings, A/B).
// Sólo se ve una pestaña a la vez: eso es lo que acorta el panel.
// ═══════════════════════════════════════════════════════════════════════════════════════════════════

namespace {

// ── s337b — AYUDA DE LOS MANDOS: nombre corto, dibujo de qué cambia y dos frases ────────────────────────────
// Neotko: «lo he creado contigo y hasta a mí me cuesta pensar qué estoy tocando». Dentro del gizmo cada mando
// lleva un nombre propio (sin «NS —», que se come el hueco) y, al pasar el ratón, una tarjeta con un DIBUJO de lo
// que cambia en la práctica, con la parte afectada en magenta (el mismo color con el que se resalta en 3D).
// Las etiquetas de la pestaña de Orca no se tocan: esto es sólo el gizmo.
struct ParamHelp { const char* key; const char* label; const char* help; };
const ParamHelp* param_help(const std::string& key)
{
    static const ParamHelp h[] = {
        { "neostroke_min_width_pct",    "Thinnest at tips",
          "How thin a line may get where a stroke ends in a point, like the tip of a wedge. Lower = sharper tips, thinner lines." },
        { "neostroke_max_bead_pct",     "Widest line (limit)",
          "Hard limit: no line is ever wider than this. Past it, NeoStroke adds one more line instead of widening." },
        { "neostroke_continuous_turns", "Join line ends",
          "Go from one line to the next with a small arc, without stopping the flow. Fewer stops, fewer dents." },
        { "neostroke_width_ref",        "What 100 % means",
          "The width every NeoStroke percentage is measured against. 0 = automatic (the solid infill line)." },
        { "neostroke_max_width_pct",    "Target line width",
          "The width each line aims for. It decides how many lines fill a stroke: lower = more, thinner lines." },
        { "neostroke_bead_min_pct",     "Thinnest printable line",
          "The thinnest single line the nozzle really lays down (a bead = one extruded line). Where it gets narrower, "
          "NeoStroke uses fewer, wider lines instead of threads that will not print." },
        { "neostroke_detail_min_pct",   "Thinnest detail",
          "Thinnest line for small details and leftover gaps. A gap that cannot take it stays empty instead of over-extruded." },
        { "neostroke_variable_k",       "Fewer lines when narrow",
          "The line count drops where the stroke narrows (3, 2, 1), so no line goes under the thinnest printable." },
        { "neostroke_max_stroke_width", "Widest shape (mm)",
          "Shapes wider than this are not strokes: they go to the normal infill." },
        { "neostroke_band_mm",          "Limit to a band (mm)",
          "0 = auto: NeoStroke fills every stroke from wall to wall. Above 0 it only fills this far inside the "
          "wall and the middle of wide areas goes to the normal infill. Strokes narrower than twice this are "
          "still filled completely, so small letters do not change." },
        { "neostroke_corner_hooks",     "Reach into corners",
          "Short spurs that reach into inside corners, like the armpits of an H, to weld them." },
        { "neostroke_curve_overlap",    "Extra flow in curves",
          "Extra plastic only on wide lines that curve, to close the seam that opens between them. Over-extrusion on purpose." },
        { "neostroke_lane_overlap",     "Overlap between lines",
          "The same little extra plastic on every line, so each one reaches over its neighbour. Closes the groove "
          "between lines without piling up in tight curves. Try 3-5 % with curve extra flow at 0." },
        { "neostroke_lead_in",          "Lead-in (mm)",
          "Each path starts this far ahead on its own line and runs back to the real start: the weak first bit "
          "after a travel lands where plastic goes again right after. Glossy PLA needs more than matte." },
        { "neostroke_end_at_junctions", "End at junctions",
          "Where paths meet, print them so they END there and start at their free end. Starts together leave a "
          "hole; ends together close (TEST25)." },
        { "neostroke_layer_jitter",     "New start each layer",
          "Start each layer at a different spot, so the small stop dents do not stack into a channel." },
        { "neostroke_skate",            "Glide over lines",
          "Move to the next path over lines already printed, without extruding or retracting." },
        { "neostroke_skate_detour",     "Glide: longest detour",
          "How much longer than the straight jump a glide may be. 1 = only straight glides." },
        { "outer_wall_line_width",      "Outer wall width",
          "Width of the Classic outer wall. NeoStroke plans inside what the wall leaves free, so this moves every path." },
        { "inner_wall_line_width",      "Inner wall width",
          "Width of the Classic inner walls." },
        { "infill_wall_overlap",        "Overlap with the wall",
          "How far NeoStroke overlaps the outer wall. It closes the joint between Classic and NeoStroke (20 % was best on TEST20)." },
    };
    for (const ParamHelp& p : h)
        if (key == p.key) return &p;
    return nullptr;
}

// Un tooltip de verdad: en varias líneas, nunca una sola de lado a lado de la pantalla.
void neo_tip(const std::string& text)
{
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(20.f * neo_u());
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

// El dibujo de un mando, en un lienzo de W × H píxeles con origen en p. Coordenadas normalizadas (0..1).
void draw_param_diagram(ImDrawList* dl, const ImVec2& p, float W, float H, const std::string& key)
{
    const float u = neo_u();
    const ImU32 bg   = neo_col_u32(NeoCol::Canvas);
    const ImU32 wall = IM_COL32(150, 156, 162, 255);
    const ImU32 line = IM_COL32(240, 160, 60, 255);
    const ImU32 hi   = IM_COL32(255, 70, 235, 255);
    const ImU32 dim  = neo_col_u32(NeoCol::TextDim);
    const ImU32 bad  = IM_COL32(235, 70, 70, 255);
    const ImU32 good = IM_COL32(70, 220, 110, 255);
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), bg, 6.f);
    auto P   = [&](float x, float y) { return ImVec2(p.x + x * W, p.y + y * H); };
    auto seg = [&](float x0, float y0, float x1, float y1, float w, ImU32 c) {
        dl->AddLine(P(x0, y0), P(x1, y1), c, w);
        dl->AddCircleFilled(P(x0, y0), 0.5f * w, c, 12);
        dl->AddCircleFilled(P(x1, y1), 0.5f * w, c, 12);
    };
    auto taper = [&](float x0, float x1, float y, float w0, float w1, ImU32 c, float hx = 2.f, ImU32 hc = 0) {
        const int n = 24;
        for (int i = 0; i < n; ++i) {
            const float t0 = float(i) / n, t1 = float(i + 1) / n;
            const float xa = x0 + (x1 - x0) * t0, xb = x0 + (x1 - x0) * t1;
            const float w  = w0 + (w1 - w0) * (0.5f * (t0 + t1));
            seg(xa, y, xb, y, std::max(1.f, w), (hc != 0 && xa >= hx) ? hc : c);
        }
    };
    auto dot  = [&](float x, float y, float r, ImU32 c) { dl->AddCircleFilled(P(x, y), r, c, 16); };
    auto text = [&](float x, float y, const char* t, ImU32 c) { dl->AddText(P(x, y), c, t); };
    auto dash = [&](float x0, float y0, float x1, float y1, ImU32 c, float w) {
        const ImVec2 a = P(x0, y0), b = P(x1, y1);
        const float len = std::hypot(b.x - a.x, b.y - a.y), step = 0.45f * u;
        for (float s = 0.f; s < len; s += 2.f * step) {
            const float e = std::min(len, s + step);
            dl->AddLine(ImVec2(a.x + (b.x - a.x) * s / len, a.y + (b.y - a.y) * s / len),
                        ImVec2(a.x + (b.x - a.x) * e / len, a.y + (b.y - a.y) * e / len), c, w);
        }
    };
    auto arc = [&](float cx, float cy, float r_px, float a0, float a1, ImU32 c, float w) {
        dl->PathArcTo(P(cx, cy), r_px, a0, a1, 24);
        dl->PathStroke(c, false, w);
    };
    const float Lw = 0.9f * u;   // ancho de una línea normal en el dibujo

    if (key == "neostroke_min_width_pct") {
        taper(0.08f, 0.88f, 0.5f, 2.6f * u, 0.15f * u, line, 0.66f, hi);
        text(0.70f, 0.10f, "tip", hi);
    } else if (key == "neostroke_max_bead_pct") {
        seg(0.08f, 0.5f, 0.40f, 0.5f, 2.6f * u, hi);
        text(0.12f, 0.06f, "limit", hi);
        text(0.46f, 0.36f, "->", dim);
        seg(0.60f, 0.33f, 0.92f, 0.33f, 1.2f * u, line);
        seg(0.60f, 0.67f, 0.92f, 0.67f, 1.2f * u, line);
        text(0.62f, 0.80f, "one more line", dim);
    } else if (key == "neostroke_continuous_turns") {
        seg(0.06f, 0.30f, 0.40f, 0.30f, Lw, line); seg(0.06f, 0.70f, 0.40f, 0.70f, Lw, line);
        dot(0.40f, 0.30f, 0.3f * u, bad); dot(0.40f, 0.70f, 0.3f * u, good);
        text(0.10f, 0.80f, "off: stop + start", dim);
        seg(0.54f, 0.30f, 0.80f, 0.30f, Lw, line); seg(0.54f, 0.70f, 0.80f, 0.70f, Lw, line);
        arc(0.80f, 0.50f, 0.20f * H, -float(PI) * 0.5f, float(PI) * 0.5f, hi, Lw);
        text(0.60f, 0.80f, "on: arc", hi);
    } else if (key == "neostroke_width_ref") {
        seg(0.10f, 0.62f, 0.90f, 0.62f, 1.3f * u, line);
        dl->AddLine(P(0.46f, 0.62f), P(0.46f, 0.30f), hi, 1.f);
        text(0.40f, 0.08f, "100 %", hi);
    } else if (key == "neostroke_max_width_pct") {
        seg(0.05f, 0.12f, 0.95f, 0.12f, 0.5f * u, wall); seg(0.05f, 0.88f, 0.95f, 0.88f, 0.5f * u, wall);
        seg(0.08f, 0.32f, 0.92f, 0.32f, Lw, line); seg(0.08f, 0.50f, 0.92f, 0.50f, Lw, hi); seg(0.08f, 0.68f, 0.92f, 0.68f, Lw, line);
        text(0.38f, 0.52f, "each line <= target", hi);
    } else if (key == "neostroke_bead_min_pct") {
        seg(0.06f, 0.40f, 0.40f, 0.40f, 0.18f * u, bad); seg(0.06f, 0.60f, 0.40f, 0.60f, 0.18f * u, bad);
        text(0.06f, 0.72f, "2 threads: no", bad);
        seg(0.58f, 0.50f, 0.92f, 0.50f, 1.1f * u, hi);
        text(0.58f, 0.72f, "1 bead: yes", hi);
    } else if (key == "neostroke_detail_min_pct") {
        dl->AddCircle(P(0.28f, 0.48f), 0.30f * H, wall, 24, 1.5f);
        seg(0.20f, 0.48f, 0.36f, 0.48f, 0.9f * u, hi);
        text(0.14f, 0.82f, "fits: filled", hi);
        dl->AddCircle(P(0.72f, 0.48f), 0.12f * H, wall, 16, 1.5f);
        text(0.60f, 0.82f, "too small: empty", dim);
    } else if (key == "neostroke_variable_k") {
        dl->AddLine(P(0.04f, 0.08f), P(0.96f, 0.36f), wall, 1.5f); dl->AddLine(P(0.04f, 0.92f), P(0.96f, 0.64f), wall, 1.5f);
        seg(0.06f, 0.30f, 0.34f, 0.34f, Lw, line); seg(0.06f, 0.50f, 0.34f, 0.50f, Lw, line); seg(0.06f, 0.70f, 0.34f, 0.66f, Lw, line);
        seg(0.40f, 0.40f, 0.66f, 0.43f, Lw, hi); seg(0.40f, 0.60f, 0.66f, 0.57f, Lw, hi);
        seg(0.72f, 0.50f, 0.94f, 0.50f, Lw, hi);
        text(0.10f, 0.02f, "3", dim); text(0.50f, 0.02f, "2", hi); text(0.80f, 0.14f, "1", hi);
    } else if (key == "neostroke_max_stroke_width") {
        dl->AddRect(P(0.06f, 0.25f), P(0.36f, 0.75f), wall, 3.f, 0, 1.5f);
        seg(0.10f, 0.42f, 0.32f, 0.42f, Lw, line); seg(0.10f, 0.58f, 0.32f, 0.58f, Lw, line);
        text(0.06f, 0.80f, "stroke", dim);
        dl->AddRect(P(0.50f, 0.10f), P(0.94f, 0.90f), hi, 3.f, 0, 1.5f);
        for (int i = 0; i < 7; ++i) dl->AddLine(P(0.52f + i * 0.06f, 0.86f), P(0.56f + i * 0.06f, 0.14f), wall, 1.f);
        text(0.54f, 0.00f, "wider: infill", hi);
    } else if (key == "neostroke_band_mm") {   // s340 — banda junto al muro, centro de relleno
        dl->AddRect(P(0.06f, 0.10f), P(0.94f, 0.90f), wall, 3.f, 0, 1.5f);
        for (int k = 0; k < 2; ++k) {
            const float m = 0.05f + 0.06f * k;
            dl->AddRect(P(0.06f + m, 0.10f + 1.6f * m), P(0.94f - m, 0.90f - 1.6f * m), line, 3.f, 0, Lw);
        }
        for (int i = 0; i < 6; ++i)
            dl->AddLine(P(0.28f + i * 0.08f, 0.66f), P(0.34f + i * 0.08f, 0.34f), hi, 1.f);
        text(0.30f, 0.44f, "infill", hi);
        text(0.08f, 0.00f, "band = NeoStroke", dim);
    } else if (key == "neostroke_corner_hooks") {
        seg(0.20f, 0.10f, 0.20f, 0.90f, 1.4f * u, wall); seg(0.80f, 0.10f, 0.80f, 0.90f, 1.4f * u, wall);
        seg(0.20f, 0.50f, 0.80f, 0.50f, 1.4f * u, wall);
        seg(0.27f, 0.50f, 0.73f, 0.50f, Lw, line);
        taper(0.27f, 0.36f, 0.40f, 0.9f * u, 0.2f * u, hi); taper(0.27f, 0.36f, 0.60f, 0.9f * u, 0.2f * u, hi);
        text(0.40f, 0.10f, "corner spurs", hi);
    } else if (key == "neostroke_curve_overlap") {
        arc(0.50f, 1.05f, 0.80f * H, -float(PI) * 0.85f, -float(PI) * 0.15f, line, 1.6f * u);
        arc(0.50f, 1.05f, 0.44f * H, -float(PI) * 0.85f, -float(PI) * 0.15f, line, 1.6f * u);
        arc(0.50f, 1.05f, 0.62f * H, -float(PI) * 0.80f, -float(PI) * 0.20f, hi, 0.35f * u);
        text(0.04f, 0.04f, "seam closed with extra flow", hi);
    } else if (key == "neostroke_layer_jitter") {
        const float xs[4] = { 0.20f, 0.62f, 0.38f, 0.80f };
        for (int i = 0; i < 4; ++i) {
            const float y = 0.18f + 0.21f * i;
            seg(0.06f, y, 0.94f, y, 0.6f * u, line);
            dot(xs[i], y, 0.28f * u, hi);
        }
    } else if (key == "neostroke_skate") {
        seg(0.06f, 0.25f, 0.45f, 0.25f, Lw, line);
        seg(0.10f, 0.55f, 0.90f, 0.55f, Lw, wall);
        seg(0.55f, 0.85f, 0.94f, 0.85f, Lw, line);
        dash(0.45f, 0.25f, 0.50f, 0.55f, hi, 2.f); dash(0.50f, 0.55f, 0.55f, 0.85f, hi, 2.f);
        text(0.58f, 0.10f, "over printed lines", hi);
    } else if (key == "neostroke_skate_detour") {
        dot(0.10f, 0.75f, 0.3f * u, line); dot(0.90f, 0.75f, 0.3f * u, line);
        dash(0.10f, 0.75f, 0.90f, 0.75f, dim, 1.5f);
        dash(0.10f, 0.75f, 0.30f, 0.20f, hi, 2.f); dash(0.30f, 0.20f, 0.70f, 0.20f, hi, 2.f); dash(0.70f, 0.20f, 0.90f, 0.75f, hi, 2.f);
        text(0.36f, 0.26f, "<= N x straight", hi);
    } else if (key == "outer_wall_line_width") {
        seg(0.05f, 0.18f, 0.95f, 0.18f, 1.2f * u, hi); seg(0.05f, 0.82f, 0.95f, 0.82f, 1.2f * u, hi);
        seg(0.10f, 0.40f, 0.90f, 0.40f, Lw, line); seg(0.10f, 0.60f, 0.90f, 0.60f, Lw, line);
    } else if (key == "inner_wall_line_width") {
        seg(0.05f, 0.14f, 0.95f, 0.14f, 1.0f * u, wall); seg(0.05f, 0.34f, 0.95f, 0.34f, 1.0f * u, hi);
        seg(0.10f, 0.62f, 0.90f, 0.62f, Lw, line);
    } else if (key == "infill_wall_overlap") {
        seg(0.05f, 0.30f, 0.95f, 0.30f, 1.6f * u, wall);
        seg(0.10f, 0.56f, 0.90f, 0.56f, 1.4f * u, line);
        dl->AddRectFilled(P(0.10f, 0.40f), P(0.90f, 0.47f), hi);
        text(0.30f, 0.74f, "overlap into the wall", hi);
    }
}

// Pastillas de opción única, una detrás de otra. Devuelve true si cambió `cur`.
bool neo_seg(const char* id, const char* const* labels, int n, int& cur, const char* const* tips = nullptr)
{
    bool changed = false;
    const float u = neo_u();
    ImGui::PushID(id);
    for (int i = 0; i < n; ++i) {
        ImGui::PushID(i);
        if (i > 0) ImGui::SameLine(0.f, 0.25f * u);
        const ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        const ImVec2 sz(ts.x + 1.1f * u, ImGui::GetFrameHeight());
        if (ImGui::InvisibleButton("##seg", sz) && cur != i) { cur = i; changed = true; }
        const bool   hv = ImGui::IsItemHovered();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        const bool   on = cur == i;
        dl->AddRectFilled(a, b, on ? neo_col_u32(NeoCol::Accent) : neo_col_u32(hv ? NeoCol::SurfaceHi : NeoCol::Surface), 5.f);
        dl->AddText(ImVec2(a.x + 0.55f * u, a.y + (sz.y - ts.y) * 0.5f),
                    on ? ink_on(neo_col_u32(NeoCol::Accent)) : neo_col_u32(NeoCol::Ink), labels[i]);
        if (hv && tips != nullptr && tips[i] != nullptr) neo_tip(tips[i]);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

// Un cuadrito de color con su nombre, en línea.
void neo_swatch(const ColorRGBA& c, const char* name)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  s = ImGui::GetTextLineHeight() * 0.8f;
    const float  dy = (ImGui::GetTextLineHeight() - s) * 0.5f;
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + dy), ImVec2(p.x + s, p.y + dy + s),
        ImGui::ColorConvertFloat4ToU32(ImVec4(c.r(), c.g(), c.b(), 1.f)), 2.f);
    ImGui::Dummy(ImVec2(s, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.f, 0.25f * neo_u());
    ImGui::TextUnformatted(name);
}

// Leyenda en filas: los cuadritos se van colocando mientras quepan.
void neo_legend(const ColorRGBA* cols, const char* const* names, int n)
{
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < n; ++i) {
        if (i > 0) {
            const float need = ImGui::CalcTextSize(names[i]).x + 1.4f * neo_u();
            ImGui::SameLine(0.f, 0.7f * neo_u());
            if (ImGui::GetCursorScreenPos().x + need > right)
                ImGui::NewLine();
        }
        neo_swatch(cols[i], names[i]);
    }
}

std::string fmt1(const char* f, double v) { char b[48]; snprintf(b, sizeof(b), f, v); return b; }

} // namespace

void GLGizmoNeoStroke::on_render_input_window(float x, float y, float bottom_limit)
{
    poll();

    const float win_w = 24.f * neo_u();
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    // 🚨 s337b — ANCHO FIJO por restricción. Con sólo AlwaysAutoResize, todo lo que pide «el ancho que quede»
    //    (barras, baldosas, pestañas, desplegable) hacía crecer la ventana un poco cada frame hasta ocupar la
    //    pantalla entera. La restricción fija el ancho y deja que el alto se ajuste solo.
    ImGui::SetNextWindowSizeConstraints(ImVec2(win_w, 0.f), ImVec2(win_w, FLT_MAX));
    neo_push_window_style();   // 🚨 antes del Begin
    GizmoImguiBegin(on_get_name(), ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);
    ImGui::SetWindowSize(ImVec2(win_w, 0.f), ImGuiCond_Always);
    ImGui::PushTextWrapPos(win_w - 16.f);
    neo_push_panel_style();
    // s337b — «se ve aplastado»: el estilo compartido fija el aire en píxeles, que en Retina es poco al lado de la
    // letra. Aquí va en unidades de letra.
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(0.45f * neo_u(), 0.42f * neo_u()));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.45f * neo_u(), 0.28f * neo_u()));

    // 🚨 Todo el cuerpo en su función: una salida temprana dentro de él no puede saltarse los Pop.
    m_bottom_limit = bottom_limit;
    render_panel_body();

    ImGui::PopStyleVar(2);
    neo_pop_panel_style();
    ImGui::PopTextWrapPos();
    GizmoImguiEnd();
    neo_pop_window_style();
}

void GLGizmoNeoStroke::render_panel_body()
{
    m_hover_key.clear();   // lo vuelve a poner el mando que esté bajo el ratón este frame
    render_header();
    render_source();
    if (m_targets.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextWrapped("%s", m_ext_model ? _u8L("The file has no printable objects.").c_str()
                               : m_has_zone  ? _u8L("No object touches the zone.").c_str()
                                             : _u8L("Select one or more objects on the plate, or draw a zone.").c_str());
        ImGui::PopStyleColor();
        return;
    }
    render_layer();
    render_strip();
    render_tabs();
}

// ── cabecera: candado · nombre · estado · Slice · Auto ─────────────────────────────────────────────────
void GLGizmoNeoStroke::render_header()
{
    const float u  = neo_u();
    const float gs = ImGui::GetFrameHeight();
    if (neo_glyph_toggle("##lock", gs, m_locked, m_locked ? Glyph::Lock : Glyph::Unlock, nullptr))
        set_locked(!m_locked);
    if (ImGui::IsItemHovered())
        neo_tip(m_locked ? _u8L("Locked: the mouse only moves the camera. Click to unlock.")
                         : _u8L("Unlocked: clicks select and move objects. Lock it to look around safely."));
    ImGui::SameLine(0.f, 0.4f * u);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(on_get_name().c_str());

    // A la derecha: Slice y Auto. El estado va en la línea de la capa (aquí se pisaba con el nombre).
    const bool  stale = m_current == nullptr || m_current->key != m_wanted_key;
    const float slice_w = ImGui::CalcTextSize(_u8L("Slice").c_str()).x + 1.2f * u;
    const float auto_w  = 2.1f * u + ImGui::CalcTextSize(_u8L("Auto").c_str()).x + 0.3f * u;
    const float right   = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 0.5f * u, right - slice_w - auto_w - 0.9f * u));
    if (neo_text_button(_u8L("Slice").c_str(), stale ? NeoBtn::Accent : NeoBtn::Normal))
        launch();
    ImGui::SameLine(0.f, 0.4f * u);
    neo_pill_toggle("##auto", &m_auto);
    if (ImGui::IsItemHovered())
        neo_tip(_u8L("Auto: slice again whenever the layer, the objects or their settings change"));
    ImGui::SameLine(0.f, 0.2f * u);
    ImGui::TextUnformatted(_u8L("Auto").c_str());
}

// ── de dónde salen los objetos: Selection · Zone · File, y la lista plegada ─────────────────────────────
void GLGizmoNeoStroke::render_source()
{
    const float u = neo_u();
    int src = m_ext_model ? 2 : (m_has_zone || m_zone_pick) ? 1 : 0;
    const std::string l0 = _u8L("Selection"), l1 = _u8L("Zone"), l2 = _u8L("File");
    const char* labels[] = { l0.c_str(), l1.c_str(), l2.c_str() };
    const std::string t0 = _u8L("The objects selected on the plate");
    const std::string t1 = _u8L("Every object that touches a rectangle you draw on the bed, selected or not");
    const std::string t2 = _u8L("An STL, OBJ or 3MF preview-only, without adding it to the plate. A 3MF keeps each object's settings.");
    const char* tips[] = { t0.c_str(), t1.c_str(), t2.c_str() };
    const int before = src;
    if (neo_seg("src", labels, 3, src, tips) && src != before) {
        if (before == 2) drop_external();
        if (before == 1) { m_has_zone = false; m_zone_pick = m_zone_dragging = false; }
        if (src == 1) m_zone_pick = true;
        if (src == 2) load_external();
        if (src == 0) { rebuild_targets(); m_last_poll = 0.0; }
    }
    // Al lado: el contador que despliega la lista.
    ImGui::SameLine(0.f, 0.6f * u);
    const std::string cnt = std::to_string(m_targets.size()) + " " + _u8L(m_targets.size() == 1 ? "object" : "objects");
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    if (ImGui::Selectable((cnt + (m_objects_open ? "  ^" : "  v") + "##objs").c_str(), false, 0,
                          ImVec2(ImGui::CalcTextSize(cnt.c_str()).x + 1.4f * u, 0.f)))
        m_objects_open = !m_objects_open;
    ImGui::PopStyleColor();

    // Lo propio de cada fuente, en una línea.
    if (src == 1) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(m_zone_pick ? NeoCol::AccentBright : NeoCol::TextDim));
        ImGui::TextUnformatted(m_zone_pick ? _u8L("Drag a rectangle on the bed.").c_str() : _u8L("Zone set.").c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (m_zone_pick) { if (neo_text_button(_u8L("Cancel").c_str(), NeoBtn::Ghost)) m_zone_pick = m_zone_dragging = false; }
        else if (neo_text_button(_u8L("Redraw").c_str(), NeoBtn::Ghost)) m_zone_pick = true;
    } else if (src == 2 && m_ext_model) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextUnformatted(fit_text(m_ext_name, 14.f * u).c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (neo_text_button(_u8L("Other file…").c_str(), NeoBtn::Ghost)) load_external();
    }

    if (!m_objects_open)
        return;
    for (size_t i = 0; i < m_targets.size(); ++i) {
        Target& t = m_targets[i];
        ImGui::PushID(int(i));
        neo_pill_toggle("##vis", &t.visible);
        if (ImGui::IsItemHovered()) neo_tip(_u8L("Show or hide this object's paths"));
        ImGui::SameLine(0.f, 0.4f * u);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(t.visible ? NeoCol::Ink : NeoCol::TextDim));
        ImGui::TextUnformatted(fit_text(t.name, 15.f * u).c_str());
        ImGui::PopStyleColor();
        if (t.has_modifiers) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::Warn));
            ImGui::TextUnformatted("!");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                neo_tip(_u8L("This object has modifiers or negative parts. The preview ignores them."));
        }
        ImGui::PopID();
    }
}

// ── capa: Top · Depth y la línea de lo que se ve ────────────────────────────────────────────────────────
void GLGizmoNeoStroke::render_layer()
{
    const float u = neo_u();
    const int n = ref_layer_count();
    if (n <= 0) {
        ImGui::TextWrapped("%s", _u8L("No layers: is any object visible?").c_str());
        return;
    }
    m_layer = std::clamp(m_layer, 1, n);
    const float lab = 3.0f * u, num = 3.4f * u;
    auto row = [&](const char* id, const std::string& caption, int& v, int lo, int hi, const std::string& tip) {
        ImGui::PushID(id);
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextUnformatted(caption.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) neo_tip(tip);
        ImGui::SameLine(lab);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - num - 0.3f * u);
        ImGui::SliderInt("##s", &v, lo, hi, "");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(num);
        if (ImGui::InputInt("##n", &v, 0, 0))
            v = std::clamp(v, lo, hi);
        v = std::clamp(v, lo, hi);
        ImGui::PopID();
    };
    row("top", _u8L("Layer"), m_layer, 1, n, _u8L("The layer shown, counted on the object chosen in Settings > Object.\n"
                                                  "Other objects show their layer at the same height.\n"
                                                  "The object is cut there: below it you see the part."));
    // s342b — «Depth» RETIRADO (Neotko): laminar varias capas de abajo costaba tiempo y enredaba el elegir. Se
    // lamina SÓLO la capa elegida; lo de abajo se ve como pieza (el corte va en esa capa).
    m_span = 0;

    const int r = ref_target();
    if (r >= 0) {
        const Target& R  = m_targets[size_t(r)];
        const auto&   L  = R.grid.layers[size_t(m_layer - 1)];
        const int     lo = std::max(1, m_layer - m_span);
        char buf[160];
        if (lo < m_layer)
            snprintf(buf, sizeof(buf), "%s %d-%d / %d  ·  ;Z %.3f  ·  h %.2f", _u8L("Layers").c_str(), lo, m_layer, n,
                     L.second + R.grid.print_z_offset + R.world_min_z, L.second - L.first);
        else
            snprintf(buf, sizeof(buf), "%d / %d %s  ·  ;Z %.3f  ·  h %.2f", m_layer, n,
                     m_targets.size() > 1 ? fit_text(R.name, 5.f * u).c_str() : "",
                     L.second + R.grid.print_z_offset + R.world_min_z, L.second - L.first);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + lab);
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextUnformatted(buf);
        ImGui::PopStyleColor();
        // El estado del laminado, al final de la misma línea (ámbar si lo que se ve ya no es lo de ahora).
        const bool stale = m_current == nullptr || m_current->key != m_wanted_key;
        const std::string st = m_running ? _u8L("Slicing…") : stale ? _u8L("Out of date") : m_status;
        ImGui::SameLine(0.f, 0.6f * u);
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(stale && !m_running ? NeoCol::Warn : NeoCol::TextDim));
        ImGui::TextUnformatted(st.c_str());
        ImGui::PopStyleColor();
    }

    // Avisos: sólo los que importan, uno por objeto.
    if (const JobOut* job = shown())
        for (const LayerOut& lo : job->layers) {
            if (lo.target >= m_targets.size()) continue;
            if (!lo.error.empty()) {
                const std::string msg = m_targets[lo.target].name + ": " + lo.error;
                neo_warn_row(("err" + std::to_string(lo.target) + "_" + std::to_string(lo.layer_idx)).c_str(), msg.c_str(), nullptr);
            } else if (!lo.neostroke_active) {
                const std::string msg = m_targets[lo.target].name + ": " + _u8L("not NeoStroke");
                neo_warn_row(("ns" + std::to_string(lo.target)).c_str(), msg.c_str(),
                             _u8L("This object does not slice its walls with NeoStroke: set Wall generator to NeoStroke.").c_str(), false);
                break;
            }
        }
}

// ── franja fija: lo de «qué tal vas de material», siempre a la vista ────────────────────────────────────
void GLGizmoNeoStroke::render_strip()
{
    const JobOut* job = shown();
    NSPrev::LayerMetrics t;
    if (job)
        for (const LayerOut& lo : job->layers)
            if (lo.target < m_targets.size() && m_targets[lo.target].visible)
                t.add(lo.metrics.m);
    const float u   = neo_u();
    const float gap = 0.3f * u;
    const float w   = (ImGui::GetContentRegionAvail().x - 2.f * gap) / 3.f;
    ImGui::Spacing();
    const bool have = job != nullptr && t.interior_mm > 0.0;
    neo_stat_tile("##ok", w, ">= 0.25", have ? fmt1("%.1f %%", t.pct_ok()).c_str() : "-",
                  neo_col_u32(NeoCol::Ink), have ? float(t.pct_ok() / 100.0) : -1.f, nullptr);
    if (ImGui::IsItemHovered()) neo_tip(_u8L("Share of NeoStroke length that is a real bead (footprint 0.25 mm or more). s332: aim for about 95 %."));
    ImGui::SameLine(0.f, gap);
    neo_stat_tile("##st", w, _u8L("starts").c_str(), have ? fmt1("%.0f", double(t.starts)).c_str() : "-",
                  neo_col_u32(NeoCol::Ink), -1.f, nullptr);
    if (ImGui::IsItemHovered()) neo_tip(_u8L("NeoStroke paths started: every start is a flow cut"));
    ImGui::SameLine(0.f, gap);
    neo_stat_tile("##mm", w, _u8L("mm / start").c_str(), have ? fmt1("%.1f", t.mm_per_start()).c_str() : "-",
                  neo_col_u32(NeoCol::Ink), -1.f, nullptr);
    if (ImGui::IsItemHovered()) neo_tip(_u8L("Average length of a continuous path. Longer is better."));

    // La barra de huecos, partida por clase, con su total.
    const double tot = t.gap_total_mm2();
    const ImVec2 p   = ImGui::GetCursorScreenPos();
    const float  bw  = ImGui::GetContentRegionAvail().x, bh = 0.45f * u;
    ImGui::InvisibleButton("##gapbar", ImVec2(bw, bh + ImGui::GetTextLineHeight() + 0.2f * u));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // s337b — lo que más se nota en la pieza, los dos juntos: donde falta (huecos) y donde sobra (batiburrillo).
    const std::string lbl = have ? _u8L("Gaps") + " " + fmt1("%.3f mm2", tot) + "     " + _u8L("Pile-ups") + " " + fmt1("%.2f mm2", t.excess_mm2)
                                 : _u8L("Gaps") + " -";
    dl->AddText(p, neo_col_u32(NeoCol::TextDim), lbl.c_str());
    const float by = p.y + ImGui::GetTextLineHeight() + 0.15f * u;
    dl->AddRectFilled(ImVec2(p.x, by), ImVec2(p.x + bw, by + bh), neo_col_u32(NeoCol::Surface), 3.f);
    if (tot > 0.0) {
        float x0 = p.x;
        for (int k = 0; k < NSPrev::kGapKinds; ++k) {
            const float wk = float(bw * t.gap_mm2[k] / tot);
            if (wk > 0.f)
                dl->AddRectFilled(ImVec2(x0, by), ImVec2(x0 + wk, by + bh),
                                  ImGui::ColorConvertFloat4ToU32(ImVec4(kGap[k].r(), kGap[k].g(), kGap[k].b(), 1.f)));
            x0 += wk;
        }
    }
    if (ImGui::IsItemHovered() && have)
        { std::string b;
          for (int k = 0; k < NSPrev::kGapKinds; ++k)
              b += (k ? "\n" : "") + _u8L(kGapName[k]) + ": " + std::to_string(t.gap_count[k]) + "  " + fmt1("%.3f mm2", t.gap_mm2[k]);
          neo_tip(b); }

    // s337b — los INTERRUPTORES de vista, aquí y siempre a la vista (Neotko): se encienden y apagan mientras se
    // cambian ajustes en cualquier pestaña, para ocultar montones y ver lo que hay debajo.
    {
        const float u = neo_u();
        ImGui::Spacing();
    const float gs = 1.7f * u;
    struct T { const char* id; bool* v; Glyph g; std::string tip; };
    const T toggles[] = {
        { "fp",  &m_real_footprint, Glyph::Patch,  _u8L("Real footprint: what each line really covers (width minus h(1-pi/4)). Off: nominal width.") },
        { "cl",  &m_show_classic,   Glyph::Square, _u8L("Classic wall, in grey") },
        { "gp",  &m_show_gaps,      Glyph::GapMap, _u8L("Gaps. Blue: next to Classic only. Magenta: the joint with Classic. Yellow: inside NeoStroke. Red: a hole (over 0.3 mm2). Purple: a pocket left to the infill, which usually does not print it in lettering.") },
        { "mk",  &m_show_marks,     Glyph::Target, _u8L("Starts (green) and stops (red) of every NeoStroke path") },
        { "tr",  &m_show_travel,    Glyph::Angle,  _u8L("Moves without extrusion: travel and skating") },
        { "ex",  &m_show_excess,    Glyph::Spot,   _u8L("Pile-ups: where NeoStroke lays more plastic than fits (over 130 % averaged over 0.35 mm). It heaps up and the nozzle drags it. The planned overlap with the wall does not count.") },
        { "hl",  &m_show_hills,     Glyph::Patch,  _u8L("Hills, in blue: a little too much plastic all along a stretch (over 105 % averaged over 1 mm).\n"
                                                        "Extra flow in curves does this: each line gets 2-17 % more and there is nowhere for it to go,\n"
                                                        "so it rises into a ridge (TEST25, the half moon on the ring in zones 4, 5 and 6).") },
        { "cut", &m_cut_object,     Glyph::Xray,   _u8L("Cut the object at the shown layers") },
    };
    for (size_t i = 0; i < sizeof(toggles) / sizeof(toggles[0]); ++i) {
        if (i > 0) ImGui::SameLine(0.f, 0.3f * u);
        if (neo_glyph_toggle(toggles[i].id, gs, *toggles[i].v, toggles[i].g, nullptr))
            *toggles[i].v = !*toggles[i].v;
        if (ImGui::IsItemHovered()) neo_tip(toggles[i].tip);
    }
    }
}

// ── pestañas ────────────────────────────────────────────────────────────────────────────────────────────
void GLGizmoNeoStroke::render_tabs()
{
    const float u = neo_u();
    ImGui::Spacing();
    const std::string names[4] = { _u8L("View"), _u8L("Numbers"), _u8L("Settings"), _u8L("A/B") };
    const float w = ImGui::GetContentRegionAvail().x / 4.f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        if (i > 0) ImGui::SameLine(0.f, 0.f);
        if (ImGui::InvisibleButton("##tab", ImVec2(w, ImGui::GetFrameHeight())))
            m_tab = i;
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool on = m_tab == i, hv = ImGui::IsItemHovered();
        const ImVec2 ts = ImGui::CalcTextSize(names[i].c_str());
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f),
                    neo_col_u32(on ? NeoCol::Ink : hv ? NeoCol::AccentBright : NeoCol::TextDim), names[i].c_str());
        dl->AddLine(ImVec2(a.x, b.y - 1.f), ImVec2(b.x, b.y - 1.f),
                    on ? neo_col_u32(NeoCol::Accent) : neo_col_u32(NeoCol::Surface), on ? 2.f : 1.f);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0.f, 0.2f * u));
    switch (m_tab) {
    case tabView:     render_tab_view();       break;
    case tabNumbers:  render_tab_numbers();    break;
    case tabSettings: render_params_section(); break;
    case tabCompare:  render_tab_compare();    break;
    default: break;
    }
}

void GLGizmoNeoStroke::render_tab_view()
{
    const float u = neo_u();
    const std::string m0 = _u8L("Width"), m1 = _u8L("Risks"), m2 = _u8L("Paths"), m3 = _u8L("Flow +"), m4 = _u8L("Owner");
    const char* labels[] = { m0.c_str(), m1.c_str(), m2.c_str(), m3.c_str(), m4.c_str() };
    const std::string t0 = _u8L("By real footprint. Red: a thin line that is loose or on an edge (it fails). Purple: thin but squeezed between neighbours (it fills).");
    const std::string t1 = _u8L("What the TEST22 macro photos showed that fails. Red: a path that starts or ends loose, too fast: the tip stretches and breaks. "
                                "Orange: a wide line next to another wide line: a seam opens. Cyan: a pass over what is already printed. "
                                "Yellow: it does not overlap its neighbour by the material closure: a groove, light through against a lamp (TEST25). "
                                "Pink: a thin loose line.");
    const std::string t2 = _u8L("One colour per continuous path");
    const std::string t3 = _u8L("How much extra plastic the extra flow adds to each line");
    const std::string t4 = _u8L("One colour for all of NeoStroke");
    const char* tips[] = { t0.c_str(), t1.c_str(), t2.c_str(), t3.c_str(), t4.c_str() };
    static const int to_mode[5] = { cmWidth, cmRisks, cmRuns, cmExtra, cmOwner };
    int seg = 0;
    for (int i = 0; i < 5; ++i) if (to_mode[i] == m_color_mode) seg = i;
    if (neo_seg("color", labels, 5, seg, tips))
        m_color_mode = to_mode[seg];

    if (m_color_mode == cmWidth) {
        const ColorRGBA cols[5]  = { kBand[0], kSqueezed, kBand[1], kBand[2], kBand[3] };
        const char*     names[5] = { "<0.25 loose", "squeezed", "0.25-0.40", "0.40-0.60", "0.60+" };
        neo_legend(cols, names, 5);
    } else if (m_color_mode == cmExtra) {
        neo_legend(kExtra, kExtraName, 4);
    } else if (m_color_mode == cmRisks) {
        neo_legend(kRisk, kRiskName, 7);
    }

    // s339 — los dos mandos de los avisos (TEST25). Cambian las cifras, así que relaminan.
    {
        const float lab = 6.2f * u;
        auto frow = [&](const char* id, const std::string& caption, float& v, float lo, float hi, const char* fmt,
                        const std::string& tip) {
            ImGui::PushID(id);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(caption.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) neo_tip(tip);
            ImGui::SameLine(lab);
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::SliderFloat("##v", &v, lo, hi, fmt);
            if (ImGui::IsItemHovered()) neo_tip(tip);
            v = std::clamp(v, lo, hi);
            ImGui::PopID();
        };
        ImGui::Spacing();
        frow("lvl", _u8L("Warning level"), m_warn_level_pct, 25.f, 300.f, "%.0f %%",
             _u8L("How early the preview warns.\n"
                  "100 %: the thresholds measured on the TEST22-25 photos.\n"
                  "Higher: it marks sooner (more marks, some will print fine).\n"
                  "Lower: only what is clearly going to show.\n"
                  "It moves every warning at once: pits, grooves, pile-ups and hills."));
        frow("clo", _u8L("Material closure"), m_closure_mm, -0.05f, 0.10f, "%.3f mm",
             _u8L("How much two lines must overlap so no groove is left between them.\n"
                  "-0.01 (default): only lines that really do not touch.\n"
                  "0: they only have to touch. Planned lines sit exactly side by side, so many light up.\n"
                  "Glossy PLA, which shows every line, wants more (0.02-0.04).\n"
                  "Matte PLA spreads and closes on its own: 0 or below.\n"
                  "Only moves the yellow 'no overlap' warning in Risks.")
             );
    }

}

void GLGizmoNeoStroke::render_tab_numbers()
{
    const JobOut* job = shown();
    if (job == nullptr) {
        ImGui::TextWrapped("%s", _u8L("Nothing sliced yet.").c_str());
        return;
    }
    std::vector<NSPrev::LayerMetrics> per(m_targets.size());
    NSPrev::LayerMetrics total;
    for (const LayerOut& lo : job->layers)
        if (lo.target < per.size() && m_targets[lo.target].visible) {
            per[lo.target].add(lo.metrics.m);
            total.add(lo.metrics.m);
        }
    // Por objeto: las columnas de `s336_zonas_por_caja.py`.
    const float u = neo_u();
    const float cols[] = { 0.f, 8.5f * u, 11.8f * u, 14.6f * u, 17.4f * u };
    auto row = [&](const std::string& name, const std::string& a, const std::string& b, const std::string& c,
                   const std::string& d, bool dim) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(dim ? NeoCol::TextDim : NeoCol::Ink));
        ImGui::TextUnformatted(fit_text(name, cols[1] - 0.3f * u).c_str());
        const std::string* v[] = { &a, &b, &c, &d };
        for (int i = 0; i < 4; ++i) { ImGui::SameLine(cols[i + 1]); ImGui::TextUnformatted(v[i]->c_str()); }
        ImGui::PopStyleColor();
    };
    row("", "mm", ">=0.25", _u8L("starts"), _u8L("gaps"), true);
    for (size_t i = 0; i < per.size(); ++i) {
        if (!m_targets[i].visible) continue;
        const NSPrev::LayerMetrics& m = per[i];
        row(m_targets[i].name, fmt1("%.0f", m.interior_mm), fmt1("%.1f %%", m.pct_ok()), fmt1("%.0f", double(m.starts)),
            fmt1("%.3f", m.gap_total_mm2()), false);
    }
    ImGui::Spacing();
    for (int k = 0; k < NSPrev::kGapKinds; ++k) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s  %zu  ·  %.3f mm2", _u8L(kGapName[k]).c_str(), total.gap_count[k], total.gap_mm2[k]);
        neo_swatch(kGap[k], buf);
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::TextWrapped("%s %.1f mm3  ·  Classic %.0f mm  ·  %s %.1f / %.1f mm", _u8L("NeoStroke plastic").c_str(),
                       total.vol_mm3, total.classic_mm, _u8L("thin loose / squeezed").c_str(),
                       total.thin_loose_mm, total.thin_squeezed_mm);
    ImGui::PopStyleColor();
    ImGui::Spacing();
    // s337b — los RIESGOS del TEST22, en una línea. Ámbar si hay puntas que se rompen o rajas; al pulsarla se
    // pasa al color «Risks», que enseña dónde están.
    {
        const NSPrev::LayerMetrics& t = total;
        const bool bad = t.tip_risk_count > 0 || t.seam_risk_mm > 0.05 || t.knot_count > 0;
        char rb[320];
        snprintf(rb, sizeof(rb), "%s: %zu %s  ·  %zu %s  ·  %.1f mm %s  ·  %.1f mm %s  ·  %.0f mm %s  ·  %.1f mm2 %s",
                 _u8L("Risks").c_str(), t.tip_risk_count, _u8L("tips").c_str(), t.knot_count, _u8L("pits").c_str(),
                 t.seam_risk_mm, _u8L("seams").c_str(), t.double_pass_mm, _u8L("double").c_str(),
                 t.contact_mm, _u8L("grooves").c_str(), t.sustained_mm2, _u8L("hills").c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(bad ? NeoCol::Warn : NeoCol::TextDim));
        if (ImGui::Selectable((std::string(rb) + "##risks").c_str(), m_color_mode == cmRisks)) {
            m_color_mode = cmRisks;
            m_tab = tabView;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            neo_tip(_u8L("From the TEST22 macro photos. Tips: paths that start or end loose above 20 mm/s (the wedge tip held "
                         "at 15 and broke at 30 and 60). Seams: lines 0.6 mm or wider next to each other (zone 6 ring). "
                         "Double: passes over what is already printed (the ironing relief on the zone 3 tip). "
                         "Pits: a path that starts right next to other path ends (TEST24, the wave). The start comes "
                         "after a retraction, short of pressure, and the spot is the same layer after layer, so a small "
                         "pit stacks up. Also two or more path STARTS bunched together, even wide ones (TEST25, the hole "
                         "at the top of every ring). Grooves: NeoStroke lines that do not overlap their neighbour by "
                         "the material closure (TEST25 zone 1: grooves and light through against a lamp). Hills: a "
                         "little too much plastic along a stretch, the ridge that extra flow in curves leaves. Click "
                         "to see them."));
    }
}

void GLGizmoNeoStroke::render_tab_compare()
{
    const float u = neo_u();
    if (neo_text_button(_u8L("Freeze as A").c_str()) && m_current) {
        m_frozen_a       = m_current;
        m_frozen_a_label = params_summary();
        m_show_a         = false;
    }
    if (ImGui::IsItemHovered())
        neo_tip(_u8L("Keep the current result as A. Change settings, then switch between A and now."));
    if (!m_frozen_a) {
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextWrapped("%s", _u8L("Freeze a result, change something, and compare.").c_str());
        ImGui::PopStyleColor();
        return;
    }
    ImGui::SameLine(0.f, 0.6f * u);
    int which = m_show_a ? 0 : 1;
    const std::string la = "A", ln = _u8L("Now");
    const char* labels[] = { la.c_str(), ln.c_str() };
    if (neo_seg("ab", labels, 2, which))
        m_show_a = which == 0;
    ImGui::SameLine(0.f, 0.6f * u);
    if (neo_text_button(_u8L("Drop A").c_str(), NeoBtn::Ghost)) {
        m_frozen_a.reset();
        m_show_a = false;
        return;
    }
    auto total_of = [&](const JobOut* j) {
        NSPrev::LayerMetrics t;
        if (j) for (const LayerOut& lo : j->layers)
            if (lo.target < m_targets.size() && m_targets[lo.target].visible) t.add(lo.metrics.m);
        return t;
    };
    const NSPrev::LayerMetrics a = total_of(m_frozen_a.get());
    const NSPrev::LayerMetrics c = total_of(m_current.get());
    // Verde = mejor, rojo = peor. Más % real y más mm por arranque es mejor; más arranques y huecos, peor.
    auto col = [](double d, bool more_is_better) {
        if (std::abs(d) < 1e-9) return neo_col_u32(NeoCol::Ink);
        const bool good = (d > 0) == more_is_better;
        return good ? IM_COL32(94, 226, 154, 255) : IM_COL32(255, 107, 107, 255);
    };
    const float gap = 0.3f * u;
    const float w   = (ImGui::GetContentRegionAvail().x - 3.f * gap) / 4.f;
    ImGui::Spacing();
    neo_stat_tile("##d1", w, ">= 0.25", fmt1("%+.1f", c.pct_ok() - a.pct_ok()).c_str(), col(c.pct_ok() - a.pct_ok(), true), -1.f, nullptr);
    ImGui::SameLine(0.f, gap);
    neo_stat_tile("##d2", w, _u8L("starts").c_str(), fmt1("%+.0f", double(c.starts) - double(a.starts)).c_str(),
                  col(double(c.starts) - double(a.starts), false), -1.f, nullptr);
    ImGui::SameLine(0.f, gap);
    neo_stat_tile("##d3", w, _u8L("mm / start").c_str(), fmt1("%+.1f", c.mm_per_start() - a.mm_per_start()).c_str(),
                  col(c.mm_per_start() - a.mm_per_start(), true), -1.f, nullptr);
    ImGui::SameLine(0.f, gap);
    neo_stat_tile("##d4", w, _u8L("gaps").c_str(), fmt1("%+.3f", c.gap_total_mm2() - a.gap_total_mm2()).c_str(),
                  col(c.gap_total_mm2() - a.gap_total_mm2(), false), -1.f, nullptr);
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::TextWrapped("A: %s", m_frozen_a_label.c_str());
    ImGui::PopStyleColor();
}

std::string GLGizmoNeoStroke::params_summary() const
{
    // Lo que valían los mandos del objeto editado al congelar A, para saber qué se compara.
    if (m_edit_target < 0 || m_edit_target >= int(m_targets.size()))
        return {};
    const Target& T = m_targets[size_t(m_edit_target)];
    const ModelObject* mo = object_of(T);
    if (mo == nullptr || T.part_volume_idxs.empty())
        return {};
    const int li = std::max(0, layer_for_target(size_t(m_edit_target), m_layer));
    const NSPrev::ConfigSnapshot s = NSPrev::snapshot_for_volume(wxGetApp().plater()->neotko_full_config(), *mo,
                                                                 *mo->volumes[size_t(T.part_volume_idxs.front())], T.grid, li);
    std::string out = T.name + " —";
    for (const ParamGroup& g : param_groups())
        for (const std::string& k : g.keys)
            if (const ConfigOption* o = snap_option(s, k))
                out += " " + k.substr(k.find('_') + 1) + "=" + o->serialize();
    return out;
}

void GLGizmoNeoStroke::render_params_section()
{
    const float u = neo_u();
    // El objeto que se edita, en un desplegable (antes era un botón de radio por objeto en la lista).
    if (m_edit_target < 0 || m_edit_target >= int(m_targets.size()))
        m_edit_target = 0;
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::TextUnformatted(_u8L("Object").c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(3.f * u);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##edit_obj", fit_text(m_targets[size_t(m_edit_target)].name, 16.f * u).c_str())) {
        for (size_t i = 0; i < m_targets.size(); ++i)
            if (ImGui::Selectable((m_targets[i].name + "##" + std::to_string(i)).c_str(), int(i) == m_edit_target)) {
                if (int(i) != m_edit_target) {   // s342 — otro objeto: sin isla elegida y en SU última capa (s342c)
                    m_edit_island = -1; m_island_pick = false; m_edit_buf.clear();
                    m_edit_target = int(i);
                    m_layer       = std::max(1, top_filled_layer());
                }
                m_edit_target = int(i);
            }
        ImGui::EndCombo();
    }
    // s337b — con varios objetos, los cambios van a TODOS (Neotko: se aplicaban sólo al de la lista y no se notaba).
    if (m_targets.size() > 1) {
        const std::string lbl = _u8L("Apply to all") + " " + std::to_string(m_targets.size()) + " " + _u8L("objects");
        neo_row_toggle("editall", lbl.c_str(), &m_edit_all,
                       _u8L("On: every change and preset goes to all the objects you are looking at. "
                            "Off: only to the object chosen above. The values shown are that object's.").c_str());
    }
    const Target& T = m_targets[size_t(m_edit_target)];
    const ModelObject* mo = object_of(T);
    if (mo == nullptr || T.part_volume_idxs.empty())
        return;
    const int li = std::max(0, layer_for_target(size_t(m_edit_target), m_layer));
    const NSPrev::ConfigSnapshot s = NSPrev::snapshot_for_volume(wxGetApp().plater()->neotko_full_config(), *mo,
                                                                 *mo->volumes[size_t(T.part_volume_idxs.front())], T.grid, li);
    // s342 — ajustes por isla. Con una isla elegida sólo se ven los mandos que pueden ser por isla, sin presets.
    if (!T.external)
        render_islands_section(T, *mo);
    const bool isl_mode = m_edit_island >= 0;
    // ── s337b — PRESETS. 🔄 s342: con los VALORES POR DEFECTO de s340 (PrintConfig.cpp), no con los de TEST20-22
    //    (175/117/70 %, curvas 15 %, solape muro 20 %), que metían demasiado plástico (Neotko). Los tres llevan los
    //    mismos anchos y flujos; sólo cambia la velocidad de NeoStroke (muro interior). Infill/Wall overlap 18 % =
    //    lo recomendado para letras en el 2_47 (es clave de Orca, va en el perfil). Si cambian los defaults de
    //    `neostroke_*`, cambiar también aquí.
    if (!isl_mode) {
        struct Preset { const char* name; const char* tip; double speed; };
        static const Preset presets[] = {
            { "Detail",   "15 mm/s: loose tips hold (TEST22 zone 3). Default widths and flows. Slowest, most closed.", 15.0 },
            { "Standard", "30 mm/s with the default widths and flows.",                                                30.0 },
            { "Fast",     "45 mm/s with the default widths and flows. Loose tips can break.",                          45.0 },
        };
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
        ImGui::TextUnformatted(_u8L("Preset").c_str());
        ImGui::PopStyleColor();
        for (const Preset& pr : presets) {
            ImGui::SameLine(0.f, 0.4f * u);
            if (neo_text_button(_u8L(pr.name).c_str())) {
                std::vector<std::pair<std::string, std::shared_ptr<ConfigOption>>> o;
                o.emplace_back("inner_wall_speed",           std::make_shared<ConfigOptionFloats>(std::vector<double>{ pr.speed })); // #794: vector; 1 valor vale para todas las variantes
                o.emplace_back("neostroke_max_bead_pct",     std::make_shared<ConfigOptionPercent>(155.0));
                o.emplace_back("neostroke_max_width_pct",    std::make_shared<ConfigOptionPercent>(100.0));
                o.emplace_back("neostroke_bead_min_pct",     std::make_shared<ConfigOptionPercent>(60.0));
                o.emplace_back("neostroke_min_width_pct",    std::make_shared<ConfigOptionPercent>(23.0));
                o.emplace_back("neostroke_curve_overlap",    std::make_shared<ConfigOptionPercent>(2.0));
                o.emplace_back("neostroke_lane_overlap",     std::make_shared<ConfigOptionPercent>(0.0));
                o.emplace_back("neostroke_lead_in",          std::make_shared<ConfigOptionFloat>(0.4));
                o.emplace_back("infill_wall_overlap",        std::make_shared<ConfigOptionPercent>(18.0));
                o.emplace_back("neostroke_continuous_turns", std::make_shared<ConfigOptionBool>(true));
                o.emplace_back("neostroke_variable_k",       std::make_shared<ConfigOptionBool>(true));
                commit_options(T.obj_idx, std::string("NeoStroke preset: ") + pr.name, std::move(o));
            }
            if (ImGui::IsItemHovered())
                neo_tip(_u8L(pr.tip));
        }
    }

    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::TextWrapped("%s", T.external
        ? _u8L("External model: changes stay inside the preview.").c_str()
        : isl_mode ? _u8L("Changes go to this island only. Filled dot: its own value (click to go back to the object's).").c_str()
                   : _u8L("Changes go to this object. Filled dot: its own value (click to go back to global).").c_str());
    ImGui::PopStyleColor();

    // 🚨 La única parte larga del panel: va en un hueco con su propio desplazamiento, del alto que quede hasta
    //    el borde de la ventana, para que el panel nunca se salga de la pantalla.
    const auto& groups = param_groups();
    if (m_group_open.size() != groups.size()) {
        m_group_open.assign(groups.size(), false);
        if (!m_group_open.empty()) m_group_open[0] = true;
    }
    // s337b — el hueco mide lo que MIDE su contenido (grupos + mandos abiertos) y sólo se desplaza si no cabe
    // hasta el borde de la ventana. Antes iba siempre hasta abajo aunque hubiera tres filas.
    // s342 — en modo isla, sólo las claves de la lista blanca (el resto es del objeto).
    auto shown_key = [&](const std::string& k) { return !isl_mode || NeoArachne::is_island_override_key(k); };
    auto group_shown = [&](size_t gi) {
        return std::any_of(groups[gi].keys.begin(), groups[gi].keys.end(), shown_key);
    };
    size_t rows_g = 0, rows_p = 0;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        if (!group_shown(gi)) continue;
        ++rows_g;
        if (m_group_open[gi]) rows_p += size_t(std::count_if(groups[gi].keys.begin(), groups[gi].keys.end(), shown_key));
    }
    const float sp     = ImGui::GetStyle().ItemSpacing.y;
    const float needed = float(rows_g) * (ImGui::GetTextLineHeight() + sp)
                       + float(rows_p) * (ImGui::GetFrameHeight() + sp) + 0.5f * u;
    const float avail  = m_bottom_limit - ImGui::GetCursorScreenPos().y - 1.0f * u;
    const float h      = std::max(6.f * u, std::min(needed, avail));
    ImGui::BeginChild("##params", ImVec2(0.f, h), false);
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        if (!group_shown(gi))
            continue;
        ImGui::PushID(int(gi));
        const bool open = m_group_open[gi];
        if (neo_glyph_toggle("##g", ImGui::GetTextLineHeight(), false, open ? Glyph::ChevD : Glyph::ChevR, nullptr))
            m_group_open[gi] = !open;
        ImGui::SameLine(0.f, 0.3f * u);
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::AccentBright));
        ImGui::TextUnformatted(_u8L(groups[gi].title).c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemClicked())
            m_group_open[gi] = !open;
        if (m_group_open[gi])
            for (const std::string& k : groups[gi].keys)
                if (shown_key(k))
                    render_param(k, s, *mo);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

bool GLGizmoNeoStroke::render_param(const std::string& key, const NSPrev::ConfigSnapshot& snap, const ModelObject& mo)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    const ConfigOption*    cur = snap_option(snap, key);
    if (def == nullptr || cur == nullptr)
        return false;
    // s342 — con una isla elegida, el valor que se ve y se guarda es el de la ISLA (si no lo tiene, el del objeto).
    const bool isl = m_edit_island >= 0 && NeoArachne::is_island_override_key(key);
    std::vector<NeoArachne::NsIslandOverride> anchors;
    std::unique_ptr<ConfigOption>             isl_opt;
    bool isl_own = false;
    if (isl) {
        anchors = anchors_of(mo);
        if (m_edit_island < int(anchors.size()))
            if (const std::string* v = anchors[size_t(m_edit_island)].find(key)) {
                isl_opt.reset(cur->clone());
                try { isl_opt->deserialize(*v); cur = isl_opt.get(); isl_own = true; } catch (...) {}
            }
    }
    const bool   own   = isl ? isl_own : mo.config.has(key);
    const float  u     = neo_u();
    const float  col   = 9.5f * u;   // s337b — panel más estrecho
    const int    obj_idx = m_targets[size_t(m_edit_target)].obj_idx;
    // Guardar: en el objeto, o en el ancla de la isla (nullptr = quitar el valor propio).
    auto put = [&](std::shared_ptr<ConfigOption> o) {
        if (!isl) {
            commit_option(obj_idx, key, std::move(o));
            return;
        }
        if (m_edit_island >= int(anchors.size()))
            return;
        if (o) anchors[size_t(m_edit_island)].set(key, o->serialize());
        else   anchors[size_t(m_edit_island)].erase(key);
        commit_anchors(anchors, "NeoStroke island: " + key);
    };

    ImGui::PushID(key.c_str());
    // «del objeto» / «global»: un punto de acento cuando el objeto tiene su propio valor.
    {
        // Un punto dibujado (no un glifo: la fuente de ImGui puede no traer ● ni ○).
        const float  d = ImGui::GetFrameHeight();
        ImGui::InvisibleButton("##own", ImVec2(0.8f * d, d));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const ImVec2 c(0.5f * (a.x + b.x), 0.5f * (a.y + b.y));
        ImDrawList*  dl = ImGui::GetWindowDrawList();
        if (own) dl->AddCircleFilled(c, 0.2f * d, neo_col_u32(NeoCol::AccentBright));
        else     dl->AddCircle(c, 0.2f * d, neo_col_u32(NeoCol::SurfaceHi), 12, 1.2f);
    }
    if (ImGui::IsItemHovered())
        neo_tip(isl ? (own ? _u8L("This island's own value. Click to go back to the object's value.")
                           : _u8L("The object's value"))
                    : own ? _u8L("This object's own value. Click to go back to the global value.").c_str()
                          : _u8L("Global value (process preset)"));
    if (own && ImGui::IsItemClicked())
        put(nullptr);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    // s337b — el nombre corto del gizmo (sin «NS —»); si el mando no tiene ayuda propia, el de Orca.
    const ParamHelp*  ph    = param_help(key);
    const std::string label = ph ? _u8L(ph->label) : (def->label.empty() ? key : _u8L(def->label.c_str()));
    ImGui::TextUnformatted(fit_text(label, col - 1.6f * u).c_str());
    bool hovered = ImGui::IsItemHovered();
    ImGui::SameLine(col);

    bool changed = false;
    const float field_w = 4.6f * u;
    std::string now_line;   // «Now: …» de la tarjeta de ayuda
    switch (def->type) {
    case coBool: {
        bool v = static_cast<const ConfigOptionBool*>(cur)->value;
        now_line = v ? "on" : "off";
        if (ImGui::Checkbox("##v", &v))
            put(std::make_shared<ConfigOptionBool>(v)), changed = true;
        break;
    }
    case coFloat:
    case coPercent:
    case coFloatOrPercent: {
        // 🚨 Leer con el tipo EXACTO (memoria `lessons_code_traps`: opt_float sobre un coPercent = crash).
        double v = 0.0; bool pct = def->type == coPercent;
        if (def->type == coFloatOrPercent) {
            const auto* o = static_cast<const ConfigOptionFloatOrPercent*>(cur);
            v = o->value; pct = o->percent;
        } else {
            v = static_cast<const ConfigOptionFloat*>(cur)->value;   // coPercent hereda de coFloat
        }
        // El buffer sólo se sincroniza con la config cuando el campo NO se está editando.
        auto it = m_edit_buf.find(key);
        if (it == m_edit_buf.end()) it = m_edit_buf.emplace(key, v).first;
        ImGui::SetNextItemWidth(field_w);
        const char* fmt = pct ? "%.0f %%" : "%.3f";
        const bool edited = ImGui::InputDouble("##v", &it->second, 0.0, 0.0, fmt);
        const bool active = ImGui::IsItemActive();
        hovered = hovered || ImGui::IsItemHovered();
        // 🚨 El orden importa. El frame del Enter el campo YA no está activo: si antes se devuelve el buffer al
        //    valor de la config, la comprobación de abajo compara el valor viejo consigo mismo y no se guarda
        //    nada (era el «escribo, Enter y no cambia»). Primero se guarda; el buffer se resincroniza después.
        const bool commit = ImGui::IsItemDeactivatedAfterEdit() && std::abs(it->second - v) > 1e-9;
        if (commit) {
            std::shared_ptr<ConfigOption> o;
            if (def->type == coPercent)             o = std::make_shared<ConfigOptionPercent>(it->second);
            else if (def->type == coFloat)          o = std::make_shared<ConfigOptionFloat>(it->second);
            else                                    o = std::make_shared<ConfigOptionFloatOrPercent>(it->second, pct);
            put(o);
            changed = true;
        } else if (!active && !edited) {
            it->second = v;
        }
        // El % en mm, al lado (paso previo al % → mm de §15a).
        const double mm = pct ? pct_to_mm(key, v, snap) : -1.0;
        now_line = pct ? fmt1("%.0f %%", v) : fmt1("%.3f", v);
        if (mm >= 0.0) now_line += "  =  " + fmt1("%.3f mm", mm);
        if (mm >= 0.0) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::Text("%.3f mm", mm);
            ImGui::PopStyleColor();
        } else if (key == "neostroke_band_mm") {   // s340 — 0 = auto
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::TextUnformatted(v <= 1e-6 ? "auto (no limit)" : "then infill");
            ImGui::PopStyleColor();
            if (v <= 1e-6) now_line += "  (auto)";
        } else if (key == "neostroke_width_ref" && v <= 1e-6) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            ImGui::Text("auto %.3f mm", NSPrev::neostroke_width_ref_mm(snap));
            ImGui::PopStyleColor();
        }
        break;
    }
    default:
        ImGui::TextUnformatted(cur->serialize().c_str());
        break;
    }
    if (hovered) {
        m_hover_key = key;
        // La tarjeta: nombre, el DIBUJO de lo que cambia (magenta = lo afectado, como en 3D), dos frases y el valor.
        ImGui::BeginTooltip();
        const float cw = 17.f * u, ch = 4.6f * u;
        ImGui::PushTextWrapPos(cw);
        ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::AccentBright));
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
        if (ph) {
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            draw_param_diagram(ImGui::GetWindowDrawList(), p0, cw, ch, key);
            ImGui::Dummy(ImVec2(cw, ch));
            ImGui::TextUnformatted(_u8L(ph->help).c_str());
        } else if (!def->tooltip.empty()) {
            ImGui::TextUnformatted(_u8L(def->tooltip.c_str()).c_str());
        }
        if (!now_line.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
            const std::string whose = isl ? (own ? _u8L("this island") : _u8L("object"))
                                          : (own ? _u8L("this object") : _u8L("global"));
            ImGui::TextUnformatted((_u8L("Now") + ": " + now_line + "  (" + whose + ")").c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return changed;
}

void GLGizmoNeoStroke::commit_option(int obj_idx, const std::string& key, std::shared_ptr<ConfigOption> opt)
{
    // s337b — un mando es un «varios» de uno: así va también a todos los objetos si la casilla está puesta.
    // opt == nullptr = quitar el valor propio del objeto (volver al global).
    std::vector<std::pair<std::string, std::shared_ptr<ConfigOption>>> o;
    o.emplace_back(key, std::move(opt));
    commit_options(obj_idx, "NeoStroke Preview: " + key, std::move(o));
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// B2 — la zona
// ═══════════════════════════════════════════════════════════════════════════════════════════════════

bool GLGizmoNeoStroke::mouse_to_bed(const Vec2d& mpos, Vec2d& out)
{
    // El rayo del ratón contra el plano de la cama (z = 0).
    const Linef3 ray = m_parent.mouse_ray(Point(coord_t(mpos.x()), coord_t(mpos.y())));
    const double dz  = ray.b.z() - ray.a.z();
    if (std::abs(dz) < 1e-9)
        return false;   // mirando en paralelo a la cama
    const double t = -ray.a.z() / dz;
    const Vec3d  p = ray.a + t * (ray.b - ray.a);
    out = Vec2d(p.x(), p.y());
    return true;
}

bool GLGizmoNeoStroke::on_mouse(const wxMouseEvent& mouse_event)
{
    // s342 — elegir isla con clic: el rayo del ratón contra el plano de la capa elegida, y la isla que contiene el
    // punto. Si ya tiene ancla, se edita esa; si no, se crea una en el punto del clic.
    if (m_island_pick) {
        if (mouse_event.LeftDown()) {
            double top = 0.0;
            const std::vector<LayerIsland> isl = edit_layer_islands(&top);
            const Linef3 ray = m_parent.mouse_ray(Point(coord_t(mouse_event.GetX()), coord_t(mouse_event.GetY())));
            const double dz  = ray.b.z() - ray.a.z();
            if (std::abs(dz) > 1e-9 && !isl.empty()) {
                const double t = (top - ray.a.z()) / dz;
                const Vec3d  p = ray.a + t * (ray.b - ray.a);
                const Point  pt(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
                for (const LayerIsland& li : isl)
                    if (li.poly.contains(pt)) {
                        if (li.anchor >= 0)
                            m_edit_island = li.anchor;
                        else
                            add_anchor_in(li.poly, Vec2d(p.x(), p.y()), true);
                        m_edit_buf.clear();
                        m_island_pick  = false;
                        m_island_hover = -1;
                        break;
                    }
            }
            m_parent.set_as_dirty();
            return true;   // 🚨 consumido: si llega al lienzo, deselecciona y cierra el gizmo
        }
        if (mouse_event.Moving()) {   // s342b — resaltar la isla bajo el ratón mientras se elige
            double top = 0.0;
            const std::vector<LayerIsland> isl = edit_layer_islands(&top);
            int hov = -1;
            const Linef3 ray = m_parent.mouse_ray(Point(coord_t(mouse_event.GetX()), coord_t(mouse_event.GetY())));
            const double dz  = ray.b.z() - ray.a.z();
            if (std::abs(dz) > 1e-9) {
                const Vec3d p = ray.a + ((top - ray.a.z()) / dz) * (ray.b - ray.a);
                const Point pt(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
                for (size_t k = 0; k < isl.size(); ++k)
                    if (isl[k].poly.contains(pt)) { hov = int(k); break; }
            }
            if (hov != m_island_hover) {
                m_island_hover = hov;
                m_parent.set_as_dirty();
            }
            return false;   // el movimiento sigue siendo de la cámara
        }
        return mouse_event.LeftUp();
    }
    if (!m_zone_pick)
        return false;   // fuera del modo zona el ratón es de la cámara y de la selección, como siempre
    const Vec2d mpos(mouse_event.GetX(), mouse_event.GetY());
    Vec2d bed;
    if (mouse_event.LeftDown()) {
        if (!mouse_to_bed(mpos, bed))
            return true;
        m_zone_a = m_zone_b = bed;
        m_zone_dragging = true;
        m_parent.set_as_dirty();
        return true;   // 🚨 consumido: si llega al lienzo, deselecciona y cierra el gizmo por debajo
    }
    if (m_zone_dragging && (mouse_event.Dragging() || mouse_event.Moving())) {
        if (mouse_to_bed(mpos, bed))
            m_zone_b = bed;
        m_parent.set_as_dirty();
        m_parent.request_extra_frame();
        return true;
    }
    if (m_zone_dragging && mouse_event.LeftUp()) {
        if (mouse_to_bed(mpos, bed))
            m_zone_b = bed;
        m_zone_dragging = false;
        m_zone_pick     = false;
        // Un clic sin arrastre no es una zona.
        if (std::abs(m_zone_b.x() - m_zone_a.x()) > 0.5 && std::abs(m_zone_b.y() - m_zone_a.y()) > 0.5) {
            m_has_zone = true;
            rebuild_targets();
            m_last_poll = 0.0;
        }
        m_parent.set_as_dirty();
        return true;
    }
    // La rueda y el botón derecho siguen siendo de la cámara también mientras se elige.
    return mouse_event.LeftUp() || mouse_event.LeftDown();
}

void GLGizmoNeoStroke::render_zone_rect()
{
    if (!m_has_zone && !m_zone_dragging)
        return;
    const float x0 = float(std::min(m_zone_a.x(), m_zone_b.x())), x1 = float(std::max(m_zone_a.x(), m_zone_b.x()));
    const float y0 = float(std::min(m_zone_a.y(), m_zone_b.y())), y1 = float(std::max(m_zone_a.y(), m_zone_b.y()));
    const float z  = 0.05f;
    GLModel::Geometry g;
    g.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P3 };
    g.add_vertex(Vec3f(x0, y0, z)); g.add_vertex(Vec3f(x1, y0, z));
    g.add_vertex(Vec3f(x1, y1, z)); g.add_vertex(Vec3f(x0, y1, z));
    g.add_line(0, 1); g.add_line(1, 2); g.add_line(2, 3); g.add_line(3, 0);
    m_zone_model.reset();
    m_zone_model.init_from(std::move(g));
    m_zone_model.set_color(m_zone_dragging ? ColorRGBA(1.f, 0.75f, 0.16f, 1.f) : ColorRGBA(0.f, 0.75f, 0.68f, 1.f));
    GLShaderProgram* sh = wxGetApp().get_shader("flat");
    if (sh == nullptr)
        return;
    const Camera& camera = wxGetApp().plater()->get_camera();
    sh->start_using();
    sh->set_uniform("view_model_matrix", camera.get_view_matrix());
    sh->set_uniform("projection_matrix", camera.get_projection_matrix());
    glsafe(::glLineWidth(2.f));
    m_zone_model.render();
    glsafe(::glLineWidth(1.f));
    sh->stop_using();
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// B4 — modelo externo
// ═══════════════════════════════════════════════════════════════════════════════════════════════════

const ModelObject* GLGizmoNeoStroke::object_of(const Target& t) const
{
    const Model* model = t.external ? m_ext_model.get() : m_parent.get_selection().get_model();
    if (model == nullptr || t.obj_idx < 0 || t.obj_idx >= int(model->objects.size()))
        return nullptr;
    const ModelObject* mo = model->objects[size_t(t.obj_idx)];
    return (mo != nullptr && mo->id() == t.object_id) ? mo : nullptr;
}

void GLGizmoNeoStroke::load_external()
{
    wxFileDialog dlg(wxGetApp().mainframe, _L("Load a model to preview"), wxEmptyString, wxEmptyString,
                     "Models (*.stl;*.3mf;*.obj)|*.stl;*.STL;*.3mf;*.3MF;*.obj;*.OBJ", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const std::string path = into_u8(dlg.GetPath());
    std::unique_ptr<Model> m;
    try {
        DynamicPrintConfig        file_cfg;   // la config de impresora/proceso del fichero NO se usa: manda la tuya
        ConfigSubstitutionContext subs(ForwardCompatibilitySubstitutionRule::Enable);
        m = std::make_unique<Model>(Model::read_from_file(path, &file_cfg, &subs,
                LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    } catch (const std::exception& e) {
        m_status = _u8L("Could not load") + ": " + e.what();
        return;
    }
    if (!m || m->objects.empty()) {
        m_status = _u8L("The file has no objects.");
        return;
    }
    // Al suelo y al centro de la placa activa, todos juntos (se conserva cómo están colocados entre sí).
    BoundingBoxf3 all;
    for (ModelObject* mo : m->objects) {
        mo->ensure_on_bed();
        for (size_t ii = 0; ii < mo->instances.size(); ++ii)
            all.merge(mo->instance_bounding_box(ii));
    }
    const Vec3d plate_c = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_plate_box().center();
    const Vec3d shift(plate_c.x() - all.center().x(), plate_c.y() - all.center().y(), 0.0);
    for (ModelObject* mo : m->objects)
        for (ModelInstance* mi : mo->instances)
            mi->set_offset(mi->get_offset() + shift);

    m_ext_model = std::move(m);
    m_ext_name  = boost::filesystem::path(path).filename().string();
    m_frozen_a.reset();
    m_show_a = false;
    m_current.reset();
    if (!m_plate_hidden) {
        m_parent.toggle_model_objects_visibility(false);
        m_plate_hidden = true;
    }
    rebuild_targets();
    m_last_poll = 0.0;
    m_parent.set_as_dirty();
}

void GLGizmoNeoStroke::drop_external()
{
    m_ext_model.reset();
    m_ext_name.clear();
    m_frozen_a.reset();
    m_show_a = false;
    m_current.reset();
    if (m_plate_hidden) {
        m_parent.toggle_model_objects_visibility(true);
        m_plate_hidden = false;
    }
    rebuild_targets();
    m_last_poll = 0.0;
    m_parent.set_as_dirty();
}

void GLGizmoNeoStroke::set_locked(bool v)
{
    m_locked = v;
    m_parent.set_neotko_selection_lock(v && get_state() == On);
    m_parent.set_as_dirty();
}

void GLGizmoNeoStroke::commit_options(int obj_idx, const std::string& snapshot_name,
                                      std::vector<std::pair<std::string, std::shared_ptr<ConfigOption>>> opts,
                                      bool only_edit_target)
{
    // A quién: el objeto editado, o todos los mirados si la casilla está puesta (cada objeto una vez, aunque tenga
    // varias instancias). Diferido por la re-entrada; todo en UN snapshot de deshacer.
    struct Dest { bool external; int obj_idx; ObjectID oid; };
    std::vector<Dest> dests;
    auto push = [&](const Target& t) {
        for (const Dest& d : dests) if (d.external == t.external && d.obj_idx == t.obj_idx) return;
        dests.push_back({ t.external, t.obj_idx, t.object_id });
    };
    if (m_edit_all && m_targets.size() > 1 && !only_edit_target) {
        for (const Target& t : m_targets) if (t.visible) push(t);
    } else if (m_edit_target >= 0 && m_edit_target < int(m_targets.size())) {
        push(m_targets[size_t(m_edit_target)]);
    }
    (void)obj_idx;
    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    wxGetApp().CallAfter([this, alive, dests, snapshot_name, opts]() {
        if (!alive->load() || get_state() != On)
            return;
        bool snap_taken = false;
        for (const Dest& d : dests) {
            Model* model = d.external ? m_ext_model.get() : &wxGetApp().model();
            if (model == nullptr || d.obj_idx < 0 || d.obj_idx >= int(model->objects.size()))
                continue;
            ModelObject* mo = model->objects[size_t(d.obj_idx)];
            if (mo == nullptr || mo->id() != d.oid)
                continue;
            if (!d.external && !snap_taken) {
                wxGetApp().plater()->take_snapshot(snapshot_name);
                snap_taken = true;
            }
            for (const auto& [key, opt] : opts) {
                if (opt) mo->config.set_key_value(key, opt->clone());
                else     mo->config.erase(key);
            }
            if (!d.external) {
                ObjectVolumeID ov;
                ov.object = mo;
                wxGetApp().obj_list()->object_config_options_changed(ov);
                wxGetApp().obj_list()->changed_object(d.obj_idx);
            }
        }
        for (const auto& kv : opts) m_edit_buf.erase(kv.first);
        m_last_poll = 0.0;
    });
}


// ═══════════════════════════════════════════════════════════════════════════════════════════════════
// s342 — AJUSTES POR ISLA (B.3). Formato y reglas en `NeoStrokeIslands.hpp`; el motor en `NeoStroke.cpp`.
// ═══════════════════════════════════════════════════════════════════════════════════════════════════

std::vector<NeoArachne::NsIslandOverride> GLGizmoNeoStroke::anchors_of(const ModelObject& mo) const
{
    // Sólo el valor PROPIO del objeto: las anclas son puntos de ESTE objeto, un valor global no significa nada.
    const auto* o = dynamic_cast<const ConfigOptionString*>(mo.config.option("neostroke_island_overrides"));
    return o ? NeoArachne::parse_island_overrides(o->value) : std::vector<NeoArachne::NsIslandOverride>{};
}

Vec2d GLGizmoNeoStroke::obj_to_world(const Target& t, double x, double y) const
{
    const double* T = t.obj_to_world;
    return Vec2d(T[0] * x + T[1] * y + T[2], T[3] * x + T[4] * y + T[5]);
}

Vec2d GLGizmoNeoStroke::world_to_obj(const Target& t, const Vec2d& w) const
{
    const double* T   = t.obj_to_world;
    const double  det = T[0] * T[4] - T[1] * T[3];
    if (std::abs(det) < 1e-12)
        return w;
    const double dx = w.x() - T[2], dy = w.y() - T[5];
    return Vec2d(( T[4] * dx - T[1] * dy) / det, (-T[3] * dx + T[0] * dy) / det);
}

std::vector<GLGizmoNeoStroke::LayerIsland> GLGizmoNeoStroke::edit_layer_islands(double* top_z) const
{
    std::vector<LayerIsland> out;
    if (m_edit_target < 0 || m_edit_target >= int(m_targets.size()))
        return out;
    const Target& T  = m_targets[size_t(m_edit_target)];
    const int     li = layer_for_target(size_t(m_edit_target), m_layer);
    if (li < 0 || li >= int(T.grid.layers.size()) || T.meshes.empty())
        return out;
    const auto&  L       = T.grid.layers[size_t(li)];
    const double slice_z = T.world_min_z + 0.5 * (L.first + L.second);
    const double top     = T.world_min_z + L.second;
    if (top_z) *top_z = top;
    // 🔑 s342b — cortar la malla aquí mismo, como el hilo del visor (`slice_closing_radius`), pero sin esperar a
    //    que termine: elegir isla no puede depender de que el laminado haya acabado.
    size_t key = 0;
    mix(key, std::hash<size_t>()(T.object_id.id));
    mix(key, std::hash<int>()(li));
    for (const auto& m : T.meshes) mix(key, std::hash<const void*>()(m.get()));
    ExPolygons isl;
    if (key == m_isl_cache_key && !m_isl_cache.empty()) {
        for (const LayerIsland& c : m_isl_cache) isl.push_back(c.poly);
    } else {
        MeshSlicingParamsEx msp;
        msp.closing_radius = 0.049f;
        if (const auto* o = wxGetApp().plater()->neotko_full_config().option<ConfigOptionFloat>("slice_closing_radius"))
            msp.closing_radius = float(std::max(0.0004, o->value));
        ExPolygons all;
        for (const auto& m : T.meshes) {
            std::vector<ExPolygons> sl = slice_mesh_ex(m->its, std::vector<float>{ float(slice_z) }, msp);
            if (!sl.empty()) append(all, std::move(sl.front()));
        }
        isl = union_ex(all);
    }
    // De izquierda a derecha (una línea de texto); a igual X, de arriba abajo.
    std::sort(isl.begin(), isl.end(), [](const ExPolygon& a, const ExPolygon& b) {
        const BoundingBox ba = a.contour.bounding_box(), bb = b.contour.bounding_box();
        if (ba.min.x() != bb.min.x()) return ba.min.x() < bb.min.x();
        return ba.max.y() > bb.max.y();
    });
    out.reserve(isl.size());
    for (ExPolygon& e : isl)
        out.push_back({ std::move(e), -1 });
    // La isla de cada ancla: la PRIMERA de la lista gana, igual que en el motor.
    if (const ModelObject* mo = object_of(T)) {
        const auto anchors = anchors_of(*mo);
        for (size_t a = 0; a < anchors.size(); ++a) {
            const Vec2d w = obj_to_world(T, anchors[a].x, anchors[a].y);
            const Point p(scaled<coord_t>(w.x()), scaled<coord_t>(w.y()));
            for (LayerIsland& L : out)
                if (L.poly.contains(p)) {
                    if (L.anchor < 0) L.anchor = int(a);
                    break;
                }
        }
    }
    if (key != m_isl_cache_key || m_isl_cache.empty()) {
        m_isl_cache     = out;
        m_isl_cache_key = key;
        m_isl_cache_top = top;
    }
    return out;
}

void GLGizmoNeoStroke::commit_anchors(const std::vector<NeoArachne::NsIslandOverride>& list, const std::string& undo_name)
{
    if (m_edit_target < 0 || m_edit_target >= int(m_targets.size()))
        return;
    std::vector<std::pair<std::string, std::shared_ptr<ConfigOption>>> o;
    o.emplace_back("neostroke_island_overrides",
                   list.empty() ? std::shared_ptr<ConfigOption>()
                                : std::make_shared<ConfigOptionString>(NeoArachne::write_island_overrides(list)));
    commit_options(m_targets[size_t(m_edit_target)].obj_idx, undo_name, std::move(o), /*only_edit_target=*/true);
}

void GLGizmoNeoStroke::add_anchor_in(const ExPolygon& island_world, const Vec2d& at_world, bool use_point)
{
    if (m_edit_target < 0 || m_edit_target >= int(m_targets.size()))
        return;
    const Target&      T  = m_targets[size_t(m_edit_target)];
    const ModelObject* mo = object_of(T);
    if (mo == nullptr)
        return;
    Vec2d w = at_world;
    if (!use_point) {
        // Un punto DENTRO de la isla: el centro si cae dentro (una O no), si no, un punto del borde encogido.
        Point p = island_world.contour.centroid();
        if (!island_world.contains(p)) {
            const ExPolygons in = offset_ex(island_world, -float(scaled<double>(0.05)));
            p = !in.empty() && !in.front().contour.points.empty() ? in.front().contour.points.front()
                                                                  : island_world.contour.points.front();
        }
        w = Vec2d(unscale<double>(p.x()), unscale<double>(p.y()));
    }
    auto anchors = anchors_of(*mo);
    NeoArachne::NsIslandOverride a;
    const Vec2d o = world_to_obj(T, w);
    a.x = o.x();
    a.y = o.y();
    for (int n = int(anchors.size()) + 1;; ++n) {   // nombre libre
        const std::string name = "Island " + std::to_string(n);
        if (std::none_of(anchors.begin(), anchors.end(), [&](const auto& x) { return x.name == name; })) {
            a.name = name;
            break;
        }
    }
    anchors.push_back(std::move(a));
    m_edit_island = int(anchors.size()) - 1;
    m_island_wait = 30;
    m_edit_buf.clear();
    commit_anchors(anchors, "NeoStroke: add island");
}

void GLGizmoNeoStroke::render_islands_section(const Target& T, const ModelObject& mo)
{
    const float u = neo_u();
    auto anchors  = anchors_of(mo);
    if (m_island_wait > 0)
        --m_island_wait;
    else if (m_edit_island >= int(anchors.size()))
        m_edit_island = -1;
    const std::vector<LayerIsland> isl = edit_layer_islands();

    // Estado de cada ancla EN ESTA CAPA: -1 = fuera; si no, la isla. `owner` = quién gana esa isla.
    std::vector<int> where(anchors.size(), -1);
    for (size_t a = 0; a < anchors.size(); ++a) {
        const Vec2d w = obj_to_world(T, anchors[a].x, anchors[a].y);
        const Point p(scaled<coord_t>(w.x()), scaled<coord_t>(w.y()));
        for (size_t k = 0; k < isl.size(); ++k)
            if (isl[k].poly.contains(p)) { where[a] = int(k); break; }
    }
    auto label_of = [&](size_t a) {
        std::string l = anchors[a].name.empty() ? "Island" : anchors[a].name;
        if (!anchors[a].values.empty())
            l += "  (" + std::to_string(anchors[a].values.size()) + ")";
        if (where[a] < 0)
            l += "  - " + _u8L("not in this layer");
        else if (isl[size_t(where[a])].anchor != int(a))
            l += "  - " + _u8L("shares with") + " " + anchors[size_t(isl[size_t(where[a])].anchor)].name;
        return l;
    };

    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, neo_col(NeoCol::TextDim));
    ImGui::TextUnformatted(_u8L("Island").c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine(3.f * u);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool editing = m_edit_island >= 0 && m_edit_island < int(anchors.size());
    const std::string cur = editing ? label_of(size_t(m_edit_island)) : _u8L("Whole object");
    if (ImGui::BeginCombo("##edit_island", fit_text(cur, 16.f * u).c_str())) {
        if (ImGui::Selectable(_u8L("Whole object").c_str(), !editing)) {
            m_edit_island = -1;
            m_edit_buf.clear();
        }
        for (size_t a = 0; a < anchors.size(); ++a)
            if (ImGui::Selectable((label_of(a) + "##a" + std::to_string(a)).c_str(), int(a) == m_edit_island)) {
                m_edit_island = int(a);
                m_edit_buf.clear();
            }
        // Las islas de esta capa que aún no tienen ajustes: elegir una la añade.
        bool sep = false;
        for (size_t k = 0; k < isl.size(); ++k) {
            if (isl[k].anchor >= 0)
                continue;
            if (!sep) { ImGui::Separator(); sep = true; }
            if (ImGui::Selectable((_u8L("Add") + ": " + _u8L("island") + " " + std::to_string(k + 1) + "##i" + std::to_string(k)).c_str(), false))
                add_anchor_in(isl[k].poly, Vec2d::Zero(), false);
            if (ImGui::IsItemHovered())
                neo_tip(_u8L("Islands of this layer, left to right. Pick one to give it its own settings.").c_str());
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        neo_tip(_u8L("Give a single island (a letter, a piece of a logo) its own NeoStroke settings.\n"
                     "The island is found by a point inside it, on every layer.\n"
                     "If two letters join in a layer, the first one in the list wins there.").c_str());

    // Clic en la vista, nombre y quitar.
    if (neo_text_button(m_island_pick ? _u8L("Click an island...").c_str() : _u8L("Pick in view").c_str()))
        m_island_pick = !m_island_pick;
    if (ImGui::IsItemHovered())
        neo_tip(_u8L("Then click a letter in the 3D view, on the layer shown.\n"
                     "If it has no settings yet, it is added.").c_str());
    if (editing) {
        if (m_island_name_for != m_edit_island) {
            m_island_name_buf = anchors[size_t(m_edit_island)].name;
            m_island_name_for = m_edit_island;
        }
        ImGui::SameLine(0.f, 0.4f * u);
        char buf[64];
        snprintf(buf, sizeof(buf), "%s", m_island_name_buf.c_str());
        ImGui::SetNextItemWidth(6.f * u);
        if (ImGui::InputText("##island_name", buf, sizeof(buf)))
            m_island_name_buf = buf;
        if (ImGui::IsItemDeactivatedAfterEdit() && m_island_name_buf != anchors[size_t(m_edit_island)].name) {
            anchors[size_t(m_edit_island)].name = m_island_name_buf;
            commit_anchors(anchors, "NeoStroke: rename island");
        }
        if (ImGui::IsItemHovered())
            neo_tip(_u8L("Name of this island").c_str());
        ImGui::SameLine(0.f, 0.4f * u);
        if (neo_text_button(_u8L("Remove").c_str())) {
            anchors.erase(anchors.begin() + m_edit_island);
            m_edit_island     = -1;
            m_island_name_for = -2;
            m_edit_buf.clear();
            commit_anchors(anchors, "NeoStroke: remove island");
        }
        if (ImGui::IsItemHovered())
            neo_tip(_u8L("This island goes back to the object's settings").c_str());
    }
}


// s342b — RESPLANDOR de isla, como el realce de los Sandwich: una franja blanca por fuera del borde que se
// desvanece (dos anillos: fuerte pegado al borde, suave más lejos) y un velo tenue encima. Blanco porque en el
// visor no hay nada blanco: el rojo, el rosa y el azul ya son contornos y caminos (Neotko: «muy parecido a otros»).
void GLGizmoNeoStroke::rebuild_glow()
{
    int sel_idx = -1;   // índice en `edit_layer_islands` de la isla elegida
    double top = 0.0;
    const std::vector<LayerIsland> isl = (m_edit_island >= 0 || m_island_pick) ? edit_layer_islands(&top)
                                                                              : std::vector<LayerIsland>{};
    for (size_t k = 0; k < isl.size(); ++k)
        if (m_edit_island >= 0 && isl[k].anchor == m_edit_island) { sel_idx = int(k); break; }
    const int hov_idx = (m_island_pick && m_island_hover >= 0 && m_island_hover < int(isl.size())
                         && m_island_hover != sel_idx) ? m_island_hover : -1;
    size_t key = 0;
    mix(key, m_isl_cache_key);
    mix(key, std::hash<int>()(sel_idx));
    mix(key, std::hash<int>()(hov_idx));
    mix(key, std::hash<double>()(top));
    if (key == m_glow_key && (sel_idx >= 0 || hov_idx >= 0) == !m_glow.empty())
        return;
    m_glow_key = key;
    m_glow.clear();

    auto add_fill = [&](const ExPolygons& area, const ColorRGBA& col, double z) {
        GLModel::Geometry g;
        g.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3 };
        for (const ExPolygon& e : area) {
            const std::vector<Vec3d> tri = triangulate_expolygon_3d(e, z);
            for (size_t i = 0; i + 2 < tri.size(); i += 3) {
                const unsigned int i0 = unsigned(g.vertices_count());
                for (size_t j = 0; j < 3; ++j)
                    g.add_vertex(Vec3f(tri[i + j].cast<float>()));
                g.add_triangle(i0, i0 + 1, i0 + 2);
            }
        }
        if (g.is_empty())
            return;
        auto b = std::make_unique<Bucket>();
        b->color = col; b->lit = false; b->lines = false;
        b->model.init_from(std::move(g));
        b->model.set_color(col);
        m_glow.push_back(std::move(b));
    };
    auto glow = [&](const ExPolygon& poly, float strength) {
        const ExPolygons base{ poly };
        const ExPolygons r1 = offset_ex(base, float(scaled<double>(0.35)));
        const ExPolygons r2 = offset_ex(base, float(scaled<double>(0.90)));
        add_fill(diff_ex(r2, r1),   ColorRGBA(1.f, 1.f, 1.f, 0.22f * strength), top + 0.06);
        add_fill(diff_ex(r1, base), ColorRGBA(1.f, 1.f, 1.f, 0.70f * strength), top + 0.07);
        add_fill(base,              ColorRGBA(1.f, 1.f, 1.f, 0.12f * strength), top + 0.08);
    };
    if (sel_idx >= 0) glow(isl[size_t(sel_idx)].poly, 1.0f);
    if (hov_idx >= 0) glow(isl[size_t(hov_idx)].poly, 0.55f);
}

}} // namespace Slic3r::GUI
