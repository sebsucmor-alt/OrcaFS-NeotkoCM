/* ===========================================================================
 * glyphs.js — the slicer's own panel icons, as inline SVG.
 *
 * Port of draw_glyph() in src/slic3r/GUI/Gizmos/GizmoNeotkoStyle.hpp. The C++
 * draws every icon inside the unit square [0,1] scaled by `s`; here the square
 * is a 100 x 100 viewBox, so every coordinate below is the C++ one times 100.
 * Alpha fills keep the C++ ratio (IM_COL32 alpha / 255) as fill-opacity.
 *
 * Only the icons the tour uses are ported. If a shape changes in the C++,
 * change it here too or the site and the app stop drawing the same thing.
 *
 *   Glyphs.svg('Paint')              -> '<svg ...>' string, colour = currentColor
 *   Glyphs.svg('Paint', 'my-class')  -> same, with an extra class
 *   <span data-glyph="Paint"></span> -> filled in on load, for plain pages
 *
 * Tour only: this file is NOT one of the two shared with docs/BLOG/assets/.
 * ========================================================================= */
(function () {
  'use strict';

  var W = 8; // stroke width: max(1.2 px, 7.5 % of s) in the C++, ~8 units here

  function st(extra) { return 'fill="none" stroke="currentColor" stroke-width="' + (extra || W) + '"'; }
  function fl(op) { return 'fill="currentColor"' + (op != null ? ' fill-opacity="' + op + '"' : ''); }
  function poly(pts, closed, attrs) {
    return '<' + (closed ? 'polygon' : 'polyline') + ' points="' + pts + '" ' + attrs + '/>';
  }
  function line(x1, y1, x2, y2, attrs) {
    return '<line x1="' + x1 + '" y1="' + y1 + '" x2="' + x2 + '" y2="' + y2 + '" ' + attrs + '/>';
  }
  function rect(x0, y0, x1, y1, r, attrs) {
    return '<rect x="' + x0 + '" y="' + y0 + '" width="' + (x1 - x0) + '" height="' + (y1 - y0) +
           '" rx="' + (r || 0) + '" ' + attrs + '/>';
  }
  function circ(cx, cy, r, attrs) { return '<circle cx="' + cx + '" cy="' + cy + '" r="' + r + '" ' + attrs + '/>'; }

  function zone(lit, penu) {
    var y0 = [16, 33, 50, 67], s = '';
    if (penu) s += rect(14, y0[0], 86, y0[1], 0, fl(0.35));
    s += rect(14, y0[lit], 86, y0[lit] + 17, 0, fl());
    for (var i = 1; i < 4; i++)
      if (i !== lit && i !== lit + 1) s += line(14, y0[i], 86, y0[i], st(W * 0.7) + ' stroke-opacity=".43"');
    return s + rect(14, 16, 86, 84, 5, st());
  }

  var G = {
    Select:   poly('24,14 74,59 51,59 63,84 53,88 41,63 24,78', true, st()),
    Paint:    poly('16,46 80,50 50,80', true, fl(0.55)) +
              poly('46,16 80,50 50,80 16,46', true, st()) + circ(85, 80, 8.5, fl()),
    Erase:    poly('16,60 33,43 63,73 52,84 34,84', true, fl(0.45)) +
              poly('16,60 50,26 80,56 52,84 34,84', true, st()) + line(58, 90, 88, 90, st()),
    Pick:     line(18, 84, 58, 44, st(W * 1.3)) + line(44, 36, 66, 58, st()) + circ(70, 30, 15, fl()),
    Sticker:  poly('84,56 58,82 58,58', true, fl(0.59)) +
              poly('16,16 84,16 84,56 56,84 16,84', true, st()),
    EraseAll: '<path d="M72.93,54.91 A24,24 0 1 1 27.07,54.91 L50,12 Z" ' + st() + '/>' +
              line(14, 16, 86, 88, st()),
    Plus:     line(50, 20, 50, 80, st(W * 1.1)) + line(20, 50, 80, 50, st(W * 1.1)),
    Save:     rect(28, 14, 72, 66, 0, fl(0.35)) + poly('28,14 72,14 72,86 50,68 28,86', true, st()),
    TabPalette: rect(14, 14, 46, 46, 5, fl()) + rect(54, 14, 86, 46, 5, fl(0.63)) +
              rect(14, 54, 46, 86, 5, fl(0.41)) + rect(54, 54, 86, 86, 5, st()),
    TabGen:   [0, 1, 2, 3].map(function (i) {
                return rect(12 + 18 * i, 30, 26 + 18 * i, 70, 0, fl(((50 + 55 * i) / 255).toFixed(2)));
              }).join('') +
              line(12, 84, 86, 84, st()) + line(74, 77, 88, 84, st()) + line(74, 91, 88, 84, st()),
    TabPro:   poly('50,14 86,32 50,50 14,32', true, fl(0.45)) + poly('50,14 86,32 50,50 14,32', true, st()) +
              poly('14,50 50,68 86,50', false, st()) + poly('14,68 50,86 86,68', false, st()),
    ZoneTop:    zone(0, false),
    ZonePenu:   zone(1, true),
    ZoneBottom: zone(3, false),
    KSolid:   rect(18, 18, 82, 82, 6, fl()),
    KStitch:  rect(18, 18, 82, 82, 6, st()) + line(18, 48, 48, 18, st()) +
              line(18, 80, 80, 18, st()) + line(48, 82, 82, 48, st()),
    KPbHalf:  poly('16,78 84,50 84,78', true, fl()) + rect(16, 22, 84, 78, 5, st()),
    KPbFull:  poly('16,78 84,22 84,78', true, fl()) + rect(16, 22, 84, 78, 5, st()),
    Up:       poly('26,62 50,36 74,62', false, st(W * 1.1)),
    Down:     poly('26,38 50,64 74,38', false, st(W * 1.1)),
    ChevR:    poly('38,24 64,50 38,76', false, st(W * 1.1)),
    ChevD:    poly('24,38 50,64 76,38', false, st(W * 1.1)),
    Angle:    line(14, 80, 86, 80, st()) + line(14, 80, 70, 28, st()) +
              '<path d="M41.80,54.10 A38,38 0 0 1 52,80" ' + st(W * 0.8) + ' stroke-opacity=".67"/>',
    Spot:     circ(50, 50, 16, fl()) + circ(50, 50, 32, st() + ' stroke-opacity=".55"'),
    NoAngle:  circ(50, 50, 32, st() + ' stroke-dasharray="14.4 10.7"') + line(50, 50, 72, 30, st()),
    Eye:      '<path d="M10,50 C30,18 70,18 90,50 C70,82 30,82 10,50 Z" ' + st() + '/>' + circ(50, 50, 12, fl()),
    Load:     line(50, 64, 50, 18, st()) + poly('32,34 50,16 68,34', false, st()) +
              poly('16,60 16,84 84,84 84,60', false, st()),
    Adv:      [[30, 34], [50, 66], [70, 46]].map(function (v) {
                return line(16, v[0], 84, v[0], st(W * 0.8) + ' stroke-opacity=".59"') + circ(v[1], v[0], 8, fl());
              }).join(''),
    Ease:     line(14, 82, 86, 82, st(W * 0.8) + ' stroke-opacity=".43"') +
              '<path d="M14,82 C50,82 50,18 86,18" ' + st() + '/>',
    Mix:      circ(38, 42, 24, fl(0.55)) + circ(62, 42, 24, fl(0.55)) + circ(50, 62, 24, fl(0.55)),
    Group:    poly('14,26 14,18 42,18 48,26', false, st()) + rect(14, 26, 86, 80, 6, st()),
    Info:     circ(50, 50, 36, st()) + line(50, 46, 50, 70, st()) + circ(50, 32, 6, fl()),
    Square:   rect(22, 22, 78, 78, 6, fl(0.27)) + rect(22, 22, 78, 78, 6, st()),
    Trash:    line(18, 28, 82, 28, st()) + line(40, 20, 60, 20, st()) +
              poly('26,32 74,32 66,84 34,84', true, st()),
    Copy:     rect(16, 16, 66, 66, 6, st() + ' stroke-opacity=".59"') + rect(34, 34, 84, 84, 6, fl(0.24)) +
              rect(34, 34, 84, 84, 6, st()),
    Cube:     poly('50,16 86,36 50,56 14,36', true, fl(0.78)) + poly('14,36 50,56 50,88 14,68', true, fl(0.43)) +
              poly('50,56 86,36 86,68 50,88', true, fl(0.59)),
    Warn:     poly('50,14 94,84 6,84', true, fl(0.22)) + poly('50,14 94,84 6,84', true, st()) +
              line(50, 38, 50, 60, st(W * 1.1)) + circ(50, 72, 6, fl()),
    Target:   rect(12, 16, 88, 30, 5, fl()) + [34, 66].map(function (u) {
                return line(u, 40, u, 74, st(W * 0.9)) + poly(u + ',86 ' + (u - 10) + ',68 ' + (u + 10) + ',68', true, fl());
              }).join(''),
    // Not a draw_glyph icon: Orca's own round "?" help button, which sits in the Painter's toolbar.
    OrcaHelp: circ(50, 50, 44, fl()) +
              '<path d="M38,40 C38,26 62,26 62,40 C62,50 50,51 50,60" fill="none" stroke="#12151a" stroke-width="9" stroke-linecap="round"/>' +
              circ(50, 73, 5.5, 'fill="#12151a"')
  };

  function svg(name, cls) {
    var body = G[name];
    if (!body) return '';
    return '<svg class="glyph' + (cls ? ' ' + cls : '') + '" viewBox="0 0 100 100" aria-hidden="true" ' +
           'focusable="false" stroke-linecap="round" stroke-linejoin="round">' + body + '</svg>';
  }

  function fill(root) {
    [].slice.call((root || document).querySelectorAll('[data-glyph]')).forEach(function (n) {
      if (!n.firstChild) n.innerHTML = svg(n.getAttribute('data-glyph'));
    });
  }

  window.Glyphs = { svg: svg, fill: fill, names: Object.keys(G) };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', function () { fill(); });
  else fill();
})();
