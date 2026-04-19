# Godot 物理平滑插值 (Physics Interpolation) 原理与实践

## 1. 为什么需要物理插值？（背景与痛点）
在游戏引擎中，通常有两个主要的循环：
*   **物理循环 (`_PhysicsProcess`)**：以固定的频率运行（例如默认的 60 Hz，即每 16.67ms 执行一次），以保证物理模拟的稳定性和确定性。
*   **渲染循环 (`_Process`)**：以可变的频率运行（即帧率 FPS，可能高达 144 Hz，也可能因为性能波动而忽高忽低）。

**痛点（Jitter 抖动现象）**：
当显示器的刷新率（如 144Hz）高于物理刷新率（60Hz）时，在两次物理更新之间，会渲染出多个画面。如果没有插值，这些画面中物理对象的位置是完全一样的。玩家看到的画面就是：物体停顿几帧，然后突然跳跃到下一个位置，产生明显的视觉卡顿和抖动（Jitter）。

## 2. 物理插值的核心原理（基于 Godot 源码分析）
物理插值（Physics Interpolation）的核心思想是：**将物理对象的“逻辑位置”和“视觉位置”分离。**

通过查阅 Godot 源码（如 `main/main_timer_sync.cpp` 和 `servers/rendering/renderer_canvas_cull.cpp`），我们可以清晰地看到其底层实现逻辑：

### 2.1 时间比例 (Interpolation Fraction) 的计算
在引擎的主循环中，物理步长（Physics Step）是固定的（例如 1/60 秒），而渲染帧的时间是可变的。
在 `main/main_timer_sync.cpp` 的 `MainTimerSync::advance` 方法中，引擎会计算一个 `time_accum`（累积时间，即距离上一次物理 Tick 过去了多少时间）。
然后计算出插值比例 `fraction`：
```cpp
// main/main_timer_sync.cpp (Line 502)
// p_physics_step 是物理步长 (例如 0.01667s)
ret.interpolation_fraction = time_accum / p_physics_step;
```
这个 `fraction`（范围在 0.0 到 1.0 之间）会被保存到 `Engine::get_singleton()->_physics_interpolation_fraction` 中，供渲染服务器使用。

### 2.2 渲染服务器中的双缓冲变换 (Double Buffered Transform)
在渲染底层（例如 2D 渲染的 `servers/rendering/renderer_canvas_cull.cpp`），每个需要渲染的节点（CanvasItem）都会维护**两个 Transform**：
*   `xform_prev`：上一个物理 Tick 的变换矩阵。
*   `xform_curr`：当前物理 Tick 的变换矩阵。

当渲染一帧时，渲染器会获取当前的 `fraction`，并使用 `TransformInterpolator` 在这两个矩阵之间进行插值：
```cpp
// servers/rendering/renderer_canvas_cull.cpp (Line 355)
real_t f = Engine::get_singleton()->get_physics_interpolation_fraction();
// 在 xform_prev 和 xform_curr 之间根据比例 f 进行插值，得到最终用于渲染的 self_xform
TransformInterpolator::interpolate_transform_2d(ci->xform_prev, ci->xform_curr, self_xform, f);
```
这就是为什么 Godot 的插值是**“回顾过去”**（在 N-1 和 N 之间插值），而不是预测未来。它在视觉上会延迟一个物理 Tick，但保证了绝对的平滑和准确。

## 3. Godot 中的物理插值演进
*   **Godot 3.x**：在 3.5 版本中引入了官方的 3D 物理插值，2D 也有相应的支持。
*   **Godot 4.x**：
    *   Godot 4.0 - 4.2 期间，由于底层架构重构，物理插值暂时缺失，开发者只能手动写代码在 `_Process` 中插值。
    *   **Godot 4.3**：官方正式在核心层重新引入了 **2D 物理插值**。
    *   **Godot 4.4**：官方正式引入了 **3D 物理插值**。

## 4. 如何在 Godot 4 中使用物理插值？

### 4.1 开启插值
1.  打开 **项目设置 (Project Settings)**。
2.  搜索 `interpolation`。
3.  在 `Physics -> Common` 下，勾选 **Physics Interpolation**（全局开启）。

### 4.2 节点控制
所有的 `Node2D` 和 `Node3D` 都有一个属性：`physics_interpolation_mode`。
*   **Inherit (继承)**：默认值，跟随父节点的设置。
*   **On (开启)**：强制开启该节点的插值。
*   **Off (关闭)**：关闭该节点的插值（例如某些不需要平滑的 UI 元素或特殊逻辑节点）。

### 4.3 关键 API：`ResetPhysicsInterpolation()` 源码揭秘
这是使用物理插值时**最容易踩坑**的地方！
当你**瞬间移动（Teleport）**一个物体时（例如玩家复活、穿过传送门），由于插值的存在，引擎会试图在“旧位置”和“新位置”之间平滑过渡，导致玩家看到物体“嗖”地一下飞过整个地图。

**解决方案**：在手动修改物体位置后，必须立刻调用重置函数 `ResetPhysicsInterpolation()`。

**底层原理**：
当我们调用这个函数时，底层到底发生了什么？查看 `servers/rendering/renderer_canvas_cull.cpp` 中的实现：
```cpp
// servers/rendering/renderer_canvas_cull.cpp (Line 2062)
void RendererCanvasCull::canvas_item_reset_physics_interpolation(RID p_item) {
    Item *canvas_item = canvas_item_owner.get_or_null(p_item);
    ERR_FAIL_NULL(canvas_item);
    // 核心逻辑：直接把当前的 Transform 赋值给上一个 Transform
    canvas_item->xform_prev = canvas_item->xform_curr;
}
```
非常简单粗暴！它直接让 `xform_prev` 等于 `xform_curr`。这样一来，在渲染层调用 `interpolate_transform_2d` 时，起点和终点是同一个位置，插值结果自然也就是这个新位置，从而完美实现了“视觉上的瞬间移动”，消除了滑动残影。

## 5. 总结
物理插值是现代动作游戏、平台跳跃游戏提升画面流畅度的必备技术。Godot 4.3+ 将其内置到了底层，开发者只需全局开启，并在“瞬移”逻辑处注意调用 `ResetPhysicsInterpolation()`，即可零成本获得丝滑的视觉体验。
