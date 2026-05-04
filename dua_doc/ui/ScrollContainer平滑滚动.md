# ScrollContainer 平滑滚动改造

> **日期**: 2026/4/2  
> **影响范围**: 仅 ScrollContainer，默认关闭，不影响现有行为

---

## 一、修改目的

Godot 原版的 `ScrollContainer` 在所有滚动路径上都是**瞬间跳转**（鼠标滚轮、Pan 手势、API 调用、`ensure_control_visible`），触摸拖拽的惯性减速也使用了硬编码的线性衰减（`-1000px/s²`），手感生硬。

本次改造为 ScrollContainer 增加了**全路径平滑滚动**支持：

- 鼠标滚轮 → 平滑动画到目标位置，连续滚动时目标值累积
- Pan 手势（触控板双指滑动）→ 同上
- `ensure_control_visible()` → 焦点跟随时平滑移动过去
- `set_h_scroll()` / `set_v_scroll()` → API 和 Inspector 设值也经过平滑
- 触摸拖拽惯性 → 从线性减速改为指数衰减，手感接近 iOS/Android

---

## 二、改动文件清单

| 文件 | 改动类型 | 说明 |
|------|----------|------|
| `scene/gui/scroll_container.h` | 修改 | 新增平滑滚动属性、状态变量、方法声明 |
| `scene/gui/scroll_container.cpp` | 修改 | 核心实现：平滑动画、指数衰减惯性、所有输入路径改造 |
| `doc/classes/ScrollContainer.xml` | 修改 | 新增三个属性的英文文档（编辑器悬浮提示） |

---

## 三、新增属性

在 Inspector 中新增了 **"Smooth Scrolling"** 属性组：

| 属性 | 类型 | 默认值 | 范围 | 说明 |
|------|------|--------|------|------|
| `smooth_scroll_enabled` | `bool` | `false` | — | 总开关，关闭时所有行为与原版完全一致 |
| `smooth_scroll_speed` | `float` | `12.0` | `[1, 50]` | 插值速度因子，控制动画逼近目标的快慢 |
| `smooth_scroll_friction` | `float` | `5.0` | `[0.1, 20]` | 指数衰减率，控制触摸惯性减速快慢 |

内部还有一个不暴露的常量 `smooth_scroll_min_velocity = 10.0f`，作为惯性停止的速度阈值。

### 属性中文说明（编辑器暂无中文翻译，参考此表）

| 属性 | 中文说明 |
|------|----------|
| `smooth_scroll_enabled` | 是否启用平滑滚动。开启后，所有滚动操作（鼠标滚轮、Pan 手势、API 调用、`ensure_control_visible`）都会以动画方式平滑移动到目标位置，而不是瞬间跳转。触摸拖拽惯性也会从线性减速改为指数衰减。 |
| `smooth_scroll_speed` | 平滑滚动的插值速度。值越大动画越快到达目标。默认值 `12.0` 表示约 83ms 到达目标距离的 63%，约 250ms 基本到位。仅在 `smooth_scroll_enabled` 开启时生效。 |
| `smooth_scroll_friction` | 触摸拖拽惯性的指数衰减率。值越大惯性停得越快。仅影响触摸拖拽松手后的惯性滑行，不影响鼠标滚轮和 Pan 手势。速度半衰期约为 `ln(2) / friction` 秒，例如：`5.0` → 半衰期约 0.14 秒，`2.0` → 约 0.35 秒（滑得更远）。仅在 `smooth_scroll_enabled` 开启时生效。 |

---

## 四、具体修改详解

### 4.1 新增核心方法

#### `_smooth_scroll_towards(const Vector2 &p_target)`

设置平滑滚动的目标位置并启动动画。

```cpp
void ScrollContainer::_smooth_scroll_towards(const Vector2 &p_target) {
    target_scroll_position = Vector2(
            CLAMP(p_target.x, h_scroll->get_min(), h_scroll->get_max() - h_scroll->get_page()),
            CLAMP(p_target.y, v_scroll->get_min(), v_scroll->get_max() - v_scroll->get_page()));
    smooth_scrolling = true;
    set_process_internal(true);
}
```

**含义**：
- 对目标位置做 CLAMP，确保不会超出滚动范围
- 设置 `smooth_scrolling = true` 标记动画激活
- 开启 `INTERNAL_PROCESS`，每帧驱动动画

#### `_animate_smooth_scroll(double p_delta)`

每帧调用，将当前滚动位置向目标位置插值逼近。

```cpp
void ScrollContainer::_animate_smooth_scroll(double p_delta) {
    Vector2 current = Vector2(h_scroll->get_value(), v_scroll->get_value());
    float factor = MIN(1.0f, (float)(smooth_scroll_speed * p_delta));
    Vector2 new_pos = current.lerp(target_scroll_position, factor);

    // 距离小于 0.5px 时 snap 到目标，避免无限逼近
    if (Math::abs(new_pos.x - target_scroll_position.x) < 0.5f) {
        new_pos.x = target_scroll_position.x;
    }
    if (Math::abs(new_pos.y - target_scroll_position.y) < 0.5f) {
        new_pos.y = target_scroll_position.y;
    }
    // ... 设置滚动值，到达目标后关闭动画
}
```

**算法原理**：

使用**指数逼近 (Exponential Approach)** 插值，这是游戏开发中最常用的平滑跟随算法：

$$
\text{new\_pos} = \text{lerp}(\text{current}, \text{target}, \min(1,\ \text{speed} \times \Delta t))
$$

等价于每帧将剩余距离缩小为原来的 $(1 - \text{speed} \times \Delta t)$。特性：

- **距离越远，移动越快**（因为每帧移动的是剩余距离的固定比例）
- **接近目标时自然减速**（剩余距离越小，每帧移动量越小）
- **帧率无关**（乘以了 `p_delta`）
- 当距离 < 0.5px 时直接 snap，避免浮点精度导致的无限微小移动

`smooth_scroll_speed` 的直觉含义：大约 `1/speed` 秒到达目标的 63%（一个时间常数）。默认 `12.0` 意味着约 83ms 到达 63%，约 250ms 基本到位。

---

### 4.2 修改鼠标滚轮处理

**位置**: `gui_input()` 中处理 `MouseButton::WHEEL_*` 的部分

**原版行为**：
```cpp
// 直接调用 scroll()，瞬间跳转
h_scroll->scroll(-h_scroll->get_page() / ScrollBar::PAGE_DIVISOR * mb->get_factor());
```

**改后行为**：
```cpp
// 先收集所有方向的 delta
Vector2 scroll_delta;
scroll_delta.y -= v_scroll->get_page() / ScrollBar::PAGE_DIVISOR * mb->get_factor();

// 平滑模式：累积到目标位置
if (smooth_scroll_enabled) {
    if (!smooth_scrolling) {
        target_scroll_position = Vector2(h_scroll->get_value(), v_scroll->get_value());
    }
    _smooth_scroll_towards(target_scroll_position + scroll_delta);
}
```

**关键设计 — 目标值累积**：

如果用户快速连续滚动（例如滚轮连转3格），每次都是在**当前 target 基础上累加**，而不是在当前实际位置上累加。这样：
- 快速滚动时不会"吃掉"滚动量
- 动画还没完成时继续滚，总滚动距离是精确的
- 体感上像是"加速"——越滚越快

---

### 4.3 修改 Pan 手势处理

**位置**: `gui_input()` 中处理 `InputEventPanGesture` 的部分

改动逻辑与鼠标滚轮完全一致：收集 delta → 平滑模式下累积到目标 → 非平滑模式保持原行为。

---

### 4.4 修改 `ensure_control_visible()`

**位置**: `ensure_control_visible()` 函数末尾

**原版**：
```cpp
set_h_scroll(get_h_scroll() + diff.x);  // 瞬间跳转
set_v_scroll(get_v_scroll() + diff.y);
```

**改后**：
```cpp
if (smooth_scroll_enabled) {
    Vector2 target = Vector2(get_h_scroll() + diff.x, get_v_scroll() + diff.y);
    _smooth_scroll_towards(target);
} else {
    set_h_scroll(get_h_scroll() + diff.x);
    set_v_scroll(get_v_scroll() + diff.y);
}
```

**含义**：当 `follow_focus = true` 时，Tab 键切换焦点会触发 `ensure_control_visible`。平滑模式下，视口会流畅地滑动到焦点控件的位置，而不是瞬间跳过去。

---

### 4.5 修改 `set_h_scroll()` / `set_v_scroll()`

**位置**: 这两个公共 API 方法

**原版**：
```cpp
void ScrollContainer::set_h_scroll(int p_pos) {
    h_scroll->set_value(p_pos);
    _cancel_drag();
}
```

**改后**：
```cpp
void ScrollContainer::set_h_scroll(int p_pos) {
    if (smooth_scroll_enabled) {
        Vector2 target = Vector2(p_pos, smooth_scrolling ? target_scroll_position.y : v_scroll->get_value());
        _smooth_scroll_towards(target);
    } else {
        h_scroll->set_value(p_pos);
        _cancel_drag();
    }
}
```

**含义**：通过代码或 Inspector 设置滚动位置时也会走平滑动画。注意如果另一个轴正在动画中，会保持其目标值不变（`smooth_scrolling ? target_scroll_position.y : v_scroll->get_value()`）。

---

### 4.6 修改触摸惯性减速算法

**位置**: `_notification(NOTIFICATION_INTERNAL_PROCESS)` 中 `drag_touching_deaccel` 分支

**原版（线性减速）**：
```cpp
float val_x = Math::abs(drag_speed.x);
val_x -= 1000 * get_process_delta_time();  // 硬编码 1000px/s²
```

**改后（指数衰减，仅 smooth_scroll_enabled 时）**：
```cpp
if (smooth_scroll_enabled) {
    drag_speed *= Math::exp(-smooth_scroll_friction * (float)get_process_delta_time());
    if (Math::abs(drag_speed.x) < smooth_scroll_min_velocity) {
        drag_speed.x = 0;
        turnoff_h = true;
    }
    // ... y 轴同理
}
```

**算法原理 — 指数衰减 vs 线性减速**：

| | 线性减速（原版） | 指数衰减（改后） |
|---|---|---|
| 公式 | $v(t) = v_0 - a \cdot t$ | $v(t) = v_0 \cdot e^{-k \cdot t}$ |
| 物理类比 | 恒定摩擦力（干摩擦） | 速度相关阻力（流体阻力） |
| 停止方式 | 某一帧速度变负，突然停止 | 渐进趋近零，自然消失 |
| 速度依赖 | 高速低速同样减速，快时太慢，慢时太突然 | 高速时减速快，低速时减速慢 |
| 手感 | 机械、生硬 | 接近 iOS/Android 原生滚动 |

公式展开：
$$
v(t + \Delta t) = v(t) \cdot e^{-k \cdot \Delta t}
$$

其中 $k$ 就是 `smooth_scroll_friction`（默认 5.0）。$k$ 越大衰减越快。半衰期为 $T_{1/2} = \frac{\ln 2}{k} \approx \frac{0.693}{5} \approx 0.14$ 秒。

当速度低于 `smooth_scroll_min_velocity`（10px/s）时停止，避免长时间微小漂移。

**非平滑模式保持原版线性减速不变。**

---

### 4.7 修改 `_cancel_drag()`

**位置**: `_cancel_drag()` 函数

**原版**：
```cpp
set_process_internal(false);  // 总是关闭 internal process
```

**改后**：
```cpp
if (!smooth_scrolling) {
    set_process_internal(false);  // 仅在没有平滑动画时关闭
}
```

**含义**：拖拽取消时，如果平滑滚动动画仍在进行（比如滚轮触发的动画），不能关掉 `INTERNAL_PROCESS`，否则动画会冻结。

---

### 4.8 `INTERNAL_PROCESS` 中新增平滑动画驱动

**位置**: `_notification(NOTIFICATION_INTERNAL_PROCESS)` 开头

```cpp
if (smooth_scrolling && !drag_touching) {
    _animate_smooth_scroll(get_process_delta_time());
}
```

**含义**：每帧驱动平滑滚动动画。条件 `!drag_touching` 确保用户正在触摸拖拽时不会同时播放动画（此时由拖拽逻辑接管）。

---

## 五、状态机总览

```
                    ┌─────────────────┐
     滚轮/Pan/API   │  设置 target    │
    ─────────────►  │  smooth_scrolling│= true
                    │  set_process     │
                    └────────┬────────┘
                             │
                             ▼
                    ┌─────────────────┐
     每帧 INTERNAL  │ _animate_smooth │
     PROCESS        │ lerp → target   │
                    │ 到达? → 关闭    │
                    └────────┬────────┘
                             │ 到达目标
                             ▼
                    ┌─────────────────┐
                    │smooth_scrolling │= false
                    │set_process_internal(false)
                    └─────────────────┘

     触摸拖拽惯性走独立的 drag_touching_deaccel 分支，
     使用指数衰减替代原线性减速。
```

---

## 六、参数调节指南

| 想要的效果 | 调什么 |
|-----------|--------|
| 动画更快/更"跟手" | 增大 `smooth_scroll_speed`（如 20-30） |
| 动画更慢/更"丝滑" | 减小 `smooth_scroll_speed`（如 5-8） |
| 触摸惯性停得更快 | 增大 `smooth_scroll_friction`（如 8-10） |
| 触摸惯性滑得更远 | 减小 `smooth_scroll_friction`（如 2-3） |
| 完全关闭，恢复原版 | `smooth_scroll_enabled = false` |