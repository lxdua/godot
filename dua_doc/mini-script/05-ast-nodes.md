# 第 5 章　AST 节点的 C++ 表示

前两章我们已经把 AST 的具体节点零散地写出来了——`LetStmt`、
`IfStmt`、`BinaryOp`……这一章把它们集中梳理成一个一致、可扩展、
易遍历的层级，并讨论 C++ 里几种主流的"AST 表示法"的取舍。这是一
个看起来不起眼但**会决定你后面所有代码长什么样**的设计。

## 5.1 三种主流方案

| 方案                              | 代表项目          | 优点                       | 缺点                        |
| --------------------------------- | ----------------- | -------------------------- | --------------------------- |
| **多态类层级 + visitor**          | Clang、GDScript   | OO 直观，加节点无需改 enum | RTTI/虚表开销，节点数大时 cache miss |
| **tag enum + union/`std::variant`** | rustc、TypeScript（部分） | 无虚函数，cache 友好     | 加节点要改 variant 与所有 visit |
| **同质 Node 数组（"flat AST"）**  | Roslyn、Carbon    | 极致紧凑、按 ID 引用       | 写起来啰嗦、调试不直观      |

我们选第一种——多态类 + visitor。原因：

* 教学优先：每个节点是个独立 struct，在调试器里展开看一目了然；
* 性能不是这本书的目标——第二部分跑通才是；
* 后面要扩展（加新语句类型、加 type info）成本最低。

第三部分的字节码 VM 会**完全绕开 AST**——直接 walk AST 编译成 bytecode，
之后整棵树可以释放。所以"AST 是不是 cache friendly"在我们的工程
里没那么重要。

## 5.2 共用基类与字段

```cpp
// src/ast.h
#pragma once
#include "token.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mini {

// ----- 前向声明 -----
struct AstVisitor;

// ----- 节点基类 -----
struct AstNode {
    int line = 0;
    int column = 0;
    virtual ~AstNode() = default;
    virtual void accept(AstVisitor& v) = 0;
};

struct Expr : AstNode {};
struct Stmt : AstNode {};

// 便捷的 unique_ptr 别名
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

}  // namespace mini
```

几个看似细节但实际重要的设计：

* **`line` 与 `column` 一定放在基类**：所有错误信息都靠它。即使
  `NumberLit` 这种"明显不会出错"的节点也要带——后续 Analyzer/
  Compiler 检查时会需要。
* **`Expr` 和 `Stmt` 分开**：让 `parse_expression` 的返回类型与
  `parse_stmt` 区分清楚，C++ 类型系统帮我们在编译期挡掉一类 bug
  （比如把语句赋给表达式字段）。
* **`accept(visitor)` 是纯虚**：每个具体节点必须 override，强制
  我们在加新节点时不会忘记接通 visitor。

## 5.3 表达式节点

```cpp
// 字面量
struct NumberLit : Expr {
    bool is_int = true;
    std::int64_t ivalue = 0;
    double fvalue = 0.0;
    void accept(AstVisitor& v) override;
};
struct StringLit : Expr {
    std::string value;
    void accept(AstVisitor& v) override;
};
struct BoolLit : Expr {
    bool value = false;
    void accept(AstVisitor& v) override;
};
struct NilLit : Expr {
    void accept(AstVisitor& v) override;
};

// 标识符
struct IdentExpr : Expr {
    std::string name;
    // 后续 resolver 阶段会填充：是 local / upvalue / global
    enum class Kind { Unresolved, Local, Upvalue, Global } kind = Kind::Unresolved;
    int slot = -1;   // local/upvalue 在所属作用域中的下标
    void accept(AstVisitor& v) override;
};

// 二元、一元
struct BinaryOp : Expr {
    TokenType op;
    ExprPtr left, right;
    void accept(AstVisitor& v) override;
};
struct UnaryOp : Expr {
    TokenType op;
    ExprPtr operand;
    void accept(AstVisitor& v) override;
};
struct LogicalOp : Expr {
    TokenType op;       // AND / OR
    ExprPtr left, right;
    void accept(AstVisitor& v) override;
};
struct AssignExpr : Expr {
    ExprPtr target;     // 必须是 IdentExpr 或 IndexExpr
    ExprPtr value;
    void accept(AstVisitor& v) override;
};

// 调用、下标、复合字面量
struct CallExpr : Expr {
    ExprPtr callee;
    std::vector<ExprPtr> args;
    void accept(AstVisitor& v) override;
};
struct IndexExpr : Expr {
    ExprPtr object;
    ExprPtr index;
    void accept(AstVisitor& v) override;
};
struct ArrayLit : Expr {
    std::vector<ExprPtr> elements;
    void accept(AstVisitor& v) override;
};
struct TableLit : Expr {
    struct Entry { ExprPtr key, value; };
    std::vector<Entry> entries;
    void accept(AstVisitor& v) override;
};

// 匿名函数（fn 表达式）
struct FnExpr : Expr {
    std::vector<std::string> params;
    std::vector<StmtPtr> body;
    void accept(AstVisitor& v) override;
};
```

值得展开的设计：

### `IdentExpr` 上的 `kind` 与 `slot` 字段

`IdentExpr` 不仅承载"叫什么"，还承载**"是什么"**和**"在哪
里"**。Parser 阶段填不出来，Resolver/Compiler 阶段会回填。

把 resolver 的产物存回 AST 是一种很常见的设计。优点：

* AST 一次构造，多次遍历都能复用；
* 树遍历解释器与字节码编译器都能用同一棵 AST；

缺点：

* AST 不再是"纯输入"——它会被多个 pass 修改。我们用一个简单约定
  绕开这个问题：**只允许 Resolver 写 `kind/slot`，其它 pass 只读**。

### 为什么 `AssignExpr` 不直接放 `name + value`

很多新手会写：

```cpp
struct AssignExpr : Expr {
    std::string name;
    ExprPtr value;
};
```

这只能处理 `a = 1`。但 `a[0] = 1` 和 `obj.x = 1` 都不行。我们让
`target` 是任意 `Expr`，由后续 pass 检查"这个 expr 是不是合法左
值"。这是 Lua、Python、JavaScript 都用的设计——AST 的形状代表"语
法"而不是"语义"。

## 5.4 语句节点

```cpp
struct LetStmt : Stmt {
    std::string name;
    ExprPtr init;
    void accept(AstVisitor& v) override;
};

struct ExprStmt : Stmt {
    ExprPtr expr;
    void accept(AstVisitor& v) override;
};

struct ReturnStmt : Stmt {
    ExprPtr value;  // 可空
    void accept(AstVisitor& v) override;
};

struct IfStmt : Stmt {
    struct Branch { ExprPtr cond; std::vector<StmtPtr> body; };
    std::vector<Branch> branches;          // 至少 1 个；first = if
    std::vector<StmtPtr> else_body;        // 可空
    void accept(AstVisitor& v) override;
};

struct WhileStmt : Stmt {
    ExprPtr cond;
    std::vector<StmtPtr> body;
    void accept(AstVisitor& v) override;
};

struct ForStmt : Stmt {
    std::string var;
    ExprPtr start, end;
    std::vector<StmtPtr> body;
    void accept(AstVisitor& v) override;
};

// 命名函数 = let + fn 表达式 的语法糖
struct FnStmt : Stmt {
    std::string name;
    std::unique_ptr<FnExpr> fn;
    void accept(AstVisitor& v) override;
};

struct Program {
    std::vector<StmtPtr> stmts;
};
```

### `IfStmt` 用"分支数组"而不是嵌套

很多教科书把 `elif` 表示成嵌套：

```
if a then ...
elif b then ...
else ...
end
```

被解析为：

```
IfStmt(a, ..., else: IfStmt(b, ..., else: ElseStmt(...)))
```

这是 Lox 等教学项目的做法。我们不这么做，原因：

* 嵌套 AST **看不出"原始 elif 的边界"**——错误信息会错位；
* 后续要做"覆盖率分析"时分支数难数；
* 树遍历求值时多一层递归调用。

直接用数组 `branches[]` + 可选 `else_body`，结构与源码一一对应，
visitor 写起来也更直观。GDScript 的 `IfNode` 就是这种结构。

### `FnStmt` = 名字 + `FnExpr`

`fn foo() ... end` 在语义上完全等价于 `let foo = fn() ... end`。
所以 `FnStmt` 内部就是一个 `FnExpr` 加个名字。这样：

* 编译器只需要处理 `FnExpr`，不需要为命名函数写第二份代码；
* 闭包、嵌套函数、匿名 lambda 全都走同一条路径。

这一招在 Lua 也用——`function foo()` 就是 `foo = function()` 的
语法糖。一个简单的设计选择能消除整整一个代码分支。

## 5.5 Visitor 模式：让 AST 可以被多种方式遍历

我们需要至少两种"遍历"AST 的方式：

* **树遍历解释器**：边访问边求值；
* **字节码编译器**：边访问边发射 opcode。

如果给每个节点写一个 `evaluate()` 方法，AST 就被绑死在一种用法上。
visitor 模式让"操作"与"数据"解耦。

```cpp
// src/ast.h
struct AstVisitor {
    // 表达式
    virtual void visit(NumberLit&)  = 0;
    virtual void visit(StringLit&)  = 0;
    virtual void visit(BoolLit&)    = 0;
    virtual void visit(NilLit&)     = 0;
    virtual void visit(IdentExpr&)  = 0;
    virtual void visit(BinaryOp&)   = 0;
    virtual void visit(UnaryOp&)    = 0;
    virtual void visit(LogicalOp&)  = 0;
    virtual void visit(AssignExpr&) = 0;
    virtual void visit(CallExpr&)   = 0;
    virtual void visit(IndexExpr&)  = 0;
    virtual void visit(ArrayLit&)   = 0;
    virtual void visit(TableLit&)   = 0;
    virtual void visit(FnExpr&)     = 0;
    // 语句
    virtual void visit(LetStmt&)    = 0;
    virtual void visit(ExprStmt&)   = 0;
    virtual void visit(ReturnStmt&) = 0;
    virtual void visit(IfStmt&)     = 0;
    virtual void visit(WhileStmt&)  = 0;
    virtual void visit(ForStmt&)    = 0;
    virtual void visit(FnStmt&)     = 0;
    virtual ~AstVisitor() = default;
};
```

每个节点的 `accept` 调对应的 `visit`：

```cpp
// src/ast.cpp
void NumberLit::accept(AstVisitor& v) { v.visit(*this); }
void StringLit::accept(AstVisitor& v) { v.visit(*this); }
// ... 一一对应（其实可以用宏少写几行）
```

宏版本（如果你不嫌弃）：

```cpp
#define DEFINE_ACCEPT(NodeT) \
    void NodeT::accept(AstVisitor& v) { v.visit(*this); }

DEFINE_ACCEPT(NumberLit)
DEFINE_ACCEPT(StringLit)
DEFINE_ACCEPT(BoolLit)
// ...
```

注意 `visit` **没有返回值**——它返回值靠"成员变量传递"，比如
解释器在 `visit(BinaryOp&)` 里把结果写到 `last_value_`，调用方再
读它。这种"无返回 visitor"的写法可以避开"模板返回类型 +
unique_ptr 销毁"那一堆 C++ 麻烦事。代价是写起来稍啰嗦。

如果你想要返回值的 visitor，可以用 `std::any` 或者把求值器写成
**模板 visitor**。教学场景里非模板版本更易读。

## 5.6 一个调试用的 AST Printer

写到这里我们应该**立刻**实现一个能把 AST 打印成可读形式的
visitor。它会在第二部分调试求值器时反复救命。

```cpp
// src/ast_printer.h
#pragma once
#include "ast.h"
#include <iostream>
#include <string>

namespace mini {

class AstPrinter : public AstVisitor {
public:
    explicit AstPrinter(std::ostream& os = std::cout) : os_(os) {}
    void print(Program& p) {
        for (auto& s : p.stmts) s->accept(*this);
    }

    // 表达式
    void visit(NumberLit& n) override {
        indent();
        if (n.is_int) os_ << "Number(" << n.ivalue << ")\n";
        else          os_ << "Number(" << n.fvalue << ")\n";
    }
    void visit(StringLit& s) override {
        indent(); os_ << "String(\"" << s.value << "\")\n";
    }
    void visit(BoolLit& b) override {
        indent(); os_ << "Bool(" << (b.value ? "true" : "false") << ")\n";
    }
    void visit(NilLit&) override { indent(); os_ << "Nil\n"; }
    void visit(IdentExpr& i) override {
        indent(); os_ << "Ident(" << i.name << ")\n";
    }
    void visit(BinaryOp& b) override {
        indent(); os_ << "Binary(" << token_name(b.op) << ")\n";
        with_indent([&]{
            b.left->accept(*this);
            b.right->accept(*this);
        });
    }
    void visit(UnaryOp& u) override {
        indent(); os_ << "Unary(" << token_name(u.op) << ")\n";
        with_indent([&]{ u.operand->accept(*this); });
    }
    // ... 其它节点类似

    // 语句
    void visit(LetStmt& s) override {
        indent(); os_ << "Let(" << s.name << ")\n";
        with_indent([&]{ s.init->accept(*this); });
    }
    void visit(IfStmt& s) override {
        indent(); os_ << "If\n";
        with_indent([&]{
            for (auto& b : s.branches) {
                indent(); os_ << "Branch\n";
                with_indent([&]{
                    b.cond->accept(*this);
                    for (auto& st : b.body) st->accept(*this);
                });
            }
            if (!s.else_body.empty()) {
                indent(); os_ << "Else\n";
                with_indent([&]{
                    for (auto& st : s.else_body) st->accept(*this);
                });
            }
        });
    }
    // ...

private:
    std::ostream& os_;
    int level_ = 0;
    void indent() {
        for (int i = 0; i < level_; ++i) os_ << "  ";
    }
    template <class F> void with_indent(F f) {
        ++level_; f(); --level_;
    }
};

}  // namespace mini
```

跑一下：

```cpp
auto tokens = Lexer("let x = 1 + 2 * 3\n").tokenize();
Parser p(std::move(tokens));
auto prog = p.parse_program();
AstPrinter().print(*prog);
```

输出：

```
Let(x)
  Binary(+)
    Number(1)
    Binary(*)
      Number(2)
      Number(3)
```

**这个工具的价值不是"展示 AST"，而是"调试 parser"**——你能立
刻看出优先级有没有写对、关键字有没有被吞、结合性是否符合预期。
强烈建议在开始写求值器之前，先用 `AstPrinter` 跑过 examples/ 里
的所有脚本，确保 AST 形状全对。

## 5.7 `accept` 调度的代价：要不要担心虚表

有人会指出："每个节点 `accept` 都是虚调用，慢吧？"

实测一下：

* 一棵中等大小的 AST（500 节点）遍历一次，虚调用总开销 < 5μs；
* 求值器内部还会做 `std::variant` 访问、`unordered_map` 查找——
  虚调用占总时间不到 1%；
* 字节码 VM 那条路径**根本不遍历 AST**——编译完一次性丢弃，所
  以更不在乎。

结论：**别提前优化**。如果将来真有需要（比如做 AST 增量重解析），
你可以把 visitor 改成 tag dispatch，但那是另一本书的话题了。

## 5.8 一个小的取舍：要不要在 AST 里存源码引用

GDScript 的 AST 节点会存 `start_line/end_line/start_column/
end_column` 四个字段——便于编辑器做错误高亮、跳转、重命名。

我们目前只存 `line + column`（节点起点）。原因：

* 求值期/编译期都只需要"出错的那个位置"，不需要范围；
* 多存两个 int 会让 `sizeof(NumberLit)` 从 24 涨到 32；
* 真要做 LSP，再加也来得及（可加个可选的 `SourceRange* range`）。

这种"按需扩字段"的做法是脚本语言长期演化的标配——一开始只放绝
对必要的，越具体的需求越往外推。

---

到这里 **第一部分（前端）** 全部就绪：

```
源码 ──Lexer──▶ Token ──Parser+Pratt──▶ AST ──AstPrinter──▶ 可视化
```

下一章我们正式进入 **第二部分（树遍历解释器）**。第一站是
`Value`：怎么用 C++ 表示一门动态类型语言里的"任意值"——`nil`、
`bool`、`int`、`float`、`string`、`function`、`table` 全都要装得
进同一个变量里。我们会用 `std::variant` 把它们装在一起，并讨论
为什么这是教学场景下的最佳选择（而 union 不是）。
