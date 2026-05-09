# 第 8 章　Evaluator：表达式求值

到了把所有零件拼起来的时候。这一章我们写一个 `Interpreter` 类，
它是个 `AstVisitor`——visit 每个表达式节点时，把求值结果"放在
一个内部寄存器里"，让上层节点取走。等本章结束时，Mini 就能跑这
样的代码：

```python
1 + 2 * 3            # 表达式求值
"hello" .. " world"  # 字符串拼接
not (1 < 2)          # 比较 + 布尔
a and b or c         # 短路逻辑
```

但还跑不了 `let / if / while / fn` ——那些是下一章的事。

## 8.1 Interpreter 的状态

`src/interpreter.h`：

```cpp
#pragma once
#include "ast.h"
#include "env.h"
#include "value.h"
#include <stdexcept>

namespace mini {

struct RuntimeError : public std::runtime_error {
    int line;
    RuntimeError(const std::string& msg, int l)
        : std::runtime_error(msg), line(l) {}
};

class Interpreter : public AstVisitor {
public:
    Interpreter();

    // 跑整个程序
    void execute(Program& p);

    // 求一个表达式的值（REPL 用）
    Value eval(Expr& e);

    EnvRef global_env() const { return globals_; }

private:
    EnvRef globals_;
    EnvRef current_env_;     // 当前求值所在的作用域

    // visitor 的"返回值"靠这个寄存器传出
    Value last_value_;

    // helper：求值一个表达式并返回结果
    Value evaluate(Expr& e);

    // ===== 表达式 visitor（本章）=====
    void visit(NumberLit&)  override;
    void visit(StringLit&)  override;
    void visit(BoolLit&)    override;
    void visit(NilLit&)     override;
    void visit(IdentExpr&)  override;
    void visit(BinaryOp&)   override;
    void visit(UnaryOp&)    override;
    void visit(LogicalOp&)  override;
    void visit(AssignExpr&) override;
    void visit(CallExpr&)   override;        // 第 10 章
    void visit(IndexExpr&)  override;        // 第 11 章
    void visit(ArrayLit&)   override;        // 第 11 章
    void visit(TableLit&)   override;        // 第 11 章
    void visit(FnExpr&)     override;        // 第 10 章

    // ===== 语句 visitor（下一章）=====
    void visit(LetStmt&)    override;
    void visit(ExprStmt&)   override;
    void visit(ReturnStmt&) override;
    void visit(IfStmt&)     override;
    void visit(WhileStmt&)  override;
    void visit(ForStmt&)    override;
    void visit(FnStmt&)     override;
};

}  // namespace mini
```

几个核心模式：

* **`last_value_` 寄存器**：visitor 没返回值，所以求完表达式把
  结果放在这里，调用方从 `evaluate()` 取出来；
* **`current_env_` 是个普通成员**：进入新作用域换它，离开时复原；
* **`RuntimeError` 携带行号**：用户的错误信息看到的是"第 5 行：
  expected number"。

## 8.2 `evaluate`：visitor 的"取出返回值"封装

```cpp
Value Interpreter::evaluate(Expr& e) {
    e.accept(*this);
    return std::move(last_value_);
}
```

仅此而已。整个解释器内部所有"我要这个表达式的值"都通过 evaluate
间接调用，永远不直接 `e.accept(*this)` 后再读 `last_value_`——
那样容易忘掉，也降低可读性。

## 8.3 字面量

```cpp
void Interpreter::visit(NumberLit& n) {
    if (n.is_int) last_value_ = Value(n.ivalue);
    else          last_value_ = Value(n.fvalue);
}
void Interpreter::visit(StringLit& s) { last_value_ = Value(s.value); }
void Interpreter::visit(BoolLit& b)   { last_value_ = Value(b.value); }
void Interpreter::visit(NilLit&)      { last_value_ = Value();        }
```

字面量没什么戏。

## 8.4 标识符：从 env 取值

```cpp
void Interpreter::visit(IdentExpr& i) {
    try {
        last_value_ = current_env_->get(i.name);
    } catch (const std::runtime_error& e) {
        throw RuntimeError(e.what(), i.line);
    }
}
```

注意我们把 env 的 `runtime_error` 重新包装成 `RuntimeError` 加上
行号。env 不知道哪一行——只有 visitor 知道。

## 8.5 一元运算

```cpp
void Interpreter::visit(UnaryOp& u) {
    Value v = evaluate(*u.operand);
    switch (u.op) {
        case TokenType::MINUS:
            if (v.is_int())   { last_value_ = Value(-v.as_int()); return; }
            if (v.is_float()) { last_value_ = Value(-v.as_float()); return; }
            throw RuntimeError("unary '-' requires number, got "
                               + std::string(v.type_name()), u.line);
        case TokenType::NOT:
            last_value_ = Value(!v.truthy());
            return;
        default:
            throw RuntimeError("internal: bad unary op", u.line);
    }
}
```

`-` 保留整数性：`-3` 仍然是 int 而不是变成 float。这是个细节，
但用户会注意到：

```python
let x = -3
print(type(x))   # 应该是 "int"
```

## 8.6 二元算术：int / float 自动提升

我们要正确处理：

* `1 + 2` → int 3
* `1 + 2.0` → float 3.0
* `1.0 + 2.0` → float 3.0
* `"a" .. "b"` → string "ab"
* `1 + "2"` → 类型错误（不像 JS 那样隐式转）

```cpp
static Value arith(const Value& a, const Value& b, TokenType op, int line) {
    if (!a.is_number() || !b.is_number()) {
        throw RuntimeError(
            std::string("'") + token_name(op) + "' expects numbers, got "
            + a.type_name() + " and " + b.type_name(), line);
    }
    bool both_int = a.is_int() && b.is_int();
    if (both_int) {
        std::int64_t x = a.as_int(), y = b.as_int();
        switch (op) {
            case TokenType::PLUS:    return Value(x + y);
            case TokenType::MINUS:   return Value(x - y);
            case TokenType::STAR:    return Value(x * y);
            case TokenType::SLASH:
                if (y == 0) throw RuntimeError("integer division by zero", line);
                return Value(x / y);
            case TokenType::PERCENT:
                if (y == 0) throw RuntimeError("integer modulo by zero", line);
                return Value(x % y);
            default: break;
        }
    }
    double x = a.to_float(), y = b.to_float();
    switch (op) {
        case TokenType::PLUS:    return Value(x + y);
        case TokenType::MINUS:   return Value(x - y);
        case TokenType::STAR:    return Value(x * y);
        case TokenType::SLASH:   return Value(x / y);   // float 除 0 = inf/nan
        case TokenType::PERCENT: return Value(std::fmod(x, y));
        default: break;
    }
    throw RuntimeError("internal: bad binop", line);
}
```

设计要点：

* **只有两边都是 int 才走整数路径**，否则一律走 double；
* **`int / 0` 报错，但 `float / 0` 不报错**——后者按 IEEE754 给
  inf/nan，与 Lua、Python 一致；
* **`%` 的语义**：int 用 C++ `%`（trunc 除法），float 用 `fmod`。
  注意 C++ 的 `%` 对负数 `-7 % 3 = -1`，而 Python 是 `2`。我们抄
  C++ 的——简单清楚，用户偶尔抱怨我们就让他们自己写
  `((a % b) + b) % b`。

## 8.7 字符串拼接 `..`

```cpp
case TokenType::DOT_DOT: {
    if (!a.is_string() || !b.is_string()) {
        throw RuntimeError("'..' expects two strings", b->line);
    }
    last_value_ = Value(a.as_string() + b.as_string());
    return;
}
```

为什么不允许 `1 .. 2` 自动转字符串？因为这种"自动 toString"在 JS
里制造了无数 bug（`[] + [] === ""`）。Mini 故意保守：要拼接就先
显式 `to_string()`：

```python
print("count = " .. to_string(n))
```

`to_string` 是个内置函数（第 12 章登记）。

## 8.8 比较 `< <= > >=`

```cpp
static Value compare(const Value& a, const Value& b, TokenType op, int line) {
    // 数字之间
    if (a.is_number() && b.is_number()) {
        double x = a.to_float(), y = b.to_float();
        switch (op) {
            case TokenType::LESS:          return Value(x <  y);
            case TokenType::LESS_EQUAL:    return Value(x <= y);
            case TokenType::GREATER:       return Value(x >  y);
            case TokenType::GREATER_EQUAL: return Value(x >= y);
            default: break;
        }
    }
    // 字符串之间
    if (a.is_string() && b.is_string()) {
        switch (op) {
            case TokenType::LESS:          return Value(a.as_string() <  b.as_string());
            case TokenType::LESS_EQUAL:    return Value(a.as_string() <= b.as_string());
            case TokenType::GREATER:       return Value(a.as_string() >  b.as_string());
            case TokenType::GREATER_EQUAL: return Value(a.as_string() >= b.as_string());
            default: break;
        }
    }
    throw RuntimeError(std::string("cannot compare ")
        + a.type_name() + " and " + b.type_name(), line);
}
```

跨类型比较一律错。这避免了 Python 2 时代 `1 < "a"` 不报错那种黑
魔法。

## 8.9 `==` / `!=`：直接走 `Value::equals`

```cpp
case TokenType::EQUAL_EQUAL: last_value_ = Value(a.equals(b)); return;
case TokenType::BANG_EQUAL:  last_value_ = Value(!a.equals(b)); return;
```

`equals` 内部已经处理了类型差异、数字跨类型等情况——所有的"语义"
都集中在 Value 模块，visitor 只是调用。这种**职责分离**让以后改
相等性规则只改一处。

## 8.10 `BinaryOp` visitor 整合

```cpp
void Interpreter::visit(BinaryOp& b) {
    Value left  = evaluate(*b.left);
    Value right = evaluate(*b.right);
    switch (b.op) {
        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT:
            last_value_ = arith(left, right, b.op, b.line);
            return;
        case TokenType::DOT_DOT:
            if (!left.is_string() || !right.is_string()) {
                throw RuntimeError("'..' expects two strings", b.line);
            }
            last_value_ = Value(left.as_string() + right.as_string());
            return;
        case TokenType::LESS:
        case TokenType::LESS_EQUAL:
        case TokenType::GREATER:
        case TokenType::GREATER_EQUAL:
            last_value_ = compare(left, right, b.op, b.line);
            return;
        case TokenType::EQUAL_EQUAL:
            last_value_ = Value(left.equals(right)); return;
        case TokenType::BANG_EQUAL:
            last_value_ = Value(!left.equals(right)); return;
        default:
            throw RuntimeError("internal: bad binop", b.line);
    }
}
```

## 8.11 短路求值：`and` / `or`

短路意味着**右子树不一定被求值**：

```python
fn side_effect()
    print("called")
    return true
end

let r = false and side_effect()    # 不应该 print "called"
```

所以 `and` / `or` 必须用单独的 `LogicalOp` 节点，不能走 `BinaryOp`
那种"先求值两个操作数"的路径：

```cpp
void Interpreter::visit(LogicalOp& l) {
    Value left = evaluate(*l.left);
    if (l.op == TokenType::OR) {
        if (left.truthy()) { last_value_ = std::move(left); return; }
    } else { // AND
        if (!left.truthy()) { last_value_ = std::move(left); return; }
    }
    last_value_ = evaluate(*l.right);
}
```

注意一个细节：`a and b` 在 Lua 里返回 `b` 而不是 `true`——是值
本身。我们抄这个语义：

```python
let v = nil or 42       # v == 42（不是 true）
let w = "first" and "second"   # w == "second"
```

这让 `or` 能当默认值用：`let x = arg or "default"`。

## 8.12 赋值

```cpp
void Interpreter::visit(AssignExpr& a) {
    Value v = evaluate(*a.value);

    if (auto* id = dynamic_cast<IdentExpr*>(a.target.get())) {
        try {
            current_env_->assign(id->name, v);
        } catch (const std::runtime_error& e) {
            throw RuntimeError(e.what(), a.line);
        }
        last_value_ = std::move(v);   // 赋值表达式的值就是被赋的值
        return;
    }

    // a[k] = v 形式（第 11 章 Table 时再展开）
    if (auto* idx = dynamic_cast<IndexExpr*>(a.target.get())) {
        Value obj = evaluate(*idx->object);
        Value key = evaluate(*idx->index);
        if (!obj.is_table()) {
            throw RuntimeError("can only index assign into table", a.line);
        }
        obj.as_table()->set(key, v);
        last_value_ = std::move(v);
        return;
    }

    throw RuntimeError("invalid assignment target", a.line);
}
```

注意三件事：

1. **左值类型检查在求值时做**——parser 不管它是不是合法 lvalue，
   就让 `dynamic_cast` 来分派；
2. **赋值表达式的值是被赋的值**——这样 `let y = (x = 1)` 让
   `y == 1`，与 C/JS 一致；
3. **`obj` 拿到的是 shared_ptr 副本**——通过它修改 table 内容会
   改动**所有持有该 table 的 Value**，因为 table 是引用语义。这
   正是我们想要的 Lua/Python 风格。

## 8.13 跑通第一个真正的表达式

把前面这些拼起来，已经能跑：

```cpp
auto tokens = Lexer("1 + 2 * 3").tokenize();
Parser p(std::move(tokens));
auto prog = p.parse_program();

Interpreter ip;
auto* es = dynamic_cast<ExprStmt*>(prog->stmts[0].get());
Value result = ip.eval(*es->expr);
std::cout << result << "\n";   // → 7
```

REPL 里现在也能"算东西"，但还没法 `let x = 1`——下一章解决。

## 8.14 一组求值器的单元测试

```cpp
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include <gtest/gtest.h>

using namespace mini;

static Value eval_expr(std::string_view src) {
    auto tokens = Lexer(src).tokenize();
    Parser p(std::move(tokens));
    auto prog = p.parse_program();
    Interpreter ip;
    auto* es = dynamic_cast<ExprStmt*>(prog->stmts[0].get());
    return ip.eval(*es->expr);
}

TEST(Eval, Arithmetic) {
    EXPECT_EQ(eval_expr("1 + 2 * 3").as_int(), 7);
    EXPECT_EQ(eval_expr("(1 + 2) * 3").as_int(), 9);
    EXPECT_EQ(eval_expr("10 / 3").as_int(), 3);
    EXPECT_DOUBLE_EQ(eval_expr("10.0 / 3").as_float(), 10.0/3.0);
}

TEST(Eval, IntStaysInt) {
    Value v = eval_expr("-3");
    EXPECT_TRUE(v.is_int());
    EXPECT_EQ(v.as_int(), -3);
}

TEST(Eval, IntDivByZeroThrows) {
    EXPECT_THROW(eval_expr("1 / 0"), RuntimeError);
}

TEST(Eval, StringConcat) {
    Value v = eval_expr(R"("hello" .. ", " .. "world")");
    EXPECT_EQ(v.as_string(), "hello, world");
}

TEST(Eval, Comparison) {
    EXPECT_TRUE(eval_expr("1 < 2").as_bool());
    EXPECT_FALSE(eval_expr("2 < 2").as_bool());
    EXPECT_TRUE(eval_expr("2 <= 2").as_bool());
    EXPECT_TRUE(eval_expr(R"("a" < "b")").as_bool());
}

TEST(Eval, EqualityCrossType) {
    EXPECT_TRUE(eval_expr("1 == 1.0").as_bool());
    EXPECT_FALSE(eval_expr("1 == \"1\"").as_bool());
}

TEST(Eval, LogicalShortCircuit_OrReturnsValue) {
    Value v = eval_expr(R"(nil or "fallback")");
    EXPECT_EQ(v.as_string(), "fallback");
}

TEST(Eval, LogicalShortCircuit_AndStopsOnFalse) {
    // 如果短路坏了，下面会算 1/0 抛异常
    EXPECT_NO_THROW(eval_expr("false and (1 / 0)"));
}

TEST(Eval, NotOperator) {
    EXPECT_TRUE(eval_expr("not nil").as_bool());
    EXPECT_TRUE(eval_expr("not false").as_bool());
    EXPECT_FALSE(eval_expr("not 0").as_bool());      // 0 is truthy in Mini
}
```

最后一个测试**特别重要**——我们抄 Lua 的"0 is truthy"语义，要
单测固化这个决定，避免日后被新人"修正"成 Python 行为。

## 8.15 一个性能小贴士（先记下来）

`evaluate` 每次都拷贝 / move 一个 `Value`。`sizeof(Value) ≈ 24
字节`，看起来不大，但深嵌套表达式会反复构造析构 shared_ptr，原
子操作的开销不小。

加速路径有两条：

1. **改 `last_value_` 为引用语义**：visitor 直接写一个共享缓冲，
   避免每次构造；但代码变难读；
2. **第三部分换字节码 VM**：用栈数组 + index，move 到位，零拷
   贝；这是工业方案。

现在不做。

---

下一章我们把语句 visitor 写完——`let`、`if`、`while`、`for`，
让 Mini 终于能跑出有控制流的程序。然后第 10 章再加函数与闭包，
Mini 就是一门完整的脚本语言了。
