
# Enhanced Input 增强输入系统

> 为 Godot 引擎引入 UE 风格的增强输入系统，在现有 InputMap 基础上增加 **输入映射上下文（InputMappingContext）**、**输入修饰器（InputModifier）**、**输入触发器（InputTrigger）** 三大核心概念，支持上下文优先级切换、输入值变换和丰富的触发条件。

---

## 一、要解决的问题

### Godot 现有 InputMap 的痛点

1. **没有上下文切换**：所有 Action 始终全局生效。步行、开车、游泳、UI 菜单的输入无法按场景动态启用/禁用，开发者只能手动用 `if` 判断状态。
2. **缺少输入修饰器**：摇杆死区只有全局 deadzone，没有径向死区、轴反转、灵敏度曲线、平滑插值等常见需求。
3. **触发条件单一**：只有 pressed / released 两种状态，没有"按住超过 0.5 秒触发"、"双击触发"、"蓄力释放触发"、"组合键触发"等丰富触发模式。
4. **输入值类型单一**：Action 只输出 `bool`（pressed）和 `float`（strength），无法直接输出 `Vector2`（移动方向）或 `Vector3`。
5. **同一按键不同行为难处理**：比如"E 键"在不同上下文中代表"拾取"、"交互"、"上车"，需要手动管理优先级和冲突。

### UE Enhanced Input 的核心思想

> **将输入处理拆分为三个独立阶段：映射（哪个按键）→ 修饰（值变换）→ 触发（何时算触发），每个阶段都可以自由组合和替换。**

---

## 二、核心概念对照

| UE 概念 | Godot 新增概念 | 说明 |
|---------|---------------|------|
| Input Action | `InputAction`（Resource） | 定义一个逻辑输入动作及其值类型（Bool/Float/Vector2/Vector3） |
| Input Mapping Context | `InputMappingContext`（Resource） | 一组 Action ↔ 按键的映射关系，可动态启用/禁用，有优先级 |
| Input Modifier | `InputModifier`（Resource） | 对原始输入值进行变换（死区、反转、灵敏度曲线等） |
| Input Trigger | `InputTrigger`（Resource） | 定义触发条件（按下、按住、长按、双击、蓄力等） |
| Enhanced Input Component | `EnhancedInputManager` 单例 | 管理上下文栈、处理输入管线 |

---

## 三、与现有系统的关系

### 设计原则：扩展而非替代

- **现有 InputMap 保持不动**：`Input.is_action_pressed()` 等 API 继续可用
- Enhanced Input 是**并行的高级系统**，通过新的 API 使用
- 两套系统可以共存，项目可以渐进迁移
- Enhanced Input 底层仍然监听同样的 `InputEvent`

### 代码位置

放在 `core/input/` 目录下，作为核心输入系统的扩展：

```
core/input/
├── input.h/.cpp                          ← 现有，不改动
├── input_map.h/.cpp                      ← 现有，不改动
├── input_event.h/.cpp                    ← 现有，不改动
├── enhanced_input_action_value.h/.cpp    ← 新增：InputActionValue 多类型值
├── enhanced_input_action.h/.cpp          ← 新增：InputAction 资源
├── enhanced_input_mapping_context.h/.cpp ← 新增：InputActionMapping + InputMappingContext 资源
├── enhanced_input_modifier.h/.cpp        ← 新增：InputModifier 基类 + 8 个内置修饰器
├── enhanced_input_trigger.h/.cpp         ← 新增：InputTrigger 基类 + 8 个内置触发器
└── enhanced_input_manager.h/.cpp         ← 新增：EnhancedInputManager 单例（GDScript 中通过 EnhancedInput 访问）
```

---

## 四、InputActionValue — 多类型输入值

### 设计动机

Godot 现有 Action 只输出 bool + float。但实际游戏中：
- **移动**需要 Vector2（方向 + 强度）
- **3D 飞行**需要 Vector3
- **开火**只需要 Bool
- **油门**需要 Float（0~1 范围）

### 类定义

```cpp
// core/input/enhanced_input_action_value.h
class InputActionValue : public RefCounted {
    GDCLASS(InputActionValue, RefCounted);

public:
    enum ValueType {
        BOOL,       // 数字按键，开关类
        FLOAT,      // 单轴，如油门、缩放
        VECTOR2,    // 双轴，如移动方向、鼠标 delta
        VECTOR3,    // 三轴，如 6DOF 飞行
    };

private:
    ValueType value_type = BOOL;
    Vector3 value = Vector3();  // 内部统一用 Vector3 存储

protected:
    static void _bind_methods();

public:
    // 工厂方法
    static Ref<InputActionValue> create_bool(bool p_value);
    static Ref<InputActionValue> create_float(float p_value);
    static Ref<InputActionValue> create_vector2(Vector2 p_value);
    static Ref<InputActionValue> create_vector3(Vector3 p_value);

    // 读取
    ValueType get_value_type() const;
    bool get_bool() const;        // value.x > 0
    float get_float() const;      // value.x
    Vector2 get_vector2() const;  // (value.x, value.y)
    Vector3 get_vector3() const;  // value

    // 内部用
    Vector3 get_raw() const;
    void set_raw(Vector3 p_value);
    void set_value_type(ValueType p_type);

    bool is_non_zero() const;
};
```

### 设计说明

- 内部统一用 `Vector3` 存储，避免 union/variant 的复杂性
- `Bool` 映射到 `value.x > 0`，`Float` 映射到 `value.x`
- 零分配：工厂方法返回 `Ref<>`，GDScript 友好

---

## 五、InputAction — 输入动作资源

### 设计动机

将"动作"从 InputMap 的字符串名升级为独立的 Resource 对象，可以：
- 声明值类型（Bool/Float/Vector2/Vector3）
- 在 Inspector 中作为 `@export` 属性引用
- 保存为 `.tres` 文件，多个上下文共享同一个 Action

### 类定义

```cpp
// core/input/enhanced_input_action.h
class InputAction : public Resource {
    GDCLASS(InputAction, Resource);

public:
    using ValueType = InputActionValue::ValueType;

    enum AccumulationMode {
        CUMULATIVE,   // 累加（适合移动：WASD 四个键各贡献一个方向）
        TAKE_HIGHEST, // 取最大值
    };

private:
    StringName action_name;
    ValueType value_type = ValueType::BOOL;
    AccumulationMode accumulation_mode = CUMULATIVE;

protected:
    static void _bind_methods();

public:
    void set_action_name(const StringName &p_name);
    StringName get_action_name() const;

    void set_value_type(ValueType p_type);
    ValueType get_value_type() const;

    void set_accumulation_mode(AccumulationMode p_mode);
    AccumulationMode get_accumulation_mode() const;
};
```

> **注意**：`consume_input` 已移至 `InputActionMapping`（按绑定粒度控制消费，与 UE 一致）。`description` 已移除（减少 Inspector 空间占用）。

### GDScript 使用

```gdscript
# 定义动作（保存为 .tres）
var move_action = InputAction.new()
move_action.action_name = "Move"
move_action.value_type = InputActionValue.VECTOR2

var jump_action = InputAction.new()
jump_action.action_name = "Jump"
jump_action.value_type = InputActionValue.BOOL
```

---

## 六、InputModifier — 输入修饰器

### 设计动机

在原始输入值和最终触发之间，插入一系列值变换步骤。每个修饰器是一个小的处理单元，可以自由组合。

### 基类定义

```cpp
// core/input/enhanced_input_modifier.h
class InputModifier : public Resource {
    GDCLASS(InputModifier, Resource);

protected:
    static void _bind_methods();
    GDVIRTUAL2R(Vector3, _modify, Vector3, float); // (raw_value, delta_time) -> modified_value

public:
    // C++ 子类重写
    virtual Vector3 modify(Vector3 p_value, float p_delta) const;
};
```

### 内置修饰器列表

| 修饰器类名 | 功能 | 参数 |
|-----------|------|------|
| `InputModifierDeadZone` | 死区过滤 | `lower_threshold`（0.2）, `upper_threshold`（1.0）, `type`（Axial/Radial） |
| `InputModifierScalar` | 标量缩放 | `scale`: Vector3（各轴缩放系数） |
| `InputModifierNegate` | 轴反转 | `negate_x`, `negate_y`, `negate_z`: bool |
| `InputModifierSmooth` | 平滑插值 | `speed`: float（插值速度） |
| `InputModifierResponseCurve` | 响应曲线 | `curve`: Curve（自定义曲线资源） |
| `InputModifierSwizzle` | 轴重映射 | `order`: enum（YXZ, ZYX, 等） |
| `InputModifierNormalize` | 归一化 | 无参数，将 Vector2/Vector3 归一化 |
| `InputModifierClamp` | 值钳制 | `min`, `max`: Vector3 |

### InputModifierDeadZone 详细设计

```cpp
class InputModifierDeadZone : public InputModifier {
    GDCLASS(InputModifierDeadZone, InputModifier);

public:
    enum DeadZoneType {
        AXIAL,   // 每轴独立计算死区
        RADIAL,  // 按向量长度计算死区（摇杆推荐）
    };

private:
    float lower_threshold = 0.2f;
    float upper_threshold = 1.0f;
    DeadZoneType type = RADIAL;

protected:
    static void _bind_methods();

public:
    virtual Vector3 modify(Vector3 p_value, float p_delta) const override;
    // Radial: 计算 length，如果 < lower 则 0，否则 remap 到 [0, 1]
    // Axial: 每个分量独立做同样的处理
};
```

### InputModifierResponseCurve 详细设计

```cpp
class InputModifierResponseCurve : public InputModifier {
    GDCLASS(InputModifierResponseCurve, InputModifier);

private:
    Ref<Curve> curve_x;  // X 轴响应曲线
    Ref<Curve> curve_y;  // Y 轴响应曲线（可选，不设则复用 curve_x）
    Ref<Curve> curve_z;  // Z 轴响应曲线（可选）

protected:
    static void _bind_methods();

public:
    virtual Vector3 modify(Vector3 p_value, float p_delta) const override;
    // 对每个分量的绝对值查 curve，保留符号
};
```

### GDScript 自定义修饰器

```gdscript
class_name MyCustomModifier extends InputModifier

@export var multiplier: float = 2.0

func _modify(value: Vector3, delta: float) -> Vector3:
    return value * multiplier
```

---

## 七、InputTrigger — 输入触发器

### 设计动机

Godot 现有系统只有"按下/释放"两种状态。Enhanced Input 引入丰富的触发条件，每个触发器是独立的 Resource，可自由组合。

### 触发状态枚举

```cpp
enum TriggerState {
    TRIGGER_NONE,      // 未触发
    TRIGGER_ONGOING,   // 正在进行中（如正在按住，但还没达到触发条件）
    TRIGGER_TRIGGERED, // 已触发
};
// 注意：没有 CANCELED 状态。Canceled 是信号层面的概念
// （ONGOING → NONE 时发送 action_canceled 信号），不是 TriggerState 枚举值。

enum TriggerEvent {
    TRIGGER_EVENT_NONE      = 0,
    TRIGGER_EVENT_STARTED   = 1 << 0,  // 从 None → Ongoing/Triggered
    TRIGGER_EVENT_ONGOING   = 1 << 1,  // 持续 Ongoing 中（每帧）
    TRIGGER_EVENT_TRIGGERED = 1 << 2,  // 到达 Triggered 状态
    TRIGGER_EVENT_COMPLETED = 1 << 3,  // Triggered → None（松手）
    TRIGGER_EVENT_CANCELED  = 1 << 4,  // Ongoing → None（未完成就松手）
};
```

### 基类定义

```cpp
// core/input/enhanced_input_trigger.h
class InputTrigger : public Resource {
    GDCLASS(InputTrigger, Resource);

protected:
    static void _bind_methods();
    GDVIRTUAL3R(int, _update_state, int, Vector3, double);
    // GDScript 可重写 _update_state(current_state, value, delta) -> new TriggerState

public:
    // C++ 接口：带 r_elapsed 引用参数，运行时状态由 Manager 外部管理
    virtual TriggerState update_state(
        TriggerState p_current_state,
        const Vector3 &p_value,
        double p_delta,
        float &r_elapsed   // 外部传入的计时器，触发器 Resource 不存运行时状态
    );

    // 注意：如果 Action 没有配置任何触发器，Manager 会默认使用 InputTriggerDown 行为
};
```

### 内置触发器列表

| 触发器类名 | 功能 | 参数 | 触发时机 |
|-----------|------|------|---------|
| `InputTriggerDown` | 按下即触发 | 无 | 值非零时每帧 Triggered |
| `InputTriggerPressed` | 按下瞬间触发 | 无 | 从零变为非零的那一帧 Triggered |
| `InputTriggerReleased` | 释放瞬间触发 | 无 | 从非零变为零的那一帧 Triggered |
| `InputTriggerHold` | 长按触发 | `hold_time`: float（秒）, `one_shot`: bool | 按住达到时间后 Triggered |
| `InputTriggerHoldAndRelease` | 长按后释放触发 | `hold_time`: float | 按住够时间后松手才 Triggered |
| `InputTriggerTap` | 快速点击触发 | `tap_time`: float（最大按住时间） | 在时间内松手则 Triggered |
| `InputTriggerPulse` | 重复脉冲触发 | `interval`: float, `trigger_on_start`: bool | 按住期间每隔 interval Triggered |
| `InputTriggerChordAction` | 组合键触发 | `chord_action`: InputAction | 另一个 Action 也按下时才 Triggered |

### InputTriggerHold 详细设计

```cpp
class InputTriggerHold : public InputTrigger {
    GDCLASS(InputTriggerHold, InputTrigger);

private:
    float hold_time = 0.5f;  // 需要按住的时间（秒）
    bool one_shot = false;    // true = 只触发一次，false = 达到后每帧触发
    // 注意：没有 elapsed 成员变量！
    // 计时器通过 update_state 的 r_elapsed 引用参数从外部（MappingState）传入

protected:
    static void _bind_methods();

public:
    virtual TriggerState update_state(
        TriggerState p_current_state,
        const Vector3 &p_value,
        double p_delta,
        float &r_elapsed
    ) override;
    // 值非零时累加 r_elapsed
    // r_elapsed < hold_time → ONGOING
    // r_elapsed >= hold_time → TRIGGERED
    // 值归零 → NONE（重置 r_elapsed）
};
```

### GDScript 自定义触发器

```gdscript
class_name DoubleTapTrigger extends InputTrigger

@export var tap_interval: float = 0.3  # 两次点击间隔

var _last_tap_time: float = -1.0
var _tap_count: int = 0

func _update_state(current_state: int, value: Vector3, delta: float) -> int:
    if value.x > 0:  # 按下
        var now = Time.get_ticks_msec() / 1000.0
        if _last_tap_time > 0 and (now - _last_tap_time) < tap_interval:
            _tap_count += 1
        else:
            _tap_count = 1
        _last_tap_time = now

        if _tap_count >= 2:
            _tap_count = 0
            return InputTrigger.TRIGGER_TRIGGERED
        return InputTrigger.TRIGGER_ONGOING
    return InputTrigger.TRIGGER_NONE
```

---

## 八、InputMappingContext — 输入映射上下文

### 设计动机

这是 Enhanced Input 的**核心创新点**。上下文解决了"同一个按键在不同场景有不同含义"的问题。

### 类定义

```cpp
// core/input/enhanced_input_mapping_context.h

// 单个按键到 Action 的映射配置
class InputActionMapping : public Resource {
    GDCLASS(InputActionMapping, Resource);

private:
    Ref<InputAction> action;
    Ref<InputEvent> input_event;  // 复用 Godot 现有的 InputEvent 体系
    TypedArray<InputModifier> modifiers;  // 修饰器链
    TypedArray<InputTrigger> triggers;    // 触发器列表
    bool consume_input = true;    // 是否消费输入：匹配后阻止低优先级上下文收到同一物理按键

protected:
    static void _bind_methods();

public:
    void set_action(const Ref<InputAction> &p_action);
    Ref<InputAction> get_action() const;

    void set_input_event(const Ref<InputEvent> &p_event);
    Ref<InputEvent> get_input_event() const;

    void set_modifiers(const TypedArray<InputModifier> &p_modifiers);
    TypedArray<InputModifier> get_modifiers() const;
    void add_modifier(const Ref<InputModifier> &p_modifier);

    void set_triggers(const TypedArray<InputTrigger> &p_triggers);
    TypedArray<InputTrigger> get_triggers() const;
    void add_trigger(const Ref<InputTrigger> &p_trigger);

    void set_consume_input(bool p_consume);
    bool get_consume_input() const;
};

// 输入映射上下文
class InputMappingContext : public Resource {
    GDCLASS(InputMappingContext, Resource);

private:
    StringName context_name;
    TypedArray<InputActionMapping> mappings;

protected:
    static void _bind_methods();

public:
    void set_context_name(const StringName &p_name);
    StringName get_context_name() const;

    void set_mappings(const TypedArray<InputActionMapping> &p_mappings);
    TypedArray<InputActionMapping> get_mappings() const;

    void add_mapping(const Ref<InputActionMapping> &p_mapping);
    void remove_mapping(int p_index);

    // 便捷方法：快速添加一个映射
    Ref<InputActionMapping> map_action(
        const Ref<InputAction> &p_action,
        const Ref<InputEvent> &p_event
    );
};
```

### 上下文栈与优先级

```
上下文栈（按优先级从高到低排列）：

优先级 100: UI_Context       ← 菜单打开时推入，关闭时弹出
优先级  50: Vehicle_Context  ← 上车时推入，下车时弹出
优先级   0: OnFoot_Context   ← 始终存在的默认上下文

当按下 "E" 键时：
1. 先查 UI_Context → 有 "E = Confirm" → 触发 Confirm，消费输入
2. Vehicle_Context 和 OnFoot_Context 中的 "E" 映射被跳过

当菜单关闭后，UI_Context 弹出：
1. 查 Vehicle_Context → 有 "E = Exit Vehicle" → 触发
```

---

## 九、EnhancedInputManager — 核心管理器

### 职责

- 维护上下文栈（优先级有序）
- 每帧处理输入管线：`InputEvent → 匹配 → 修饰 → 触发 → 回调`
- 提供查询 API（Action 当前值、是否触发等）
- 发出信号通知

### 类定义

```cpp
// core/input/enhanced_input_manager.h
// GDScript 中通过 EnhancedInput 单例名访问（而非 EnhancedInputManager）
class EnhancedInputManager : public Object {
    GDCLASS(EnhancedInputManager, Object);

private:
    static EnhancedInputManager *singleton;

    struct ContextEntry {
        Ref<InputMappingContext> context;
        int priority = 0;
    };

    Vector<ContextEntry> context_stack; // 按 priority 降序排列

    // 每个 InputActionMapping 的运行时状态
    struct MappingState {
        Ref<InputActionMapping> mapping;
        Vector3 raw_value;         // 当前原始输入值
        bool value_active = false;
        Vector<TriggerState> trigger_states;  // 每个触发器的状态
        Vector<float> trigger_elapsed;        // 每个触发器的计时器
    };

    // 每个 InputAction 的运行时状态
    struct ActionState {
        Ref<InputAction> action;
        Vector3 accumulated_value;            // 修饰+累积后的值
        TriggerState trigger_state = TRIGGER_NONE;
        TriggerState last_trigger_state = TRIGGER_NONE;
        float elapsed_time = 0.0f;
        Vector<MappingState *> mapping_states; // 所有指向此 Action 的 mapping
    };

    Vector<MappingState> all_mapping_states;
    HashMap<StringName, ActionState> action_states;

    // Per-action 绑定
    struct ActionBinding {
        StringName action_name;
        TriggerEvent event_type; // 使用 TriggerEvent 枚举，支持自动补全
        Callable callable;
    };
    Vector<ActionBinding> action_bindings;

public:
    static EnhancedInputManager *get_singleton();

    // === 上下文管理 ===
    void push_mapping_context(const Ref<InputMappingContext> &p_context, int p_priority = 0);
    void pop_mapping_context(const Ref<InputMappingContext> &p_context);
    void clear_all_contexts();
    bool has_mapping_context(const Ref<InputMappingContext> &p_context) const;
    TypedArray<InputMappingContext> get_active_contexts() const;

    // === 输入查询 ===
    Ref<InputActionValue> get_action_value(const Ref<InputAction> &p_action) const;
    bool is_action_triggered(const Ref<InputAction> &p_action) const;
    int get_action_trigger_state(const Ref<InputAction> &p_action) const;
    float get_action_elapsed_time(const Ref<InputAction> &p_action) const;

    // === Per-Action 绑定（使用 TriggerEvent 枚举） ===
    void bind_action(const Ref<InputAction> &p_action, TriggerEvent p_event_type, const Callable &p_callable);
    void unbind_action(const Ref<InputAction> &p_action, TriggerEvent p_event_type, const Callable &p_callable);

    // === 核心处理（由引擎内部调用） ===
    void process_input_event(const Ref<InputEvent> &p_event); // Input 转发事件
    void tick(double p_delta);  // SceneTree 每帧调用
};
```

### 信号列表

| 信号 | 参数 | 触发时机 |
|------|------|---------|
| `action_triggered` | `action, value, state` | 任何触发事件发生时（通用信号） |
| `action_started` | `action, value` | 从 None 变为 Ongoing 或 Triggered |
| `action_ongoing` | `action, value` | 每帧持续 Ongoing |
| `action_completed` | `action, value` | Triggered 后回到 None（完成） |
| `action_canceled` | `action, value` | Ongoing 中回到 None（取消） |
| `context_pushed` | `context` | 上下文被推入 |
| `context_popped` | `context` | 上下文被移除 |

---

## 十、输入处理管线

### 每帧处理流程

```
┌────────────────────────────────────────────────────────┐
│  1. InputEvent 到达（键盘/鼠标/手柄/触屏）              │
├────────────────────────────────────────────────────────┤
│  2. 遍历上下文栈（按优先级从高到低）                      │
│     ├── 遍历上下文中的每个 InputActionMapping           │
│     │   ├── 匹配 InputEvent ↔ Mapping.input_event     │
│     │   │                                              │
│     │   ├── 3. 提取原始值                               │
│     │   │   ├── Key → (1.0, 0, 0)                     │
│     │   │   ├── JoyAxis → (axis_value, 0, 0)          │
│     │   │   └── MouseMotion → (delta.x, delta.y, 0)   │
│     │   │                                              │
│     │   ├── 4. 修饰器链处理（按顺序）                    │
│     │   │   ├── DeadZone.modify(value)                 │
│     │   │   ├── ResponseCurve.modify(value)            │
│     │   │   └── Scalar.modify(value)                   │
│     │   │                                              │
│     │   ├── 5. 按 AccumulationMode 累加到 Action       │
│     │   │                                              │
│     │   └── 6. consume_input? → 阻止低优先级上下文匹配  │
│     └──                                                │
├────────────────────────────────────────────────────────┤
│  7. 触发器更新（对每个有值变化的 Action）                 │
│     ├── 遍历该 Action 的所有 Trigger                    │
│     ├── trigger.update_state(current, value, delta)     │
│     └── 合并结果 → 最终 TriggerState                   │
├────────────────────────────────────────────────────────┤
│  8. 状态变化检测 & 发送信号                              │
│     ├── None → Triggered : emit action_started          │
│     ├── Ongoing → Ongoing : emit action_ongoing         │
│     ├── * → Triggered    : emit action_triggered        │
│     ├── Triggered → None : emit action_completed        │
│     └── Ongoing → None   : emit action_canceled         │
└────────────────────────────────────────────────────────┘
```

---

## 十一、GDScript 使用示例

### 示例 1：基础 FPS 控制

```gdscript
extends CharacterBody3D

# 定义 Action（可保存为 .tres 在编辑器中创建）
var move_action: InputAction
var jump_action: InputAction
var look_action: InputAction

# 定义上下文
var on_foot_context: InputMappingContext

func _ready():
    # 创建 Actions
    move_action = InputAction.new()
    move_action.action_name = "Move"
    move_action.value_type = InputActionValue.VECTOR2

    jump_action = InputAction.new()
    jump_action.action_name = "Jump"
    jump_action.value_type = InputActionValue.BOOL

    look_action = InputAction.new()
    look_action.action_name = "Look"
    look_action.value_type = InputActionValue.VECTOR2

    # 创建上下文并添加映射
    on_foot_context = InputMappingContext.new()
    on_foot_context.context_name = "OnFoot"

    # WASD 移动（四个键组合为 Vector2）
    # 键盘按下的原始值是 (1,0,0)，需要用 Swizzle/Negate 把值搬到正确的分量
    _map_move_key(on_foot_context, move_action, KEY_W, true, true)   # W: swizzle→(0,1,0)→negate Y→(0,-1,0)
    _map_move_key(on_foot_context, move_action, KEY_S, true, false)  # S: swizzle→(0,1,0)
    _map_move_key(on_foot_context, move_action, KEY_A, false, true)  # A: negate X→(-1,0,0)
    _map_move_key(on_foot_context, move_action, KEY_D, false, false) # D: (1,0,0) 无需修饰

    # 空格跳跃
    var space_key = InputEventKey.new()
    space_key.keycode = KEY_SPACE
    var jump_mapping = on_foot_context.map_action(jump_action, space_key)
    jump_mapping.add_trigger(InputTriggerPressed.new())  # 按下瞬间触发

    # 推入上下文
    EnhancedInput.push_mapping_context(on_foot_context, 0)

    # 连接信号
    EnhancedInput.action_triggered.connect(_on_action_triggered)

func _on_action_triggered(action: InputAction, value: InputActionValue, state: int):
    if action == jump_action:
        # 跳跃
        velocity.y = 5.0

func _physics_process(delta):
    # 直接查询移动值
    var move_value = EnhancedInput.get_action_value(move_action)
    if move_value:
        var dir = move_value.get_vector2()
        velocity.x = dir.x * speed
        velocity.z = dir.y * speed
    move_and_slide()

# 键盘原始值是 (1,0,0)，用 Swizzle 把 X 搬到 Y 轴，用 Negate 取反
func _map_move_key(ctx, action, key, use_swizzle, use_negate):
    var ev = InputEventKey.new()
    ev.keycode = key
    var mapping = ctx.map_action(action, ev)
    if use_swizzle:
        var swizzle = InputModifierSwizzle.new()
        swizzle.order = InputModifierSwizzle.SWIZZLE_YXZ  # (1,0,0) → (0,1,0)
        mapping.add_modifier(swizzle)
    if use_negate:
        var neg = InputModifierNegate.new()
        if use_swizzle:
            neg.negate_y = true   # 反转 Y（前进方向）
        else:
            neg.negate_x = true   # 反转 X（左移方向）
        mapping.add_modifier(neg)
```

### 示例 2：上下文切换（步行 vs 载具）

```gdscript
var on_foot_ctx: InputMappingContext
var vehicle_ctx: InputMappingContext
var interact_action: InputAction
var exit_vehicle_action: InputAction

func enter_vehicle():
    # 推入载具上下文（更高优先级）
    EnhancedInput.push_mapping_context(vehicle_ctx, 50)
    # 同一个 E 键，在载具上下文中是"下车"

func exit_vehicle():
    # 移除载具上下文，回到步行
    EnhancedInput.pop_mapping_context(vehicle_ctx)
    # E 键自动恢复为步行上下文中的"交互"
```

### 示例 3：长按蓄力攻击

```gdscript
var attack_action: InputAction

func _ready():
    attack_action = InputAction.new()
    attack_action.action_name = "Attack"
    attack_action.value_type = InputActionValue.FLOAT

    var mapping = combat_ctx.map_action(attack_action,
        InputEventMouseButton.new())

    # 使用 HoldAndRelease 触发器：按住蓄力，松手释放
    var trigger = InputTriggerHoldAndRelease.new()
    trigger.hold_time = 0.5
    mapping.add_trigger(trigger)

    EnhancedInput.action_ongoing.connect(func(action, value):
        if action == attack_action:
            # 正在蓄力，更新 UI
            charge_bar.value = EnhancedInput.get_action_elapsed_time(action)
    )

    EnhancedInput.action_triggered.connect(func(action, value):
        if action == attack_action:
            # 蓄力释放！伤害 = 蓄力时间
            var charge_time = value.get_float()
            deal_damage(charge_time * base_damage)
    )
```

### 示例 4：使用 @export 在编辑器中配置

```gdscript
extends CharacterBody3D

@export var move_action: InputAction
@export var jump_action: InputAction
@export var movement_context: InputMappingContext

func _ready():
    EnhancedInput.push_mapping_context(movement_context)
```

---

## 十二、实现文件清单

### 新建文件

| 文件 | 说明 |
|------|------|
| `core/input/enhanced_input_action_value.h/.cpp` | InputActionValue 多类型值 |
| `core/input/enhanced_input_action.h/.cpp` | InputAction 资源 |
| `core/input/enhanced_input_modifier.h/.cpp` | InputModifier 基类 + 所有内置修饰器 |
| `core/input/enhanced_input_trigger.h/.cpp` | InputTrigger 基类 + 所有内置触发器 |
| `core/input/enhanced_input_mapping_context.h/.cpp` | InputActionMapping + InputMappingContext 资源 |
| `core/input/enhanced_input_manager.h/.cpp` | EnhancedInputManager 全局单例 |

### 修改文件

| 文件 | 修改内容 |
|------|---------|
| `core/input/SCsub` | 无需修改（已有 `*.cpp` 通配自动收集） |
| `core/register_core_types.cpp` | 注册所有新类型、创建/销毁 EnhancedInputManager 单例 |
| `core/input/input.cpp` | 在 `_parse_input_event_impl` 中转发事件给 EnhancedInputManager |
| `scene/main/scene_tree.cpp` | 在帧循环中调用 `EnhancedInputManager::tick(delta)` |

### 可选：编辑器集成（Phase 2）

| 文件 | 说明 |
|------|------|
| `editor/inspector/enhanced_input_editor_plugin.h/.cpp` | Inspector 中 InputMappingContext 的可视化编辑器 |
| `editor/settings/enhanced_input_settings.h/.cpp` | ProjectSettings 中的 Enhanced Input 设置面板 |

---

## 十三、设计决策

### 1. 为什么放在 core/input/ 而不是 modules/？

- 输入系统是引擎最基础的功能之一
- 需要在 `Input::_parse_input_event_impl` 中集成
- 需要在 `SceneTree` 帧循环中调用 `tick()`
- 需要被 scene/ 和 editor/ 无条件引用

### 2. 为什么复用现有 InputEvent 而不是自定义绑定类？

Godot 已有完善的 `InputEvent` 体系（键盘、鼠标、手柄、触屏），`InputActionMapping` 直接引用 `Ref<InputEvent>` 作为匹配条件：
- 复用 Inspector 中已有的 InputEvent 选择器 UI
- 复用 `InputEvent::action_match()` 匹配逻辑
- 零重复代码

### 3. 为什么 InputAction 是 Resource 而不是 StringName？

- 可以 `@export`，Inspector 中直接选择
- 可以保存为 `.tres`，多处共享
- 携带值类型、累积模式等元数据
- 编辑器可以提供自动补全和验证

### 4. 触发器有状态（如 elapsed 计时），如何处理？

每个 `InputActionMapping` 实例化后，EnhancedInputManager 内部为其创建运行时状态副本（`ActionInstance`）。触发器的 `update_state` 方法接收外部传入的状态，不在 Resource 对象上存储运行时数据。

### 5. 与 GameplayTag 系统的联动？

- InputAction 可以带 `gameplay_tags` 属性
- 可以按标签批量禁用/启用 Action
- 例如：`EnhancedInputManager.disable_actions_with_tag("Input.Combat")`

---

## 十四、实施顺序

| 阶段 | 内容 | 依赖 |
|------|------|------|
| **Phase 1** | InputActionValue + InputAction | 无 |
| **Phase 2** | InputModifier 基类 + DeadZone + Scalar + Negate | Phase 1 |
| **Phase 3** | InputTrigger 基类 + Down + Pressed + Released + Hold | Phase 1 |
| **Phase 4** | InputActionMapping + InputMappingContext | Phase 1 |
| **Phase 5** | EnhancedInputManager 核心管线 + 上下文栈 | Phase 2,3,4 |
| **Phase 6** | 集成到 Input + SceneTree | Phase 5 |
| **Phase 7** | 剩余内置修饰器和触发器 | Phase 5 |
| **Phase 8** | 编辑器可视化编辑器 | Phase 5 |
| **Phase 9** | GameplayTag 联动 | Phase 5 + GameplayTag |

---

## 十五、待讨论的设计决策

### 1. EnhancedInputManager 是全局单例还是节点？

- **方案 A（推荐）**：全局单例（类似 `Input`），通过 `EnhancedInputManager.xxx()` 使用
- **方案 B**：节点（`EnhancedInputComponent`），挂在玩家身上，支持多玩家

初版推荐方案 A（简单），后续可扩展方案 B。

### 2. 上下文切换是栈模式还是集合模式？

- **方案 A（推荐）**：集合 + 优先级（每个上下文有 priority，可随时增删）
- **方案 B**：严格栈模式（后进先出）

推荐方案 A 更灵活，UE 也是优先级模式。

### 3. 多个触发器的合并逻辑？

一个 Mapping 上配置了多个 Trigger 时：
- **方案 A（推荐）**：ANY 模式 — 任一触发器达到 Triggered 就触发
- **方案 B**：ALL 模式 — 所有触发器都达到 Triggered 才触发
- **方案 C**：可配置 AND/OR

初版用 ANY，后续可加配置。

### 4. WASD 四个键如何合并为 Vector2？

- 每个键映射到同一个 `InputAction`（value_type = Vector2）
- 键盘按下的原始值始终是 `(1, 0, 0)`（X=1），不能用 `Scalar` 做方向映射（逐分量乘法会把非 X 轴的值乘以零）
- 正确做法：用 `InputModifierSwizzle`（YXZ）把 X 值搬到 Y 轴，再用 `InputModifierNegate` 取反
  - W → Swizzle YXZ → (0,1,0) → Negate Y → (0,-1,0)
  - S → Swizzle YXZ → (0,1,0)
  - A → Negate X → (-1,0,0)
  - D → 无修饰 → (1,0,0)
- Action 的 `accumulation_mode = CUMULATIVE` 将四个键的值累加
- 最终得到 Vector2 方向

### 5. 是否需要 PlayerIndex / 本地多人支持？

初版不考虑，所有输入默认属于单一玩家。后续可通过给上下文绑定 device ID 实现多人。

---

## 十六、已知优化点（非阻塞）

> 以下是 code review 过程中发现的可优化项，当前不影响功能，优先级较低，后续迭代时可考虑。

### 1. tick() 每帧分配 InputActionValue 对象

**位置**：`EnhancedInputManager::tick()` → 构建 `InputActionValue` 用于信号和回调

**现象**：每帧对每个活跃的 Action 都会 `memnew(InputActionValue)` 创建一个 RefCounted 对象，产生微小的堆分配和引用计数开销。

**优化方案**：在 `ActionState` 中缓存一个 `Ref<InputActionValue>` 成员，每帧只更新其内部 `raw` 值，而不是重新分配对象。

**影响**：微小 GC 压力。活跃 Action 数量通常 < 10，实际影响极低。

---

### 2. InputModifierSmooth 运行时状态存在 Resource 上

**位置**：`InputModifierSmooth::current_value` 成员变量

**现象**：`InputModifierSmooth` 继承自 `Resource`，但 `current_value` 是运行时插值状态。如果多个 `InputActionMapping` 共享同一个 `InputModifierSmooth` 实例（比如通过 `.tres` 资源引用），它们的平滑状态会互相干扰。

**优化方案**：
- 方案 A：在 `MappingState` 中为每个 modifier 维护独立的运行时状态（类似 trigger 的 `elapsed` 外部管理）
- 方案 B：文档中明确说明 Smooth 修饰器不应跨 mapping 共享（当前实际使用中极少共享，影响很小）

**影响**：仅在多个 mapping 共享同一 Smooth 资源实例时出现。实际项目中通常每个 mapping 各自 `new()` 一个，所以问题极罕见。

---

### 3. _event_matches_mapping 多次 Ref 类型转换

**位置**：`EnhancedInputManager::_event_matches_mapping()`

**现象**：每次输入事件到达时，对每个 mapping 做 `Object::cast_to<InputEventKey>()`、`Object::cast_to<InputEventMouseButton>()` 等多次动态类型判断。

**优化方案**：
- 方案 A：在 `MappingState` 构建时预缓存 mapping 的事件类型标记（枚举），匹配时先比较枚举跳过不匹配的类型
- 方案 B：按事件类型分组存储 mapping，事件到达时直接查对应组

**影响**：输入事件频率远低于渲染帧率（通常每秒几十次），cast_to 本身也很快（虚表指针比较），实际无性能瓶颈。

---

### 4. 上下文栈排序使用插入排序

**位置**：`EnhancedInputManager::push_mapping_context()` 中的排序逻辑

**现象**：当前使用手动插入排序维护 `context_stack` 的优先级顺序。

**说明**：这实际上是**正确的选择**，因为：
- 上下文栈通常只有 2~5 个元素
- push/pop 操作频率极低（场景切换级别）
- 插入排序对小数组效率最高，且避免了 Godot `Vector::sort_custom` 不支持 lambda 的问题

**状态**：✅ 无需优化，仅作记录。

---

### 5. 未来可扩展方向

| 方向 | 说明 | 优先级 |
|------|------|--------|
| 编辑器可视化编辑器 | Inspector 中用树形界面编辑 InputMappingContext，类似 UE 的面板 | ⭐⭐⭐ |
| 输入调试面板 | 运行时显示所有 Action 的当前值、触发状态、上下文栈 | ⭐⭐ |
| 本地多人支持 | 给上下文绑定 device ID，支持分屏多人 | ⭐⭐ |
| GameplayTag 联动 | 按标签批量禁用/启用 Action | ⭐ |
| 输入录制/回放 | 利用全局信号录制输入流，用于自动化测试 | ⭐ |
| 触发器 ALL 模式 | 多个触发器全部满足才触发（当前只有 ANY 模式） | ⭐ |
