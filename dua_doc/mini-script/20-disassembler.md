# 第 20 章　反汇编器与调试支持

VM 跑通后，下一个需求是**看清楚它在跑什么**。Tree-walking 时
代你能直接打印 AST 节点；切到 bytecode 后所有逻辑都被翻译成 30
条 opcode 的密集编码——不会反汇编就只能盲调。

这一章我们做三件事：

1. 写一个 **disassembler**，把 `FunctionProto` dump 成可读的人
   类汇编；
2. 暴露一个 `--dump` CLI 选项，便于 review 编译结果；
3. 加最简单的 **trace 模式**：每条指令执行前打印当前指令、栈
   顶状态——肉眼看 bug 利器。

## 20.1 disassemble_proto

```cpp
// src/disassembler.h
std::string disassemble(const FunctionProto& proto);

// src/disassembler.cpp
static int disassemble_inst(const FunctionProto& p, int ip, std::ostream& out) {
    std::uint32_t inst = p.code[ip];
    Opcode op = get_op(inst);
    out << std::setw(4) << std::setfill('0') << ip << "  ";
    out << std::setw(20) << std::left << opcode_name(op) << " ";

    switch (op) {
        case OP_PUSH_INT:
            out << get_sarg(inst);
            break;

        case OP_LOAD_CONST:
        case OP_LOAD_GLOBAL:
        case OP_STORE_GLOBAL:
        case OP_DEFINE_GLOBAL: {
            int k = get_uarg(inst);
            out << k << "  ; " << p.constants[k].repr();
            break;
        }

        case OP_LOAD_LOCAL:
        case OP_STORE_LOCAL:
        case OP_STORE_LOCAL_POP:
        case OP_LOAD_UPVALUE:
        case OP_STORE_UPVALUE:
        case OP_CLOSE_UPVALUE:
            out << get_uarg(inst);
            break;

        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_FALSE_POP: {
            int off = get_sarg(inst);
            out << (off >= 0 ? "+" : "") << off
                << "  -> " << (ip + 1 + off);
            break;
        }

        case OP_CALL:
            out << "argc=" << get_uarg(inst);
            break;

        case OP_CLOSURE: {
            int k = get_uarg(inst);
            auto sub = p.constants[k].as_proto();
            out << k << "  ; <fn " << (sub->name.empty() ? "?" : sub->name) << ">";
            break;
        }

        case OP_LINE:
            out << "(line " << get_sarg(inst) << ")";
            break;

        default:
            // 无参数 op
            break;
    }
    out << "\n";
    return ip + 1;
}

std::string disassemble(const FunctionProto& proto) {
    std::ostringstream out;
    out << "=== fn " << (proto.name.empty() ? "<main>" : proto.name)
        << " (params=" << proto.num_params
        << ", locals=" << proto.num_locals
        << ", upvalues=" << proto.upvalue_descs.size()
        << ") ===\n";

    for (int ip = 0; ip < (int)proto.code.size(); ) {
        ip = disassemble_inst(proto, ip, out);
    }

    out << "\n--- constants ---\n";
    for (size_t i = 0; i < proto.constants.size(); ++i) {
        out << "  [" << i << "] " << proto.constants[i].repr() << "\n";
    }

    out << "\n";
    // 递归 dump 嵌套函数
    for (auto& c : proto.constants) {
        if (c.is_proto()) out << disassemble(*c.as_proto());
    }
    return out.str();
}
```

跑一下，输入：

```python
fn add(a, b)
    return a + b
end

let x = add(1, 2)
print(x)
```

dump 结果：

```
=== fn <main> (params=0, locals=2, upvalues=0) ===
0000  OP_LINE              (line 1)
0001  OP_CLOSURE           0  ; <fn add>
0002  OP_DEFINE_GLOBAL     1  ; "add"
0003  OP_LINE              (line 5)
0004  OP_LOAD_GLOBAL       1  ; "add"
0005  OP_PUSH_INT          1
0006  OP_PUSH_INT          2
0007  OP_CALL              argc=2
0008  OP_STORE_LOCAL_POP   0
0009  OP_LINE              (line 6)
0010  OP_LOAD_GLOBAL       2  ; "print"
0011  OP_LOAD_LOCAL        0
0012  OP_CALL              argc=1
0013  OP_POP
0014  OP_RETURN_VOID

--- constants ---
  [0] <fn add>
  [1] "add"
  [2] "print"

=== fn add (params=2, locals=2, upvalues=0) ===
0000  OP_LINE              (line 2)
0001  OP_LOAD_LOCAL        0
0002  OP_LOAD_LOCAL        1
0003  OP_ADD
0004  OP_RETURN

--- constants ---
```

每条指令有 ip、op 名、参数；jump 自动算出**绝对目标地址**；
constants 池单独列出。这是看 compiler 输出对不对的标准工具。

## 20.2 CLI 入口

```cpp
// src/main.cpp
int main(int argc, char** argv) {
    bool dump = false;
    bool trace = false;
    std::string source_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--dump") dump = true;
        else if (a == "--trace") trace = true;
        else if (a == "-h" || a == "--help") { print_usage(); return 0; }
        else source_path = a;
    }

    if (source_path.empty()) { repl(); return 0; }

    std::string src = read_file(source_path);
    auto ast = parse(src);
    if (!ast) return 1;

    auto proto = compile(ast.get());
    if (!proto) return 1;

    if (dump) {
        std::cout << disassemble(*proto);
        return 0;
    }

    VM vm;
    register_builtins(vm);
    if (trace) vm.set_trace(true);
    try {
        vm.run(proto);
    } catch (const RuntimeError& e) {
        std::cerr << "runtime error: " << e.what() << "\n";
        return 2;
    }
    return 0;
}
```

`mini --dump file.mini` 直接 dump 不执行；`mini --trace file.mini`
带 trace 跑。

## 20.3 trace 模式

```cpp
// src/vm.h
class VM {
public:
    void set_trace(bool t) { trace_ = t; }
private:
    bool trace_ = false;
    void trace_step(const CallFrame& fr);
};

// src/vm.cpp
void VM::trace_step(const CallFrame& fr) {
    std::cerr << "[" << frames_.size() << "] ";
    disassemble_inst(*fr.proto, fr.ip, std::cerr);
    std::cerr << "    stack: [";
    for (size_t i = 0; i < stack_.size(); ++i) {
        if (i) std::cerr << ", ";
        std::cerr << stack_[i].repr();
    }
    std::cerr << "]\n";
}
```

主循环开头：

```cpp
while (true) {
    CallFrame& fr = frames_.back();
    if (trace_) trace_step(fr);
    std::uint32_t inst = fr.code[fr.ip++];
    // ...
}
```

跑 `mini --trace fib.mini` 你会看到几千行类似：

```
[1] 0001  OP_LOAD_LOCAL  0
    stack: [<closure fib>, 5, 5]
[1] 0002  OP_PUSH_INT    2
    stack: [<closure fib>, 5, 5, 2]
[1] 0003  OP_LT
    stack: [<closure fib>, 5, false]
[1] 0004  OP_JUMP_IF_FALSE_POP +5  -> 9
    stack: [<closure fib>, 5]
```

Bug 时立刻能看清"哪条指令操作了不该操作的栈顶"。

## 20.4 断点：实验性

简单加一个 `OP_BREAK`——compiler 不发射它，但提供 API
`set_breakpoint(file, line)`：

```cpp
class VM {
public:
    void set_breakpoint(int line) { breakpoints_.insert(line); }
private:
    std::unordered_set<int> breakpoints_;
};
```

每次执行 `OP_LINE` 时检查：

```cpp
case OP_LINE: {
    int line = get_sarg(inst);
    current_line_ = line;
    if (breakpoints_.count(line)) {
        prompt_debugger();   // 进 mini-debugger REPL
    }
    break;
}
```

`prompt_debugger` 是个简单 REPL：
* `c`（continue）→ 继续；
* `s`（step）→ 设单步标志，下一条 OP_LINE 也停；
* `bt`（backtrace）→ 打 frames_ 栈；
* `p name` → 找 local 或 upvalue 或 global，打值；
* `q` → 异常退出。

200 行 C++ 就有了一个最低限度的 debugger。GDB / LLDB 也是这样
起步的。Mini 不展开实现细节——属于读者作业。

## 20.5 反汇编与编辑器联动

如果做 IDE/editor plugin，反汇编通常通过 LSP 的"Show
Disassembly"自定义命令暴露：

* IDE 发 `mini/disassemble {uri}` JSON-RPC；
* mini-lsp server 拿到，调 `compile + disassemble`，把字符串
  返回；
* IDE 在新 tab 显示。

GDScript 的 `--gdscript-docclass` / `--script ... --check-only`
是类似的"内省"接口——把内部数据结构 dump 成命令行可见格式。

## 20.6 源码 → 行号 → ip 的反向映射

带 trace 时我们已经在用 `OP_LINE` 当源映射。但 release 构建为
了省字节码，会把 `OP_LINE` 编进**单独的一张 line table**而不是
插在指令流里：

```cpp
struct LineEntry {
    int ip;
    int line;
};
struct FunctionProto {
    // ...
    std::vector<LineEntry> line_info;  // 升序，按 ip
};

int lookup_line(const FunctionProto& p, int ip) {
    auto it = std::upper_bound(p.line_info.begin(), p.line_info.end(),
        ip, [](int v, const LineEntry& e){ return v < e.ip; });
    if (it == p.line_info.begin()) return -1;
    return (it - 1)->line;
}
```

`ip → line` 二分查找 O(log n)。

CPython `co_lnotab` / Lua `lineinfo` / GDScript `_lines` 都是这
个套路——带行号的同时不污染指令流。

## 20.7 第 20 章小结

* **disassembler** 把 `FunctionProto` dump 成人类可读汇编，是
  调试 compiler 的第一工具；
* CLI 加 `--dump` / `--trace`，前者只编不跑，后者带每步打印；
* trace 模式把每条指令前的栈状态打出，bug 难逃；
* 断点机制借助 `OP_LINE` 可以小成本添加；
* 行号信息与指令流分离能省 release 体积，二分查 ip→line。

下一章 **第 21 章 Native 函数与 FFI**：怎么注册 print、
math.sqrt、io.read 这种从 C++ 暴露的 builtin，让 Mini 真正能
"做事"。
