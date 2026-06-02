// NEOTKO_PROFILE_TAG_START
#include "SurfaceEffectProfile.hpp"
#include "Config.hpp"
#include "PrintConfig.hpp"
#include "SurfaceColorMix.hpp" // NEOTKO_PROFILE_TAG — NeoDebug PROFILE channel
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
        << " colormix=" << (p.colormix.present  ? "yes" : "no")
        << " pathblend=" << (p.pathblend.present ? "yes" : "no")
        << " multipass=" << (p.multipass.present ? "yes" : "no")
        << " cm_kv=" << p.colormix.kv.size()
        << " mp_kv=" << p.multipass.kv.size()
        << " pb_kv=" << p.pathblend.kv.size()
        << " total=" << m_profiles.size());
    return p.id;
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

void SurfaceEffectProfileManager::restore_keys(DynamicPrintConfig& cfg,
                                               const SurfaceEffectPayload& payload)
{
    if (!payload.present) return;
    for (const auto& [k, v] : payload.kv) {
        if (cfg.has(k))
            cfg.set_deserialize_strict(k, v);
    }
}

const std::vector<std::string>& SurfaceEffectProfileManager::colormix_keys()
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
        e["colormix"]        = payload_to_json(p.colormix);
        e["pathblend"]       = payload_to_json(p.pathblend);
        e["multipass"]       = payload_to_json(p.multipass);
        e["stack_top_json"]  = p.stack_top_json;
        e["stack_penu_json"] = p.stack_penu_json;
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
        if (e.contains("colormix"))  p.colormix  = payload_from_json(e["colormix"]);
        if (e.contains("pathblend")) p.pathblend = payload_from_json(e["pathblend"]);
        if (e.contains("multipass")) p.multipass = payload_from_json(e["multipass"]);
        // NEOTKO_PROFILE_TAG — Fase 6 (v2): preview stacks. Absent in v1 → "".
        if (e.contains("stack_top_json")  && e["stack_top_json"].is_string())
            p.stack_top_json  = e["stack_top_json"].get<std::string>();
        if (e.contains("stack_penu_json") && e["stack_penu_json"].is_string())
            p.stack_penu_json = e["stack_penu_json"].get<std::string>();
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
