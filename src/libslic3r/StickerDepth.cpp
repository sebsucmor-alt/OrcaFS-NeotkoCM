// Depth recipe del Sticker, F4: motor de capas (sólido extra, herramienta por capa, subcapas).
// Enchufe optional_extra_solid_mask(): bajo la huella de cada color de un sticker con depth_mode, las capas
// que caben en su profundidad pasan de relleno sparse a sólido (lo hace PrintObject con la misma cuenta del
// «Surface depth» del Pro-Painter). Sin material debajo no se crea nada: la intersección con stInternal
// ya recorta a lo que existe.
#include "Print.hpp"
#include "Model.hpp"
#include "ColorStitch.hpp"
#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace Slic3r {

// mm de la receta de un color: la suma de sus bloques, o el máximo del sticker si aún no tiene receta
// (así F4 se ve antes de que el panel calcule recetas). Siempre ≤ depth_max_mm.
static double colour_depth_mm(const ColorStitchSticker& st, const std::vector<ColorStitchDepthBlock>& blocks)
{
    double mm = 0.;
    for (const ColorStitchDepthBlock& b : blocks)
        mm += std::max(0.f, b.mm);
    if (blocks.empty())
        mm = st.depth_max_mm;
    return std::min<double>(mm, std::max(0.f, st.depth_max_mm));
}

std::vector<ExPolygons> optional_extra_solid_mask(const PrintObject& po)
{
    std::vector<ExPolygons> out;
    const ModelObject* mo = po.model_object();
    if (!mo || mo->colorstitch_stickers.empty() || po.layers().empty())
        return out;

    const Transform3d trafo = po.trafo_centered();   // s161: NUNCA trafo()
    const double z_tol = 0.02;                        // la misma holgura que enumerate_stickers_in_z_range
    const size_t n_layers = po.layers().size();

    for (const ColorStitchSticker& st : mo->colorstitch_stickers) {
        if (!st.depth_mode || st.hidden || st.svg_data.empty())
            continue;
        // 22-oct-2026 — también los que miran ABAJO: la cara vista es la de abajo y la receta crece hacia ARRIBA.
        const bool down = ColorStitch::sticker_faces_down(st);
        const double face_z = (trafo * st.transform * Vec3d(0., 0., 0.)).z();

        // Capa de la cara vista: la que contiene face_z.
        int top_idx = -1;
        for (size_t i = 0; i < n_layers; ++i) {
            const Layer* l = po.layers()[i];
            if (face_z >= l->bottom_z() - z_tol && face_z <= l->print_z + z_tol) { top_idx = int(i); break; }
        }
        if (top_idx < 0) {
            NEOTKO_LOG(PROFILE, "DEPTH_SOLID sticker='" << st.name << "' face_z=" << face_z << " sin capa");
            continue;
        }

        for (ColorStitch::StickerColorMask& cm : ColorStitch::sticker_color_masks_slice_frame(st, &po)) {
            if (cm.mask.empty())
                continue;
            const bool by_colour = cm.color_idx >= 0 && size_t(cm.color_idx) < st.colors.size();
            if (by_colour && st.colors[cm.color_idx].hidden)
                continue;
            const double depth = colour_depth_mm(st, by_colour ? st.colors[cm.color_idx].depth_blocks : st.depth_blocks);
            if (depth <= 0.)
                continue;
            int deepest = top_idx;
            if (!down) {
                const double z_floor = po.layers()[top_idx]->print_z - depth;
                // De la cara hacia abajo, mientras la capa esté entera dentro de la profundidad. La capa 0 nunca
                // (MinLayer Rule 2 del Sandwich).
                for (int i = top_idx; i >= 1; --i) {
                    if (po.layers()[i]->bottom_z() < z_floor - z_tol)
                        break;
                    if (out.empty())
                        out.resize(n_layers);
                    expolygons_append(out[i], cm.mask);
                    deepest = i;
                }
            } else {
                // Mira abajo: de la cara hacia ARRIBA. La cara misma ya es bottom sólido (y en la cama es la
                // capa 0, que no se toca aquí); desde la siguiente, mientras la capa quepa en la profundidad.
                const double z_ceil = po.layers()[top_idx]->bottom_z() + depth;
                for (int i = top_idx + 1; i < int(n_layers); ++i) {
                    if (po.layers()[i]->print_z > z_ceil + z_tol)
                        break;
                    if (out.empty())
                        out.resize(n_layers);
                    expolygons_append(out[i], cm.mask);
                    deepest = i;
                }
            }
            NEOTKO_LOG(PROFILE, "DEPTH_SOLID sticker='" << st.name << "' color=" << cm.color_idx
                << (down ? " ABAJO" : "") << " face_z=" << face_z << " pedida=" << depth
                << " capas=" << (std::abs(deepest - top_idx) + 1)
                << " hasta_z=" << (down ? po.layers()[deepest]->print_z : po.layers()[deepest]->bottom_z()));
        }
    }
    for (ExPolygons& ex : out)
        if (ex.size() > 1)
            ex = union_ex(ex);
    return out;
}

// ---------------------------------------------------------------------------
// F5 — qué filamento lleva cada capa.

// Capa que contiene la cara vista del sticker; nullptr si no hay.
static const Layer* face_layer(const PrintObject& po, const ColorStitchSticker& st)
{
    const double face_z = (po.trafo_centered() * st.transform * Vec3d(0., 0., 0.)).z();
    for (const Layer* l : po.layers())
        if (face_z >= l->bottom_z() - 0.02 && face_z <= l->print_z + 0.02)
            return l;
    return nullptr;
}

// Distancia (mm) de la cara vista al borde de ESTA capa que la mira: para los de arriba, de la cara (print_z de su
// capa) al techo de esta capa; para los de abajo, del suelo de su capa al suelo de esta. Así tool_at() sirve igual
// a los dos lados: el bloque que toca la cara es siempre el último de la receta. < 0 = esta capa está al otro lado.
static double offset_from_face(const Layer& face, bool down, double layer_print_z, double layer_height)
{
    return down ? (layer_print_z - layer_height) - face.bottom_z()
                : face.print_z - layer_print_z;
}

// Herramienta (0-based) de la receta a `offset_mm` de la cara (ver offset_from_face), o -1. La receta va de abajo a
// arriba y su bloque de ARRIBA (el último) toca la cara vista; se mira el CENTRO de la capa.
static int tool_at(const std::vector<ColorStitchDepthBlock>& blocks, double offset_mm, double layer_h, double max_mm)
{
    const double d = offset_mm + 0.5 * layer_h;
    if (d < 0. || d > max_mm + 1e-6)
        return -1;
    double acc = 0.;
    for (auto it = blocks.rbegin(); it != blocks.rend(); ++it) {
        acc += std::max(0.f, it->mm);
        if (d < acc + 1e-6)
            return it->tool;
    }
    return -1;
}

static const std::vector<ColorStitchDepthBlock>* blocks_of(const ColorStitchSticker& st, int color_idx)
{
    if (color_idx >= 0 && size_t(color_idx) < st.colors.size())
        return st.colors[color_idx].hidden ? nullptr : &st.colors[color_idx].depth_blocks;
    return &st.depth_blocks;
}

int optional_depth_tool_at(const PrintObject& po, const ColorStitchSticker& st, int color_idx,
                           double layer_print_z, double layer_height)
{
    if (!st.depth_mode || st.hidden)
        return -1;
    const std::vector<ColorStitchDepthBlock>* blocks = blocks_of(st, color_idx);
    if (!blocks || blocks->empty())
        return -1;
    const Layer* face = face_layer(po, st);
    if (!face)
        return -1;
    const double off = offset_from_face(*face, ColorStitch::sticker_faces_down(st), layer_print_z, layer_height);
    return tool_at(*blocks, off, layer_height, st.depth_max_mm);
}

// Corta las líneas de relleno SÓLIDO interno (erSolidInfill) de `coll`: lo que cae dentro de `mask` se mueve a
// `inside`, el resto se queda. Solo ExtrusionPath (y las de un ExtrusionMultiPath); lo demás no se toca.
static void split_solid(ExtrusionEntityCollection& coll, const Polygons& mask, const BoundingBox& mask_bb,
                        ExtrusionEntityCollection& inside)
{
    ExtrusionEntitiesPtr keep;
    keep.reserve(coll.entities.size());
    auto clip_path = [&](const ExtrusionPath& p, ExtrusionEntitiesPtr& out_keep) {
        Polylines in  = intersection_pl(Polylines{ p.polyline }, mask);
        Polylines out = diff_pl(Polylines{ p.polyline }, mask);
        for (Polyline& pl : in)
            if (pl.size() >= 2) { auto* np = new ExtrusionPath(p); np->polyline = std::move(pl); inside.entities.push_back(np); }
        for (Polyline& pl : out)
            if (pl.size() >= 2) { auto* np = new ExtrusionPath(p); np->polyline = std::move(pl); out_keep.push_back(np); }
    };
    for (ExtrusionEntity* e : coll.entities) {
        if (e->is_collection()) {
            split_solid(*static_cast<ExtrusionEntityCollection*>(e), mask, mask_bb, inside);
            keep.push_back(e);
            continue;
        }
        if (e->role() != erSolidInfill) { keep.push_back(e); continue; }
        if (auto* p = dynamic_cast<ExtrusionPath*>(e)) {
            if (!get_extents(p->polyline).overlap(mask_bb)) { keep.push_back(e); continue; }
            clip_path(*p, keep);
            delete e;
        } else if (auto* mp = dynamic_cast<ExtrusionMultiPath*>(e)) {
            BoundingBox bb;
            for (const ExtrusionPath& p : mp->paths) bb.merge(get_extents(p.polyline));
            if (!bb.overlap(mask_bb)) { keep.push_back(e); continue; }
            for (const ExtrusionPath& p : mp->paths) clip_path(p, keep);
            delete e;
        } else {
            keep.push_back(e);
        }
    }
    coll.entities = std::move(keep);
}

// ---------------------------------------------------------------------------
// 4-nov-2026 — MixedFilament Object con receta por profundidad. A diferencia del sticker (una cara a una Z), un
// objeto tiene tops y bottoms a muchas alturas: cada trozo de cada capa coge la herramienta de su top (o bottom)
// MÁS CERCANO que quepa en la profundidad. Se calcula una vez aquí (prepare_infill, antes del relleno paralelo)
// y el relleno solo lo lee.
std::vector<ExPolygons> mixed_depth_prepare(PrintObject& po)
{
    std::vector<ExPolygons> solid;
    auto& zones = po.mixed_depth_zones();
    zones.clear();
    const Print* print = po.print();
    const std::vector<ColorStitchDepthBlock>* blocks = print ? print->mixed_filament_depth_blocks(&po) : nullptr;
    // 6-nov-2026 — profundidad de lo PINTADO con cada filamento físico (ver PrintApply.cpp, MF_DEPTH_PAINTED).
    const std::map<unsigned int, std::vector<ColorStitchDepthBlock>>* painted = print ? print->mixed_filament_depth_painted(&po) : nullptr;
    if (!print || !po.config().mixed_filament_sandwich_mode.value || !po.config().mixed_filament_depth_mode.value
        || po.layers().empty())
        return solid;

    // 6-nov-2026 — tope duro: cada receta ya viene con su máx. (el del objeto o el de su MF en la lista).
    const double max_mm = 10.;
    auto depth_of = [&](const std::vector<ColorStitchDepthBlock>* bl) {
        double d = 0.;
        if (bl) for (const ColorStitchDepthBlock& b : *bl) d += std::max(0.f, b.mm);
        return std::min(d, max_mm);
    };
    double total = depth_of(blocks);
    if (painted)
        for (const auto& [e, pb] : *painted) total = std::max(total, depth_of(&pb));
    if (total <= 0.)
        return solid;
    static const std::vector<ColorStitchDepthBlock> k_empty;

    // 5-nov-2026 — modo «seguir la mezcla»: un único bloque cuyo tool es el id VIRTUAL del MixedFilament (1-based,
    // > nº de físicos). Toda la profundidad pasa a sólido y cada capa lleva la herramienta que el MF le da a esa capa
    // (la misma alternancia A/B que la pared). Bajo un top fino con relleno disperso y TD alto la mezcla no sale
    // (se ve aire y diagonales); con N mm de mezcla maciza sí.
    const size_t num_physical = print->config().filament_colour.values.size();
    const bool follow_mf = blocks && blocks->size() == 1 && (*blocks)[0].tool > int(num_physical);
    const unsigned int mf_id = follow_mf ? unsigned((*blocks)[0].tool) : 0u;
    const bool chamfer = po.config().mixed_filament_depth_chamfer.value;
    // Piel = ancho del perímetro exterior (se respeta siempre); band = todo el anillo de perímetros + margen, para
    // llevar la huella del top (que vive dentro de los perímetros) hasta el borde del objeto.
    coord_t skin = 0, band = 0;
    if (chamfer && !po.layers().empty() && !po.layers().front()->regions().empty()) {
        const LayerRegion* lr0 = po.layers().front()->regions().front();
        const coord_t ext_w = lr0->flow(frExternalPerimeter).scaled_width();
        const coord_t per_w = lr0->flow(frPerimeter).scaled_width();
        const int loops = std::max(1, lr0->region().config().wall_loops.value);
        skin = ext_w;
        band = ext_w + coord_t(loops) * per_w + per_w;
    }

    const size_t n = po.layers().size();
    const double z_tol = 0.02;
    // Caras vistas de cada capa: top (mira arriba) y bottom/bridge (mira abajo), separadas por QUIÉN las imprime:
    // grupo -1 = el MixedFilament del objeto; grupo e = lo pintado con el filamento e (id 1-based, físico u otro MF).
    // 6-nov-2026 — antes se juntaban todas y la receta del MF pisaba lo pintado.
    std::vector<std::map<int, ExPolygons>> tops(n), bottoms(n);
    for (size_t i = 0; i < n; ++i) {
        for (const LayerRegion* lr : po.layers()[i]->regions()) {
            const unsigned int ex = lr->region().extruder(frSolidInfill);   // 1-based
            const int grp = (painted && painted->count(ex)) ? int(ex) : -1;
            // Un trozo físico puede venir de un MF pintado (Orca lo partió): la huella del MF decide de cuál.
            const std::map<unsigned int, ExPolygons>* mfm =
                (ex >= 1 && ex <= num_physical && i < po.mixed_paint_masks().size()) ? &po.mixed_paint_masks()[i] : nullptr;
            for (const Surface& s : lr->fill_surfaces.surfaces) {
                const bool is_top = s.surface_type == stTop;
                if (!is_top && s.surface_type != stBottom && s.surface_type != stBottomBridge)
                    continue;
                auto& dst = is_top ? tops[i] : bottoms[i];
                ExPolygons rest{ s.expolygon };
                if (mfm)
                    for (const auto& [mid, mask] : *mfm) {
                        if (rest.empty()) break;
                        if (!painted || !painted->count(mid)) continue;
                        if (const MixedFilament* pm = print->mixed_filament_manager().mixed_filament_from_id(mid, num_physical);
                            !pm || (pm->component_a != ex && pm->component_b != ex))
                            continue;
                        ExPolygons in = intersection_ex(rest, mask);
                        if (in.empty()) continue;
                        rest = diff_ex(rest, mask);
                        expolygons_append(dst[int(mid)], std::move(in));
                    }
                expolygons_append(dst[grp], std::move(rest));
            }
        }
        for (auto* m : { &tops[i], &bottoms[i] })
            for (auto& [g, ex] : *m)
                if (ex.size() > 1) ex = union_ex(ex);
    }
    auto blocks_for = [&](int grp) -> const std::vector<ColorStitchDepthBlock>& {
        if (grp < 0) return blocks ? *blocks : k_empty;
        auto it = painted ? painted->find(unsigned(grp)) : decltype(painted->end()){};
        return (painted && it != painted->end()) ? it->second : k_empty;
    };

    zones.assign(n, {});
    solid.assign(n, {});
    size_t n_zones = 0;
    for (size_t li = 0; li < n; ++li) {
        const Layer& L = *po.layers()[li];
        // Candidatas, de la más cercana a la más lejana: cada trozo se queda con la primera que lo toca.
        struct Cand { double off; const ExPolygons* face; int grp; };
        std::vector<Cand> cands;
        for (size_t u = li; u < n; ++u) {
            const double off = po.layers()[u]->print_z - L.print_z;
            if (off > total + z_tol) break;
            for (const auto& [g, ex] : tops[u]) if (!ex.empty()) cands.push_back({ off, &ex, g });
        }
        for (int b = int(li); b >= 0; --b) {
            const double off = L.bottom_z() - po.layers()[b]->bottom_z();
            if (off > total + z_tol) break;
            for (const auto& [g, ex] : bottoms[b]) if (!ex.empty()) cands.push_back({ off, &ex, g });
        }
        if (cands.empty()) continue;
        // 6-nov-2026 — a igual distancia, lo PINTADO reclama antes que el MF: con Chamfer la huella del MF se agranda
        // hasta el borde del objeto y le mordía los bordes al trazo en la capa de arriba (la que se ve).
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
            if (std::abs(x.off - y.off) > 1e-6) return x.off < y.off;
            return (x.grp >= 0) > (y.grp >= 0);
        });

        int layer_tool = -1;
        if (follow_mf) {
            const unsigned int phys = print->mixed_filament_manager().resolve(mf_id, num_physical, int(li),
                float(L.print_z), float(L.height), false, &po);
            layer_tool = (phys >= 1 && phys <= num_physical) ? int(phys) - 1 : -1;
            if (layer_tool < 0) continue;
        }
        ExPolygons claimed;
        std::map<std::pair<int, bool>, ExPolygons> by_tool;   // (herramienta, pintado)
        for (const Cand& c : cands) {
            const bool is_painted = c.grp >= 0;
            const int t = (follow_mf && !is_painted) ? layer_tool : tool_at(blocks_for(c.grp), c.off, L.height, max_mm);
            if (t < 0) {
                // Lo pintado sin receta (o fuera de su profundidad) igual RESERVA su huella: el MF no se la quita.
                if (is_painted) {
                    ExPolygons keep = intersection_ex(*c.face, L.lslices);
                    if (!keep.empty()) { expolygons_append(claimed, std::move(keep)); claimed = union_ex(claimed); }
                }
                continue;
            }
            // Chaflán (opción): la huella va hasta la piel (borde del objeto menos el perímetro exterior, que manda el
            // MixedFilament) y encoge hacia dentro tanto como se baja (45°). Lo que caiga sobre perímetros interiores
            // se lo come mixed_depth_eat_perimeters(). Sin chaflán: la huella del top tal cual (solo relleno).
            ExPolygons area;
            if (chamfer && !is_painted) {
                const ExPolygons full = intersection_ex(offset_ex(*c.face, float(band)), L.lslices);
                area = intersection_ex(offset_ex(full, -float(scale_(c.off))), offset_ex(L.lslices, -float(skin)));
            } else
                area = intersection_ex(*c.face, L.lslices);
            if (!claimed.empty()) area = diff_ex(area, claimed);
            if (area.empty()) continue;
            expolygons_append(claimed, area);
            claimed = union_ex(claimed);
            expolygons_append(by_tool[{ t, is_painted }], std::move(area));
        }
        for (auto& [key, ex] : by_tool) {
            ExPolygons u = union_ex(ex);
            if (li > 0)   // la capa 0 nunca se reclasifica (MinLayer Rule 2 del Sandwich)
                expolygons_append(solid[li], u);
            zones[li].push_back({ key.first, std::move(u), key.second });
            ++n_zones;
        }
    }
    for (ExPolygons& ex : solid)
        if (ex.size() > 1) ex = union_ex(ex);
    NEOTKO_LOG(PROFILE, "MF_DEPTH_ZONES obj='" << (po.model_object() ? po.model_object()->name : std::string("?"))
        << "' total_mm=" << total << " layers=" << n << " zones=" << n_zones << " follow_mf=" << follow_mf << " chamfer=" << chamfer);
    return solid;
}

// 5-nov-2026 — Chamfer que se COME perímetros interiores. Tras mixed_depth_prepare (perímetros ya hechos, relleno
// aún no): en cada capa con zona, los perímetros NO exteriores (y el gap fill) que caen dentro de la zona se recortan,
// y ese hueco pasa a relleno sólido (stInternalSolid) que el Fill imprime con la herramienta de la zona. El perímetro
// exterior no se toca (lo manda el MixedFilament).
static void clip_inner_perimeters(ExtrusionEntityCollection& coll, const Polygons& mask, const BoundingBox& mask_bb, size_t& n_cut)
{
    ExtrusionEntitiesPtr keep;
    keep.reserve(coll.entities.size());
    auto cut_paths = [&](const ExtrusionPaths& paths) {
        for (const ExtrusionPath& p : paths) {
            if (is_external_perimeter(p.role())) {   // trozo exterior dentro de un multipath/loop: intacto
                keep.push_back(new ExtrusionPath(p));
                continue;
            }
            for (Polyline& pl : diff_pl(Polylines{ p.polyline }, mask))
                if (pl.size() >= 2) { auto* np = new ExtrusionPath(p); np->polyline = std::move(pl); keep.push_back(np); }
        }
    };
    for (ExtrusionEntity* e : coll.entities) {
        if (e->is_collection()) {
            clip_inner_perimeters(*static_cast<ExtrusionEntityCollection*>(e), mask, mask_bb, n_cut);
            keep.push_back(e);
            continue;
        }
        if (is_external_perimeter(e->role())) { keep.push_back(e); continue; }
        if (!get_extents(e->as_polyline()).overlap(mask_bb)) { keep.push_back(e); continue; }
        if (auto* lp = dynamic_cast<ExtrusionLoop*>(e)) {
            cut_paths(lp->paths);
        } else if (auto* mp = dynamic_cast<ExtrusionMultiPath*>(e)) {
            cut_paths(mp->paths);
        } else if (auto* p = dynamic_cast<ExtrusionPath*>(e)) {
            cut_paths(ExtrusionPaths{ *p });
        } else {
            keep.push_back(e);
            continue;
        }
        ++n_cut;
        delete e;
    }
    coll.entities = std::move(keep);
}

void mixed_depth_eat_perimeters(PrintObject& po)
{
    if (!po.config().mixed_filament_depth_mode.value || !po.config().mixed_filament_depth_chamfer.value)
        return;
    const auto& zones = po.mixed_depth_zones();
    size_t n_cut = 0, n_layers = 0;
    for (size_t li = 1; li < zones.size() && li < po.layers().size(); ++li) {   // capa 0 nunca (MinLayer Rule 2)
        if (zones[li].empty()) continue;
        ExPolygons all;
        for (const PrintObject::MixedDepthZone& z : zones[li]) if (!z.painted) expolygons_append(all, z.area);
        if (all.empty()) continue;
        all = union_ex(all);
        const Polygons mask = to_polygons(all);
        const BoundingBox mask_bb = get_extents(mask);
        bool any = false;
        const auto* painted_ex = po.print() ? po.print()->mixed_filament_depth_painted(&po) : nullptr;
        const size_t n_phys2 = po.print() ? po.print()->config().filament_colour.values.size() : 0;
        for (LayerRegion* lr : po.layers()[li]->regions()) {
            // 6-nov-2026 — solo regiones del MixedFilament: los perímetros de lo pintado no se tocan.
            const unsigned int pex = lr->region().extruder(frPerimeter);
            if ((pex >= 1 && pex <= n_phys2) || (painted_ex && painted_ex->count(pex)))
                continue;
            const size_t before = n_cut;
            clip_inner_perimeters(lr->perimeters, mask, mask_bb, n_cut);
            clip_inner_perimeters(lr->thin_fills, mask, mask_bb, n_cut);
            // El hueco que dejan pasa a sólido: la zona dentro de esta región menos lo que ya es relleno.
            ExPolygons fill_now;
            for (const Surface& s : lr->fill_surfaces.surfaces) fill_now.push_back(s.expolygon);
            const ExPolygons gap = diff_ex(intersection_ex(all, lr->slices.surfaces.empty() ? ExPolygons{} :
                                                           to_expolygons(lr->slices.surfaces)), fill_now);
            for (const ExPolygon& ex : gap)
                lr->fill_surfaces.surfaces.emplace_back(Surface(stInternalSolid, ex));
            expolygons_append(lr->fill_expolygons, gap);
            any |= (n_cut > before) || !gap.empty();
        }
        n_layers += any;
    }
    NEOTKO_LOG(PROFILE, "MF_DEPTH_EAT obj='" << (po.model_object() ? po.model_object()->name : std::string("?"))
        << "' layers=" << n_layers << " cut_entities=" << n_cut);
}

// Herramienta de la receta por profundidad para un trozo de esta capa: la zona que más lo cubre. -1 = ninguna.
int mixed_depth_tool_for(const PrintObject& po, size_t layer_idx, const ExPolygon& piece)
{
    const auto& zones = po.mixed_depth_zones();
    if (layer_idx >= zones.size())
        return -1;
    int best = -1;
    double best_area = 0.;
    for (const PrintObject::MixedDepthZone& z : zones[layer_idx]) {
        double a = 0.;
        for (const ExPolygon& e : intersection_ex(ExPolygons{ piece }, z.area))
            a += e.area();
        if (a > best_area) { best_area = a; best = z.tool; }
    }
    return best;
}

void optional_extra_sublayers(Layer& layer)
{
    PrintObject* po = layer.object();
    if (!po)
        return;
    const ModelObject* mo = po->model_object();
    const size_t li = layer.id();   // mismo índice que Fill.cpp (multipass_sublayers()[this->id()])
    const bool has_mf_depth = li < po->mixed_depth_zones().size() && !po->mixed_depth_zones()[li].empty();
    if (!mo || (mo->colorstitch_stickers.empty() && !has_mf_depth))
        return;
    if (li == 0 || li >= po->multipass_sublayers().size())
        return;

    // Máscara por herramienta en ESTA capa (colores con la misma herramienta a esta profundidad se juntan).
    std::map<int, ExPolygons> by_tool;
    // 4-nov-2026 — MixedFilament depth recipe: las zonas ya vienen resueltas por herramienta.
    if (has_mf_depth)
        for (const PrintObject::MixedDepthZone& z : po->mixed_depth_zones()[li])
            expolygons_append(by_tool[z.tool], z.area);
    for (const ColorStitchSticker& st : mo->colorstitch_stickers) {
        if (!st.depth_mode || st.hidden)
            continue;
        const Layer* face = face_layer(*po, st);
        if (!face)
            continue;
        const bool down = ColorStitch::sticker_faces_down(st);
        const double off = offset_from_face(*face, down, layer.print_z, layer.height);
        if (off < -1e-6)
            continue;
        for (ColorStitch::StickerColorMask& cm : ColorStitch::sticker_color_masks_slice_frame(st, po)) {
            const std::vector<ColorStitchDepthBlock>* blocks = blocks_of(st, cm.color_idx);
            if (!blocks || blocks->empty() || cm.mask.empty())
                continue;
            const int t = tool_at(*blocks, off, layer.height, st.depth_max_mm);
            if (t >= 0)
                expolygons_append(by_tool[t], std::move(cm.mask));
        }
    }
    if (by_tool.empty())
        return;

    std::vector<MultiPassSubLayer>& slot = po->multipass_sublayers()[li];
    int next_pass = 0;
    for (const MultiPassSubLayer& s : slot)
        next_pass = std::max(next_pass, s.pass_idx + 1);

    int k = 0;   // escalón de 1 µm entre herramientas de la misma capa: clave única, como los cubos de ColorStitch
    for (auto& [tool, mask_ex] : by_tool) {
        const Polygons mask = to_polygons(union_ex(mask_ex));
        const BoundingBox mask_bb = get_extents(mask);
        ExtrusionEntityCollection inside;
        for (LayerRegion* layerm : layer.regions())
            split_solid(layerm->fills, mask, mask_bb, inside);
        if (inside.entities.empty())
            continue;
        MultiPassSubLayer sub;
        sub.island_key = -1000 - tool;        // propia: no se funde con las del Sandwich
        sub.print_z    = layer.print_z - 2. * EPSILON - 0.001 * k++;   // la capa entera, justo bajo la Z real
        sub.height     = float(layer.height);
        sub.pass_idx   = next_pass++;
        sub.role       = erSolidInfill;
        sub.effect     = SurfacePassKind::Solid;
        sub.tool_id    = tool;
        const size_t n = inside.entities.size();
        sub.fills      = std::move(inside);
        slot.push_back(std::move(sub));
        NEOTKO_LOG(PROFILE, "DEPTH_FILL layer=" << layer.id() << " z=" << layer.print_z
            << " T" << tool << " paths=" << n);
    }
}

} // namespace Slic3r
