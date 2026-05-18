// NEOTKO_PROFILE_TAG_START
#include "GLGizmoColorMixPainter.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SurfaceColorMix.hpp" // NEOTKO_PROFILE_TAG — NeoDebug PROFILE channel
#include "libslic3r/SurfaceEffectProfile.hpp"

#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <GL/glew.h>
#include <algorithm>
#include <boost/nowide/convert.hpp>

namespace Slic3r::GUI {

// ----------------------------------------------------------------------------
// Boilerplate (mirror of FuzzySkin)
// ----------------------------------------------------------------------------

GLGizmoColorMixPainter::GLGizmoColorMixPainter(GLCanvas3D& parent,
                                               const std::string& icon_filename,
                                               unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id)
    , m_current_tool(ImGui::FillButtonIcon) // Smart fill — primary use case for ColorMix Painter
{
}

std::string GLGizmoColorMixPainter::on_get_name() const
{
    return _u8L("ColorMix Painter");
}

// FFF only, no minimum-filament requirement — single-filament prints can still
// benefit from per-surface ColorMix profiles.
bool GLGizmoColorMixPainter::on_is_selectable() const
{
    return wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF;
}

bool GLGizmoColorMixPainter::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    return !selection.is_empty()
        && (selection.is_single_full_instance() || selection.is_any_volume());
}

void GLGizmoColorMixPainter::on_shutdown()
{
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

PainterGizmoType GLGizmoColorMixPainter::get_painter_type() const
{
    // No dedicated enum value (the field is informational only — never switched on).
    return PainterGizmoType::MM_SEGMENTATION;
}

wxString GLGizmoColorMixPainter::handle_snapshot_action_name(bool shift_down,
                                                             GLGizmoPainterBase::Button /*button_down*/) const
{
    return shift_down ? _L("Erase ColorMix paint") : _L("ColorMix paint");
}

bool GLGizmoColorMixPainter::on_init()
{
    m_shortcut_key = WXK_CONTROL_M; // tentative; collisions reviewed in B-polish

    const wxString ctrl  = _L("Ctrl+");
    const wxString alt   = _L("Alt+");
    const wxString shift = _L("Shift+");

    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"]         = _L("Section view");
    m_desc["reset_direction"]          = _L("Reset direction");
    m_desc["cursor_size_caption"]      = ctrl + _L("Mouse wheel");
    m_desc["cursor_size"]              = _L("Brush size");
    m_desc["paint_caption"]            = _L("Left mouse button");
    m_desc["paint"]                    = _L("Paint with selected profile");
    m_desc["erase_caption"]            = shift + _L("Left mouse button");
    m_desc["erase"]                    = _L("Erase paint");
    m_desc["remove_all"]               = _L("Erase all painting");
    m_desc["tool_type"]                = _L("Tool type");
    m_desc["smart_fill_angle_caption"] = ctrl + _L("Mouse wheel");
    m_desc["smart_fill_angle"]         = _L("Smart fill angle");
    m_desc["profiles"]                 = _L("Profiles");
    m_desc["no_profiles"]              = _L("No profiles saved yet. Use 'Save as profile' in the Surface Color Mixer.");
    m_desc["slots_full"]               = _L("All 15 slots are used on this object. Erase a profile to free one.");

    // Smart fill default — coplanar bias for top surfaces (option 4 design choice).
    m_smart_fill_angle = 1.5f;

    return true;
}

void GLGizmoColorMixPainter::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);
    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

EnforcerBlockerType GLGizmoColorMixPainter::get_left_button_state_type() const
{
    if (m_active_slot >= 1 && m_active_slot <= 15)
        return static_cast<EnforcerBlockerType>(m_active_slot);
    return EnforcerBlockerType::NONE;
}

// ----------------------------------------------------------------------------
// Slot / profile resolution
// ----------------------------------------------------------------------------

// Deterministic preview color when profile->preview_argb is unset.
static ColorRGBA fallback_color_for_id(int id)
{
    // Golden-ratio hue stepping → distinguishable hues for 15 ids.
    const float hue = std::fmod(0.61803398875f * float(id), 1.f);
    const float s = 0.55f, v = 0.85f;
    const float h6 = hue * 6.f;
    const int   i  = int(std::floor(h6)) % 6;
    const float f  = h6 - std::floor(h6);
    const float p = v * (1 - s);
    const float q = v * (1 - f * s);
    const float t = v * (1 - (1 - f) * s);
    float r=0, g=0, b=0;
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }
    return ColorRGBA(r, g, b, 1.f);
}

static ColorRGBA color_for_profile(const SurfaceEffectProfile& p)
{
    if (p.preview_argb == 0u)
        return fallback_color_for_id(p.id);
    const uint8_t a = (p.preview_argb >> 24) & 0xFF;
    const uint8_t r = (p.preview_argb >> 16) & 0xFF;
    const uint8_t g = (p.preview_argb >>  8) & 0xFF;
    const uint8_t b = (p.preview_argb >>  0) & 0xFF;
    return ColorRGBA(r / 255.f, g / 255.f, b / 255.f, a == 0 ? 1.f : a / 255.f);
}

int GLGizmoColorMixPainter::slot_for_selected_profile(bool assign_if_missing)
{
    if (m_selected_profile_id == 0) return 0;
    ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return 0;

    // First look for the profile id in any existing slot of any model_part volume.
    int existing_slot = 0;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        for (int s = 1; s < MAX_SLOTS; ++s) {
            if (mv->colormix_slot_to_profile_id[s] == m_selected_profile_id) {
                existing_slot = s; break;
            }
        }
        if (existing_slot) break;
    }
    if (existing_slot) {
        NEOTKO_LOG(PROFILE, "PAINT slot_lookup profile_id=" << m_selected_profile_id
            << " → existing slot=" << existing_slot);
        return existing_slot;
    }
    if (!assign_if_missing) return 0;

    // Find the lowest slot index that is free across ALL model_part volumes.
    for (int s = 1; s < MAX_SLOTS; ++s) {
        bool free_everywhere = true;
        for (const ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part()) continue;
            if (mv->colormix_slot_to_profile_id[s] != 0) { free_everywhere = false; break; }
        }
        if (free_everywhere) {
            for (ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                mv->colormix_slot_to_profile_id[s] = m_selected_profile_id;
            }
            NEOTKO_LOG(PROFILE, "PAINT slot_assign profile_id=" << m_selected_profile_id
                << " → new slot=" << s);
            return s;
        }
    }
    NEOTKO_LOG(PROFILE, "PAINT slot_assign FAILED — all 15 slots used");
    return 0; // all 15 slots taken — caller shows the "full" warning.
}

std::vector<ColorRGBA>
GLGizmoColorMixPainter::build_ebt_colors_for_volume(const ModelVolume* mv) const
{
    // Index 0 holds the volume's neutral base, indices 1..15 hold per-slot colors.
    // TriangleSelectorPatch internally prepends the volume base color, then maps
    // EnforcerBlockerType(N) → m_ebt_colors[N+1]. We mirror MMU's layout:
    //   ebt_colors[0] = volume base (unpainted)
    //   ebt_colors[1..15] = per-slot colors
    std::vector<ColorRGBA> ebt(MAX_SLOTS, ColorRGBA(0.6f, 0.6f, 0.6f, 1.f));
    ebt[0] = GLVolume::NEUTRAL_COLOR;
    if (mv) {
        const auto& mgr = SurfaceEffectProfileManager::get();
        for (int s = 1; s < MAX_SLOTS; ++s) {
            const int pid = mv->colormix_slot_to_profile_id[s];
            if (pid == 0) continue;
            if (const SurfaceEffectProfile* p = mgr.find(pid))
                ebt[s] = color_for_profile(*p);
        }
    }
    return ebt;
}

void GLGizmoColorMixPainter::refresh_selector_palettes()
{
    const ModelObject* mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (!mo) return;
    int idx = -1;
    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++idx;
        if (idx >= int(m_triangle_selectors.size())) break;
        auto* tsp = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[idx].get());
        if (tsp) {
            tsp->set_ebt_colors(build_ebt_colors_for_volume(mv));
            tsp->request_update_render_data();
        }
    }
}

// ----------------------------------------------------------------------------
// Sidebar UI
// ----------------------------------------------------------------------------

void GLGizmoColorMixPainter::show_tooltip_information(float caption_max, float x, float y)
{
    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(std::string_view{": "}).x + 15.f;

    const float  scale       = m_parent.get_scale();
    const ImVec2 button_size(25 * scale, 25 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max](const wxString& caption, const wxString& text) {
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };
        for (const auto& t : { "paint", "erase", "cursor_size", "smart_fill_angle", "clipping_of_view" })
            draw_text_with_caption(m_desc.at(std::string(t) + "_caption") + ": ", m_desc.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GLGizmoColorMixPainter::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info()->model_object()) return;

    const float approx_height = m_imgui->scaled(22.f);
    y = std::min(y, bottom_limit - approx_height);
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(),
                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize
                  | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float space_size            = m_imgui->get_style_scaling() * 8;
    const float clipping_slider_left  = std::max(
        m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x + m_imgui->scaled(1.5f),
        m_imgui->calc_text_size(m_desc.at("reset_direction")).x + m_imgui->scaled(1.5f)
            + ImGui::GetStyle().FramePadding.x * 2);
    const float cursor_slider_left     = m_imgui->calc_text_size(m_desc.at("cursor_size")).x       + m_imgui->scaled(1.5f);
    const float smart_fill_slider_left = m_imgui->calc_text_size(m_desc.at("smart_fill_angle")).x  + m_imgui->scaled(1.5f);
    const float sliders_left_width     = std::max(smart_fill_slider_left,
                                            std::max(cursor_slider_left, clipping_slider_left));
    const float sliders_width          = m_imgui->scaled(7.0f);
    const float slider_icon_width      = m_imgui->get_slider_icon_size().x;
    const float drag_left_width        = ImGui::GetStyle().WindowPadding.x + sliders_width - space_size;
    const float window_width           = std::max(m_imgui->scaled(18.f),
                                            sliders_left_width + sliders_width + slider_icon_width);
    const float max_tooltip_width      = ImGui::GetFontSize() * 20.0f;

    float caption_max = 0.f;
    for (const auto& t : { "paint", "erase", "cursor_size", "smart_fill_angle", "clipping_of_view" })
        caption_max = std::max(caption_max, m_imgui->calc_text_size(m_desc[std::string(t) + "_caption"]).x);
    caption_max += m_imgui->scaled(1.f);

    // ---- Profile list (scrollable) -----------------------------------------
    auto& mgr = SurfaceEffectProfileManager::get();

    // Drop selection if its profile was deleted under us.
    if (m_selected_profile_id != 0 && mgr.find(m_selected_profile_id) == nullptr) {
        m_selected_profile_id = 0;
        m_active_slot         = 0;
    }
    // Resync active slot every frame (cheap; no mutation when slot already exists).
    if (m_selected_profile_id != 0)
        m_active_slot = slot_for_selected_profile(/*assign_if_missing=*/false);

    m_imgui->text(m_desc["profiles"]);

    const float list_height = m_imgui->scaled(8.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::BeginChild("##cmp_profile_list", ImVec2(window_width, list_height), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (mgr.size() == 0) {
        m_imgui->text_colored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), m_desc["no_profiles"]);
    } else {
        const ModelObject* mo = m_c->selection_info()->model_object();
        const ModelVolume* first_mv = nullptr;
        if (mo) for (const ModelVolume* mv : mo->volumes) if (mv->is_model_part()) { first_mv = mv; break; }

        // Helper: which slot (if any) does profile P occupy in this object?
        auto slot_of = [&](int pid) -> int {
            if (!first_mv) return 0;
            for (int s = 1; s < MAX_SLOTS; ++s)
                if (first_mv->colormix_slot_to_profile_id[s] == pid) return s;
            return 0;
        };

        for (const SurfaceEffectProfile& p : mgr.list()) {
            ImGui::PushID(p.id);
            const ColorRGBA col = color_for_profile(p);
            // Swatch
            ImVec2 sw_pos = ImGui::GetCursorScreenPos();
            const float text_h = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(
                sw_pos, ImVec2(sw_pos.x + text_h * 1.6f, sw_pos.y + text_h),
                ImGuiWrapper::to_ImU32(col));
            ImGui::GetWindowDrawList()->AddRect(
                sw_pos, ImVec2(sw_pos.x + text_h * 1.6f, sw_pos.y + text_h),
                IM_COL32(40, 40, 40, 255));
            ImGui::Dummy(ImVec2(text_h * 1.6f + 4, text_h));
            ImGui::SameLine();

            // Selectable row: id + name + flags
            char flags[16];
            std::snprintf(flags, sizeof(flags), "[%s%s%s]",
                          p.colormix.present  ? "C" : "-",
                          p.pathblend.present ? "P" : "-",
                          p.multipass.present ? "M" : "-");
            const int slot = slot_of(p.id);
            const std::string slot_str = slot ? (" (s" + std::to_string(slot) + ")") : std::string();
            char label[160];
            std::snprintf(label, sizeof(label), "#%d  %s %s%s##cmp_row",
                          p.id, p.name.c_str(), flags, slot_str.c_str());
            const bool sel = (m_selected_profile_id == p.id);
            if (ImGui::Selectable(label, sel)) {
                if (m_selected_profile_id != p.id) {
                    m_selected_profile_id = p.id;
                    m_active_slot = slot_for_selected_profile(/*assign_if_missing=*/true);
                    refresh_selector_palettes();
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // Capacity warning if user picked a profile but no slot is available.
    if (m_selected_profile_id != 0 && m_active_slot == 0)
        m_imgui->text_colored(ImVec4(0.9f, 0.5f, 0.1f, 1.0f), m_desc["slots_full"]);

    ImGui::Separator();

    // ---- Tool type (Circle / Sphere / Triangle / Smart-Fill) ----------------
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc["tool_type"]);

    std::array<wchar_t, 4> tool_ids = {
        ImGui::CircleButtonIcon, ImGui::SphereButtonIcon,
        ImGui::TriangleButtonIcon, ImGui::FillButtonIcon };
    std::array<wchar_t, 4> icons;
    if (m_is_dark_mode)
        icons = { ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon,
                  ImGui::TriangleButtonDarkIcon, ImGui::FillButtonDarkIcon };
    else
        icons = tool_ids;
    const std::array<wxString, 4> tool_tips = {
        _L("Circle"), _L("Sphere"), _L("Triangle"), _L("Smart fill") };

    const float empty_button_width = m_imgui->calc_button_size("").x;
    for (size_t i = 0; i < tool_ids.size(); ++i) {
        if (i != 0) ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Text,   ImVec4(1.f, 1.f, 1.f, 1.f));
        const bool active = (m_current_tool == tool_ids[i]);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f, 0.59f, 0.53f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.59f, 0.53f, 0.25f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.f, 0.59f, 0.53f, 0.30f));
            ImGui::PushStyleColor(ImGuiCol_Border,        ImGuiWrapper::COL_ORCA);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0);
        }
        std::wstring btn_name(1, icons[i]);
        const bool clicked = ImGui::Button(into_u8(btn_name).c_str());
        if (active) { ImGui::PopStyleColor(4); ImGui::PopStyleVar(2); }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(1);

        if (clicked && !active) {
            m_current_tool = tool_ids[i];
            for (auto& sel : m_triangle_selectors) {
                sel->seed_fill_unselect_all_triangles();
                sel->request_update_render_data();
            }
        }
        if (ImGui::IsItemHovered()) m_imgui->tooltip(tool_tips[i], max_tooltip_width);
    }

    ImGui::Dummy(ImVec2(0.f, ImGui::GetFontSize() * 0.1f));

    // ---- Tool-dependent controls -------------------------------------------
    if (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::SphereButtonIcon) {
        m_cursor_type = (m_current_tool == ImGui::CircleButtonIcon)
                      ? TriangleSelector::CursorType::CIRCLE
                      : TriangleSelector::CursorType::SPHERE;
        m_tool_type = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cmp_cursor_radius", &m_cursor_radius,
                                        CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5f * slider_icon_width);
        ImGui::BBLDragFloat("##cmp_cursor_radius_input", &m_cursor_radius, 0.05f, 0.f, 0.f, "%.2f");
    } else if (m_current_tool == ImGui::TriangleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BRUSH;
    } else {
        // Smart-Fill (default tool for ColorMix Painter — coplanar top surfaces).
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::SMART_FILL;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["smart_fill_angle"]);
        const std::string fmt = std::string("%.1f") + I18N::translate_utf8("°", "deg");
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        if (m_imgui->bbl_slider_float_style("##cmp_smart_fill_angle", &m_smart_fill_angle,
                                            SmartFillAngleMin, SmartFillAngleMax, fmt.c_str(), 1.0f, true))
            for (auto& sel : m_triangle_selectors) {
                sel->seed_fill_unselect_all_triangles();
                sel->request_update_render_data();
            }
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5f * slider_icon_width);
        ImGui::BBLDragFloat("##cmp_smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.f, 0.f, "%.2f");
    }

    ImGui::Separator();

    // ---- Clipping plane ----------------------------------------------------
    if (m_c->object_clipper()->get_position() == 0.f) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("clipping_of_view"));
    } else {
        if (m_imgui->button(m_desc.at("reset_direction")))
            wxGetApp().CallAfter([this]() { m_c->object_clipper()->set_position_by_ratio(-1., false); });
    }
    auto clp = float(m_c->object_clipper()->get_position());
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(sliders_width);
    const bool sl_clp = m_imgui->bbl_slider_float_style("##cmp_clp", &clp, 0.f, 1.f, "%.2f", 1.0f, true);
    ImGui::SameLine(drag_left_width + sliders_left_width);
    ImGui::PushItemWidth(1.5f * slider_icon_width);
    const bool dr_clp = ImGui::BBLDragFloat("##cmp_clp_input", &clp, 0.05f, 0.f, 0.f, "%.2f");
    if (sl_clp || dr_clp) m_c->object_clipper()->set_position_by_ratio(clp, true);

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    const float cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(caption_max, x, cur_y);
    const float f_scale = m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));
    ImGui::SameLine();

    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Reset ColorMix paint"),
                                      UndoRedo::SnapshotType::GizmoAction);
        ModelObject* mo  = m_c->selection_info()->model_object();
        int          idx = -1;
        for (ModelVolume* mv : mo->volumes) {
            if (!mv->is_model_part()) continue;
            ++idx;
            m_triangle_selectors[idx]->reset();
            m_triangle_selectors[idx]->request_update_render_data(true);
            std::fill(std::begin(mv->colormix_slot_to_profile_id),
                      std::end  (mv->colormix_slot_to_profile_id), 0);
        }
        m_selected_profile_id = 0;
        m_active_slot         = 0;
        update_model_object();
        refresh_selector_palettes();
        m_parent.set_as_dirty();
    }
    ImGui::PopStyleVar(2);
    GizmoImguiEnd();

    ImGuiWrapper::pop_toolbar_style();
}

// ----------------------------------------------------------------------------
// Model ↔ TriangleSelector sync
// ----------------------------------------------------------------------------

void GLGizmoColorMixPainter::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++idx;
        updated |= mv->color_mix_paint_facets.set(*m_triangle_selectors[idx]);
    }
    if (updated) {
        if (NeoDebug::enabled(NeoDebug::PROFILE)) {
            int painted_volumes = 0, painted_slots_total = 0;
            for (ModelVolume* mv : mo->volumes) {
                if (!mv->is_model_part()) continue;
                ++painted_volumes;
                for (int s = 1; s < 16; ++s)
                    if (mv->colormix_slot_to_profile_id[s] != 0) ++painted_slots_total;
            }
            std::ostringstream os;
            os << "PAINT update_model_object: volumes=" << painted_volumes
               << " slot_assignments=" << painted_slots_total
               << " selected_profile=" << m_selected_profile_id
               << " active_slot=" << m_active_slot;
            NeoDebug::write(NeoDebug::PROFILE, os.str());
        }
        const ModelObjectPtrs& mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}

void GLGizmoColorMixPainter::update_from_model_object(bool /*first_update*/)
{
    wxBusyCursor wait;
    const ModelObject* mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    // Selection is a per-gizmo field; clear it when the active object changes
    // (or when a fresh 3mf is loaded) so the "slots full" warning doesn't fire
    // against a stale profile id whose slot table has been wiped.
    m_selected_profile_id = 0;
    m_active_slot         = 0;
    if (!mo) return;

    for (const ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(
            *mesh, build_ebt_colors_for_volume(mv)));
        const EnforcerBlockerType max_ebt = static_cast<EnforcerBlockerType>(MAX_SLOTS - 1);
        m_triangle_selectors.back()->deserialize(mv->color_mix_paint_facets.get_data(), false, max_ebt);
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
    }
}

} // namespace Slic3r::GUI
// NEOTKO_PROFILE_TAG_END
