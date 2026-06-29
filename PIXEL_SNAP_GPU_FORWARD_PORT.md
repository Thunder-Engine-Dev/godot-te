# Forward-port: GPU 2D transform pixel snap (Godot 4.7+)

Инструкция для агента/разработчика при переносе патча на новые версии Godot и разрешении merge-конфликтов.

**Базовая версия патча:** Godot 4.7 / 4.7.1-rc  
**Цель патча:** режим `GPU` для `snap_2d_transforms_to_pixel` — без jitter (как Godot 3.6), без растяжения углов (в отличие от `snap_2d_vertices_to_pixel`).

---

## 1. Краткое описание патча

### Проблема upstream (4.4+ CPU snap)

- CPU snap на каждом узле иерархии + отдельный snap viewport/camera (`floor` / `ceil`) → ±1 px jitter при physics interpolation + scrolling camera + moving platform.
- `snap_2d_vertices_to_pixel` убирает jitter, но snap'ит **каждый угол** → иногда ломает длину спрайта.

### Решение патча

1. Новый enum **`snap_2d_transforms_method`**: `CPU` (поведение upstream) / `GPU` (новое).
2. В режиме **GPU**:
   - CPU snap в `renderer_canvas_cull.cpp` и `renderer_viewport.cpp` **отключён**.
   - Snap origin в шейдере **после** `canvas_transform` (уже с учётом камеры).
   - Snap **только origin**, не каждый corner → размер quad сохраняется.
3. Per-viewport настройка на `Viewport` / `SubViewport` (не только project setting).
4. Коррекция draw offset в `Sprite2D` / `AnimatedSprite2D` при любом transform snap (не только CPU).

### Рекомендуемая конфигурация (SubViewport)

```
snap_2d_transforms_to_pixel = true
snap_2d_transforms_method = GPU   (Viewport.SNAP_2D_TRANSFORMS_METHOD_GPU)
snap_2d_vertices_to_pixel = false
```

---

## 2. Файлы патча

### Новый файл (обязательно добавить)

| Файл | Назначение |
|------|------------|
| `servers/rendering/renderer_snap_2d.h` | Хелпер `use_cpu_transform_snap` / `use_gpu_transform_snap` |

### Изменённые файлы (ядро)

| Область | Файлы |
|---------|-------|
| Scene API | `scene/main/viewport.h`, `scene/main/viewport.cpp` |
| Project settings | `scene/main/scene_tree.cpp`, `editor/editor_node.cpp` |
| RenderingServer | `servers/rendering/rendering_server.h`, `.cpp`, `rendering_server_default.h` |
| Viewport renderer | `servers/rendering/renderer_viewport.h`, `renderer_viewport.cpp` |
| Canvas cull | `servers/rendering/renderer_canvas_cull.h`, `renderer_canvas_cull.cpp` |
| Canvas render API | `servers/rendering/renderer_canvas_render.h` |
| RD backend | `servers/rendering/renderer_rd/renderer_canvas_render_rd.h`, `.cpp` |
| GLES3 backend | `drivers/gles3/rasterizer_canvas_gles3.h`, `.cpp` |
| Dummy backend | `servers/rendering/dummy/rasterizer_canvas_dummy.h` |
| RD shaders | `servers/rendering/renderer_rd/shaders/canvas.glsl`, `canvas_uniforms_inc.glsl` |
| GLES3 shaders | `drivers/gles3/shaders/canvas.glsl` |
| Scene offsets | `scene/2d/sprite_2d.cpp`, `scene/2d/animated_sprite_2d.cpp`, `scene/gui/rich_text_label.cpp` |
| Parallax (CPU only) | `scene/2d/parallax_2d.cpp` — workaround только для CPU snap |
| Docs | `doc/classes/Viewport.xml`, `doc/classes/ProjectSettings.xml` |

### Сгенерированные шейдеры (если есть в репозитории)

- `servers/rendering/renderer_rd/shaders/canvas.glsl.gen.h`
- `servers/rendering/renderer_rd/shaders/canvas_uniforms_inc.glsl.gen.h`
- `drivers/gles3/shaders/canvas.glsl.gen.h`

> Если upstream перегенерирует `.gen.h` при сборке — править **исходные** `.glsl`, затем пересобрать или синхронизировать `.gen.h` вручную.

---

## 3. Ключевая логика (не ломать при конфликтах)

### 3.1. `RendererSnap2D` (`renderer_snap_2d.h`)

```cpp
enum TransformSnapMethod : uint8_t { TRANSFORM_SNAP_CPU = 0, TRANSFORM_SNAP_GPU = 1 };

use_cpu_transform_snap(enabled, method)  // CPU path upstream 4.4+
use_gpu_transform_snap(enabled, method)  // GPU path патча
```

**Важно:** метод берётся из **viewport** (`snap_2d_transforms_method`), не из `GLOBAL_GET` в runtime.

### 3.2. CPU path (только `method == CPU`)

**`renderer_canvas_cull.cpp`** — snap origin узлов:

```cpp
if (snapping_2d_transforms_to_pixel) {
    self_xform.columns[2] = (self_xform.columns[2] + Point2(0.5, 0.5)).floor();
    parent_xform.columns[2] = (parent_xform.columns[2] + Point2(0.5, 0.5)).floor();
}
```

Устанавливать: `snapping_2d_transforms_to_pixel = RendererSnap2D::use_cpu_transform_snap(...)`.

**`renderer_viewport.cpp`** — `_canvas_get_transform()`: `pixel_snap_offset` + `ceil` на viewport/canvas transform (PR #93786 upstream). Только при `use_cpu_transform_snap`.

### 3.3. GPU path (шейдер) — критичный порядок

**Неверно** (вызывает jitter): snap origin **до** `canvas_transform` или snap в `model_matrix` space.

**Верно** (текущий патч):

```glsl
vertex = (model_matrix * vec4(vertex, 0.0, 1.0)).xy;
vertex = (canvas_transform * vec4(vertex, 0.0, 1.0)).xy;

// GPU transform snap — только origin, ПОСЛЕ canvas_transform
if (transform_snap_flag) {
    vec2 transform_origin = (canvas_transform * model_matrix * vec4(0.0, 0.0, 0.0, 1.0)).xy;
    vec2 snapped_origin = floor(transform_origin + vec2(0.5));
    vertex += snapped_origin - transform_origin;
    uv += 1e-5;
}

// Отдельно: snap_2d_vertices_to_pixel (upstream) — floor каждого corner, НЕ смешивать с GPU transform snap
if (use_pixel_snap) {
    vertex = floor(vertex + 0.5);
    uv += 1e-5;
}
```

**RD:** флаг `CANVAS_FLAGS_USE_TRANSFORM_PIXEL_SNAP (1 << 1)` в `canvas_data.flags`.  
**GLES3:** тот же смысл через `state_buffer.pad1 = 1` (без изменения UBO layout).

### 3.4. Проброс параметров по цепочке

Сигнатура `canvas_render_items` получает **дополнительные** аргументы:

```cpp
bool p_snap_2d_transforms_to_pixel,
bool p_snap_2d_vertices_to_pixel,
uint8_t p_snap_2d_transforms_method,  // NEW
```

Цепочка вызовов:

```
RendererViewport (draw)
  → RendererCanvasCull::render_canvas(..., method)
    → _render_canvas_item_tree(..., method)
      → canvas_render_items(..., method)
        → state: flags / pad1 для GPU snap
```

При forward-port искать по строкам: `snap_2d_transforms_method`, `render_canvas`, `canvas_render_items`.

### 3.5. Viewport / Scene

**`viewport.h`:**

- Enum `Snap2DTransformsMethod` + `VARIANT_ENUM_CAST(Viewport::Snap2DTransformsMethod);` в конце файла (**обязательно**, иначе C2027 при `BIND_ENUM_CONSTANT`).
- Поле `snap_2d_transforms_method`.
- `set/get_snap_2d_transforms_method`, `is_snap_2d_transforms_to_pixel_cpu_enabled()`.

**`RenderingServer`:**

- `viewport_set_snap_2d_transforms_method(RID, int)`.

**Parallax2D:** offset workaround — только `is_snap_2d_transforms_to_pixel_cpu_enabled()`, **не** для GPU.

**Sprite2D / AnimatedSprite2D:** `(offset + 0.5).floor()` при `is_snap_2d_transforms_to_pixel_enabled()` (**CPU и GPU**).

---

## 4. Workflow forward-port на новую версию Godot

### Шаг 1 — подготовка

```bash
git fetch origin
git checkout -b pixel-snap-gpu origin/4.x   # или нужный stable tag
git log --oneline -1                          # зафиксировать целевую версию
```

Сохранить патч с базовой версии:

```bash
git format-patch <base-commit>..<patch-tip> -o patches/pixel-snap-gpu/
# или
git diff <upstream-stable>..<patch-branch> > patches/pixel-snap-gpu.patch
```

### Шаг 2 — cherry-pick / apply

```bash
git cherry-pick <commit>    # предпочтительно, если есть отдельный commit
# или
git apply --3way patches/pixel-snap-gpu.patch
```

### Шаг 3 — разрешение конфликтов

Использовать **секцию 5** ниже. После каждого блока — `grep` по ключевым символам (секция 6).

### Шаг 4 — проверка целостности

```bash
rg "snap_2d_transforms_method|CANVAS_FLAGS_USE_TRANSFORM_PIXEL_SNAP|RendererSnap2D" --type-add 'godot:*.{cpp,h,glsl}' -t godot
rg "canvas_render_items\(" servers/rendering drivers/gles3 -A1
```

Убедиться, что **все** реализации `canvas_render_items` имеют одинаковую сигнатуру (RD, GLES3, dummy).

### Шаг 5 — функциональный чеклист

- [ ] SubViewport: `transforms=on`, `method=GPU`, `vertices=off`
- [ ] Нет ±1 px jitter: игрок на движущейся платформе + camera follow + physics interpolation
- [ ] Спрайты не растягиваются (в отличие от `vertices=on`)
- [ ] Centered Sprite2D с нечётным размером — без лишнего blur (offset snap)
- [ ] CPU mode (`method=CPU`) — поведение как upstream 4.4+ (реgression)
- [ ] Parallax2D не ломается в GPU mode (workaround не активен)

---

## 5. Разрешение конфликтов по зонам

### 5.1. `renderer_viewport.cpp` — `_canvas_get_transform`

Upstream часто меняет camera/viewport rounding (#93786 и последующие fix).

| При конфликте | Действие |
|---------------|----------|
| Upstream добавил новый snap/round | Обернуть **весь** блок в `if (RendererSnap2D::use_cpu_transform_snap(...))` |
| Upstream переименовал переменные | Сохранить upstream имена, сохранить guard CPU-only |
| Upstream удалил viewport snap | **Не** восстанавливать для GPU; для CPU — перенести логику из патча |

**Нельзя:** применять CPU viewport snap при `method == GPU` — вернёт jitter.

### 5.2. `renderer_canvas_cull.cpp`

| При конфликте | Действие |
|---------------|----------|
| Изменён physics interpolation block | Snap остаётся **после** интерполяции, только если CPU mode |
| Изменён y-sort / transform compose | `snapping_2d_transforms_to_pixel` = `use_cpu_transform_snap(...)` |
| Новый параметр в `render_canvas` | Добавить `p_snap_2d_transforms_method` и пробросить дальше |

### 5.3. `canvas_render_items` signature

Типичный конфликт: upstream добавил параметр в середину списка.

**Правило:** `p_snap_2d_transforms_method` — после `p_snap_2d_vertices_to_pixel`, перед `r_sdf_used`. Обновить **все** override: RD, GLES3, dummy, virtual в `renderer_canvas_render.h`.

### 5.4. Шейдеры (`canvas.glsl`)

| При конфликте | Действие |
|---------------|----------|
| Upstream изменил vertex transform order | GPU snap block ставить **после** `canvas_transform`, **до** `use_pixel_snap` |
| Upstream изменил `canvas_data` UBO | RD: использовать бит в `flags`, **не** добавлять поля в std140 без расчёта alignment |
| GLES3: изменён `CanvasData` | GPU flag через `pad1`, не ломать sizeof `StateBuffer` |
| Upstream переписал на другой backend | Найти аналог vertex shader canvas item, воспроизвести тот же порядок операций |

**Нельзя:**

- Переносить GPU transform snap **до** `canvas_transform` (jitter вернётся).
- Объединять GPU transform snap с `use_pixel_snap` / `snap_2d_vertices_to_pixel`.

### 5.5. `viewport.cpp` / bindings

При конфликте в `_bind_methods`:

1. Восстановить `BIND_ENUM_CONSTANT(SNAP_2D_TRANSFORMS_METHOD_CPU/GPU)`.
2. Проверить `VARIANT_ENUM_CAST(Viewport::Snap2DTransformsMethod)` в `viewport.h`.
3. `ADD_PROPERTY` для `snap_2d_transforms_method` рядом с `snap_2d_transforms_to_pixel`.

### 5.6. Документация XML

Конфликты в `doc/classes/*.xml` — принять **обе** стороны: upstream текст + новые `<member>` для `snap_2d_transforms_method`.

---

## 6. Grep-якоря для поиска после refactor upstream

```bash
rg "snap_2d_transforms_to_pixel" servers/rendering scene/main
rg "_canvas_get_transform" servers/rendering/renderer_viewport.cpp
rg "snapping_2d_transforms_to_pixel" servers/rendering
rg "use_pixel_snap" servers/rendering/renderer_rd drivers/gles3
rg "floor\(transform_origin \+ vec2\(0\.5\)\)" servers/rendering/renderer_rd/shaders drivers/gles3/shaders
rg "canvas_render_items" servers/rendering drivers/gles3
rg "VARIANT_ENUM_CAST\(Viewport::" scene/main/viewport.h
```

Если upstream переименовал `RendererCanvasRenderRD` / split файлов — искать по `final_transform`, `canvas_transform_inverse`, `CANVAS_FLAGS_CONVERT_ATTRIBUTES_TO_LINEAR`.

---

## 7. Частые ошибки при мерже

1. **GPU snap до camera** — jitter возвращается.
2. **CPU snap не guarded by method** — двойной snap или jitter.
3. **Забыли обновить dummy/GLES3** после смены virtual signature — ошибка линковки.
4. **Нет `VARIANT_ENUM_CAST`** — `error C2027` на `BIND_ENUM_CONSTANT`.
5. **Смешали transform snap и vertex snap** в одном флаге — stretch или blur.
6. **Sprite offset snap только для CPU** — blur на centered sprites в GPU mode.
7. **Parallax workaround включён для GPU** — лишние артеfacts.
8. **Правили только `.glsl`, забыли `.gen.h`** — расхождение при CI/build без regen.

---

## 8. Минимальный diff для ручного восстановления (если patch не применяется)

Если автomerge полностью провалился, восстановить в порядке:

1. Добавить `servers/rendering/renderer_snap_2d.h`
2. Viewport enum + RS API + `RendererViewport::snap_2d_transforms_method`
3. Guard CPU snap (cull + viewport)
4. Расширить `canvas_render_items` + проброс `method`
5. RD: `CANVAS_FLAGS_USE_TRANSFORM_PIXEL_SNAP` + shader block
6. GLES3: `pad1` + shader block
7. Scene: viewport property, scene_tree/editor_node init, sprite offsets
8. Docs

---

## 9. Связь с upstream (для контекста агента)

| Версия | Поведение без патча |
|--------|---------------------|
| Godot 3.6 | Глобальный `use_gpu_pixel_snap`, snap в шейдере |
| Godot 4.0–4.3 | CPU `.floor()` без half-pixel offset |
| Godot 4.4+ | CPU `floor(+0.5)` + viewport `ceil` (#93786) |
| **Патч** | Per-viewport `CPU` / `GPU`; GPU = origin snap после `canvas_transform` |

Патч **не заменяет** upstream `snap_2d_vertices_to_pixel` — это отдельная опция.

---

## 10. Коммит / PR (рекомендация)

```
Add GPU mode for 2D transform pixel snapping

Introduce snap_2d_transforms_method (CPU/GPU) on Viewport and
RenderingServer. GPU mode snaps transform origin in the canvas
shader after the camera transform, avoiding CPU/viewport rounding
jitter with physics interpolation while preserving sprite dimensions.
```

---

*Документ относится к форку/патчу на базе Godot 4.7. Обновляйте секцию «Базовая версия» после успешного forward-port.*
