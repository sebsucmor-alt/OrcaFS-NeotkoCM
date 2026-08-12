/* ===========================================================================
 * tour.js — the shell, plus the small helpers every demo shares.
 *
 * Rules this file follows:
 *   - the page must be complete and readable with this script absent,
 *   - no network, no storage beyond one theme preference,
 *   - demos boot lazily; a page with six canvases must not cost six canvases
 *     of work before the visitor has scrolled to the first one.
 * =========================================================================== */

(function (global) {
  'use strict';

  /* ------------------------------------------------------------------ dom */

  const $  = (sel, root) => (root || document).querySelector(sel);
  const $$ = (sel, root) => Array.from((root || document).querySelectorAll(sel));

  /** el('div.foo', {attrs}, [children|string]) */
  function el(spec, attrs, kids) {
    const m = /^([a-z0-9]+)?((?:[.#][\w-]+)*)$/i.exec(spec) || [];
    const node = document.createElement(m[1] || 'div');
    (m[2] || '').split(/(?=[.#])/).forEach(tok => {
      if (!tok) return;
      if (tok[0] === '.') node.classList.add(tok.slice(1));
      else node.id = tok.slice(1);
    });
    if (attrs) {
      for (const k in attrs) {
        const v = attrs[k];
        if (k === 'text') { node.textContent = v; continue; }
        if (k === 'html') { node.innerHTML = v; continue; }
        if (k.startsWith('on') && typeof v === 'function') { node.addEventListener(k.slice(2), v); continue; }
        // ARIA states are strings, not HTML boolean attributes: aria-pressed
        // must literally read "true"/"false" or a [aria-pressed="true"]
        // selector never matches and nothing ever looks selected.
        if (k.startsWith('aria-') && typeof v === 'boolean') { node.setAttribute(k, String(v)); continue; }
        if (v == null || v === false) continue;
        node.setAttribute(k, v === true ? '' : v);
      }
    }
    if (kids != null) {
      (Array.isArray(kids) ? kids : [kids]).forEach(k => {
        if (k == null) return;
        node.appendChild(typeof k === 'string' ? document.createTextNode(k) : k);
      });
    }
    return node;
  }

  /* ---------------------------------------------------------------- theme */

  const THEME_KEY = 'fs-tour-theme';

  function applyTheme(t) {
    if (t === 'light' || t === 'dark') document.documentElement.setAttribute('data-theme', t);
    else document.documentElement.removeAttribute('data-theme');
    $$('[data-theme-toggle]').forEach(b => {
      b.setAttribute('title', 'Theme: ' + (t || 'system') + ' — click to change');
      b.setAttribute('aria-label', 'Theme: ' + (t || 'system'));
    });
    document.dispatchEvent(new CustomEvent('tour:themechange'));
  }

  function currentTheme() {
    let t = null;
    try { t = localStorage.getItem(THEME_KEY); } catch (e) { /* private mode */ }
    return t;
  }

  function initTheme() {
    applyTheme(currentTheme());
    $$('[data-theme-toggle]').forEach(btn => {
      btn.addEventListener('click', () => {
        // system -> light -> dark -> system
        const order = [null, 'light', 'dark'];
        const next = order[(order.indexOf(currentTheme()) + 1) % order.length];
        try {
          if (next) localStorage.setItem(THEME_KEY, next);
          else localStorage.removeItem(THEME_KEY);
        } catch (e) { /* ignore */ }
        applyTheme(next);
      });
    });
  }

  /** True when the page is currently rendering dark, however that was decided. */
  function isDark() {
    const t = document.documentElement.getAttribute('data-theme');
    if (t) return t === 'dark';
    return global.matchMedia && global.matchMedia('(prefers-color-scheme: dark)').matches;
  }

  /** Read a CSS custom property — so canvases can match the page. */
  function token(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  /* ------------------------------------------------------- TOC + scrollspy */

  function buildToc() {
    const rail = $('[data-toc]');
    const article = $('.article');
    if (!rail || !article) return;

    const heads = $$('h2[id], h3[id]', article);
    if (!heads.length) { rail.remove(); return; }

    const ol = el('ol');
    heads.forEach(h => {
      const li = el('li' + (h.tagName === 'H3' ? '.rail--sub' : ''));
      li.appendChild(el('a', { href: '#' + h.id, text: h.dataset.short || h.textContent }));
      ol.appendChild(li);
    });
    rail.appendChild(ol);

    const links = new Map();
    $$('a', ol).forEach(a => links.set(a.getAttribute('href').slice(1), a));

    let active = null;
    const setActive = id => {
      if (id === active) return;
      if (active && links.get(active)) links.get(active).classList.remove('is-active');
      active = id;
      const a = links.get(id);
      if (a) {
        a.classList.add('is-active');
        // keep the active entry in view inside a long, scrolling rail
        const r = rail.getBoundingClientRect(), ar = a.getBoundingClientRect();
        if (ar.top < r.top || ar.bottom > r.bottom) {
          rail.scrollTop += ar.top - r.top - r.height / 2 + ar.height / 2;
        }
      }
    };

    if (!('IntersectionObserver' in global)) return;

    // Track which headings are above the fold line; the last one wins.
    const seen = new Map();
    const io = new IntersectionObserver(entries => {
      entries.forEach(e => seen.set(e.target.id, e));
      let best = null;
      heads.forEach(h => {
        const e = seen.get(h.id);
        if (!e) return;
        if (e.boundingClientRect.top < 120) best = h.id;
      });
      if (!best) {
        const visible = heads.find(h => { const e = seen.get(h.id); return e && e.isIntersecting; });
        best = visible ? visible.id : heads[0].id;
      }
      setActive(best);
    }, { rootMargin: '-110px 0px -70% 0px', threshold: [0, 1] });

    heads.forEach(h => io.observe(h));
  }

  /* --------------------------------------------- anchor links on headings */

  function anchorHeadings() {
    $$('.article h2[id], .article h3[id]').forEach(h => {
      if ($('.hanchor', h)) return;
      const a = el('a.hanchor', {
        href: '#' + h.id, 'aria-hidden': 'true', tabindex: '-1', text: '¶'
      });
      a.style.cssText =
        'margin-left:.4em;font-family:var(--mono);font-size:.62em;color:var(--ink-faint);' +
        'text-decoration:none;opacity:0;transition:opacity .15s';
      h.appendChild(a);
      h.addEventListener('mouseenter', () => { a.style.opacity = '1'; });
      h.addEventListener('mouseleave', () => { a.style.opacity = '0'; });
    });
  }

  /* --------------------------------------------------------- demo booting */

  const registry = new Map();

  /** Register a demo factory against a `data-demo="name"` element. */
  function demo(name, factory) { registry.set(name, factory); }

  function bootDemos() {
    const nodes = $$('[data-demo]');
    if (!nodes.length) return;

    const boot = node => {
      if (node.dataset.booted) return;
      const f = registry.get(node.dataset.demo);
      if (!f) return;
      node.dataset.booted = '1';
      try { f(node); }
      catch (err) {
        node.dataset.booted = 'error';
        // A broken demo must not take the page down with it.
        node.appendChild(el('p.small.muted', {
          text: 'This demo failed to start in this browser. The text above still describes it in full.'
        }));
        if (global.console) console.error('[tour] demo "' + node.dataset.demo + '" failed:', err);
      }
    };

    // Arriving on a deep link is the one case where lazy booting is wrong:
    // a demo further up the page boots later, grows from an empty box to a
    // few hundred pixels, and pushes the thing you linked to off the screen.
    // So when there is a hash, boot everything first and re-aim afterwards.
    if (location.hash) {
      nodes.forEach(boot);
      const target = document.getElementById(decodeURIComponent(location.hash.slice(1)));
      if (target) {
        const aim = () => target.scrollIntoView({ block: 'start', behavior: 'auto' });
        requestAnimationFrame(() => requestAnimationFrame(aim));
        // images finishing after that would move it again
        global.addEventListener('load', () => requestAnimationFrame(aim), { once: true });
      }
      return;
    }

    if (!('IntersectionObserver' in global)) { nodes.forEach(boot); return; }

    const io = new IntersectionObserver((entries, obs) => {
      entries.forEach(e => {
        if (!e.isIntersecting) return;
        obs.unobserve(e.target);
        boot(e.target);
      });
    }, { rootMargin: '260px 0px' });

    nodes.forEach(n => io.observe(n));
  }

  /**
   * In-page hash links have the same problem in reverse: a demo below the
   * fold has not booted yet, so the browser scrolls to where it currently is
   * rather than where it ends up. Boot it before the jump.
   */
  function interceptHashLinks() {
    document.addEventListener('click', ev => {
      const a = ev.target.closest && ev.target.closest('a[href^="#"]');
      if (!a) return;
      const id = decodeURIComponent(a.getAttribute('href').slice(1));
      if (!id) return;
      const target = document.getElementById(id);
      if (!target) return;
      $$('[data-demo]', target).forEach(n => {
        const f = registry.get(n.dataset.demo);
        if (f && !n.dataset.booted) { n.dataset.booted = '1'; try { f(n); } catch (e) { /* handled on boot */ } }
      });
    }, true);
  }

  /* --------------------------------------------------- canvas with real DPI */

  /**
   * A canvas that stays sharp on retina and re-renders on resize and on a
   * theme change (because demos read CSS tokens for their chrome colours).
   *
   * @param {HTMLElement} host
   * @param {(ctx, w, h) => void} draw   w/h are CSS pixels, not device pixels
   */
  function canvas(host, draw, opts) {
    const o = opts || {};
    const cv = el('canvas');
    cv.style.cssText = 'display:block;width:100%;' +
      (o.height ? 'height:' + o.height + 'px;' : '') +
      (o.cursor ? 'cursor:' + o.cursor + ';' : '');
    if (o.aspect) cv.style.aspectRatio = o.aspect;
    host.appendChild(cv);

    const ctx = cv.getContext('2d');
    let w = 0, h = 0;

    function render() {
      const r = cv.getBoundingClientRect();
      if (!r.width) return;
      const dpr = Math.min(global.devicePixelRatio || 1, 2.5);
      w = r.width; h = r.height;
      cv.width = Math.round(w * dpr);
      cv.height = Math.round(h * dpr);
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.clearRect(0, 0, w, h);
      draw(ctx, w, h);
    }

    let raf = 0;
    const schedule = () => {
      if (raf) return;
      raf = requestAnimationFrame(() => { raf = 0; render(); });
    };

    if ('ResizeObserver' in global) new ResizeObserver(schedule).observe(cv);
    else global.addEventListener('resize', schedule);
    document.addEventListener('tour:themechange', schedule);

    schedule();
    return { canvas: cv, ctx, render: schedule, get width() { return w; }, get height() { return h; } };
  }

  /** Pointer position in CSS pixels relative to an element. */
  function localPoint(ev, node) {
    const r = node.getBoundingClientRect();
    const p = ev.touches ? ev.touches[0] : ev;
    return { x: p.clientX - r.left, y: p.clientY - r.top };
  }

  /**
   * Drag helper that works for mouse and touch and cleans up after itself.
   * `onDown` returns false to decline the drag.
   */
  function draggable(node, handlers) {
    const h = handlers || {};
    let dragging = false;

    const down = ev => {
      const p = localPoint(ev, node);
      if (h.onDown && h.onDown(p, ev) === false) return;
      dragging = true;
      if (ev.cancelable) ev.preventDefault();
      global.addEventListener('mousemove', move);
      global.addEventListener('touchmove', move, { passive: false });
      global.addEventListener('mouseup', up);
      global.addEventListener('touchend', up);
    };
    const move = ev => {
      if (!dragging) return;
      if (ev.cancelable) ev.preventDefault();
      if (h.onMove) h.onMove(localPoint(ev, node), ev);
    };
    const up = ev => {
      dragging = false;
      global.removeEventListener('mousemove', move);
      global.removeEventListener('touchmove', move);
      global.removeEventListener('mouseup', up);
      global.removeEventListener('touchend', up);
      if (h.onUp) h.onUp(ev);
    };

    node.addEventListener('mousedown', down);
    node.addEventListener('touchstart', down, { passive: false });
    if (h.onHover) node.addEventListener('mousemove', ev => { if (!dragging) h.onHover(localPoint(ev, node), ev); });
    if (h.onLeave) node.addEventListener('mouseleave', h.onLeave);
    return () => { node.removeEventListener('mousedown', down); node.removeEventListener('touchstart', down); };
  }

  /* ------------------------------------------------------ control builders */

  /** A labelled range with a live value readout. */
  function slider(opts) {
    const o = opts || {};
    const val = el('b', { text: o.format ? o.format(o.value) : String(o.value) });
    const input = el('input', {
      type: 'range',
      min: o.min, max: o.max, step: o.step == null ? 1 : o.step, value: o.value
    });
    const wrap = el('label.ctrl', null, [
      el('span.ctrl__label', null, [el('span', { text: o.label }), val]),
      input
    ]);
    input.addEventListener('input', () => {
      const v = parseFloat(input.value);
      val.textContent = o.format ? o.format(v) : String(v);
      if (o.onInput) o.onInput(v);
    });
    wrap.set = v => {
      input.value = v;
      val.textContent = o.format ? o.format(v) : String(v);
    };
    wrap.get = () => parseFloat(input.value);
    return wrap;
  }

  /** A labelled <select>. `options` is [[value, label], ...]. */
  function select(opts) {
    const o = opts || {};
    const sel = el('select');
    (o.options || []).forEach(([v, l]) => {
      sel.appendChild(el('option', { value: v, text: l, selected: String(v) === String(o.value) }));
    });
    const wrap = el('label.ctrl', null, [
      el('span.ctrl__label', null, [el('span', { text: o.label })]),
      sel
    ]);
    sel.addEventListener('change', () => { if (o.onChange) o.onChange(sel.value); });
    wrap.get = () => sel.value;
    wrap.set = v => { sel.value = v; };
    return wrap;
  }

  /** A segmented button group. `options` is [[value, label], ...]. */
  function segmented(opts) {
    const o = opts || {};
    const group = el('div.segmented', { role: 'group', 'aria-label': o.label || '' });
    let value = o.value;
    const btns = (o.options || []).map(([v, l]) => {
      const b = el('button', { type: 'button', text: l, 'aria-pressed': String(v) === String(value) });
      b.addEventListener('click', () => {
        value = v;
        btns.forEach(x => x.setAttribute('aria-pressed', 'false'));
        b.setAttribute('aria-pressed', 'true');
        if (o.onChange) o.onChange(v);
      });
      group.appendChild(b);
      return b;
    });
    group.get = () => value;
    return o.label
      ? el('div.ctrl', null, [el('span.ctrl__label', null, [el('span', { text: o.label })]), group])
      : group;
  }

  /** A checkbox that reads as a sentence. */
  function toggle(opts) {
    const o = opts || {};
    const input = el('input', { type: 'checkbox', checked: !!o.value });
    const wrap = el('label.switch', null, [input, el('span', { text: o.label })]);
    input.addEventListener('change', () => { if (o.onChange) o.onChange(input.checked); });
    wrap.get = () => input.checked;
    wrap.set = v => { input.checked = !!v; };
    return wrap;
  }

  /* ------------------------------------------------------------------ init */

  function init() {
    initTheme();
    buildToc();
    anchorHeadings();
    interceptHashLinks();
    bootDemos();
    document.documentElement.classList.add('js');
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();

  global.Tour = {
    $, $$, el, token, isDark,
    demo, canvas, draggable, localPoint,
    slider, select, segmented, toggle
  };
})(window);
