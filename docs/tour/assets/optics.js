/* ===========================================================================
 * optics.js — the colour truth.
 *
 * This is a direct port of the fork's own colour math, not an approximation.
 * If a swatch on this website disagrees with the slicer's Result preview for
 * the same recipe, one of the two is wrong and it is worth finding out which.
 *
 * Sources:
 *   src/libslic3r/ColorSci/ColorSci.cpp        (srgb<->linear, blend_stacked)
 *   docs/REALCOLOR_OPTICS_AND_MATH.md  §3      (the physical model)
 *
 * The model, in three lines:
 *   TD_mm = neotko_td_N * nominal_layer_height     ratio units -> millimetres
 *   t(d)  = pow(0.1, d / TD_mm)                    decadic Beer-Lambert
 *   acc_k = c_k*(1 - t_k) + acc_{k-1}*t_k          bottom -> top, print order
 *
 * Everything composites in LINEAR RGB. sRGB is decoded once on the way in and
 * encoded once on the way out — never in between.
 * =========================================================================== */

(function (global) {
  'use strict';

  /* ---------------------------------------------------------------- colour */

  // ColorSci.cpp:14-26, verbatim.
  function srgbToLinear(c) {
    return c <= 0.04045 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4);
  }
  function linearToSrgb(c) {
    return c <= 0.0031308 ? c * 12.92 : 1.055 * Math.pow(c, 1 / 2.4) - 0.055;
  }

  function clamp01(v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

  /** "#rrggbb" -> [r,g,b] in linear space. */
  function hexToLinear(hex) {
    const h = hex.replace('#', '');
    const n = parseInt(h.length === 3 ? h.split('').map(c => c + c).join('') : h, 16);
    return [
      srgbToLinear(((n >> 16) & 255) / 255),
      srgbToLinear(((n >> 8) & 255) / 255),
      srgbToLinear((n & 255) / 255)
    ];
  }

  /** [r,g,b] linear -> "#rrggbb". */
  function linearToHex(rgb) {
    const b = rgb.map(v => Math.round(clamp01(linearToSrgb(clamp01(v))) * 255));
    return '#' + b.map(v => v.toString(16).padStart(2, '0')).join('');
  }

  /** [r,g,b] linear -> "rgb(...)" in sRGB, for canvas fills. */
  function linearToCss(rgb) {
    const b = rgb.map(v => Math.round(clamp01(linearToSrgb(clamp01(v))) * 255));
    return 'rgb(' + b[0] + ',' + b[1] + ',' + b[2] + ')';
  }

  /** Perceived lightness of a linear colour, for picking readable label ink. */
  function luminance(rgb) {
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
  }
  function inkOn(hexOrRgb) {
    const rgb = typeof hexOrRgb === 'string' ? hexToLinear(hexOrRgb) : hexOrRgb;
    return luminance(rgb) > 0.32 ? '#15181d' : '#ffffff';
  }

  /* ----------------------------------------------------------- the physics */

  /**
   * Transmittance of a slab of thickness `d` mm made of a filament whose TD,
   * expressed in millimetres, is `tdMm`.
   *
   * TD is defined as the path length at which one decade of attenuation
   * occurs, so t = 10^(-d/TD). Low TD = opaque, high TD = translucent.
   */
  function transmittance(d, tdMm) {
    if (!(tdMm > 0)) return 0;          // TD 0 -> perfectly opaque
    if (d <= 0) return 1;               // no material -> nothing absorbed
    return Math.pow(0.1, d / tdMm);
  }

  /**
   * TD as stored in the app (`neotko_td_1..4`, ratio units — a multiple of one
   * nominal layer height) converted to the millimetres this module works in.
   * This is the one unit conversion RealColor does at the source; see
   * REALCOLOR_OPTICS_AND_MATH.md §3.1 for why it exists.
   */
  function tdToMm(tdRatio, layerHeightMm) {
    return tdRatio * layerHeightMm;
  }

  /**
   * Composite a stack of passes, bottom to top — i.e. in print order.
   *
   *   ColorSci::blend_stacked(), ColorSci.cpp:146
   *   acc_0 = bg ; acc_k = c_k*(1 - t_k) + acc_{k-1}*t_k
   *
   * @param {Array<{colorHex:string, thickness:number, tdMm:number}>} passes
   *        bottom-most first.
   * @param {string} bgHex  what sits under the stack (the object's own fill).
   * @returns {number[]} the composited colour, linear RGB.
   */
  function blendStacked(passes, bgHex) {
    let acc = hexToLinear(bgHex || '#ffffff');
    for (let k = 0; k < passes.length; k++) {
      const p = passes[k];
      const c = hexToLinear(p.colorHex);
      const t = transmittance(p.thickness, p.tdMm);
      acc = [
        c[0] * (1 - t) + acc[0] * t,
        c[1] * (1 - t) + acc[1] * t,
        c[2] * (1 - t) + acc[2] * t
      ];
    }
    return acc;
  }

  /**
   * How much of the background still reaches the eye through the whole stack.
   * This is the `transmit=` readout in the Filament & TD panel.
   */
  function stackTransmittance(passes) {
    let t = 1;
    for (let k = 0; k < passes.length; k++) {
      t *= transmittance(passes[k].thickness, passes[k].tdMm);
    }
    return t;
  }

  /**
   * A ColorStitch pass is not one colour — it is N fill lines, each printed in
   * one of several tools. The eye integrates them. So: composite each line's
   * own stack, then average in linear space (which is what integrating
   * radiance actually means; averaging in sRGB would be wrong and visibly so).
   *
   * @param {number[]} toolSeq   per-line tool index, from stitch.js
   * @param {Array}    toolPasses  toolPasses[toolIndex] = passes for that line
   */
  function blendLineAverage(toolSeq, toolPasses, bgHex) {
    if (!toolSeq.length) return hexToLinear(bgHex || '#ffffff');
    const sum = [0, 0, 0];
    const cache = new Map();
    for (let i = 0; i < toolSeq.length; i++) {
      const tool = toolSeq[i];
      let c = cache.get(tool);
      if (!c) { c = blendStacked(toolPasses[tool] || [], bgHex); cache.set(tool, c); }
      sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2];
    }
    const n = toolSeq.length;
    return [sum[0] / n, sum[1] / n, sum[2] / n];
  }

  /* -------------------------------------------------------- CIE Lab / dE00 */

  // D65 white point.
  const XN = 0.95047, YN = 1.00000, ZN = 1.08883;

  function linearToXyz(rgb) {
    return [
      rgb[0] * 0.4124564 + rgb[1] * 0.3575761 + rgb[2] * 0.1804375,
      rgb[0] * 0.2126729 + rgb[1] * 0.7151522 + rgb[2] * 0.0721750,
      rgb[0] * 0.0193339 + rgb[1] * 0.1191920 + rgb[2] * 0.9503041
    ];
  }

  function labF(t) {
    return t > 216 / 24389 ? Math.cbrt(t) : (841 / 108) * t + 4 / 29;
  }

  /** linear RGB -> CIE L*a*b* (D65). */
  function linearToLab(rgb) {
    const xyz = linearToXyz(rgb);
    const fx = labF(xyz[0] / XN), fy = labF(xyz[1] / YN), fz = labF(xyz[2] / ZN);
    return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
  }

  /**
   * CIEDE2000. This is the metric the ColorStitch Studio's "Target + Match"
   * minimises, so a match found here is the match the app would find.
   * Sharma, Wu & Dalal (2005) formulation.
   */
  function deltaE2000(lab1, lab2) {
    const [L1, a1, b1] = lab1, [L2, a2, b2] = lab2;
    const kL = 1, kC = 1, kH = 1;

    const C1 = Math.hypot(a1, b1), C2 = Math.hypot(a2, b2);
    const Cbar = (C1 + C2) / 2;
    const Cbar7 = Math.pow(Cbar, 7);
    const G = 0.5 * (1 - Math.sqrt(Cbar7 / (Cbar7 + Math.pow(25, 7))));

    const a1p = (1 + G) * a1, a2p = (1 + G) * a2;
    const C1p = Math.hypot(a1p, b1), C2p = Math.hypot(a2p, b2);

    const h = (x, y) => {
      if (x === 0 && y === 0) return 0;
      const d = Math.atan2(y, x) * 180 / Math.PI;
      return d >= 0 ? d : d + 360;
    };
    const h1p = h(a1p, b1), h2p = h(a2p, b2);

    const dLp = L2 - L1;
    const dCp = C2p - C1p;

    let dhp;
    if (C1p * C2p === 0) dhp = 0;
    else if (Math.abs(h2p - h1p) <= 180) dhp = h2p - h1p;
    else if (h2p - h1p > 180) dhp = h2p - h1p - 360;
    else dhp = h2p - h1p + 360;
    const dHp = 2 * Math.sqrt(C1p * C2p) * Math.sin(dhp * Math.PI / 360);

    const Lbarp = (L1 + L2) / 2;
    const Cbarp = (C1p + C2p) / 2;

    let hbarp;
    if (C1p * C2p === 0) hbarp = h1p + h2p;
    else if (Math.abs(h1p - h2p) <= 180) hbarp = (h1p + h2p) / 2;
    else if (h1p + h2p < 360) hbarp = (h1p + h2p + 360) / 2;
    else hbarp = (h1p + h2p - 360) / 2;

    const rad = Math.PI / 180;
    const T = 1
      - 0.17 * Math.cos((hbarp - 30) * rad)
      + 0.24 * Math.cos((2 * hbarp) * rad)
      + 0.32 * Math.cos((3 * hbarp + 6) * rad)
      - 0.20 * Math.cos((4 * hbarp - 63) * rad);

    const dTheta = 30 * Math.exp(-Math.pow((hbarp - 275) / 25, 2));
    const Cbarp7 = Math.pow(Cbarp, 7);
    const RC = 2 * Math.sqrt(Cbarp7 / (Cbarp7 + Math.pow(25, 7)));
    const SL = 1 + (0.015 * Math.pow(Lbarp - 50, 2)) / Math.sqrt(20 + Math.pow(Lbarp - 50, 2));
    const SC = 1 + 0.045 * Cbarp;
    const SH = 1 + 0.015 * Cbarp * T;
    const RT = -Math.sin(2 * dTheta * rad) * RC;

    return Math.sqrt(
      Math.pow(dLp / (kL * SL), 2) +
      Math.pow(dCp / (kC * SC), 2) +
      Math.pow(dHp / (kH * SH), 2) +
      RT * (dCp / (kC * SC)) * (dHp / (kH * SH))
    );
  }

  /** Convenience: perceptual distance between two linear-RGB colours. */
  function colourDistance(rgbA, rgbB) {
    return deltaE2000(linearToLab(rgbA), linearToLab(rgbB));
  }

  /* ------------------------------------------------------------- filaments */

  /**
   * The default four slots every demo starts from. Colours are ordinary
   * filament colours; TD values are in ratio units (multiples of one nominal
   * layer height), the same units the `neotko_td_1..4` sliders use, and they
   * are spread on purpose so that "opaque" and "translucent" both show up.
   */
  const DEFAULT_FILAMENTS = [
    { name: 'T1', hex: '#f2f0eb', td: 0.9 },   // warm white, fairly opaque
    { name: 'T2', hex: '#1d3f8f', td: 1.4 },   // blue
    { name: 'T3', hex: '#d61f5c', td: 2.6 },   // magenta, lets a lot through
    { name: 'T4', hex: '#f2b705', td: 3.4 }    // yellow, translucent
  ];

  /**
   * TD bands as documented in WIKI §1e — used to label a slider position in
   * words rather than leaving the visitor to guess what 2.6 means.
   */
  function tdBand(td) {
    if (td <= 0.5) return 'highly opaque';
    if (td <= 3.0) return 'opaque–translucent';
    if (td <= 7.0) return 'translucent';
    return 'highly translucent';
  }

  /* ---------------------------------------------------------------- export */

  global.Optics = {
    srgbToLinear, linearToSrgb,
    hexToLinear, linearToHex, linearToCss, inkOn, luminance,
    transmittance, tdToMm, blendStacked, stackTransmittance, blendLineAverage,
    linearToLab, deltaE2000, colourDistance,
    DEFAULT_FILAMENTS, tdBand, clamp01
  };
})(window);
