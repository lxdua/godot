# 《GDScript 设计与实现》

> 一本以 Godot 引擎源码（`modules/gdscript/`）为底本，自下而上剖析 GDScript
> 从源代码字符串到字节码再到虚拟机执行全过程的中文技术书。

---

## 写作目的

GDScript 是 Godot 引擎自带的脚本语言，它以"渐进式类型（gradually typed）+ 紧凑语法 + 字节码虚拟机"的设计在游戏脚本领域独树一帜。但在中文社区中，关于 GDScript 内部实现的资料几乎是空白：

- 大多数文档停留在"如何使用 GDScript 写游戏"。
- 极少有材料解释：一个 `.gd` 文件从磁盘加载到被 VM 执行，中间到底发生了什么？
- 引擎自身的 `modules/gdscript/README.md` 给出了极佳的总览，但缺少代码级细节与实现脉络的铺陈。

本书希望填补这一空白：以 Godot 当前主线源码为唯一权威，系统性地拆解 GDScript 的**前端（Tokenizer / Parser / Analyzer）**、**中端（Compiler / ByteCodeGenerator）**、**后端（GDScriptFunction / VM）**，以及**编辑器集成**与**资源系统**等周边设施。

读完本书，你应当能够：

1. 看懂 `modules/gdscript/` 目录下任何一个文件的存在意义与代码组织。
2. 跟踪一段 GDScript 代码从源串到执行的全部关键路径。
3. 自行修改、扩展 GDScript（例如添加关键字、注解、Opcode、内建函数）。
4. 在性能调优、Bug 排查、自定义编辑器特性时，知道"该看哪儿"。

## 适合的读者

- 熟悉 C++ 基础语法，能阅读中等复杂度的 C++ 代码。
- 至少使用过 GDScript 写过若干游戏脚本，了解类、函数、信号、`@tool`、`@onready` 这类基本概念。
- 对编译原理（词法/语法/语义/字节码/VM）有最浅层的认识即可，不要求精通。

如果你完全没接触过编译原理，可以先把每章末尾的"延伸阅读"中提到的概念性资料粗读一遍再回来。

## 源码版本与阅读约定

- **底本**：本书所有引用的代码均来自当前仓库 `modules/gdscript/` 与相关核心目录（`core/object/`、`core/variant/` 等）。
- **路径写法**：形如 `modules/gdscript/gdscript_parser.h` 表示从仓库根目录起的相对路径。
- **代码片段**：出于篇幅考虑，正文中的代码块会做适度精简（删除日志、`#ifdef DEBUG_ENABLED` 分支、与本节无关的成员），但保留可还原回原文件的关键结构。需要"原汁原味"的实现细节时，请直接对照仓库源码。
- **行号引用**：会标注行号附近，但请以源码 `git blame` 为准，行号会随着 Godot 主线变动。
- **术语**：英文术语首次出现时给出中文翻译，之后视语境混用，例如 Token / 词元、Parser / 解析器、Opcode / 操作码、Datatype / 数据类型。

## 整体阅读路径

本书章节按**编译流水线 → 运行时 → 工具链**的自然顺序排列，前后存在一定依赖：

```
        ┌──────────────────────────────────────────────────────────────┐
        │                      第一部分 总览                              │
        │   第1章 整体架构（必读）                                          │
        └──────────────────────────────────────────────────────────────┘
                                   │
        ┌──────────────────────────┴───────────────────────────────────┐
        │                      第二部分 前端                              │
        │   第2章 Tokenizer → 第3章 Parser/AST → 第4章 Analyzer →           │
        │   第5章 注解系统                                                 │
        └──────────────────────────────────────────────────────────────┘
                                   │
        ┌──────────────────────────┴───────────────────────────────────┐
        │                第三部分 编译器与字节码                          │
        │   第6章 Compiler → 第7章 ByteCodeGenerator → 第8章 字节码格式      │
        └──────────────────────────────────────────────────────────────┘
                                   │
        ┌──────────────────────────┴───────────────────────────────────┐
        │                      第四部分 运行时                            │
        │   第9章 GDScriptFunction → 第10章 VM 主循环 →                     │
        │   第11章 Opcode 详解 → 第12章 await/协程 →                        │
        │   第13章 Lambda → 第14章 RPC                                    │
        └──────────────────────────────────────────────────────────────┘
                                   │
        ┌──────────────────────────┴───────────────────────────────────┐
        │                  第五部分 类型与实例                            │
        │   第15章 GDScript 类对象 → 第16章 GDScriptInstance →               │
        │   第17章 Variant 互操作                                           │
        └──────────────────────────────────────────────────────────────┘
                                   │
        ┌──────────────────────────┴───────────────────────────────────┐
        │                  第六部分 资源与缓存                            │
        │   第18章 资源加载 → 第19章 GDScriptCache                          │
        └──────────────────────────────────────────────────────────────┘
                                   │
        ┌──────────────────────────┴───────────────────────────────────┐
        │                  第七部分 编辑器集成                            │
        │   第20章 编辑器集成 → 第21章 LSP → 第22章 警告 → 第23章 调试器       │
        └──────────────────────────────────────────────────────────────┘
```

> **快速通道**：如果你只关心"GDScript 是怎么跑起来的"，最短路径是：
> **第 1 章 → 第 6 章 → 第 7 章 → 第 9 章 → 第 10 章**。

## 完整目录

### 第一部分　总览

- **第 1 章　GDScript 概述与整体架构**
  设计哲学、与 Godot 的集成关系、`Script` / `ScriptInstance` / `ScriptLanguage` 三件套、编译流水线全景图、源码目录导览。

### 第二部分　前端：从源码到 AST

- **第 2 章　词法分析：`GDScriptTokenizer`**
  Token 结构、关键字与运算符表、`INDENT` / `DEDENT` 的生成、行延续、续行符、Lambda 缩进栈、`scan()` 状态机。
- **第 3 章　语法分析：`GDScriptParser` 与 AST**
  单 Token 前瞻设计、Pratt 风格表达式解析、`Node` 子类全景、`ClassNode` / `FunctionNode` / `IfNode` 等核心节点、错误恢复策略。
- **第 4 章　语义分析：`GDScriptAnalyzer` 与类型系统**
  `reduce_*` 与 `resolve_*` 两族函数、`DataType` 数据结构、循环依赖与 `RESOLVING` 哨兵、类接口解析、按需成员解析、未类型化代码的"trust the programmer"策略、常量折叠。
- **第 5 章　注解系统：`@tool`、`@export`、`@onready` 等**
  `AnnotationNode` 的注册与生效时机、注解对编译器的影响、自定义注解扩展点。

### 第三部分　编译器与字节码

- **第 6 章　编译器主流程：`GDScriptCompiler`**
  `_prepare_compilation()` 与 `_compile_class()` 的分工、为什么编译期不能跨类调用、`GDScript` 对象的成员填充、初始化函数的合成。
- **第 7 章　字节码生成：`GDScriptByteCodeGenerator`**
  抽象基类 `GDScriptCodeGenerator`、寄存器/栈式混合模型、临时寄存器分配、跳转回填（patching）、常量池与类型池的构建。
- **第 8 章　字节码格式与反汇编：`gdscript_disassembler.cpp`**
  `GDScriptFunction` 中 `code` 数组布局、操作数编码、`disassemble()` 的实现思路、如何阅读一段 disasm 结果。

### 第四部分　运行时：虚拟机如何执行字节码

- **第 9 章　可执行函数：`GDScriptFunction`**
  函数对象的字段、栈帧布局、参数与默认值处理、调试信息（行号映射、Profiler 计数）、跨调用的生命周期。
- **第 10 章　虚拟机主循环：`GDScriptFunction::call()`**
  指令分发的实现策略（`switch` vs computed-goto 的可选路径）、错误处理与 `Callable::CallError` 的传播、调试器钩子的插入点。
- **第 11 章　Opcode 详解**
  按类别拆解：算术、比较、内建调用、成员访问、迭代器、跳转、`await`、`yield`、构造与销毁等。配合反汇编实例。
- **第 12 章　`await`、信号与协程**
  `GDScriptFunctionState`（如有）/ `await` 的具体实现、信号唤醒路径、与引擎主循环的交互、与 `Callable` 的耦合。
- **第 13 章　Lambda 与 `GDScriptLambdaCallable`**
  Lambda 的解析、捕获、为何需要专门的 Callable 类型、与 GC 行为的关系。
- **第 14 章　RPC 与 `GDScriptRPCCallable`**
  RPC 标记如何在编译期保存到 `GDScriptFunction`、运行时调用的代理路径、与 `MultiplayerAPI` 的衔接。

### 第五部分　类型与实例

- **第 15 章　`GDScript` 类对象、内部类与继承**
  一个 `.gd` 文件如何对应一个 `GDScript`、内部类（`class Foo: ...`）的内嵌 `GDScript`、`base` 链与 `native` 类的查找。
- **第 16 章　`GDScriptInstance` 与运行时实例**
  实例与脚本的关系、成员变量在内存中的布局、`set` / `get` 的回退路径、`get_script_property_list()` 的生成。
- **第 17 章　与 Variant、ClassDB 的互操作**
  Variant 的类型枚举对 GDScript 类型系统的映射、`ClassDB::class_get_method` 的调用模式、内建工具函数（`GDScriptUtilityFunctions`）与全局函数（`Variant::_register_variant_utility_functions()`）的双轨注册。

### 第六部分　资源与缓存

- **第 18 章　脚本资源加载：`ResourceFormatLoaderGDScript`**
  `ResourceLoader::load()` → `ResourceFormatLoaderGDScript::load()` 的完整路径、与 `.gdc` 二进制 token 缓存（`gdscript_tokenizer_buffer`）的关系。
- **第 19 章　脚本缓存：`GDScriptCache` 与浅/全脚本**
  `get_shallow_script()` / `get_full_script()` 的语义差别、`GDScriptParserRef` 的多阶段缓存、缓存如何避免循环依赖、`GDScript::reload()` 在整个体系中的核心地位。

### 第七部分　编辑器集成与工具链

- **第 20 章　编辑器集成：自动补全、跳转、文档生成**
  `gdscript_editor.cpp` 的服务面、`GDScriptSyntaxHighlighter`、`GDScriptDocGen`。
- **第 21 章　语言服务器（LSP）：`language_server/`**
  GDScript LSP 的协议适配层、Symbol 索引、与 Analyzer 的复用关系。
- **第 22 章　警告系统：`GDScriptWarning`**
  警告分级、抑制（`@warning_ignore`）的实现、警告与 Analyzer 的协作。
- **第 23 章　调试器与 Profiler**
  `EngineDebugger` 接口、断点与单步、热重载（`reload_tool_script`）、性能采样的字节码级实现。

### 附录

- **附录 A**　关键字、运算符与 Token 速查表
- **附录 B**　Opcode 速查表
- **附录 C**　常用调试与定位技巧（如何下断点、如何打印 disasm、如何写最小复现）
- **附录 D**　与 C# / GDExtension 的对比阅读建议

## 关于本书的"非目标"

为了避免无限延伸，下列内容**不属于本书的核心范围**，仅在必要处一笔带过：

- GDScript 的语言**使用教程**——请参考官方文档。
- C# 脚本（`modules/mono`）与 GDExtension（`core/extension`）的实现细节——只在对比章节中提及。
- 渲染、物理、动画等与脚本无关的引擎子系统。

## 反馈与勘误

由于 Godot 主线在持续演进，本书内容可能与最新 `master` 出现局部偏差。如发现明显错误或希望补充内容，请直接在 `dua_doc/gdscript/gdscript-design-and-implementation/` 目录下新建 issue 笔记或修改对应章节。

---

接下来请翻到 [第 1 章 GDScript 概述与整体架构](./01-overview.md)。
