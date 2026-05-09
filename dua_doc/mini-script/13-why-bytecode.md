# 第 13 章　为什么要换字节码 VM

第二部分我们写的树遍历解释器（tree-walking interpreter）能跑、
能读、能改——是教学的最佳起点。但它有几个**结构性**的性能问题
注定它跑不快。这一章先把这些问题看清楚，再讲清楚字节码 VM 是
怎么把它们逐一干掉的。

理解"为什么慢"比"怎么改快"更重要——你会在后续章节看到 Lua、
Python、V8 是怎么用同一组思想反复推到极致的。

## 13.1 树遍历到底慢在哪

跑一段最普通的代码：

```python
let s = 0
let i = 0
while i < 1000000 do
    s = s + i
    i = i + 1
end
```

1000000 次循环，每次循环做这些事：

1. **读 `i`**：visit `IdentExpr` → 调 `current_env_->get("i")`
   * 字符串 hash
   * unordered_map bucket walk
   * 字符串 == 比较
   * Value 复制（含 shared_ptr 原子加 1）
2. **常量 `1000000` 求值**：visit `NumberLit` → Value 构造
3. **比较**：visit `BinaryOp(<)` → 走 evaluator 大 switch
4. **判 truthy**：分支跳转
5. **进入循环体**：进入新作用域 → make_shared<Environment>
6. 重复 read/write s, i……

仔细看，每条语句的开销由以下几部分构成：

| 操作 | 估算耗时（ns） | 占比 |
| --- | --- | --- |
| 虚函数 `accept` | ~2 | 5% |
| `unordered_map::find` 一次 | ~50 | 60% |
| Value move/copy（含 shared_ptr） | ~10 | 15% |
| visitor 大 switch 查表 | ~3 | 5% |
| `make_shared<Environment>` | ~80 | （只在 push scope 时）|
| 实际算术 | ~1 | < 5% |

**做事的部分（实际算术）只占 5%**，剩下 95% 都是"组织开销"。
慢的根因不在某个具体函数，而在**架构**：

1. **变量名是字符串**，每次访问都要 hash + 查 map；
2. **作用域是堆分配的对象**，每次 push 都 alloc；
3. **AST 是树结构**，遍历时 cache miss 严重——节点散在堆上；
4. **每个节点都是虚调用**——CPU 分支预测帮不了忙；
5. **递归遍历让 C++ 调用栈深得很**——每个表达式至少多一层。

## 13.2 字节码 VM 怎么干掉这些

字节码 VM 的核心就是把上面 5 条**一一翻案**：

### 翻案 1：变量名 → 槽位下标

编译期把变量名解析为**编号**：第 1 个 local 是 0 号槽，第 2 个
是 1 号槽……运行时只需 `stack[base + i]`。**字符串 hash 消失。**

```
let i = 0          → STORE_LOCAL 0
let s = 0          → STORE_LOCAL 1
i + 1              → LOAD_LOCAL 0 ; PUSH_INT 1 ; ADD
```

### 翻案 2：堆分配的 Environment → 单一连续栈

所有 local 都活在一个 `vector<Value>` 上。函数调用只是"栈指针前
进 N 个槽"，不分配。**make_shared 消失。**

```
[ ... | a | b | c | x | y | z | s | i | ... ]
       ↑                       ↑       ↑
       fn outer 的栈帧         inner    新调用入口
```

### 翻案 3：树状 AST → 线性字节码数组

字节码就是 `vector<uint8_t>` 或 `vector<Instruction>`，连续内存
一字排开。CPU 预取器爱死这种结构。**Cache miss 消失。**

```
[ OP_LOAD_LOCAL, 0, OP_PUSH_INT, 1, OP_ADD, OP_STORE_LOCAL, 0, ... ]
```

### 翻案 4：虚调用 → 大 switch / computed goto

VM 主循环就是一个大 switch（或 GCC 的 `&&label` 计算 goto），
所有 dispatch 在一个函数内。**虚函数表与分支预测完全友好。**

```cpp
while (true) {
    switch (*ip++) {
        case OP_LOAD_LOCAL:  /* ... */ break;
        case OP_ADD:         /* ... */ break;
        // ...
    }
}
```

### 翻案 5：递归遍历 → 显式调用栈

VM 自己维护一个"调用帧栈"——`vector<CallFrame>`。函数调用是数
据操作，不是 C++ 函数调用。**深递归不会爆 C++ 栈。**

## 13.3 几个数字对比

我们后面会一步一步实现，最终在 fib(30) 这种纯递归测试上能看到：

| 实现 | fib(30) 耗时 |
| --- | --- |
| 第二部分树遍历 | ~500ms |
| 第三部分字节码 VM | ~30ms |
| Lua 5.4 | ~10ms |
| LuaJIT | < 1ms |

我们能做到的极致是第三档（Lua 5.4 同水平）——再快需要 JIT，那
是另一本书的事。

## 13.4 字节码 VM 不是免费的

天下没有白给的午饭。字节码 VM 比树遍历多了这些代价：

* **多一个编译 pass**：AST → bytecode，需要写 `Compiler` 类；
* **闭包变复杂**：环境链没了，得用 **upvalue** 机制——这是第 19
  章的硬骨头；
* **调试更难**：bytecode 与源码的对应关系要单独维护（line table）；
* **错误信息要走源码反查**：bytecode 里没有"语句"概念，错位的
  字节码索引不会自动告诉你"这是第 17 行的 +"；
* **代码体量翻一倍**：Compiler ~600 行，VM ~500 行，Opcode 表 ~50
  行——你得写多一倍的东西。

代价值不值？看你的目标：

* 想学清楚解释器原理 → 树遍历足够，第二部分就停；
* 想做能上线的脚本语言 → 必须上字节码；
* 想做游戏引擎脚本（Godot / Unity 风格）→ 必须上字节码；
* 想极致性能 → 字节码上面再加 JIT。

## 13.5 栈式 vs 寄存器式：两种 VM 架构

字节码 VM 内部还分两大派：

### 栈式（stack-based）

代表：JVM、CPython、CIL（.NET）。

每个操作数都从一个**操作数栈**上取，结果也压回栈。

```
LOAD_LOCAL 0     ; push i
PUSH_INT 1       ; push 1
ADD              ; pop 2, push i+1
STORE_LOCAL 0    ; pop, write to i
```

* **优点**：指令本身极短（无操作数或只有一个），编码紧凑；
  Compiler 写起来非常直观；
* **缺点**：每次运算都至少两次"读栈 → 写栈"——内存流量大。

### 寄存器式（register-based）

代表：Lua 5+、Dalvik、V8 Ignition。

每条指令显式指定"读哪几个槽 → 写哪个槽"。

```
ADD R0, R0, 1    ; R0 = R0 + 1
LT  R2, R0, R1   ; R2 = R0 < R1
```

* **优点**：每条指令做的事更多，**指令数量减少 30~50%**；运算
  直接在槽间走，不来回 push/pop；
* **缺点**：每条指令更长（通常 4 字节）；Compiler 要做"虚拟寄
  存器分配"。

实测表明：**同等优化水平下，寄存器式比栈式快 20~40%**。Lua 5
当年从栈式改成寄存器式提升非常显著。

我们 Mini 走**栈式**，原因：

* 代码量少 30%——教学优先；
* Compiler 写起来一目了然，不需要寄存器分配算法；
* 字节码可视化（disassembler）易读；
* 慢一点但仍比树遍历快 10 倍以上。

如果你看完本书想自己写寄存器式版本，第 14 章会顺带画出两者的指
令布局对比，方便你对照改。

## 13.6 重构的复用与改动

我们的代码规划：

| 模块 | 复用情况 |
| --- | --- |
| `Lexer` | **完全复用**，0 改动 |
| `Parser` / Pratt | **完全复用** |
| `AST` | **完全复用**（visitor 还在，只是不再被解释器用） |
| `Value` | **几乎复用**，可能微调（NaN-boxing 优化） |
| `Table` | **完全复用** |
| `Function` | **加字段**：`bytecode`、`constants`、`upvalues` |
| `Environment` | **不再使用**——被栈帧取代 |
| `Interpreter` | **拆成两个类**：`Compiler` + `VM` |
| `builtins` | **完全复用** |

也就是说我们整个前端不动，只换"AST 之后的执行机制"。这种"前
后端解耦"是教学项目设计上很值的——你能反复实验不同的 backend
而不破坏 frontend。

## 13.7 第三部分要造的东西总览

接下来 8 章我们会按这条路推进：

* **第 14 章 Opcode 设计**：列出 Mini 的 35 条指令，说明每条做
  什么，为什么这样设计；
* **第 15 章 Compiler**：visitor 模式 walk AST，发射 opcode；
* **第 16 章 常量池与符号解析**：把字面量、全局名集中存放；
* **第 17 章 VM 主循环**：fetch/decode/execute 大 switch，对比
  `computed goto`；
* **第 18 章 Call Frame**：函数调用怎么在栈上完成，没有 C++ 递归；
* **第 19 章 Upvalue**：闭包在字节码 VM 里怎么实现——这是最有
  趣也最难的一章；
* **第 20 章 Disassembler**：把字节码反汇编成可读形式，调试必备。

每一章都会跑通对应的功能，并保留前面已通过的所有单元测试。当第
20 章结束时，Mini 拥有一个完整的字节码后端，前端不动一行。

## 13.8 一个心理准备：你会重写一些代码

把树遍历当**原型**——它存在的目的是让你**先把语义跑通**：什么
是闭包、`for` 应该 push 几层 scope、`return` 怎么跨 if 跳出。这
些语义决定一旦定下来，无论 backend 是树遍历还是字节码，行为都
要一致。

第三部分写 VM 时，你会反复回头跑第二部分的单测（已有 60+ 测试），
确保两个 backend 行为完全一致——这是工业项目的常用做法：
**前端 + 多个 backend + 共享测试集**。Lua、Python 内部都是这种
结构（CPython 的 ceval.c 是字节码 VM；早期 Python 1.x 还有过树
遍历版本）。

## 13.9 一个有趣的副产品：Disassembler

字节码的好处之一是它可以**被打印**。我们写的 Disassembler 能输
出：

```
fn fib (1 param, 2 locals)
   0  LOAD_LOCAL    0       ; n
   2  PUSH_INT      2
   5  LT
   6  JUMP_IF_FALSE 14
   9  LOAD_LOCAL    0
  11  RETURN
  14  LOAD_LOCAL    0       ; n
  16  PUSH_INT      1
  19  SUB
  20  GET_GLOBAL    0       ; fib
  23  CALL          1
  ...
```

这个工具本身就是 Mini 这门语言"成熟"的标志——你能像看汇编一样
看自己的脚本运行了什么。后续做 profiler、调试器、优化都靠它。

GDScript 的 `gdscript_disassembler.cpp` 也是出于同样的理由存在
的——开发者能查看任意脚本编译出的 opcode，定位性能问题。

## 13.10 准备好了吗？

如果你跟着第二部分把代码全跑通了，现在的状态是：

* 一个 1900 行的 Mini 解释器，能跑你写的任意脚本；
* 60+ 单元测试，全部通过；
* REPL 和 examples/ 里若干程序；
* 对每个特性的语义清晰（"为什么 `for` 要每轮 push scope"）。

接下来的 8 章会让 Mini 跑**快 10~20 倍**，并让你理解 Lua / Python /
GDScript 内部 VM 的几乎所有关键概念。

下一章 **第 14 章 Opcode 设计** 我们先把字节码"指令集"定下来。
就像设计 CPU 的指令集一样——决定指令多少、每条指令做什么、用什
么编码、参数布局。这些决定会影响后面所有章节的代码长什么样。
