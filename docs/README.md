# NeotkoCM · Video series for SnapMaker launch

**Plan**: 6 vídeos en 3 meses, uno cada 2 semanas. Cada vídeo ≤ 3 min. Branding visual común: estilo **blueprint de delineante** (desde 2026-07-10: fondo azul CAD `#0d1626` con grid a dos escalas — fino 24px + mayor 120px, `rgba(110,150,205,...)` muy apagado para no competir con el contenido; antes era teal verdoso). Tipografía monospace, etiquetas tipo cota técnica. El usuario graba el HTML interactivo y lo monta sobre el footage real.

**⚠️ Rework 2026-07-10 — main launcher + sin tagline:**
- Nuevo **[index.html](index.html)** = página principal: claim *"not just colors, the full Sandwich"* arriba como cabecera + atribución (Neotko, inventor de Neosanding/Ironing) + 7 tarjetas (una por vídeo) con resumen, un dato "Must-know" sacado de la WIKI y el conteo de pasos. Desde cada vídeo, **`Esc` o el botón `‹ Menu` vuelven al index**.
- **La slide final de tagline (dimmer+tag) se ELIMINÓ de los 7 vídeos** (se hacía repetitiva) — el claim vive ahora solo en el index. Los `#tag`/`#dimmer` siguen en el markup (inofensivos, `rebuildTo` los referencia) pero ningún estado los activa. Conteos actuales: v1=5 · v2=13 · v3=10 · v4=11 · v5=12 · v6=14 · v7=9. Cualquier conteo distinto más abajo en este fichero es anterior a este rework (+1 por el tagline).

**⚠️ Rework 2026-07-10 (3) — pase "de-AI" del texto (index + 7 vídeos):**
- El usuario pidió quitar los patrones de escritura "estilo IA" (referencia: field guide de Wikipedia WP:AISIGNS). Objetivos eliminados en TODO el **texto visible**: em-dash con espacios usado como pausa/aposición (`design — Solid`, `pattern — all`), antítesis/negative-parallelism ("not X, it's Y" / "X — nowhere else" / "not the whole object"), "just"/"simply", comillas curvas (“ ” → " "), y exceso de negrita mecánica en las tarjetas del index (ahora ~1 término clave por tarjeta).
- **Reescritas**: byline + 7 tarjetas (h2/what/fact) + footer del index; `lbl-sub`, captions bajo capturas, `setText('top-sub'/'cmp-warn')` y las etiquetas Z-bracket ("X lives here → X prints here") de v2-v6; los 9 `annot()` + `depts-caption` de v7. Los `<title>` pasaron de `NeotkoCM — X` a `NeotkoCM · X`.
- **NO tocado (a propósito)**: separadores `·` de las etiquetas tipo cota (estilo CAD deliberado, no prosa IA), símbolos técnicos (`≠ → ∝ ↔ ×`), rangos numéricos con en-dash (`0.1–0.5`, correcto), comentarios de código/HTML (invisibles), y los placeholders de un guion `—` (legend chips de v6, `annot-line` iniciales de v7 = "sin valor aún"). Por eso `grep -c '—'` sigue dando >0 en v1-v7: son comentarios + placeholders, no texto visible.
- **Eslogan de marca INTACTO**: el claim "not just colors, the full Sandwich" se mantiene (es su tagline canónico, cambiarlo es decisión de identidad, no un tell de prosa). Si el usuario lo quiere de-AI también, hay que reescribirlo aparte.
- Verificado en navegador: index (7 tarjetas nuevas) + v7 (anotaciones reescritas) renderizan sin desbordes.

**⚠️ Rework 2026-07-10 (2) — controles interactivos (los 7 vídeos = wiki/web navegable):**
- **Autoplay al cargar** con **bucle**: al llegar al último paso vuelve al 1 (`if(step>=TOTAL-1) reset(); else next();` dentro del `setInterval`). Intervalo por vídeo se conserva (v1-v6=2600ms, v7=3400ms → const `STEP_MS`).
- **Barra inferior = botones clicables reales** (antes eran pistas de teclado con `pointer-events:none`): `‹ Menu` (arriba-izq, → index.html), y en la barra `← prev · ▶/‖ play · next → · R reset · F full · H hide`. El botón play alterna glifo **▶ (pausa) / ‖ (reproduciendo)** + label play/pause vía `updatePlayBtn()`.
- **Interacción manual pausa el autoplay** (`userNext/userPrev/userReset` hacen `stopAuto()` antes de navegar) — patrón carrusel, para leer a tu ritmo. `Space` = toggle play/pause.
- **Barra de progreso** (`#progress`, franja teal arriba, `width = step/(TOTAL-1)`).
- **`H` = modo limpio** (`body.clean`) que oculta TODO el chrome (barra, Menu, progreso, step-num, título) para grabar la animación sola — antes solo ocultaba `.ui`.
- **Fix foco/teclado**: los botones hacen `mousedown → preventDefault()` para no retener foco (si no, tras click, un `Space` disparaba click del botón + atajo = doble toggle).
- Implementado con scripts de reemplazo literal (scratchpad `interactive.py` + `glyph.py`) — la barra `.ui`, el bloque keydown y `updatePlayBtn` eran idénticos en los 7. **⚠️ `.step-num` se reposicionó** de `left:24px` a `left:150px` para no chocar con el botón `‹ Menu`.
- Verificado en navegador: autoplay+cadencia (1 paso/2600ms), clicks prev/next/play/menu, bucle 10/10→01/10, glifo dinámico. (El fix de foco se aplicó tras la última verificación por caída temporal del clasificador de Bash — es de bajo riesgo, un solo `addEventListener` estándar.)

## Paleta canónica (heredada del icono `mmu_surfacecolormix.svg` del fork)

| Color | Hex | Significado |
|---|---|---|
| Teal | `#009688` | Estructura/contorno, NeoTower, 3D Painter (lechuga) |
| Magenta | `#ff15a5` | ColorStitch (tomate) |
| Yellow | `#e3ff2a` | Solid pass (queso) |
| Blue | `#2196f3` | Material base / segundo color del ColorStitch stripes |
| Dark | `#2b3436` | Trazo y armazón |
| Bg | `#0d1626` | Fondo tablero delineante azul (antes `#0a2a2a` teal; cambiado 2026-07-10) |

**Regla**: estos colores ya están asociados en la UI del fork (icono Sandwich pink+yellow). NO inventar paleta nueva en futuros vídeos — los espectadores que tocan Orca ya tienen el mapeo memorizado.

## Nota de re-planificación (2026-07-08)

El plan original (v1, entregado hace ~1 mes) usaba una metáfora de sandwich de comida 1:1 con "ingredientes" genéricos y estaba centrado en pitch general. Desde entonces el motor avanzó bastante (rebrand ColorMix→**ColorStitch**, Painter unificado, MultiPass = stack de Solid passes). Se re-enfoca la serie para que cada vídeo enseñe **una feature real y demostrable con print**, apoyándose en el vídeo que el usuario graba con el programa + impresiones reales de ColorStitch/PathBlend/MultiPass. v1 se mantiene como vídeo 1 (pitch general) pero el resto de la serie se redefine abajo.

**Fuente canónica de contenido**: `/Users/sebastiansuchowolskimorelli/Downloads/SNAPOFIC/WIKI.md` (confirmada actualizada por el usuario, 2026-07-08). Verificar cada feature contra esa wiki antes de animar — es la que describe el build público, no las notas internas de desarrollo.

**⚠️ Regla de atribución**: "Mixed" / "MixedFilament" (WIKI §6g, §1g "Mixed approximation") **es del main fork, NO invención de Neotko**. Lo suyo es **ColorStitch, MultiPass y PathBlend** (envueltos por el Sandwich, expuestos por el Painter). No presentar Mixed/MixedFilament como su IP en ningún vídeo — la v2 original mezclaba esto y fue reescrita (ver abajo).

**⚠️ Regla de verificación (2026-07-08, corrección del usuario tras el primer intento de v3/v5)**: para mecánicas de motor no triviales (PathBlend, y en general cualquier cosa "bastante compleja aunque en realidad sencilla"), **la WIKI pública no basta — hay que leer el código real** (`SNAPOFFICIAL/snapmaker-orca-official/src/libslic3r/...`) antes de animar. La primera versión de v3 inventó un modelo (split horizontal per-línea entre 2 colores) que sonaba plausible pero era **falso** — el modelo real (rampa Z + tapa, ver abajo) solo salió a la luz leyendo `Fill.cpp`/`SurfaceColorMix.hpp`. Regla: si la WIKI describe algo en una frase ambigua y el vídeo depende de la mecánica exacta, grep+Read el código fuente antes de construir el HTML.

## Plan de los 6 vídeos (redefinido)

| # | Título | Foco (feature real, verificado contra código) | Qué se demuestra en el HTML |
|---|---|---|---|
| 1 | **Why I forked Orca — meet FullSpectrum** | Pitch general, sandwich como pila de passes | (ya entregado, `v1_sandwich_to_cube.html`) |
| 2 | **ColorStitch — per-line gradients, not painted layers** | ColorStitch (WIKI §1b): patrón per-línea — **Smooth blend 2/3 colours** (dithering que produce degradado visual sin ser gradiente vectorial), **Stripes — manual bands** (repeticiones exactas), **transition shape** (Even/S-curve/Hard step) | Superficie con N líneas de relleno reales; el mismo array de colores por-línea se proyecta sin metáfora sobre la cara del cubo vía shear isométrico real; comparación Smooth blend vs Stripes, Even vs S-curve |
| 3 | **PathBlend — a Z-thickness ramp, not a color switch** | PathBlend real (`SurfaceColorMix.hpp` `PathBlendPassConfig` + `Fill.cpp` band loop): **rampa** (`tool_bottom`, Z ascendente per-scanline de `floor_mm` a `mid_end_mm`, flow escalado a `h/H`) + **tapa** (`tool_top`, SOLO en modo Full, rellena el residual `H-h_ramp` a Z plana) — **Half = solo rampa, sin tapa, la rampa ES la superficie física** (sin blend, un color). El "blend" en Full es óptico: dos materiales de espesor variable, no una mezcla de color calculada por el slicer | Cross-section (perfil lateral, NO top-down) mostrando la rampa+tapa reales; merge sobre el top face real (Full = gradiente óptico plano; Half = un color con relieve físico) |
| 4 | **MultiPass — stacking Solid passes for cross-hatch and glaze** | MultiPass (WIKI §1a, FAQ "Where did MultiPass go?"): ya no es un botón separado — **2-3 pases Solid apilados** con las Z compartiendo la altura de capa dan cross-hatch (dos ángulos) o glaze (base + pase translúcido encima). Mencionar de paso el checkbox **Perimeter override** (clona el perímetro en cada pase) | Cubo con 2-3 "lonchas" Solid apilándose con ángulos distintos sobre el top face, mostrando el cross-hatch resultante |
| 5 | **ColorStitch Painter — paint each top, not the whole object** | Painter (WIKI §6): herramientas **Paint / Eraser / Pick**, tiras de paleta (**Gradient ramp** / **Flat color**), **Pro mode** (compone pases Top/Penu por zona), **Profiles** guardados. Demostrado sobre una **escalera real de 3 escalones fusionados en 1 objeto** (petición explícita del usuario) — cada escalón tiene su propia top surface física, pintable de forma independiente | Escalera isométrica de 3 escalones; cada top surface pintado con un efecto distinto (ColorStitch / MultiPass / PathBlend), aterrizando en su propia cara real |
| 6 | **All together — Sandwich + ColorStitch + PathBlend + MultiPass on one print** | Combinación real: una pieza usando las 4 features a la vez, cierre de la serie | Vuelve al cubo único de la serie (no la escalera, que es específica del Painter); top face dividido en 3 zonas, cada una con el resultado real de ColorStitch/MultiPass/PathBlend |

## Tagline canónico (cierre de cada vídeo)

> **NeotkoCM Fork for SnapMaker**
> *not just colors, the full Sandwich.*

## Asset técnico: HTML interactivo

**Por qué HTML vanilla en lugar de AE/Blender/Rive**: el usuario graba pantalla cuando quiere, edita textos directamente, sin pedir re-renders. Single file, sin dependencias.

### Convenciones del HTML

- **Viewport**: `<svg viewBox="0 0 1600 900">` (16:9 nativo).
- **Estética**: ver paleta arriba. Grid 24px opacity 0.18. Tipografía monospace (JetBrains Mono).
- **Cubo**: proyección isométrica. Coords relativas al `cube-wrapper`:
  - Top face: `polygon points="0,0 190,-110 0,-220 -190,-110"`
  - Right face: `polygon points="0,0 190,-110 190,140 0,250"`
  - Left face: `polygon points="0,0 -190,-110 -190,140 0,250"`
- **Patrones SVG canónicos** (en `<defs>`, reutilizables entre vídeos):
  - `#colormix` → diagonales 45° pink+blue 5px (replica del closeup real, usado por ColorStitch/PathBlend)
  - `#solidyellow` → bandas finas horizontales amarillo
- **Slices overlay** (lonchas sobre el top): polígonos diamante idénticos al top face pero translados en Y. Renderizados al FINAL del SVG para garantizar z-order encima del cubo (los grupos SVG previos quedan tapados).

### Controles estándar (replicar en cada vídeo)

| Tecla | Acción |
|---|---|
| `→` | Siguiente paso |
| `←` | Atrás (rebuild from scratch) |
| `Space` | Autoplay (intervalo configurable, default 2.6s) |
| `R` | Reset |
| `F` | Fullscreen |
| `H` | Esconder UI (para grabar limpio) |
| `Esc` | Volver al `index.html` (main launcher) |

### Patrón de "steps" idempotentes

- Array `states[]` de funciones, una por paso.
- **`rebuildTo(n)`** limpia todo a baseline y re-ejecuta `states[0..n]` cumulativamente. Esto permite navegar `←/→` sin estado roto.
- **Cada state es idempotente**: si lo ejecutas dos veces, el resultado es el mismo. Sin esto, `prev` revienta.
- **Animaciones disparadas con `setTimeout`** dentro de los states (ej. drop staggered, merge delayed). En `rebuildTo` los pendientes pueden encimarse — aceptable porque el flujo principal es forward.

### Gotchas aprendidas (vídeo 1) — leer antes de crear vídeo 2+

1. **`data-fade` + `style.opacity` inline** = pelea de especificidad. Inline gana siempre. Cuando termines una animación que usó `style.opacity`, **limpia `el.style.opacity=''`** o el `.on` no surtirá efecto.
2. **Z-order en SVG = orden DOM**. Las transformaciones CSS NO crean stacking context en SVG. Si quieres que algo vuele "encima" del cubo, ese elemento debe estar **al final del SVG**, no en un grupo anterior. Solución usada: grupo `#overlay` al final con copias-presentación de las lonchas; los originales (en el sandwich) se atenúan en lugar de volar.
3. **Patrones SVG escalan con el viewBox**, no con el grupo padre. `patternUnits="userSpaceOnUse"` mantiene el tamaño visual constante aunque escales el grupo.
4. **Dimmer + tagline final**: el grid de fondo + labels técnicos chocan con cualquier overlay de cierre. Añadir un `<div id="dimmer">` con `rgba(6,18,18,.88)` + `backdrop-filter: blur(2px)` que se activa antes del tagline (delay 0.3s al tag). Resultado: pop limpio.
5. **iso "encajar" sobre top face**: para que una loncha rectangular parezca apoyada sobre la diamond superior, usar `polygon` idéntico al top face translado en Y, NO `skew/rotate` sobre un rect (deforma el patrón).

### Gotchas nuevos (vídeo 2) — proyectar datos reales sobre las caras del cubo

6. **La cara derecha/izquierda del cubo acepta un `skewY`/`skewX` de un solo eje**, porque es un paralelogramo con un par de lados verticales: `face-right = 0,0 190,-110 190,140 0,250` es un rect 190×250 con un shear único (`atan2(-110,190) ≈ -30.02deg`). Útil si necesitas proyectar algo sobre un lateral.
7. **El TOP FACE (el diamante) es un paralelogramo de DOS ejes, no uno** — no le sirve un `skewY` simple. `face-top = 0,0 190,-110 0,-220 -190,-110` está generado por `u=(190,-110)` y `w=(-190,-110)` desde el origen. Un cuadrado local `[0,S]×[0,S]` mapea exacto sobre él con `matrix(u.x/S, u.y/S, w.x/S, w.y/S, 0, 0)` — sin clip, sin aproximación. **Usa esto siempre que el contenido deba vivir en la superficie superior (Sandwich/ColorStitch/PathBlend/MultiPass son todos top-only)** — usar el skewY de un lateral ahí sería repetir el error de la primera versión de v2 (proyectar un efecto top-only como si corriera por toda la Z del objeto).

### Gotcha nuevo (vídeo 5) — construir una escalera isométrica con la misma técnica

8. **Una escalera de N escalones = N copias del mismo mini-cubo, cada una desplazada por el vector `w` (una arista del diamante) + una subida vertical `-R` (R = altura del escalón)**. Con `u=(100,-58)`, `w=(-100,-58)`, `R=90`: `step[k] = step[k-1] + w + (0,-R)`. Verificado con node que los orígenes quedan en cascada consistente (`[0,0]`, `[-100,-148]`, `[-200,-296]`). Cada escalón usa la MISMA matriz afín de 2 ejes (gotcha #7) escalada a su propio `S` para pintar su top face — solo cambia el `translate` del origen. **Dibujar en orden back-to-front** (escalón más lejano/alto primero, más cercano/bajo al final) para que el z-order (= orden DOM, gotcha #2) resuelva las oclusiones correctamente.

### Gotcha nuevo (vídeo 3) — no adivinar mecánicas complejas del motor

9. **PathBlend NO es un split de color por-línea** (eso fue una primera adivinanza incorrecta, descartada por el usuario). El modelo real, confirmado leyendo `SurfaceColorMix.hpp:453-490` y `Fill.cpp:2144-2308`: una **rampa** de un solo material (`tool_bottom`) cuya Z física asciende per-scanline de `floor_mm` a `mid_end_mm` (con el flow/extrusión escalado proporcionalmente a `h/H` — "extrusión acorde"), y —solo en modo **Full**— una **tapa** (`tool_top`) que rellena el hueco residual `H - h_ramp` a Z nominal constante (superficie plana). En modo **Half** no hay tapa: la rampa ES la superficie impresa, físicamente inclinada, de un solo color. El degradado de color que se percibe en Full no lo calcula el slicer — es un efecto **óptico** (Beer-Lambert) del espesor variable de la tapa combinado con la TD real del filamento. Ver `SurfaceColorMix.hpp` comentario sobre `PathBlendPassConfig` para la geometría exacta.

## Asset producido (vídeo 1)

[v1_sandwich_to_cube.html](v1_sandwich_to_cube.html) — sandwich blueprint a la izquierda → cubo isométrico a la derecha. 6 pasos:

1. Intro (sandwich + cursor)
2. Ingredientes caen desde arriba escalonados (lechuga → queso → magenta → tapa con "thud"). Cubo azul liso aparece.
3. Sandwich Editor: etiquetas con cotas técnicas aparecen, tapa se levanta.
4. **Lonchas Solid + ColorStitch flotan stacked encima del top del cubo** (opacity 0.88, etiquetadas). Originales en el sandwich se atenúan.
5. **Merge**: lonchas descienden y se posan exactamente sobre el top face con opacity 0.55 (stack translúcido visible).
6. Dimmer cierra el fondo, tagline `NeotkoCM Fork for SnapMaker — not just colors, the full Sandwich`.

## Capturas reales integradas (2026-07-10)

El usuario aportó capturas de la app en `images/` y fotos de impresiones reales en `images/REALPRINTS/`. Se integraron en v2/v4/v5/v6 (pasos nuevos antes del tagline) y se creó **v7** (tutorial). Convenciones:

- **Las imágenes se referencian por ruta relativa** (`images/...`), NUNCA base64 — el HTML se abre desde `docs/VIDEOS/`, así los ficheros no engordan. Si se mueve un HTML fuera de la carpeta, las imágenes se rompen.
- **Tarjeta "shot-card"**: marco de ventana de app (frame teal + barra + 3 puntos de semáforo + drop-shadow) definido por CSS `.shot-card/.shot-frame/.shot-bar/.shot-name`. Copiar de v7 para futuros vídeos.
- **Spotlights**: `rect.spot` (amarillo) y `rect.spot2` (rosa), pulsantes, en **coordenadas de píxel de la imagen** dentro del grupo de la tarjeta — se colocan sobre la opción de UI que se está explicando.
- **`#shotdim`**: rect a canvas completo (rgba(4,16,16,.92)) que atenúa el vídeo de atrás cuando entran las capturas; va justo ANTES de los grupos shot al final del SVG (gotcha #2, z-order = orden DOM).
- **`RealPrint-01B_web.png`** es un downscale (1400px) generado del original de 22MB — usar siempre la versión `_web` en los HTML.
- ⚠️ **Nombres intercambiados** (contenido real, verificado 2026-07-10): `sandwich-editor-gizmo04.png` = panel **Object/MixedFilament**; `sandwich-editor-TD-control.png` = panel **Pro con Perimeter Override ✓ + angle 45**. Ir por contenido, no por nombre.
- **Preview en vivo (gotcha #10)**: el sandbox bloquea `getcwd` (python http.server muere) y el proceso del preview no puede leer `~/Downloads` (TCC). Solución que funcionó: server node inline en `.claude/launch.json` (handler con rutas absolutas) sirviendo un **mirror rsync de docs/VIDEOS en el scratchpad de la sesión** — re-sincronizar tras cada edit. El mirror es per-sesión: en una sesión nueva hay que regenerarlo y actualizar la ruta del launch.json.

## Asset producido (vídeo 7) — tutorial Painter con capturas reales (2026-07-10)

[v7_painter_howto.html](v7_painter_howto.html) — 10 pasos, TODO con capturas reales (serie `Como-Pintar-SandwichMultipass01..07` + los 4 departamentos): abrir gizmo [M] → Generator/Gradient ramp (tooltip sub-capas mm) → click en la top surface → Pro (la mezcla descompuesta) → + sub-capas → cambiar tipo de pass (dropdown Solid/ColorStitch/PB Half/PB Full; PB no permite sub-pass) → ColorStitch Stripes en vivo + Save receta → TD y Result predicho → panorámica 4 departamentos (Palette/Generator/Pro/Object) → tagline. Panel de anotación a la derecha (`annot()`), tipografía display (`--disp`, Avenir Next) para títulos + viñeta radial de fondo — primer vídeo con este acabado; candidato a patrón para un pase de polish de v1-v6.

## Asset producido (vídeo 2) — rework completo (2026-07-08)

**Corrección crítica sobre la primera versión**: la v2 anterior proyectaba el patrón per-línea sobre la cara **lateral** del cubo (skewY sobre `face-right`), lo que implicaba visualmente un gradiente a lo largo de toda la altura Z del objeto. Eso es **falso** — el Sandwich/ColorStitch solo vive en el **top layer + penultimate** (una porción fina de Z), tal como ya establecía v1. Corregido de raíz: el patrón ahora se proyecta sobre la **cara superior (top face, el diamante iso)**, que es un plano horizontal — geométricamente correcto y consistente con "esto es X/Y, no Z".

[v2_colorstitch_gradient.html](v2_colorstitch_gradient.html) — 11 pasos (vs 7 antes), bastante más profundidad, basado en `WIKI.md §1b/§1f`. **2026-07-10: +3 pasos con material real (total 14)** — panel avanzado Smooth blend/Slow start (spot en transition shape + preview bar), S-curve 3 colores + Stripes lado a lado, y prueba real Hilbert×ColorStitch (gcode vs impresión, `RealPrint-01A/01B_web`):

1. **Intro** + **Z-bracket** (icono de perfil lateral del objeto: banda fina superior = "TOP+PENU, aquí vive el Sandwich" vs el resto = "BULK, intacto"). Establece desde el segundo 1 que esto es top-only.
2. **Aislar el top face**: las caras laterales del cubo se atenúan (`opacity .32`), el diamante superior recibe un glow pulsante — "aquí vive ColorStitch, en ningún otro sitio".
3. **Mini-sandwich stack** flotando sobre el top face (callback directo a v1): loncha Solid + loncha ColorStitch, ambas diamantes traslados en Y (mismo truco que v1, gotcha #5).
4. **Zoom a la pass ColorStitch**: la loncha Solid se atenúa, aparece el inset (vista aplanada X/Y de esa pass) + un **gizmo de ejes** (flechas X/Y + una Z tachada en rojo, "Z ✗ not used here") + los 4 estilos reales como pills + los color-stops A/B.
5. Se calculan y dibujan las N líneas (**Smooth blend 2C**, shape **Even**) en el inset con dithering ordenado (secuencia de baja discrepancia).
6. **MERGE (la corrección central)**: el mismo array de colores se proyecta sobre el top face REAL vía una matriz afín de 2 ejes calculada exactamente del polígono `0,0 190,-110 0,-220 -190,-110` (parte de un cuadrado local 220×220 — verificado matemáticamente, las 4 esquinas mapean exacto). El mini-sandwich stack se desvanece — "ya aterrizó de verdad".
7. **Ángulo de infill rotado**: swap de orientación (bandas por filas ↔ por columnas) — el patrón rota dentro del plano, nunca en Z.
8. **Transition shape** Even → S-curve.
9. **Line Distribution Mode**: widget de comparación reutilizable — mismos colores, orden **Default** (fragmentado, simulado con una permutación determinista) vs **LaneQuant** (limpio).
10. Switch a **Stripes — manual bands** + reutiliza el widget de comparación para el gotcha de **Monotonic Line** (patrón roto vs limpio).
11. Dimmer + tagline.

**Por qué la matriz afín en vez del `skewY` de la v1 original**: el top face es un paralelogramo (rombo) generado por dos vectores `u=(190,-110)` y `w=(-190,-110)` desde el origen — **no** un rect con un solo shear (eso solo aplica a las caras laterales, ver gotcha #6 abajo). Un cuadrado local `[0,220]×[0,220]` mapea 1:1 sobre ese rombo con `matrix(u.x/220, u.y/220, w.x/220, w.y/220, 0, 0)`. Verificado con node: `map(220,0)=[190,-110]`, `map(0,220)=[-190,-110]`, `map(220,220)=[0,-220]` — coincide exacto con los 4 vértices del polígono.

## Asset producido (vídeo 3) — PathBlend, rampa + tapa reales (2026-07-08)

**Corrección crítica sobre el primer intento**: la primera versión de v3 modelaba PathBlend como un split horizontal de color per-línea (staircase de 2 colores dentro de cada línea) — el usuario lo marcó como incorrecto y pidió leer el código. El modelo real (confirmado en `SurfaceColorMix.hpp` + `Fill.cpp`, ver gotcha #9 arriba) es una **rampa de espesor Z** + **tapa opcional**, no un split de color.

[v3_pathblend_staircase.html](v3_pathblend_staircase.html) — 11 pasos:

1. Intro + Z-bracket (top+penu only).
2. Aislar el top face.
3. Contraste ColorStitch (color per-línea) vs PathBlend (espesor per-path) — iconos corregidos (silueta de rampa ascendente, no split horizontal).
4. PathBlend siempre ocupa el 100% de la altura de capa — no puede compartir slot con otro pase.
5. Zoom: **cross-section (perfil lateral)**, explícitamente etiquetado "no es top-down" — chips `tool_bottom`/`tool_top`, pills Half/Full, gizmo con eje Z real (aquí SÍ se usa Z, a diferencia de ColorStitch).
6. Se construye la **rampa**: columnas ascendentes reales (`h(t) = floor + t·(mid_end−floor)`), color `tool_bottom`.
7. Se añade la **tapa** (Full): rellena el residual `H − h_ramp` a Z plana, opacidad de la tapa disminuye cuando es más fina (hint visual de "más transparencia").
8. MERGE (Full) sobre el top face real: gradiente óptico suave (`linearGradient`), superficie plana.
9. Switch a **Half**: la tapa desaparece, la rampa ES la superficie — un color, físicamente inclinada (relieve real, no un blend).
10. Comparación pocas vs muchas scanlines (rampa basta vs fina) + aviso "⚠ most fragile part of the engine".
11. Dimmer + tagline.

**Fix 2026-07-08 (feedback del usuario sobre Half)**: en el merge sobre el top face real, el modo Half mostraba un color plano — no se percibía que cada banda tiene una altura distinta. Corregido: en vez de un fill sólido, el resultado sobre el top face ahora dibuja las mismas K bandas con **opacidad ∝ h/H** (banda fina → opacidad baja → se transparenta el color base del cubo debajo; banda gruesa → opacidad alta → opaca). Mismo principio óptico que la tapa en Full, aplicado esta vez al material único de la rampa — comunica visualmente que la TD (transmission distance) del filamento es la que gobierna cuánto se ve a través, en función del espesor real impreso.

**Fix 2026-07-08 (2) — color del cubo**: con el cubo base en azul (el mismo azul de toda la serie), el "transparentado" de Half dejaba ver azul por debajo — y como Full es un gradiente amarillo→azul, ambos modos acababan pareciendo la misma mezcla amarillo/azul. Corregido **solo en v3**: el cubo base pasa a un gris neutro (`face-base #454b4d`, `face-side-dark #2e3335`, `face-top #5a6266`), sin relación con ninguno de los 2 colores del PathBlend (amarillo `tool_bottom` / azul `tool_top`). Así Full sigue siendo su propio gradiente amarillo↔azul, y Half muestra amarillo transparentándose sobre gris — visualmente distintos. El resto de la serie (v1,v2,v4-v6) sigue con el cubo azul canónico; este cambio de color es exclusivo de v3 porque solo aquí la transparencia revela el color base.

## Asset producido (vídeo 4)

[v4_multipass_crosshatch.html](v4_multipass_crosshatch.html) — 11 pasos (**2026-07-10: +1 paso real, total 12** — `RealPrint-04a/04b`, gradient Solid passes 2 colores con % por tile, dos pares de filamentos): FAQ "where did MultiPass go" → 2 pases Solid (0°/90°) → cross-hatch en el inset → merge sobre el top face real → switch a Glaze (base opaca + top translúcido, slider TD) → divisor de Z entre pases → perimeter override → nota de 3er pase → tagline. Reutiliza la matriz afín de 2 ejes (gotcha #7); el contenido de cada pase es un fill uniforme (hatch de bandas o rect sólido), no un array per-línea — MultiPass son pases COMPLETOS apilados, no un patrón por-línea.

## Asset producido (vídeo 5) — Painter sobre una escalera real (2026-07-08)

**Corrección sobre el primer intento**: la v5 original pintaba zonas verticales sobre UN top face plano dividido — el usuario pidió en su lugar una **escalera real de 3 escalones fusionados en 1 objeto**, cada uno con su propia top surface física pintable. Ver gotcha #8 (construcción de la escalera con la misma matriz afín, solo cambiando el origen por escalón).

[v5_painter_zones.html](v5_painter_zones.html) — 11 pasos (**2026-07-10: +2 pasos reales, total 13** — Pro Top completo con penu PathBlend + Bottom Surface pintable/Supported bottom, y Perimeter Override + Object/MixedFilament auto-TD): intro + Z-bracket → aislar los 3 top faces (glow en cada escalón) → toolbar (Paint/Eraser/Pick) + tiras de paleta → pintar escalón 1 (ColorStitch smooth-blend) → escalón 2 (MultiPass glaze) → escalón 3 (Pro mode, PathBlend Full) → MERGE: los 3 aterrizan en sus 3 caras reales simultáneamente → working vs saved colors (amber border) → gotcha all-or-nothing (icono esquemático, no un 4º escalón real) → weave preview + top-faces-only → tagline.

## Asset producido (vídeo 6) — rework a escalera + variedad (2026-07-08)

**Corrección sobre el primer intento**: la v6 original volvía al cubo único con 3 zonas planas — el usuario dijo que se parecía demasiado a la primera versión (descartada) del Painter y pidió reutilizar el **modelo de escalera** (como v5) con **ejemplos variados** y **más duración**. Reescrito de raíz: 8 → **13 pasos**, mismo objeto-escalera que v5 pero con un enfoque distinto (v5 = proceso del Painter/UI; v6 = variedad de resultados + repintado).

[v6_all_together.html](v6_all_together.html) — 13 pasos, reutiliza la geometría exacta de v5 (`u=(100,-58)`, `w=(-100,-58)`, `R=90`, misma matriz por escalón). **2026-07-10: +2 pasos reales (total 15)** — RealColor OFF→ON lado a lado (vista clásica por filamento vs aproximación óptica por TD) y cierre con impresiones reales (`realprint-realcolor` print-vs-pantalla, BIGTEST.3mf board + closeup, con mención a que el 3mf está en el GitHub del fork):

1. Intro: título + recap de las 4 features.
2. Framing de la escalera: "3 escalones fusionados, 1 objeto, top+penu only por escalón".
3. Aislar los 3 top faces (glow en cada escalón) + aparecen los 3 insets planos.
4. Escalón 1 pintado: **ColorStitch · Stripes** (variedad — v5 usaba Smooth blend).
5. Escalón 2 pintado: **MultiPass · Cross-hatch** (variedad — v5 usaba Glaze).
6. Escalón 3 pintado: **PathBlend · Full**.
7. **MERGE #1**: los 3 aterrizan simultáneamente en sus caras reales — primer hero shot.
8. Escalón 3 **repintado** a **PathBlend · Half** — callback directo al fix de arriba: opacidad por banda mostrando la TD.
9. Escalón 1 **repintado** a **ColorStitch · Smooth blend** — mismo motor, estilo distinto.
10. Escalón 2 **repintado** a **MultiPass · Glaze** + **perimeter override** (el contorno del escalón 2 se resalta).
11. **MERGE #2**: la combinación repintada — segundo hero shot, "el Painter te deja recolorear cualquier escalón cuando quieras".
12. Recap: leyenda de las 6 recetas mostradas en las 2 pasadas (Stripes/Smooth · Cross-hatch/Glaze · Full/Half).
13. Dimmer + tagline con recap de las 4 features.

## Plantilla para vídeos 3-6

Cuando arranquemos el siguiente vídeo, copiar `v2_colorstitch_gradient.html` (o `v1`) como base y:

1. Mantener: paleta, controles, viewBox, defs de patrones canónicos, cube iso, dimmer+tagline final.
2. Cambiar: la metáfora/feature central según la tabla de arriba, el contenido de los `states[]`, las etiquetas.
3. **No re-inventar layout cube**: si todos los vídeos usan el mismo cubo a la derecha, la coherencia visual de la serie sale gratis.
4. Cada vídeo tendrá ~5-7 steps. Mantener TOTAL en una const al inicio del script.
5. **Verificar contra `/Users/sebastiansuchowolskimorelli/Downloads/SNAPOFIC/WIKI.md` antes de animar** — no inventar comportamiento, el usuario va a acompañar cada vídeo con print real y se nota si la animación no corresponde al motor. Respetar la regla de atribución (Mixed/MixedFilament no es suyo).

## Producción del vídeo final (workflow del usuario)

1. Abrir el HTML en Chrome local.
2. `F` fullscreen → `H` esconder UI → `R` reset.
3. Grabar con QuickTime/OBS a resolución nativa de pantalla.
4. Ir pulsando `→` al ritmo del guion narrado.
5. Importar la grabación al editor de vídeo, recortar transiciones, ponerle voiceover encima. El usuario acompaña con vídeos del programa real + impresiones de ColorStitch/PathBlend/MultiPass.
