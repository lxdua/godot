# 第 12 章　内置函数与 REPL

第二部分的最后一章。我们把内置函数（`print`、`len`、`type`、
`assert`、`to_string`、`push`、`pop`、`keys`...）一股脑注册
进去，并把 REPL 做得稍微像样一些——多行续行、表达式回显、
错误恢复。

完成后我们就有一门能日常使用的 Mini 解释器，并准备好进入第三
部分（字节码 VM）。

## 12.1 注册框架

第 10 章我们已经设计了 `Function` 的原生分支，第 11 章用 `len`
试了一次。这章把所有 builtin 集中注册：

```cpp
// src/builtins.h
#pragma once
#include "value.h"
#include "env.h"

namespace mini {
void register_builtins(Environment& global);
}
```

```cpp
// src/builtins.cpp
#include "builtins.h"
#include "function.h"
#include "table.h"
#include <iostream>
#include <sstream>

namespace mini {
namespace {

Value make_native(Function::NativeFn fn, int arity = -1) {
    return Value(std::make_shared<Function>(std::move(fn), arity));
}

// ============= 内置函数实现 =============

Value bi_print(std::vector<Value>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i) std::cout << '\t';
        std::cout << args[i];
    }
    std::cout << '\n';
    return Value();
}

Value bi_len(std::vector<Value>& args) {
    if (args.size() != 1)
        throw std::runtime_error("len() takes 1 argument");
    const Value& v = args[0];
    if (v.is_string()) {
        return Value(static_cast<std::int64_t>(v.as_string().size()));
    }
    if (v.is_table()) {
        return Value(static_cast<std::int64_t>(v.as_table()->size()));
    }
    throw std::runtime_error("len() expects string or table");
}

Value bi_type(std::vector<Value>& args) {
    if (args.size() != 1)
        throw std::runtime_error("type() takes 1 argument");
    return Value(std::string(args[0].type_name()));
}

Value bi_to_string(std::vector<Value>& args) {
    if (args.size() != 1)
        throw std::runtime_error("to_string() takes 1 argument");
    return Value(args[0].to_string());
}

Value bi_to_int(std::vector<Value>& args) {
    if (args.size() != 1)
        throw std::runtime_error("to_int() takes 1 argument");
    const Value& v = args[0];
    if (v.is_int())   return v;
    if (v.is_float()) return Value(static_cast<std::int64_t>(v.as_float()));
    if (v.is_string()) {
        try {
            return Value(static_cast<std::int64_t>(std::stoll(v.as_string())));
        } catch (...) {
            throw std::runtime_error("to_int(): not a number string");
        }
    }
    if (v.is_bool()) return Value(static_cast<std::int64_t>(v.as_bool() ? 1 : 0));
    throw std::runtime_error(std::string("cannot convert ")
        + v.type_name() + " to int");
}

Value bi_to_float(std::vector<Value>& args) {
    if (args.size() != 1)
        throw std::runtime_error("to_float() takes 1 argument");
    const Value& v = args[0];
    if (v.is_int())   return Value(static_cast<double>(v.as_int()));
    if (v.is_float()) return v;
    if (v.is_string()) {
        try {
            return Value(std::stod(v.as_string()));
        } catch (...) {
            throw std::runtime_error("to_float(): not a number string");
        }
    }
    throw std::runtime_error(std::string("cannot convert ")
        + v.type_name() + " to float");
}

Value bi_assert(std::vector<Value>& args) {
    if (args.empty())
        throw std::runtime_error("assert() takes at least 1 argument");
    if (!args[0].truthy()) {
        std::string msg = args.size() >= 2 ? args[1].to_string()
                                            : std::string("assertion failed");
        throw std::runtime_error(msg);
    }
    return args[0];   // 与 Lua 一致：返回断言对象
}

// table 相关
Value bi_push(std::vector<Value>& args) {
    if (args.size() != 2)
        throw std::runtime_error("push(table, value) takes 2 args");
    if (!args[0].is_table())
        throw std::runtime_error("push() expects table as first arg");
    auto& t = *args[0].as_table();
    t.set(Value(static_cast<std::int64_t>(t.array_size())), args[1]);
    return Value();
}

Value bi_pop(std::vector<Value>& args) {
    if (args.size() != 1)
        throw std::runtime_error("pop(table) takes 1 arg");
    if (!args[0].is_table())
        throw std::runtime_error("pop() expects table");
    auto& t = *args[0].as_table();
    if (t.array_size() == 0) return Value();
    Value v = t.at(t.array_size() - 1);
    t.set(Value(static_cast<std::int64_t>(t.array_size() - 1)), Value());
    return v;
}

Value bi_keys(std::vector<Value>& args) {
    if (args.size() != 1 || !args[0].is_table())
        throw std::runtime_error("keys(table) takes 1 table arg");
    // 返回一个新 table 装所有 key
    auto out = std::make_shared<Table>();
    auto& t = *args[0].as_table();
    std::int64_t idx = 0;
    // 数组部分：键是 0..n-1
    for (std::size_t i = 0; i < t.array_size(); ++i) {
        if (!t.at(i).is_nil()) {
            out->set(Value(idx++), Value(static_cast<std::int64_t>(i)));
        }
    }
    // hash 部分：略（教学版偷懒，因为我们还没暴露 hash 的 iterator）
    return Value(TableRef(std::move(out)));
}

// 时间 / I/O
Value bi_clock(std::vector<Value>& args) {
    if (!args.empty())
        throw std::runtime_error("clock() takes no args");
    using namespace std::chrono;
    return Value(duration<double>(
        steady_clock::now().time_since_epoch()).count());
}

Value bi_input(std::vector<Value>& args) {
    if (args.size() > 1)
        throw std::runtime_error("input([prompt]) takes 0 or 1 arg");
    if (args.size() == 1) std::cout << args[0];
    std::string s;
    if (!std::getline(std::cin, s)) return Value();
    return Value(std::move(s));
}

}  // namespace

void register_builtins(Environment& g) {
    g.define("print",     make_native(bi_print));
    g.define("len",       make_native(bi_len, 1));
    g.define("type",      make_native(bi_type, 1));
    g.define("to_string", make_native(bi_to_string, 1));
    g.define("to_int",    make_native(bi_to_int, 1));
    g.define("to_float",  make_native(bi_to_float, 1));
    g.define("assert",    make_native(bi_assert));
    g.define("push",      make_native(bi_push, 2));
    g.define("pop",       make_native(bi_pop, 1));
    g.define("keys",      make_native(bi_keys, 1));
    g.define("clock",     make_native(bi_clock, 0));
    g.define("input",     make_native(bi_input));
}

}  // namespace mini
```

设计上的几个小习惯：

* **每个 builtin 自己检查 arity**（即使注册时也声明了 arity，
  也建议在函数体里再 check 一次——如果你注册时 arity 写错了，运
  行时仍能给出清晰错误）；
* **错误用 `std::runtime_error`**：Function 的 native 调用会被 VM
  / Interpreter 包成 `RuntimeError + line`；
* **空 namespace 包住所有 `bi_*`**：避免污染，编译器还能内联化
  它们。

## 12.2 把它接进 Interpreter

```cpp
// src/interpreter.cpp
Interpreter::Interpreter() {
    globals_ = std::make_shared<Environment>();
    current_env_ = globals_;
    register_builtins(*globals_);
}
```

一行连接搞定。注意我们传 `*globals_` 是引用而不是 shared_ptr——
避免循环引用（builtin 不需要持有 env 的 shared_ptr，它们是无状态
的纯函数）。

## 12.3 一个有趣的边界：能否覆盖内置

```python
let print = fn(x) end    # 把 print 覆盖成空操作
print("hi")              # 不输出
```

我们故意允许。这是 Lua / JS 的传统。带来的副作用：用户写
`let print = ...` 时**不会得到任何警告**。

如果你想做"内置变量受保护"，可以让 `register_builtins` 标记这些
名字为 readonly，然后 env 在 assign / define 时拒绝。但教学版本
保持开放——简单且符合传统。

## 12.4 REPL 的"像样化"

我们之前的 REPL（第 1 章）只是个最小循环。现在做几个改进：

1. **多行续行**：用户写 `if ... then` 没敲 `end` 时，提示符变
   `...`，继续读直到能成功 parse；
2. **表达式回显**：用户在 REPL 里输入 `1 + 2`，我们打印 `3`；
3. **保留全局 env**：每条命令在同一个 env 里执行，下一条能看到
   上一条 define 的变量；
4. **错误不退出**：解析或求值出错只打印错误，REPL 继续。

```cpp
// src/main.cpp
static int run_repl() {
    mini::Interpreter ip;
    std::string buffer;
    std::string line;
    bool continuation = false;

    std::cout << "Mini REPL. Ctrl+D to exit.\n";
    while (true) {
        std::cout << (continuation ? "... " : ">>> ");
        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            return 0;
        }
        if (!buffer.empty()) buffer.push_back('\n');
        buffer.append(line);

        // 尝试解析；如果是"未闭合的块"错误，进入续行模式
        auto tokens = mini::Lexer(buffer).tokenize();
        mini::Parser parser(tokens);
        auto prog = parser.parse_program();
        if (parser.has_errors()) {
            // 简化判断：错误消息含 "expected 'end'" / "unterminated"
            // 就当作续行；否则报错重置
            const auto& err = parser.errors().front();
            std::string msg = err.what();
            bool wants_more =
                msg.find("end of block") != std::string::npos ||
                msg.find("unterminated") != std::string::npos ||
                msg.find("expected expression") != std::string::npos;
            if (wants_more) {
                continuation = true;
                continue;
            }
            for (const auto& e : parser.errors()) {
                std::cerr << "parse error " << e.line << ":" << e.column
                          << ": " << e.what() << "\n";
            }
            buffer.clear();
            continuation = false;
            continue;
        }

        // 解析成功，跑
        try {
            // 顶层若是单个表达式，回显结果
            if (prog->stmts.size() == 1) {
                if (auto* es = dynamic_cast<mini::ExprStmt*>(
                        prog->stmts[0].get())) {
                    mini::Value v = ip.eval(*es->expr);
                    if (!v.is_nil()) std::cout << v << '\n';
                    buffer.clear();
                    continuation = false;
                    continue;
                }
            }
            ip.execute(*prog);
        } catch (const mini::RuntimeError& e) {
            std::cerr << "runtime error " << e.line << ": "
                      << e.what() << "\n";
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
        }
        buffer.clear();
        continuation = false;
    }
}
```

设计要点：

* **续行的判断很 naive**——靠错误消息字符串匹配。生产实现会用
  parser 显式吐一个"unexpected EOF, expected `end`"标志位。本
  书第一版凑合着用，第 21 章会做正经的；
* **表达式回显跟 Python REPL 一致**：用户输入裸表达式直接看到
  值，但 `let x = 1` 不打印；
* **错误捕获分两层**：`RuntimeError` 含行号，其它 `std::exception`
  做兜底。

## 12.5 examples/ 里的几个完整脚本

第 1 章我们说每写完一个特性就往 examples 里加东西。到这章结束，
应该有：

`examples/fib.mini`：

```python
fn fib(n)
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end

for i in 0..10 do
    print(fib(i))
end
```

`examples/closure.mini`：

```python
fn make_counter(start)
    let n = start
    return {
        "next":  fn() n = n + 1; return n end,   # 注：; 在 Mini 不合法，下面改写
        "value": fn() return n end,
        "reset": fn() n = start end,
    }
end
```

哦，等等——上面 `n = n + 1; return n` 用了分号。Mini 没有分号
（语句终止符是 NEWLINE）。改写：

```python
fn make_counter(start)
    let n = start
    fn next()
        n = n + 1
        return n
    end
    fn value() return n end
    fn reset() n = start end
    return {"next": next, "value": value, "reset": reset}
end

let c = make_counter(10)
print(c["next"]())     # 11
print(c["next"]())     # 12
print(c["value"]())    # 12
c["reset"]()
print(c["value"]())    # 10
```

`examples/fizzbuzz.mini`：

```python
for i in 1..16 do
    if i % 15 == 0 then
        print("FizzBuzz")
    elif i % 3 == 0 then
        print("Fizz")
    elif i % 5 == 0 then
        print("Buzz")
    else
        print(i)
    end
end
```

`examples/wordcount.mini`：

```python
fn count_words(text)
    let counts = {}
    let i = 0
    let cur = ""
    while i < len(text) do
        let ch = text[i]
        if ch == " " or ch == "\n" or ch == "\t" then
            if len(cur) > 0 then
                let n = counts[cur]
                if n == nil then n = 0 end
                counts[cur] = n + 1
                cur = ""
            end
        else
            cur = cur .. ch
        end
        i = i + 1
    end
    if len(cur) > 0 then
        let n = counts[cur]
        if n == nil then n = 0 end
        counts[cur] = n + 1
    end
    return counts
end

let text = "the quick brown fox jumps over the lazy dog the fox"
let c = count_words(text)
print(c["the"])    # 3
print(c["fox"])    # 2
```

跑一下 wordcount.mini，你写的整门语言里没有任何"作弊"——一切
都从 Lexer / Parser / 求值器走过——感觉非常奇妙。

## 12.6 一些 builtin 的常见疑问

### 为什么不实现 `range(start, end, step)`？

因为 `for i in s..e do` 语法已经覆盖了最常用情况。`range` 作为
内置函数会返回一个 table——多余、慢，不如把 step 作为 for 的
扩展语法（练习题）。

### 为什么 `assert` 抛 `runtime_error` 而不是 `RuntimeError`？

因为 `RuntimeError` 在 `interpreter.h` 里，builtin 模块不该依赖
Interpreter 内部。`runtime_error` 在 Interpreter 调用 native 时
会被 catch 并加上行号——这是个职责分层的小细节。

### 怎么加文件 I/O？

```cpp
Value bi_read_file(std::vector<Value>& args) {
    if (args.size() != 1 || !args[0].is_string())
        throw std::runtime_error("read_file(path) takes 1 string");
    std::ifstream in(args[0].as_string());
    if (!in) throw std::runtime_error("cannot open " + args[0].as_string());
    std::stringstream ss;
    ss << in.rdbuf();
    return Value(ss.str());
}

g.define("read_file", make_native(bi_read_file, 1));
```

加一行就行。这就是把 builtin 设计成 "C++ 函数 + 注册"的好处：
扩展是无痛的。

## 12.7 第二部分回顾

到这章结束，我们的 Mini 解释器已经完成。规模：

* `lexer.{h,cpp}`：~200 行
* `parser.{h,cpp}`：~450 行（含 Pratt 表）
* `ast.{h,cpp}`：~150 行
* `value.{h,cpp}`：~200 行
* `env.h`：~70 行
* `function.h`：~50 行
* `table.{h,cpp}`：~120 行
* `interpreter.{h,cpp}`：~400 行
* `builtins.{h,cpp}`：~200 行
* `main.cpp`：~100 行
* **总计：约 1900 行 C++**

它能做什么：

* 完整词法 + 语法 + 错误恢复；
* 动态类型 + 7 种内置类型；
* 词法作用域 + 闭包 + 递归；
* 一等公民函数；
* 数组/字典统一的 Table；
* REPL 与文件运行；
* 行号级别的错误信息；
* 一组覆盖所有特性的单元测试。

性能：fib(30) 大概 200~500 ms（取决于编译器）。比 CPython 慢 3~5
倍，比 Lua 慢 20~50 倍——树遍历的天花板就是这样。

要更快就得换字节码——这是第三部分的事。

## 12.8 一个调试小技巧：用 REPL 当 inspector

写完 REPL 后你会发现它是**调试 Mini 自身实现的最好工具**：

```
$ ./mini
>>> let f = fn(x) return x + 1 end
>>> type(f)
function
>>> f(41)
42
>>> let t = [10, 20, 30]
>>> t[1] = 999
>>> t
<table>
>>> len(t)
3
```

每次给 Lexer / Parser / 求值器加新功能后，第一时间在 REPL 里手
试一下，比写单测快得多。做完一遍手试再补单测。

---

到这里 **第二部分（树遍历解释器）** 全部完成。你已经亲手写了一
门完整、可用的脚本语言。

第三部分我们要把这一切**重构**成字节码 VM。你会发现：

* Lexer / Parser / AST：**完全复用**，一行不改；
* `Value` / `Table` / `Function`：基本不变，只在 Function 内部加
  一个 `bytecode` 字段；
* `Environment` 的概念被 **栈帧 + 槽位下标** 取代，运行时性能提
  升 10~50 倍；
* Interpreter 被拆成 `Compiler`（AST → 字节码）+ `VM`（执行字节
  码）两部分；
* 闭包从"shared_ptr 持有 env"演进成"upvalue 引用栈槽"——**Lua
  的核心算法**，下一章详细讲。

下一章：**第 13 章 为什么要换字节码 VM**。
