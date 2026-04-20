# DuaDot Engine

**基于 [Godot Engine](https://godotengine.org) 的定制游戏引擎**

---

## ✨ 新增功能

### 已实施

| 功能 | 说明 | 文档 |
|------|------|------|
| **增强输入系统** | 借鉴 UE 的 Input Action / Mapping 架构 | [设计](dua_doc/EnhancedInput增强输入系统.md) · [计划](dua_doc/EnhancedInput落地实施计划.md) |
| **Gameplay Tag** | 层级标签系统 | [设计](dua_doc/GameplayTag层级标签系统.md) |
| **设备姿态 API** | `Input.get_device_orientation()` 返回硬件融合四元数，Android/iOS 双平台 | [方案](dua_doc/DeviceOrientation设备姿态API方案.md) · [总结](dua_doc/DeviceOrientation设备姿态API实施总结.md) |
| **传感器采样率控制** | 项目设置 `update_rate_hz` 可配置传感器频率（1~200Hz） | [文档](dua_doc/SensorUpdateRate传感器采样率控制.md) |
| **Camera2D 相机震动** | Camera2D 内置震动系统 | [文档](dua_doc/Camera2D相机震动.md) |
| **Canvas 后处理** | 2D 画布后处理管线 | [文档](dua_doc/CanvasPostProcess后处理.md) |
| **平滑滚动** | ScrollContainer 惯性滚动增强 | [文档](dua_doc/ScrollContainer平滑滚动.md) |
| **屏幕空间描边** | Inverted Hull 等宽描边方案 | [文档](dua_doc/Inverted%20Hull屏幕空间等宽描边.md) |
| **GDScript 抽象类 Quick Fix** | 编辑器错误面板一键实现抽象方法 `[Implement All]` | [文档](dua_doc/GDScript抽象类QuickFix.md) |
| **导航 BVH 空间索引** | 为导航多边形查询引入 BVH 加速，O(N) → O(log N) | [设计](dua_doc/navigation/impl_polygon_spatial_index.md) · [改动记录](dua_doc/navigation/impl_polygon_spatial_index_changelog.md) · [测试](dua_doc/navigation/impl_polygon_spatial_index_test.md) |
| **Bug 修复：法线未归一化** | `map_get_closest_point_normal()` 返回未归一化向量（Godot 原有 bug） | [记录](dua_doc/navigation/bugfix_closest_point_normal_unnormalized.md) |
| **Bug 修复：include_regions 笔误** | `included_regions` 白名单下 NavLink 过滤误用 `excluded_regions`（Godot 原有 bug） | — |

### 未实施

| 功能 | 说明 | 文档 |
|------|------|------|
| **Shader 参数集合** | 全局着色器参数集合管理 | [设计](dua_doc/ShaderParameterCollection着色器参数集合.md) |
| **对象池** | 通用对象池系统 | [设计](dua_doc/ObjectPool对象池系统.md) |
| **子系统架构** | 借鉴 UE Subsystem 的生命周期管理 | [设计](dua_doc/Subsystem子系统架构设计方案.md) |
| **UI 智能测距** | Figma 风格的 Alt 悬停测距与盒模型可视化 | [设计](dua_doc/figma_smart_measure_proposal.md) |
| **Funnel Radius 感知** | 路径后处理考虑 Agent 半径，避免贴墙 | [设计](dua_doc/navigation/impl_funnel_radius_aware.md) |
| **Tile 增量更新** | 将全量重建改为基于 Tile 的局部更新，含交错布局优化 | [设计](dua_doc/navigation/impl_tile_based_incremental_update.md) |
| **动态障碍物切割** | 运行时障碍物对 NavMesh 的实时切割 | [设计](dua_doc/navigation/impl_dynamic_obstacle_pathfinding.md) |
| **可变 Agent 尺寸** | 不同半径/高度的 Agent 使用不同 NavMesh | [设计](dua_doc/navigation/impl_variable_agent_size.md) |

> 完整的功能借鉴清单见 [从 UE 借鉴到 Godot 的功能清单](dua_doc/从UE借鉴到Godot的功能清单.md)

---

## 📜 许可证

与 Godot Engine 一致，采用 [MIT 许可证](LICENSE.txt)。
