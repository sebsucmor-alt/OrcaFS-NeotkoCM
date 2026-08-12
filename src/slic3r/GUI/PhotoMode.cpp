// NEOTKO_PHOTOMODE_TAG s242 — see PhotoMode.hpp for what this is and, more importantly, for the
// "off == bit-identical to the previous build" contract every default here exists to honour.

#include "PhotoMode.hpp"

#include "GUI_App.hpp"
#include "ImGuiWrapper.hpp"
#include "imgui/imgui_internal.h"
#include "GLShader.hpp"
#include "libslic3r/AppConfig.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

namespace Slic3r {

// ---------------------------------------------------------------------------------------------
// PhotoLight
// ---------------------------------------------------------------------------------------------

Vec3d PhotoLight::dir_world() const
{
    const double az = double(azimuth_deg)   * M_PI / 180.0;
    const double el = double(elevation_deg) * M_PI / 180.0;
    const double c  = std::cos(el);
    return Vec3d(c * std::cos(az), c * std::sin(az), std::sin(el)).normalized();
}

void PhotoLight::set_from_dir_world(const Vec3d& dir)
{
    const Vec3d d = dir.normalized();
    elevation_deg = float(std::asin(std::clamp(d.z(), -1.0, 1.0)) * 180.0 / M_PI);
    // atan2(0,0) is 0 on every platform we ship, but a light aimed exactly along Z has no
    // meaningful azimuth anyway - leaving whatever was there beats writing a fake 0.
    if (std::abs(d.x()) > 1e-9 || std::abs(d.y()) > 1e-9)
        azimuth_deg = float(std::atan2(d.y(), d.x()) * 180.0 / M_PI);
}

// ---------------------------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------------------------

const char* photo_light_preset_name(PhotoLightPreset p)
{
    switch (p) {
    case PhotoLightPreset::Neutral:      return "Neutral (as viewport)";
    case PhotoLightPreset::Studio3Point: return "Studio 3-point";
    case PhotoLightPreset::SoftboxTop:   return "Softbox top";
    case PhotoLightPreset::RimBacklight: return "Rim / backlight";
    case PhotoLightPreset::FlatCatalog:  return "Flat catalog";
    case PhotoLightPreset::Dramatic:     return "Dramatic";
    default:                             return "?";
    }
}

PhotoModeState::PhotoModeState()
{
    apply_light_preset(PhotoLightPreset::Neutral);
}

void PhotoModeState::apply_light_preset(PhotoLightPreset p)
{
    last_preset = p;

    // Every branch writes all three lights and the ambient pair. Partial presets that only touch
    // what they "care about" leave the rig in a half-state that depends on what was selected
    // before, which makes the combo non-reproducible.
    auto set_light = [](PhotoLight& l, bool on, float az, float el, float intensity) {
        l.enabled       = on;
        l.azimuth_deg   = az;
        l.elevation_deg = el;
        l.intensity     = intensity;
        l.tint          = { 1.0f, 1.0f, 1.0f };
    };

    key_specular  = LIGHT_TOP_SPECULAR_DEFAULT;

    switch (p) {
    default:
    case PhotoLightPreset::Neutral:
        // Exactly the two directions shells_lit.vs has always used, converted to world az/el:
        // LIGHT_TOP_DIR   -> az 135.0, el 49.7
        // LIGHT_FRONT_DIR -> az  11.3, el 44.3
        // Rim off, because there has never been a third light. This is what makes "Photo Mode on,
        // untouched" look like the viewport it was launched from.
        set_light(key,  true,  135.0f, 49.7f, LIGHT_TOP_DIFFUSE_DEFAULT);
        set_light(fill, true,   11.3f, 44.3f, LIGHT_FRONT_DIFFUSE_DEFAULT);
        set_light(rim,  false, 315.0f, 25.0f, 0.0f);
        ambient_ground = AMBIENT_GROUND_DEFAULT;
        ambient_sky    = AMBIENT_SKY_DEFAULT;
        break;

    case PhotoLightPreset::Studio3Point:
        // The classic rig: key high and to one side, softer fill from the opposite side to open
        // the shadows, rim low and behind to separate the object from the backdrop.
        set_light(key,  true,  135.0f, 45.0f, 0.55f);
        set_light(fill, true,  -30.0f, 25.0f, 0.22f);
        set_light(rim,  true,  -80.0f, 15.0f, 0.30f);
        ambient_ground = 0.16f;
        ambient_sky    = 0.30f;
        break;

    case PhotoLightPreset::SoftboxTop:
        // One big source overhead, ambient doing most of the fill. Near-vertical shadow, very
        // little modelling - the "product on white" look.
        set_light(key,  true,   95.0f, 78.0f, 0.50f);
        set_light(fill, true,   20.0f, 30.0f, 0.14f);
        set_light(rim,  false, 315.0f, 20.0f, 0.0f);
        ambient_ground = 0.26f;
        ambient_sky    = 0.42f;
        break;

    case PhotoLightPreset::RimBacklight:
        // Key pulled down and weak, strong rim from behind. Reads as a silhouette with a bright
        // edge; good for showing profile and surface finish.
        set_light(key,  true,  120.0f, 30.0f, 0.26f);
        set_light(fill, true,   40.0f, 20.0f, 0.12f);
        set_light(rim,  true,  -95.0f, 22.0f, 0.55f);
        ambient_ground = 0.14f;
        ambient_sky    = 0.24f;
        break;

    case PhotoLightPreset::FlatCatalog:
        // Deliberately boring: light almost straight down the camera axis, heavy ambient, almost
        // no shadow. What a shop listing wants, and what survives being cropped.
        set_light(key,  true,   90.0f, 62.0f, 0.34f);
        set_light(fill, true,  -90.0f, 55.0f, 0.30f);
        set_light(rim,  false, 315.0f, 20.0f, 0.0f);
        ambient_ground = 0.38f;
        ambient_sky    = 0.46f;
        // A tight highlight on a flat-lit object just looks like a defect.
        key_specular   = 0.03f;
        break;

    case PhotoLightPreset::Dramatic:
        // Grazing key, almost no fill, dark ambient. Long shadows across the floor - the shot that
        // sells geometry rather than colour.
        set_light(key,  true,  160.0f, 18.0f, 0.62f);
        set_light(fill, true,  -20.0f, 12.0f, 0.06f);
        set_light(rim,  true,  -70.0f, 10.0f, 0.24f);
        ambient_ground = 0.08f;
        ambient_sky    = 0.16f;
        key_specular   = 0.11f;
        break;
    }
}

// ---------------------------------------------------------------------------------------------
// Materials (F3)
// ---------------------------------------------------------------------------------------------

const char* photo_material_preset_name(PhotoMaterial::Preset p)
{
    switch (p) {
    case PhotoMaterial::Preset::Plastic: return "Plastic";
    case PhotoMaterial::Preset::Glossy:  return "Glossy / resin";
    case PhotoMaterial::Preset::Matte:   return "Matte PLA";
    case PhotoMaterial::Preset::Rubber:  return "Rubber / TPU";
    case PhotoMaterial::Preset::Metal:   return "Metal";
    case PhotoMaterial::Preset::Silk:    return "Silk";
    default:                             return "?";
    }
}

void photo_material_apply_preset(PhotoMaterial& m, PhotoMaterial::Preset p)
{
    m.preset = p;
    switch (p) {
    default:
    case PhotoMaterial::Preset::Plastic:
        // The reference point: these three values are what "Photo Mode off" sends, so the default
        // material is not a look, it is the absence of one.
        m.roughness = 0.45f; m.metallic = 0.0f; m.spec_scale = 1.0f;
        break;
    case PhotoMaterial::Preset::Glossy:
        // Small, hard highlight — the read on a polished or resin-printed surface.
        m.roughness = 0.12f; m.metallic = 0.0f; m.spec_scale = 1.6f;
        break;
    case PhotoMaterial::Preset::Matte:
        m.roughness = 0.80f; m.metallic = 0.0f; m.spec_scale = 0.45f;
        break;
    case PhotoMaterial::Preset::Rubber:
        // Near-lambert with a very weak, very broad sheen. What makes TPU read as TPU is the
        // ABSENCE of a highlight, so this is mostly about turning things down.
        m.roughness = 0.92f; m.metallic = 0.0f; m.spec_scale = 0.22f;
        break;
    case PhotoMaterial::Preset::Metal:
        // metallic 1 tints the highlight with the base colour and removes the diffuse. Needs the
        // environment to be on to look like anything but grey plastic — see PhotoMaterial.
        m.roughness = 0.25f; m.metallic = 1.0f; m.spec_scale = 2.2f;
        break;
    case PhotoMaterial::Preset::Silk:
        // "Silk PLA" really is half-metallic: a coloured sheen over a coloured body.
        m.roughness = 0.30f; m.metallic = 0.45f; m.spec_scale = 1.8f;
        break;
    }
}

void photo_set_material_uniforms(GLShaderProgram* shader, int extruder_id)
{
    if (shader == nullptr)
        return;
    const PhotoMaterialUniforms m = photo_material_uniforms_for(extruder_id);
    shader->set_uniform("u_mat_metallic",   m.metallic);
    shader->set_uniform("u_mat_shininess",  m.shininess);
    shader->set_uniform("u_mat_spec_scale", m.spec_scale);
}

PhotoMaterial& PhotoModeState::material(int slot)
{
    if (slot < 0)
        slot = 0;
    if ((size_t)slot >= materials.size())
        materials.resize((size_t)slot + 1);
    return materials[(size_t)slot];
}

PhotoMaterialUniforms photo_material_uniforms_for(int extruder_id)
{
    PhotoMaterialUniforms u;   // defaults == the original constants
    const PhotoModeState& pm = photo_mode();
    if (!pm.active)
        return u;

    // GLVolume::extruder_id is 1-based; 0 or -1 means "no extruder assigned", which happens for
    // modifiers and for single-material scenes. Falling back to slot 0 there is deliberate: the
    // object still has to be shaded like something, and slot 0 is what the user was editing.
    const int slot = (extruder_id > 0) ? (extruder_id - 1) : 0;
    if ((size_t)slot >= pm.materials.size())
        return u;   // never set => never overridden

    const PhotoMaterial& m = pm.materials[(size_t)slot];
    u.metallic   = std::clamp(m.metallic, 0.0f, 1.0f);
    u.spec_scale = std::max(m.spec_scale, 0.0f);
    // Perceptual roughness -> Phong exponent, exponentially (perceived gloss is roughly log in the
    // exponent, which is why a linear slider on the exponent itself feels useless at both ends).
    //
    // The curve is ANCHORED rather than hand-tuned: k is chosen so that PHOTO_DEFAULT_ROUGHNESS
    // maps exactly onto LIGHT_TOP_SHININESS_DEFAULT. That is what makes the Plastic preset a
    // provable no-op instead of "about the same" — and it keeps holding if either constant is ever
    // changed, which a magic number would not.
    //   r = 0.45 -> 20 (the original), r = 0.12 -> ~120, r = 0.92 -> ~1.8
    static const float k = std::log2(LIGHT_TOP_SHININESS_DEFAULT) / (1.0f - PHOTO_DEFAULT_ROUGHNESS);
    const float r = std::clamp(m.roughness, 0.02f, 1.0f);
    u.shininess = std::max(1.0f, std::exp2(k * (1.0f - r)));
    return u;
}

// ---------------------------------------------------------------------------------------------
// Quality tier & screenshot helper (F2.5)
// ---------------------------------------------------------------------------------------------

const char* photo_quality_name(PhotoModeState::Quality q)
{
    switch (q) {
    case PhotoModeState::Quality::Normal: return "Normal (interactive)";
    case PhotoModeState::Quality::High:   return "High";
    case PhotoModeState::Quality::Ultra:  return "Ultra";
    default:                              return "?";
    }
}

int photo_shadow_map_res()
{
    const PhotoModeState& pm = photo_mode();
    if (!pm.active)
        return 2048;   // the original constant, untouched outside Photo Mode
    switch (pm.quality) {
    case PhotoModeState::Quality::High:  return 4096;   // ~67 MB of depth texture
    // 8192 is ~268 MB. Offered because a still photo can afford it, but not the default for
    // exactly that reason.
    case PhotoModeState::Quality::Ultra: return 8192;
    default:                             return 2048;
    }
}

float photo_shadow_pcf_spread()
{
    const PhotoModeState& pm = photo_mode();
    if (!pm.active)
        return 1.0f;
    // Widen the 3x3 kernel in step with the resolution, so the shadow's softness in WORLD terms
    // stays constant while its aliasing drops. Leaving the spread at 1.0 while quadrupling the
    // resolution would just produce a sharper, harder, more obviously computed edge — better
    // measured, worse looking.
    switch (pm.quality) {
    case PhotoModeState::Quality::High:  return 2.0f;
    case PhotoModeState::Quality::Ultra: return 3.5f;
    default:                             return 1.0f;
    }
}

bool photo_reflection_enabled()
{
    const PhotoModeState& pm = photo_mode();
    return pm.active
        && pm.reflection_enabled
        && pm.reflection_strength > 0.001f
        && pm.quality != PhotoModeState::Quality::Normal
        // A reflection needs a floor to happen in. On the Bed stage there is a printer bed there
        // instead, and mirroring the scene into it would look like a rendering fault.
        && pm.stage.kind != PhotoStage::Kind::Bed;
}

// Monotonic seconds since first call. Deliberately not wall-clock: a clock change mid-countdown
// would either cut the shot short or hide the UI for hours.
static double photo_now_seconds()
{
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

bool photo_ui_hidden()
{
    const PhotoModeState& pm = photo_mode();
    return pm.active && pm.screenshot_hide_until > 0.0 && photo_now_seconds() < pm.screenshot_hide_until;
}

void photo_begin_screenshot()
{
    PhotoModeState& pm = photo_mode();
    pm.screenshot_hide_until = photo_now_seconds() + std::clamp((double)pm.screenshot_seconds, 1.0, 60.0);
}

// ---------------------------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------------------------

PhotoModeState& photo_mode()
{
    static PhotoModeState s_state;

    // Load once, lazily, rather than from some startup hook. The state is reachable from render
    // paths that run before any Plater/GUI_App init step we could hang a call on, and AppConfig
    // itself is not ready at static-init time — a lazy first touch is the only point that is
    // guaranteed to be both "app_config exists" and "before anyone reads the values".
    static bool s_loaded = false;
    if (!s_loaded && GUI::wxGetApp().app_config != nullptr) {
        // Set BEFORE loading, not after: photo_mode_load_from_app_config() calls photo_mode()
        // itself, and this flag is what stops that from recursing forever.
        s_loaded = true;
        photo_mode_load_from_app_config();
    }
    return s_state;
}

bool photo_mode_available()
{
    // GUI_App::app_config is null very early in startup and again during teardown; several render
    // paths that call this run in both windows.
    const AppConfig* ac = GUI::wxGetApp().app_config;
    return ac != nullptr && ac->get_bool("neotko_libre_mode");
}

bool photo_mode_hides_bed()
{
    const PhotoModeState& s = photo_mode();
    if (!s.active)
        return false;

    // NEOTKO_PHOTOMODE_TAG s253 — EN EL VISOR DE GCODE, DE MOMENTO, NO SE ESCONDE NADA.
    //
    // Esta función es lo que vacía el plato para la foto, y su contrapartida es el ciclorama, que
    // se dibuja en el hueco que deja. En el visor de gcode ese ciclorama **todavía no está
    // portado**: quitar la cama aquí dejaría el suelo vacío, sin nada debajo de la pieza.
    //
    // Y el fallo sería especialmente desagradable porque el estado del modo es COMPARTIDO entre las
    // dos pestañas (decisión del usuario, s253): basta con haber dejado el escenario en Lightbox
    // en Prepare para que entrar en el modo desde Preview hiciera desaparecer la cama sin haber
    // tocado nada aquí. Un ajuste puesto en otra pantalla, días antes, rompiendo ésta.
    //
    // Se quita esta guarda cuando el ciclorama exista en Preview, no antes.
    if (s.owner == PhotoOwner::Preview)
        return false;

    return s.stage.kind != PhotoStage::Kind::Bed;
}

// ---------------------------------------------------------------------------------------------
// Persistence & user presets
// ---------------------------------------------------------------------------------------------
//
// ONE list of fields, used for three things: remembering the last session, saving a named preset,
// and loading either back. The earlier version had the field list written out twice (a setter pass
// and a getter pass) which is precisely how two halves of a config drift apart — add a field,
// forget one side, and it silently stops round-tripping.

namespace {

// A tiny "key=value;" blob. Not JSON: AppConfig stores flat strings, this has to survive being one
// of them, and a dependency-free format that a human can read in the config file is worth more
// here than a schema.
struct Blob
{
    std::string out;
    void put(const char* k, float v)       { out += k; out += '='; out += std::to_string(v); out += ';'; }
    void put(const char* k, int v)         { out += k; out += '='; out += std::to_string(v); out += ';'; }
    void put(const char* k, bool v)        { out += k; out += '='; out += (v ? "1" : "0");   out += ';'; }
};

std::map<std::string, std::string> blob_parse(const std::string& in)
{
    std::map<std::string, std::string> kv;
    size_t i = 0;
    while (i < in.size()) {
        const size_t semi = in.find(';', i);
        const std::string item = in.substr(i, (semi == std::string::npos) ? std::string::npos : semi - i);
        const size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0)
            kv[item.substr(0, eq)] = item.substr(eq + 1);
        if (semi == std::string::npos)
            break;
        i = semi + 1;
    }
    return kv;
}

float get_f(const std::map<std::string, std::string>& kv, const char* k, float fallback)
{
    auto it = kv.find(k);
    if (it == kv.end())
        return fallback;
    // A hand-edited or truncated config must not take the app down on launch.
    try { return std::stof(it->second); } catch (...) { return fallback; }
}
int get_i(const std::map<std::string, std::string>& kv, const char* k, int fallback)
{
    auto it = kv.find(k);
    if (it == kv.end())
        return fallback;
    try { return std::stoi(it->second); } catch (...) { return fallback; }
}
bool get_b(const std::map<std::string, std::string>& kv, const char* k, bool fallback)
{
    auto it = kv.find(k);
    return (it == kv.end()) ? fallback : (it->second == "1" || it->second == "true");
}

void put_light(Blob& b, const std::string& p, const PhotoLight& l)
{
    b.put((p + "on").c_str(),  l.enabled);
    b.put((p + "az").c_str(),  l.azimuth_deg);
    b.put((p + "el").c_str(),  l.elevation_deg);
    b.put((p + "in").c_str(),  l.intensity);
    b.put((p + "tr").c_str(),  l.tint[0]);
    b.put((p + "tg").c_str(),  l.tint[1]);
    b.put((p + "tb").c_str(),  l.tint[2]);
}
void get_light(const std::map<std::string, std::string>& kv, const std::string& p, PhotoLight& l)
{
    l.enabled       = get_b(kv, (p + "on").c_str(), l.enabled);
    l.azimuth_deg   = get_f(kv, (p + "az").c_str(), l.azimuth_deg);
    l.elevation_deg = get_f(kv, (p + "el").c_str(), l.elevation_deg);
    l.intensity     = get_f(kv, (p + "in").c_str(), l.intensity);
    l.tint[0]       = get_f(kv, (p + "tr").c_str(), l.tint[0]);
    l.tint[1]       = get_f(kv, (p + "tg").c_str(), l.tint[1]);
    l.tint[2]       = get_f(kv, (p + "tb").c_str(), l.tint[2]);
}

std::vector<PhotoUserPreset>& presets_storage()
{
    static std::vector<PhotoUserPreset> s;
    return s;
}

} // namespace

std::string photo_serialize()
{
    const PhotoModeState& s = photo_mode();
    Blob b;
    put_light(b, "k_", s.key);
    put_light(b, "f_", s.fill);
    put_light(b, "r_", s.rim);
    b.put("ambg", s.ambient_ground);
    b.put("ambs", s.ambient_sky);
    b.put("frp",  s.fresnel_power);
    b.put("frs",  s.fresnel_strength);
    b.put("kspec", s.key_specular);
    b.put("preset", int(s.last_preset));

    b.put("stage", int(s.stage.kind));
    b.put("str", s.stage.base_color[0]);
    b.put("stg", s.stage.base_color[1]);
    b.put("stb", s.stage.base_color[2]);
    b.put("stext", s.stage.extent_factor);
    b.put("strad", s.stage.wall_radius_mm);

    b.put("envon", s.env_enabled);
    b.put("envint", s.env_intensity);
    b.put("envrot", s.env_rotation_deg);

    b.put("qual", int(s.quality));
    b.put("reflon", s.reflection_enabled);
    b.put("reflstr", s.reflection_strength);
    b.put("shotsec", s.screenshot_seconds);

    b.put("expw", s.export_w);
    b.put("exph", s.export_h);
    b.put("expss", s.export_ss);
    b.put("expa", s.transparent_bg);

    // Only the preset index per slot: the three derived numbers are computed FROM it, and freezing
    // them into every saved preset would make the presets un-improvable later.
    b.put("matn", int(s.materials.size()));
    for (size_t i = 0; i < s.materials.size(); ++i)
        b.put(("mat" + std::to_string(i)).c_str(), int(s.materials[i].preset));
    return b.out;
}

void photo_deserialize(const std::string& blob)
{
    const std::map<std::string, std::string> kv = blob_parse(blob);
    PhotoModeState& s = photo_mode();

    get_light(kv, "k_", s.key);
    get_light(kv, "f_", s.fill);
    get_light(kv, "r_", s.rim);
    s.ambient_ground   = get_f(kv, "ambg", s.ambient_ground);
    s.ambient_sky      = get_f(kv, "ambs", s.ambient_sky);
    s.fresnel_power    = get_f(kv, "frp",  s.fresnel_power);
    s.fresnel_strength = get_f(kv, "frs",  s.fresnel_strength);
    s.key_specular     = get_f(kv, "kspec", s.key_specular);

    const int lp = get_i(kv, "preset", int(s.last_preset));
    if (lp >= 0 && lp < int(PhotoLightPreset::Count))
        s.last_preset = PhotoLightPreset(lp);

    const int kind = get_i(kv, "stage", int(s.stage.kind));
    if (kind >= int(PhotoStage::Kind::Bed) && kind <= int(PhotoStage::Kind::Backdrop))
        s.stage.kind = PhotoStage::Kind(kind);
    s.stage.base_color[0]  = get_f(kv, "str", s.stage.base_color[0]);
    s.stage.base_color[1]  = get_f(kv, "stg", s.stage.base_color[1]);
    s.stage.base_color[2]  = get_f(kv, "stb", s.stage.base_color[2]);
    s.stage.extent_factor  = std::clamp(get_f(kv, "stext", s.stage.extent_factor), 1.2f, 12.0f);
    s.stage.wall_radius_mm = std::clamp(get_f(kv, "strad", s.stage.wall_radius_mm), 0.0f, 1000.0f);

    s.env_enabled      = get_b(kv, "envon", s.env_enabled);
    s.env_intensity    = std::clamp(get_f(kv, "envint", s.env_intensity), 0.0f, 2.0f);
    s.env_rotation_deg = get_f(kv, "envrot", s.env_rotation_deg);

    const int q = get_i(kv, "qual", int(s.quality));
    if (q >= 0 && q < int(PhotoModeState::Quality::Count))
        s.quality = PhotoModeState::Quality(q);
    s.reflection_enabled  = get_b(kv, "reflon", s.reflection_enabled);
    s.reflection_strength = std::clamp(get_f(kv, "reflstr", s.reflection_strength), 0.0f, 1.0f);
    s.screenshot_seconds  = std::clamp(get_f(kv, "shotsec", s.screenshot_seconds), 1.0f, 60.0f);

    s.export_w  = std::clamp(get_i(kv, "expw", s.export_w), 16, 16384);
    s.export_h  = std::clamp(get_i(kv, "exph", s.export_h), 16, 16384);
    s.export_ss = std::clamp(get_i(kv, "expss", s.export_ss), 1, 4);
    s.transparent_bg = get_b(kv, "expa", s.transparent_bg);

    const int matn = std::clamp(get_i(kv, "matn", 0), 0, 64);
    s.materials.clear();
    s.materials.resize((size_t)matn);
    for (int i = 0; i < matn; ++i) {
        const int mp = get_i(kv, ("mat" + std::to_string(i)).c_str(), int(PhotoMaterial::Preset::Plastic));
        if (mp >= 0 && mp < int(PhotoMaterial::Preset::Count))
            photo_material_apply_preset(s.materials[(size_t)i], PhotoMaterial::Preset(mp));
    }
}

void photo_mode_save_to_app_config()
{
    AppConfig* ac = GUI::wxGetApp().app_config;
    if (ac == nullptr)
        return;

    // `active` and screenshot_hide_until are deliberately NOT saved. Photo Mode is something you
    // enter to take a shot, not a state the app should boot into: reopening the slicer into a
    // bedless white room with no toolbars would read as a broken install.
    ac->set(std::string("photo_state"), photo_serialize());

    const std::vector<PhotoUserPreset>& ps = presets_storage();
    ac->set(std::string("photo_preset_count"), std::to_string(ps.size()));
    for (size_t i = 0; i < ps.size(); ++i) {
        ac->set("photo_preset_" + std::to_string(i) + "_name", ps[i].name);
        ac->set("photo_preset_" + std::to_string(i) + "_data", ps[i].data);
    }
}

void photo_mode_load_from_app_config()
{
    const AppConfig* ac = GUI::wxGetApp().app_config;
    if (ac == nullptr)
        return;

    if (ac->has("photo_state"))
        photo_deserialize(ac->get(std::string("photo_state")));

    std::vector<PhotoUserPreset>& ps = presets_storage();
    ps.clear();
    int n = 0;
    if (ac->has("photo_preset_count")) {
        try { n = std::stoi(ac->get(std::string("photo_preset_count"))); } catch (...) { n = 0; }
    }
    n = std::clamp(n, 0, 64);
    for (int i = 0; i < n; ++i) {
        PhotoUserPreset p;
        p.name = ac->get("photo_preset_" + std::to_string(i) + "_name");
        p.data = ac->get("photo_preset_" + std::to_string(i) + "_data");
        if (!p.name.empty())
            ps.push_back(std::move(p));
    }

    PhotoModeState& s = photo_mode();
    s.active = false;
    s.screenshot_hide_until = 0.0;
}

const std::vector<PhotoUserPreset>& photo_presets()
{
    return presets_storage();
}

void photo_preset_save(const std::string& name)
{
    if (name.empty())
        return;
    // ';' and '=' are the blob's own separators, and a name carrying one would corrupt the file it
    // is stored next to. Sanitised rather than rejected: silently dropping a save because of a
    // character the user cannot see is worse than quietly cleaning it.
    std::string clean;
    for (char c : name)
        clean += (c == ';' || c == '=' || c == '\n') ? '_' : c;

    std::vector<PhotoUserPreset>& ps = presets_storage();
    for (PhotoUserPreset& p : ps) {
        if (p.name == clean) {
            p.data = photo_serialize();   // overwrite in place: "save" over a known name is a save
            photo_mode_save_to_app_config();
            return;
        }
    }
    ps.push_back({ clean, photo_serialize() });
    photo_mode_save_to_app_config();
}

void photo_preset_apply(size_t index)
{
    const std::vector<PhotoUserPreset>& ps = presets_storage();
    if (index >= ps.size())
        return;
    photo_deserialize(ps[index].data);
}

void photo_preset_delete(size_t index)
{
    std::vector<PhotoUserPreset>& ps = presets_storage();
    if (index >= ps.size())
        return;
    ps.erase(ps.begin() + (long)index);
    photo_mode_save_to_app_config();
}

// NEOTKO_PHOTOMODE_TAG s242 — a draggable lighting sphere.
//
// Three XYZ sliders are the obvious way to aim a light and they are unusable: the user has to
// solve for a direction one component at a time while watching the scene. Every tool that aims
// lights for a living (Keyshot, Rhino, Blender's HDRI widget) uses a ball you drag instead,
// because the mapping from "where I want the highlight" to "where I drag" is direct.
//
// Returns true while being dragged, so the caller can react on the same frame.
bool photo_light_ball(const char* id, PhotoLight& light, float size_px)
{
    ImGui::PushID(id);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##ball", ImVec2(size_px, size_px));
    const bool active = ImGui::IsItemActive();

    const ImVec2 centre(origin.x + size_px * 0.5f, origin.y + size_px * 0.5f);
    const float  r = size_px * 0.5f - 2.0f;

    if (active) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        float dx = (m.x - centre.x) / r;
        float dy = (m.y - centre.y) / r;
        // Clamp to the disc rather than ignoring out-of-bounds drags: releasing control the
        // instant the cursor leaves the ball makes the widget feel broken. Dragging past the edge
        // now slides the light along the horizon, which is what the gesture implies.
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 1.0f) { dx /= len; dy /= len; }
        // Screen disc -> azimuth/elevation. Centre = straight overhead (elevation 90), rim =
        // horizon (elevation 0); the angle around the centre is the azimuth. Screen Y grows
        // downward while world +Y goes away from the default camera, hence the negated dy.
        const float rad = std::min(std::sqrt(dx * dx + dy * dy), 1.0f);
        light.elevation_deg = (1.0f - rad) * 90.0f;
        if (rad > 1e-3f)
            light.azimuth_deg = std::atan2(-dy, dx) * 180.0f / float(M_PI);
    }

    // Paint it: disc, a couple of guide rings, and the handle where the light currently sits.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col_bg   = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 col_ring = ImGui::GetColorU32(ImGuiCol_Border);
    const ImU32 col_knob = light.enabled ? IM_COL32(255, 214, 92, 255) : IM_COL32(130, 130, 130, 255);
    dl->AddCircleFilled(centre, r, col_bg, 48);
    dl->AddCircle(centre, r, col_ring, 48);
    dl->AddCircle(centre, r * 0.5f, col_ring, 32);
    dl->AddLine(ImVec2(centre.x - r, centre.y), ImVec2(centre.x + r, centre.y), col_ring);
    dl->AddLine(ImVec2(centre.x, centre.y - r), ImVec2(centre.x, centre.y + r), col_ring);

    const float knob_rad = (1.0f - std::clamp(light.elevation_deg, 0.0f, 90.0f) / 90.0f) * r;
    const float az = light.azimuth_deg * float(M_PI) / 180.0f;
    const ImVec2 knob(centre.x + knob_rad * std::cos(az), centre.y - knob_rad * std::sin(az));
    dl->AddLine(centre, knob, col_knob, 1.5f);
    dl->AddCircleFilled(knob, 5.0f, col_knob, 16);

    ImGui::PopID();
    return active;
}

} // namespace Slic3r
