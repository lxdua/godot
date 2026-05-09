# 第 10 章　函数与闭包：第一等公民

这一章我们把函数变成 Mini 的**一等公民**——能赋值给变量、能作
为参数传递、能从函数返回、能在函数体内创建并捕获外层环境。

写完后这种代码就能跑：

```python
fn make_adder(n)
    fn adder(x)
        return x + n      # 闭包：n 是 make_adder 的局部
    end
    return adder
end

let add5 = make_adder(5)
print(add5(10))     # 15
print(add5(20))     # 25
```

闭包是一切现代脚本语言的基石。前面 9 章铺垫的 env 链、`ReturnSignal`、
`CallExpr` 此刻全部派上用场。

## 10.1 `Function` 类的设计

我们要装两类函数：

* **脚本函数**：用户写的 `fn ... end`，由参数列表 + AST body +
  捕获环境组成；
* **原生函数**：C++ 写的内置函数（`print`、`len` ...），用一个
  `std::function` 包起来。

```cpp
// src/function.h
#pragma once
#include "ast.h"
#include "env.h"
#include "value.h"
#include <functional>
#include <vector>

namespace mini {

class Function {
public:
    // 类型 1：脚本函数
    Function(std::vector<std::string> params,
             std::vector<StmtPtr>* body,    // 借用，不拥有
             EnvRef captured)
        : params_(std::move(params)),
          body_(body),
          captured_env_(std::move(captured)),
          arity_(static_cast<int>(params_.size())),
          native_(nullptr) {}

    // 类型 2：原生函数
    using NativeFn = std::function<Value(std::vector<Value>&)>;
    Function(NativeFn fn, int arity = -1)
        : arity_(arity), native_(std::move(fn)) {}

    int arity() const { return arity_; }
    bool is_native() const { return static_cast<bool>(native_); }

    const NativeFn& native() const { return native_; }
    const std::vector<std::string>& params() const { return params_; }
    std::vector<StmtPtr>* body() const { return body_; }
    const EnvRef& captured_env() const { return captured_env_; }

private:
    std::vector<std::string> params_;
    std::vector<StmtPtr>* body_ = nullptr;   // 指向 AST 的 body 数组
    EnvRef captured_env_;                    // 闭包环境
    int arity_;                              // -1 表示可变参数
    NativeFn native_;                        // 非空表示原生
};

}  // namespace mini
```

几个值得展开的设计：

### 1. `body_` 是裸指针，不是拥有

函数对象的 body **不拥有 AST**。AST 的所有权在 `Program` 里，
Interpreter 持有它直到运行结束。函数只是借用一份指针。

代价是：**Program 必须在所有 Function 死之前活着**。我们的
Interpreter 持有 Program，二者同生命周期，没问题。

如果将来要让函数能在多个解释器实例间传递，或者要支持"运行时
unload AST"，就得把 body 拷贝/拥有进 Function。但那是另一个量
级的工程（GDScript 走的是字节码方案，AST 直接丢弃）。

### 2. `captured_env_` 是 `EnvRef` 而不是 `Environment*`

闭包的关键。`EnvRef` 是 shared_ptr，意味着**只要这个函数还活着，
它捕获的 env 就活着**——包括已经"出栈"的外层函数的 env。这正
是闭包能记住外层变量的原因。

### 3. 原生函数用 `std::function`

```cpp
using NativeFn = std::function<Value(std::vector<Value>&)>;
```

签名故意简单——接收参数数组，返回单个值。错误用 `throw RuntimeError`
传播。第 12 章注册内置函数时会反复用到。

### 4. `arity` 与 `-1` 哨兵

* `arity = N`：必须传 N 个参数；
* `arity = -1`：可变参数（C++ 端自己检查）。

在调用时检查：

```cpp
if (fn->arity() >= 0 && static_cast<int>(args.size()) != fn->arity()) {
    throw RuntimeError("expected " + std::to_string(fn->arity())
                       + " args, got " + std::to_string(args.size()),
                       line);
}
```

## 10.2 `FnExpr`：构造函数对象

```cpp
void Interpreter::visit(FnExpr& f) {
    auto fn = std::make_shared<Function>(
        f.params,           // 参数名
        &f.body,            // body 指针（借用 AST）
        current_env_        // 捕获**当前**作用域
    );
    last_value_ = Value(FunctionRef(std::move(fn)));
}
```

**关键的一行**：`current_env_` 是函数**声明时**的作用域链。函数
不管被传到哪里调用，它看到的"自由变量"永远是这条链上的——这就
是词法作用域。

## 10.3 `FnStmt`：命名函数 = let + FnExpr

```cpp
void Interpreter::visit(FnStmt& s) {
    visit(*s.fn);                        // 走 FnExpr 路径，把 Function 放到 last_value_
    Value f = std::move(last_value_);
    current_env_->define(s.name, std::move(f));
}
```

但这里有一个**很隐蔽的 bug**：递归函数。

```python
fn fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end
```

按上面写法：

1. `visit(FnExpr)` 时 `current_env_` **不包含 `fib`**；
2. 创建 Function 对象，captured_env 指向不含 fib 的 env；
3. 然后才 `define("fib", fn)`；
4. fib 内部 `fib(n-1)` 找不到 fib——爆炸。

修法是**在求值 init 之前先 define**：

```cpp
void Interpreter::visit(FnStmt& s) {
    // 先占一个空位，让 FnExpr 创建的函数能看到自己
    current_env_->define(s.name, Value());
    visit(*s.fn);
    current_env_->assign(s.name, std::move(last_value_));
}
```

这样 captured_env 里**已经有名字 `fib`**，只是值是 nil；后续
assign 后 fib 就指向真正的函数。下次调用 `fib(n-1)` 沿 env 链查
找时，会找到正确的函数引用。

> **小知识**：这个"前向声明"问题在所有动态语言里都存在。Python
> 用了不同方案——它**调用时**才查找全局 fib，每次都是 late binding，
> 所以哪怕函数体写在自己被定义之前，只要调用之前 define 了就行。
> JS 的 `function foo() {}` 走 hoisting。我们走最直白的"先 define
> 占位"方案，省得讨论细节。

## 10.4 `CallExpr`：调用

```cpp
void Interpreter::visit(CallExpr& c) {
    Value callee = evaluate(*c.callee);
    if (!callee.is_function()) {
        throw RuntimeError(
            std::string("can only call functions, got ")
            + callee.type_name(), c.line);
    }
    std::vector<Value> args;
    args.reserve(c.args.size());
    for (auto& a : c.args) {
        args.push_back(evaluate(*a));
    }

    last_value_ = call(callee.as_function(), args, c.line);
}

Value Interpreter::call(const FunctionRef& fn,
                        std::vector<Value>& args,
                        int line) {
    // 参数个数检查
    if (fn->arity() >= 0
        && static_cast<int>(args.size()) != fn->arity()) {
        throw RuntimeError(
            "expected " + std::to_string(fn->arity())
            + " args, got " + std::to_string(args.size()),
            line);
    }

    // 原生函数：直接转发
    if (fn->is_native()) {
        return fn->native()(args);
    }

    // 脚本函数：构造调用 env，绑定参数
    auto call_env = std::make_shared<Environment>(fn->captured_env());
    const auto& params = fn->params();
    for (std::size_t i = 0; i < params.size(); ++i) {
        call_env->define(params[i], std::move(args[i]));
    }

    // 切换 env，执行 body
    EnvRef saved = current_env_;
    current_env_ = std::move(call_env);

    Value result;
    try {
        for (auto& s : *fn->body()) {
            execute(*s);
        }
    } catch (ReturnSignal& r) {
        result = std::move(r.value);
    }

    current_env_ = std::move(saved);
    return result;
}
```

整段代码做了这些事：

1. 检查 arity；
2. 原生函数直接调；
3. 脚本函数：在**捕获 env** 之上 push 一个新 env 作为调用 env；
4. 把参数绑到调用 env；
5. 临时切换 `current_env_` 到调用 env；
6. 执行 body；
7. 用 `try/catch` 接住 `ReturnSignal`；
8. 复原 `current_env_`，返回结果。

注意**第 3 步**的 parent：`fn->captured_env()`，**不是**
`current_env_`！这是词法作用域的核心——调用环境的 parent 是
"声明时"的环境，而不是"调用时"的环境。

如果你写错成 `parent = current_env_`，你就**意外实现了动态作用
域**——函数能看到 caller 的局部变量。这种语言（早期 Lisp、bash）
几十年前被发现是 bug 大坑，已经被淘汰了。

## 10.5 闭包跑通了：手动追一遍

```python
fn make_counter()
    let n = 0
    fn inc()
        n = n + 1
        return n
    end
    return inc
end

let c = make_counter()
print(c())   # 1
print(c())   # 2
```

跟着代码走一遍：

1. `make_counter` 在全局 env 里被 define；
2. 调用 `make_counter()`：
   * 创建 `call_env_outer`，parent = make_counter 的 captured_env =
     全局 env；
   * 在 call_env_outer 里 `let n = 0`；
   * 求值 `fn inc() ... end`：创建 Function `inc`，captured_env =
     **call_env_outer**；
   * `return inc` → 返回这个 Function；
3. `c = make_counter()` 后：
   * `c` 是个 Function，captured_env 指向 call_env_outer；
   * call_env_outer 里有 `n = 0`；
   * **call_env_outer 仍活着**——因为 `c.captured_env_` 这个
     shared_ptr 持着它；
4. `c()`：
   * 创建 `call_env_inc`，parent = c.captured_env_ = call_env_outer；
   * 执行 body：
     * `n = n + 1`：assign 沿 env 链查找 n。call_env_inc 没有，找
       到 call_env_outer 的 n，写它（n = 1）；
     * `return n` → 返回 1；
5. `c()` 第二次：
   * 同样进入 call_env_outer，n 现在是 1；
   * 写完变 2，返回 2。

环境链的形状（第二次调用时）：

```
call_env_inc ──parent──▶ call_env_outer ──parent──▶ globals
   {}                        {n: 1→2}                {make_counter, c, ...}
```

整个闭包机制就是 env 链 + shared_ptr 引用计数的**自然产物**——
我们没写一行专门处理闭包的代码。

## 10.6 一个常见的反直觉：捕获时机

```python
let funcs = [nil, nil, nil]
let i = 0
while i < 3 do
    let captured = i
    funcs[i] = fn() return captured end
    i = i + 1
end

print(funcs[0]())   # 0
print(funcs[1]())   # 1
print(funcs[2]())   # 2
```

这里我们在循环体里手动 `let captured = i` 制造每轮都新的局部，
让闭包捕获到独立的 captured。每轮 `let captured` 都生成一个新的
绑定，三个闭包捕获的 env 各自不同。

如果直接用 `i`：

```python
while i < 3 do
    funcs[i] = fn() return i end
    i = i + 1
end
```

由于 `i` 是 while 外层的同一个变量，三个闭包**共享同一个 env**，
都返回最终的 3。这是**正确**的 lexical capture 行为，但与新手
的直觉相反——所有动态语言都踩过这个坑。

但 `for i in 0..N do` 不会有这个问题，因为我们在第 9.8 节让
`for` 每轮 push 新作用域，`i` 在每轮里都是不同的绑定。这是个
**故意**的小取舍，让 for 的常用形式不踩坑。

## 10.7 性能：每次调用都 alloc 一个 env

我们的实现里每次函数调用都 `std::make_shared<Environment>` 一次，
分配一个 unordered_map。fib(30) 大概要做 230 万次分配，慢得肉眼
可见。

加速路径：

* **作用域复用**：同一个函数的连续调用如果上一次已经退栈，可以
  复用上次的 env（清空 vars_ 即可）；但这破坏了闭包语义；
* **栈帧化**：把 env 改成 `vector<Value>`，参数和局部都用下标
  访问；这就是 Lua / 字节码 VM 的做法；
* **预分配**：把 unordered_map 换成 SmallMap（< 8 项时用 vector，
  超过转 hash）。

我们**全部留到第三部分**字节码 VM 时一次性解决。教学版本现在能
跑 fib(20) 几百毫秒就够了。

## 10.8 一组函数与闭包的单测

```cpp
TEST(Func, BasicCall) {
    auto out = run(
        "fn add(a, b)\n  return a + b\nend\n"
        "print(add(1, 2))\n");
    EXPECT_EQ(out, "3\n");
}

TEST(Func, Recursion) {
    auto out = run(
        "fn fib(n)\n"
        "  if n < 2 then return n end\n"
        "  return fib(n - 1) + fib(n - 2)\n"
        "end\n"
        "print(fib(10))\n");
    EXPECT_EQ(out, "55\n");
}

TEST(Func, Closure) {
    auto out = run(
        "fn make_adder(n)\n"
        "  fn adder(x) return x + n end\n"
        "  return adder\n"
        "end\n"
        "let add5 = make_adder(5)\n"
        "print(add5(10))\n"
        "print(add5(20))\n");
    EXPECT_EQ(out, "15\n25\n");
}

TEST(Func, CounterRetainsState) {
    auto out = run(
        "fn make_counter()\n"
        "  let n = 0\n"
        "  fn inc()\n"
        "    n = n + 1\n"
        "    return n\n"
        "  end\n"
        "  return inc\n"
        "end\n"
        "let c = make_counter()\n"
        "print(c())\n"
        "print(c())\n"
        "print(c())\n");
    EXPECT_EQ(out, "1\n2\n3\n");
}

TEST(Func, FirstClass_PassedAsArg) {
    auto out = run(
        "fn apply(f, x) return f(x) end\n"
        "fn double(x) return x * 2 end\n"
        "print(apply(double, 21))\n");
    EXPECT_EQ(out, "42\n");
}

TEST(Func, ArityMismatch) {
    EXPECT_THROW(run(
        "fn f(a, b) return a + b end\n"
        "f(1)\n"), RuntimeError);
}

TEST(Func, LexicalNotDynamic) {
    auto out = run(
        "let x = 10\n"
        "fn f() return x end\n"
        "fn g()\n"
        "  let x = 20\n"
        "  return f()\n"
        "end\n"
        "print(g())\n");
    EXPECT_EQ(out, "10\n");   // 词法作用域 → 10，不是 20
}
```

最后一个测试是最重要的——它**固化"我们是词法作用域"这个语义决
策**。一旦有人改了 call() 里 parent 那一行，这个测试会立刻 fail。

## 10.9 现状回顾

到这一章为止，Mini 已经是一门**完整的、能写真实程序的语言**：

* 词法、语法、AST、env 链、求值器、控制流、函数、闭包；
* 错误带行号；
* REPL（虽然简陋）和文件运行；
* 大概 1500 行 C++。

剩下的就是把数据结构（Table）和内置函数补上，让它真正"实用"。

---

下一章 **第 11 章 Table** 我们仿 Lua 把数组和字典合二为一——
`[1, 2, 3]` 和 `{"a": 1}` 都是同一个 Table 类型。这种"一个数据
结构装一切"的设计是 Lua 的精髓，也是教学项目里**最优雅**的复合
类型方案。
