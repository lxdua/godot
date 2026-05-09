# 第 18 章　调用栈帧：CallFrame

VM 主循环跑到现在，碰到 `OP_CALL` 还是直接 `runtime_error`。
本章把这个坑填上。核心问题：

* 一个 Mini 函数被调用时，要切换 `code` / `ip` / `constants`
  / `local_base` 这一组上下文；
* `return` 时要恢复调用方的上下文；
* 在原生函数（C++ builtin）和 Mini 函数之间能互相调用。

不能用 C++ 递归——递归会让每一层 Mini 调用消耗一段 C 栈，深递
归直接把宿主程序爆栈，而且不利于做协程。

解法：**显式调用帧栈** `vector<CallFrame>`，主循环从最顶层 frame
取上下文执行。这是 Lua/Python/JVM 都用的标准方案。

## 18.1 CallFrame 结构

```cpp
// src/vm.h（新增）
struct CallFrame {
    std::shared_ptr<FunctionProto> proto;   // 持有引用，避免被释放
    std::shared_ptr<Closure>       closure; // 含 upvalue（第 19 章）

    int ip = 0;             // 指令指针
    int local_base = 0;     // 本帧 local 0 在 stack_ 中的下标
    int stack_top_save = 0; // 调用前 stack 大小，return 时回滚

    // 缓存的解码用指针（避免每次都走 proto->...）
    const std::uint32_t* code = nullptr;
    const Value*         constants = nullptr;
    int                  code_size = 0;
};
```

`local_base` 是这一帧"局部变量第 0 个"在全局 `stack_` 里的位
置。所以本帧第 N 个局部 = `stack_[local_base + N]`。

`stack_top_save` 用于 return 时清掉本帧产生的所有临时值，
`stack_.resize(stack_top_save)`，再 push 返回值。

## 18.2 函数调用规约（Calling Convention）

我们采用**栈传参**：调用前 caller 按顺序 push 参数 + 函数对象，
然后发射 `OP_CALL argc`。VM 执行 `OP_CALL`：

```
栈布局（OP_CALL 执行前）：
  [..., callee, arg0, arg1, ..., arg_{n-1}]  ← top
                                ^^^^^^^^^^^^^
                                argc 个参数
```

这是 Lua 的方式。GDScript / CPython 也类似。

VM 处理流程：

1. 从栈顶向下数 `argc` 找到 callee；
2. 检查是 Closure（Mini 函数）还是 NativeFunction（C++ builtin）；
3. **Mini 函数**：
   - 创建新 CallFrame，`local_base = 当前栈大小 - argc`
     （这样 arg0..arg_{n-1} 自动成为 local 0..n-1）；
   - **callee 那个槽要被覆盖**——后续 frame return 时把返回值
     塞进这个槽，刚好让调用方栈干净；
   - resize stack 到 `local_base + num_locals` 给本帧腾足空间；
   - push 新 frame；主循环从新 frame fetch 下一条指令。
4. **Native 函数**：直接 C++ call，把结果写到 callee 槽，pop 掉
   args。

return 流程：

1. 取栈顶为返回值（如果 `OP_RETURN_VOID` 则用 `nil`）；
2. `stack_.resize(frame.local_base - 1)`——把 callee 槽连同
   args 一起截断；
3. push 返回值；
4. pop 当前 frame；如果 frame 栈空，返回到 host。

`local_base - 1` 那个 `-1` 就是 callee 槽——它在 caller 看来
就是 call 表达式的求值位置，被返回值替换。

## 18.3 frames 栈替换主循环

```cpp
// src/vm.h
class VM {
public:
    Value run(std::shared_ptr<FunctionProto> main);
private:
    std::vector<Value>      stack_;
    std::vector<CallFrame>  frames_;
    std::shared_ptr<Environment> globals_;
    int current_line_ = 0;
};
```

`run()` 入口构造第一个 frame（main 函数没有参数）：

```cpp
Value VM::run(std::shared_ptr<FunctionProto> main) {
    auto closure = std::make_shared<Closure>(main);
    CallFrame f;
    f.proto = main;
    f.closure = closure;
    f.code = main->code.data();
    f.code_size = static_cast<int>(main->code.size());
    f.constants = main->constants.data();
    f.local_base = 0;
    f.stack_top_save = 0;

    stack_.resize(main->num_locals, Value());
    frames_.push_back(std::move(f));

    return main_loop();
}
```

`main_loop()` 每条指令都引用"当前 frame"：

```cpp
Value VM::main_loop() {
    while (true) {
        CallFrame& fr = frames_.back();
        std::uint32_t inst = fr.code[fr.ip++];
        Opcode op = get_op(inst);

        switch (op) {
            case OP_LOAD_LOCAL:
                stack_.push_back(stack_[fr.local_base + get_uarg(inst)]);
                break;
            case OP_STORE_LOCAL:
                stack_[fr.local_base + get_uarg(inst)] = stack_.back();
                break;
            case OP_LOAD_CONST:
                stack_.push_back(fr.constants[get_uarg(inst)]);
                break;
            // ...

            case OP_CALL:
                do_call(get_uarg(inst));
                break;

            case OP_RETURN: {
                Value rv = stack_.back();
                do_return(rv);
                if (frames_.empty()) return rv;
                break;
            }
            case OP_RETURN_VOID: {
                do_return(Value());
                if (frames_.empty()) return Value();
                break;
            }
            // ...
        }
    }
}
```

注意 `CallFrame& fr = frames_.back();` 必须**每次循环重新取**
——因为 `do_call` / `do_return` 会改变 `frames_`，原引用就悬空
了。这是新手常见 bug。

性能版本会把 fr.ip / fr.code 拷到局部变量，在 call/return 边界
保存恢复。Mini 不做。

## 18.4 `do_call`

```cpp
void VM::do_call(int argc) {
    int callee_index = static_cast<int>(stack_.size()) - argc - 1;
    Value& callee = stack_[callee_index];

    if (callee.is_native()) {
        // C++ builtin
        auto native = callee.as_native();
        std::vector<Value> args;
        args.reserve(argc);
        for (int i = 0; i < argc; ++i) {
            args.push_back(std::move(stack_[callee_index + 1 + i]));
        }
        Value result;
        try {
            result = native->call(args);
        } catch (const std::exception& e) {
            runtime_error(std::string("native error: ") + e.what());
        }
        stack_.resize(callee_index);
        stack_.push_back(std::move(result));
        return;
    }

    if (!callee.is_closure()) {
        runtime_error("can only call functions, got " + std::string(callee.type_name()));
    }

    auto closure = callee.as_closure();
    auto proto = closure->proto;

    if (argc != proto->num_params) {
        runtime_error("function '" + proto->name + "' expects " +
            std::to_string(proto->num_params) + " args, got " +
            std::to_string(argc));
    }

    CallFrame nf;
    nf.proto = proto;
    nf.closure = closure;
    nf.code = proto->code.data();
    nf.code_size = static_cast<int>(proto->code.size());
    nf.constants = proto->constants.data();
    nf.local_base = callee_index + 1;     // arg0 现在的位置
    nf.stack_top_save = callee_index;     // 注意把 callee 槽也算进要清的范围
    nf.ip = 0;

    // 给非参数局部变量留空槽
    int needed = nf.local_base + proto->num_locals;
    if (static_cast<int>(stack_.size()) < needed) {
        stack_.resize(needed, Value());
    }

    frames_.push_back(std::move(nf));
}
```

* 参数检查：args 数 ≠ params 数直接报错。Lua 是宽松的（多 args
  忽略，少 args 补 nil），Mini 严格——简单且能尽早发现 bug；
* `local_base = callee_index + 1`：恰好让 arg0 ~ arg_{n-1} 落在
  本帧 local 0 ~ n-1 上，**零拷贝传参**；
* `num_locals` 包括了参数槽（参数本身就是声明在头部的局部）。

## 18.5 `do_return`

```cpp
void VM::do_return(Value rv) {
    CallFrame fr = std::move(frames_.back());
    frames_.pop_back();

    // 闭包关闭（第 19 章会展开）
    close_upvalues(fr.local_base);

    // 截断本帧及 callee 槽，再 push 返回值
    stack_.resize(fr.stack_top_save);
    stack_.push_back(std::move(rv));
}
```

`close_upvalues` 现在留空——下一章实现。

`stack_top_save` 是调用前 callee 在栈中的位置；resize 到那个位
置就抹掉了 callee + args + 本帧所有 local + 临时值，干净一刀；
然后 push 返回值——刚好填回 callee 那个槽。这样 caller 看到的
就是 "call 表达式 = 返回值"。

## 18.6 跑一段：递归 fib

```python
fn fib(n)
    if n < 2 then
        return n
    end
    return fib(n - 1) + fib(n - 2)
end

let result = fib(20)
```

VM 跑出来 `result == 6765`。验证：

* 函数对象作为 global 被 LOAD_GLOBAL 取出；
* OP_CALL 切换上下文，递归 22 层（Mini fib 树深度 = 20）；
* 每层 do_return 正确截断栈、回到 caller；
* 累加 `+` 结果上传到 caller 表达式槽。

如果跑 `fib(30)` ≈ 1.3M 次调用，Mini 大约 1.5 秒（树遍历版本约
8 秒）——bytecode 已经带来 **5x** 加速。

## 18.7 调用栈深度限制

无限递归会无限 push frames，最终 `vector` 重分配把内存占满。
必须设上限：

```cpp
static constexpr int MAX_FRAMES = 200;

void VM::do_call(int argc) {
    if ((int)frames_.size() >= MAX_FRAMES) {
        runtime_error("call stack overflow");
    }
    // ...
}
```

200 是个保守值——给递归算法（如 quicksort）留空间，但又不会让
错误的无限递归吃掉所有栈空间。Lua 默认 200，CPython 默认 1000。

## 18.8 错误的栈追踪（traceback）

报错时光说"call stack overflow"还不够，要打出栈帧序列：

```cpp
[[noreturn]] void VM::runtime_error(const std::string& msg) {
    std::string trace = msg + "\n";
    for (int i = (int)frames_.size() - 1; i >= 0; --i) {
        const auto& fr = frames_[i];
        trace += "  in " + (fr.proto->name.empty() ? "<main>" : fr.proto->name);
        // 找到当前 ip 对应的行号
        int line = lookup_line(fr.proto.get(), fr.ip);
        if (line > 0) trace += " (line " + std::to_string(line) + ")";
        trace += "\n";
    }
    throw RuntimeError(trace, current_line_);
}
```

`lookup_line` 用我们之前发射的 `OP_LINE` 表（compiler 顺便维护
一个 `vector<pair<int ip, int line>>`）二分查找。或者更省的方
案：每个 proto 都带一个 `line_info: vector<int>`，下标是 ip，
值是源行——空间换时间。

GDScript 用的是后者。Mini 这章我们暂用 `current_line_` 全局变
量（每条 OP_LINE 写一次），简单但跨 frame 不准——下章修。

## 18.9 第 18 章小结

完成了函数调用的核心：

* `CallFrame` 结构封装"一次函数调用的所有上下文"；
* `frames_` 栈替代 C++ 递归——不会爆 host 栈，便于以后做
  coroutine；
* `OP_CALL` / `OP_RETURN` 在 frames_ 上 push/pop；
* 参数通过栈直接成为新帧 local，零拷贝；
* native 函数和 Mini 函数走统一的 `OP_CALL` 入口，按 Value 类
  型分流。

跑 fib(20) 已经能验证一切都对。但还差一块：**闭包**。Mini 的函
数能捕获外层局部变量吗？需要 upvalue 机制——下章解决。

下一章 **第 19 章 Upvalue 与闭包**。
