# Device Orientation API — Real Device Test Report

> Date: 2026-04-22
> Device: Redmi 2602BRT18C (Android, arm64)
> Engine: Godot v4.7.dev custom build
> Mode: Landscape (ROTATION_90)

---

## Test Setup

### Build Steps

1. Compiled Android export template from modified engine source:
   ```
   scons platform=android target=template_debug arch=arm64
   gradlew.bat generateGodotTemplates
   ```
2. Deployed to device via USB debug (`adb install`)

### Project Settings

```
input_devices/sensors/enable_gyroscope = true
input_devices/sensors/enable_device_orientation = true
```

### Test Script

A 3D box model driven directly by `Input.get_device_orientation()`:

```gdscript
func _process(_delta: float) -> void:
    var o := Input.get_device_orientation()
    phone_mesh.quaternion = o
```

No manual quaternion correction needed in user code.

---

## Test Results

### Basic Orientation

| Action | Expected | Actual | Status |
|--------|----------|--------|--------|
| Phone flat on table (landscape) | Euler ≈ (0, 0, 0) | Euler ≈ (0, 0, 0) | ✅ |
| Tilt left ~30° | Roll changes ~30° | Roll changed ~30° | ✅ |
| Tilt forward ~30° | Pitch changes ~30° | Pitch changed ~30° | ✅ |
| Rotate on table (yaw) | Yaw changes | Yaw changed | ✅ |
| 3D model follows phone rotation | 1:1 match | 1:1 match | ✅ |

### Rotation Smoothness

| Aspect | Result |
|--------|--------|
| Slow rotation | Smooth, no jitter |
| Fast rotation | Responsive, no visible lag |
| Sudden stop | No overshoot or oscillation |
| Compared to `get_gravity()` | Significantly smoother and more stable |

### Data Quality

The quaternion data from `TYPE_GAME_ROTATION_VECTOR` (hardware sensor fusion) is noticeably superior to manual approaches:

- No drift over extended use (tested ~5 minutes continuous rotation)
- No gimbal lock artifacts
- Clean separation of pitch/yaw/roll axes
- Zero setup required — one line of code: `Input.get_device_orientation()`

---

## Platform Implementation Details

### Data Flow (Android)

```
Sensor.TYPE_GAME_ROTATION_VECTOR (DSP hardware fusion)
  → SensorManager.getQuaternionFromVector()
  → Screen rotation correction (quaternion multiply)
  → JNI bridge → C++ static variable
  → DisplayServerAndroid::process_device_orientation()
  → Coordinate system conversion (negate X, Y)
  → Input::set_device_orientation()
  → User reads via Input.get_device_orientation()
```

### Key Implementation Notes

1. **Sensor priority**: `TYPE_GAME_ROTATION_VECTOR` is preferred (gyro + accelerometer, no magnetometer interference). Falls back to `TYPE_ROTATION_VECTOR` if unavailable.

2. **Screen rotation correction**: Pre-computed correction quaternions handle all 4 screen orientations (0°, 90°, 180°, 270°) via quaternion multiplication. This ensures the returned orientation is always relative to the current screen coordinate system, not the physical device.

3. **Coordinate system conversion**: Android's sensor coordinate system differs from Godot's 3D convention. The X and Y components of the quaternion are negated at the platform bridge layer (`display_server_android.cpp`), so users receive data in Godot's native coordinate system without any manual correction.

4. **Performance**: Zero CPU cost for fusion — all computation is done by the phone's dedicated Sensor Hub DSP. The engine only reads the pre-computed quaternion.

---

## Files Modified (12 files)

### Core (2 files)
- `core/input/input.h` — Added `device_orientation` member + getter/setter
- `core/input/input.cpp` — Binding, ProjectSettings integration

### Android (8 files)
- `Godot.kt` — Sensor registration (GAME_ROTATION_VECTOR with fallback)
- `GodotInputHandler.java` — Quaternion extraction, screen rotation correction
- `InputEventRunnable.java` — New DEVICE_ORIENTATION event type
- `GodotLib.java` — JNI method declaration
- `java_godot_lib_jni.h/cpp` — JNI implementation + coordinate conversion
- `display_server_android.h/cpp` — Bridge to Input system + axis correction

### iOS (3 files)
- `godot_view_apple_embedded.mm` — CMDeviceMotion.attitude reading
- `display_server_apple_embedded.h/cpp` — Bridge to Input system

### Docs (2 files)
- `doc/classes/Input.xml` — API documentation
- `doc/classes/ProjectSettings.xml` — Setting documentation

---

## Comparison with Other Engines

| Feature | Godot (this proposal) | Unity |
|---------|----------------------|-------|
| API | `Input.get_device_orientation()` | `Input.gyro.attitude` |
| Return type | `Quaternion` | `Quaternion` |
| Sensor source | TYPE_GAME_ROTATION_VECTOR | Unknown (system level) |
| Screen rotation handling | Automatic | Manual |
| Setup required | 1 project setting | Enable gyro + manual axis remap |
| Default state | Disabled (battery saving) | Disabled |
