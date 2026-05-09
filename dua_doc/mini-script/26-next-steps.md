# 第 26 章　继续走下去：练习题与扩展方向

恭喜——你已经从零写完了一门动态脚本语言。回顾一下我们做了什
么：

* 1500-3000 行 C++（不含测试），实现了 Mini 的完整工具链；
* 词法、语法、AST、Tree-walking Evaluator、Bytecode Compiler、
  Stack VM、GC、Native FFI、LSP、反汇编 ……；
* 比 GDScript 简单，但**核心架构同构**——读 Mini 源码不会觉得
  虚，读 GDScript / Lua / Python 源码也不会陌生。

现在你已经具备**修改任何主流脚本语言的能力**。这一章给你下一
步的路线图。

## 26.1 练习题：从易到难

### 入门（半小时~一天）

**练习 1**：给 Mini 加 `+=` / `-=` / `*=` / `/=` 复合赋值。
提示：编译期把 `x += y` 展开成 `x = x + y`，OP_LOAD_LOCAL 上
来的值先存一份，AST 加 `CompoundAssign` 节点。

**练习 2**：加一个 `mini -e "expr"` 命令行选项，直接计算表达式
不写文件。提示：parser 加 expression-only 入口，套个 `print()`
壳。

**练习 3**：给 `print` 加格式化 —— `print("a={}, b={}", a, b)`。
提示：检测第一个参数是 string 且包含 `{}`，pure-C++ 实现替换。

**练习 4**：让 Mini 支持单引号字符串（`'hello'` ≡ `"hello"`），
检查 lexer。

### 进阶（一周）

**练习 5**：加 `for k, v in tbl do ... end`（pairs 风格）。
提示：实现一个内置的 `__iter` 协议：`for` 在编译期翻译成
`__iter, __state, var = expr; while true do var = __iter(__state, var); if var == nil then break end ...`。

**练习 6**：`break` 与 `continue`。提示：编译 `while` 时维护
`std::vector<int> break_jumps_`，每条 break 发射 `OP_JUMP placeholder`
入栈，循环结束后回填。

**练习 7**：给 Mini 加 OOP：`class Foo { fn new() { ... } }` →
脱糖成 table + 元方法。提示：Lua 的实现思路。

**练习 8**：实现 `try / catch`。提示：在 VM 里维护
`exception_handlers_` 栈，`try` 时 push 一个 (frame, ip, slot)
三元组；任意 RuntimeError 抛出时不直接 throw 出 main_loop，而
是先看栈顶是否有 handler，有就 unwind 到那里跳到 catch_ip。

### 高阶（一个月+）

**练习 9**：Mini 协程。提示：CallFrame stack 抽出成
`Coroutine`，`yield` 切换 frames 指针。Lua 的协程实现 ≈ 300
行——非常优雅。

**练习 10**：模块系统 `import "math"`. 提示：`require` 函数
+ 全局缓存表 `_LOADED`. 加一层路径解析。

**练习 11**：渐进式静态类型。`let x: int = 0`. 编译期类型推
断 + warning"`x` 被赋了 string"。提示：Hindley-Milner 想都别
想，先做最朴素的"declared type 必须匹配 RHS 推出的 type"。

**练习 12**：JIT。先做 template JIT —— 每条 opcode 一段固定
汇编（用 [asmjit](https://asmjit.com/) 这种 runtime assembler
库），把热函数翻译成机器码 thunks 拼接。

## 26.2 扩展方向：把 Mini 变得"有用"

光有语言不够，要让 Mini 真的能用做事。几个方向：

### 嵌入引擎（最有市场）

把 Mini 当游戏引擎/工具/服务器的脚本层。需要：

* 稳定 ABI 的 C 头文件 (mini.h)，让 host 能 link Mini lib；
* 双向 marshalling（host 暴露的 C struct ↔ Mini table）；
* 沙箱模式（剥离 io、os）；
* 热重载（编译完替换函数对象，老的 closure 还活着）。

参考 Lua 的 `lua.h` 和 GDScript 的 `ScriptLanguage` 接口。

### 解释器即配置文件 (DSL)

把 Mini 用作配置——比游戏里的 JSON / YAML 强得多，能写条件、
循环、函数。Nix、Starlark (Bazel)、Pkl 都是这个方向。需要的额
外工作：

* 严格沙箱（非 pure 调用全部禁用）；
* 确定性（一份输入永远得同一结果——禁随机、时间）；
* 增量重算（修改一行配置不要重跑全部）。

### 服务端编排

类似 nginx 的 Lua 模块——HTTP 请求走 C 主框架，业务逻辑用
Mini 写。需要：

* 协程 + epoll 集成；
* 高性能 string 处理；
* JSON / HTTP / Redis client 写成 native 模块。

OpenResty 就是这个生态的标杆。

### REPL / Notebook

把 Mini 作为数据科学的"小工具"——快速一次性脚本。需要：

* 持久化 REPL（重启后变量还在）；
* 漂亮的 print（table 自动 pretty-format）；
* 历史记录、自动补全、syntax highlight；
* Jupyter kernel 适配。

## 26.3 不要做的事

已经被验证是大坑的方向，新手最好别碰：

* **设计自己的"全新"语法范式**——你以为很酷，用户觉得很怪。
  抄 Python/Lua/Rust 已知设计，把精力放实现上。
* **过早优化**——Mini 跑 fib(30) 1.5 秒不是问题，能跑通才是。
  优化是写完之后做的。
* **未经测试的"大改"**——加 GC 时不要顺手改 calling convention。
  一次只改一件事，每次都跑回归测试。
* **追求"完整 Pythoncompatibility"**——意味着无穷无尽的坑。
  Mini 要清楚自己的边界在哪。

## 26.4 推荐阅读

按"动手做"友好度排序：

### 必读

1. **《Crafting Interpreters》** by Robert Nystrom（[免费在线](https://craftinginterpreters.com/)）
   - 写得最好的解释器入门书，全程跟做能写出 Lox 语言；
   - 我们 Mini 很多设计思路跟 Lox 一致（Pratt parsing、字节码 +
     stack VM、upvalue 机制、单链表 GC）；
   - 强烈建议做完 Mini 后通读对照——会有"原来如此"的连贯感。

2. **Lua 5.x 源码**（[lua.org](https://www.lua.org/source/5.4/)）
   - 30K 行 C，世上最干净的脚本实现；
   - 重点看 `lvm.c`（VM 主循环）、`lcode.c`（codegen）、`lgc.c`
     （增量 GC）；
   - 搭配 Roberto Ierusalimschy 的论文《The Implementation of
     Lua 5.0》一起看，事半功倍。

### 进阶

3. **《Engineering a Compiler》** by Cooper & Torczon
   - 现代编译原理，比龙书读起来顺；
   - 重点章节：SSA、寄存器分配、循环优化；
   - 想做静态语言或 JIT 必备。

4. **《Optimizing Compilers for Modern Architectures》** by Kennedy
   - 循环变换、向量化的圣经；
   - 想让脚本语言跑得跟 C 一样快才需要看。

5. **CPython 源码**（[github.com/python/cpython](https://github.com/python/cpython)）
   - 几十万行，但 `Python/ceval.c` 的主循环值得反复读；
   - PEP 659（specialized opcodes）、PEP 703（去 GIL）这些 PEP
     是 Python 内部演进的一手资料；
   - Brett Cannon 的"Python's Inner Workings"博客系列。

### 论文

6. **The Implementation of Lua 5.0** （Ierusalimschy 等，2005）
   - 把 stack VM、register VM、upvalue、table 设计讲透；
   - 任何写脚本语言的人都该读三遍。

7. **A Tail-Recursive Machine With Stack Inspection** （Clements
   & Felleisen）
   - 尾调用优化的形式化处理。

8. **PEP 659: Specializing Adaptive Interpreter** （Mark Shannon）
   - CPython 3.11 的 inline cache 设计，最现代的 bytecode 优化
     文献。

### V8 / JIT 路线

9. **Sea of Nodes** （Cliff Click）
   - HotSpot JIT 的 IR 设计；
   - 想做 method JIT 必读。

10. **A Brief History of Just-In-Time** （John Aycock，2003）
    - JIT 历史综述，整理思路用。

## 26.5 GDScript 反向阅读建议

Mini 写完后再看 GDScript 源码，很多东西会有"原来 production
版本是这么处理"的感觉。建议阅读顺序：

1. `gdscript_tokenizer.{h,cpp}` ↔ Mini lexer；
2. `gdscript_parser.{h,cpp}` ↔ Mini parser（注意 panic-mode、
   warning_ignore 这些工业细节）；
3. `gdscript_analyzer.{h,cpp}` ↔ Mini 没有的"类型分析"阶段——
   GDScript 的核心复杂度都在这；
4. `gdscript_codegen.{h,cpp}` 与 `gdscript_byte_codegen.{h,cpp}`
   ↔ Mini compiler；
5. `gdscript_function.{h,cpp}` ↔ Mini VM；
6. `gdscript_warning.{h,cpp}` ↔ Mini 第 23 章 diagnostics（GDScript
   的更系统）；
7. `language_server/` 目录 ↔ Mini 第 24 章 LSP。

每读完一节都问自己：

* GDScript 这里多做了什么？为什么要多做？
* 能不能简化（教学/嵌入用）成 Mini 那样？
* 复杂度的分摊点在哪——是性能、是 UX、是和 Godot 引擎的集成
  约束？

这种对比阅读会让你既懂"原理"又懂"工程"。

## 26.6 写这本教程时我学到的

最后说点元。写这本教程的时候，我自己的几个体会：

1. **"教学版"和"生产版"差的不是核心算法，而是边界情况**。
   Mini 的 evaluator 200 行，GDScript 的 evaluator 千行——多出
   来的全是"用户写出某种刁钻语法时不要 crash"的兜底；

2. **设计决策永远是 trade-off**：栈式 vs 寄存器式、RC vs GC、
   tree-walking vs bytecode、static vs dynamic typing——没有
   "最好"的选择，只有"最适合 use case"的选择；

3. **把工具链做完比把语言做完重要得多**：能跑的 100% 语言 + 没
   有 LSP 没有 debugger 没有 disassembler，跟带全套工具的 70%
   语言比起来，前者根本没人用；

4. **写解释器是性价比极高的"通用编程能力训练"**：你会被迫接
   触 OS（栈管理、内存分配）、CPU（dispatch、cache）、数据结构
   （hash 表、红黑树）、算法（图遍历、动态规划）的方方面面——
   非常综合。

5. **从 1500 行到 3000 行 ROI 远高于 3000 → 30000 行**：第一
   个版本能跑就该上 git tag v0.1，开源放出去，有人用了再迭代。
   不要憋大招。

## 26.7 全书完结

到这里 Mini 教程就结束了。你写的代码：

* `src/lexer.{h,cpp}`：~300 行
* `src/parser.{h,cpp}`：~700 行
* `src/ast.{h,cpp}`：~200 行
* `src/value.{h,cpp}`：~250 行
* `src/environment.{h,cpp}`：~100 行
* `src/evaluator.{h,cpp}`：~500 行（树遍历版）
* `src/compiler.{h,cpp}`：~800 行
* `src/vm.{h,cpp}`：~700 行
* `src/gc.{h,cpp}`：~600 行
* `src/disassembler.{h,cpp}`：~150 行
* `src/builtins.{h,cpp}`：~300 行
* `src/diagnostic.{h,cpp}`：~200 行
* `src/lsp/*`：~800 行

总计约 5500 行——比"3000 行"超了一些，但这是包含 LSP / GC /
diagnostics 的"工业向"实现。如果只到第三部分（VM 跑通），
实际不到 3000 行。

去 GitHub 开个 repo 吧。从 Mini 变成你的 Mini——加上你想要的
特性、改成你自己的语法。**写一门自己的语言**是程序员最好玩的
事之一，你已经入门了。

祝你 hack 愉快。

---

## 附录索引

* 附录 A [完整 Mini 语法 BNF](appendix-a-grammar.md)
* 附录 B [Opcode 速查表](appendix-b-opcodes.md)
* 附录 C [推荐阅读](appendix-c-reading.md)

