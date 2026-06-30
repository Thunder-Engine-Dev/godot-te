# Forward-port: Screen Space 2D transform pixel snap (Godot 4.7+)

Guide for developers and agents when forward-porting this patch to newer Godot versions and resolving merge conflicts.

**Patch base version:** Godot 4.7 / 4.7.1-rc  
**Patch goal:** A **Screen Space** mode for `snap_2d_transforms_to_pixel` — no jitter with physics interpolation and scrolling cameras, without corner stretching (unlike `snap_2d_vertices_to_pixel`).

---

## 1. Patch overview

### Upstream problem (canvas space snap)

- Canvas space rounding on every node in the hierarchy plus a separate viewport/camera snap (`floor` / `ceil`) causes ±1 px jitter with physics interpolation, scrolling cameras, and moving platforms.
- `snap_2d_vertices_to_pixel` removes jitter but snaps **every corner**, which can distort sprite dimensions.

### Patch solution

1. New enum **`snap_2d_transforms_method`**: **Canvas Space** (upstream behavior, int `0`) / **Screen Space** (this patch, int `1`).
2. In **Screen Space** mode:
   - Canvas space rounding in `renderer_canvas_cull.cpp` and `renderer_viewport.cpp` is **disabled** (unless overridden per item).
   - Transform origins are rounded in the shader **after** `canvas_transform` (camera already applied).
   - Only the **origin** is rounded, not each corner — quad size is preserved.
3. Per-viewport setting on `Viewport` / `SubViewport` (not only a project setting).
4. Draw offset correction in `Sprite2D` / `AnimatedSprite2D` / `RichTextLabel` when transform snap is enabled.
5. Per-item override on `CanvasItem`: **`snap_2d_transforms_mode`** (`Inherit` / **Canvas Space**) to opt into canvas space rounding while the viewport uses screen space snapping.
6. Particle quad mesh offset correction when screen space snap is active (centered quads).

### Recommended configuration (SubViewport)

```
snap_2d_transforms_to_pixel = true
snap_2d_transforms_method = Screen Space   (Viewport.SNAP_2D_TRANSFORMS_METHOD_SCREEN)
snap_2d_vertices_to_pixel = false
```

Optional per-item opt-in to canvas space rounding (legacy-style behavior for specific nodes):

```
snap_2d_transforms_mode = Canvas Space   (CanvasItem.SNAP_2D_TRANSFORMS_MODE_CANVAS)
```

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
};

enum Snap2DTransformsItemMode : uint8_t {
    SNAP_2D_TRANSFORMS_ITEM_INHERIT = 0,
    SNAP_2D_TRANSFORMS_ITEM_CANVAS = 1,
};

use_canvas_transform_snap(enabled, method)  // Canvas space path (upstream)
use_screen_transform_snap(enabled, method)  // Screen space path (this patch)
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

**`renderer_viewport.cpp`** — `_canvas_get_transform()`: `pixel_snap_offset` + `ceil` on viewport/canvas transform. Only when `use_canvas_transform_snap(...)`.

### 3.3. Screen space path (shader) — critical order

**Wrong** (causes jitter): snap origin **before** `canvas_transform`, or snap in `model_matrix` space only.

**Correct** (current patch):

```glsl
vertex = (model_matrix * vec4(vertex, 0.0, 1.0)).xy;
vertex = (canvas_data.canvas_transform * vec4(vertex, 0.0, 1.0)).xy;

// Screen space transform snap — origin only, AFTER canvas_transform
if (bool(canvas_data.flags & CANVAS_FLAGS_USE_TRANSFORM_PIXEL_SNAP)
        && !bool(read_draw_data_flags & INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP)) {
    vec2 transform_origin = (canvas_data.canvas_transform * model_matrix * vec4(0.0, 0.0, 0.0, 1.0)).xy;
    vec2 snapped_origin = floor(transform_origin + vec2(0.5));
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

**Per-item opt-out of screen space snap:** `INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP (1 << 29)` on instance flags when `Item::skip_screen_transform_snap` is set (canvas space override on a screen space viewport).

### 3.4. Parameter plumbing

`canvas_render_items` receives an additional argument:

```cpp
bool p_snap_2d_transforms_to_pixel,
bool p_snap_2d_vertices_to_pixel,
uint8_t p_snap_2d_transforms_method,  // NEW — int values unchanged: 0 = Canvas, 1 = Screen
```

Call chain:

```
RendererViewport (draw)
  → RendererCanvasCull::render_canvas(..., method)
    → _render_canvas_item_tree(..., method)
      → canvas_render_items(..., method)
        → state: flags / pad1 for screen space snap
        → per-item: skip_screen_transform_snap → INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP
```

When forward-porting, search for: `snap_2d_transforms_method`, `render_canvas`, `canvas_render_items`, `skip_screen_transform_snap`.

### 3.5. Viewport / Scene

**`viewport.h`:**

- Enum `Snap2DTransformsMethod`: `SNAP_2D_TRANSFORMS_METHOD_CANVAS` (0), `SNAP_2D_TRANSFORMS_METHOD_SCREEN` (1).
- `VARIANT_ENUM_CAST(Viewport::Snap2DTransformsMethod);` at end of file (**required**, otherwise C2027 on `BIND_ENUM_CONSTANT`).
- `set/get_snap_2d_transforms_method`, `is_snap_2d_transforms_to_pixel_canvas_enabled()`, `is_snap_2d_transforms_to_pixel_screen_enabled()`.

**`canvas_item.h`:**

- Enum `Snap2DTransformsMode`: `SNAP_2D_TRANSFORMS_MODE_INHERIT` (0), `SNAP_2D_TRANSFORMS_MODE_CANVAS` (1).
- `VARIANT_ENUM_CAST(CanvasItem::Snap2DTransformsMode);`
- `set/get_snap_2d_transforms_mode`, `is_snap_2d_transforms_canvas_space_in_tree()`.
- Property `snap_2d_transforms_mode` — **Inherit** / **Canvas Space**; propagates to children with Inherit.

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
- [ ] No ±1 px jitter: player on moving platform + camera follow + physics interpolation
- [ ] Sprites are not stretched (unlike `vertices = on`)
- [ ] Centered Sprite2D with odd dimensions — no extra blur (offset snap)
- [ ] Particles — sharp textures in screen space mode (mesh offset)
- [ ] Canvas Space mode (`method = Canvas Space`) — matches upstream canvas space behavior (regression)
- [ ] Per-item `snap_2d_transforms_mode = Canvas Space` on screen space viewport — no screen space shader shift
- [ ] Parallax2D unchanged in screen space mode (workaround inactive)

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
| Y-sort / transform compose changed | Preserve `_item_uses_canvas_transform_snap` and `skip_screen_transform_snap` |
| New `render_canvas` parameter | Add `p_snap_2d_transforms_method` and pass it through |

### 5.3. `canvas_render_items` signature

Typical conflict: upstream added a parameter in the middle of the list.

**Rule:** `p_snap_2d_transforms_method` goes after `p_snap_2d_vertices_to_pixel`, before `r_sdf_used`. Update **all** overrides: RD, GLES3, dummy, virtual in `renderer_canvas_render.h`.

### 5.4. Shaders (`canvas.glsl`)

| On conflict | Action |
|-------------|--------|
| Upstream changed vertex transform order | Place screen space snap **after** `canvas_transform`, **before** `use_pixel_snap` |
| Upstream changed `canvas_data` UBO | RD: use a bit in `flags`; do not add std140 fields without alignment |
| GLES3: `CanvasData` changed | Screen space flag via `pad1`; do not break `StateBuffer` sizeof |
| Upstream rewrote backend | Find canvas item vertex shader equivalent; reproduce the same operation order |

**Do not:**

- Move screen space transform snap **before** `canvas_transform` (jitter returns).
- Merge screen space transform snap with `use_pixel_snap` / `snap_2d_vertices_to_pixel`.
- Forget `INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP` guard for per-item canvas space override.

### 5.5. `viewport.cpp` / bindings

On conflict in `_bind_methods`:

1. Restore `BIND_ENUM_CONSTANT(SNAP_2D_TRANSFORMS_METHOD_CANVAS/SCREEN)`.
2. Verify `VARIANT_ENUM_CAST(Viewport::Snap2DTransformsMethod)` in `viewport.h`.
3. `ADD_PROPERTY` for `snap_2d_transforms_method` next to `snap_2d_transforms_to_pixel`.

**`canvas_item.cpp`:**

1. `BIND_ENUM_CONSTANT(SNAP_2D_TRANSFORMS_MODE_INHERIT/CANVAS/MAX)`.
2. `VARIANT_ENUM_CAST(CanvasItem::Snap2DTransformsMode)` in `canvas_item.h`.

### 5.6. Documentation XML

Conflicts in `doc/classes/*.xml` — merge upstream text with new members:

- `Viewport.snap_2d_transforms_method` + enum constants `SNAP_2D_TRANSFORMS_METHOD_CANVAS` / `SCREEN`
- `CanvasItem.snap_2d_transforms_mode` + enum constants `SNAP_2D_TRANSFORMS_MODE_*`
- `ProjectSettings.rendering/2d/snap/snap_2d_transforms_method`

Property key names and int values are unchanged (`0` = canvas, `1` = screen).

---

## 6. Grep anchors after upstream refactors

```bash
rg "snap_2d_transforms_to_pixel" servers/rendering scene/main
rg "_canvas_get_transform" servers/rendering/renderer_viewport.cpp
rg "use_canvas_transform_snap|use_screen_transform_snap" servers/rendering
rg "_item_uses_canvas_transform_snap|skip_screen_transform_snap" servers/rendering
rg "use_pixel_snap" servers/rendering/renderer_rd drivers/gles3
rg "floor\(transform_origin \+ vec2\(0\.5\)\)" servers/rendering/renderer_rd/shaders drivers/gles3/shaders
rg "INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP" servers/rendering drivers/gles3
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

---

## 8. Minimal manual recovery (if patch does not apply)

If automerge fails completely, restore in this order:

1. Add `servers/rendering/renderer_snap_2d.h`
2. Viewport enum + RS API + `RendererViewport::snap_2d_transforms_method`
3. CanvasItem enum + `canvas_item_set_snap_2d_transforms_mode`
4. Guard canvas space snap (cull + viewport) with per-item resolution
5. Extend `canvas_render_items` + pass `method`
6. RD: `CANVAS_FLAGS_USE_TRANSFORM_PIXEL_SNAP` + shader block + skip flag
7. GLES3: `pad1` + shader block + skip flag
8. Scene: viewport/canvas_item properties, scene_tree/editor_node init, sprite/particle offsets
9. Docs + enum constants in XML

---

## 9. Upstream context

| Mode | Behavior |
|------|----------|
| **Canvas Space** (default, int `0`) | Rounds transform origins in canvas space on the CPU, including viewport transform |
| **Screen Space** (int `1`, this patch) | Rounds transform origins in screen space on the GPU after `canvas_transform` |
| **Per-item Canvas Space** | Forces canvas space rounding for a subtree; disables screen space shader shift via `INSTANCE_FLAGS_SKIP_SCREEN_TRANSFORM_SNAP` |

This patch does **not** replace upstream `snap_2d_vertices_to_pixel` — that remains a separate option.

---

## 10. Suggested commit / PR message

```
Add screen space mode for 2D transform pixel snapping

Introduce snap_2d_transforms_method (Canvas Space / Screen Space) on
Viewport and RenderingServer. Screen Space rounds transform origins in
the canvas shader after the camera transform, avoiding canvas space
rounding jitter with physics interpolation while preserving sprite
dimensions. Add CanvasItem.snap_2d_transforms_mode for per-item canvas
space opt-in.
```

---

*This document applies to the fork/patch based on Godot 4.7. Update the **Patch base version** section after a successful forward-port.*
