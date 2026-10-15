// s348 — Fase A: ver los stickers SIEMPRE (no solo el que se edita). Dueño único del dibujo:
// lo usan el gizmo ColorStitch (todos los objetos, menos el sticker en edición, que pinta el
// gizmo con su contorno) y GLCanvas3D en la vista normal (bajo el mismo interruptor que la
// pintura: ColorStitchPaintPreview::show_outside_gizmo()).
#ifndef slic3r_ColorStitchStickerPreview_hpp_
#define slic3r_ColorStitchStickerPreview_hpp_

#include "libslic3r/Point.hpp"   // Transform3d

#include <vector>

namespace Slic3r {
struct ColorStitchSticker;
class Model;
class ModelObject;
namespace GUI {
struct Camera;
namespace ColorStitchStickerPreview {

// Dibuja los stickers de todos los objetos (todas sus instancias). Un relleno por color, con la
// MISMA oclusión que el corte (ColorStitch::sticker_color_masks) y el color de su receta.
// skip_object/skip_sticker: el que el gizmo ya dibuja (en edición); -1 = ninguno.
void render(const Model& model, const Camera& camera, int skip_object = -1, int skip_sticker = -1);

// s349 — UN sticker (el que se edita en el gizmo): mismos rellenos recortados a la cara que
// render(), con la matriz de la instancia dada y la opacidad pedida. Sin contorno.
// s350 — focus_color ≥ 0: ese color del sticker (y, si focus_b0 ≥ 0, sólo sus bandas
// [focus_b0, focus_b1]) se RESALTA latiendo con `pulse` (0..1) y el resto se atenúa: es el
// «estás cambiando esta zona» del panel de colores.
void render_one(const ModelObject& mo, int sticker_idx, const Transform3d& instance_matrix,
                const Camera& camera, float alpha,
                int focus_color = -1, int focus_b0 = -1, int focus_b1 = -1, float pulse = 0.f);

// s350 — el sticker en PLANO para dibujarlo en ImGui (panel «Colours of this SVG»): por cada
// máscara de color (misma oclusión que el corte), sus triángulos en mm del frame local del
// sticker, con su color_idx y, si es un degradado suave, sus extremos. bb = caja de todo.
struct Flat2D {
    std::vector<std::vector<Vec2f>> tris;        // 3 vértices por triángulo, mm
    std::vector<int>                color_idx;   // 1:1 con tris (-1 = 1 color; ≥1000 = banda)
    std::vector<char>               grad;        // 1:1: degradado suave
    std::vector<Vec2d>              g0, g1;      // 1:1: extremos en mm (si grad)
    Vec2f                           bb_min = Vec2f::Zero(), bb_max = Vec2f::Zero();
};
const Flat2D& flat_2d(const ColorStitchSticker& st);

// Vacía la caché de geometría (al cerrar proyecto, etc.). Se poda sola si crece.
void clear_cache();

// Alzado (mm, Z mundo) del sticker n.º `pile_index` de su pila: crece hacia arriba para que dos
// stickers solapados no peleen en profundidad. El overlay de edición usa el mismo.
double sticker_lift_mm(int pile_index);
// s350 — con el lado: un sticker bottom se separa hacia ABAJO.
double sticker_lift_mm(const ColorStitchSticker& st, int pile_index);

} // namespace ColorStitchStickerPreview
} // namespace GUI
} // namespace Slic3r

#endif
