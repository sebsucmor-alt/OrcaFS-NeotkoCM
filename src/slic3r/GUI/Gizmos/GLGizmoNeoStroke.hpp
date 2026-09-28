// NEOTKO_NEOSTROKE_TAG s337 — EL VISOR DE NEOSTROKE, COMO GIZMO DEL LIENZO 3D.
//
// Por qué un gizmo y no el panel de la ventana Avanzado (idea de Neotko al cerrar s336, pre-plan
// `docs/WIP/NEOSTROKE_VISOR_PREPLAN.md §7`):
//   · La config sale del OBJETO (global → objeto → volumen → rango de capas), no de la pestaña desde la que
//     se abría el panel. Ése era el fallo de base del visor de s335: dibujaba cualquier objeto con los
//     ajustes globales, y en placas con ajustes por objeto enseñaba otra cosa que el G-code.
//   · Los objetos que se miran son los SELECCIONADOS en el lienzo, uno o varios, cada uno con su config.
//   · Los caminos se dibujan sobre la pieza, en su Z, con OpenGL: zoom fluido y sin tope de 8 islas.
// El motor es el MISMO de siempre: `NeoArachne::Preview::preview_slice` → `NeoArachne::Plan::run`.
// Nada de aquí lamina la placa ni toca claves de perfil; lo único que escribe son los mandos de NeoStroke
// del objeto elegido, y lo hace como la lista de objetos (con deshacer).
//
// Lo que enseña (fases del pre-plan):
//   A — la config real de cada objeto          C1 — la huella real (sep = w − h·(1 − π/4))
//   B — uno o varios objetos, varias capas      C2 — Classic en gris, NeoStroke en color
//   D — las cifras de s336 en pantalla          C3 — los huecos: sólo Classic / JUNTA / sólo NeoStroke
//   E — los mandos, con su valor en mm          C4 — arranques y paradas de cada recorrido
//   B5 — A/B: congelar un resultado y alternar
// ⚠️ Lo que NO reproduce: volúmenes modificadores y negativos (avisa), las capas vecinas (el generador corre
//    sin capa de arriba ni de abajo, igual que el panel viejo), y la costura (sin seam_placer).
#ifndef slic3r_GLGizmoNeoStroke_hpp_
#define slic3r_GLGizmoNeoStroke_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLModel.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/NeoArachne/Preview/PreviewConfigSnapshot.hpp"
#include "libslic3r/NeoArachne/Preview/PreviewEffectiveConfig.hpp"
#include "libslic3r/NeoArachne/Preview/PreviewMetrics.hpp"
#include "libslic3r/NeoArachne/Preview/PreviewResult.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r {
class TriangleMesh;
class ConfigOption;
class ModelObject;
class Model;
namespace GUI {

class GLGizmoNeoStroke : public GLGizmoBase
{
public:
    GLGizmoNeoStroke(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);
    ~GLGizmoNeoStroke() override;

    // Cambia la selección (o un deshacer): se rehace la lista de objetos que se miran.
    void data_changed(bool is_serializing) override;

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    void        on_set_state() override;
    void        on_render() override;
    void        on_render_input_window(float x, float y, float bottom_limit) override;
    // B2 — dibujar la zona sobre la cama. Sólo se come el ratón mientras se está eligiendo la zona.
    bool        on_mouse(const wxMouseEvent& mouse_event) override;

private:
    // ── lo que se mira ───────────────────────────────────────────────────────────────────────────────
    struct Target {
        ObjectID    object_id;
        int         obj_idx  = -1;
        int         inst_idx = 0;
        std::string name;
        bool        visible  = true;
        bool        external = false;            // B4: vive en m_ext_model, no en la placa
        bool        has_modifiers = false;   // modificadores o volúmenes negativos: el visor no los aplica
        double      world_min_z   = 0.0;     // la base de la pieza en la placa: z relativa 0 del laminado
        Slic3r::NeoArachne::Preview::ObjectLayerGrid grid;
        std::vector<int>                                 part_volume_idxs;
        std::vector<std::shared_ptr<const TriangleMesh>> meshes;   // en coordenadas de PLACA, una por pieza
    };

    // ── el trabajo del hilo ──────────────────────────────────────────────────────────────────────────
    struct VolumeTask {
        Slic3r::NeoArachne::Preview::ConfigSnapshot snap;
        size_t                                     snap_hash = 0;
        std::shared_ptr<const TriangleMesh>        mesh;
    };
    struct Task {
        size_t                  target   = 0;
        int                     layer_idx = -1;
        double                  slice_z_world = 0.0;
        double                  top_z_world   = 0.0;
        double                  print_z       = 0.0;   // lo que dirá el `;Z:` del G-code
        std::vector<VolumeTask> vols;
        double                  warn_level = 1.0;   // s339 — nivel de aviso (1 = 100 %)
        double                  closure_mm = 0.0;   // s339 — cierre del material
    };
    struct LayerOut {
        size_t      target    = 0;
        int         layer_idx = -1;
        double      top_z_world = 0.0;
        double      print_z     = 0.0;
        double      height      = 0.2;
        double      w_ref_mm    = 0.4;     // la referencia de los % de NeoStroke de este objeto
        double      nozzle_mm   = 0.4;
        bool        neostroke_active = false;
        std::vector<Slic3r::NeoArachne::Preview::PreviewResult> results;
        Slic3r::NeoArachne::Preview::MetricsResult               metrics;
        std::vector<std::vector<unsigned char>>                  seg_flags;   // SegFlag, uno por resultado
        std::string error;
    };
    struct JobOut {
        std::vector<LayerOut> layers;
        double                seconds = 0.0;
        size_t                key     = 0;
        bool                  cancelled = false;
    };

    std::vector<Target> m_targets;

    // ── B2: la zona ─────────────────────────────────────────────────────────────────────────────────
    // Con zona, se mira TODO objeto (y toda instancia) cuya caja toque el rectángulo, esté seleccionado
    // o no. No se recorta la geometría por el borde: eso inventaría muros que el laminado no hace.
    bool   m_zone_pick     = false;   // esperando el arrastre
    bool   m_zone_dragging = false;
    bool   m_has_zone      = false;
    Vec2d  m_zone_a        = Vec2d::Zero();   // esquinas en mm de la cama
    Vec2d  m_zone_b        = Vec2d::Zero();
    GLModel m_zone_model;

    // ── B4: un modelo externo ───────────────────────────────────────────────────────────────────────
    // Se carga en un Model PRIVADO del gizmo: la placa ni se entera (ni deshacer, ni relaminado). Si es un
    // 3MF, cada objeto trae sus ajustes propios. Mientras está cargado, se miran sólo sus objetos y las
    // piezas de la placa se ocultan.
    std::unique_ptr<Model> m_ext_model;
    std::string            m_ext_name;
    bool                   m_plate_hidden = false;
    void   load_external();
    void   drop_external();
    // El ModelObject de un Target (de la placa o del externo); nullptr si ya no existe.
    const ModelObject* object_of(const Target& t) const;
    bool   mouse_to_bed(const Vec2d& mpos, Vec2d& out);
    void   render_zone_rect();
    int   m_layer = 1;          // capa (1-based) del objeto de referencia, el que más capas tiene
    int   m_span  = 0;          // capas de más por arriba y por abajo
    int   m_edit_target = 0;    // el objeto cuyos mandos se editan
    bool  m_edit_all    = true; // s337b — los cambios van a TODOS los objetos mirados (Neotko: no se aplicaba y no se veía)
    bool  m_auto  = true;       // relaminar solo al cambiar algo
    // Candado: el ratón sólo mueve la cámara (ver GLCanvas3D::m_neotko_selection_lock). Se suelta al cerrar.
    bool  m_locked = false;
    void  set_locked(bool v);

    // vistas
    enum ColorMode : int { cmWidth = 0, cmRuns = 1, cmOwner = 2, cmExtra = 3, cmRisks = 4 };
    int   m_color_mode    = cmWidth;
    bool  m_real_footprint = true;   // huella real (sep) o ancho nominal
    bool  m_show_classic  = true;
    bool  m_show_travel   = false;
    bool  m_show_marks    = true;
    bool  m_show_gaps     = true;
    bool  m_show_excess   = true;   // s337b — el batiburrillo
    bool  m_show_hills    = true;   // s339 — la montaña (sobra sostenida)
    // s339 — los dos mandos de los avisos (TEST25): relaminan porque cambian las cifras.
    float m_warn_level_pct = 100.f;   // nivel de aviso
    // Por defecto −0,01: sólo los que de verdad NO se tocan. Con 0 salta cualquier par tangente, que es lo que el campo
    // planea (TEST25: 73 % del NS en las zonas sin curva, 23 % con curva 5 %), y la zona 8 (la mejor) sale igual de
    // marcada que la 1: la diferencia de surco entre ellas no está en la geometría.
    float m_closure_mm     = -0.01f;  // cierre del material
    bool  m_cut_object    = true;

    // resultados
    std::shared_ptr<const JobOut> m_current;
    std::shared_ptr<const JobOut> m_frozen_a;
    std::string                   m_frozen_a_label;
    bool                          m_show_a = false;
    size_t                        m_wanted_key = 0;   // la clave de lo que se ve AHORA en pantalla si se laminara
    double                        m_last_poll  = 0.0;
    std::string                   m_status;

    // hilo
    std::thread                        m_worker;
    std::mutex                         m_mutex;
    std::atomic<bool>                  m_cancel{false};
    bool                               m_running = false;
    bool                               m_rerun   = false;
    std::shared_ptr<JobOut>            m_worker_out;     // lo escribe el hilo bajo m_mutex
    std::shared_ptr<std::atomic<bool>> m_alive;

    // mandos
    std::string                   m_hover_key;       // el mando bajo el ratón: se resalta dónde manda
    std::map<std::string, double> m_edit_buf;

    // GL
    struct Bucket {
        GLModel   model;
        ColorRGBA color;
        bool      lit = true;    // tubos con luz (gouraud) o superficie plana (flat)
        bool      lines = false;
    };
    std::vector<std::unique_ptr<Bucket>> m_buckets;
    const JobOut* m_models_for   = nullptr;
    std::string   m_models_hover;
    size_t        m_models_view_key = 0;
    bool          m_clip_active = false;

    // ── pasos ──
    void   rebuild_targets();
    int    ref_target() const;
    int    ref_layer_count() const;
    // Capa (0-based) del objeto t que cae a la altura de la capa `ref_layer_1based` de la referencia; -1 si no.
    int    layer_for_target(size_t t, int ref_layer_1based) const;
    std::vector<Task> build_tasks(size_t& key) const;
    void   launch();
    void   worker_finished();
    void   poll();
    static std::shared_ptr<JobOut> run_tasks(std::vector<Task> tasks, std::atomic<bool>& cancel);

    // ── dibujo ──
    const JobOut* shown() const;
    size_t view_key() const;
    void   rebuild_models();
    void   update_clipping();
    void   release_clipping();

    // ── panel (s337b, rediseño: cabecera, fuente, capa, franja fija de cifras y 4 pestañas) ──
    enum PanelTab : int { tabView = 0, tabNumbers = 1, tabSettings = 2, tabCompare = 3 };
    int                m_tab = tabView;
    bool               m_objects_open = false;   // la lista de objetos, plegada
    float              m_bottom_limit = 0.f;     // hasta dónde puede bajar el panel (lo da el gestor)
    std::vector<bool>  m_group_open;              // grupos de mandos abiertos (el primero, sí)
    void   render_panel_body();
    void   render_header();
    void   render_source();
    void   render_layer();
    void   render_strip();          // la franja fija: ≥0.25, arranques, mm/arranque, huecos
    void   render_tabs();
    void   render_tab_view();
    void   render_tab_numbers();
    void   render_tab_compare();
    void   render_params_section();
    bool   render_param(const std::string& key, const Slic3r::NeoArachne::Preview::ConfigSnapshot& snap,
                        const ModelObject& mo);
    void   commit_option(int obj_idx, const std::string& key, std::shared_ptr<ConfigOption> opt);
    // s337b — varios mandos de golpe, en UN solo deshacer (los presets Detail / Standard / Fast).
    void   commit_options(int obj_idx, const std::string& snapshot_name,
                          std::vector<std::pair<std::string, std::shared_ptr<ConfigOption>>> opts);
    std::string params_summary() const;
};

}} // namespace Slic3r::GUI

#endif
