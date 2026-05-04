
# Enhanced Input 落地实施计划

> 基于 [EnhancedInput增强输入系统.md](./EnhancedInput增强输入系统.md) 设计文档的具体落地步骤。

---

## 总览

共分 **6 步**，每步独立可编译可测试，前后有依赖关系：

```
Step 1          Step 2          Step 3          Step 4
InputAction  →  InputModifier →  InputTrigger →  InputMapping
Value            (8个内置)       (8个内置)        Context
                    │               │               │
                    └───────┬───────┘               │
                            │                       │
                            ▼                       ▼
                        Step 5                   Step 5
                    EnhancedInputManager ◄────────┘
                            │
                            ▼
                        Step 6
                    集成到 Input + SceneTree
```

---

## Step 1：InputActionValue + InputAction

> 最底层的数据定义，无任何依赖。

### 新建文件

| 文件 | 内容 |
|------|------|
| `core/input/enhanced_input_action_value.h` | `InputActionValue` 类声明 |
| `core/input/enhanced_input_action_value.cpp` | `InputActionValue` 实现 + `_bind_methods` |
| `core/input/enhanced_input_action.h` | `InputAction` 类声明 |
| `core/input/enhanced_input_action.cpp` | `InputAction` 实现 + `_bind_methods` |

### 实现清单

**InputActionValue** (`RefCounted`)
- [ ] `ValueType` 枚举：`BOOL, FLOAT, VECTOR2, VECTOR3`
- [ ] 内部存储 `Vector3 value` + `ValueType value_type`
- [ ] 工厂方法：`create_bool()`, `create_float()`, `create_vector2()`, `create_vector3()`
- [ ] 读取方法：`get_bool()`, `get_float()`, `get_vector2()`, `get_vector3()`
- [ ] 辅助方法：`get_raw()`, `set_raw()`, `is_non_zero()`
- [ ] `_bind_methods`：绑定所有方法 + 枚举

**InputAction** (`Resource`)
- [x] 属性：`action_name` (StringName), `value_type` (ValueType)
- [x] 属性：`accumulation_mode` 枚举 (CUMULATIVE / TAKE_HIGHEST)
- [x] `_bind_methods`：绑定所有属性 getter/setter + 枚举

> `consume_input` 已移至 `InputActionMapping`。`description` 已移除。

### 修改文件

| 文件 | 修改 |
|------|------|
| `core/register_core_types.cpp` | 添加 include + `GDREGISTER_CLASS(InputActionValue)` + `GDREGISTER_CLASS(InputAction)` |

### 验证方法

```gdscript
# 编辑器控制台或测试脚本
var val = InputActionValue.create_vector2(Vector2(0.5, 0.8))
print(val.get_vector2())       # (0.5, 0.8)
print(val.get_float())         # 0.5
print(val.is_non_zero())       # true

var action = InputAction.new()
action.action_name = "Move"
action.value_type = InputActionValue.VECTOR2
print(action.action_name)      # Move
```

---

## Step 2：InputModifier 基类 + 内置修饰器

> 依赖 Step 1 的 `InputActionValue`（用到 Vector3 值传递）。

### 新建文件

| 文件 | 内容 |
|------|------|
| `core/input/enhanced_input_modifier.h` | `InputModifier` 基类 + 所有内置修饰器声明 |
| `core/input/enhanced_input_modifier.cpp` | 所有实现 |

### 实现清单

**InputModifier** 基类 (`Resource`)
- [ ] 虚方法：`virtual Vector3 modify(Vector3 p_value, float p_delta) const`
- [ ] GDVIRTUAL：`GDVIRTUAL2R(Vector3, _modify, Vector3, float)` — GDScript 可重写
- [ ] `_bind_methods`

**内置修饰器**（全部在同一对 h/cpp 中）

- [ ] `InputModifierDeadZone` — 死区过滤
  - 属性：`lower_threshold(0.2)`, `upper_threshold(1.0)`, `type(AXIAL/RADIAL)`
  - Radial：按 length 计算；Axial：每轴独立计算
- [ ] `InputModifierScalar` — 标量缩放
  - 属性：`scale: Vector3`
  - 实现：`value * scale`（逐分量乘）
- [ ] `InputModifierNegate` — 轴反转
  - 属性：`negate_x`, `negate_y`, `negate_z`: bool
  - 实现：对应轴取负
- [ ] `InputModifierSmooth` — 平滑插值
  - 属性：`speed: float`
  - 内部状态：`current_value: Vector3`
  - 实现：`current_value = current_value.lerp(target, speed * delta)`
- [ ] `InputModifierResponseCurve` — 响应曲线
  - 属性：`curve_x`, `curve_y`, `curve_z`: Ref<Curve>
  - 实现：对每轴绝对值查曲线，保留符号
- [ ] `InputModifierSwizzle` — 轴重映射
  - 属性：`order: SwizzleOrder` 枚举 (YXZ, ZYX, XZY, 等)
  - 实现：按 order 重排 xyz
- [ ] `InputModifierNormalize` — 归一化
  - 实现：`value.normalized()`，length 为 0 时返回零向量
- [ ] `InputModifierClamp` — 值钳制
  - 属性：`min: Vector3`, `max: Vector3`
  - 实现：逐分量 clamp

### 修改文件

| 文件 | 修改 |
|------|------|
| `core/register_core_types.cpp` | 添加 include + `GDREGISTER_VIRTUAL_CLASS(InputModifier)` + 每个内置修饰器的 `GDREGISTER_CLASS` |

### 验证方法

```gdscript
var dz = InputModifierDeadZone.new()
dz.lower_threshold = 0.2
dz.type = InputModifierDeadZone.RADIAL
print(dz.modify(Vector3(0.1, 0, 0), 0.016))  # Vector3(0, 0, 0) — 在死区内
print(dz.modify(Vector3(0.5, 0, 0), 0.016))  # Vector3(~0.375, 0, 0) — 重映射后

var scalar = InputModifierScalar.new()
scalar.scale = Vector3(0, 1, 0)
print(scalar.modify(Vector3(1, 0, 0), 0.016))  # Vector3(0, 0, 0) — W键方向转换
```

---

## Step 3：InputTrigger 基类 + 内置触发器

> 依赖 Step 1 的 `InputActionValue`（用到 Vector3 + TriggerState 枚举）。

### 新建文件

| 文件 | 内容 |
|------|------|
| `core/input/enhanced_input_trigger.h` | `InputTrigger` 基类 + `TriggerState` 枚举 + 所有内置触发器声明 |
| `core/input/enhanced_input_trigger.cpp` | 所有实现 |

### 实现清单

**枚举定义**
- [ ] `TriggerState`：`NONE, ONGOING, TRIGGERED`
- [ ] `TriggerEvent`：`NONE, STARTED, ONGOING, TRIGGERED, COMPLETED, CANCELED`（位掩码）

**InputTrigger** 基类 (`Resource`)
- [ ] 虚方法：`virtual TriggerState update_state(TriggerState p_current, Vector3 p_value, float p_delta) const`
- [ ] GDVIRTUAL：`GDVIRTUAL3R(int, _update_state, int, Vector3, float)`
- [ ] `_bind_methods`：绑定枚举常量

**内置触发器**

- [ ] `InputTriggerDown` — 按下即触发
  - 值非零 → TRIGGERED，值归零 → NONE
- [ ] `InputTriggerPressed` — 按下瞬间触发
  - 上帧 NONE 且当前值非零 → TRIGGERED，之后 → NONE
- [ ] `InputTriggerReleased` — 释放瞬间触发
  - 上帧非 NONE 且当前值归零 → TRIGGERED
- [ ] `InputTriggerHold` — 长按触发
  - 属性：`hold_time(0.5)`, `one_shot(false)`
  - 需要外部传入 elapsed 累计时间（不在 Resource 上存状态）
- [ ] `InputTriggerHoldAndRelease` — 长按后释放触发
  - 属性：`hold_time(0.5)`
  - 按住够时间 → ONGOING，松手后 → TRIGGERED
- [ ] `InputTriggerTap` — 快速点击触发
  - 属性：`tap_time(0.3)`
  - 按住时间 < tap_time 内松手 → TRIGGERED
- [ ] `InputTriggerPulse` — 重复脉冲触发
  - 属性：`interval(0.5)`, `trigger_on_start(true)`
  - 按住期间每隔 interval 输出一次 TRIGGERED
- [ ] `InputTriggerChordAction` — 组合键触发
  - 属性：`chord_action: Ref<InputAction>`
  - 查询 EnhancedInputManager 另一个 Action 是否激活（Step 5 完成后才完整可用，本步先写框架）

### 修改文件

| 文件 | 修改 |
|------|------|
| `core/register_core_types.cpp` | 添加 include + 注册所有触发器类 |

### 验证方法

```gdscript
var trigger = InputTriggerHold.new()
trigger.hold_time = 0.5
# 模拟：按住 0.3 秒
print(trigger.update_state(0, Vector3(1,0,0), 0.3))  # ONGOING
# 继续按住 0.3 秒（总共 0.6 秒）
print(trigger.update_state(1, Vector3(1,0,0), 0.3))  # TRIGGERED
```

### ⚠️ 触发器状态管理说明

触发器本身是 Resource（数据定义），不应该存运行时状态。计时器等运行时数据由 Step 5 的 `EnhancedInputManager` 内部的 `ActionInstance` 管理，通过参数传给 `update_state()`。本步骤中 Hold / Pulse 等需要计时的触发器，先用简单的内部状态实现，Step 5 时重构为外部状态注入。

---

## Step 4：InputActionMapping + InputMappingContext

> 依赖 Step 1（InputAction）、Step 2（InputModifier）、Step 3（InputTrigger）。

### 新建文件

| 文件 | 内容 |
|------|------|
| `core/input/enhanced_input_mapping_context.h` | `InputActionMapping` + `InputMappingContext` 声明 |
| `core/input/enhanced_input_mapping_context.cpp` | 实现 |

### 实现清单

**InputActionMapping** (`Resource`)
- [x] 属性：`action: Ref<InputAction>`
- [x] 属性：`input_event: Ref<InputEvent>` — 复用 Godot 现有 InputEvent
- [x] 属性：`modifiers: TypedArray<InputModifier>` — 修饰器链
- [x] 属性：`triggers: TypedArray<InputTrigger>` — 触发器列表
- [x] 属性：`consume_input: bool`（默认 true）— 从 InputAction 移至此处
- [x] 便捷方法：`add_modifier()`, `add_trigger()`
- [x] `_bind_methods`

**InputMappingContext** (`Resource`)
- [x] 属性：`context_name: StringName`
- [x] 属性：`mappings: TypedArray<InputActionMapping>`
- [x] 方法：`add_mapping()`, `remove_mapping(index)`
- [x] 便捷方法：`map_action(action, event) -> InputActionMapping` — 快速创建并添加映射
- [x] `_bind_methods`

> `description` 已移除。

### 修改文件

| 文件 | 修改 |
|------|------|
| `core/register_core_types.cpp` | 注册 `InputActionMapping` + `InputMappingContext` |

### 验证方法

```gdscript
var ctx = InputMappingContext.new()
ctx.context_name = "OnFoot"

var move = InputAction.new()
move.action_name = "Move"
move.value_type = InputActionValue.VECTOR2

var mapping = ctx.map_action(move, InputEventKey.create_reference(KEY_W))
var scalar = InputModifierScalar.new()
scalar.scale = Vector3(0, 1, 0)
mapping.add_modifier(scalar)

print(ctx.get_mappings().size())  # 1
print(mapping.get_action().action_name)  # Move
print(mapping.get_modifiers().size())  # 1
```

---

## Step 5：EnhancedInputManager — 核心管理器

> 这是最关键的一步，把前四步的所有零件串联成完整管线。
> 依赖 Step 1~4 全部完成。

### 新建文件

| 文件 | 内容 |
|------|------|
| `core/input/enhanced_input_manager.h` | `EnhancedInputManager` 声明 |
| `core/input/enhanced_input_manager.cpp` | 完整实现 |

### 实现清单

**内部数据结构**
- [ ] `ContextEntry { context, priority, enabled }` — 上下文条目
- [ ] `context_stack: Vector<ContextEntry>` — 按 priority 降序排列
- [ ] `MappingInstance` — 每个 InputActionMapping 的运行时状态
  - `current_raw_value: Vector3` — 当前原始输入值
  - `trigger_states: Vector<TriggerState>` — 每个触发器的状态
  - `trigger_elapsed: Vector<float>` — 每个触发器的计时器
- [ ] `ActionInstance` — 每个 InputAction 的运行时状态
  - `action: Ref<InputAction>`
  - `accumulated_value: Vector3` — 累积后的值
  - `trigger_state: TriggerState` — 最终触发状态
  - `last_trigger_state: TriggerState`
  - `elapsed_time: float`

**上下文管理**
- [ ] `push_mapping_context(context, priority)` — 插入并按 priority 排序
- [ ] `pop_mapping_context(context)` — 移除
- [ ] `clear_all_contexts()`
- [ ] `has_mapping_context(context) -> bool`
- [ ] `get_active_contexts() -> TypedArray<InputMappingContext>`
- [ ] 推入/移除时重建内部 MappingInstance 和 ActionInstance

**核心管线：process_input_event(event)**
- [ ] 遍历 context_stack（按优先级从高到低）
- [ ] 对每个上下文的每个 mapping：
  - [ ] 匹配 `event` 和 `mapping.input_event`（用 `InputEvent::action_match` 或类似逻辑）
  - [ ] 提取原始值（key → 1.0, axis → axis_value, mouse_motion → delta）
  - [ ] 跑 Modifier 链：依次调用每个 modifier.modify(value, delta)
  - [ ] 按 AccumulationMode 累积到对应 ActionInstance
  - [x] 如果 mapping.consume_input == true，标记该 event 已消费（按物理按键粒度消费）

**核心管线：tick(delta)**
- [ ] 对每个 ActionInstance：
  - [ ] 遍历其所有 trigger，调用 update_state(state, value, delta)
  - [ ] 合并触发器结果（ANY 模式）
  - [ ] 检测状态变化，发出对应信号

**查询 API**
- [ ] `get_action_value(action) -> InputActionValue`
- [ ] `is_action_triggered(action) -> bool`
- [ ] `get_action_trigger_state(action) -> TriggerState`
- [ ] `get_action_elapsed_time(action) -> float`

**Per-Action 绑定（便捷 API）**
- [x] `bind_action(action, event_type, callable)` — event_type 使用 `TriggerEvent` 枚举，支持自动补全
- [x] `unbind_action(action, event_type, callable)`

**信号**
- [ ] `action_triggered(action, value, state)`
- [ ] `action_started(action, value)`
- [ ] `action_ongoing(action, value)`
- [ ] `action_completed(action, value)`
- [ ] `action_canceled(action, value)`
- [ ] `context_pushed(context)`
- [ ] `context_popped(context)`

### 修改文件

| 文件 | 修改 |
|------|------|
| `core/register_core_types.cpp` | `GDREGISTER_CLASS(EnhancedInputManager)` + `memnew` 创建单例 + 注册到 Engine 单例 + `memdelete` 销毁 |

### 验证方法

```gdscript
var manager = EnhancedInputManager

# 创建 Action 和 Context
var jump = InputAction.new()
jump.action_name = "Jump"
jump.value_type = InputActionValue.BOOL

var ctx = InputMappingContext.new()
var mapping = ctx.map_action(jump, InputEventKey.create_reference(KEY_SPACE))
mapping.add_trigger(InputTriggerPressed.new())

manager.push_mapping_context(ctx, 0)
manager.action_triggered.connect(func(action, value, state):
    print("Triggered: ", action.action_name)
)
# 按空格 → 控制台输出 "Triggered: Jump"
```

---

## Step 6：集成到 Input + SceneTree

> 把 EnhancedInputManager 接入 Godot 的输入事件流和帧循环。

### 修改文件

| 文件 | 修改内容 |
|------|---------|
| `core/input/input.cpp` | 在 `_parse_input_event_impl()` 末尾添加 `EnhancedInputManager::get_singleton()->process_input_event(p_event)` |
| `scene/main/scene_tree.cpp` | 在 `process_frame` 的合适位置调用 `EnhancedInputManager::get_singleton()->tick(delta)` |

### Input::_parse_input_event_impl 集成点

```cpp
// core/input/input.cpp — _parse_input_event_impl() 末尾
void Input::_parse_input_event_impl(const Ref<InputEvent> &p_event, bool p_is_emulated) {
    // ... 现有的处理逻辑 ...

    // 转发给 Enhanced Input 系统
    if (EnhancedInputManager::get_singleton()) {
        EnhancedInputManager::get_singleton()->process_input_event(p_event);
    }
}
```

### SceneTree 集成点

```cpp
// scene/main/scene_tree.cpp — process_frame 相关位置
// 在 emit_signal("process_frame") 之前调用 tick
if (EnhancedInputManager::get_singleton()) {
    EnhancedInputManager::get_singleton()->tick(p_time);
}
```

### 验证方法

完整的端到端测试——创建一个场景，用 Enhanced Input 系统控制角色移动和跳跃，确认：
- [ ] 按键事件能被正确匹配
- [ ] Modifier 链正确变换值
- [ ] Trigger 正确判定触发条件
- [ ] 信号正确发出
- [ ] 上下文切换正常工作（push/pop）
- [ ] consume_input 正确阻止低优先级上下文
- [ ] AccumulationMode 正确合并多个绑定

---

## 文件总结

### 新建文件（6 对 = 12 个文件）

```
core/input/
├── enhanced_input_action_value.h
├── enhanced_input_action_value.cpp
├── enhanced_input_action.h
├── enhanced_input_action.cpp
├── enhanced_input_modifier.h
├── enhanced_input_modifier.cpp
├── enhanced_input_trigger.h
├── enhanced_input_trigger.cpp
├── enhanced_input_mapping_context.h
├── enhanced_input_mapping_context.cpp
├── enhanced_input_manager.h
└── enhanced_input_manager.cpp
```

### 修改文件（3 个）

```
core/register_core_types.cpp   ← Step 1~5 每步都改，累积添加注册代码
core/input/input.cpp            ← Step 6 添加事件转发
scene/main/scene_tree.cpp       ← Step 6 添加 tick 调用
```

### 注意：SCsub 不需要改

`core/input/SCsub` 已有 `env.add_source_files(env.core_sources, "*.cpp")`，会自动收集所有 `.cpp` 文件。

---

## 预估工作量

| Step | 内容 | 复杂度 | 预估 |
|------|------|--------|------|
| Step 1 | InputActionValue + InputAction | ⭐ 简单 | 数据类 + 绑定 |
| Step 2 | InputModifier × 8 | ⭐⭐ 中等 | 8 个小类，逻辑简单 |
| Step 3 | InputTrigger × 8 | ⭐⭐ 中等 | 8 个小类，状态机逻辑 |
| Step 4 | Mapping + Context | ⭐ 简单 | 容器类 + 绑定 |
| Step 5 | EnhancedInputManager | ⭐⭐⭐ 复杂 | 核心管线，信号，状态管理 |
| Step 6 | 集成 | ⭐ 简单 | 两处代码插入 |

建议从 Step 1 开始，逐步推进。准备好了就说，我们直接开始写代码。
