# Android 导出模板编译与测试教程

> 目标：用修改过的引擎源码编译 Android 导出模板，在真机上测试设备姿态 API。

---

## 一、环境准备

### 1.1 安装 Android Studio

1. 下载 Android Studio：https://developer.android.com/studio
2. 安装时选择默认配置即可
3. 首次启动会自动下载 Android SDK

### 1.2 安装 SDK 和 NDK

打开 Android Studio → Settings → Languages & Frameworks → Android SDK：

**SDK Platforms 标签页**：
- 勾选 **Android 14 (API 34)**（或最新版本）

**SDK Tools 标签页**：
- 勾选 **Android SDK Build-Tools**（最新版）
- 勾选 **NDK (Side by side)**（版本 23.2.x 或更新）
- 勾选 **Android SDK Command-line Tools**
- 勾选 **CMake**

点击 Apply 等待下载完成。

### 1.3 记住安装路径

默认路径通常是：
```
SDK: C:\Users\<你的用户名>\AppData\Local\Android\Sdk
NDK: C:\Users\<你的用户名>\AppData\Local\Android\Sdk\ndk\<版本号>
```

### 1.4 配置环境变量

在系统环境变量中添加：

| 变量名 | 值 |
|--------|---|
| `ANDROID_HOME` | `C:\Users\<用户名>\AppData\Local\Android\Sdk` |
| `ANDROID_NDK_ROOT` | `C:\Users\<用户名>\AppData\Local\Android\Sdk\ndk\<版本号>` |

并将以下路径加入 `PATH`：
```
%ANDROID_HOME%\platform-tools
%ANDROID_HOME%\cmdline-tools\latest\bin
```

### 1.5 安装 Java JDK 17

Android 编译需要 JDK 17。如果已装 Android Studio 则自带了，路径通常在：
```
C:\Program Files\Android\Android Studio\jbr
```

设置环境变量：
| 变量名 | 值 |
|--------|---|
| `JAVA_HOME` | `C:\Program Files\Android\Android Studio\jbr` |

---

## 二、编译 Android 导出模板

### 2.1 验证环境

打开 CMD，逐个验证：

```bash
# 确认 Python 和 SCons（编译编辑器时应该已经有了）
python --version
scons --version

# 确认 Java
java -version

# 确认 Android SDK
adb --version
```

### 2.2 编译命令

在引擎根目录（`e:\UGit\godot`）执行：

```bash
# 编译 debug 版 Android 模板（用于测试）
# arch=arm64 是现在绝大多数 Android 手机的架构
scons platform=android target=template_debug arch=arm64 -j8
```

`-j8` 是并行编译线程数，根据你的 CPU 核心数调整（比如 12 核用 `-j12`）。

首次编译大约需要 **15-30 分钟**。

### 2.3 编译产物

编译完成后，产物在：
```
e:\UGit\godot\bin\android_debug.apk
```

实际上 Godot Android 模板是一个 gradle 项目，最终的 APK 需要通过 gradle 构建。运行：

```bash
cd e:\UGit\godot\platform\android\java
# Windows 用 gradlew.bat
gradlew.bat generateGodotTemplates
```

模板会生成在：
```
e:\UGit\godot\bin\android_debug.apk
e:\UGit\godot\bin\android_release.apk
```

---

## 三、在编辑器中配置 Android 导出

### 3.1 配置编辑器的 Android 设置

打开你的自定义引擎编辑器 → Editor → Editor Settings → Export → Android：

| 设置项 | 值 |
|--------|---|
| **Android SDK Path** | `C:\Users\<用户名>\AppData\Local\Android\Sdk` |
| **Debug Keystore** | 留空（会自动生成）或指定已有的 debug.keystore |

### 3.2 使用自定义导出模板

1. 打开测试项目 → Project → Export → 点击 **Add...** → 选择 **Android**
2. 在 Android 导出设置中：
   - **Custom Template → Debug**：选择你编译出的 `android_debug.apk`
3. 其他设置保持默认

---

## 四、创建测试项目

在 `C:\Users\linxinda\Desktop\引擎测试` 中新建测试脚本（或新建一个单独的项目）。

### 4.1 项目设置

在 Project → Project Settings 中：
- `input_devices/sensors/enable_gyroscope` → **true**
- `input_devices/sensors/enable_device_orientation` → **true**

### 4.2 测试脚本

创建一个简单的场景，挂上以下脚本：

```gdscript
extends Control

@onready var label: Label = $Label

func _process(_delta: float) -> void:
    var orientation := Input.get_device_orientation()
    var euler := orientation.get_euler()
    
    label.text = """Device Orientation Test
    
Quaternion:
  x = %.4f
  y = %.4f
  z = %.4f
  w = %.4f

Euler (rad):
  Pitch = %.4f
  Yaw   = %.4f
  Roll  = %.4f

Euler (deg):
  Pitch = %.1f°
  Yaw   = %.1f°
  Roll  = %.1f°
""" % [
        orientation.x, orientation.y, orientation.z, orientation.w,
        euler.x, euler.y, euler.z,
        rad_to_deg(euler.x), rad_to_deg(euler.y), rad_to_deg(euler.z)
    ]
```

场景结构：
```
Control (挂脚本)
  └── Label (全屏，字体大一点)
```

---

## 五、真机运行

### 5.1 手机开启开发者模式

1. 设置 → 关于手机 → 连续点击「版本号」7 次
2. 返回设置 → 开发者选项 → 打开 **USB 调试**

### 5.2 连接手机

1. USB 数据线连接手机和电脑
2. 手机弹出「允许 USB 调试」→ 点允许
3. CMD 中运行 `adb devices`，应显示你的设备

### 5.3 一键运行

在编辑器中：
1. 点击右上角的 **Android 小图标**（Remote Debug）
2. 或者 Project → Export → Android → **Export Project** 生成 APK，然后手动安装

### 5.4 手动安装 APK

如果一键运行不成功，可以：
```bash
# 导出 APK 后
adb install -r path/to/your_game.apk
```

然后在手机上找到并打开 App。

---

## 六、测试用例

### 6.1 基础功能测试

| 操作 | 预期 |
|------|------|
| 手机平放桌上 | Quaternion 接近 IDENTITY；Roll/Pitch ≈ 0° |
| 手机竖起来（看手机姿势） | Pitch ≈ -90° |
| 手机向左倾斜 45° | Roll ≈ -45° |
| 手机向右倾斜 45° | Roll ≈ 45° |
| 手机面朝下扣桌上 | Pitch ≈ 180° 或 Roll 跳变 |
| 缓慢旋转手机 | 数值平滑变化，无突跳 |
| 快速摇晃手机 | 数值快速跟随，无明显延迟 |

### 6.2 对比测试

同时显示 `get_gravity()` 和 `get_device_orientation()` 的数据：
- gravity 应该有更多抖动
- orientation 应该更平滑
- 快速运动时 gravity 会受线性加速度影响，orientation 不会

### 6.3 屏幕旋转测试

锁定横屏运行 → 数据应该自动修正为横屏坐标系（Roll 轴不变）。

---

## 七、常见问题

| 问题 | 解决方法 |
|------|---------|
| 编译报错找不到 Android SDK | 检查 `ANDROID_HOME` 环境变量 |
| 编译报错找不到 NDK | 检查 `ANDROID_NDK_ROOT` 环境变量，确认 NDK 版本号目录存在 |
| `adb devices` 显示空列表 | 检查 USB 连接、USB 调试是否开启、驱动是否安装 |
| 手机上 orientation 全是 0 | 检查项目设置中 `enable_device_orientation` 是否为 true |
| 编辑器没有 Android 导出选项 | 需要先在 Editor Settings 中配置 Android SDK Path |
| gradle 报错 | 确认 `JAVA_HOME` 指向 JDK 17 |
