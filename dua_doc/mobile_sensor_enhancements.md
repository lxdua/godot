# Godot 移动端传感器增强提案

> 日期：2026-04-08  
> 状态：提案草案  
> 相关模块：`core/input`、`platform/android`、`drivers/apple_embedded`  
> 参照项目：《Rotaeno》（Unity 开发的体感音游）

## 1. 背景与动机

Godot 当前对移动端（Android/iOS）的设备传感器提供了基础支持，包括加速度计、重力传感器、陀螺仪和磁力计。但在实现《Rotaeno》这类需要精确、低延迟设备旋转检测的应用时，现有 API 存在以下不足：

- **只有原始传感器数据**，没有系统级传感器融合后的设备姿态
- **采样率硬编码**，无法根据应用需求调整
- **缺少时间戳**，无法精确计算积分或判断数据是否刷新
- **传感器数据不参与 `InputEvent` 体系**，与引擎输入架构脱节
- **无法查询传感器可用性**，也无法在运行时动态启停传感器

本文档提出一系列增强方案，按优先级分组。

---

## 2. 现有实现分析

### 2.1 当前 API

| 方法 | 返回值 | 说明 |
|------|--------|------|
| `Input.get_accelerometer()` | `Vector3` | 加速度 (m/s²) |
| `Input.get_gravity()` | `Vector3` | 重力分量 (m/s²) |
| `Input.get_gyroscope()` | `Vector3` | 角速度 (rad/s) |
| `Input.get_magnetometer()` | `Vector3` | 磁场强度 (μT) |

### 2.2 平台实现现状

**Android** (`platform/android/java/lib/src/main/java/org/godotengine/godot/Godot.kt`)：
- 使用 `SensorManager` 注册四类传感器
- 采样率固定为 `SensorManager.SENSOR_DELAY_GAME`（约 50Hz）
- 数据通过 JNI 传递到 C++ 层的 `Input` 单例
- 传感器在 `onResume` 注册，`onPause` 注销

**iOS** (`drivers/apple_embedded/godot_view_apple_embedded.mm`)：
- 使用 `CMMotionManager` 的 `deviceMotion`
- 更新频率固定为 `1.0 / 70.0`（约 70Hz）
- 以轮询方式读取数据（源码注释：*"Just using polling approach for now"*）
- 根据 `UIInterfaceOrientation` 做坐标旋转修正

**数据流**：
```
平台传感器 → Java/ObjC 层 → JNI/C++ → Input 单例全局变量 → 用户轮询
```

### 2.3 核心限制

1. `Input` 中传感器数据是简单的 `Vector3` 成员变量，无时间戳、无缓冲
2. `set_gyroscope()` 等方法只是简单赋值，无事件派发
3. 启用/禁用通过 `GLOBAL_DEF_RST_BASIC` 实现（需重启生效）
4. Android 端未使用 `TYPE_ROTATION_VECTOR`；iOS 端未暴露 `CMDeviceMotion.attitude`

---

## 3. 与 Unity 的差距分析

以《Rotaeno》所用的 Unity 引擎为参照，对比 Godot 在移动端传感器方面的差距。

### 3.1 功能对比总表

| 功能 | Unity | Godot (当前) | 差距 |
|------|-------|-------------|------|
| 加速度计 | `Input.acceleration` | `Input.get_accelerometer()` | ✅ 基本对齐 |
| 陀螺仪原始数据 | `Input.gyro.rotationRate` | `Input.get_gyroscope()` | ✅ 基本对齐 |
| 重力传感器 | `Input.gyro.gravity` | `Input.get_gravity()` | ✅ 基本对齐 |
| **设备姿态（四元数）** | `Input.gyro.attitude` | ❌ 无 | 🔴 **关键缺失** |
| **陀螺仪启停控制** | `Input.gyro.enabled = true/false` | 仅项目设置（需重启） | 🟡 需改进 |
| **采样率控制** | `Input.gyro.updateInterval` | 硬编码不可配置 | 🟡 需改进 |
| 传感器可用性查询 | `SystemInfo.supportsGyroscope` | ❌ 无 | 🟡 需补充 |
| 磁力计 | 需插件 | `Input.get_magnetometer()` | ✅ **Godot 更好** |
| 罗盘/航向 | `Input.compass` | ❌ 无 | 🟢 低优先 |
| 传感器数据时间戳 | 隐含在事件系统中 | ❌ 无 | 🟡 需补充 |
| 传感器事件化 | 轮询模式 | 轮询模式 | ✅ 相同模式 |

### 3.2 差距详解

#### 3.2.1 🔴 设备姿态 — 最关键的差距

这是 Godot 与 Unity 之间**最大的功能差距**，也是《Rotaeno》实现其核心玩法最依赖的能力。

**Unity（一行代码）**：
```csharp
Input.gyro.enabled = true;
Quaternion attitude = Input.gyro.attitude;
float roll = attitude.eulerAngles.z;  // 直接获取 roll 角
```

Unity 的 `Input.gyro.attitude` 返回经过**系统级传感器融合**的 `Quaternion`，底层使用：
- Android：`TYPE_ROTATION_VECTOR` / `TYPE_GAME_ROTATION_VECTOR`（硬件级加速度计+陀螺仪+磁力计融合）
- iOS：`CMDeviceMotion.attitude`（Core Motion 框架的融合结果）

特点：无漂移、低延迟、高精度、绝对姿态（由专用 DSP 芯片处理）。

**Godot（需要大量手动工作）**：
```gdscript
# 没有直接的姿态 API，必须自行处理：

# 方案 1：用重力近似（抖动大，精度差）
var roll = atan2(gravity.x, -gravity.y)

# 方案 2：自己做互补滤波（代码多，效果不如硬件融合）
var gyro_roll = current_roll + gyro.z * delta
var accel_roll = atan2(gravity.x, -gravity.y)
current_roll = 0.98 * gyro_roll + 0.02 * accel_roll
```

**影响**：开发者门槛高、软件融合精度不如系统硬件融合、代码量和调试成本显著增加。而实际上 Android/iOS 系统已经提供了融合后数据，Godot 只是没有暴露。

#### 3.2.2 🟡 启停控制

**Unity**：
```csharp
// 运行时随时开关，立即生效
Input.gyro.enabled = true;   // 开启
Input.gyro.enabled = false;  // 关闭（省电）
```

**Godot**：
```gdscript
# 只能通过项目设置（需重启生效），无法运行时控制
# ProjectSettings.input_devices/sensors/enable_gyroscope = true
```

Godot 源码中使用 `GLOBAL_DEF_RST_BASIC`（`RST` = restart required）。而讽刺的是，Godot 的手柄传感器已经有了运行时启停接口 `set_joy_motion_sensors_enabled()`，设备传感器反而没有。

**影响**：不能按需启停传感器，持续运行浪费电量。《Rotaeno》有非体感模式，需要运行时关闭陀螺仪。

#### 3.2.3 🟡 采样率控制

**Unity**：
```csharp
Input.gyro.updateInterval = 1.0f / 100.0f;  // 100Hz
Input.gyro.updateInterval = 1.0f / 200.0f;  // 200Hz
```

**Godot**：
```
# 硬编码，不可配置
# Android: SensorManager.SENSOR_DELAY_GAME ≈ 50Hz
# iOS: 1.0 / 70.0 ≈ 70Hz
```

同样，Godot 手柄传感器已有 `set_joy_motion_sensors_rate()` 可控制采样率，设备传感器反而不行。

**影响**：无法根据应用需求优化功耗/精度平衡。音游等高精度场景可能需要 100-200Hz。

#### 3.2.4 🟡 传感器可用性检测

**Unity**：
```csharp
if (SystemInfo.supportsGyroscope) {
    EnableGyroControls();
} else {
    EnableTouchControls();
}
```

**Godot**：
```gdscript
# 没有查询接口，只能间接猜测
var gyro = Input.get_gyroscope()
if gyro == Vector3.ZERO:
    # 可能没有陀螺仪...也可能设备刚好静止不动
    pass
```

**影响**：无法可靠地做功能降级，无法在启动时提示"设备不支持体感"。

### 3.3 坐标系差异

Unity 的陀螺仪使用**右手坐标系**，但 `attitude` 需要手动转换才能适配 Unity 的左手坐标系：
```csharp
// Unity 开发者的常见痛点
Quaternion GyroToUnity(Quaternion q) {
    return new Quaternion(q.x, q.y, -q.z, -q.w);
}
```

Godot 使用**右手坐标系**（Y 朝上），与系统传感器坐标系更自然对齐，实现设备姿态 API 时比 Unity 少一层转换，对开发者更友好。

### 3.4 Godot 的独特优势

| 方面 | Godot 优势 |
|------|-----------|
| **磁力计** | 内置 `Input.get_magnetometer()`，Unity 需要插件 |
| **手柄传感器** | 已有完整的 `get_joy_gyroscope()`、校准、采样率控制等 API |
| **开源可扩展** | 可直接修改引擎源码，Unity 需等官方更新或写原生插件 |
| **坐标系** | 右手坐标系与传感器坐标系天然对齐，无需转换 |
| **轻量运行时** | 传感器数据路径更短，理论延迟更低 |

### 3.5 补齐差距的工作量估算

| 功能 | 估计改动量 | 难度 | 说明 |
|------|-----------|------|------|
| 设备姿态 API | ~200 行 | 中等 | 透传系统数据，Android 加传感器类型，iOS 读已有属性 |
| 运行时启停 | ~150 行 | 中等 | 打通 Input → 平台层通信 |
| 采样率控制 | ~50 行 | 简单 | 改两处硬编码为可配置值 |
| 可用性查询 | ~80 行 | 简单 | 各平台实现一个查询方法 |
| **总计** | **~480 行** | | 即可全面对齐 Unity |

> 如果开发者不修改引擎而是写插件来弥补差距，需要 Android Java 插件 + JNI 绑定、iOS GDExtension + ObjC 绑定，双平台维护工作量远大于引擎内实现。

---

## 4. 提案功能列表

### 4.1 🔴 高优先级

#### 4.1.1 设备姿态 API（Rotation Vector）

**目标**：提供经过系统级传感器融合的设备绝对姿态。

**新增 API**：

```cpp
// core/input/input.h
Quaternion get_device_orientation() const;
Vector3 get_device_euler() const; // 便捷方法，返回 (pitch, yaw, roll)

void set_device_orientation(const Quaternion &p_orientation); // 调试用
```

```gdscript
# GDScript 使用示例
var orientation: Quaternion = Input.get_device_orientation()
var euler: Vector3 = Input.get_device_euler()
var roll_angle: float = euler.z  # 直接获取 roll 角
```

**平台实现**：

Android (`Godot.kt`)：
```kotlin
// 新增传感器
private val mRotationVector: Sensor? by lazy {
    mSensorManager?.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR)
        ?: mSensorManager?.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
}

// 注册
if (orientationEnabled.get() && mRotationVector != null) {
    mSensorManager?.registerListener(godotInputHandler, mRotationVector, sensorDelay)
}
```

```java
// InputEventRunnable.java - SENSOR case 新增
case Sensor.TYPE_GAME_ROTATION_VECTOR:
case Sensor.TYPE_ROTATION_VECTOR:
    float[] quaternion = new float[4];
    SensorManager.getQuaternionFromVector(quaternion, values);
    GodotLib.deviceOrientation(quaternion[1], quaternion[2], quaternion[3], quaternion[0]);
    break;
```

iOS (`godot_view_apple_embedded.mm`)：
```objc
// 在 handleMotion 中，attitude 已经可用：
CMAttitude *attitude = self.motionManager.deviceMotion.attitude;
CMQuaternion q = attitude.quaternion;
DisplayServerAppleEmbedded::get_singleton()->update_device_orientation(
    Quaternion(q.x, q.y, q.z, q.w)  // 需根据屏幕方向旋转
);
```

**项目设置**：
```
input_devices/sensors/enable_device_orientation = false  # 默认关闭
```

**优势**：
- 系统级硬件融合，精度远超软件实现
- 直接返回四元数/欧拉角，无需用户自行积分或滤波
- 在 Android 上 `TYPE_GAME_ROTATION_VECTOR` 使用加速度计 + 陀螺仪融合，不依赖磁力计，避免磁干扰

---

#### 4.1.2 传感器采样率可配置

**目标**：允许开发者根据应用需求选择传感器采样率。

**方案 A：项目设置（推荐）**

```
input_devices/sensors/update_rate_hz = 60  # 默认 60Hz
```

**方案 B：运行时 API（配合 4.2.3 动态启停）**

```cpp
void set_sensor_update_rate(float p_rate_hz);
float get_sensor_update_rate() const;
```

**平台实现**：

Android：
```kotlin
// 将 Hz 转换为微秒间隔
val sensorDelayUs = (1_000_000.0 / updateRateHz).toInt()
mSensorManager?.registerListener(godotInputHandler, sensor, sensorDelayUs)
```

> 注：Android `registerListener` 的第三个参数可直接传微秒值，`SENSOR_DELAY_GAME` 等常量只是预设值。

iOS：
```objc
self.motionManager.deviceMotionUpdateInterval = 1.0 / updateRateHz;
```

**涉及文件改动**：

| 文件 | 改动 |
|------|------|
| `core/input/input.cpp` | 新增 `GLOBAL_DEF` 读取采样率设置 |
| `platform/android/.../Godot.kt` | `registerSensorsIfNeeded()` 使用可配置值 |
| `drivers/apple_embedded/godot_view_apple_embedded.mm` | `deviceMotionUpdateInterval` 使用可配置值 |
| `doc/classes/ProjectSettings.xml` | 文档 |

**与手柄 API 的一致性**：
手柄传感器已有 `Input.set_joy_motion_sensors_rate(device, rate)` 和 `Input.get_joy_motion_sensors_rate(device)`，设备传感器应保持类似设计。

---

#### 4.1.3 传感器数据时间戳

**目标**：让用户获取传感器数据的采样时间，用于精确积分和状态判断。

**方案**：在 `Input` 类中为每个传感器增加时间戳存储。

```cpp
// core/input/input.h
struct SensorData {
    Vector3 value;
    uint64_t timestamp_usec = 0; // 微秒时间戳
};

SensorData gyroscope_data;
SensorData accelerometer_data;
// ...
```

**新增 API**：

```cpp
uint64_t get_gyroscope_timestamp() const;
uint64_t get_accelerometer_timestamp() const;
uint64_t get_gravity_timestamp() const;
uint64_t get_magnetometer_timestamp() const;
```

```gdscript
# GDScript 使用示例 - 精确陀螺仪积分
var gyro = Input.get_gyroscope()
var timestamp = Input.get_gyroscope_timestamp()

if last_timestamp > 0:
    var dt = (timestamp - last_timestamp) / 1_000_000.0  # 微秒转秒
    current_angle += gyro.z * dt

last_timestamp = timestamp
```

**平台实现**：

Android - `SensorEvent.timestamp` 提供纳秒级时间戳，当前被完全忽略：
```java
// GodotInputHandler.java - onSensorChanged
public void onSensorChanged(SensorEvent event) {
    long timestampUsec = event.timestamp / 1000; // 纳秒转微秒
    runnable.setSensorEvent(event.sensor.getType(), 
        rotatedValue0, rotatedValue1, rotatedValue2, timestampUsec);
}
```

iOS - `CMDeviceMotion.timestamp` 提供秒级双精度时间戳：
```objc
uint64_t timestampUsec = (uint64_t)(self.motionManager.deviceMotion.timestamp * 1000000.0);
```

---

### 4.2 🟡 中优先级

#### 4.2.1 InputEventSensor 事件化

**目标**：将传感器数据纳入 Godot 的 `InputEvent` 体系。

**新增类**：

```cpp
// core/input/input_event.h
class InputEventSensor : public InputEvent {
    GDCLASS(InputEventSensor, InputEvent);

public:
    enum SensorType {
        ACCELEROMETER,
        GRAVITY,
        GYROSCOPE,
        MAGNETOMETER,
        DEVICE_ORIENTATION,
    };

private:
    SensorType sensor_type = ACCELEROMETER;
    Vector3 value;
    Quaternion orientation; // 仅 DEVICE_ORIENTATION 时使用
    uint64_t sensor_timestamp = 0;

public:
    void set_sensor_type(SensorType p_type);
    SensorType get_sensor_type() const;

    void set_value(const Vector3 &p_value);
    Vector3 get_value() const;

    void set_orientation(const Quaternion &p_orientation);
    Quaternion get_orientation() const;

    void set_sensor_timestamp(uint64_t p_timestamp);
    uint64_t get_sensor_timestamp() const;
};
```

```gdscript
# 使用示例
func _input(event: InputEvent) -> void:
    if event is InputEventSensor:
        match event.sensor_type:
            InputEventSensor.GYROSCOPE:
                process_gyro(event.value, event.sensor_timestamp)
            InputEventSensor.DEVICE_ORIENTATION:
                process_orientation(event.orientation)
```

**注意事项**：
- 传感器数据频率高（50-100Hz），事件量大，需要考虑性能影响
- 建议默认只在用户 opt-in 时才发射事件（通过项目设置控制）
- 可作为现有轮询 API 的补充而非替代
- 轮询 API 保持不变以确保向后兼容

#### 4.2.2 传感器可用性查询

**目标**：允许运行时检查设备是否具备特定传感器。

**新增 API**：

```cpp
// core/input/input.h
enum SensorType {
    SENSOR_ACCELEROMETER,
    SENSOR_GRAVITY,
    SENSOR_GYROSCOPE,
    SENSOR_MAGNETOMETER,
    SENSOR_DEVICE_ORIENTATION,
};

bool has_sensor(SensorType p_sensor) const;
```

**平台实现需要**：

在 `DisplayServer` 或平台层新增查询接口：

```cpp
// 需要各平台实现
virtual bool has_device_sensor(Input::SensorType p_sensor) const;
```

Android：
```kotlin
fun hasSensor(type: Int): Boolean {
    return mSensorManager?.getDefaultSensor(type) != null
}
```

iOS：
```objc
- (BOOL)hasSensor:(SensorType)type {
    switch (type) {
        case GYROSCOPE: return self.motionManager.isGyroAvailable;
        case MAGNETOMETER: return self.motionManager.isMagnetometerAvailable;
        // ...
    }
}
```

**使用场景**：
```gdscript
func _ready():
    if Input.has_sensor(Input.SENSOR_GYROSCOPE):
        # 启用体感控制
        pass
    else:
        # 降级为触屏控制
        show_touch_controls()
```

#### 4.2.3 运行时动态启用/禁用传感器

**目标**：允许在游戏运行过程中按需启停传感器，节省电量。

**现状问题**：
当前使用 `GLOBAL_DEF_RST_BASIC`，带有 `RST`（restart）标记，修改后需要重启才生效。

**新增 API**：

```cpp
void set_sensor_enabled(SensorType p_sensor, bool p_enabled);
bool is_sensor_enabled(SensorType p_sensor) const;
```

**实现要点**：
- `set_sensor_enabled` 需要通知平台层注册/注销传感器监听
- 需要一个类似 `DisplayServer` 的虚函数或信号机制将请求传递到平台层
- Android 端调用 `registerListener` / `unregisterListener`
- iOS 端调用 `startDeviceMotionUpdates` / `stopDeviceMotionUpdates`

```gdscript
# 使用示例 - 仅在需要时启用陀螺仪
func enter_gyro_level():
    Input.set_sensor_enabled(Input.SENSOR_GYROSCOPE, true)
    Input.set_sensor_enabled(Input.SENSOR_GRAVITY, true)

func exit_gyro_level():
    Input.set_sensor_enabled(Input.SENSOR_GYROSCOPE, false)
    Input.set_sensor_enabled(Input.SENSOR_GRAVITY, false)
```

**与手柄 API 一致性**：
手柄已有 `Input.set_joy_motion_sensors_enabled(device, enabled)`，设备传感器应对齐此设计。

---

### 4.3 🟢 低优先级

#### 4.3.1 传感器数据缓冲

**目标**：在一帧内可能有多次传感器更新（尤其高采样率下），提供帧间所有采样的访问。

```cpp
struct SensorSample {
    Vector3 value;
    uint64_t timestamp_usec;
};

TypedArray<Dictionary> get_gyroscope_samples() const;
void clear_sensor_buffer(); // 每帧开头自动调用或手动清除
```

**使用场景**：
- 极高精度的运动追踪
- 物理模拟中使用亚帧传感器数据
- 音游中对每个音符做精确的时间点判定

#### 4.3.2 内置传感器融合辅助类

**目标**：为不支持硬件融合或需要自定义融合的场景提供辅助。

```gdscript
class_name SensorFusion extends RefCounted

enum Mode {
    COMPLEMENTARY,  # 互补滤波（轻量）
    MADGWICK,       # Madgwick 滤波器（平衡）
    MAHONY,         # Mahony 滤波器（高质量）
}

func set_mode(mode: Mode) -> void
func update(delta: float) -> void  # 自动从 Input 读取传感器
func get_orientation() -> Quaternion
func get_euler() -> Vector3  # (pitch, yaw, roll)
func reset() -> void
```

> 注：这个功能也可以作为官方示例/插件提供，不一定需要进引擎核心。
> 如果 4.1.1（设备姿态 API）实现了，此功能的必要性大大降低。

---

## 5. 涉及文件清单

| 文件路径 | 涉及功能 | 改动类型 |
|---------|---------|---------|
| `core/input/input.h` | 全部 | 新增成员/方法声明 |
| `core/input/input.cpp` | 全部 | 新增方法实现、绑定 |
| `core/input/input_event.h` | 4.2.1 | 新增 InputEventSensor 类 |
| `core/input/input_event.cpp` | 4.2.1 | InputEventSensor 实现 |
| `platform/android/.../Godot.kt` | 4.1.1, 4.1.2, 4.2.3 | 传感器注册逻辑 |
| `platform/android/.../GodotInputHandler.java` | 4.1.1, 4.1.3 | onSensorChanged 扩展 |
| `platform/android/.../InputEventRunnable.java` | 4.1.1, 4.1.3 | 传感器事件数据扩展 |
| `platform/android/.../GodotLib.java` | 4.1.1 | 新增 JNI 方法声明 |
| `platform/android/java_godot_lib_jni.h` | 4.1.1 | 新增 JNI 函数 |
| `platform/android/java_godot_lib_jni.cpp` | 4.1.1 | JNI 实现 |
| `platform/android/display_server_android.h` | 4.1.1, 4.2.3 | 新增处理方法 |
| `platform/android/display_server_android.cpp` | 4.1.1, 4.2.3 | 实现 |
| `drivers/apple_embedded/godot_view_apple_embedded.mm` | 4.1.1, 4.1.2, 4.1.3 | iOS 传感器实现 |
| `drivers/apple_embedded/display_server_apple_embedded.h` | 4.1.1, 4.2.3 | 新增方法 |
| `drivers/apple_embedded/display_server_apple_embedded.mm` | 4.1.1, 4.2.3 | 实现 |
| `doc/classes/Input.xml` | 全部 | API 文档 |
| `doc/classes/ProjectSettings.xml` | 4.1.1, 4.1.2 | 项目设置文档 |
| `doc/classes/InputEventSensor.xml` | 4.2.1 | 新增类文档 |

---

## 6. 兼容性考虑

- **向后兼容**：所有新增功能为纯增量，不修改现有 API 行为
- **默认关闭**：新传感器类型默认不启用，不影响现有项目性能和电量
- **平台降级**：不支持的平台（桌面端）返回零值/false，与现有行为一致
- **Web 平台**：`DeviceOrientationEvent` / `DeviceMotionEvent` 可作为后续扩展（需 HTTPS + 用户授权）

## 7. 性能考虑

- 传感器回调频率高（50-200Hz），需确保跨线程数据传递开销最小
- `InputEventSensor` 事件化应可选，避免默认产生大量 InputEvent 对象
- 传感器缓冲（4.3.1）需要限制最大缓冲大小，防止内存膨胀
- Android 端 `SENSOR_DELAY_FASTEST` 可能达到 500Hz+，应在文档中提醒开发者注意性能影响

## 8. 参考

- Android Sensor API: https://developer.android.com/reference/android/hardware/SensorManager
- iOS Core Motion: https://developer.apple.com/documentation/coremotion/cmmotionmanager
- Godot 手柄传感器 PR 设计（可参考一致性）：`Input.get_joy_gyroscope()` 系列 API
- Madgwick 滤波器论文: https://x-io.co.uk/open-source-imu-and-ahrs-algorithms/
