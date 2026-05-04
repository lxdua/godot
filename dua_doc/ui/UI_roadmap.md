# Godot UI 借鉴路线图

> 梳理从 Android / iOS / Flutter / Web / Unreal / Unity UI Toolkit 等成熟框架可以借鉴到 Godot 的 UI 改进方向。
> 每条给出：**来源 / Godot 现状 / 提议形态 / ROI 评估**，尽量做到可直接拆为独立 proposal。
>
> 状态：构思 v0.1
> 范围：不改变 Godot 的 `Control` 基础心智，而是在其上补齐缺失的原语与系统级能力。

---

## 目录

1. [列表 / 滚动族](#1-列表--滚动族)
2. [布局模型进化](#2-布局模型进化)
3. [适配与响应式](#3-适配与响应式godot-最弱的一块)
4. [主题系统升级](#4-主题系统升级)
5. [输入与焦点](#5-输入与焦点)
6. [动画的结构化](#6-动画的结构化)
7. [渲染性能原语](#7-渲染性能原语)
8. [数据绑定与声明式](#8-数据绑定与声明式)
9. [无障碍](#9-无障碍严重欠账)
10. [导航与 UI 流](#10-导航与-ui-流)
11. [优先级总表](#11-优先级总表)

---

## 1. 列表 / 滚动族

与 `VirtualBoxContainer`（见 `ui/virtual_container/`）同族。做完 `VirtualBoxContainer` 后，下列特性应作为其扩展能力顺势补齐。

### 1.1 DiffUtil：结构化差分更新

| 字段 | 内容 |
|---|---|
| 来源 | Android `RecyclerView.DiffUtil`、Flutter `SliverAnimatedList`、React `key` |
| Godot 现状 | 无。列表增删要自己跟踪哪一项变了，动画完全手搓。 |
| 提议形态 | `VirtualBoxContainer.submit_list(new_ids: PackedInt64Array)`：列表内部跑 Myers diff，自动算出 insert / remove / move，派发对应 `notify_*` 并驱动动画。 |
| ROI | ★★★ 依附在 `VirtualBoxContainer` 上即可，不是独立大工程，做完"动画列表"几乎零代码。 |

### 1.2 Sliver 模型：统一滚动原语

| 字段 | 内容 |
|---|---|
| 来源 | Flutter `CustomScrollView` + `Sliver*` |
| Godot 现状 | "顶部大图 → 滚动折叠为标题栏 + 粘性分组 + 嵌套列表" 要手撸上百行。 |
| 提议形态 | 抽象 `Sliver` 基类，子类含 `SliverList`（≈ VirtualBoxContainer）、`SliverSticky`、`SliverCollapsingHeader`、`SliverFillRemaining`。`ScrollView` 把它们串成主轴。 |
| ROI | ★★☆ 工程量中等，但一次解锁一整套高级 UI 模式。 |

### 1.3 Pull-to-refresh / 无限分页

| 字段 | 内容 |
|---|---|
| 来源 | Android `SwipeRefreshLayout`、iOS `UIRefreshControl`、Android Paging 3 |
| Godot 现状 | 无。各项目重复实现下拉刷新、触底加载、loading spinner。 |
| 提议形态 | `VirtualBoxContainer` 两个信号： `refresh_requested()`、`load_more_requested(tail_index: int)`。分页模式下列表自管节流 / 失败重试。 |
| ROI | ★★★ 成本低，移动 / MMO 背包 / 任务列表刚需。 |

---

## 2. 布局模型进化

### 2.1 `ConstraintContainer`：扁平约束布局

| 字段 | 内容 |
|---|---|
| 来源 | Android `ConstraintLayout`、iOS Auto Layout |
| Godot 现状 | 做"A 在 B 右边 8px，C 水平居中于 A 和 D 之间"要套 5 层容器，UI 树极深。 |
| 提议形态 | 一个扁平的 `ConstraintContainer`，子节点用导出属性挂约束：`left_to`、`left_offset`、`center_x_to`、`aspect_ratio`……一次求解线性方程组。 |
| 风险 | 需要线性求解器（接入 Kiwi / Cassowary 或自写简化版）。 |
| ROI | ★★☆ 架构级收益，大幅压扁 UI 树。 |

### 2.2 `GridContainerLine`：CSS Grid 风格

| 字段 | 内容 |
|---|---|
| 来源 | CSS Grid、SwiftUI `Grid` |
| Godot 现状 | `GridContainer` 等列宽，跨格/混合单位/比例都做不了。 |
| 提议形态 | 导出 `track_sizes: Array`（支持 `1fr`、`auto`、`120px`）、`auto_flow`。子节点导出 `grid_row: Vector2i`、`grid_column: Vector2i` 含跨越。 |
| ROI | ★★☆ 设置页 / HUD / 看板直接起飞。 |

### 2.3 纵横比与固有尺寸

| 字段 | 内容 |
|---|---|
| 来源 | CSS `aspect-ratio`，Flutter `AspectRatio` / `IntrinsicHeight` |
| Godot 现状 | `AspectRatioContainer` 孤立存在，不是布局一等公民。 |
| 提议形态 | `Control.aspect_ratio` 作为 size flags 的补充；`Control.intrinsic_size_hint` 允许子节点上报自然尺寸让容器按需撑开。 |
| ROI | ★☆☆ 补全性。 |

---

## 3. 适配与响应式（Godot 最弱的一块）

### 3.1 Safe Area Insets

| 字段 | 内容 |
|---|---|
| 来源 | iOS `safeAreaInsets`、Android `WindowInsets` |
| Godot 现状 | `DisplayServer.get_display_safe_area()` 存在但无 Control 层面联动。刘海屏 / 手势条 / 软键盘弹出一律要手动在 `_process` 挪锚点。 |
| 提议形态 | `Control.apply_safe_area: int flags`（TOP/BOTTOM/LEFT/RIGHT），自动响应新增的 `DisplayServer.safe_area_changed` 信号；软键盘作为 bottom inset 动态参与。 |
| ROI | ★★★ 移动项目人人踩，错误率极高。 |

### 3.2 Size Classes / 断点

| 字段 | 内容 |
|---|---|
| 来源 | iOS Size Classes、CSS `@media`、Android Resource qualifiers |
| Godot 现状 | 无。一套 UI 适配手机 / 平板 / PC 要么写三套，要么全靠脚本判断 `get_viewport_rect()`。 |
| 提议形态 | `Viewport.ui_size_class`：`COMPACT / REGULAR / EXPANDED`（按宽度阈值自动切）；`BreakpointContainer` 按当前 class 切换子节点或主题变体；`Theme` 支持按 class 覆写。 |
| ROI | ★★★ |

### 3.3 RTL 与双向布局

| 字段 | 内容 |
|---|---|
| 来源 | Android `layoutDirection`、CSS logical properties |
| Godot 现状 | 文本支持 RTL，但 `HBoxContainer` 不镜像；"左/右"锚语义在 RTL 下错位。 |
| 提议形态 | 引入 `start / end` 逻辑锚替代机械的 `left / right`；容器按 locale 自动翻转主轴；编辑器加 "Preview as RTL" 切换。 |
| ROI | ★★☆ 阿语 / 希伯来语市场必需。 |

---

## 4. 主题系统升级

### 4.1 设计令牌 / 变量引用

| 字段 | 内容 |
|---|---|
| 来源 | CSS Custom Properties、Material Design Tokens、Figma Variables |
| Godot 现状 | `Theme` 只有"类名 + 状态 + 项名 → 字面值"的三层字典，**无命名变量**。改一处主色要手动挪十几个地方。 |
| 提议形态 | `Theme.set_token(&"color.primary", Color("#3498db"))`；主题项可引用 token；支持派生（如 `color.primary.hover = color.primary.darken(10%)`）；主题文件保存为可读格式，设计师可参与。 |
| ROI | ★★★ 现代设计工作流必备。做完深色 / 浅色 / 品牌化一键切。 |

### 4.2 伪状态全项支持

| 字段 | 内容 |
|---|---|
| 来源 | CSS `:hover/:focus/:active/:disabled`、Flutter `MaterialState` |
| Godot 现状 | 只有 StyleBox 支持 normal/hover/pressed；颜色、字体、图标需要手写 `mouse_entered/exited`。 |
| 提议形态 | 主题项全面支持状态修饰：`Button / font_color / :hover`、`Button / font_color / :disabled`。优先级按 CSS 规则：`:disabled > :pressed > :hover > :focus > normal`。 |
| ROI | ★★★ 能删掉大量项目里的手写状态脚本。 |

---

## 5. 输入与焦点

### 5.1 输入动作栈（Modal Input Stack）

| 字段 | 内容 |
|---|---|
| 来源 | Unreal `CommonUI` Input Stack、Windows Focus Scope |
| Godot 现状 | 弹出暂停菜单后主游戏仍接 WASD，要手动 `set_process_input(false)` 全家；多个模态共存时会打架。 |
| 提议形态 | `InputActionStack` 单例，每个模态窗口压一层；`Input.is_action_*` 按栈顶可见动作集过滤；与 `Control.mouse_filter` 模型正交。 |
| ROI | ★★★ 每个游戏项目都在重写，错误率高。 |

### 5.2 焦点策略 & 手柄导航增强

| 字段 | 内容 |
|---|---|
| 来源 | Unreal `UINavigation`、Android `android:nextFocus*` |
| Godot 现状 | 有 `focus_neighbor_*` 但手动四向配线；动态增删后需重算；列表内焦点滚出视口不自动跟。 |
| 提议形态 | `FocusGroup` 组件声明一块区域的导航策略（网格按行列最近邻、列表按主轴）；`VirtualBoxContainer` 原生处理"焦点被回收 → 转移到列表本身并 `scroll_to`"；支持 `focus_trap`（模态内循环）。 |
| ROI | ★★★ 主机 / 掌机认证硬指标。 |

### 5.3 手势识别器

| 字段 | 内容 |
|---|---|
| 来源 | iOS `UIGestureRecognizer`、Android `GestureDetector` |
| Godot 现状 | 在 `_input` 里判断 `InputEventScreenDrag` + 自己算距离 / 速度 / 角度，每人一套轮子。 |
| 提议形态 | `control.add_gesture(TapGesture.new(tap_count=2))`、`PanGesture`、`PinchGesture`、`LongPressGesture`；内置互斥策略（长按识别失败才触发点击）。 |
| ROI | ★★★ 移动端必备。 |

---

## 6. 动画的结构化

### 6.1 共享元素过渡 / Hero

| 字段 | 内容 |
|---|---|
| 来源 | Android `SharedElementTransition`、Flutter `Hero` |
| Godot 现状 | 两场景间"把卡片飞到详情页顶部"只能手算 rect + AnimationPlayer。 |
| 提议形态 | `Control.hero_tag: StringName`。场景切换时引擎匹配两端相同 tag 的 Control，自动合成飞跃动画。 |
| ROI | ★★☆ 做得好立刻显档次。 |

### 6.2 布局态动画 / MotionLayout / FLIP

| 字段 | 内容 |
|---|---|
| 来源 | Android `MotionLayout` + `TransitionManager.beginDelayedTransition`、FLIP（First-Last-Invert-Play）技术 |
| Godot 现状 | 容器中 insert / remove / reorder 子节点瞬间跳位置，要平滑得手动记 `rect_position` 再 tween。 |
| 提议形态 | `container.animate_layout_changes = true` + `layout_transition_duration`。内部 FLIP：改前记位置 → 改后记位置 → 反向 offset → tween 回 0。 |
| ROI | ★★★ 与 VirtualBoxContainer 同族、且是其 `notify_*` 动画路径的底座；做完"动画列表"几乎零代码（见 `layout_transition/开发文档.md`）。 |

---

## 7. 渲染性能原语

### 7.1 `RetainerBox` / `InvalidationBox`

| 字段 | 内容 |
|---|---|
| 来源 | Unreal Slate `SRetainerWidget` / `SInvalidationPanel` |
| Godot 现状 | 静态复杂 UI（HUD 背景、小地图、装饰层）每帧重算。 |
| 提议形态 | `RetainerBox`：子 UI 渲到 `SubViewport` 作贴图参与主渲染，内部不改就不重绘。`InvalidationBox`：跟踪子节点 dirty 标记，整块干净时跳过布局 / 绘制。 |
| ROI | ★★☆ 重度 UI 项目（卡牌、RTS、大地图）立竿见影，轻量项目感知低。 |

### 7.2 UI 画布层的渲染提示

| 字段 | 内容 |
|---|---|
| 来源 | 浏览器的 `will-change` / `contain` |
| Godot 现状 | `CanvasLayer` 有但无"这层会频繁变 / 不会变"的元数据，引擎无法分层缓存。 |
| 提议形态 | `CanvasLayer.hint = STATIC / DYNAMIC / TRANSIENT`，引擎据此决定是否合成到离屏 buffer。 |
| ROI | ★★☆ 和上一条组合做。 |

---

## 8. 数据绑定与声明式

### 8.1 可观察属性 / 单向数据绑定

| 字段 | 内容 |
|---|---|
| 来源 | Android `StateFlow` / `LiveData`、SwiftUI `@State` / `@Binding`、Vue `ref` |
| Godot 现状 | 属性变化 → UI 更新走信号，手动接；库存 / 状态栏 / 商店类 UI 工程量大。 |
| 提议形态 | 轻量 `Observable[T]` 资源 + `Control.bind(property, observable, converter)`。仅提供"单向数据绑定原语"，不做完整响应式系统。 |
| 风险 | 容易被做成半吊子响应式；做不好不如不做。 |
| ROI | ★★☆ |

### 8.2 声明式 UI DSL（长期）

| 字段 | 内容 |
|---|---|
| 来源 | Jetpack Compose、SwiftUI、Flutter |
| Godot 现状 | `Control` 树是命令式构建。 |
| 提议形态 | 不动引擎，做 GDScript 级 DSL / 插件：`VBox({"separation": 8}, [Label(...), HBox({}, [...])])`，diff 后挂到 Control 树。 |
| ROI | ★☆☆ 先做社区 addon 探索，不进核心。 |

---

## 9. 无障碍（严重欠账）

| 字段 | 内容 |
|---|---|
| 来源 | Android TalkBack、iOS VoiceOver、Web ARIA |
| Godot 现状 | 4.x 有 AccessibilityDriver 雏形，但 Control 层面 API 很少。基本空白。 |
| 提议形态 | `Control` 增加：`accessibility_label`、`accessibility_role`（button / heading / list / …）、`accessibility_hint`、`accessibility_live`（polite / assertive）。搭配 `Viewport.font_scale` 动态字体缩放、高对比主题预设。 |
| 战略意义 | 欧盟 EAA 2025 合规驱动；越晚做包袱越重。 |
| ROI | ★☆☆（技术 ROI 不高，但战略 ROI 高） |

---

## 10. 导航与 UI 流

### 10.1 导航图作为资源

| 字段 | 内容 |
|---|---|
| 来源 | Android `NavigationComponent`、SwiftUI `NavigationStack` |
| Godot 现状 | `change_scene_to_packed` 仅切场景；复杂 UI 流（登录 → 大厅 → 对局 → 结算）的路由、返回栈、参数传递全手搓。 |
| 提议形态 | `NavigationGraph` 资源：节点 = 场景，边 = 动作，内置 `push / pop / replace / pop_to_root` API 与过渡动画挂钩。 |
| ROI | ★☆☆ 社区对路由共识不足。 |

### 10.2 模态与 Overlay 管理

| 字段 | 内容 |
|---|---|
| 来源 | Flutter `Navigator.push` with dialogs、Web `<dialog>` / Popover API |
| Godot 现状 | 对话框系统每个项目都手撸一遍。 |
| 提议形态 | 全局 `OverlayStack.push_modal(scene, barrier_dismiss=true)`，自动处理遮罩、焦点陷阱、返回键。 |
| ROI | ★★☆ |

---

## 11. 优先级总表

按"痛点普遍程度 × 可落地性 × 投入产出"综合排序：

| 优先级 | 项目 | 所属章节 | 理由 |
|---|---|---|---|
| ★★★ | Safe Area Insets + 软键盘响应 | 3.1 | 移动项目必踩；成本低 |
| ★★★ | 主题令牌 / 变量引用 | 4.1 | 现代设计工作流必需；架构级长期收益 |
| ★★★ | 伪状态主题（全项） | 4.2 | 天天遇到；动一处得利全局 |
| ★★★ | 输入动作栈 | 5.1 | 每个游戏项目都重写；错误率高 |
| ★★★ | 手柄导航增强（FocusGroup） | 5.2 | 主机认证硬指标 |
| ★★★ | 手势识别器 | 5.3 | 移动 UI 必备 |
| ★★★ | Size Classes / 断点 | 3.2 | 一套 UI 跑多形态 |
| ★★★ | VirtualBoxContainer | 见 `virtual_container/` | 已有专项文档 |
| ★★★ | 布局态动画（FLIP） | 6.2 | VirtualBoxContainer `notify_*` 动画路径的底座；优先于虚拟化实现 |
| ★★☆ | VirtualBoxContainer + DiffUtil + 分页 | 1.1 / 1.3 | 与虚拟化容器同族，顺势补齐 |
| ★★☆ | Sliver 模型 | 1.2 | 一次解锁一整套高级模式 |
| ★★☆ | ConstraintContainer | 2.1 | 架构级提升，工程量中等 |
| ★★☆ | `GridContainerLine`（CSS Grid） | 2.2 | 看板 / 设置页提效 |
| ★★☆ | 共享元素 / Hero | 6.1 | 依赖 FLIP 基础设施 |
| ★★☆ | RetainerBox / InvalidationBox | 7.1 | 重度 UI 项目关键 |
| ★★☆ | CanvasLayer 渲染提示 | 7.2 | 与 7.1 组合做 |
| ★★☆ | RTL / 逻辑锚 | 3.3 | 国际化 |
| ★★☆ | 可观察属性 / 单向绑定 | 8.1 | 需克制设计，不做过度 |
| ★★☆ | Overlay / Modal 管理 | 10.2 | 对话框系统标准化 |
| ★☆☆ | 纵横比 / 固有尺寸 | 2.3 | 补全性 |
| ★☆☆ | 无障碍 API | 9 | 战略重要，长期投入 |
| ★☆☆ | 导航图资源 | 10.1 | 社区共识不足 |
| ★☆☆ | 声明式 DSL | 8.2 | 先做社区 addon |

---

## 12. 推进建议

1. **近期（3~6 个月）**：集中火力完成 **Safe Area + 主题令牌 + 伪状态 + 输入动作栈** 四件套。这四项都是"无争议刚需 + 工程量可控"，也是目前社区频繁重复造轮的热点。
2. **中期（6~12 个月）**：**布局态动画（FLIP）→ VirtualBoxContainer 族 → 手势识别器 + ConstraintContainer**。动画先行的理由：
   - VirtualBoxContainer 的 `notify_item_inserted / _removed / _moved` 需要 Container FLIP 作为底座（见 `layout_transition/开发文档.md` §6.9 与 `virtual_container/开发文档.md` §6.9）
   - FLIP 独立可用，先做能立刻改善普通 `VBox` / `HBox` / `Grid` 的观感，是纯增量收益
   - VirtualBoxContainer 量级更大，留在 FLIP 稳定之后再推，减少相互耦合风险
3. **长期**：**Sliver 模型 / 声明式 DSL / 无障碍**。需要社区共识与长期投入，不宜一次提大 PR。
4. **所有条目的共同推进模式**：
   - 先在用户仓做原型 / addon；
   - 收集真实项目反馈与 API 验证；
   - 再作为完整 proposal（description + API + mock + migration）提 godot-proposals；
   - 最后伴随参考实现 PR 推进合并。

---

## 附：同族文档索引

- `ui/virtual_container/开发文档.md`、`ui/virtual_container/proposal.md` — 本路线图第 1 章 / §11 ★★★（已单独深度设计）
- `ui/layout_transition/开发文档.md` — 本路线图 §6.2 布局态动画 / FLIP 专项设计（★★★，VirtualBoxContainer 动画路径的底座）
- `ui/ScrollContainer平滑滚动.md` — 与第 1 章滚动族相关的既有工作
- `architecture/ObjectPool对象池系统.md` — 与第 1 章回收池设计理念相通
- `architecture/Subsystem子系统架构设计方案.md` — 与第 5.1 输入动作栈的栈式架构相关
