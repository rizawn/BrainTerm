# BrainTerm — Brain Graph v2 Design & Implementation Plan

| Field | Value |
|---|---|
| Document | `docs/graph-v2.md` |
| Version | 1.0 — 2026-11-22 |
| Stack | C17, GTK4 + libadwaita, Cairo, OpenGL 3.3 Core / GLES 3.0 (`GtkGLArea` + `libepoxy`), Meson |
| Theme | Dusk (`data/theme.css`, `src/theme.h`, `src/theme.c`) |
| Single Source of Truth | `src/graph.c` (`BtGraph` model + deterministic spine/brainstem/cortex layout) |

---

## 1. Visual Direction & Clean-Room Identity

Brain Graph v2 elevates BrainTerm's graph view to a **dark, luminous, dense** aesthetic while preserving our Dusk palette and BrainTerm identity (zero copied assets, logos, branding, or code from any external video or product):

1. **Atmospheric canvas:** Deep dark background (`@bt_graph_bg`) with a subtle radial edge vignette (`@bt_graph_vignette`) and sparse, slow-drifting dust particles (`@bt_graph_dust`).
2. **Glowing nodes:** Crisp bright cores with soft radial halos, radius scaled by degree, tinted by semantic kind (`note`, `ai`, `tag`, `folder`, `session`, `ghost`). Hovering or selecting a node lights up the node and its 1-hop neighborhood while dimming all other nodes and edges to **25%**.
3. **Luminous filaments (edges):** Thin, low-alpha slightly curved / long lines rendered with **additive blending** so crossing bundles naturally glow brighter without muddying the canvas. Incident edges of the hovered/selected node illuminate in `@bt_graph_edge_active`.
4. **Cached fading labels:** Crisp Pango-rendered labels shown on hover, 1-hop neighbors, focused node, and top-degree hubs (zoom-scaled) with smooth alpha fade. In GL mode, labels are baked once into a single RGBA **Pango glyph/label texture atlas** (rebuilt only on graph or scale-factor change — zero per-frame Pango layout).
5. **Optional 3D orbit view:** Cortex point-cloud / bilateral brain-like volume deterministically derived from `BtGraph` on a worker thread (cortex clusters arched onto an ellipsoidal shell, spine along the vertical axis, brainstem/sessions anchored below). Orbitable via drag + scroll zoom, with slow idle rotation that stops automatically on user interaction, window unfocus, or Reduce Motion.
6. **Subtle edge pulse (optional):** Soft light ripples traveling outward along edges incident to the selected node; **off by default** on Low quality or Reduce Motion, toggleable in the graph toolbar.
7. **Two-pass half-res bloom (High quality):** Offscreen FBO bright-pass + separable horizontal/vertical Gaussian blur at half resolution, composited additively onto the main scene. Disabled cleanly on Low quality.

---

## 2. Architecture

### 2.1 Unchanged Core Model (`src/graph.c`)
`BtGraph` (`nodes`, `edges`, `bt_graph_build`, `bt_graph_layout`, `bt_graph_apply_filter`, `bt_graph_pick`) remains untouched as the single source of truth. No layout output or struct contract changes are required.

### 2.2 Pure-C Graph View Logic (`src/graph_state.c/.h`, inside `libbt_core`, GTK-free)
Test-friendly, zero-GTK helper module containing:
- **Backend selection & fallback rules (`bt_graph_select_backend()`):**
  - Accepts runtime probe struct (`gl_context_ok`, `is_gles`, `gl_major`, `gl_minor`, `shaders_ok`, `fbo_ok`, `force_cairo_env`).
  - Requires OpenGL **≥ 3.3 Core** or OpenGL ES **≥ 3.0**.
  - Returns chosen backend (`BT_GRAPH_BACKEND_GL` or `BT_GRAPH_BACKEND_CAIRO`), whether bloom is allowed (`fbo_ok`), and a human-readable fallback reason string.
- **Hover/selection neighbor mask (`bt_graph_compute_neighbors()`):**
  - Fills a caller-owned `uint8_t *mask` (zero per-frame heap allocation) marking the target node (`2`), its 1-hop neighbors (`1`), and all others (`0`).
- **Label visibility rules (`bt_graph_label_alpha()`):**
  - Deterministic priority & alpha rule for any node given `(is_hovered, is_selected, is_focused, is_neighbor, degree, degree_p90, zoom, hover_fade, dim_factor)`.
- **Deterministic 3D layout (`bt_graph_compute_3d_layout()`):**
  - Derives `(x3, y3, z3)` per node purely from `BtGraph`'s existing `(x, y, hue, kind, degree)` — bilateral cortex ellipsoid, central spine axis, lower brainstem cluster — run on a `GThreadPool` worker and swapped atomically into the renderer.

### 2.3 Renderer Abstraction (`src/ui/graphview_renderer.h`)
Defines `BtGraphQuality` (`LOW`, `HIGH`), `BtGraphMode` (`2D`, `3D`), `BtGraphRenderState` (camera, hover/select/focus indices, pre-allocated neighbor mask, dust phase, pulse phase, quality, mode, reduce-motion), and the `BtGraphRenderer` vtable:
- `init` / `destroy`
- `upload_graph(renderer, graph)` — uploads static vertex/instance buffers & rebuilds Pango label atlas only when graph changes
- `upload_layout3d(renderer, pts3d, count)` — swaps in worker-computed 3D coordinates
- `resize(renderer, width, height, scale)`
- `render(renderer, state)` — **zero heap allocation per frame**

### 2.4 Two Backends
1. **Cairo 2D Fallback (`src/graphview.c`):**
   - Always compiled and always functional (works headless, over RDP, and on software/old drivers).
   - Pre-baked radial-gradient glow sprite surfaces per node kind (created once on init/scale change — zero per-frame `cairo_pattern_create_radial` calls).
   - Subtle radial vignette + fixed-size pre-allocated dust array (skipped when `reduce_motion` is on or `Low` quality cuts count).
   - Thin low-alpha edges (`CAIRO_OPERATOR_ADD` on High with subtle quadratic curve for ≤ 600 edges; fast batched single stroke on heavy graphs to preserve 60 fps).
   - Smooth fading Pango labels.
   - **Visual differences vs. GL (by design):** Cairo fallback intentionally differs visually from the GL renderer — there is **no half-resolution two-pass Gaussian bloom post-process** (node halos rely on pre-baked radial sprites instead), edge rendering differs under heavy graphs (quadratic curves with additive compositing on small/medium High-quality graphs, switching to straight batched strokes at high node counts to hold 60 FPS, whereas GL renders additive quads/curves at full density), and **3D orbit mode is GL-only** (disabled cleanly in Cairo fallback).
2. **OpenGL Renderer (`src/ui/graphgl.c/.h` on `GtkGLArea` + `libepoxy`):**
   - Shaders stored in `data/shaders/*.glsl` and embedded via `data/brainterm.gresource.xml`:
     - `graph_bg.vert/.frag` — deep canvas + subtle radial vignette
     - `graph_dust.vert/.frag` — GPU-driven drifting point sprites (uniform `u_time` only)
     - `graph_edge.vert/.frag` — instanced thin edge quads/curves with additive blending + optional pulse wave along selected-node edges
     - `graph_node.vert/.frag` — instanced quads with analytic bright core, radial halo, hover/focus ring, dashed ghost ring, rounded session badge
     - `graph_label.vert/.frag` — instanced textured quads sampling the cached Pango RGBA label atlas
     - `graph_bloom_extract.frag`, `graph_bloom_blur.frag`, `graph_bloom_composite.frag` — half-resolution ping-pong FBO two-pass separable Gaussian blur (High quality only)
   - Shader loader prepends `#version 330 core` (desktop GL) or `#version 300 es\nprecision highp float;` (GLES 3.0) automatically based on `gdk_gl_context_get_use_es()`.

### 2.5 Runtime Fallback Logic (Zero Crashes)
1. `BtGraphView` starts with a `GtkStack` containing `"gl"` (`GtkGLArea`), `"cairo"` (`GtkDrawingArea`), and `"empty"`.
2. If `BRAINTERM_GRAPH_BACKEND=cairo` is set, skip GL creation and activate `"cairo"` directly.
3. On `GtkGLArea::realize`:
   - Call `gtk_gl_area_make_current()` and check `gtk_gl_area_get_error()`.
   - Verify `epoxy_gl_version() >= (is_es ? 30 : 33)`.
   - Compile/link all core shaders.
   - If any step fails: mark `gl_failed = TRUE`, clean up partial GL objects, switch visible stack child to `"cairo"`, disable the `3D` toggle with an explanatory tooltip, and invoke the status-bar notice callback: `"Graph: Cairo fallback (<reason>)"`.
   - If core shaders succeed but half-res bloom FBO fails completeness: keep GL active, downgrade bloom off, and log a non-fatal notice.

---

## 3. Dusk Theme Tokens

All existing 12 tokens in `data/theme.css` / `src/theme.h` stay unchanged. Six role-named graph tokens are added and enforced by `tests/unit/test_theme.c` (CSS ↔ C parity) and documented in `docs/project.md`:

| CSS Token | C Macro (`src/theme.h`) | Hex | Role |
|---|---|---|---|
| `@bt_graph_bg` | `BT_COL_GRAPH_BG` | `#0B0C10` | Deep luminous dark graph canvas center |
| `@bt_graph_vignette` | `BT_COL_GRAPH_VIGNETTE` | `#06070A` | Subtle outer radial vignette edge |
| `@bt_graph_node_glow` | `BT_COL_GRAPH_NODE_GLOW` | `#9E91FA` | Soft halo tint for active/hovered node bloom |
| `@bt_graph_edge_dim` | `BT_COL_GRAPH_EDGE_DIM` | `#6E778A` | Low-alpha base filament color (additive) |
| `@bt_graph_edge_active` | `BT_COL_GRAPH_EDGE_ACTIVE` | `#A79BFA` | Highlighted edge & pulse color on hover/select |
| `@bt_graph_dust` | `BT_COL_GRAPH_DUST` | `#7A8296` | Sparse drifting background dust particle tint |

Existing node-kind tokens (`BT_COL_NODE_NOTE`, `BT_COL_NODE_AI`, `BT_COL_NODE_TAG`, `BT_COL_NODE_FOLDER`) continue to drive semantic node coloring.

---

## 4. Performance Budgets & Benchmarks

| Metric | Target Budget | Measurement |
|---|---|---|
| 1k nodes / 3k edges — High (GL, Intel UHD) | **≥ 60 FPS** (≤ 16.6 ms/frame) | `tests/bench/bench_graph_v2` |
| 1k nodes / 3k edges — Cairo fallback | **≥ 60 FPS** (≤ 16.6 ms/frame) | `tests/bench/bench_graph` |
| 3k nodes / 9k edges — High | **≥ 45 FPS** (≤ 22.2 ms/frame) | `tests/bench/bench_graph_v2` |
| 3k nodes / 9k edges — Low | **≥ 60 FPS** (≤ 16.6 ms/frame) | `tests/bench/bench_graph_v2` |
| Renderer init time | ≤ 150 ms (GL) / ≤ 20 ms (Cairo) | `tests/bench/bench_graph_v2` |
| Idle redraws (no animation / unfocused / reduced motion) | **0 draws/sec** | Tick callback removed immediately when idle |
| Per-frame heap allocations in render loop | **0** | Reusable buffers uploaded only on graph/layout change |

**Tick-callback discipline:** `gtk_widget_add_tick_callback()` runs ONLY while at least one continuous effect is active (birth animation, hover fade, camera ease, pulse, dust drift, or 3D idle orbit) AND the widget is mapped AND the toplevel window is active. When `reduce_motion` is enabled, dust drift, pulses, and 3D idle rotation are disabled so idle ticks drop strictly to zero.

---

## 5. Interaction, Controls & Accessibility

- **Graph Overlay Toolbar (Dusk-styled):**
  - Top-left pill: `[2D | 3D]` mode switch, `[Low | High]` quality toggle, `[Pulse]` toggle (off by default), `[Reduce motion]` toggle (persisted in `state.ini` `[ui] reduce_motion` and honoring `gtk-enable-animations == FALSE`).
  - Top-right pill: existing `[+]` `[-]` `[Fit]` `[Reset]` controls (Dusk-styled).
  - Bottom-left: subtle Dusk node-type legend.
- **Mouse:**
  - Hover: smooth highlight of node + 1-hop neighbors + incident edges; dims others to 25%.
  - Single-click node: select node + open note in editor (and trigger subtle pulse if Pulse is enabled).
  - Double-click node: smoothly center camera on node.
  - Drag background: pan (2D) or orbit yaw/pitch (3D); stops idle rotation.
  - Scroll: zoom (2D cursor-anchored; 3D dolly).
  - Right-click node: "Open terminal here".
- **Keyboard** (`focusable = TRUE`, visible Dusk accent focus ring on canvas & focused node):
  - Arrow keys: pan (2D) / orbit (3D).
  - `+` / `-` (and `=` / `_`): zoom in/out.
  - `F`: fit graph to view.
  - `Tab` / `Shift+Tab`: cycle visible nodes (shows crisp focus ring + label + gently centers if off-screen).
  - `Enter` / `Space`: open focused/selected note in editor.
- **Empty State:** Centered Dusk icon + title + actionable subtitle when the vault has 0 notes.

---

## 6. Phased Delivery Plan

| Step | Scope | Verification & Commit |
|---|---|---|
| **Step 0 (now)** | Recon + `docs/graph-v2.md` plan & budgets | Wait for approval |
| **Step 1** | Dusk graph tokens (`theme.css`/`theme.h`/`theme.c`), `src/graph_state.c/.h`, `src/ui/graphview_renderer.h`, upgraded Cairo 2D backend (vignette, cached radial glow sprites, additive/curved edges, dust, label fade), toolbar toggles (Quality, Reduce Motion), CMocka unit tests (`test_graph_v2.c`), 1k/3k Cairo bench | Build, tests, benches, `package-win.ps1 -SmokeTest`, verify stamp, commit |
| **Step 2** | `src/ui/graphgl.c/.h` (`GtkGLArea` + `libepoxy`), `data/shaders/*.glsl` via GResource, instanced 2D nodes + additive edges + dust sprites + Pango label atlas, automatic Cairo fallback + status-bar notice, 1k/3k GL bench & init-time bench | Build, tests, benches (report FPS), `package-win.ps1 -SmokeTest`, verify stamp, commit |
| **Step 3** | Half-res two-pass Gaussian bloom FBO (High quality), optional edge pulse, worker-thread 3D cortex layout + 3D orbit mode with focus-aware idle rotation | Build, tests, benches (report FPS), `package-win.ps1 -SmokeTest`, verify stamp, commit |
| **Step 4** | Full mouse & keyboard interactions (`Tab`, arrows, `+/-`, `F`, `Enter`, double-click center), focus rings, Reduce Motion persistence, empty-state polish, `docs/project.md` + `CHANGELOG.md` updates | Build, tests, benches, `package-win.ps1 -SmokeTest`, verify stamp, commit |
