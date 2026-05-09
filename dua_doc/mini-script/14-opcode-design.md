# 第 14 章　Opcode 设计：栈式 vs 寄存器式

写虚拟机的第一步不是写 VM 主循环，而是**确定指令集**。指令集像
CPU 的 ISA——决定了 Compiler 和 VM 之间的契约。一旦定下来，后
面所有代码都是对它的实现。

这一章把 Mini 字节码的指令集敲定。我们走栈式 VM（理由在第 13 章
讲过），共 35 条指令——足以覆盖整门语言。

## 14.1 一个最简单的栈式例子

先看 `1 + 2 * 3` 怎么跑：

```
PUSH_INT 1            ; 栈：[1]
PUSH_INT 2            ; 栈：[1, 2]
PUSH_INT 3            ; 栈：[1, 2, 3]
MUL                   ; 栈：[1, 6]   (pop 2,3 → push 6)
ADD                   ; 栈：[7]
```

栈上**只剩**一个结果——这就是表达式求值的固定模式。每个表达式
执行完都"在栈顶留下一个值"。

每个语句执行完则要"把栈清干净"——临时表达式的值被消费或显式
`POP` 掉。这是栈式 VM 的不变量。

## 14.2 指令编码：两种主流方案

### 方案 A：变长指令（每条 1~5 字节）

```
[OP_PUSH_INT][int64: 8 字节]
[OP_LOAD_LOCAL][slot: 1 字节]
[OP_ADD]
```

* 优点：紧凑，常用指令只占 1 字节；
* 缺点：fetch/decode 麻烦，反汇编要按字节走。

### 方案 B：定长指令（每条 4 字节）

```
struct Instruction {
    uint8_t op;
    uint8_t a;     // 寄存器/槽位
    uint16_t bx;   // 立即数 / 跳转偏移
};
```

* 优点：fetch 一次 32-bit 一气拿到，可以 `pc++` 简单递增；
* 缺点：8KB 的程序变成 32KB——但谁在乎呢；

我们走 **C 方案的简化版** —— `vector<uint32_t>`，每条指令 1 个
32-bit word。第 17 章 VM 主循环时你会看到它如何让 dispatch 极简。

```cpp
// src/opcode.h
#pragma once
#include <cstdint>

namespace mini {

enum Opcode : std::uint8_t {
    // ===== 栈操作 =====
    OP_PUSH_NIL,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_PUSH_INT,        // arg = signed 24bit 立即数（小整数快路径）
    OP_LOAD_CONST,      // arg = 常量池下标（其余字面量都走这里）
    OP_POP,             // 弹一个

    // ===== 局部变量（栈帧上的槽位）=====
    OP_LOAD_LOCAL,      // arg = slot
    OP_STORE_LOCAL,     // arg = slot；不弹栈
    OP_STORE_LOCAL_POP, // arg = slot；弹栈

    // ===== 全局变量（按名查全局表）=====
    OP_LOAD_GLOBAL,     // arg = 常量池里的名字下标
    OP_STORE_GLOBAL,
    OP_DEFINE_GLOBAL,

    // ===== 闭包 upvalue（第 19 章）=====
    OP_LOAD_UPVALUE,
    OP_STORE_UPVALUE,
    OP_CLOSE_UPVALUE,

    // ===== 算术与逻辑 =====
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_NEG, OP_NOT,
    OP_CONCAT,                 // 字符串拼接 ..

    // ===== 比较 =====
    OP_EQ, OP_NE,
    OP_LT, OP_LE, OP_GT, OP_GE,

    // ===== 控制流 =====
    OP_JUMP,                   // arg = 偏移（24bit signed）
    OP_JUMP_IF_FALSE,          // 不弹栈
    OP_JUMP_IF_FALSE_POP,      // 弹栈版本

    // ===== 函数 =====
    OP_CLOSURE,                // arg = 函数原型常量池下标
    OP_CALL,                   // arg = 实参个数
    OP_RETURN,                 // 弹栈顶为返回值

    // ===== 复合数据 =====
    OP_NEW_TABLE,              // 创建空 table
    OP_GET_INDEX,              // 弹 obj/key，push value
    OP_SET_INDEX,              // 弹 value/obj/key

    // ===== 调试 =====
    OP_LINE,                   // arg = 源码行号；只在 debug 构建发射

    OP_HALT,                   // VM 退出

    OP_COUNT,
};

}  // namespace mini
```

35 条指令——能完整描述 Mini 的全部语义。

## 14.3 32-bit 字格式

我们的每条指令编码：

```
 31           24 23                                 0
+---------------+-----------------------------------+
|    opcode     |              arg (24-bit)         |
+---------------+-----------------------------------+
```

* 高 8 位：opcode（最多 256 条指令，够用）
* 低 24 位：参数（slot / const-index / signed jump offset / 等）

24 bit 能表达：

* 无符号：0..16777215（够任何函数的常量池/局部数）
* 有符号：-8388608..8388607（够函数体的跳转偏移）

如果有指令需要更多参数（比如 `CALL` 需要 callee + argc + ret），
我们用"两条相邻指令"——第一条是主指令、第二条是辅助参数。但
Mini 暂时不需要这种。

```cpp
// src/opcode.h（续）
inline std::uint32_t encode(Opcode op, std::int32_t arg = 0) {
    // arg 范围检查（debug only）
    return (static_cast<std::uint32_t>(op) << 24)
         | (static_cast<std::uint32_t>(arg) & 0x00FFFFFFu);
}

inline Opcode get_op(std::uint32_t inst) {
    return static_cast<Opcode>(inst >> 24);
}

// arg 既能取无符号也能取符号（视指令而定）
inline std::uint32_t get_uarg(std::uint32_t inst) {
    return inst & 0x00FFFFFFu;
}

inline std::int32_t get_sarg(std::uint32_t inst) {
    std::int32_t v = static_cast<std::int32_t>(inst & 0x00FFFFFFu);
    // sign-extend from 24 to 32
    if (v & 0x00800000) v |= 0xFF000000;
    return v;
}
```

`get_sarg` 的 sign-extend 是一行可被 `if (v & ... )` 优雅替代但
显式写出来更安全。GDScript 的 opcode 就用类似的小段。

## 14.4 详解一些有意思的指令

### `OP_PUSH_INT` 的 24-bit 立即数

字面量 `1`、`-1`、`0`、`100` 这种**小整数**非常常见。如果都走
常量池：

```
LOAD_CONST 0   ; 0 号常量是 1
LOAD_CONST 1   ; 1 号常量是 2
ADD
```

每个常量都占常量池一个 Value 槽（24 字节），还要一次 array 索
引。**24-bit 立即数足以覆盖 ±800 万**——绝大多数小整数：

```
PUSH_INT 1     ; 立即数直接进字节码
PUSH_INT 2
ADD
```

省一次 cache miss。Lua、V8、JVM 都做这种优化。

但 1.5 这种浮点不能 inline，仍走 `LOAD_CONST`。

### `STORE_LOCAL` vs `STORE_LOCAL_POP`

为什么要两个变体？看：

```python
let x = 1 + 2     # 表达式留在栈顶 → STORE_LOCAL_POP，pop 进 x
x                  # 单独表达式语句 → 求值后 POP
print(x = 5)      # x = 5 是表达式，5 进 x **同时**还要传给 print
```

最后一种情况下我们要 `STORE_LOCAL`（不弹栈）让 5 留在栈上给
`print`。Compiler 根据上下文选用哪个变体。

类似的还有 `JUMP_IF_FALSE` vs `JUMP_IF_FALSE_POP`：`if cond then`
要求消费 cond，但 `a and b` 短路时要保留 cond 作为整个表达式的
结果。

这种"成对"的指令是栈式 VM 的常见手法——增加 1~2 条指令换运行
时少做一次 push/pop。

### `OP_CLOSURE`：从函数原型到运行时函数

这是最有趣的一条。

我们 Compiler 编译一个函数时，会把它的 bytecode + 元数据打包成一
个**FunctionProto**（函数原型）。运行时执行 `OP_CLOSURE` 时：

```
执行 OP_CLOSURE arg=K 的语义：
1. 取常量池第 K 项（一个 FunctionProto）
2. 创建一个 Function 对象，绑定当前 upvalue 列表
3. 把 Function push 到栈
```

闭包语义就藏在第 2 步——"绑定当前 upvalue 列表"。第 19 章会
详细讲怎么实现。

为什么需要"原型 + 运行时实例"两层？因为同一个 `fn` 表达式被多
次执行时（比如在循环里），每次都该产生一个独立的闭包：

```python
let fns = []
for i in 0..3 do
    fns[i] = fn() return i end   # 每轮创建新闭包，捕获不同的 i
end
```

bytecode 只生成**一份**——但执行 3 次 `OP_CLOSURE` 产生 3 个
Function 实例。这是 Lua 的关键设计。

### `OP_CALL` 的栈布局

调用前：

```
... | callee | arg0 | arg1 | arg2 |   ← 栈顶
                                    sp
```

执行 `OP_CALL 3`（3 个实参）：

1. 从 sp 往下数 3+1 找到 callee；
2. 检查 callee 是 function；
3. 创建 CallFrame，base = callee 位置；
4. **栈不动**——参数槽就是新栈帧的 local 0..2，callee 槽放第一
   个 upvalue 之类用途；
5. PC 跳到函数 bytecode 起点。

调用结束（`OP_RETURN`）：

1. 把返回值（栈顶）拷贝到 callee 位置；
2. 把 sp 调回 callee 之上一个；
3. 弹 CallFrame，恢复 PC。

栈的"原地复用"非常关键——参数不需要再拷贝到 frame 的 local 区，
它们**就是** local 区的前 N 个槽。

### `OP_LINE`：仅 debug 的"行号锚"

```
OP_LINE arg=42       ; 接下来的指令对应源码第 42 行
```

只在 `#ifdef DEBUG` 时发射。VM 主循环遇到它就把 `current_line` 更
新一下，让运行时错误能给出"第 42 行"——而不是"指令偏移
0x1A4F"这种用户看不懂的位置。

GDScript 也是同样的设计——见前面 GDScript 书第 23 章那一段。

## 14.5 栈式 vs 寄存器式：同一段代码对比

`a = b + c`：

栈式（5 条指令）：

```
LOAD_LOCAL 1      ; b
LOAD_LOCAL 2      ; c
ADD
STORE_LOCAL_POP 0 ; a
```

寄存器式（1 条）：

```
ADD R0, R1, R2    ; a = b + c
```

看起来寄存器式碾压栈式？是的——**指令数差距大**。但：

* 寄存器式每条指令 4 字节，栈式 5 条 × 4 字节 = 20 字节，确实多；
* 但**实际运行时**，寄存器式的 fetch/decode 仍然是 1 次，栈式是
  4 次——所以 dispatch 开销栈式更高；
* 寄存器式需要 Compiler 做"虚拟寄存器分配"——这是优化器领域
  的硬骨头（活跃变量分析、register coloring）。

Lua 选择寄存器式，每个函数最多 250 个寄存器（用 8 bit 索引），
带来的性能收益让 Lua 5 比 4 快了 50%。但 Lua 的 Compiler 因此变得
非常复杂。

我们的 Mini Compiler 走栈式——15 章你会看到它的 visitor 写起来
简直是"自然 push/pop"，完全不需要寄存器分配算法。

## 14.6 一个完整例子：fib 编译成什么

```python
fn fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end
```

Compiler 会生成（伪反汇编）：

```
fib (1 param, 1 local: n)
   0  LOAD_LOCAL      0       ; n
   1  PUSH_INT        2
   2  LT
   3  JUMP_IF_FALSE_POP  6
   4  LOAD_LOCAL      0
   5  RETURN
   6  LOAD_GLOBAL     0       ; fib
   7  LOAD_LOCAL      0       ; n
   8  PUSH_INT        1
   9  SUB
  10  CALL            1
  11  LOAD_GLOBAL     0       ; fib
  12  LOAD_LOCAL      0
  13  PUSH_INT        2
  14  SUB
  15  CALL            1
  16  ADD
  17  RETURN
```

只 18 条指令——同样的算法在树遍历里要走 50+ 次 `accept` 调用 +
n 次 `unordered_map::find`。10 倍加速不是吹的。

注意几个细节：

* **`LOAD_GLOBAL 0`**：这里的 0 是常量池里"fib"这个字符串的索
  引，不是全局表的 slot。运行时 `LOAD_GLOBAL` 用这个字符串去查
  全局表——一次 hash。Lua 把这优化成"global cache"，我们留作
  练习；
* **第 4-5 行**：`return n` 在 `if` 分支里，`JUMP_IF_FALSE_POP`
  到 6（如果条件假，跳过 return）；
* **No `OP_LINE`**：这是 release 反汇编，debug 构建会在每条语
  句前插一个。

## 14.7 指令集设计的几条经验

回顾设计这套指令集时的一些选择：

1. **少而正交**：35 条指令覆盖一切——比 Lua 5（38 条）还少。
   每多一条都要在 Compiler 和 VM 里分别加一份处理代码。
2. **常用情况快路径**：`PUSH_INT` 用 24-bit 立即数，避开常量池
   一次访问。
3. **"先做事后通用"**：`STORE_LOCAL_POP` 和 `STORE_LOCAL` 双
   变体看起来冗余，但运行时省一次 push/pop——值。
4. **跳转用相对偏移而非绝对地址**：函数 bytecode 可以独立移动、
   合并。这是 .NET / JVM 都用的。
5. **debug 行号是独立指令**：不污染普通指令的参数位；release
   构建直接不发射，零开销。

## 14.8 字节码不是机器码

最后强调一下：字节码 VM 仍是**解释器**——每条 opcode 在 VM 主
循环里被 fetch + decode + dispatch 后才执行。它不是 JIT 编译成机
器码。

JIT（Just-In-Time）做的事是：识别"热"函数，把它的字节码翻译成
真实 x86/ARM 指令，运行时直接跳进去执行。这能把开销再降 10~50
倍——LuaJIT、V8、HotSpot 都是这种。

写一个像样的 JIT 起码 5000 行汇编生成器代码，超出本书范围。我们
做完字节码 VM 就停——它已经能撑大多数游戏脚本和小型 DSL 的需求。

---

下一章 **第 15 章 Compiler** 我们正式写 visitor，把 AST 翻译成
上面这套 opcode 流。你会发现栈式 VM 的 Compiler 几乎是"机械翻
译"——visit 表达式时按"先求子节点，再发射运算符"的固定套路
走，几乎不需要思考。
