#ifndef slic3r_GLShader_hpp_
#define slic3r_GLShader_hpp_

#include <array>
#include <string>
#include <string_view>

#include "libslic3r/Point.hpp"

namespace Slic3r {

class ColorRGB;
class ColorRGBA;

class GLShaderProgram
{
public:
    enum class EShaderType
    {
        Vertex,
        Fragment,
        Geometry,
        TessEvaluation,
        TessControl,
        Compute,
        Count
    };

    typedef std::array<std::string, static_cast<size_t>(EShaderType::Count)> ShaderFilenames;
    typedef std::array<std::string, static_cast<size_t>(EShaderType::Count)> ShaderSources;

private:
    std::string m_name;
    unsigned int m_id{ 0 };
    std::vector<std::pair<std::string, int>> m_attrib_location_cache;
    std::vector<std::pair<std::string, int>> m_uniform_location_cache;

public:
    ~GLShaderProgram();

    bool init_from_files(const std::string& name, const ShaderFilenames& filenames, const std::initializer_list<std::string_view> &defines = {});
    bool init_from_texts(const std::string& name, const ShaderSources& sources);

    const std::string& get_name() const { return m_name; }
    unsigned int get_id() const { return m_id; }

    void start_using() const;
    void stop_using() const;

    void set_uniform(const char* name, int value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, bool value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, float value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, double value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<int, 2>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<int, 3>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<int, 4>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<float, 2>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<float, 3>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<float, 4>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const std::array<double, 4>& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const float* value, size_t size) const { set_uniform(get_uniform_location(name), value, size); }
    void set_uniform(const char* name, const Transform3f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Transform3d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix3f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix3d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix4f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Matrix4d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec2f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec2d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec3f& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const Vec3d& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const ColorRGB& value) const { set_uniform(get_uniform_location(name), value); }
    void set_uniform(const char* name, const ColorRGBA& value) const { set_uniform(get_uniform_location(name), value); }

    void set_uniform(int id, int value) const;
    void set_uniform(int id, bool value) const;
    void set_uniform(int id, float value) const;
    void set_uniform(int id, double value) const;
    void set_uniform(int id, const std::array<int, 2>& value) const;
    void set_uniform(int id, const std::array<int, 3>& value) const;
    void set_uniform(int id, const std::array<int, 4>& value) const;
    void set_uniform(int id, const std::array<float, 2>& value) const;
    void set_uniform(int id, const std::array<float, 3>& value) const;
    void set_uniform(int id, const std::array<float, 4>& value) const;
    void set_uniform(int id, const std::array<double, 4>& value) const;
    void set_uniform(int id, const float* value, size_t size) const;
    void set_uniform(int id, const Transform3f& value) const;
    void set_uniform(int id, const Transform3d& value) const;
    void set_uniform(int id, const Matrix3f& value) const;
    void set_uniform(int id, const Matrix3d& value) const;
    void set_uniform(int id, const Matrix4f& value) const;
    void set_uniform(int id, const Matrix4d& value) const;
    void set_uniform(int id, const Vec2f& value) const;
    void set_uniform(int id, const Vec2d& value) const;
    void set_uniform(int id, const Vec3f& value) const;
    void set_uniform(int id, const Vec3d& value) const;
    void set_uniform(int id, const ColorRGB& value) const;
    void set_uniform(int id, const ColorRGBA& value) const;

    // returns -1 if not found
    int get_attrib_location(const char* name) const;
    // returns -1 if not found
    int get_uniform_location(const char* name) const;
};

// NEOTKO_SMOOTHNORMALS_TAG s229 — live shading tuning for the 3D editor view.
//
// The lighting model in gouraud.vs used to be a wall of #defines, which meant every question
// about the shading ("is this the specular? how much of it? what happens at shininess 8?") cost a
// full rebuild. These are now uniforms, pushed once per shader bind from GLShaderProgram::
// start_using(), and the values live here so the debug panel (GLCanvas3D::_render_shading_debug_panel,
// gated on ORCA_DEBUG_SHADING) can drag them around at 60 fps.
//
// Safety property worth keeping: while `override_lighting` is false the shaders use their original
// baked-in constants and the render is byte-identical to before this struct existed. Nothing here
// is persisted - it is a probe, not a feature.
struct ShadingTuning
{
    // Master switch. False => shaders ignore every lighting field below.
    bool override_lighting = false;

    // Mesh-side knob rather than a shader one: the crease angle used by smooth_corner_normals()
    // (GLModel.cpp). Changing it means re-uploading the volumes, which the panel does explicitly.
    float crease_angle = 30.0f;

    // Defaults below mirror the original #defines in gouraud.vs exactly, INTENSITY_CORRECTION
    // (0.6) already folded in, so flipping override_lighting on alone changes nothing on screen.
    std::array<float, 3> light_top_dir    = { -0.4574957f, 0.4574957f, 0.7624929f };
    float                top_diffuse      = 0.48f;   // 0.8   * 0.6
    float                top_specular     = 0.075f;  // 0.125 * 0.6
    float                top_shininess    = 20.0f;
    std::array<float, 3> light_front_dir  = { 0.6985074f, 0.1397015f, 0.6985074f };
    float                front_diffuse    = 0.18f;   // 0.3   * 0.6
    float                ambient          = 0.3f;

    // Diagnostic views, straight out of the fragment shader:
    //   0 = normal shading
    //   1 = eye-space normal as RGB - shading noise becomes blatant, no lighting to hide it
    //   2 = amplified deviation from the flat-shaded reference, for hunting subtle noise
    int   debug_view      = 0;
    float debug_amplify   = 20.0f;

    // --- LibreMode shells_lit pipeline --------------------------------------------------------
    // NEOTKO_SMOOTHNORMALS_TAG s229: with "neotko_libre_mode" on, the Prepare tab does NOT draw
    // objects with gouraud at all - GLCanvas3D::_render_objects hands them to
    // GCodeViewer::render_volumes_lit() and the shells_lit shader. Everything above is dead in
    // that configuration, so the same live-tuning treatment is applied to the three screen-space
    // effects that shader runs, each of which is a candidate for camera-dependent noise on flat
    // faces: SSAO, screen-space contact shadows, and the shadow map.
    bool  override_libre     = false;   // false => the SHELLS_*/SHADOW_* constants in GCodeViewer.cpp
    float ao_radius_px       = 6.0f;    // SHELLS_AO_RADIUS_PX
    float ao_strength        = 0.35f;   // SHELLS_AO_STRENGTH
    float ao_bias_mm         = 0.05f;   // SHELLS_AO_BIAS_MM - tangent-plane tolerance, see shells_lit.fs
    float sscs_length_mm     = 6.0f;    // SHELLS_SSCS_LENGTH_MM
    float sscs_thickness_mm  = 4.0f;    // SHELLS_SSCS_THICKNESS_MM
    float sscs_strength      = 0.5f;    // SHELLS_SSCS_STRENGTH
    float shadow_strength    = 0.55f;   // SHADOW_MAP_STRENGTH
    float shadow_bias_min    = 0.0006f; // SHADOW_BIAS_MIN
    float shadow_bias_max    = 0.0035f; // SHADOW_BIAS_MAX

    // Isolate one term of shells_lit to see what it contributes on its own:
    //   0 = off, 1 = AO only, 2 = contact shadows only, 3 = shadow map only, 4 = normals as RGB
    int   libre_isolate      = 0;
};

// Process-wide, GUI thread only (same lifetime assumption as the ImGui panel that edits it).
ShadingTuning& shading_tuning();

} // namespace Slic3r

#endif /* slic3r_GLShader_hpp_ */
