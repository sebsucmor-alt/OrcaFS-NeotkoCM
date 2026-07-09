#include "ZBumpConfigUI.hpp"
#include "TextureBumpConfigUI.hpp" // shared compact-row widget kit + section icons

#include "libslic3r/Model.hpp"
#include "libslic3r/format.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/I18N.hpp"

#include <algorithm>

namespace Slic3r::GUI {

using Feature::ZBump::ZBumpConfig;

ZBumpConfig resolve_base_zbump_config(const ModelObject* model_object)
{
    ZBumpConfig cfg; // defaults from the struct itself: disabled, 0.4mm/20mm/1x/3mm margin/0.5 slope
    if (!model_object)
        return cfg;

    const DynamicPrintConfig& obj_cfg = model_object->config.get();
    const DynamicPrintConfig& glb_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    auto find_opt = [&](const char* key) -> const ConfigOption* {
        if (const ConfigOption* opt = obj_cfg.option(key)) return opt;
        return glb_cfg.option(key);
    };

    if (const ConfigOption* opt = find_opt("zbump_enabled"))
        cfg.enabled = static_cast<const ConfigOptionBool*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_image_path"))
        cfg.image_path = static_cast<const ConfigOptionString*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_thickness"))
        cfg.thickness_mm = static_cast<const ConfigOptionFloat*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_scale"))
        cfg.scale_mm = static_cast<const ConfigOptionFloat*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_repeat"))
        cfg.repeat = static_cast<const ConfigOptionInt*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_offset_x"))
        cfg.offset_x_mm = static_cast<const ConfigOptionFloat*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_offset_y"))
        cfg.offset_y_mm = static_cast<const ConfigOptionFloat*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_edge_margin"))
        cfg.edge_margin_mm = static_cast<const ConfigOptionFloat*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_max_slope"))
        cfg.max_slope = static_cast<const ConfigOptionFloat*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_first_layer"))
        cfg.first_layer = static_cast<const ConfigOptionBool*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_relief_segment"))
        cfg.relief_segment_mm = static_cast<const ConfigOptionFloat*>(opt)->value;
    if (const ConfigOption* opt = find_opt("zbump_max_passes"))
        cfg.max_passes = std::max(1, static_cast<const ConfigOptionInt*>(opt)->value);
    return cfg;
}

namespace {

// NEOTKO_ZBUMP_TAG -- warning only, explicitly NOT a hard cap (user decision: wants to
// experiment past this and judge from the resulting G-code himself; a real clamp can come later
// once there's print evidence for where it should actually sit). Same heuristic already used
// for max layer height (~0.8x nozzle diameter) -- a single continuous relief pass can't climb
// further above the solid layer beneath it than a normal layer could, for the same reason
// (the bead runs out of reach/bonding). Room left for the bump is that ceiling minus whatever
// height this layer's own extrusion already uses.
double zbump_safe_max_offset_mm()
{
    double nozzle_mm = 0.4;
    if (const auto* opt = wxGetApp().preset_bundle->printers.get_edited_preset()
                              .config.option<ConfigOptionFloats>("nozzle_diameter"))
        if (!opt->values.empty() && opt->values.front() > 0.001)
            nozzle_mm = opt->values.front();

    double layer_height_mm = 0.2;
    if (const auto* opt = wxGetApp().preset_bundle->prints.get_edited_preset()
                              .config.option<ConfigOptionFloat>("layer_height"))
        if (opt->value > 0.001)
            layer_height_mm = opt->value;

    return std::max(0.0, 0.8 * nozzle_mm - layer_height_mm);
}

} // anonymous namespace

double zbump_perimeter_inset_mm(const ModelObject* model_object)
{
    const DynamicPrintConfig& glb_cfg = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const DynamicPrintConfig* obj_cfg = model_object ? &model_object->config.get() : nullptr;
    auto find_opt = [&](const char* key) -> const ConfigOption* {
        if (obj_cfg)
            if (const ConfigOption* opt = obj_cfg->option(key)) return opt;
        return glb_cfg.option(key);
    };

    double nozzle_mm = 0.4;
    if (const auto* opt = wxGetApp().preset_bundle->printers.get_edited_preset()
                              .config.option<ConfigOptionFloats>("nozzle_diameter"))
        if (!opt->values.empty() && opt->values.front() > 0.001)
            nozzle_mm = opt->values.front();

    int wall_loops = 2;
    if (const auto* opt = dynamic_cast<const ConfigOptionInt*>(find_opt("wall_loops")))
        wall_loops = std::max(0, opt->value);
    if (wall_loops <= 0)
        return 0.0;

    // line_width is the base "100%" most wall-width percent options are relative to; 0/auto
    // falls back to nozzle diameter, the same rough approximation Flow::auto_extrusion_width()
    // converges to for standard wall roles.
    double line_width_mm = nozzle_mm;
    if (const auto* opt = dynamic_cast<const ConfigOptionFloatOrPercent*>(find_opt("line_width"))) {
        const double v = opt->get_abs_value(nozzle_mm);
        if (v > 0.001) line_width_mm = v;
    }
    auto wall_width_mm = [&](const char* key) -> double {
        if (const auto* opt = dynamic_cast<const ConfigOptionFloatOrPercent*>(find_opt(key))) {
            const double v = opt->get_abs_value(line_width_mm);
            if (v > 0.001) return v;
        }
        return line_width_mm;
    };
    const double outer_mm = wall_width_mm("outer_wall_line_width");
    const double inner_mm = wall_width_mm("inner_wall_line_width");
    // One outer loop + (wall_loops-1) inner loops, same convention PerimeterGenerator uses.
    return outer_mm + double(wall_loops - 1) * inner_mm;
}

void render_zbump_config_section(ImGuiWrapper* imgui, ZBumpConfig& cfg,
                                  const std::function<void(const char* field)>& on_commit)
{
    // NEOTKO_ZBUMP_TAG -- drawn entirely with the Bump Mapping Editor's shared compact-row kit
    // (TextureBumpConfigUI.hpp) so all three panel modes stay pixel-identical; the kit also
    // carries the s184 live-persist-then-commit-on-release contract that fixed the ImGui
    // snap-back-on-release bug, so it can't be reintroduced here field by field.
    const BumpSectionIcons& icons = bump_section_icons();

    // ---- Source ----
    bump_ui_section(icons.source, _u8L("Source"));
    if (imgui->checkbox(_L("Enable Z Bump"), cfg.enabled))
        on_commit("enabled");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Emboss the image as height relief into the top surfaces.").c_str());

    if (!cfg.enabled)
        return;

    if (bump_ui_image_picker("##zb_image_card", cfg.image_path, _u8L("Select a PNG height map")))
        on_commit("image_path");

    // ---- Relief ----
    bump_ui_section(icons.relief, _u8L("Relief"));
    // NEOTKO_ZBUMP_TAG -- warning only, explicitly NOT a hard cap (see zbump_safe_max_offset_mm()
    // above): past the estimate the label/value turn red and the tooltip explains, nothing blocks.
    const double safe_max_offset_mm = zbump_safe_max_offset_mm();
    const double safe_max_total_mm  = safe_max_offset_mm * double(std::max(1, cfg.max_passes));
    const bool   zbump_exceeds_safe = cfg.thickness_mm > safe_max_total_mm;
    std::string height_tip = Slic3r::format(
        _u8L("Maximum top-surface displacement where the image is full white.\n"
             "Safe total for this nozzle/layer height: ~%.2f mm (%d pass(es) x ~%.2f mm)."),
        safe_max_total_mm, cfg.max_passes, safe_max_offset_mm);
    if (zbump_exceeds_safe)
        height_tip += "\n" + _u8L("Above the safe estimate -- experimental, not blocked.");
    float thickness_mm = float(cfg.thickness_mm);
    BumpRowEdit e = bump_ui_drag_float(_u8L("Height"), "##zb_thickness", thickness_mm, 0.01f,
                                       0.0f, 5.0f, "%.2f mm", height_tip, zbump_exceeds_safe);
    if (e.changed)   cfg.thickness_mm = double(thickness_mm);
    if (e.committed) on_commit("thickness");

    int max_passes_ui = cfg.max_passes;
    e = bump_ui_drag_int(_u8L("Passes"), "##zb_max_passes", max_passes_ui, 0.05f, 1, 10, "%d",
                         _u8L("Reinforcement passes: split the total relief height across several stacked passes."));
    if (e.changed)   cfg.max_passes = max_passes_ui;
    if (e.committed) on_commit("max_passes");

    // ---- Transform ----
    bump_ui_section(icons.transform, _u8L("Transform"));
    float scale_mm = float(cfg.scale_mm);
    e = bump_ui_drag_float(_u8L("Scale"), "##zb_scale", scale_mm, 0.5f, 0.1f, 1000.0f, "%.1f mm",
                           _u8L("Size of one image tile."));
    if (e.changed)   cfg.scale_mm = double(scale_mm);
    if (e.committed) on_commit("scale");

    int repeat = cfg.repeat;
    e = bump_ui_drag_int(_u8L("Repeat"), "##zb_repeat", repeat, 0.05f, 1, 50, "x%d",
                         _u8L("How many times the tile repeats."));
    if (e.changed)   cfg.repeat = repeat;
    if (e.committed) on_commit("repeat");

    // NEOTKO_ZBUMP_TAG -- numeric fallback for the 3D pan handle (GLGizmoTextureBump.cpp, Top
    // mode grabber 4 -- the green dot ties these rows to it). No min/max clamp (v_min == v_max
    // means unclamped in the row kit) -- offset is a tiling PHASE shift (wraps via fmod in
    // ZBump.cpp's build()), any value is valid, a hard range would just be an arbitrary
    // drag-speed limit.
    const ImU32       kDotPan = IM_COL32(51, 230, 102, 255); // grabber 4, green
    const std::string pan_tip = _u8L("Tiling phase shift -- drag the green handle in the 3D view, or scrub here. Wraps, any value is valid.");
    float offset_x_mm = float(cfg.offset_x_mm);
    e = bump_ui_drag_float(_u8L("Pan X"), "##zb_offset_x", offset_x_mm, 0.5f, 0.0f, 0.0f,
                           "%.1f mm", pan_tip, false, kDotPan);
    if (e.changed)   cfg.offset_x_mm = double(offset_x_mm);
    if (e.committed) on_commit("offset_x");

    float offset_y_mm = float(cfg.offset_y_mm);
    e = bump_ui_drag_float(_u8L("Pan Y"), "##zb_offset_y", offset_y_mm, 0.5f, 0.0f, 0.0f,
                           "%.1f mm", pan_tip, false, kDotPan);
    if (e.changed)   cfg.offset_y_mm = double(offset_y_mm);
    if (e.committed) on_commit("offset_y");

    float edge_margin_mm = float(cfg.edge_margin_mm);
    e = bump_ui_drag_float(_u8L("Edge ramp"), "##zb_edge_margin", edge_margin_mm, 0.05f,
                           0.0f, 20.0f, "%.1f mm",
                           _u8L("Smoothstep margin from the top-fill contour, so the wall itself stays flat."));
    if (e.changed)   cfg.edge_margin_mm = double(edge_margin_mm);
    if (e.committed) on_commit("edge_margin");

    // ---- Safety (collapsed by default -- mirrors comAdvanced mode, PrintConfig.cpp) ----
    bump_ui_section(icons.safety, _u8L("Safety"));
    if (ImGui::TreeNodeEx("##zb_safety", ImGuiTreeNodeFlags_None, "%s", _u8L("Slope limiter").c_str())) {
        if (imgui->checkbox(_L("Apply to first layer"), cfg.first_layer))
            on_commit("first_layer");

        float max_slope = float(cfg.max_slope);
        e = bump_ui_drag_float(_u8L("Max slope"), "##zb_max_slope", max_slope, 0.02f, 0.01f, 10.0f,
                               "%.2f", _u8L("Steepest allowed relief slope, in mm of Z per mm of XY travel."));
        if (e.changed)   cfg.max_slope = double(max_slope);
        if (e.committed) on_commit("max_slope");

        float relief_segment_mm = float(cfg.relief_segment_mm);
        e = bump_ui_drag_float(_u8L("Segment"), "##zb_relief_segment", relief_segment_mm, 0.05f,
                               0.1f, 20.0f, "%.1f mm",
                               _u8L("Relief segment length: how finely top-fill lines are subdivided to follow the height map."));
        if (e.changed)   cfg.relief_segment_mm = double(relief_segment_mm);
        if (e.committed) on_commit("relief_segment");
        ImGui::TreePop();
    }
}

} // namespace Slic3r::GUI
