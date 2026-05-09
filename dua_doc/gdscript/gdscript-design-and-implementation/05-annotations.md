# 第 5 章　注解系统：`@tool`、`@export`、`@onready` 等

> 本章对应源码：
> `modules/gdscript/gdscript_parser.h`（`AnnotationNode` / `AnnotationInfo`）、
> `modules/gdscript/gdscript_parser.cpp`（注解注册表、`parse_annotation`、各 `*_annotation` 方法）。

GDScript 的注解（Annotation）写法完全复刻了 Python 装饰器的视觉风格——都是 `@xxx` 放在声明前面。它们却不是用户可以自行定义的扩展点，而是**一张硬编码、覆盖整个编辑器生态的"指令白名单"**：

```gdscript
@tool
extends EditorPlugin

@export_range(0.0, 1.0, 0.01) var volume: float = 0.5
@onready var player: AudioStreamPlayer = $Player
@warning_ignore("unused_parameter")
func _ready() -> void:
    pass
```

这些看似平凡的 `@xxx` 背后，横跨 Parser、Analyzer、Compiler 甚至编辑器、Inspector、RPC 系统。本章把注解当成一个独立子系统单独抽出来讲，是因为它 **"横切关注点"** 的性质——只在任何一章里提都讲不完整。

## 5.1　注解能做什么：一个粗略分类

按影响范围可以把 GDScript 的注解分成四大类：

| 分类 | 典型注解 | 影响阶段 | 作用 |
|---|---|---|---|
| 脚本级指令 | `@tool`、`@icon`、`@static_unload`、`@abstract` | Parser/Analyzer | 改变脚本的"模式"（能否在编辑器中运行、图标、是否抽象） |
| 成员标记 | `@onready`、`@export*`、`@rpc` | 主要影响 Compiler 与运行时 | 改变变量初始化时机、是否暴露到 Inspector、是否可远程调用 |
| 警告控制 | `@warning_ignore`、`@warning_ignore_start/restore` | Analyzer | 调整警告输出 |
| 独立语句 | `@export_category`、`@export_group`、`@export_subgroup` | Compiler 导出信息 | 在 Inspector 里插入分组/分类标题 |

分类本身就是代码里的 `TargetKind` 位掩码（稍后看），不是我随便归纳的。

## 5.2　`AnnotationNode`：AST 里的注解载体

Parser 解析到 `@xxx(...)` 时产出一个 `AnnotationNode`：

```cpp
struct AnnotationNode : public Node {
    StringName name;                          // "@onready"
    Vector<ExpressionNode *> arguments;       // 括号里的表达式，未求值
    Vector<Variant>          resolved_arguments; // Analyzer 后的常量值
    AnnotationInfo          *info = nullptr;  // 指向全局注册表项
};
```

注意两点：

1. **`arguments` 与 `resolved_arguments` 是两份**：前者是 AST 表达式（供补全、跳转、错误定位用），后者是 Analyzer 后求出的 `Variant` 值（供 Compiler 消费）。Analyzer 在 `resolve_annotation()` 里填后者。
2. **`info` 指回全局表**：注解的元数据（参数签名、可用目标、执行函数）不是存在 AST 上，而是查一张全局 `HashMap<StringName, AnnotationInfo>`。AST 只存**对这张表的引用**。

## 5.3　注解注册表：`valid_annotations`

注册表在 `GDScriptParser::valid_annotations`（`gdscript_parser.h:1426`）：

```cpp
static HashMap<StringName, AnnotationInfo> valid_annotations;

struct AnnotationInfo {
    enum TargetKind {
        NONE     = 0,
        SCRIPT   = 1 << 0,
        CLASS    = 1 << 1,
        VARIABLE = 1 << 2,
        CONSTANT = 1 << 3,
        SIGNAL   = 1 << 4,
        FUNCTION = 1 << 5,
        STATEMENT= 1 << 6,
        STANDALONE = 1 << 7,
        CLASS_LEVEL = CLASS | VARIABLE | CONSTANT | SIGNAL | FUNCTION,
    };
    uint32_t target_kind = 0;                 // 位掩码
    AnnotationAction apply = nullptr;         // 执行函数（成员函数指针）
    MethodInfo info;                          // 参数签名
};

typedef bool (GDScriptParser::*AnnotationAction)(AnnotationNode *p_annotation,
                                                 Node *p_target,
                                                 ClassNode *p_class);
```

`AnnotationAction` 是一个 **`GDScriptParser` 的成员函数指针**。这意味着注解的执行逻辑 **不**是通过继承或 virtual 方法分发，而是通过一个 `MemberFunctionPointer` 查表调用——这是 Godot 里非常典型的"表驱动 + 成员函数指针"模式。

### 5.3.1　注册：`register_annotation`

`GDScriptParser::register_annotation`（`gdscript_parser.cpp:150` 起）是个静态函数，在 Parser 静态初始化阶段被调用。注册条目节选：

```cpp
register_annotation(MethodInfo("@tool"),
                    AnnotationInfo::SCRIPT,
                    &GDScriptParser::tool_annotation);

register_annotation(MethodInfo("@icon", PropertyInfo(Variant::STRING, "icon_path")),
                    AnnotationInfo::SCRIPT,
                    &GDScriptParser::icon_annotation);

register_annotation(MethodInfo("@onready"),
                    AnnotationInfo::VARIABLE,
                    &GDScriptParser::onready_annotation);

register_annotation(MethodInfo("@export"),
                    AnnotationInfo::VARIABLE,
                    &GDScriptParser::export_annotations<PROPERTY_HINT_NONE, Variant::NIL>);

register_annotation(MethodInfo("@export_range", PropertyInfo(Variant::FLOAT, "min"),
                                                PropertyInfo(Variant::FLOAT, "max"),
                                                PropertyInfo(Variant::FLOAT, "step"),
                                                PropertyInfo(Variant::STRING, "extra_hints")),
                    AnnotationInfo::VARIABLE,
                    &GDScriptParser::export_annotations<PROPERTY_HINT_RANGE, Variant::FLOAT>,
                    varray(1.0, ""), /* is_vararg */ true);

register_annotation(MethodInfo("@warning_ignore", PropertyInfo(Variant::STRING, "warning")),
                    AnnotationInfo::CLASS_LEVEL | AnnotationInfo::STATEMENT,
                    &GDScriptParser::warning_ignore_annotation,
                    varray(), /* is_vararg */ true);

register_annotation(MethodInfo("@rpc", PropertyInfo(Variant::STRING, "mode"),
                                       PropertyInfo(Variant::STRING, "sync"),
                                       PropertyInfo(Variant::STRING, "transfer_mode"),
                                       PropertyInfo(Variant::INT,    "transfer_channel")),
                    AnnotationInfo::FUNCTION,
                    &GDScriptParser::rpc_annotation,
                    varray("authority", "call_remote", "reliable", 0));
```

几件事值得展开说：

#### 5.3.1.1　参数签名来自 `MethodInfo`

注册时把注解看成一个"虚拟方法"，用 Godot 内核的 `MethodInfo` 描述它的参数列表。这让注解与引擎其它一切 API 共享同一套反射/校验基础设施：类型检查、默认值、是否可变参数，都直接复用。

#### 5.3.1.2　模板版 Action：`export_annotations<PROPERTY_HINT_X, Variant::Y>`

`@export*` 系列表面上有三十多个（`@export`、`@export_range`、`@export_flags_3d_physics`……），实现却只是一个模板函数：

```cpp
template <PropertyHint t_hint, Variant::Type t_type>
bool export_annotations(AnnotationNode *, Node *, ClassNode *);
```

每个具体 `@export_xxx` 都是这个模板的一次实例化。注册时在指针那一栏填 `&GDScriptParser::export_annotations<PROPERTY_HINT_RANGE, Variant::FLOAT>`，C++ 在编译期为每个实例化生成一份专用函数。运行时根本不需要 `switch` 分支 —— **模板把注解名到 hint/type 的映射固化到了每个具体函数的地址里**。

这是 GDScript 代码库里运用 C++ 模板把"大量看似独立的注解"折叠成单一实现的典型案例。

#### 5.3.1.3　`TargetKind` 位掩码允许多挂点

`@abstract` 的注册：

```cpp
register_annotation(MethodInfo("@abstract"),
                    AnnotationInfo::SCRIPT | AnnotationInfo::CLASS | AnnotationInfo::FUNCTION,
                    &GDScriptParser::abstract_annotation);
```

它可以挂在脚本级（使整份脚本抽象）、内部类、甚至函数上。`target_kind` 是位掩码，能轻易表达这种"多目标"的注解。Parser 挂载时用 `parse_annotation(uint32_t p_valid_targets)` 做位与校验。

## 5.4　Parser 阶段：`parse_annotation` 与 pending 队列

### 5.4.1　解析过程

`parse_annotation(uint32_t p_valid_targets)` 的主要步骤：

1. `consume(ANNOTATION, ...)` 消费 `@xxx` Token；
2. 查 `valid_annotations`——名字不存在直接报错；
3. 如果 `current` 是 `(`，按普通函数调用的方式解析参数表（产 `Vector<ExpressionNode *>`）；
4. 对比 `info->target_kind` 与 `p_valid_targets`——不兼容就报"该注解不能挂在这里"；
5. 返回 `AnnotationNode`。

注意第 4 步里 `p_valid_targets` 不是静态常量，而是**调用方传进来的上下文约束**。比如 `parse_program` 的文件顶部调用时传 `SCRIPT | CLASS | VARIABLE | CONSTANT | SIGNAL | FUNCTION`，说明"顶层这里上面几类都允许"；而 `parse_statement` 在函数体内部调用时只传 `STATEMENT | STANDALONE`。

### 5.4.2　pending 队列：注解的"前缀挂载"

注解语法上出现在声明**之前**：

```gdscript
@tool
@icon("res://icon.png")
class_name MyTool extends Node
```

解析 `@tool` 时 Parser 根本不知道下一个结构是什么。它的做法是**把 `AnnotationNode` 推到一个缓冲队列 `annotation_stack`**（`gdscript_parser.h:1427`）里，等到下一个"能被注解的语法结构"诞生时，再把队列清空、全部挂上去。

核心逻辑在 `parse_class_body`、`parse_class_member`、`parse_statement` 等函数的开头：

```cpp
// 先把积累的 annotations 拿出来
List<AnnotationNode *> annotations;
while (!annotation_stack.is_empty()) {
    annotations.push_back(annotation_stack.front()->get());
    annotation_stack.pop_front();
}

// 解析实际成员
Node *member = parse_function(...);

// 把注解全都挂上去，顺便校验每个注解的 TargetKind
for (AnnotationNode *a : annotations) {
    if (!(a->info->target_kind & FUNCTION)) {
        push_error("@xxx cannot be used on functions");
    }
    member->annotations.push_back(a);
}
```

这就是"**虽然注解写在声明前面，但它们最终是挂在声明 AST 节点上**"的实现基础。文件结束时如果 `annotation_stack` 还不空，`clear_unused_annotations()` 会把剩下的全部报为"注解后没有可供挂载的声明"。

### 5.4.3　独立注解：`STANDALONE`

`@export_category`、`@export_group`、`@warning_ignore_start` 这些**不挂在任何声明上**——它们就是语句本身。它们的 `target_kind = STANDALONE`，解析时直接作为一个 `AnnotationNode` 语句加入 `ClassNode::members` 或 `SuiteNode::statements`，不进 pending 队列。这让 Inspector 的"分组标题"、"警告区段"能出现在它们被写下的具体位置，而不被自动"吸附"到下一个成员上。

## 5.5　Analyzer 阶段：`resolve_annotation`

Parser 把参数解析成 `ExpressionNode *`，但真正把这些表达式求值成 `Variant` 要等到 Analyzer 阶段：

```cpp
void GDScriptAnalyzer::resolve_annotation(AnnotationNode *p_annotation);
```

它依次做：

1. 对 `p_annotation->arguments` 里每个表达式调用 `reduce_expression`；
2. 要求每个参数都是常量（不然报错——注解参数必须编译期可算）；
3. 按 `p_annotation->info->info`（参数签名 `MethodInfo`）做类型检查；
4. 把 `reduced_value` 逐个塞进 `resolved_arguments`；
5. 如有变参（`@export_enum("A", "B", ...)`），允许任意多个同类型参数。

注解参数必须是常量这条规则是故意的——**注解本质上是把编译期信息告诉引擎**。如果允许运行时表达式，那很多用途（Inspector 展示、RPC 路由配置）根本对不上时机。

## 5.6　Action：真正"执行"注解的地方

`apply` 函数指针在什么时候被调？取决于注解种类：

- **大部分注解在 Parser 挂载阶段就调用**——即 `AnnotationNode::apply(this_parser, annotation, target_node, current_class)`。这些注解只需要在 AST 上设置若干旗标，不需要类型信息。
- **需要类型信息的注解（如 `@export*`）延后到 Analyzer 之后、Compiler 之前调用**——因为 Inspector 要生成的 `PropertyInfo` 依赖变量的最终类型。

我们看几个典型实现：

### 5.6.1　`tool_annotation`：最简单的 action

```cpp
bool GDScriptParser::tool_annotation(AnnotationNode *, Node *, ClassNode *p_class) {
    p_class->tool = true;
    return true;
}
```

就是在 `ClassNode` 上置一个标志位。这个标志后来会经由 Compiler 传到 `GDScript::is_tool`，决定这个脚本能不能在编辑器里实际运行。

### 5.6.2　`onready_annotation`：改变变量初始化时机

`@onready var player = $Player` 的语义是"初始化发生在 `_ready()` 被调用之前"，而不是脚本实例构造时。注解 action 只需要：

```cpp
bool GDScriptParser::onready_annotation(AnnotationNode *, Node *p_target, ClassNode *) {
    VariableNode *var = static_cast<VariableNode *>(p_target);
    var->onready = true;
    current_class->onready_used = true;
    return true;
}
```

真正搬运初始化时机的工作在 **Compiler**：它把 `onready=true` 的变量的初始化表达式从 `_init` 函数里剔除，合成到一个叫 `@implicit_ready` 的内部方法里，由 `Node::_ready` 通过脚本系统钩到。这是"**注解改变字节码生成策略**"的典型例子——第 6 章会再提。

### 5.6.3　`export_annotations<hint, type>`：填 `PropertyInfo`

`@export var v: int = 0` 的效果是"变量出现在 Inspector 里并可编辑"。action 的本质是构造一个 `PropertyInfo`：

```cpp
bool GDScriptParser::export_annotations<PROPERTY_HINT_RANGE, Variant::FLOAT>(
        AnnotationNode *p_annotation, Node *p_target, ClassNode *) {
    VariableNode *var = static_cast<VariableNode *>(p_target);
    var->export_info.type  = Variant::FLOAT;
    var->export_info.hint  = PROPERTY_HINT_RANGE;
    var->export_info.hint_string = vformat("%s,%s,%s,...",
            p_annotation->resolved_arguments[0],    // min
            p_annotation->resolved_arguments[1],    // max
            p_annotation->resolved_arguments[2]);   // step
    return true;
}
```

这个 `export_info: PropertyInfo` 会被 Compiler 复制进 `GDScript` 的导出属性表，进而在 Inspector 里展示出滑块控件。

### 5.6.4　`rpc_annotation`：标记函数可远程调用

```cpp
bool GDScriptParser::rpc_annotation(AnnotationNode *a, Node *p_target, ClassNode *) {
    FunctionNode *fn = static_cast<FunctionNode *>(p_target);
    fn->rpc_config = build_rpc_config(a->resolved_arguments);
    return true;
}
```

`fn->rpc_config` 最终会被 Compiler 塞进 `GDScriptFunction` 的 RPC 元数据字段；运行时 `GDScriptRPCCallable`（第 14 章）根据这个字段决定这次调用"走本地还是走网络"。这里能看到一个微妙的分层：**Parser 挂 AST 旗标 → Compiler 搬进运行时对象 → 运行时按旗标分发**。注解就是这条链的起点。

### 5.6.5　`warning_ignore_annotation`：直接操作 Parser 状态

```cpp
bool GDScriptParser::warning_ignore_annotation(AnnotationNode *a, Node *p_target, ClassNode *) {
    for (const Variant &code_name : a->resolved_arguments) {
        parser->ignored_warnings_for_next_node.push_back(code_name);
    }
    return true;
}
```

`@warning_ignore` 的 action 直接修改 Parser 的"下一个节点要忽略的警告"集合。具体的忽略过滤发生在 Analyzer 的 `apply_pending_warnings()`——警告系统会在第 22 章细讲。

## 5.7　特殊注解：`@icon`、`@abstract`、`@static_unload`

这些注解相对孤僻，但从侧面展示了系统的灵活性：

- **`@icon("res://...")`**：给 `class_name` 加一个在编辑器里显示的图标。action 只是把路径存到 `ClassNode::icon_path`；编辑器侧（`gdscript_editor.cpp`）在生成"New Inherited Scene"菜单时去读这个路径并显示图标。
- **`@abstract`**：标记脚本/类/函数为抽象——不能直接实例化，必须被子类 override。action 只是置位 `ClassNode::is_abstract` / `FunctionNode::is_abstract`；Analyzer 在实例化位置做检查；Compiler 在成员查找时把抽象方法视为 "未实现"。
- **`@static_unload`**：允许脚本在没有引用时被卸载（影响 `ResourceLoader` 的 UID 记账）。action 置位 `ClassNode::static_unload`；主要作用在 `GDScriptCache`（第 19 章）。

## 5.8　注解 vs 内置语言特性

一个常见疑问：**为什么 `static` 是关键字，`@tool` 却是注解？** 明明它们都在"修饰"声明。答案写在 README 的设计哲学里：

> Features are added because they are _needed_, and not because they can be added or are interesting to develop.

关键字会占用标识符空间、影响语法解析、改变 AST 结构；注解不会——它们只是挂在 AST 节点上的"元数据"。因此 GDScript 的选择是：

- **凡是影响语法结构（变量作用域、控制流、表达式）的，用关键字**：`var`、`const`、`static`、`if`、`await`……
- **凡是只影响"元数据/编辑器/运行时行为"的，用注解**：`@tool`、`@export`、`@onready`、`@rpc`、`@icon`……

这条分界让注解系统非常可扩展：想加一个新的编辑器特性（比如 `@experimental`、`@deprecated`），只要在 `register_annotation` 表里加一行 + 写一个 action 函数，不碰语法、不破坏 `.gdc` 兼容。事实上如果你去翻 Godot 的历史 commit，会看到注解列表这两年在持续增长。

## 5.9　调用链总览

把前面所有零碎串成一张图：

```
Parser 静态初始化
   └── register_annotation(...)     [valid_annotations 填表]

Parser.parse_annotation(@xxx)
   ├── 查 valid_annotations 得 AnnotationInfo
   ├── 解析 (args) → Vector<ExpressionNode *>
   └── 返回 AnnotationNode（挂到 pending 队列 / STANDALONE 直接入语句列表）

Parser.parse_class_member / parse_statement 等
   ├── 从 pending 队列取出 annotations
   ├── 按 TargetKind 校验
   └── target->annotations.append(...)

某些 action 立即执行（tool/onready/rpc/warning_ignore 等）：
   annotation->info->apply(parser, annotation, target, current_class)

Analyzer.analyze
   ├── resolve_annotation(...)     [对每个 annotation 求常量参数]
   │      └── reduce_expression(每个参数) + 签名校验
   └── 需要类型信息的 action 此时执行（export* 系列）

Compiler._compile_class
   ├── 把 ClassNode.icon_path / tool / onready / export_info / rpc_config
   │   搬到 GDScript 对象对应字段
   └── 合成 @implicit_ready、onready 初始化代码
```

注解在这条链上几乎每一站都出现，但每一站都只做"自己那一小步"——这是它看似精巧、实现上却不复杂的核心原因。

## 5.10　自定义注解？

读到这里你可能会想：既然机制这么清晰，能不能在模块外注册自定义注解？

**答案是：目前不能。** `valid_annotations` 表在 Parser 静态初始化时填入，外部没有 `register_annotation` 的公开入口；而且注解 Action 是 `GDScriptParser` 的成员函数指针——要在别处写一个 Action 需要访问 Parser 私有字段，不现实。

如果你要扩展（例如引擎 fork），合理的改法是：

1. 在 `gdscript_parser.cpp` 的静态初始化段里加 `register_annotation(...)`；
2. 在 `gdscript_parser.h/.cpp` 里加上你的 `*_annotation` 成员函数；
3. 如果 Action 需要类型信息，记得把它从"Parser 阶段立即调用"挪到"Analyzer `resolve_annotation` 后调用"的路径上。

这是本书里少数"鼓励你去改源码"的章节——因为 GDScript 的注解机制本来就是为扩展而设计的，只是目前**扩展点是内部的**。

## 本章小结

- GDScript 的注解用"一张硬编码表（`valid_annotations`）+ 成员函数指针（`AnnotationAction`）"实现；语法上抄袭 Python 装饰器，语义上更接近 C# 特性（Attribute）。
- AST 载体是 `AnnotationNode`；Parser 先把它放到 pending 队列，再"前缀挂载"到下一个声明节点的 `annotations` 列表里。`STANDALONE` 注解是例外——它们就是语句本身。
- `TargetKind` 位掩码让同一个注解可以适用于多种声明；`MethodInfo` 复用了引擎的参数签名描述。
- 大量 `@export*` 被 **C++ 模板折叠**成一个泛型 Action，参数差异固化到函数地址里，避免运行时分支。
- 注解 Action 的调用时机分两波：无需类型信息的立即执行（Parser 阶段），需要类型信息的推迟到 Analyzer 的 `resolve_annotation` 之后——前者影响 AST 结构标志，后者影响 Inspector 导出、RPC 配置等。
- `@onready`、`@rpc` 等注解最终会影响 Compiler 的字节码生成和运行时分发，是理解"注解到底干了什么"时的必看例子。

下一章我们进入中端：**第 6 章 Compiler**。把 Analyzer 加工完的带类型、带注解、带常量值的 AST，转化成以 `GDScriptFunction` 为单位的字节码对象。

---

[← 上一章：第 4 章 Analyzer](./04-analyzer.md) · [目录](./README.md) · [下一章：第 6 章 编译器主流程：`GDScriptCompiler` →](./06-compiler.md)
