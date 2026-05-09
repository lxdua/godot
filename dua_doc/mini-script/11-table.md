# 第 11 章　Table：数组与字典合体

到目前为止 Mini 还没有"复合数据结构"——没法装一个列表、一个
字典、一个对象。这一章我们仿 Lua 设计一个 **Table**：同一个类
型既能当数组也能当字典，键可以是几乎任意 Value。

写完后这种代码就能跑：

```python
let arr = [10, 20, 30]
print(arr[0])              # 10
arr[3] = 40
print(len(arr))            # 4

let person = {"name": "alice", "age": 30}
print(person["name"])      # alice

# 数组与字典混用
let mixed = {1: "one", "two": 2}
```

## 11.1 Lua 的"统一表"：一个数据结构装一切

Lua 的核心抽象只有一个：**table**。它同时是：

* 数组（`t[1] = "a"`）
* 字典（`t.name = "bob"` ≡ `t["name"] = "bob"`）
* 集合（用 key 当成员标记）
* 对象（带元方法的 table）
* 命名空间（模块就是个 table）

这种"少即是多"的设计让 Lua 实现极小却表达力惊人。我们抄它——
但只抄"数组+字典"这两面，不上元表（metatable）。

## 11.2 实现策略

我们要解决两个矛盾：

* **数组要快**：`arr[i]` 应该是 O(1) 的下标访问；
* **字典要灵活**：键可以是任意 Value；查找 O(1)。

最朴素的实现：用 `unordered_map<Value, Value>` 装一切。问题：

* 数组场景下连续整数键 `0,1,2,...` 用 hashmap 浪费 cache；
* `Value` 作为 key 需要写 hash + ==；
* 迭代顺序乱（用户的 `[1,2,3]` 会被打乱）。

Lua 的解法：**双区结构**。每个 table 内部有：

* `array` 部分：紧凑 vector，存"小整数索引"的值；
* `hash` 部分：unordered_map，存其他键。

写入 `t[i]`，如果 `i` 是 1..N 范围的整数（N 是当前数组长度），
走 array；否则走 hash。读时优先查 array，不在再查 hash。

我们走简化版本——**用 vector 存 0..N-1 整数键的值，用 unordered_map
存其它**。教学性大于性能。

## 11.3 Table 的实现

`src/table.h`：

```cpp
#pragma once
#include "value.h"
#include <unordered_map>
#include <vector>

namespace mini {

class Table {
public:
    // 任意 Value 作 key
    Value get(const Value& key) const;
    void  set(const Value& key, Value v);
    bool  has(const Value& key) const;

    // 数组语义
    std::size_t array_size() const { return array_.size(); }
    Value at(std::size_t i) const {
        if (i < array_.size()) return array_[i];
        return Value();   // nil
    }

    // 总元素个数（数组 + 字典）
    std::size_t size() const { return array_.size() + hash_.size(); }

    // 迭代 helper（第 12 章 builtin len 用）
    const std::vector<Value>& array() const { return array_; }

private:
    // key 的 hash + eq（针对 Value 的简化版）
    struct ValueHash {
        std::size_t operator()(const Value& v) const noexcept;
    };
    struct ValueEq {
        bool operator()(const Value& a, const Value& b) const noexcept {
            return a.equals(b);
        }
    };

    std::vector<Value> array_;
    std::unordered_map<Value, Value, ValueHash, ValueEq> hash_;
};

}  // namespace mini
```

## 11.4 `Value` 作为 key：hash 和 eq

我们的 `Value::equals` 已经写好。还差 hash。

```cpp
std::size_t Table::ValueHash::operator()(const Value& v) const noexcept {
    return std::visit([](const auto& x) -> std::size_t {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Nil>)        return 0;
        else if constexpr (std::is_same_v<T, bool>)  return std::hash<bool>{}(x);
        else if constexpr (std::is_same_v<T, std::int64_t>) {
            return std::hash<std::int64_t>{}(x);
        }
        else if constexpr (std::is_same_v<T, double>) {
            // 让 1 和 1.0 hash 相同，与 equals 一致
            if (x == static_cast<std::int64_t>(x)) {
                return std::hash<std::int64_t>{}(static_cast<std::int64_t>(x));
            }
            return std::hash<double>{}(x);
        }
        else if constexpr (std::is_same_v<T, StringRef>) {
            return std::hash<std::string>{}(*x);
        }
        else {
            // function / table：按指针 hash（与 equals 一致）
            return std::hash<const void*>{}(x.get());
        }
    }, v.raw());
}
```

最关键的一行——**`1` 与 `1.0` 必须 hash 相同**：

```cpp
if (x == static_cast<std::int64_t>(x)) {
    return std::hash<std::int64_t>{}(static_cast<std::int64_t>(x));
}
```

因为我们在 `Value::equals` 里规定 `1 == 1.0`。如果 hash 不一致，
`t[1] = "a"; t[1.0]` 就会查不到——hashmap 的核心要求是
"a == b ⇒ hash(a) == hash(b)"。

这个要求在动态类型语言里特别容易踩坑。Python 也强制要求
`hash(1) == hash(1.0) == hash(True)`，原因相同。

## 11.5 `get` / `set`：双区调度

```cpp
// src/table.cpp
Value Table::get(const Value& key) const {
    // 整数 key 走数组
    if (key.is_int()) {
        std::int64_t i = key.as_int();
        if (i >= 0 && static_cast<std::size_t>(i) < array_.size()) {
            return array_[i];
        }
    }
    auto it = hash_.find(key);
    if (it != hash_.end()) return it->second;
    return Value();   // 不存在 = nil
}

void Table::set(const Value& key, Value v) {
    if (key.is_nil()) {
        throw std::runtime_error("table key cannot be nil");
    }

    // 整数 key + 在数组范围内：写数组
    if (key.is_int()) {
        std::int64_t i = key.as_int();
        if (i >= 0 && static_cast<std::size_t>(i) < array_.size()) {
            array_[i] = std::move(v);
            return;
        }
        // 整数 key 紧接着 array 末尾 → append（让 [1,2,3] 能 t[3] = 4）
        if (i >= 0 && static_cast<std::size_t>(i) == array_.size()) {
            array_.push_back(std::move(v));
            // 别忘了把 hash 里可能有的"洞"也推进 array
            try_extend_array_from_hash();
            return;
        }
    }
    hash_[key] = std::move(v);
}

bool Table::has(const Value& key) const {
    if (key.is_int()) {
        std::int64_t i = key.as_int();
        if (i >= 0 && static_cast<std::size_t>(i) < array_.size()) {
            return !array_[i].is_nil();
        }
    }
    return hash_.find(key) != hash_.end();
}

// 把 hash 里可能"接续 array 末尾"的整数键搬到 array
void Table::try_extend_array_from_hash() {
    while (true) {
        Value k(static_cast<std::int64_t>(array_.size()));
        auto it = hash_.find(k);
        if (it == hash_.end()) break;
        array_.push_back(std::move(it->second));
        hash_.erase(it);
    }
}
```

`try_extend_array_from_hash` 处理这种情况：

```python
let t = {}
t[2] = "c"   # 走 hash（因为 array 还空）
t[0] = "a"   # 走 array → 长度变 1
t[1] = "b"   # 走 array → 长度变 2，触发 extend：发现 hash 里有 2，搬过来 → 长度 3
```

这是 Lua 内部"rehash"逻辑的简化版——保证连续整数最终都能进 array
区，让数组语义享受 cache 友好。

## 11.6 接通 Interpreter

之前 8.12 节我们留了 IndexExpr 的位置，现在补全：

```cpp
void Interpreter::visit(IndexExpr& e) {
    Value obj = evaluate(*e.object);
    Value key = evaluate(*e.index);
    if (obj.is_string()) {
        // 字符串下标：返回单字节字符串（教学版偷懒，不处理 UTF-8 边界）
        if (!key.is_int()) {
            throw RuntimeError("string index must be int", e.line);
        }
        std::int64_t i = key.as_int();
        const auto& s = obj.as_string();
        if (i < 0 || static_cast<std::size_t>(i) >= s.size()) {
            throw RuntimeError("string index out of range", e.line);
        }
        last_value_ = Value(std::string(1, s[i]));
        return;
    }
    if (!obj.is_table()) {
        throw RuntimeError(
            std::string("cannot index ") + obj.type_name(), e.line);
    }
    last_value_ = obj.as_table()->get(key);
}

void Interpreter::visit(ArrayLit& a) {
    auto t = std::make_shared<Table>();
    for (std::size_t i = 0; i < a.elements.size(); ++i) {
        Value v = evaluate(*a.elements[i]);
        t->set(Value(static_cast<std::int64_t>(i)), std::move(v));
    }
    last_value_ = Value(TableRef(std::move(t)));
}

void Interpreter::visit(TableLit& tl) {
    auto t = std::make_shared<Table>();
    for (auto& e : tl.entries) {
        Value k = evaluate(*e.key);
        Value v = evaluate(*e.value);
        t->set(k, std::move(v));
    }
    last_value_ = Value(TableRef(std::move(t)));
}
```

`AssignExpr` 那边的 `idx->object` 分支我们在 8.12 已经写好——
直接调 `obj.as_table()->set(key, v)` 就完事。

## 11.7 引用语义：用户能看见的副作用

```python
let a = [1, 2, 3]
let b = a            # b 与 a 指向同一个 table
b[0] = 99
print(a[0])          # 99（不是 1）
```

这是因为 `Value` 内部存的是 `TableRef`（shared_ptr）——`b = a`
只是 shared_ptr 复制。要拷贝 table 得显式：

```python
fn copy(t)
    let r = []
    let i = 0
    while i < len(t) do
        r[i] = t[i]
        i = i + 1
    end
    return r
end
```

这个语义和 Lua / Python / JS 完全一致——容器都是引用，拷贝靠
显式 API。这通常比"自动深拷贝"少 99% 的性能问题。

## 11.8 字符串 key 的常见模式：模拟对象

```python
let p = {
    "name": "alice",
    "age":  30,
    "greet": fn(self)
        print("hi, " .. self["name"])
    end,
}
p["greet"](p)   # hi, alice
```

Mini 没有 `obj.field` 语法糖（懒得加`.`运算符——加上去也只是
`IndexExpr` 包装），用 `[]` 直接访问也能干所有事。GDScript 的早
期版本也是这样的——后来才加了 `.`。

把 `self` 当第一个参数显式传入，这是 Python 风格。Lua 用 `:` 语
法糖（`p:greet()`）来隐式传 self；我们不做，让用户自己显式传。

## 11.9 `len`：Table 的"长度"

`len(t)` 这个内置函数（第 12 章正式注册）的语义：

* 对**数组形态**的 table，返回数组长度；
* 对**字典形态**，返回总键数；
* 对字符串，返回字节数。

```cpp
Value builtin_len(std::vector<Value>& args) {
    if (args.size() != 1) {
        throw std::runtime_error("len() takes 1 argument");
    }
    const Value& v = args[0];
    if (v.is_string()) {
        return Value(static_cast<std::int64_t>(v.as_string().size()));
    }
    if (v.is_table()) {
        return Value(static_cast<std::int64_t>(v.as_table()->size()));
    }
    throw std::runtime_error("len() expects string or table");
}
```

这里有个值得讨论的小坑：Lua 的 `#t` 在"有空洞"的数组上**未定
义**——`{[1]="a", [3]="c"}` 长度是 1 还是 3 取决于实现。我们用
"array_ + hash_ 总数"，不会有这个问题；但用户写
`t[10] = "x"` 后 `len(t) == 1`（因为 10 进了 hash 而不是 array），
他可能觉得意外。

第一版接受这种行为，文档里说清楚就行。第 24 章给出"统一长度
规范"作为练习题。

## 11.10 一组 Table 单测

```cpp
TEST(Table, ArrayLiteralAndIndex) {
    auto out = run(
        "let a = [10, 20, 30]\n"
        "print(a[0])\n"
        "print(a[2])\n");
    EXPECT_EQ(out, "10\n30\n");
}

TEST(Table, AppendByNextIndex) {
    auto out = run(
        "let a = [1, 2]\n"
        "a[2] = 3\n"
        "print(len(a))\n");
    EXPECT_EQ(out, "3\n");
}

TEST(Table, DictLiteral) {
    auto out = run(
        R"(let d = {"name": "alice", "age": 30}
           print(d["name"])
           print(d["age"])
           )");
    EXPECT_EQ(out, "alice\n30\n");
}

TEST(Table, IntFloatKeyEquivalence) {
    auto out = run(
        "let t = {}\n"
        "t[1] = \"a\"\n"
        "print(t[1.0])\n");
    EXPECT_EQ(out, "a\n");
}

TEST(Table, NilKeyForbidden) {
    EXPECT_THROW(run("let t = {}\nt[nil] = 1\n"), RuntimeError);
}

TEST(Table, ReferenceSemantics) {
    auto out = run(
        "let a = [1, 2, 3]\n"
        "let b = a\n"
        "b[0] = 99\n"
        "print(a[0])\n");
    EXPECT_EQ(out, "99\n");
}

TEST(Table, MissingKeyReturnsNil) {
    auto out = run(
        "let d = {}\n"
        "if d[\"missing\"] == nil then print(\"none\") end\n");
    EXPECT_EQ(out, "none\n");
}
```

注意 `IntFloatKeyEquivalence` 这个测试——它固化了"int / float
key hash 一致"那个非常容易写错的设计点。

## 11.11 进阶：能加什么

* **`pairs(t)` / `ipairs(t)`** 迭代器：第 12 章；
* **`.field` 语法糖**：在 Pratt rules 里加一个 `DOT`，prefix 是
  IDENT 后缀，等价于 `["field"]`。10 行代码；
* **元方法 / metatable**：能让 `t1 + t2` 重载、`obj.x` 触发
  `__index`——这是 Lua 的精髓但代码量翻一倍。本书不做，留作练习；
* **数组的 `push` / `pop`**：第 12 章作为内置函数加上去。

## 11.12 一个让人意外的"少代码"成果

我们这章只写了 ~150 行 C++，Mini 的复合类型就齐了——既能装数组
又能装字典，键类型几乎任意。再加上前面的函数、闭包、控制流，
你已经能用 Mini 写出非常多真实程序：

```python
# 实现一个简单的 stack（用 table）
fn make_stack()
    let s = []
    return {
        "push": fn(x) s[len(s)] = x end,
        "pop":  fn()
            let n = len(s) - 1
            let v = s[n]
            s[n] = nil
            return v
        end,
        "size": fn() return len(s) end,
    }
end

let st = make_stack()
st["push"](10)
st["push"](20)
print(st["pop"]())   # 20
print(st["size"]())  # 1
```

这就是"用 table + 闭包写对象"的完整例子。Lua 的整个面向对象生态
都建立在这个模式上。

---

下一章是 **第 12 章 内置函数与 REPL**，我们把 `print / len / type
/ assert / to_string` 等核心 builtin 注册起来，并把 REPL 做得稍
微像样一点（输入历史、多行续行、表达式回显）。完成后第二部分
（树遍历解释器）就全部结束——你将拥有一门**完整、能用、能扩展**
的脚本语言。
