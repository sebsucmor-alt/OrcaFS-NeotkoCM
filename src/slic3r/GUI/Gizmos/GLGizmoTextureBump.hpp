#ifndef slic3r_GLGizmoTextureBump_hpp_
#define slic3r_GLGizmoTextureBump_hpp_

#include "GLGizmoBase.hpp"

// NEOTKO_TEXTUREBUMP_TAG — see docs/ATTRIBUTION_TEXTURE_BUMP.md. v1 scope: this gizmo does not
// paint a region (unlike GLGizmoFuzzySkin/GLGizmoPainterBase) -- Fuzzy Skin's own painting-related
// parameters are edited through its gizmo, but its non-painting parameters (thickness, noise
// type, ...) live in the standalone Object Settings panel, same as the rest of Texture Bump's
// settings here. The one exception is the image path: the file dialog writes
// texture_bump_image_path directly onto the selected object's config override
// (object->config.set_key_value(...), the same primitive GUI_ObjectList.cpp uses everywhere) so
// the user doesn't have to hand-type/paste an absolute path.

namespace Slic3r {

namespace GUI {

class GLGizmoTextureBump : public GLGizmoBase
{
public:
    GLGizmoTextureBump(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

protected:
    bool        on_init() override;
    std::string on_get_name() const override;
    bool        on_is_activable() const override;
    bool        on_is_selectable() const override;
    void        on_render() override {}
    void        on_render_input_window(float x, float y, float bottom_limit) override;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoTextureBump_hpp_
