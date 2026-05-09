# 第 9 章　Evaluator：语句执行与控制流

表达式能算了，下一步是**语句**。本章把 `let`、`if`、`while`、
`for`、`return` 全部跑通。

写完本章后 Mini 已经能干这种事：

```python
let n = 0
let i = 1
while i <= 10 do
    n = n + i
    i = i + 1
end
print(n)        # 55
```

只剩函数没接通，那是下一章。

## 9.1 块作用域：什么时候 push / pop env

教科书上说"每进入一个块就 push 新作用域，离开时 pop"。但实际
取舍比这更微妙：

| 结构 | 是否独立作用域？ |
| --- | --- |
| `if ... then ... end` 的 then/else 体 | **是**（块内 let 出去看不见） |
| `while ... do ... end` 的循环体 | **是** |
| `for i in ... do ... end` 的循环体 | **是**，并且 `i` 也在这一层 |
| 函数体 | **是**，再加上独立的 env 链 |
| 顶层语句 | 共享同一个全局 env |

这意味着大多数控制流语句的 visitor 都长这样：

```cpp
EnvRef saved = current_env_;
current_env_ = saved->new_child();
try {
    for (auto& s : block) execute(*s);
} catch (...) {
    current_env_ = saved;
    throw;
}
current_env_ = saved;
```

每段都写五行 try/catch 太啰嗦——我们用 RAII 包一下。

## 9.2 RAII：`ScopedEnv`

```cpp
// src/interpreter.cpp 里的小 helper
class ScopedEnv {
public:
    ScopedEnv(EnvRef& slot, EnvRef new_env)
        : slot_(slot), prev_(slot) {
        slot_ = std::move(new_env);
    }
    ~ScopedEnv() { slot_ = std::move(prev_); }
private:
    EnvRef& slot_;
    EnvRef prev_;
};
```

用法：

```cpp
{
    ScopedEnv guard(current_env_, current_env_->new_child());
    for (auto& s : block) execute(*s);
}   // 这里自动复原 current_env_
```

异常或 return 跳出都不会泄漏作用域。这种"用栈对象保障状态恢复"
的小技巧在 C++ 里值千金——比 try/finally 干净，比手写 cleanup
不容易漏。

## 9.3 `execute` helper

跟 `evaluate` 配对的语句版：

```cpp
void Interpreter::execute(Stmt& s) {
    s.accept(*this);
}
```

只是个语义命名层——但写代码时 `execute(*stmt)` 比 `stmt->accept(*this)`
意图更清晰。

## 9.4 `LetStmt`：声明并初始化

```cpp
void Interpreter::visit(LetStmt& s) {
    Value v = evaluate(*s.init);
    current_env_->define(s.name, std::move(v));
}
```

注意我们故意**总是 define 当前 env**，不去看上层是否已有同名变量。
Mini 的语义：`let` 是当前作用域的声明，会**遮蔽**外层同名。

```python
let x = 1
if true then
    let x = 2     # 新的 x，不影响外层
    print(x)      # 2
end
print(x)          # 1
```

## 9.5 `ExprStmt`：求值，丢弃结果

```cpp
void Interpreter::visit(ExprStmt& s) {
    evaluate(*s.expr);
    // 结果被丢弃；副作用（赋值、调用）已经发生
}
```

REPL 模式下我们想看到结果，所以 REPL 的入口会**特殊处理顶层
ExprStmt**：直接 `print(eval(...))`。这一点放在第 12 章 REPL 那
节展开。

## 9.6 `IfStmt`：分支挑选

```cpp
void Interpreter::visit(IfStmt& s) {
    for (auto& br : s.branches) {
        if (evaluate(*br.cond).truthy()) {
            execute_block(br.body);
            return;
        }
    }
    if (!s.else_body.empty()) {
        execute_block(s.else_body);
    }
}

void Interpreter::execute_block(std::vector<StmtPtr>& body) {
    ScopedEnv guard(current_env_, current_env_->new_child());
    for (auto& s : body) execute(*s);
}
```

`branches` 是 if + 所有 elif 的并列数组（参考第 5 章设计）——visitor
只需简单线性扫一遍，第一个为真的就执行。

## 9.7 `WhileStmt`：循环

```cpp
void Interpreter::visit(WhileStmt& s) {
    while (evaluate(*s.cond).truthy()) {
        execute_block(s.body);
    }
}
```

每次迭代 `execute_block` 会 push 新作用域——意味着循环体里
`let x = ...` 每轮都是新的 x。这正是用户期望的。

## 9.8 `ForStmt`：半开区间

```python
for i in 0..10 do
    print(i)
end
```

打印 0~9。

```cpp
void Interpreter::visit(ForStmt& s) {
    Value start = evaluate(*s.start);
    Value end   = evaluate(*s.end);
    if (!start.is_int() || !end.is_int()) {
        throw RuntimeError("for range requires integers", s.line);
    }

    ScopedEnv guard(current_env_, current_env_->new_child());
    current_env_->define(s.var, Value(start.as_int()));

    for (std::int64_t i = start.as_int(); i < end.as_int(); ++i) {
        current_env_->assign(s.var, Value(i));
        // 循环体用一个再嵌套一层的子作用域，let 在循环体里每轮独立
        ScopedEnv inner(current_env_, current_env_->new_child());
        for (auto& st : s.body) execute(*st);
    }
}
```

设计选择：

* **循环变量在外层定义**：这样**循环结束后**还能用 `i`（如果你
  把 `for` 的外层作用域当成"循环外"）。我们这里因为 `ScopedEnv`
  在 visit 函数返回时复原，所以 `i` 在 for 之后就消失了——
  与 Lua / Rust 一致；
* **每次迭代再嵌一层**：让循环体里 `let x = 0` 每轮都重新声明，
  避免"上轮的 x 还在"这种诡异行为；
* **只支持整数 range**：`0..10` 半开。要支持步长 / float / 反向
  迭代，加新的 AST 节点形式（`for i in 0..10 step 2`），第一版不
  做。

## 9.9 `ReturnStmt`：用异常做非局部跳转

`return` 要做的事：**立刻从当前函数跳出去**。在树遍历解释器里，
"跳出去"意味着穿过任意层 if/while/for 直接到函数的 caller。

C++ 里"非局部跳转"最干净的做法是异常：

```cpp
struct ReturnSignal {
    Value value;
};

void Interpreter::visit(ReturnStmt& s) {
    Value v = s.value ? evaluate(*s.value) : Value();
    throw ReturnSignal{std::move(v)};
}
```

函数调用那边（第 10 章）会 `try { ... } catch (ReturnSignal& r) { ... }`
接住它。

为什么 `ReturnSignal` 不继承 `std::exception`？因为我们**不希望
它被当作错误**。把它做成裸 struct，只能被显式 catch，避开 `catch
(const std::exception&)` 这种泛接。

> **小贴士**：用异常做控制流通常被认为是反模式，但解释器是它合
> 法的少数场景之一。Lua 和 Python 的 C 实现都用 setjmp/longjmp
> 干同样的事——本质相同，都是非局部跳转。

`break` 与 `continue` 也用同样的模式：

```cpp
struct BreakSignal {};
struct ContinueSignal {};
```

我们这章先不实现 break/continue（Mini 暂时没有这关键字），第 24
章扩展时会顺便加上。

## 9.10 `FnStmt`：暂时占位

`fn foo() ... end` 完整实现在第 10 章。这里给个占位让代码能编过：

```cpp
void Interpreter::visit(FnStmt& s) {
    visit(*s.fn);                    // 创建 FunctionRef，落到 last_value_
    current_env_->define(s.name, std::move(last_value_));
}

void Interpreter::visit(FnExpr& f) {
    // 第 10 章实现
    last_value_ = Value();
}

void Interpreter::visit(CallExpr&) {
    throw RuntimeError("function calls not implemented yet", 0);
}
```

跑测试时只要别在脚本里写 `fn ... end`，就能用所有控制流。

## 9.11 `execute(Program& p)`

最后是顶层入口：

```cpp
Interpreter::Interpreter() {
    globals_ = std::make_shared<Environment>();
    current_env_ = globals_;
    // 注册内置函数（第 12 章）
    register_builtins();
}

void Interpreter::execute(Program& p) {
    for (auto& s : p.stmts) {
        try {
            execute(*s);
        } catch (const ReturnSignal&) {
            throw RuntimeError("'return' outside of function", s->line);
        }
    }
}
```

一段顶层 `return` 应该是错——我们在这里捕获 `ReturnSignal` 并报
错，不让它一路冒泡出去。

## 9.12 跑一段完整的程序

把 fib 用 while 写出来（递归 fib 等下一章再跑）：

```python
let a = 0
let b = 1
let i = 0
while i < 10 do
    let t = b
    b = a + b
    a = t
    i = i + 1
end
print(a)        # 55
```

调试输出：

```
$ ./mini examples/fib_iter.mini
55
```

我们写了大约 600 行 C++，已经有一门**完整带控制流的解释型动态
语言**。

## 9.13 一组语句执行的单测

```cpp
TEST(Stmt, LetAndArith) {
    auto out = run("let x = 1\nlet y = 2\nprint(x + y)\n");
    EXPECT_EQ(out, "3\n");
}

TEST(Stmt, IfElifElse) {
    auto out = run(
        "let x = 5\n"
        "if x < 0 then\n"
        "  print(\"neg\")\n"
        "elif x == 0 then\n"
        "  print(\"zero\")\n"
        "else\n"
        "  print(\"pos\")\n"
        "end\n");
    EXPECT_EQ(out, "pos\n");
}

TEST(Stmt, WhileSum) {
    auto out = run(
        "let s = 0\nlet i = 1\n"
        "while i <= 10 do\n  s = s + i\n  i = i + 1\nend\n"
        "print(s)\n");
    EXPECT_EQ(out, "55\n");
}

TEST(Stmt, ForRange) {
    auto out = run("for i in 0..3 do\n  print(i)\nend\n");
    EXPECT_EQ(out, "0\n1\n2\n");
}

TEST(Stmt, BlockShadowing) {
    auto out = run(
        "let x = 1\n"
        "if true then\n"
        "  let x = 2\n"
        "  print(x)\n"
        "end\n"
        "print(x)\n");
    EXPECT_EQ(out, "2\n1\n");
}

TEST(Stmt, AssignToUndefinedFails) {
    EXPECT_THROW(run("x = 1\n"), RuntimeError);
}

TEST(Stmt, ReturnAtTopLevelFails) {
    EXPECT_THROW(run("return 1\n"), RuntimeError);
}
```

`run(src)` 是一个"跑脚本，捕获 print 输出"的小测试 helper，做法
就是替换 `std::cout`（用 `std::stringstream`）然后调 interpreter。

## 9.14 控制流的几个常见坑

记下来你写完后大概率会遇到的几件事：

### 坑 1：循环变量被闭包"按引用捕获"了

下一章接通函数后，这段代码：

```python
let fns = []
for i in 0..3 do
    fns[i] = fn() return i end
end
print(fns[0]())   # 期望 0，实际？
```

Lua 5.0 / Python 2 / JavaScript var 全部踩过这个坑——`i` 是循环
变量，闭包捕获的是**变量本身**而不是当时的值，于是三个闭包都返
回循环结束后的 `i = 3`。

我们的设计**自动绕过了它**——因为 `for` 的循环体每轮都 push 新
作用域：每轮闭包捕获的是**新的 `i`**，所以打印 0、1、2 而不是
全部 3。这是第 9.8 节里"每次迭代再嵌一层"那个细节带来的好处，
不是巧合。

### 坑 2：`return nil` vs `return` 行为

```cpp
Value v = s.value ? evaluate(*s.value) : Value();
```

我们让 `return` 不带值时等价于 `return nil`。不区分两者，省得
parser 还要标记一个 `has_value` 字段。多数脚本语言都这么做。

### 坑 3：循环条件每次都求值

`while (evaluate(*s.cond).truthy())` 每轮都重新求值条件——这是
对的，但要确保你**没把 cond 缓存到第一次的值**。Visitor 里**永
远**重新 evaluate AST 节点是惯例。

---

下一章是这本书最有趣的一章之一——**第 10 章 函数与闭包**。我们
要把"函数"做成 Mini 的一等公民：能传参、能赋值、能从函数返回函
数、能闭包捕获外层变量。所有零件（env 链、ReturnSignal、CallExpr、
FnExpr）已经就位，把它们焊起来就行。
