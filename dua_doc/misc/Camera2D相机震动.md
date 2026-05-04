
# Camera2D 内置相机震动系统

> 为 Camera2D 新增基于 Noise 噪声的内置相机震动功能，支持一次性触发和持续震动两种模式。

---

## 一、修改目的

### 原版问题

Godot 的 Camera2D **完全没有内置相机震动功能**。这是 2D 游戏开发中最常用的效果之一（爆炸、受击、着地反馈、Boss 登场等），用户只能通过 GDScript 手动实现：

```gdscript
# 用户以前的做法（需要自己写）
var shake_amount = 0.0
func _process(delta):
    offset = Vector2(randf_range(-1,1), randf_range(-1,1)) * shake_amount
    shake_amount = lerp(shake_amount, 0.0, delta * 5.0)
```

这种手写方式有几个问题：
1. 每帧随机方向导致画面"颤抖"而非自然"晃动"
2. 帧率不同时表现不一致
3. 没有旋转维度，效果单薄
4. 重复造轮子，几乎每个项目都要写一遍

### 改动目标

在 Camera2D 中内置基于 **OpenSimplex Noise** 的相机震动系统，提供：
- 帧率无关的平滑震动
- 位置偏移 + 旋转偏移
- 一次性触发（`apply_shake`）和持续震动（`start_shake`/`stop_shake`）两种 API
- 在编辑器属性面板中可调参数

---

## 二、改动文件清单

| 文件 | 改动内容 |
|------|----------|
| `scene/2d/camera_2d.h` | 新增震动相关成员变量、噪声实例、方法声明 |
| `scene/2d/camera_2d.cpp` | 新增震动核心逻辑、getter/setter、属性绑定、构造函数初始化 |
| `doc/classes/Camera2D.xml` | 新增震动方法和属性的英文文档 |
| `dua_doc/Camera2D相机震动.md` | 本文件 |

---

## 三、新增属性

| 属性名 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `shake_strength` | float | 10.0 | 0~100+ px | 最大位置偏移强度（像素） |
| `shake_rotation_strength` | float | 0.05 | 0~1+ rad | 最大旋转偏移强度（弧度） |
| `shake_frequency` | float | 15.0 | 0.1~50+ | 噪声采样频率（越高越急促） |
| `shake_decay_rate` | float | 3.0 | 0~20+ | 指数衰减速率（仅 apply_shake 模式） |

---

## 四、新增方法

| 方法 | 签名 | 说明 |
|------|------|------|
| `apply_shake` | `apply_shake(strength: float = -1.0, duration: float = 0.3)` | 一次性触发震动，自动衰减 |
| `start_shake` | `start_shake(strength: float = -1.0)` | 开始持续震动 |
| `stop_shake` | `stop_shake()` | 停止震动 |
| `is_shaking` | `is_shaking() -> bool` | 查询是否正在震动 |
| `get_shake_offset` | `get_shake_offset() -> Vector2` | 获取当前帧的位置偏移 |
| `get_shake_rotation_offset` | `get_shake_rotation_offset() -> float` | 获取当前帧的旋转偏移 |

---

## 五、属性中文说明

| 属性名 | 中文说明 |
|--------|----------|
| shake_strength | 震动强度。相机震动时的最大位置偏移量（像素）。值越大，画面晃动幅度越大。 |
| shake_rotation_strength | 旋转震动强度。相机震动时的最大旋转偏移量（弧度）。设为 0 可禁用旋转震动。 |
| shake_frequency | 震动频率。噪声在时间轴上的采样频率。值越高，画面抖动越快速急促；值越低，晃动越缓慢像手持摄像机。 |
| shake_decay_rate | 衰减速率。仅在 apply_shake（一次性触发）模式下生效。控制震动强度的指数衰减速度。值越大，震动消失越快。 |

---

## 六、具体修改详解

### 6.1 camera_2d.h — 新增成员

```cpp
// 三个独立的噪声实例（X/Y/旋转各用不同 seed，避免同步）
fastnoiselite::FastNoiseLite _shake_noise_x;
fastnoiselite::FastNoiseLite _shake_noise_y;
fastnoiselite::FastNoiseLite _shake_noise_rot;

// 可调参数
real_t shake_strength = 10.0;
real_t shake_rotation_strength = 0.05;
real_t shake_frequency = 15.0;
real_t shake_decay_rate = 3.0;

// 运行时状态
real_t _shake_current_strength = 0.0;  // 当前实际强度（衰减中）
real_t _shake_initial_strength = 0.0;  // 本次震动的初始强度（衰减基准值）
real_t _shake_time = 0.0;              // 累计时间（噪声采样用）
real_t _shake_duration = 0.0;          // apply_shake 的总时长
real_t _shake_elapsed = 0.0;           // apply_shake 已经过时间
bool _shake_active = false;            // 是否正在震动
bool _shake_is_oneshot = false;        // true=apply_shake 模式

Vector2 _shake_offset;                 // 当前帧的位置偏移
real_t _shake_rotation_offset = 0.0;   // 当前帧的旋转偏移
```

### 6.2 camera_2d.cpp — 噪声初始化

```cpp
void Camera2D::_init_shake_noise() {
    // 三个噪声实例使用不同 seed，确保 X/Y/旋转方向不同步
    _shake_noise_x.SetSeed(0);
    _shake_noise_y.SetSeed(1337);
    _shake_noise_rot.SetSeed(2674);
    // 使用 OpenSimplex2 类型，频率设为 1.0（实际频率通过采样坐标控制）
}
```

### 6.3 camera_2d.cpp — 核心更新逻辑

```cpp
void Camera2D::_update_shake(real_t p_delta) {
    _shake_time += p_delta;
    _shake_elapsed += p_delta;

    // oneshot 模式：检查是否过期 + 指数衰减
    if (_shake_is_oneshot) {
        if (_shake_elapsed >= _shake_duration) { stop; return; }
        _shake_current_strength = _shake_initial_strength * exp(-decay_rate * elapsed);
    }

    // 在时间轴上采样噪声（返回 [-1, 1]）
    real_t sample_t = _shake_time * shake_frequency;
    noise_x = _shake_noise_x.GetNoise(sample_t, 0);
    noise_y = _shake_noise_y.GetNoise(0, sample_t);

    _shake_offset = Vector2(noise_x, noise_y) * _shake_current_strength;
    _shake_rotation_offset = noise_rot * shake_rotation_strength * (current/max);
}
```

### 6.4 camera_2d.cpp — 叠加到相机变换

在 `get_camera_transform()` 中，offset 处理之后、构建最终 Transform2D 之前：

```cpp
// 位置偏移叠加
if (_shake_active) {
    screen_rect.position += _shake_offset;
}

// 旋转偏移叠加（即使 ignore_rotation = true 也生效）
if (!ignore_rotation) {
    final_angle = camera_angle + _shake_rotation_offset;
} else if (_shake_active) {
    xform.set_rotation(_shake_rotation_offset);
}
```

### 6.5 camera_2d.cpp — 在 _notification 中驱动更新

```cpp
case NOTIFICATION_INTERNAL_PROCESS: {
    if (_shake_active) {
        _update_shake(get_process_delta_time());
    }
    _update_scroll();
} break;
```

---

## 七、算法原理

### 为什么用 Noise 而不是随机数？

| 特性 | 随机数 | Noise |
|------|--------|-------|
| 帧间连续性 | ❌ 突变 | ✅ 平滑过渡 |
| 帧率一致性 | ❌ 帧率高=抖得快 | ✅ 基于时间采样 |
| 自然感 | 像电视雪花 | 像手持摄像机 |

### 核心公式

**Noise 采样**：
\[
\text{offset}_x = \text{Noise}_x(t \times f, 0) \times S_{\text{current}}
\]
\[
\text{offset}_y = \text{Noise}_y(0, t \times f) \times S_{\text{current}}
\]

其中：
- \( t \) = 累计时间
- \( f \) = `shake_frequency`
- \( S_{\text{current}} \) = 当前强度

**指数衰减（apply_shake 模式）**：
\[
S_{\text{current}} = S_{\text{max}} \times e^{-\lambda \cdot t_{\text{elapsed}}}
\]

其中 \( \lambda \) = `shake_decay_rate`

### 为什么用三个不同 seed 的噪声实例？

如果 X 和 Y 使用同一个噪声，震动轨迹会沿对角线来回移动（因为 X=Y）。使用不同 seed 确保三个维度（X、Y、旋转）的运动轨迹独立，产生更自然的二维晃动。

---

## 八、状态机总览

```
[Idle]
  │
  ├── apply_shake(strength, duration) ──► [OneShot Active]
  │                                           │
  │                                     elapsed >= duration?
  │                                      Yes ──► [Idle]
  │                                      No  ──► 继续衰减
  │
  ├── start_shake(strength) ──► [Continuous Active]
  │                                   │
  │                              stop_shake()
  │                                   │
  │                                   ▼
  └───────────────────────────── [Idle]
```

---

## 九、参数调节指南

### 快速参考

| 效果 | strength | rotation | frequency | decay_rate | duration |
|------|----------|----------|-----------|------------|----------|
| 轻微受击 | 3~5 | 0.02 | 15~20 | 5.0 | 0.15 |
| 爆炸冲击 | 15~25 | 0.08 | 10~15 | 3.0 | 0.4 |
| 着地震动 | 5~10 | 0.03 | 20~25 | 4.0 | 0.2 |
| 地震（持续） | 8~15 | 0.05 | 5~8 | — | — |
| 手持摄像机 | 1~3 | 0.01 | 2~5 | — | — |
| Boss 登场 | 20~30 | 0.1 | 8~12 | 2.0 | 1.0 |

### 调参直觉

- **shake_strength**：想象画面偏移多少像素。10px 大约是"中等力度"。
- **shake_rotation_strength**：0.05 rad ≈ 2.9°，微妙但有效。超过 0.15 rad 会让人头晕。
- **shake_frequency**：
  - 2~5 → 缓慢晃动（手持/醉酒效果）
  - 10~15 → 标准震动
  - 20~30 → 快速颤抖（高压电击）
- **shake_decay_rate**：越大衰减越快。3.0 是"自然消退"，8.0+ 是"瞬间消失"。

### GDScript 使用示例

```gdscript
# 爆炸
$Camera2D.apply_shake(20.0, 0.5)

# 轻微受击（使用默认 strength）
$Camera2D.apply_shake()

# 持续地震
$Camera2D.start_shake(10.0)
# ... 地震结束后
$Camera2D.stop_shake()

# 自定义参数后触发
$Camera2D.shake_strength = 15.0
$Camera2D.shake_frequency = 8.0
$Camera2D.shake_decay_rate = 2.0
$Camera2D.apply_shake(-1.0, 1.0)  # 使用属性中的 strength，持续 1 秒
```

---

## 十、注意事项

1. **零性能开销**：不震动时 `_shake_active = false`，不执行任何噪声计算
2. **不依赖 noise 模块**：直接使用 `thirdparty/misc/FastNoiseLite.h`（纯头文件库），即使禁用 `modules/noise` 也能正常工作
3. **震动不受 limit 约束**：与 `offset` 属性一样，震动偏移可以突破相机边界限制
4. **ignore_rotation = true 时**：旋转震动仍然生效（独立于节点旋转的忽略设置）
