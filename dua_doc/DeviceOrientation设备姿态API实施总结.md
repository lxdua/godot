# DeviceOrientation 设备姿态 API — 实施总结

> 日期：2026-04-10
> 状态：已实施
> 关联：[方案文档](DeviceOrientation设备姿态API方案.md)、[mobile_sensor_enhancements.md](mobile_sensor_enhancements.md) 4.1.1 节

---

## 1. 功能说明

新增 `Input.get_device_orientation()` API，返回经过移动端系统级传感器融合的设备绝对姿态（`Quaternion`）。

底层数据来源：
- **Android**：`TYPE_GAME_ROTATION_VECTOR`（优先）或 `TYPE_ROTATION_VECTOR`（兜底），由手机 Sensor Hub DSP 芯片完成硬件级融合
- **iOS**：`CMDeviceMotion.attitude.quaternion`，由 Core Motion 框架完成系统级融合

两者都**不是我们自己用陀螺仪积分计算的**，而是直接读取系统已经算好的四元数。

### 使用方式

1. 项目设置中启用：`input_devices/sensors/enable_device_orientation = true`
2. GDScript 中调用：

```gdscript
var orientation := Input.get_device_orientation()
var euler := orientation.get_euler()
var roll := euler.z  # 设备绕屏幕法线的旋转角（弧度）
```

---

## 2. 改动文件清单（12 个文件）

### 2.1 Core 层

| 文件 | 改动 |
|------|------|
| `core/input/input.h` | +成员 `device_orientation_enabled`、`device_orientation`；+方法声明 `get/set_device_orientation` |
| `core/input/input.cpp` | +`_bind_methods` 两个 bind；+getter（含 DEBUG 警告）；+setter；+构造函数中 `GLOBAL_DEF_RST_BASIC` |

### 2.2 Android 层

| 文件 | 改动 |
|------|------|
| `Godot.kt` | +`deviceOrientationEnabled`、`mRotationVector`（GAME 优先 fallback ROTATION）；+设置读取；+`registerListener` |
| `GodotInputHandler.java` | +`import SensorManager`；+四元数修正常量和乘法方法；**重构 `onSensorChanged()`**：将 rotation vector 分流到 `length!=3` 检查之前 |
| `InputEventRunnable.java` | +`DEVICE_ORIENTATION` 事件类型；+orientation 字段/setter；+`run()` case 调用 JNI |
| `GodotLib.java` | +`deviceOrientation(x,y,z,w)` native 方法声明 |
| `java_godot_lib_jni.h` | +JNI 函数声明 |
| `java_godot_lib_jni.cpp` | +静态变量 `Quaternion device_orientation`；+`step()` 中调用 `process_device_orientation`；+JNI 函数实现 |
| `display_server_android.h` | +`process_device_orientation()` 方法声明 |
| `display_server_android.cpp` | +`process_device_orientation()` 实现 → `Input::set_device_orientation()` |

### 2.3 iOS 层

| 文件 | 改动 |
|------|------|
| `godot_view_apple_embedded.mm` | +读取 `attitude.quaternion`；+各屏幕方向的四元数乘法修正；+调用 `update_device_orientation` |
| `display_server_apple_embedded.h` | +`update_device_orientation()` 方法声明 |
| `display_server_apple_embedded.mm` | +`update_device_orientation()` 实现 → `Input::set_device_orientation()` |

### 2.4 文档

| 文件 | 改动 |
|------|------|
| `doc/classes/Input.xml` | +`get_device_orientation`（含代码示例）、`set_device_orientation` 方法文档 |
| `doc/classes/ProjectSettings.xml` | +`enable_device_orientation` 设置文档 |

---

## 3. Code Review 结论

### ✅ 通过项

| 检查项 | 结论 |
|--------|------|
| `values.length != 3` 兼容性 | ✅ rotation vector 在 length 检查之前分流处理 |
| 四元数屏幕旋转修正 | ✅ 使用四元数乘法（预计算常量），非分量交换 |
| 四元数分量映射 | ✅ Android `getQuaternionFromVector` 返回 `[w,x,y,z]`，传 JNI 时映射为 `(x,y,z,w)` 匹配 Godot `Quaternion(x,y,z,w)` 构造函数 |
| 传感器优先级 | ✅ `TYPE_GAME_ROTATION_VECTOR` 优先（不受磁场干扰），`TYPE_ROTATION_VECTOR` 兜底 |
| iOS 数据源 | ✅ 直接读取已启动的 `deviceMotion.attitude`，无需额外初始化 |
| 线程安全 | ✅ getter/setter 均有 `_THREAD_SAFE_METHOD_` |
| 默认关闭 | ✅ `enable_device_orientation = false`，不影响现有项目 |
| 向后兼容 | ✅ 纯增量，不修改任何现有 API 或行为 |
| DEBUG 警告 | ✅ 未启用时 `get_device_orientation()` 在 Android DEBUG 模式打印一次警告 |
| 文档完整 | ✅ Input.xml + ProjectSettings.xml 均已更新 |

### ⚠️ 注意事项

| 项目 | 说明 |
|------|------|
| 需重启生效 | `GLOBAL_DEF_RST_BASIC` 标记，与其他传感器设置一致 |
| 航向漂移 | `GAME_ROTATION_VECTOR` 不使用磁力计，yaw 角长时间运行可能慢漂，对相对旋转检测（如 Rotaeno）无影响 |
| 低端设备兼容 | 没有陀螺仪的设备可能没有 `GAME_ROTATION_VECTOR`，此时 fallback 到 `ROTATION_VECTOR`（加速度计+磁力计融合），精度较低 |
| 收敛时间 | 系统传感器融合启动后需要几百毫秒收敛到稳定值 |

---

## 4. 数据流全链路

### Android

```
Sensor.TYPE_GAME_ROTATION_VECTOR（硬件 DSP 融合）
  → GodotInputHandler.onSensorChanged()         [Java, 渲染线程]
     → SensorManager.getQuaternionFromVector()   [w,x,y,z]
     → 屏幕旋转修正（四元数乘法，预计算常量）
     → InputEventRunnable.setOrientationEvent()  [x,y,z,w → Godot 顺序]
        → GodotLib.deviceOrientation(x,y,z,w)   [JNI]
           → 静态变量 device_orientation
              → step() → DisplayServerAndroid::process_device_orientation()
                 → Input::set_device_orientation()
```

### iOS

```
CMDeviceMotion.attitude.quaternion（Core Motion 框架融合）
  → godot_view_apple_embedded.mm handleMotion    [主线程轮询]
     → 屏幕方向修正（四元数乘法）
     → DisplayServerAppleEmbedded::update_device_orientation()
        → Input::set_device_orientation()
```

---

## 5. 测试建议

### 5.1 基础验证脚本

```gdscript
extends Control

var last_orient := Quaternion.IDENTITY
var change_count := 0
var elapsed := 0.0

func _process(delta: float) -> void:
    var orient := Input.get_device_orientation()
    var euler := orient.get_euler()

    $LabelOrientation.text = "Q: (%.3f, %.3f, %.3f, %.3f)" % [orient.x, orient.y, orient.z, orient.w]
    $LabelEuler.text = "Euler: (%.1f°, %.1f°, %.1f°)" % [
        rad_to_deg(euler.x), rad_to_deg(euler.y), rad_to_deg(euler.z)
    ]

    if orient != last_orient:
        change_count += 1
        last_orient = orient
    elapsed += delta
    if elapsed >= 1.0:
        $LabelRate.text = "Updates/sec: %d" % change_count
        change_count = 0
        elapsed = 0.0
```

### 5.2 测试矩阵

| 测试场景 | 预期 |
|---------|------|
| 设备平放桌面 | 接近 `Quaternion.IDENTITY` |
| 竖直手持（Portrait） | pitch ≈ 90° |
| 左转 90°（Landscape Left） | roll ≈ -90°（已修正屏幕方向） |
| 右转 90°（Landscape Right） | roll ≈ +90°（已修正屏幕方向） |
| 缓慢旋转 | 平滑变化，无跳变 |
| 快速甩动 | 实时跟随，无明显延迟 |
| 长时间静置 5 分钟 | 数据稳定，无明显漂移 |
| 未启用设置 | 返回 `Quaternion(0,0,0,1)` = IDENTITY |
| 桌面端运行 | 返回 IDENTITY |

### 5.3 边界测试

| 场景 | 预期 |
|------|------|
| 没有陀螺仪的设备 | fallback 到 `TYPE_ROTATION_VECTOR`（精度降低但可用） |
| 两种 rotation vector 都不支持 | `mRotationVector` 为 null，不注册，返回 IDENTITY |
| 设置为 false 但调用 API | Android DEBUG 打印一次警告，返回 IDENTITY |
