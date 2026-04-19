# Godot UI 智能测距功能 (Figma-like Smart Measure) 实现方案

## 1. 背景与痛点
目前在 Godot 引擎中进行 UI 搭建时，开发人员和美术人员面临着“设计与实现割裂”的痛点。Figma 等现代设计工具提供了直观的“智能测距”（按住 Alt/Option 键悬停查看元素间距、尺寸、相对位置）和“盒模型可视化”功能，而 Godot 原生的 2D 编辑器缺乏此类微交互。这导致：
*   **复刻成本高**：程序需要手动计算坐标、边距，难以快速验证与设计稿的 1:1 还原度。
*   **调试困难**：在复杂的容器嵌套（VBox/HBox/MarginContainer）中，难以直观看出是哪个节点的 Margin 或 Separation 导致了排版错误。
*   **沟通成本高**：美术无法在引擎中直观地检查 UI 还原情况，必须依赖程序截图或运行游戏。

## 2. 核心功能需求
*   **智能测距 (Smart Measurement)**：在 2D 视图中选中一个 Control 节点，按住特定快捷键（如 Alt），鼠标悬停在其他 Control 节点上时，实时绘制出两者之间的相对距离（上、下、左、右的像素差）和辅助线。
*   **尺寸与位置提示 (Hover Info)**：鼠标悬停在节点上时，显示一个轻量级的 Tooltip，包含节点的 Size、Position、Anchors 等关键信息。
*   **盒模型可视化 (Box Model Overlay)**：可选功能，高亮显示 MarginContainer 的内边距区域，或 BoxContainer 的子节点间距（Separation）区域。

## 3. 实现方案一：Godot 编辑器插件 (Editor Plugin)
这是最推荐的短期落地方式。Godot 的编辑器本身就是用 Godot 编写的，其强大的插件系统允许我们深度定制 2D 视图的交互。

### 3.1 技术路线
1.  **创建 EditorPlugin**：编写一个继承自 `EditorPlugin` 的工具脚本。
2.  **拦截输入事件**：重写 `_ForwardCanvasGuiInput(event)` 方法，监听 2D 视图中的鼠标移动（MouseMotion）和按键（Alt 键）事件。
3.  **节点拾取**：利用 `EditorInterface.GetSelection().GetSelectedNodes()` 获取当前选中的基准节点。通过鼠标坐标和 `GetTree().Root.GetViewport().GuiGetHoveredControl()`（或自定义的射线检测/包围盒碰撞）获取当前鼠标悬停的目标节点。
4.  **计算与绘制**：
    *   计算基准节点和目标节点的 `GlobalRect`（全局包围盒）。
    *   计算两者边缘的垂直和水平距离。
    *   在 2D 视图的 Overlay 层（可以通过向 2D 编辑器视口注入一个自定义的 `Control` 节点作为画布）使用 `_Draw()` 方法绘制红色的虚线、箭头和距离数值文本。

### 3.2 优点与缺点
*   **优点**：开发成本极低（1-2名程序几天即可完成原型）；无需修改引擎源码，即插即用；方便在团队内部快速迭代和分发；可以作为开源项目发布到 Asset Library。
*   **缺点**：性能可能略逊于 C++ 原生实现（但在编辑器环境下通常可忽略）；可能与 Godot 原生的一些 2D 拖拽操作产生轻微的快捷键冲突，需要细致处理事件拦截逻辑。

## 4. 实现方案二：Godot 引擎源码集成 (Engine Integration)
如果希望将此功能作为 Godot 的原生特性，或者追求极致的性能和深度集成，可以考虑直接修改 Godot 引擎的 C++ 源码。

### 4.1 技术路线
1.  **定位源码**：主要修改 `editor/plugins/canvas_item_editor_plugin.cpp` 和相关的头文件。这是 Godot 2D 编辑器的核心逻辑所在。
2.  **状态机扩展**：在 `CanvasItemEditor` 的状态机中增加一个新的状态（例如 `TOOL_MEASURE`）或在现有的选择/移动状态中增加对 Alt 键的修饰符处理。
3.  **底层绘制**：利用 Godot 底层的 `RenderingServer` 或 `CanvasItem` 的绘制 API，在编辑器的上层 Overlay 中直接绘制测距线和文本。
4.  **UI 暴露**：在编辑器设置（Editor Settings）中添加相关选项，允许用户自定义测距线的颜色、字体大小或开启/关闭该功能。

### 4.2 优点与缺点
*   **优点**：性能最佳；与编辑器原生操作（如吸附、网格）结合更紧密；可以成为 Godot 官方的卖点功能。
*   **缺点**：开发门槛高，需要熟悉 Godot 庞大的 C++ 源码架构；每次引擎升级都需要重新编译自定义版本（除非提交 PR 并被官方合并，但这通常需要漫长的审核周期）。

## 5. 总结与建议
对于绝大多数游戏开发团队而言，**方案一（编辑器插件）是性价比最高的选择**。它能够以极低的成本迅速解决当前的 UI 开发痛点，提升美术与程序的协作效率。

建议团队内的工具向程序（TA 或 Tool Programmer）先花 1-2 天时间，使用 GDScript 或 C# 编写一个基础版本的测距插件，在内部项目中试用并收集反馈，后续再逐步完善“盒模型可视化”等高级功能。
