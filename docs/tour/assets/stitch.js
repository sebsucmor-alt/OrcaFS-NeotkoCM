/* ===========================================================================
 * stitch.js — the pattern truth.
 *
 * A ColorStitch pass does one thing: for each fill line of a surface, it names
 * the tool that prints it. Everything else — dither, stripes, weaves — is a
 * different way of producing that one array of integers.
 *
 * This is a port of the engine's own sequence builders, so the strip drawn on
 * this website is the exact sequence the slicer emits for the same inputs.
 *
 * Source: src/libslic3r/SurfaceColorMix.cpp
 *   colormix_easing_apply       :2389
 *   build_dithered_tools_2color :2416
 *   build_dithered_tools_3color :2466
 *   build_custom_bands          :2547
 * =========================================================================== */

(function (global) {
  'use strict';

  /* --------------------------------------------------------------- easings */

  const EASING = {
    LINEAR: 0,      // even — same density everywhere
    EASE_IN: 1,     // slow start (the default when you first pick a blend)
    EASE_OUT: 2,    // slow end
    EASE_IN_OUT: 3, // S-curve
    GAMMA: 4,       // custom shape
    HARD_BAND: 5    // hard step at the midpoint
  };

  const EASING_LABELS = [
    'Even', 'Slow start', 'Slow end', 'S-curve', 'Custom (γ)', 'Hard step'
  ];

  /** SurfaceColorMix::colormix_easing_apply — reshapes t before the dither. */
  function easingApply(t, easing, gamma) {
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    switch (easing) {
      case EASING.EASE_IN:     return t * t;
      case EASING.EASE_OUT:    return 1 - (1 - t) * (1 - t);
      case EASING.EASE_IN_OUT: return t * t * (3 - 2 * t);           // smoothstep
      case EASING.GAMMA: {
        const g = Math.min(10, Math.max(0.1, gamma == null ? 1 : gamma));
        return Math.pow(t, g);
      }
      case EASING.HARD_BAND:   return t < 0.5 ? 0 : 1;
      default:                 return t;
    }
  }

  /* ---------------------------------------------------------------- dither */

  /**
   * Two-colour Bresenham dither.
   *
   * The whole idea in one sentence: we want exactly round(n * pct_b/100)
   * instances of B, distributed so that in any window of length W the fraction
   * of B is within 1/W of the target. A single integer accumulator does that —
   * no randomness, no quality knob, identical every slice.
   *
   * Easing reshapes the *target curve*, not the distribution quality: the
   * cumulative count of B at position i follows easing(t) instead of t, so the
   * emissions cluster where the curve rises fastest.
   */
  function dither2(nLines, toolA, toolB, pctA, easing, gamma) {
    const out = [];
    if (nLines <= 0) return out;

    const pA = Math.min(100, Math.max(0, pctA));
    const pB = 100 - pA;
    if (pB === 0) { for (let i = 0; i < nLines; i++) out.push(toolA); return out; }
    if (pA === 0) { for (let i = 0; i < nLines; i++) out.push(toolB); return out; }

    const totalB = Math.round(nLines * pB / 100);
    const denom = Math.max(1, nLines - 1);
    let emittedB = 0;

    for (let i = 0; i < nLines; i++) {
      const tLin = nLines === 1 ? 0.5 : i / denom;
      const tEff = easingApply(tLin, easing, gamma);
      const needB = Math.round(tEff * totalB);
      if (emittedB < needB && emittedB < totalB) { out.push(toolB); emittedB++; }
      else out.push(toolA);
    }
    return out;
  }

  /**
   * Three-colour dither: A dominates at t=0, B peaks in the middle, C at t=1.
   *
   * Three Bresenham trackers race each other; at every line the tool with the
   * largest deficit against its own expected share wins. The expected share
   * comes from a hat function centred on each tool's anchor (A@0, B@0.5, C@1)
   * whose half-width is set by `overlap`:
   *
   *   overlap 0 -> halfwidth 0.5 -> triangular weights -> three hard bands
   *   overlap 1 -> halfwidth 1.0 -> every colour appears everywhere, with
   *                                 locally varying odds
   *
   * The global ratios stay exactly (pA, pB, pC) whatever the overlap — only
   * the local distribution changes shape. Exhausted tools sink to -1e9 so the
   * old "extra A band at the far end" glitch cannot come back.
   */
  function dither3(nLines, toolA, toolB, toolC, pctA, pctB, easing, gamma, overlap) {
    const out = [];
    if (nLines <= 0) return out;

    const pA = Math.min(100, Math.max(0, pctA));
    const pB = Math.min(100 - pA, Math.max(0, pctB));

    const targetA = Math.round(nLines * pA / 100);
    const targetB = Math.round(nLines * pB / 100);
    const targetC = nLines - targetA - targetB;

    const halfwidth = 0.5 + Math.min(1, Math.max(0, overlap == null ? 0 : overlap)) * 0.5;
    const denom = Math.max(1, nLines - 1);

    const cumA = new Float64Array(nLines);
    const cumB = new Float64Array(nLines);
    const cumC = new Float64Array(nLines);
    let sA = 0, sB = 0, sC = 0;

    for (let i = 0; i < nLines; i++) {
      const tLin = nLines === 1 ? 0.5 : i / denom;
      const tEff = easingApply(tLin, easing, gamma);
      const hat = a => Math.max(0, halfwidth - Math.abs(tEff - a)) / halfwidth;
      sA += hat(0.0); sB += hat(0.5); sC += hat(1.0);
      cumA[i] = sA; cumB[i] = sB; cumC[i] = sC;
    }
    const areaA = Math.max(1e-9, sA), areaB = Math.max(1e-9, sB), areaC = Math.max(1e-9, sC);

    let eA = 0, eB = 0, eC = 0;
    for (let i = 0; i < nLines; i++) {
      const expA = (cumA[i] / areaA) * targetA;
      const expB = (cumB[i] / areaB) * targetB;
      const expC = (cumC[i] / areaC) * targetC;
      const defA = eA < targetA ? expA - eA : -1e9;
      const defB = eB < targetB ? expB - eB : -1e9;
      const defC = eC < targetC ? expC - eC : -1e9;

      if (defA >= defB && defA >= defC)  { out.push(toolA); eA++; }
      else if (defB >= defC)             { out.push(toolB); eB++; }
      else                               { out.push(toolC); eC++; }
    }
    return out;
  }

  /* ----------------------------------------------------------------- bands */

  /**
   * Manual stripes. `slots` is [{tool, count}, ...]; a slot with count 0 or a
   * negative tool is skipped entirely (that skip is what stops a misconfigured
   * slot silently falling back to T0).
   */
  function bands(nLines, slots) {
    const out = [];
    if (nLines <= 0) return out;

    const active = (slots || []).filter(s => s.count > 0 && s.tool >= 0);
    if (!active.length) {
      const t = slots && slots.length && slots[0].tool >= 0 ? slots[0].tool : 0;
      for (let i = 0; i < nLines; i++) out.push(t);
      return out;
    }

    let produced = 0;
    while (produced < nLines) {
      for (let b = 0; b < active.length && produced < nLines; b++) {
        for (let j = 0; j < active[b].count && produced < nLines; j++, produced++) {
          out.push(active[b].tool);
        }
      }
    }
    return out;
  }

  /* ---------------------------------------------------- custom digit string */

  /**
   * A custom pattern is a digit string looped across the lines: line 1 takes
   * the first digit, line 2 the second, and it wraps. Digits are 1-based in
   * the UI ("1" = the first filament), tool indices here are 0-based.
   */
  function fromPattern(nLines, patternStr) {
    const digits = String(patternStr || '')
      .split('')
      .filter(c => c >= '1' && c <= '9')
      .map(c => c.charCodeAt(0) - 49);
    const out = [];
    if (nLines <= 0) return out;
    if (!digits.length) { for (let i = 0; i < nLines; i++) out.push(0); return out; }
    for (let i = 0; i < nLines; i++) out.push(digits[i % digits.length]);
    return out;
  }

  /** Rewrite a two-colour pattern string onto a chosen pair of tools. */
  function remapPattern(patternStr, toolA, toolB) {
    return String(patternStr || '')
      .split('')
      .map(c => c === '1' ? String(toolA + 1) : c === '2' ? String(toolB + 1) : c)
      .join('');
  }

  /* ----------------------------------------------------------------- weaves */

  /**
   * The five textile structures, as the digit strings the dialog generates.
   * `note` is the caveat the dialog itself shows, kept verbatim in spirit —
   * the diagonal offset that a real twill needs is not implemented, so twills
   * currently print as a static repeat.
   */
  const WEAVES = [
    {
      id: 'plain', name: 'Plain (tafetán)', pattern: '1212',
      look: 'Balanced 50/50 mix — neither colour dominates.'
    },
    {
      id: 'twill22', name: 'Twill 2/2 (sarga)', pattern: '1122',
      look: 'Diagonal weave.',
      note: 'The per-layer diagonal offset is not implemented yet, so today this prints as a static repeat.'
    },
    {
      id: 'twill31', name: 'Twill 3/1', pattern: '1112',
      look: 'One colour dominates, the other marks a diagonal.',
      note: 'Same static-repeat caveat as Twill 2/2.'
    },
    {
      id: 'satin5', name: 'Satin 5 (satén)', pattern: '11112',
      look: 'One colour covers ~80%; the other lands as scattered accent points.'
    },
    {
      id: 'houndstooth', name: 'Houndstooth', pattern: '1122', counterpart: '2211',
      look: 'The classic pattern only appears where Top and Penultimate cross at 90°.',
      note: 'The dialog writes the correct half for the surface you are editing and shows you the string for the other one.'
    }
  ];

  /* --------------------------------------------------------- the front door */

  /**
   * One entry point for every pattern style, mirroring the ADV dialog's
   * mutually-exclusive "Pattern style" selector.
   *
   * @param {number} nLines
   * @param {object} cfg
   *   style      'blend2' | 'blend3' | 'stripes' | 'custom' | 'weave'
   *   tools      [a, b, c, d] tool indices for Colour 1..4
   *   pctA,pctB  percentages for the blend styles
   *   easing     EASING.*
   *   gamma      for EASING.GAMMA
   *   overlap    0..1, blend3 only
   *   counts     [n1, n2, n3, n4] for stripes
   *   pattern    digit string for custom
   *   weave      weave id
   *   invert     reverse the resulting sequence
   *   repeat     compress the pattern so it repeats N times across the surface
   */
  function build(nLines, cfg) {
    const c = cfg || {};
    const tools = c.tools || [0, 1, 2, 3];
    const reps = Math.max(1, c.repeat || 1);

    // "Gradient repetitions": build one period, then tile it. A blend repeated
    // three times is three complete gradients across the surface, not one
    // gradient sampled three times.
    const period = reps > 1 ? Math.max(1, Math.round(nLines / reps)) : nLines;

    let seq;
    switch (c.style) {
      case 'blend3':
        seq = dither3(period, tools[0], tools[1], tools[2],
                      c.pctA == null ? 34 : c.pctA,
                      c.pctB == null ? 33 : c.pctB,
                      c.easing || 0, c.gamma, c.overlap);
        break;
      case 'stripes':
        seq = bands(period, (c.counts || [10, 10, 0, 0]).map((n, i) => ({ tool: tools[i], count: n })));
        break;
      case 'custom':
        seq = fromPattern(period, c.pattern);
        break;
      case 'weave': {
        const w = WEAVES.find(x => x.id === c.weave) || WEAVES[0];
        seq = fromPattern(period, remapPattern(w.pattern, tools[0], tools[1]));
        break;
      }
      case 'blend2':
      default:
        seq = dither2(period, tools[0], tools[1],
                      c.pctA == null ? 50 : c.pctA,
                      c.easing || 0, c.gamma);
        break;
    }

    if (reps > 1) {
      const tiled = [];
      while (tiled.length < nLines) {
        for (let i = 0; i < seq.length && tiled.length < nLines; i++) tiled.push(seq[i]);
      }
      seq = tiled;
    }
    if (seq.length > nLines) seq = seq.slice(0, nLines);
    while (seq.length < nLines) seq.push(seq[seq.length - 1] != null ? seq[seq.length - 1] : tools[0]);

    if (c.invert) seq.reverse();
    return seq;
  }

  /**
   * How many fill lines a surface of `sizeMm` square would have at a given
   * line width. The dialog shows this next to the strip so a percentage means
   * something physical: 3% of 300 lines is nine lines, 3% of 12 lines is none.
   */
  function estimateLines(sizeMm, lineWidthMm, overlap) {
    const spacing = lineWidthMm * (1 - (overlap == null ? 0.15 : overlap));
    return Math.max(1, Math.round(sizeMm / spacing));
  }

  /** Run-length encode a sequence — handy for describing a strip in words. */
  function runs(seq) {
    const out = [];
    for (let i = 0; i < seq.length; i++) {
      if (out.length && out[out.length - 1].tool === seq[i]) out[out.length - 1].count++;
      else out.push({ tool: seq[i], count: 1 });
    }
    return out;
  }

  global.Stitch = {
    EASING, EASING_LABELS, easingApply,
    dither2, dither3, bands, fromPattern, remapPattern,
    WEAVES, build, estimateLines, runs
  };
})(window);
