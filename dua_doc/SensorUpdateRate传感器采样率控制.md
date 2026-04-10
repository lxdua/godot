# SensorUpdateRate 传感器采样率控制

> 日期：2026-04-10
> 状态：已实施
> 关联文档：[mobile_sensor_enhancements.md](mobile_sensor_enhancements.md)、[godot_vs_unity_sensors.md](godot_vs_unity_sensors.md)

## 1. 概述

新增项目设置 `input_devices/sensors/update_rate_hz`，允许开发者配置移动端传感器（加速度计、重力、陀螺仪、磁力计）的采样率（Hz）。

### 改动前（硬编码）

| 平台 | 原始值 | 等效频率 |
|------|--------|---------|
| Android | `SensorManager.SENSOR_DELAY_GAME` | ~50 Hz |
| iOS | `1.0 / 70.0` | ~70 Hz |

### 改动后（可配置）

| 平台 | 新行为 |
|------|--------|
| Android | Hz → 微秒间隔 (`1_000_000 / Hz`)，传给 `registerListener` |
| iOS | 直接设置 `deviceMotionUpdateInterval = 1.0 / Hz` |

默认值：**60 Hz**，范围：**1 ~ 200 Hz**。

---

## 2. 使用方式

### 编辑器

**项目 → 项目设置 → Input Devices → Sensors → Update Rate Hz**

### project.godot

```ini
[input_devices]
sensors/update_rate_hz=100
```

### 推荐值

| 场景 | 推荐 Hz | 说明 |
|------|---------|------|
| 简单倾斜检测（如赛车转向） | 20-30 | 节省电量，足够平滑 |
| 一般体感控制 | 60 | 默认值，平衡功耗与精度 |
| 音游 / 高精度体感 | 100-200 | 更高精度，注意功耗 |

> ⚠️ **注意**：此设置需要重启应用才生效（`RST` 标记），与现有传感器启停设置一致。

---

## 3. 改动文件清单

### 3.1 core/input/input.h

新增成员变量：

```cpp
int sensor_update_rate_hz = 60; // Default sensor update rate in Hz.
```

位于 `gyroscope` 变量之后。当前仅在构造函数中被赋值，未对外暴露 getter/setter（运行时不可变）。

### 3.2 core/input/input.cpp

在 `Input::Input()` 构造函数中新增：

```cpp
sensor_update_rate_hz = GLOBAL_DEF_RST_BASIC("input_devices/sensors/update_rate_hz", 60);
ProjectSettings::get_singleton()->set_custom_property_info(
    PropertyInfo(Variant::INT, "input_devices/sensors/update_rate_hz",
                 PROPERTY_HINT_RANGE, "1,200,1,suffix:Hz"));
```

- `GLOBAL_DEF_RST_BASIC`：在项目设置面板中可见（BASIC），修改需重启（RST）
- `PropertyInfo` 提供编辑器中的范围滑块和 Hz 单位后缀

### 3.3 platform/android/.../Godot.kt

新增成员：

```kotlin
private var sensorUpdateDelayUs = SensorManager.SENSOR_DELAY_GAME
```

`onGodotMainLoopStarted()` 中读取设置并转换：

```kotlin
val updateRateHz = GodotLib.getGlobal("input_devices/sensors/update_rate_hz").toIntOrNull() ?: 60
sensorUpdateDelayUs = if (updateRateHz > 0) 1_000_000 / updateRateHz else SensorManager.SENSOR_DELAY_GAME
```

`registerSensorsIfNeeded()` 中四个 `registerListener` 调用的第三个参数全部从 `SensorManager.SENSOR_DELAY_GAME` 改为 `sensorUpdateDelayUs`。

> **技术细节**：Android `registerListener()` 第三个参数为 `int`，接受预定义常量（0~3）或**微秒值**（>3 时视为微秒间隔）。`1_000_000 / 60 = 16666μs` 正确。

### 3.4 drivers/apple_embedded/godot_view_apple_embedded.mm

替换硬编码：

```objc
// 改动前
self.motionManager.deviceMotionUpdateInterval = 1.0 / 70.0;

// 改动后
int sensor_update_rate_hz = GLOBAL_DEF_RST_BASIC("input_devices/sensors/update_rate_hz", 60);
if (sensor_update_rate_hz <= 0) {
    sensor_update_rate_hz = 60;
}
self.motionManager.deviceMotionUpdateInterval = 1.0 / (double)sensor_update_rate_hz;
```

使用 `GLOBAL_DEF_RST_BASIC` 而非 `GLOBAL_GET`，确保 `godot_commonInit` 早期调用时即使 `input.cpp` 的定义尚未执行，也能注册默认值。多次调用同一 key 不冲突。

### 3.5 doc/classes/ProjectSettings.xml

新增文档条目，说明设置的用途、推荐值和平台限制。

---

## 4. Code Review 要点

### ✅ 正确性

| 检查项 | 结论 |
|--------|------|
| Android `registerListener` 接受微秒值 | ✅ 值 > 3 时视为微秒 |
| iOS `deviceMotionUpdateInterval` 接受秒 | ✅ `1.0 / 60.0 ≈ 0.01667` 正确 |
| 整数除法精度 | ✅ `1_000_000 / 60 = 16666μs ≈ 60.0024Hz`，误差 < 0.01% |
| 默认值 60Hz | ✅ 合理中间值（原 Android 50Hz / iOS 70Hz） |
| 防御性检查（<= 0） | ✅ iOS 兜底 60Hz；Android 退回 `SENSOR_DELAY_GAME` |
| `GLOBAL_DEF_RST_BASIC` 重复注册 | ✅ Godot 允许同一 key 多次 `GLOBAL_DEF` |
| 向后兼容 | ✅ 纯增量，不修改任何现有 API |

### ⚠️ 已知限制

| 限制 | 说明 |
|------|------|
| 需重启生效 | 使用 `RST` 标记，与现有传感器设置一致 |
| 所有传感器共用一个采样率 | 不支持为陀螺仪和加速度计设置不同频率 |
| Web 平台不受影响 | Web 端传感器由浏览器控制 |
| 未暴露运行时 API | GDScript 中不能动态读取/修改 |

---

## 5. 测试建议

### 5.1 不同 Hz 值测试矩阵

| 设置值 | Android 预期间隔 | iOS 预期间隔 |
|--------|-----------------|-------------|
| 20 Hz | 50000μs | 0.05s |
| 60 Hz（默认） | 16666μs | ~0.0167s |
| 100 Hz | 10000μs | 0.01s |
| 200 Hz | 5000μs | 0.005s |

### 5.2 边界值测试

| 设置值 | 预期行为 |
|--------|---------|
| 0 或负数 | iOS 兜底 60Hz；Android 退回 `SENSOR_DELAY_GAME` |
| 1 Hz | 低频更新，每秒仅 1 次 |
| 200 Hz | 最高允许值 |
| 未设置 | 默认 60Hz |

### 5.3 GDScript 验证脚本

```gdscript
var frame_count := 0
var elapsed := 0.0
var last_gyro := Vector3.ZERO

func _process(delta: float) -> void:
    var gyro := Input.get_gyroscope()
    if gyro != last_gyro:
        frame_count += 1
        last_gyro = gyro
    elapsed += delta
    if elapsed >= 1.0:
        print("Sensor data changes per second: ~", frame_count)
        frame_count = 0
        elapsed = 0.0
```

> 注意：主循环帧率通常为 60fps，传感器设为 100+Hz 时每帧可能覆盖多个采样。此脚本只能观测「每帧是否有新数据」，不能精确测量实际传感器频率。

---

## 6. 后续扩展方向

- **运行时启停传感器** — 支持 `Input.set_sensor_enabled()` 运行时开关
- **运行时调整采样率** — 暴露 `Input.set_sensor_update_rate()` 方法
- **设备姿态 API** — 新增 `Input.get_device_orientation()` 返回四元数
- **传感器可用性查询** — 新增 `Input.has_sensor()` 方法