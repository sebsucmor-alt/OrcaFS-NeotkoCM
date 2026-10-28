#include "GLGizmoMmuSegmentation.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/MixedColorMatchHelpers.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"
#include "slic3r/Utils/UndoRedo.hpp"


#include <GL/glew.h>
#include <algorithm>
#include <cmath>
#include <boost/log/trivial.hpp>

namespace Slic3r::GUI {

static inline void show_notification_extruders_limit_exceeded()
{
    wxGetApp()
        .plater()
        ->get_notification_manager()
        ->push_notification(NotificationType::MmSegmentationExceededExtrudersLimit, NotificationManager::NotificationLevel::PrintInfoNotificationLevel,
                            GUI::format(_L("Filament count exceeds the maximum number that painting tool supports. Only the "
                                           "first %1% filaments will be available in painting tool."), GLGizmoMmuSegmentation::EXTRUDERS_LIMIT));
}

// --- Gradient rendering helpers (ported from MixedFilamentBadge) ---

static ImU32 interpolate_ImU32(ImU32 c0, ImU32 c1, double t)
{
    t = std::max(0.0, std::min(1.0, t));
    int r = int(((c0 >> IM_COL32_R_SHIFT) & 0xFF) * (1.0 - t) + ((c1 >> IM_COL32_R_SHIFT) & 0xFF) * t);
    int g = int(((c0 >> IM_COL32_G_SHIFT) & 0xFF) * (1.0 - t) + ((c1 >> IM_COL32_G_SHIFT) & 0xFF) * t);
    int b = int(((c0 >> IM_COL32_B_SHIFT) & 0xFF) * (1.0 - t) + ((c1 >> IM_COL32_B_SHIFT) & 0xFF) * t);
    return IM_COL32(r, g, b, 255);
}

static ImU32 interpolate_ImU32(const std::vector<ImU32>& colors, double pos)
{
    if (colors.empty())
        return IM_COL32(0x26, 0xA6, 0x9A, 255);
    if (colors.size() == 1)
        return colors.front();

    pos = std::clamp(pos, 0.0, 1.0);
    const double scaled = pos * double(colors.size() - 1);
    const size_t segment = std::min<size_t>(colors.size() - 2, size_t(std::floor(scaled)));
    const double local = scaled - double(segment);
    return interpolate_ImU32(colors[segment], colors[segment + 1], local);
}

static float bT601_luminance(ImU32 c)
{
    float r = ((c >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
    float g = ((c >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
    float b = ((c >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
    return 0.299f * r + 0.587f * g + 0.114f * b;
}

static bool very_light(ImU32 c)
{
    return ((c >> IM_COL32_R_SHIFT) & 0xFF) > 224
        && ((c >> IM_COL32_G_SHIFT) & 0xFF) > 224
        && ((c >> IM_COL32_B_SHIFT) & 0xFF) > 224;
}

static bool is_gradient_definition(const MixedFilamentDefinition& definition)
{
    return definition.recipe.kind == MixedFilamentRecipeKind::WeightedBlend &&
           definition.behavior.gradient.enabled &&
           definition.recipe.blend.components.size() >= 2;
}

static std::vector<ImU32> mixed_filament_gradient_stops_ImU32(const MixedFilamentDefinition& definition,
                                                              const MixedFilamentDisplayContext& ctx)
{
    const std::vector<wxColour> preview_colors =
        build_mixed_filament_gradient_preview(definition, ctx).sampled_colors;

    std::vector<ImU32> colors;
    colors.reserve(preview_colors.size());
    for (const wxColour& color : preview_colors)
        colors.emplace_back(IM_COL32(color.Red(), color.Green(), color.Blue(), 255));
    return colors;
}

void GLGizmoMmuSegmentation::on_opening()
{
    if (get_extruders_colors().size() > GLGizmoMmuSegmentation::EXTRUDERS_LIMIT)
        show_notification_extruders_limit_exceeded();

}

void GLGizmoMmuSegmentation::on_shutdown()
{
    m_parent.use_slope(false);
    m_parent.toggle_model_objects_visibility(true);
}

std::string GLGizmoMmuSegmentation::on_get_name() const
{
    return _u8L("Color Painting");
}

bool GLGizmoMmuSegmentation::on_is_selectable() const
{
    return (wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptFFF
            && /*wxGetApp().get_mode() != comSimple && */wxGetApp().filaments_cnt() > 1);
}

bool GLGizmoMmuSegmentation::on_is_activable() const
{
    const Selection& selection = m_parent.get_selection();
    return !selection.is_empty() && (selection.is_single_full_instance() || selection.is_any_volume()) && wxGetApp().filaments_cnt() > 1;
}

//BBS: use the global one in 3DScene.cpp
/*static std::vector<ColorRGBA> get_extruders_colors()
{
    unsigned char                     rgb_color[3] = {};
    std::vector<std::string>          colors       = Slic3r::GUI::wxGetApp().plater()->get_extruder_colors_from_plater_config();
    std::vector<ColorRGBA> colors_out(colors.size());
    for (const std::string &color : colors) {
        Slic3r::GUI::BitmapCache::parse_color(color, rgb_color);
        size_t color_idx      = &color - &colors.front();
        colors_out[color_idx] = {float(rgb_color[0]) / 255.f, float(rgb_color[1]) / 255.f, float(rgb_color[2]) / 255.f, 1.f};
    }

    return colors_out;
}*/

static std::vector<int> get_extruder_id_for_volumes(const ModelObject &model_object)
{
    std::vector<int> extruders_idx;
    extruders_idx.reserve(model_object.volumes.size());
    for (const ModelVolume *model_volume : model_object.volumes) {
        if (!model_volume->is_model_part())
            continue;

        extruders_idx.emplace_back(model_volume->extruder_id());
    }

    return extruders_idx;
}

static std::vector<unsigned int> get_display_filament_ids(size_t total_filaments)
{
    std::vector<unsigned int> ordered_filament_ids;
    if (wxGetApp().plater() != nullptr)
        ordered_filament_ids = wxGetApp().plater()->sidebar().get_ui_ordered_filament_ids();

    std::vector<unsigned int> sanitized_filament_ids;
    sanitized_filament_ids.reserve(total_filaments);
    std::vector<bool> used_filament_ids(total_filaments + 1, false);
    for (const unsigned int filament_id : ordered_filament_ids) {
        if (filament_id == 0 || filament_id > total_filaments || used_filament_ids[filament_id])
            continue;
        used_filament_ids[filament_id] = true;
        sanitized_filament_ids.emplace_back(filament_id);
    }

    for (unsigned int filament_id = 1; filament_id <= total_filaments; ++filament_id) {
        if (!used_filament_ids[filament_id])
            sanitized_filament_ids.emplace_back(filament_id);
    }

    return sanitized_filament_ids;
}

void GLGizmoMmuSegmentation::init_extruders_data(const std::vector<ColorRGBA> &extruder_colors)
{
    const unsigned int old_selected_filament_id =
        m_selected_extruder_idx < m_display_filament_ids.size() ? m_display_filament_ids[m_selected_extruder_idx] :
        (m_selected_extruder_idx < m_extruders_colors.size() ? unsigned(m_selected_extruder_idx + 1) : 0);

    m_extruders_colors = extruder_colors;
    m_display_filament_ids = get_display_filament_ids(m_extruders_colors.size());

    m_selected_extruder_idx = 0;
    if (!m_display_filament_ids.empty()) {
        auto selected_it = std::find(m_display_filament_ids.begin(), m_display_filament_ids.end(), old_selected_filament_id);
        if (selected_it != m_display_filament_ids.end())
            m_selected_extruder_idx = size_t(std::distance(m_display_filament_ids.begin(), selected_it));
    }

    // keep remap table consistent with current extruder count
    m_extruder_remap.resize(m_display_filament_ids.size());
    for (size_t i = 0; i < m_extruder_remap.size(); ++i)
        m_extruder_remap[i] = i;

    // Keep gradient rendering on the same color engine, TDs, and preview settings as the mixed-filament UI.
    std::vector<std::string> physical_hex = wxGetApp().plater()->get_extruder_colors_from_plater_config(nullptr, false);
    m_mixed_display_context = build_mixed_filament_display_context(physical_hex);
}

void GLGizmoMmuSegmentation::init_extruders_data()
{
    init_extruders_data(get_extruders_colors());
}

bool GLGizmoMmuSegmentation::on_init()
{
    // BBS
    m_shortcut_key = WXK_CONTROL_N;

    // FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
    const wxString ctrl  = _L("Ctrl+");
    // FIXME: maybe should be using GUI::shortkey_alt_prefix() or equivalent?
    const wxString alt   = _L("Alt+");
    const wxString shift = _L("Shift+");

    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"]     = _L("Section view");
    m_desc["reset_direction"]     = _L("Reset direction");
    m_desc["cursor_size_caption"]  = ctrl + _L("Mouse wheel");
    m_desc["cursor_size"]          = _L("Pen size");
    m_desc["cursor_type"]          = _L("Pen shape");

    m_desc["paint_caption"]        = _L("Left mouse button");
    m_desc["paint"]                = _L("Paint");
    m_desc["erase_caption"]        = shift + _L("Left mouse button");
    m_desc["erase"]                = _L("Erase");
    m_desc["shortcut_key_caption"] = _L("Key 1~9");
    m_desc["shortcut_key"]         = _L("Choose filament");
    m_desc["edge_detection"]       = _L("Edge detection");
    m_desc["gap_area_caption"]     = ctrl + _L("Mouse wheel");
    m_desc["gap_area"]             = _L("Gap area");
    m_desc["perform"]              = _L("Perform");

    m_desc["remove_all"]           = _L("Erase all painting");
    m_desc["circle"]               = _L("Circle");
    m_desc["sphere"]               = _L("Sphere");
    m_desc["pointer"]              = _L("Triangles");

    m_desc["filaments"]            = _L("Filaments");
    m_desc["tool_type"]            = _L("Tool type");
    m_desc["tool_brush"]           = _L("Brush");
    m_desc["tool_smart_fill"]      = _L("Smart fill");
    m_desc["tool_bucket_fill"]     = _L("Bucket fill");

    m_desc["smart_fill_angle_caption"] = ctrl + _L("Mouse wheel");
    m_desc["smart_fill_angle"]     = _L("Smart fill angle");

    m_desc["height_range_caption"] = ctrl + _L("Mouse wheel");
    m_desc["height_range"]         = _L("Height range");

    //add toggle wire frame hint
    m_desc["toggle_wireframe_caption"]        = alt + shift + _L("Enter");
    m_desc["toggle_wireframe"]                = _L("Toggle Wireframe");

    // Filament remapping descriptions
    m_desc["perform_remap"]                   = _L("Remap filaments");
    m_desc["remap"]                           = _L("Remap");
    m_desc["cancel_remap"]                    = _L("Cancel");

    init_extruders_data();

    return true;
}

GLGizmoMmuSegmentation::GLGizmoMmuSegmentation(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{
}

void GLGizmoMmuSegmentation::render_painter_gizmo()
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

void GLGizmoMmuSegmentation::data_changed(bool is_serializing)
{
    GLGizmoPainterBase::data_changed(is_serializing);
    if (m_state != On || wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() != ptFFF || wxGetApp().extruders_edited_cnt() <= 1)
        return;

    ModelObject* model_object = m_c->selection_info()->model_object();
    const std::vector<ColorRGBA> current_extruder_colors = get_extruders_colors();
    const int prev_extruders_count = int(m_extruders_colors.size());
    const int current_extruders_count = int(current_extruder_colors.size());
    const std::vector<unsigned int> current_display_filament_ids = get_display_filament_ids(current_extruder_colors.size());
    if (prev_extruders_count != current_extruders_count) {
        if (current_extruder_colors.size() > GLGizmoMmuSegmentation::EXTRUDERS_LIMIT)
            show_notification_extruders_limit_exceeded();

        this->init_extruders_data(current_extruder_colors);
        // Reinitialize triangle selectors because of change of extruder count need also change the size of GLIndexedVertexArray
        if (prev_extruders_count != current_extruders_count)
            this->init_model_triangle_selectors();
    }
    else if (current_extruder_colors != m_extruders_colors) {
        this->init_extruders_data(current_extruder_colors);
        this->update_triangle_selectors_colors();
    }
    else if (current_display_filament_ids != m_display_filament_ids) {
        this->init_extruders_data(current_extruder_colors);
    }
    else if (model_object != nullptr && get_extruder_id_for_volumes(*model_object) != m_volumes_extruder_idxs) {
        this->init_model_triangle_selectors();
    }
}

// BBS
bool GLGizmoMmuSegmentation::on_number_key_down(int number)
{
    int extruder_idx = number - 1;
    if (extruder_idx >= 0 && size_t(extruder_idx) < m_display_filament_ids.size())
        m_selected_extruder_idx = extruder_idx;

    return true;
}

bool GLGizmoMmuSegmentation::on_key_down_select_tool_type(int keyCode) {
    // B / V switch the panel mode; a brush tool key also goes to Brush.
    if (keyCode == 'B' || keyCode == 'V') {
        set_paint_mode(keyCode == 'B' ? PaintMode::Brush : PaintMode::Vector);
        return true;
    }
    if (keyCode == 'F' || keyCode == 'T' || keyCode == 'S' || keyCode == 'C' || keyCode == 'H' || keyCode == 'G')
        set_paint_mode(PaintMode::Brush);
    switch (keyCode)
    {
    case 'F':
        m_current_tool = ImGui::FillButtonIcon;
        break;
    case 'T':
        m_current_tool = ImGui::TriangleButtonIcon;
        break;
    case 'S':
        m_current_tool = ImGui::SphereButtonIcon;
        break;
    case 'C':
        m_current_tool = ImGui::CircleButtonIcon;
        break;
    case 'H':
        m_current_tool = ImGui::HeightRangeIcon;
        break;
    case 'G':
        m_current_tool = ImGui::GapFillIcon;
        break;
    default:
        return false;
        break;
    }
    return true;
}

static void render_extruders_combo(const std::string& label,
                                   const std::vector<std::string>& extruders,
                                   const std::vector<ColorRGBA>& extruders_colors,
                                   size_t& selection_idx)
{
    assert(!extruders_colors.empty());
    assert(extruders.size() == extruders_colors.size());

    size_t selection_out = selection_idx;
    // It is necessary to use BeginGroup(). Otherwise, when using SameLine() is called, then other items will be drawn inside the combobox.
    ImGui::BeginGroup();
    ImVec2 combo_pos = ImGui::GetCursorScreenPos();
    if (ImGui::BeginCombo(label.c_str(), "")) {
        for (size_t extruder_idx = 0; extruder_idx < extruders.size(); ++extruder_idx) {
            ImGui::PushID(int(extruder_idx));
            ImVec2 start_position = ImGui::GetCursorScreenPos();

            if (ImGui::Selectable("", extruder_idx == selection_idx))
                selection_out = extruder_idx;

            ImGui::SameLine();
            ImGuiStyle &style  = ImGui::GetStyle();
            float       height = ImGui::GetTextLineHeight();
            ImGui::GetWindowDrawList()->AddRectFilled(start_position, ImVec2(start_position.x + height + height / 2, start_position.y + height), ImGuiWrapper::to_ImU32(extruders_colors[extruder_idx]));
            ImGui::GetWindowDrawList()->AddRect(start_position, ImVec2(start_position.x + height + height / 2, start_position.y + height), IM_COL32_BLACK);

            ImGui::SetCursorScreenPos(ImVec2(start_position.x + height + height / 2 + style.FramePadding.x, start_position.y));
            ImGui::Text("%s", extruders[extruder_idx].c_str());
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    ImVec2      backup_pos = ImGui::GetCursorScreenPos();
    ImGuiStyle &style      = ImGui::GetStyle();

    ImGui::SetCursorScreenPos(ImVec2(combo_pos.x + style.FramePadding.x, combo_pos.y + style.FramePadding.y));
    ImVec2 p      = ImGui::GetCursorScreenPos();
    float  height = ImGui::GetTextLineHeight();

    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + height + height / 2, p.y + height), ImGuiWrapper::to_ImU32(extruders_colors[selection_idx]));
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + height + height / 2, p.y + height), IM_COL32_BLACK);

    ImGui::SetCursorScreenPos(ImVec2(p.x + height + height / 2 + style.FramePadding.x, p.y));
    ImGui::Text("%s", extruders[selection_out].c_str());
    ImGui::SetCursorScreenPos(backup_pos);
    ImGui::EndGroup();

    selection_idx = selection_out;
}

void GLGizmoMmuSegmentation::show_tooltip_information(float caption_max, float x, float y)
{
    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(std::string_view{": "}).x + 15.f;

    float  scale       = m_parent.get_scale();
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0}); // ORCA: Dont add padding
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max](const wxString &caption, const wxString &text) {
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        std::vector<std::string> tip_items;
        switch (m_tool_type) {
            case ToolType::BRUSH: 
                tip_items = {"paint", "erase", "cursor_size", "clipping_of_view", "toggle_wireframe"};
                break;
            case ToolType::BUCKET_FILL: 
                tip_items = {"paint", "erase", "smart_fill_angle", "clipping_of_view", "toggle_wireframe"};
                break;
            case ToolType::SMART_FILL:
                // TODO:
                break;
            case ToolType::GAP_FILL:
                tip_items = {"gap_area", "toggle_wireframe"};
                break;
            default:
                break;
        }
        for (const auto &t : tip_items) draw_text_with_caption(m_desc.at(t + "_caption") + ": ", m_desc.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GLGizmoMmuSegmentation::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info()->model_object()) return;

    const float approx_height = m_imgui->scaled(22.0f);
    y = std::min(y, bottom_limit - approx_height);
    GizmoImguiSetNextWIndowPos(x, y, ImGuiCond_Always);

    wchar_t old_tool = m_current_tool;

    // BBS
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float space_size = m_imgui->get_style_scaling() * 8;
    const float clipping_slider_left  = std::max(m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x + m_imgui->scaled(1.5f),
        m_imgui->calc_text_size(m_desc.at("reset_direction")).x + m_imgui->scaled(1.5f) + ImGui::GetStyle().FramePadding.x * 2);
    const float cursor_slider_left = m_imgui->calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.5f);
    const float smart_fill_slider_left = m_imgui->calc_text_size(m_desc.at("smart_fill_angle")).x + m_imgui->scaled(1.5f);
    const float edge_detect_slider_left = m_imgui->calc_text_size(m_desc.at("edge_detection")).x + m_imgui->scaled(1.f);
    const float gap_area_slider_left = m_imgui->calc_text_size(m_desc.at("gap_area")).x + m_imgui->scaled(1.5f) + space_size;
    const float height_range_slider_left = m_imgui->calc_text_size(m_desc.at("height_range")).x + m_imgui->scaled(2.f);

    const float remove_btn_width = m_imgui->calc_text_size(m_desc.at("remove_all")).x + m_imgui->scaled(1.f);
    const float filter_btn_width = m_imgui->calc_text_size(m_desc.at("perform")).x + m_imgui->scaled(1.f);
    const float remap_btn_width = m_imgui->calc_text_size(m_desc.at("perform_remap")).x + m_imgui->scaled(1.f);
    const float buttons_width = remove_btn_width + filter_btn_width + remap_btn_width + m_imgui->scaled(2.f);
    const float minimal_slider_width = m_imgui->scaled(4.f);
    const float color_button_width = m_imgui->calc_text_size(std::string_view{""}).x + m_imgui->scaled(1.75f);
    const size_t total_filament_count = m_extruders_colors.size();
    const size_t num_physical = m_mixed_display_context.num_physical;
    const float  font_size = ImGui::GetFontSize() * 0.85f;
    const std::string max_filament_label = (total_filament_count > num_physical) ? "WW" : std::to_string(std::max<size_t>(total_filament_count, 1));
    const ImVec2 max_filament_label_size = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, max_filament_label.c_str());

    float caption_max = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 6>{"paint", "erase", "cursor_size", "smart_fill_angle", "height_range", "clipping_of_view"}) {
        caption_max = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }
    total_text_max += caption_max + m_imgui->scaled(1.f);
    caption_max += m_imgui->scaled(1.f);

    const float circle_max_width = std::max(clipping_slider_left,cursor_slider_left);
    const float height_max_width = std::max(clipping_slider_left,height_range_slider_left);
    const float sliders_left_width = std::max(smart_fill_slider_left,
                                         std::max(cursor_slider_left, std::max(edge_detect_slider_left, std::max(gap_area_slider_left, std::max(height_range_slider_left,
                                                                                                                                              clipping_slider_left))))) + space_size;
    const float slider_icon_width = m_imgui->get_slider_icon_size().x;
    float window_width = minimal_slider_width + sliders_left_width + slider_icon_width;
    const int max_filament_items_per_line = 8;
    const float empty_button_width = m_imgui->calc_button_size("").x;
    const float filament_button_h = std::round(ImGui::GetFrameHeight() + m_imgui->scaled(0.2f));
    const float filament_button_w = filament_button_h;
    const float filament_item_spacing = m_imgui->scaled(0.75f);
    const float filament_item_width = filament_button_w + filament_item_spacing;

    window_width = std::max(window_width, total_text_max);
    window_width = std::max(window_width, buttons_width);
    window_width = std::max(window_width, max_filament_items_per_line * filament_item_width + m_imgui->scaled(0.5f));

    const float sliders_width = m_imgui->scaled(7.0f);
    const float drag_left_width = ImGui::GetStyle().WindowPadding.x + sliders_width - space_size;

    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;
    ImDrawList * draw_list = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    static float color_button_high  = 25.0;
    draw_list->AddRectFilled({pos.x - 10.0f, pos.y - 7.0f}, {pos.x + window_width + ImGui::GetFrameHeight(), pos.y + color_button_high}, ImGui::GetColorU32(ImGuiCol_FrameBgActive, 1.0f), 5.0f);

    float color_button = ImGui::GetCursorPos().y;

    m_imgui->text(m_desc.at("filaments"));

    float start_pos_x = ImGui::GetCursorPos().x;
    size_t n_extruder_colors = std::min(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT, m_display_filament_ids.size());
    const bool separate_mixed_row = (n_extruder_colors > 10);
    int current_col = 0;
    bool prev_was_physical = false;

    for (size_t extruder_idx = 0; extruder_idx < n_extruder_colors; ++extruder_idx) {
        const unsigned int actual_filament_id = m_display_filament_ids[extruder_idx];
        if (actual_filament_id == 0 || actual_filament_id > m_extruders_colors.size())
            continue;
        const bool is_mixed = (actual_filament_id > num_physical);

        if (separate_mixed_row && is_mixed && prev_was_physical && current_col > 0) {
            current_col = 0;
        }
        prev_was_physical = !is_mixed;

        const ColorRGBA &extruder_color = m_extruders_colors[actual_filament_id - 1];
        ImVec4           color_vec      = ImGuiWrapper::to_ImVec4(extruder_color);
        std::string color_label = std::string("##extruder color ") + std::to_string(extruder_idx);
        std::string item_text = (!is_mixed)
                                    ? std::to_string(actual_filament_id)
                                    : Slic3r::mixed_filament_index_to_letter(actual_filament_id - num_physical - 1);

        const ImVec2 button_size(filament_button_w, filament_button_h);

        if (current_col > 0) {
            float button_offset = start_pos_x + filament_item_width * current_col;
            ImGui::SameLine(button_offset);
        }

        // Multi-component blends also occupy legacy gradient fields, so the typed behavior flag is authoritative here.
        const std::optional<MixedFilamentDefinition> mixed_definition =
            wxGetApp().preset_bundle != nullptr ?
                wxGetApp().preset_bundle->mixed_filaments.mixed_filament_definition_from_id(
                    actual_filament_id, m_mixed_display_context.num_physical) :
                std::nullopt;
        const bool is_gradient = mixed_definition && is_gradient_definition(*mixed_definition);

        if (is_gradient) {
            // --- Gradient path: ColorButton for sizing + interaction, then draw gradient on top ---
            const std::vector<ImU32> gradient_stops =
                mixed_filament_gradient_stops_ImU32(*mixed_definition, m_mixed_display_context);

            int avg_r = 0;
            int avg_g = 0;
            int avg_b = 0;
            for (const ImU32 stop : gradient_stops) {
                avg_r += int((stop >> IM_COL32_R_SHIFT) & 0xFF);
                avg_g += int((stop >> IM_COL32_G_SHIFT) & 0xFF);
                avg_b += int((stop >> IM_COL32_B_SHIFT) & 0xFF);
            }
            const int stop_count = std::max(1, int(gradient_stops.size()));
            avg_r /= stop_count;
            avg_g /= stop_count;
            avg_b /= stop_count;
            ImVec4 avg_color_vec(float(avg_r) / 255.f, float(avg_g) / 255.f, float(avg_b) / 255.f, 1.f);

            // ColorButton provides exact sizing, click detection, border, and cursor advance
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip;
            if (m_selected_extruder_idx != extruder_idx) flags |= ImGuiColorEditFlags_NoBorder;
            #ifdef __APPLE__
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0);
                bool color_picked = ImGui::ColorButton(color_label.c_str(), avg_color_vec, flags, button_size);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(1);
            #else
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0);
                bool color_picked = ImGui::ColorButton(color_label.c_str(), avg_color_vec, flags, button_size);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(1);
            #endif
            color_button_high = ImGui::GetCursorPos().y - color_button - 2.0;
            if (color_picked) { m_selected_extruder_idx = extruder_idx; }

            // Draw gradient slightly inset to match solid color block visual size
            ImVec2 btn_min = ImGui::GetItemRectMin();
            ImVec2 btn_max = ImGui::GetItemRectMax();
            const float inset = 2.5f;
            ImVec2 fill_min(btn_min.x + inset, btn_min.y + inset);
            ImVec2 fill_max(btn_max.x - inset, btn_max.y - inset);
            float fill_h = fill_max.y - fill_min.y;

            // Per-scanline gradient fill (pos=0 bottom, pos=1 top)
            if (fill_h > 0.0f) {
                for (int y_i = 0; y_i < int(std::round(fill_h)); ++y_i) {
                    double pos = 1.0 - double(y_i) / double(fill_h);
                    ImU32 c = interpolate_ImU32(gradient_stops, pos);
                    draw_list->AddRectFilled(
                        ImVec2(fill_min.x, fill_min.y + float(y_i)),
                        ImVec2(fill_max.x, fill_min.y + float(y_i + 1)), c);
                }
            }

            // Very-light gradient border
            const bool all_stops_light = !gradient_stops.empty() &&
                std::all_of(gradient_stops.begin(), gradient_stops.end(), [](ImU32 color) { return very_light(color); });
            if (all_stops_light)
                draw_list->AddRect(fill_min, fill_max, IM_COL32(128, 128, 128, 255), ImGui::GetStyle().FrameRounding, 0, 1.0f);

            // Tooltip
            if (ImGui::IsItemHovered()) {
                if (extruder_idx < 9)
                    m_imgui->tooltip(_L("Shortcut Key ") + std::to_string(extruder_idx + 1), max_tooltip_width);
                else if (actual_filament_id <= num_physical)
                    m_imgui->tooltip(wxString::Format(_L("Filament %d"), int(actual_filament_id)), max_tooltip_width);
                else
                    m_imgui->tooltip(wxString::Format(_L("Mixed Filament %s"), item_text), max_tooltip_width);
            }

            // Number text centered on button
            float lum_avg = 0.0f;
            for (const ImU32 stop : gradient_stops)
                lum_avg += bT601_luminance(stop);
            lum_avg /= float(stop_count);
            ImU32 text_col = (lum_avg < 0.51f) ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
            float text_w = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, item_text.c_str()).x;
            draw_list->AddText(ImGui::GetFont(), font_size,
                ImVec2(btn_min.x + (btn_max.x - btn_min.x - text_w) / 2.f,
                       btn_min.y + (btn_max.y - btn_min.y - font_size) / 2.f),
                text_col, item_text.c_str());

        } else {
            // draw filament background
            ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip;
            if (m_selected_extruder_idx != extruder_idx) flags |= ImGuiColorEditFlags_NoBorder;
            #ifdef __APPLE__
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA); // ORCA use orca color for selected filament border
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0);
                bool color_picked = ImGui::ColorButton(color_label.c_str(), color_vec, flags, button_size);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(1);
            #else
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA); // ORCA use orca color for selected filament border
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0);
                bool color_picked = ImGui::ColorButton(color_label.c_str(), color_vec, flags, button_size);
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor(1);
            #endif
            color_button_high = ImGui::GetCursorPos().y - color_button - 2.0;
            if (color_picked) { m_selected_extruder_idx = extruder_idx; }

            if (ImGui::IsItemHovered()) {
                if (extruder_idx < 9)
                    m_imgui->tooltip(_L("Shortcut Key ") + std::to_string(extruder_idx + 1), max_tooltip_width);
                else if (actual_filament_id <= num_physical)
                    m_imgui->tooltip(wxString::Format(_L("Filament %d"), int(actual_filament_id)), max_tooltip_width);
                else
                    m_imgui->tooltip(wxString::Format(_L("Mixed Filament %s"), item_text), max_tooltip_width);
            }

            // draw filament id centered on button
            ImVec2 btn_min = ImGui::GetItemRectMin();
            ImVec2 btn_max = ImGui::GetItemRectMax();
            float gray = 0.299f * extruder_color.r() + 0.587f * extruder_color.g() + 0.114f * extruder_color.b();
            ImU32 text_col = (gray * 255.f < 130.f) ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);
            float text_w = ImGui::GetFont()->CalcTextSizeA(font_size, FLT_MAX, 0.0f, item_text.c_str()).x;
            draw_list->AddText(ImGui::GetFont(), font_size,
                ImVec2(btn_min.x + (btn_max.x - btn_min.x - text_w) / 2.f,
                       btn_min.y + (btn_max.y - btn_min.y - font_size) / 2.f),
                text_col, item_text.c_str());
        }

        current_col++;
        if (current_col >= max_filament_items_per_line) {
            current_col = 0;
        }
    }
    //ImGui::NewLine();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (n_extruder_colors > 0) {
        int selected_filament = int(m_selected_extruder_idx) + 1;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Selected filament"));
        ImGui::SameLine();
        ImGui::PushItemWidth(m_imgui->scaled(4.5f));
        if (ImGui::InputInt("##selected_filament", &selected_filament, 1, 10, ImGuiInputTextFlags_CharsDecimal)) {
            selected_filament = std::clamp(selected_filament, 1, int(n_extruder_colors));
            m_selected_extruder_idx = size_t(selected_filament - 1);
        }
        ImGui::SameLine();
        m_imgui->text(wxString::Format(_L("/ %d"), int(n_extruder_colors)));
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
    }

    // Brush | Vector. Same filaments for both; each mode shows only its own tools.
    {
        auto mode_button = [this](const wxString &label, PaintMode mode, const wxString &tip) {
            const bool on = m_paint_mode == mode;
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f, 0.59f, 0.53f, 0.35f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.59f, 0.53f, 0.45f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.f, 0.59f, 0.53f, 0.55f));
                ImGui::PushStyleColor(ImGuiCol_Border,        ImGuiWrapper::COL_ORCA);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            if (m_imgui->button(label))
                set_paint_mode(mode);
            if (on) {
                ImGui::PopStyleVar(1);
                ImGui::PopStyleColor(4);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", into_u8(tip).c_str());
        };
        mode_button(_L("Brush"), PaintMode::Brush, _L("Paint by hand: brushes, fill, height range.\nKey: B"));
        ImGui::SameLine();
        mode_button(_L("Vector"), PaintMode::Vector, _L("Shapes and SVG decals, applied in one go.\nKey: V"));
        // Vector mode: a shape starts only from here, so a plain drag keeps orbiting the camera.
        if (m_paint_mode == PaintMode::Vector) {
            const bool armed = m_shape_draw_armed || !m_shape_nodes.empty();
            if (armed) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.59f, 0.53f, 0.35f));
                ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORCA);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            }
            m_imgui->disabled_begin(!m_svg_zones.empty() || !m_shape_nodes.empty());
            if (m_imgui->button(into_u8(std::wstring(1, ImGui::NeotkoDrawShapeIcon)) + " " + _u8L("Draw shape"))) {
                m_shape_draw_armed = !m_shape_draw_armed;
                m_parent.set_as_dirty();
            }
            m_imgui->disabled_end();
            if (armed) {
                ImGui::PopStyleVar(1);
                ImGui::PopStyleColor(2);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Then click on the object to place the points.\n"
                    "Click: corner point.  Click and drag: curved point.\n"
                    "Click the first point to close the shape.\n"
                    "Drag points or handles to edit.  Click on the outline to add a point.\n"
                    "Alt+click a point: corner <-> curve.  Alt+drag a handle: break the curve.\n"
                    "Right click: cancel.").c_str());
            if (m_shape_draw_armed && m_shape_nodes.empty()) {
                ImGui::SameLine();
                m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Click on the object."));
            }
        }
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));
    }

    if (m_paint_mode == PaintMode::Brush) {   // Orca's brush tools
    m_imgui->text(m_desc.at("tool_type"));

    std::array<wchar_t, 6> tool_ids;
    tool_ids = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::HeightRangeIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };
    std::array<wchar_t, 6> icons;
    if (m_is_dark_mode)
        icons = { ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon, ImGui::TriangleButtonDarkIcon, ImGui::HeightRangeDarkIcon, ImGui::FillButtonDarkIcon, ImGui::GapFillDarkIcon };
    else
        icons = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon, ImGui::TriangleButtonIcon, ImGui::HeightRangeIcon, ImGui::FillButtonIcon, ImGui::GapFillIcon };
    std::array<wxString, 6> tool_tips = { _L("Circle"), _L("Sphere"), _L("Triangle"), _L("Height Range"), _L("Fill"), _L("Gap Fill") };
    for (int i = 0; i < tool_ids.size(); i++) {
        std::string  str_label = std::string("");
        std::wstring btn_name  = icons[i] + boost::nowide::widen(str_label);

        if (i != 0) ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));                     // ORCA Removes button background on dark mode
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));                       // ORCA Fixes icon rendered without colors while using Light theme
        if (m_current_tool == tool_ids[i]) {
            ImGui::PushStyleColor(ImGuiCol_Button,          ImVec4(0.f, 0.59f, 0.53f, 0.25f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,   ImVec4(0.f, 0.59f, 0.53f, 0.25f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,    ImVec4(0.f, 0.59f, 0.53f, 0.30f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_Border,          ImGuiWrapper::COL_ORCA);            // ORCA use orca color for border on selected tool / brush
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0);
        }
        bool btn_clicked = ImGui::Button(into_u8(btn_name).c_str());
        if (m_current_tool == tool_ids[i])
        {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(1);

        if (btn_clicked) {
            const bool changed_tool = m_current_tool != tool_ids[i];
            m_current_tool = tool_ids[i];
            m_rectangle_mask_active = false;
            m_polygon_mask_active = false;
            m_rect_dragging = false;
            shape_clear();
            if (changed_tool) {
                for (auto &triangle_selector : m_triangle_selectors) {
                    triangle_selector->seed_fill_unselect_all_triangles();
                    triangle_selector->request_update_render_data();
                }
            }
        }

        if (ImGui::IsItemHovered()) {
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool != old_tool)
        this->tool_changed(old_tool, m_current_tool);

    if (m_current_tool == ImGui::CircleButtonIcon || m_current_tool == ImGui::SphereButtonIcon) {
        if (m_current_tool == ImGui::CircleButtonIcon)
            m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        else
             m_cursor_type = TriangleSelector::CursorType::SPHERE;
        m_tool_type = ToolType::BRUSH;

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("cursor_size"));
        ImGui::SameLine(circle_max_width);
        ImGui::PushItemWidth(sliders_width);
        m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + circle_max_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");

        ImGui::Separator();
        if (m_c->object_clipper()->get_position() == 0.f) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("clipping_of_view"));
        }
        else {
            if (m_imgui->button(m_desc.at("reset_direction"))) {
                wxGetApp().CallAfter([this]() {
                    m_c->object_clipper()->set_position_by_ratio(-1., false);
                    });
            }
        }

        auto clp_dist = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(circle_max_width);
        ImGui::PushItemWidth(sliders_width);
        bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + circle_max_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position_by_ratio(clp_dist, true); }

    } else if (m_current_tool == ImGui::TriangleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_tool_type   = ToolType::BRUSH;

        if (m_c->object_clipper()->get_position() == 0.f) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("clipping_of_view"));
        }
        else {
            if (m_imgui->button(m_desc.at("reset_direction"))) {
                wxGetApp().CallAfter([this]() {
                    m_c->object_clipper()->set_position_by_ratio(-1., false);
                    });
            }
        }

        auto clp_dist = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(clipping_slider_left);
        ImGui::PushItemWidth(sliders_width);
        bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + clipping_slider_left);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position_by_ratio(clp_dist, true); }

    } else if (m_current_tool == ImGui::FillButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        m_imgui->bbl_checkbox(m_desc["edge_detection"], m_detect_geometry_edge);
        m_tool_type = ToolType::BUCKET_FILL;

        if (m_detect_geometry_edge) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc["smart_fill_angle"]);
            std::string format_str = std::string("%.f") + I18N::translate_utf8("°", "Face angle threshold,"
                                                                                    "placed after the number with no whitespace in between.");
            ImGui::SameLine(sliders_left_width);
            ImGui::PushItemWidth(sliders_width);
            if (m_imgui->bbl_slider_float_style("##smart_fill_angle", &m_smart_fill_angle, SmartFillAngleMin, SmartFillAngleMax, format_str.data(), 1.0f, true))
                for (auto &triangle_selector : m_triangle_selectors) {
                    triangle_selector->seed_fill_unselect_all_triangles();
                    triangle_selector->request_update_render_data();
                }
            ImGui::SameLine(drag_left_width + sliders_left_width);
            ImGui::PushItemWidth(1.5 * slider_icon_width);
            ImGui::BBLDragFloat("##smart_fill_angle_input", &m_smart_fill_angle, 0.05f, 0.0f, 0.0f, "%.2f");
        } else {
            // set to negative value to disable edge detection
            m_smart_fill_angle = -1.f;
        }
        ImGui::Separator();
        if (m_c->object_clipper()->get_position() == 0.f) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("clipping_of_view"));
        }
        else {
            if (m_imgui->button(m_desc.at("reset_direction"))) {
                wxGetApp().CallAfter([this]() {
                    m_c->object_clipper()->set_position_by_ratio(-1., false);
                    });
            }
        }

        auto clp_dist = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(sliders_left_width);
        ImGui::PushItemWidth(sliders_width);
        bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + sliders_left_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position_by_ratio(clp_dist, true);}

    } else if (m_current_tool == ImGui::HeightRangeIcon) {
        m_tool_type   = ToolType::BRUSH;
        m_cursor_type = TriangleSelector::CursorType::HEIGHT_RANGE;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["height_range"] + ":");
        ImGui::SameLine(height_max_width);
        ImGui::PushItemWidth(sliders_width);
        std::string format_str = std::string("%.2f") + I18N::translate_utf8("mm", "Heigh range," "Facet in [cursor z, cursor z + height] will be selected.");
        m_imgui->bbl_slider_float_style("##cursor_height", &m_cursor_height, CursorHeightMin, CursorHeightMax, format_str.data(), 1.0f, true);
        ImGui::SameLine(drag_left_width + height_max_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##cursor_height_input", &m_cursor_height, 0.05f, 0.0f, 0.0f, "%.2f");

        ImGui::Separator();
        if (m_c->object_clipper()->get_position() == 0.f) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("clipping_of_view"));
        }
        else {
            if (m_imgui->button(m_desc.at("reset_direction"))) {
                wxGetApp().CallAfter([this]() {
                    m_c->object_clipper()->set_position_by_ratio(-1., false);
                    });
            }
        }

        auto clp_dist = float(m_c->object_clipper()->get_position());
        ImGui::SameLine(height_max_width);
        ImGui::PushItemWidth(sliders_width);
        bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);
        ImGui::SameLine(drag_left_width + height_max_width);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");

        if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position_by_ratio(clp_dist, true); }
    }
    else if (m_current_tool == ImGui::GapFillIcon) {
        m_tool_type = ToolType::GAP_FILL;
        m_cursor_type = TriangleSelector::CursorType::POINTER;
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc["gap_area"] + ":");
        ImGui::SameLine(gap_area_slider_left);
        ImGui::PushItemWidth(sliders_width);
        std::string format_str = std::string("%.2f") + I18N::translate_utf8("", "Triangle patch area threshold,""triangle patch will be merged to neighbor if its area is less than threshold");
        m_imgui->bbl_slider_float_style("##gap_area", &TriangleSelectorPatch::gap_area, TriangleSelectorPatch::GapAreaMin, TriangleSelectorPatch::GapAreaMax, format_str.data(), 1.0f, true);
        ImGui::SameLine(drag_left_width + gap_area_slider_left);
        ImGui::PushItemWidth(1.5 * slider_icon_width);
        ImGui::BBLDragFloat("##gap_area_input", &TriangleSelectorPatch::gap_area, 0.05f, 0.0f, 0.0f, "%.2f");
    }

    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Paint precision"));
    ImGui::SameLine(sliders_left_width);
    ImGui::PushItemWidth(sliders_width);
    m_imgui->bbl_slider_float_style("##paint_precision", &m_precision_factor, PrecisionFactorMin, PrecisionFactorMax,
                                    "%.0fx", 1.f, true);
    ImGui::SameLine(drag_left_width + sliders_left_width);
    ImGui::PushItemWidth(1.5f * slider_icon_width);
    ImGui::BBLDragFloat("##paint_precision_input", &m_precision_factor, 0.5f, PrecisionFactorMin, PrecisionFactorMax, "%.0fx");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Subdivides the mesh more finely at painted boundaries. Higher values use more CPU and memory.").c_str());

    if (m_imgui->bbl_checkbox(_L("Rectangle mask"), m_rectangle_mask_active)) {
        m_rect_dragging = false;
        shape_clear();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Drag a screen-space rectangle to paint the facets you can see inside it. Shift-drag erases; right-click cancels.").c_str());


    ImGui::Separator();
    if(m_imgui->bbl_checkbox(_L("Vertical"), m_vertical_only)){
        if(m_vertical_only){
            m_horizontal_only = false;
        }
    }
    if(m_imgui->bbl_checkbox(_L("Horizontal"), m_horizontal_only)){
        if(m_horizontal_only){
            m_vertical_only = false;
        }
    }
    } // end of Brush mode tools

    ImGui::Separator();
    // Vector mode's own panel.
    if (m_paint_mode == PaintMode::Vector) {
        render_vector_panel();
        ImGui::Separator();
    }
    // Brush symmetry: every stroke repeated across the mirror planes.
    if (m_paint_mode == PaintMode::Brush) {
        ModelObject *sym_mo = m_c->selection_info()->model_object();
        m_imgui->text(_L("Mirror brush"));
        for (int axis = 0; axis < 3; ++axis) {
            if (axis > 0)
                ImGui::SameLine();
            bool on = (sym_mo->paint_symmetry.brush & (1 << axis)) != 0;
            if (m_imgui->bbl_checkbox(axis == 0 ? _L("Left/Right") : axis == 1 ? _L("Front/Back") : _L("Top/Bottom"), on)) {
                Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Mirror brush"), UndoRedo::SnapshotType::GizmoAction);
                sym_mo->paint_symmetry.brush = on ? (sym_mo->paint_symmetry.brush | (1 << axis)) : (sym_mo->paint_symmetry.brush & ~(1 << axis));
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Every brush stroke is painted again on the mirrored side,\n"
                    "through the orange mark (the same centre as the decal mirrors).\n"
                    "Dashed rings show where the copies land. Where the object is not\n"
                    "symmetric and has nothing on the other side, the copy is skipped.\n"
                    "Works with the Circle and Sphere brushes.").c_str());
        }
        if (sym_mo->paint_symmetry.brush != 0)
            render_mirror_centre_controls();
        ImGui::Separator();
    }
    if (m_paint_mode == PaintMode::Brush && !m_c->selection_info()->model_object()->paint_decals.empty()) {
        if (ImGui::Checkbox(_u8L("Show decals").c_str(), &m_show_decals_in_brush))
            m_parent.set_as_dirty();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Show the SVG decals of the Vector mode while you paint,\n"
                "as a reference. They are not painted until you Apply them.").c_str());
        ImGui::Separator();
    }
    // Vector mode is the shape tool; Brush mode never is.
    if (m_paint_mode == PaintMode::Vector) {
        m_rectangle_mask_active = false;
        m_polygon_mask_active   = true;
        m_tool_type             = ToolType::POLYGON;
    } else {
        m_polygon_mask_active = false;
        if (m_rectangle_mask_active)
            m_tool_type = ToolType::RECTANGLE;
    }

    ImGui::Separator();


    if (m_imgui->button(m_desc.at("perform_remap"))) {
        m_show_filament_remap_ui = !m_show_filament_remap_ui;
        if (m_show_filament_remap_ui) {
            // reset remap to identity on opening
            m_extruder_remap.resize(m_extruders_colors.size());
            for (size_t i = 0; i < m_extruder_remap.size(); ++i)
                m_extruder_remap[i] = i;
        }
    }
    
    // Render filament swap UI if enabled
    if (m_show_filament_remap_ui) {
        ImGui::Separator();
        render_filament_remap_ui(window_width, max_tooltip_width);
    }
    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(caption_max, x, get_cur_y);

    float f_scale =m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::SameLine();

    if (m_current_tool == ImGui::GapFillIcon) {
        if (m_imgui->button(m_desc.at("perform"))) {
            Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Gap fill", UndoRedo::SnapshotType::GizmoAction);

            for (int i = 0; i < m_triangle_selectors.size(); i++) {
                TriangleSelectorPatch* ts_mm = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[i].get());
                if (!ts_mm) continue;
                ts_mm->update_selector_triangles();
                ts_mm->request_update_render_data(true);
            }
            update_model_object();
            m_parent.set_as_dirty();
        }

        ImGui::SameLine();
    }

    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);
        ModelObject *        mo  = m_c->selection_info()->model_object();
        int                  idx = -1;
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }

        update_model_object();
        m_parent.set_as_dirty();
    }
    ImGui::PopStyleVar(2);
    GizmoImguiEnd();

    // BBS
    ImGuiWrapper::pop_toolbar_style();
}


void GLGizmoMmuSegmentation::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->mmu_segmentation_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        const size_t num_physical = static_cast<size_t>(std::max(wxGetApp().filaments_cnt(), 0));
        size_t       num_total    = num_physical;
        if (wxGetApp().preset_bundle != nullptr)
            num_total = wxGetApp().preset_bundle->mixed_filaments.total_filaments(num_physical);

        size_t max_used_state = 0;
        for (const ModelVolume *mv : mo->volumes) {
            if (!mv->is_model_part())
                continue;
            const auto &used_states = mv->mmu_segmentation_facets.get_data().used_states;
            for (size_t state_idx = static_cast<size_t>(EnforcerBlockerType::Extruder1); state_idx < used_states.size(); ++state_idx) {
                if (used_states[state_idx])
                    max_used_state = std::max(max_used_state, state_idx);
            }
        }

        if (max_used_state > num_physical) {
            BOOST_LOG_TRIVIAL(warning) << "GLGizmoMmuSegmentation::update_model_object painted virtual extruder state detected"
                                       << " max_used_state=" << max_used_state
                                       << " physical_filaments=" << num_physical
                                       << " total_filaments=" << num_total;
        }

        const ModelObjectPtrs &mos = wxGetApp().model().objects;
        size_t obj_idx = std::find(mos.begin(), mos.end(), mo) - mos.begin();
        wxGetApp().obj_list()->update_info_items(obj_idx);
        wxGetApp().plater()->get_partplate_list().notify_instance_update(obj_idx, 0);
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
        wxGetApp().plater()->notify_filament_usage_changed();
    }
}

void GLGizmoMmuSegmentation::init_model_triangle_selectors()
{
    const ModelObject *mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();
    m_volumes_extruder_idxs.clear();

    // Don't continue when extruders colors are not initialized
    if(m_extruders_colors.empty())
        return;

    // BBS: Don't continue when model object is null
    if (mo == nullptr)
        return;

    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;

        int extruder_idx = (mv->extruder_id() > 0) ? mv->extruder_id() - 1 : 0;
        extruder_idx = std::min(extruder_idx, int(m_extruders_colors.size()) - 1);
        std::vector<ColorRGBA> ebt_colors;
        ebt_colors.push_back(m_extruders_colors[size_t(extruder_idx)]);
        ebt_colors.insert(ebt_colors.end(), m_extruders_colors.begin(), m_extruders_colors.end());

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();
        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors, 0.2));
        // Reset of TriangleSelector is done inside TriangleSelectorMmGUI's constructor, so we don't need it to perform it again in deserialize().
        EnforcerBlockerType max_ebt = (EnforcerBlockerType)std::min(m_extruders_colors.size(), (size_t)EnforcerBlockerType::ExtruderMax);
        m_triangle_selectors.back()->deserialize(mv->mmu_segmentation_facets.get_data(), false, max_ebt);
        m_triangle_selectors.back()->request_update_render_data();
        m_triangle_selectors.back()->set_wireframe_needed(true);
        m_volumes_extruder_idxs.push_back(mv->extruder_id());
    }
}

void GLGizmoMmuSegmentation::update_triangle_selectors_colors()
{
    if (m_extruders_colors.empty())
        return;
    for (int i = 0; i < m_triangle_selectors.size(); i++) {
        TriangleSelectorPatch* selector = dynamic_cast<TriangleSelectorPatch*>(m_triangle_selectors[i].get());
        if (!selector) continue;
        int extruder_idx = m_volumes_extruder_idxs[i];
        int extruder_color_idx = std::max(0, extruder_idx - 1);
        extruder_color_idx = std::min(extruder_color_idx, int(m_extruders_colors.size()) - 1);
        std::vector<ColorRGBA> ebt_colors;
        ebt_colors.push_back(m_extruders_colors[extruder_color_idx]);
        ebt_colors.insert(ebt_colors.end(), m_extruders_colors.begin(), m_extruders_colors.end());
        selector->set_ebt_colors(ebt_colors);
    }
}

void GLGizmoMmuSegmentation::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    // Extruder colors need to be reloaded before calling init_model_triangle_selectors to render painted triangles
    // using colors from loaded 3MF and not from printer profile in Slicer.
    const std::vector<ColorRGBA> current_extruder_colors = get_extruders_colors();
    if (int prev_extruders_count = int(m_extruders_colors.size());
        prev_extruders_count != int(current_extruder_colors.size()) || current_extruder_colors != m_extruders_colors)
        this->init_extruders_data(current_extruder_colors);

    this->init_model_triangle_selectors();
}

void GLGizmoMmuSegmentation::tool_changed(wchar_t old_tool, wchar_t new_tool)
{
    if ((old_tool == ImGui::GapFillIcon && new_tool == ImGui::GapFillIcon) ||
        (old_tool != ImGui::GapFillIcon && new_tool != ImGui::GapFillIcon))
        return;

    for (auto& selector_ptr : m_triangle_selectors) {
        TriangleSelectorPatch* tsp = dynamic_cast<TriangleSelectorPatch*>(selector_ptr.get());
        if (!tsp) continue;
        tsp->set_filter_state(new_tool == ImGui::GapFillIcon);
    }
}

PainterGizmoType GLGizmoMmuSegmentation::get_painter_type() const
{
    return PainterGizmoType::MM_SEGMENTATION;
}

// BBS
ColorRGBA GLGizmoMmuSegmentation::get_cursor_hover_color() const
{
    if (m_selected_extruder_idx < m_display_filament_ids.size()) {
        const unsigned int actual_filament_id = m_display_filament_ids[m_selected_extruder_idx];
        if (actual_filament_id >= 1 && actual_filament_id <= m_extruders_colors.size())
            return m_extruders_colors[actual_filament_id - 1];
    }
    return m_extruders_colors.empty() ? ColorRGBA() : m_extruders_colors[0];
}

void GLGizmoMmuSegmentation::on_set_state()
{
    GLGizmoPainterBase::on_set_state();

    if (get_state() == Off) {
        ModelObject* mo = m_c->selection_info()->model_object();
        if (mo) Slic3r::save_object_mesh(*mo);
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_FORCE_UPDATE));
    }
}

ColorRGBA GLGizmoMmuSegmentation::get_shape_fill_color() const
{
    ColorRGBA color = m_selected_extruder_idx < m_extruders_colors.size() ?
        m_extruders_colors[m_selected_extruder_idx] : ColorRGBA(0.0f, 1.0f, 0.3f, 1.0f);
    color.a(0.35f);
    return color;
}

// B / V. Leaving Vector keeps the shape or SVG being edited: it only stops
// being drawn and grabbed until Vector comes back.
void GLGizmoMmuSegmentation::set_paint_mode(PaintMode mode)
{
    if (m_paint_mode == mode)
        return;
    m_paint_mode = mode;
    m_parent.set_as_dirty();
}

// Brush mode only: drag a box on screen and paint what is under it.
bool GLGizmoMmuSegmentation::render_svg_colour_row(int id, uint32_t rgb, bool grad, int &state, bool &hidden)
{
    bool changed = false;
    const float row_start = ImGui::GetCursorScreenPos().x;
    const size_t n_filaments = std::min(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT, m_display_filament_ids.size());
    auto filament_label = [this, n_filaments](int s) {
        for (size_t i = 0; i < n_filaments; ++i)
            if (int(m_display_filament_ids[i]) == s)
                return std::to_string(i + 1);
        return std::string("?");
    };
    const ImGuiColorEditFlags swatch_flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop;
    const ImVec2 swatch_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    ImGui::PushID(id);
    const ImVec4 svg_col(float((rgb >> 16) & 0xFF) / 255.f, float((rgb >> 8) & 0xFF) / 255.f, float(rgb & 0xFF) / 255.f, 1.f);
    ImGui::ColorButton("##svg_colour", svg_col, swatch_flags, swatch_size);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", (grad ? _u8L("Colour in the SVG (a gradient: its mean colour)") : _u8L("Colour in the SVG")).c_str());
    ImGui::SameLine();
    m_imgui->text(">");
    ImGui::SameLine();
    ImGui::ColorButton("##filament_colour", ImGuiWrapper::to_ImVec4(get_state_color(state)), swatch_flags, swatch_size);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(m_imgui->scaled(3.5f));
    if (ImGui::BeginCombo("##filament", filament_label(state).c_str())) {
        for (size_t i = 0; i < n_filaments; ++i) {
            const int s = int(m_display_filament_ids[i]);
            ImGui::PushID(int(i));
            ImGui::ColorButton("##c", ImGuiWrapper::to_ImVec4(get_state_color(s)), swatch_flags, swatch_size);
            ImGui::SameLine();
            if (ImGui::Selectable(std::to_string(i + 1).c_str(), s == state)) {
                state   = s;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Filament this colour paints with.").c_str());
    ImGui::SameLine();
    bool visible = !hidden;
    if (ImGui::Checkbox("##visible", &visible)) {
        hidden  = !visible;
        changed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Off: this colour is not painted and lets through\nwhat the SVG has under it.").c_str());
    m_colour_row_w = std::max(m_colour_row_w, ImGui::GetItemRectMax().x - row_start);
    ImGui::PopID();
    return changed;
}

// Pick on object / Box centre, shared by the decals and the brush.
// Pick on object / Box centre, shared by the decals and the brush.
void GLGizmoMmuSegmentation::render_mirror_centre_controls()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    auto snapshot = [](const std::string &name) { Plater::TakeSnapshot(wxGetApp().plater(), name, UndoRedo::SnapshotType::GizmoAction); };
    m_imgui->text(_L("Mirror centre"));
    ImGui::SameLine();
    const bool picking = m_mirror_pick_armed;
    if (picking) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.59f, 0.53f, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORCA);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    }
    if (m_imgui->button(_L("Pick on object"))) {
        m_mirror_pick_armed = !m_mirror_pick_armed;
        m_parent.set_as_dirty();
    }
    if (picking) {
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Then click on the object: the mirror planes cross there (the orange mark).\n"
            "For an object that is not centred in its box, like a face turned a little.").c_str());
    if (mo->paint_symmetry.custom_centre) {
        ImGui::SameLine();
        if (m_imgui->button(_L("Box centre"))) {
            snapshot(_u8L("Mirror centre"));
            mo->paint_symmetry.custom_centre = false;
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Back to the centre of the object's box.").c_str());
    }
    if (picking)
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Click on the object."));
}

bool GLGizmoMmuSegmentation::render_part_filament_combo(const char *id, int &state)
{
    bool changed = false;
    const size_t n_filaments = std::min(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT, m_display_filament_ids.size());
    std::string label = _u8L("Colour");
    for (size_t i = 0; i < n_filaments; ++i)
        if (int(m_display_filament_ids[i]) == state)
            label = std::to_string(i + 1);
    const ImGuiColorEditFlags swatch_flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop;
    const ImVec2 swatch_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    ImGui::SetNextItemWidth(m_imgui->scaled(5.f));
    if (ImGui::BeginCombo(id, label.c_str())) {
        if (ImGui::Selectable(_u8L("Colour").c_str(), state < 0)) {
            state   = -1;
            changed = true;
        }
        for (size_t i = 0; i < n_filaments; ++i) {
            const int s = int(m_display_filament_ids[i]);
            ImGui::PushID(int(i));
            ImGui::ColorButton("##c", ImGuiWrapper::to_ImVec4(get_state_color(s)), swatch_flags, swatch_size);
            ImGui::SameLine();
            if (ImGui::Selectable(std::to_string(i + 1).c_str(), s == state)) {
                state   = s;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

bool GLGizmoMmuSegmentation::render_svg_parts()
{
    if (m_svg_shapes.size() < 2)
        return false;   // one shape: its colour row already is the part
    bool changed = false;
    const int hover_before = m_svg_part_hover;
    m_svg_part_hover = -1;
    ImGui::PushID("svg_parts");
    m_imgui->text(GUI::format(_L("Parts: %1%"), m_svg_shapes.size()));
    ImGui::SameLine();
    if (m_imgui->bbl_checkbox(_L("Pick parts"), m_svg_pick_parts))
        m_parent.set_as_dirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Click a part of the SVG to select it, Shift + click to add or remove one.\n"
            "While this is on, dragging does not move the SVG.").c_str());

    const ImGuiColorEditFlags swatch_flags = ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop;
    const ImVec2 swatch_size(ImGui::GetFrameHeight(), ImGui::GetFrameHeight());
    auto zone_colour = [this](int shape) {
        const int z = m_svg_shapes[size_t(shape)].zone;
        const uint32_t rgb = (z >= 0 && size_t(z) < m_svg_zones.size()) ? m_svg_zones[size_t(z)].rgb : 0;
        return ImVec4(float((rgb >> 16) & 0xFF) / 255.f, float((rgb >> 8) & 0xFF) / 255.f, float(rgb & 0xFF) / 255.f, 1.f);
    };

    // The selection: one filament and one eye for all of it.
    if (!m_svg_part_sel.empty()) {
        m_imgui->text(GUI::format(_L("Selected: %1%"), m_svg_part_sel.size()));
        ImGui::SameLine();
        const PaintDecalPart *first = svg_part(*m_svg_part_sel.begin(), false);
        int  state  = first ? first->state : -1;
        bool hidden = first ? first->hidden : false;
        if (render_part_filament_combo("##sel_filament", state)) {
            for (int k : m_svg_part_sel)
                svg_part(k, true)->state = state;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Filament the selected parts paint with.\nColour: the one of their colour row.").c_str());
        ImGui::SameLine();
        bool visible = !hidden;
        if (ImGui::Checkbox("##sel_visible", &visible)) {
            for (int k : m_svg_part_sel)
                svg_part(k, true)->hidden = !visible;
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Off: the selected parts are not painted and let through\nwhat the SVG has under them.").c_str());
        ImGui::SameLine();
        if (m_imgui->button(_L("Reset"))) {
            for (int k : m_svg_part_sel)
                if (PaintDecalPart *p = svg_part(k, false)) {
                    p->state  = -1;
                    p->hidden = false;
                }
            changed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("The selected parts follow their colour again.").c_str());
        ImGui::SameLine();
        if (m_imgui->button(_L("Clear")))
            m_svg_part_sel.clear();
    }

    // The list: the parts changed or selected, or all of them.
    if (ImGui::TreeNode(_u8L("Part list").c_str())) {
        ImGui::RadioButton(_u8L("Changed").c_str(), &m_svg_parts_filter, 0);
        ImGui::SameLine();
        ImGui::RadioButton(_u8L("All").c_str(), &m_svg_parts_filter, 1);
        std::vector<int> rows;
        if (m_svg_parts_filter == 1) {
            for (size_t k = 0; k < m_svg_shapes.size(); ++k)
                rows.push_back(int(k));
        } else {
            std::set<int> shown(m_svg_part_sel.begin(), m_svg_part_sel.end());
            for (const PaintDecalPart &p : m_svg_parts)
                shown.insert(p.shape);
            rows.assign(shown.begin(), shown.end());
        }
        if (rows.empty())
            m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("No part changed. Pick parts on the SVG, or show All."));
        const float row_h = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("##parts", ImVec2(0.f, std::min(float(rows.size()), 8.f) * row_h + 4.f), false);
        ImGuiListClipper clipper;
        clipper.Begin(int(rows.size()), row_h);
        while (clipper.Step())
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                const int k = rows[size_t(r)];
                ImGui::PushID(k);
                const PaintDecalPart *p = svg_part(k, false);
                int  state  = p ? p->state : -1;
                bool hidden = p ? p->hidden : false;
                bool sel    = m_svg_part_sel.count(k) != 0;
                const float row_y = ImGui::GetCursorScreenPos().y;
                if (ImGui::Checkbox("##sel", &sel)) {
                    if (sel)
                        m_svg_part_sel.insert(k);
                    else
                        m_svg_part_sel.erase(k);
                    m_parent.set_as_dirty();
                }
                ImGui::SameLine();
                ImGui::ColorButton("##colour", zone_colour(k), swatch_flags, swatch_size);
                ImGui::SameLine();
                m_imgui->text("#" + std::to_string(k + 1));
                ImGui::SameLine();
                if (render_part_filament_combo("##filament", state)) {
                    svg_part(k, true)->state = state;
                    changed = true;
                }
                ImGui::SameLine();
                bool visible = !hidden;
                if (ImGui::Checkbox("##visible", &visible)) {
                    svg_part(k, true)->hidden = !visible;
                    changed = true;
                }
                if (ImGui::IsMouseHoveringRect(ImVec2(ImGui::GetWindowPos().x, row_y), ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), row_y + row_h)))
                    m_svg_part_hover = k;
                ImGui::PopID();
            }
        ImGui::EndChild();
        ImGui::TreePop();
    }
    ImGui::PopID();
    if (changed)
        svg_prune_parts();
    if (changed || m_svg_part_hover != hover_before)
        m_parent.set_as_dirty();
    return changed;
}

// Vector mode: actions, the decal layers, the selected decal, and the shape
// or SVG being edited on screen. Same layout as the mockup approved on 27-oct-2026.
// Vector mode: actions, the decal layers, the selected decal, and the shape
// or SVG being edited on screen. Same layout as the mockup approved on 27-oct-2026.
void GLGizmoMmuSegmentation::render_vector_panel()
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    auto snapshot = [](const std::string &name) { Plater::TakeSnapshot(wxGetApp().plater(), name, UndoRedo::SnapshotType::GizmoAction); };
    std::vector<PaintDecal> &decals = mo->paint_decals;
    // Colour rows go in two columns when the panel is wide enough for two of them side by side.
    // The row width is MEASURED (render_svg_colour_row), never guessed: a second column placed past
    // the window's edge widens an auto-resized window, which moves the edge, and so on every frame.
    // The second column is aligned to the right edge, so it can never push it.
    const ImGuiStyle &style = ImGui::GetStyle();
    auto colour_column = [this, &style](size_t k, float line_x, float avail) {
        const float w = m_colour_row_w;
        if (w > 1.f && avail >= 2.f * w + style.ItemSpacing.x * 2.f && k % 2 == 1)
            ImGui::SameLine(line_x + avail - w);
    };
    if (m_selected_decal >= int(decals.size()))
        m_selected_decal = decals.empty() ? -1 : int(decals.size()) - 1;

    // --- Actions
    if (m_imgui->button(_L("Load SVG..."))) {
        wxFileDialog dlg(wxGetApp().plater(), _L("Choose an SVG file"), "", "", "SVG files (*.svg)|*.svg", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() == wxID_OK)
            shape_load_svg(std::string(dlg.GetPath().ToUTF8().data()));
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Load an SVG, centred on the view.\nEach of its colours gets a filament.").c_str());
    ImGui::SameLine();
    m_imgui->disabled_begin(decals.empty());
    if (m_imgui->button(_L("Apply all"))) {
        std::vector<size_t> idxs;
        for (size_t i = 0; i < decals.size(); ++i)
            if (!decals[i].hidden)
                idxs.push_back(i);
        snapshot(_u8L("Apply SVG decals"));
        decal_apply(idxs, false);
        update_model_object();
        m_parent.set_as_dirty();
    }
    m_imgui->disabled_end();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Apply every visible decal, top of the list first\n(the lower ones paint over).").c_str());
    if (!m_shape_svg_error.empty())
        m_imgui->warning_text(m_shape_svg_error);
    ImGui::Checkbox(_u8L("Lock view").c_str(), &m_shape_view_lock);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Freeze the camera while you work on screen:\nno zoom, orbit or pan, view cube or view keys.").c_str());
    ImGui::SameLine();
    if (ImGui::Checkbox(_u8L("Preview colours").c_str(), &m_decal_preview_colours))
        m_parent.set_as_dirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Fill the decal cards with their filaments' colours, see-through,\n"
            "for an idea of the result before Apply.").c_str());
    ImGui::SameLine();
    if (ImGui::Checkbox(_u8L("On surface").c_str(), &m_decal_on_surface))
        m_parent.set_as_dirty();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Show the cards laid on the object, exactly where they will paint\n"
            "(mirrored copies too). Off: floating around the object, as a reference.\n"
            "On folds and edges the preview can show small gaps: only the preview,\n"
            "Apply paints the whole area.").c_str());
    // A left drag orbits outside the SVG or shape being placed (and always before Draw shape).
    if (!m_shape_view_lock) {
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Orbit: drag outside the SVG or shape. Pan: right drag."));
    }

    // where the mirror planes cross, for every decal of the object (shown while any mirror is on).
    bool any_mirror = m_svg_mirror != 0;
    for (const PaintDecal &d : decals)
        any_mirror = any_mirror || d.mirror != 0;
    if (any_mirror)
        render_mirror_centre_controls();

    // --- Layers
    if (!decals.empty()) {
        ImGui::Separator();
        m_imgui->text(GUI::format(_L("Decals (%1%)"), decals.size()));
        int move_from = -1, move_to = -1;
        const float dot = ImGui::GetTextLineHeight() * 0.45f;
        for (size_t i = 0; i < decals.size(); ++i) {
            PaintDecal &d = decals[i];
            ImGui::PushID(int(i) + 7000);
            bool visible = !d.hidden;
            if (ImGui::Checkbox("##visible", &visible)) {
                snapshot(_u8L("Show/hide SVG decal"));
                d.hidden = !visible;
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Show the card, and include it in Apply all.").c_str());
            ImGui::SameLine();
            bool locked = d.locked;
            if (ImGui::Checkbox("##locked", &locked)) {
                snapshot(_u8L("Lock/unlock SVG decal"));
                d.locked = locked;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Locked: it cannot be edited or deleted.").c_str());
            ImGui::SameLine();
            // The filaments it paints with, as small dots.
            {
                ImDrawList  *draw = ImGui::GetWindowDrawList();
                const ImVec2 pos  = ImGui::GetCursorScreenPos();
                const float  cy   = pos.y + ImGui::GetFrameHeight() * 0.5f;
                float        cx   = pos.x + dot;
                int          shown = 0;
                for (const PaintDecalZone &z : d.zones) {
                    if (z.hidden || shown >= 4)
                        continue;
                    draw->AddCircleFilled(ImVec2(cx, cy), dot, ImGuiWrapper::to_ImU32(get_state_color(z.state)));
                    draw->AddCircle(ImVec2(cx, cy), dot, IM_COL32(0, 0, 0, 255));
                    cx += dot * 2.4f;
                    ++shown;
                }
                ImGui::Dummy(ImVec2(dot * 2.4f * 4.f, ImGui::GetFrameHeight()));
            }
            ImGui::SameLine();
            std::string label = d.name.empty() ? std::string("SVG") : d.name;
            label += d.perspective ? "  [P]" : "  [O]";
            if (d.through)
                label += " [T]";
            if (int(i) == m_svg_edit_decal)
                label += "  *";
            ImGui::AlignTextToFramePadding();
            if (ImGui::Selectable(label.c_str(), int(i) == m_selected_decal, 0, ImVec2(m_imgui->scaled(9.f), 0.f)))
                m_selected_decal = int(i);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("[P] perspective, [O] parallel rays, [T] through.\n* = being edited on screen.").c_str());
            ImGui::SameLine();
            if (ImGui::ArrowButton("##up", ImGuiDir_Up) && i > 0) {
                move_from = int(i);
                move_to   = int(i) - 1;
            }
            ImGui::SameLine();
            if (ImGui::ArrowButton("##down", ImGuiDir_Down) && i + 1 < decals.size()) {
                move_from = int(i);
                move_to   = int(i) + 1;
            }
            ImGui::PopID();
        }
        if (move_from >= 0) {
            snapshot(_u8L("Reorder SVG decals"));
            std::swap(decals[size_t(move_from)], decals[size_t(move_to)]);
            auto follow = [move_from, move_to](int &idx) {
                if (idx == move_from)
                    idx = move_to;
                else if (idx == move_to)
                    idx = move_from;
            };
            follow(m_selected_decal);
            follow(m_svg_edit_decal);
            m_decal_rings_cache.clear();
            m_parent.set_as_dirty();
        }

        // --- The selected decal
        if (m_selected_decal >= 0 && size_t(m_selected_decal) < decals.size() && m_selected_decal == m_svg_edit_decal) {
            ImGui::Separator();
            m_imgui->text(GUI::format(_L("Selected: %1%"), decals[size_t(m_selected_decal)].name));
            m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Being edited on screen, below."));
        } else if (!m_svg_zones.empty() || !m_shape_nodes.empty()) {
            // Something else is on screen: the selected decal's controls wait, so they are not
            // mixed up with the ones of the SVG or shape being placed.
            ImGui::Separator();
            m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Pin or cancel what is on screen to edit the decals."));
        } else if (m_selected_decal >= 0 && size_t(m_selected_decal) < decals.size()) {
            // Own ID scope: ImGui tells buttons apart by their label, and the on-screen block below
            // also has Apply and Erase (the second one never fired).
            ImGui::PushID("selected_decal");
            PaintDecal &d = decals[size_t(m_selected_decal)];
            ImGui::Separator();
            m_imgui->text(GUI::format(_L("Selected: %1%"), d.name.empty() ? std::string("SVG") : d.name));
            m_imgui->disabled_begin(d.locked);
            if (m_imgui->button(_L("Edit")))
                svg_edit_decal(m_selected_decal);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Go to the view it was pinned from and adjust it on screen:\n"
                    "move, scale, rotate, colours. Then Save (editing never paints).").c_str());
            m_imgui->disabled_end();
            ImGui::SameLine();
            if (m_imgui->button(_L("Apply"))) {
                snapshot(_u8L("Apply SVG decal"));
                decal_apply({ size_t(m_selected_decal) }, false);
                update_model_object();
                m_parent.set_as_dirty();
            }
            ImGui::SameLine();
            if (m_imgui->button(_L("Erase"))) {
                snapshot(_u8L("Erase SVG decal"));
                decal_apply({ size_t(m_selected_decal) }, true);
                update_model_object();
                m_parent.set_as_dirty();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Remove the paint this decal's area covers.").c_str());
            ImGui::SameLine();
            m_imgui->disabled_begin(d.locked);
            const bool delete_it = m_imgui->button(_L("Delete"));
            m_imgui->disabled_end();
            // slide it over the object with the mouse, seeing where it paints.
            {
                const bool moving = m_decal_move_armed;
                if (moving) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.59f, 0.53f, 0.35f));
                    ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORCA);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                }
                m_imgui->disabled_begin(d.locked);
                if (m_imgui->button(_L("Move on object"))) {
                    m_decal_move_armed = !m_decal_move_armed;
                    if (m_decal_move_armed)
                        m_decal_on_surface = true;   // moving it only makes sense seeing where it lands
                    m_parent.set_as_dirty();
                }
                m_imgui->disabled_end();
                if (moving) {
                    ImGui::PopStyleVar(1);
                    ImGui::PopStyleColor(2);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("Then drag on the object: the decal slides over it, turning around the object,\n"
                        "and you see live where it will paint. Drag off the object to orbit.\n"
                        "Click the button again to stop.").c_str());
                if (moving) {
                    ImGui::SameLine();
                    m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Drag on the object."));
                }
            }

            // turn and scale it on its own card, around the centre of the SVG. Each slider gesture
            // starts from the decal as it was when grabbed, and is one undo step.
            if (!d.locked) {
                const float tw = -(std::max(ImGui::CalcTextSize(_u8L("Turn").c_str()).x, ImGui::CalcTextSize(_u8L("Size").c_str()).x) + style.ItemInnerSpacing.x * 2.f);
                auto gesture = [&](bool changed) {
                    if (ImGui::IsItemActivated()) {
                        snapshot(_u8L("Turn or scale SVG decal"));
                        m_decal_ts_start = d;
                    }
                    if (changed)
                        decal_turn_scale(size_t(m_selected_decal), m_decal_ts_start, m_decal_turn_ui, m_decal_size_ui / 100.);
                    if (ImGui::IsItemDeactivated()) {
                        m_decal_turn_ui = 0.f;
                        m_decal_size_ui = 100.f;
                    }
                    if (changed)
                        m_parent.set_as_dirty();
                };
                ImGui::SetNextItemWidth(tw);
                gesture(ImGui::SliderFloat(_u8L("Turn").c_str(), &m_decal_turn_ui, -180.f, 180.f, "%.0f deg"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("Turn the decal on the object, around its centre.\nWith Move on object: mouse wheel.").c_str());
                ImGui::SetNextItemWidth(tw);
                gesture(ImGui::SliderFloat(_u8L("Size").c_str(), &m_decal_size_ui, 25.f, 400.f, "%.0f%%"));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("Scale the decal on the object, around its centre.\nWith Move on object: Shift + mouse wheel.").c_str());
            }

            // Projection and depth as two-way choices.
            auto choice = [this](const wxString &a, const wxString &b, bool &second) {
                bool changed = false;
                for (int k = 0; k < 2; ++k) {
                    const bool on = (k == 1) == second;
                    if (on) {
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.59f, 0.53f, 0.35f));
                        ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORCA);
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                    }
                    if (m_imgui->button(k == 0 ? a : b) && !on) {
                        second  = k == 1;
                        changed = true;
                    }
                    if (on) {
                        ImGui::PopStyleVar(1);
                        ImGui::PopStyleColor(2);
                    }
                    if (k == 0)
                        ImGui::SameLine();
                }
                return changed;
            };
            const float half = ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x * 0.5f,
                m_imgui->calc_button_size(_L("Parallel")).x + m_imgui->calc_button_size(_L("Perspective")).x + style.ItemSpacing.x * 3.f);
            m_imgui->text(_L("Projection"));
            ImGui::SameLine(half);
            m_imgui->text(_L("Depth"));
            bool perspective = d.perspective;
            if (choice(_L("Parallel"), _L("Perspective"), perspective)) {
                snapshot(_u8L("SVG decal projection"));
                d.perspective = perspective;
                if (perspective && d.eye_dist <= 0.)
                    d.eye_dist = std::max(3. * mo->raw_mesh_bounding_box().size().norm(), 50.);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Perspective: rays from the eye it was pinned from, as the camera saw it.\n"
                    "Parallel: the same size at any depth.").c_str());
            ImGui::SameLine(half);
            bool through = d.through;
            if (choice(_L("First surface"), _L("Through"), through)) {
                snapshot(_u8L("SVG decal depth"));
                d.through = through;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Through: every surface the rays cross, folds and hidden faces too.\n"
                    "It also comes out on the back of the object.").c_str());
            // symmetry, mirrored copies through the centre of the object's box.
            m_imgui->text(_L("Mirror"));
            for (int axis = 0; axis < 3; ++axis) {
                if (axis > 0)
                    ImGui::SameLine();
                bool on = (d.mirror & (1 << axis)) != 0;
                if (m_imgui->bbl_checkbox(axis == 0 ? _L("Left/Right") : axis == 1 ? _L("Front/Back") : _L("Top/Bottom"), on)) {
                    snapshot(_u8L("SVG decal mirror"));
                    d.mirror = on ? (d.mirror | (1 << axis)) : (d.mirror & ~(1 << axis));
                    m_parent.set_as_dirty();
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Paint mirrored copies through the centre of the object:\n"
                    "Left/Right across X, Front/Back across Y, Top/Bottom across Z.\n"
                    "Two give three copies. The copies are drawn as cards too and follow the decal.").c_str());
            if (d.mirror != 0) {
                bool readable = d.mirror_readable;
                if (m_imgui->bbl_checkbox(_L("Keep readable"), readable)) {
                    snapshot(_u8L("SVG decal mirror"));
                    d.mirror_readable = readable;
                    m_parent.set_as_dirty();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", _u8L("The copies land in the same mirrored place but are not flipped,\n"
                        "so text and logos read the right way.").c_str());
            }
            m_imgui->text(_L("Colours and filaments"));
            const float zones_x = ImGui::GetCursorPosX(), zones_avail = ImGui::GetContentRegionAvail().x;
            for (size_t k = 0; k < d.zones.size(); ++k) {
                colour_column(k, zones_x, zones_avail);
                PaintDecalZone &z = d.zones[k];
                int  state  = z.state;
                bool hidden = z.hidden;
                if (render_svg_colour_row(int(k) + 9000, z.rgb, false, state, hidden)) {
                    snapshot(_u8L("SVG decal colours"));
                    z.state  = state;
                    z.hidden = hidden;
                    m_parent.set_as_dirty();
                }
            }
            if (delete_it) {
                snapshot(_u8L("Delete SVG decal"));
                const int del = m_selected_decal;
                decals.erase(decals.begin() + del);
                if (m_svg_edit_decal == del)
                    m_svg_edit_decal = -1;
                else if (m_svg_edit_decal > del)
                    --m_svg_edit_decal;
                m_selected_decal = decals.empty() ? -1 : std::min(del, int(decals.size()) - 1);
                m_decal_rings_cache.clear();
                m_parent.set_as_dirty();
            }
            ImGui::PopID();
        }
    }

    // --- On screen: the shape or SVG being edited
    ImGui::Separator();
    const bool has_svg   = !m_svg_zones.empty();
    const bool has_shape = !m_shape_nodes.empty();
    if (!has_svg && !has_shape) {
        m_imgui->text_colored(ImGuiWrapper::COL_GREY_LIGHT, _L("Load an SVG, or Draw shape above."));
        return;
    }
    ImGui::PushID("on_screen");
    if (has_svg)
        m_imgui->text(m_svg_edit_decal >= 0 ? GUI::format(_L("On screen: editing %1%"), m_svg_name)
                                            : GUI::format(_L("On screen: %1% (not pinned)"), m_svg_name.empty() ? std::string("SVG") : m_svg_name));
    else
        m_imgui->text(GUI::format(m_shape_closed ? _L("On screen: shape, %1% points") : _L("On screen: open shape, %1% points"), m_shape_nodes.size()));

    if (has_svg) {
        m_imgui->text(_L("Drag inside its frame to move it."));
        float pct = m_shape_scale_pct;
        const float slider_w = -(std::max({ ImGui::CalcTextSize(_u8L("Scale").c_str()).x, ImGui::CalcTextSize(_u8L("Rotate").c_str()).x, ImGui::CalcTextSize(_u8L("Width").c_str()).x, ImGui::CalcTextSize(_u8L("Height").c_str()).x }) + style.ItemInnerSpacing.x * 2.f);
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat(_u8L("Scale").c_str(), &pct, 25.f, 400.f, "%.0f%%")) {
            shape_scale(pct / m_shape_scale_pct);
            m_shape_scale_pct = pct;
            m_parent.set_as_dirty();
        }
        float deg = m_shape_rotate_deg;
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat(_u8L("Rotate").c_str(), &deg, -180.f, 180.f, "%.0f deg")) {
            shape_rotate(deg - m_shape_rotate_deg);
            m_shape_rotate_deg = deg;
            m_parent.set_as_dirty();
        }
        // width and height on their own, along the SVG's axes.
        float wpct = m_svg_width_pct;
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat(_u8L("Width").c_str(), &wpct, 25.f, 400.f, "%.0f%%")) {
            svg_stretch(0, wpct / m_svg_width_pct);
            m_svg_width_pct = wpct;
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Stretch or squeeze the SVG along its own width.").c_str());
        float hpct = m_svg_height_pct;
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat(_u8L("Height").c_str(), &hpct, 25.f, 400.f, "%.0f%%")) {
            svg_stretch(1, hpct / m_svg_height_pct);
            m_svg_height_pct = hpct;
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Stretch or squeeze the SVG along its own height.").c_str());
        // corner deform, handles on the corners of the frame.
        if (m_imgui->bbl_checkbox(_L("Deform corners"), m_svg_deform))
            m_parent.set_as_dirty();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Drag a corner of the frame to bend the SVG in perspective.\n"
                "Straight lines stay straight. A corner that would fold the frame stops.").c_str());
        if (svg_is_deformed()) {
            ImGui::SameLine();
            if (m_imgui->button(_L("Reset corners"))) {
                svg_reset_corners();
                m_parent.set_as_dirty();
            }
        }
        // mirror planes, the copies drawn live on the object while you place it.
        m_imgui->text(_L("Mirror"));
        for (int axis = 0; axis < 3; ++axis) {
            if (axis > 0)
                ImGui::SameLine();
            bool on = (m_svg_mirror & (1 << axis)) != 0;
            if (m_imgui->bbl_checkbox(axis == 0 ? _L("Left/Right") : axis == 1 ? _L("Front/Back") : _L("Top/Bottom"), on)) {
                m_svg_mirror = on ? (m_svg_mirror | (1 << axis)) : (m_svg_mirror & ~(1 << axis));
                m_parent.set_as_dirty();
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Mirrored copies through the centre of the object:\n"
                "Left/Right across X, Front/Back across Y, Top/Bottom across Z.\n"
                "They show as cards where they will land, and are kept when you save.").c_str());
        if (m_svg_mirror != 0) {
            if (m_imgui->bbl_checkbox(_L("Keep readable"), m_svg_mirror_readable))
                m_parent.set_as_dirty();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("The copies land in the same mirrored place but are not flipped,\n"
                    "so text and logos read the right way.").c_str());
        }
        bool masks_dirty = false;
        const float zones_x = ImGui::GetCursorPosX(), zones_avail = ImGui::GetContentRegionAvail().x;
        for (size_t z = 0; z < m_svg_zones.size(); ++z) {
            colour_column(z, zones_x, zones_avail);
            SvgZone &zone = m_svg_zones[z];
            const bool hidden_before = zone.hidden;
            if (render_svg_colour_row(int(z), zone.rgb, zone.grad, zone.state, zone.hidden)) {
                // Parts: the paint groups carry the filaments, so any change rebuilds them.
                masks_dirty = true;
                (void) hidden_before;
                m_parent.set_as_dirty();
            }
        }
        masks_dirty = render_svg_parts() || masks_dirty;
        if (svg_is_drawn_shape()) {
            if (m_imgui->button(_L("Edit points")))
                svg_to_shape_nodes();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", _u8L("Turn this drawn shape back into points and handles to edit them.\n"
                    "Save it again to update the decal. A corner deform is only approximated.").c_str());
        }
        if (masks_dirty) {
            svg_rebuild_masks();
            svg_update_screen_rings();
        }
    }

    // Keep it: as a decal pinned to this view (an SVG, or a closed shape turned into one).
    // An SVG on screen is only adjusted here; it paints from its layer once saved, so what is
    // painted is always what is stored.
    if (has_svg) {
        if (m_imgui->button(m_svg_edit_decal >= 0 ? _L("Save") : _L("Pin to view"))) {
            const int editing = m_svg_edit_decal;
            if (svg_pin_to_view())
                m_selected_decal = editing >= 0 ? editing : int(decals.size()) - 1;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", (m_svg_edit_decal >= 0
                ? _u8L("Store the changes in the decal and close. Then Apply it from its layer.")
                : _u8L("Keep it as a decal of the object, seen from this view.\n"
                       "It floats as a card that follows the object.\nThen Apply it from its layer.")).c_str());
        ImGui::SameLine();
        if (m_imgui->button(_L("Cancel"))) {
            shape_clear();
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Drop the changes. A pinned decal keeps its last saved version.").c_str());
        ImGui::PopID();
        return;
    } else if (!m_shape_closed) {
        m_imgui->disabled_begin(m_shape_nodes.size() < 3);
        if (m_imgui->button(_L("Close shape"))) {
            m_shape_closed = true;
            m_parent.set_as_dirty();
        }
        m_imgui->disabled_end();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Join the last point to the first one.").c_str());
    } else {
        if (m_imgui->button(m_shape_from_decal >= 0 ? _L("Save") : _L("Save as decal"))) {
            if (shape_to_svg_session() && svg_pin_to_view())
                m_selected_decal = int(decals.size()) - 1;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Keep the shape as a decal pinned to this view, painting with\n"
                "the selected filament. Edit points brings its points back later.").c_str());
    }

    m_imgui->disabled_begin(!m_shape_closed);
    if (m_imgui->button(_L("Apply")))
        shape_apply(get_left_button_state_type(), std::string(handle_snapshot_action_name(false, Button::Left).ToUTF8().data()));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", (has_svg ? _u8L("Paint the SVG from this view, each colour with its filament.")
                                         : _u8L("Paint the shape with the selected filament.")).c_str());
    ImGui::SameLine();
    if (m_imgui->button(_L("Erase")))
        shape_apply(EnforcerBlockerType::NONE, std::string(handle_snapshot_action_name(true, Button::Left).ToUTF8().data()));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Remove the paint inside it.").c_str());
    m_imgui->disabled_end();
    ImGui::SameLine();
    if (m_imgui->button(_L("Cancel"))) {
        shape_clear();
        m_parent.set_as_dirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Drop what is on screen. A pinned decal keeps its last version.").c_str());
    if (has_shape) {
        ImGui::SameLine();
        if (m_imgui->button(_L("Undo point"))) {
            if (m_shape_closed)
                m_shape_closed = false;
            else
                m_shape_nodes.pop_back();
            m_parent.set_as_dirty();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Reopen a closed shape, or remove the last point of an open one.").c_str());
    }
    ImGui::Checkbox(_u8L("Keep on screen after Apply").c_str(), &m_shape_keep_after_apply);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Leave it on screen after Apply,\nto paint it again with another filament or tweak it.").c_str());
    ImGui::PopID();
}

// `state` is a filament id (1-based, Mixed Filaments included).
ColorRGBA GLGizmoMmuSegmentation::get_state_color(int state) const
{
    if (state >= 1 && size_t(state) <= m_extruders_colors.size())
        return m_extruders_colors[size_t(state) - 1];
    return ColorRGBA(0.6f, 0.6f, 0.6f, 1.0f);
}

// The filament whose colour is closest (RGB distance) to the SVG colour, among the ones the
// panel offers; the selected one if there is none.
// The filament whose colour is closest (RGB distance) to the SVG colour, among the ones the
// panel offers; the selected one if there is none.
int GLGizmoMmuSegmentation::svg_default_state(uint32_t rgb) const
{
    const float r = float((rgb >> 16) & 0xFF) / 255.f, g = float((rgb >> 8) & 0xFF) / 255.f, b = float(rgb & 0xFF) / 255.f;
    int   best      = int(get_left_button_state_type());
    float best_dist = std::numeric_limits<float>::max();
    const size_t n = std::min(GLGizmoMmuSegmentation::EXTRUDERS_LIMIT, m_display_filament_ids.size());
    for (size_t i = 0; i < n; ++i) {
        const int state = int(m_display_filament_ids[i]);
        if (state < 1 || size_t(state) > m_extruders_colors.size())
            continue;
        const ColorRGBA &c = m_extruders_colors[size_t(state) - 1];
        const float dist = (c.r() - r) * (c.r() - r) + (c.g() - g) * (c.g() - g) + (c.b() - b) * (c.b() - b);
        if (dist < best_dist) {
            best_dist = dist;
            best      = state;
        }
    }
    return best;
}

wxString GLGizmoMmuSegmentation::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    wxString action_name;
    if (shift_down)
        action_name = _L("Remove painted color");
    else {
        action_name        = GUI::format(_L("Painted using: Filament %1%"), m_selected_extruder_idx + 1);
    }
    return action_name;
}

void GLMmSegmentationGizmo3DScene::release_geometry() {
    if (this->vertices_VBO_id) {
        glsafe(::glDeleteBuffers(1, &this->vertices_VBO_id));
        this->vertices_VBO_id = 0;
    }
    for(auto &triangle_indices_VBO_id : triangle_indices_VBO_ids) {
        glsafe(::glDeleteBuffers(1, &triangle_indices_VBO_id));
        triangle_indices_VBO_id = 0;
    }

    this->clear();
}

void GLMmSegmentationGizmo3DScene::render(size_t triangle_indices_idx) const
{
    assert(triangle_indices_idx < this->triangle_indices_VBO_ids.size());
    assert(this->triangle_patches.size() == this->triangle_indices_VBO_ids.size());
    assert(this->vertices_VBO_id != 0);
    assert(this->triangle_indices_VBO_ids[triangle_indices_idx] != 0);

    GLShaderProgram* shader = wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    // the following binding is needed to set the vertex attributes
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->vertices_VBO_id));
    const GLint position_id = shader->get_attrib_location("v_position");
    if (position_id != -1) {
        glsafe(::glVertexAttribPointer(position_id, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (GLvoid*)0));
        glsafe(::glEnableVertexAttribArray(position_id));
    }

    // Render using the Vertex Buffer Objects.
    if (this->triangle_indices_VBO_ids[triangle_indices_idx] != 0 &&
        this->triangle_indices_sizes[triangle_indices_idx] > 0) {
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->triangle_indices_VBO_ids[triangle_indices_idx]));
        glsafe(::glDrawElements(GL_TRIANGLES, GLsizei(this->triangle_indices_sizes[triangle_indices_idx]), GL_UNSIGNED_INT, nullptr));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    }

    if (position_id != -1)
        glsafe(::glDisableVertexAttribArray(position_id));

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
}

void GLMmSegmentationGizmo3DScene::finalize_vertices()
{
    assert(this->vertices_VBO_id == 0);
    if (!this->vertices.empty()) {
        glsafe(::glGenBuffers(1, &this->vertices_VBO_id));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, this->vertices_VBO_id));
        glsafe(::glBufferData(GL_ARRAY_BUFFER, this->vertices.size() * sizeof(float), this->vertices.data(), GL_STATIC_DRAW));
        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
        this->vertices.clear();
    }
}

void GLMmSegmentationGizmo3DScene::finalize_triangle_indices()
{
    triangle_indices_VBO_ids.resize(this->triangle_patches.size());
    triangle_indices_sizes.resize(this->triangle_patches.size());
    assert(std::all_of(triangle_indices_VBO_ids.cbegin(), triangle_indices_VBO_ids.cend(), [](const auto &ti_VBO_id) { return ti_VBO_id == 0; }));

    for (size_t buffer_idx = 0; buffer_idx < this->triangle_patches.size(); ++buffer_idx) {
        std::vector<int>& triangle_indices = this->triangle_patches[buffer_idx].triangle_indices;
        triangle_indices_sizes[buffer_idx] = triangle_indices.size();
        if (!triangle_indices.empty()) {
            glsafe(::glGenBuffers(1, &this->triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->triangle_indices_VBO_ids[buffer_idx]));
            glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, triangle_indices.size() * sizeof(int), triangle_indices.data(), GL_STATIC_DRAW));
            glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
            triangle_indices.clear();
        }
    }
}

void GLGizmoMmuSegmentation::render_filament_remap_ui(float window_width, float max_tooltip_width)
{
    size_t n_extr = std::min((size_t)EnforcerBlockerType::ExtruderMax, m_display_filament_ids.size());

    const std::string max_label = std::to_string(std::max<size_t>(n_extr, 1));
    const ImVec2 max_label_size = ImGui::CalcTextSize(max_label.c_str(), NULL, true);
    const ImVec2 button_size(max_label_size.x + m_imgui->scaled(0.5f), 0.f);
    const int max_items_per_line = 8;
    const float item_width = button_size.x + m_imgui->scaled(1.5f);
    const float start_pos_x = ImGui::GetCursorPosX();

    for (int src = 0; src < (int)n_extr; ++src) {
        const unsigned int dst_filament_id = m_extruder_remap[src] < m_display_filament_ids.size() ? m_display_filament_ids[m_extruder_remap[src]] : 0;
        if (dst_filament_id == 0 || dst_filament_id > m_extruders_colors.size())
            continue;
        const ColorRGBA &dst_col = m_extruders_colors[dst_filament_id - 1];
        ImVec4 col_vec = ImGuiWrapper::to_ImVec4(dst_col);

        if (src % max_items_per_line != 0) {
            ImGui::SameLine(start_pos_x + item_width * (src % max_items_per_line));
        }
        std::string btn_id = "##remap_src_" + std::to_string(src);
        
        ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs |
                                    ImGuiColorEditFlags_NoLabel  | ImGuiColorEditFlags_NoPicker |
                                    ImGuiColorEditFlags_NoTooltip;
        if (m_selected_extruder_idx != src) flags |= ImGuiColorEditFlags_NoBorder;
        
        #ifdef __APPLE__
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0);
            bool clicked = ImGui::ColorButton(btn_id.c_str(), col_vec, flags, button_size);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(1);
        #else
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0);
            bool clicked = ImGui::ColorButton(btn_id.c_str(), col_vec, flags, button_size);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(1);
        #endif

        // overlay destination number with proper contrast calculation
        const size_t num_physical = m_mixed_display_context.num_physical;
        std::string dst_txt = (dst_filament_id <= num_physical)
                                  ? std::to_string(dst_filament_id)
                                  : Slic3r::mixed_filament_index_to_letter(dst_filament_id - num_physical - 1);
        float gray = 0.299f * dst_col.r() + 0.587f * dst_col.g() + 0.114f * dst_col.b();
        ImVec2 txt_sz = ImGui::CalcTextSize(dst_txt.c_str());
        ImVec2 pos = ImGui::GetItemRectMin();
        ImVec2 size = ImGui::GetItemRectSize();
        
        if (gray * 255.f < 80.f)
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + (size.x - txt_sz.x) * 0.5f, pos.y + (size.y - txt_sz.y) * 0.5f),
                IM_COL32(255,255,255,255), dst_txt.c_str());
        else
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(pos.x + (size.x - txt_sz.x) * 0.5f, pos.y + (size.y - txt_sz.y) * 0.5f),
                IM_COL32(0,0,0,255), dst_txt.c_str());

        // popup with possible destinations
        std::string pop_id = "popup_" + std::to_string(src);
        if (clicked) {
            // Calculate popup position centered below the current button
            ImVec2 button_pos = ImGui::GetItemRectMin();
            ImVec2 button_size = ImGui::GetItemRectSize();
            ImVec2 popup_pos(button_pos.x + button_size.x * 0.5f, button_pos.y + button_size.y);
            
            // Set popup styling BEFORE opening popup
            ImGui::SetNextWindowPos(popup_pos, ImGuiCond_Appearing, ImVec2(0.5f, -0.1f));
            ImGui::SetNextWindowBgAlpha(1.0f); // Ensure full opacity
            ImGui::OpenPopup(pop_id.c_str());
        }
        
        // Apply popup styling before BeginPopup using standard Orca colors
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 4.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, m_is_dark_mode ? ImGuiWrapper::COL_WINDOW_BG_DARK : ImGuiWrapper::COL_WINDOW_BG);
        ImGui::PushStyleColor(ImGuiCol_Border, m_is_dark_mode ? ImVec4(0.5f, 0.5f, 0.5f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f));
        
        if (ImGui::BeginPopup(pop_id.c_str())) {
            const float popup_start_pos_x = ImGui::GetCursorPosX();
            
            for (int dst = 0; dst < (int)n_extr; ++dst) {
                const unsigned int popup_filament_id = m_display_filament_ids[dst];
                if (popup_filament_id == 0 || popup_filament_id > m_extruders_colors.size())
                    continue;
                const ColorRGBA &dst_col_popup = m_extruders_colors[popup_filament_id - 1];
                ImVec4 dst_vec = ImGuiWrapper::to_ImVec4(dst_col_popup);
                if (dst % max_items_per_line != 0)
                    ImGui::SameLine(popup_start_pos_x + item_width * (dst % max_items_per_line));
                std::string dst_btn = "##dst_" + std::to_string(src) + "_" + std::to_string(dst);
                
                // Apply same styling to destination buttons
                ImGuiColorEditFlags dst_flags = ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoInputs |
                                               ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoPicker |
                                               ImGuiColorEditFlags_NoTooltip;
                // Show border for currently selected destination filament
                if (m_extruder_remap[src] != dst) dst_flags |= ImGuiColorEditFlags_NoBorder;
                
                #ifdef __APPLE__
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0);
                    bool dst_clicked = ImGui::ColorButton(dst_btn.c_str(), dst_vec, dst_flags, button_size);
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor(1);
                #else
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGuiWrapper::COL_ORCA);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0);
                    bool dst_clicked = ImGui::ColorButton(dst_btn.c_str(), dst_vec, dst_flags, button_size);
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor(1);
                #endif
                
                // overlay destination number on popup buttons
                std::string dst_num_txt = (popup_filament_id <= num_physical)
                                              ? std::to_string(popup_filament_id)
                                              : Slic3r::mixed_filament_index_to_letter(popup_filament_id - num_physical - 1);
                float dst_gray = 0.299f * dst_col_popup.r() + 0.587f * dst_col_popup.g() + 0.114f * dst_col_popup.b();
                ImVec2 dst_txt_sz = ImGui::CalcTextSize(dst_num_txt.c_str());
                ImVec2 dst_pos = ImGui::GetItemRectMin();
                ImVec2 dst_size = ImGui::GetItemRectSize();
                
                if (dst_gray * 255.f < 80.f)
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(dst_pos.x + (dst_size.x - dst_txt_sz.x) * 0.5f, dst_pos.y + (dst_size.y - dst_txt_sz.y) * 0.5f),
                        IM_COL32(255,255,255,255), dst_num_txt.c_str());
                else
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2(dst_pos.x + (dst_size.x - dst_txt_sz.x) * 0.5f, dst_pos.y + (dst_size.y - dst_txt_sz.y) * 0.5f),
                        IM_COL32(0,0,0,255), dst_num_txt.c_str());
                
                if (dst_clicked)
                {
                    m_extruder_remap[src] = dst;
                    // update the source button color immediately
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
        
        // Clean up popup styling (always pop, whether popup was open or not)
        ImGui::PopStyleColor(2); // PopupBg and Border
        ImGui::PopStyleVar(2);   // PopupRounding and PopupBorderSize
    }

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.3f));

    if (m_imgui->button(m_desc.at("remap"))) {
        remap_filament_assignments();
        m_show_filament_remap_ui = false;
    }

    ImGui::SameLine();
    if (m_imgui->button(m_desc.at("cancel_remap")))
        m_show_filament_remap_ui = false;
}

void GLGizmoMmuSegmentation::remap_filament_assignments()
{
    if (m_extruder_remap.empty())
        return;

    constexpr size_t MAX_EBT = (size_t)EnforcerBlockerType::ExtruderMax;
    EnforcerBlockerStateMap state_map;

    // identity mapping by default
    for (size_t i = 0; i <= MAX_EBT; ++i)
        state_map[i] = static_cast<EnforcerBlockerType>(i);

    size_t n_extr = std::min({m_extruder_remap.size(), m_display_filament_ids.size(), MAX_EBT});
    bool   any_change = false;
    for (size_t src = 0; src < n_extr; ++src) {
        const size_t dst = m_extruder_remap[src];
        if (dst >= m_display_filament_ids.size())
            continue;

        const unsigned int src_state = m_display_filament_ids[src];
        const unsigned int dst_state = m_display_filament_ids[dst];
        if (src_state == 0 || dst_state == 0 || src_state == dst_state)
            continue;

        state_map[src_state] = static_cast<EnforcerBlockerType>(dst_state);
        if (src_state == 1)
            state_map[0] = static_cast<EnforcerBlockerType>(dst_state);

        any_change = true;
    }
    if (!any_change)
        return;

    Plater::TakeSnapshot snapshot(wxGetApp().plater(),
                                  "Remap filament assignments",
                                  UndoRedo::SnapshotType::GizmoAction);

    bool updated = false;
    int idx = -1;
    ModelObject* mo = m_c->selection_info()->model_object();
    if (!mo) return;

    for (ModelVolume* mv : mo->volumes) {
        if (!mv->is_model_part()) continue;
        ++idx;
        TriangleSelectorGUI* ts = m_triangle_selectors[idx].get();
        if (!ts) continue;
        ts->remap_triangle_state(state_map);
        ts->request_update_render_data(true);
        updated = true;
    }

    if (updated) {
        wxGetApp().plater()->get_notification_manager()->push_notification(
            _L("Filament remapping finished.").ToStdString());
        update_model_object();
        m_parent.set_as_dirty();
    }
}

} // namespace Slic3r
