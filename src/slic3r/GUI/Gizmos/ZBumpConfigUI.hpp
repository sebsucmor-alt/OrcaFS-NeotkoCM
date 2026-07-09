#ifndef slic3r_ZBumpConfigUI_hpp_
#define slic3r_ZBumpConfigUI_hpp_

#include "libslic3r/Feature/ZBump/ZBump.hpp" // ZBumpConfig
#include <functional>

namespace Slic3r {
class ModelObject;
}

namespace Slic3r::GUI {

class ImGuiWrapper;

// NEOTKO_ZBUMP_TAG -- Top mode's own shadow-config resolver: model_object's zbump_* options fall
// back to the global preset, same pattern TextureBumpConfigUI's resolve_base_texture_bump_config()
// uses -- but its own function/struct, no shared code (different domain/fields).
Feature::ZBump::ZBumpConfig resolve_base_zbump_config(const ModelObject* model_object);

// NEOTKO_ZBUMP_TAG -- approximate inset, in mm, from the object's own outer contour to where
// top-fill actually starts (wall_loops perimeter loops eating into it). GUI-preview use only
// (GLGizmoTextureBump.cpp's Top-mode overlay) -- the real slice always uses its own precise
// ExPolygon offsetting regardless of what this estimates.
double zbump_perimeter_inset_mm(const ModelObject* model_object);

// NEOTKO_ZBUMP_TAG -- single form for editing a ZBumpConfig. Mirrors the "persist to cfg on every
// change frame, on_commit only on release" pattern render_texture_bump_config_sections()
// (TextureBumpConfigUI.hpp) established to fix an ImGui slider snap-back-on-release bug -- applied
// here by construction, not rediscovered -- but this is its own function/panel, not a branch
// bolted onto that one (different field set: no projection/axis, has edge ramp/max slope).
//
// `on_commit(field)` fires once per interaction that changed a field, AFTER `cfg` has already
// been mutated in place -- `field` is one of: "enabled", "image_path", "thickness", "scale",
// "repeat", "offset_x", "offset_y", "edge_margin", "max_slope", "first_layer".
void render_zbump_config_section(ImGuiWrapper* imgui, Feature::ZBump::ZBumpConfig& cfg,
                                  const std::function<void(const char* field)>& on_commit);

} // namespace Slic3r::GUI

#endif // slic3r_ZBumpConfigUI_hpp_
