# 第 17 章　VM 主循环：dispatch 的几种写法

到了把 bytecode "**跑**"起来的时候。VM 主循环就是一个 fetch-
decode-execute 大 switch，但写法上有几种取舍能影响 30~50% 的性
能。这一章我们：

1. 写一个最直白的 switch 版本，跑通；
2. 介绍 `computed goto`（GCC 扩展），看是怎么再快的；
3. 讨论尾调用、寄存器缓存、热 op 内联等优化方向。

完成本章后 Mini 字节码 VM 就能跑表达式与控制流——但函数调用要
等下一章 Call Frame。

## 17.1 VM 类骨架

```cpp
// src/vm.h
#pragma once
#include "function_proto.h"
#include "value.h"
#include <vector>
#include <unordered_map>

namespace mini {

class VM {
public:
    VM();
    Value run(std::shared_ptr<FunctionProto> main);

    Environment* globals() { return globals_.get(); }   // 给 builtin 注册用

private:
    // 操作数栈
    std::vector<Value> stack_;
    // 全局表（直接用 env 凑合，名字 → Value）
    std::shared_ptr<Environment> globals_;

    // 当前调用上下文（第 18 章会改成 CallFrame 栈）
    const std::uint32_t* code_ = nullptr;
    int ip_ = 0;
    int code_size_ = 0;
    const Value* constants_ = nullptr;
    int local_base_ = 0;            // 当前栈帧 local 的起始位置

    // 调试
    int current_line_ = 0;

    // helpers
    void push(Value v) { stack_.push_back(std::move(v)); }
    Value pop() {
        Value v = std::move(stack_.back());
        stack_.pop_back();
        return v;
    }
    Value& top() { return stack_.back(); }
    Value& peek(int n) { return stack_[stack_.size() - 1 - n]; }

    [[noreturn]] void runtime_error(const std::string& msg);
};

}  // namespace mini
```

几个设计决策：

* **`stack_` 是 `vector<Value>`**——简单，能 grow；性能版会预分
  配 8MB 直接寻址；
* **`code_` / `ip_` 等是裸指针/索引**——主循环里不停访问，必须
  无开销；
* **本章先用单一栈帧**——下一章引入 CallFrame 后这些字段会
  迁移到 frame 里。

## 17.2 主循环：switch 版本

最朴素的写法：

```cpp
// src/vm.cpp
Value VM::run(std::shared_ptr<FunctionProto> main) {
    code_ = main->code.data();
    code_size_ = static_cast<int>(main->code.size());
    constants_ = main->constants.data();
    ip_ = 0;
    local_base_ = static_cast<int>(stack_.size());

    // 给 main 预留 num_locals 个槽
    stack_.resize(local_base_ + main->num_locals, Value());

    while (true) {
        std::uint32_t inst = code_[ip_++];
        Opcode op = get_op(inst);

        switch (op) {
            case OP_PUSH_NIL:    push(Value()); break;
            case OP_PUSH_TRUE:   push(Value(true)); break;
            case OP_PUSH_FALSE:  push(Value(false)); break;
            case OP_PUSH_INT:    push(Value(static_cast<std::int64_t>(get_sarg(inst)))); break;

            case OP_LOAD_CONST: {
                push(constants_[get_uarg(inst)]);
                break;
            }

            case OP_POP: stack_.pop_back(); break;

            case OP_LOAD_LOCAL: {
                push(stack_[local_base_ + get_uarg(inst)]);
                break;
            }
            case OP_STORE_LOCAL: {
                stack_[local_base_ + get_uarg(inst)] = top();
                break;
            }
            case OP_STORE_LOCAL_POP: {
                stack_[local_base_ + get_uarg(inst)] = std::move(top());
                stack_.pop_back();
                break;
            }

            case OP_LOAD_GLOBAL: {
                const std::string& name = constants_[get_uarg(inst)].as_string();
                if (!globals_->has(name)) {
                    runtime_error("undefined global: " + name);
                }
                push(globals_->get(name));
                break;
            }
            case OP_STORE_GLOBAL: {
                const std::string& name = constants_[get_uarg(inst)].as_string();
                globals_->assign(name, top());
                break;
            }
            case OP_DEFINE_GLOBAL: {
                const std::string& name = constants_[get_uarg(inst)].as_string();
                globals_->define(name, std::move(top()));
                stack_.pop_back();
                break;
            }

            case OP_ADD: {
                Value b = pop(), a = pop();
                push(arith(a, b, OP_ADD));
                break;
            }
            // 类似的 OP_SUB / OP_MUL / OP_DIV / OP_MOD ...

            case OP_NEG: {
                Value a = pop();
                if (a.is_int())   push(Value(-a.as_int()));
                else if (a.is_float()) push(Value(-a.as_float()));
                else runtime_error("'-' requires number");
                break;
            }
            case OP_NOT: {
                Value a = pop();
                push(Value(!a.truthy()));
                break;
            }
            case OP_CONCAT: {
                Value b = pop(), a = pop();
                if (!a.is_string() || !b.is_string()) {
                    runtime_error("'..' expects strings");
                }
                push(Value(a.as_string() + b.as_string()));
                break;
            }

            case OP_EQ: { Value b = pop(), a = pop(); push(Value(a.equals(b))); break; }
            case OP_NE: { Value b = pop(), a = pop(); push(Value(!a.equals(b))); break; }
            case OP_LT: { Value b = pop(), a = pop(); push(Value(compare_lt(a, b))); break; }
            // ... LE / GT / GE 类似

            case OP_JUMP: {
                ip_ += get_sarg(inst);
                break;
            }
            case OP_JUMP_IF_FALSE: {
                if (!top().truthy()) ip_ += get_sarg(inst);
                break;
            }
            case OP_JUMP_IF_FALSE_POP: {
                Value v = pop();
                if (!v.truthy()) ip_ += get_sarg(inst);
                break;
            }

            case OP_NEW_TABLE: {
                push(Value(std::make_shared<Table>()));
                break;
            }
            case OP_GET_INDEX: {
                Value k = pop(), o = pop();
                if (o.is_table()) push(o.as_table()->get(k));
                else if (o.is_string()) {
                    if (!k.is_int()) runtime_error("string index must be int");
                    auto i = k.as_int();
                    const auto& s = o.as_string();
                    if (i < 0 || (std::size_t)i >= s.size())
                        runtime_error("string index out of range");
                    push(Value(std::string(1, s[i])));
                } else {
                    runtime_error("cannot index " + std::string(o.type_name()));
                }
                break;
            }
            case OP_SET_INDEX: {
                Value v = pop(), k = pop(), o = pop();
                if (!o.is_table()) runtime_error("can only index assign into table");
                o.as_table()->set(k, v);
                push(v);   // 留一个 v 在栈上作为 = 表达式的结果
                break;
            }

            case OP_LINE:
                current_line_ = get_sarg(inst);
                break;

            case OP_HALT:
                return stack_.empty() ? Value() : pop();

            case OP_RETURN:
                // 第 18 章实现真正的返回
                return stack_.empty() ? Value() : pop();

            case OP_CALL:
            case OP_CLOSURE:
            case OP_LOAD_UPVALUE:
            case OP_STORE_UPVALUE:
            case OP_CLOSE_UPVALUE:
                // 第 18、19 章实现
                runtime_error("not implemented yet");

            default:
                runtime_error("unknown opcode");
        }
    }
}
```

写完这一段（约 200 行）就可以跑：

```python
let s = 0
let i = 0
while i < 1000 do
    s = s + i
    i = i + 1
end
print(s)
```

呃，print 是函数调用，这章还跑不了。退一步：

```python
let s = 0
let i = 0
while i < 1000 do
    s = s + i
    i = i + 1
end
```

跑完这段，`s` 应该等于 499500。我们暂时无法 print，但可以在 VM
return 后从栈上读它。这就足以验证主循环是对的。

## 17.3 性能：switch 慢在哪

GCC / Clang / MSVC 对一个 ~30 个 case 的 switch，几乎肯定生成
**跳转表**（jump table）：

```asm
mov  rax, [rdi+ip*4]      ; fetch
shr  rax, 24              ; decode op
mov  rax, [jmp_table + rax*8]
jmp  rax                  ; dispatch
```

代价：

1. **CPU 分支预测器**面对单一 jmp 几乎总是 miss——它无法预测下
   一条 op 是哪个；
2. 每条 case 末尾都得 `jmp 主循环顶部`，再做一次 fetch+decode+
   dispatch；
3. 每次 dispatch ≈ 5 周期。

对 ADD 这种 1 周期就完事的 op，dispatch 比真正干活还慢。

## 17.4 Computed goto：消除一次跳转

GCC 与 Clang 支持的非标准扩展，叫"taken pointer to label"
（`&&label`）：

```cpp
static const void* dispatch_table[] = {
    &&L_PUSH_NIL, &&L_PUSH_TRUE, &&L_PUSH_FALSE, &&L_PUSH_INT,
    // ... 一一对应
};

#define DISPATCH() goto *dispatch_table[get_op(code_[ip_++])]

DISPATCH();

L_PUSH_NIL:   push(Value());                    DISPATCH();
L_PUSH_TRUE:  push(Value(true));                DISPATCH();
L_PUSH_INT:   push(Value(get_sarg(...)));       DISPATCH();
// ...
```

每条 op 自己负责 `DISPATCH()` 跳到下一条 op。**没有"主循环顶
部"**——dispatch 在每条 op 末尾分散发生。

CPU 分支预测器对**每个 op 出口**有一个独立的预测槽——某些 op
后面经常跟某些 op（比如 `LOAD_LOCAL` 后常跟 `LOAD_LOCAL` 或
`ADD`），预测命中率大幅提升。

实测在 Lua 上 computed goto 比 switch 快 20~40%。

GCC 支持，Clang 支持，**MSVC 不支持**——这是个 portability 痛
点。GDScript 用宏抽象：

```cpp
#ifdef __GNUC__
#define USE_COMPUTED_GOTO
#endif

#ifdef USE_COMPUTED_GOTO
#define OPCODE(op)   L_##op
#define DISPATCH()   goto *dispatch_table[get_op(code_[ip_++])]
#else
#define OPCODE(op)   case op
#define DISPATCH()   continue
#endif

while (true) {
    switch (get_op(code_[ip_++])) {
        OPCODE(OP_ADD): { ... } DISPATCH();
        // ...
    }
}
```

跑同一份代码，GCC 走 computed goto，MSVC 走 switch。我们 Mini
本书示例用最简单的 switch 版本——你在自己的工程里加宏切换即可。

## 17.5 几个进一步的小优化

### 优化 1：把 `ip` / `stack 顶` 拷贝到局部寄存器

```cpp
const uint32_t* code = code_;
const Value* consts = constants_;
int ip = ip_;
Value* sp = &stack_[stack_.size()];

while (true) {
    uint32_t inst = code[ip++];
    // ...
    case OP_LOAD_LOCAL: {
        *sp++ = stack_data[local_base + get_uarg(inst)];
        break;
    }
}
```

主循环开始把 hot 字段拷到局部变量——编译器更可能把它们放到寄存
器，避免每次 fetch 都重读 `this->ip_`。

代价：异常 / 函数调用时要把 ip 写回 `ip_`。Lua 用宏 `savestate()`
处理。

### 优化 2：栈顶热缓存

观察：很多 op 立刻消费栈顶又立刻 push 一个。比如 `ADD` 是 `pop;
pop; push`——三次内存访问都打到同一个栈顶位置。

如果把"栈顶值"缓存到一个**寄存器**，op 就只在最后一次 push 时
写回内存：

```cpp
// 栈顶缓存
Value tos;          // top-of-stack
bool has_tos = false;

case OP_LOAD_LOCAL: {
    if (has_tos) push(std::move(tos));
    tos = stack_[local_base_ + get_uarg(inst)];
    has_tos = true;
    break;
}
case OP_ADD: {
    Value b = std::move(tos);
    Value a = pop();
    tos = arith(a, b, OP_ADD);
    has_tos = true;
    break;
}
```

JVM 的 HotSpot template interpreter 用的就是这种思路。我们 Mini
不做——代码会变难读。

### 优化 3：opcode 融合（superinstruction）

经常连续出现的 op 序列可以合并成一条 superop：

* `LOAD_LOCAL + LOAD_LOCAL + ADD` → `ADD_LOCAL_LOCAL slot1 slot2`（写回栈顶）

CPython 3.11 引入了大量这种合并指令，是 Python 3.11 比 3.10 快
30% 的主要原因之一。我们 Mini 的指令集太小不值得做——但你做大
后这是可观的优化方向。

## 17.6 错误处理：异常 + 行号

```cpp
[[noreturn]] void VM::runtime_error(const std::string& msg) {
    throw RuntimeError(msg, current_line_);
}
```

`current_line_` 由 `OP_LINE` 不断更新。Compiler 在每条语句前发
射 `OP_LINE`，VM 跑到时记下来。出错时拿这个行号 throw。

代价：每条语句多一条 `OP_LINE`——bytecode 体积增加 20% 左右，
执行多 1 条 op。

可选：release 构建不发射 `OP_LINE`，错误信息只能给"指令偏移
0x12AF"；用户拿这个偏移去 disassembler 反查行号。这是 GDScript
的 release 行为。

## 17.7 第 17 章小结

到这里 VM 主循环已经能跑大部分非函数代码：

* 算术、逻辑、比较、字符串拼接；
* 局部变量读写；
* 全局变量读写；
* if / while / for 控制流；
* 数组与字典字面量、下标读写。

唯一缺的是函数调用——下一章。

我们写了 ~250 行 VM 代码（不含 helper 与 builtin），加上之前
~600 行 Compiler、~200 行常量池/符号管理。第三部分正在快速接近
完成。

下一章 **第 18 章 Call Frame** 会完成函数调用的最关键部分：
怎么在不依赖 C++ 递归的前提下让 Mini 函数互相调用、return 跨层
跳出。然后第 19 章上闭包，第 20 章给反汇编器，VM 完整就位。
