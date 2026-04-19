# Godot 物理平滑插值（Physics Interpolation / FTI）底层原理

> 本文基于 Godot 引擎源码分析，深入讲解物理平滑插值（Fixed Timestep Interpolation, FTI）的底层实现原理。

---

## 目录

1. [为什么需要物理插值](#1-为什么需要物理插值)
2. [核心架构总览](#2-核心架构总览)
3. [插值分数的计算](#3-插值分数的计算)
4. [Transform 插值的数学实现](#4-transform-插值的数学实现)
5. [3D 插值的完整数据流](#5-3d-插值的完整数据流)
6. [2D 插值的实现差异](#6-2d-插值的实现差异)
7. [节点数据结构](#7-节点数据结构)
8. [SceneTreeFTI 调度核心](#8-scenetreefti-调度核心)
9. [控制流程：开启与关闭](#9-控制流程开启与关闭)
10. [重置机制](#10-重置机制)
11. [客户端插值](#11-客户端插值)
12. [关键源码文件索引](#12-关键源码文件索引)

---

## 1. 为什么需要物理插值

Godot 的物理引擎以**固定频率**运行（默认 60Hz），而渲染帧率是**可变的**（可能是 30fps、60fps、144fps 甚至更高）。这就导致了一个根本矛盾：

```
物理帧:   |----T0----|----T1----|----T2----|----T3----|
渲染帧:   |--R0--|--R1--|--R2--|--R3--|--R4--|--R5--|--R6--|
```

如果不做插值，渲染帧只能使用**最近一次物理帧**的位置数据。当渲染帧率高于物理帧率时，多个渲染帧会显示相同的位置，导致**视觉抖动（jitter）**和**不流畅感**。

**物理插值的核心思想**：在两个相邻物理帧的状态之间，根据当前渲染时刻所处的"位置"，**线性插值**出一个平滑的中间状态，用于渲染。

---

## 2. 核心架构总览

Godot 的 FTI 系统分为**三个层次**：

```
┌─────────────────────────────────────────────────────┐
│              Layer 1: 时间同步层                      │
│         MainTimerSync (main/main_timer_sync.cpp)     │
│         计算 interpolation_fraction ∈ [0, 1)         │
├─────────────────────────────────────────────────────┤
│              Layer 2: 场景树管理层                     │
│         SceneTreeFTI (scene/main/scene_tree_fti.cpp) │
│         管理节点的 prev/curr 变换、调度插值更新          │
├─────────────────────────────────────────────────────┤
│              Layer 3: 渲染同步层                      │
│   RenderingServer / RendererCanvasCull               │
│   接收插值后的 transform，执行最终渲染                  │
└─────────────────────────────────────────────────────┘
```

**关键原则**：3D 使用 `SceneTreeFTI` 实现完整的逐节点插值；2D 使用 `RendererCanvasCull` 在渲染服务器层级处理，两者**不共享插值代码**。

---

## 3. 插值分数的计算

### 3.1 什么是插值分数

插值分数（interpolation fraction）是一个 `[0, 1)` 范围内的浮点数，表示当前渲染时刻在两个物理帧之间的"进度"：

- `fraction = 0.0` → 正好在上一个物理帧的时刻
- `fraction = 0.5` → 恰好在两个物理帧的中间
- `fraction → 1.0` → 接近下一个物理帧的时刻

### 3.2 计算公式

核心计算位于 `main/main_timer_sync.cpp`（约第 502 行）：

```cpp
interpolation_fraction = time_accum / p_physics_step;
```

其中：
- `time_accum`：自上次物理 tick 以来累积的时间
- `p_physics_step`：物理步长（1.0 / physics_fps，默认 1/60 秒）

计算完成后，存入引擎全局变量，供所有系统使用：

```cpp
// main/main.cpp (约第 4918 行)
Engine::get_singleton()->_physics_interpolation_fraction = interpolation_fraction;
```

### 3.3 时序图示

```
时间轴:
        物理帧 N          物理帧 N+1
           │                  │
           ├──────────────────┤
           │    p_physics_step│
           │                  │
           │◄─ time_accum ─►│ │
           │                 ↑ │
           │            渲染帧 R
           │                  │
           fraction = time_accum / p_physics_step
```

---

## 4. Transform 插值的数学实现

核心实现位于 `core/math/transform_interpolator.cpp`。

### 4.1 2D Transform 插值

```cpp
// core/math/transform_interpolator.cpp:36-47
void TransformInterpolator::interpolate_transform_2d(
    const Transform2D &p_prev, const Transform2D &p_curr,
    Transform2D &r_result, real_t p_fraction)
{
    // 检测镜像翻转：行列式符号改变时，不插值基矩阵
    if (_sign(p_prev.determinant()) != _sign(p_curr.determinant())) {
        // 镜像翻转会导致基矩阵插值出现严重变形
        // 仅插值位置，基矩阵直接使用当前帧
        r_result = p_curr;
        r_result.columns[2] = p_prev.columns[2] +
            (p_curr.columns[2] - p_prev.columns[2]) * p_fraction;
        return;
    }
    // 正常情况：位置和基矩阵都做线性插值
    r_result = p_prev.interpolate_with(p_curr, p_fraction);
}
```

**关键设计**：当检测到行列式符号翻转（如从正面翻到反面），说明发生了"镜像"变换。此时基矩阵的插值会导致严重畸变（矩阵收缩到零再展开），所以只插值位置。

### 4.2 3D Transform 插值

3D 的插值更复杂，因为 Basis（3×3 矩阵）可能包含旋转、缩放、错切等多种变换。

```cpp
// core/math/transform_interpolator.cpp:49-51
void TransformInterpolator::interpolate_transform_3d(
    const Transform3D &p_prev, const Transform3D &p_curr,
    Transform3D &r_result, real_t p_fraction)
{
    // 位置：始终线性插值
    r_result.origin = p_prev.origin +
        (p_curr.origin - p_prev.origin) * p_fraction;

    // 基矩阵：动态选择插值方法
    _interpolate_basis(p_prev.basis, p_curr.basis,
                       r_result.basis, p_fraction);
}
```

### 4.3 Basis 插值方法的动态选择

引擎会根据基矩阵的特征**自动选择**最佳插值方法，在 `_test_basis()` 函数中（约第 241-300 行）：

```
输入 Basis
    │
    ├─ 行列式 ≈ 0？ ──→ LERP（退化矩阵，无法安全处理）
    │
    ├─ 任意轴长度 < 0.00001？ ──→ LERP（近零缩放，数值不稳定）
    │
    ├─ 是否正交？─── 否 ──→ LERP（有错切，SLERP 不适用）
    │       │
    │      是
    │       │
    ├─ 两帧缩放差异 < 0.001？
    │       │           │
    │      是           否
    │       │           │
    │    SLERP    SCALED_SLERP
    │  (纯旋转)  (归一化轴→SLERP旋转，
    │            单独LERP缩放)
    └──────────────────────────
```

三种方法的特点：

| 方法 | 适用场景 | 原理 |
|------|---------|------|
| **LERP** | 退化/错切矩阵 | 逐分量线性插值，最安全但可能不保持长度 |
| **SLERP** | 纯旋转（正交+等缩放） | 转为四元数做球面线性插值，保持旋转恒速 |
| **SCALED_SLERP** | 旋转+非均匀缩放 | 先归一化各轴→SLERP旋转，再单独 LERP 各轴缩放 |

**注意**：插值方法是**每帧每节点**实时计算的，不做缓存。这确保了正确性，但也意味着每帧都有一定的分析开销。

---

## 5. 3D 插值的完整数据流

### 5.1 三阶段流水线

```
阶段 1: Tick 更新（物理帧）
    MainTimerSync → SceneTreeFTI::tick_update()
    │
    ├─ "泵送"(Pump): local_transform → local_transform_prev
    │   （保存上一帧的变换作为插值起点）
    │
    ├─ 执行 _physics_process()
    │   （物理过程可能修改 local_transform）
    │
    └─ 此时: prev = 上一帧位置, curr = 本帧新位置

阶段 2: Frame 更新（渲染帧）— 第一遍
    SceneTreeFTI::frame_update(frame_start=true)
    │
    ├─ 获取 interpolation_fraction
    │
    ├─ 遍历脏节点（按深度排序）
    │   │
    │   ├─ 计算 local_interp = lerp(prev, curr, fraction)
    │   │
    │   ├─ global_transform_interpolated =
    │   │     parent.global_transform_interpolated * local_interp
    │   │
    │   └─ 设置 fti_global_xform_interp_set 标志
    │
    └─ 确保所有节点的全局插值变换计算完毕

阶段 3: Frame 更新（渲染帧）— 第二遍
    SceneTreeFTI::frame_update(frame_start=false)
    │
    ├─ 同步到 RenderingServer:
    │   VisualInstance3D::fti_update_servers_xform()
    │   → RenderingServer::instance_set_transform(
    │         instance, global_transform_interpolated)
    │
    └─ 更新插值属性
```

### 5.2 泵送（Pump）机制

泵送是 FTI 系统最关键的操作之一：

```cpp
// scene/3d/node_3d.cpp:389-391
void Node3D::fti_pump_xform() {
    data.local_transform_prev = data.local_transform;
}
```

**泵送时机至关重要**：在 `tick_update()` 中，泵送发生在物理处理**之前**。如果在物理处理之后泵送，prev 和 curr 会相同，插值就失效了。

```
正确顺序:
  pump (prev = curr的旧值) → physics_process (curr 被更新) → 现在 prev ≠ curr ✓

错误顺序:
  physics_process (curr 被更新) → pump (prev = curr的新值) → prev == curr ✗
```

### 5.3 层级变换计算

全局插值变换是**层级累乘**的：

```cpp
// scene/main/scene_tree_fti.cpp (约第 545-576 行)
void SceneTreeFTI::_update_dirty_nodes(...) {
    // 计算本地插值变换
    Transform3D local_interp;
    TransformInterpolator::interpolate_transform_3d(
        node->data.local_transform_prev,
        node->data.local_transform,
        local_interp, fraction);

    // 与父节点的全局插值变换相乘
    if (parent && parent->data.fti_global_xform_interp_set) {
        node->data.global_transform_interpolated =
            parent->data.global_transform_interpolated * local_interp;
    } else {
        node->data.global_transform_interpolated =
            parent->get_global_transform() * local_interp;
    }
}
```

**优化**：如果节点的变换是单位矩阵（`fti_is_identity_xform` 标志），跳过矩阵乘法。

---

## 6. 2D 插值的实现差异

2D 和 3D 使用**完全不同**的插值路径：

| 方面 | 3D | 2D |
|------|----|----|
| 管理层 | SceneTreeFTI（场景树层） | RendererCanvasCull（渲染服务器层） |
| 数据存储 | Node3D 内部的 prev/curr | RendererCanvasCull::InterpolationData |
| 插值时机 | frame_update 时预计算 | **渲染时**实时计算 |
| 列表管理 | ping-pong 双缓冲列表 | curr/prev 两个 RID 列表 |

2D 的实现位于 `RendererCanvasCull`：

```
RendererCanvasCull::InterpolationData
    ├─ canvas_item_curr_list   (当前帧变换的 RID 列表)
    ├─ canvas_item_prev_list   (上一帧变换的 RID 列表)
    └─ on_interpolate_transform_list (需要插值的项目列表)

每个 Tick:
    update_interpolation_tick()
    → 遍历列表，xform_prev = xform_curr（泵送）

每个 渲染帧:
    使用 Engine::get_singleton()->get_physics_interpolation_fraction()
    在渲染时实时插值
```

特殊节点如 `Camera2D` 和 `CPUParticles2D` 还实现了自己的 `_interpolation_data` 结构来处理额外的插值需求。

---

## 7. 节点数据结构

### 7.1 Node3D 的 FTI 数据

```cpp
// scene/3d/node_3d.h:105-172
struct Data {
    // 核心变换数据
    Transform3D local_transform;                    // 当前帧的本地变换
    Transform3D local_transform_prev;               // 上一帧的本地变换（插值起点）
    Transform3D global_transform_interpolated;      // 计算后的全局插值变换

    // FTI 标志位（共 8 个 bit flag）
    bool fti_on_tick_xform_list : 1;                // 是否在 tick 变换列表中
    bool fti_global_xform_interp_set : 1;           // 全局插值变换是否已计算
    bool fti_is_identity_xform : 1;                 // 变换是否为单位矩阵
    // ... 其他标志
};
```

### 7.2 客户端插值数据

```cpp
// scene/3d/node_3d.h:93-98
struct ClientPhysicsInterpolationData {
    Transform3D global_xform_curr;    // 当前全局变换
    Transform3D global_xform_prev;    // 上一帧全局变换
    uint64_t current_physics_tick;    // 当前物理 tick 编号
    // 256-tick 超时机制
};
```

---

## 8. SceneTreeFTI 调度核心

`SceneTreeFTI` 是 3D 物理插值的调度中心。

### 8.1 核心数据结构

```cpp
// scene/main/scene_tree_fti.h:70-171
class SceneTreeFTI {
    struct Data {
        // Ping-pong 双缓冲：tick 变换列表
        LocalVector<Node3D *> tick_xform_list[2];

        // 帧变换列表
        LocalVector<Node3D *> frame_xform_list;
        LocalVector<Node3D *> frame_xform_list_forced;

        // Tick 属性列表（双缓冲）
        LocalVector<Node *> tick_property_list[2];

        // 深度列表优化：按树深度分桶（最大 48 层）
        LocalVector<Node3D *> dirty_node_depth_lists[48];
    };
};
```

### 8.2 深度列表优化

场景树的深度上限为 **48 层**。脏节点按其在树中的深度分桶存储：

```
depth_lists[0]: [根节点附近的脏节点...]
depth_lists[1]: [深度1的脏节点...]
depth_lists[2]: [深度2的脏节点...]
  ...
depth_lists[47]: [最深层的脏节点...]
```

**优势**：只需遍历脏节点及其祖先，而不是整棵场景树。从浅到深依次处理，确保父节点总是在子节点之前完成插值计算。

### 8.3 两种遍历模式

```cpp
// scene/main/scene_tree_fti.cpp (约第 640-655 行)
if (use_optimized_mode) {
    // 优化模式：只遍历脏节点的深度列表
    _update_dirty_nodes(data, ..., true);  // active=true
} else {
    // 遗留模式：遍历整棵场景树
    _update_dirty_nodes(data, ..., false); // active=false
}
```

### 8.4 帧更新的两遍策略

```
第一遍 (frame_start=true):
    ├─ 计算所有脏节点的 global_transform_interpolated
    ├─ 设置 fti_global_xform_interp_set 标志
    └─ 确保整棵树的插值变换一致

第二遍 (frame_start=false):
    ├─ 将插值结果同步到 RenderingServer
    │   VisualInstance3D::fti_update_servers_xform()
    │   → RS::instance_set_transform(instance, global_transform_interpolated)
    └─ 更新插值属性
```

两遍策略确保 RenderingServer 在一帧内看到**完整一致**的变换数据。

---

## 9. 控制流程：开启与关闭

FTI 采用**三级层次**控制：

```
项目级别 (Project Settings)
    │  physics/common/physics_interpolation = true/false
    │  存储在: SceneTree::_physics_interpolation_enabled_in_project
    │
    ├─ SceneTree 级别
    │      set_physics_interpolation_enabled(bool)
    │      → 调用 scene_tree_fti.set_enabled()
    │      → 传播重置到所有节点
    │
    └─ 节点级别 (Node)
           set_physics_interpolation_mode(mode)
           │
           ├─ INHERIT: 继承父节点设置
           ├─ ON:      强制开启
           └─ OFF:     强制关闭
           存储为 2-bit 字段: data.physics_interpolation_mode
```

```cpp
// scene/main/node.cpp:942-972
void Node::set_physics_interpolation_mode(PhysicsInterpolationMode p_mode) {
    if (data.physics_interpolation_mode == p_mode) return;
    data.physics_interpolation_mode = p_mode;
    // 传播到子节点（INHERIT 模式会向上查找）
    _propagate_physics_interpolation_mode_changed();
}
```

---

## 10. 重置机制

当节点"瞬移"（如场景切换、位置突变）时，需要**重置插值**，否则会出现物体从旧位置"滑动"到新位置的视觉伪影。

### 10.1 触发时机

- 节点**首次进入场景树**
- 插值模式**改变**
- **暂停/挂起**恢复
- 手动调用 `reset_physics_interpolation()`

### 10.2 实现流程

```cpp
// 1. 节点请求重置
node->_set_physics_interpolation_reset_requested(true);

// 2. SceneTreeFTI 在 tick 开始时处理
// scene/main/scene_tree_fti.cpp:203-218
void SceneTreeFTI::_update_request_resets() {
    // 向可见节点发送通知
    node->notification(NOTIFICATION_RESET_PHYSICS_INTERPOLATION);
}

// 3. 节点响应通知：将 prev 设为 curr
// 效果：fraction 乘以 (curr - prev) = (curr - curr) = 0
// 即：没有插值偏移，物体直接出现在新位置
```

---

## 11. 客户端插值

`Node3D` 提供了 `get_global_transform_interpolated()` 方法，供用户在脚本中获取插值后的全局变换。

```cpp
// scene/3d/node_3d.cpp:489-532
Transform3D Node3D::_get_global_transform_interpolated() {
    // 懒分配 ClientPhysicsInterpolationData
    if (!data.client_physics_interpolation_data) {
        data.client_physics_interpolation_data = memnew(ClientPhysicsInterpolationData);
    }

    // 更新客户端数据
    update_client_physics_interpolation_data();

    // 使用引擎的 interpolation_fraction 进行插值
    Transform3D result;
    TransformInterpolator::interpolate_transform_3d(
        data.client_physics_interpolation_data->global_xform_prev,
        data.client_physics_interpolation_data->global_xform_curr,
        result,
        Engine::get_singleton()->get_physics_interpolation_fraction());
    return result;
}
```

### 超时与瞬移处理

```cpp
// scene/3d/node_3d.cpp:463-466
// 如果超过 256 个 tick 没有更新，认为是瞬移
if (tick_diff > 256) {
    // 直接使用当前变换，不做插值（防止从很远的旧位置滑过来）
    cpid->global_xform_prev = cpid->global_xform_curr;
}
```

---

## 12. 关键源码文件索引

| 文件路径 | 角色 |
|---------|------|
| `main/main_timer_sync.cpp` | 计算 interpolation_fraction |
| `main/main.cpp` | 主循环，设置全局 fraction 值 |
| `core/math/transform_interpolator.cpp` | 2D/3D Transform 插值数学实现 |
| `scene/main/scene_tree_fti.h/.cpp` | FTI 调度核心，管理脏节点和遍历 |
| `scene/main/scene_tree.cpp` | SceneTree 级别的 FTI 开关控制 |
| `scene/main/node.cpp` | 节点级别的插值模式管理 |
| `scene/3d/node_3d.h/.cpp` | 3D 节点的 prev/curr 数据存储、泵送、客户端插值 |
| `scene/3d/visual_instance_3d.cpp` | 将插值变换同步到 RenderingServer |
| `servers/rendering/renderer_canvas_cull.cpp` | 2D Canvas 项的插值处理 |
| `scene/2d/camera_2d.cpp` | Camera2D 自定义插值数据 |
| `scene/2d/cpu_particles_2d.cpp` | CPUParticles2D 自定义插值数据 |

---

## 总结

Godot 的物理平滑插值系统是一个精心设计的多层架构：

1. **时间层**精确计算每个渲染帧在物理帧之间的位置（fraction）
2. **场景层**通过"泵送"机制维护每个节点的前后两帧变换
3. **数学层**根据矩阵特征自动选择最优插值方法（LERP/SLERP/SCALED_SLERP）
4. **渲染层**接收插值后的变换完成最终绘制

这套系统在保证**视觉流畅性**的同时，通过深度列表优化、单位矩阵跳过、脏节点跟踪等手段将**性能开销降到最低**。
