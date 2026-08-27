/* ===========================================================================
 * svgcanvas.js — record the figures as editable SVG instead of pixels.
 *
 * The figures are written against a 2D canvas context and that is not going to
 * change: canvas is what makes them quick to write and quick to look at. So
 * this is a stand-in context that speaks the same subset of the canvas API and,
 * instead of painting, builds an element tree that serialises to SVG.
 *
 * The subset is closed, and it is the whole of what the figure files use:
 *
 *   state   save restore globalAlpha fillStyle strokeStyle lineWidth lineJoin
 *           setLineDash font textAlign textBaseline letterSpacing scale
 *   path    beginPath moveTo lineTo arc arcTo rect closePath
 *   paint   fill stroke fillRect strokeRect clip
 *   text    fillText measureText
 *
 * If a figure ever reaches for something outside that list it throws here
 * rather than quietly dropping the drawing, because a silently incomplete
 * export is worse than a failed one.
 *
 * ---------------------------------------------------------------------------
 * LAYERS
 *
 * Illustrator turns each top-level <g id="..."> into a layer, which is the
 * whole point of this file. Two ways to get them:
 *
 *   Iso.layer('Section')  in a figure, to start a named layer. Draw order is
 *                         preserved exactly, so a figure that opts in looks
 *                         identical and just arrives sorted.
 *
 *   layers: 'kind'        at export time, which needs no changes to any
 *                         figure: everything lands in "Artwork" and "Text",
 *                         with the order inside each preserved.
 *
 * 'kind' does move text relative to artwork. In this kit text is drawn on top
 * of the artwork it labels everywhere, so it comes out the same, and the sheet
 * offers both so you can check rather than take my word for it.
 * =========================================================================== */

(function (global) {
  'use strict';

  /* Real metrics. SVG cannot tell us how wide a string is, and the figures lay
     themselves out from measureText, so an offscreen canvas does the measuring
     and the SVG inherits the positions that came out of it. */
  var metricsCtx = null;
  function metrics() {
    if (!metricsCtx) metricsCtx = document.createElement('canvas').getContext('2d');
    return metricsCtx;
  }

  function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
                    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }
  function n(v) {
    // Three decimals is well under a printer's resolution and keeps the file
    // small enough to open without Illustrator thinking about it.
    var r = Math.round(v * 1000) / 1000;
    return Object.is(r, -0) ? 0 : r;
  }

  /* ------------------------------------------------------------ the tree -- */

  function Group(id, cls) { this.tag = 'g'; this.id = id; this.cls = cls; this.kids = []; this.attrs = {}; }
  function Leaf(tag, attrs, body, kind) {
    this.tag = tag; this.attrs = attrs; this.body = body; this.kind = kind || 'artwork';
  }

  /**
   * ids have to be unique across the document. In 'kind' mode a section is
   * mirrored into both halves, so the same name legitimately turns up twice,
   * and Illustrator will not open a file with duplicates. Names stay readable
   * and a repeat gets a suffix.
   */
  function uniquifier() {
    var seen = {};
    return function (id) {
      if (!id) return null;
      if (!seen[id]) { seen[id] = 1; return id; }
      return id + ' (' + (++seen[id]) + ')';
    };
  }

  function serialise(node, out, indent, uniq) {
    var pad = new Array(indent + 1).join('  ');
    if (node.tag === 'g') {
      var a = '';
      var gid = uniq(node.id);
      if (gid) a += ' id="' + esc(gid) + '"';
      if (node.cls) a += ' class="' + esc(node.cls) + '"';
      Object.keys(node.attrs).forEach(function (k) { a += ' ' + k + '="' + esc(node.attrs[k]) + '"'; });
      if (!node.kids.length) return;
      out.push(pad + '<g' + a + '>');
      node.kids.forEach(function (k) { serialise(k, out, indent + 1, uniq); });
      out.push(pad + '</g>');
      return;
    }
    var s = pad + '<' + node.tag;
    Object.keys(node.attrs).forEach(function (k) {
      if (node.attrs[k] != null) s += ' ' + k + '="' + esc(node.attrs[k]) + '"';
    });
    if (node.body != null) s += '>' + esc(node.body) + '</' + node.tag + '>';
    else s += '/>';
    out.push(s);
  }

  /* ----------------------------------------------------------- the shim -- */

  /**
   * @param {number} w  width in CSS px, the same units the figures draw in
   * @param {number} h  height
   * @param {string} bg background fill, or null for a transparent page
   */
  function SvgCanvas(w, h, bg) {
    var root = new Group(null, null);
    var layers = {};            // name -> Group, so a figure can come back to one
    var layerOrder = [];
    var clipSeq = 0;
    var defs = [];

    var current = null;         // the group new elements land in
    var stack = [];             // [{state, node}] for save/restore

    var st = {
      fillStyle: '#000', strokeStyle: '#000', lineWidth: 1, lineJoin: 'miter',
      globalAlpha: 1, dash: null, font: '13px sans-serif',
      textAlign: 'start', textBaseline: 'alphabetic', letterSpacing: '',
      sx: 1, sy: 1
    };

    var path = [];              // accumulated SVG path commands
    var cursor = null;          // current point, for arcTo
    var start = null;           // subpath start, for closePath

    function layer(name) {
      if (!layers[name]) {
        layers[name] = new Group(name, 'layer');
        layerOrder.push(name);
        root.kids.push(layers[name]);
      }
      current = layers[name];
      return current;
    }
    layer('Figure');            // the default, so a figure need not say anything

    function put(node) { current.kids.push(node); }

    function paintAttrs(mode) {
      var a = {};
      if (mode === 'fill' || mode === 'both') a.fill = st.fillStyle; else a.fill = 'none';
      if (mode === 'stroke' || mode === 'both') {
        a.stroke = st.strokeStyle;
        a['stroke-width'] = n(st.lineWidth);
        if (st.lineJoin && st.lineJoin !== 'miter') a['stroke-linejoin'] = st.lineJoin;
        if (st.dash && st.dash.length) a['stroke-dasharray'] = st.dash.map(n).join(' ');
      }
      if (st.globalAlpha !== 1) a.opacity = n(st.globalAlpha);
      return a;
    }

    function unsupported(name) {
      return function () {
        throw new Error('svgcanvas: ' + name + ' is not implemented. Either add it here or ' +
                        'keep it out of the figures, but do not let the export lie.');
      };
    }

    var ctx = {
      /* ---- state ---- */
      save: function () {
        stack.push({ st: JSON.parse(JSON.stringify(st)), node: current });
      },
      restore: function () {
        var f = stack.pop();
        if (!f) return;
        st = f.st;
        current = f.node;       // this is what closes a clip group
      },
      scale: function (x, y) {
        // Only ever used for the export backing scale, which SVG does not
        // need. Anything else would silently misplace everything.
        if (x !== 1 || y !== 1) { st.sx *= x; st.sy *= y; }
      },
      setLineDash: function (d) { st.dash = d && d.length ? d.slice() : null; },
      getLineDash: function () { return st.dash ? st.dash.slice() : []; },

      /* ---- path building ---- */
      beginPath: function () { path = []; cursor = null; start = null; },
      moveTo: function (x, y) { path.push('M' + n(x) + ' ' + n(y)); cursor = [x, y]; start = [x, y]; },
      lineTo: function (x, y) { path.push('L' + n(x) + ' ' + n(y)); cursor = [x, y]; },
      closePath: function () { path.push('Z'); if (start) cursor = start.slice(); },
      rect: function (x, y, w, h) {
        path.push('M' + n(x) + ' ' + n(y) + 'h' + n(w) + 'v' + n(h) + 'h' + n(-w) + 'Z');
        cursor = [x, y]; start = [x, y];
      },
      arc: function (cx, cy, r, a0, a1, ccw) {
        var TAU = Math.PI * 2;
        var sweep = ccw ? 0 : 1;
        var delta = a1 - a0;
        if (!ccw && delta < 0) delta = ((delta % TAU) + TAU) % TAU;
        if (ccw && delta > 0) delta = -(((-delta % TAU) + TAU) % TAU);
        var full = Math.abs(delta) >= TAU - 1e-9;
        if (full) delta = ccw ? -TAU : TAU;

        var p0 = [cx + r * Math.cos(a0), cy + r * Math.sin(a0)];
        if (cursor) path.push('L' + n(p0[0]) + ' ' + n(p0[1]));
        else { path.push('M' + n(p0[0]) + ' ' + n(p0[1])); start = p0; }

        if (full) {
          // One A command cannot draw a closed circle, so it goes as two halves.
          var mid = [cx + r * Math.cos(a0 + delta / 2), cy + r * Math.sin(a0 + delta / 2)];
          path.push('A' + n(r) + ' ' + n(r) + ' 0 0 ' + sweep + ' ' + n(mid[0]) + ' ' + n(mid[1]));
          path.push('A' + n(r) + ' ' + n(r) + ' 0 0 ' + sweep + ' ' + n(p0[0]) + ' ' + n(p0[1]));
          cursor = p0;
          return;
        }
        var p1 = [cx + r * Math.cos(a1), cy + r * Math.sin(a1)];
        var large = Math.abs(delta) > Math.PI ? 1 : 0;
        path.push('A' + n(r) + ' ' + n(r) + ' 0 ' + large + ' ' + sweep + ' ' + n(p1[0]) + ' ' + n(p1[1]));
        cursor = p1;
      },
      arcTo: function (x1, y1, x2, y2, r) {
        // The corner-fillet form, which is what roundRect is built out of.
        var p0 = cursor || [x1, y1];
        var v1 = [p0[0] - x1, p0[1] - y1], v2 = [x2 - x1, y2 - y1];
        var l1 = Math.hypot(v1[0], v1[1]), l2 = Math.hypot(v2[0], v2[1]);
        if (l1 < 1e-9 || l2 < 1e-9 || r < 1e-9) { ctx.lineTo(x1, y1); return; }
        v1 = [v1[0] / l1, v1[1] / l1];
        v2 = [v2[0] / l2, v2[1] / l2];
        var ang = Math.acos(Math.max(-1, Math.min(1, v1[0] * v2[0] + v1[1] * v2[1])));
        if (Math.abs(ang) < 1e-6 || Math.abs(ang - Math.PI) < 1e-6) { ctx.lineTo(x1, y1); return; }
        var tan = r / Math.tan(ang / 2);
        tan = Math.min(tan, l1, l2);
        var t1 = [x1 + v1[0] * tan, y1 + v1[1] * tan];
        var t2 = [x1 + v2[0] * tan, y1 + v2[1] * tan];
        var cross = v1[0] * v2[1] - v1[1] * v2[0];
        ctx.lineTo(t1[0], t1[1]);
        path.push('A' + n(r) + ' ' + n(r) + ' 0 0 ' + (cross < 0 ? 1 : 0) + ' ' + n(t2[0]) + ' ' + n(t2[1]));
        cursor = t2;
      },

      /* ---- painting ---- */
      fill: function () { if (path.length) put(new Leaf('path', mix({ d: path.join('') }, paintAttrs('fill')))); },
      stroke: function () { if (path.length) put(new Leaf('path', mix({ d: path.join('') }, paintAttrs('stroke')))); },
      fillRect: function (x, y, w, h) {
        if (w === 0 || h === 0) return;
        put(new Leaf('rect', mix(rectAttrs(x, y, w, h), paintAttrs('fill'))));
      },
      strokeRect: function (x, y, w, h) {
        put(new Leaf('rect', mix(rectAttrs(x, y, w, h), paintAttrs('stroke'))));
      },
      clip: function () {
        var id = 'clip' + (++clipSeq);
        defs.push('<clipPath id="' + id + '"><path d="' + path.join('') + '"/></clipPath>');
        var g = new Group(null, null);
        g.attrs['clip-path'] = 'url(#' + id + ')';
        put(g);
        current = g;            // everything until the matching restore lands here
      },

      /* ---- text ---- */
      fillText: function (str, x, y) {
        if (str == null || str === '') return;
        var f = parseFont(st.font);
        var a = {
          x: n(x), y: n(y),
          'font-family': f.family,
          'font-size': f.size,
          'font-weight': f.weight === '400' ? null : f.weight,
          fill: st.fillStyle,
          'text-anchor': st.textAlign === 'center' ? 'middle'
                       : (st.textAlign === 'right' || st.textAlign === 'end') ? 'end' : null,
          'dominant-baseline': baseline(st.textBaseline),
          'letter-spacing': st.letterSpacing || null,
          'xml:space': /^\s|\s$/.test(str) ? 'preserve' : null
        };
        if (st.globalAlpha !== 1) a.opacity = n(st.globalAlpha);
        put(new Leaf('text', a, str, 'text'));
      },
      measureText: function (str) {
        var m = metrics();
        m.font = st.font;
        if (st.letterSpacing !== undefined && 'letterSpacing' in m) m.letterSpacing = st.letterSpacing || '0px';
        return m.measureText(str);
      },

      /* ---- things a figure must not reach for without adding them here ---- */
      drawImage: unsupported('drawImage'),
      createLinearGradient: unsupported('createLinearGradient'),
      createRadialGradient: unsupported('createRadialGradient'),
      createPattern: unsupported('createPattern'),
      bezierCurveTo: unsupported('bezierCurveTo'),
      quadraticCurveTo: unsupported('quadraticCurveTo'),
      ellipse: unsupported('ellipse'),
      getImageData: unsupported('getImageData'),

      /* ---- the layer hook ---- */
      __layer: function (name) {
        if (stack.length) {
          // Inside a save/clip the insertion point belongs to the clip group,
          // and moving it would strand everything drawn after the restore.
          if (global.console) console.warn('svgcanvas: Iso.layer("' + name +
            '") ignored, it was called inside a save() or a clip()');
          return;
        }
        layer(name);
      },
      __finish: function (mode) { return finish(mode); }
    };

    /* Properties, so `ctx.fillStyle = x` works the way the figures expect. */
    ['fillStyle', 'strokeStyle', 'lineWidth', 'lineJoin', 'globalAlpha', 'font',
     'textAlign', 'textBaseline', 'letterSpacing'].forEach(function (k) {
      Object.defineProperty(ctx, k, {
        get: function () { return st[k]; },
        set: function (v) { st[k] = v; }
      });
    });

    function rectAttrs(x, y, w, h) {
      return {
        x: n(w < 0 ? x + w : x), y: n(h < 0 ? y + h : y),
        width: n(Math.abs(w)), height: n(Math.abs(h))
      };
    }
    function mix(a, b) { Object.keys(b).forEach(function (k) { a[k] = b[k]; }); return a; }

    function baseline(v) {
      if (v === 'middle') return 'central';
      if (v === 'top' || v === 'hanging') return 'hanging';
      if (v === 'bottom') return 'text-after-edge';
      return null;              // alphabetic is SVG's own default
    }

    function parseFont(f) {
      // The figures build their font strings in one place, iso.js text(), in
      // the form "<weight> <size>px <family list>".
      var m = /^\s*(\d+)?\s*(\d+(?:\.\d+)?)px\s+(.*)$/.exec(f);
      if (!m) return { weight: '400', size: 13, family: 'sans-serif' };
      return { weight: m[1] || '400', size: n(parseFloat(m[2])), family: m[3] };
    }

    /** Rebuild the tree with text lifted into its own top-level layer. */
    function byKind() {
      var art = new Group('Artwork', 'layer');
      var txt = new Group('Text', 'layer');
      (function walk(node, artParent, txtParent) {
        node.kids.forEach(function (k) {
          if (k.tag === 'g') {
            // A clip group has to be mirrored into both halves or the text
            // inside it loses its clip.
            // The mirrored copies are ordinary groups: only Artwork and Text
            // are layers here, or Illustrator would show each section twice at
            // the top level and neither copy would be the whole section.
            var ga = new Group(k.id, null), gt = new Group(k.id, null);
            ga.attrs = k.attrs; gt.attrs = k.attrs;
            walk(k, ga, gt);
            if (ga.kids.length) artParent.kids.push(ga);
            if (gt.kids.length) txtParent.kids.push(gt);
          } else if (k.kind === 'text') txtParent.kids.push(k);
          else artParent.kids.push(k);
        });
      })(root, art, txt);
      var r = new Group(null, null);
      if (art.kids.length) r.kids.push(art);
      if (txt.kids.length) r.kids.push(txt);
      return r;
    }

    function finish(mode) {
      var tree = mode === 'kind' ? byKind() : root;
      var out = [];
      out.push('<?xml version="1.0" encoding="UTF-8"?>');
      out.push('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" ' +
               'width="' + n(w) + '" height="' + n(h) + '" viewBox="0 0 ' + n(w) + ' ' + n(h) + '">');
      if (defs.length) out.push('  <defs>' + defs.join('') + '</defs>');
      if (bg) out.push('  <rect id="Background" width="' + n(w) + '" height="' + n(h) +
                       '" fill="' + esc(bg) + '"/>');
      var uniq = uniquifier();
      tree.kids.forEach(function (k) { serialise(k, out, 1, uniq); });
      out.push('</svg>');
      return out.join('\n');
    }

    return { ctx: ctx, toSVG: finish, layerNames: function () { return layerOrder.slice(); } };
  }

  global.SvgCanvas = SvgCanvas;
})(window);
