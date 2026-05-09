# 附录 C　推荐阅读

## C.1 入门必读

### Crafting Interpreters

* 作者：Robert Nystrom
* 在线：<https://craftinginterpreters.com/>
* 推荐理由：解释器入门最好的一本书。两个语言（Lox / 字节码版
  Lox）从零跟做，章末有挑战题；
* Mini 教程在很多设计上跟 Lox 一致——做完 Mini 再读这本能产生
  "原来这样可以这么写"的对照感。

### The Implementation of Lua 5.0

* 作者：Roberto Ierusalimschy 等
* PDF：<https://www.lua.org/doc/jucs05.pdf>
* 30 页的论文，把 Lua 内部所有关键机制讲清楚了：
  * register-based VM 设计；
  * upvalue / closure 实现；
  * table 的混合 array+hash 表示；
  * 增量 GC 三色不变量。

只要做脚本语言这篇必读。

## C.2 进阶教材

### Engineering a Compiler (2nd ed.)

* 作者：Cooper & Torczon
* 比龙书可读性强得多。重点章节：
  * Ch 4-5：递归下降 vs LR；
  * Ch 7-8：codegen；
  * Ch 9：数据流分析 / SSA；
  * Ch 13：寄存器分配。

如果你想从 Mini 这种 dynamic 语言往**静态语言**走，这本书把基
础打牢。

### Modern Compiler Implementation in ML/Java/C ("虎书")

* 作者：Andrew Appel
* 三个版本分别用 ML/Java/C 实现一个 Tiger 语言完整编译器；
* 比龙书更现代，比 Cooper 更动手；
* 想啃静态语言全套（lexer → 后端寄存器分配）选这本。

## C.3 性能与 JIT

### A Brief History of Just-In-Time

* 作者：John Aycock，2003
* 在线：<https://eecs.ucf.edu/~dcm/Teaching/COT4810-Spring2011/Literature/JustInTimeCompilation.pdf>
* JIT 历史综述，从 LISP 1960 到 HotSpot 全梳理。

### PEP 659: Specializing Adaptive Interpreter

* 作者：Mark Shannon
* <https://peps.python.org/pep-0659/>
* CPython 3.11 的 inline cache 设计。每一项 specialization 的
  收益数据都公开，是"数据驱动优化"的范本。

### Sea of Nodes

* 作者：Cliff Click（HotSpot lead）
* 论文：*"A Simple Graph-Based Intermediate Representation"*
* HotSpot JIT 的 IR；想做 method JIT 的必读。

### LuaJIT 的内部设计

* Mike Pall 在 lua-l 邮件列表写过大量解释；
* 整理博客：<http://wiki.luajit.org/Internals>
* trace JIT 入门最好的非学术资料。

## C.4 GC 圣经

### The Garbage Collection Handbook

* 作者：Jones / Hosking / Moss，2011
* 600 页，把 mark-sweep / 引用计数 / 分代 / 增量 / 并发 / 实时
  GC 全收齐。

如果你打算给 Mini 做工业级 GC，没有比这本更全面的。

### A Unified Theory of Garbage Collection

* 作者：David Bacon 等，OOPSLA 2004
* 短论文，证明"引用计数"和"追踪 GC"是同一算法的两端，中间
  各种混合方案都能由一个统一框架推出。
* 思想性很强，能把你的"GC 设计直觉"整明白。

## C.5 类型系统

如果你想给 Mini 加静态类型，从浅到深：

### Types and Programming Languages ("TaPL")

* 作者：Benjamin Pierce
* 编程语言类型系统的"龙书"；
* 从 simply-typed lambda 一直到 System F，每章都有 OCaml 实现。

### Programming Language Concepts (in OCaml)

* 作者：Peter Sestoft
* 比 TaPL 工程化，每个概念都有完整代码。

## C.6 工程化

### Build a Language Server in 60 Minutes

* GitHub: <https://github.com/Microsoft/vscode-extension-samples>
* MS 官方 LSP sample，照着跑能立刻上手。

### Tree-sitter

* <https://tree-sitter.github.io/tree-sitter/>
* 增量 parser 框架，被 GitHub / Neovim / Helix 大量采用；
* 想给 Mini 做 IDE 高亮 / 代码折叠用 tree-sitter 写一份 grammar
  最快。

## C.7 同类语言源码

按"读起来不头疼"程度排序：

| 项目 | 行数 | 语言 | 推荐章节 |
|------|------|------|---------|
| **Lua 5.4** | ~30K | C | `lvm.c`, `lcode.c`, `lgc.c` |
| **Wren** | ~15K | C | 整个项目，是 Crafting Interpreters 作者的产品级实现 |
| **Lox (clox)** | ~3K | C | 整个项目，Crafting Interpreters 的字节码版 |
| **Squirrel** | ~30K | C++ | `sqvm.cpp`, `sqcompiler.cpp` |
| **Pocketlang** | ~10K | C | 小巧，专为嵌入设计 |
| **CPython** | ~600K | C | `Python/ceval.c`, `Objects/dictobject.c` |
| **GDScript** | ~50K | C++ | 见第二部分逐章解析 |
| **MicroPython** | ~100K | C | `py/vm.c`, `py/objgenerator.c` |
| **LuaJIT** | ~80K + ASM | C + DynASM | 难，但 trace JIT 教学价值高 |

读源码顺序建议：**Lox → Wren → Lua → 你想深入的那门**。

## C.8 博客与资料站

* **craftinginterpreters.com 博客**：作者 Bob Nystrom 还写过一
  本《Game Programming Patterns》也很值得看。
* **Lambda the Ultimate** (lambda-the-ultimate.org)：编程语言研
  究领域的论坛，能看到学界讨论。
* **Eli Bendersky 的博客** (eli.thegreenplace.net)：很多解释器
  /编译器实战文章，代码可读性极佳。
* **/r/ProgrammingLanguages**：Reddit 同好社区，新人提问得到答
  复的速度快。
* **HN tag "compilers"**：每隔几周就有高质量长文。

## C.9 视频课程

* **CS 6120: Advanced Compilers** (Cornell, Adrian Sampson)
  - Bril IR、SSA、数据流分析；YouTube 公开。
* **Stanford CS143** (Compilers)
  - 经典编译器课，配 Cool 语言。
* **PL Zoo** (plzoo.andrej.com)
  - 各种小语言的 OCaml 实现（mini-prolog、mini-haskell、
    mini-Prolog ……），看完对"语言家族图谱"会有直观理解。

## C.10 写在最后

读这些资料的策略：

* **先动手再读**：自己写过 Mini 之后再读 Lua 源码，会有"对，
  我的 do_call 也是这样"的共振；先读代码很容易"看懂但不
  会"。
* **不求覆盖，求深度**：把 Crafting Interpreters 的 clox 部分
  读 3 遍，比把 5 本编译书读 1 遍收获大得多；
* **跟踪 1 个 PEP / 1 个 LuaJIT 邮件**：跟着真实 issue 读 PR/
  patch，比读教材更接近"工业开发"。

写解释器是终身有用的技能——你后续做 build system、做配置 DSL、
做查询引擎、做模板渲染、做正则引擎，本质都是"把符号解析成行
为"。每一种新语言/新工具都是同一套技术的变体。

Mini 是你的起点。继续走下去。

