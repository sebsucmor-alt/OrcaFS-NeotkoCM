#ifndef slic3r_PhotoMode_hpp_
#define slic3r_PhotoMode_hpp_

// NEOTKO_PHOTOMODE_TAG s242 — "Photo Mode": a studio-photo presentation mode for the Prepare tab.
//
// Purpose (docs/FUTURE/PHOTO_MODE_PLAN.md): send a client a picture of the design without leaving
// the slicer. The day-to-day case is a colour change - "here, this is how it looks in red and
// grey" - where firing up a real renderer breaks the workflow.
//
// This header holds nothing but state. The rendering it drives already exists: with
// "neotko_libre_mode" on, the Prepare tab draws its objects through
// GCodeViewer::render_volumes_lit() / shells_lit (SSAO + contact shadows + a real 2048^2
// directional shadow map, s229). Photo Mode does not add a renderer - it makes that renderer's
// light movable, cleans the scene around it, and gets the frame out to a PNG.
//
// *** DESIGN PROPERTY THAT MUST NOT BE BROKEN ***
// While `active` is false, every consumer must behave EXACTLY as it did before this file existed.
// Every default below reproduces the constant it replaces, so "Photo Mode off" is bit-identical to
// the previous build. Same contract ShadingTuning (GLShader.hpp) established for the debug panel;
// it is what makes this feature reviewable phase by phase and abortable at any point.
//
// Unlike ShadingTuning, this state IS persisted (AppConfig, "photo_*" keys) - it is a user
// feature, not a debug panel.

#include <array>
#include <string>
#include <vector>

#include "libslic3r/Point.hpp"

namespace Slic3r {

// The three lighting constants shells_lit.vs has always used, kept here as the single source of
// truth now that they are uniforms. Photo Mode off => these exact values go to the shader.
//
// NOTE these are VIEW-space vectors: the shading this fork inherited from Slic3r pins the light to
// the observer (see the comment block in shells_lit.vs). Photo Mode moves the light into WORLD
// space instead, which is what a user expects - orbit the camera and the light stays put in the
// room. The switch happens in C++, not in the shader; see photo_light_dirs() in the .cpp.
inline constexpr float LIGHT_TOP_DIR_DEFAULT[3]   = { -0.4574957f, 0.4574957f, 0.7624929f };
inline constexpr float LIGHT_FRONT_DIR_DEFAULT[3] = {  0.6985074f, 0.1397015f, 0.6985074f };

// INTENSITY_CORRECTION (0.6) already folded in, exactly like ShadingTuning does, so that flipping
// Photo Mode on with untouched defaults changes nothing about the direct lighting.
inline constexpr float LIGHT_TOP_DIFFUSE_DEFAULT   = 0.48f;   // 0.8   * 0.6
inline constexpr float LIGHT_TOP_SPECULAR_DEFAULT  = 0.075f;  // 0.125 * 0.6
inline constexpr float LIGHT_TOP_SHININESS_DEFAULT = 20.0f;
inline constexpr float LIGHT_FRONT_DIFFUSE_DEFAULT = 0.18f;   // 0.3   * 0.6
inline constexpr float AMBIENT_GROUND_DEFAULT      = 0.18f;
inline constexpr float AMBIENT_SKY_DEFAULT         = 0.32f;
inline constexpr float FRESNEL_POWER_DEFAULT       = 5.0f;
inline constexpr float FRESNEL_STRENGTH_DEFAULT    = 0.06f;

// A light aimed in WORLD space. Stored as azimuth/elevation rather than a vector because that is
// what the UI edits (a draggable sphere widget) and what a user can write down and reproduce -
// three free-floating XYZ sliders are unusable for aiming a light.
//
// Convention, matching the rest of the project (bed = XY plane, Z up):
//   azimuth   - degrees CCW around +Z, measured from +X.
//   elevation - degrees above the bed plane. +90 = straight overhead.
// The resulting vector points FROM the surface TOWARD the light, i.e. it is the vector that goes
// into dot(normal, L) - same sense as LIGHT_TOP_DIR has always had.
struct PhotoLight
{
    bool  enabled       = true;
    float azimuth_deg   = 135.0f;
    float elevation_deg = 49.7f;
    // Diffuse contribution. Not a separate "intensity" multiplier on top of a base value: this IS
    // the value the shader multiplies N.L by, so the defaults below are literally the old
    // #defines and there is no hidden factor to reason about.
    float intensity     = LIGHT_TOP_DIFFUSE_DEFAULT;
    std::array<float, 3> tint = { 1.0f, 1.0f, 1.0f };

    // Unit vector in world space, surface -> light.
    Vec3d dir_world() const;
    // Inverse of dir_world(), for seeding az/el from a legacy constant vector.
    void  set_from_dir_world(const Vec3d& dir);
};

// The "set" the objects sit in.
struct PhotoStage
{
    enum class Kind {
        Bed,        // untouched: the real print bed, grid, logo, plate icons. The default.
        Lightbox,   // cyclorama: floor + curved back wall, no bed furniture.
        Backdrop    // floor only, no wall. For top-down shots.
    };

    Kind  kind = Kind::Bed;
    std::array<float, 3> base_color = { 0.92f, 0.92f, 0.92f };
    // How far the cyclorama floor extends past the bed, as a multiple of the bed's size.
    float extent_factor = 3.0f;
    // Fillet radius between floor and back wall, in mm. Large enough that the seam never reads as
    // a hard line, which is the whole point of a cyclorama.
    float wall_radius_mm = 100.0f;

    // --- reserved for pass 2 (texture library). Declared so persistence does not need a format
    // change later; nothing reads them yet.
    int   texture_slot     = -1;    // -1 = flat colour
    float tex_scale_mm     = 200.0f;
    float tex_rotation_deg = 0.0f;
    float roughness        = 0.6f;  // floor gloss, drives the planar reflection in pass 2
};

// NEOTKO_PHOTOMODE_TAG s242 (F3) — how a filament slot is shaded.
//
// Stored per SLOT (t0, t1, ...), not per filament preset: swap the filament in slot 2 and the slot
// keeps its look, which is what a user changing colours all day actually wants. It also keeps this
// out of PrintConfig/Preset/Tab entirely — a material is a presentation choice, not a print setting.

// The roughness that must map onto the original hardcoded shininess (20.0). Anchoring the
// conversion to this value is what makes the Plastic preset provably "no change" rather than
// "close enough" — see photo_material_uniforms_for().
inline constexpr float PHOTO_DEFAULT_ROUGHNESS = 0.45f;

struct PhotoMaterial
{
    enum class Preset { Plastic = 0, Glossy, Matte, Rubber, Metal, Silk, Count };

    Preset preset = Preset::Plastic;
    // Perceptual 0..1. Converted to a Phong exponent on the way to the shader — the shader keeps
    // speaking "shininess" so that Photo Mode off can send the literal old constant (20.0).
    float roughness  = 0.45f;
    // 0 = dielectric (white highlight, full diffuse), 1 = metal (highlight tinted by the base
    // colour, diffuse gone). Metals are essentially mirrors, so this only reads as metal once the
    // environment (F5) is on — without it, a metal preset looks like shiny grey plastic.
    float metallic   = 0.0f;
    // Multiplies the key light's specular strength. 1.0 == exactly the pre-Photo-Mode look.
    float spec_scale = 1.0f;
};
const char* photo_material_preset_name(PhotoMaterial::Preset p);
// Fills roughness/metallic/spec_scale from the preset. Custom leaves them alone.
void photo_material_apply_preset(PhotoMaterial& m, PhotoMaterial::Preset p);

// What 3DScene.cpp sends per volume. A tiny struct rather than three out-params so the
// "Photo Mode off => the original constants" decision lives in exactly one function.
struct PhotoMaterialUniforms
{
    float metallic   = 0.0f;
    float shininess  = LIGHT_TOP_SHININESS_DEFAULT;  // 20.0, the old #define
    float spec_scale = 1.0f;
};
// extruder_id is 1-based, as GLVolume stores it; -1/0 (no extruder) falls back to the defaults.
PhotoMaterialUniforms photo_material_uniforms_for(int extruder_id);

// Pushes the three material uniforms onto `shader` for one filament slot. Declared here (taking an
// opaque shader pointer) so both callers — the per-volume default in GLVolumeCollection::render()
// and the per-segment override inside GLVolume::simple_render() — go through the same code.
class GLShaderProgram;
void photo_set_material_uniforms(GLShaderProgram* shader, int extruder_id);

// Canned three-light rigs. The combo that sits at the top of the panel and does 90% of the work:
// the real flow is "pick Studio, shoot", not "tune nine sliders because a client asked for grey".
//
// Neutral is first and is the default *because it reproduces the viewport exactly* (key + fill,
// no rim, the two directions this fork has always used). That keeps one promise worth keeping:
// entering Photo Mode changes the scene around the object, not the object's shading. Everything
// after Neutral is a deliberate departure the user asked for.
enum class PhotoLightPreset {
    Neutral = 0,
    Studio3Point,
    SoftboxTop,
    RimBacklight,
    FlatCatalog,
    Dramatic,
    Count
};
const char* photo_light_preset_name(PhotoLightPreset p);

struct PhotoModeState
{
    bool active = false;

    PhotoLight key;    // the only one that casts the shadow map
    PhotoLight fill;
    PhotoLight rim;

    // Ambient hemisphere + rim/fresnel. Defaults == the old shells_lit.vs #defines.
    float ambient_ground   = AMBIENT_GROUND_DEFAULT;
    float ambient_sky      = AMBIENT_SKY_DEFAULT;
    float fresnel_power    = FRESNEL_POWER_DEFAULT;
    float fresnel_strength = FRESNEL_STRENGTH_DEFAULT;
    // Key-only specular. Kept out of PhotoLight because only the key has one today, and moving
    // specular per-light before it is per-material would be inventing a knob nobody asked for.
    float key_specular  = LIGHT_TOP_SPECULAR_DEFAULT;
    // NOTE there is deliberately no key_shininess here. Shininess is a property of the SURFACE,
    // not of the light — it moved to PhotoMaterial::roughness (F3), which is per slot. Keeping a
    // global one too would mean two knobs fighting over one uniform, and the loser would look
    // like a bug.

    PhotoStage stage;

    // NEOTKO_PHOTOMODE_TAG s242 (F3) — indexed by (extruder_id - 1). Grown on demand rather than
    // sized from the filament count: the count changes while the app runs, and a material the user
    // set should survive them adding and removing a filament.
    std::vector<PhotoMaterial> materials;
    PhotoMaterial& material(int slot);   // slot is 0-based; grows the vector as needed

    // --- environment (F5). Off by default: with it on, "Photo Mode untouched" would no longer
    // reproduce the viewport, which is the one promise this whole feature keeps.
    bool  env_enabled   = false;
    float env_intensity = 1.0f;
    // Rotates the virtual room around Z without moving the lights. The knob that makes a metal
    // look right, because what sells a metal is what it reflects, not how it is lit.
    float env_rotation_deg = 0.0f;

    // NEOTKO_PHOTOMODE_TAG s242 (F2.5) — quality tier.
    //
    // Exists because the planar floor reflection (F6a) draws the whole scene a SECOND time. That
    // is not something to switch on silently, and it is not something to leave off either — so it
    // gets a dial, and the shadow map resolution rides along on the same dial since both are
    // "spend more per frame for a better picture".
    enum class Quality { Normal = 0, High, Ultra, Count };
    Quality quality = Quality::Normal;

    // Mirror the objects in the floor. Gated by quality because of the doubled draw cost — see
    // photo_reflection_enabled().
    bool  reflection_enabled = false;
    // 0 = matte floor (no reflection), 1 = mirror. Not the same as stage.roughness: this is how
    // MUCH is reflected, roughness is how sharp — and only the first is implemented in this pass.
    float reflection_strength = 0.35f;

    // Screenshot helper: hide every piece of UI for a few seconds so the user can grab the frame
    // with the OS screenshot tool. The stopgap while the off-screen export is parked.
    float screenshot_seconds = 5.0f;
    // Wall-clock deadline, seconds since app start. NOT persisted — it is a countdown, not a
    // setting, and restoring one from a config file would hide the UI on launch.
    double screenshot_hide_until = 0.0;

    // --- export
    int  export_w  = 2560;
    int  export_h  = 1440;
    // Render at export_ss x the requested size and box-filter down. This single knob is most of
    // what reads as "quality": what dates the current render is aliasing on the shadow map and
    // the SSAO, not its physics.
    int  export_ss = 2;
    // Off by default on purpose: with the white cyclorama behind it the shot is already
    // mail-ready, and a default-on alpha produces the classic "I pasted it and it went black".
    // The toggle is for crops and shop listings.
    bool transparent_bg = false;

    PhotoLightPreset last_preset = PhotoLightPreset::Neutral;

    // Seeds the three lights from PhotoLightPreset::Neutral, so a default-constructed state is
    // already "the viewport exactly". The member initialisers above cannot express that on their
    // own (fill and rim need different aim than key), and duplicating the numbers here and in the
    // preset table is how they drift apart.
    PhotoModeState();

    void apply_light_preset(PhotoLightPreset p);
};

// Process-wide, GUI thread only - same lifetime assumption as shading_tuning() and as the ImGui
// panel that edits it.
PhotoModeState& photo_mode();

// True only where Photo Mode is allowed to exist at all. Photo Mode rides entirely on the
// shells_lit pipeline, which only draws the Prepare objects while LibreMode is on; rather than
// port every shader change to gouraud for a configuration nobody shoots photos in, the feature is
// simply gated. Callers use this to hide the plate icon and the context-menu item too, so the
// button never appears where pressing it would do nothing.
bool photo_mode_available();

// Convenience for the render paths: is the plate furniture (bed model, grid, logo, numbers,
// exclude area) supposed to be hidden right now?
bool photo_mode_hides_bed();

void photo_mode_load_from_app_config();
void photo_mode_save_to_app_config();

// NEOTKO_PHOTOMODE_TAG s242 — named user presets ("the look I use for client shots").
//
// Stored in AppConfig, i.e. per USER and per machine — deliberately NOT in the 3mf. A lighting
// setup is a photographer's preference, not a property of the model: it should follow the person
// across every project, and it has no business bloating a file that gets shared or printed.
//
// One flat string per preset (see photo_serialize) rather than a key per field: presets are
// opaque blobs to everything except this file, so adding a field later cannot corrupt an old one
// — a missing key simply keeps its default.
struct PhotoUserPreset
{
    std::string name;
    std::string data;
};
const std::vector<PhotoUserPreset>& photo_presets();
void photo_preset_save(const std::string& name);   // overwrites a same-named preset
void photo_preset_apply(size_t index);
void photo_preset_delete(size_t index);

// The whole state as one string, and back. Also what the plain "remember what I was doing"
// persistence uses, so there is exactly ONE list of fields to keep in step.
std::string photo_serialize();
void        photo_deserialize(const std::string& blob);

// NEOTKO_PHOTOMODE_TAG s242 (F2.5) ------------------------------------------------------------

const char* photo_quality_name(PhotoModeState::Quality q);
// Shadow map edge, in texels. 2048 (the pre-s242 constant) / 4096 / 8192.
//
// This matters more than it used to: the cyclorama made the light's ortho frustum grow to cover
// where the shadow LANDS, not just where the casters are, so the same 2048 texels are now spread
// over a much larger area than before F4. Raising the resolution is not a luxury here, it is
// paying back what the cyclorama cost.
int   photo_shadow_map_res();
// Multiplies the PCF tap spacing. Resolution alone gives a thinner but equally hard edge; widening
// the kernel is what makes it read as a soft studio shadow rather than a stencil.
float photo_shadow_pcf_spread();

// Reflection only runs above Normal — it redraws the entire scene mirrored.
bool  photo_reflection_enabled();

// True while the screenshot countdown is running: every overlay, the panel and the plate icons
// must draw nothing.
bool  photo_ui_hidden();
// Starts the countdown. Call from the button.
void  photo_begin_screenshot();

} // namespace Slic3r

#endif // slic3r_PhotoMode_hpp_
