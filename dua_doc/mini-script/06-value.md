# 第 6 章　Value：用 std::variant 装一切

第二部分开始。我们要写一个**树遍历解释器**——直接走 AST 求值，
不经过字节码。这是最短能让 Mini 跑起来的路径，写完大约 600 行 C++。

第一件事是设计一个**统一的运行时值**。Mini 是动态类型的，所以一
个变量可能持有 `42`、`"hello"`、`true`、`nil`，甚至是一个函数或
一个 table。我们需要一个 C++ 类型把这些**都装得进去**。

## 6.1 候选方案

### 方案 A：所有值都 `void*` 指针 + 类型标签

老式 Lisp 解释器的写法：

```cpp
struct Value {
    enum Type { NIL, INT, BOOL, STRING, ... } type;
    void* data;
};
```

* 优点：极简；
* 缺点：内存散乱、`int` 也要堆分配、易写出 use-after-free。

不用。

### 方案 B：手写 union + tag

```cpp
struct Value {
    enum Type { NIL, BOOL, INT, FLOAT, STRING, ... } type;
    union {
        bool       b;
        int64_t    i;
        double     f;
        std::shared_ptr<std::string>* s;  // 注意 union 不能直接放有析构的类型
        // ...
    };
};
```

* 优点：紧凑、`int` 内联存储；
* 缺点：**C++ 的 union 对非平凡类型几乎不可用**——必须自己写
  copy ctor / move ctor / dtor，调用 placement new、显式析构，几
  十行模板代码才能让它正确。Lua 用 union 因为它是 C 写的；C++
  里不值得。

### 方案 C：`std::variant`

```cpp
using Value = std::variant<Nil, bool, int64_t, double, StringRef, ...>;
```

* 优点：**自动管理析构、移动、复制**；类型安全；`std::visit`
  做模式匹配；
* 缺点：API 略啰嗦（`std::get`、`std::holds_alternative`）；不
  容易做"有 GC 的指针"。

我们用 C 方案。第三部分 VM 那边我们会讨论怎么改成"NaN-boxed
64bit Value"——那是性能向的设计，但教学版本用 variant 就够。

## 6.2 Value 的具体定义

`src/value.h`：

```cpp
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include <unordered_map>
#include <iosfwd>

namespace mini {

class Value;
class Function;   // 见第 10 章
class Table;      // 见第 11 章

// 字符串用 shared_ptr<const string>，便于多个 Value 共享同一份内容
using StringRef   = std::shared_ptr<const std::string>;
// 函数与 Table 都是引用语义对象（一等公民）
using FunctionRef = std::shared_ptr<Function>;
using TableRef    = std::shared_ptr<Table>;

struct Nil {};   // 用空 struct 占位

class Value {
public:
    using Variant = std::variant<
        Nil,
        bool,
        std::int64_t,
        double,
        StringRef,
        FunctionRef,
        TableRef
    >;

    Value() : v_(Nil{}) {}
    Value(Nil)            : v_(Nil{}) {}
    Value(bool b)         : v_(b) {}
    Value(std::int64_t i) : v_(i) {}
    Value(double d)       : v_(d) {}
    Value(int i)          : v_(static_cast<std::int64_t>(i)) {}  // 防止 bool 重载抢走 int
    Value(const char* s)  : v_(std::make_shared<const std::string>(s)) {}
    Value(std::string s)  : v_(std::make_shared<const std::string>(std::move(s))) {}
    Value(StringRef s)    : v_(std::move(s)) {}
    Value(FunctionRef f)  : v_(std::move(f)) {}
    Value(TableRef t)     : v_(std::move(t)) {}

    enum class Type { Nil, Bool, Int, Float, String, Function, Table };
    Type type() const { return static_cast<Type>(v_.index()); }
    const char* type_name() const;

    // 类型查询
    bool is_nil()      const { return std::holds_alternative<Nil>(v_); }
    bool is_bool()     const { return std::holds_alternative<bool>(v_); }
    bool is_int()      const { return std::holds_alternative<std::int64_t>(v_); }
    bool is_float()    const { return std::holds_alternative<double>(v_); }
    bool is_number()   const { return is_int() || is_float(); }
    bool is_string()   const { return std::holds_alternative<StringRef>(v_); }
    bool is_function() const { return std::holds_alternative<FunctionRef>(v_); }
    bool is_table()    const { return std::holds_alternative<TableRef>(v_); }

    // 取值（不做类型检查；调用方先 is_xxx）
    bool         as_bool()     const { return std::get<bool>(v_); }
    std::int64_t as_int()      const { return std::get<std::int64_t>(v_); }
    double       as_float()    const { return std::get<double>(v_); }
    const std::string& as_string() const { return *std::get<StringRef>(v_); }
    const StringRef&   as_string_ref() const { return std::get<StringRef>(v_); }
    const FunctionRef& as_function()   const { return std::get<FunctionRef>(v_); }
    const TableRef&    as_table()      const { return std::get<TableRef>(v_); }

    // 强制转 double（int 自动 widen），用于算术
    double to_float() const {
        if (is_int())   return static_cast<double>(as_int());
        if (is_float()) return as_float();
        throw type_error("expected number");
    }

    // 真值性（控制流用）
    bool truthy() const;

    // 字符串化（print 用）
    std::string to_string() const;

    // 相等性（== 运算）
    bool equals(const Value& other) const;

    const Variant& raw() const { return v_; }

private:
    Variant v_;

    static std::runtime_error type_error(const std::string& msg);
};

std::ostream& operator<<(std::ostream& os, const Value& v);

}  // namespace mini
```

几个值得展开的设计：

### `Nil` 是空 struct，不是 `std::monostate`

`std::variant` 默认推荐用 `std::monostate` 做"空值"占位。我们写
个空 struct `Nil` 替代，原因：

* 后续重载 `Value(Nil)` 构造比 `Value(std::monostate)` 可读得多；
* 可以给 `Nil` 加方法（比如 `to_string` 返回 `"nil"`）；
* 调试器里看类型名是 `mini::Nil` 不是 `std::monostate`。

### `Value(int i)` 显式 widening 到 int64_t

```cpp
Value(int i) : v_(static_cast<std::int64_t>(i)) {}
```

不写这一行的话，`Value(42)` 会调用 `Value(bool)`——因为标准 C++
里 `int → bool` 是隐式转换，比 `int → int64_t` 优先级一样但匹配
更"直接"。这是 `std::variant + 重载构造`的经典坑。

### 字符串用 `shared_ptr<const string>` 共享

为什么不直接 `std::string`？

```python
let a = "hello world this is a long string"
let b = a   # 是不是要拷一份？
```

如果 Value 内部存 `string`，每次复制 Value 都拷一份字符串内容，
散落在数据结构里的 string 数量爆炸。用 `shared_ptr<const string>`：

* `Value` 拷贝只增引用计数；
* `const` 保证字符串内容**不可变**——这是 Lua / Python 都遵循
  的语义；如果用户要改，就生成新字符串。
* GC 简单——shared_ptr 帮我们自动释放（第 22 章会讨论它在循环引
  用上的局限）。

### `truthy()` 的"哲学问题"

```cpp
bool Value::truthy() const {
    if (is_nil())   return false;
    if (is_bool())  return as_bool();
    return true;   // 其它一切都是 truthy
}
```

主流脚本语言对此分成两派：

| 语言       | 0 是 truthy？ | "" 是 truthy？ | 空 list 是 truthy？ |
| ---------- | ------------- | -------------- | ------------------- |
| Lua        | **是**        | **是**         | **是**              |
| Python     | 否            | 否             | 否                  |
| JavaScript | 否            | 否             | 是                  |

我们抄 Lua——**只有 `nil` 和 `false` 假，其它一切真**。这有两
个好处：

1. 实现极简（两行）；
2. `if some_table then ...` 这种"是否存在"判断不需要写
   `if some_table != nil`。

代价是 `if 0 then ...` 会进分支——但用户用过 Lua 后会习惯。

## 6.3 `to_string`：print 的核心

```cpp
std::string Value::to_string() const {
    return std::visit([](const auto& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Nil>) return "nil";
        else if constexpr (std::is_same_v<T, bool>) return x ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::int64_t>) return std::to_string(x);
        else if constexpr (std::is_same_v<T, double>) {
            // 让 1.0 打印成 "1.0" 而不是 "1"
            std::string s = std::to_string(x);
            if (s.find('.') == std::string::npos) s += ".0";
            return s;
        }
        else if constexpr (std::is_same_v<T, StringRef>) return *x;
        else if constexpr (std::is_same_v<T, FunctionRef>) {
            return "<function>";
        }
        else if constexpr (std::is_same_v<T, TableRef>) {
            return "<table>";
        }
    }, v_);
}
```

这就是 `std::visit` + `if constexpr` 的"模式匹配"经典写法。每个
分支由 `T` 的类型决定，编译期分派——零运行时开销。

注意 `double` 那一段——`std::to_string(1.0)` 会给出 `"1.000000"`，
我们希望 `"1.0"`。生产环境会用 `std::format("{:.6g}", x)`，这里
为了不引 C++20 先用 `to_string` 凑合。`<table>`、`<function>` 这
种简略表示参考 Lua——后续可以加 `repr` 让它显示地址/字段。

## 6.4 `equals`：== 的精细规则

`==` 在动态语言里看似简单，实际坑非常多。我们抄 Lua + Python 的混
合规则：

* **类型不同** → 不等（除了 int 与 float 互比）；
* **`nil == nil`** → true；
* **数字之间**：int vs int 直接比；任一是 float 用 double 比；
* **字符串之间**：内容比较（不是地址）；
* **函数 / table** → 地址（引用）比较；

```cpp
bool Value::equals(const Value& o) const {
    // 数字跨类型
    if (is_number() && o.is_number()) {
        if (is_int() && o.is_int()) return as_int() == o.as_int();
        return to_float() == o.to_float();
    }
    if (type() != o.type()) return false;
    return std::visit([&](const auto& a) -> bool {
        using T = std::decay_t<decltype(a)>;
        const auto& b = std::get<T>(o.v_);
        if constexpr (std::is_same_v<T, Nil>)        return true;
        else if constexpr (std::is_same_v<T, bool>)  return a == b;
        else if constexpr (std::is_same_v<T, StringRef>) {
            // 字符串：先看是不是同一个 shared 实例（快路径），再比内容
            return a == b || *a == *b;
        }
        else {
            return a == b;   // 其它情况（function/table）shared_ptr 比地址
        }
    }, v_);
}
```

一行**字符串快路径**：`a == b || *a == *b`。当多个 Value 复制自
同一个字面量时，它们共享同一个 `shared_ptr`，相等性查询 O(1) 完
成；只有真不是同一份时才走内容比较 O(n)。Python 的字符串 interning
也是同样的优化思路。

## 6.5 类型错误：`type_error`

`Value` 不持有"类型检查"职责——`as_bool()` / `as_int()` 不验
证就直接 `std::get`，错了会抛 `std::bad_variant_access`。这种 C++
异常对用户不友好。所以我们提供一个 helper：

```cpp
std::runtime_error Value::type_error(const std::string& msg) {
    return std::runtime_error(msg);
}
```

求值器会主动调用：

```cpp
double left = a.to_float();   // 内部自动 throw 带 "expected number"
```

未来加上 line/column 之后，这个异常会被 Evaluator 捕获并加上"在
第几行"的上下文，这是后面第 21 章错误处理章节的事。

## 6.6 让 Value 可以走进 stdout

```cpp
std::ostream& operator<<(std::ostream& os, const Value& v) {
    return os << v.to_string();
}
```

REPL 和 `print()` 都靠它。

## 6.7 一组 Value 的单元测试

```cpp
#include "value.h"
#include <gtest/gtest.h>

using namespace mini;

TEST(Value, DefaultIsNil) {
    Value v;
    EXPECT_TRUE(v.is_nil());
    EXPECT_EQ(v.to_string(), "nil");
}

TEST(Value, IntNotMistakenAsBool) {
    Value v(42);
    EXPECT_TRUE(v.is_int());
    EXPECT_FALSE(v.is_bool());
    EXPECT_EQ(v.as_int(), 42);
}

TEST(Value, FloatPrintHasDot) {
    Value v(1.0);
    EXPECT_EQ(v.to_string(), "1.000000");  // 第一版接受这种格式
}

TEST(Value, TruthyRules) {
    EXPECT_FALSE(Value().truthy());                // nil
    EXPECT_FALSE(Value(false).truthy());
    EXPECT_TRUE(Value(true).truthy());
    EXPECT_TRUE(Value(0).truthy());                // 抄 Lua：0 是 true
    EXPECT_TRUE(Value("").truthy());               // 空串也是 true
}

TEST(Value, IntFloatEquality) {
    EXPECT_TRUE(Value(1).equals(Value(1.0)));
    EXPECT_TRUE(Value(2.0).equals(Value(2)));
    EXPECT_FALSE(Value(1).equals(Value(2)));
}

TEST(Value, StringEqualityByContent) {
    Value a("hello");
    Value b("hello");
    EXPECT_TRUE(a.equals(b));
}

TEST(Value, TableIdentity) {
    auto t1 = std::make_shared<Table>();
    auto t2 = std::make_shared<Table>();
    EXPECT_TRUE(Value(t1).equals(Value(t1)));
    EXPECT_FALSE(Value(t1).equals(Value(t2)));
}

TEST(Value, TypeError) {
    Value v(42);
    EXPECT_THROW(v.as_string(), std::bad_variant_access);
    // 我们的 to_float 自己抛 runtime_error
    Value s("hi");
    EXPECT_THROW(s.to_float(), std::runtime_error);
}
```

## 6.8 一些"现在不做但要记下"的事

* **数字 hash**：当 table 用作字典时，需要给 Value 实现一个 hash
  函数。先放第 11 章 Table 那里讲；
* **NaN 的相等性**：IEEE754 规定 `NaN != NaN`。我们的 `equals`
  会保留这个行为（C++ `==` 自动处理）——和 Lua/JS 一致；
* **整数溢出**：`9223372036854775807 + 1` 在我们这里是 wraparound
  到负数。Python 会自动转 bignum，我们不做——这也是教学项目的边
  界；
* **浮点格式化**：第 23 章性能小节会改成 `std::format`，更准更短。

## 6.9 Value 的内存代价

`sizeof(Value)` 大概多大？

* `std::variant<...>` 的 size = max(成员 size) + 一个 tag 字节（对
  齐后通常 8 字节）；
* 我们最大的成员是 `shared_ptr`（16 字节，指针 + 控制块指针）；
* 所以 `sizeof(Value) ≈ 24` 字节（取决于编译器和对齐）。

对教学版本完全可接受。第三部分 VM 我们会试着把它压到 16 甚至 8
字节（NaN-boxing）——但那是为了大型 benchmark 时的 cache 压力，
在 Mini 的日常使用里看不出差别。

---

下一章是 **第 7 章 Environment：变量作用域链**——我们要回答"变
量名怎么找到对应的 Value"这个问题，以及为什么"全局变量 + 嵌套
作用域"用一棵单链表的 HashMap 链就能优雅地解决。这个数据结构会
在第 10 章成为闭包的载体。
