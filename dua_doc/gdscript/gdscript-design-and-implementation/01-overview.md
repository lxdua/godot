# 第 1 章　GDScript 概述与整体架构

> 本章对应源码：`modules/gdscript/README.md`、`modules/gdscript/gdscript.h`、
> `modules/gdscript/register_types.cpp`、`core/object/script_language.h`、
> `core/object/script_instance.h`。

本章是整本书的"地图"。在拆解任何一个具体模块（Tokenizer、Parser、VM……）之前，我们必须先回答几个高层次的问题：

1. GDScript **在 Godot 这套引擎里扮演什么角色**，它和 C++ 核心、Variant、ClassDB 是怎样组合起来的？
2. 一段 `.gd` 源码从被加载到最终被执行，**到底要经过哪些阶段**？
3. `modules/gdscript/` 目录里几十个 `.cpp` / `.h` 文件，**每个大致负责什么**？读源码时该从哪儿入手？

本章给出这三个问题的答案，并为后续各章建立一套统一的"坐标系"。

## 1.1　设计哲学：紧凑、嵌入式、渐进类型

`modules/gdscript/README.md` 开篇就给 GDScript 下了三句"宣言"，可以视为整门语言的设计基线：

1. **渐进式类型（gradually typed）**——类型注解是可选的；类型化的代码可以与未类型化的代码自由互操作；编译器会尽量利用类型信息来生成更安全、更快的字节码，但也始终容许"无类型"代码存在。
2. **特性紧凑（tightly designed）**——只有在确实**需要**时才会引入新特性，绝不为了"看起来更现代"而堆砌语法糖。
3. **解释执行的脚本语言**——它编译为字节码，由内置 VM 执行；目标是写**游戏玩法逻辑**，不追求 CPU 密集型计算性能。需要重活时，请改用 C# 或 GDExtension。

这三条决定了后续所有实现选择，理解它们能帮你预测——甚至在源码里看到陌生设计时主动猜对——为什么作者要这么写。比如：

- 为什么 Parser 只允许 1 个 token 的前瞻？因为"紧凑"——避免语言被人为做复杂。
- 为什么 Analyzer 对未类型化代码大量使用 `mark_node_unsafe()` 而不是直接报错？因为"渐进类型"——容忍是第一公民。
- 为什么 VM 是字节码解释器而不是 JIT？因为"嵌入式脚本"——可移植性、可热重载、编辑器内运行的场景比单线程数值性能更重要。

## 1.2　GDScript 在 Godot 里的位置

GDScript 并不是 Godot 内核的一部分，而是作为一个**可选模块**挂载上去的：

```
Godot Engine
├── core/                    # 引擎内核：Object、Variant、ClassDB、ResourceLoader……
├── scene/                   # 场景树、Node、节点资源
├── servers/                 # 渲染、物理、音频等子系统
└── modules/
    ├── gdscript/            # ← 本书主题
    ├── mono/                # C# 脚本支持（同样是模块）
    └── ...                  # 其它可关闭的扩展模块
```

`modules/gdscript/README.md` 中明确指出：

> Since modules are optional, this means that Godot may be built without GDScript and work perfectly fine without it!

这一点不是吹嘘"模块化做得好"，而是对设计有实质约束——**GDScript 模块不能修改核心，只能继承和注册**。具体来说：

### 1.2.1　继承的接口：`ScriptLanguage` / `Script` / `ScriptInstance`

Godot 把"脚本"这件事抽象成三个核心抽象类，定义在 `core/object/`：

| 抽象类 | 定义文件 | GDScript 中的实现 | 角色 |
|---|---|---|---|
| `ScriptLanguage` | `core/object/script_language.h` | `GDScriptLanguage` | 一门"脚本语言"本身（关键字、扩展名、解析入口、调试支持……） |
| `Script` | `core/object/script_language.h` | `GDScript` | 一段可被实例化的"脚本资源"（一份编译产物） |
| `ScriptInstance` | `core/object/script_instance.h` | `GDScriptInstance` | 把脚本绑定到具体 `Object` 上之后的"运行时实例"（持有成员变量值） |

这是一套教科书式的策略模式：引擎核心只跟基类打交道，它根本不知道下面跑的是 GDScript、C# 还是别的什么。

注册过程发生在 `modules/gdscript/register_types.cpp`：模块初始化时构造一个全局的 `GDScriptLanguage` 单例并 `ScriptServer::register_language(...)`，从此引擎拿到任何 `.gd` 文件就知道要走 GDScript 这条路。

### 1.2.2　借用的接口：`ClassDB` 与 Variant 工具函数

GDScript 自己几乎没有"内置类型库"——`Node2D`、`Vector3`、`Color` 这些都来自引擎核心。GDScript 通过两个机制把它们暴露给脚本：

- **引擎类**：通过 [`ClassDB`](../../../core/object/class_db.h) 查询。GDScript 之所以"知道" `Node2D` 有 `get_parent()`，是因为引擎在 C++ 端用 `ClassDB::bind_method(...)` 把方法注册了进来，Analyzer 与 VM 都通过 `ClassDB` 查到它。
- **全局函数**：`@GDScript` 命名空间下的内置方法（如 `len`、`print`、`type_string`）由 `GDScriptUtilityFunctions`（`gdscript_utility_functions.h`）注册；而像 `sin`、`abs`、`min` 这类**全局作用域**函数则由核心的 `Variant::_register_variant_utility_functions()`（`core/variant/variant_utility.cpp`）注册，所有脚本语言共享。

> 这两个分工很重要：**`@GDScript` 内的函数是 GDScript 私有的**（C# 用不到），而**全局函数是所有脚本共享的**。后续讨论符号解析时会反复提到这个二分。

### 1.2.3　把所有人串起来：一张架构图

```
          ┌──────────────────────────────────────────────────────────┐
          │                      Godot Engine Core                    │
          │                                                            │
          │   ScriptServer ──┐                                         │
          │                  │ register_language()                     │
          │   ResourceLoader ┼──► ResourceFormatLoaderGDScript ──┐     │
          │                  │                                   │     │
          │   ClassDB ◄──────┼───────────────┐                   │     │
          │                  │               │                   │     │
          │   Variant util ◄─┘               │                   │     │
          └─────────────────┬────────────────┼───────────────────┼─────┘
                            │                │                   │
                            ▼                ▼                   ▼
          ┌──────────────────────────────────────────────────────────┐
          │                    modules/gdscript/                      │
          │                                                            │
          │    GDScriptLanguage  ──编译流水线──►  GDScript            │
          │         (单例)                       (脚本资源 / 类对象)   │
          │                                          │                │
          │                                          ▼                │
          │                                  GDScriptInstance         │
          │                                   (附着到 Object 的实例)  │
          └──────────────────────────────────────────────────────────┘
```

把这张图记在脑子里。本书后面任何一段实现细节，都能往这张图上某个位置插。

## 1.3　从源码到执行：编译流水线全景

`modules/gdscript/README.md` 用一句话概括了流水线：

> tokenizing, parsing, analyzing, and finally compiling.

但实际过程比这句话稍微"非线性"。下面给出更精确的版本：

```
┌────────────────────────────────────────────────────────────────────────┐
│                                                                          │
│   ① 源码字符串                                                            │
│        String source = GDScript::load_source_code(path)                  │
│                                                                          │
│        │                                                                 │
│        ▼                                                                 │
│   ② 词元流（Token Stream）                  ──── gdscript_tokenizer.cpp   │
│        GDScriptTokenizer::scan()                                         │
│        （同时存在二进制版本：gdscript_tokenizer_buffer，用于 .gdc）        │
│        │                                                                 │
│        ▼                                                                 │
│   ③ 抽象语法树（AST）                       ──── gdscript_parser.cpp      │
│        GDScriptParser::parse()                                           │
│        产出 ClassNode 及其下属 Node 树                                    │
│        │                                                                 │
│        ▼                                                                 │
│   ④ 类型化 / 语义校验后的 AST              ──── gdscript_analyzer.cpp     │
│        GDScriptAnalyzer::resolve_program()                               │
│        （分两步：先 resolve_class_interface，后 resolve 函数体）          │
│        │                                                                 │
│        ▼                                                                 │
│   ⑤ GDScript 类对象（含成员/常量/签名等）   ──── gdscript_compiler.cpp    │
│        GDScriptCompiler::_prepare_compilation()                          │
│        ──"interface 阶段"，仍不依赖其它脚本的编译结果                      │
│        │                                                                 │
│        ▼                                                                 │
│   ⑥ 字节码                                  ──── gdscript_byte_codegen   │
│        GDScriptCompiler::_compile_class()                                │
│        通过 GDScriptByteCodeGenerator 写入 GDScriptFunction.code         │
│        │                                                                 │
│        ▼                                                                 │
│   ⑦ 可被实例化、可被调用的 GDScript                                       │
│        GDScript::reload()  完成上述 ②~⑥ 全过程                            │
│        │                                                                 │
│        ▼                                                                 │
│   ⑧ 运行时执行                              ──── gdscript_vm.cpp         │
│        GDScriptFunction::call(GDScriptInstance* p_instance, …)           │
│                                                                          │
└────────────────────────────────────────────────────────────────────────┘
```

几条非常重要的"实现观察"：

- **③ 之后源码字符串和词元流就被丢弃**——AST 是后续所有阶段的"事实来源"。这意味着任何想要回溯到源码位置的需求（比如错误信息、调试信息、跳转定义），都必须在 AST 节点上携带行号/列号，否则后面拿不到。
- **④ 与 ⑤ 之间存在一道"墙"**：Analyzer 工作在纯 AST 上，它不需要、也不应该看到任何 `GDScript` 对象的内部布局；Compiler 才会真正去构造 `GDScript`。这道墙是为了**避免循环依赖**，参见 1.4 节。
- **⑦ `GDScript::reload()`** 是一切的入口和出口——README 中称之为"the primary way in which scripts get compiled in Godot"。读源码若不知道从哪进，就从 `GDScript::reload()` 进。

### 1.3.1　Tokenizer：把字符流变 Token

`GDScriptTokenizer`（`gdscript_tokenizer.h`）定义了一个抽象基类，对外只暴露一个核心 API：

```cpp
virtual Token scan() = 0;
```

具体实现有两个子类：

- `GDScriptTokenizerText`：从源码字符串实时扫描，编辑器与从 `.gd` 加载脚本时使用。
- `GDScriptTokenizerBuffer`：从二进制缓存（`.gdc`）反序列化 Token，部署后的游戏可以使用。

值得一提的是 `Token::Type` 枚举（`gdscript_tokenizer.h:49` 起）一共有上百种取值，覆盖了所有关键字、运算符、标点、缩进控制（`INDENT` / `DEDENT`）、内建常量（`CONST_PI` / `CONST_TAU` / `CONST_INF` / `CONST_NAN`）等。注释里一句话需要重点记住：

> If this enum changes, please increment the TOKENIZER_VERSION in gdscript_tokenizer_buffer.h

也就是说**词元枚举是字节码的一部分契约**——`.gdc` 文件里存的就是这些枚举值，改动会破坏向后兼容。这种"枚举即 ABI"的小细节散落在 GDScript 各处，是阅读源码时的常见陷阱。

第 2 章会详细拆解 `scan()` 的状态机以及 INDENT/DEDENT 的生成机制。

### 1.3.2　Parser：单 Token 前瞻 + AST

`GDScriptParser`（`gdscript_parser.h` + `gdscript_parser.cpp`）有一个被作者本人在 [博客](https://godotengine.org/article/gdscript-progress-report-writing-new-parser/) 里专门写过的设计原则：**only one token of look-ahead**。

为什么坚持只前瞻一个 Token？README 给出的解释是：

> This parsing limitation ensures that GDScript will remain syntactically simple and accessible, and that the parsing process cannot become overly complex.

注意这是**自我克制式的语言设计**：限制 Parser 的能力，反过来约束语言不会变得过于复杂。如果你曾经在某门语言里见过"为了实现某语法糖，前瞻了 5 个 Token"的恐怖代码，就能体会这条约束的价值。

Parser 的产出是一棵以 `ClassNode` 为根的 AST，节点类型全部是 `GDScriptParser::Node` 的内嵌子类，常见的有：

- `ClassNode`：对应一个类（包括内部类）。
- `FunctionNode`：函数定义。
- `IfNode` / `ForNode` / `WhileNode` / `MatchNode`：控制流。
- `BinaryOpNode` / `UnaryOpNode` / `CallNode` / `IdentifierNode` / `LiteralNode`：表达式。
- `AnnotationNode`：注解（如 `@tool`、`@export`）。
- ……

第 3 章会给出完整的 Node 家族图谱与表达式优先级表。

### 1.3.3　Analyzer：reduce 与 resolve 的双重奏

`GDScriptAnalyzer`（`gdscript_analyzer.h`）是整条流水线里最"聪明"也最"绕"的一站。它的职责一句话概括：

> 给 AST 上的每一处表达式标注 `DataType`，并校验所有语义规则；在不引入循环依赖的前提下，让类型系统尽可能地精确。

它的代码组织成两族函数：

- `reduce_*(Node*)`：作用于**表达式**节点。它要**算出**这个表达式的类型（写到节点的 `Datatype`）。如果整个表达式是常量，还要做**常量折叠**——直接把结果存下来，运行时就不必再算。比如 `1 + 2 * 3` 在 reduce 之后就会变成一个 `LiteralNode(7)`。
- `resolve_*(Node*)`：作用于**语句**节点。它处理控制流、作用域、声明、调用其他 `reduce_*` / `resolve_*`。例如：
  - `resolve_if()` 先 reduce 条件，再分别 resolve 两个分支。
  - `resolve_for()` 不光要 resolve 循环体，还得**为循环变量打类型**、确认右侧表达式可迭代等。

更微妙的是 Analyzer 处理类的方式。考虑 `class A extends B`，Analyzer 不能在 A 还没分析完时就傻傻地把 B 也完整分析一遍——如果 B 又依赖 A，立刻死锁。它的做法是：

1. 先调用 `resolve_class_interface(A)`：只把 A 的"对外接口"（成员变量、函数签名、信号、常量）填好，**不进入函数体**。
2. 真正需要 A 的某个成员时，再调用 `resolve_class_member(A, "foo")`。
3. 如果在解析过程中发现一个 `DataType` 已经被标记为 `RESOLVING`，就报循环依赖。

这是 GDScript 解决"渐进类型 + 跨文件依赖 + 内部类"的核心招数，第 4 章会详细推演这个过程。

### 1.3.4　Compiler 与 ByteCodeGenerator：从 AST 到字节码

到了 `GDScriptCompiler`（`gdscript_compiler.h`）这一层，AST 已经被打上了完整的类型标签。Compiler 做两件事：

1. **`_prepare_compilation(GDScript*, ClassNode*)`**：编译期"接口阶段"。它把 Analyzer 算出来的成员、常量、信号、函数签名等"硬塞"进 `GDScript` 对象，但**不生成任何字节码**。这一步对外可见的是一个"还没有函数体但已经长得像样"的 `GDScript`。
2. **`_compile_class(GDScript*, ClassNode*)`**：真正把每个函数体翻译成字节码。这一步通过 `GDScriptByteCodeGenerator`（`gdscript_byte_codegen.h`）写入 `GDScriptFunction::code`。

为什么要把 1、2 拆开？因为 `_compile_class()` 之间**必须不能互相调用**——否则两个相互引用的类一编译就死锁。所有跨类共享的信息必须在 1 阶段就准备好，2 阶段只看自己。

第 6 章会完整拆解 `_prepare_compilation()` 与 `_compile_class()` 的合作模式，第 7 章会进入 ByteCodeGenerator 的内部，看它如何分配寄存器、回填跳转、构造常量池。

### 1.3.5　VM：`GDScriptFunction::call()` 即虚拟机

GDScript 没有一个名为 "GDScriptVM" 的独立类——它的虚拟机就是 `GDScriptFunction::call()` 这个方法本身。`gdscript_vm.cpp`（约数千行）整个文件几乎只为这一个函数服务。

它的本质是一个巨大的 `switch` over Opcode 的循环。函数对象 `GDScriptFunction` 携带：

- 字节码数组 `code`；
- 常量池、全局名池、操作数解释所需的元信息；
- 行号映射、调试信息；
- 默认参数、参数/返回类型表；
- 用于 Profiler 的计数器。

VM 在执行前会从函数对象上分配一个**栈帧**（参数、局部变量、临时寄存器混用同一片连续内存），然后进入主循环。每条 Opcode 从字节码里取出若干操作数下标，去栈帧里读写 `Variant`。

第 9～11 章会把这一段逐条 Opcode 地讲清楚。

## 1.4　两个贯穿全书的"暗线"

读 GDScript 源码时，有两条"暗线"会反复出现，提前点出来能省掉无数困惑。

### 暗线 1：**循环依赖**始终是头号敌人

GDScript 允许：

- 同一个文件里的类之间互相引用；
- 不同文件之间通过 `preload`、`extends`、类型注解互相引用；
- 内部类与外部类互相引用。

这些"可以循环"的依赖让"先编译谁"变成了不可能完美回答的问题。GDScript 的应对策略是**把每一阶段都拆成"接口 + 实现"两层**：

| 阶段 | "接口" | "实现" |
|---|---|---|
| Analyzer | `resolve_class_interface()`：成员/方法签名 | `resolve_class_body()` 等：函数体内部 |
| Compiler | `_prepare_compilation()`：填充 `GDScript` 对象壳 | `_compile_class()`：生成字节码 |
| Cache | `get_shallow_script()`：仅 parse 完 | `get_full_script()`：analyze + compile 完 |

任何时候你看到一段"为什么不一次做完"的奇怪写法，先反问一句"这是不是为了避免循环依赖"，往往八九不离十。

### 暗线 2：**未类型化代码必须能跑**

每次 Analyzer 想做静态判断时，都得先回答："如果这段是无类型的呢？"

源码里到处可见这样的模式：

```cpp
if (operand_type.is_hard_type()) {
    // 静态校验，可能直接报错
} else {
    // 标记 unsafe，让 VM 在运行时再说
    parser->push_warning(...);
    mark_node_unsafe(node);
}
```

`mark_node_unsafe()` 是渐进类型的"逃生口"——表示"这一处分析器无法确定，请相信程序员"。在编辑器里，这些被标记为 unsafe 的代码行号会显示为灰色；纯类型化的、analyzer 可以保证的代码行号才显示为绿色。

VM 也照应这条暗线：很多 Opcode 都有"快路径（typed）"和"慢路径（untyped）"两种版本。例如成员访问就有 `OPCODE_GET_NAMED` / `OPCODE_GET_NAMED_VALIDATED` / 各类型专用变体；字节码里出现哪个版本，完全取决于编译期 Analyzer 给出的类型信息够不够强。

读 Opcode 表（第 11 章）时记着这条暗线，能立刻看懂"为什么同样的事情有这么多版本"。

## 1.5　源码目录导览

`modules/gdscript/` 顶层文件按"职责相近"重新排列后大致如下：

| 子系统 | 关键文件 | 第几章 |
|---|---|---|
| **模块注册** | `register_types.cpp/h`、`SCsub`、`config.py` | 第 1 章 |
| **语言入口** | `gdscript.h/cpp`（`GDScript` / `GDScriptLanguage` / `GDScriptInstance`） | 第 1、15、16 章 |
| **词法** | `gdscript_tokenizer.h/cpp`、`gdscript_tokenizer_buffer.h/cpp` | 第 2 章 |
| **语法** | `gdscript_parser.h/cpp` | 第 3 章 |
| **语义** | `gdscript_analyzer.h/cpp` | 第 4 章 |
| **编译** | `gdscript_compiler.h/cpp`、`gdscript_codegen.h`、`gdscript_byte_codegen.h/cpp` | 第 6、7 章 |
| **反汇编** | `gdscript_disassembler.cpp` | 第 8 章 |
| **运行时函数** | `gdscript_function.h/cpp` | 第 9 章 |
| **虚拟机** | `gdscript_vm.cpp` | 第 10、11 章 |
| **Lambda** | `gdscript_lambda_callable.h/cpp` | 第 13 章 |
| **RPC** | `gdscript_rpc_callable.h/cpp` | 第 14 章 |
| **缓存** | `gdscript_cache.h/cpp` | 第 19 章 |
| **资源加载** | `gdscript_resource_format.h/cpp` | 第 18 章 |
| **内建函数** | `gdscript_utility_functions.h/cpp`、`gdscript_utility_callable.h/cpp` | 第 17 章 |
| **警告** | `gdscript_warning.h/cpp` | 第 22 章 |
| **编辑器集成** | `gdscript_editor.cpp`、`editor/` | 第 20 章 |
| **LSP** | `language_server/` | 第 21 章 |
| **文档** | `doc_classes/`、`editor/gdscript_docgen.*` | 第 20 章 |
| **图标** | `icons/` | —— |
| **测试** | `tests/` | 散见各章 |

> **小提示**：Godot 的命名相当一致——`gdscript_xxx.h` 几乎一定包含一个 `GDScriptXxx` 类。养成"看文件名猜类名"的习惯能极大提升源码浏览速度。

## 1.6　第一次走读：从 `GDScript::reload()` 入手

如果你愿意，现在就可以打开 `modules/gdscript/gdscript.cpp`，在 `GDScript::reload()` 这个方法上下断点（或直接通读一遍）。它会按顺序调用：

1. `GDScriptParser` 解析当前类的源码；
2. `GDScriptAnalyzer` 分析得到的 AST；
3. `GDScriptCompiler` 把分析后的 AST 编译为字节码；
4. 设置好 `valid = true`、初始化常量、注册到 `GDScriptLanguage::get_singleton()`；
5. 触发依赖该脚本的实例做热重载。

走完一遍，你就会对"这个 `.gd` 文件被点一下保存按钮，引擎里到底发生了什么"有非常具体的感受。

接下来的章节就是把这五步逐一展开。

---

## 本章小结

- GDScript 是 Godot 的**可选模块**，通过继承 `Script` / `ScriptInstance` / `ScriptLanguage` 接入引擎。
- 它是**渐进类型**的字节码解释型语言，设计哲学强调"紧凑而非完备"。
- 编译流水线为：源码 → Tokenizer → Parser → Analyzer → Compiler / ByteCodeGenerator → `GDScriptFunction.code` → VM。
- 整个实现里始终有两条暗线：**避免循环依赖**与**容忍未类型化代码**。理解这两条暗线，就理解了一大半"奇怪写法"的动机。
- 想真正跟一遍流程，从 `GDScript::reload()` 开始是最短路径。

下一章我们将进入流水线的第一站——`GDScriptTokenizer`，看看它是如何把一串 `char32_t` 变成结构化的 `Token` 序列的，特别是那让 Python 类语言总让人头痛的 INDENT / DEDENT 是怎么生成出来的。
