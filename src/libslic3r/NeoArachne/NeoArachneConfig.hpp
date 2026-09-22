// NEOTKO_NEOARACHNE_TAG fase0+fase2
// Per-feature config POD for the NeoArachne hybrid wall generator.
// See memory/neoarachne_canonical_plan.md
#ifndef slic3r_NeoArachneConfig_hpp_
#define slic3r_NeoArachneConfig_hpp_

#include <cstdint>
#include "../PrintConfig.hpp"   // for NeoArachneWallSource (top-level Slic3r::)

namespace Slic3r { namespace NeoArachne {

// Alias the top-level enum so internal code reads as `WallSource`. The enum
// itself MUST live in top-level Slic3r namespace because CONFIG_OPTION_ENUM_
// DEFINE_STATIC_MAPS can't expand a nested-namespace name. See PrintConfig.hpp.
using WallSource = NeoArachneWallSource;

struct Config {
    bool       enabled = false;

    // ── Wall source selectors (Fase 2) ──────────────────────────────────────
    // Defaults revisados s91 (Neotko Hybrid v2): Classic gobierna outer; Arachne
    // gobierna todo el interior (inner walls + gap-fill integrados). Ver
    // memory/neoarachne_canonical_plan.md "Arquitectura por defecto — REVISADA s91".
    WallSource outer_wall  = WallSource::Classic;
    // NEOTKO_NEOARACHNE_TAG v3-spine (s323) — default Classic: muros de ancho fijo + espina.
    WallSource inner_walls = WallSource::Classic;
    WallSource gap_fill    = WallSource::Off;
    WallSource thin_walls  = WallSource::Classic;  // Fase 6

    // ── v3 espina (s323) — sólo con inner_walls = Classic ───────────────────
    bool   spine                 = true;
    double spine_min_width_pct   = 50.0;   // % del ancho de muro interior — SUELO
    double spine_max_width_pct   = 300.0;  // % del ancho de muro interior — TECHO
    double spine_min_length_mm   = 0.1;
    double spine_sliver_pct      = 35.0;   // % del ancho de muro interior: grietas por debajo, fuera (0 = off)

    // ── NeoStroke (s325) ────────────────────────────────────────────────────
    // Ganchos de esquina: las ramitas del esqueleto que mueren en un cruce. Ver NeoStroke.cpp.
    bool   neostroke_corner_hooks = false;
    double neostroke_min_width_pct = 12.0;    // % del cabezal — suelo
    double neostroke_max_width_pct = 200.0;   // % del cabezal — techo, decide k
    // s329 — cordón más fino que la máquina sabe hacer; suelo de los DETALLES (residuo y caminos
    // cortos), donde antes estaba el diámetro del cabezal a pelo. Una boquilla de 0.4 saca 0.25.
    double neostroke_detail_min_pct = 60.0;   // % de la referencia
    // s331c — referencia de TODOS los % de NeoStroke. 0 = automática (ancho de línea del relleno
    // macizo). Lo que se compara contra ella es la SEPARACIÓN del cordón, no su huella impresa.
    double neostroke_width_ref      = 0.0;    // mm
    // s331 — curva de overlap: 0 = apagada. % de ancho (y por tanto de material) que se le suma a
    // un cordón que es a la vez más ancho que el cabezal y está en curva. Ver NeoStroke.cpp.
    double neostroke_curve_overlap = 0.0;     // %
    // La FORMA de las dos rampas. Sólo hacen algo con el overlap encendido.
    double neostroke_overlap_width_end = 125.0;   // % del cabezal; la rampa empieza siempre en 100
    double neostroke_overlap_turn_min  = 4.0;     // grados/mm: por debajo, se considera recto
    double neostroke_overlap_turn_max  = 15.0;    // grados/mm: por encima, el overlap entero
    double neostroke_overlap_span      = 1.0;     // mm sobre los que se mide el giro
    double neostroke_overlap_straight  = 100.0;   // % de la rampa que se aplica en recto
    // s331d — costura de la vuelta en U: factor sobre el hueco CALCULADO de la tapa. 100 % = exacto.
    double neostroke_cap_join          = 100.0;   // %
    // s331b — tope duro del cordón (% del cabezal) y ancho máximo de un trazo (mm).
    double neostroke_max_bead_pct      = 150.0;
    double neostroke_max_stroke_width  = 5.0;
    // s332: el cordon minimo real del cabezal (% del cabezal)
    double neostroke_bead_min_pct = 70.0;
    // s332: que cada capa arranque por otro sitio, para que los cortes de flujo no se apilen en Z
    bool   neostroke_layer_jitter  = true;
    bool   neostroke_skate         = false;   // C6: patinar sobre lo ya puesto entre caminos
    double neostroke_skate_detour  = 5.0;     // largo máximo del patín / salto recto (como S3D)

    // ── Edge Closure params (Fase 3.0 — S3D heritage) ───────────────────────
    // Defaults reflect "PA-safe" starting point; user adjusts via UI.
    double allowed_overlap_pct   = 0.0;    // % of ext_perimeter_spacing (s93: raised from 50→0; the structural ~11% from spacing math is enough seam closure, more causes over-deposit)
    double min_bead_width_pct    = 30.0;   // % of nozzle_diameter
    // NEOTKO_NEOARACHNE_TAG max-bead-width — upper cap on variable bead width.
    // Sentinel 0 = use stock auto-derivation (WallToolPaths.cpp:511). Any value
    // >100 overrides wall_add_middle_threshold to (pct/100)-1, capping the
    // single-bead width at pct% of nominal nozzle width. Default 200 matches
    // physical ceiling for typical extrusion setups.
    double max_bead_width_pct    = 200.0;  // % of nozzle_diameter (100-200 range)
    double min_feature_size_pct  = 20.0;   // % of nozzle_diameter
    bool   keep_short_tails      = true;

    // ── NeotkoEdge math knobs (Fase 3 — NeotkoEdgeBeadingStrategy) ─────────
    // pin_outer_width: when true AND NeotkoEdge engaged, bead_count 1 and 2
    // force outer width to optimal_width_outer exactly. Upstream Arachne uses
    // thickness/bead_count for those cases → outer width "breathes" along
    // borderline strokes. true = S3D-style constant outer width.
    bool   pin_outer_width                = true;
    // Spatial hysteresis (% of optimal_width_outer) applied to bead-count
    // transitions. 0 = no hysteresis (upstream Arachne behaviour). Higher =
    // bigger deadband, fewer transitions across borderline thickness.
    double bead_count_hysteresis_pct      = 20.0;
    // ── Fase 4 — SkeletalTrapezoidation transition smoothing ────────────────
    double transition_filter_dist_mm      = 100.0;  // default 100 = upstream Arachne behavior
    // ── Future math knobs (placeholder — Fase 5) ────────────────────────────
    bool   gap_only_skeletal_mode         = true;   // Fase 5

    // ── Debug channels (gated; cheap when off) ──────────────────────────────
    bool emit_svg_per_layer  = false;
    bool emit_gcode_comments = true;
    int  svg_layer_from = -1;
    int  svg_layer_to   = -1;
};

}} // namespace Slic3r::NeoArachne

#endif
