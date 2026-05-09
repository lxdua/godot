# 第 16 章　常量池与符号解析

上一章我们用 `add_constant(Value)` 简单往池子里塞东西。这章把常
量池和符号系统认真梳理一下——它们是字节码 VM 的"数据存储层"，
决定了三件事：

* 字面量如何被找回（`OP_LOAD_CONST K`）；
* 全局变量按名字怎么解析（`OP_LOAD_GLOBAL K`）；
* 字符串能否复用同一份内存（intern）。

这些决策对 VM 的内存占用与查找速度影响巨大。

## 16.1 常量池的三类住户

每个 `FunctionProto` 拥有一个 `constants` 数组，里面通常装：

| 类别 | 来源 | 例子 |
| --- | --- | --- |
| **数字字面量**（超出 24-bit 范围） | `LOAD_CONST` | `1.5`、`10000000` |
| **字符串字面量** | `LOAD_CONST` | `"hello"` |
| **全局变量名** | `LOAD_GLOBAL` / `STORE_GLOBAL` | `print`、`fib` |
| **嵌套函数原型** | `OP_CLOSURE` | `fn() ... end` 的产物 |

把这四类一律塞进 `vector<Value>`——`Value` 已经能装数字、字符串、
TableRef、FunctionRef，多加一个 `ProtoRef` 就行。

> 注：FunctionProto 不是 Value 的合法成员（运行时拿到的是 Function，
> 不是 Proto）。我们要么把 Proto 包成 shared_ptr 单独存，要么扩
> 展 Value。后者更简单。

## 16.2 扩展 Value 接受 ProtoRef

```cpp
// src/value.h（增量）
class FunctionProto;
using ProtoRef = std::shared_ptr<FunctionProto>;

class Value {
public:
    using Variant = std::variant<
        Nil, bool, std::int64_t, double,
        StringRef, FunctionRef, TableRef,
        ProtoRef                              // ← 新增
    >;

    Value(ProtoRef p) : v_(std::move(p)) {}
    bool is_proto() const { return std::holds_alternative<ProtoRef>(v_); }
    const ProtoRef& as_proto() const { return std::get<ProtoRef>(v_); }
    // ...
};
```

`Proto` 在用户脚本里**永远不可见**——它只是 Compiler 与 VM 之间
的内部存储格式。`type()` 不需要新增一个 case，因为用户根本拿不到
它。

## 16.3 常量池去重：必须要做

我们 15 章 `add_constant` 已经做了 O(N) 线性查重。这看似浪费时
间，但**编译期一次性开销**，运行时零成本。

为什么必须去重？看一段循环：

```python
for i in 0..1000 do
    print("loop")
end
```

如果不去重，`"loop"` 这个字符串字面量被发射 1 次。但如果代码里
出现 100 次 `"loop"`，常量池会塞 100 个相同字符串——浪费内存，
也让全局查找时缓存命中率降低。

去重后，常量池只保留 1 份，所有 `LOAD_CONST` 都指向同一个 slot。
GDScript 也是这种设计，bytecode 反汇编时你会看到 `LOAD_CONST 7`
重复多次。

## 16.4 字符串 interning：再深一层的去重

去重在 `add_constant` 里做的是"**池内**去重"。还有更深一层——
**进程级**字符串 intern。

意思是：所有相同的字符串字面量（无论来自哪个函数、哪个 Proto）都
指向同一个 `shared_ptr<const string>`。

```cpp
// src/string_pool.h
#pragma once
#include "value.h"
#include <unordered_map>
#include <string>
#include <memory>

namespace mini {

class StringPool {
public:
    StringRef intern(const std::string& s) {
        auto it = pool_.find(s);
        if (it != pool_.end()) {
            if (auto sp = it->second.lock()) return sp;
            pool_.erase(it);
        }
        auto sp = std::make_shared<const std::string>(s);
        pool_[s] = sp;
        return sp;
    }
private:
    std::unordered_map<std::string, std::weak_ptr<const std::string>> pool_;
};

}  // namespace mini
```

* 用 `weak_ptr` 不"持有"字符串——等到所有使用者释放后字符串自
  动回收；
* `intern` 调用 `shared_from_this`-free 版本；
* Compiler 创建字符串字面量时一律走 intern，全局只有一份。

带来的好处：

1. **内存压缩**：同一个字符串字面量在多个函数里只占一份内存；
2. **`Value::equals` 极快**：相同 intern 的字符串 `shared_ptr ==`
   是 O(1)，不需要走内容比较快路径；
3. **table key 命中率高**：用字符串当 key 时 hash 一致 + intern
   后 `==` 是 1 ns；
4. **未来 GC 友好**：所有字符串集中管理，便于做"字符串去重 GC"。

Lua、Python 都做这件事。Python 的 `intern()` 函数甚至暴露给用户
显式调用——常用字典的 key 强制 intern。

## 16.5 全局变量怎么找

现状：`OP_LOAD_GLOBAL K`，`K` 是常量池里全局名（字符串）的下
标。VM 执行时：

```cpp
case OP_LOAD_GLOBAL: {
    int k = get_uarg(inst);
    const Value& name = constants[k];
    auto it = globals.find(name.as_string());
    if (it == globals.end()) {
        runtime_error("undefined global: " + name.as_string());
    }
    push(it->second);
    break;
}
```

这里**每次**都做一次字符串 hash + map 查找。在循环里读全局是
hot path，开销不小。

### 优化路径 1：global cache

Lua 5 / V8 做的：在 `OP_LOAD_GLOBAL` 指令旁边记一个**inline cache**：

```
OP_LOAD_GLOBAL K       ; arg = 常量池下标
[cache: 8 字节]         ; 上次查找的全局表指针 + slot
```

下次执行同一条指令时，先看 cache 是否仍有效（全局表有没有被改）；
是则直接 O(1) 取值。

我们 Mini 不做这个——加 50 行代码 + 一个版本号机制。教学价值
不如代码长度。

### 优化路径 2：编译期把全局变量"绑定到下标"

如果整个 program 在编译完之前就知道所有全局名（typical 脚本就
是如此），可以把全局也做成一个数组，按下标访问：

```
OP_LOAD_GLOBAL_FAST 5   ; 第 5 号全局
```

代价是不能在运行时动态新增全局——但绝大多数脚本不这么干。GDScript
的 named_globals 就是这种思路。

我们 Mini 的全局表保持动态——简单，符合 Lua/JS 风格。

## 16.6 line_info：源码位置的紧凑存储

每条 bytecode 都对应**一条**源码行。我们 15 章的简单做法：

```cpp
std::vector<int> line_info;   // line_info[i] 对应 code[i] 的行号
```

每条指令多 4 字节——bytecode 体积翻倍。Lua 以前也是这样，5.3
之后改成了**run-length encoding**：

```
[行号, 长度, 行号, 长度, ...]
[1, 5, 2, 8, 5, 2]
```

意思是"前 5 条指令在第 1 行，接下来 8 条在第 2 行"。绝大多数
源码行对应的 bytecode 是连续的——RLE 能把 line_info 压缩到原来
的 1/3~1/5。

我们教学版**先用简单数组**——内存浪费但代码 5 行写完。第 23 章
性能小节会顺便给出 RLE 版本作为练习。

## 16.7 嵌套函数的 Proto 嵌套

考虑：

```python
fn outer()
    fn inner()
        return 1
    end
    return inner
end
```

Compiler 编译时：

* `outer.proto.constants[0]` = `inner.proto`（一个 ProtoRef）；
* `outer.proto.code` 里有 `OP_CLOSURE 0` —— "用 0 号常量当函数
  原型，构造一个 Function"；
* `inner.proto.code` 里有 `OP_PUSH_INT 1; OP_RETURN`。

整棵 Proto 树是嵌套的——但**字节码本身是扁平的**，每个 Proto
内部是一段连续 bytecode。这个区分很重要：

* AST 是树（节点散落堆上）；
* Proto 是 N 棵小线性 bytecode 数组组成的森林（每个内部连续）。

VM 不会"递归遍历 Proto 树"——它只在 `OP_CLOSURE` 时拿出一个 Proto
实例化成 Function，然后**调用**该 Function 时切到那个 Proto 的
code 跑。

## 16.8 num_params / num_locals 是怎么算出来的

`num_params` 一目了然——`fn(a, b, c)` 编译时看 params.size。

`num_locals` 略微 tricky：它要在 Compiler 整个函数体扫完之后才
知道——因为不同分支可能声明不同数量的 local，我们要的是**任意
时刻栈深的最大值**。15.14 节 `declare_local` 里：

```cpp
if (slot + 1 > cur_->proto->num_locals) {
    cur_->proto->num_locals = slot + 1;
}
```

每次 declare 都更新最大值。运行时 VM 用这个数预留栈空间——避免
在循环里频繁 grow vector。

## 16.9 一个易错点：常量池查重对 ProtoRef

我们 `add_constant` 里 `equals` 比较时，对 ProtoRef 走指针比较。
两个不同的 inner 函数即使代码完全相同，也会被识别为不同的常量。

这是**对的**——同一个 `fn() ... end` 在 Compiler 里只出现一次，
不可能出现两个相同的 Proto。如果你硬要识别"等价的 Proto 然后合
并"，是 LTO 级别的优化，超出本书范围。

## 16.10 调试输出：常量池可视化

```cpp
void dump_constants(const FunctionProto& p) {
    std::cout << "constants:\n";
    for (std::size_t i = 0; i < p.constants.size(); ++i) {
        std::cout << "  " << i << ": ";
        const Value& v = p.constants[i];
        if (v.is_proto()) {
            std::cout << "<proto " << v.as_proto()->name << ">";
        } else {
            std::cout << v;
        }
        std::cout << "\n";
    }
}
```

跑 fib 函数：

```
constants:
  0: 2
  1: "fib"
  2: 1
  3: "fib"     ← 噢，重复了
```

如果看到 `1` 和 `3` 都是 `"fib"`，说明你忘了去重——回去检查
`add_constant` 的 equals 是否对字符串走内容比较。这就是为什么
这种调试工具值得早写：肉眼能看出 5 秒不写要 debug 1 小时的 bug。

## 16.11 一组相关单测

```cpp
TEST(Constants, IntegerLiteralAreInlined) {
    auto proto = compile("let x = 1 + 2\n");
    EXPECT_EQ(proto->constants.size(), 0u);   // 1 和 2 都走 PUSH_INT
}

TEST(Constants, BigIntGoesToPool) {
    auto proto = compile("let x = 10000000000\n");  // > 2^33
    EXPECT_EQ(proto->constants.size(), 1u);
}

TEST(Constants, FloatGoesToPool) {
    auto proto = compile("let x = 1.5\n");
    EXPECT_EQ(proto->constants.size(), 1u);
}

TEST(Constants, StringDeduplicated) {
    auto proto = compile(
        "print(\"hi\")\nprint(\"hi\")\nprint(\"hi\")\n");
    // print 一次 + "hi" 一次 = 2 个常量
    EXPECT_EQ(proto->constants.size(), 2u);
}

TEST(Constants, GlobalNameAddedOnce) {
    auto proto = compile(
        "let f = print\nf(1)\nf(2)\n");
    // print 仍只一次
    int print_count = 0;
    for (const auto& c : proto->constants) {
        if (c.is_string() && c.as_string() == "print") print_count++;
    }
    EXPECT_EQ(print_count, 1);
}
```

测试 `add_constant` 的去重比测试 VM 简单——你只需要看常量池规
模就能验证。

## 16.12 一句话总结

* 常量池是**Compiler 与 VM 之间的内存数据共享**——bytecode 用
  下标引用 Value；
* 池内**必须去重**，否则膨胀；
* 字符串建议**进程级 intern**，让相等性查询是指针比较；
* 全局变量用名字常量 + 全局 hashmap 实现，可加 inline cache 优化
  （第一版不做）；
* line_info 教学版用简单数组，后续可做 RLE 压缩。

下一章我们终于把 VM 主循环写出来——所有这些常量与字节码会**真
正运行起来**。
