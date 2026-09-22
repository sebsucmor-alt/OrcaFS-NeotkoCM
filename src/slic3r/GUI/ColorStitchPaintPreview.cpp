// NEOTKO_PROFILE_TAG_START — s233, ver la cabecera. Cuerpos movidos TAL CUAL desde
// GLGizmoColorStitchPainter.cpp (fallback_color_for_id, color_for_profile,
// gizmo_materials, resolve_object_base_bg, build_ebt_colors_for_volume); el único
// cambio funcional es que el ModelObject dueño llega por parámetro en vez de salir de
// m_c->selection_info(), porque aquí no hay gizmo del que colgar.
#include "ColorStitchPaintPreview.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ColorStitch.hpp"                 // NEOTKO_LOG (canal PROFILE)
#include "libslic3r/SurfaceEffectProfile.hpp"
#include "libslic3r/ColorSci/StackFlatten.hpp"           // sandwich_colour_stacked
#include "libslic3r/TriangleSelector.hpp"                // get_facets por slot (islas)

#include "slic3r/GUI/3DScene.hpp"                        // GLVolume::NEUTRAL_COLOR
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"    // NEOTKO_COLORSTITCH_TAG — request_extra_frame()

#include <algorithm>
#include <cmath>
#include <cstdlib>     // strtoul (tool_col_rgba)
#include <functional>   // std::hash (context_key)
#include <numeric>     // std::iota (union-find de islas)
#include <sstream>   // NEOTKO_LOG arma un ostringstream

namespace Slic3r::GUI::ColorStitchPaintPreview {

ColorRGBA fallback_color_for_id(int id)
{
    // Golden-ratio hue stepping → distinguishable hues for 15 ids.
    const float hue = std::fmod(0.61803398875f * float(id), 1.f);
    const float s = 0.55f, v = 0.85f;
    const float h6 = hue * 6.f;
    const int   i  = int(std::floor(h6)) % 6;
    const float f  = h6 - std::floor(h6);
    const float p = v * (1 - s);
    const float q = v * (1 - f * s);
    const float t = v * (1 - (1 - f) * s);
    float r=0, g=0, b=0;
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
    return ColorRGBA(r, g, b, 1.f);
}

ColorRGBA color_for_profile(const SurfaceEffectProfile& p)
{
    if (p.preview_argb == 0u)
        return fallback_color_for_id(p.id);
    const uint8_t a = (p.preview_argb >> 24) & 0xFF;
    const uint8_t r = (p.preview_argb >> 16) & 0xFF;
    const uint8_t g = (p.preview_argb >>  8) & 0xFF;
    const uint8_t b = (p.preview_argb >>  0) & 0xFF;
    return ColorRGBA(r / 255.f, g / 255.f, b / 255.f, a == 0 ? 1.f : a / 255.f);
}

// NEOTKO_COLORSTITCH_TAG_START — PR.2 palette panel (style strips in the gizmo)
//
// Materiales del contexto actual: color de filamento (project_config) + TD
// (app_config neotko_td_N). Mismo origen que el SandwichDialog.
void materials(Slic3r::ColorSci::Material out[4], std::vector<std::string>& fcolors_out)
{
    fcolors_out.clear();
    if (auto* o = wxGetApp().preset_bundle->project_config
                      .option<ConfigOptionStrings>("filament_colour"))
        fcolors_out = o->values;
    while (fcolors_out.size() < 4) fcolors_out.push_back("#808080");

    auto* ac = wxGetApp().app_config;
    for (int t = 0; t < 4; ++t) {
        float td = 1.f;
        if (ac) {
            const std::string v = ac->get("neotko_td_" + std::to_string(t + 1));
            try { if (!v.empty()) td = std::stof(v); } catch (...) {}
        }
        td = std::max(0.01f, std::min(10.f, td));
        out[t] = Slic3r::ColorSci::material_from_hex(fcolors_out[t], td);
    }
}

// NEOTKO_SANDWICH_TAG — Fase 2 (s167 plan): mirrors the tool-detection half of
// Print::resolve_mixed_filament_sandwich_profiles() (PrintApply.cpp) — same
// "first model_part volume's extruder_id()" pattern, GUI-side, so every
// preview swatch composes against the colour the object is actually assigned
// to instead of the black every ColorSci::sandwich_colour_stacked caller
// hardcoded before this.
bool object_base_bg(const Slic3r::ColorSci::Material mats[4],
                    const ModelObject*               mo,
                    float                            bg_rgb[3])
{
    if (!mo)
        return false;

    int extruder_id = 0;
    for (const ModelVolume* mv : mo->volumes)
        if (mv && mv->is_model_part()) { extruder_id = mv->extruder_id(); break; }
    // ModelVolume::extruder_id() returns 0 when neither the volume nor the
    // object has an explicit "extruder" option set (Model.cpp) — that's NOT
    // "unresolvable", the engine's own default-extruder convention treats an
    // unassigned object as T0. Bailing out here would wrongly fall back to
    // black for the (very common) freshly-imported, no-tool-assigned object.
    if (extruder_id <= 0)
        extruder_id = 1;

    // Same source as the "MixedFilament Object" toggle (mf_num_physical) —
    // filament_colour, not filament_presets, so the physical/virtual boundary
    // matches what that block already treats as authoritative.
    size_t num_physical = 0;
    if (auto* o = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour"))
        num_physical = o->values.size();
    if ((size_t)extruder_id <= num_physical) {
        const int idx = std::clamp(extruder_id - 1, 0, 3);
        bg_rgb[0] = mats[idx].rgb[0];
        bg_rgb[1] = mats[idx].rgb[1];
        bg_rgb[2] = mats[idx].rgb[2];
        return true;
    }

    // Virtual MixedFilament id — approximate with the same TD-aware
    // side-by-side blend_parallel() build_mixed_filament_recipe() uses
    // (ColorPredict.cpp), NOT the naive RGB average the swatch-list display
    // color uses. Known approximation (the real print composes via the
    // engine's actual dither pattern), not a new gap — documented in
    // PAINTED_EFFECTS_PREVIEW_TD_PLAN.md §2.1.
    const MixedFilament* mf = wxGetApp().preset_bundle->mixed_filaments
                                   .mixed_filament_from_id((unsigned)extruder_id, num_physical);
    if (!mf)
        return false;
    const int mix_b = std::clamp(mf->mix_b_percent, 0, 100);
    const int a = std::clamp<int>((int)mf->component_a - 1, 0, 3);
    const int b = std::clamp<int>((int)mf->component_b - 1, 0, 3);
    std::vector<Slic3r::ColorSci::Slice> slices;
    slices.push_back({ a, (100 - mix_b) / 100.f });
    slices.push_back({ b, mix_b / 100.f });
    Slic3r::ColorSci::blend_parallel(slices, mats, bg_rgb);
    return true;
}

std::vector<ColorRGBA> slot_colors(const ModelVolume* mv, const ModelObject* owner)
{
    // Index 0 holds the volume's neutral base, indices 1..COLORSTITCH_SLOT_COUNT-1 hold
    // per-slot colors. TriangleSelectorPatch internally prepends the volume base color,
    // then maps EnforcerBlockerType(N) → m_ebt_colors[N+1]. We mirror MMU's layout:
    //   [0] = volume base (unpainted)
    //   [1..N-1] = per-slot colors
    const int MAX_SLOTS = ModelVolume::COLORSTITCH_SLOT_COUNT;
    std::vector<ColorRGBA> ebt(MAX_SLOTS, ColorRGBA(0.6f, 0.6f, 0.6f, 1.f));
    ebt[0] = GLVolume::NEUTRAL_COLOR;
    int _slots_set = 0, _slots_resolved = 0;   // NEOTKO_COLORSTITCH_TAG — s118 dbg (punto 1)
    if (mv) {
        const auto& mgr = SurfaceEffectProfileManager::get();
        // NEOTKO_COLORSTITCH_TAG — s231 F5: el color de la MALLA se sacaba de
        // `p.preview_argb`, un valor CONGELADO al crear/editar el perfil, mientras que
        // el swatch del panel se re-predice en vivo con los TD actuales
        // (predict_argb_for). Cambiar un TD o un color de filamento actualizaba el
        // panel y NO la mancha del modelo: dos verdades para el mismo color. Aquí el
        // color pasa a DERIVARSE del stack con el mismo motor que el panel — mats/bg se
        // resuelven una sola vez para todos los slots, no por slot.
        Slic3r::ColorSci::Material mats[4];
        std::vector<std::string>   fcolors;
        materials(mats, fcolors);
        float bg[3] = {0.f, 0.f, 0.f};
        object_base_bg(mats, owner, bg);
        for (int s = 1; s < MAX_SLOTS; ++s) {
            const int pid = mv->colorstitch_slot_to_profile_id[s];
            if (pid == 0) continue;
            ++_slots_set;
            if (const SurfaceEffectProfile* p = mgr.find(pid)) {
                // Perfil sin stacks visuales (payload legacy) → no hay nada que
                // componer: se conserva el camino viejo (preview_argb / fallback).
                if (p->stack_top_json.empty() && p->stack_penu_json.empty()) {
                    ebt[s] = color_for_profile(*p);
                } else {
                    const SurfacePassStack st_top  = SurfacePassStack::from_json(p->stack_top_json);
                    const SurfacePassStack st_penu = SurfacePassStack::from_json(p->stack_penu_json);
                    float out[3] = {0.f, 0.f, 0.f};
                    Slic3r::ColorSci::sandwich_colour_stacked(st_top, st_penu, mats, bg, out);
                    ebt[s] = ColorRGBA(std::min(1.f, out[0]), std::min(1.f, out[1]),
                                       std::min(1.f, out[2]), 1.f);
                }
                ++_slots_resolved;
            }
        }
        // NEOTKO_COLORSTITCH_TAG — s118 (punto 1: al cargar no aparecen los
        // pintados). Discrimina las 3 hipótesis en el momento de construir los
        // colores del selector: slots_set=0 → tabla slot→perfil per-volumen NO
        // restaurada (parse/dedup shared-object); slots_set>0 & resolved=0 →
        // perfil no encontrado (manager vacío/timing o id mismatch); set==resolved
        // → colores OK aquí (el problema sería render/llamada).
        NEOTKO_LOG(PROFILE, "EBT_BUILD vol='" << (mv->name.empty() ? "?" : mv->name)
            << "' slots_set=" << _slots_set << " resolved=" << _slots_resolved
            << " mgr_size=" << mgr.size());
    }
    return ebt;
}

// NEOTKO_PROFILE_TAG — s233. Ver la nota de la cabecera: el timestamp de
// colorstitch_paint_facets sólo cubre la GEOMETRÍA pintada; esta clave cubre el COLOR.
uint64_t context_key(const ModelVolume* mv, const ModelObject* owner)
{
    auto mix = [](uint64_t& h, uint64_t v) {
        // splitmix-ish; sirve para detectar cambio, no para criptografía.
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    };
    const std::hash<std::string> hs;
    uint64_t h = 0xcbf29ce484222325ull;

    // Contexto global: colores de filamento + TD.
    if (auto* o = wxGetApp().preset_bundle->project_config
                      .option<ConfigOptionStrings>("filament_colour"))
        for (const std::string& c : o->values) mix(h, hs(c));
    if (auto* ac = wxGetApp().app_config)
        for (int t = 1; t <= 4; ++t) mix(h, hs(ac->get("neotko_td_" + std::to_string(t))));

    // Fondo del objeto: depende del tool asignado.
    if (owner) {
        for (const ModelVolume* v : owner->volumes)
            if (v && v->is_model_part()) { mix(h, (uint64_t)(int64_t)v->extruder_id()); break; }
    }

    // Tabla slot→perfil + contenido de cada perfil referenciado (editar un perfil no
    // cambia su id, pero sí su color).
    if (mv) {
        const auto& mgr = SurfaceEffectProfileManager::get();
        for (int s = 1; s < ModelVolume::COLORSTITCH_SLOT_COUNT; ++s) {
            const int pid = mv->colorstitch_slot_to_profile_id[s];
            if (pid == 0) continue;
            mix(h, (uint64_t)(s * 1000003 + pid));
            if (const SurfaceEffectProfile* p = mgr.find(pid)) {
                mix(h, (uint64_t)p->preview_argb);
                mix(h, hs(p->stack_top_json));
                mix(h, hs(p->stack_penu_json));
            }
        }
    }
    // NEOTKO_COLORSTITCH_TAG — s318 F3: el tejido depende también de la TRANSFORMACIÓN
    // (weave_frame) y del preset (paso de línea, altura de capa, ángulos). Sin esto, escalar o
    // girar la pieza dejaba FUERA del gizmo el tejido calculado antes, porque esta clave no
    // cambiaba. Síntoma que vio el usuario: al escalar, dentro del gizmo bien y fuera mal
    // hasta volver a aplicar el perfil. La traslación de la instancia no entra: el ancla la
    // cancela y mover la pieza no cambia el tejido.
    const std::hash<double> hd;
    if (owner && !owner->instances.empty()) {
        const Transform3d im = owner->instances.front()->get_transformation().get_matrix();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) mix(h, (uint64_t) hd(im(r, c)));
    }
    if (mv) {
        const Transform3d vm = mv->get_matrix();
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) mix(h, (uint64_t) hd(vm(r, c)));
    }
    mix(h, (uint64_t) hd(weave_top_line_spacing()));
    mix(h, (uint64_t) hd(weave_layer_height()));
    mix(h, (uint64_t) hd(double(weave_solid_infill_dir_rad())));
    {
        const auto& pc = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        for (const char* k : { "interlayer_colormix_angle", "interlayer_colormix_penu_angle" })
            if (auto* o = pc.option<ConfigOptionInt>(k)) mix(h, (uint64_t)(int64_t) o->value);
    }
    return h;
}

bool show_outside_gizmo()
{
    auto* ac = wxGetApp().app_config;
    if (!ac) return true;
    const std::string v = ac->get("neotko_show_paint_outside_gizmo");
    return v.empty() || v == "1";   // ausente = ON
}

// ==================================================================================
// s235 F5 — MMU × Sandwich en la GUI. Ver la nota del .hpp.
// ==================================================================================

bool show_in_mmu_gizmo()
{
    auto* ac = wxGetApp().app_config;
    if (!ac) return true;
    const std::string v = ac->get("neotko_mmu_show_sandwich");
    return v.empty() || v == "1";   // ausente = ON
}

uint64_t overlap_key(const ModelObject* mo)
{
    if (!mo) return 0;
    uint64_t h = 0x84222325cbf29ce4ull;
    auto mix = [&h](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part()) continue;
        mix((uint64_t) mv->colorstitch_paint_facets.timestamp());
        mix((uint64_t) mv->mmu_segmentation_facets.timestamp());
    }
    return h;
}

CoexistOverlap mmu_sandwich_overlap(const ModelObject* mo)
{
    CoexistOverlap out;
    if (!mo) return out;

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv || !mv->is_model_part())
            continue;
        // Early-out por los dos lados: sin una de las dos pinturas no hay solape posible y
        // no se paga ni un deserialize (que es lo caro de todo esto).
        if (mv->colorstitch_paint_facets.empty() || mv->mmu_segmentation_facets.empty())
            continue;

        TriangleSelector sw(mv->mesh());
        sw.deserialize(mv->colorstitch_paint_facets.get_data(), false,
                       static_cast<EnforcerBlockerType>(ModelVolume::COLORSTITCH_SLOT_COUNT - 1));
        TriangleSelector mm(mv->mesh());
        mm.deserialize(mv->mmu_segmentation_facets.get_data(), false,
                       EnforcerBlockerType::ExtruderMax);

        std::vector<float> a_sw, a_mm;
        sw.painted_area_per_source_facet(a_sw);
        mm.painted_area_per_source_facet(a_mm);

        const size_t n = std::min(a_sw.size(), a_mm.size());
        for (size_t f = 0; f < n; ++f) {
            out.sandwich_mm2 += a_sw[f];
            if (a_sw[f] <= 0.f || a_mm[f] <= 0.f)
                continue;
            ++out.facets;
            out.area_mm2 += std::min(a_sw[f], a_mm[f]);
        }
        for (size_t f = n; f < a_sw.size(); ++f)
            out.sandwich_mm2 += a_sw[f];
    }
    return out;
}


// ==================================================================================
// Tejido (weave) — s233 F3: bloque movido LITERAL desde GLGizmoColorStitchPainter.cpp.
// Las matemáticas no se han tocado; sólo dejan de ser `static` de aquel fichero y el
// objeto/selector llegan por parámetro en vez de salir del estado del gizmo. Los logs
// TOP_WEAVE / BOTTOM_WEAVE (canal BOTTOM) viajan con el código a propósito: el usuario
// pidió expresamente NO retirarlos porque sirven justo para este frente.
// ==================================================================================

double weave_layer_height(const Slic3r::DynamicPrintConfig* cfg)
{
    const Slic3r::DynamicPrintConfig& c = cfg
        ? *cfg : wxGetApp().preset_bundle->prints.get_edited_preset().config;
    if (auto* o = c.option<ConfigOptionFloat>("layer_height"))
        if (o->value > 0.001) return o->value;
    return 0.2;
}


// NEOTKO_COLORSTITCH_TAG — resolved TOP-surface line width (mm), "casi real": the
// weave runs on top-facing surfaces, so the stripe pitch should match what the slicer
// lays there. Resolved from config WITHOUT a slice (top_surface_line_width → line_width
// → nozzle), so a Calculate/pre-slice pass is not required for the pitch. See on-screen
// notice for the only thing slicing would add (per-layer auto-angle).
double weave_top_line_width(const Slic3r::DynamicPrintConfig* cfg_in)
{
    const Slic3r::DynamicPrintConfig& cfg = cfg_in
        ? *cfg_in : wxGetApp().preset_bundle->prints.get_edited_preset().config;
    double nozzle = 0.4;
    if (auto* nd = wxGetApp().preset_bundle->printers.get_edited_preset()
                       .config.option<ConfigOptionFloats>("nozzle_diameter"))
        if (!nd->values.empty() && nd->values.front() > 0.05) nozzle = nd->values.front();
    // base line width: 0/auto → Orca's ~nozzle default.
    double line_w = nozzle * 1.125;
    if (auto* lw = cfg.option<ConfigOptionFloatOrPercent>("line_width")) {
        const double v = lw->get_abs_value(nozzle);
        if (v > 0.05) line_w = v;
    }
    // top width is ratio_over line_width.
    double top = line_w;
    if (auto* tw = cfg.option<ConfigOptionFloatOrPercent>("top_surface_line_width")) {
        const double v = tw->get_abs_value(line_w);
        if (v > 0.05) top = v;
    }
    return (top > 0.05) ? top : 0.45;
}


// NEOTKO_COLORSTITCH_TAG — s314: separación REAL entre líneas. Ver la nota de la
// declaración en el .hpp para el porqué de la fórmula y la medida contra el gcode.
// Es la única función que debe usarse para convertir milímetros en número de líneas.
double weave_top_line_spacing(const Slic3r::DynamicPrintConfig* cfg_in)
{
    const double w = weave_top_line_width(cfg_in);
    const double h = weave_layer_height(cfg_in);
    // Flow::rounded_rectangle_extrusion_spacing (Flow.cpp:183), literal.
    const double sp = w - h * (1.0 - 0.25 * M_PI);
    // El motor ABORTA el slice si esto sale <= 0 (FlowErrorNegativeSpacing). Aquí es sólo
    // preview, así que se devuelve un mínimo utilizable en vez de reventar el diálogo.
    return (sp > 0.01) ? sp : std::max(0.01, w);
}


// NEOTKO_COLORSTITCH_TAG — dirección base del relleno sólido superior, en radianes.
// Es el ángulo del que parte una banda en AUTO (-1): el motor lo saca de
// `solid_infill_direction` vía calculate_infill_rotation_angle() y luego FillBase.cpp
// le suma +90° en las capas impares. El preview usaba un M_PI/4 clavado, que sólo
// acertaba si solid_infill_direction valía 45 — el default, pero es configurable.
// ⚠️ Si hay plantilla de rotación (`solid_infill_rotate_template` no vacía) el motor
// recalcula por capa con su mini-lenguaje y esto es sólo una aproximación; da igual,
// porque en ese caso la banda va marcada como auto y el aviso lo da el contorno en vez de
// afirmar un ángulo.
float weave_solid_infill_dir_rad()
{
    const auto& cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    double deg = 45.0;
    if (auto* d = cfg.option<ConfigOptionFloat>("solid_infill_direction"))
        deg = d->value;
    return float(deg) * float(M_PI) / 180.f;
}


// NEOTKO_COLORSTITCH_TAG — single source of truth for the ColorStitch per-line tool
// sequence used by ALL previews (3D weave + pro-tray strip), built with the SAME
// engine builders the slicer uses (ColorStitch.cpp:1326+): modes 1-3 →
// build_dithered_tools_2color/_3color / build_custom_bands over n_lines; mode 0 →
// pattern string (digit → 0-based physical tool). penu=false → top-role keys.
// Returns physical tool indices (length n_lines for modes 1-3; pattern length for
// mode 0 → caller tiles). Empty kv ⇒ empty result.
std::vector<int> colorstitch_tool_sequence(
    const std::map<std::string, std::string>& kv, bool penu, int n_lines,
    double spacing_mm)
{
    if (kv.empty()) return {};
    n_lines = std::max(1, n_lines);
    const std::string pre = penu ? "interlayer_colormix_penu_" : "interlayer_colormix_";
    auto gi = [&](const char* k, int d) {
        const auto it = kv.find(pre + k);
        if (it != kv.end()) { try { return std::stoi(it->second); } catch (...) {} }
        return d; };
    auto gd = [&](const char* k, double d) {
        const auto it = kv.find(pre + k);
        if (it != kv.end()) { try { return std::stod(it->second); } catch (...) {} }
        return d; };
    auto gb = [&](const char* k, bool d) {
        const auto it = kv.find(pre + k);
        return it != kv.end() ? (it->second == "1" || it->second == "true") : d; };

    const int    mode = gi("mode", 0);
    const int    ta = gi("tool_a", 0), tb = gi("tool_b", 1), tc = gi("tool_c", 2), td = gi("tool_d", 3);
    const int    pa = gi("pct_a", 50), pb = gi("pct_b", 33);
    const int    ea = gi("easing", 0);
    const double ga = gd("gamma", 1.0), ov = gd("overlap", 0.6);
    const int    ba = gi("band_count_a", 0), bb = gi("band_count_b", 0);
    const int    bc = gi("band_count_c", 0), bd = gi("band_count_d", 0);
    const bool   inv = gb("invert", false);

    std::vector<int> seq;
    switch (mode) {
    case 1: seq = Slic3r::ColorStitch::build_dithered_tools_2color(n_lines, ta, tb, pa, ea, ga); break;
    case 2: seq = Slic3r::ColorStitch::build_dithered_tools_3color(n_lines, ta, tb, tc, pa, pb, ea, ga, ov); break;
    case 3: seq = Slic3r::ColorStitch::build_custom_bands(n_lines, ta, ba, tb, bb, tc, bc, td, bd); break;
    // NEOTKO_COLORSTITCH_TAG — s314: modo 4, bandas en MM. Aquí NO se llama a ningún
    // builder del motor porque en modo 4 el motor tampoco construye una secuencia: cada
    // línea muestrea el diseño (compute_slot_per_line_band_mm). Lo que se replica es esa
    // función, no un builder — misma cuenta, mismas unidades, mismo fmod sobre el periodo.
    case 4: {
        const double ma = gd("band_mm_a", 0.0), mb = gd("band_mm_b", 0.0);
        const double mc = gd("band_mm_c", 0.0), md = gd("band_mm_d", 0.0);
        constexpr double kMinBandMM = 1e-4;
        std::vector<std::pair<double,int>> bands;   // (mm, tool)
        if (ma > kMinBandMM && ta >= 0) bands.emplace_back(ma, ta);
        if (mb > kMinBandMM && tb >= 0) bands.emplace_back(mb, tb);
        if (mc > kMinBandMM && tc >= 0) bands.emplace_back(mc, tc);
        if (md > kMinBandMM && td >= 0) bands.emplace_back(md, td);
        if (bands.size() < 2) break;                // < 2 bandas → sin tejido, como el motor
        if (inv) std::reverse(bands.begin(), bands.end());
        double period = 0.0;
        for (auto& b : bands) period += b.first;
        if (period <= 1e-6) break;
        const double sp = (spacing_mm > 1e-4) ? spacing_mm : weave_top_line_spacing();
        seq.reserve(n_lines);
        for (int i = 0; i < n_lines; ++i) {
            // Centro de la línea i, igual que el centroide que proyecta el motor.
            double r = std::fmod((double(i) + 0.5) * sp, period);
            if (r < 0.0) r += period;
            int slot = int(bands.size()) - 1;
            double acc = 0.0;
            for (size_t k = 0; k < bands.size(); ++k) {
                acc += bands[k].first;
                if (r < acc) { slot = int(k); break; }
            }
            seq.push_back(bands[slot].second);
        }
        // El invert de los modos 0-3 voltea la secuencia entera al final; en modo 4 ya se
        // aplicó volteando el CICLO (igual que en el motor), así que hay que salir aquí
        // para no aplicarlo dos veces.
        return seq;
    }
    default: {   // mode 0 — pattern string, digit → 0-based physical tool ('1' → 0)
        std::string pat;
        auto pit = kv.find(penu ? std::string("interlayer_colormix_pattern_penultimate")
                                : std::string("interlayer_colormix_pattern_top"));
        if (pit == kv.end() || pit->second.empty()) pit = kv.find("pattern");
        if (pit != kv.end()) pat = pit->second;
        for (char ch : pat) if (ch >= '1' && ch <= '9') seq.push_back(ch - '1');
        if (seq.empty()) seq.push_back(ta);
        break;
    }
    }
    if (inv && seq.size() > 1) std::reverse(seq.begin(), seq.end());
    return seq;
}

// PB blob round-trip on a pass. Self-contained (kv only) — unlike the dialog's
// read_pb_blob/write_pb_blob there is no live region config to mirror here.
PathBlendPassConfig pro_pb_read(const SurfacePass& p)
{
    const auto it = p.pathblend.kv.find("blob");
    return PathBlendPassConfig::from_blob_json(
        it != p.pathblend.kv.end() ? it->second : std::string());
}

// Color real de filamento para un tool 0-based, como ColorRGBA. Gris si no se puede
// leer (mismo criterio que el tool_col_u32 del gizmo, pero sin depender de ImGui).
ColorRGBA tool_col_rgba(const std::vector<std::string>& fcolors, int tool0)
{
    if (tool0 >= 0 && tool0 < (int)fcolors.size() && !fcolors[tool0].empty()) {
        std::string s = fcolors[tool0];
        if (!s.empty() && s[0] == '#') s = s.substr(1);
        if (s.size() >= 6) {
            const unsigned long rgb = std::strtoul(s.substr(0, 6).c_str(), nullptr, 16);
            return ColorRGBA(float((rgb >> 16) & 0xFF) / 255.f,
                             float((rgb >>  8) & 0xFF) / 255.f,
                             float((rgb >>  0) & 0xFF) / 255.f, 1.f);
        }
    }
    return ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
}

// Extract the ColorStitch Top pass's config keys (interlayer_colormix_*). Prefers the
// resolved Top stack (same source as the mini-sandwich preview); falls back to the raw
// colorstitch payload (predict swatches with no resolved stack). Empty ⇒ not ColorStitch.
// s231 F7 — extractores por STACK (antes estaban cableados a `stack_top_json`, que es
// justo por lo que el Bottom no tenía forma de llegar al preview: los tres puntos de
// entrada del weave leían Top y sólo Top).
std::map<std::string, std::string>
colorstitch_kv_from_stack(const Slic3r::SurfacePassStack& st)
{
    for (const Slic3r::SurfacePass& p : st.passes)
        if (p.kind == Slic3r::SurfacePassKind::ColorStitch && !p.colorstitch.kv.empty())
            return p.colorstitch.kv;
    return {};
}

bool pathblend_from_stack(const Slic3r::SurfacePassStack& st, PathBlendPassConfig& out)
{
    for (const Slic3r::SurfacePass& p : st.passes)
        if (p.kind == Slic3r::SurfacePassKind::PathBlend) { out = pro_pb_read(p); return true; }
    return false;
}

std::map<std::string, std::string>
colorstitch_top_kv(const Slic3r::SurfaceEffectProfile& prof)
{
    if (!prof.stack_top_json.empty()) {
        const std::map<std::string, std::string> kv =
            colorstitch_kv_from_stack(Slic3r::SurfacePassStack::from_json(prof.stack_top_json));
        if (!kv.empty()) return kv;
    }
    if (prof.colorstitch.present && !prof.colorstitch.kv.empty())
        return prof.colorstitch.kv;
    return {};
}

// NEOTKO_COLORSTITCH_TAG — band orientation for a ColorStitch slot = printed fill lines =
// cm_angle + 90°. `is_auto` set when the kv leaves the angle on auto (-1).
float colorstitch_weave_theta(const std::map<std::string, std::string>& kv, bool& is_auto)
{
    // NEOTKO_COLORSTITCH_TAG — MISMA resolución que el motor, y por el mismo orden:
    // Fill.cpp hace `cm_cfg_override = <config de región>` y luego aplica el kv de la
    // lámina ENCIMA. Así que aquí el fallback de una clave ausente NO puede ser una
    // constante: tiene que ser el valor de la config, o preview y slice se separan otra
    // vez. Caso real: un proyecto guardado con -1 (BIGTEST) trae el -1 en su config y el
    // kv sin la clave — con una constante el preview diría "45 fijo" y sin aviso mientras
    // el gcode alterna, que es exactamente el bug que estamos cerrando.
    // ⚠️ El motor lee la config de REGIÓN y esto lee el preset de impresión editado: si
    // hay override por objeto/región pueden discrepar. Limitación que ya tenía
    // weave_top_line_width(); no se arregla aquí.
    int angle = COLORSTITCH_DEFAULT_ANGLE_DEG;
    {
        const auto& pcfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (auto* a = pcfg.option<ConfigOptionInt>("interlayer_colormix_angle"))
            angle = a->value;
    }
    const auto it = kv.find("interlayer_colormix_angle");
    if (it != kv.end()) { try { angle = std::stoi(it->second); } catch (...) {} }
    float base_rad;
    if (angle >= 0) { base_rad = float(angle) * float(M_PI) / 180.f; is_auto = false; }
    // AUTO: no hay un ángulo único que sea cierto — el relleno alterna base y base+90°
    // dentro de la propia banda. Devolvemos la base REAL (solid_infill_direction) y
    // marcamos is_auto para que el painter la marque con el contorno violeta pulsante.
    else            { base_rad = weave_solid_infill_dir_rad();       is_auto = true;  }
    // NEOTKO_COLORSTITCH_TAG — band direction = cm_angle (verified against the slice). The
    // per-layer alternation that used to scramble this is now locked out for a fixed angle via
    // f->is_using_template_angle in Fill.cpp, so weave == slice == pro-tray bar at cm_angle.
    return base_rad;
}

// NEOTKO_COLORSTITCH_TAG — s280d: el PARPADEO se retiró.
// s280b/s280c hacían girar el tejido de una banda en auto entre sus dos direcciones
// reales (base y base+90°). El mensaje era correcto —en auto no hay UNA dirección— pero
// el usuario lo descartó por dos motivos buenos: cansa la vista, y obliga al canvas, que
// es de repintado BAJO DEMANDA, a repintar sin parar mientras haya una zona en auto a la
// vista. El aviso vive ahora en el CONTORNO: violeta, latiendo hacia fuera y hacia dentro
// (`GLGizmoColorStitchPainter::render_auto_angle_marker`), y el tejido se queda quieto en
// su ángulo base. `WeaveParams::auto_angle` sigue siendo la marca por isla y es lo que
// alimenta ese marcador — sólo ha cambiado CÓMO se comunica, no qué se detecta.

// NEOTKO_COLORSTITCH_TAG — one WeaveParams from a projected extent [pmin,pmax] along `theta`.
// N (line count) comes from the REAL line spacing; pitch = span/N so each stripe is one line
// wide until the LUT cap (64), beyond which it coarsens to still cover the span.
// ⚠️ s314 — `line_w` recibe ahora weave_top_line_spacing(), NO el ancho. El nombre se
// conserva para no tocar las firmas, pero la magnitud es la separación real entre líneas
// (ancho − altura·(1−π/4)). Con el ancho se contaban un 10-17% menos de líneas de las que
// el slicer pone de verdad, y el error era distinto en cada objeto del plato.
// NEOTKO_COLORSTITCH_TAG — s318 F3. Ver la nota larga de la declaración (.hpp).
WeaveFrame weave_frame(const ModelVolume* mv, const ModelObject* owner,
                       float theta_rad, bool canon_half_plane)
{
    WeaveFrame fr;
    // perp en el marco de REBANADO, igual que lane_perp_axis(): θ mod π si es autorado, y el
    // semiplano sólo cuando no lo es (en auto el motor mide el eje de la geometría).
    double ang = double(theta_rad);
    if (!canon_half_plane) {
        ang = std::fmod(ang, M_PI);
        if (ang < 0.0) ang += M_PI;
    }
    Vec3d perp(-std::sin(ang), std::cos(ang), 0.0);
    if (canon_half_plane
        && (perp.y() < -1e-12 || (std::abs(perp.y()) <= 1e-12 && perp.x() < 0.0)))
        perp = -perp;

    // Tipos escritos a mano: con `auto` Eigen guarda la receta, no el resultado.
    Matrix3d L = Matrix3d::Identity();
    if (owner && !owner->instances.empty()) {
        const Transform3d inst = owner->instances.front()->get_transformation().get_matrix();
        L = inst.linear();
    }
    Matrix3d Rv = Matrix3d::Identity();
    Vec3d    tv = Vec3d::Zero();
    if (mv) {
        const Transform3d vm = mv->get_matrix();
        Rv = vm.linear();
        tv = vm.translation();
    }
    const Matrix3d LR = L * Rv;
    const Vec3d    ax = LR.transpose() * perp;
    const Vec3d    Lt = L * tv;
    fr.axis[0] = float(ax.x());
    fr.axis[1] = float(ax.y());
    fr.axis[2] = float(ax.z());
    fr.anchor_proj = float(-perp.dot(Lt));
    return fr;
}

// NEOTKO_COLORSTITCH_TAG — s318 F3: degradado (modos 1/2) muestreado POR CARRIL, como el
// motor (ColorStitch.cpp, rama GRAD_FIELD: compute_field_groups + FieldSampler +
// build_dithered_tools_*_at). Antes el preview repartía N líneas contadas desde el borde de
// la isla con los builders de RECUENTO, e ignoraba `gradient_span_mm` y `repetitions`: un
// degradado con periodo físico salía en pantalla estirado una vez sobre toda la zona.
// Todo va en posiciones RELATIVAS al ancla (origen del objeto), como en el motor:
//   · periodo > 0: carril k = round(fmod(pos, periodo)/sp), t = k/(periodo/sp). Se tesela
//     un periodo con paso periodo/n exacto, para que no derive de un ciclo al siguiente;
//   · ajustar a la superficie: t = (k−k0)/(k1−k0) entre los carriles observados. Los
//     centros de línea caen medio paso por dentro del borde, de ahí el `inset`.
//   · `repetitions` > 1 sin periodo = periodo (k1−k0)·sp/reps, la misma traducción del motor.
//   · invertir = dar la vuelta a la lista de tools (el motor invierte la `t` y el orden de los
//     grupos, que con carriles equiespaciados es lo mismo).
// Devuelve false si la receta no da tools válidos (mismo filtro que el preflight del motor)
// o si la superficie es de un solo carril; el llamador cae entonces al camino de siempre.
// NEOTKO_COLORSTITCH_TAG — s318 F3: la rejilla de carriles del motor (FieldSampler) traducida
// a una LUT del shader. DUEÑA ÚNICA de esa traducción en el preview: la usan el degradado de
// ColorStitch y la rampa de PathBlend, igual que en el motor comparten FieldSampler.
// Todo relativo al ancla (origen del objeto proyectado):
//   · periodo > 0: carril k = round(fmod(pos, periodo)/sp), t = k/(periodo/sp). Se tesela un
//     periodo con paso periodo/n EXACTO para que no derive de un ciclo al siguiente;
//   · ajustar: t = (k−k0)/(k1−k0) entre los carriles de los extremos. `inset` = los extremos
//     son centros de línea (medio paso por dentro del borde, lo que ve ColorStitch); sin
//     inset son el contorno (lo que usa PathBlend, Fill.cpp _pb_renorm);
//   · `reps` > 1 sin periodo = periodo (k1−k0)·sp/reps (traducción del motor, ColorStitch).
// false = superficie de un solo carril (el motor da t = 0,5 ahí).
struct FieldLut { std::vector<double> t; bool tile = false; double pitch = 1.0, p0 = 0.0; };
static bool field_lut(double pmin, double pmax, double anchor, double sp, double period,
                      int reps, bool inset_edges, FieldLut& out)
{
    sp = std::max(1e-3, sp);
    const double r0 = pmin - anchor, r1 = pmax - anchor;
    const double inset = (inset_edges && r1 - r0 > sp) ? 0.5 * sp : 0.0;
    const long long k0 = std::llround((r0 + inset) / sp);
    const long long k1 = std::max(k0, (long long) std::llround((r1 - inset) / sp));
    if (period <= 1e-4 && reps > 1)
        period = double(k1 - k0) * sp / double(reps);
    out.t.clear();
    if (period > 1e-4) {
        const int n = std::clamp((int) std::lround(period / sp), 2, 64);
        out.pitch = period / double(n);
        out.t.resize(n);
        for (int i = 0; i < n; ++i) out.t[i] = double(i) / double(n);
        out.tile = true;
        out.p0   = anchor - 0.5 * out.pitch;
        return true;
    }
    const long long N = k1 - k0 + 1;
    if (N < 2) return false;
    const int n = int(std::min<long long>(N, 64));
    out.pitch = double(N) * sp / double(n);   // == sp mientras quepa en la LUT
    out.t.resize(n);
    for (int i = 0; i < n; ++i) out.t[i] = double(i) / double(n - 1);
    out.tile = false;
    out.p0   = anchor + (double(k0) - 0.5) * sp;
    return true;
}

static bool gradient_field_weave(const std::map<std::string, std::string>& kv,
                                 const std::vector<std::string>& fcolors,
                                 float theta, float pmin, float pmax, float sp_f,
                                 float anchor_proj, WeaveParams& w,
                                 std::vector<int>* out_tools = nullptr)
{
    const std::string pre = "interlayer_colormix_";
    auto gi = [&](const char* k, int d) {
        const auto it = kv.find(pre + k);
        if (it != kv.end()) { try { return std::stoi(it->second); } catch (...) {} }
        return d; };
    auto gd = [&](const char* k, double d) {
        const auto it = kv.find(pre + k);
        if (it != kv.end()) { try { return std::stod(it->second); } catch (...) {} }
        return d; };
    auto gb = [&](const char* k, bool d) {
        const auto it = kv.find(pre + k);
        return it != kv.end() ? (it->second == "1" || it->second == "true") : d; };

    const int mode = gi("mode", 0);
    const int ta = gi("tool_a", 0), tb = gi("tool_b", 1), tc = gi("tool_c", 2);
    if (mode == 1 && !(ta >= 0 && tb >= 0 && ta != tb)) return false;
    if (mode == 2 && !(ta >= 0 && tb >= 0 && tc >= 0 && (ta != tb || tb != tc))) return false;
    if (mode != 1 && mode != 2) return false;

    const double sp      = std::max(1e-3, double(sp_f));
    const double span_mm = std::max(0.0, gd("gradient_span_mm", 0.0));
    const int    reps    = std::max(1, gi("repetitions", 1));
    const bool   inv     = gb("invert", false);

    FieldLut lut;
    if (!field_lut(pmin, pmax, anchor_proj, sp, span_mm, reps, /*inset*/ true, lut))
        return false;
    const std::vector<double>& t_asc = lut.t;
    w.tile  = lut.tile;
    w.pitch = float(lut.pitch);
    w.p0    = float(lut.p0);

    std::vector<int> tools = (mode == 1)
        ? Slic3r::ColorStitch::build_dithered_tools_2color_at(
              t_asc, ta, tb, gi("pct_a", 50), gi("easing", 0), gd("gamma", 1.0))
        : Slic3r::ColorStitch::build_dithered_tools_3color_at(
              t_asc, ta, tb, tc, gi("pct_a", 50), gi("pct_b", 33),
              gi("easing", 0), gd("gamma", 1.0), gd("overlap", 0.6));
    if (tools.size() != t_asc.size()) return false;
    if (inv) std::reverse(tools.begin(), tools.end());
    if (out_tools) *out_tools = tools;

    w.cols.resize(tools.size());
    for (size_t i = 0; i < tools.size(); ++i)
        w.cols[i] = (tools[i] >= 0) ? tool_col_rgba(fcolors, tools[i])
                                    : ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
    w.on = true;
    w.angle_rad = theta;
    return true;
}

WeaveParams
colorstitch_make_weave(const std::map<std::string, std::string>& kv,
                       const std::vector<std::string>& fcolors,
                       float theta, float pmin, float pmax, float line_w,
                       float anchor_proj, std::vector<int>* out_tools)
{
    WeaveParams w;
    const float span = pmax - pmin;
    if (span < 1e-3f) return w;   // w.on stays false
    const float lw = std::max(line_w, 0.05f);

    int mode = 0;
    { const auto it = kv.find("interlayer_colormix_mode");
      if (it != kv.end()) { try { mode = std::stoi(it->second); } catch (...) {} } }

    auto fill_cols = [&](const std::vector<int>& seq, int count) {
        w.cols.resize(count);
        if (out_tools) out_tools->assign(count, 0);   // s318 F3
        for (int i = 0; i < count; ++i) {
            const int tool0 = seq[(size_t) i % seq.size()];
            if (out_tools) (*out_tools)[i] = tool0;
            w.cols[i] = (tool0 >= 0) ? tool_col_rgba(fcolors, tool0)
                                     : ColorRGBA(0.5f, 0.5f, 0.5f, 1.f);
        }
    };

    if (mode == 0) {
        // PATTERN (periodic, e.g. "12"): tile ONE period at the real line width so each
        // stripe == one printed line, independent of island size (no 64-line stretch).
        const std::vector<int> base = colorstitch_tool_sequence(kv, /*penu*/false, 1);
        if (base.empty()) return w;
        const int n = std::min<int>((int) base.size(), 64);
        // s318 F3 — FASE del motor (rama PatternField): carril k = round((pos − ancla)/sp),
        // entrada = k mod longitud. Con p0 = ancla − medio paso, el floor() del shader da
        // exactamente ese round(). Antes p0 era el borde de la isla: patrón bien, fase corrida.
        w.on = true; w.tile = true; w.angle_rad = theta; w.pitch = lw;
        w.p0 = anchor_proj - 0.5f * lw;
        fill_cols(base, n);
    } else if (mode == 4) {
        // NEOTKO_COLORSTITCH_TAG — s314: bandas en MM. Se TESELA un periodo, como el modo 0,
        // porque el diseño es periódico en milímetros y no depende del tamaño de la isla —
        // que es justamente lo que este modo viene a arreglar. Una entrada de la LUT por
        // línea impresa, a paso `lw` (que el llamador pasa ya como SPACING real, no ancho).
        //
        // s318 F3 — CERRADO el límite de la FASE: se ancla en el origen del objeto
        // (`anchor_proj`, ver weave_frame), igual que compute_slot_per_line_band_mm(). Y el
        // paso pasa a ser periodo/n EXACTO: con paso = spacing, n·paso ≠ periodo y las
        // bandas derivaban un poco en cada ciclo teselado (8 mm con paso 0,374 → 21 líneas =
        // 7,85 mm, 0,15 mm de deriva por ciclo).
        double period_mm = 0.0;
        for (const char* k : { "interlayer_colormix_band_mm_a", "interlayer_colormix_band_mm_b",
                               "interlayer_colormix_band_mm_c", "interlayer_colormix_band_mm_d" }) {
            const auto it = kv.find(k);
            if (it == kv.end()) continue;
            try { period_mm += std::max(0.0, std::stod(it->second)); } catch (...) {}
        }
        if (period_mm <= 1e-4) return w;
        // Un periodo entero medido en líneas. Si no cabe en la LUT de 64 se engorda el paso
        // para que quepa: las proporciones entre bandas se conservan, sólo se pierde
        // resolución de borde. Mejor eso que recortar el periodo, que mentiría en el ancho.
        const int   n     = std::clamp((int) std::lround(period_mm / lw), 2, 64);
        const float pitch = float(period_mm / double(n));
        const std::vector<int> base =
            colorstitch_tool_sequence(kv, /*penu*/false, n, /*spacing_mm*/ pitch);
        if (base.empty()) return w;
        w.on = true; w.tile = true; w.angle_rad = theta; w.p0 = anchor_proj; w.pitch = pitch;
        fill_cols(base, n);
    } else if ((mode == 1 || mode == 2)
               && gradient_field_weave(kv, fcolors, theta, pmin, pmax, lw, anchor_proj, w,
                                       out_tools)) {
        // s318 F3 — degradado por carril, ver gradient_field_weave(). Ya relleno.
    } else {
        // GRADIENT / dither (modes 1-3): span the island once with N real-width lines,
        // capped at the 64-entry LUT (then it coarsens, but the ramp still reads right).
        const int N = std::clamp((int) std::lround(span / lw), 4, 64);
        const std::vector<int> base = colorstitch_tool_sequence(kv, /*penu*/false, N);
        if (base.empty()) return w;
        w.on = true; w.tile = false; w.angle_rad = theta; w.p0 = pmin; w.pitch = span / float(N);
        fill_cols(base, N);
    }
    return w;
}

// NEOTKO_SANDWICH_TAG — Fase 3.2 (s167 plan): extract a profile's Top PathBlend
// config, mirror of colorstitch_top_kv (empty kv -> "not this kind"). Returns
// false when Top has no PathBlend pass.
bool pathblend_top_config(const Slic3r::SurfaceEffectProfile& prof, PathBlendPassConfig& out)
{
    if (prof.stack_top_json.empty())
        return false;
    const Slic3r::SurfacePassStack st = Slic3r::SurfacePassStack::from_json(prof.stack_top_json);
    for (const Slic3r::SurfacePass& p : st.passes)
        if (p.kind == Slic3r::SurfacePassKind::PathBlend) {
            out = pro_pb_read(p);
            return true;
        }
    return false;
}

// NEOTKO_SANDWICH_TAG — Fase 3.2: PathBlend on-mesh preview. Reuses the same
// WeaveParams/u_weave_cols infrastructure as ColorStitch (colorstitch_make_weave
// above) — NO shader changes — but the per-step colour comes from the REAL
// ramp+cap Beer-Lambert physics (pathblend_canonical_model.md, cross-checked
// against Fill.cpp's actual ramp block) instead of a flat tool-sequence lookup:
// PathBlend's "gradient" is a true Z-wedge composed by transmission (thin cap
// lets the ramp show through), not a dithered sequence of solid-colour lines.
// theta is fixed at 0 (pure object-local Y projection): Fill.cpp's `_t_of()`
// reads un-rotated slice-space Y regardless of fill_angle (fill_surface_extrusion
// rotates only to generate the lines, then rotates back before returning), so
// this axis matches the engine's — unlike ColorStitch's weave, which follows
// the configured cm_angle.
WeaveParams
pathblend_make_weave(const PathBlendPassConfig& pbc,
                     const Slic3r::ColorSci::Material mats[4],
                     const float bg_rgb[3],
                     double layer_h_mm,
                     float theta,
                     float pmin, float pmax, float line_w,
                     float anchor_proj,
                     std::vector<std::vector<Slic3r::ColorSci::Layer>>* out_layers)
{
    WeaveParams w;
    const float span = pmax - pmin;
    if (span < 1e-3f || pbc.tool_bottom < 0) return w;   // w.on stays false
    if (out_layers) out_layers->clear();

    // Same auto-resolution + clamp as the real ramp block (Fill.cpp): mid_end_mm
    // < 0 means auto (default thin cap H-0.04 Full / H Half). s191: hard ceiling
    // is now H for both modes (0.04 cap reserve removed).
    const double H = std::max(0.01, layer_h_mm);
    const float floor_pb = std::max(0.01f, pbc.floor_mm);
    const bool  is_full  = (pbc.mode == PathBlendPassConfig::Mode::Full) && pbc.tool_top >= 0;
    const float mid_pref = (pbc.mid_end_mm < 0.f)
        ? (is_full ? float(H - 0.04) : float(H))
        : pbc.mid_end_mm;
    const float mid_end  = std::min(mid_pref, float(H));
    const double range = double(mid_end) - double(floor_pb);

    // NEOTKO_PATHBLEND_TAG — s318 F3: la rampa se muestrea con la MISMA rejilla de carriles
    // que el motor (Fill.cpp, `_pb_smp`: FieldSampler con el periodo `span_mm`, el ancla en el
    // origen del objeto y los extremos del CONTORNO, sin inset), y la altura pasa por
    // profile_u() como en la escalera real (s190). Antes: N líneas contadas desde el borde de
    // la isla con t lineal, sin periodo ni perfil de entrada/salida.
    FieldLut lut;
    if (!field_lut(pmin, pmax, anchor_proj, std::max(line_w, 0.05f),
                   std::max(0.0, double(pbc.span_mm)), 1, /*inset*/ false, lut)) {
        // Un solo carril: el motor le da t = 0,5 (FieldSampler, k1 <= k0).
        lut.t.assign(1, 0.5); lut.tile = true; lut.pitch = 1.0; lut.p0 = 0.0;
    }
    const int N = int(lut.t.size());
    w.cols.resize(N);
    const int tb = std::clamp(pbc.tool_bottom, 0, 3);
    const int tt = std::clamp(pbc.tool_top,    0, 3);
    for (int i = 0; i < N; ++i) {
        const double t      = lut.t[i];
        const double h_ramp = floor_pb + pbc.profile_u(t) * range;
        const double h_cap  = H - h_ramp;

        std::vector<Slic3r::ColorSci::Layer> layers;
        Slic3r::ColorSci::Layer bottom;
        bottom.rgb   = mats[tb].rgb;
        bottom.td    = mats[tb].td;
        bottom.ratio = float(h_ramp / H);
        layers.push_back(bottom);
        // Half mode has no cap — the area above the ramp is genuinely unfilled
        // (authorized semi-fill, see PathBlendPassConfig comment), not a second
        // material. Letting the resolved real bg show through there (via the
        // single-layer Beer-Lambert blend below) is the closest honest preview:
        // it's the same "what's really behind this" bg Fase 2 already resolves.
        if (is_full) {
            Slic3r::ColorSci::Layer cap;
            cap.rgb   = mats[tt].rgb;
            cap.td    = mats[tt].td;
            cap.ratio = float(h_cap / H);
            layers.push_back(cap);
        }
        float rgb[3];
        Slic3r::ColorSci::blend_stacked(layers, bg_rgb, rgb);
        w.cols[i] = ColorRGBA(rgb[0], rgb[1], rgb[2], 1.f);
        if (out_layers) out_layers->push_back(layers);   // s318 F3
    }
    w.on = true; w.tile = lut.tile; w.angle_rad = theta;
    w.p0 = float(lut.p0); w.pitch = float(lut.pitch);
    return w;
}

// ==================================================================================
// NEOTKO_COLORSTITCH_TAG — s318 F3, OPCIÓN A: Top y Penu compuestos por fragmento.
// Decisión del usuario (s318): "es el correcto, ya que aproxima lo que sucede en la
// realidad".
//
// 🔑 Por qué se puede hacer exacto con dos tablas. blend_stacked() (ColorSci.cpp) compone
// cada capa como  acc = color·(1−T) + acc·T  por canal, en LINEAL, con T = 0,1^(ratio/td)
// (T = 0 si td≈0: opaca). Eso es AFÍN en `acc`. Una pila entera de pases es entonces una
// composición de afines = otro afín  (A, B): lo de abajo entra multiplicado por A y se le
// suma B. Así que para dos pases con efecto, cada uno con su eje:
//     color = arriba ∘ PaseAlto_j ∘ medio ∘ PaseBajo_i ∘ abajo (fondo)
// se reparte en dos LUT: la del pase bajo guarda el color YA compuesto hasta él (c2_i) y la
// del alto guarda su afín con lo de encima ya plegado (A_j, B_j). El shader sólo hace
// A_j·c2_i + B_j y lo pasa a sRGB. Sin aproximar nada que el color plano del slot
// (sandwich_colour_stacked) no aproxime ya.
//
// El afín de un pase se MIDE con la propia física: se compone sobre negro y sobre blanco
// (sRGB 0 y 1 = lineal 0 y 1) ⇒ B = lin(sobre negro), A = lin(sobre blanco) − B. Así los
// pases fijos pasan por el MISMO pass_to_layer del color del slot, sin copiarlo.
// Cada franja de ColorStitch es su pase con kind=Solid y el tool de esa franja; cada franja
// de PathBlend son sus capas rampa+tapa (las de pathblend_make_weave).
//
// Qué pases llevan tejido: el primer ColorStitch (o, si no hay, el primer PathBlend) de la
// pila Penu, y el de la pila Top; es el mismo criterio que colorstitch_top_kv /
// pathblend_top_config. Un tercer pase con efecto se compone plano, como hace el slot.
// ==================================================================================
struct TopZoneRecipe {
    struct Aff { float A[3] = {1.f, 1.f, 1.f}; float B[3] = {0.f, 0.f, 0.f}; };
    struct Field {
        bool   on = false, pb = false, penu = false, is_auto = false;
        std::map<std::string, std::string> kv;   // ColorStitch, YA con claves de Top
        PathBlendPassConfig pbc;
        SurfacePass pass;                        // copia: ratio y tipo, para las franjas
        float theta = 0.f;
        WeaveFrame fr;
    };
    Field lo, hi;            // lo = el pase de abajo (o el único); hi = el de arriba
    Aff   below, mid, above; // pases fijos: bajo lo, entre lo y hi, sobre hi (o sobre lo)
};

namespace {
using ZAff = TopZoneRecipe::Aff;

// g∘f: primero f (abajo) y encima g.
ZAff zaff_then(const ZAff& f, const ZAff& g)
{
    ZAff r;
    for (int c = 0; c < 3; ++c) {
        r.A[c] = g.A[c] * f.A[c];
        r.B[c] = g.A[c] * f.B[c] + g.B[c];
    }
    return r;
}

ZAff zaff_from_black_white(const float o0[3], const float o1[3])
{
    ZAff r;
    for (int c = 0; c < 3; ++c) {
        const float e = Slic3r::ColorSci::srgb_to_linear(o0[c]);
        r.B[c] = e;
        r.A[c] = std::max(0.f, Slic3r::ColorSci::srgb_to_linear(o1[c]) - e);
    }
    return r;
}

ZAff zaff_of_pass(const SurfacePass& p, bool penu_role, const Slic3r::ColorSci::Material mats[4])
{
    SurfacePassStack one;
    one.enabled = true;
    one.passes.push_back(p);
    const SurfacePassStack none;
    const float k0[3] = {0.f, 0.f, 0.f}, k1[3] = {1.f, 1.f, 1.f};
    float o0[3], o1[3];
    // El rol importa: pass_to_layer busca el patrón de un ColorStitch con las claves del rol.
    const SurfacePassStack& t = penu_role ? none : one;
    const SurfacePassStack& q = penu_role ? one  : none;
    Slic3r::ColorSci::sandwich_colour_stacked(t, q, mats, k0, o0);
    Slic3r::ColorSci::sandwich_colour_stacked(t, q, mats, k1, o1);
    return zaff_from_black_white(o0, o1);
}

ZAff zaff_of_layers(const std::vector<Slic3r::ColorSci::Layer>& layers)
{
    const float k0[3] = {0.f, 0.f, 0.f}, k1[3] = {1.f, 1.f, 1.f};
    float o0[3], o1[3];
    Slic3r::ColorSci::blend_stacked(layers, k0, o0);
    Slic3r::ColorSci::blend_stacked(layers, k1, o1);
    return zaff_from_black_white(o0, o1);
}

// El kv de un pase de la pila Penu trae las claves `interlayer_colormix_penu_*` (el motor
// elige el subconjunto por rol). Todo el preview lee claves de Top, así que se traducen.
std::map<std::string, std::string> kv_as_top_role(const std::map<std::string, std::string>& kv,
                                                  bool penu)
{
    if (!penu) return kv;
    static const std::string pp = "interlayer_colormix_penu_", tp = "interlayer_colormix_";
    std::map<std::string, std::string> out = kv;
    for (const auto& [k, v] : kv) {
        if (k.compare(0, pp.size(), pp) == 0)
            out[tp + k.substr(pp.size())] = v;
        else if (k == "interlayer_colormix_pattern_penultimate")
            out["interlayer_colormix_pattern_top"] = v;
    }
    // Sin ángulo en el pase, manda el del Penu del preset (no el del Top, que es lo que
    // colorstitch_weave_theta usaría de fallback).
    if (kv.find(pp + "angle") == kv.end()) {
        const auto& pc = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        if (auto* a = pc.option<ConfigOptionInt>("interlayer_colormix_penu_angle"))
            out[tp + "angle"] = std::to_string(a->value);
    }
    return out;
}

// Tejido de UN pase con efecto y el afín de cada una de sus franjas.
bool zone_field_stripes(const TopZoneRecipe::Field& F,
                        const std::vector<std::string>& fcolors,
                        const Slic3r::ColorSci::Material mats[4], const float bg_rgb[3],
                        float line_w, double lh, float pmin, float pmax,
                        WeaveParams& w, std::vector<ZAff>& aff)
{
    aff.clear();
    if (F.pb) {
        std::vector<std::vector<Slic3r::ColorSci::Layer>> layers;
        w = pathblend_make_weave(F.pbc, mats, bg_rgb, lh, F.theta, pmin, pmax, line_w,
                                 F.fr.anchor_proj, &layers);
        if (!w.on || layers.size() != w.cols.size()) return false;
        // Las capas de pathblend_make_weave son fracciones de la capa ENTERA; dentro de una
        // pila el pase ocupa `ratio` de ella. Con un PathBlend solo (ratio 1) no cambia nada.
        const float r = (F.pass.ratio > 1e-6) ? float(F.pass.ratio) : 1.f;
        for (auto& L : layers) {
            for (auto& l : L) l.ratio *= r;
            aff.push_back(zaff_of_layers(L));
        }
    } else {
        std::vector<int> tools;
        w = colorstitch_make_weave(F.kv, fcolors, F.theta, pmin, pmax, line_w,
                                   F.fr.anchor_proj, &tools);
        if (!w.on || tools.size() != w.cols.size()) return false;
        for (int t : tools) {
            SurfacePass s = F.pass;
            s.kind       = SurfacePassKind::Solid;
            s.solid_tool = std::clamp(t, 0, 3);
            aff.push_back(zaff_of_pass(s, F.penu, mats));
        }
    }
    std::copy(F.fr.axis, F.fr.axis + 3, w.axis);
    return true;
}
} // namespace

std::shared_ptr<const TopZoneRecipe> resolve_top_zone(const SurfaceEffectProfile& prof,
                                                      const ModelVolume* mv, const ModelObject* owner,
                                                      const Slic3r::ColorSci::Material mats[4])
{
    // Perfil legacy (payload sin pilas): se queda en el camino de siempre.
    if (prof.stack_top_json.empty() && prof.stack_penu_json.empty()) return nullptr;
    if (prof.stack_top_json.empty() && prof.colorstitch.present) return nullptr;
    const SurfacePassStack st_t = SurfacePassStack::from_json(prof.stack_top_json);
    const SurfacePassStack st_p = SurfacePassStack::from_json(prof.stack_penu_json);

    // La pila física de abajo arriba, con el mismo filtro `enabled` que sandwich_colour_stacked.
    struct Item { const SurfacePass* p; bool penu; };
    std::vector<Item> L;
    if (st_p.enabled) for (const SurfacePass& p : st_p.passes) L.push_back({ &p, true });
    if (st_t.enabled) for (const SurfacePass& p : st_t.passes) L.push_back({ &p, false });

    auto pick = [&](bool penu) -> int {
        int first_pb = -1;
        for (int i = 0; i < (int) L.size(); ++i) {
            if (L[i].penu != penu) continue;
            const SurfacePass& p = *L[i].p;
            if (p.kind == SurfacePassKind::ColorStitch && !p.colorstitch.kv.empty()) return i;
            if (p.kind == SurfacePassKind::PathBlend && first_pb < 0) first_pb = i;
        }
        return first_pb;
    };
    const int ip = pick(true), it = pick(false);
    if (ip < 0 && it < 0) return nullptr;
    const int ilo = (ip >= 0) ? ip : it;
    const int ihi = (ip >= 0 && it >= 0) ? it : -1;

    auto R = std::make_shared<TopZoneRecipe>();
    auto fill = [&](int idx, TopZoneRecipe::Field& F) {
        const SurfacePass& p = *L[idx].p;
        F.on = true; F.penu = L[idx].penu; F.pass = p;
        if (p.kind == SurfacePassKind::PathBlend) {
            F.pb      = true;
            F.pbc     = pro_pb_read(p);
            F.is_auto = (F.pbc.fill_angle < 0);
            F.theta   = F.is_auto ? weave_solid_infill_dir_rad()
                                  : float(F.pbc.fill_angle) * float(M_PI) / 180.f;
        } else {
            F.kv    = kv_as_top_role(p.colorstitch.kv, F.penu);
            F.theta = colorstitch_weave_theta(F.kv, F.is_auto);
        }
        // Semiplano cuando el motor MIDE el eje: ColorStitch en auto y PathBlend siempre
        // (Fill.cpp _pb_measure_axis canoniza el eje medido a +Y / +X).
        F.fr = weave_frame(mv, owner, F.theta, /*canon*/ F.pb || F.is_auto);
    };
    fill(ilo, R->lo);
    if (ihi >= 0) fill(ihi, R->hi);

    auto seg = [&](int a, int b) {
        ZAff f;
        for (int i = std::max(0, a); i < std::min(b, (int) L.size()); ++i)
            f = zaff_then(f, zaff_of_pass(*L[i].p, L[i].penu, mats));
        return f;
    };
    R->below = seg(0, ilo);
    if (ihi >= 0) { R->mid = seg(ilo + 1, ihi); R->above = seg(ihi + 1, (int) L.size()); }
    else          { R->above = seg(ilo + 1, (int) L.size()); }
    return R;
}

const WeaveFrame& top_zone_frame(const TopZoneRecipe& r, bool hi)
{
    return (hi && r.hi.on) ? r.hi.fr : r.lo.fr;
}

bool top_zone_auto(const TopZoneRecipe& r)
{
    return r.lo.is_auto || (r.hi.on && r.hi.is_auto);
}

WeaveParams make_zone_weave(const TopZoneRecipe& R,
                            const std::vector<std::string>& fcolors,
                            const Slic3r::ColorSci::Material mats[4], const float bg_rgb[3],
                            float line_w, double lh,
                            float lo_min, float lo_max, float hi_min, float hi_max)
{
    WeaveParams out;
    WeaveParams wlo;
    std::vector<ZAff> alo;
    if (!zone_field_stripes(R.lo, fcolors, mats, bg_rgb, line_w, lh, lo_min, lo_max, wlo, alo))
        return out;   // .on = false → color plano del slot

    // ¿Hay pase de arriba con tejido? Si existe pero no da tejido (isla diminuta), entra
    // como pase fijo: su afín se pliega con lo de encima.
    WeaveParams       whi;
    std::vector<ZAff> ahi;
    const bool two = R.hi.on
        && zone_field_stripes(R.hi, fcolors, mats, bg_rgb, line_w, lh, hi_min, hi_max, whi, ahi);
    ZAff after_lo = R.above;
    if (R.hi.on && !two)
        after_lo = zaff_then(zaff_then(R.mid, zaff_of_pass(R.hi.pass, R.hi.penu, mats)), R.above);
    else if (two)
        after_lo = R.mid;

    float c0[3];
    for (int c = 0; c < 3; ++c)
        c0[c] = R.below.A[c] * Slic3r::ColorSci::srgb_to_linear(bg_rgb[c]) + R.below.B[c];

    // LUT 2 = el pase de abajo, con todo lo que tiene debajo y lo fijo de encima ya compuesto.
    out.cols2.resize(alo.size());
    for (size_t i = 0; i < alo.size(); ++i) {
        const ZAff f = zaff_then(alo[i], after_lo);
        out.cols2[i] = ColorRGBA(f.A[0] * c0[0] + f.B[0], f.A[1] * c0[1] + f.B[1],
                                 f.A[2] * c0[2] + f.B[2], 1.f);
    }
    out.tile2 = wlo.tile; out.pitch2 = wlo.pitch; out.p0_2 = wlo.p0;
    std::copy(wlo.axis, wlo.axis + 3, out.axis2);

    // LUT 1 = el pase de arriba (su afín con lo de encima plegado) o, si no hay, la identidad.
    if (two) {
        out.tile = whi.tile; out.pitch = whi.pitch; out.p0 = whi.p0; out.angle_rad = whi.angle_rad;
        std::copy(whi.axis, whi.axis + 3, out.axis);
        out.cols.resize(ahi.size());
        out.dual_a.resize(ahi.size());
        for (size_t j = 0; j < ahi.size(); ++j) {
            const ZAff f = zaff_then(ahi[j], R.above);
            out.dual_a[j] = ColorRGBA(f.A[0], f.A[1], f.A[2], 1.f);
            out.cols[j]   = ColorRGBA(f.B[0], f.B[1], f.B[2], 1.f);
        }
    } else {
        out.tile = true; out.pitch = 1.f; out.p0 = 0.f; out.angle_rad = wlo.angle_rad;
        out.cols.assign(1, ColorRGBA(0.f, 0.f, 0.f, 1.f));
        out.dual_a.assign(1, ColorRGBA(1.f, 1.f, 1.f, 1.f));
    }
    out.dual = true;
    out.on   = true;
    return out;
}


void weave_islands_for_volume(const ModelVolume*              mv,
                              const Slic3r::TriangleSelector* sel,
                              const ModelObject*              owner,
                              std::unordered_map<int,int>&    facet_weave_idx,
                              std::vector<WeaveParams>&       weave_list,
                              bool*                           any_auto_angle)
{
    facet_weave_idx.clear();
    weave_list.clear();
    if (!mv || !sel) return;

    Slic3r::ColorSci::Material mats[4];
    std::vector<std::string>   fcolors;
    materials(mats, fcolors);
    // NEOTKO_SANDWICH_TAG — Fase 3.2: real bg, see build_ebt_weave_for_volume.
    float bg_rgb[3] = {0.f, 0.f, 0.f};
    object_base_bg(mats, owner, bg_rgb);
    // NEOTKO_COLORSTITCH_TAG — s314: SPACING, no ancho. Ver weave_top_line_spacing().
    const float line_w = (float) weave_top_line_spacing();
    const double lh = weave_layer_height();

    // NEOTKO_BOTTOM_TAG — s231 F7: clasificación TOP / BOTTOM de cada faceta. Es lo que
    // faltaba para que el Bottom pudiera verse: el preview leía sólo `stack_top_json`,
    // así que un perfil con receta Bottom (distinta desde s230, con sus propios caps)
    // se dibujaba plano o directamente con el color del Top. El criterio y el umbral
    // (0.30) son los MISMOS que ya usa update_model_object para decidir qué caras
    // conserva cada slot (discard_non_zone_facing) — si divergieran, el preview
    // enseñaría tejido en caras que la pintura descarta.
    Transform3d trafo = mv->get_matrix();
    if (owner && !owner->instances.empty())
        trafo = owner->instances.front()->get_transformation().get_matrix() * mv->get_matrix();
    const Matrix3d nrm_mat = trafo.matrix().block(0, 0, 3, 3).inverse().transpose();
    // +1 = mira hacia arriba, -1 = hacia abajo, 0 = pared (nunca se pinta, s145)
    auto facing_of = [&](const stl_vertex& a, const stl_vertex& b, const stl_vertex& c) -> int {
        const Vec3d n = nrm_mat * (b.cast<double>() - a.cast<double>())
                                    .cross(c.cast<double>() - a.cast<double>());
        const double len = n.norm();
        if (len < 1e-12) return 0;
        const double nz = n.z() / len;
        if (nz >=  0.30) return  1;
        if (nz <= -0.30) return -1;
        return 0;
    };

    const auto& mgr = SurfaceEffectProfileManager::get();
    for (int s = 1; s < ModelVolume::COLORSTITCH_SLOT_COUNT; ++s) {
        const int pid = mv->colorstitch_slot_to_profile_id[s];
        if (pid == 0) continue;
        const SurfaceEffectProfile* p = mgr.find(pid);
        if (!p) continue;

        // s231 F7 — dos pasadas por slot: la zona TOP (que es lo que había) y la zona
        // BOTTOM (nueva). Cada una con su receta y sobre SUS caras.
        for (int zone = 0; zone < 2; ++zone) {
        const bool zone_bottom = (zone == 1);
        std::map<std::string, std::string> kv;
        PathBlendPassConfig pbc;
        bool is_pathblend = false;
        if (!zone_bottom) {
            kv = colorstitch_top_kv(*p);
            is_pathblend = kv.empty() && pathblend_top_config(*p, pbc);
            // s232 DEBUG — el punto ciego del log anterior: sólo instrumenté la rama
            // BOTTOM, y en el caso del Assemble lo que se queda sin tejido es el TOP
            // (en el log no aparece NI UNA línea `zone=top`, o sea que sale por
            // `kv.empty() && !is_pathblend` → sin tejido → color plano = las caras
            // grises). Aquí se ve si el que se queda vacío es el stack, el payload de
            // respaldo, o los dos.
            {
                // s232 — KINDS por zona + volcado del json cuando el kv sale vacío: el
                // contador solo ya demostró que perfiles LLAMADOS "ColorStitch CM/CM"
                // tienen el top sin un solo pase ColorStitch (`kv=0` + `payload_cm=0`), con
                // 564 bytes de stack y penu VACÍO — o sea ~2 pases donde la receta del
                // generador tenía 1+1. Falta ver qué son esos pases y con qué claves,
                // que es lo que distingue "se degradaron a Solid" de "son ColorStitch con
                // el payload perdido" (arreglos distintos).
                const Slic3r::SurfacePassStack st_t =
                    Slic3r::SurfacePassStack::from_json(p->stack_top_json);
                const Slic3r::SurfacePassStack st_p =
                    Slic3r::SurfacePassStack::from_json(p->stack_penu_json);
                auto kinds = [](const Slic3r::SurfacePassStack& st) {
                    std::string o; using K = Slic3r::SurfacePassKind;
                    for (const auto& pp : st.passes) {
                        o += (pp.kind == K::Solid ? 'S' : pp.kind == K::ColorStitch ? 'C'
                            : pp.kind == K::PathBlend ? 'P' : '?');
                        o += std::to_string(pp.colorstitch.kv.size());
                        o += ' ';
                    }
                    return o.empty() ? std::string("-") : o;
                };
                std::ostringstream os;
                os << "TOP_WEAVE slot=" << s << " pid=" << pid
                   << " top_len=" << p->stack_top_json.size()
                   << " penu_len=" << p->stack_penu_json.size()
                   << " kv=" << kv.size() << " pathblend=" << (is_pathblend ? 1 : 0)
                   << " payload_cm=" << (p->colorstitch.present ? p->colorstitch.kv.size() : 0)
                   << " payload_pb=" << (p->pathblend.present ? 1 : 0)
                   << " top_kinds=[" << kinds(st_t) << "] penu_kinds=[" << kinds(st_p) << "]"
                   << " name='" << p->name << "'";
                if (kv.empty() && !is_pathblend)
                    os << "\n    TOP_JSON=" << p->stack_top_json.substr(0, 400);
                NeoDebug::write(NeoDebug::BOTTOM, os.str());
            }
        } else {
            if (p->stack_bottom_json.empty()) continue;
            const Slic3r::SurfacePassStack stb =
                Slic3r::SurfacePassStack::from_json(p->stack_bottom_json);
            kv = colorstitch_kv_from_stack(stb);
            is_pathblend = kv.empty() && pathblend_from_stack(stb, pbc);
            // s232 DEBUG — "el bottom se pinta y al soltar queda gris-neutro sin
            // textura", con el SLICE aplicando el efecto correcto: el fallo es de
            // preview, en esta rama. Log INCONDICIONAL de qué ve el extractor, para no
            // seguir apostando entre "kv vacío → color plano" y "weave que sale off".
            {
                std::ostringstream os;
                os << "BOTTOM_WEAVE slot=" << s << " pid=" << pid
                   << " json_len=" << p->stack_bottom_json.size()
                   << " passes=" << stb.passes.size() << " kinds=[";
                for (const Slic3r::SurfacePass& sp : stb.passes)
                    os << int(sp.kind) << ",";
                os << "] kv=" << kv.size() << " pathblend=" << (is_pathblend ? 1 : 0)
                   << " payload_cm=" << (p->colorstitch.present ? p->colorstitch.kv.size() : 0);
                NeoDebug::write(NeoDebug::BOTTOM, os.str());
            }
            // Bottom SIN ColorStitch ni PathBlend (p.ej. 1-2 pases Solid) no tiene
            // tejido que dibujar, pero SÍ un color propio que el slot no representa
            // (el color del slot se compone de Top+Penu). Se emite un "weave" de una
            // sola banda con ese color para que la cara inferior deje de mentir.
            if (kv.empty() && !is_pathblend) {
                float outc[3] = {0.f, 0.f, 0.f};
                Slic3r::ColorSci::sandwich_colour_stacked(stb, Slic3r::SurfacePassStack{},
                                                          mats, bg_rgb, outc);
                WeaveParams flat;
                flat.on = true; flat.tile = true; flat.angle_rad = 0.f;
                // Con UNA sola entrada en cols, el tiling devuelve ese color para
                // cualquier posición: el pitch es irrelevante (1.0 evita rarezas de
                // coma flotante que sí daría un valor enorme).
                flat.p0 = 0.f;  flat.pitch = 1.f;
                flat.cols.assign(1, ColorRGBA(std::min(1.f, outc[0]), std::min(1.f, outc[1]),
                                              std::min(1.f, outc[2]), 1.f));
                std::vector<int> src_flat;
                const indexed_triangle_set its_flat =
                    sel->get_facets(static_cast<EnforcerBlockerType>(s), src_flat);
                int wi_flat = -1;
                for (size_t k = 0; k < src_flat.size(); ++k) {
                    const auto& tri = its_flat.indices[k];
                    if (facing_of(its_flat.vertices[tri[0]], its_flat.vertices[tri[1]],
                                  its_flat.vertices[tri[2]]) != -1) continue;
                    if (wi_flat < 0) { wi_flat = int(weave_list.size()); weave_list.push_back(flat); }
                    facet_weave_idx[src_flat[k]] = wi_flat;
                }
                {   // s232 DEBUG — rama "color plano": es la que produciría el gris.
                    std::ostringstream os;
                    os << "BOTTOM_WEAVE slot=" << s << " → FLAT rgb="
                       << outc[0] << "," << outc[1] << "," << outc[2]
                       << " bg=" << bg_rgb[0] << "," << bg_rgb[1] << "," << bg_rgb[2]
                       << " mapped=" << (wi_flat >= 0 ? 1 : 0);
                    NeoDebug::write(NeoDebug::BOTTOM, os.str());
                }
                continue;
            }
        }
        // s318 F3 — OPCIÓN A: la zona Top sale compuesta con el Penu (ver make_zone_weave).
        // Se resuelve aunque el Top no tenga efecto: un Penu con ColorStitch bajo un Top
        // sólido también se ve a través de él.
        std::shared_ptr<const TopZoneRecipe> zr;
        if (!zone_bottom) zr = resolve_top_zone(*p, mv, owner, mats);
        if (!zr && kv.empty() && !is_pathblend) continue;

        bool is_auto = false;
        // NEOTKO_PATHBLEND_TAG — s280e: el ángulo de PathBlend YA se dibuja.
        // Estaba clavado a 0.f, y eso era medio correcto por accidente: con theta=0 el eje
        // de color del shader sale `proj = model_pos.y`, que coincidía con el eje Y al que
        // el motor tenía clavado el degradado. O sea el degradado salía bien y la DIRECCIÓN
        // DE LAS LÍNEAS mal — poner 45 y no ver ni rastro de 45. Con el motor arreglado
        // (s280e: el eje se mide perpendicular a las líneas reales) el eje del preview es
        // simplemente el ángulo, misma convención que ColorStitch: sin +90, verificada
        // contra el slice por el ojo del usuario en s146. Y ya no hay caso degenerado que
        // avisar: a cualquier ángulo el degradado existe y va perpendicular a las líneas.
        // Auto (-1) → base real del relleno, igual que ColorStitch; el aviso lo da el
        // contorno violeta pulsante.
        float theta;
        if (!is_pathblend)
            theta = colorstitch_weave_theta(kv, is_auto);
        else if (pbc.fill_angle >= 0)
            theta = float(pbc.fill_angle) * float(M_PI) / 180.f;
        else
            theta = weave_solid_infill_dir_rad();
        // NEOTKO_PATHBLEND_TAG — s280b/s280d/s280e: PathBlend en ángulo AUTO también se marca.
        // No es cosmético: `_t_of()` (Fill.cpp) saca la altura de cada línea de su posición
        // sobre el eje del degradado, y ese eje es perpendicular a las líneas ⇒ el ángulo
        // decide el degradado, no sólo el acabado. Con -1 el ángulo alterna 90° por paridad
        // de capa, así que el degradado GIRA de una capa a la siguiente y no hay uno único
        // que previsualizar. Por eso el aviso sigue haciendo falta justo aquí.
        // 📌 s280e — ya NO es "centroide Y": el eje estaba clavado a Y y ése era el bug que
        // colapsaba el efecto a 90°. Ahora se mide sobre las líneas reales.
        if (is_pathblend && pbc.fill_angle < 0) is_auto = true;
        if (zr) is_auto = top_zone_auto(*zr);
        if (is_auto && any_auto_angle) *any_auto_angle = true;
        // s318 F3 — proyectar en el MISMO marco que el motor (ver weave_frame). Semiplano cuando
        // el motor MIDE el eje: ColorStitch en auto y PathBlend siempre (Fill.cpp
        // _pb_measure_axis lo canoniza a +Y). Con receta compuesta, cada pase trae su marco.
        const WeaveFrame fr  = zr ? top_zone_frame(*zr, false)
                                  : weave_frame(mv, owner, theta, /*canon*/ is_pathblend || is_auto);
        const WeaveFrame fr2 = zr ? top_zone_frame(*zr, true) : fr;

        std::vector<int> src;   // parallel to its.indices → original facet index
        const indexed_triangle_set its = sel->get_facets(static_cast<EnforcerBlockerType>(s), src);
        if (its.indices.empty()) continue;

        // -- connected components (islands) via union-find over TRIANGLES, joined by a shared
        // EDGE (2 common vertices). Edge adjacency — not vertex adjacency — so coplanar zones
        // that only touch at a CORNER (1 shared vertex) stay separate islands (each flat zone
        // gets its own gradient instead of bleeding across the corner). Welded verts from
        // get_facets give stable shared indices.
        const int nt = int(its.indices.size());
        // s231 F7 — sólo las caras de ESTA zona entran en el cálculo: las islas del Top
        // se forman con las caras que miran arriba y las del Bottom con las que miran
        // abajo. Mezclarlas uniría por una arista compartida una zona superior con una
        // inferior (un borde vertical del escalón) y el degradado se estiraría por las
        // dos, que es justo lo que hay que evitar.
        const int want_facing = zone_bottom ? -1 : 1;
        std::vector<int> face_of(nt, 0);
        for (int k = 0; k < nt; ++k) {
            const auto& tri = its.indices[k];
            face_of[k] = facing_of(its.vertices[tri[0]], its.vertices[tri[1]], its.vertices[tri[2]]);
        }
        std::vector<int> parent(nt);
        std::iota(parent.begin(), parent.end(), 0);
        auto find = [&parent](int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
        auto unite = [&](int a, int b) { a = find(a); b = find(b); if (a != b) parent[a] = b; };
        {
            std::map<std::pair<int,int>, int> edge_owner;   // sorted (v0,v1) → first triangle
            auto add_edge = [&](int a, int b, int tri) {
                if (a > b) std::swap(a, b);
                const std::pair<int,int> key{a, b};
                auto it = edge_owner.find(key);
                if (it == edge_owner.end()) edge_owner.emplace(key, tri);
                else                        unite(tri, it->second);
            };
            for (int k = 0; k < nt; ++k) {
                if (face_of[k] != want_facing) continue;   // s231 F7
                const auto& tri = its.indices[k];
                add_edge(tri[0], tri[1], k);
                add_edge(tri[1], tri[2], k);
                add_edge(tri[2], tri[0], k);
            }
        }

        // per-island projected extent + which island each emitted triangle belongs to
        std::map<int,int> root_to_island;
        std::vector<float> imin, imax;
        std::vector<float> jmin, jmax;   // s318 F3 — extremos sobre el eje del pase de arriba
        std::vector<int>   tri_island(its.indices.size(), -1);
        for (size_t k = 0; k < its.indices.size(); ++k) {
            if (face_of[k] != want_facing) continue;   // s231 F7 — no es de esta zona
            const int root = find(int(k));
            auto rit = root_to_island.find(root);
            int isl;
            if (rit == root_to_island.end()) {
                isl = int(imin.size());
                root_to_island.emplace(root, isl);
                imin.push_back(1e9f); imax.push_back(-1e9f);
                jmin.push_back(1e9f); jmax.push_back(-1e9f);
            } else isl = rit->second;
            tri_island[k] = isl;
            for (int c = 0; c < 3; ++c) {
                const stl_vertex& v = its.vertices[its.indices[k][c]];
                const float pr = fr.proj(v.x(), v.y(), v.z());   // s318 F3
                imin[isl] = std::min(imin[isl], pr);
                imax[isl] = std::max(imax[isl], pr);
                const float pr2 = fr2.proj(v.x(), v.y(), v.z());
                jmin[isl] = std::min(jmin[isl], pr2);
                jmax[isl] = std::max(jmax[isl], pr2);
            }
        }

        // build one WeaveParams per island; map its facets to the new weave_list entry
        std::vector<int> island_weave(imin.size(), -1);
        for (size_t isl = 0; isl < imin.size(); ++isl) {
            WeaveParams w;
            if (zr) {
                // s318 F3 — opción A: los ejes ya van dentro (axis / axis2).
                w = make_zone_weave(*zr, fcolors, mats, bg_rgb, line_w, lh,
                                    imin[isl], imax[isl], jmin[isl], jmax[isl]);
            } else {
                w = is_pathblend
                    ? pathblend_make_weave(pbc, mats, bg_rgb, lh, theta, imin[isl], imax[isl], line_w,
                                           fr.anchor_proj)
                    : colorstitch_make_weave(kv, fcolors, theta, imin[isl], imax[isl], line_w,
                                             fr.anchor_proj);
                std::copy(fr.axis, fr.axis + 3, w.axis);   // s318 F3 — el shader proyecta con esto
            }
            // NEOTKO_COLORSTITCH_TAG — marca la banda: la lee el marcador de ángulo auto.
            w.auto_angle = is_auto;
            if (!w.on) continue;
            island_weave[isl] = int(weave_list.size());
            weave_list.push_back(std::move(w));
        }
        int n_mapped = 0;
        for (size_t k = 0; k < src.size(); ++k) {
            if (tri_island[k] < 0) continue;   // s231 F7 — faceta de la otra zona
            const int wi = island_weave[tri_island[k]];
            if (wi >= 0) { facet_weave_idx[src[k]] = wi; ++n_mapped; }
        }
        {   // s232 DEBUG — cuántas islas de ESTA zona recibieron tejido de verdad. Con
            // islas > 0 y weaves == 0 el culpable es `*_make_weave` devolviendo off, y
            // las caras caen al weave por slot (el del Top) o al color plano del slot.
            int n_on = 0;
            for (int wi : island_weave) if (wi >= 0) ++n_on;
            std::ostringstream os;
            os << "BOTTOM_WEAVE slot=" << s << " zone=" << (zone_bottom ? "bottom" : "top")
               << " islands=" << imin.size() << " weaves_on=" << n_on
               << " facets_mapped=" << n_mapped << " theta=" << theta;
            NeoDebug::write(NeoDebug::BOTTOM, os.str());
        }
        }   // zone (s231 F7)
    }
}

} // namespace Slic3r::GUI::ColorStitchPaintPreview
// NEOTKO_PROFILE_TAG_END
