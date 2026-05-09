# 第 15 章　Compiler：从 AST 到字节码

Compiler 的工作：**把 AST visit 一遍，发射 opcode**。

栈式 VM 的好处在这里特别明显——大部分 visit 函数就是"求左子，
求右子，发射运算符"，几乎不用思考。

写完这一章我们能把 Mini 脚本编译成 bytecode，但还没法运行——
VM 主循环是第 17 章的事。本章要保持的不变量是：**编译完的
bytecode 应该能被反汇编出可读形式（第 20 章）**。

## 15.1 Compiler 的核心数据结构

Compiler 一次只编译一个**函数**——每遇到一个 `FnExpr` 就嵌套
一个新的 Compiler 上下文。整段 program 是一个特殊的"主函数"。

```cpp
// src/compiler.h
#pragma once
#include "ast.h"
#include "function_proto.h"
#include <unordered_map>
#include <vector>
#include <memory>

namespace mini {

class Compiler : public AstVisitor {
public:
    Compiler();

    // 入口：把整个 program 编译成一个"主函数"
    std::shared_ptr<FunctionProto> compile(Program& p);

private:
    // 当前正在编译的函数
    struct Scope {
        std::shared_ptr<FunctionProto> proto;
        std::vector<std::string> locals;        // local 名 → slot 由下标决定
        std::vector<int> scope_depth;           // 每个 local 在哪一层 block
        int current_depth = 0;
        // upvalue 第 19 章再加
        std::shared_ptr<Scope> parent;
    };
    std::shared_ptr<Scope> cur_;

    // 发射 opcode
    int emit(Opcode op, std::int32_t arg = 0);   // 返回这条指令的偏移
    void patch_jump(int instr_offset, std::int32_t to);

    // 常量池
    int add_constant(Value v);
    int add_string_constant(const std::string& s);  // 复用相同字符串

    // 局部变量管理
    int declare_local(const std::string& name);
    int resolve_local(const std::string& name);     // 返回 slot 或 -1
    void enter_block();
    void leave_block();

    // visitor（只列分类，每个都是简短实现）
    void visit(NumberLit&)  override;
    void visit(StringLit&)  override;
    void visit(BoolLit&)    override;
    void visit(NilLit&)     override;
    void visit(IdentExpr&)  override;
    void visit(BinaryOp&)   override;
    void visit(UnaryOp&)    override;
    void visit(LogicalOp&)  override;
    void visit(AssignExpr&) override;
    void visit(CallExpr&)   override;
    void visit(IndexExpr&)  override;
    void visit(ArrayLit&)   override;
    void visit(TableLit&)   override;
    void visit(FnExpr&)     override;
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

## 15.2 FunctionProto：编译产物

```cpp
// src/function_proto.h
#pragma once
#include "value.h"
#include <vector>
#include <string>
#include <memory>

namespace mini {

// 编译期产物：函数原型（运行时配合 upvalue 实例化为 Function）
struct FunctionProto {
    std::vector<std::uint32_t> code;      // 字节码
    std::vector<Value> constants;         // 常量池
    std::vector<int> line_info;           // code[i] 对应的源码行
    int num_params = 0;
    int num_locals = 0;                   // 包含参数
    int num_upvalues = 0;                 // 第 19 章
    std::string name;                     // 调试用
    std::string source;                   // 调试用
};

}  // namespace mini
```

每个函数有自己的 `FunctionProto`。嵌套函数的 proto 也作为父函数
常量池里的一个 Value——这样 `OP_CLOSURE arg=K` 就能从常量池取出。

## 15.3 几个基础工具

```cpp
int Compiler::emit(Opcode op, std::int32_t arg) {
    int off = static_cast<int>(cur_->proto->code.size());
    cur_->proto->code.push_back(encode(op, arg));
    cur_->proto->line_info.push_back(current_line_);  // 后面 visit 时维护
    return off;
}

int Compiler::add_constant(Value v) {
    auto& c = cur_->proto->constants;
    // 简单线性查重，规模小不在乎
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (c[i].equals(v)) return static_cast<int>(i);
    }
    c.push_back(std::move(v));
    return static_cast<int>(c.size() - 1);
}

int Compiler::add_string_constant(const std::string& s) {
    return add_constant(Value(s));
}

void Compiler::patch_jump(int instr_offset, std::int32_t to) {
    auto& code = cur_->proto->code;
    Opcode op = get_op(code[instr_offset]);
    std::int32_t rel = to - instr_offset - 1;
    code[instr_offset] = encode(op, rel);
}
```

`patch_jump` 是 Compiler 里最常用的小工具——发射跳转时不知道目
的地，先发射一个占位 0，等知道目的地后回填。这是单趟编译器的
经典套路。

## 15.4 字面量

```cpp
void Compiler::visit(NumberLit& n) {
    if (n.is_int && n.ivalue >= -8388608 && n.ivalue <= 8388607) {
        // 24-bit 立即数快路径
        emit(OP_PUSH_INT, static_cast<std::int32_t>(n.ivalue));
    } else if (n.is_int) {
        emit(OP_LOAD_CONST, add_constant(Value(n.ivalue)));
    } else {
        emit(OP_LOAD_CONST, add_constant(Value(n.fvalue)));
    }
}

void Compiler::visit(StringLit& s) {
    emit(OP_LOAD_CONST, add_string_constant(s.value));
}

void Compiler::visit(BoolLit& b) {
    emit(b.value ? OP_PUSH_TRUE : OP_PUSH_FALSE);
}

void Compiler::visit(NilLit&) {
    emit(OP_PUSH_NIL);
}
```

`PUSH_INT` 的 24-bit 范围检查是性能关键——超出范围才走常量池。

## 15.5 标识符：local / global 双路径

```cpp
void Compiler::visit(IdentExpr& i) {
    int slot = resolve_local(i.name);
    if (slot >= 0) {
        emit(OP_LOAD_LOCAL, slot);
    } else {
        emit(OP_LOAD_GLOBAL, add_string_constant(i.name));
    }
    // upvalue 路径在第 19 章插入
}

int Compiler::resolve_local(const std::string& name) {
    const auto& locals = cur_->locals;
    // 从最新的往前找——内层 shadow 外层
    for (int i = static_cast<int>(locals.size()) - 1; i >= 0; --i) {
        if (locals[i] == name) return i;
    }
    return -1;
}
```

注意我们**从后往前**找——因为新声明的 local 会 shadow 同名的旧
local。这与第 9 章的 env 链查找是一回事，只不过这里在编译期完
成。

## 15.6 二元 / 一元运算

```cpp
void Compiler::visit(BinaryOp& b) {
    b.left->accept(*this);
    b.right->accept(*this);
    switch (b.op) {
        case TokenType::PLUS:    emit(OP_ADD); break;
        case TokenType::MINUS:   emit(OP_SUB); break;
        case TokenType::STAR:    emit(OP_MUL); break;
        case TokenType::SLASH:   emit(OP_DIV); break;
        case TokenType::PERCENT: emit(OP_MOD); break;
        case TokenType::DOT_DOT: emit(OP_CONCAT); break;
        case TokenType::EQUAL_EQUAL: emit(OP_EQ); break;
        case TokenType::BANG_EQUAL:  emit(OP_NE); break;
        case TokenType::LESS:        emit(OP_LT); break;
        case TokenType::LESS_EQUAL:  emit(OP_LE); break;
        case TokenType::GREATER:     emit(OP_GT); break;
        case TokenType::GREATER_EQUAL: emit(OP_GE); break;
        default: throw std::runtime_error("bad binop");
    }
}

void Compiler::visit(UnaryOp& u) {
    u.operand->accept(*this);
    switch (u.op) {
        case TokenType::MINUS: emit(OP_NEG); break;
        case TokenType::NOT:   emit(OP_NOT); break;
        default: throw std::runtime_error("bad unary");
    }
}
```

栈式 VM 的最大魅力：**visit 二元运算就是"先左后右再发射"**，一
个机械动作。

## 15.7 短路逻辑：跳转编织

`a and b`：

* 求 a；
* 如果 a 假，跳过 b（保留 a 作为整个表达式的值）；
* 否则求 b（结果就是 a and b 的值）。

```cpp
void Compiler::visit(LogicalOp& l) {
    l.left->accept(*this);
    if (l.op == TokenType::AND) {
        // a and b：a 为假就跳过 b
        int jmp = emit(OP_JUMP_IF_FALSE);   // 不弹栈：a 留在栈顶作为结果
        emit(OP_POP);                       // 真分支：弹 a
        l.right->accept(*this);
        patch_jump(jmp, static_cast<int>(cur_->proto->code.size()));
    } else { // OR
        int jmp_if_true = emit(OP_JUMP_IF_FALSE);  // 如果 a 假，跳到求 b
        // a 真：跳到末尾，保留 a
        int jmp_end = emit(OP_JUMP);
        patch_jump(jmp_if_true, static_cast<int>(cur_->proto->code.size()));
        emit(OP_POP);
        l.right->accept(*this);
        patch_jump(jmp_end, static_cast<int>(cur_->proto->code.size()));
    }
}
```

短路的关键是跳转目标——把"什么时候跳到哪里"用 patch_jump 编织
出来。这种"先发射占位，后回填地址"的模式贯穿后面所有控制流。

## 15.8 赋值表达式

```cpp
void Compiler::visit(AssignExpr& a) {
    a.value->accept(*this);   // 求值，结果在栈顶

    if (auto* id = dynamic_cast<IdentExpr*>(a.target.get())) {
        int slot = resolve_local(id->name);
        if (slot >= 0) {
            emit(OP_STORE_LOCAL, slot);   // 不弹栈，作为表达式结果
        } else {
            emit(OP_STORE_GLOBAL, add_string_constant(id->name));
        }
        return;
    }

    if (auto* idx = dynamic_cast<IndexExpr*>(a.target.get())) {
        // 栈布局：value
        idx->object->accept(*this);   // value, obj
        idx->index->accept(*this);    // value, obj, key
        emit(OP_SET_INDEX);            // 弹掉三个，留下 value 作为结果（VM 实现时考虑）
        // 实际我们让 SET_INDEX 弹 value/obj/key，然后再 push value
        // 简化：value 留在栈底
        return;
    }

    throw std::runtime_error("invalid assignment target");
}
```

Compiler 里**就在这里检查左值合法性**——`1 = 2` 的 target 不是
IdentExpr 也不是 IndexExpr，到 throw。这是把 Parser 阶段不好
做的语义检查推迟到 Compiler 的好处。

## 15.9 函数调用

```cpp
void Compiler::visit(CallExpr& c) {
    c.callee->accept(*this);
    for (auto& a : c.args) a->accept(*this);
    emit(OP_CALL, static_cast<std::int32_t>(c.args.size()));
}
```

四行——栈式 VM 调用是机械的。`OP_CALL` 的 arg 告诉 VM"实参有
几个"，VM 自己沿栈往下数找 callee。

## 15.10 Index / Array / Table 字面量

```cpp
void Compiler::visit(IndexExpr& e) {
    e.object->accept(*this);
    e.index->accept(*this);
    emit(OP_GET_INDEX);
}

void Compiler::visit(ArrayLit& a) {
    emit(OP_NEW_TABLE);
    for (std::size_t i = 0; i < a.elements.size(); ++i) {
        // 栈：[..., table]
        emit(OP_PUSH_INT, static_cast<std::int32_t>(i));   // [..., table, i]
        a.elements[i]->accept(*this);                      // [..., table, i, v]
        emit(OP_SET_INDEX_KEEP_TABLE);   // 弹 i, v；保留 table
    }
}
```

注意 `OP_SET_INDEX_KEEP_TABLE` ——为了让数组字面量构造完后
table 仍在栈顶，我们需要一个变体。或者更简单：在每次 SET 之后
插一个 `DUP table` 之类。本书里我们走"加一个变体"的路子（指令
集为此多一个 entry）。

实际工程上你也可以这样写：

```cpp
void Compiler::visit(ArrayLit& a) {
    emit(OP_NEW_TABLE);          // [table]
    for (std::size_t i = 0; i < a.elements.size(); ++i) {
        emit(OP_DUP);            // [table, table]
        emit(OP_PUSH_INT, i);    // [table, table, i]
        a.elements[i]->accept(*this);  // [table, table, i, v]
        emit(OP_SET_INDEX);      // [table]
    }
}
```

`OP_DUP` 是个通用工具指令——很多 VM 都有。我们就用这种方式，
不再加专用变体。

## 15.11 控制流：if

```cpp
void Compiler::visit(IfStmt& s) {
    std::vector<int> end_jumps;   // 所有分支结束后跳到 if 末尾

    for (auto& br : s.branches) {
        br.cond->accept(*this);
        int jmp_next = emit(OP_JUMP_IF_FALSE_POP);

        enter_block();
        for (auto& st : br.body) st->accept(*this);
        leave_block();

        end_jumps.push_back(emit(OP_JUMP));   // 跳到末尾
        patch_jump(jmp_next, static_cast<int>(cur_->proto->code.size()));
    }

    if (!s.else_body.empty()) {
        enter_block();
        for (auto& st : s.else_body) st->accept(*this);
        leave_block();
    }

    int end = static_cast<int>(cur_->proto->code.size());
    for (int j : end_jumps) patch_jump(j, end);
}
```

模式：每个分支执行完都跳到 if 末尾；下一个 elif 的入口由当前分
支条件假时跳到。`end_jumps` 收集所有"跳到末尾"的指令，最后统
一回填。

## 15.12 控制流：while

```cpp
void Compiler::visit(WhileStmt& s) {
    int loop_start = static_cast<int>(cur_->proto->code.size());
    s.cond->accept(*this);
    int jmp_exit = emit(OP_JUMP_IF_FALSE_POP);

    enter_block();
    for (auto& st : s.body) st->accept(*this);
    leave_block();

    // 回到循环开始
    emit(OP_JUMP, loop_start - static_cast<int>(cur_->proto->code.size()) - 1);
    patch_jump(jmp_exit, static_cast<int>(cur_->proto->code.size()));
}
```

注意 `OP_JUMP` 的 arg 是**负偏移**——往前跳。bytecode VM 的循
环就是"反向跳转"。

## 15.13 控制流：for

```cpp
void Compiler::visit(ForStmt& s) {
    enter_block();   // for 整体一层
    int var_slot = declare_local(s.var);

    s.start->accept(*this);
    emit(OP_STORE_LOCAL_POP, var_slot);

    int loop_start = static_cast<int>(cur_->proto->code.size());

    // 检查 i < end
    emit(OP_LOAD_LOCAL, var_slot);
    s.end->accept(*this);
    emit(OP_LT);
    int jmp_exit = emit(OP_JUMP_IF_FALSE_POP);

    enter_block();   // 循环体一层（每轮独立 let）
    for (auto& st : s.body) st->accept(*this);
    leave_block();

    // i = i + 1
    emit(OP_LOAD_LOCAL, var_slot);
    emit(OP_PUSH_INT, 1);
    emit(OP_ADD);
    emit(OP_STORE_LOCAL_POP, var_slot);

    emit(OP_JUMP, loop_start - static_cast<int>(cur_->proto->code.size()) - 1);
    patch_jump(jmp_exit, static_cast<int>(cur_->proto->code.size()));

    leave_block();
}
```

注意我们求值 `s.end` 在每轮循环里——这是**故意**的，与树遍历版
本一致：保证用户在循环体里改 end 时新值立刻生效。如果想做"end
只算一次"的优化，可以把 end 求一次存到一个临时 local。Lua 这么
做——但代码长一些。

## 15.14 块作用域：enter / leave

```cpp
void Compiler::enter_block() { cur_->current_depth++; }

void Compiler::leave_block() {
    auto& locals = cur_->locals;
    auto& depth = cur_->scope_depth;
    while (!depth.empty() && depth.back() == cur_->current_depth) {
        emit(OP_POP);            // 把这个 local 弹出栈
        locals.pop_back();
        depth.pop_back();
    }
    cur_->current_depth--;
}

int Compiler::declare_local(const std::string& name) {
    cur_->locals.push_back(name);
    cur_->scope_depth.push_back(cur_->current_depth);
    int slot = static_cast<int>(cur_->locals.size()) - 1;
    if (slot + 1 > cur_->proto->num_locals) {
        cur_->proto->num_locals = slot + 1;
    }
    return slot;
}
```

`leave_block` 发射多条 `OP_POP`——离开作用域时把这块的 local
全部从栈上抹掉。这对应树遍历里"作用域销毁，env 引用计数归零"
的行为。

## 15.15 嵌套函数：FnExpr

```cpp
void Compiler::visit(FnExpr& f) {
    // 嵌套一个新 Compiler 上下文
    auto child = std::make_shared<Scope>();
    child->proto = std::make_shared<FunctionProto>();
    child->proto->num_params = static_cast<int>(f.params.size());
    child->parent = cur_;

    auto saved = cur_;
    cur_ = child;

    // 参数作为前 N 个 local
    for (auto& p : f.params) {
        declare_local(p);
    }

    for (auto& s : f.body) s->accept(*this);
    // 兜底返回 nil
    emit(OP_PUSH_NIL);
    emit(OP_RETURN);

    cur_ = saved;

    // 在父函数里发射 OP_CLOSURE
    int proto_idx = add_constant(Value(/* TODO: 把 child->proto 包成 Value */));
    emit(OP_CLOSURE, proto_idx);
    // upvalue 列表的紧随指令在第 19 章讨论
}
```

注意：

* **嵌套时切换 cur_**：保存父，编译完子函数再恢复；
* **兜底 RETURN**：让没有显式 return 的函数也能正常退出，返回 nil；
* **proto 进父常量池**：父函数的 `OP_CLOSURE` 引用它。

`Value` 暂时不能装 FunctionProto——下一章我们会扩展 Value 加一
个 `ProtoRef` 或者用一个 wrapper。这是为什么书的章节是渐进的：
Value 一开始没考虑这种需求，到这里我们再回去补。这种"螺旋式
迭代"是写编译器的常态。

## 15.16 LetStmt 与 ExprStmt 的小细节

```cpp
void Compiler::visit(LetStmt& s) {
    s.init->accept(*this);
    // 注意：先 emit init，再 declare local——否则 init 里若引用同名变量会偏移
    int slot = declare_local(s.name);
    // init 求值结果在栈顶；我们希望它就在 slot 位置
    emit(OP_STORE_LOCAL_POP, slot);
}

void Compiler::visit(ExprStmt& s) {
    s.expr->accept(*this);
    emit(OP_POP);   // 表达式语句的结果丢弃
}
```

关键是 LetStmt 这一行的顺序——**先求 init，后 declare**。否则
你写 `let x = x + 1`（外层有同名 x）时，init 会引用到新 slot 而
不是外层的 x。这是 JavaScript `let` 不允许 TDZ 内自引用的原因。
我们抄它的语义。

## 15.17 ReturnStmt

```cpp
void Compiler::visit(ReturnStmt& s) {
    if (s.value) {
        s.value->accept(*this);
    } else {
        emit(OP_PUSH_NIL);
    }
    emit(OP_RETURN);
}
```

## 15.18 程序入口编译

```cpp
std::shared_ptr<FunctionProto> Compiler::compile(Program& p) {
    auto root = std::make_shared<Scope>();
    root->proto = std::make_shared<FunctionProto>();
    root->proto->name = "<main>";
    cur_ = root;

    for (auto& s : p.stmts) {
        s->accept(*this);
    }

    emit(OP_PUSH_NIL);
    emit(OP_RETURN);

    return cur_->proto;
}
```

整个 program 被编译成一个**主函数**。VM 启动时就调用它。

## 15.19 一个调试小习惯：边写边反汇编

第 20 章会写完整 disassembler。但你**现在**就可以写一个 30 行的
最小版本快速验证 Compiler 写对没：

```cpp
void dump_proto(const FunctionProto& p) {
    std::cout << "fn " << p.name << " (params=" << p.num_params
              << " locals=" << p.num_locals << ")\n";
    for (std::size_t i = 0; i < p.code.size(); ++i) {
        auto inst = p.code[i];
        std::cout << "  " << i << "  " << opcode_name(get_op(inst));
        std::cout << " " << get_sarg(inst) << "\n";
    }
}
```

跑：

```cpp
auto tokens = Lexer("let x = 1 + 2 * 3\n").tokenize();
Parser p(std::move(tokens));
auto prog = p.parse_program();
Compiler c;
auto proto = c.compile(*prog);
dump_proto(*proto);
```

输出：

```
fn <main> (params=0 locals=1)
  0  PUSH_INT 1
  1  PUSH_INT 2
  2  PUSH_INT 3
  3  MUL
  4  ADD
  5  STORE_LOCAL_POP 0
  6  PUSH_NIL
  7  RETURN
```

看到这个输出，你就知道 Compiler 写对了。这种"中间产物可视化"
是写 Compiler 的精神食粮。

## 15.20 第 15 章小结

到这章为止，我们的 Compiler 能编译：

* 全部表达式（含运算符、调用、下标、字面量）；
* 全部语句（let、if、while、for、return、表达式语句）；
* 嵌套函数（**不含**闭包捕获 upvalue —— 第 19 章补全）。

代码量 ~600 行 C++。

剩下的拼图：

* 第 16 章：常量池怎么设计（去重、复用、字符串内联）；
* 第 17 章：VM 主循环把 bytecode 跑起来；
* 第 18 章：Call Frame 让函数调用工作；
* 第 19 章：Upvalue 让闭包工作；
* 第 20 章：Disassembler 让我们看清生成的代码。

下一章先把"常量池"这个看似不起眼但**会决定 VM 性能**的部分讲
清楚——为什么字符串需要 intern、为什么 add_constant 需要去重、
全局名怎么走"先查缓存再 hash"。
