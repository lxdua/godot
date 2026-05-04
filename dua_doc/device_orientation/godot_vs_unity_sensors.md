# Godot vs Unity：移动端传感器功能对比

> 日期：2026-04-08  
> 关联文档：[移动端传感器增强提案](mobile_sensor_enhancements.md)

本文以《Rotaeno》（Unity 开发的体感音游）为参照，对比 Godot 与 Unity 在移动端传感器支持上的差异，明确 Godot 需要补齐的能力。

---

## 1. 功能对比总表

| 功能 | Unity | Godot (当前) | 差距 |
|------|-------|-------------|------|
| 加速度计 | `Input.acceleration` | `Input.get_accelerometer()` | ✅ 基本对齐 |
| 陀螺仪原始数据 | `Input.gyro.rotationRate` | `Input.get_gyroscope()` | ✅ 基本对齐 |
| 重力传感器 | `Input.gyro.gravity` | `Input.get_gravity()` | ✅ 基本对齐 |
| **设备姿态（四元数）** | `Input.gyro.attitude` | ❌ 无 | 🔴 **关键缺失** |
| **陀螺仪启停控制** | `Input.gyro.enabled = true/false` | 仅项目设置（需重启） | 🟡 需改进 |
| **采样率控制** | `Input.gyro.updateInterval` | 硬编码不可配置 | 🟡 需改进 |
| 传感器可用性查询 | `SystemInfo.supportsGyroscope` | ❌ 无 | 🟡 需补充 |
| 磁力计 | 需插件 | `Input.get_magnetometer()` | ✅ Godot 反而更好 |
| 罗盘/航向 | `Input.compass` | ❌ 无 | 🟢 低优先 |
| **传感器数据时间戳** | 隐含在事件系统中 | ❌ 无 | 🟡 需补充 |
| 传感器事件化 | 通过轮询（与 Godot 相同） | 轮询 | ✅ 相同模式 |

---

## 2. 逐项详细对比

### 2.1 设备姿态 — 最关键的差异

这是 Godot 与 Unity 之间**最大的功能差距**，也是《Rotaeno》实现其核心玩法最依赖的能力。

#### Unity

```csharp
// Unity - 一行代码获取设备姿态
Input.gyro.enabled = true;
Quaternion attitude = Input.gyro.attitude;

// 获取 roll 角（Rotaeno 核心需求）
Vector3 euler = attitude.eulerAngles;
float roll = euler.z;
```

Unity 的 `Input.gyro.attitude` 返回一个经过**系统级传感器融合**的 `Quaternion`，底层：
- **Android**：使用 `TYPE_ROTATION_VECTOR` 或 `TYPE_GAME_ROTATION_VECTOR`（硬件级加速度计+陀螺仪+磁力计融合）
- **iOS**：使用 `CMDeviceMotion.attitude`（Core Motion 框架的融合结果）

特点：
- 无漂移（长时间使用不会偏移）
- 低延迟（硬件级融合，比软件算法快）
- 高精度（专用 DSP 芯片处理）
- 绝对姿态（相对于参考坐标系，非增量）

#### Godot

```gdscript
# Godot - 没有直接的姿态 API，需要自己算
Input.get_gyroscope()       # 只有角速度 (rad/s)
Input.get_gravity()         # 只有重力方向
Input.get_accelerometer()   # 只有加速度

# 要算 roll 角，必须自己处理：
# 方案 1：用重力近似（抖动大）
var roll = atan2(gravity.x, -gravity.y)

# 方案 2：自己做互补滤波（要写一堆代码）
var gyro_roll = current_roll + gyro.z * delta
var accel_roll = atan2(gravity.x, -gravity.y)
current_roll = 0.98 * gyro_roll + 0.02 * accel_roll
```

**差距影响**：
- 开发者需要自行实现传感器融合算法，门槛高
- 软件融合精度不如系统硬件融合
- 代码量和调试成本显著增加
- Android/iOS 系统已经提供了融合后数据，Godot 只是没有暴露

#### 《Rotaeno》视角

《Rotaeno》需要的是设备绕 Z 轴的 roll 角度，用于旋转屏幕上的判定线。Unity 中一行 `Input.gyro.attitude` 即可；Godot 中需要自己写滤波算法，且效果不如系统融合。

---

### 2.2 启停控制

#### Unity

```csharp
// 运行时随时开关，立即生效
Input.gyro.enabled = true;   // 开启
Input.gyro.enabled = false;  // 关闭（省电）

// 场景切换时灵活控制
void OnEnable() {
    Input.gyro.enabled = true;
}
void OnDisable() {
    Input.gyro.enabled = false;
}
```

Unity 的陀螺仪是一个独立的 `Gyroscope` 对象，`enabled` 属性可以运行时随意切换。内部会相应地注册/注销系统传感器监听。

#### Godot

```gdscript
# Godot - 只能通过项目设置，且需要重启
# ProjectSettings.input_devices/sensors/enable_gyroscope = true

# 代码中没有办法动态启停
# Input 类没有 set_sensor_enabled() 这样的方法
```

Godot 当前实现 (`core/input/input.cpp:2378`)：
```cpp
gyroscope_enabled = GLOBAL_DEF_RST_BASIC("input_devices/sensors/enable_gyroscope", false);
// RST = restart required
```

**差距影响**：
- 不能按需启停传感器，持续运行浪费电量
- 对于只有部分关卡需要体感的游戏（如《Rotaeno》的非体感模式），无法在运行时关闭
- 手柄传感器已有 `set_joy_motion_sensors_enabled()`，设备传感器缺少对应接口

---

### 2.3 采样率控制

#### Unity

```csharp
// Unity - 可精确控制更新间隔
Input.gyro.updateInterval = 1.0f / 100.0f;  // 100Hz
Input.gyro.updateInterval = 1.0f / 200.0f;  // 200Hz（高精度场景）
```

Unity 允许开发者设置 `updateInterval`（单位：秒），底层会调用系统 API 设置对应的采样率。

#### Godot

```
# Godot - 硬编码，不可配置
# Android: SensorManager.SENSOR_DELAY_GAME（约 50Hz，不可变）
# iOS: 1.0 / 70.0（约 70Hz，不可变）
```

相关源码：

Android (`Godot.kt:714`)：
```kotlin
mSensorManager?.registerListener(godotInputHandler, mGyroscope, SensorManager.SENSOR_DELAY_GAME)
// SENSOR_DELAY_GAME ≈ 20ms ≈ 50Hz，固定值
```

iOS (`godot_view_apple_embedded.mm:142`)：
```objc
self.motionManager.deviceMotionUpdateInterval = 1.0 / 70.0;
// 固定 70Hz
```

**差距影响**：
- 无法根据应用需求优化功耗/精度平衡
- 音游等高精度场景可能需要 100-200Hz 采样
- 简单倾斜检测场景 20Hz 就够，却被迫用 50Hz 浪费电量
- 对比之下，Godot 手柄传感器已有 `set_joy_motion_sensors_rate()` 可控制采样率

---

### 2.4 传感器可用性检测

#### Unity

```csharp
// Unity - 直接查询
if (SystemInfo.supportsGyroscope) {
    EnableGyroControls();
} else {
    EnableTouchControls();
}

if (SystemInfo.supportsAccelerometer) {
    // ...
}
```

#### Godot

```gdscript
# Godot - 没有查询接口
# 只能间接判断：
var gyro = Input.get_gyroscope()
if gyro == Vector3.ZERO:
    # 可能没有陀螺仪...也可能设备刚好静止不动
    pass
```

**差距影响**：
- 无法可靠地做功能降级
- 无法在游戏启动时提示用户"你的设备不支持体感"

---

### 2.5 坐标系

#### Unity

Unity 的陀螺仪使用**右手坐标系**，但 `attitude` 需要做坐标转换才能正确对应 Unity 的左手坐标系：

```csharp
// Unity 中常见的陀螺仪姿态修正
Quaternion GyroToUnity(Quaternion q) {
    return new Quaternion(q.x, q.y, -q.z, -q.w);
}
```

这是 Unity 开发者的常见痛点，需要手动做转换。

#### Godot

Godot 目前在平台层已经做了屏幕方向的坐标修正：
- Android：根据 `Surface.ROTATION_0/90/180/270` 旋转坐标轴
- iOS：根据 `UIInterfaceOrientation` 旋转坐标轴

但由于没有 `attitude` API，坐标系问题尚未完全暴露。如果实现设备姿态 API，需要注意：
- Godot 使用**右手坐标系**（Y 朝上），与系统传感器坐标系更自然对齐
- 比 Unity 的左手坐标系少一层转换，对开发者更友好

---

### 2.6 Godot 的独特优势

Godot 在某些方面实际上**优于** Unity：

| 方面 | Godot 优势 |
|------|-----------|
| **磁力计** | 内置 `Input.get_magnetometer()`，Unity 需要插件 |
| **手柄传感器** | 已有完整的 `get_joy_gyroscope()`、校准、采样率控制等 API（experimental） |
| **开源可扩展** | 可以直接修改引擎源码添加缺失功能，Unity 需要等官方更新或写原生插件 |
| **轻量运行时** | 传感器数据路径更短，理论延迟更低 |

---

## 3. 对标 Unity 需要补齐的功能清单

根据以上对比，按照对齐 Unity 功能的优先级排序：

### 必须补齐（Unity 有且常用）

| # | 功能 | Unity 对应 API | 提案章节 |
|---|------|---------------|---------|
| 1 | 设备姿态（四元数/欧拉角） | `Input.gyro.attitude` | 3.1.1 |
| 2 | 运行时启停传感器 | `Input.gyro.enabled` | 3.2.3 |
| 3 | 采样率控制 | `Input.gyro.updateInterval` | 3.1.2 |
| 4 | 传感器可用性查询 | `SystemInfo.supportsGyroscope` | 3.2.2 |

### 可以超越 Unity（差异化优势）

| # | 功能 | 说明 |
|---|------|------|
| 5 | 传感器数据时间戳 | Unity 没有直接暴露，Godot 可以做得更好 |
| 6 | InputEventSensor 事件化 | Unity 也是轮询，Godot 可以额外提供事件模式 |
| 7 | 传感器数据缓冲 | 两者都没有，Godot 可以率先支持 |

### 不需要补齐（低价值或已有替代）

| # | 功能 | 原因 |
|---|------|------|
| 8 | 罗盘/航向 (`Input.compass`) | 使用场景窄，可用磁力计+重力自行计算 |
| 9 | 屏幕自动旋转控制 | 属于 DisplayServer 职责，非传感器范畴 |

---

## 4. 实现工作量对比

如果要让 Godot 在传感器方面达到 Unity 同等水平，核心工作量集中在：

| 功能 | 估计改动量 | 难度 | 说明 |
|------|-----------|------|------|
| 设备姿态 API | ~200 行 | 中等 | 主要是透传系统数据，Android 加一个传感器类型，iOS 读一个已有属性 |
| 运行时启停 | ~150 行 | 中等 | 需要打通 Input → 平台层的通信，Android/iOS 各自实现注册/注销 |
| 采样率控制 | ~50 行 | 简单 | 改两处硬编码为可配置值 |
| 可用性查询 | ~80 行 | 简单 | 各平台实现一个查询方法 |
| **总计** | **~480 行** | | 四个核心功能，工作量不大 |

相比之下，如果开发者在 Godot 中**不修改引擎**而是写插件来弥补这些差距：
- Android 需要写 Java 插件 + JNI 绑定
- iOS 需要写 GDExtension + ObjC 绑定
- 双平台维护，工作量远大于引擎内实现

---

## 5. 结论

**Godot 与 Unity 在移动端传感器方面的差距集中在 4 个点**，其中「设备姿态 API」是最关键的。好消息是这些差距**并非架构性缺陷**，而是 Godot 在现有基础设施上"少暴露了几个系统能力"。底层的传感器管线（数据采集 → JNI/ObjC 传递 → Input 单例存储）已经完整，只需要在此基础上扩展即可。

以总计约 480 行代码的改动，Godot 就能在传感器方面**全面对齐 Unity**，甚至在时间戳、手柄传感器等方面**超越 Unity**。
