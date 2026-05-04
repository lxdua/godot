# Proposal: `VirtualBoxContainer` — virtualized counterpart of `BoxContainer` / `GridContainer` for `PackedScene` cells

**Describe the project you are working on:**

A general-purpose 2D game engine feature, but specifically motivated by game UIs that display large, homogeneous or semi-homogeneous collections of scripted cells: inventories, chat logs, leaderboards, shop catalogs, asset browsers, dialog history, notification feeds, photo grids, etc. In each of these cases, a single "cell" is naturally expressed as a `PackedScene` (icon + labels + buttons + animation + child controls), not as a cell drawn by the container itself.

**Describe the problem or limitation you are having in your project:**

Today, as soon as a UI cell is implemented as a custom `PackedScene` rather than plain text/icon, **there is no virtualization path in Godot**. Any scene containing such cells must sit inside a `VBoxContainer` / `HBoxContainer` / `GridContainer`, and the user is forced to instantiate one full node tree per data entry. With ~100 entries this is already noticeable; with thousands (MMO inventories, chat backlog, catalogs, patch notes, leaderboards) it is unusable.

The built-in virtualized controls are locked to their own self-drawn cells:

| Control | Virtualized | Accepts `PackedScene` as cell | Notes |
|---|---|---|---|
| `ItemList` | Yes (self-drawn) | No | Restricted to icon + text. |
| `Tree` | Yes (self-drawn) | No | Data-tree oriented, not a general cell container. |
| `BoxContainer` (V/H) / `GridContainer` / `FlowContainer` + `ScrollContainer` | **No** | Yes | The only `PackedScene`-friendly path, but creates every node up front. |

The missing piece is specifically **the virtualized counterpart of the `BoxContainer` / `GridContainer` family** — the containers users already reach for when arranging custom cells. Framing this as "a new list widget" obscures the point; it is really "what `VBoxContainer` should be for 1000+ items".

As a result every non-trivial Godot project reinvents a virtualized container:

- Third-party `VirtualList` / `RecyclerScrollContainer` / `EnhancedScroller`-style add-ons appear regularly on the Asset Library; coverage is inconsistent (variable height, focus, selection, async binding, keyboard navigation, multi-type cells, insertion animation). Most implementations silently ship subtle bugs around scroll anchoring, focus loss on recycling, or index/animation desync on insert/remove.
- Every other major UI framework ships this out of the box: Unreal `UListView` / `UTileView` / `UTreeView`, Unity UI Toolkit `ListView`, Flutter `ListView.builder`, Android `RecyclerView`, iOS `UITableView` / `UICollectionView`, React `react-window`. Godot is the odd one out.

**Describe the feature / enhancement and how it helps to overcome the problem or limitation:**

Add a new core `Container` class **`VirtualBoxContainer`** (plus a thin derived `VirtualGridContainer`) that:

1. Is positioned as the **virtualized counterpart of `BoxContainer` / `GridContainer`** — same mental model, same naming conventions (`vertical` property, `separation` theme item), same role in the editor's "Add Node" dialog.
2. Takes one or more `PackedScene` templates as cell factories.
3. Takes only a total `item_count` plus a user-provided `bind_item(node, index)` signal handler — the container itself does not own the data.
4. Instantiates only the cells intersecting the visible viewport (plus a small buffer).
5. On scroll, **recycles** off-screen cell nodes back into a pool keyed by cell type, and re-binds them to incoming indices, so the node count stays O(viewport), not O(item_count).
6. Exposes an explicit notification API (`refresh`, `refresh_item`, `notify_item_inserted`, `notify_item_removed`, `notify_items_moved`) that mirrors `RecyclerView.Adapter` — a battle-tested model — so the container can preserve scroll anchoring and know which indices to re-bind.

This is a **performance primitive**, not a convenience wrapper: the savings scale with data size (for a 10k-item inventory: ~a dozen live nodes vs ~10k), and correct implementation is non-trivial enough that user-space rewrites are consistently buggy.

**When NOT to use it.** The proposal explicitly keeps `BoxContainer` / `GridContainer` in place and documents a clear decision rule:

| Scenario | Use | Rationale |
|---|---|---|
| < 100 homogeneous cells | `BoxContainer` / `GridContainer` | Zero migration cost; virtualization bookkeeping is net negative. |
| 100 ~ 1000 homogeneous cells | Either (measure) | Depends on cell complexity. |
| 1000+ homogeneous cells | **`VirtualBoxContainer`** | Node count stays constant with viewport size. |
| Heterogeneous structural UI (settings, HUD, dialogs) | `BoxContainer` family | Virtualization requires data-driven cells. |
| Children referenced via `$Container/ChildX` | `BoxContainer` family | Virtualized nodes may be recycled at any time. |
| Children with external lifecycle (drag-and-drop source) | `BoxContainer` family | Virtualized container owns node lifecycle. |

The core rule: **can each cell be reconstructed from data + template?** If yes, consider `VirtualBoxContainer`; if no, keep `BoxContainer`.

**Describe how your proposal will work, with code, pseudo-code, mock-ups, and/or diagrams:**

### API sketch

```gdscript
class_name VirtualBoxContainer extends Container

# --- Templates ---
@export var item_scene: PackedScene                       # single template
@export var item_scenes: Array[PackedScene]               # multiple templates (mixed cells)
var type_provider: Callable                               # (index:int) -> int, picks a template

# --- Data extent ---
@export var item_count: int = 0

# --- Layout (mirrors BoxContainer.vertical) ---
@export var vertical: bool = true
enum SizeMode { FIXED_SIZE, VARIABLE_SIZE }
enum ScrollAlign { ALIGN_VISIBLE, ALIGN_START, ALIGN_CENTER, ALIGN_END }
@export var size_mode: SizeMode = FIXED_SIZE
@export var item_min_size: Vector2 = Vector2(0, 48)
@export var buffer_items: int = 2
@export var pool_size_limit: int = 32

# --- Data-change notifications (explicit, RecyclerView-style) ---
func refresh() -> void
func refresh_item(index: int) -> void
func notify_item_inserted(index: int) -> void
func notify_item_removed(index: int) -> void
func notify_items_moved(from: int, to: int) -> void

# --- Queries ---
func get_bound_index(node: Control) -> int     # -1 if not currently bound
func get_node_at_index(index: int) -> Control  # null if not in viewport
func is_item_visible(index: int) -> bool
func get_first_visible_index() -> int
func get_last_visible_index() -> int

# --- Scrolling ---
func scroll_to_index(index: int, align: ScrollAlign = ALIGN_VISIBLE) -> void

# --- Signals ---
signal bind_item(node: Control, index: int)            # fill node from data[index]
signal unbind_item(node: Control, index: int)          # commit dirty state back to data[index]
signal item_activated(index: int)                      # click / Enter on the cell
signal item_action(index: int, action: StringName)     # bubbled from inside the cell
signal visible_range_changed(first: int, last: int)


class_name VirtualGridContainer extends VirtualBoxContainer

@export var columns: int = 1                              # mirrors GridContainer.columns
```

Note on naming: no separate `VirtualVBoxContainer` / `VirtualHBoxContainer` shell classes are introduced. `BoxContainer` itself is Godot's V/H-unified base; its V/H presets are thin compatibility shells. The new class follows the unified `BoxContainer` model directly, exposing `vertical` as a property. Shell presets can be added later backwards-compatibly if community demand appears.

### Migration from `VBoxContainer`

The migration from a naive VBox-with-hand-instantiated-cells to `VirtualBoxContainer` is mostly mechanical:

```diff
- @onready var list: VBoxContainer = $Inventory
+ @onready var list: VirtualBoxContainer = $Inventory

  func refresh():
-     for c in list.get_children():
-         c.queue_free()
-     for item in inventory:
-         var n = SLOT.instantiate()
-         n.set_data(item)
-         list.add_child(n)
+     list.item_count = inventory.size()

+ func _ready():
+     list.item_scene = SLOT
+     list.bind_item.connect(func(n, i): n.set_data(inventory[i]))
```

### Typical inventory usage (vertical)

```gdscript
@onready var list: VirtualBoxContainer = $Inventory
var inventory: Array[ItemData] = []

func _ready() -> void:
    list.item_scene = preload("res://ui/inventory_slot.tscn")
    list.vertical = true
    list.item_min_size = Vector2(0, 64)
    list.bind_item.connect(_on_bind)
    list.item_activated.connect(_on_use)
    inventory = load_inventory()
    list.item_count = inventory.size()

func _on_bind(node: Control, index: int) -> void:
    node.set_data(inventory[index])

func _on_use(index: int) -> void:
    player.use_item(inventory[index])
    inventory.remove_at(index)
    list.notify_item_removed(index)          # preserves scroll, re-binds affected cells
```

### Chat / heterogeneous cells

```gdscript
list.item_scenes = [TEXT_MSG, IMAGE_MSG, SYSTEM_MSG]
list.type_provider = func(i): return messages[i].kind

messages.append(new_msg)
list.notify_item_inserted(messages.size() - 1)
list.scroll_to_index(messages.size() - 1, VirtualBoxContainer.ALIGN_END)
```

Internally, one recycle pool per type is maintained — a cell node is only ever re-bound to an index of its own type; otherwise a new instance is created (up to `pool_size_limit`).

### Photo grid (`VirtualGridContainer`)

```gdscript
@onready var grid: VirtualGridContainer = $Album
grid.item_scene = THUMBNAIL
grid.columns = 4
grid.item_min_size = Vector2(160, 160)
grid.bind_item.connect(func(node, i): node.load_thumb(photos[i]))
grid.item_count = photos.size()
```

### Recycling flow

```
                 enters viewport
  (index) ──────────────────────────▶ acquire(type):
                                        pool.pop()  or  item_scenes[type].instantiate()
                                      │
                                      ▼
                              emit bind_item(node, index)      ← user fills node
                                      │
                                      ▼
                              position_node(node, index)
                                      │
                  leaves viewport     │
                      ─────────────────
                                      ▼
                              emit unbind_item(node, index)    ← user writes dirty state back
                                      │
                                      ▼
                              remove_child → pool  (or queue_free if pool full)
```

### Edge cases the API deliberately addresses

| Pitfall | API answer |
|---|---|
| Per-cell widget state (e.g. a `LineEdit` inside the cell) survives recycling | `unbind_item` is the documented commit point — push state back to data there, read it back in `bind_item`. |
| Async binding races (image request finishes after scroll moves on) | `get_bound_index(node)` lets the callback confirm the node is still bound to the original index. |
| Buttons inside a cell emitting signals | Cell script emits an `action(name: StringName)` signal; the container auto-forwards it as `item_action(index, name)`. The user never has to track individual cell nodes. |
| Focus on a cell that gets recycled | Container catches it and moves focus to itself; keyboard navigation re-scrolls and re-binds as needed. |
| Insertion / deletion during scroll | `notify_item_inserted` / `notify_item_removed` remap active indices in place, preserving scroll anchor so the visible item stays visible. |
| Misuse on small lists | §1.5-style decision table printed at the top of the class's docs; virtualization bookkeeping is documented as a net negative below ~100 items. |

### Scope of the initial implementation (kept deliberately small)

- Phase 1: `vertical = true`, fixed-size cells, single template. Core recycling, scrolling, `notify_*`.
- Phase 2: multi-template via `type_provider`, selection / focus model, `item_action` bubbling.
- Phase 3: `vertical = false` and `VirtualGridContainer` (derived class with `columns`).
- Phase 4: variable-size cells via measurement cache + estimated extents (same approach as `react-virtualized` and Flutter `ListView.builder`).

A `VirtualFlowContainer` is deliberately **not** part of this proposal: virtualizing flow layout requires per-item measurement with prefix-sum bookkeeping and is considerably more involved than the box/grid cases. It can be a follow-up.

**If this enhancement will not be used often, can it be worked around with a few lines of script?**

No — this is the opposite of a "few lines of script" helper. A correct implementation is several hundred lines and needs to handle:

- Pool-per-type recycling with lifecycle signals.
- Scroll-anchor-preserving insert / remove / move.
- Focus transfer and keyboard navigation across recycling boundaries.
- Selection state that outlives recycled nodes.
- Async binding safety (`get_bound_index`).
- Variable item sizes with progressive measurement (phase 4).

Evidence that it is used often: every major UI framework ships it, and multiple half-implementations keep appearing on the Godot Asset Library because each team ends up needing one. `ItemList` and `Tree` already implement this pattern internally — the proposal is simply to expose the pattern for arbitrary `PackedScene` cells, positioned as the virtualized counterpart of the `BoxContainer` / `GridContainer` users already know.

**Is there a reason why this should be core and not an add-on in the asset library?**

1. **Correctness is hard and the existing add-ons prove it.** There is no single Asset Library implementation that covers variable height + multi-type + insert-anchored scrolling + focus + async-safe binding at engine quality. Fragmentation has produced a long tail of subtly broken versions rather than one good one.
2. **It is a missing building block, not a feature.** Once virtualized cells exist in core, a lot of downstream UI patterns (sectioned lists, infinite feeds, virtualized trees, paging) become straightforward. Keeping it in user space keeps blocking those.
3. **Godot already embeds the pattern internally.** `ItemList`, `Tree`, the editor's file-system dock and inspector all implement bespoke virtualization. Promoting one shared, `PackedScene`-compatible mechanism into core reduces total engine complexity in the long run.
4. **It is a missing member of an existing built-in family.** `VBoxContainer` / `HBoxContainer` / `GridContainer` are in core; their virtualized counterpart logically belongs in core beside them, with matching discovery in the editor's "Add Node" dialog and matching naming in API docs.
5. **Integration with editor, theming, focus, accessibility, and i18n** is much easier when the control lives alongside the other `Control` subclasses and participates in the same notification / theming pipeline.
6. **Aligns Godot with every peer framework.** Unreal, Unity UI Toolkit, Flutter, Android, iOS, and the web all provide this as a first-class engine/framework primitive. Godot having no equivalent is a concrete gap, not a philosophical choice.
