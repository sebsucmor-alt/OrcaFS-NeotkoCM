/* ===========================================================================
 * iso.js — a small pseudo-3D toolkit for drawing printed plastic.
 *
 * Everything here draws in true isometric (30°), on a 2D canvas, with no
 * dependencies. It exists because a cross-section teaches you the mechanism
 * and a block teaches you the thing. The tour had plenty of the first and
 * none of the second.
 *
 * The projection:
 *   +X  goes right and down the screen
 *   +Y  goes left  and down the screen
 *   +Z  goes straight up
 *
 *   sx = (x - y) * cos30 * sxy
 *   sy = (x + y) * sin30 * sxy  -  z * sz
 *
 * `sxy` and `sz` are deliberately separate. A layer is 0.2 mm tall and a test
 * tile is 60 mm wide; at honest scale a Sandwich is invisible. So Z gets
 * exaggerated and every figure that does it says so on its own face. Lying
 * about the scale silently would be the one unforgivable thing here.
 *
 * Face visibility, derived once so nobody has to again:
 *   the corner at (x_max, y_max) is the nearest one, at the bottom of the
 *   footprint diamond. So the two visible vertical faces are x = x_max
 *   (drawn on the RIGHT of the screen) and y = y_max (drawn on the LEFT).
 * =========================================================================== */

(function (global) {
  'use strict';

  var COS30 = Math.cos(Math.PI / 6);   // 0.8660
  var SIN30 = 0.5;

  /* ------------------------------------------------------------- the view */

  /**
   * A view is a projection plus its two scales and a screen origin.
   * @param {{ox:number, oy:number, sxy:number, sz:number}} o
   */
  function view(o) {
    var sxy = o.sxy == null ? 6 : o.sxy;
    var sz = o.sz == null ? sxy : o.sz;
    var ox = o.ox || 0, oy = o.oy || 0;

    // Optional turntable. The projection stays isometric; the model spins
    // underneath it, about (cx, cy) in world millimetres. Four 90° steps get
    // you every corner, and anything in between reads as a nudge rather than
    // a different drawing, which is what you want in a teaching demo.
    var az = (o.azimuth || 0) * Math.PI / 180;
    var ca = Math.cos(az), sa = Math.sin(az);
    var cx = o.cx || 0, cy = o.cy || 0;
    var spun = !!o.azimuth;

    var V = {
      sxy: sxy, sz: sz, ox: ox, oy: oy, azimuth: o.azimuth || 0, cx: cx, cy: cy,
      /** world (mm) -> screen (px) */
      p: function (x, y, z) {
        if (spun) {
          var dx = x - cx, dy = y - cy;
          x = cx + dx * ca - dy * sa;
          y = cy + dx * sa + dy * ca;
        }
        return {
          x: ox + (x - y) * COS30 * sxy,
          y: oy + (x + y) * SIN30 * sxy - z * sz
        };
      },
      /** painter's-algorithm depth key: bigger = nearer the camera */
      depth: function (x, y, z) { return x + y + z * 0.001; },
      /** how tall `mm` of Z reads on screen, in px */
      zpx: function (mm) { return mm * sz; },
      /** the Z exaggeration factor, for the label every figure owes the reader */
      exaggeration: function () { return sz / sxy; }
    };
    return V;
  }

  /* ------------------------------------------------------------- shading */

  // Face brightness. Top is lit, the left face (y = max) is in the most shade.
  var FACE = { top: 1.0, right: 0.80, left: 0.62 };

  function clamp01(v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

  /**
   * Shade an sRGB hex by a multiplier, working in linear light so a dark
   * filament does not turn to mud and a light one does not blow out.
   * Uses Optics when it is present, and degrades to a naive multiply if not.
   */
  function shade(css, k) {
    // Accept both "#rrggbb" and "rgb(r,g,b)". The composite helpers hand back
    // CSS strings and it is far too easy to feed one of those in here; when
    // that used to fall through to parseInt it produced NaN, and NaN paints
    // black. A whole face went missing before anyone noticed.
    var hex = css;
    if (typeof css === 'string' && css.charAt(0) !== '#') {
      var m = css.match(/-?\d+(\.\d+)?/g);
      if (m && m.length >= 3) {
        hex = '#' + m.slice(0, 3).map(function (v) {
          var n = Math.max(0, Math.min(255, Math.round(parseFloat(v))));
          return n.toString(16).padStart(2, '0');
        }).join('');
      }
    }
    if (global.Optics) {
      var lin = global.Optics.hexToLinear(hex);
      return global.Optics.linearToCss([lin[0] * k, lin[1] * k, lin[2] * k]);
    }
    var q = parseInt(hex.replace('#', ''), 16);
    return 'rgb(' + Math.round(((q >> 16) & 255) * k) + ',' +
                    Math.round(((q >> 8) & 255) * k) + ',' +
                    Math.round((q & 255) * k) + ')';
  }

  /** Same, but takes a linear-RGB triple (what Optics.blendStacked returns). */
  function shadeLin(rgb, k) {
    return global.Optics
      ? global.Optics.linearToCss([rgb[0] * k, rgb[1] * k, rgb[2] * k])
      : 'rgb(0,0,0)';
  }

  /* ------------------------------------------------------------ polygons */

  function poly(ctx, V, pts, fill, stroke, lw) {
    ctx.beginPath();
    for (var i = 0; i < pts.length; i++) {
      var s = V.p(pts[i][0], pts[i][1], pts[i][2]);
      if (i === 0) ctx.moveTo(s.x, s.y); else ctx.lineTo(s.x, s.y);
    }
    ctx.closePath();
    if (fill) { ctx.fillStyle = fill; ctx.fill(); }
    if (stroke) {
      ctx.strokeStyle = stroke;
      ctx.lineWidth = lw == null ? 1 : lw;
      ctx.lineJoin = 'round';
      ctx.stroke();
    }
  }

  /* ----------------------------------------------------------- the boxes */

  /**
   * A solid box. The workhorse.
   *
   * @param opts {faces}  draw only these ('top','left','right'), default all
   *             {edge}   stroke colour for the silhouette
   *             {edgeW}  stroke width
   *             {alpha}  global alpha for the whole box
   *             {topFill} override the top face fill (a gradient, a pattern)
   */
  function box(ctx, V, x, y, z, w, d, h, hex, opts) {
    opts = opts || {};
    var faces = opts.faces || ['left', 'right', 'top'];
    var edge = opts.edge, edgeW = opts.edgeW == null ? 1 : opts.edgeW;
    var a = ctx.globalAlpha;
    if (opts.alpha != null) ctx.globalAlpha = a * opts.alpha;

    // left face: y = y+d
    if (faces.indexOf('left') >= 0 && h > 0) {
      poly(ctx, V, [
        [x, y + d, z], [x + w, y + d, z], [x + w, y + d, z + h], [x, y + d, z + h]
      ], opts.leftFill || shade(hex, FACE.left), edge, edgeW);
    }
    // right face: x = x+w
    if (faces.indexOf('right') >= 0 && h > 0) {
      poly(ctx, V, [
        [x + w, y, z], [x + w, y + d, z], [x + w, y + d, z + h], [x + w, y, z + h]
      ], opts.rightFill || shade(hex, FACE.right), edge, edgeW);
    }
    // top face: z = z+h
    if (faces.indexOf('top') >= 0) {
      poly(ctx, V, [
        [x, y, z + h], [x + w, y, z + h], [x + w, y + d, z + h], [x, y + d, z + h]
      ], opts.topFill || shade(hex, FACE.top), edge, edgeW);
    }
    ctx.globalAlpha = a;
  }

  /**
   * The top face of a box, drawn as individual extrusion beads instead of one
   * flat quad. This is the single thing that makes a drawing read as printed
   * plastic rather than as a coloured rectangle, so it is worth the cost.
   *
   * Beads run along +X. Each one is a strip of constant Y, `lw` wide, drawn
   * with a soft highlight down its spine so the corrugation shows.
   *
   * @param colourAt  function(i, n) -> css colour for bead i
   */
  function beadTop(ctx, V, x, y, z, w, d, lw, colourAt, opts) {
    opts = opts || {};
    var n = Math.max(1, Math.round(d / lw));
    var step = d / n;
    for (var i = 0; i < n; i++) {
      var y0 = y + i * step;
      var c = colourAt(i, n);
      poly(ctx, V, [
        [x, y0, z], [x + w, y0, z], [x + w, y0 + step, z], [x, y0 + step, z]
      ], c, null, 0);

      // The spine highlight and the shadow in the valley. Two hairlines in the
      // projected direction of +X, offset along +Y, sized in screen px.
      if (opts.relief !== false && step * V.sxy > 1.6) {
        var pA = V.p(x, y0 + step * 0.18, z);
        var pB = V.p(x + w, y0 + step * 0.18, z);
        ctx.beginPath(); ctx.moveTo(pA.x, pA.y); ctx.lineTo(pB.x, pB.y);
        ctx.strokeStyle = 'rgba(255,255,255,' + (opts.gloss == null ? 0.20 : opts.gloss) + ')';
        ctx.lineWidth = Math.max(0.6, step * V.sxy * 0.30);
        ctx.stroke();

        var qA = V.p(x, y0 + step * 0.94, z);
        var qB = V.p(x + w, y0 + step * 0.94, z);
        ctx.beginPath(); ctx.moveTo(qA.x, qA.y); ctx.lineTo(qB.x, qB.y);
        ctx.strokeStyle = 'rgba(0,0,0,0.16)';
        ctx.lineWidth = Math.max(0.5, step * V.sxy * 0.16);
        ctx.stroke();
      }
    }
  }

  /**
   * A slab whose beads each have their own height as well as their own colour.
   * This is PathBlend: one print-height step per fill line, each step a real
   * physical thing sitting on the layer below.
   *
   * @param heightAt  function(i, n) -> mm, this bead's own printed height
   * @param hexAt     function(i, n) -> "#rrggbb", the filament it is printed in
   * @param opts.topAt  optional function(i, n) -> css, to paint the top face
   *        with the composited colour instead of the raw filament. That
   *        distinction is the point of most of these figures: the sides show
   *        what went in, the top shows what comes out.
   */
  function beadRamp(ctx, V, x, y, z, w, d, lw, heightAt, hexAt, opts) {
    opts = opts || {};
    var n = Math.max(1, Math.round(d / lw));
    var step = d / n;
    // Far to near, so the near beads overlap the far ones correctly.
    for (var i = 0; i < n; i++) {
      var y0 = y + i * step;
      var hh = heightAt(i, n);
      box(ctx, V, x, y0, z, w, step, hh, hexAt(i, n), {
        faces: ['left', 'right', 'top'],
        topFill: opts.topAt ? opts.topAt(i, n) : null,
        edge: opts.edge, edgeW: opts.edgeW
      });
    }
  }

  /* ------------------------------------------------------------ annotation */

  function text(ctx, str, x, y, o) {
    o = o || {};
    ctx.save();
    ctx.font = (o.weight || 400) + ' ' + (o.size || 13) + 'px ' +
      (o.mono ? 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace'
              : 'ui-sans-serif, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif');
    ctx.fillStyle = o.color || '#15181d';
    ctx.textAlign = o.align || 'left';
    ctx.textBaseline = o.baseline || 'alphabetic';
    if (o.letterSpacing && ctx.letterSpacing !== undefined) ctx.letterSpacing = o.letterSpacing;
    ctx.fillText(str, x, y);
    ctx.restore();
  }

  function measure(ctx, str, o) {
    o = o || {};
    ctx.save();
    ctx.font = (o.weight || 400) + ' ' + (o.size || 13) + 'px ' +
      (o.mono ? 'ui-monospace, SFMono-Regular, Menlo, Consolas, monospace'
              : 'ui-sans-serif, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif');
    var m = ctx.measureText(str).width;
    ctx.restore();
    return m;
  }

  /** Text that wraps to `maxW`, returns the y it finished at. */
  function paragraph(ctx, str, x, y, maxW, o) {
    o = o || {};
    var lh = o.lineHeight || (o.size || 13) * 1.45;
    var words = str.split(' ');
    var line = '';
    for (var i = 0; i < words.length; i++) {
      var test = line ? line + ' ' + words[i] : words[i];
      if (measure(ctx, test, o) > maxW && line) {
        text(ctx, line, x, y, o); y += lh; line = words[i];
      } else line = test;
    }
    if (line) { text(ctx, line, x, y, o); y += lh; }
    return y;
  }

  /**
   * A leader line from a point in the world to a label in screen space.
   * The elbow is drawn at 45° because that is what reads as a callout rather
   * than as part of the drawing.
   */
  function leader(ctx, V, wx, wy, wz, dx, dy, label, o) {
    o = o || {};
    var a = V.p(wx, wy, wz);
    var bx = a.x + dx, by = a.y + dy;
    var side = dx >= 0 ? 1 : -1;
    var tx = bx + side * (o.tail == null ? 12 : o.tail);

    ctx.save();
    ctx.strokeStyle = o.color || '#858d94';
    ctx.lineWidth = o.width || 1;
    ctx.beginPath();
    ctx.moveTo(a.x, a.y);
    ctx.lineTo(bx, by);
    ctx.lineTo(tx, by);
    ctx.stroke();

    ctx.beginPath();
    ctx.arc(a.x, a.y, o.dot == null ? 2.4 : o.dot, 0, Math.PI * 2);
    ctx.fillStyle = o.color || '#858d94';
    ctx.fill();
    ctx.restore();

    if (label) {
      text(ctx, label, tx + side * 5, by, {
        size: o.size || 12,
        weight: o.weight || 500,
        color: o.labelColor || o.color || '#4a5158',
        align: side > 0 ? 'left' : 'right',
        baseline: 'middle',
        mono: o.mono
      });
    }
    return { x: tx, y: by, side: side };
  }

  /** A vertical dimension bracket in screen space, with a mm label. */
  function dimV(ctx, x, y0, y1, label, o) {
    o = o || {};
    var c = o.color || '#858d94';
    ctx.save();
    ctx.strokeStyle = c;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x, y0); ctx.lineTo(x, y1);
    ctx.moveTo(x - 4, y0); ctx.lineTo(x + 4, y0);
    ctx.moveTo(x - 4, y1); ctx.lineTo(x + 4, y1);
    ctx.stroke();
    ctx.restore();
    if (label) {
      text(ctx, label, x + (o.side === 'left' ? -8 : 8), (y0 + y1) / 2, {
        size: o.size || 11, weight: 500, color: o.labelColor || c, mono: true,
        align: o.side === 'left' ? 'right' : 'left', baseline: 'middle'
      });
    }
  }

  /** A rounded rectangle, for chips and panels. */
  function roundRect(ctx, x, y, w, h, r, fill, stroke, lw) {
    var rr = Math.min(r, w / 2, h / 2);
    ctx.beginPath();
    ctx.moveTo(x + rr, y);
    ctx.arcTo(x + w, y, x + w, y + h, rr);
    ctx.arcTo(x + w, y + h, x, y + h, rr);
    ctx.arcTo(x, y + h, x, y, rr);
    ctx.arcTo(x, y, x + w, y, rr);
    ctx.closePath();
    if (fill) { ctx.fillStyle = fill; ctx.fill(); }
    if (stroke) { ctx.strokeStyle = stroke; ctx.lineWidth = lw == null ? 1 : lw; ctx.stroke(); }
  }

  /** A filament chip: swatch, name, TD. Returns the width it used. */
  function chip(ctx, x, y, fil, o) {
    o = o || {};
    var h = o.h || 20;
    var sw = o.sw || 20;
    roundRect(ctx, x, y, sw, h, 4, fil.hex, 'rgba(0,0,0,.20)', 1);
    var label = fil.name + (o.td === false ? '' : '  TD ' + fil.td.toFixed(1));
    text(ctx, label, x + sw + 7, y + h / 2, {
      size: o.size || 11.5, weight: 500, color: o.color || '#4a5158',
      baseline: 'middle', mono: o.mono !== false
    });
    return sw + 7 + measure(ctx, label, { size: o.size || 11.5, weight: 500, mono: o.mono !== false });
  }

  /** The "Z exaggerated ×N" stamp every distorted figure owes the reader. */
  function zStamp(ctx, V, x, y, o) {
    o = o || {};
    var k = V.exaggeration();
    if (k < 1.4) return;
    var s = 'Z exaggerated ×' + (k >= 10 ? Math.round(k) : k.toFixed(1));
    var w = measure(ctx, s, { size: 10.5, mono: true }) + 14;
    roundRect(ctx, x, y, w, 18, 9, o.bg || 'rgba(21,24,29,.055)', null);
    text(ctx, s, x + 7, y + 9, { size: 10.5, color: o.color || '#858d94', baseline: 'middle', mono: true });
    return w;
  }

  /* --------------------------------------------------------------- swatch */

  /** A flat colour swatch with an optional label under it. */
  function swatch(ctx, x, y, w, h, css, label, o) {
    o = o || {};
    roundRect(ctx, x, y, w, h, o.r == null ? 4 : o.r, css, o.border || 'rgba(0,0,0,.14)', 1);
    if (label) {
      text(ctx, label, x + w / 2, y + h + 13, {
        size: o.size || 11, color: o.color || '#858d94',
        align: 'center', mono: o.mono !== false
      });
    }
  }

  /* ------------------------------------------------------- the palette */

  /**
   * The four spools this article was written around, which is also what the
   * U1 holds at once. TD values are stated as assumptions, not measurements:
   * matte PLA has no published TD and the fork has no measuring tool yet.
   * Every figure that depends on them says so.
   */
  var SPOOLS = [
    { name: 'Dark blue',   hex: '#1b2a63', td: 0.9 },
    { name: 'Butter',      hex: '#f4e3b0', td: 1.1 },
    { name: 'Magenta',     hex: '#cf2b7f', td: 1.0 },
    { name: 'Matte cyan',  hex: '#17a5c4', td: 0.8 }
  ];

  var INK = {
    paper: '#ffffff',
    paper2: '#f7f8f9',
    ink: '#15181d',
    soft: '#4a5158',
    faint: '#858d94',
    rule: '#dfe3e7',
    ruleFirm: '#c3cad0',
    accent: '#0d7d84',
    warn: '#b26a00',
    good: '#1f7a4d'
  };

  /* ------------------------------------------------------- figure runner */

  var registry = {};

  /**
   * Register a figure. `draw(ctx, w, h, F)` gets a context already scaled so
   * you work in CSS pixels, on a canvas backed at `SCALE`× for export.
   */
  function figure(id, def) { registry[id] = def; }

  var SCALE = 2;

  function renderInto(host, id) {
    var def = registry[id];
    if (!def) return null;
    var w = def.w || 1200, h = def.h || 640;

    var cv = document.createElement('canvas');
    cv.width = w * SCALE;
    cv.height = h * SCALE;
    cv.style.width = '100%';
    cv.style.height = 'auto';
    cv.style.display = 'block';
    cv.setAttribute('role', 'img');
    cv.setAttribute('aria-label', def.alt || def.title || id);

    var ctx = cv.getContext('2d');
    ctx.scale(SCALE, SCALE);
    ctx.fillStyle = def.bg || INK.paper;
    ctx.fillRect(0, 0, w, h);
    try {
      def.draw(ctx, w, h);
    } catch (e) {
      ctx.fillStyle = '#b00';
      text(ctx, 'figure "' + id + '" failed: ' + e.message, 20, 30, { size: 13 });
      if (global.console) console.error(id, e);
    }
    host.appendChild(cv);
    return cv;
  }

  /**
   * Start a named layer. On a real canvas this does nothing at all; on the SVG
   * recorder it opens a top-level <g id="...">, which is what Illustrator turns
   * into a layer. Draw order is untouched either way, so adding these to a
   * figure cannot change how it looks.
   *
   * Call it between drawing operations, never inside a save() or a clip().
   */
  function layer(ctx, name) {
    if (ctx && typeof ctx.__layer === 'function') ctx.__layer(name);
  }

  /**
   * Draw a figure into an SVG string instead of onto a canvas. Same figure
   * code, same measurements: the recorder measures text with a real canvas so
   * every position the figure computed still holds.
   *
   * @param mode 'sections' (default) keeps the figure's own layers
   *             'kind' sorts everything into Artwork and Text, which needs no
   *             cooperation from the figure
   */
  function toSvg(id, mode) {
    // The tour loads this same file and does not load the recorder, so say so
    // plainly rather than letting a ReferenceError surface from three frames in.
    if (typeof global.SvgCanvas !== 'function')
      throw new Error('SVG export needs assets/svgcanvas.js loaded before iso.js');
    var def = registry[id];
    if (!def) throw new Error('no figure registered as "' + id + '"');
    var w = def.w || 1200, h = def.h || 640;
    var rec = SvgCanvas(w, h, def.bg || INK.paper);
    def.draw(rec.ctx, w, h);
    return rec.toSVG(mode || 'sections');
  }

  function downloadSvg(id, name, mode) {
    var blob = new Blob([toSvg(id, mode)], { type: 'image/svg+xml' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.download = (name || id) + '.svg';
    a.href = url;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(function () { URL.revokeObjectURL(url); }, 4000);
  }

  function download(cv, name) {
    var a = document.createElement('a');
    a.download = name + '.png';
    a.href = cv.toDataURL('image/png');
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
  }

  function list() { return Object.keys(registry); }
  function get(id) { return registry[id]; }

  /* ---------------------------------------------------------------- misc */

  /** Linear interpolation on a hex pair, in linear light. Handy for gradients. */
  function mixHex(a, b, t) {
    if (!global.Optics) return a;
    var A = global.Optics.hexToLinear(a), B = global.Optics.hexToLinear(b);
    return global.Optics.linearToHex([
      A[0] + (B[0] - A[0]) * t,
      A[1] + (B[1] - A[1]) * t,
      A[2] + (B[2] - A[2]) * t
    ]);
  }

  /** A dashed hairline, for cut planes and construction lines. */
  function dashed(ctx, x0, y0, x1, y1, o) {
    o = o || {};
    ctx.save();
    ctx.setLineDash(o.dash || [4, 3]);
    ctx.strokeStyle = o.color || INK.faint;
    ctx.lineWidth = o.width || 1;
    ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke();
    ctx.restore();
  }

  /** An arrow in screen space. */
  function arrow(ctx, x0, y0, x1, y1, o) {
    o = o || {};
    var c = o.color || INK.faint;
    ctx.save();
    ctx.strokeStyle = c; ctx.fillStyle = c;
    ctx.lineWidth = o.width || 1.4;
    ctx.beginPath(); ctx.moveTo(x0, y0); ctx.lineTo(x1, y1); ctx.stroke();
    var ang = Math.atan2(y1 - y0, x1 - x0);
    var s = o.head || 6;
    ctx.beginPath();
    ctx.moveTo(x1, y1);
    ctx.lineTo(x1 - s * Math.cos(ang - 0.42), y1 - s * Math.sin(ang - 0.42));
    ctx.lineTo(x1 - s * Math.cos(ang + 0.42), y1 - s * Math.sin(ang + 0.42));
    ctx.closePath(); ctx.fill();
    ctx.restore();
  }

  /** A section-title rule: small caps label with a hairline running off it. */
  function ruleTitle(ctx, x, y, w, label, o) {
    o = o || {};
    text(ctx, label.toUpperCase(), x, y, {
      size: o.size || 10.5, weight: 600, color: o.color || INK.faint,
      mono: true, letterSpacing: '0.09em', baseline: 'middle'
    });
    var lw = measure(ctx, label.toUpperCase(), { size: o.size || 10.5, weight: 600, mono: true }) + 12;
    ctx.save();
    ctx.strokeStyle = o.rule || INK.rule;
    ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(x + lw, y); ctx.lineTo(x + w, y); ctx.stroke();
    ctx.restore();
  }

  /**
   * Repoint the ink palette at whatever the host page's CSS tokens say. The
   * blog kit is always on white so it never calls this; the tour has three
   * theme states and a canvas that ignores them looks broken in dark mode.
   *
   * @param {function(string):string} token  e.g. Tour.token
   */
  function syncInk(token) {
    if (typeof token !== 'function') return INK;
    var map = {
      paper: '--paper', paper2: '--paper-2', ink: '--ink', soft: '--ink-soft',
      faint: '--ink-faint', rule: '--rule', ruleFirm: '--rule-firm',
      accent: '--accent', warn: '--warn', good: '--good'
    };
    Object.keys(map).forEach(function (k) {
      var v = token(map[k]);
      if (v) INK[k] = v;
    });
    return INK;
  }

  /* -------------------------------------------------------------- export */

  global.Iso = {
    syncInk: syncInk,
    view: view, box: box, poly: poly, beadTop: beadTop, beadRamp: beadRamp,
    shade: shade, shadeLin: shadeLin, FACE: FACE,
    text: text, measure: measure, paragraph: paragraph,
    leader: leader, dimV: dimV, roundRect: roundRect, chip: chip,
    swatch: swatch, zStamp: zStamp, dashed: dashed, arrow: arrow,
    ruleTitle: ruleTitle, mixHex: mixHex,
    SPOOLS: SPOOLS, INK: INK,
    figure: figure, renderInto: renderInto, download: download,
    layer: layer, toSvg: toSvg, downloadSvg: downloadSvg,
    list: list, get: get, SCALE: SCALE, clamp01: clamp01
  };
})(window);
