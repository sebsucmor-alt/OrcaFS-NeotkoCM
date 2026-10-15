// NEOTKO_IMPORT_TAG_START
// NeotkoImportCarry — lo que tiene que VIAJAR con un objeto cuando un 3mf se IMPORTA dentro de un
// proyecto ya abierto (soltar el fichero y elegir «Import geometry only», File > Import, Reload
// from disk), en vez de abrirse como proyecto.
//
// DUEÑO ÚNICO de ese viaje. Los ficheros de upstream sólo llaman a las tres entradas de abajo:
//   Format/bbs_3mf.cpp  load_bbs_3mf()                 -> PaletteMerge
//   Model.cpp           Model::convert_multipart_object -> stickers_into_multipart
//   GUI/Plater.cpp      Plater::priv::load_model_objects-> shift_stickers
//
// Por qué hace falta (s362):
//  1. La paleta de recetas (SurfaceEffectProfileManager) es UNA para todo el programa y la pintura
//     y las pegatinas guardan sólo el NÚMERO de receta. El lector del 3mf vaciaba la paleta y
//     cargaba la del fichero: lo que ya había en el proyecto se quedaba sin receta (o con otra).
//  2. «Multi-part object detected» → Yes crea un objeto nuevo y las pegatinas no pasaban a él.
//  3. Ese objeto nuevo se centra al entrar; ModelObject::translate mueve los volúmenes pero no las
//     pegatinas (mismo fallo que s212 en Assemble).
#ifndef slic3r_NeotkoImportCarry_hpp_
#define slic3r_NeotkoImportCarry_hpp_

#include "Point.hpp"
#include "SurfaceEffectProfile.hpp"

#include <vector>

namespace Slic3r {

class Model;
class ModelObject;

namespace NeotkoImportCarry {

// Envuelve la lectura de un 3mf. Con `active` (importar): aparta la paleta del proyecto, deja que
// el lector cargue la del fichero en una paleta VACÍA y, en finish(), devuelve la del proyecto,
// le añade una COPIA de cada receta que el modelo importado usa (siempre número nuevo, nunca
// se comparte con una igual del proyecto: lo importado se edita sin tocar lo que ya había)
// y renumera pegatinas y huecos pintados del modelo importado.
// Sin `active` (abrir como proyecto) no hace nada. Si finish() no llega a llamarse (lectura
// fallida, excepción), el destructor devuelve la paleta del proyecto intacta.
class PaletteMerge
{
public:
    explicit PaletteMerge(bool active);
    ~PaletteMerge();
    PaletteMerge(const PaletteMerge&)            = delete;
    PaletteMerge& operator=(const PaletteMerge&) = delete;

    void finish(Model& imported);

private:
    bool                              m_active = false;
    std::vector<SurfaceEffectProfile> m_kept;
    int                               m_kept_next_id = 1;
};

// Pasa las pegatinas de los objetos que Model::convert_multipart_object está juntando al objeto
// resultante, en el mismo marco al que van sus volúmenes (instancia * des-centrado).
void stickers_into_multipart(const std::vector<ModelObject*>& sources, ModelObject& merged);

// Mueve las pegatinas de un objeto lo mismo que se acaban de mover sus volúmenes.
void shift_stickers(ModelObject& object, const Vec3d& shift);

} // namespace NeotkoImportCarry
} // namespace Slic3r

#endif // slic3r_NeotkoImportCarry_hpp_
// NEOTKO_IMPORT_TAG_END
