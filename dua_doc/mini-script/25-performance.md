# 第 25 章　性能：你能做的几件小事

到目前为止 Mini 的字节码 VM 大概是树遍历版本的 5~10 倍速度。
和 Lua 5.4 比还差一个数量级，和 LuaJIT / V8 差三个数量级——但
**别忙着上 JIT**，先把基础优化做满。

实测在 fib(30) 这种 micro-benchmark 上，把这一章的优化全做完，
Mini 能从 1.5 秒降到 0.4 秒——单单"基本功"就能再提升 3-4
倍。

本章按"性价比从高到低"排：

1. 字符串 interning（已经做了，回顾）；
2. 栈预分配；
3. Local 变量解析直接用 slot index 而不是名字；
4. 内联缓存（IC）：global / table 字段加速；
5. NaN-boxing：Value 从 16-byte 压到 8-byte；
6. opcode 融合（superinstruction）；
7. JIT（点到为止）。

## 25.1 字符串 interning（回顾）

第 16 章已经实现：所有 string 通过 `string_intern_table` 唯一化，
比较 = 指针比较。Mini Table 的 hash key 全是 string 时性能特别
受益。

实测：fib 这种没有大量 string 的 benchmark 提升 5%，但模板渲染、
JSON parse 这种 string-heavy workload 能提升 30%+。

## 25.2 栈预分配：vector → 数组

```cpp
// 之前
std::vector<Value> stack_;

// 之后
static constexpr int STACK_MAX = 1 << 16;   // 64K slots
Value stack_[STACK_MAX];
Value* sp_ = stack_;     // top-of-stack 指针
```

收益：

* 没有 push_back 触发 realloc 的检查；
* push/pop 是 `*sp_++ = v` / `*--sp_`——直接指针运算；
* sp_ 是个寄存器变量，编译器更容易优化；
* upvalue 指向栈位置不会因为 realloc 失效（第 19 章那个坑直接消
  失）。

代价：固定 64K × `sizeof(Value)` ≈ 1MB 内存常驻——服务器场景
要小心，但单脚本/游戏场景完全可以接受。

测得 OP_LOAD_LOCAL / OP_STORE_LOCAL 这两条最热指令快约 20%。

## 25.3 Local 解析：编译期固定 slot

第 15 章已经做了——每个 local 编译期分配 slot 0/1/2…，运行期
`OP_LOAD_LOCAL slot` 直接 `stack[base + slot]`。这是从 tree-walking
版本到 bytecode 版本的最大单一加速来源。

回顾对比：

```cpp
// tree-walking: O(scope深度) 字符串查找
Value get(string name) {
    for (env = current; env; env = env->parent) {
        auto it = env->vars.find(name);
        if (it != end) return it->second;
    }
    throw "undefined";
}

// bytecode: O(1) 数组下标
Value get_local(int slot) { return stack[base + slot]; }
```

但 GLOBAL 还慢——下一节解决。

## 25.4 内联缓存：global 加速

`OP_LOAD_GLOBAL` 现在每次都查 hash table：

```cpp
case OP_LOAD_GLOBAL: {
    const std::string& name = constants_[get_uarg(inst)].as_string();
    push(globals_->get(name));    // hash O(1) 但常数大
    break;
}
```

string hash + table find ≈ 30 周期。比 `stack[base+slot]` 的 1 周
期慢 30 倍。

**Inline cache (IC)**：第一次解析后，把"这个名字 → globals_ 表
里的下标"记到 opcode 旁边的"cache slot"里，下次直接用下标：

```cpp
struct GlobalCache {
    std::uint32_t version;   // globals_ 修改一次 +1
    int           index;     // 在 globals_ 内部数组里的下标
};
std::vector<GlobalCache> global_caches_;  // FunctionProto 每条 OP_LOAD_GLOBAL 一格

case OP_LOAD_GLOBAL: {
    int cache_slot = get_uarg(inst) >> 16;        // 高 16 位 = cache slot
    int name_const = get_uarg(inst) & 0xFFFF;     // 低 16 位 = 常量池下标
    auto& cache = global_caches_[cache_slot];
    if (cache.version == globals_->version()) {
        push(globals_->at_index(cache.index));
        break;
    }
    // miss: 走慢路径
    const std::string& name = constants_[name_const].as_string();
    int idx = globals_->find_index(name);
    cache.index = idx;
    cache.version = globals_->version();
    push(globals_->at_index(idx));
    break;
}
```

`globals_` 内部是 `vector<Value> + unordered_map<string, int>`：
slot 在数组里，map 给 string→slot 映射；version 在每次 define
时 +1。

效果：循环里反复读同一个 global，第一次后所有访问都是 1 cache
read + 1 array access ≈ 4 周期。**全局变量访问近似 local**。

CPython 3.11 引入的"specialized opcode" 是同样思路——
LOAD_GLOBAL 一旦稳定就 specialize 成 LOAD_GLOBAL_BUILTIN_CACHED。

GDScript 也走这条路，在 `gdscript_function.cpp` 你能看到大量
`address` 解析后缓存到 instruction 槽里。

## 25.5 NaN-boxing：Value 从 16 字节到 8 字节

我们的 `Value` 现在大约是：

```cpp
struct Value {
    Tag tag;        // 4 字节 + 4 padding
    union {         // 8 字节
        bool b; std::int64_t i; double d; GCObject* obj;
    };
};
// sizeof(Value) == 16
```

栈、Table、constants 池里塞的都是 `Value` 数组——栈一空就要写 64K
× 16 = 1MB。每条 push 是 16 字节内存写。

**NaN-boxing**：把所有 Value 压到 8 字节（一个 double 的位宽）。

观察：IEEE 754 double 的 NaN 有 2^52 种 bit pattern，但用户代
码只关心一种"NaN"——剩下 2^52 - 1 种 NaN 都是冗余空间，可以
拿来塞别的类型。

```cpp
//   sign: 1 bit | exponent: 11 bit | mantissa: 52 bit
//   QNaN: 0|11111111111|1xxxxxxxxx...
// 我们用最高 4 bit 当 type tag，剩 48 bit 当 payload（够装裸指针，x86_64 用户态指针实际只 47 bit）

union Value {
    double d;
    std::uint64_t bits;
};

constexpr std::uint64_t QNAN_MASK = 0x7FFC000000000000ull;
constexpr std::uint64_t TAG_NIL    = QNAN_MASK | 0x0001000000000000;
constexpr std::uint64_t TAG_TRUE   = QNAN_MASK | 0x0002000000000000;
constexpr std::uint64_t TAG_FALSE  = QNAN_MASK | 0x0003000000000000;
constexpr std::uint64_t TAG_INT    = QNAN_MASK | 0x0004000000000000;
constexpr std::uint64_t TAG_OBJ    = QNAN_MASK | 0x0008000000000000;

bool is_double(Value v) { return (v.bits & QNAN_MASK) != QNAN_MASK; }
bool is_obj(Value v) { return (v.bits & (QNAN_MASK | 0x000F000000000000)) ==
                              (QNAN_MASK | 0x0008000000000000); }
GCObject* as_obj(Value v) { return (GCObject*)(v.bits & 0x0000FFFFFFFFFFFFull); }
```

收益：

* `sizeof(Value) = 8`——栈占用减半，Table value 减半，constants
  减半；
* 每条 push/pop 写 8 字节而不是 16 字节——cache miss 减半；
* Lua 5.3+、SpiderMonkey、JavaScriptCore 全用了 NaN-boxing。

代价：

* 代码可读性差（位操作宏一堆）；
* int 只能存 47 位（≈ ±70 万亿），溢出 fallback 成 double。

Mini 教学版可以做成可选：默认走 `Tag + union` 易读版，开
`-DMINI_NAN_BOX=1` 切到 8 字节版。性能差 1.5x。

## 25.6 Opcode 融合：superinstructions

剖析 fib 的 trace 会看到这种序列高频出现：

```
OP_LOAD_LOCAL  0
OP_PUSH_INT    1
OP_SUB
```

dispatch 3 次 + 3 次栈操作 = 大约 15 周期，但实际"算 a-1"只
要 1 周期。如果合并成一条 superop：

```
OP_LOCAL_SUB_INT  slot=0, imm=1
```

编码：高 16 bit slot、低 16 bit imm，一次 dispatch、一次栈写：

```cpp
case OP_LOCAL_SUB_INT: {
    int slot = (get_uarg(inst) >> 16);
    int imm  = (get_uarg(inst) & 0xFFFF);
    *sp_++ = Value(stack_[base + slot].as_int() - imm);
    break;
}
```

挑选哪些 superop 合并要靠 profiling。CPython 3.11 引入了 200+
specialized opcode，这是 Python 史上最大单一性能跃升（30%）的来
源。

Mini 不必铺开做——但如果 profile 发现 fib 的瓶颈是 LOAD_LOCAL +
PUSH_INT + SUB 这三条，加一条 superop 就能让 fib 再快 20%。

## 25.7 Computed goto

第 17 章讨论过——GCC/Clang 上加 `&&label` + 分散 dispatch，能
让 CPU 分支预测命中率大幅提升。MSVC 不支持，所以我们 Mini 默
认 switch，在编译宏里提供 `USE_COMPUTED_GOTO` 切换。

实测在 GCC 上 computed goto 给 fib 提速 25%。

## 25.8 关于 JIT：要不要做

JIT (Just-In-Time compilation) 把热点字节码翻译成机器码。理论
上界是"和 C 同速"——LuaJIT、V8、HotSpot 全做了。

收益巨大（10-100x），但代价：

* JIT 自身代码量是 VM 的 5-10 倍——LuaJIT ≈ 5 万行，是 Lua 主
  解释器的 6 倍；
* 跨平台麻烦（x86_64 / arm64 各写一套 codegen，不然走 LLVM 增
  加 30MB 体积）；
* 安全 / 沙箱受影响（JIT 区域要 W^X 转换，在 iOS 等限制平台不
  能跑）；
* 启动时间增加；
* 调试工具链（gdb/lldb 看到的是 JIT 代码不是源码）。

**Mini 不做 JIT**。如果某天你真的需要：

* 第一阶段做 **template JIT**：每条 opcode 对应一段固定汇编，把
  字节码翻译成"汇编片段拼接"。HotSpot interpreter 就是这样起
  步的——3000 行汇编换 2x 速度；
* 第二阶段做 **method JIT**：以函数为单位，跟踪 hot 后整体编译
  成机器码。LuaJIT 做的是这种 + trace JIT。

但说实话：把第 25 章前面 6 节的优化全做完，Mini 已经够用绝大
多数嵌入场景。先优化字节码层再考虑 JIT。

## 25.9 性能调优工作流

对你写的解释器，建议工作流：

1. 跑 benchmark 集合（fib、quicksort、json parse、模板渲染……）
   建立基线；
2. perf / Instruments 找 hottest 的 op，看是 dispatch 慢还是真
   工作慢；
3. 对 hot op 做 specialization（IC、superinstruction）；
4. 隔几天重新 profile——优化会让 hottest 变化（之前的 hot 解决
   了，新 hot 暴露出来）；
5. 性能图表对比，如果变慢能 git bisect。

CPython 3.11 团队的 PEP 659 文档值得一读——他们把每一项 specia-
lization 的收益数据公开了，能学到"如何用数据驱动优化"。

## 25.10 第 25 章小结

按性价比从高到低：

| 优化 | 实现复杂度 | 预期提速 |
|------|-----------|---------|
| 编译期 local slot | 中 | 5-10x（已做） |
| 栈预分配 | 低 | 1.2x |
| 字符串 interning | 低 | 1.05-1.3x（已做） |
| Inline cache (global / 字段) | 中 | 1.5-2x |
| Computed goto | 低（仅 GCC/Clang） | 1.2-1.4x |
| NaN-boxing | 中 | 1.3-1.5x |
| Superinstructions | 中-高 | 1.2-1.5x（按 workload） |
| JIT | 极高 | 5-50x |

通常做完前 5 条 + 选择性几条 superinstruction，能让 Mini 达到
Lua 5.4 的 50-70% 速度——已经是非常体面的脚本语言了。

下一章 **第 26 章 继续走下去**：练习题、扩展方向、推荐阅读。
