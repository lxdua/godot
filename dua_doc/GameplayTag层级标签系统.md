# GameplayTag 层级标签系统

> 为 Godot 引擎核心层新增层级化的 Gameplay Tag 系统，支持标签注册、层级匹配、容器查询和项目设置集成。灵感来自 Unreal Engine 的 GameplayTag 系统。

---

## 一、修改目的

### 原版问题

Godot 现有的标识系统存在明显局限：

- **Groups（组）**：扁平结构，无法表达层级关系。`is_in_group("Enemy")` 不能匹配 `Enemy.Undead.Skeleton`。
- **Meta（元数据）**：键值对，手动输入字符串，无统一管理，无编辑器下拉选择。
- **Layer/Mask（碰撞层）**：32 位 bitmask，数量极其有限，无法表达语义。

开发者在制作技能系统、AI 条件判断、伤害类型匹配、状态效果管理时，几乎每个项目都要从零造轮子。

### 改动目标

在 `core/` 核心层新增 **GameplayTag** 系统：

- 层级化标签（如 `Damage.Fire.DoT`），支持父子匹配
- 标签容器（`GameplayTagContainer`），支持 has_any / has_all / has_none 集合查询
- 全局标签管理器单例（`GameplayTagManager`），统一注册、验证、查询
- 与 ProjectSettings 集成，标签定义持久化到 `project.godot`
- 所有 API 完整暴露给 GDScript，支持 `@export`

---

## 二、改动文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `core/gameplay_tag/gameplay_tag.h` | **新建** | `GameplayTag` 类声明（RefCounted） |
| `core/gameplay_tag/gameplay_tag.cpp` | **新建** | `GameplayTag` 实现：匹配、层级查询 |
| `core/gameplay_tag/gameplay_tag_container.h` | **新建** | `GameplayTagContainer` 类声明（Resource） |
| `core/gameplay_tag/gameplay_tag_container.cpp` | **新建** | `GameplayTagContainer` 实现：增删查、集合运算 |
| `core/gameplay_tag/gameplay_tag_manager.h` | **新建** | `GameplayTagManager` 全局单例声明 |
| `core/gameplay_tag/gameplay_tag_manager.cpp` | **新建** | `GameplayTagManager` 实现：标签树、注册、验证 |
| `core/gameplay_tag/SCsub` | **新建** | SCons 构建脚本 |
| `core/SCsub` | **修改** | 添加 `SConscript("gameplay_tag/SCsub")` |
| `core/register_core_types.cpp` | **修改** | 注册三个类、创建/销毁单例、加载项目标签 |
| `dua_doc/GameplayTag层级标签系统.md` | **新建** | 本文件 |

---

## 三、类结构总览

```
GameplayTag (RefCounted)
│  单个标签，如 "Damage.Fire.DoT"
│  底层存储 StringName，比较极快
│
GameplayTagContainer (Resource)
│  标签容器，持有多个标签
│  自动缓存父标签，支持层级匹配
│  可序列化为 .tres 文件
│
GameplayTagManager (Object, 全局单例)
   标签树管理器
   注册/注销标签，维护层级树
   与 ProjectSettings 集成
```

---

## 四、GameplayTag 类

### 属性

| 属性名 | 类型 | 说明 |
|--------|------|------|
| `tag_name` | StringName | 标签的完整路径，如 `"Damage.Fire.DoT"` |

### 方法

| 方法 | 签名 | 说明 |
|------|------|------|
| `create` | `static create(tag_name: StringName) -> GameplayTag` | 静态工厂方法 |
| `get_tag_name` | `get_tag_name() -> StringName` | 获取完整标签名 |
| `matches_tag` | `matches_tag(other: GameplayTag) -> bool` | **层级匹配**。`"Damage.Fire.DoT".matches_tag("Damage.Fire")` → true |
| `matches_tag_exact` | `matches_tag_exact(other: GameplayTag) -> bool` | 精确匹配，不考虑层级 |
| `get_parent_tag` | `get_parent_tag() -> GameplayTag` | 获取父标签。`"Damage.Fire.DoT"` → `"Damage.Fire"` |
| `get_depth` | `get_depth() -> int` | 获取层级深度。`"Damage.Fire.DoT"` → 3 |
| `get_ancestor_tags` | `get_ancestor_tags() -> Array[GameplayTag]` | 获取所有祖先标签 |
| `is_child_of` | `is_child_of(parent: GameplayTag) -> bool` | 判断是否是某标签的后代 |
| `is_valid` | `is_valid() -> bool` | 标签名是否非空 |

### 层级匹配规则

```
对象拥有标签: "Damage.Fire.DoT"

matches_tag("Damage")           → ✅ true（DoT 是 Damage 的后代）
matches_tag("Damage.Fire")      → ✅ true（DoT 是 Damage.Fire 的后代）
matches_tag("Damage.Fire.DoT")  → ✅ true（精确匹配）
matches_tag("Damage.Ice")       → ❌ false（不在同一分支）
matches_tag("Status")           → ❌ false（完全不同的分支）

// 注意：父标签不匹配子标签
"Damage.Fire".matches_tag("Damage.Fire.DoT") → ❌ false
```

### matches_tag 性能实现

不使用字符串拼接，零堆分配：

```cpp
// 1. 先比较 StringName（整数比较，O(1)）
if (tag_name == other.tag_name) return true;

// 2. 长度检查：this 必须比 other 长
if (this_len <= other_len) return false;

// 3. 分隔符检查：this[other_len] 必须是 '.'
if (this_str[other_len] != '.') return false;

// 4. 前缀逐字符比较
for (i = 0..other_len) if (this[i] != other[i]) return false;
```

---

## 五、GameplayTagContainer 类

### 内部结构

```cpp
Vector<StringName> explicit_tags;       // 用户显式添加的标签（有序，用于序列化）
HashSet<StringName> explicit_tags_set;  // 快速查重和精确查询（O(1)）
HashSet<StringName> parent_tags_cache;  // 自动缓存的所有父标签（O(1) 层级匹配）
```

当 `explicit_tags` 变化时，自动重建 `parent_tags_cache`：

```
添加 "Damage.Fire.DoT" 后:
  explicit_tags = ["Damage.Fire.DoT"]
  parent_tags_cache = {"Damage.Fire", "Damage"}

has_tag_name("Damage.Fire")  → 查 parent_tags_cache → ✅ true
has_tag_name("Damage")       → 查 parent_tags_cache → ✅ true
has_tag_name("Status")       → 查两个 set 都没有    → ❌ false
```

### 属性（序列化）

| 属性名 | 类型 | 说明 |
|--------|------|------|
| `tag_names` | PackedStringArray | 所有标签名，用于 .tres 序列化和 `@export` |

### 方法 — 标签操作

| 方法 | 签名 | 说明 |
|------|------|------|
| `add_tag` | `add_tag(tag: GameplayTag)` | 添加标签（去重） |
| `add_tag_name` | `add_tag_name(tag_name: StringName)` | 按名称添加 |
| `remove_tag` | `remove_tag(tag: GameplayTag)` | 移除标签 |
| `remove_tag_name` | `remove_tag_name(tag_name: StringName)` | 按名称移除 |
| `clear` | `clear()` | 清空所有标签 |

### 方法 — 查询

| 方法 | 签名 | 说明 |
|------|------|------|
| `has_tag` | `has_tag(tag: GameplayTag) -> bool` | **层级匹配查询** |
| `has_tag_name` | `has_tag_name(tag_name: StringName) -> bool` | 按名称层级匹配 |
| `has_tag_exact` | `has_tag_exact(tag: GameplayTag) -> bool` | 精确匹配（不查父标签） |
| `has_tag_exact_name` | `has_tag_exact_name(tag_name: StringName) -> bool` | 按名称精确匹配 |
| `has_any` | `has_any(other: GameplayTagContainer) -> bool` | other 中**任意一个**标签匹配 |
| `has_all` | `has_all(other: GameplayTagContainer) -> bool` | other 中**全部**标签匹配 |
| `has_none` | `has_none(other: GameplayTagContainer) -> bool` | other 中**没有**标签匹配 |

### 方法 — 集合运算

| 方法 | 签名 | 说明 |
|------|------|------|
| `union_with` | `union_with(other) -> GameplayTagContainer` | 并集 |
| `intersection_with` | `intersection_with(other) -> GameplayTagContainer` | 交集 |
| `difference_with` | `difference_with(other) -> GameplayTagContainer` | 差集 |
| `append_tags` | `append_tags(other)` | 原地合并 |

### 方法 — 访问器

| 方法 | 签名 | 说明 |
|------|------|------|
| `get_tag_count` | `get_tag_count() -> int` | 标签数量 |
| `get_tags` | `get_tags() -> Array[GameplayTag]` | 获取所有标签对象 |
| `get_tag_names` | `get_tag_names() -> PackedStringArray` | 获取所有标签名 |
| `is_empty` | `is_empty() -> bool` | 是否为空 |
| `create_from_array` | `static create_from_array(names: PackedStringArray) -> GameplayTagContainer` | 静态工厂 |

---

## 六、GameplayTagManager 单例

### 访问方式

```gdscript
# GDScript 中直接使用全局单例
GameplayTagManager.register_tag("Damage.Fire.DoT")
```

### 内部结构 — 标签树

```
注册 "Enemy.Type.Undead", "Enemy.Type.Dragon", "Enemy.Rank.Boss" 后：

[Root]
└── Enemy           (自动创建的中间节点)
    ├── Type
    │   ├── Undead
    │   └── Dragon
    └── Rank
        └── Boss

tag_lookup: { "Enemy"->node, "Enemy.Type"->node, "Enemy.Type.Undead"->node, ... }
registered_tags: { "Enemy", "Enemy.Type", "Enemy.Type.Undead", "Enemy.Type.Dragon", "Enemy.Rank", "Enemy.Rank.Boss" }
```

注册一个叶子标签会自动创建所有中间节点。

### 方法 — 注册

| 方法 | 签名 | 说明 |
|------|------|------|
| `register_tag` | `register_tag(tag_name: StringName, comment: String = "")` | 注册标签（自动创建父标签） |
| `register_tags` | `register_tags(tag_names: PackedStringArray)` | 批量注册 |
| `unregister_tag` | `unregister_tag(tag_name: StringName) -> bool` | 注销标签（仅无子标签时可用） |

### 方法 — 查询

| 方法 | 签名 | 说明 |
|------|------|------|
| `is_tag_registered` | `is_tag_registered(tag_name: StringName) -> bool` | 是否已注册 |
| `get_tag` | `get_tag(tag_name: StringName) -> GameplayTag` | 获取标签对象 |
| `get_all_tag_names` | `get_all_tag_names() -> PackedStringArray` | 所有已注册标签名（排序） |
| `get_children_of` | `get_children_of(parent: StringName) -> PackedStringArray` | 直接子标签 |
| `get_descendants_of` | `get_descendants_of(parent: StringName) -> PackedStringArray` | 所有后代标签 |
| `get_tag_count` | `get_tag_count() -> int` | 已注册标签总数 |
| `get_tag_comment` | `get_tag_comment(tag_name: StringName) -> String` | 获取标签备注 |
| `set_tag_comment` | `set_tag_comment(tag_name: StringName, comment: String)` | 设置标签备注 |

### 方法 — 便捷

| 方法 | 签名 | 说明 |
|------|------|------|
| `request_tag` | `request_tag(tag_name: StringName) -> GameplayTag` | 获取标签，不存在则自动注册 |
| `request_tag_container` | `request_tag_container(names: PackedStringArray) -> GameplayTagContainer` | 创建容器，自动注册 |
| `is_valid_tag_name` | `static is_valid_tag_name(tag_name: StringName) -> bool` | 验证标签名格式 |

### 方法 — 生命周期

| 方法 | 签名 | 说明 |
|------|------|------|
| `load_project_tags` | `load_project_tags()` | 从 ProjectSettings 加载标签（启动时自动调用） |
| `save_project_tags` | `save_project_tags()` | 保存标签到 ProjectSettings |

### 信号

| 信号 | 参数 | 说明 |
|------|------|------|
| `tag_registered` | `tag_name: StringName` | 新标签注册时触发 |
| `tag_unregistered` | `tag_name: StringName` | 标签注销时触发 |

### 标签名格式规则

- 由 `.` 分隔的多个片段组成
- 每个片段只能包含：字母（a-z, A-Z）、数字（0-9）、下划线（_）
- 不能以 `.` 开头或结尾
- 不能有连续的 `..`

合法示例：`Damage.Fire`、`Status.Buff.SpeedUp`、`My_Tag.V2`

非法示例：`.Damage`、`Damage..Fire`、`Damage Fire`、`标签.火焰`

---

## 七、ProjectSettings 集成

标签列表持久化到 `project.godot` 的 `gameplay_tags/tag_list` 字段：

```ini
[gameplay_tags]
tag_list=PackedStringArray("Damage", "Damage.Fire", "Damage.Fire.DoT", "Status.Buff.SpeedUp")
```

- **启动时**：`register_core_settings()` 中自动调用 `GameplayTagManager.load_project_tags()`
- **编辑器中**：修改后调用 `GameplayTagManager.save_project_tags()` 持久化

---

## 八、GDScript 使用示例

### 基础标签操作

```gdscript
# 创建标签
var fire_tag = GameplayTag.create("Damage.Fire")
var dot_tag = GameplayTag.create("Damage.Fire.DoT")

# 层级匹配
dot_tag.matches_tag(fire_tag)  # true —— DoT 是 Fire 的后代
fire_tag.matches_tag(dot_tag)  # false —— 父不匹配子

# 获取父标签
var parent = dot_tag.get_parent_tag()
print(parent.get_tag_name())  # "Damage.Fire"
```

### 容器查询

```gdscript
# 创建容器
var tags = GameplayTagContainer.new()
tags.add_tag_name("Damage.Fire.DoT")
tags.add_tag_name("Status.Buff.SpeedUp")

# 层级查询
tags.has_tag_name("Damage.Fire")  # true（层级匹配）
tags.has_tag_name("Damage")       # true（层级匹配）
tags.has_tag_name("Damage.Ice")   # false

# 精确查询
tags.has_tag_exact_name("Damage.Fire")      # false
tags.has_tag_exact_name("Damage.Fire.DoT")  # true

# 集合查询
var required = GameplayTagContainer.create_from_array(PackedStringArray(["Damage", "Status.Buff"]))
tags.has_all(required)   # true —— 两个都匹配
tags.has_any(required)   # true
tags.has_none(required)  # false
```

### 使用 @export 在编辑器中配置

```gdscript
extends CharacterBody2D

# 在 Inspector 面板中可以编辑标签容器
@export var tags: GameplayTagContainer

func take_damage(damage_tags: GameplayTagContainer):
    # 火焰免疫
    if tags.has_tag_name("Status.Immune.Fire"):
        if damage_tags.has_tag_name("Damage.Fire"):
            print("免疫火焰伤害!")
            return

    # Boss 受到额外伤害
    if tags.has_tag_name("Enemy.Rank.Boss"):
        if damage_tags.has_tag_name("Damage.Holy"):
            print("Boss 受到圣光伤害加成!")
```

### 全局标签管理

```gdscript
# 注册标签（通常在项目启动时或编辑器中操作）
GameplayTagManager.register_tag("Damage.Fire.DoT", "持续火焰伤害")
GameplayTagManager.register_tag("Damage.Fire.Explosion", "火焰爆炸")

# 查询标签树
GameplayTagManager.get_children_of("Damage.Fire")
# → ["Damage.Fire.DoT", "Damage.Fire.Explosion"]

GameplayTagManager.get_descendants_of("Damage")
# → ["Damage.Fire", "Damage.Fire.DoT", "Damage.Fire.Explosion"]

# 监听标签变化
GameplayTagManager.tag_registered.connect(func(tag_name):
    print("新标签注册: ", tag_name)
)
```

### 技能系统示例

```gdscript
class_name Ability extends Resource

@export var ability_tags: GameplayTagContainer          # 技能自身标签
@export var required_tags: GameplayTagContainer         # 释放前置条件
@export var blocked_by_tags: GameplayTagContainer       # 被这些标签阻止
@export var apply_tags: GameplayTagContainer            # 释放后给目标添加的标签

func can_activate(caster_tags: GameplayTagContainer) -> bool:
    # 必须拥有所有前置标签
    if not caster_tags.has_all(required_tags):
        return false
    # 不能拥有任何阻止标签
    if not caster_tags.has_none(blocked_by_tags):
        return false
    return true
```

---

## 九、设计决策

### 为什么放在 core/ 而不是 modules/？

放在核心层可以：
- 给 Node 直接添加 `gameplay_tags` 属性
- SceneTree 支持按标签查询节点
- `@export` 在 Inspector 中原生显示标签选择器
- 编辑器无条件集成，不需要 `#ifdef` 条件编译
- 其他核心系统（物理、动画、AI）可以自然引用

### 为什么 GameplayTag 继承 RefCounted 而不是值类型？

GDScript 无法使用 C++ 值类型的自定义类。`RefCounted` 是 Godot 暴露轻量对象到脚本层的标准模式（类似 `InputEvent`）。对于高频场景，提供了 `has_tag_name(StringName)` 直通 API，避免创建 `GameplayTag` 对象。

### 为什么 GameplayTagContainer 继承 Resource？

- 可以保存为 `.tres` 文件，在多个场景间共享
- 支持 `@export`，在 Inspector 属性面板中可编辑
- `emit_changed()` 信号自动触发编辑器刷新和序列化

---

## 十、注意事项

1. **GameplayTagContainer 的 has_tag 是层级匹配**：如果容器有 `Damage.Fire.DoT`，查询 `Damage.Fire` 也会返回 true。如果只要精确匹配，使用 `has_tag_exact`。
2. **标签不需要预先注册也能使用**：`GameplayTagContainer` 可以添加任意标签名，不要求在 Manager 中注册。Manager 的注册主要用于编辑器的下拉选择和项目级管理。
3. **性能特征**：
   - `has_tag_name` / `has_tag_exact_name`：O(1)（HashSet 查找）
   - `add_tag_name`：O(n)（重建父标签缓存，n = 标签数量 × 平均深度）
   - `matches_tag`（GameplayTag 上）：O(m)（m = 标签名长度，逐字符比较）
4. **序列化**：GameplayTagContainer 通过 `tag_names` 属性（PackedStringArray）序列化，所有 `.tres` / `.tscn` 文件中存储的是字符串数组。

---

## 十一、Node 集成

每个 Node 都天然拥有 `gameplay_tags` 属性，在 Inspector 中显示为 "Gameplay Tags" 分组。

### Node 上新增的 API

| 方法/属性 | 签名 | 说明 |
|----------|------|------|
| `gameplay_tags` | `Ref<GameplayTagContainer>` | 属性，Inspector 中可编辑 |
| `set_gameplay_tags` | `set_gameplay_tags(tags: GameplayTagContainer)` | 设置标签容器 |
| `get_gameplay_tags` | `get_gameplay_tags() -> GameplayTagContainer` | 获取标签容器 |
| `add_gameplay_tag` | `add_gameplay_tag(tag_name: StringName)` | 添加标签（自动创建容器） |
| `remove_gameplay_tag` | `remove_gameplay_tag(tag_name: StringName)` | 移除标签 |
| `has_gameplay_tag` | `has_gameplay_tag(tag_name: StringName) -> bool` | 层级匹配查询 |
| `has_gameplay_tag_exact` | `has_gameplay_tag_exact(tag_name: StringName) -> bool` | 精确匹配查询 |

### 使用示例

```gdscript
# 通过便捷方法
var enemy = get_node("Enemy")
enemy.add_gameplay_tag("Enemy.Type.Undead")
enemy.add_gameplay_tag("Enemy.Rank.Boss")

if enemy.has_gameplay_tag("Enemy.Type"):     # true（层级匹配）
    print("这是一个敌人")

if enemy.has_gameplay_tag("Enemy.Rank.Boss"): # true
    print("这是一个Boss!")
```

### 设计细节

- **懒初始化**：初始时 `gameplay_tags` 为 null，不占内存。首次调用 `add_gameplay_tag()` 时自动创建容器。
- **修改文件**：`scene/main/node.h`（添加成员和方法声明）、`scene/main/node.cpp`（方法实现和绑定）

---

## 十二、编辑器集成

### 改动文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `editor/gui/gameplay_tag_picker_dialog.h/.cpp` | **新建** | Tag 选择器弹窗（公共组件） |
| `editor/settings/gameplay_tag_settings_editor.h/.cpp` | **新建** | ProjectSettings → 全局 → Gameplay Tags 面板 |
| `editor/inspector/gameplay_tag_editor_plugin.h/.cpp` | **新建** | Inspector 中 GameplayTagContainer 的自定义编辑器 |
| `editor/settings/project_settings_editor.h` | **修改** | 添加 GameplayTagSettingsEditor 前向声明和成员 |
| `editor/settings/project_settings_editor.cpp` | **修改** | 创建并嵌入 Gameplay Tags 标签页 |
| `editor/register_editor_types.cpp` | **修改** | 注册 GameplayTagEditorPlugin |

### 12.1 GameplayTagPickerDialog — Tag 选择器弹窗

可复用的弹窗对话框，被 ProjectSettings 面板和 Inspector 属性编辑器共享。

**功能**：
- 从 `GameplayTagManager` 读取所有已注册标签，以树形结构显示
- 搜索框实时过滤（匹配完整标签路径）
- 单选模式：点击选中，双击确认
- 多选模式：勾选框独立勾选，确认后返回所有勾选项
- 每次弹出时自动刷新标签树

### 12.2 GameplayTagSettingsEditor — 项目设置面板

嵌入 **Project Settings → 全局 → Gameplay Tags** 标签页。

**功能**：
- 顶部：输入标签名 + 备注 + Add 按钮
- 主体：两列 Tree（Tag 名称 + Comment），树形层级显示
- 每行有删除按钮（仅叶子标签可删除）
- Comment 列可内联编辑
- 修改后自动保存到 `project.godot`

### 12.3 GameplayTagEditorPlugin — Inspector 属性编辑器

当 Inspector 中正在编辑一个 `GameplayTagContainer` 资源时，注入自定义编辑界面。

**功能**：
- 显示当前容器中的所有标签列表
- 每个标签旁有 × 删除按钮
- "Add Tag..." 按钮弹出 GameplayTagPickerDialog
- 实时响应容器变化（通过 `connect_changed`）

### 属性隐藏

- `GameplayTagContainer.tag_names` 属性标记为 `PROPERTY_USAGE_STORAGE`，仅用于序列化，不在 Inspector 中显示原始 PackedStringArray 编辑器

---

## 十三、TODO List

- [x] 核心层：GameplayTag / GameplayTagContainer / GameplayTagManager
- [x] ProjectSettings 持久化集成
- [x] Node 集成（`gameplay_tags` 属性、便捷方法）
- [x] 编辑器：GameplayTagPickerDialog（标签选择器弹窗）
- [x] 编辑器：GameplayTagSettingsEditor（项目设置面板）
- [x] 编辑器：GameplayTagEditorPlugin（Inspector 属性编辑器）
- [x] 编辑器：修复 Tree 勾选模式下文字消失（`set_cell_mode` 在 `set_text` 之前）
- [x] 编辑器：修复 Picker Dialog 打开时不还原已选 Tag（pending selection 机制）
- [x] 编辑器：Picker Dialog 展开含已选 Tag 的树节点（选了 `Damage.Fire.DoT` 时自动展开 Damage → Fire）
- [x] 编辑器：SceneTree 按 Tag 搜索/过滤节点（`tag:` 前缀过滤 + 右键菜单快捷入口）
- [x] Node 集成：`find_children_by_tag(tag_name, recursive)` 按标签查找子节点

### 待实现方案备忘

#### Picker Dialog 还原已选 Tag

**问题**：打开选择面板时所有勾选都是空的，没有还原之前已选的 Tag。

**根因**：时序问题。`_add_tag_pressed()` 中先调用 `set_selected_tags()` 再 `popup_centered()`，但 popup 时触发 `NOTIFICATION_VISIBILITY_CHANGED` → `refresh()` → `_update_tree()` 清空重建了整棵树，之前的勾选被覆盖。

**方案**：引入 pending selection 机制。
1. 新增 `set_pending_selected_tags()`，只存储待选标签到成员变量，不直接操作 Tree。
2. `_notification(VISIBILITY_CHANGED)` 中，`refresh()` 重建树之后，检查 pending → 调用 `set_selected_tags()` 应用勾选。
3. 调用方 `_add_tag_pressed()` 改为调用 `set_pending_selected_tags()`。

**改动文件**：
- `editor/gui/gameplay_tag_picker_dialog.h` — 新增 `pending_selected_tags`、`has_pending_selection`、`set_pending_selected_tags()`
- `editor/gui/gameplay_tag_picker_dialog.cpp` — 实现 pending 逻辑
- `editor/inspector/gameplay_tag_editor_plugin.cpp` — `_add_tag_pressed()` 改调用

#### Picker Dialog 自动展开已选节点

**问题**：还原勾选后，如果已选 Tag 在深层（如 `Damage.Fire.DoT`），Tree 默认折叠，用户看不到勾选状态。

**方案**：在 `set_selected_tags()` 中，对每个被勾选的 item，向上遍历 parent 逐级 `set_collapsed(false)`。

```cpp
// set_selected_tags() 末尾追加：
for (int i = 0; i < p_tags.size(); i++) {
    StringName tag_sn = StringName(p_tags[i]);
    if (tag_items.has(tag_sn)) {
        TreeItem *item = tag_items[tag_sn];
        // 展开所有祖先节点
        TreeItem *parent = item->get_parent();
        while (parent && parent != tag_tree->get_root()) {
            parent->set_collapsed(false);
            parent = parent->get_parent();
        }
    }
}
```

#### SceneTree 按 Tag 搜索/过滤节点

**问题**：场景树中节点很多时，想快速找到带某个 Tag 的节点很困难。

**方案**：在 SceneTree Dock 的搜索框旁增加 "Tag Filter" 模式，或在现有的 `type:` 过滤语法基础上增加 `tag:` 前缀支持。遍历场景树时调用 `node->has_gameplay_tag()` 做匹配。

#### find_children_by_tag()

**问题**：GDScript 中按标签查找子节点需要手写递归遍历。

**方案**：在 `Node` 类新增方法：

```cpp
TypedArray<Node> Node::find_children_by_tag(const StringName &p_tag_name, bool p_recursive) const {
    TypedArray<Node> result;
    for (int i = 0; i < get_child_count(); i++) {
        Node *child = get_child(i);
        if (child->has_gameplay_tag(p_tag_name)) {
            result.push_back(child);
        }
        if (p_recursive) {
            result.append_array(child->find_children_by_tag(p_tag_name, true));
        }
    }
    return result;
}
```

---

## 十四、更新日志

| 日期 | 变更 |
|------|------|
| 2025-04-04 | 修复：Picker Dialog 打开时不还原已选 Tag。原因是 `NOTIFICATION_VISIBILITY_CHANGED` 中 `refresh()` 重建了 Tree，覆盖了之前的 `set_selected_tags()`。引入 `set_pending_selected_tags()` 延迟选择机制。 |
- `gameplay_tags/tag_list` 项目设置也标记为 `PROPERTY_USAGE_STORAGE`，不在"常规"面板中显示，通过专用 Gameplay Tags 标签页管理

### 使用流程

1. 打开 **Project Settings → 全局 → Gameplay Tags**
2. 输入标签名（如 `Damage.Fire.DoT`），点击 Add
3. 标签自动以树形结构显示（Damage → Fire → DoT）
4. 在场景中选中一个 Node，在 Inspector 的 **Gameplay Tags** 分组中点击 GameplayTagContainer
5. 点击 **"Add Tag..."**，弹出树形选择器
6. 勾选需要的标签，点击确定
7. 标签显示在 Inspector 的标签列表中
