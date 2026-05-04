# DeviceOrientation 设备姿态 API 方案

> 日期：2026-04-10
> 状态：方案设计
> 关联文档：[mobile_sensor_enhancements.md](mobile_sensor_enhancements.md) 4.1.1 节

## 1. 目标

新增 `Input.get_device_orientation()` API，返回经过系统级传感器融合的设备绝对姿态（四元数），填补 Godot 与 Unity `Input.gyro.attitude` 之间最关键的差距。

### 核心价值

开发者现状（Godot）：
```gdscript
# 方案1：用重力近似，抖动大
var roll = atan2(gravity.x, -gravity.y)

# 方案2：自己做互补滤波，效果不如硬件融合
var gyro_roll = current_roll + gyro.z * delta
var accel_roll = atan2(gravity.x, -gravity.y)
current_roll = 0.98 * gyro_roll + 0.02 * accel_roll
```

新增后（一行搞定）：
```gdscript
var orientation: Quaternion = Input.get_device_orientation()
var euler: Vector3 = orientation.get_euler()
var roll: float = euler.z  # 直接获取 roll 角
```

底层使用硬件级传感器融合（由手机 DSP 芯片完成），无漂移、低延迟、高精度。

---

## 2. 新增 API

### 2.1 GDScript / C++ API

```cpp
// core/input/input.h

// 新增成员
bool device_orientation_enabled = false;
Quaternion device_orientation;

// 新增方法
Quaternion get_device_orientation() const;
void set_device_orientation(const Quaternion &p_orientation);
```

### 2.2 GDScript 使用示例

```gdscript
# 获取设备姿态四元数
var orientation := Input.get_device_orientation()

# 转换为欧拉角（pitch, yaw, roll）
var euler := orientation.get_euler()

# 类 Rotaeno 的旋转检测
var roll_angle := euler.z
rotate_game_world(roll_angle)
```

### 2.3 项目设置

```
input_devices/sensors/enable_device_orientation = false  # 默认关闭
```

与现有传感器设置（`enable_gyroscope` 等）保持一致的命名和行为模式。

---

## 3. 平台数据源

### 3.1 Android

使用 `TYPE_GAME_ROTATION_VECTOR` 传感器（优先）或 `TYPE_ROTATION_VECTOR`（兜底）。

| 传感器类型 | 融合源 | 特点 |
|-----------|--------|------|
| `TYPE_GAME_ROTATION_VECTOR` | 加速度计 + 陀螺仪 | ✅ 推荐。不受磁场干扰，适合游戏 |
| `TYPE_ROTATION_VECTOR` | 加速度计 + 陀螺仪 + 磁力计 | 有绝对航向，但容易受磁干扰 |

`SensorEvent.values` 返回 4 个浮点数 `[x, y, z, cos(θ/2)]`，需调用 `SensorManager.getQuaternionFromVector()` 转换为标准四元数 `[w, x, y, z]`。

### 3.2 iOS

使用 `CMDeviceMotion.attitude.quaternion`，已在 `CMMotionManager.deviceMotion` 中可用（当前代码已经启动了 `deviceMotion` 更新，只是没有读取 attitude）。

```objc
CMQuaternion q = self.motionManager.deviceMotion.attitude.quaternion;
// q.x, q.y, q.z, q.w
```

### 3.3 其他平台

桌面端 / Web 端返回 `Quaternion.IDENTITY`，与现有传感器的降级行为一致。

---

## 4. 完整数据流

### 4.1 Android 数据流

```
Sensor.TYPE_GAME_ROTATION_VECTOR
  → GodotInputHandler.onSensorChanged()        [Java, UI线程]
     → 屏幕旋转修正（四元数旋转）
     → InputEventRunnable.setOrientationEvent() [新增方法]
        → GodotLib.deviceOrientation(x, y, z, w) [新增 JNI]
           → java_godot_lib_jni.cpp 存储到静态变量
              → step() 中调用 process_device_orientation()
                 → DisplayServerAndroid::process_device_orientation()
                    → Input::set_device_orientation()
```

### 4.2 iOS 数据流

```
CMMotionManager.deviceMotion.attitude.quaternion
  → godot_view_apple_embedded.mm handleMotion   [主线程轮询]
     → 屏幕方向修正
     → DisplayServerAppleEmbedded::update_device_orientation()
        → Input::set_device_orientation()
```

---

## 5. 文件改动清单

### 5.1 Core 层（3 个文件）

#### `core/input/input.h`

```cpp
// 新增成员（在 gyroscope 之后）
bool device_orientation_enabled = false;
Quaternion device_orientation;

// 新增方法声明
Quaternion get_device_orientation() const;
void set_device_orientation(const Quaternion &p_orientation);
```

#### `core/input/input.cpp`

1. **`_bind_methods()`**：新增绑定
```cpp
ClassDB::bind_method(D_METHOD("get_device_orientation"), &Input::get_device_orientation);
ClassDB::bind_method(D_METHOD("set_device_orientation", "value"), &Input::set_device_orientation);
```

2. **`get_device_orientation()`**：实现（含 Android DEBUG 警告）
```cpp
Quaternion Input::get_device_orientation() const {
    _THREAD_SAFE_METHOD_
#if defined(DEBUG_ENABLED) && defined(ANDROID_ENABLED)
    if (!device_orientation_enabled) {
        WARN_PRINT_ONCE("`input_devices/sensors/enable_device_orientation` is not enabled.");
    }
#endif
    return device_orientation;
}
```

3. **`set_device_orientation()`**：实现
```cpp
void Input::set_device_orientation(const Quaternion &p_orientation) {
    _THREAD_SAFE_METHOD_
    device_orientation = p_orientation;
}
```

4. **`Input::Input()`**：读取项目设置
```cpp
device_orientation_enabled = GLOBAL_DEF_RST_BASIC("input_devices/sensors/enable_device_orientation", false);
```

#### `doc/classes/Input.xml`

新增 `get_device_orientation` 和 `set_device_orientation` 方法文档。

#### `doc/classes/ProjectSettings.xml`

新增 `input_devices/sensors/enable_device_orientation` 设置文档。

---

### 5.2 Android 层（6 个文件）

#### `platform/android/.../Godot.kt`

```kotlin
// 新增传感器声明
private val deviceOrientationEnabled = AtomicBoolean(false)
private val mRotationVector: Sensor? by lazy {
    mSensorManager?.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR)
        ?: mSensorManager?.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR)
}

// onGodotMainLoopStarted() 中读取设置
deviceOrientationEnabled.set(
    java.lang.Boolean.parseBoolean(
        GodotLib.getGlobal("input_devices/sensors/enable_device_orientation")
    )
)

// registerSensorsIfNeeded() 中注册
if (deviceOrientationEnabled.get() && mRotationVector != null) {
    mSensorManager?.registerListener(godotInputHandler, mRotationVector, sensorUpdateDelayUs)
}

// unregisterSensorsIfNeeded() 中无需单独处理（已有 unregisterListener(this) 全量注销）
```

#### `platform/android/.../GodotInputHandler.java`

`onSensorChanged()` 需要特殊处理：

- **当前代码**：硬性检查 `values.length != 3` 就 return，但 rotation vector 返回 4~5 个值
- **改动**：移除对 length 的硬限制，或在 length 检查前先分流处理 rotation vector

```java
@Override
public void onSensorChanged(SensorEvent event) {
    final float[] values = event.values;
    if (values == null) {
        return;
    }

    InputEventRunnable runnable = InputEventRunnable.obtain();
    if (runnable == null) {
        return;
    }

    int sensorType = event.sensor.getType();

    // Rotation vector 返回 4~5 个浮点数，需要单独处理
    if (sensorType == Sensor.TYPE_GAME_ROTATION_VECTOR
            || sensorType == Sensor.TYPE_ROTATION_VECTOR) {
        float[] quaternion = new float[4];
        SensorManager.getQuaternionFromVector(quaternion, values);
        // quaternion: [w, x, y, z]
        // 需要根据屏幕旋转修正四元数
        // ... 修正逻辑 ...
        runnable.setOrientationEvent(quaternion[1], quaternion[2], quaternion[3], quaternion[0]);
        godot.runOnRenderThread(runnable);
        return;
    }

    // 现有的 3 分量传感器处理（保持不变）
    if (values.length != 3) {
        return;
    }
    // ... 原有的 switch(cachedRotation) 和 setSensorEvent 逻辑 ...
}
```

**屏幕旋转修正注意事项**：
- 现有 3 分量传感器用简单的轴交换 + 取反来修正屏幕旋转
- 四元数不能用同样方式，需要构造一个旋转四元数来修正：
  - ROTATION_0: 不修正
  - ROTATION_90: 绕 Z 轴旋转 -90°
  - ROTATION_180: 绕 Z 轴旋转 180°
  - ROTATION_270: 绕 Z 轴旋转 90°

```java
// 构造修正四元数
Quaternion correction;
switch (cachedRotation) {
    case Surface.ROTATION_0:   // 无修正
        break;
    case Surface.ROTATION_90:  // 绕 Z 轴 -90°
        // q_correction = (cos(-45°), 0, 0, sin(-45°))
        float[] corrected = multiplyQuaternion(correctionQuat, quaternion);
        break;
    // ...
}
```

#### `platform/android/.../InputEventRunnable.java`

```java
// 新增事件类型
enum EventType {
    // ... 现有类型 ...
    SENSOR,
    DEVICE_ORIENTATION  // 新增
}

// 新增字段
private float orientW, orientX, orientY, orientZ;

// 新增 setter
void setOrientationEvent(float x, float y, float z, float w) {
    this.currentEventType = EventType.DEVICE_ORIENTATION;
    this.orientX = x;
    this.orientY = y;
    this.orientZ = z;
    this.orientW = w;
}

// run() 中新增 case
case DEVICE_ORIENTATION:
    GodotLib.deviceOrientation(orientX, orientY, orientZ, orientW);
    break;
```

#### `platform/android/.../GodotLib.java`

```java
/**
 * Forward device orientation (rotation vector) events.
 */
public static native void deviceOrientation(float x, float y, float z, float w);
```

#### `platform/android/java_godot_lib_jni.h`

```cpp
JNIEXPORT void JNICALL Java_org_godotengine_godot_GodotLib_deviceOrientation(
    JNIEnv *env, jclass clazz, jfloat x, jfloat y, jfloat z, jfloat w);
```

#### `platform/android/java_godot_lib_jni.cpp`

```cpp
// 新增静态变量
static Quaternion device_orientation;

// 新增 JNI 函数
JNIEXPORT void JNICALL Java_org_godotengine_godot_GodotLib_deviceOrientation(
        JNIEnv *env, jclass clazz, jfloat x, jfloat y, jfloat z, jfloat w) {
    device_orientation = Quaternion(x, y, z, w);
}

// step() 中新增
DisplayServerAndroid::get_singleton()->process_device_orientation(device_orientation);
```

#### `platform/android/display_server_android.h/cpp`

```cpp
// .h
void process_device_orientation(const Quaternion &p_orientation);

// .cpp
void DisplayServerAndroid::process_device_orientation(const Quaternion &p_orientation) {
    Input::get_singleton()->set_device_orientation(p_orientation);
}
```

---

### 5.3 iOS 层（2 个文件）

#### `drivers/apple_embedded/godot_view_apple_embedded.mm`

在 `handleMotion` 方法末尾、switch 的每个 case 内新增：

```objc
// 读取四元数姿态
CMQuaternion q = self.motionManager.deviceMotion.attitude.quaternion;
Quaternion orientation(q.x, q.y, q.z, q.w);

// 根据屏幕方向修正（与 gravity/gyroscope 相同的旋转逻辑）
switch (interfaceOrientation) {
    case UIInterfaceOrientationLandscapeLeft:
        orientation = Quaternion(Vector3(0, 0, 1), -Math::PI * 0.5) * orientation;
        break;
    case UIInterfaceOrientationLandscapeRight:
        orientation = Quaternion(Vector3(0, 0, 1), Math::PI * 0.5) * orientation;
        break;
    case UIInterfaceOrientationPortraitUpsideDown:
        orientation = Quaternion(Vector3(0, 0, 1), Math::PI) * orientation;
        break;
    default:
        break;
}

DisplayServerAppleEmbedded::get_singleton()->update_device_orientation(orientation);
```

**优化**：姿态更新可以提取到 switch 外面，只做一次四元数乘法修正，无需在每个 case 重复。

#### `drivers/apple_embedded/display_server_apple_embedded.h/mm`

```cpp
// .h
void update_device_orientation(const Quaternion &p_orientation);

// .mm
void DisplayServerAppleEmbedded::update_device_orientation(const Quaternion &p_orientation) {
    Input::get_singleton()->set_device_orientation(p_orientation);
}
```

---

## 6. 坐标系与四元数约定

### 6.1 Godot 坐标系

Godot 使用右手坐标系：X→右，Y→上，Z→向观察者。

### 6.2 系统传感器坐标系

Android 和 iOS 的传感器坐标系均为：X→右，Y→上，Z→屏幕外（面向用户）。
- 设备竖直手持（Portrait 模式）时，与 Godot 2D 坐标系天然对齐
- 设备平放桌面时，Z 轴朝上

### 6.3 返回值约定

`Input.get_device_orientation()` 返回的 `Quaternion` 表示：
- **从设备坐标系到世界坐标系的旋转**
- 已根据当前屏幕方向进行修正
- 设备平放桌面、屏幕朝上时接近 `Quaternion.IDENTITY`（取决于参考帧）

### 6.4 与 Unity 的差异

| 方面 | Unity | Godot（本方案） |
|------|-------|----------------|
| 坐标系 | 左手 | 右手 |
| 返回类型 | `Quaternion` | `Quaternion` |
| 需要手动转换 | 是（`GyroToUnity()`） | 否，天然对齐 |
| 参考帧 | 相对于启动时姿态 | Android: 相对于重力+磁北 / iOS: 可配 |

---

## 7. 关键设计决策

### 7.1 为什么不复用现有的 `setSensorEvent` 3 分量通道？

Rotation vector 返回四元数（4个分量），与现有的 3 分量传感器不兼容。新增独立的 `setOrientationEvent` / `DEVICE_ORIENTATION` 事件类型更清晰，避免歧义。

### 7.2 为什么优先 `TYPE_GAME_ROTATION_VECTOR`？

`TYPE_GAME_ROTATION_VECTOR` 不使用磁力计，避免了磁场干扰（室内金属多、电磁环境复杂），更适合游戏场景。航向可能会漂移，但对于相对旋转检测（如 Rotaeno）不影响。

`TYPE_ROTATION_VECTOR` 作为 fallback，在没有陀螺仪的低端设备上也可用（纯加速度计 + 磁力计融合）。

### 7.3 `onSensorChanged` 中的 `values.length != 3` 检查

这是本次改动中最需要注意的一个**兼容性关键点**：

- 现有代码第 800 行：`if (values == null || values.length != 3) { return; }`
- rotation vector 的 `values.length` 为 4 或 5（取决于设备/API 版本）
- 如果不修改此检查，rotation vector 事件会被静默丢弃

**方案**：将 length 检查移到 rotation vector 分流之后。

### 7.4 屏幕旋转修正

现有 3 分量传感器用简单的分量交换 + 取反来修正屏幕旋转。四元数需要通过四元数乘法修正：

```
corrected = rotation_correction × raw_quaternion
```

其中 `rotation_correction` 是绕 Z 轴的旋转四元数（0°/90°/180°/270°对应屏幕 ROTATION_0/90/180/270）。

---

## 8. 测试计划

### 8.1 基本功能

```gdscript
func _process(delta):
    var orient := Input.get_device_orientation()
    if orient != Quaternion.IDENTITY:
        var euler := orient.get_euler()
        $Label.text = "Pitch: %.1f° Yaw: %.1f° Roll: %.1f°" % [
            rad_to_deg(euler.x),
            rad_to_deg(euler.y),
            rad_to_deg(euler.z)
        ]
```

### 8.2 测试矩阵

| 测试项 | 验证内容 |
|--------|---------|
| 设备平放 | 姿态接近 identity |
| 设备竖立（Portrait） | pitch ≈ 90° |
| 设备左转 90°（Landscape） | roll ≈ ±90° |
| 缓慢旋转 | 数据平滑，无突变 |
| 快速旋转 | 跟随实时，无明显延迟 |
| 长时间运行 | 无明显漂移（GAME_ROTATION_VECTOR 的航向可能慢漂） |
| 不同屏幕方向 | 四个方向各自验证修正正确 |
| 未启用设置 | 返回 `Quaternion.IDENTITY`（Android DEBUG 模式有警告） |
| 不支持的设备 | 返回 `Quaternion.IDENTITY` |
| 桌面端 | 返回 `Quaternion.IDENTITY` |

### 8.3 与 Unity 对比测试

在同一台设备上同时运行 Unity 和 Godot 测试应用，对比：
- `Input.gyro.attitude`（Unity）vs `Input.get_device_orientation()`（Godot）
- 验证旋转方向一致性（注意左手 vs 右手坐标系差异）
- 验证延迟和精度可比

---

## 9. 预估工作量

| 层级 | 文件数 | 新增代码量（行） |
|------|--------|----------------|
| Core (input.h/cpp + docs) | 4 | ~50 |
| Android (Kt + Java + JNI) | 6 | ~100 |
| iOS (mm + h) | 2 | ~30 |
| **总计** | **12** | **~180** |

---

## 10. 兼容性与风险

| 方面 | 评估 |
|------|------|
| 向后兼容 | ✅ 纯增量，不修改任何现有 API |
| 默认行为 | ✅ 默认关闭，不影响现有项目 |
| 性能影响 | ✅ 关闭时零开销；开启时与现有传感器类似 |
| 设备覆盖率 | ⚠️ `TYPE_GAME_ROTATION_VECTOR` 需要陀螺仪硬件（2015年后中高端设备基本都有） |
| 破坏性改动风险 | ⚠️ `onSensorChanged` 中的 `values.length` 检查需谨慎处理 |
