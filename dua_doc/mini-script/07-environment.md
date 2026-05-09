# 第 7 章　Environment：变量作用域链

有了 `Value`，下一个问题是：**变量名怎么找到值**？

```python
let x = 10
fn outer()
    let y = 20
    fn inner()
        let z = 30
        print(x + y + z)   # 同时能看到 x、y、z
    end
    inner()
end
outer()
```

`inner` 里 `print(x + y + z)`：

* `x` 在最外层（"全局"）；
* `y` 在 `outer` 的作用域里；
* `z` 在 `inner` 自己的作用域里。

我们需要一个数据结构能回答："`x` 是谁？"，并且能正确按照**词法
作用域**（lexical scope）的规则找到它。这一章就来设计它，名字叫
`Environment`（业内通常简称 env）。

## 7.1 单链表 + HashMap：最经典的 env

把每个作用域做成一个 `unordered_map<string, Value>`，再让它们形
成一条单链表：

```
inner_env  { z: 30 }       ──parent──▶
outer_env  { y: 20 }       ──parent──▶
global_env { x: 10, print: <fn> }
```

查找时从当前 env 往上找；找不到就跳到 parent；一直跳到全局；再
找不到就报"undefined variable"。

`src/env.h`：

```cpp
#pragma once
#include "value.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace mini {

class Environment;
using EnvRef = std::shared_ptr<Environment>;

class Environment : public std::enable_shared_from_this<Environment> {
public:
    Environment() = default;
    explicit Environment(EnvRef parent) : parent_(std::move(parent)) {}

    // 创建一个孩子作用域（push）
    EnvRef new_child() {
        return std::make_shared<Environment>(shared_from_this());
    }

    EnvRef parent() const { return parent_; }

    // 在当前作用域声明变量（let）
    // 已存在则覆盖（这是我们故意的：脚本里 let x; let x 不报错）
    void define(const std::string& name, Value v) {
        vars_[name] = std::move(v);
    }

    // 读取：沿链向上查找
    const Value& get(const std::string& name) const {
        const Environment* env = this;
        while (env) {
            auto it = env->vars_.find(name);
            if (it != env->vars_.end()) return it->second;
            env = env->parent_.get();
        }
        throw std::runtime_error("undefined variable: " + name);
    }

    // 赋值：沿链向上找到第一个有该名字的作用域，写它
    // 如果整条链都找不到，报错（不像 Python 那样会自动创建全局）
    void assign(const std::string& name, Value v) {
        Environment* env = this;
        while (env) {
            auto it = env->vars_.find(name);
            if (it != env->vars_.end()) {
                it->second = std::move(v);
                return;
            }
            env = env->parent_.get();
        }
        throw std::runtime_error(
            "cannot assign to undefined variable: " + name);
    }

    bool has(const std::string& name) const {
        const Environment* env = this;
        while (env) {
            if (env->vars_.count(name)) return true;
            env = env->parent_.get();
        }
        return false;
    }

private:
    std::unordered_map<std::string, Value> vars_;
    EnvRef parent_;   // 词法外层（"上一层作用域"）
};

}  // namespace mini
```

整个类只有 70 行——这就是为什么经典教材都从 env 开始讲解释器。

## 7.2 几个关键设计决策

### 决策 1：`define` vs `assign` 严格分离

很多新手会写一个 `set(name, value)`，"如果存在就更新，否则创
建"。这是 Python `=` 的行为。

我们故意分开。原因：

* Mini 强制 `let` 才能创建变量；
* 这样 `assign` 找不到变量是**错误**（用户漏写了 let）；
* 区分"声明"和"赋值"能挡掉 typo——`couter = counter + 1`
  不会偷偷创建一个新变量；

GDScript 也是这种风格，Lua / JavaScript 则相反（`x = 1` 自动创建
全局）——后者多年来制造了无数 bug，不学。

### 决策 2：作用域是引用类型（`shared_ptr`）

为什么不直接用栈对象？

```cpp
Environment outer(global_env);
Environment inner(outer);     // 错：outer 必须比 inner 活得久
```

栈对象生命周期太僵硬。我们的 env 要能：

* 被闭包**捕获**——函数返回后捕获的 env 仍要活着；
* 被多个调用同时访问——一个嵌套函数的两次调用产生两个 inner
  env，但共享同一个 outer。

`shared_ptr` 的引用计数自然解决这两个问题。代价是引用计数本身
有原子加减开销——教学版本完全不在乎，性能版本（第 22 章 GC）
我们会换成自定义 GC。

### 决策 3：`enable_shared_from_this`

```cpp
class Environment : public std::enable_shared_from_this<Environment> {
    EnvRef new_child() {
        return std::make_shared<Environment>(shared_from_this());
    }
};
```

要在成员函数里得到"指向自己的 shared_ptr"，必须继承
`enable_shared_from_this`。如果忘了它，`shared_from_this()` 会
throw `bad_weak_ptr`——这是新手最容易踩的 C++ 标准库坑之一。

## 7.3 词法 vs 动态作用域：一句话讲清

这一节穿插一个概念性的展开。我们写的是**词法作用域**：变量绑定
由"源代码里的位置"决定。

```python
let x = 10
fn f()
    print(x)
end

fn g()
    let x = 20
    f()        # 打印什么？
end
g()
```

* 词法作用域：打印 `10`（`f` 看到的是声明 `f` 时的环境，那时 `x = 10`）；
* 动态作用域：打印 `20`（`f` 看到的是调用栈上的 `x`）。

主流脚本语言全部用词法作用域。我们的 `Environment` 实现用
**parent 指针**这一招就实现了它——`f` 创建时 parent 指针指向声
明它的那个 env，无论谁来调它，往上找永远找到最初的 env。这是
LISP 1958 年就发现的小聪明，今天仍是脚本语言最常用的实现方式。

## 7.4 闭包预告：env 不是被动的"调用栈"

注意我们的 env 与 C 函数调用栈是**两个完全不同的东西**：

* C 调用栈：函数返回时整个栈帧消失；
* env 链：函数返回时，**如果没有任何闭包捕获它**，shared_ptr 计
  数归零自动销毁；**如果有捕获**，它继续活着。

正是这一点让闭包成为可能：

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
print(c())  # 1
print(c())  # 2
```

`make_counter` 返回后，包含 `n = 0` 的 env 没有消失——`inc` 这
个函数持有指向它的引用。每次 `c()` 都从同一个 env 里读写 `n`。

第 10 章我们会把这一切打通，`Function` 类内部就持有一个 `EnvRef
captured_env`。

## 7.5 一个常见陷阱：循环引用

如果两个 env 互相持有 shared_ptr，引用计数永远不归零，内存泄漏。
这种情况在 Mini 里**几乎不会发生**——env 链是树状的（每个 env 只
有一个 parent），不会成环。

但更高级的语言会出现这种情况：

```python
fn a()
    let f = nil
    fn b()
        f()      # b 引用 f，f 引用 b 所在的 env
    end
    f = b
    b()
end
```

这会让 env_a 与 b 的内部状态互相引用：

```
env_a ──持有 f──▶ Function b ──captured_env──▶ env_a
```

引用计数搞不定这种环。第 22 章我们用三色标记的真正 GC 解决这个
问题，本章先用 shared_ptr 凑合。

## 7.6 全局 env 的预热

REPL 启动时，我们要先把内置函数（`print`、`len`、`type` 等）注
册到全局 env：

```cpp
// 在 Interpreter 构造函数里
EnvRef Interpreter::make_global_env() {
    auto g = std::make_shared<Environment>();
    g->define("print",  Value(make_native_fn(builtin_print)));
    g->define("len",    Value(make_native_fn(builtin_len)));
    g->define("type",   Value(make_native_fn(builtin_type)));
    g->define("assert", Value(make_native_fn(builtin_assert)));
    return g;
}
```

`make_native_fn` / `Function` 是第 10、12 章的内容，这里只展示一
下用法的形状。

## 7.7 性能：vector vs unordered_map

`unordered_map<string, Value>` 在教学场景完全够用，但每次查找：

* 字符串 hash（O(n) of name length）；
* bucket 链表 walk；
* string 比较；

对于热路径上的局部变量，这其实**比预期慢得多**。Lua、Python、
GDScript 都做了一个优化：

> **编译期把局部变量名解析成"槽位下标"，运行时按下标读写。**

也就是说 `print(x + y + z)` 在编译期就被变成"读槽 0 的 x、读槽 1
的 y、读槽 2 的 z"。运行时是 `vector<Value>` 的 `[i]` 访问，O(1)
且 cache 友好。

我们树遍历版本**先不做这个优化**——先用 unordered_map 跑通。
等到第三部分字节码 VM，我们会顺便引入"局部变量槽位"这个概念，
那时 env 就不再用了，全部走 `stack[base + i]`。

但有个例外是值得提的：**`define` 完后立刻 `get` 的情况**。我们
本章的 hashmap 实现：

```cpp
void define(const std::string& name, Value v) {
    vars_[name] = std::move(v);
}
const Value& get(const std::string& name) const {
    auto it = vars_.find(name);
    if (it != vars_.end()) return it->second;
    // ...
}
```

这两个函数会做**两次** hash 计算。如果我们用 C++17 `try_emplace`
和 `find` 配合一个迭代器缓存，能少一次 hash——但完全不值得在第
一版做，先跑起来再说。

## 7.8 小结：env 是一个能往上爬的字典链

这一章的全部内容可以浓缩成一张图：

```
inner_env ─parent▶ outer_env ─parent▶ global_env ─parent▶ nullptr
   │                  │                   │
   ▼                  ▼                   ▼
 {z: 30}            {y: 20}      {x: 10, print: ..., len: ...}
```

加上四条规则：

1. `let x = v`：写当前 env；
2. `x = v`：从当前 env 沿链找到 `x`，写那一层；找不到报错；
3. `x`（读）：从当前 env 沿链找；找到就用；找不到报错；
4. 函数对象额外保留一个"声明它时的 env"作为闭包环境。

下一章我们就用这套机制实现表达式求值：把 AST 节点 visit 一遍，
把 `Value` 算出来。
