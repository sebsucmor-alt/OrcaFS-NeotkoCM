// NEOTKO_COLORSTITCH_TAG — s316 fase B: retirada del legacy de ColorStitch al cargar.
// Declaración y porqué en ColorStitch.hpp (namespace ColorStitchLegacyMigration).
//
// 🚨 POR QUÉ ESTÁ EN SU PROPIO FICHERO. PrintConfig.cpp lo llama desde handle_legacy_composite, así
// que CUALQUIER binario que enlace libslic3r necesita estas definiciones, incluido el validador de
// perfiles. Cuando vivían en ColorStitch.cpp, el enlazador arrastraba ColorStitch.cpp.o entero, y
// con él NSVGUtils (stickers) y nanosvg, que el validador no enlaza: "_nsvgParse ... not found".
// Este fichero no puede depender de nada más pesado que la config. Si hace falta algo de
// ColorStitch.cpp, no se incluye aquí: se pasa como parámetro.
// s317 fase D: Preset.cpp (que también enlaza el validador) llama aquí para apagar el Sandwich de
// los presets. Por eso sandwich_active() lee el blob con nlohmann a pelo y NO con
// SurfacePassStack::from_json, que vive en ColorStitch.cpp.

#include "ColorStitch.hpp"       // sólo la declaración (las inline del header no crean enlace)
#include "PrintConfig.hpp"
#include "LocalesUtils.hpp"      // números sin depender del locale (el Mac del usuario, en español)
#include "SurfacePassKind.hpp"   // header ligero, sólo el enum

#include <nlohmann/json.hpp>

#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

namespace ColorStitchLegacyMigration {

static std::atomic<int> g_gradients{0};
static std::atomic<int> g_bands{0};
static std::atomic<int> g_sandwich_sources{0};
static std::atomic<int> g_sandwich_profiles{0};

static std::mutex               g_presets_mutex;
static std::vector<std::string> g_sandwich_presets;

void reset_report() { g_gradients = 0; g_bands = 0; g_sandwich_sources = 0; g_sandwich_profiles = 0; }

void note(int gradients, int bands) { g_gradients += gradients; g_bands += bands; }

void note_sandwich(int sources, int profiles) { g_sandwich_sources += sources; g_sandwich_profiles += profiles; }

Report take_report()
{
    Report r;
    r.gradients         = g_gradients.exchange(0);
    r.bands             = g_bands.exchange(0);
    r.sandwich_sources  = g_sandwich_sources.exchange(0);
    r.sandwich_profiles = g_sandwich_profiles.exchange(0);
    return r;
}

double nominal_top_line_spacing_mm(const ConfigBase& cfg, double nozzle_mm)
{
    const double nozzle = (nozzle_mm > 0.05) ? nozzle_mm : 0.4;
    double line_w = nozzle * 1.125;                       // 0/auto → el default de Orca
    if (auto* lw = cfg.option<ConfigOptionFloatOrPercent>("line_width")) {
        const double v = lw->get_abs_value(nozzle);
        if (v > 0.05) line_w = v;
    }
    double top = line_w;                                  // top es ratio sobre line_width
    if (auto* tw = cfg.option<ConfigOptionFloatOrPercent>("top_surface_line_width")) {
        const double v = tw->get_abs_value(line_w);
        if (v > 0.05) top = v;
    }
    double h = 0.2;
    if (auto* lh = cfg.option<ConfigOptionFloat>("layer_height"))
        if (lh->value > 0.001) h = lh->value;
    const double sp = top - h * (1.0 - 0.25 * M_PI);      // Flow.cpp:183, literal
    return (sp > 0.01) ? sp : std::max(0.01, top);
}

static const char* const kRolePrefixes[2] = { "interlayer_colormix_", "interlayer_colormix_penu_" };
static const char* const kBandSuffix[4]   = { "a", "b", "c", "d" };

// Lectura sin depender del locale (el Mac del usuario está en español: stod leería "8,5").
static bool kv_num(const std::map<std::string, std::string>& kv, const std::string& k, double& out)
{
    auto it = kv.find(k);
    if (it == kv.end() || it->second.empty()) return false;
    size_t pos = 0;
    const double v = string_to_double_decimal_point(it->second, &pos);
    if (pos == 0) return false;
    out = v;
    return true;
}

bool migrate_kv(std::map<std::string, std::string>& kv, double sp_mm, unsigned* changed_roles, bool count)
{
    bool changed = false;
    unsigned roles = 0;
    for (int r = 0; r < 2; ++r) {
        const std::string P(kRolePrefixes[r]);
        double mode_d = 0.0;
        if (!kv_num(kv, P + "mode", mode_d)) continue;    // este rol no tiene receta aquí
        const int mode = int(std::lround(mode_d));
        if (mode == 1 || mode == 2) {
            double span = -1.0;
            const bool has = kv_num(kv, P + "gradient_span_mm", span);
            if (!has || span < 0.0) {
                kv[P + "gradient_span_mm"] = "0";
                if (count) ++g_gradients;
                roles |= (r == 0) ? kRoleTopGradient : kRolePenuGradient;
                changed = true;
            }
        } else if (mode == 3) {
            // Sólo si las cuentas viven AQUÍ. Si el payload no las trae (heredaría las del
            // preset), convertir daría bandas de 0 mm y el modo 4 caería al pattern string.
            double cnt[4] = { 0, 0, 0, 0 };
            int live = 0;
            for (int i = 0; i < 4; ++i)
                if (kv_num(kv, P + "band_count_" + kBandSuffix[i], cnt[i]) && cnt[i] > 0.5) ++live;
            if (live < 2) continue;
            for (int i = 0; i < 4; ++i)
                kv[P + "band_mm_" + kBandSuffix[i]] =
                    float_to_string_decimal_point(cnt[i] > 0.5 ? std::lround(cnt[i]) * sp_mm : 0.0, 4);
            // `invert` se deja TAL CUAL: el modo 4 también lo respeta (da la vuelta a las bandas
            // y a sus tools, ColorStitch.cpp, paso posterior al preflight). s316: hubo aquí un
            // "arreglo" que las invertía y desmarcaba invert; daba lo mismo y tocaba más datos.
            kv[P + "mode"] = "4";
            if (count) ++g_bands;
            roles |= (r == 0) ? kRoleTopBands : kRolePenuBands;
            changed = true;
        }
    }
    if (changed_roles) *changed_roles |= roles;
    return changed;
}

bool migrate_config(DynamicPrintConfig& cfg, double sp_mm)
{
    bool changed = false;
    double sp = sp_mm;
    if (!(sp > 0.0)) {
        double nozzle = 0.4;
        if (auto* nd = cfg.option<ConfigOptionFloats>("nozzle_diameter"))
            if (!nd->values.empty() && nd->values.front() > 0.05) nozzle = nd->values.front();
        sp = nominal_top_line_spacing_mm(cfg, nozzle);
    }
    for (const char* pre : kRolePrefixes) {
        const std::string P(pre);
        ConfigOption* mode_opt = cfg.option(P + "mode");
        if (!mode_opt) continue;
        const int mode = mode_opt->getInt();
        if (mode == 1 || mode == 2) {
            const ConfigOption* span = cfg.option(P + "gradient_span_mm");
            if (!span || span->getFloat() < 0.0) {
                cfg.set_key_value(P + "gradient_span_mm", new ConfigOptionFloat(0.0));
                ++g_gradients;
                changed = true;
            }
        } else if (mode == 3) {
            int cnt[4] = { 0, 0, 0, 0 };
            int live = 0;
            for (int i = 0; i < 4; ++i)
                if (const ConfigOption* c = cfg.option(P + "band_count_" + kBandSuffix[i])) {
                    cnt[i] = c->getInt();
                    if (cnt[i] > 0) ++live;
                }
            if (live < 2) continue;
            for (int i = 0; i < 4; ++i)
                cfg.set_key_value(P + "band_mm_" + kBandSuffix[i],
                                  new ConfigOptionFloat(cnt[i] > 0 ? double(cnt[i]) * sp : 0.0));
            mode_opt->setInt(4);
            ++g_bands;
            changed = true;
        }
    }
    return changed;
}

// NEOTKO_SANDWICH_TAG_START — s317 fase D: el Sandwich Editor desaparece.
static const char* const kSandwichEnables[4] = {
    "multipass_enabled", "penultimate_multipass_enabled",
    "interlayer_colormix_enabled", "multipass_path_gradient" };
static const char* const kSandwichBlobs[2] = { "neotko_surface_passes_top", "neotko_surface_passes_penu" };

static bool opt_bool(const ConfigBase& cfg, const char* key)
{
    auto* o = cfg.option<ConfigOptionBool>(key);
    return o && o->value;
}

static int opt_int(const ConfigBase& cfg, const char* key, int def)
{
    auto* o = cfg.option<ConfigOptionInt>(key);
    return o ? o->value : def;
}

// Espejo LITERAL de SurfacePassStack::from_json, sólo para saber si el blob manda y si hace algo:
//   -1 = sin autorar ("" / JSON roto / sin pases) → resolve() cae a las claves legacy;
//    0 = autorado sin efecto (enabled=false, o todo None) → resolve() NO sintetiza;
//    1 = autorado con efecto. Un pase sin "kind" (o fuera de 0..3) es Solid, igual que from_json.
static int blob_state(const std::string& text)
{
    if (text.empty()) return -1;
    nlohmann::json root;
    try { root = nlohmann::json::parse(text); }
    catch (...) { return -1; }
    if (!root.is_object() || !root.contains("passes") || !root["passes"].is_array()) return -1;
    bool any_pass = false, any_effect = false;
    for (const auto& e : root["passes"]) {
        if (!e.is_object()) continue;
        any_pass = true;
        int k = int(SurfacePassKind::Solid);
        if (e.contains("kind") && e["kind"].is_number_integer()) {
            const int v = e["kind"].get<int>();
            if (v >= 0 && v <= 3) k = v;
        }
        if (k != int(SurfacePassKind::None)) any_effect = true;
    }
    if (!any_pass) return -1;
    const bool enabled = !(root.contains("enabled") && root["enabled"].is_boolean() && !root["enabled"].get<bool>());
    return (enabled && any_effect) ? 1 : 0;
}

bool sandwich_active(const ConfigBase& cfg)
{
    for (int z = 0; z < 2; ++z) {
        std::string blob;
        if (auto* o = cfg.option<ConfigOptionString>(kSandwichBlobs[z])) blob = o->value;
        const int st = blob_state(blob);
        if (st == 1) return true;
        if (st == 0) continue;
        // Sin autorar: lo que sintetizaría synthesize_from_legacy para esta zona.
        const bool penu = (z == 1);
        if (opt_bool(cfg, penu ? "penultimate_multipass_enabled" : "multipass_enabled")) return true;
        const int want = penu ? 2 : 1;
        if (opt_bool(cfg, "interlayer_colormix_enabled")) {
            const int s = opt_int(cfg, "interlayer_colormix_surface", 0);
            if (s == 0 || s == want) return true;
        }
        if (opt_bool(cfg, "multipass_path_gradient")) {
            const int s = opt_int(cfg, "pathblend_surface", 0);
            if (s == 0 || s == want) return true;
        }
    }
    return false;
}

bool switch_off_sandwich(DynamicPrintConfig& cfg)
{
    bool changed = false;
    for (const char* k : kSandwichEnables)
        if (auto* o = cfg.option<ConfigOptionBool>(k); o && o->value) { o->value = false; changed = true; }
    for (const char* k : kSandwichBlobs)
        if (auto* o = cfg.option<ConfigOptionString>(k); o && !o->value.empty()) { o->value.clear(); changed = true; }
    return changed;
}

bool switch_off_sandwich_preset(DynamicPrintConfig& cfg, const std::string& preset_name)
{
    if (!sandwich_active(cfg)) return false;
    switch_off_sandwich(cfg);
    std::lock_guard<std::mutex> lock(g_presets_mutex);
    g_sandwich_presets.push_back(preset_name);
    return true;
}

std::vector<std::string> take_sandwich_presets()
{
    std::lock_guard<std::mutex> lock(g_presets_mutex);
    std::vector<std::string> out;
    out.swap(g_sandwich_presets);
    return out;
}
// NEOTKO_SANDWICH_TAG_END — s317 fase D

} // namespace ColorStitchLegacyMigration

} // namespace Slic3r
