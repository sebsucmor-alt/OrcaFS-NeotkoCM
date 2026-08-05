#include "libslic3r/libslic3r.h"
#include "libslic3r/Platform.hpp"
#include "libslic3r/SurfaceColorMix.hpp" // NEOTKO_REALCOLOR_TAG — NeoDebug REALCOLOR channel (NEOTKO_LOG macro)
#include "GLShadersManager.hpp"
#include "3DScene.hpp"
#include "GUI_App.hpp"

#include <cassert>
#include <algorithm>
#include <string_view>
using namespace std::literals;

#include <GL/glew.h>

namespace Slic3r {

std::pair<bool, std::string> GLShadersManager::init()
{
    std::string error;

    auto append_shader = [this, &error](const std::string& name, const GLShaderProgram::ShaderFilenames& filenames,
        const std::initializer_list<std::string_view> &defines = {}) {
        m_shaders.push_back(std::make_unique<GLShaderProgram>());
        if (!m_shaders.back()->init_from_files(name, filenames, defines)) {
            error += name + "\n";
            // if any error happens while initializating the shader, we remove it from the list
            m_shaders.pop_back();
            return false;
        }
        return true;
    };

    assert(m_shaders.empty());

    bool valid = true;

    const std::string prefix = GUI::wxGetApp().is_gl_version_greater_or_equal_to(3, 1) ? "140/" : "110/";
    // imgui shader
    valid &= append_shader("imgui", { prefix + "imgui.vs", prefix + "imgui.fs" });
    // basic shader, used to render all what was previously rendered using the immediate mode
    valid &= append_shader("flat", { prefix + "flat.vs", prefix + "flat.fs" });
    // basic shader with plane clipping, used to render volumes in picking pass
    valid &= append_shader("flat_clip", { prefix + "flat_clip.vs", prefix + "flat_clip.fs" });
    // basic shader for textures, used to render textures
    valid &= append_shader("flat_texture", { prefix + "flat_texture.vs", prefix + "flat_texture.fs" });
    // used to render 3D scene background
    valid &= append_shader("background", { prefix + "background.vs", prefix + "background.fs" });
    // used to render bed axes and model, selection hints, gcode sequential view marker model, preview shells, options in gcode preview
    valid &= append_shader("gouraud_light", { prefix + "gouraud_light.vs", prefix + "gouraud_light.fs" });
    //used to render thumbnail
    valid &= append_shader("thumbnail", { prefix + "thumbnail.vs", prefix + "thumbnail.fs"});
    // used to render printbed
    valid &= append_shader("printbed", { prefix + "printbed.vs", prefix + "printbed.fs" });
    // used to render options in gcode preview
    if (GUI::wxGetApp().is_gl_version_greater_or_equal_to(3, 3)) {
        valid &= append_shader("gouraud_light_instanced", { prefix + "gouraud_light_instanced.vs", prefix + "gouraud_light_instanced.fs" });
    }

    // NEOTKO_REALCOLOR_TAG: depth-peeled Beer-Lambert compositing for EViewType::RealColor.
    // Loaded unconditionally now, like every other shader here — the 110/ variants use
    // gl_FragData[N] (core GLSL 1.10 MRT, no layout(location=N)/GL_ARB_explicit_attrib_location)
    // so they work under the legacy/compatibility profile too, not just Core 3.1+. `prefix`
    // (140/ vs 110/) picks the right pair automatically, same as every other shader above.
    // Actual GPU-capability gating (FBO + float-texture support) lives in
    // GCodeViewer.cpp::realcolor_gpu_supported(), which hides the combo entry, not the load.
    {
        const bool peel_ok = append_shader("realcolor_peel", { prefix + "realcolor_peel.vs", prefix + "realcolor_peel.fs" });
        const bool accum_ok = append_shader("realcolor_accum", { prefix + "realcolor_quad.vs", prefix + "realcolor_accum.fs" });
        const bool present_ok = append_shader("realcolor_present", { prefix + "realcolor_quad.vs", prefix + "realcolor_present.fs" });
        valid &= peel_ok && accum_ok && present_ok;
        NEOTKO_LOG(REALCOLOR, "GLShadersManager::init: prefix=\"" << prefix << "\" realcolor_peel=" << peel_ok
            << " realcolor_accum=" << accum_ok << " realcolor_present=" << present_ok);
    }

    // NEOTKO_REALCOLOR_TAG s166 (item 4): Phong+fresnel+SSAO shells for render_shells(), gated
    // at USE time by NeoDebug::REALCOLOR (see render_shells() in GCodeViewer.cpp) — loaded
    // unconditionally here like RealColor's own shaders above, so `valid` intentionally does
    // NOT gate the overall init() result on these: if they fail to load on some GPU,
    // render_shells() falls back to plain "gouraud_light" (same null-check pattern already used
    // for realcolor_present in render_toolpaths_realcolor), it doesn't break shader init for the
    // rest of the app. Not gated behind is_gl_version_greater_or_equal_to(3,1) for the same
    // reason RealColor's aren't — the 110/ variants exist precisely to cover that legacy path.
    {
        const bool gbuf_ok   = append_shader("shells_gbuffer", { prefix + "shells_gbuffer.vs", prefix + "shells_gbuffer.fs" });
        const bool lit_ok    = append_shader("shells_lit", { prefix + "shells_lit.vs", prefix + "shells_lit.fs" });
        const bool shadow_ok = append_shader("shells_shadow", { prefix + "shells_shadow.vs", prefix + "shells_shadow.fs" });
        // NEOTKO_SHADOW_TAG s229 (Fase 2): depth-only pass rendered from the light, feeding the
        // real directional shadow map that shells_lit.fs samples. Same not-gating-`valid` policy as
        // the shaders above: if it fails to load, render_volumes_lit() just runs with
        // u_shadow_enabled=false and keeps the AO + SSCS path, it doesn't break shader init.
        const bool sdepth_ok = append_shader("shadow_depth", { prefix + "shadow_depth.vs", prefix + "shadow_depth.fs" });
        // NEOTKO_PHOTOMODE_TAG s242: the cyclorama that replaces the bed in Photo Mode. Same
        // not-gating-`valid` policy as its neighbours above: if it fails to load, _render_photo_stage()
        // simply draws nothing and the user gets an empty backdrop instead of a dead app.
        const bool stage_ok  = append_shader("photo_stage", { prefix + "photo_stage.vs", prefix + "photo_stage.fs" });
        NEOTKO_LOG(REALCOLOR, "GLShadersManager::init: prefix=\"" << prefix << "\" shells_gbuffer=" << gbuf_ok
            << " shells_lit=" << lit_ok << " shells_shadow=" << shadow_ok << " shadow_depth=" << sdepth_ok
            << " photo_stage=" << stage_ok);
    }

    // used to render objects in 3d editor
    valid &= append_shader("gouraud", { prefix + "gouraud.vs", prefix + "gouraud.fs" }
#if ENABLE_ENVIRONMENT_MAP
        , { "ENABLE_ENVIRONMENT_MAP"sv }
#endif // ENABLE_ENVIRONMENT_MAP
        );
    // used to render variable layers heights in 3d editor
    valid &= append_shader("variable_layer_height", { prefix + "variable_layer_height.vs", prefix + "variable_layer_height.fs" });
    // used to render highlight contour around selected triangles inside the multi-material gizmo
    valid &= append_shader("mm_contour", { prefix + "mm_contour.vs", prefix + "mm_contour.fs" });
    // Used to render painted triangles inside the multi-material gizmo. Triangle normals are computed inside fragment shader.
    // For Apple's on Arm CPU computed triangle normals inside fragment shader using dFdx and dFdy has the opposite direction.
    // Because of this, objects had darker colors inside the multi-material gizmo.
    // Based on https://stackoverflow.com/a/66206648, the similar behavior was also spotted on some other devices with Arm CPU.
    // Since macOS 12 (Monterey), this issue with the opposite direction on Apple's Arm CPU seems to be fixed, and computed
    // triangle normals inside fragment shader have the right direction.
    if (platform_flavor() == PlatformFlavor::OSXOnArm && wxPlatformInfo::Get().GetOSMajorVersion() < 12)
        valid &= append_shader("mm_gouraud", { prefix + "mm_gouraud.vs", prefix + "mm_gouraud.fs" }, { "FLIP_TRIANGLE_NORMALS"sv });
    else
        valid &= append_shader("mm_gouraud", { prefix + "mm_gouraud.vs", prefix + "mm_gouraud.fs" });

    return { valid, error };
}

void GLShadersManager::shutdown()
{
    m_shaders.clear();
}

GLShaderProgram* GLShadersManager::get_shader(const std::string& shader_name)
{
    auto it = std::find_if(m_shaders.begin(), m_shaders.end(), [&shader_name](std::unique_ptr<GLShaderProgram>& p) { return p->get_name() == shader_name; });
    return (it != m_shaders.end()) ? it->get() : nullptr;
}

GLShaderProgram* GLShadersManager::get_current_shader()
{
    GLint id = 0;
    glsafe(::glGetIntegerv(GL_CURRENT_PROGRAM, &id));
    if (id == 0)
        return nullptr;

    auto it = std::find_if(m_shaders.begin(), m_shaders.end(), [id](std::unique_ptr<GLShaderProgram>& p) { return static_cast<GLint>(p->get_id()) == id; });
    return (it != m_shaders.end()) ? it->get() : nullptr;
}

} // namespace Slic3r

