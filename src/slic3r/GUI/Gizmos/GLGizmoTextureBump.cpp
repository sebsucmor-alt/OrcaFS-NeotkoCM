#include "GLGizmoTextureBump.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "libslic3r/NeoDebug.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <wx/filedlg.h>

namespace Slic3r {
namespace GUI {

GLGizmoTextureBump::GLGizmoTextureBump(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{
}

bool GLGizmoTextureBump::on_init()
{
    return true;
}

std::string GLGizmoTextureBump::on_get_name() const
{
    return _u8L("Texture Bump");
}

bool GLGizmoTextureBump::on_is_activable() const
{
    // NEOTKO_TEXTUREBUMP_TAG — WIP gate, requested explicitly: only offered when the LibreMode
    // master switch is on (same pattern as GLGizmoAlignStack::on_is_activable) AND the user opted
    // into the debug/WIP build via `export ORCA_DEBUG_ALL=1` (or ORCA_DEBUG_TEXTUREBUMP=1
    // specifically). Mirrors the engine-side gate in PrintObject::make_perimeters().
    return wxGetApp().app_config != nullptr
        && wxGetApp().app_config->get_bool("neotko_libre_enabled")
        && NeoDebug::enabled(NeoDebug::TEXTUREBUMP)
        && m_parent.get_selection().is_single_full_instance();
}

bool GLGizmoTextureBump::on_is_selectable() const
{
    // NEOTKO_TEXTUREBUMP_TAG — controls whether the icon appears in the toolbar at all
    // (GLGizmosManager::get_selectable_idxs() only renders gizmos that pass this check).
    // on_is_activable() alone only greys the icon out once it's already shown -- without this
    // override the icon was always visible (just disabled) even with the gate off. Same feature
    // gate as on_is_activable(), minus the selection check (visibility shouldn't flicker with
    // whatever happens to be selected right now).
    return wxGetApp().app_config != nullptr
        && wxGetApp().app_config->get_bool("neotko_libre_enabled")
        && NeoDebug::enabled(NeoDebug::TEXTUREBUMP);
}

void GLGizmoTextureBump::on_render_input_window(float x, float y, float bottom_limit)
{
    y = std::min(y, bottom_limit - ImGui::GetWindowHeight());

    const float scale = m_parent.get_scale();
    ImGuiWrapper::push_toolbar_style(scale);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
    GizmoImguiBegin("TextureBump", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    m_imgui->text(_u8L("Texture Bump: deterministic image relief"));
    ImGui::Separator();
    m_imgui->text(_u8L("Enable and tune the rest of the settings under:"));
    m_imgui->text(_u8L("Object Settings -> Others -> Texture Bump"));
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.3f));

    // NEOTKO_TEXTUREBUMP_TAG — writes texture_bump_image_path directly on the selected object's
    // config override, same primitive GUI_ObjectList.cpp uses everywhere
    // (object->config.set_key_value(...)). on_is_activable() already guarantees a single full
    // instance is selected while this panel is showing.
    const Selection& selection = m_parent.get_selection();
    const int        obj_idx   = selection.get_object_idx();
    ModelObject*     model_object = (obj_idx >= 0 && selection.get_model() != nullptr)
        ? selection.get_model()->objects[obj_idx] : nullptr;

    if (model_object != nullptr) {
        const std::string current_path = model_object->config.has("texture_bump_image_path")
            ? model_object->config.get().opt_string("texture_bump_image_path") : std::string();

        m_imgui->text(_u8L("Current texture image:"));
        m_imgui->text_wrapped(current_path.empty() ? _u8L("(none set)") : current_path, ImGui::GetFontSize() * 20.0f);
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.2f));

        if (m_imgui->button(_u8L("Browse for texture image..."))) {
            wxFileDialog dialog(nullptr, _u8L("Select a PNG displacement map"), wxEmptyString, wxEmptyString,
                                 "PNG files (*.png)|*.png", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() == wxID_OK) {
                wxGetApp().plater()->take_snapshot("Texture Bump: set image");
                model_object->config.set_key_value("texture_bump_image_path", new ConfigOptionString(dialog.GetPath().ToStdString()));
                // NEOTKO_TEXTUREBUMP_TAG — default the mode to AllWalls: External/Contour-only
                // never touches more than the outermost wall, so it never exercises the taper
                // that keeps the innermost wall untouched (see TextureBump.cpp's
                // texture_bump_effect_scale) -- easy to pick by mistake expecting the full
                // layered effect. Only set it if the object doesn't already have an explicit
                // choice, so a deliberate override in Object Settings is never clobbered.
                const bool has_explicit_type = model_object->config.has("texture_bump")
                    && model_object->config.get().opt_enum<TextureBumpType>("texture_bump") != TextureBumpType::None;
                if (!has_explicit_type)
                    model_object->config.set_key_value("texture_bump", new ConfigOptionEnum<TextureBumpType>(TextureBumpType::AllWalls));
                wxGetApp().plater()->update();
            }
        }
    }

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace GUI
} // namespace Slic3r
