// NEOTKO_PROFILE_TAG_START
// SurfaceEffectProfile — snapshot de un Surface Color Mixer configurado.
// Fase A (Opción 4 — 3D Painter): solo se rellena el payload ColorStitch.
// La estructura ya reserva slots para PathBlend (Fase G) y MultiPass (Fase F)
// para que el manager / serializer / gizmo no se reescriban más tarde.
#ifndef slic3r_SurfaceEffectProfile_hpp_
#define slic3r_SurfaceEffectProfile_hpp_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
struct SurfacePassStack;   // NEOTKO_PROFILE_TAG — payload_from_stacks (ColorStitch.hpp)

enum class SurfaceEffectKind : uint8_t {
    ColorStitch  = 1,
    PathBlend = 2,
    MultiPass = 3,
};

// Each payload stores serialized config-key -> serialized-value strings
// (via DynamicPrintConfig::opt_serialize / set_deserialize_strict).
// `present` distinguishes "empty default" from "user-saved snapshot".
struct SurfaceEffectPayload {
    bool                               present = false;
    std::map<std::string, std::string> kv;
};

struct SurfaceEffectProfile {
    int                  id = 0;            // assigned by manager, 1-based
    std::string          name;
    uint32_t             preview_argb = 0;  // swatch hint for the 3D painter

    // NEOTKO_COLORSTITCH_TAG — PR.3 two-tier palette. `auto_generated` marks a
    // profile materialized on-the-fly when a gizmo swatch is painted (a working
    // paint colour), as opposed to a deliberately SAVED palette entry. Auto
    // profiles are hidden from the saved-palette list and garbage-collected when
    // no slot references them. Persisted so the distinction survives a reload
    // (a painted .3mf still needs the recipe; it just stays "auto"). Default
    // false → legacy/loaded profiles are treated as saved (never GC'd).
    bool                 auto_generated = false;

    SurfaceEffectPayload colorstitch;          // Fase A: lo único que se rellena
    SurfaceEffectPayload pathblend;         // Fase G — placeholder
    SurfaceEffectPayload multipass;         // Fase F — placeholder

    // NEOTKO_PROFILE_TAG — Fase 6 (aditivo): el sandwich completo resuelto como
    // 2 blobs de SurfacePassStack (= SurfacePassStack::to_json() por zona, las
    // keys neotko_surface_passes_top/penu). NO los consume el motor de slice
    // (sigue leyendo los 3 payloads de arriba) — son SOLO para el preview
    // mini-sandwich del 3D Painter y el round-trip de UX. La migración real del
    // motor a estos stacks es un debug posterior. Vacío = sin sandwich resuelto.
    std::string          stack_top_json;
    std::string          stack_penu_json;
    // NEOTKO_BOTTOM_TAG — Fase 0 (WIP): bottom-surface zone stack, authored in the
    // "Bottom WIP" zone of the Sandwich Painter Pro tray. Like the top/penu blobs it
    // is NOT yet consumed by the slice engine — Fase 0 only instruments (logs whether
    // a bottom stack is present for a surface/role). The engine wiring (new bottom
    // role + sublayer compile, plus the wipe-tower-sensitive layer-0 path) lands in
    // later phases. Empty = no bottom sandwich authored.
    std::string          stack_bottom_json;
};

class SurfaceEffectProfileManager
{
public:
    static SurfaceEffectProfileManager& get();

    // Mutators — `add` assigns an id, returns it.
    int  add(SurfaceEffectProfile profile);
    // NEOTKO_PROFILE_TAG — s238: inserta CONSERVANDO el id que trae el perfil (a
    // diferencia de `add`, que asigna uno nuevo). Lo usa la recuperación de slots
    // huérfanos al cargar un 3mf cuya pintura referencia ids sin receta. No pisa un
    // id existente; devuelve false si ya estaba o si el id no es válido.
    bool adopt(SurfaceEffectProfile profile);
    bool remove(int id);
    bool rename(int id, const std::string& new_name);

    // Accessors
    const SurfaceEffectProfile*               find(int id) const;
    SurfaceEffectProfile*                     find_mut(int id);            // NEOTKO_PROFILE_TAG — edit support
    const std::vector<SurfaceEffectProfile>&  list() const { return m_profiles; }
    size_t                                    size() const { return m_profiles.size(); }
    void                                      clear();

    // Snapshot helpers — Fase A only fills ColorStitch; the others stay `present=false`.
    // The caller picks which keys to snapshot (so we keep the manager engine-agnostic).
    static SurfaceEffectPayload snapshot_keys(const DynamicPrintConfig& cfg,
                                              const std::vector<std::string>& keys);

    // NEOTKO_COLORSTITCH_TAG — s112 fix (PAINTER_SLICE_PAYLOAD_GAP.md). Derive the
    // engine-readable PROFILE payload from a resolved sandwich (top/penu stacks)
    // by LIFTING each ColorStitch/PathBlend pass's already-canonical `kv` up to the
    // profile level. The painter's auto profiles only stored the visual stacks
    // (stack_*_json); the slicer's painter-mode gate (ColorStitch.cpp:1094)
    // requires `colorstitch.present`. This bridges them — mirror of what Tab.cpp's
    // "Save profile" does, but sourced from the stacks instead of the live config.
    // Solid passes are intentionally ignored (they ride the stack_json/MultiPass
    // path in Fill.cpp, not the colorstitch payload). Sets `interlayer_colormix_*`
    // enable + surface (0=Both/1=Top/2=Penu) / `multipass_path_gradient` +
    // `pathblend_surface` based on which zones carry each effect.
    static void payload_from_stacks(const SurfacePassStack& top,
                                    const SurfacePassStack& penu,
                                    SurfaceEffectProfile& p);
    static void                  restore_keys (DynamicPrintConfig& cfg,
                                              const SurfaceEffectPayload& payload);

    // Canonical ColorStitch key list (top + _penu_ variants + global flags).
    // Used by Tab.cpp "Save as profile" and future 3mf serializer.
    static const std::vector<std::string>& colorstitch_keys();

    // NEOTKO_PROFILE_TAG — Fase F: canonical MultiPass key list (top +
    // penultimate_ mirror). Excludes `multipass_prime_volume` (print-wide,
    // not per-profile).
    static const std::vector<std::string>& multipass_keys();

    // NEOTKO_PROFILE_TAG — Fase G: canonical PathBlend key list. Includes
    // `multipass_path_gradient` as the master enable flag (per-region in the
    // preset; in a profile it carries the "use PathBlend for this paint"
    // intent).
    static const std::vector<std::string>& pathblend_keys();

    // JSON round-trip — placeholder for Fase C (3mf integration). Empty for now.
    std::string to_json() const;
    bool        from_json(const std::string& text);

private:
    SurfaceEffectProfileManager() = default;
    std::vector<SurfaceEffectProfile> m_profiles;
    int                               m_next_id = 1;
};

} // namespace Slic3r

#endif // slic3r_SurfaceEffectProfile_hpp_
// NEOTKO_PROFILE_TAG_END
