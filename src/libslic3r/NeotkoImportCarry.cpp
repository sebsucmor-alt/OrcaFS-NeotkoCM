// NEOTKO_IMPORT_TAG_START — ver NeotkoImportCarry.hpp
#include "NeotkoImportCarry.hpp"

#include "ColorStitch.hpp" // NEOTKO_LOG / canal PROFILE
#include "Model.hpp"

#include <map>
#include <set>

namespace Slic3r {
namespace NeotkoImportCarry {

// Aplica `fn` a cada número de receta que el modelo referencia (pegatinas y huecos pintados).
template<class Fn> static void for_each_recipe_ref(Model& model, Fn fn)
{
    for (ModelObject* mo : model.objects) {
        if (mo == nullptr) continue;
        for (ColorStitchSticker& st : mo->colorstitch_stickers) {
            fn(st.profile_id);
            for (ColorStitchStickerColor& c : st.colors) {
                fn(c.profile_id);
                for (int& bp : c.band_profiles) fn(bp);
            }
        }
        for (ModelVolume* mv : mo->volumes) {
            if (mv == nullptr) continue;
            for (int s = 1; s < ModelVolume::COLORSTITCH_SLOT_COUNT; ++s)
                fn(mv->colorstitch_slot_to_profile_id[s]);
        }
    }
}

PaletteMerge::PaletteMerge(bool active) : m_active(active)
{
    if (!m_active) return;
    auto& mgr      = SurfaceEffectProfileManager::get();
    m_kept         = mgr.list();
    m_kept_next_id = mgr.next_id();
    mgr.clear();
    NEOTKO_LOG(PROFILE, "IMPORT_CARRY begin: project palette set aside, recipes=" << m_kept.size()
        << " next_id=" << m_kept_next_id);
}

PaletteMerge::~PaletteMerge()
{
    if (!m_active) return;
    SurfaceEffectProfileManager::get().restore(std::move(m_kept), m_kept_next_id);
    NEOTKO_LOG(PROFILE, "IMPORT_CARRY aborted: project palette put back untouched");
}

void PaletteMerge::finish(Model& imported)
{
    if (!m_active) return;
    m_active = false;

    auto& mgr = SurfaceEffectProfileManager::get();
    // Lo que el lector ha dejado: la paleta del FICHERO (ya migrada y con los huecos huérfanos
    // recuperados), numerada como en el fichero.
    const std::vector<SurfaceEffectProfile> from_file = mgr.list();
    const size_t n_kept = m_kept.size();
    mgr.restore(std::move(m_kept), m_kept_next_id);

    std::set<int> used;
    for_each_recipe_ref(imported, [&used](int& pid) { if (pid != 0) used.insert(pid); });

    std::map<int, int> remap; // número en el fichero -> número en el proyecto
    int added = 0;
    for (const SurfaceEffectProfile& p : from_file) {
        if (used.count(p.id) == 0) continue;
        // SIEMPRE copia, aunque el proyecto ya tenga una receta idéntica (p. ej. al importar el
        // propio fichero para poner variantes al lado): si se compartiera, editar la receta de lo
        // importado cambiaría también lo que ya estaba. add() asigna número nuevo.
        const int target = mgr.add(p);
        ++added;
        remap[p.id] = target;
        NEOTKO_LOG(PROFILE, "IMPORT_CARRY recipe file_id=" << p.id << " -> project_id=" << target
            << " name='" << p.name << "' auto=" << (p.auto_generated ? 1 : 0));
    }

    int refs = 0, dead = 0;
    for_each_recipe_ref(imported, [&](int& pid) {
        if (pid == 0) return;
        const auto it = remap.find(pid);
        if (it != remap.end()) { pid = it->second; ++refs; }
        else                   { pid = 0;          ++dead; } // el fichero no traía esa receta
    });

    size_t n_stickers = 0;
    for (const ModelObject* mo : imported.objects)
        if (mo != nullptr) n_stickers += mo->colorstitch_stickers.size();
    NEOTKO_LOG(PROFILE, "IMPORT_CARRY done: project_recipes=" << n_kept
        << " file_recipes=" << from_file.size() << " used_by_import=" << used.size()
        << " added=" << added
        << " refs_renumbered=" << refs << " refs_without_recipe=" << dead
        << " objects=" << imported.objects.size() << " stickers=" << n_stickers
        << " palette_now=" << mgr.size());
}

void stickers_into_multipart(const std::vector<ModelObject*>& sources, ModelObject& merged)
{
    for (const ModelObject* o : sources) {
        if (o == nullptr || o->colorstitch_stickers.empty()) continue;
        // Mismo marco que los volúmenes en Model::convert_multipart_object: se deshace el
        // centrado del objeto de origen y se aplica cada instancia (sin instancias, sólo lo primero).
        const Transform3d uncentre(Eigen::Translation3d(-o->origin_translation));
        std::vector<Transform3d> frames;
        if (o->instances.empty())
            frames.push_back(uncentre);
        else
            for (const ModelInstance* i : o->instances)
                frames.push_back(i->get_transformation().get_matrix() * uncentre);
        for (const Transform3d& frame : frames)
            for (const ColorStitchSticker& st : o->colorstitch_stickers) {
                ColorStitchSticker moved = st;
                moved.transform = frame * st.transform;
                const Vec3d a = moved.transform * Vec3d::Zero();
                NEOTKO_LOG(PROFILE, "IMPORT_CARRY multipart sticker='" << st.name << "' from='" << o->name
                    << "' anchor=[" << a.x() << "," << a.y() << "," << a.z() << "]");
                merged.colorstitch_stickers.emplace_back(std::move(moved));
            }
    }
}

void shift_stickers(ModelObject& object, const Vec3d& shift)
{
    if (object.colorstitch_stickers.empty() || shift.isZero()) return;
    for (ColorStitchSticker& st : object.colorstitch_stickers)
        st.transform = Eigen::Translation3d(shift) * st.transform;
    NEOTKO_LOG(PROFILE, "IMPORT_CARRY shift obj='" << object.name << "' stickers="
        << object.colorstitch_stickers.size()
        << " by=[" << shift.x() << "," << shift.y() << "," << shift.z() << "]");
}

} // namespace NeotkoImportCarry
} // namespace Slic3r
// NEOTKO_IMPORT_TAG_END
