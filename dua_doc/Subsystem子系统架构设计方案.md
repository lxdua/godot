# Subsystem 子系统架构设计方案

> 为 Godot 引擎引入 UE 风格的 Subsystem 架构，解决 Autoload 滥用问题，提供生命周期自动管理的子系统机制。

---

## 一、要解决的问题

### Autoload 的痛点

1. **生命周期失控**：所有 Autoload 在游戏启动时创建，结束时销毁。即使某个系统只在特定场景需要，也始终占着内存。
2. **场景切换不清理**：切换场景后，Autoload 中缓存的旧数据不会自动清理，需要开发者手动 reset。
3. **无作用域**：全部是全局的，没有"只属于当前关卡"或"只属于当前玩家"的概念。
4. **初始化顺序不可控**：多个 Autoload 之间如果有依赖，只能靠列表排列顺序，容易出 bug。
5. **测试困难**：全局单例难以在单元测试中替换或 mock。
6. **列表膨胀**：项目越大，Autoload 列表越长，最终变成一堆互相引用的"上帝对象"。

### Subsystem 的核心思想

> **子系统的生命周期自动绑定到一个"宿主对象"（Owner），宿主创建时子系统自动创建，宿主销毁时子系统自动销毁。**

---

## 二、类结构设计

```
Subsystem (RefCounted)                    ← 基类
│
├── SceneTreeSubsystem                    ← 绑定到 SceneTree（跨场景，等价于 Autoload）
├── SceneSubsystem                        ← 绑定到当前场景（场景切换时自动重建）
└── ViewportSubsystem                     ← 绑定到 Viewport（多人分屏，每个视口一份）
```

### 辅助类

```
SubsystemCollection<T>                    ← 泛型管理器，负责创建/销毁/查询子系统
```

---

## 三、Subsystem 基类

### 类定义

```cpp
// core/object/subsystem.h
class Subsystem : public RefCounted {
    GDCLASS(Subsystem, RefCounted);

protected:
    static void _bind_methods();

    // 子系统所属的宿主对象。
    Object *outer = nullptr;

    GDVIRTUAL0(_initialize);
    GDVIRTUAL0(_deinitialize);
    GDVIRTUAL0RC(bool, _should_create);

public:
    // 获取宿主对象。
    Object *get_outer() const;

    // 生命周期回调（C++ 子类重写）。
    virtual void initialize() {}
    virtual void deinitialize() {}

    // 是否应该创建此子系统（默认 true，可条件化禁用）。
    virtual bool should_create() const { return true; }
};
```

### GDScript 使用

```gdscript
class_name MySubsystem extends SceneTreeSubsystem

func _should_create() -> bool:
    # 条件创建：只在非编辑器模式下创建
    return not Engine.is_editor_hint()

func _initialize():
    print("子系统启动")

func _deinitialize():
    print("子系统关闭")
```

### 虚方法设计

| 方法 | 调用时机 | 用途 |
|------|---------|------|
| `_should_create() -> bool` | 宿主创建时，创建子系统之前 | 条件化启用/禁用 |
| `_initialize()` | 子系统被创建后 | 初始化资源、连接信号 |
| `_deinitialize()` | 子系统被销毁前 | 清理资源、断开信号 |

---

## 四、SubsystemCollection 管理器

### 职责

- 维护一个宿主对象上所有子系统的集合
- 负责创建、初始化、反初始化、销毁子系统
- 提供 `get_subsystem<T>()` 类型安全查询

### 类定义

```cpp
// core/object/subsystem_collection.h
class SubsystemCollection {
    Object *outer = nullptr;

    // 类名 -> 子系统实例
    HashMap<StringName, Ref<Subsystem>> subsystems;

public:
    void initialize(Object *p_outer);
    void deinitialize();

    // 注册一个子系统类（引擎启动时由 ClassDB 自动发现）。
    void add_subsystem(const StringName &p_class_name);

    // 获取子系统（C++ 侧用模板方法）。
    template <typename T>
    T *get_subsystem() const;

    // 获取子系统（GDScript 侧用类名字符串）。
    Ref<Subsystem> get_subsystem_by_name(const StringName &p_class_name) const;

    // 获取所有子系统。
    TypedArray<Subsystem> get_all_subsystems() const;
};
```

### 自动发现机制

引擎启动时，SubsystemCollection 通过 ClassDB 扫描所有 `SceneTreeSubsystem`、`SceneSubsystem`、`ViewportSubsystem` 的子类，自动注册。开发者只需要定义类，不需要手动注册。

```
ClassDB 扫描
    ↓
找到 SaveSubsystem (extends SceneTreeSubsystem)
找到 WeatherSubsystem (extends SceneSubsystem)
找到 HUDSubsystem (extends ViewportSubsystem)
    ↓
分别注册到对应的 Collection
```

GDScript 脚本定义的子系统需要考虑：
- 方案 A：在 ProjectSettings 中注册（类似 Autoload）
- 方案 B：放在约定目录下自动扫描（如 `res://subsystems/`）
- 方案 C：通过 `class_name` 注册的脚本类自动被 ClassDB 发现

**推荐方案 C**，最符合 Godot 风格。

---

## 五、三种 Subsystem 的生命周期

### 5.1 SceneTreeSubsystem

```
绑定到：SceneTree
生命周期：SceneTree 创建 → SceneTree 销毁
等价于：Autoload（但更规范）
典型用途：存档系统、成就系统、音频管理、网络管理

时间线：
  SceneTree._initialize()
      ↓
  SubsystemCollection.initialize()
      ↓ 对每个 SceneTreeSubsystem 子类:
      ↓   should_create() → true? → 创建实例 → initialize()
      ↓
  ... 游戏运行（场景可以随意切换，Subsystem 不受影响）...
      ↓
  SceneTree._finalize()
      ↓
  SubsystemCollection.deinitialize()
      ↓ 对每个实例:
      ↓   deinitialize() → 释放引用
```

### 5.2 SceneSubsystem

```
绑定到：当前场景的根节点（SceneTree.current_scene）
生命周期：场景加载 → 场景切换/卸载
典型用途：关卡天气、场景AI管理、关卡任务

时间线：
  SceneTree.change_scene_to_file("level_2.tscn")
      ↓
  旧场景的 SceneSubsystem 全部 deinitialize() + 销毁
      ↓
  新场景加载
      ↓
  新的 SceneSubsystem 全部 should_create() → initialize()
      ↓
  ... 场景运行 ...
      ↓
  再次切换场景 → 重复上述过程
```

**核心价值**：切换场景时**自动销毁旧的、创建新的**。所有缓存数据自动清理，零手动 reset。

### 5.3 ViewportSubsystem

```
绑定到：Viewport
生命周期：Viewport 创建 → Viewport 销毁
典型用途：玩家HUD状态、本地输入设置、分屏相机控制

特点：每个 Viewport 拥有独立的一份实例
  Viewport_P1 → HUDSubsystem (P1的血量)
  Viewport_P2 → HUDSubsystem (P2的血量)  ← 完全独立的另一份
```

---

## 六、宿主集成方案

### 6.1 SceneTree 集成

```cpp
// scene/main/scene_tree.h
class SceneTree : public MainLoop {
    SubsystemCollection scene_tree_subsystems;  // 新增
    SubsystemCollection scene_subsystems;       // 新增

public:
    // 获取 SceneTree 级子系统。
    template <typename T>
    T *get_subsystem() const;
    Ref<Subsystem> get_subsystem_by_name(const StringName &p_class_name) const;

    // 获取当前场景级子系统。
    template <typename T>
    T *get_scene_subsystem() const;
    Ref<Subsystem> get_scene_subsystem_by_name(const StringName &p_class_name) const;
};
```

修改点：
- `SceneTree::initialize()` 中初始化 `scene_tree_subsystems`
- `SceneTree::_change_scene()` 中销毁旧 `scene_subsystems`，创建新 `scene_subsystems`
- `SceneTree::finalize()` 中销毁所有

### 6.2 Viewport 集成

```cpp
// scene/main/viewport.h
class Viewport : public Node {
    SubsystemCollection viewport_subsystems;  // 新增

public:
    template <typename T>
    T *get_subsystem() const;
    Ref<Subsystem> get_subsystem_by_name(const StringName &p_class_name) const;
};
```

修改点：
- `Viewport::_enter_tree()` 中初始化
- `Viewport::_exit_tree()` 中销毁

### 6.3 Node 便捷方法

```cpp
// 任何 Node 都可以方便地获取子系统
class Node {
public:
    // 先查 Viewport 级，再查 Scene 级，最后查 SceneTree 级。
    Ref<Subsystem> get_subsystem_by_name(const StringName &p_class_name) const;
};
```

GDScript 中：
```gdscript
# 最简洁的使用方式
var weather = get_subsystem("WeatherSubsystem")
weather.set_rain()

# 明确指定级别
var save = get_tree().get_subsystem("SaveSubsystem")
var hud = get_viewport().get_subsystem("HUDSubsystem")
```

---

## 七、GDScript 使用示例

### 示例 1：存档系统（SceneTreeSubsystem）

```gdscript
class_name SaveSubsystem extends SceneTreeSubsystem

var save_data: Dictionary = {}

func _initialize():
    _load_from_disk()
    print("存档系统初始化完成")

func _deinitialize():
    _save_to_disk()
    print("存档系统关闭，数据已保存")

func save_game():
    save_data["timestamp"] = Time.get_unix_time_from_system()
    _save_to_disk()

func _load_from_disk():
    if FileAccess.file_exists("user://save.json"):
        var file = FileAccess.open("user://save.json", FileAccess.READ)
        save_data = JSON.parse_string(file.get_as_text())

func _save_to_disk():
    var file = FileAccess.open("user://save.json", FileAccess.WRITE)
    file.store_string(JSON.stringify(save_data))
```

### 示例 2：关卡天气（SceneSubsystem）

```gdscript
class_name WeatherSubsystem extends SceneSubsystem

var current_weather: String = "sunny"
var temperature: float = 20.0

func _initialize():
    # 每次进入新关卡时自动调用
    current_weather = "sunny"
    temperature = 20.0
    print("天气系统初始化：晴天 20°C")

func _deinitialize():
    # 离开关卡时自动调用，无需手动清理
    print("天气系统关闭")

func set_weather(type: String):
    current_weather = type
    match type:
        "rain":
            temperature -= 5
        "snow":
            temperature -= 15
    print("天气变为: ", type, " 温度: ", temperature)
```

### 示例 3：分屏HUD（ViewportSubsystem）

```gdscript
class_name HUDSubsystem extends ViewportSubsystem

var health: int = 100
var score: int = 0

func _initialize():
    health = 100
    score = 0

func take_damage(amount: int):
    health -= amount
    if health <= 0:
        _on_player_died()
```

### 示例 4：在游戏逻辑中使用

```gdscript
extends CharacterBody2D

func _ready():
    # 获取场景级子系统
    var weather = get_subsystem("WeatherSubsystem")
    if weather:
        weather.set_weather("rain")

    # 获取跨场景子系统
    var save = get_tree().get_subsystem("SaveSubsystem")
    health = save.save_data.get("player_health", 100)

func _on_death():
    # 获取当前视口的 HUD 子系统
    var hud = get_viewport().get_subsystem("HUDSubsystem")
    hud.show_death_screen()
```

---

## 八、和 Autoload 的对比

| 特性 | Autoload | Subsystem |
|------|----------|-----------|
| **注册方式** | project.godot 静态列表 | class_name 自动发现 |
| **生命周期** | 整个应用程序 | 绑定到 Owner |
| **场景切换** | 不清理，数据残留 | SceneSubsystem 自动重建 |
| **作用域** | 全局唯一 | 可以每个 Viewport 一份 |
| **获取方式** | 直接用全局名 | `get_subsystem("Name")` |
| **条件创建** | 始终创建 | `_should_create()` 可条件化 |
| **初始化顺序** | 按列表顺序 | 可声明依赖关系 |
| **测试友好** | 难以 mock | 可以替换实例 |
| **类型安全** | 字符串名 | 类名，编辑器能自动补全 |
| **是否向后兼容** | — | ✅ Autoload 继续可用，不冲突 |

---

## 九、实现文件清单

### 新建文件

| 文件 | 说明 |
|------|------|
| `core/object/subsystem.h/.cpp` | Subsystem 基类 |
| `core/object/subsystem_collection.h/.cpp` | SubsystemCollection 管理器 |
| `scene/main/scene_tree_subsystem.h/.cpp` | SceneTreeSubsystem |
| `scene/main/scene_subsystem.h/.cpp` | SceneSubsystem |
| `scene/main/viewport_subsystem.h/.cpp` | ViewportSubsystem |

### 修改文件

| 文件 | 修改内容 |
|------|---------|
| `scene/main/scene_tree.h/.cpp` | 添加 SubsystemCollection 成员，集成生命周期 |
| `scene/main/viewport.h/.cpp` | 添加 SubsystemCollection 成员 |
| `scene/main/node.h/.cpp` | 添加 `get_subsystem()` 便捷方法 |
| `core/register_core_types.cpp` | 注册 Subsystem 基类 |
| `scene/register_scene_types.cpp` | 注册三种 Subsystem 子类 |

### 可选：编辑器集成

| 文件 | 说明 |
|------|------|
| `editor/debugger/subsystem_debugger_plugin.h/.cpp` | 调试器面板：显示当前活跃的子系统列表及状态 |

---

## 十、实施顺序

1. **Phase 1：基础框架** — Subsystem 基类 + SubsystemCollection
2. **Phase 2：SceneTreeSubsystem** — 最简单的一种，等价于 Autoload
3. **Phase 3：SceneSubsystem** — 集成场景切换生命周期
4. **Phase 4：ViewportSubsystem** — 集成 Viewport
5. **Phase 5：Node 便捷 API** — `get_subsystem()` 方法
6. **Phase 6（可选）：编辑器面板** — 调试器中显示子系统列表

---

## 十一、待讨论的设计决策

### 1. GDScript 子系统如何被发现？

- **方案 A**：ProjectSettings 中配置列表（类似 Autoload）
- **方案 B**：约定目录扫描（`res://subsystems/`）
- **方案 C**：通过 `class_name` 注册的脚本类自动被 ClassDB 发现（推荐）

### 2. 子系统之间如何声明依赖？

- **方案 A**：不支持依赖，按字母顺序初始化
- **方案 B**：重写 `_get_dependencies() -> Array[StringName]` 返回依赖列表，拓扑排序后初始化（推荐）

### 3. Subsystem 继承 RefCounted 还是 Object？

- **RefCounted**：自动内存管理，和 Resource 一致，GDScript 友好（推荐）
- **Object**：需要手动管理内存，但可以作为 Node 的子节点

### 4. 是否支持运行时动态添加/移除子系统？

- **方案 A**：不支持，所有子系统在宿主初始化时一次性创建（更简单，推荐初版）
- **方案 B**：支持动态注册（后续扩展）

### 5. 和 GameplayTag 系统的联动？

- 每个子系统可以有 `gameplay_tags` 属性，用于按标签查询子系统
- 例如：`get_subsystems_with_tag("System.Combat")` 返回所有带有战斗标签的子系统
