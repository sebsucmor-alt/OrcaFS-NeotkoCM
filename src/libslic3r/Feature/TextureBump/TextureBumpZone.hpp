#ifndef libslic3r_TextureBumpZone_hpp_
#define libslic3r_TextureBumpZone_hpp_

#include <cstdint>
#include <string>
#include <vector>

#include "libslic3r/PerimeterGenerator.hpp" // TextureBumpConfig + its operator==/std::hash

// NEOTKO_TEXTUREBUMP_TAG_START — Fase 3 (paint a zone, give it its own Texture Bump). Dedicated,
// deliberately minimal registry for painted-zone configs: same "id/name, edited from a painter"
// shape as SurfaceEffectProfileManager, but NOT that manager -- Texture Bump zones describe wall
// geometry (image/scale/thickness/...), a different domain from ColorMix/PathBlend/MultiPass
// surface-pass profiles, and reuse TextureBumpConfig directly as the payload (it already has
// operator==/std::hash; no generic kv/JSON snapshot layer is needed here).
// See docs/ATTRIBUTION_TEXTURE_BUMP.md.

namespace Slic3r {

struct TextureBumpZoneProfile
{
    int               id = 0;    // assigned by the manager, 1-based
    std::string       name;
    TextureBumpConfig config;    // the resolved config this zone applies where painted
};

class TextureBumpZoneManager
{
public:
    static TextureBumpZoneManager& get();

    // Mutators -- `add` assigns an id, returns it.
    int  add(TextureBumpZoneProfile zone);
    bool remove(int id);

    // Accessors
    const TextureBumpZoneProfile*              find(int id) const;
    TextureBumpZoneProfile*                    find_mut(int id);
    const std::vector<TextureBumpZoneProfile>& list() const { return m_zones; }
    size_t                                     size() const { return m_zones.size(); }
    void                                       clear();

    // Content fingerprint of the zones referenced by `ids` (hash-combine of each resolved
    // TextureBumpConfig's own std::hash). Used by Model.cpp's
    // model_texture_bump_paint_data_changed() to detect "same paint, edited zone content" --
    // mirrors ModelVolume::colormix_profiles_fingerprint's role for ColorMix profiles. Ids that no
    // longer resolve (zone deleted) are skipped, not treated as an error.
    static uint64_t fingerprint_of_ids(const std::vector<int>& ids);

    // Project-level JSON round-trip (same shape as SurfaceEffectProfileManager::to_json/from_json,
    // see Format/bbs_3mf.cpp for the base64-wrapped metadata entry this feeds).
    std::string to_json() const;
    bool        from_json(const std::string& text);

private:
    TextureBumpZoneManager() = default;
    std::vector<TextureBumpZoneProfile> m_zones;
    int                                 m_next_id = 1;
};

} // namespace Slic3r

#endif // libslic3r_TextureBumpZone_hpp_
// NEOTKO_TEXTUREBUMP_TAG_END
