# Forward-port: Screen Space 2D transform pixel snap (Godot 4.7+)

Guide for developers and agents when forward-porting this patch to newer Godot versions and resolving merge conflicts.

**Patch base version:** Godot 4.7 / 4.7.1-rc  
**Patch goal:** Screen-space 2D transform pixel snap modes for `snap_2d_transforms_to_pixel` — no jitter with physics interpolation and scrolling cameras, without corner stretching (unlike `snap_2d_vertices_to_pixel`).

**Modes:**

| Int | Name | Summary |
|-----|------|---------|
| `0` | **Canvas Space** | Upstream CPU canvas snap (default) |
| `1` | **Screen Space** | Always GPU screen snap (full patch v1) |
| `2` | **Screen Space When Moving** | Hybrid: canvas snap when static in world space, per-axis GPU screen snap when moving |

### Post-port fixes (documented in §3.4)

| Fix | Problem | Solution |
|-----|---------|----------|
| **Per-axis draw transform** | Mixed GPU/canvas: `final_xform` quantizes moving axis before shader; unsnapped-only loses static-axis canvas snap on descendants | `_build_screen_transform_snap_draw_xform()` + `_attach_canvas_item_for_draw` uses **`draw_xform`** |
| **Child snapped chain** | `child_snapped_parent = gpu ? unsnapped : final` broke cumulative canvas snap under fractional static parents | **`child_snapped_parent = final_xform` always**; unsnapped chain for detection only |
| **Per-item Screen Space** | No way to force always-GPU without axis detection | `SNAP_2D_TRANSFORMS_MODE_SCREEN` + `_item_uses_forced_screen_transform_snap()` |
| **Jitter detection** | Dominance ratio (2×) redundant with jitter epsilon for typical physics oscillation | Removed `DOMINANT_AXIS_RATIO`; keep epsilon `0.01` + jitter `0.02` only |

---

## 1. Patch overview

### Upstream problem (canvas space snap)

- Canvas space rounding on every node in the hierarchy plus a separate viewport/camera snap (`floor` / `ceil`) causes ±1 px jitter with physics interpolation, scrolling cameras, and moving platforms.
- `snap_2d_vertices_to_pixel` removes jitter but snaps **every corner**, which can distort sprite dimensions.

### Patch solution

1. New enum **`snap_2d_transforms_method`**: **Canvas Space** (upstream, `0`) / **Screen Space** (`1`) / **Screen Space When Moving** (`2`, hybrid).
2. In **Screen Space** and **Screen Space When Moving** modes:
   - Canvas space rounding in `renderer_canvas_cull.cpp` and `renderer_viewport.cpp` is **disabled** (unless overridden per item).
   - Transform origins are rounded in the shader **after** `canvas_transform` (camera already applied).
   - Only the **origin** is rounded, not each corner — quad size is preserved.
3. **Screen Space When Moving** additionally:
   - Detects movement per axis in **world/canvas space** (camera-independent origin).
   - Applies **CPU canvas snap** (same as mode `0`) on static axes.
   - Applies **GPU screen snap** on moving axes (per-axis instance flags in shader).
   - Passes **dual parent transform chains** during cull (snapped vs unsnapped) so static siblings keep cumulative canvas snap while GPU items (e.g. player) are not poisoned by ancestor canvas snap on the camera.
   - Propagates **`inherit_gpu`** from a moving parent to children with fixed local offset.
4. Per-viewport setting on `Viewport` / `SubViewport` (not only a project setting).
5. Draw offset correction in `Sprite2D` / `AnimatedSprite2D` / `RichTextLabel` when transform snap is enabled.
6. Per-item override on `CanvasItem`: **`snap_2d_transforms_mode`** (`Inherit` / **Canvas Space** / **Screen Space**) to opt into canvas space rounding, force always-GPU screen snap, or inherit from parent.
7. Particle quad mesh offset correction when screen space snap is active (centered quads).

### Recommended configuration (SubViewport)

**Always screen space** (moving platforms, camera follow, physics interpolation):

```
snap_2d_transforms_to_pixel = true
snap_2d_transforms_method = Screen Space   (Viewport.SNAP_2D_TRANSFORMS_METHOD_SCREEN = 1)
snap_2d_vertices_to_pixel = false
```

**Hybrid — autoscroll / static tilemap at fractional positions** (MF Community Edition default):

```
snap_2d_transforms_to_pixel = true
snap_2d_transforms_method = Screen Space When Moving   (Viewport.SNAP_2D_TRANSFORMS_METHOD_SCREEN_MOVING = 2)
snap_2d_vertices_to_pixel = false
```

Use case for mode `2`:
- **Static** objects (tilemap, props at non-integer world positions) → **canvas snap** — stay sharp relative to each other; no jitter vs integer-aligned tiles when the camera scrolls.
- **Moving** objects (player, enemies, autoscroll platform/camera group) → **GPU screen snap** — no jitter between each other or vs camera; relative subpixel jitter between moving objects is invisible.

Optional per-item overrides:

```
# Legacy-style canvas rounding on a screen-space viewport (no GPU shift):
snap_2d_transforms_mode = Canvas Space   (CanvasItem.SNAP_2D_TRANSFORMS_MODE_CANVAS)

# Always GPU screen snap on both axes — no movement detection (mode 2 only):
snap_2d_transforms_mode = Screen Space   (CanvasItem.SNAP_2D_TRANSFORMS_MODE_SCREEN)
```

Per-item priority: **Canvas Space > Screen Space > Inherit** (hybrid detection). Children with **Inherit** follow the resolved mode from the parent chain.

---

## 2. Patch files

### New file (must be added)

| File | Purpose |
|------|---------|
| `servers/rendering/renderer_snap_2d.h` | Helpers `use_canvas_transform_snap` / `use_screen_transform_snap` and item mode enum |

### Modified files (core)

| Area | Files |
|------|-------|
| Scene API | `scene/main/viewport.h`, `scene/main/viewport.cpp`, `scene/main/canvas_item.h`, `scene/main/canvas_item.cpp` |
| Project settings | `scene/main/scene_tree.cpp`, `editor/editor_node.cpp` |
| RenderingServer | `servers/rendering/rendering_server.h`, `.cpp`, `rendering_server_default.h` |
| Viewport renderer | `servers/rendering/renderer_viewport.h`, `renderer_viewport.cpp` |
| Canvas cull | `servers/rendering/renderer_canvas_cull.h`, `renderer_canvas_cull.cpp` |
| Canvas render API | `servers/rendering/renderer_canvas_render.h` |
| RD backend | `servers/rendering/renderer_rd/renderer_canvas_render_rd.h`, `.cpp` |
| GLES3 backend | `drivers/gles3/rasterizer_canvas_gles3.h`, `.cpp` |
| Dummy backend | `servers/rendering/dummy/rasterizer_canvas_dummy.h` |
| RD shaders | `servers/rendering/renderer_rd/shaders/canvas.glsl`, `canvas_uniforms_inc.glsl` |
| GLES3 shaders | `drivers/gles3/shaders/canvas.glsl`, `canvas_uniforms_inc.glsl` |
| Scene offsets | `scene/2d/sprite_2d.cpp`, `scene/2d/animated_sprite_2d.cpp`, `scene/gui/rich_text_label.cpp` |
| Particles | `scene/2d/cpu_particles_2d.cpp`, `scene/2d/gpu_particles_2d.cpp` |
| Parallax (canvas space only) | `scene/2d/parallax_2d.cpp` — offset workaround only for canvas space snap |
| Docs | `doc/classes/Viewport.xml`, `doc/classes/ProjectSettings.xml`, `doc/classes/CanvasItem.xml` |

### Generated shaders (if present in the repo)

- `servers/rendering/renderer_rd/shaders/canvas.glsl.gen.h`
- `servers/rendering/renderer_rd/shaders/canvas_uniforms_inc.glsl.gen.h`
- `drivers/gles3/shaders/canvas.glsl.gen.h`

> If upstream regenerates `.gen.h` during the build, edit the source **`.glsl`** files, then rebuild or sync `.gen.h` manually.

---

## 3. Core logic (do not break during conflicts)

### 3.1. `RendererSnap2D` (`renderer_snap_2d.h`)

```cpp
enum TransformSnapMethod : uint8_t {
    TRANSFORM_SNAP_CANVAS = 0,
    TRANSFORM_SNAP_SCREEN = 1,
    TRANSFORM_SNAP_SCREEN_MOVING = 2,
};

enum Snap2DTransformsItemMode : uint8_t {
    SNAP_2D_TRANSFORMS_ITEM_INHERIT = 0,
    SNAP_2D_TRANSFORMS_ITEM_CANVAS = 1,
    SNAP_2D_TRANSFORMS_ITEM_SCREEN = 2,
};

// Movement detection (mode 2 only) — compared in world/canvas space per render frame
static constexpr float SCREEN_TRANSFORM_SNAP_MOVING_EPSILON = 0.01f;
static constexpr float SCREEN_TRANSFORM_SNAP_MOVING_JITTER_EPSILON = 0.02f;

use_canvas_transform_snap(enabled, method)           // method == CANVAS only
use_screen_transform_snap(enabled, method)           // method == SCREEN or SCREEN_MOVING (shader path enabled)
use_screen_transform_snap_moving(enabled, method)    // method == SCREEN_MOVING only (hybrid CPU/GPU cull)
```

**Important:** The viewport method is read from the **viewport** (`snap_2d_transforms_method`), not from `GLOBAL_GET` at runtime.

### 3.2. Canvas space path (viewport method == Canvas Space, or per-item override)

**`renderer_canvas_cull.cpp`** — per-item origin rounding:

```cpp
if (uses_canvas_transform_snap) {
    self_xform.columns[2] = (self_xform.columns[2] + Point2(0.5, 0.5)).floor();
    parent_xform.columns[2] = (parent_xform.columns[2] + Point2(0.5, 0.5)).floor();
}
```

Resolved per item via `_item_uses_canvas_transform_snap()`:
- Viewport default: canvas space when `use_canvas_transform_snap(...)`.
- Item override: `snap_2d_transforms_mode == CANVAS`, or inherited from parent chain.

Forced screen space via `_item_uses_forced_screen_transform_snap()` (mode `2` only):
- Item override: `snap_2d_transforms_mode == SCREEN`, or inherited from parent chain.
- Sets both GPU flags always; skips movement detection. Canvas override on the same item wins.

**`renderer_viewport.cpp`** — `_canvas_get_transform()`: `pixel_snap_offset` + `ceil` on viewport/canvas transform. Only when `use_canvas_transform_snap(...)`.

### 3.3. Screen space path (shader) — critical order

**Wrong** (causes jitter): snap origin **before** `canvas_transform`, or snap in `model_matrix` space only.

**Correct** (current patch — mode `1` always both axes; mode `2` per-axis):

```glsl
vertex = (model_matrix * vec4(vertex, 0.0, 1.0)).xy;
vertex = (canvas_data.canvas_transform * vec4(vertex, 0.0, 1.0)).xy;

// Screen space transform snap — origin only, AFTER canvas_transform
if (bool(canvas_data.flags & CANVAS_FLAGS_USE_TRANSFORM_PIXEL_SNAP)
        && !bool(read_draw_data_flags & INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP)) {
    vec2 transform_origin = (canvas_data.canvas_transform * model_matrix * vec4(0.0, 0.0, 0.0, 1.0)).xy;
    vec2 snapped_origin = transform_origin;
    // Mode 1: both flags always set by cull (_finalize_screen_transform_snap_axes).
    // Mode 2: only axes flagged as "moving" get GPU snap; static axes rely on CPU canvas snap.
    if (bool(read_draw_data_flags & INSTANCE_FLAGS_SCREEN_TRANSFORM_SNAP_X)) {
        snapped_origin.x = floor(transform_origin.x + 0.5);
    }
    if (bool(read_draw_data_flags & INSTANCE_FLAGS_SCREEN_TRANSFORM_SNAP_Y)) {
        snapped_origin.y = floor(transform_origin.y + 0.5);
    }
    vertex += snapped_origin - transform_origin;
    uv += 1e-5;
}

// Separate: snap_2d_vertices_to_pixel (upstream) — floor each corner; do NOT merge with screen space transform snap
if (canvas_data.use_pixel_snap) {
    vertex = floor(vertex + 0.5);
    uv += 1e-5;
}
```

**RD:** flag `CANVAS_FLAGS_USE_TRANSFORM_PIXEL_SNAP (1 << 1)` in `canvas_data.flags`.  
**GLES3:** same meaning via `state_buffer.pad1 = 1` (without changing UBO layout).

**RD instance flags:**

| Flag | Bit | Meaning |
|------|-----|---------|
| `INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP` | 29 | Per-item canvas override — no GPU snap |
| `INSTANCE_FLAGS_SCREEN_TRANSFORM_SNAP_X` | 30 | GPU screen snap on X (mode `2` moving axis) |
| `INSTANCE_FLAGS_SCREEN_TRANSFORM_SNAP_Y` | 31 | GPU screen snap on Y (mode `2` moving axis) |

Mode `1` (Screen Space): `_finalize_screen_transform_snap_axes` sets both X/Y flags unless skipped.  
Mode `2` (Screen Moving): flags come from cull (`screen_transform_snap_x/y` on `Item`).

**Per-item opt-out of screen space snap:** `INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP (1 << 29)` when `Item::skip_screen_transform_snap` is set (canvas space override on a screen space viewport).

### 3.4. Screen Space When Moving — hybrid cull (`renderer_canvas_cull.cpp`)

**Design goal:** combine canvas sharpness for world-static objects with screen snap for world-moving objects, independently per X/Y axis.

#### World-space movement detection

Movement is detected from **world/canvas origin**, not local `self` origin:

```cpp
world_origin = inverse(current_camera_transform) * unsnapped_final_transform.origin;
delta = world_origin - screen_transform_snap_world_origin_prev;  // per item, per render frame
```

Why world space:
- Autoscroll: tilemap fixed in world → stable origin → **canvas snap**, even though it moves on screen with the camera.
- Player on moving platform: world origin changes → **GPU snap**.
- Camera child with fixed local offset: world origin stable → canvas; use **`inherit_gpu`** from moving parent instead.

Detection runs on the **unsnapped** final transform (before CPU canvas snap).

Constants (`renderer_snap_2d.h`):

| Constant | Value | Role |
|----------|-------|------|
| `SCREEN_TRANSFORM_SNAP_MOVING_EPSILON` | `0.01` | Axis counts as moving if `\|delta\| >= epsilon` |
| `SCREEN_TRANSFORM_SNAP_MOVING_JITTER_EPSILON` | `0.02` | Secondary-axis physics jitter threshold |

**Physics jitter suppression:** when **both** axes exceed `epsilon`, disable GPU on any axis whose `\|delta\|` is below `JITTER_EPSILON` (e.g. CharacterBody Y oscillating ~0.017 px while walking horizontally — do not enable GPU Y). There is no separate dominance-ratio pass; jitter threshold alone handles the common case.

#### GPU vs canvas resolution (per axis)

```cpp
gpu_snap_x = inherit_gpu_snap_x || detected_moving_x;
gpu_snap_y = inherit_gpu_snap_y || detected_moving_y;
// Default is canvas snap when neither inherit nor detected moving on that axis.
```

**Inheritance:** only `inherit_gpu_snap_x/y` is passed to children (when parent uses GPU on that axis). There is **no** `inherit_canvas` — static children decide canvas/GPU from their own world movement.

#### CPU canvas snap (static axes)

When `gpu_snap_x && gpu_snap_y` → no CPU snap (GPU handles both axes in shader).

When fully static (`!gpu_snap_x && !gpu_snap_y`) → **identical to mode `0`**:

```cpp
self_xform.columns[2] = (self_xform.columns[2] + Point2(0.5, 0.5)).floor();
snapped_parent_xform.columns[2] = (snapped_parent_xform.columns[2] + Point2(0.5, 0.5)).floor();
final_xform = snapped_parent_xform * self_xform;
```

When mixed (one axis GPU, one canvas) → per-axis floor on `self` + `snapped_parent` via `_apply_axis_canvas_transform_snap`.

Y-sort path (no self/parent split): `_apply_render_origin_axis_canvas_snap` on combined `final_xform` for static axes.

#### Dual parent transform chains + draw transform (critical)

`_cull_canvas_item` takes **two** parent transforms plus a separate **draw** transform for attaching items:

```cpp
void _cull_canvas_item(...,
    const Transform2D &p_snapped_parent_xform,
    const Transform2D &p_unsnapped_parent_xform,
    ...,
    bool p_parent_uses_canvas_transform_snap,
    bool p_parent_uses_forced_screen_transform_snap,
    bool p_inherit_gpu_snap_x,
    bool p_inherit_gpu_snap_y, ...);
```

Root call: both parent chains start as the camera/canvas transform; forced-screen parent flag is `false`.

Per item (mode `2`, non-canvas-override):

```cpp
unsnapped_final = p_unsnapped_parent * self;
// detect + resolve gpu flags on unsnapped_final (skipped when forced screen)
// CPU canvas snap → final_xform (hybrid final with per-level static-axis rounding)

// Children:
child_snapped_parent   = final_xform;          // ALWAYS — cumulative canvas snap on static axes
child_unsnapped_parent = unsnapped_final;      // movement detection only

// Draw attach (NOT final_xform directly in mixed-axis cases):
draw_xform = _build_screen_transform_snap_draw_xform(
    final_xform, unsnapped_final, gpu_snap_x, gpu_snap_y);
```

**Why two chains:** a **static ancestor** (e.g. Level at fractional X) must pass **cumulative canvas-snapped** transforms to static descendants (tilemap). The **unsnapped** chain is kept separately so movement detection is not poisoned by CPU canvas snap. GPU siblings still compose from unsnapped for moving axes at draw time.

**Why `draw_xform` ≠ `final_xform` in mixed-axis mode:** passing only `unsnapped_final` to children loses per-level canvas snap on static axes (e.g. Path2D parent at X = -1888.4). Passing only `final_xform` to draw quantizes the moving axis in CPU space before the GPU shader runs. `_build_screen_transform_snap_draw_xform` fixes both:

```cpp
// Both GPU → unsnapped (shader snaps both axes)
// Both static → final (CPU already snapped)
// Mixed → unsnapped chain, but copy static-axis origin from hybrid final:
draw_origin.x = gpu_snap_x ? unsnapped.origin.x : final.origin.x;
draw_origin.y = gpu_snap_y ? unsnapped.origin.y : final.origin.y;
```

`_attach_canvas_item_for_draw` and `global_rect` use **`draw_xform`**, not `final_xform`.

**Forced screen** (`snap_2d_transforms_mode == Screen Space`, mode `2` only): both GPU flags always; `final_xform = unsnapped_final`; `draw_xform = unsnapped_final`; `child_inherit_gpu_snap_x/y = true`; no dual-chain special case for unsnapped parent (both chains equal unsnapped).

#### `Item` fields (`renderer_canvas_render.h`)

```cpp
bool screen_transform_snap_x = false;              // GPU output → instance flag X
bool screen_transform_snap_y = false;              // GPU output → instance flag Y
bool screen_transform_snap_world_origin_valid = false;
Point2 screen_transform_snap_world_origin_prev;    // detection only (world space)
```

#### Cull helpers (`renderer_canvas_cull.h`)

| Function | Role |
|----------|------|
| `_detect_screen_transform_snap_axes` | World-origin delta → `moving_x/y`; jitter suppression |
| `_resolve_screen_transform_snap_axes` | `gpu = inherit \|\| moving` per axis |
| `_apply_screen_transform_snap_moving` | Detect + resolve; writes `screen_transform_snap_x/y` |
| `_apply_hybrid_canvas_transform_snap` | CPU canvas snap for static axes |
| `_apply_axis_canvas_transform_snap` | Per-axis self + parent floor |
| `_apply_render_origin_axis_canvas_snap` | Y-sort fallback on combined final |
| `_get_screen_transform_snap_world_origin` | `inverse(camera) * final.origin` |
| `_build_screen_transform_snap_draw_xform` | Mixed-axis draw transform (unsnapped + static origin from final) |
| `_item_uses_canvas_transform_snap` | Per-item canvas override + inherit |
| `_item_uses_forced_screen_transform_snap` | Per-item forced GPU both axes + inherit (mode `2`) |
| `_finalize_screen_transform_snap_axes` | Mode `1`: force both GPU flags; mode `2`: leave cull flags |

#### Viewport cull flags

```cpp
_viewport_uses_canvas_transform_snap      // mode == 0
_viewport_uses_screen_transform_snap     // mode == 1 or 2 (shader enabled)
_viewport_uses_screen_transform_snap_moving  // mode == 2 only (hybrid cull)
```

### 3.5. Parameter plumbing

`canvas_render_items` receives an additional argument:

```cpp
bool p_snap_2d_transforms_to_pixel,
bool p_snap_2d_vertices_to_pixel,
uint8_t p_snap_2d_transforms_method,  // 0 = Canvas, 1 = Screen, 2 = Screen When Moving
```

Call chain:

```
RendererViewport (draw)
  → RendererCanvasCull::render_canvas(..., method)
    → _render_canvas_item_tree(..., method)
      → canvas_render_items(..., method)
        → state: flags / pad1 for screen space snap
        → per-item: skip_screen_transform_snap → INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP
        → per-item (mode 2): screen_transform_snap_x/y → INSTANCE_FLAGS_SCREEN_TRANSFORM_SNAP_X/Y
```

When forward-porting, search for: `snap_2d_transforms_method`, `render_canvas`, `canvas_render_items`, `skip_screen_transform_snap`, `screen_transform_snap`, `_viewport_uses_screen_transform_snap_moving`, `child_snapped_parent_xform`, `draw_xform`, `_build_screen_transform_snap_draw_xform`, `_item_uses_forced_screen_transform_snap`.

### 3.6. Viewport / Scene

**`viewport.h`:**

- Enum `Snap2DTransformsMethod`: `SNAP_2D_TRANSFORMS_METHOD_CANVAS` (0), `SNAP_2D_TRANSFORMS_METHOD_SCREEN` (1), `SNAP_2D_TRANSFORMS_METHOD_SCREEN_MOVING` (2).
- `VARIANT_ENUM_CAST(Viewport::Snap2DTransformsMethod);` at end of file (**required**, otherwise C2027 on `BIND_ENUM_CONSTANT`).
- `set/get_snap_2d_transforms_method`, `is_snap_2d_transforms_to_pixel_canvas_enabled()`, `is_snap_2d_transforms_to_pixel_screen_enabled()`.

**`canvas_item.h`:**

- Enum `Snap2DTransformsMode`: `SNAP_2D_TRANSFORMS_MODE_INHERIT` (0), `SNAP_2D_TRANSFORMS_MODE_CANVAS` (1), `SNAP_2D_TRANSFORMS_MODE_SCREEN` (2).
- `VARIANT_ENUM_CAST(CanvasItem::Snap2DTransformsMode);`
- `set/get_snap_2d_transforms_mode`, `is_snap_2d_transforms_canvas_space_in_tree()`.
- Property `snap_2d_transforms_mode` — **Inherit** / **Canvas Space** / **Screen Space**; propagates to children with Inherit.

**`RenderingServer`:**

- `viewport_set_snap_2d_transforms_method(RID, int)`
- `canvas_item_set_snap_2d_transforms_mode(RID, int)`

**Parallax2D:** offset workaround — only `is_snap_2d_transforms_to_pixel_canvas_enabled()`, **not** for screen space.

**Sprite2D / AnimatedSprite2D / RichTextLabel:** `(offset + 0.5).floor()` when `is_snap_2d_transforms_to_pixel_enabled()`.

**CPUParticles2D / GPUParticles2D:** mesh vertex offset `(dest_offset + 0.5).floor()` when screen space snap is active and the item does **not** use canvas space in tree.

---

## 4. Forward-port workflow

### Step 1 — Preparation

```bash
git fetch origin
git checkout -b pixel-snap-screen origin/4.x   # or target stable tag
git log --oneline -1                            # record target version
```

Save the patch from the base version:

```bash
git format-patch <base-commit>..<patch-tip> -o patches/pixel-snap-screen/
# or
git diff <upstream-stable>..<patch-branch> > patches/pixel-snap-screen.patch
```

### Step 2 — Cherry-pick / apply

```bash
git cherry-pick <commit>    # preferred if there is a dedicated commit
# or
git apply --3way patches/pixel-snap-screen.patch
```

### Step 3 — Conflict resolution

Use **section 5** below. After each block, run `grep` on key symbols (section 6).

### Step 4 — Integrity check

```bash
rg "snap_2d_transforms_method|CANVAS_FLAGS_USE_TRANSFORM_PIXEL_SNAP|RendererSnap2D|skip_screen_transform_snap" --type-add 'godot:*.{cpp,h,glsl}' -t godot
rg "canvas_render_items\(" servers/rendering drivers/gles3 -A1
```

Ensure **all** `canvas_render_items` implementations share the same signature (RD, GLES3, dummy).

### Step 5 — Functional checklist

- [ ] SubViewport: `snap_2d_transforms_to_pixel = on`, `method = Screen Space`, `vertices = off`
- [ ] SubViewport hybrid: `method = Screen Space When Moving` — static tilemap sharp at fractional positions during autoscroll; moving player/platform use GPU snap
- [ ] No ±1 px jitter: player on moving platform + camera follow + physics interpolation
- [ ] Horizontal walk on sloped/smooth camera rise: no vertical jitter from physics Y oscillation (~0.017 px) falsely enabling GPU Y
- [ ] Sprites are not stretched (unlike `vertices = on`)
- [ ] Centered Sprite2D with odd dimensions — no extra blur (offset snap)
- [ ] Particles — sharp textures in screen space mode (mesh offset)
- [ ] Canvas Space mode (`method = Canvas Space`) — matches upstream canvas space behavior (regression)
- [ ] Screen Space mode (`method = Screen Space`) — always GPU snap both axes (regression)
- [ ] Per-item `snap_2d_transforms_mode = Canvas Space` on screen space viewport — no screen space shader shift
- [ ] Per-item `snap_2d_transforms_mode = Screen Space` on mode `2` viewport — always GPU both axes without movement detection; children with Inherit follow
- [ ] Mode `2` mixed-axis: object moving on one axis only gets GPU on that axis; static axis keeps canvas snap (Path2D at fractional X + moving child)
- [ ] Mode `2` per-axis: movement on X only does not enable GPU Y (and vice versa)
- [ ] Parallax2D unchanged in screen space mode (workaround inactive)
- [ ] GPU sibling (player) not affected by static ancestor canvas-snapping the camera into parent chain
- [ ] `_attach_canvas_item_for_draw` uses `draw_xform`, not raw `final_xform`, in mixed-axis hybrid

---

## 5. Conflict resolution by area

### 5.1. `renderer_viewport.cpp` — `_canvas_get_transform`

Upstream often changes camera/viewport rounding.

| On conflict | Action |
|-------------|--------|
| Upstream added new snap/round logic | Wrap the **entire** block in `if (RendererSnap2D::use_canvas_transform_snap(...))` |
| Upstream renamed variables | Keep upstream names; keep canvas-space-only guard |
| Upstream removed viewport snap | Do **not** restore it for screen space; for canvas space — port patch logic |

**Do not:** apply canvas space viewport rounding when `method == Screen Space` — jitter returns.

### 5.2. `renderer_canvas_cull.cpp`

| On conflict | Action |
|-------------|--------|
| Physics interpolation block changed | Rounding stays **after** interpolation; per-item via `uses_canvas_transform_snap` |
| Y-sort / transform compose changed | Preserve `_item_uses_canvas_transform_snap`, `_item_uses_forced_screen_transform_snap`, `skip_screen_transform_snap`, dual parent xforms, **`draw_xform`** |
| New `render_canvas` parameter | Add `p_snap_2d_transforms_method` and pass it through |
| `_cull_canvas_item` signature changed | Restore **two** parent transforms + **`p_parent_uses_forced_screen_transform_snap`**; restore `_build_screen_transform_snap_draw_xform` for draw attach |
| Movement detection touched | Preserve world-origin detection, jitter suppression (epsilon + jitter only), `inherit_gpu` only (no `inherit_canvas`) |

### 5.3. `canvas_render_items` signature

Typical conflict: upstream added a parameter in the middle of the list.

**Rule:** `p_snap_2d_transforms_method` goes after `p_snap_2d_vertices_to_pixel`, before `r_sdf_used`. Update **all** overrides: RD, GLES3, dummy, virtual in `renderer_canvas_render.h`.

### 5.4. Shaders (`canvas.glsl`)

| On conflict | Action |
|-------------|--------|
| Upstream changed vertex transform order | Place screen space snap **after** `canvas_transform`, **before** `use_pixel_snap` |
| Upstream changed per-instance flags layout | Preserve bits 29–31: SKIP, SNAP_X, SNAP_Y |
| Upstream changed `canvas_data` UBO | RD: use a bit in `flags`; do not add std140 fields without alignment |
| GLES3: `CanvasData` changed | Screen space flag via `pad1`; do not break `StateBuffer` sizeof |
| Upstream rewrote backend | Find canvas item vertex shader equivalent; reproduce the same operation order |

**Do not:**

- Move screen space transform snap **before** `canvas_transform` (jitter returns).
- Merge screen space transform snap with `use_pixel_snap` / `snap_2d_vertices_to_pixel`.
- Forget `INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP` guard for per-item canvas space override.
- Forget per-axis `INSTANCE_FLAGS_SCREEN_TRANSFORM_SNAP_X/Y` for mode `2`.

### 5.5. `viewport.cpp` / bindings

On conflict in `_bind_methods`:

1. Restore `BIND_ENUM_CONSTANT(SNAP_2D_TRANSFORMS_METHOD_CANVAS/SCREEN/SCREEN_MOVING)`.
2. Verify `VARIANT_ENUM_CAST(Viewport::Snap2DTransformsMethod)` in `viewport.h`.
3. `ADD_PROPERTY` for `snap_2d_transforms_method` next to `snap_2d_transforms_to_pixel`.

**`canvas_item.cpp`:**

1. `BIND_ENUM_CONSTANT(SNAP_2D_TRANSFORMS_MODE_INHERIT/CANVAS/SCREEN/MAX)`.
2. `VARIANT_ENUM_CAST(CanvasItem::Snap2DTransformsMode)` in `canvas_item.h`.

### 5.6. Documentation XML

Conflicts in `doc/classes/*.xml` — merge upstream text with new members:

- `Viewport.snap_2d_transforms_method` + enum constants `SNAP_2D_TRANSFORMS_METHOD_CANVAS` / `SCREEN` / `SCREEN_MOVING`
- `CanvasItem.snap_2d_transforms_mode` + enum constants `SNAP_2D_TRANSFORMS_MODE_*`
- `ProjectSettings.rendering/2d/snap/snap_2d_transforms_method`

Property key names: `0` = canvas, `1` = screen, `2` = screen when moving.

---

## 6. Grep anchors after upstream refactors

```bash
rg "snap_2d_transforms_to_pixel" servers/rendering scene/main
rg "_canvas_get_transform" servers/rendering/renderer_viewport.cpp
rg "use_canvas_transform_snap|use_screen_transform_snap|use_screen_transform_snap_moving" servers/rendering
rg "_item_uses_canvas_transform_snap|_item_uses_forced_screen_transform_snap|skip_screen_transform_snap|screen_transform_snap" servers/rendering
rg "_detect_screen_transform_snap|_build_screen_transform_snap_draw_xform|_apply_hybrid_canvas|child_snapped_parent|draw_xform" servers/rendering
rg "use_pixel_snap" servers/rendering/renderer_rd drivers/gles3
rg "INSTANCE_FLAGS_SCREEN_TRANSFORM_SNAP" servers/rendering/renderer_rd/shaders drivers/gles3/shaders
rg "INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP|INSTANCE_FLAGS_SCREEN_TRANSFORM_SNAP" servers/rendering drivers/gles3
rg "canvas_render_items" servers/rendering drivers/gles3
rg "VARIANT_ENUM_CAST\(Viewport::" scene/main/viewport.h
rg "VARIANT_ENUM_CAST\(CanvasItem::" scene/main/canvas_item.h
rg "snap_2d_transforms_mode" scene/main
```

If upstream renamed `RendererCanvasRenderRD` or split files, search for `final_transform`, `canvas_transform_inverse`, `CANVAS_FLAGS_CONVERT_ATTRIBUTES_TO_LINEAR`.

---

## 7. Common merge mistakes

1. **Screen space snap before camera** — jitter returns.
2. **Canvas space snap not guarded by method / per-item resolution** — double snap or jitter.
3. **Forgot dummy/GLES3** after virtual signature change — link error.
4. **Missing `VARIANT_ENUM_CAST`** — `error C2027` on `BIND_ENUM_CONSTANT`.
5. **Merged transform snap and vertex snap into one flag** — stretch or blur.
6. **Sprite offset snap missing** — blur on centered sprites in screen space mode.
7. **Parallax workaround enabled for screen space** — unwanted artifacts.
8. **Edited `.glsl` only, forgot `.gen.h`** — CI/build mismatch without regen.
9. **Missing `INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP`** — per-item canvas space override has no effect.
10. **Particle mesh not adjusted for screen space** — blurred particle textures.
11. **Mode `2`: single parent chain only** — static descendants lose canvas snap, or GPU siblings get camera-snapped parent (Mario jitter on platform).
12. **Mode `2`: `child_snapped_parent = unsnapped` when any GPU axis** — loses cumulative per-level canvas snap on static axes for descendants (fractional Path2D parent).
13. **Mode `2`: draw attach uses `final_xform` instead of `draw_xform`** — mixed-axis moving axis quantized in CPU before GPU shader; per-axis snap broken.
14. **Mode `2`: movement detected in local space instead of world** — autoscroll platform sprites miss GPU; tilemap false-moving.
15. **Mode `2`: `inherit_canvas` reintroduced** — static parent forces canvas on all descendants including moving children.
16. **Mode `2`: world snap on final instead of self+parent canvas snap** — static objects blurred (camera subpixel not handled like mode `0`).
17. **Mode `2`: physics Y jitter (~0.017) enables GPU Y during horizontal walk** — vertical jitter with rising camera; jitter epsilon must stay ≥ typical floor oscillation.
18. **Mode `2`: `_finalize_screen_transform_snap_axes` overwrites per-axis flags** — must leave flags from cull when `use_screen_transform_snap_moving`.
19. **Mode `2`: child GPU inherit uses `\|\|` instead of per-axis assignment from parent flags** — GPU snap only when both axes move simultaneously.

---

## 8. Minimal manual recovery (if patch does not apply)

If automerge fails completely, restore in this order:

1. Add `servers/rendering/renderer_snap_2d.h`
2. Viewport enum + RS API + `RendererViewport::snap_2d_transforms_method`
3. CanvasItem enum + `canvas_item_set_snap_2d_transforms_mode`
4. Guard canvas space snap (cull + viewport) with per-item resolution
5. Extend `canvas_render_items` + pass `method`
6. RD: `CANVAS_FLAGS_USE_TRANSFORM_PIXEL_SNAP` + shader block + skip + per-axis snap flags
7. GLES3: `pad1` + shader block + skip + per-axis snap flags
8. Mode `2`: hybrid cull (world detection, dual parent chains, **`draw_xform`**, `inherit_gpu`, CPU canvas snap helpers, per-item forced screen)
9. Scene: viewport/canvas_item properties, scene_tree/editor_node init, sprite/particle offsets
10. Docs + enum constants in XML

---

## 9. Upstream context

| Mode | Behavior |
|------|----------|
| **Canvas Space** (default, int `0`) | Rounds transform origins in canvas space on the CPU, including viewport transform |
| **Screen Space** (int `1`) | Always rounds transform origins in screen space on the GPU after `canvas_transform` (both axes) |
| **Screen Space When Moving** (int `2`) | Hybrid per axis: world-static → CPU canvas snap (mode `0` equivalent); world-moving → GPU screen snap; dual parent chains + **`draw_xform`** during cull |
| **Per-item Canvas Space** | Forces canvas space rounding for a subtree; disables screen space shader shift via `INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP` |
| **Per-item Screen Space** | Forces GPU screen snap on both axes without movement detection (mode `2` only); children with Inherit follow |

### Mode `2` reference scenario (autoscroll platformer)

Typical scene (MF Community Edition):

```
Level (Node2D, static in world)
├── Mario / enemies (move in world → GPU snap)
├── PathFollow2D + Camera2D + platform (move in world → GPU snap, inherit_gpu to children)
└── TileMapLayer (fixed world coords → canvas snap, sharp vs fractional tile positions)
```

Movement detection uses **render frames** (not physics ticks): `screen_transform_snap_world_origin_prev` updates each viewport draw.

This patch does **not** replace upstream `snap_2d_vertices_to_pixel` — that remains a separate option.

---

## 10. Suggested commit / PR message

```
Add screen space modes for 2D transform pixel snapping

Introduce snap_2d_transforms_method (Canvas Space / Screen Space /
Screen Space When Moving) on Viewport and RenderingServer. Screen Space
rounds transform origins in the canvas shader after the camera transform.
Screen Space When Moving hybridizes CPU canvas snap for world-static
objects with per-axis GPU screen snap for world-moving objects, using
world-origin detection, dual parent transform chains, draw_xform for
mixed-axis attach, and inherit_gpu. Add CanvasItem.snap_2d_transforms_mode
for per-item canvas/screen space overrides (Inherit / Canvas Space /
Screen Space).
```

---

*This document applies to the fork/patch based on Godot 4.7. Update the **Patch base version** section after a successful forward-port.*
