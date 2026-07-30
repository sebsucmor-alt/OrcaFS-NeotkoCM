// NEOTKO_PROFILE_TAG_START — s233, ver la cabecera. Cuerpos movidos TAL CUAL desde
// GLGizmoColorMixPainter.cpp (fallback_color_for_id, color_for_profile,
// gizmo_materials, resolve_object_base_bg, build_ebt_colors_for_volume); el único
// cambio funcional es que el ModelObject dueño llega por parámetro en vez de salir de
// m_c->selection_info(), porque aquí no hay gizmo del que colgar.
#include "ColorMixPaintPreview.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SurfaceColorMix.hpp"                 // NEOTKO_LOG (canal PROFILE)
#include "libslic3r/SurfaceEffectProfile.hpp"
#include "libslic3r/ColorSci/StackFlatten.hpp"           // sandwich_colour_stacked
#include "libslic3r/TriangleSelector.hpp"                // get_facets por slot (islas)

#include "slic3r/GUI/3DScene.hpp"                        // GLVolume::NEUTRAL_COLOR
#include "slic3r/GUI/GUI_App.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>     // strtoul (tool_col_rgba)
#include <functional>   // std::hash (context_key)
#include <numeric>     // std::iota (union-find de islas)
#include <sstream>   // NEOTKO_LOG arma un ostringstream

namespace Slic3r::GUI::ColorMixPaintPreview {

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
    // Index 0 holds the volume's neutral base, indices 1..COLORMIX_SLOT_COUNT-1 hold
    // per-slot colors. TriangleSelectorPatch internally prepends the volume base color,
    // then maps EnforcerBlockerType(N) → m_ebt_colors[N+1]. We mirror MMU's layout:
    //   [0] = volume base (unpainted)
    //   [1..N-1] = per-slot colors
    const int MAX_SLOTS = ModelVolume::COLORMIX_SLOT_COUNT;
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
            const int pid = mv->colormix_slot_to_profile_id[s];
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
// color_mix_paint_facets sólo cubre la GEOMETRÍA pintada; esta clave cubre el COLOR.
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
        for (int s = 1; s < ModelVolume::COLORMIX_SLOT_COUNT; ++s) {
            const int pid = mv->colormix_slot_to_profile_id[s];
            if (pid == 0) continue;
            mix(h, (uint64_t)(s * 1000003 + pid));
            if (const SurfaceEffectProfile* p = mgr.find(pid)) {
                mix(h, (uint64_t)p->preview_argb);
                mix(h, hs(p->stack_top_json));
                mix(h, hs(p->stack_penu_json));
            }
        }
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
        mix((uint64_t) mv->color_mix_paint_facets.timestamp());
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
        if (mv->color_mix_paint_facets.empty() || mv->mmu_segmentation_facets.empty())
            continue;

        TriangleSelector sw(mv->mesh());
        sw.deserialize(mv->color_mix_paint_facets.get_data(), false,
                       static_cast<EnforcerBlockerType>(ModelVolume::COLORMIX_SLOT_COUNT - 1));
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
// Tejido (weave) — s233 F3: bloque movido LITERAL desde GLGizmoColorMixPainter.cpp.
// Las matemáticas no se han tocado; sólo dejan de ser `static` de aquel fichero y el
// objeto/selector llegan por parámetro en vez de salir del estado del gizmo. Los logs
// TOP_WEAVE / BOTTOM_WEAVE (canal BOTTOM) viajan con el código a propósito: el usuario
// pidió expresamente NO retirarlos porque sirven justo para este frente.
// ==================================================================================

double weave_layer_height()
{
    if (auto* o = wxGetApp().preset_bundle->prints.get_edited_preset()
                      .config.option<ConfigOptionFloat>("layer_height"))
        if (o->value > 0.001) return o->value;
    return 0.2;
}


// NEOTKO_COLORSTITCH_TAG — resolved TOP-surface line width (mm), "casi real": the
// weave runs on top-facing surfaces, so the stripe pitch should match what the slicer
// lays there. Resolved from config WITHOUT a slice (top_surface_line_width → line_width
// → nozzle), so a Calculate/pre-slice pass is not required for the pitch. See on-screen
// notice for the only thing slicing would add (per-layer auto-angle).
double weave_top_line_width()
{
    const auto& cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
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


// NEOTKO_COLORSTITCH_TAG — single source of truth for the ColorStitch per-line tool
// sequence used by ALL previews (3D weave + pro-tray strip), built with the SAME
// engine builders the slicer uses (SurfaceColorMix.cpp:1326+): modes 1-3 →
// build_dithered_tools_2color/_3color / build_custom_bands over n_lines; mode 0 →
// pattern string (digit → 0-based physical tool). penu=false → top-role keys.
// Returns physical tool indices (length n_lines for modes 1-3; pattern length for
// mode 0 → caller tiles). Empty kv ⇒ empty result.
std::vector<int> colorstitch_tool_sequence(
    const std::map<std::string, std::string>& kv, bool penu, int n_lines)
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
    case 1: seq = Slic3r::SurfaceColorMix::build_dithered_tools_2color(n_lines, ta, tb, pa, ea, ga); break;
    case 2: seq = Slic3r::SurfaceColorMix::build_dithered_tools_3color(n_lines, ta, tb, tc, pa, pb, ea, ga, ov); break;
    case 3: seq = Slic3r::SurfaceColorMix::build_custom_bands(n_lines, ta, ba, tb, bb, tc, bc, td, bd); break;
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
// colormix payload (predict swatches with no resolved stack). Empty ⇒ not ColorStitch.
// s231 F7 — extractores por STACK (antes estaban cableados a `stack_top_json`, que es
// justo por lo que el Bottom no tenía forma de llegar al preview: los tres puntos de
// entrada del weave leían Top y sólo Top).
std::map<std::string, std::string>
colorstitch_kv_from_stack(const Slic3r::SurfacePassStack& st)
{
    for (const Slic3r::SurfacePass& p : st.passes)
        if (p.kind == Slic3r::SurfacePassKind::ColorMix && !p.colormix.kv.empty())
            return p.colormix.kv;
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
    if (prof.colormix.present && !prof.colormix.kv.empty())
        return prof.colormix.kv;
    return {};
}

// NEOTKO_COLORSTITCH_TAG — band orientation for a ColorStitch slot = printed fill lines =
// cm_angle + 90°. `is_auto` set when the kv leaves the angle on auto (-1).
float colorstitch_weave_theta(const std::map<std::string, std::string>& kv, bool& is_auto)
{
    int angle = -1;
    const auto it = kv.find("interlayer_colormix_angle");
    if (it != kv.end()) { try { angle = std::stoi(it->second); } catch (...) {} }
    float base_rad;
    if (angle >= 0) { base_rad = float(angle) * float(M_PI) / 180.f; is_auto = false; }
    else            { base_rad = float(M_PI) / 4.f;                  is_auto = true;  }
    // NEOTKO_COLORSTITCH_TAG — band direction = cm_angle (verified against the slice). The
    // per-layer alternation that used to scramble this is now locked out for a fixed angle via
    // f->is_using_template_angle in Fill.cpp, so weave == slice == pro-tray bar at cm_angle.
    return base_rad;
}

// NEOTKO_COLORSTITCH_TAG — one WeaveParams from a projected extent [pmin,pmax] along `theta`.
// N (line count) comes from the REAL line width; pitch = span/N so each stripe is one line
// wide (≈ line_w) until the LUT cap (64), beyond which it coarsens to still cover the span.
WeaveParams
colorstitch_make_weave(const std::map<std::string, std::string>& kv,
                       const std::vector<std::string>& fcolors,
                       float theta, float pmin, float pmax, float line_w)
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
        for (int i = 0; i < count; ++i) {
            const int tool0 = seq[(size_t) i % seq.size()];
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
        w.on = true; w.tile = true; w.angle_rad = theta; w.p0 = pmin; w.pitch = lw;
        fill_cols(base, n);
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
                     float pmin, float pmax, float line_w)
{
    WeaveParams w;
    const float span = pmax - pmin;
    if (span < 1e-3f || pbc.tool_bottom < 0) return w;   // w.on stays false

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

    const int N = std::clamp((int)std::lround(span / std::max(line_w, 0.05f)), 4, 64);
    w.cols.resize(N);
    const int tb = std::clamp(pbc.tool_bottom, 0, 3);
    const int tt = std::clamp(pbc.tool_top,    0, 3);
    for (int i = 0; i < N; ++i) {
        const double t      = (N > 1) ? double(i) / double(N - 1) : 0.5;
        const double h_ramp = floor_pb + t * range;
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
    }
    w.on = true; w.tile = false; w.angle_rad = 0.f; w.p0 = pmin; w.pitch = span / float(N);
    return w;
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
    const float line_w = (float) weave_top_line_width();
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
    for (int s = 1; s < ModelVolume::COLORMIX_SLOT_COUNT; ++s) {
        const int pid = mv->colormix_slot_to_profile_id[s];
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
                // tienen el top sin un solo pase ColorMix (`kv=0` + `payload_cm=0`), con
                // 564 bytes de stack y penu VACÍO — o sea ~2 pases donde la receta del
                // generador tenía 1+1. Falta ver qué son esos pases y con qué claves,
                // que es lo que distingue "se degradaron a Solid" de "son ColorMix con
                // el payload perdido" (arreglos distintos).
                const Slic3r::SurfacePassStack st_t =
                    Slic3r::SurfacePassStack::from_json(p->stack_top_json);
                const Slic3r::SurfacePassStack st_p =
                    Slic3r::SurfacePassStack::from_json(p->stack_penu_json);
                auto kinds = [](const Slic3r::SurfacePassStack& st) {
                    std::string o; using K = Slic3r::SurfacePassKind;
                    for (const auto& pp : st.passes) {
                        o += (pp.kind == K::Solid ? 'S' : pp.kind == K::ColorMix ? 'C'
                            : pp.kind == K::PathBlend ? 'P' : '?');
                        o += std::to_string(pp.colormix.kv.size());
                        o += ' ';
                    }
                    return o.empty() ? std::string("-") : o;
                };
                std::ostringstream os;
                os << "TOP_WEAVE slot=" << s << " pid=" << pid
                   << " top_len=" << p->stack_top_json.size()
                   << " penu_len=" << p->stack_penu_json.size()
                   << " kv=" << kv.size() << " pathblend=" << (is_pathblend ? 1 : 0)
                   << " payload_cm=" << (p->colormix.present ? p->colormix.kv.size() : 0)
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
                   << " payload_cm=" << (p->colormix.present ? p->colormix.kv.size() : 0);
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
        if (kv.empty() && !is_pathblend) continue;

        bool is_auto = false;
        const float theta = is_pathblend ? 0.f : colorstitch_weave_theta(kv, is_auto);
        if (is_auto && any_auto_angle) *any_auto_angle = true;
        const float sN = std::sin(theta), cN = std::cos(theta);

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
            } else isl = rit->second;
            tri_island[k] = isl;
            for (int c = 0; c < 3; ++c) {
                const stl_vertex& v = its.vertices[its.indices[k][c]];
                const float pr = -v.x() * sN + v.y() * cN;
                imin[isl] = std::min(imin[isl], pr);
                imax[isl] = std::max(imax[isl], pr);
            }
        }

        // build one WeaveParams per island; map its facets to the new weave_list entry
        std::vector<int> island_weave(imin.size(), -1);
        for (size_t isl = 0; isl < imin.size(); ++isl) {
            WeaveParams w = is_pathblend
                ? pathblend_make_weave(pbc, mats, bg_rgb, lh, imin[isl], imax[isl], line_w)
                : colorstitch_make_weave(kv, fcolors, theta, imin[isl], imax[isl], line_w);
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

} // namespace Slic3r::GUI::ColorMixPaintPreview
// NEOTKO_PROFILE_TAG_END
