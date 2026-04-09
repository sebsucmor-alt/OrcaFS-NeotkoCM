---
name: Neoweaving pending features and known issues
description: Wave mode crash analysis + features still missing from FULLSPECTRUM095 port
type: project
---

## Wave mode crash (DISABLED)

**Status**: Wave mode returns `false` in `needs_weave()` — code intact but unreachable.

**Root cause**: Wave subdivides every extrusión line into `ceil(line_length / (period/8))` micro-segments. On a complex top surface (10k+ lines) this produces millions of `std::string::append` calls inside `apply_path()`. On an 8 GB Mac mini M2 where Orca already uses 5.5 GB, this exhausts physical RAM → OS page-in stall (apfs_vnop_pagein / buf_biowait) → freeze + crash.

**Fix path for Wave**: pre-reserve `gcode.reserve(estimated_bytes)` before the loop, or switch to streaming output (write directly to the GCode file rather than building a string in RAM).

**Location**: `SurfaceColorMix.cpp` — `NeoweaveEngine::needs_weave()`, gate at `NeoweaveMode::Wave`.

---

## Missing features (not yet ported)

### 1. Penultimate layer count selector
Allow user to choose how many penultimate layers receive Neoweaving/ColorMix treatment (currently always 1). Needs:
- New config key `neoweave_penultimate_layers` (int, default 1)
- Check in `needs_weave()`: role `erPenultimateInfill` AND layer is within N layers of top
- UI: `optgroup->append_single_option_line("neoweave_penultimate_layers")` in Tab.cpp Neoweaving group

### 2. Neoweaving speed override
Allow reducing print speed for neoweaved lines (useful for narrow lines with high Z motion). Needs:
- New config key `neoweave_speed_pct` (int %, default 100)
- Apply in `apply_path()`: `weave_F = F * (cfg.neoweave_speed_pct.value / 100.0)` (separate from the wave Z-speed cap)
- UI: `optgroup->append_single_option_line("neoweave_speed_pct")` in Tab.cpp

**Why:** the wave Z-speed cap (`weave_max_z_speed × period / 2π×amplitude`) only applies to Wave mode. Linear mode has no speed control, and narrow lines benefit from slower print speed for better Z precision.
