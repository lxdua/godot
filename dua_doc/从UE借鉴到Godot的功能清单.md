# 从 Unreal Engine 借鉴到 Godot 的功能清单

> 基于 Godot 4.7-dev 源码，梳理 UE 中值得借鉴到 Godot 的功能点。
> 按优先级和可行性分类整理。

---

## 一、高价值 & 可行性强

### 1. Gameplay Ability System (GAS) — 技能/能力系统

- **UE 的做法**：GAS 提供了一套标准化的 Ability、Effect、Attribute、Tag 系统，用于构建 RPG/动作游戏的技能体系
- **Godot 的差距**：Godot 完全没有类似系统，开发者需要从零造轮子
- **建议**：可以做一个轻量版的 `GameplayAbilitySystem`，基于 Godot 的 Node/Resource 体系实现
- **涉及模块**：`scene/`、`modules/`

### 2. Gameplay Tag 系统

- **UE 的做法**：层级化的标签系统（如 `Damage.Fire.DoT`），支持父子匹配、容器查询
- **Godot 的差距**：Godot 只有 `groups` 和 `meta`，缺乏层级化标签
- **建议**：这个实现相对简单，收益很大，适合做成核心模块
- **涉及模块**：`core/`、`editor/`

### 3. Enhanced Input System — 增强输入系统

- **UE 的做法**：输入动作 + 输入映射上下文 + 修饰器 + 触发器，支持优先级和动态切换
- **Godot 的差距**：Godot 的 InputMap 比较基础，缺少上下文切换、输入修饰器、组合输入等
- **建议**：可以扩展现有 `InputMap`，增加 InputMappingContext 和 Modifier/Trigger 概念
- **涉及模块**：`core/input/`、`scene/main/`

### 4. Data Table / Data Asset 系统

- **UE 的做法**：DataTable 可以从 CSV/JSON 导入结构化游戏数据，DataAsset 是轻量级数据容器
- **Godot 的差距**：虽然有 Resource，但缺乏批量数据导入和表格化编辑工具
- **建议**：在编辑器中增加 DataTable 编辑视图，支持 CSV 导入导出
- **涉及模块**：`editor/`、`core/io/`

---

## 二、架构级改进

### 5. Subsystem 架构

- **UE 的做法**：GameInstance Subsystem / World Subsystem / LocalPlayer Subsystem，生命周期自动管理
- **Godot 的差距**：Godot 用 Autoload 单例模式，缺乏生命周期绑定和作用域管理
- **建议**：引入 Subsystem 概念，绑定到 SceneTree / Viewport / 特定 Node 的生命周期
- **涉及模块**：`scene/main/`、`core/`

### 6. World Partition / Level Streaming — 世界分区与关卡流式加载

- **UE 的做法**：World Partition 自动按网格流式加载大世界，HLOD 系统
- **Godot 的差距**：Godot 4 虽然有 `ResourceLoader` 异步加载，但缺少自动化的世界分区系统
- **建议**：可做一个轻量级的 WorldPartition 节点，自动管理子场景的加载卸载
- **涉及模块**：`scene/main/`、`scene/3d/`、`servers/`

### 7. Gameplay Framework（GameMode / GameState / PlayerController）

- **UE 的做法**：标准化的游戏框架类，定义了游戏规则、状态、玩家控制器的分层
- **Godot 的差距**：Godot 完全没有这层抽象，每个项目都要自己搭架子
- **建议**：可以做成可选模块（module），提供 `GameMode`、`GameState`、`PlayerController` 基类
- **涉及模块**：`modules/`、`scene/main/`

---

## 三、编辑器 & 工作流

### 8. Blueprint 式可视化脚本的回归（改进版）

- **UE 的做法**：Blueprint 虽然被吐槽，但在策划/美术手中是神器
- **Godot 的差距**：VisualScript 在 4.0 被移除了
- **建议**：可以参考 UE Blueprint 的数据流 + 执行流模型，重新设计一个更简洁的可视化脚本
- **涉及模块**：`modules/`、`editor/`

### 9. Asset Validation / Data Validation 框架

- **UE 的做法**：可以编写验证规则，批量检查资产合规性
- **Godot 的差距**：缺乏系统化的资产验证工具
- **建议**：在导出/CI 流程中增加资产验证钩子
- **涉及模块**：`editor/`、`core/io/`

### 10. 编辑器工具控件（Editor Utility Widget）

- **UE 的做法**：可以用 Blueprint/C++ 快速创建编辑器内工具面板
- **Godot 的差距**：虽然有 `@tool` 和 EditorPlugin，但创建自定义编辑器面板的体验不够流畅
- **建议**：简化 EditorPlugin 的 API，提供更多开箱即用的编辑器控件模板
- **涉及模块**：`editor/`

---

## 四、网络 & 多人

### 11. Replication Graph / Relevancy 系统

- **UE 的做法**：基于相关性的网络同步，只同步对玩家有意义的 Actor
- **Godot 的差距**：Godot 4 的 MultiplayerSynchronizer 比较基础，缺乏空间相关性优化
- **建议**：为 MultiplayerSynchronizer 增加距离/区域相关性过滤
- **涉及模块**：`modules/multiplayer/`、`scene/main/`

---

## 优先推荐 Top 5

| 排名 | 功能 | 原因 |
|------|------|------|
| 1 | **Gameplay Tag** | 实现简单，收益巨大，几乎所有游戏类型都能用 |
| 2 | **Enhanced Input** | 痛点明确，Godot 社区呼声很高，设计文档已完成 |
| 3 | **Subsystem 架构** | 改善 Autoload 滥用问题，提升架构质量 |
| 4 | **Data Table** | 策划友好，提升数据驱动开发体验 |
| 5 | **GAS (轻量版)** | 对 RPG/动作品类是杀手级功能 |

---

## 实施原则

1. **Godot 风格优先**：不是照搬 UE，而是用 Godot 的 Node/Resource/Signal 哲学重新设计
2. **轻量化**：UE 的系统往往过度工程化，Godot 版本应追求简洁
3. **可选模块**：大多数功能建议做成 `modules/` 下的可选模块，不增加核心包体积
4. **编辑器集成**：每个功能都应有对应的编辑器面板支持，降低使用门槛
5. **GDScript 友好**：API 设计优先考虑 GDScript 的使用体验

---

## 实施进度

### ✅ Gameplay Tag 系统 — 已完成（core 层）

已实现文件：
- `core/gameplay_tag/gameplay_tag.h/.cpp` — GameplayTag 类（RefCounted）
- `core/gameplay_tag/gameplay_tag_container.h/.cpp` — GameplayTagContainer 类（Resource）
- `core/gameplay_tag/gameplay_tag_manager.h/.cpp` — GameplayTagManager 全局单例
- `core/gameplay_tag/SCsub` — 构建脚本

修改的文件：
- `core/SCsub` — 添加 gameplay_tag 子目录
- `core/register_core_types.cpp` — 注册类型和单例
