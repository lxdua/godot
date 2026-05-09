# 第 3 章　Parser：递归下降构造 AST

Lexer 把字符变成了 Token，但 Token 还是"线性"的，没有任何结构。
Parser 的任务是把这串 Token 折叠成**抽象语法树**（AST，Abstract
Syntax Tree）——一棵能被求值器遍历或被编译器翻译的树。

```
[LET] [IDENT(x)] [EQUAL] [NUMBER(1)] [PLUS] [NUMBER(2)] [NEWLINE]
                                │
                                ▼
                       LetStmt(name="x",
                               init=BinOp(op=+,
                                          left=NumLit(1),
                                          right=NumLit(2)))
```

业界主流的两种 Parser 实现：

| 方案 | 代表 | 说明 |
| --- | --- | --- |
| LR / LALR + 工具生成（yacc / bison） | 老 GCC | 自动化、表大、错误信息差 |
| 递归下降（手写） | Lua、Go、Python、GDScript、TypeScript | 一个文法非终结符 = 一个函数，错误信息友好 |

我们走第二条。这一章先把**语句层**的递归下降骨架搭好；表达式层
（要处理优先级和结合性）会在下一章用 Pratt Parser 单独讲。

## 3.1 一份能拿来即用的 BNF

EBNF 风格，方括号表示可选，星号表示零或多次：

```
program       ::= { statement } EOF

statement     ::= let_stmt
                | fn_stmt
                | if_stmt
                | while_stmt
                | for_stmt
                | return_stmt
                | expr_stmt
                | NEWLINE        // 空行直接吃掉

let_stmt      ::= "let" IDENT "=" expression terminator
fn_stmt       ::= "fn" IDENT "(" [ params ] ")" block "end"
params        ::= IDENT { "," IDENT }

if_stmt       ::= "if" expression "then" block
                  { "elif" expression "then" block }
                  [ "else" block ]
                  "end"

while_stmt    ::= "while" expression "do" block "end"

for_stmt      ::= "for" IDENT "in" expression ".." expression "do" block "end"

return_stmt   ::= "return" [ expression ] terminator

expr_stmt     ::= expression terminator   // 也允许 a = b（赋值是表达式）

block         ::= { statement }

terminator    ::= NEWLINE | EOF | <followed by 'end'/'elif'/'else'>

expression    ::= ...   // 见下一章
```

几个值得注意的设计：

* **`block` 不需要单独的开闭符号**——它由外层结构（`then` / `do`
  / `else`）包住，遇到 `end / elif / else` 就停。这样 parser 写起
  来很干净。
* **`terminator` 是个"软"概念**——NEWLINE 是它，EOF 也是它，下一
  个 token 是 `end / elif / else` 时也算"语句结束"。这避免了用户
  漏写换行被骂。
* **赋值是表达式而不是语句**——`a = b = 1` 这种链式赋值天然支持。
  我们在 Pratt Parser 里把 `=` 设成右结合最低优先级即可。

## 3.2 Parser 的状态与基础操作

`src/parser.h`：

```cpp
#pragma once
#include "ast.h"
#include "token.h"
#include <stdexcept>
#include <vector>

namespace mini {

struct ParseError : public std::runtime_error {
    int line;
    int column;
    ParseError(const std::string& msg, int l, int c)
        : std::runtime_error(msg), line(l), column(c) {}
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

    // 解析整个 program；遇到错误会收集到 errors_ 而不是抛出
    std::unique_ptr<Program> parse_program();

    const std::vector<ParseError>& errors() const { return errors_; }
    bool has_errors() const { return !errors_.empty(); }

private:
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    std::vector<ParseError> errors_;
    bool panic_ = false;

    // 基础操作
    const Token& peek(std::size_t offset = 0) const;
    const Token& advance();
    bool check(TokenType t) const;
    bool match(TokenType t);
    const Token& expect(TokenType t, const char* what);
    bool at_end() const;

    // 错误处理
    [[noreturn]] void error_at(const Token& t, const std::string& msg);
    void synchronize();

    // 语句
    std::unique_ptr<Stmt>      parse_stmt();
    std::unique_ptr<LetStmt>   parse_let();
    std::unique_ptr<FnStmt>    parse_fn();
    std::unique_ptr<IfStmt>    parse_if();
    std::unique_ptr<WhileStmt> parse_while();
    std::unique_ptr<ForStmt>   parse_for();
    std::unique_ptr<ReturnStmt> parse_return();
    std::unique_ptr<Stmt>      parse_expr_stmt();

    std::vector<std::unique_ptr<Stmt>> parse_block(
        std::initializer_list<TokenType> end_tokens);

    // 表达式（下一章详细展开）
    std::unique_ptr<Expr> parse_expression();
};

}  // namespace mini
```

几个基础操作的实现都很短：

```cpp
const Token& Parser::peek(std::size_t offset) const {
    return tokens_[std::min(pos_ + offset, tokens_.size() - 1)];
}

const Token& Parser::advance() {
    if (!at_end()) pos_++;
    return tokens_[pos_ - 1];
}

bool Parser::check(TokenType t) const { return peek().type == t; }

bool Parser::match(TokenType t) {
    if (check(t)) { advance(); return true; }
    return false;
}

const Token& Parser::expect(TokenType t, const char* what) {
    if (check(t)) return advance();
    error_at(peek(), std::string("expected ") + what +
                     ", got '" + peek().lexeme + "'");
    // error_at 是 [[noreturn]]，永远不会到这里
}

bool Parser::at_end() const { return peek().type == TokenType::END_OF_FILE; }
```

注意 `peek` 越界 clamp 到 EOF——这样调用方不需要先判 `at_end()`，
代码会清爽很多。

## 3.3 错误处理：panic mode + synchronize

Parser 出错后**最重要**的事不是停下，而是**找到一个可信的恢复
点继续**。这样一个文件里的 5 个错误能一次报完，而不是改一个跑一
次发现下一个。

业界标准做法叫 **panic-mode synchronization**：

* 报第一条错误时把 `panic_` 置 true；
* 在 panic 模式下，所有后续的 `expect` 失败都**不再产生新错误**
  （避免级联噪声）；
* 调用 `synchronize()` 跳到下一个"安全"的语句起点（NEWLINE 之
  后、`fn / let / if / while / for / return` 之前），然后清掉
  `panic_`。

```cpp
void Parser::error_at(const Token& t, const std::string& msg) {
    if (!panic_) {
        errors_.emplace_back(msg, t.line, t.column);
        panic_ = true;
    }
    throw ParseError(msg, t.line, t.column);
}

void Parser::synchronize() {
    panic_ = false;
    while (!at_end()) {
        // NEWLINE 之后认为是新语句
        if (tokens_[pos_ - 1].type == TokenType::NEWLINE) return;
        switch (peek().type) {
            case TokenType::FN:
            case TokenType::LET:
            case TokenType::IF:
            case TokenType::WHILE:
            case TokenType::FOR:
            case TokenType::RETURN:
                return;
            default: break;
        }
        advance();
    }
}
```

主入口 `parse_program` 把异常接住、恢复、继续：

```cpp
std::unique_ptr<Program> Parser::parse_program() {
    auto prog = std::make_unique<Program>();
    while (!at_end()) {
        if (match(TokenType::NEWLINE)) continue;
        try {
            if (auto s = parse_stmt()) {
                prog->stmts.push_back(std::move(s));
            }
        } catch (const ParseError&) {
            synchronize();
        }
    }
    return prog;
}
```

这就是教科书里那条"用异常做错误传播 + try/catch 恢复"的常见模
式——不是真的拿异常做控制流，而是借它"非局部跳转"那一面，避免
在每个 helper 函数里都写 `if (failed) return nullptr;`。

## 3.4 顶层 parse_stmt

```cpp
std::unique_ptr<Stmt> Parser::parse_stmt() {
    switch (peek().type) {
        case TokenType::LET:    return parse_let();
        case TokenType::FN:     return parse_fn();
        case TokenType::IF:     return parse_if();
        case TokenType::WHILE:  return parse_while();
        case TokenType::FOR:    return parse_for();
        case TokenType::RETURN: return parse_return();
        case TokenType::END:
        case TokenType::ELIF:
        case TokenType::ELSE:
            // 不应该在 parse_stmt 看到这些；交给 parse_block 处理
            error_at(peek(), std::string("unexpected '")
                              + peek().lexeme + "'");
        default:
            return parse_expr_stmt();
    }
}
```

这就是递归下降的"调度器"：看一个 token，分派给一个子 parser。每
个子 parser 负责自己那一段语法。

## 3.5 几个具体的语句 parser

`let`：

```cpp
std::unique_ptr<LetStmt> Parser::parse_let() {
    Token kw = advance(); // 吃掉 'let'
    Token name = expect(TokenType::IDENT, "variable name");
    expect(TokenType::EQUAL, "'=' after variable name");
    auto init = parse_expression();
    consume_terminator();
    auto s = std::make_unique<LetStmt>();
    s->line = kw.line;
    s->name = name.lexeme;
    s->init = std::move(init);
    return s;
}
```

`if`：

```cpp
std::unique_ptr<IfStmt> Parser::parse_if() {
    Token kw = advance(); // 'if'
    auto s = std::make_unique<IfStmt>();
    s->line = kw.line;

    s->branches.push_back({parse_expression(), {}});
    expect(TokenType::THEN, "'then' after if condition");
    s->branches.back().body = parse_block(
        {TokenType::ELIF, TokenType::ELSE, TokenType::END});

    while (match(TokenType::ELIF)) {
        auto cond = parse_expression();
        expect(TokenType::THEN, "'then' after elif condition");
        auto body = parse_block(
            {TokenType::ELIF, TokenType::ELSE, TokenType::END});
        s->branches.push_back({std::move(cond), std::move(body)});
    }

    if (match(TokenType::ELSE)) {
        s->else_body = parse_block({TokenType::END});
    }
    expect(TokenType::END, "'end' to close if");
    return s;
}
```

`for`：

```cpp
std::unique_ptr<ForStmt> Parser::parse_for() {
    Token kw = advance(); // 'for'
    auto s = std::make_unique<ForStmt>();
    s->line = kw.line;
    s->var = expect(TokenType::IDENT, "loop variable").lexeme;
    expect(TokenType::IN, "'in' after loop variable");
    s->start = parse_expression();
    expect(TokenType::DOT_DOT, "'..' in for range");
    s->end = parse_expression();
    expect(TokenType::DO, "'do' to start loop body");
    s->body = parse_block({TokenType::END});
    expect(TokenType::END, "'end' to close for");
    return s;
}
```

可以看到模式高度一致：**关键字 → 子结构 → end**。这种"对齐感"
是手写递归下降相比工具生成的最大好处之一——你能直接看出每个语
法构造的形状。

## 3.6 `parse_block`：靠"结束符集合"停下

`parse_block` 是 Mini 没有花括号的关键。它的签名很有意思：

```cpp
std::vector<std::unique_ptr<Stmt>> Parser::parse_block(
    std::initializer_list<TokenType> end_tokens)
{
    std::vector<std::unique_ptr<Stmt>> body;
    while (!at_end()) {
        if (match(TokenType::NEWLINE)) continue;
        // 看到任意一个结束符就返回（不消费它）
        for (TokenType t : end_tokens) {
            if (check(t)) return body;
        }
        try {
            body.push_back(parse_stmt());
        } catch (const ParseError&) {
            synchronize();
        }
    }
    return body;
}
```

调用方告诉 `parse_block` "你遇到 `elif / else / end` 就停"，然后
**自己**消费那个结束符。这种"由外层负责吃自己等待的 token"的写
法，在递归下降里是标准做法——它让每一层 parser 的责任范围非常清
晰：

* `parse_if` 期待 `end`，所以它消费 `end`；
* `parse_block` 期待"块结束"，但不知道具体是哪个 token，所以让外
  层告诉自己候选集合，遇到就**让出**控制权。

## 3.7 `parse_expr_stmt`：表达式与赋值的统一

```cpp
std::unique_ptr<Stmt> Parser::parse_expr_stmt() {
    auto expr = parse_expression();
    consume_terminator();
    auto s = std::make_unique<ExprStmt>();
    s->line = expr->line;
    s->expr = std::move(expr);
    return s;
}
```

注意我们没有单独的 `parse_assign`——因为赋值在 Mini 里**是表达
式**。`a = 1` 整体是一个 `BinOp(op="=", left=Ident("a"),
right=NumLit(1))`，被一个 `ExprStmt` 包起来作为语句。

这带来两个好处：

1. `a = b = 1` 自然就支持了（右结合最低优先级）；
2. parser 不需要先 lookahead 看"接下来是 `=` 吗"再决定走哪个分支。

代价：分析时要拒绝 `1 = 2` 这种"左值不合法"的情况。这个检查放在
后续的 Analyzer 或 Compiler 里做就行，Parser 阶段不管它。

## 3.8 `consume_terminator`：宽容地吃换行

```cpp
void Parser::consume_terminator() {
    if (match(TokenType::NEWLINE)) return;
    if (at_end()) return;
    // 块结束符也算"语句之后"
    switch (peek().type) {
        case TokenType::END:
        case TokenType::ELIF:
        case TokenType::ELSE:
            return;
        default:
            error_at(peek(),
                "expected newline or end of block, got '" +
                peek().lexeme + "'");
    }
}
```

这个小函数是"用户体验"的关键。Python 早期对 NEWLINE 的处理很严
格——少一个换行就报 `SyntaxError`。Mini 走宽容路线：只要下一个
token 是块结束符，就认为上一个语句已经隐式结束。这样这两段都合法：

```python
if x > 0 then return 1 end          # 单行 if
if x > 0 then
    return 1
end                                 # 多行 if
```

## 3.9 最小可用的 AST 节点

虽然第 5 章会专门讲 AST 设计，但这里先给一个"足以让 parser 编译
通过"的最小版本：

```cpp
// src/ast.h（节选，第 5 章会扩展）
namespace mini {

struct Expr {
    int line = 0;
    virtual ~Expr() = default;
};
struct Stmt {
    int line = 0;
    virtual ~Stmt() = default;
};

struct Program {
    std::vector<std::unique_ptr<Stmt>> stmts;
};

struct LetStmt : Stmt {
    std::string name;
    std::unique_ptr<Expr> init;
};
struct FnStmt : Stmt {
    std::string name;
    std::vector<std::string> params;
    std::vector<std::unique_ptr<Stmt>> body;
};
struct IfBranch {
    std::unique_ptr<Expr> cond;
    std::vector<std::unique_ptr<Stmt>> body;
};
struct IfStmt : Stmt {
    std::vector<IfBranch> branches;
    std::vector<std::unique_ptr<Stmt>> else_body;  // 可空
};
struct WhileStmt : Stmt {
    std::unique_ptr<Expr> cond;
    std::vector<std::unique_ptr<Stmt>> body;
};
struct ForStmt : Stmt {
    std::string var;
    std::unique_ptr<Expr> start, end;
    std::vector<std::unique_ptr<Stmt>> body;
};
struct ReturnStmt : Stmt {
    std::unique_ptr<Expr> value;  // 可空
};
struct ExprStmt : Stmt {
    std::unique_ptr<Expr> expr;
};

}  // namespace mini
```

设计上的取舍：

* **`virtual ~Expr() / ~Stmt()`**：我们用 `dynamic_cast` 或后续章
  节的 visitor 模式区分节点类型，需要 RTTI；想避开 RTTI 的话可以
  改成"tag enum + union"，但代码量翻倍，不值得；
* **`std::unique_ptr<Expr>`**：表达式节点天然有树形所有权，
  unique_ptr 完美匹配；
* **AST 节点带 `line`**：错误信息和栈跟踪都要它。

## 3.10 一组 Parser 的单元测试

我们暂时还没写表达式 parser（下一章），所以先写一些只涉及语句结
构的简单测试。表达式部分用占位的"identifier 或 number"。

```cpp
#include "lexer.h"
#include "parser.h"
#include <gtest/gtest.h>

using namespace mini;

static std::unique_ptr<Program> parse(std::string_view src) {
    auto tokens = Lexer(src).tokenize();
    Parser p(std::move(tokens));
    auto prog = p.parse_program();
    if (p.has_errors()) {
        for (const auto& e : p.errors()) {
            std::cerr << e.line << ":" << e.column << " " << e.what() << "\n";
        }
    }
    return prog;
}

TEST(Parser, LetStmt) {
    auto prog = parse("let x = 1\n");
    ASSERT_EQ(prog->stmts.size(), 1u);
    auto* s = dynamic_cast<LetStmt*>(prog->stmts[0].get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->name, "x");
}

TEST(Parser, IfElifElse) {
    auto prog = parse(
        "if a then\n"
        "  let x = 1\n"
        "elif b then\n"
        "  let x = 2\n"
        "else\n"
        "  let x = 3\n"
        "end\n");
    ASSERT_EQ(prog->stmts.size(), 1u);
    auto* s = dynamic_cast<IfStmt*>(prog->stmts[0].get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->branches.size(), 2u);   // if + elif
    EXPECT_EQ(s->else_body.size(), 1u);
}

TEST(Parser, ForLoop) {
    auto prog = parse("for i in 0..10 do\n  i\nend\n");
    auto* s = dynamic_cast<ForStmt*>(prog->stmts[0].get());
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->var, "i");
    EXPECT_EQ(s->body.size(), 1u);
}

TEST(Parser, MissingThenReportsError) {
    auto tokens = Lexer("if a\n  let x = 1\nend\n").tokenize();
    Parser p(std::move(tokens));
    auto prog = p.parse_program();
    ASSERT_TRUE(p.has_errors());
    EXPECT_NE(p.errors()[0].what(), std::string());
}

TEST(Parser, RecoversFromErrorAndContinues) {
    auto tokens = Lexer(
        "let = 1\n"        // 缺标识符
        "let y = 2\n"      // 应该能正常解析
    ).tokenize();
    Parser p(std::move(tokens));
    auto prog = p.parse_program();
    EXPECT_EQ(p.errors().size(), 1u);  // 只有一条错误（panic 模式）
    // 第二条 let 仍能解析出来
    bool found_y = false;
    for (auto& s : prog->stmts) {
        if (auto* ls = dynamic_cast<LetStmt*>(s.get())) {
            if (ls->name == "y") found_y = true;
        }
    }
    EXPECT_TRUE(found_y);
}
```

最后一组测试是这章最重要的测试——它验证 parser 在错误后**能继
续工作**。这是手写 parser 相比工具生成的核心优势，一定要写覆盖。

## 3.11 我们暂时没处理的几件事

* **表达式 parsing**：占位为 `parse_expression()`，下一章用 Pratt
  Parser 实现；
* **左值合法性**：`1 = 2` 是合法 expr，但作为语句应该报错；放
  Compiler 阶段做；
* **`break` / `continue`**：暂时没有，第 9 章 Evaluator 章节会顺
  便加上；
* **多行表达式**：`(\n a + b \n)` 这种括号内换行；目前 NEWLINE 在
  `match` 里要小心吞掉。下一章 Pratt Parser 会顺便处理。

---

下一章是 **第 4 章 Pratt Parser**——我们用这个非常优雅的小算法
处理运算符优先级与结合性，避开传统教材里那种"每个优先级一层
parse_addition / parse_multiplication / parse_unary"的恶心嵌套。
你会看到加一个新运算符变成**只改一张优先级表**的事，可读性碾压
传统写法。
