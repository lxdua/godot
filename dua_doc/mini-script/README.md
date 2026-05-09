# 如何手写一个简单的脚本语言

—— 用 C++ 从零实现 **Mini**：一门动态类型、类 Python/Lua 的小型脚本语言

## 这本教程是写给谁的

* 你看过编译原理课本，但被龙书劝退过；
* 你想知道 GDScript / Lua / Python 解释器内部到底是怎么把
  `a = b + 1` 跑起来的；
* 你愿意写 1500~3000 行 C++，换一个能跑、能扩展、能自己玩的解释器。

教程不假设你写过编译器，但假设你能熟练使用 C++17：模板、智能指针、
`std::variant`、`std::unique_ptr`、`std::unordered_map`。

## 我们要造的东西：Mini 语言

```python
# fib.mini
fn fib(n)
    if n < 2 then
        return n
    end
    return fib(n - 1) + fib(n - 2)
end

let i = 0
while i < 10 do
    print(fib(i))
    i = i + 1
end
```

特性清单：

* 动态类型：`nil` / `bool` / `int` / `float` / `string` / `function` / `table`
* 表达式：算术、比较、逻辑、字符串拼接
* 语句：`let`、赋值、`if/elif/else`、`while`、`for`、`return`
* 函数：一等公民，支持闭包，支持递归
* 复合数据结构：`table`（兼用作数组与字典，仿 Lua）
* 内置函数：`print`、`len`、`type`、`assert`
* REPL 与文件运行两种模式

故意**不做**的东西（避免本书变成一本龙书）：

* 类与继承（练习题留给读者）
* 模块系统、import
* 协程、异常、await
* GC（先用 `shared_ptr` 引用计数兜底）

## 两条实现路径

```
源码  ──Lexer──▶  Token 流  ──Parser──▶  AST
                                          │
                          ┌───────────────┴────────────────┐
                          ▼                                ▼
                  Tree-Walking Interpreter        Bytecode Compiler
                  （第二部分：1~2 天跑通）         （第三部分：再花 2~3 天）
                          │                                │
                          ▼                                ▼
                       直接求值                         字节码 + 栈式 VM
```

## 目录

### 第一部分：前端（两种实现共用）

* 第 1 章 [总览：我们要做什么 & 项目骨架](01-overview.md)
* 第 2 章 [Lexer：把字符流切成 Token](02-lexer.md)
* 第 3 章 [Parser：递归下降构造 AST](03-parser.md)
* 第 4 章 [Pratt Parser：优雅地处理运算符优先级](04-pratt-parser.md)
* 第 5 章 [AST 节点的 C++ 表示](05-ast-nodes.md)

### 第二部分：树遍历解释器（最短路径跑通语言）

* 第 6 章 [Value 类型：用 std::variant 装一切](06-value.md)
* 第 7 章 [Environment：变量作用域链](07-environment.md)
* 第 8 章 [Evaluator：表达式求值](08-evaluator-expr.md)
* 第 9 章 [Evaluator：语句执行与控制流](09-evaluator-stmt.md)
* 第 10 章 [函数与闭包：第一等公民](10-functions-closures.md)
* 第 11 章 [Table：数组与字典合体](11-table.md)
* 第 12 章 [内置函数与 REPL](12-builtins-repl.md)

### 第三部分：字节码虚拟机（性能与工业感）

* 第 13 章 [为什么要换字节码 VM](13-why-bytecode.md)
* 第 14 章 [Opcode 设计：栈式 vs 寄存器式](14-opcode-design.md)
* 第 15 章 [Compiler：从 AST 到字节码](15-compiler.md)
* 第 16 章 [常量池与符号解析](16-constants-symbols.md)
* 第 17 章 [VM 主循环：dispatch 的几种写法](17-vm-loop.md)
* 第 18 章 [函数调用：Call Frame 与栈管理](18-call-frames.md)
* 第 19 章 [闭包的字节码实现：Upvalue](19-upvalues.md)
* 第 20 章 [反汇编器：让字节码可读](20-disassembler.md)

### 第四部分：把它做得像样

* 第 21 章 [Native 函数与 FFI](21-native-ffi.md)
* 第 22 章 [基础 GC：从引用计数到三色标记](22-gc.md)
* 第 23 章 [错误诊断与恢复](23-diagnostics.md)
* 第 24 章 [LSP 服务器：接入 VSCode](24-lsp.md)
* 第 25 章 [性能：你能做的几件小事](25-performance.md)
* 第 26 章 [继续走下去：练习题与扩展方向](26-next-steps.md)

### 附录

* 附录 A [完整 Mini 语法 BNF](appendix-a-grammar.md)
* 附录 B [Opcode 速查表](appendix-b-opcodes.md)
* 附录 C [推荐阅读](appendix-c-reading.md)
