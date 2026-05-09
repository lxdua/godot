# 第 4 章　Pratt Parser：优雅地处理运算符优先级

上一章我们留了个悬念：`parse_expression()` 是个空壳。这一章把它
填满。

表达式 parsing 是手写解析器最容易写糟的地方。新手通常会用教科书
教的"每个优先级一层函数"模式：

```cpp
parse_expression  → parse_or
parse_or          → parse_and
parse_and         → parse_equality
parse_equality    → parse_comparison
parse_comparison  → parse_term
parse_term        → parse_factor
parse_factor      → parse_unary
parse_unary       → parse_call
parse_call        → parse_primary
```

九层调用栈、九个函数、九段几乎一样的循环——每加一个运算符就要
插一层。这种代码是真实存在的，比如 Lox 教程的早期版本。但我们有
更好的选择：**Pratt parsing**（也叫 top-down operator precedence
parsing）。它是 Vaughan Pratt 在 1973 年发明的小算法，被 Crockford
在《JavaScript: The Good Parts》后翻红，今天 V8、Roslyn、TypeScript、
GDScript 全都在用它。

## 4.1 核心想法

Pratt 的洞察是：**别把"优先级"做成调用栈深度，把它做成数据**。

我们给每个 token 类型挂三样东西：

* **prefix 函数**：当这个 token 出现在表达式开头时怎么解析（数字、
  括号、`-x`、`not x`...）；
* **infix 函数**：当这个 token 出现在两个表达式之间时怎么解析
  （`+`、`*`、`==`、`(` 函数调用、`[` 下标...）；
* **优先级**：这个 token 作为 infix 时的"绑定力"。

主循环只有十几行：

```
parse_expression(min_prec):
    left = prefix_of(current).parse()
    while precedence_of_infix(current) > min_prec:
        op = advance()
        left = infix_of(op).parse(left)
    return left
```

`min_prec` 是"我至少要比这个优先级强才接着合并"。这一行让整个
算法工作起来。

举例：解析 `1 + 2 * 3`，初始 `min_prec = 0`：

```
parse_expression(0)
    left = 1
    peek = '+' (prec=10) > 0 → 进入
        op = '+'
        right = parse_expression(10)        # '+' 是左结合，传 prec
            left = 2
            peek = '*' (prec=20) > 10 → 进入
                op = '*'
                right = parse_expression(20)
                    left = 3
                    peek = EOF → return 3
                left = BinOp(*, 2, 3)
            peek = EOF → return BinOp(*, 2, 3)
        left = BinOp(+, 1, BinOp(*, 2, 3))   ✓ 优先级正确
```

如果是 `1 * 2 + 3`：

```
parse_expression(0)
    left = 1
    peek = '*' (prec=20) > 0 → 进入
        right = parse_expression(20)
            left = 2
            peek = '+' (prec=10) > 20 ? 否 → return 2
        left = BinOp(*, 1, 2)
    peek = '+' (prec=10) > 0 → 进入
        right = parse_expression(10)
            left = 3
            peek = EOF
        left = BinOp(+, BinOp(*, 1, 2), 3)   ✓
```

整个算法就是"**把更高优先级的运算符吸进当前子表达式**"。一旦
peek 的优先级**不高于** `min_prec`，就把控制权交还给上层。

## 4.2 左结合 vs 右结合：±1 的小诡计

`+` 是左结合：`1 + 2 + 3` 应该是 `(1 + 2) + 3`。
`=` 是右结合：`a = b = 1` 应该是 `a = (b = 1)`。
`**`（幂）是右结合：`2 ** 3 ** 2` 应该是 `2 ** (3 ** 2) = 512`。

实现方式只差一个数字：

* 左结合：递归调用传 `parse_expression(prec)`——下一次循环时同
  优先级 token **不会** `> prec`，于是停下来，新左侧节点重新由外
  层吸收；
* 右结合：递归调用传 `parse_expression(prec - 1)`——同优先级 token
  **会** `> prec - 1`，于是被吸到右子树里。

```cpp
auto right = parse_expression(rule.is_right_associative ? prec - 1 : prec);
```

就这一行。Pratt parsing 的精妙就在这里——**结合性不需要单独的代
码路径，只需调整 min_prec**。

## 4.3 Mini 的优先级表

把 Mini 所有运算符按优先级从低到高排：

| 优先级 | 运算符        | 说明              | 结合性 |
| ------ | ------------- | ----------------- | ------ |
| 1      | `=`           | 赋值              | 右     |
| 2      | `or`          | 短路或            | 左     |
| 3      | `and`         | 短路与            | 左     |
| 4      | `==` `!=`     | 相等              | 左     |
| 5      | `<` `<=` `>` `>=` | 比较          | 左     |
| 6      | `..`          | 字符串拼接        | 右     |
| 7      | `+` `-`       | 加减              | 左     |
| 8      | `*` `/` `%`   | 乘除模            | 左     |
| 9      | `not` `-`(unary) | 一元           | 前缀   |
| 10     | `()` `[]` `.` | 调用 / 下标 / 成员 | 后缀  |

注意 `..` 在 Mini 里是字符串拼接（仿 Lua），优先级低于算术——这
样 `"x = " .. x + 1` 等价于 `"x = " .. (x + 1)` 而不是
`("x = " .. x) + 1`。

## 4.4 把规则做成表

```cpp
// src/parser.cpp
struct PrattRule {
    using PrefixFn = std::unique_ptr<Expr> (Parser::*)();
    using InfixFn  = std::unique_ptr<Expr> (Parser::*)(std::unique_ptr<Expr>);
    PrefixFn prefix = nullptr;
    InfixFn  infix  = nullptr;
    int prec = 0;
    bool right_assoc = false;
};

static const PrattRule& get_rule(TokenType t);
```

整张表（节选关键项）：

```cpp
static const std::unordered_map<TokenType, PrattRule>& rules() {
    static const std::unordered_map<TokenType, PrattRule> R = {
        // 字面量与标识符（只有 prefix）
        {TokenType::NUMBER, {&Parser::parse_number}},
        {TokenType::STRING, {&Parser::parse_string}},
        {TokenType::TRUE,   {&Parser::parse_bool_lit}},
        {TokenType::FALSE,  {&Parser::parse_bool_lit}},
        {TokenType::NIL,    {&Parser::parse_nil_lit}},
        {TokenType::IDENT,  {&Parser::parse_ident}},
        {TokenType::LPAREN, {&Parser::parse_group, &Parser::parse_call, 10}},
        {TokenType::LBRACKET, {&Parser::parse_array_lit, &Parser::parse_index, 10}},
        {TokenType::LBRACE, {&Parser::parse_table_lit}},
        // 一元 / 二元都用 '-'
        {TokenType::MINUS,  {&Parser::parse_unary, &Parser::parse_binary, 7}},
        {TokenType::NOT,    {&Parser::parse_unary}},
        // 纯二元
        {TokenType::PLUS,           {nullptr, &Parser::parse_binary, 7}},
        {TokenType::STAR,           {nullptr, &Parser::parse_binary, 8}},
        {TokenType::SLASH,          {nullptr, &Parser::parse_binary, 8}},
        {TokenType::PERCENT,        {nullptr, &Parser::parse_binary, 8}},
        {TokenType::DOT_DOT,        {nullptr, &Parser::parse_binary, 6, true}},
        {TokenType::EQUAL_EQUAL,    {nullptr, &Parser::parse_binary, 4}},
        {TokenType::BANG_EQUAL,     {nullptr, &Parser::parse_binary, 4}},
        {TokenType::LESS,           {nullptr, &Parser::parse_binary, 5}},
        {TokenType::LESS_EQUAL,     {nullptr, &Parser::parse_binary, 5}},
        {TokenType::GREATER,        {nullptr, &Parser::parse_binary, 5}},
        {TokenType::GREATER_EQUAL,  {nullptr, &Parser::parse_binary, 5}},
        {TokenType::AND,            {nullptr, &Parser::parse_logical, 3}},
        {TokenType::OR,             {nullptr, &Parser::parse_logical, 2}},
        {TokenType::EQUAL,          {nullptr, &Parser::parse_assign, 1, true}},
    };
    return R;
}

const PrattRule& get_rule(TokenType t) {
    static const PrattRule empty{};
    auto it = rules().find(t);
    return it == rules().end() ? empty : it->second;
}
```

这张表是整个表达式 parser 的**全部规则**。后面想加一个 `**`（幂
运算）？在表里多一行。想加一个 `?:` 三元运算？多一行+一个 infix
函数。Pratt Parser 的扩展性远胜传统递归下降。

## 4.5 主循环

```cpp
std::unique_ptr<Expr> Parser::parse_expression() {
    return parse_precedence(1);   // 1 = 任何优先级都能进入
}

std::unique_ptr<Expr> Parser::parse_precedence(int min_prec) {
    const Token& tok = peek();
    const PrattRule& rule = get_rule(tok.type);
    if (!rule.prefix) {
        error_at(tok, "expected expression, got '" + tok.lexeme + "'");
    }
    auto left = (this->*rule.prefix)();

    while (true) {
        const PrattRule& irule = get_rule(peek().type);
        if (!irule.infix || irule.prec < min_prec) break;

        Token op = advance();
        // 把 op 信息塞进调用上下文：用一个成员变量传，避免改签名
        current_op_ = op;
        left = (this->*irule.infix)(std::move(left));
    }
    return left;
}
```

`current_op_` 是把"刚被消费的 infix 运算符"传给 infix handler 的
小技巧——比改所有 infix 函数签名加一个 `Token op` 参数更轻量。

## 4.6 Prefix 与 Infix 的实际实现

最简单的字面量：

```cpp
std::unique_ptr<Expr> Parser::parse_number() {
    Token t = advance();
    auto n = std::make_unique<NumberLit>();
    n->line = t.line;
    if (std::holds_alternative<std::int64_t>(t.literal)) {
        n->is_int = true;
        n->ivalue = std::get<std::int64_t>(t.literal);
    } else {
        n->is_int = false;
        n->fvalue = std::get<double>(t.literal);
    }
    return n;
}

std::unique_ptr<Expr> Parser::parse_ident() {
    Token t = advance();
    auto e = std::make_unique<IdentExpr>();
    e->line = t.line;
    e->name = t.lexeme;
    return e;
}
```

括号表达式：

```cpp
std::unique_ptr<Expr> Parser::parse_group() {
    advance(); // '('
    auto e = parse_expression();
    expect(TokenType::RPAREN, "')' to close group");
    return e;
}
```

一元运算（`-x` / `not x`）：

```cpp
std::unique_ptr<Expr> Parser::parse_unary() {
    Token op = advance();
    // 一元的 min_prec 用 9（比所有二元都高）
    auto operand = parse_precedence(9);
    auto u = std::make_unique<UnaryOp>();
    u->line = op.line;
    u->op = op.type;
    u->operand = std::move(operand);
    return u;
}
```

二元：

```cpp
std::unique_ptr<Expr> Parser::parse_binary(std::unique_ptr<Expr> left) {
    Token op = current_op_;
    const PrattRule& r = get_rule(op.type);
    auto right = parse_precedence(r.right_assoc ? r.prec : r.prec + 1);
    auto b = std::make_unique<BinaryOp>();
    b->line = op.line;
    b->op = op.type;
    b->left = std::move(left);
    b->right = std::move(right);
    return b;
}
```

注意**左结合传 `prec + 1`，右结合传 `prec`** ——这是 4.2 节那条
±1 诡计的另一种等价写法（哪种更直观看个人，效果完全一样）。

逻辑（短路）和算术分开是因为后续求值时它们的字节码不同：

```cpp
std::unique_ptr<Expr> Parser::parse_logical(std::unique_ptr<Expr> left) {
    Token op = current_op_;
    const PrattRule& r = get_rule(op.type);
    auto right = parse_precedence(r.prec + 1);
    auto l = std::make_unique<LogicalOp>();
    l->line = op.line;
    l->op = op.type;   // AND / OR
    l->left = std::move(left);
    l->right = std::move(right);
    return l;
}
```

赋值：

```cpp
std::unique_ptr<Expr> Parser::parse_assign(std::unique_ptr<Expr> left) {
    Token eq = current_op_;
    auto value = parse_precedence(1);  // 右结合，传 1
    auto a = std::make_unique<AssignExpr>();
    a->line = eq.line;
    a->target = std::move(left);  // 左值合法性后续检查
    a->value = std::move(value);
    return a;
}
```

函数调用：

```cpp
std::unique_ptr<Expr> Parser::parse_call(std::unique_ptr<Expr> callee) {
    auto c = std::make_unique<CallExpr>();
    c->line = callee->line;
    c->callee = std::move(callee);
    if (!check(TokenType::RPAREN)) {
        do {
            c->args.push_back(parse_expression());
        } while (match(TokenType::COMMA));
    }
    expect(TokenType::RPAREN, "')' after arguments");
    return c;
}
```

下标：

```cpp
std::unique_ptr<Expr> Parser::parse_index(std::unique_ptr<Expr> obj) {
    auto e = std::make_unique<IndexExpr>();
    e->line = obj->line;
    e->object = std::move(obj);
    e->index = parse_expression();
    expect(TokenType::RBRACKET, "']' after index");
    return e;
}
```

数组与表字面量：

```cpp
std::unique_ptr<Expr> Parser::parse_array_lit() {
    Token open = advance(); // '['
    auto a = std::make_unique<ArrayLit>();
    a->line = open.line;
    if (!check(TokenType::RBRACKET)) {
        do {
            a->elements.push_back(parse_expression());
        } while (match(TokenType::COMMA));
    }
    expect(TokenType::RBRACKET, "']' to close array");
    return a;
}

std::unique_ptr<Expr> Parser::parse_table_lit() {
    Token open = advance(); // '{'
    auto t = std::make_unique<TableLit>();
    t->line = open.line;
    if (!check(TokenType::RBRACE)) {
        do {
            auto key = parse_expression();
            expect(TokenType::COLON, "':' between key and value");
            auto val = parse_expression();
            t->entries.push_back({std::move(key), std::move(val)});
        } while (match(TokenType::COMMA));
    }
    expect(TokenType::RBRACE, "'}' to close table");
    return t;
}
```

## 4.7 一个有趣的边界情况：`-` 既是一元又是二元

`-` 在 `let x = -1` 里是一元，在 `let x = a - b` 里是二元。
Pratt Parser 处理这种情况非常优雅——**同一个 TokenType 的规则同
时填 prefix 和 infix 即可**：

```cpp
{TokenType::MINUS, {&Parser::parse_unary, &Parser::parse_binary, 7}},
```

由于主循环先调 prefix 再循环 infix，自然就根据"位置"分派对了：

* `-1` 中第一个 token 就是 `-`，走 prefix（parse_unary）；
* `a - b` 中 `-` 在 `a` 之后出现，走 infix（parse_binary）。

不需要任何"前一个 token 是不是运算符"这种 hack。GDScript 的 Pratt
Parser 也是这么做的。

## 4.8 处理表达式内的换行

`(`、`[`、`{` 内允许跨行：

```python
let arr = [
    1,
    2,
    3,
]
```

实现时只需在这几个 prefix 函数里**吞掉 NEWLINE**：

```cpp
std::unique_ptr<Expr> Parser::parse_array_lit() {
    Token open = advance();
    auto a = std::make_unique<ArrayLit>();
    a->line = open.line;
    skip_newlines();
    if (!check(TokenType::RBRACKET)) {
        do {
            skip_newlines();
            a->elements.push_back(parse_expression());
            skip_newlines();
        } while (match(TokenType::COMMA));
    }
    skip_newlines();
    expect(TokenType::RBRACKET, "']' to close array");
    return a;
}
```

`skip_newlines()` 是个三行 helper：

```cpp
void Parser::skip_newlines() {
    while (check(TokenType::NEWLINE)) advance();
}
```

注意只在"开放结构"里吞 NEWLINE，外层语句 parser 仍依赖 NEWLINE
作为 terminator。

## 4.9 一组 Pratt Parser 的单元测试

```cpp
#include "lexer.h"
#include "parser.h"
#include <gtest/gtest.h>

using namespace mini;

static std::unique_ptr<Expr> parse_expr(std::string_view src) {
    auto tokens = Lexer(src).tokenize();
    Parser p(std::move(tokens));
    auto prog = p.parse_program();
    if (prog->stmts.empty()) return nullptr;
    auto* es = dynamic_cast<ExprStmt*>(prog->stmts[0].get());
    if (!es) return nullptr;
    return std::move(es->expr);
}

TEST(Pratt, AdditionAndMultiplication) {
    auto e = parse_expr("1 + 2 * 3\n");
    auto* root = dynamic_cast<BinaryOp*>(e.get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op, TokenType::PLUS);
    auto* right = dynamic_cast<BinaryOp*>(root->right.get());
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(right->op, TokenType::STAR);
}

TEST(Pratt, LeftAssociative) {
    auto e = parse_expr("1 - 2 - 3\n");
    auto* root = dynamic_cast<BinaryOp*>(e.get());
    ASSERT_NE(root, nullptr);
    // (1 - 2) - 3
    auto* left = dynamic_cast<BinaryOp*>(root->left.get());
    ASSERT_NE(left, nullptr);
}

TEST(Pratt, RightAssociativeAssign) {
    auto e = parse_expr("a = b = 1\n");
    auto* root = dynamic_cast<AssignExpr*>(e.get());
    ASSERT_NE(root, nullptr);
    // 右子树仍是 AssignExpr
    auto* inner = dynamic_cast<AssignExpr*>(root->value.get());
    ASSERT_NE(inner, nullptr);
}

TEST(Pratt, UnaryMinus) {
    auto e = parse_expr("-1 + 2\n");
    auto* root = dynamic_cast<BinaryOp*>(e.get());
    ASSERT_NE(root, nullptr);
    auto* left = dynamic_cast<UnaryOp*>(root->left.get());
    ASSERT_NE(left, nullptr);
    EXPECT_EQ(left->op, TokenType::MINUS);
}

TEST(Pratt, CallChain) {
    auto e = parse_expr("foo(1)(2)\n");
    auto* outer = dynamic_cast<CallExpr*>(e.get());
    ASSERT_NE(outer, nullptr);
    auto* inner = dynamic_cast<CallExpr*>(outer->callee.get());
    ASSERT_NE(inner, nullptr);
}

TEST(Pratt, IndexAndCall) {
    auto e = parse_expr("a[0](1, 2)\n");
    auto* call = dynamic_cast<CallExpr*>(e.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->args.size(), 2u);
    auto* idx = dynamic_cast<IndexExpr*>(call->callee.get());
    ASSERT_NE(idx, nullptr);
}

TEST(Pratt, LogicalShortCircuitNode) {
    auto e = parse_expr("a and b or c\n");
    // 'or' 优先级低，根应是 LogicalOp(OR)
    auto* root = dynamic_cast<LogicalOp*>(e.get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op, TokenType::OR);
}

TEST(Pratt, MultilineArrayLiteral) {
    auto e = parse_expr("[\n  1,\n  2,\n  3,\n]\n");
    auto* a = dynamic_cast<ArrayLit*>(e.get());
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->elements.size(), 3u);
}
```

## 4.10 为什么 Pratt Parser 值得学

展开比较一下两种写法。传统递归下降：

```cpp
parse_term() {
    auto left = parse_factor();
    while (match(PLUS) || match(MINUS)) {
        Token op = previous();
        auto right = parse_factor();
        left = make_binary(op, left, right);
    }
    return left;
}
parse_factor() {
    auto left = parse_unary();
    while (match(STAR) || match(SLASH)) { ... }
    return left;
}
parse_unary() { ... }
parse_call() { ... }
parse_primary() { ... }
```

* 加一个 `**`（介于 `*` 和 unary 之间）→ 插一个 `parse_power` 函
  数 + 改 `parse_factor` 调它；
* 改 `+` 为右结合 → 改 `parse_term` 内部的 while 改成递归；
* 加 `?:` 三元 → 在某层 while 里特判，逻辑乱掉。

Pratt：

* 加 `**` → 优先级表加一行，写一个 5 行的 `parse_binary` 复用；
* 改结合性 → 表里 `right_assoc = true`；
* 加 `?:` → 加一个 infix 函数 + 表里一行。

**所有改动都是局部的**，没有"层级重排"。这就是为什么真实世界的
脚本语言几乎都用 Pratt：维护多年后语言还在加运算符，传统递归下
降会把你逼疯。

---

到这里 **第一部分（前端）** 就完整了：Lexer + Parser，能把任意
Mini 源码变成 AST。下一章我们会专门梳理 AST 节点的设计，把分散
在 `ast.h` 里的若干结构整理成一个 visitor 友好的层级，为第二部分
的求值器做准备。
