// NEOTKO_NEOSTROKE_TAG s337 — ver la cabecera.
#include "PreviewEffectiveConfig.hpp"

#include "../../Model.hpp"
#include "../../Print.hpp"
#include "../../PrintConfig.hpp"
#include "../../Slicing.hpp"
#include "../../Flow.hpp"
#include "../../NeoDebug.hpp"

#include <algorithm>

namespace Slic3r {

// 🚨 Declaración a mano: la función vive en `PrintObject.cpp` y ninguna cabecera la exporta (la única
//    declaración está suelta en `PrintApply.cpp`). Tiene que casar EXACTA con la firma real o no enlaza.
PrintRegionConfig region_config_from_model_volume(const PrintRegionConfig &default_or_parent_region_config,
                                                  const DynamicPrintConfig *layer_range_config,
                                                  const ModelVolume &volume, size_t num_extruders);

namespace NeoArachne { namespace Preview {

int ObjectLayerGrid::layer_at(double z_rel) const
{
    for (size_t i = 0; i < layers.size(); ++i)
        if (z_rel >= layers[i].first - 1e-9 && z_rel < layers[i].second - 1e-9)
            return int(i);
    return -1;
}

ObjectLayerGrid object_layer_grid(const DynamicPrintConfig& full, const ModelObject& mo)
{
    ObjectLayerGrid grid;
    const SlicingParameters sp = PrintObject::slicing_parameters(full, mo, float(mo.max_z()), Vec3d::Ones());
    if (!sp.valid)
        return grid;
    // El MISMO perfil que usará el laminado: el del objeto si lo tiene (Precision ALH, el pincel de
    // alturas), los rangos de altura si los tiene, o uno plano. `update_layer_height_profile` es la
    // función que decide eso para el laminado real.
    std::vector<coordf_t> profile;
    PrintObject::update_layer_height_profile(mo, sp, profile);
    const bool precise_z = full.has("precise_z_height") && full.opt_bool("precise_z_height");
    const std::vector<coordf_t> bounds = generate_object_layers(sp, profile, precise_z);
    grid.layers.reserve(bounds.size() / 2);
    for (size_t i = 1; i < bounds.size(); i += 2)
        grid.layers.emplace_back(double(bounds[i - 1]), double(bounds[i]));
    grid.print_z_offset = double(sp.object_print_z_min);
    return grid;
}

void compute_flows(ConfigSnapshot& snap)
{
    const int   extruder_idx = std::max(0, snap.region.wall_filament.value - 1);
    const float nozzle_d     = float(snap.print.nozzle_diameter.get_at(size_t(extruder_idx)));
    const float h            = float(snap.layer_height > 0.0 ? snap.layer_height : 0.2);

    const ConfigOptionFloatOrPercent& ext_w = snap.region.outer_wall_line_width.value > 0
        ? snap.region.outer_wall_line_width : snap.object.line_width;
    const ConfigOptionFloatOrPercent& inn_w = snap.region.inner_wall_line_width.value > 0
        ? snap.region.inner_wall_line_width : snap.object.line_width;
    const ConfigOptionFloatOrPercent& sol_w = snap.region.internal_solid_infill_line_width.value > 0
        ? snap.region.internal_solid_infill_line_width : snap.object.line_width;

    snap.ext_perimeter_flow         = Flow::new_from_config_width(frExternalPerimeter, ext_w, nozzle_d, h);
    snap.perimeter_flow             = Flow::new_from_config_width(frPerimeter,         inn_w, nozzle_d, h);
    snap.solid_infill_flow          = Flow::new_from_config_width(frSolidInfill,       sol_w, nozzle_d, h);
    snap.overhang_flow              = snap.perimeter_flow.with_flow_ratio(snap.region.bridge_flow);
    snap.smaller_ext_perimeter_flow = snap.ext_perimeter_flow;
}

ConfigSnapshot snapshot_for_volume(const DynamicPrintConfig& full, const ModelObject& mo, const ModelVolume& mv,
                                   const ObjectLayerGrid& grid, int layer_idx)
{
    FullPrintConfig fullc;
    fullc.apply(full, /*ignore_nonexistent=*/true);
    return snapshot_for_volume(fullc, mo, mv, grid, layer_idx);
}

ConfigSnapshot snapshot_for_volume(const FullPrintConfig& fullc, const ModelObject& mo, const ModelVolume& mv,
                                   const ObjectLayerGrid& grid, int layer_idx)
{
    ConfigSnapshot snap;

    const size_t n_ext = std::max<size_t>(1, fullc.filament_diameter.values.size());
    snap.print  = static_cast<const PrintConfig&>(fullc);
    // = `PrintObject::object_config_from_model_object` (es PRIVADA, por eso se copia aquí). Si cambia allí,
    //   cambia aquí: los ajustes del objeto normalizados encima de los globales y los filamentos de soporte
    //   fuera de rango al 1.
    snap.object = static_cast<const PrintObjectConfig&>(fullc);
    {
        DynamicPrintConfig src_normalized(mo.config.get());
        src_normalized.normalize_fdm();
        snap.object.apply(src_normalized, true);
        if (snap.object.support_filament.value > int(n_ext))           snap.object.support_filament.value = 1;
        if (snap.object.support_interface_filament.value > int(n_ext)) snap.object.support_interface_filament.value = 1;
    }

    // El rango de capas que cubre ESTA capa (por su plano medio), como hace `PrintApply` con sus
    // `LayerRanges`. Sin rango, nullptr, que es lo mismo que pasa en el laminado.
    const DynamicPrintConfig* range_cfg = nullptr;
    double lo = 0.0, hi = 0.0;
    if (layer_idx >= 0 && layer_idx < int(grid.layers.size())) {
        lo = grid.layers[size_t(layer_idx)].first;
        hi = grid.layers[size_t(layer_idx)].second;
        const double mid = 0.5 * (lo + hi);
        for (const auto& kv : mo.layer_config_ranges)
            if (mid >= kv.first.first && mid < kv.first.second) {
                range_cfg = &kv.second.get();
                break;
            }
    }
    // 🚨 El rango sólo vale para piezas; `region_config_from_model_volume` lo afirma con un assert.
    if (!mv.is_model_part())
        range_cfg = nullptr;
    snap.region = region_config_from_model_volume(static_cast<const PrintRegionConfig&>(fullc), range_cfg, mv, n_ext);

    // La altura REAL de esta capa (con altura variable no es `layer_height`), y su número de verdad: el
    // generador de perímetros decide cosas de la primera y de la última capa con él.
    if (hi > lo) {
        snap.layer_height       = hi - lo;
        snap.effective_layer_id = layer_idx;
        snap.is_bottom_layer    = layer_idx == 0;
        snap.is_top_layer       = layer_idx + 1 == int(grid.layers.size());
    } else {
        snap.layer_height = snap.object.layer_height.value > 0.0 ? snap.object.layer_height.value : 0.2;
    }
    snap.force_isolated_layer_defaults();
    compute_flows(snap);
    return snap;
}

namespace {
inline void mix(size_t& seed, size_t h) { seed ^= h + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2); }

template<class Cfg> void mix_config(size_t& seed, const Cfg& cfg)
{
    for (const std::string& k : cfg.keys())
        if (const ConfigOption* opt = cfg.option(k))
            mix(seed, opt->hash());
}
} // namespace

size_t hash_snapshot(const ConfigSnapshot& snap)
{
    size_t seed = 0;
    mix_config(seed, snap.region);
    mix_config(seed, snap.object);
    mix(seed, snap.print.nozzle_diameter.hash());
    mix(seed, snap.print.filament_diameter.hash());
    mix(seed, std::hash<double>()(snap.layer_height));
    mix(seed, std::hash<int>()(snap.effective_layer_id));
    mix(seed, neostroke_active(snap) ? 0x9E3779B1u : 0x85EBCA6Bu);
    return seed;
}

bool neostroke_active(const ConfigSnapshot& snap)
{
    return snap.object.wall_generator.value == PerimeterGeneratorType::NeoStroke
        && NeoDebug::enabled(NeoDebug::NEOSTROKE);
}

double neostroke_width_ref_mm(const ConfigSnapshot& snap)
{
    const double ref = snap.region.neostroke_width_ref.value > 1e-6 ? snap.region.neostroke_width_ref.value
                                                                   : double(snap.solid_infill_flow.width());
    return std::max(0.05, std::min(5.0, ref));
}

}} // namespace NeoArachne::Preview
} // namespace Slic3r
