#ifndef slic3r_TextureBumpConfigUI_hpp_
#define slic3r_TextureBumpConfigUI_hpp_

#include "libslic3r/PerimeterGenerator.hpp" // TextureBumpConfig
#include <functional>
#include <string>

#include <imgui/imgui.h> // ImTextureID / ImU32 for the shared row widgets below

namespace Slic3r {
class ModelObject;
}

namespace Slic3r::GUI {

class ImGuiWrapper;

// NEOTKO_TEXTUREBUMP_TAG -- compact-row widget kit shared by every mode of the Bump Mapping
// Editor panel (All / zone editor here, Top in ZBumpConfigUI.cpp). One row = label column +
// mouse-draggable value field (ImGui::DragFloat: drag to scrub, Ctrl+click to type), replacing
// the old label-line + full-width-slider-line pairs. Lives here so both config UIs draw
// pixel-identical rows.

// Section icons (texture_bump_*.svg), loaded once and shared by all modes.
struct BumpSectionIcons
{
    ImTextureID source     = nullptr;
    ImTextureID projection = nullptr;
    ImTextureID transform  = nullptr;
    ImTextureID relief     = nullptr;
    ImTextureID safety     = nullptr;
};
const BumpSectionIcons& bump_section_icons();

// Fixed two-column geometry every row shares (label column / control width), so the
// auto-resizing gizmo window settles at one consistent width across all three modes.
float bump_ui_label_width();
float bump_ui_control_width();

// Icon + label + hairline rule filling the rest of the row.
void bump_ui_section(ImTextureID icon, const std::string& label);

// `changed` = value moved this frame (persist it into cfg so it survives to the release frame --
// same live-persist-then-commit-on-release contract the old sliders used, see the s184 snap-back
// root cause note in TextureBumpConfigUI.cpp). `committed` = interaction ended (fire on_commit).
struct BumpRowEdit { bool changed = false; bool committed = false; };

// `dot` (0 = none) draws a small colored bullet before the label, tying the row to the
// same-colored 3D viewport handle. `warn` tints label + value red (details go in `tooltip`).
// v_min == v_max means unclamped (pan/offset fields are a tiling phase, any value is valid).
BumpRowEdit bump_ui_drag_float(const std::string& label, const char* id, float& v, float speed,
                               float v_min, float v_max, const char* fmt,
                               const std::string& tooltip = {}, bool warn = false, ImU32 dot = 0);
BumpRowEdit bump_ui_drag_int(const std::string& label, const char* id, int& v, float speed,
                             int v_min, int v_max, const char* fmt,
                             const std::string& tooltip = {});
// Two side-by-side fields on one row (pivot / pan X-Y pairs).
BumpRowEdit bump_ui_drag_float2(const std::string& label, const char* id, float& a, float& b,
                                float speed, float v_min, float v_max, const char* fmt,
                                const std::string& tooltip = {}, ImU32 dot = 0);

// Thumbnail + filename card; the whole card is one click target that opens a PNG file dialog.
// Full path lives in the tooltip. Returns true when a new file was picked (path already updated).
bool bump_ui_image_picker(const char* id, std::string& image_path, const std::string& dialog_title);

// NEOTKO_TEXTUREBUMP_TAG -- unification pass (docs/ATTRIBUTION_TEXTURE_BUMP.md §6 handoff):
// promoted out of GLGizmoTextureBumpPainter.cpp (was `static`, zone-seeding only) so
// GLGizmoTextureBump's "All" mode can build the exact same shadow TextureBumpConfig from the
// object's texture_bump_* options instead of mirroring each field into its own separate member.
// Does NOT read ModelObject::texture_bump_plane_transform (not a ConfigOption, lives directly on
// the model) -- callers that need it (the base gizmo's live config) overlay it themselves after
// calling this; zone-seeding callers intentionally leave plane_transform at Identity for a new
// zone, unchanged behavior from before this move.
TextureBumpConfig resolve_base_texture_bump_config(const ModelObject* model_object);

// NEOTKO_TEXTUREBUMP_TAG -- single shared form for editing a TextureBumpConfig, replacing two
// near-identical hand-written ImGui panels (GLGizmoTextureBump.cpp's "All" mode,
// GLGizmoTextureBumpPainter.cpp's per-zone editor). Draws 4 icon-labeled sections: Source
// (enable + image + projection), Transform, Relief, Safety (collapsed by default, mirrors these
// fields' comAdvanced mode in PrintConfig.cpp).
//
// `on_commit(field)` fires once per interaction that changed a field, AFTER `cfg` has already
// been mutated in place -- `field` is one of: "type", "image_path", "projection_mode", "axis",
// "scale", "repeat_u", "plane_yaw", "plane_pivot", "plane_reset", "thickness", "point_distance",
// "first_layer", "max_angle", "blur_strength". The caller decides how to persist it (a
// PrintRegionConfig set_key_value() per field for the object-wide config; a single
// commit_and_reslice() for a zone, since the struct field is already written by reference).
//
// `show_plane_handles_hint`: true for the base gizmo's "All" mode (shows a one-line reminder
// that scale/repeat/yaw/pivot can also be dragged with the 3D viewport handles); false for the
// painter's per-zone editor (a painted zone has no single on-screen 3D handle to hint at).
void render_texture_bump_config_sections(ImGuiWrapper* imgui, TextureBumpConfig& cfg,
                                          const std::function<void(const char* field)>& on_commit,
                                          bool show_plane_handles_hint);

} // namespace Slic3r::GUI

#endif // slic3r_TextureBumpConfigUI_hpp_
