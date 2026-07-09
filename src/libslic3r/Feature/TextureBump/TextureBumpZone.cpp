// NEOTKO_TEXTUREBUMP_TAG_START
#include "TextureBumpZone.hpp"

#include <nlohmann/json.hpp>
#include <boost/functional/hash.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

TextureBumpZoneManager& TextureBumpZoneManager::get()
{
    static TextureBumpZoneManager s_instance;
    return s_instance;
}

int TextureBumpZoneManager::add(TextureBumpZoneProfile zone)
{
    zone.id = m_next_id++;
    if (zone.name.empty())
        zone.name = "Zone " + std::to_string(zone.id);
    m_zones.push_back(std::move(zone));
    return m_zones.back().id;
}

bool TextureBumpZoneManager::remove(int id)
{
    for (auto it = m_zones.begin(); it != m_zones.end(); ++it)
        if (it->id == id) { m_zones.erase(it); return true; }
    return false;
}

const TextureBumpZoneProfile* TextureBumpZoneManager::find(int id) const
{
    for (const auto& z : m_zones)
        if (z.id == id) return &z;
    return nullptr;
}

TextureBumpZoneProfile* TextureBumpZoneManager::find_mut(int id)
{
    for (auto& z : m_zones)
        if (z.id == id) return &z;
    return nullptr;
}

void TextureBumpZoneManager::clear()
{
    m_zones.clear();
    m_next_id = 1;
}

uint64_t TextureBumpZoneManager::fingerprint_of_ids(const std::vector<int>& ids)
{
    std::size_t seed = 0;
    for (int id : ids) {
        const TextureBumpZoneProfile* z = TextureBumpZoneManager::get().find(id);
        if (!z) continue; // orphaned id (zone deleted) -- ignored, not an error
        boost::hash_combine(seed, id);
        boost::hash_combine(seed, std::hash<TextureBumpConfig>{}(z->config));
    }
    return static_cast<uint64_t>(seed);
}

// ----------------------------------------------------------------------------
// JSON round-trip -- same shape/versioning convention as
// SurfaceEffectProfileManager::to_json/from_json (SurfaceEffectProfile.cpp).
//
//   { "v": 1, "next_id": 3,
//     "zones": [
//       { "id": 1, "name": "Grip", "type": 1, "thickness": 200000, "point_distance": 300000,
//         "first_layer": false, "projection_mode": 0, "axis": 2, "scale": 20.0, "repeat_u": 1,
//         "max_angle_rad": 0.785, "blur_strength": 1.0, "image_path": "..." },
//       ... ] }
//
// `thickness`/`point_distance` are TextureBumpConfig's own scaled coord_t (integer, already the
// engine's native unit) -- stored verbatim so round-tripping never needs a unit conversion.
// ----------------------------------------------------------------------------

std::string TextureBumpZoneManager::to_json() const
{
    nlohmann::json root;
    root["v"]       = 1;
    root["next_id"] = m_next_id;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& z : m_zones) {
        nlohmann::json e;
        e["id"]              = z.id;
        e["name"]            = z.name;
        e["type"]            = static_cast<int>(z.config.type);
        e["thickness"]       = static_cast<int64_t>(z.config.thickness);
        e["point_distance"]  = static_cast<int64_t>(z.config.point_distance);
        e["first_layer"]     = z.config.first_layer;
        e["projection_mode"] = static_cast<int>(z.config.projection_mode);
        e["axis"]            = static_cast<int>(z.config.axis);
        e["scale"]           = z.config.scale;
        e["repeat_u"]        = z.config.repeat_u;
        e["max_angle_rad"]   = z.config.max_angle_rad;
        e["blur_strength"]   = z.config.blur_strength;
        e["image_path"]      = z.config.image_path;
        // NEOTKO_TEXTUREBUMP_TAG — Fase 4.2/4.3: this zone's own projection-plane transform,
        // independent of the object-level one (ModelObject::texture_bump_plane_transform,
        // Format/bbs_3mf.cpp). Omitted when Identity so zones untouched by the new plane controls
        // round-trip byte-identical to before this field existed.
        if (z.config.plane_transform.matrix() != Transform3d::Identity().matrix()) {
            nlohmann::json t = nlohmann::json::array();
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    t.push_back(z.config.plane_transform.matrix()(r, c));
            e["plane_transform"] = std::move(t);
        }
        arr.push_back(std::move(e));
    }
    root["zones"] = std::move(arr);
    return root.dump();
}

bool TextureBumpZoneManager::from_json(const std::string& text)
{
    if (text.empty()) return false;
    nlohmann::json root;
    try { root = nlohmann::json::parse(text); }
    catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "TextureBumpZoneManager::from_json parse error: " << e.what();
        return false;
    }
    if (!root.is_object() || !root.contains("zones") || !root["zones"].is_array())
        return false;

    m_zones.clear();
    m_next_id = 1;

    for (const auto& e : root["zones"]) {
        if (!e.is_object()) continue;
        TextureBumpZoneProfile z;
        if (e.contains("id")   && e["id"].is_number_integer())   z.id   = e["id"].get<int>();
        if (e.contains("name") && e["name"].is_string())         z.name = e["name"].get<std::string>();
        if (e.contains("type")            && e["type"].is_number_integer())
            z.config.type = static_cast<TextureBumpType>(e["type"].get<int>());
        if (e.contains("thickness")       && e["thickness"].is_number_integer())
            z.config.thickness = static_cast<coord_t>(e["thickness"].get<int64_t>());
        if (e.contains("point_distance")  && e["point_distance"].is_number_integer())
            z.config.point_distance = static_cast<coord_t>(e["point_distance"].get<int64_t>());
        if (e.contains("first_layer")     && e["first_layer"].is_boolean())
            z.config.first_layer = e["first_layer"].get<bool>();
        if (e.contains("projection_mode") && e["projection_mode"].is_number_integer())
            z.config.projection_mode = static_cast<TextureProjectionMode>(e["projection_mode"].get<int>());
        if (e.contains("axis")            && e["axis"].is_number_integer())
            z.config.axis = static_cast<TextureProjectionAxis>(e["axis"].get<int>());
        if (e.contains("scale")           && e["scale"].is_number())
            z.config.scale = e["scale"].get<double>();
        if (e.contains("repeat_u")        && e["repeat_u"].is_number_integer())
            z.config.repeat_u = e["repeat_u"].get<int>();
        if (e.contains("max_angle_rad")   && e["max_angle_rad"].is_number())
            z.config.max_angle_rad = e["max_angle_rad"].get<double>();
        if (e.contains("blur_strength")   && e["blur_strength"].is_number())
            z.config.blur_strength = e["blur_strength"].get<double>();
        if (e.contains("image_path")      && e["image_path"].is_string())
            z.config.image_path = e["image_path"].get<std::string>();
        if (e.contains("plane_transform") && e["plane_transform"].is_array() && e["plane_transform"].size() == 16) {
            int k = 0;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    z.config.plane_transform.matrix()(r, c) = e["plane_transform"][k++].get<double>();
        }

        if (z.id <= 0) z.id = m_next_id;
        m_next_id = std::max(m_next_id, z.id + 1);
        m_zones.push_back(std::move(z));
    }
    if (root.contains("next_id") && root["next_id"].is_number_integer())
        m_next_id = std::max(m_next_id, root["next_id"].get<int>());
    return true;
}

} // namespace Slic3r
// NEOTKO_TEXTUREBUMP_TAG_END
