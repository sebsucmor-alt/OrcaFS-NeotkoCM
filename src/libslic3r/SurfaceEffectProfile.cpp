// NEOTKO_PROFILE_TAG_START
#include "SurfaceEffectProfile.hpp"
#include "Config.hpp"
#include "PrintConfig.hpp"
#include "ColorStitch.hpp" // NEOTKO_PROFILE_TAG — NeoDebug PROFILE channel
#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

SurfaceEffectProfileManager& SurfaceEffectProfileManager::get()
{
    static SurfaceEffectProfileManager s_instance;
    return s_instance;
}

int SurfaceEffectProfileManager::add(SurfaceEffectProfile profile)
{
    profile.id = m_next_id++;
    if (profile.name.empty())
        profile.name = "Profile " + std::to_string(profile.id);
    m_profiles.push_back(std::move(profile));
    const auto& p = m_profiles.back();
    NEOTKO_LOG(PROFILE, "MGR add id=" << p.id << " name='" << p.name << "'"
        << " colormix=" << (p.colorstitch.present  ? "yes" : "no")
        << " pathblend=" << (p.pathblend.present ? "yes" : "no")
        << " multipass=" << (p.multipass.present ? "yes" : "no")
        << " cm_kv=" << p.colorstitch.kv.size()
        << " mp_kv=" << p.multipass.kv.size()
        << " pb_kv=" << p.pathblend.kv.size()
        << " total=" << m_profiles.size());
    return p.id;
}

// NEOTKO_PROFILE_TAG — s238: ver la declaración en el header. `add` no sirve para
// esto porque reasigna el id, y el id es justo lo que la pintura ya guardó.
bool SurfaceEffectProfileManager::adopt(SurfaceEffectProfile profile)
{
    if (profile.id <= 0) return false;
    if (find(profile.id) != nullptr) return false;
    if (profile.name.empty())
        profile.name = "Profile " + std::to_string(profile.id);
    m_next_id = std::max(m_next_id, profile.id + 1);
    NEOTKO_LOG(PROFILE, "MGR adopt id=" << profile.id << " name='" << profile.name
        << "' total=" << (m_profiles.size() + 1));
    m_profiles.push_back(std::move(profile));
    return true;
}

bool SurfaceEffectProfileManager::remove(int id)
{
    for (auto it = m_profiles.begin(); it != m_profiles.end(); ++it)
        if (it->id == id) {
            NEOTKO_LOG(PROFILE, "MGR remove id=" << id << " name='" << it->name
                << "' remaining=" << (m_profiles.size() - 1));
            m_profiles.erase(it); return true;
        }
    NEOTKO_LOG(PROFILE, "MGR remove id=" << id << " NOT_FOUND");
    return false;
}

bool SurfaceEffectProfileManager::rename(int id, const std::string& new_name)
{
    for (auto& p : m_profiles)
        if (p.id == id) {
            NEOTKO_LOG(PROFILE, "MGR rename id=" << id << " '" << p.name
                << "' -> '" << new_name << "'");
            p.name = new_name; return true;
        }
    return false;
}

const SurfaceEffectProfile* SurfaceEffectProfileManager::find(int id) const
{
    for (const auto& p : m_profiles)
        if (p.id == id) return &p;
    return nullptr;
}

SurfaceEffectProfile* SurfaceEffectProfileManager::find_mut(int id)
{
    for (auto& p : m_profiles)
        if (p.id == id) return &p;
    return nullptr;
}

void SurfaceEffectProfileManager::clear()
{
    m_profiles.clear();
    m_next_id = 1;
}

SurfaceEffectPayload
SurfaceEffectProfileManager::snapshot_keys(const DynamicPrintConfig& cfg,
                                          const std::vector<std::string>& keys)
{
    SurfaceEffectPayload out;
    for (const auto& k : keys) {
        if (cfg.has(k)) {
            // opt_serialize is non-const in DynamicPrintConfig; const_cast is safe
            // since serialize() does not mutate state.
            out.kv[k] = const_cast<DynamicPrintConfig&>(cfg).opt_serialize(k);
        }
    }
    out.present = !out.kv.empty();
    return out;
}

// NEOTKO_COLORSTITCH_TAG — s112 fix. Lift ColorStitch/PathBlend pass payloads from
// the resolved stacks to the profile level so the slicer's painter-mode gate
// (which reads p.colorstitch.present, not the visual stack_*_json) fires.
void SurfaceEffectProfileManager::payload_from_stacks(const SurfacePassStack& top,
                                                      const SurfacePassStack& penu,
                                                      SurfaceEffectProfile& p)
{
    bool cm_top = false, cm_penu = false;
    bool pb_top = false, pb_penu = false;

    auto lift = [](const SurfacePassStack& st, bool penu_zone,
                   SurfaceEffectProfile& prof, bool& cm_hit, bool& pb_hit) {
        for (const SurfacePass& pass : st.passes) {
            if (pass.kind == SurfacePassKind::ColorStitch && pass.colorstitch.present) {
                // kv keys are already role-prefixed (interlayer_colormix_ for top,
                // interlayer_colormix_penu_ for penu) — straight merge.
                for (const auto& [k, v] : pass.colorstitch.kv)
                    prof.colorstitch.kv[k] = v;
                cm_hit = true;
            } else if (pass.kind == SurfacePassKind::PathBlend && pass.pathblend.present) {
                for (const auto& [k, v] : pass.pathblend.kv)
                    prof.pathblend.kv[k] = v;
                pb_hit = true;
            }
        }
        (void)penu_zone;
    };

    lift(top,  false, p, cm_top,  pb_top);
    lift(penu, true,  p, cm_penu, pb_penu);

    // surface enum: 0=Both / 1=Top only / 2=Penu only (mirror Tab.cpp:4627).
    auto surface_enum = [](bool t, bool pen) -> std::string {
        if (t && pen) return "0";
        if (t)        return "1";
        return "2";
    };

    if (cm_top || cm_penu) {
        p.colorstitch.present = true;
        p.colorstitch.kv["interlayer_colormix_enabled"] = "1";
        p.colorstitch.kv["interlayer_colormix_surface"] = surface_enum(cm_top, cm_penu);
    }
    if (pb_top || pb_penu) {
        p.pathblend.present = true;
        p.pathblend.kv["multipass_path_gradient"] = "1";
        p.pathblend.kv["pathblend_surface"] = surface_enum(pb_top, pb_penu);
    }
    NEOTKO_LOG(PROFILE, "payload_from_stacks cm=[" << cm_top << "," << cm_penu
        << "] pb=[" << pb_top << "," << pb_penu << "]"
        << " cm_kv=" << p.colorstitch.kv.size() << " pb_kv=" << p.pathblend.kv.size());
}

void SurfaceEffectProfileManager::restore_keys(DynamicPrintConfig& cfg,
                                               const SurfaceEffectPayload& payload)
{
    if (!payload.present) return;
    for (const auto& [k, v] : payload.kv) {
        if (cfg.has(k))
            cfg.set_deserialize_strict(k, v);
    }
}

const std::vector<std::string>& SurfaceEffectProfileManager::colorstitch_keys()
{
    // Canonical list — must stay in sync with PrintConfig.hpp interlayer_colormix_*.
    static const std::vector<std::string> k = {
        // global flags
        "interlayer_colormix_enabled",
        "interlayer_colormix_surface",
        "interlayer_colormix_filament_filter",
        "interlayer_colormix_min_length",
        "interlayer_colormix_use_virtual",
        "interlayer_colormix_top_zone",
        "interlayer_colormix_penu_zone",
        // pattern strings
        "interlayer_colormix_pattern_top",
        "interlayer_colormix_pattern_penultimate",
        // top gradient
        "interlayer_colormix_mode",
        "interlayer_colormix_pct_a",
        "interlayer_colormix_pct_b",
        "interlayer_colormix_easing",
        "interlayer_colormix_gamma",
        "interlayer_colormix_min_surface_lines",
        "interlayer_colormix_overlap",
        "interlayer_colormix_invert",
        "interlayer_colormix_repetitions",
        "interlayer_colormix_band_count_a",
        "interlayer_colormix_band_count_b",
        "interlayer_colormix_band_count_c",
        "interlayer_colormix_band_count_d",
        // NEOTKO_COLORSTITCH_TAG — s314: bandas en mm (Pattern mode 4). Sin estas cuatro
        // el diseño en mm no viaja dentro del payload del pase y el round-trip por
        // perfil / .3mf se pierde a mitad de camino.
        "interlayer_colormix_band_mm_a",
        "interlayer_colormix_band_mm_b",
        "interlayer_colormix_band_mm_c",
        "interlayer_colormix_band_mm_d",
        "interlayer_colormix_gradient_span_mm",   // s315
        "interlayer_colormix_tool_a",
        "interlayer_colormix_tool_b",
        "interlayer_colormix_tool_c",
        "interlayer_colormix_tool_d",
        // penu gradient mirrors
        "interlayer_colormix_penu_mode",
        "interlayer_colormix_penu_pct_a",
        "interlayer_colormix_penu_pct_b",
        "interlayer_colormix_penu_easing",
        "interlayer_colormix_penu_gamma",
        "interlayer_colormix_penu_min_surface_lines",
        "interlayer_colormix_penu_overlap",
        "interlayer_colormix_penu_invert",
        "interlayer_colormix_penu_repetitions",
        "interlayer_colormix_penu_band_count_a",
        "interlayer_colormix_penu_band_count_b",
        "interlayer_colormix_penu_band_count_c",
        "interlayer_colormix_penu_band_count_d",
        "interlayer_colormix_penu_band_mm_a",
        "interlayer_colormix_penu_band_mm_b",
        "interlayer_colormix_penu_band_mm_c",
        "interlayer_colormix_penu_band_mm_d",
        "interlayer_colormix_penu_gradient_span_mm",   // s315
        "interlayer_colormix_penu_tool_a",
        "interlayer_colormix_penu_tool_b",
        "interlayer_colormix_penu_tool_c",
        "interlayer_colormix_penu_tool_d",
    };
    return k;
}

const std::vector<std::string>& SurfaceEffectProfileManager::multipass_keys()
{
    // NEOTKO_PROFILE_TAG — Fase F. Canonical list — must stay in sync with
    // PrintConfig.hpp multipass_* / penultimate_multipass_* declarations.
    // EXCLUDED: multipass_prime_volume (print-wide, read directly from preset).
    static const std::vector<std::string> k = {
        // Top role
        "multipass_enabled",
        "multipass_surface",
        "multipass_num_passes",
        "multipass_tool_1",
        "multipass_tool_2",
        "multipass_tool_3",
        "multipass_width_ratio_1",
        "multipass_width_ratio_2",
        "multipass_width_ratio_3",
        "multipass_vary_pattern",
        "multipass_angle_1",
        "multipass_angle_2",
        "multipass_angle_3",
        "multipass_pa_mode",
        "multipass_pa_value",
        "multipass_fan_1",
        "multipass_fan_2",
        "multipass_fan_3",
        "multipass_speed_pct_1",
        "multipass_speed_pct_2",
        "multipass_speed_pct_3",
        "multipass_gcode_start_1",
        "multipass_gcode_start_2",
        "multipass_gcode_start_3",
        "multipass_gcode_end_1",
        "multipass_gcode_end_2",
        "multipass_gcode_end_3",
        "multipass_perimeter_override",
        "multipass_path_gradient",
        // Penultimate role mirrors
        "penultimate_multipass_enabled",
        "penultimate_multipass_num_passes",
        "penultimate_multipass_tool_1",
        "penultimate_multipass_tool_2",
        "penultimate_multipass_tool_3",
        "penultimate_multipass_width_ratio_1",
        "penultimate_multipass_width_ratio_2",
        "penultimate_multipass_width_ratio_3",
        "penultimate_multipass_vary_pattern",
        "penultimate_multipass_angle_1",
        "penultimate_multipass_angle_2",
        "penultimate_multipass_angle_3",
        "penultimate_multipass_fan_1",
        "penultimate_multipass_fan_2",
        "penultimate_multipass_fan_3",
        "penultimate_multipass_speed_pct_1",
        "penultimate_multipass_speed_pct_2",
        "penultimate_multipass_speed_pct_3",
        "penultimate_multipass_gcode_start_1",
        "penultimate_multipass_gcode_start_2",
        "penultimate_multipass_gcode_start_3",
        "penultimate_multipass_gcode_end_1",
        "penultimate_multipass_gcode_end_2",
        "penultimate_multipass_gcode_end_3",
    };
    return k;
}

const std::vector<std::string>& SurfaceEffectProfileManager::pathblend_keys()
{
    // NEOTKO_PROFILE_TAG — Fase G. Must stay in sync with PrintConfig.hpp
    // pathblend_* declarations. `multipass_path_gradient` is the master
    // enable flag (per-region in preset land; per-profile here means "this
    // profile drives PathBlend on its painted area").
    static const std::vector<std::string> k = {
        "multipass_path_gradient",
        "pathblend_num_passes",
        "pathblend_tool_1",
        "pathblend_tool_2",
        "pathblend_tool_3",
        "pathblend_tool_4",
        "pathblend_layer_ratio_1",
        "pathblend_layer_ratio_2",
        "pathblend_layer_ratio_3",
        "pathblend_layer_ratio_4",
        "pathblend_min_ratio",
        "pathblend_max_ratio",
        "pathblend_ease_mode",
        "pathblend_surface",
        "pathblend_invert_gradient",
        "pathblend_fill_angle",
    };
    return k;
}

// ----------------------------------------------------------------------------
// JSON round-trip — Fase C (3mf integration).
//
// Schema (versioned for forward compat):
//   { "v": 1,
//     "next_id": 7,
//     "profiles": [
//       { "id": 1, "name": "Fade red→blue", "preview_argb": 4287827834,
//         "colormix":  { "present": true,  "kv": { "key": "val", ... } },
//         "pathblend": { "present": false, "kv": {} },
//         "multipass": { "present": false, "kv": {} } },
//       ...
//     ] }
//
// Unknown fields are tolerated on read so a newer save still loads in an older
// build (best-effort — payloads it does not recognise are simply ignored).
// ----------------------------------------------------------------------------

static nlohmann::json payload_to_json(const SurfaceEffectPayload& p)
{
    nlohmann::json j;
    j["present"] = p.present;
    j["kv"]      = p.kv;
    return j;
}

static SurfaceEffectPayload payload_from_json(const nlohmann::json& j)
{
    SurfaceEffectPayload out;
    if (j.is_object()) {
        if (j.contains("present") && j["present"].is_boolean())
            out.present = j["present"].get<bool>();
        if (j.contains("kv") && j["kv"].is_object())
            for (auto it = j["kv"].begin(); it != j["kv"].end(); ++it)
                if (it.value().is_string())
                    out.kv[it.key()] = it.value().get<std::string>();
    }
    return out;
}

// NEOTKO_COLORSTITCH_TAG — s316 fase B. El payload del perfil Y cada pase de sus tres pilas: el
// motor lee de las dos fuentes (la pila manda cuando hay override de pase), así que migrar sólo una
// dejaría la otra en legacy.
// Un perfil: payload + sus tres pilas. true si cambió algo; lo anota en el informe una vez por rol.
static bool migrate_one_profile(SurfaceEffectProfile& p, double sp_mm)
{
    namespace M = ColorStitchLegacyMigration;
    // Se migra con count = false y se anota UNA vez por perfil y rol: el mismo degradado vive
    // en el payload y en la pila, y contarlo por almacén daba el doble en el aviso.
    unsigned roles = 0;
    bool changed = M::migrate_kv(p.colorstitch.kv, sp_mm, &roles, /*count*/ false);
    for (std::string* js : { &p.stack_top_json, &p.stack_penu_json, &p.stack_bottom_json }) {
        if (js->empty()) continue;
        SurfacePassStack st = SurfacePassStack::from_json(*js);
        bool st_changed = false;
        for (auto& pass : st.passes)
            st_changed |= M::migrate_kv(pass.colorstitch.kv, sp_mm, &roles, /*count*/ false);
        if (st_changed) { *js = st.to_json(); changed = true; }
    }
    if (changed)
        M::note(int((roles & M::kRoleTopGradient) != 0) + int((roles & M::kRolePenuGradient) != 0),
                int((roles & M::kRoleTopBands)    != 0) + int((roles & M::kRolePenuBands)    != 0));
    return changed;
}

int SurfaceEffectProfileManager::migrate_legacy_colorstitch(double sp_mm)
{
    int n = 0;
    for (auto& p : m_profiles)
        if (migrate_one_profile(p, sp_mm)) ++n;
    return n;
}

// NEOTKO_SANDWICH_TAG_START — s317 fase D: la receta del Sandwich Editor pasa a la paleta.
// Declaración y porqué en el header y en ColorStitch.hpp (ColorStitchLegacyMigration).
namespace {

// Mismo juego de claves que horneaba el "Save as profile" del Sandwich Editor (Tab.cpp,
// zone_colorstitch_snapshot, ya borrado). 🚨 Hace falta: un pase ColorStitch sintetizado de las
// claves legacy va con el kv VACÍO ("el motor lee el preset"). En modo pintor ese kv vacío cae al
// preset del objeto, y aquí lo acabamos de apagar: sin hornear, el perfil pintaría nada.
std::vector<std::string> sandwich_zone_colorstitch_keys(bool penu)
{
    const std::string gp = penu ? "interlayer_colormix_penu_" : "interlayer_colormix_";
    std::vector<std::string> keys;
    keys.push_back(penu ? "interlayer_colormix_pattern_penultimate" : "interlayer_colormix_pattern_top");
    for (const char* s : { "mode", "pct_a", "pct_b", "easing", "gamma",
                           "min_surface_lines", "overlap", "invert", "repetitions",
                           "band_count_a", "band_count_b", "band_count_c", "band_count_d",
                           "band_mm_a", "band_mm_b", "band_mm_c", "band_mm_d",
                           "gradient_span_mm",
                           "tool_a", "tool_b", "tool_c", "tool_d", "angle" })
        keys.push_back(gp + s);
    return keys;
}

// La receta que `cfg` aplicaría sola, como perfil autocontenido. false si no hay ninguna.
bool profile_from_sandwich_config(const DynamicPrintConfig& cfg, SurfaceEffectProfile& out)
{
    SurfacePassStack st[2] = { SurfacePassStack::resolve_for_zone(cfg, false),
                               SurfacePassStack::resolve_for_zone(cfg, true) };
    bool any = false;
    for (int z = 0; z < 2; ++z) {
        if (!st[z].enabled || !st[z].any_effect()) { st[z] = SurfacePassStack(); continue; }
        any = true;
        for (SurfacePass& pass : st[z].passes)
            if (pass.kind == SurfacePassKind::ColorStitch && pass.colorstitch.kv.empty()) {
                pass.colorstitch = SurfaceEffectProfileManager::snapshot_keys(cfg, sandwich_zone_colorstitch_keys(z == 1));
                pass.colorstitch.present = true;
            }
    }
    if (!any) return false;
    out.stack_top_json  = st[0].to_json();
    out.stack_penu_json = st[1].to_json();
    SurfaceEffectProfileManager::payload_from_stacks(st[0], st[1], out);
    return true;
}

// ¿Toca el objeto algo del Sandwich? Si no, hereda la receta del proyecto y no hay nada propio.
bool object_config_touches_sandwich(const DynamicPrintConfig& oc)
{
    static const char* const kPrefixes[] = { "interlayer_colormix_", "multipass_", "penultimate_multipass_",
                                             "pathblend_", "path_gradient_", "neotko_surface_passes_" };
    for (const std::string& k : oc.keys())
        for (const char* pre : kPrefixes)
            if (k.rfind(pre, 0) == 0) return true;
    return false;
}

} // namespace

int SurfaceEffectProfileManager::move_sandwich_editor_recipes(
    DynamicPrintConfig& project_cfg,
    const std::vector<std::pair<std::string, ModelConfig*>>& objects,
    double sp_mm)
{
    namespace M = ColorStitchLegacyMigration;

    // 1) Fase B en la config de cada objeto: se lee clave a clave (bbs_3mf, set_deserialize) y NO
    //    pasa por handle_legacy_composite. Antes de sacar recetas, para que salgan ya migradas.
    for (const auto& [name, mc] : objects) {
        if (!mc) continue;
        DynamicPrintConfig oc = mc->get();
        if (M::migrate_config(oc, sp_mm)) {
            mc->assign_config(std::move(oc));
            NEOTKO_LOG(PROFILE, "SANDWICH_MOVE object='" << name << "' legacy ColorStitch keys migrated");
        }
    }

    int sources = 0, created = 0;
    auto keep = [&](SurfaceEffectProfile&& p, const std::string& label) {
        migrate_one_profile(p, sp_mm);   // pases con kv propio autorados en el editor (mode 3, span<0)
        for (const auto& e : m_profiles)
            if (e.stack_top_json == p.stack_top_json && e.stack_penu_json == p.stack_penu_json
                && e.stack_bottom_json.empty()) {
                NEOTKO_LOG(PROFILE, "SANDWICH_MOVE '" << label << "' = existing id=" << e.id
                    << " name='" << e.name << "' (deduplicated)");
                return;
            }
        p.name = label;
        const int id = add(std::move(p));
        ++created;
        NEOTKO_LOG(PROFILE, "SANDWICH_MOVE '" << label << "' → new profile id=" << id);
    };

    // 2) Recetas, TODAS con la config del proyecto aún intacta (los objetos la heredan).
    {
        SurfaceEffectProfile p;
        if (profile_from_sandwich_config(project_cfg, p)) { ++sources; keep(std::move(p), "From Sandwich editor"); }
    }
    for (const auto& [name, mc] : objects) {
        if (!mc || !object_config_touches_sandwich(mc->get())) continue;
        DynamicPrintConfig merged = project_cfg;
        merged.apply(mc->get(), true);
        SurfaceEffectProfile p;
        if (profile_from_sandwich_config(merged, p)) {
            ++sources;
            keep(std::move(p), "From Sandwich editor (" + name + ")");
        }
    }

    // 3) Y se APAGA. 🚨 Sin esto resolve() la seguiría aplicando sola a lo no pintado.
    const bool proj_off = M::switch_off_sandwich(project_cfg);
    int obj_off = 0;
    for (const auto& [name, mc] : objects) {
        if (!mc) continue;
        DynamicPrintConfig oc = mc->get();
        if (M::switch_off_sandwich(oc)) { mc->assign_config(std::move(oc)); ++obj_off; }
    }
    NEOTKO_LOG(PROFILE, "SANDWICH_MOVE sources=" << sources << " created=" << created
        << " project_switched_off=" << proj_off << " objects_switched_off=" << obj_off);
    if (sources > 0) M::note_sandwich(sources, created);
    return sources;
}
// NEOTKO_SANDWICH_TAG_END — s317 fase D

std::string SurfaceEffectProfileManager::to_json() const
{
    nlohmann::json root;
    root["v"]       = 2; // NEOTKO_PROFILE_TAG — Fase 6: +stack_top/penu_json
    root["next_id"] = m_next_id;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : m_profiles) {
        nlohmann::json e;
        e["id"]              = p.id;
        e["name"]            = p.name;
        e["preview_argb"]    = p.preview_argb;
        e["colormix"]        = payload_to_json(p.colorstitch);
        e["pathblend"]       = payload_to_json(p.pathblend);
        e["multipass"]       = payload_to_json(p.multipass);
        e["stack_top_json"]  = p.stack_top_json;
        e["stack_penu_json"] = p.stack_penu_json;
        e["stack_bottom_json"] = p.stack_bottom_json; // NEOTKO_BOTTOM_TAG — Fase 0 (WIP)
        e["auto"]            = p.auto_generated;  // NEOTKO_COLORSTITCH_TAG — PR.3
        arr.push_back(std::move(e));
    }
    root["profiles"] = std::move(arr);
    return root.dump();
}

bool SurfaceEffectProfileManager::from_json(const std::string& text)
{
    if (text.empty()) return false;
    nlohmann::json root;
    try { root = nlohmann::json::parse(text); }
    catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "SurfaceEffectProfileManager::from_json parse error: " << e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("profiles") || !root["profiles"].is_array())
        return false;

    m_profiles.clear();
    m_next_id = 1;

    for (const auto& e : root["profiles"]) {
        if (!e.is_object()) continue;
        SurfaceEffectProfile p;
        if (e.contains("id")           && e["id"].is_number_integer())   p.id           = e["id"].get<int>();
        if (e.contains("name")         && e["name"].is_string())         p.name         = e["name"].get<std::string>();
        if (e.contains("preview_argb") && e["preview_argb"].is_number()) p.preview_argb = e["preview_argb"].get<uint32_t>();
        if (e.contains("colormix"))  p.colorstitch  = payload_from_json(e["colormix"]);
        if (e.contains("pathblend")) p.pathblend = payload_from_json(e["pathblend"]);
        if (e.contains("multipass")) p.multipass = payload_from_json(e["multipass"]);
        // NEOTKO_PROFILE_TAG — Fase 6 (v2): preview stacks. Absent in v1 → "".
        if (e.contains("stack_top_json")  && e["stack_top_json"].is_string())
            p.stack_top_json  = e["stack_top_json"].get<std::string>();
        if (e.contains("stack_penu_json") && e["stack_penu_json"].is_string())
            p.stack_penu_json = e["stack_penu_json"].get<std::string>();
        // NEOTKO_BOTTOM_TAG — Fase 0 (WIP): absent in older profiles → "" (no bottom sandwich).
        if (e.contains("stack_bottom_json") && e["stack_bottom_json"].is_string())
            p.stack_bottom_json = e["stack_bottom_json"].get<std::string>();
        // NEOTKO_COLORSTITCH_TAG — PR.3: absent (legacy) → false = saved.
        if (e.contains("auto") && e["auto"].is_boolean())
            p.auto_generated = e["auto"].get<bool>();
        // NEOTKO_COLORSTITCH_TAG — s112: backfill payload for profiles saved
        // BEFORE the fix (auto profiles only carried the visual stacks). Without
        // a payload the slicer's painter-mode gate skips them → painted .3mf
        // loaded as all-T0. Derive it from the stacks here so old files slice.
        if (!p.colorstitch.present && !p.pathblend.present &&
            (!p.stack_top_json.empty() || !p.stack_penu_json.empty())) {
            const SurfacePassStack st_top  = SurfacePassStack::from_json(p.stack_top_json);
            const SurfacePassStack st_penu = SurfacePassStack::from_json(p.stack_penu_json);
            payload_from_stacks(st_top, st_penu, p);
            NEOTKO_LOG(PROFILE, "from_json backfill id=" << p.id << " name='" << p.name
                << "' → cm=" << (p.colorstitch.present ? "yes" : "no")
                << " pb=" << (p.pathblend.present ? "yes" : "no"));
        }
        if (p.id <= 0) p.id = m_next_id;
        m_next_id = std::max(m_next_id, p.id + 1);
        m_profiles.push_back(std::move(p));
    }
    if (root.contains("next_id") && root["next_id"].is_number_integer())
        m_next_id = std::max(m_next_id, root["next_id"].get<int>());
    NEOTKO_LOG(PROFILE, "MGR from_json loaded " << m_profiles.size()
        << " profiles, next_id=" << m_next_id);
    return true;
}

} // namespace Slic3r
// NEOTKO_PROFILE_TAG_END
