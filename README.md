# DuaDot Engine

**基于 [Godot Engine](https://godotengine.org) 的定制游戏引擎**，专注于移动端游戏开发体验增强。在 Godot 强大的跨平台能力基础上，补充了移动端传感器、增强输入、后处理、对象池等实用功能。

> 上游版本：Godot 4.x | 许可证：MIT

---

## ✨ 新增功能

### 已实施

| 功能 | 说明 | 文档 |
|------|------|------|
| **设备姿态 API** | `Input.get_device_orientation()` 返回硬件融合四元数，Android/iOS 双平台 | [方案](dua_doc/DeviceOrientation设备姿态API方案.md) · [总结](dua_doc/DeviceOrientation设备姿态API实施总结.md) |
| **传感器采样率控制** | 项目设置 `update_rate_hz` 可配置传感器频率（1~200Hz） | [文档](dua_doc/SensorUpdateRate传感器采样率控制.md) |
| **Camera2D 相机震动** | Camera2D 内置震动系统 | [文档](dua_doc/Camera2D相机震动.md) |
| **Canvas 后处理** | 2D 画布后处理管线 | [文档](dua_doc/CanvasPostProcess后处理.md) |
| **平滑滚动** | ScrollContainer 惯性滚动增强 | [文档](dua_doc/ScrollContainer平滑滚动.md) |
| **屏幕空间描边** | Inverted Hull 等宽描边方案 | [文档](dua_doc/Inverted%20Hull屏幕空间等宽描边.md) |
| **Shader 参数集合** | 全局着色器参数集合管理 | [文档](dua_doc/ShaderParameterCollection着色器参数集合.md) |

### 设计中

| 功能 | 说明 | 文档 |
|------|------|------|
| **增强输入系统** | 借鉴 UE 的 Input Action / Mapping 架构 | [设计](dua_doc/EnhancedInput增强输入系统.md) · [计划](dua_doc/EnhancedInput落地实施计划.md) |
| **Gameplay Tag** | 层级标签系统 | [设计](dua_doc/GameplayTag层级标签系统.md) |
| **对象池** | 通用对象池系统 | [设计](dua_doc/ObjectPool对象池系统.md) |
| **子系统架构** | 借鉴 UE Subsystem 的生命周期管理 | [设计](dua_doc/Subsystem子系统架构设计方案.md) |

> 完整的功能借鉴清单见 [从 UE 借鉴到 Godot 的功能清单](dua_doc/从UE借鉴到Godot的功能清单.md)

---

## 🚀 快速开始

### 二进制下载

> TODO: 发布编译好的编辑器和导出模板

### 从源码编译

环境要求与 Godot 官方一致，参考 [Godot 编译文档](https://docs.godotengine.org/en/latest/engine_details/development/compiling)。

```bash
# 编译编辑器（Windows）
scons platform=windows target=editor

# 编译 Android 导出模板
scons platform=android target=template_debug arch=arm64
scons platform=android target=template_release arch=arm64

# 编译 iOS 导出模板
scons platform=ios target=template_debug arch=arm64
scons platform=ios target=template_release arch=arm64
```

---

## 📂 项目结构

```
├── core/                  # 引擎核心（Input、Math、Object 等）
├── drivers/               # 平台驱动（apple_embedded、vulkan 等）
├── platform/              # 平台适配层（android、ios、windows 等）
├── scene/                 # 场景系统（2D/3D 节点、GUI 等）
├── servers/               # 服务器（渲染、物理、音频等）
├── modules/               # 可选模块
├── doc/classes/           # API 文档（XML 格式）
├── dua_doc/               # 📝 DuaDot 自定义功能的设计文档与实施记录
└── README.md
```

---

## 📖 文档

- **DuaDot 新增功能文档**：[dua_doc/](dua_doc/) 目录
- **Godot 官方文档**：[docs.godotengine.org](https://docs.godotengine.org)
- **API 参考**：编辑器内 → 帮助 → 搜索类名
- **工作流程**：[dua_doc/工作流程.md](dua_doc/工作流程.md)

---

## 🔗 上游同步

本仓库基于 Godot 官方仓库 fork，定期同步上游更新：

```bash
git remote add upstream https://github.com/godotengine/godot.git
git fetch upstream
git merge upstream/master
```

DuaDot 的所有改动都有对应的 `dua_doc/` 文档记录，便于合并时解决冲突。

---

## 📜 许可证

与 Godot Engine 一致，采用 [MIT 许可证](LICENSE.txt)。
