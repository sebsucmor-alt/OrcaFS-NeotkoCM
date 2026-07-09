#include "TextureBumpConfigUI.hpp"
#include "TextureBumpPlaneHandles.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/format.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GLTexture.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <wx/filedlg.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace Slic3r::GUI {

// NEOTKO_TEXTUREBUMP_TAG -- moved verbatim from GLGizmoTextureBumpPainter.cpp (was `static`
// there, zone-seeding only) -- see header comment for why this is now shared.
TextureBumpConfig resolve_base_texture_bump_config(const ModelObject* model_object)
{
    // NEOTKO_TEXTUREBUMP_TAG -- AllWalls (not External), confirmed by real print test: partial-wall
    // bump (only the outer wall moves) is inherently risky for print quality. Also this is
    // create_new_blank_zone()'s ONLY source of defaults (resolve_base_texture_bump_config(nullptr)),
    // so it doubles as "what a genuinely new zone starts with".
    TextureBumpConfig cfg{
        TextureBumpType::AllWalls, scaled<coord_t>(0.2), scaled<coord_t>(0.3), false,
        TextureProjectionMode::Planar, TextureProjectionAxis::Z, 20.0, 1, 45.0 * M_PI / 180.0, 1.0, std::string()};
    if (!model_object)
        return cfg;

    const DynamicPrintConfig& obj_cfg = model_object->config.get();
    const DynamicPrintConfig& glb_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto find_opt = [&](const char* key) -> const ConfigOption* {
        if (const ConfigOption* opt = obj_cfg.option(key)) return opt;
        return glb_cfg.option(key);
    };

    if (const ConfigOption* opt = find_opt("texture_bump"))
        cfg.type = static_cast<const ConfigOptionEnum<TextureBumpType>*>(opt)->value;
    if (const ConfigOption* opt = find_opt("texture_bump_projection_mode"))
        cfg.projection_mode = static_cast<const ConfigOptionEnum<TextureProjectionMode>*>(opt)->value;
    if (const ConfigOption* opt = find_opt("texture_bump_axis"))
        cfg.axis = static_cast<const ConfigOptionEnum<TextureProjectionAxis>*>(opt)->value;
    if (const ConfigOption* opt = find_opt("texture_bump_scale"))
        cfg.scale = static_cast<const ConfigOptionFloat*>(opt)->value;
    if (const ConfigOption* opt = find_opt("texture_bump_repeat_u"))
        cfg.repeat_u = static_cast<const ConfigOptionInt*>(opt)->value;
    if (const ConfigOption* opt = find_opt("texture_bump_thickness"))
        cfg.thickness = scaled<coord_t>(static_cast<const ConfigOptionFloat*>(opt)->value);
    if (const ConfigOption* opt = find_opt("texture_bump_point_distance"))
        cfg.point_distance = scaled<coord_t>(static_cast<const ConfigOptionFloat*>(opt)->value);
    if (const ConfigOption* opt = find_opt("texture_bump_first_layer"))
        cfg.first_layer = static_cast<const ConfigOptionBool*>(opt)->value;
    if (const ConfigOption* opt = find_opt("texture_bump_max_angle"))
        cfg.max_angle_rad = static_cast<const ConfigOptionFloat*>(opt)->value * M_PI / 180.0;
    if (const ConfigOption* opt = find_opt("texture_bump_blur_strength"))
        cfg.blur_strength = static_cast<const ConfigOptionFloat*>(opt)->value;
    if (const ConfigOption* opt = find_opt("texture_bump_image_path"))
        cfg.image_path = static_cast<const ConfigOptionString*>(opt)->value;
    return cfg;
}

// ============================================================================================
// Shared compact-row widget kit (declared in TextureBumpConfigUI.hpp, used by all three modes
// of the Bump Mapping Editor panel -- ZBumpConfigUI.cpp draws with these same helpers).
// ============================================================================================

// NEOTKO_TEXTUREBUMP_TAG -- one small icon per section, loaded once and reused by every mode.
// Dedicated SVGs drawn for this feature (texture_bump_*.svg, resources/images/) -- single
// #6B6B6B fill, no dark variant needed, same convention Tab.cpp's own param_*.svg icons use.
const BumpSectionIcons& bump_section_icons()
{
    static BumpSectionIcons icons;
    static bool             loaded = false;
    if (!loaded) {
        loaded = true;
        const std::string dir = Slic3r::resources_dir() + "/images/";
        IMTexture::load_from_svg_file(dir + "texture_bump_source.svg", 32, 32, icons.source);
        IMTexture::load_from_svg_file(dir + "texture_bump_projection.svg", 32, 32, icons.projection);
        IMTexture::load_from_svg_file(dir + "texture_bump_transform.svg", 32, 32, icons.transform);
        IMTexture::load_from_svg_file(dir + "texture_bump_relief.svg", 32, 32, icons.relief);
        IMTexture::load_from_svg_file(dir + "texture_bump_safety.svg", 32, 32, icons.safety);
    }
    return icons;
}

float bump_ui_label_width()   { return ImGui::GetFontSize() * 7.5f; }
float bump_ui_control_width() { return ImGui::GetFontSize() * 9.0f; }

void bump_ui_section(ImTextureID icon, const std::string& label)
{
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.4f));
    if (icon != nullptr) {
        const float sz = ImGui::GetFontSize() * 1.05f;
        ImGui::Image(icon, ImVec2(sz, sz));
        ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.4f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (sz - ImGui::GetFontSize()) * 0.5f);
    }
    ImGui::TextUnformatted(label.c_str());
    // hairline rule filling the rest of the row, centered on the label text
    const float mid_y = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.5f);
    const ImVec2 p     = ImGui::GetCursorScreenPos();
    const float  avail = ImGui::GetContentRegionAvail().x;
    if (avail > ImGui::GetFontSize())
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, mid_y), ImVec2(p.x + avail, mid_y),
                                            ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::NewLine();
}

namespace {

// Colored bullet tying a row/stat to the same-colored 3D viewport grabber (colors mirror the
// m_grabbers[0..4] setup in GLGizmoTextureBump.cpp's on_init()).
void bump_handle_dot(ImU32 col, float center_y_offset)
{
    const float  r = ImGui::GetFontSize() * 0.20f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + center_y_offset), r, col);
    ImGui::Dummy(ImVec2(r * 2.0f, 0.0f));
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.3f);
}

void bump_drag_tooltip(const std::string& tooltip)
{
    std::string tip = tooltip;
    if (!tip.empty())
        tip += "\n";
    tip += _u8L("Drag to change - Ctrl+click to type an exact value");
    ImGui::SetTooltip("%s", tip.c_str());
}

// Label column + hover tooltip, leaving the cursor at the control column.
void bump_row_label(const std::string& label, const std::string& tooltip, bool warn, ImU32 dot)
{
    ImGui::AlignTextToFramePadding();
    if (dot != 0) {
        bump_handle_dot(dot, ImGui::GetFrameHeight() * 0.5f);
        ImGui::AlignTextToFramePadding();
    }
    if (warn)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_RED);
    ImGui::TextUnformatted(label.c_str());
    if (warn)
        ImGui::PopStyleColor();
    if (!tooltip.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip.c_str());
    ImGui::SameLine(bump_ui_label_width());
}

} // anonymous namespace

BumpRowEdit bump_ui_drag_float(const std::string& label, const char* id, float& v, float speed,
                               float v_min, float v_max, const char* fmt,
                               const std::string& tooltip, bool warn, ImU32 dot)
{
    BumpRowEdit e;
    bump_row_label(label, tooltip, warn, dot);
    ImGui::SetNextItemWidth(bump_ui_control_width());
    if (warn)
        ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_RED);
    const ImGuiSliderFlags flags = (v_min < v_max) ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
    e.changed   = ImGui::DragFloat(id, &v, speed, v_min, v_max, fmt, flags);
    if (warn)
        ImGui::PopStyleColor();
    e.committed = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
        bump_drag_tooltip(tooltip);
    return e;
}

BumpRowEdit bump_ui_drag_int(const std::string& label, const char* id, int& v, float speed,
                             int v_min, int v_max, const char* fmt,
                             const std::string& tooltip)
{
    BumpRowEdit e;
    bump_row_label(label, tooltip, false, 0);
    ImGui::SetNextItemWidth(bump_ui_control_width());
    e.changed = ImGui::DragInt(id, &v, speed, v_min, v_max, fmt, ImGuiSliderFlags_AlwaysClamp);
    if (e.changed)
        v = std::clamp(v, v_min, v_max);
    e.committed = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
        bump_drag_tooltip(tooltip);
    return e;
}

BumpRowEdit bump_ui_drag_float2(const std::string& label, const char* id, float& a, float& b,
                                float speed, float v_min, float v_max, const char* fmt,
                                const std::string& tooltip, ImU32 dot)
{
    BumpRowEdit e;
    bump_row_label(label, tooltip, false, dot);
    const float w = (bump_ui_control_width() - ImGui::GetStyle().ItemInnerSpacing.x) * 0.5f;
    const ImGuiSliderFlags flags = (v_min < v_max) ? ImGuiSliderFlags_AlwaysClamp : ImGuiSliderFlags_None;
    const std::string id_a = std::string(id) + "_a";
    const std::string id_b = std::string(id) + "_b";
    ImGui::SetNextItemWidth(w);
    e.changed = ImGui::DragFloat(id_a.c_str(), &a, speed, v_min, v_max, fmt, flags);
    e.committed = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
        bump_drag_tooltip(tooltip);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(w);
    e.changed   = ImGui::DragFloat(id_b.c_str(), &b, speed, v_min, v_max, fmt, flags) || e.changed;
    e.committed = ImGui::IsItemDeactivatedAfterEdit() || e.committed;
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
        bump_drag_tooltip(tooltip);
    return e;
}

bool bump_ui_image_picker(const char* id, std::string& image_path, const std::string& dialog_title)
{
    // GUI-only thumbnail cache keyed by path (same idea as the zone-list thumbnails in
    // GLGizmoTextureBump.cpp; a handful of PNGs per session, never evicted).
    static std::map<std::string, std::unique_ptr<GLTexture>> cache;
    GLTexture* tex = nullptr;
    if (!image_path.empty()) {
        auto it = cache.find(image_path);
        if (it == cache.end()) {
            auto t = std::make_unique<GLTexture>();
            t->load_from_file(image_path, true, GLTexture::None, false);
            it = cache.emplace(image_path, std::move(t)).first;
        }
        if (it->second->get_id() != 0)
            tex = it->second.get();
    }

    const float  font   = ImGui::GetFontSize();
    const float  pad    = font * 0.35f;
    const float  thumb  = font * 2.4f;
    const float  card_w = bump_ui_label_width() + bump_ui_control_width();
    const float  card_h = thumb + pad * 2.0f;
    const ImVec2 p0     = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + card_w, p0.y + card_h);
    ImDrawList*  dl     = ImGui::GetWindowDrawList();
    const bool   dark   = ImGuiWrapper::is_dark_mode();

    // The whole card is one click target; visuals are drawn over the invisible button so
    // hover state can tint them.
    ImGui::InvisibleButton(id, ImVec2(card_w, card_h));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();

    dl->AddRectFilled(p0, p1,
                      dark ? IM_COL32(255, 255, 255, hovered ? 18 : 9)
                           : IM_COL32(0, 0, 0, hovered ? 15 : 7),
                      font * 0.3f);
    dl->AddRect(p0, p1, ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_Separator), font * 0.3f);

    const ImVec2 t0(p0.x + pad, p0.y + pad);
    const ImVec2 t1(t0.x + thumb, t0.y + thumb);
    if (tex != nullptr)
        dl->AddImage((ImTextureID)(intptr_t) tex->get_id(), t0, t1);
    else
        dl->AddRect(t0, t1, ImGui::GetColorU32(ImGuiCol_TextDisabled), font * 0.2f);

    const std::string fname = image_path.empty()
        ? _u8L("No image")
        : image_path.substr(image_path.find_last_of("/\\") + 1);
    const float tx = t1.x + pad * 1.5f;
    dl->PushClipRect(ImVec2(tx, p0.y), ImVec2(p1.x - pad, p1.y), true);
    dl->AddText(ImVec2(tx, t0.y + font * 0.05f),
                ImGui::GetColorU32(image_path.empty() ? ImGuiCol_TextDisabled : ImGuiCol_Text),
                fname.c_str());
    dl->AddText(ImVec2(tx, t0.y + font * 1.30f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                _u8L("Click to choose a PNG").c_str());
    dl->PopClipRect();

    if (hovered && !image_path.empty())
        ImGui::SetTooltip("%s", image_path.c_str());

    if (clicked) {
        wxFileDialog dialog(nullptr, dialog_title, wxEmptyString, wxEmptyString,
                            "PNG files (*.png)|*.png", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() == wxID_OK) {
            const std::string new_path = dialog.GetPath().ToStdString();
            if (!new_path.empty()) {
                cache.erase(new_path); // re-load in case the file changed on disk since last use
                image_path = new_path;
                return true;
            }
        }
    }
    return false;
}

// ============================================================================================
// Texture Bump (All mode / per-zone) config form
// ============================================================================================

namespace {

// Grabber legend colors -- keep in sync with m_grabbers[0..3] in GLGizmoTextureBump::on_init().
constexpr ImU32 kDotScale  = IM_COL32(77, 153, 255, 255);  // V handle, blue
constexpr ImU32 kDotRepeat = IM_COL32(255, 153, 51, 255);  // U handle, orange
constexpr ImU32 kDotYaw    = IM_COL32(178, 77, 255, 255);  // yaw ring, purple
constexpr ImU32 kDotPivot  = IM_COL32(255, 77, 128, 255);  // pivot, pink

// One read-only "<dot> Name value" stat for the All-mode handle legend.
void bump_handle_stat(ImU32 col, const std::string& name, const std::string& value)
{
    bump_handle_dot(col, ImGui::GetTextLineHeight() * 0.5f);
    ImGui::TextUnformatted(name.c_str());
    ImGui::SameLine(0.0f, ImGui::GetFontSize() * 0.35f);
    ImGui::TextDisabled("%s", value.c_str());
}

} // anonymous namespace

void render_texture_bump_config_sections(ImGuiWrapper* imgui, TextureBumpConfig& cfg,
                                          const std::function<void(const char* field)>& on_commit,
                                          bool show_plane_handles_hint)
{
    const BumpSectionIcons& icons = bump_section_icons();

    // ---- Source ----
    bump_ui_section(icons.source, _u8L("Source"));
    // The engine option is an enum (None / AllWalls) but only those two values are exposed, so
    // the UI is a plain enable toggle -- same interaction as Top mode's "Enable Z Bump".
    bool enabled = cfg.type != TextureBumpType::None;
    if (imgui->checkbox(_L("Enable wall texture"), enabled)) {
        cfg.type = enabled ? TextureBumpType::AllWalls : TextureBumpType::None;
        on_commit("type");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Emboss the image onto every wall of the object.").c_str());
    if (cfg.type == TextureBumpType::None)
        return;

    if (bump_ui_image_picker("##tb_image_card", cfg.image_path, _u8L("Select a PNG displacement map")))
        on_commit("image_path");

    const std::vector<std::string> mode_options = { _u8L("Planar"), _u8L("Cylindrical"), _u8L("Spherical"), _u8L("Cubic") };
    int mode_idx = static_cast<int>(cfg.projection_mode);
    if (imgui->combo(_u8L("Projection"), mode_options, mode_idx, 0, bump_ui_label_width(), bump_ui_control_width())) {
        cfg.projection_mode = static_cast<TextureProjectionMode>(mode_idx);
        on_commit("projection_mode");
    }
    if (cfg.projection_mode == TextureProjectionMode::Cylindrical || cfg.projection_mode == TextureProjectionMode::Spherical) {
        const std::vector<std::string> axis_options = { _u8L("X"), _u8L("Y"), _u8L("Z") };
        int axis_idx = static_cast<int>(cfg.axis);
        if (imgui->combo(_u8L("Axis"), axis_options, axis_idx, 0, bump_ui_label_width(), bump_ui_control_width())) {
            cfg.axis = static_cast<TextureProjectionAxis>(axis_idx);
            on_commit("axis");
        }
    }

    // ---- Transform ----
    bump_ui_section(icons.transform, _u8L("Transform"));
    if (show_plane_handles_hint) {
        // NEOTKO_TEXTUREBUMP_TAG -- regression fix kept from the unification pass: the base
        // gizmo's "All" mode never had sliders for these 4 fields -- they are ALWAYS driven by
        // the 3D viewport handles (V/U/yaw/pivot grabbers, GLGizmoTextureBump.cpp), which update
        // every drag frame with no revert issue. Interactive fields here inherited the zone
        // editor's known-parked snap-back bug (s181), so this mode shows a read-only legend +
        // live values; only the zone editor (no 3D handle to fall back on) gets editable rows.
        ImGui::TextDisabled("%s", _u8L("Drag the matching handles in the 3D view").c_str());
        const Vec2d pivot  = texture_bump_plane_transform_pivot(cfg.plane_transform);
        const float col2_x = ImGui::GetFontSize() * 8.5f;
        bump_handle_stat(kDotScale, _u8L("Scale"), Slic3r::format("%.1f mm", cfg.scale));
        ImGui::SameLine(col2_x);
        bump_handle_stat(kDotRepeat, _u8L("Repeat"), Slic3r::format("x%d", cfg.repeat_u));
        bump_handle_stat(kDotYaw, _u8L("Yaw"),
                         Slic3r::format("%.0f deg", texture_bump_plane_transform_yaw(cfg.plane_transform) * 180.0 / M_PI));
        ImGui::SameLine(col2_x);
        bump_handle_stat(kDotPivot, _u8L("Pivot"), Slic3r::format("%.1f, %.1f", pivot.x(), pivot.y()));
        if (imgui->button(_u8L("Reset plane"))) {
            cfg.plane_transform = Transform3d::Identity();
            on_commit("plane_reset");
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", _u8L("Reset scale plane rotation and pivot to defaults.").c_str());
    } else {
        // NEOTKO_TEXTUREBUMP_TAG (s184) -- live-persist-then-commit-on-release contract: persist
        // the value into cfg on every change frame (survives to the release frame), fire
        // on_commit (the heavy reslice) only once, on release. See BumpRowEdit in the header.
        float scale_mm = float(cfg.scale);
        BumpRowEdit e = bump_ui_drag_float(_u8L("Scale"), "##tb_scale", scale_mm, 0.5f, 0.1f, 1000.0f,
                                           "%.1f mm", _u8L("Size of one texture tile."));
        if (e.changed)   cfg.scale = double(scale_mm);
        if (e.committed) on_commit("scale");

        int repeat_u = cfg.repeat_u;
        e = bump_ui_drag_int(_u8L("Repeat"), "##tb_repeat_u", repeat_u, 0.05f, 1, 50, "x%d",
                             _u8L("How many times the tile repeats."));
        if (e.changed)   cfg.repeat_u = repeat_u;
        if (e.committed) on_commit("repeat_u");

        // NEOTKO_TEXTUREBUMP_TAG -- Yaw/Pivot rows removed for the zone editor (2026-07-09, user
        // report from real testing): unlike All mode's plane_transform (driven by the 3D yaw/
        // pivot grabbers, confirmed working), a painted zone's own plane_transform has no
        // equivalent effect in practice -- kept out rather than left as dead controls that look
        // functional. cfg.plane_transform itself is untouched (still Identity for every zone,
        // still serializes fine) in case this gets wired up for real later.
    }

    // ---- Relief ----
    bump_ui_section(icons.relief, _u8L("Relief"));
    float thickness_mm = unscale_(cfg.thickness);
    // NEOTKO_TEXTUREBUMP_TAG -- s184 snap-back root cause lives on in the BumpRowEdit contract:
    // the dragged value is written into cfg on EVERY change frame (it only ever lived in this
    // local during active frames and was lost by the release frame otherwise); the commit --
    // the heavy reslice -- still fires only once, on release.
    BumpRowEdit e = bump_ui_drag_float(_u8L("Thickness"), "##tb_thickness", thickness_mm, 0.005f,
                                       0.0f, 2.0f, "%.2f mm",
                                       _u8L("Maximum wall displacement where the image is full white."));
    if (e.changed)   cfg.thickness = scaled<coord_t>(double(thickness_mm));
    if (e.committed) on_commit("thickness");

    float point_distance_mm = unscale_(cfg.point_distance);
    e = bump_ui_drag_float(_u8L("Detail"), "##tb_point_distance", point_distance_mm, 0.01f,
                           0.05f, 5.0f, "%.2f mm",
                           _u8L("Point distance: sampling step along each wall. Smaller = finer relief, heavier G-code."));
    if (e.changed)   cfg.point_distance = scaled<coord_t>(double(point_distance_mm));
    if (e.committed) on_commit("point_distance");

    // ---- Safety (collapsed by default -- mirrors these fields' comAdvanced mode, PrintConfig.cpp) ----
    bump_ui_section(icons.safety, _u8L("Safety"));
    if (ImGui::TreeNodeEx("##tb_safety", ImGuiTreeNodeFlags_None, "%s", _u8L("Slope limiter").c_str())) {
        if (imgui->checkbox(_L("Apply to first layer"), cfg.first_layer))
            on_commit("first_layer");

        float max_angle_deg = float(cfg.max_angle_rad * 180.0 / M_PI);
        e = bump_ui_drag_float(_u8L("Max angle"), "##tb_max_angle", max_angle_deg, 0.5f, 5.0f, 90.0f,
                               "%.0f deg", _u8L("Steepest allowed relief slope; steeper features get flattened."));
        if (e.changed)   cfg.max_angle_rad = double(max_angle_deg) * M_PI / 180.0;
        if (e.committed) on_commit("max_angle");

        float blur_strength = float(cfg.blur_strength);
        e = bump_ui_drag_float(_u8L("Blur"), "##tb_blur_strength", blur_strength, 0.005f, 0.0f, 1.0f,
                               "%.2f", _u8L("Softens the limited slopes instead of clipping them hard."));
        if (e.changed)   cfg.blur_strength = double(blur_strength);
        if (e.committed) on_commit("blur_strength");
        ImGui::TreePop();
    }
}

} // namespace Slic3r::GUI
