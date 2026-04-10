
# Godot 引擎源码学习路径

> 学习 Godot 引擎源码的推荐路径，从核心基石到上层系统逐步深入。

---

## 目录

### 第一阶段：核心基石

1. **[Object + ClassDB + MethodBind](01_object_classdb_methodbind.md)** —— 反射系统
   - `GDCLASS` 宏如何让 C++ 类获得反射能力
   - `ClassDB` 全局类数据库：方法、属性、信号、常量注册
   - `MethodBind` 的三种调用路径：`call` / `validated_call` / `ptrcall`
   - `Object` 基类统一的 `set/get`、`notification`、信号、Meta、脚本挂载
   - 📂 `core/object/`

2. **[Variant](02_variant.md)** —— 万能类型容器
   - union + 类型标签实现零分配值存储
   - `PackedArrayRef` 的 COW（Copy-On-Write）语义
   - 运算符、构造器、方法通过函数指针表注册（无巨型 switch-case）
   - 📂 `core/variant/`

3. **[Signal + Callable + MessageQueue](03_signal_callable_messagequeue.md)** —— 通信机制
   - Signal 类型安全注册 & 发射
   - `Callable` 设计：成员方法、lambda、bind/unbind
   - `CONNECT_DEFERRED` / `CONNECT_ONE_SHOT` 等连接标志
   - `MessageQueue`（`CallQueue`）延迟调用的消息队列实现
   - 📂 `core/object/object.cpp`, `core/object/callable.h`, `core/object/message_queue.h`

---

### 第二阶段：场景 & 架构

4. **[Node + SceneTree](04_node_scenetree.md)** —— 场景树与组合式架构
   - `Node` 生命周期：`_enter_tree` / `_ready` / `_process` / `_exit_tree`
   - `Notification` 机制：整数通知码贯穿 C++ 和脚本
   - `Group` + `call_group` / `notify_group`：场景树广播
   - Owner 机制 + `PackedScene`：场景作为可复用预制件
   - 📂 `scene/main/`

5. **[Server 架构](05_server_architecture.md)** —— 前后端分离
   - RenderingServer / PhysicsServer / AudioServer / NavigationServer / DisplayServer
   - 场景节点只是 Server 资源的前端代理
   - `RID`（Resource ID）句柄管理
   - 跨线程通信：`CommandQueue` / `MessageQueue`
   - 后端可替换（Vulkan / OpenGL / Metal）
   - 📂 `servers/`

6. **[Resource 系统](06_resource.md)** —— 数据驱动
   - Resource 引用计数 + 全局路径缓存（`ResourceCache`）
   - `.tres`（文本）/ `.res`（二进制）序列化格式
   - `@export` 让脚本属性在编辑器中可编辑
   - Sub-resource 机制实现资源嵌套
   - 📂 `core/io/resource.h`

---

### 第三阶段：性能 & 底层

7. **[自研容器库](07_templates.md)** —— STL 替代方案
   - `Vector` / `LocalVector`：CowData vs 轻量版
   - `HashMap` / `AHashMap`：开放寻址 + Robin Hood 哈希
   - `RID_Owner`：分代 ID + 自旋锁的资源句柄管理
   - `PagedAllocator`：页式分配器
   - `CommandQueueMT`：低竞争跨线程命令队列
   - 📂 `core/templates/`

---

### 第四阶段：脚本 & 扩展

8. **[GDScript 编译管线](08_gdscript.md)** —— 嵌入式语言设计
   - Tokenizer → Parser → Analyzer → Compiler → VM 完整管线
   - 脚本与 Object / ClassDB / Variant 的无缝集成
   - `ScriptInstance` 接口：C++ 行为与脚本行为共存
   - `ScriptLanguage` 抽象：支持多语言
   - 📂 `modules/gdscript/`

9. **[GDExtension](09_gdextension.md)** —— 原生插件系统
   - `ObjectGDExtension` 注册新 Object 子类
   - `ClassDB::register_extension_class` 融入类型系统
   - ABI 稳定的原生接口设计
   - 热重载支持
   - 📂 `core/extension/`

---

### 番外：实战案例

10. **[GameplayTag 系统](10_gameplay_tag.md)** —— 从 core 到 editor 的完整扩展
    - `GameplayTag`：层级化标签，RefCounted
    - `GameplayTagContainer`：标签集合 + parent cache，继承 Resource
    - `GameplayTagManager`：全局单例，标签树维护
    - 集成到 `Node`：`add_gameplay_tag` / `has_gameplay_tag` / `find_children_by_tag`
    - 编辑器支持：Inspector 插件 + Project Settings 面板
    - 与 Group / Meta 的对比：[gameplay_tag_vs_group_meta.md](../gameplay_tag_vs_group_meta.md)
    - 📂 `core/gameplay_tag/`, `scene/main/node.h`, `editor/inspector/`, `editor/settings/`

---

## 学习建议

- **先读头文件**（`.h`），理解接口设计，再看实现（`.cpp`）
- **跟着 `_bind_methods()` 走**，它是每个类暴露给脚本层的"API 清单"
- **从 Node 开始向下追**：比如 `MeshInstance3D` → 它调了哪些 `RenderingServer` API → Server 内部怎么处理
- **善用 `grep`**：Godot 的命名非常一致（`_notification`、`NOTIFICATION_`、`_bind_methods`），搜索模式固定
- **每学一个系统，尝试做一个小改动**：比如给 Node 加一个方法、注册一个新的 Server 资源类型

---

## 总览图

```
┌─────────────────────────────────────────────────┐
│                   Editor Layer                   │
│  (Inspector, SceneTreeDock, ProjectSettings...) │
├─────────────────────────────────────────────────┤
│              Scene Layer (scene/)                │
│  Node ─ SceneTree ─ PackedScene ─ Resource      │
├─────────────────────────────────────────────────┤
│             Server Layer (servers/)              │
│  RenderingServer ─ PhysicsServer ─ AudioServer  │
├─────────────────────────────────────────────────┤
│              Core Layer (core/)                  │
│  Object ─ ClassDB ─ Variant ─ Signal            │
│  Templates ─ MessageQueue ─ ScriptLanguage      │
├─────────────────────────────────────────────────┤
│           Platform / Drivers Layer              │
│  Vulkan ─ OpenGL ─ Metal ─ OS Abstraction       │
└─────────────────────────────────────────────────┘
```
