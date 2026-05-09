# 第 2 章　Lexer：把字符流切成 Token

Lexer（词法分析器，也叫 scanner / tokenizer）是流水线的第一段。
它的工作非常单纯：**把一长串字符切成一颗颗 Token**。

```
"let x = 1 + 2"
        │
        ▼
[LET] [IDENT("x")] [EQUAL] [NUMBER(1)] [PLUS] [NUMBER(2)] [EOF]
```

听起来像 `split(' ')` 就够了——但要正确处理字符串字面量、注释、
小数、关键字 vs 标识符、行号列号、Unicode 转义、错误恢复，会发现
"切字符串"这件事远比想象中麻烦。这一章我们慢慢把这些坑都踩一遍。

## 2.1 设计：手写 vs 工具生成

业界的几条路线：

| 方案 | 代表 | 适合场景 |
| --- | --- | --- |
| 工具生成（lex / flex / re2c） | 早期 GCC、PHP | 词法规则庞大、稳定 |
| 正则表达式驱动 | 一些动态语言 | 原型快、性能差 |
| 手写状态机 | Lua、Python、Go、GDScript | 性能、错误信息、可读性都最好 |

我们走第三条。手写 Lexer 看起来"重复劳动"，但**它的代码量比想象
中小**——300 行能覆盖一门成熟脚本语言的全部词法。而且：

* 每个 token 出错时你都能精准给出一句"第 3 行第 12 列：未闭合的
  字符串"，工具生成很难做到；
* 不需要构建期跑代码生成，工程结构简单；
* 性能上，一个手写的 Lexer 能轻松做到每秒几百 MB 的吞吐。

## 2.2 Token 的 C++ 表示

先看 `src/token.h`：

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <variant>

namespace mini {

enum class TokenType : std::uint8_t {
    // 字面量
    NUMBER,         // 整数或浮点
    STRING,         // "..." 字符串
    IDENT,          // 标识符
    // 关键字
    LET, FN, IF, ELIF, ELSE, THEN, END,
    WHILE, DO, FOR, IN, RETURN,
    AND, OR, NOT,
    TRUE, FALSE, NIL,
    // 单/双字符运算符
    PLUS, MINUS, STAR, SLASH, PERCENT,
    EQUAL,                          // =
    EQUAL_EQUAL, BANG_EQUAL,        // == !=
    LESS, LESS_EQUAL,               // < <=
    GREATER, GREATER_EQUAL,         // > >=
    DOT_DOT,                        // ..  字符串拼接 / range
    // 标点
    LPAREN, RPAREN,
    LBRACKET, RBRACKET,
    LBRACE, RBRACE,
    COMMA, COLON, NEWLINE,
    // 终止
    END_OF_FILE,
    ERROR,
};

struct Token {
    TokenType type;
    std::string lexeme;       // 原始文本（NUMBER 也保留原文）
    // 字面量数值在 Lexer 阶段就解析出来，避免下游再 parse 一次
    std::variant<std::monostate, std::int64_t, double, std::string> literal;
    int line = 1;             // 1-based
    int column = 1;           // 1-based，按字符计
};

const char* token_name(TokenType t);

}  // namespace mini
```

几个值得展开的设计：

1. **关键字也是 TokenType**，不是单独的 `IDENT(keyword="let")` 后处
   理。这样 parser 写 `match(TokenType::LET)` 比 `match_ident("let")`
   清晰得多。
2. **数字字面量在 Lexer 阶段就解析为 `int64_t` / `double`**。让
   parser 干"识别表达式结构"的事，不再处理"`1.5e-3` 是合法浮点
   吗"这种字符细节。
3. **`lexeme` 永远保留原文**，主要给错误信息用：`expected ';',
   got 'foo'`。
4. **`NEWLINE` 是显式 token**：因为 Mini 像 Python 一样用换行作为
   语句分隔（不是 `;`）。但和 Python 的"INDENT/DEDENT 吐出来"不
   同，Mini 用 `then/do/end` 划块，所以 NEWLINE 只是可选分隔符，
   parser 里大多直接吞掉。
5. **`ERROR` 是一种 token**：遇到非法字符不立刻 throw，而是吐一个
   `ERROR` token 继续扫，方便编辑器一次性收集多个错误（"错误恢
   复"）。

## 2.3 Lexer 的状态

`src/lexer.h`：

```cpp
#pragma once
#include "token.h"
#include <string_view>
#include <vector>

namespace mini {

class Lexer {
public:
    explicit Lexer(std::string_view source) : src_(source) {}

    // 一次性扫完整源码
    std::vector<Token> tokenize();

private:
    std::string_view src_;
    std::size_t pos_ = 0;       // 下一个要读的字符下标
    std::size_t start_ = 0;     // 当前 token 的起点
    int line_ = 1;
    int column_ = 1;
    int token_col_ = 1;         // 当前 token 起点的列号

    bool at_end() const;
    char peek(std::size_t offset = 0) const;
    char advance();
    bool match(char expected);

    Token make(TokenType t);
    Token make(TokenType t,
               std::variant<std::monostate, std::int64_t, double, std::string> lit);
    Token error(const std::string& msg);

    Token scan_token();
    Token scan_number();
    Token scan_string();
    Token scan_identifier();
    void skip_whitespace_and_comments();
};

}  // namespace mini
```

设计要点：

* **Lexer 是一次性消费品**：`tokenize()` 跑一次就完事，不复用。这
  避免了"上次没扫干净"这种隐藏 bug。
* **`std::string_view src_`**：源码内存的所有权交给上层；Lexer 只
  做只读视图。这样我们能用 `mmap` 大文件、能做嵌入字符串测试，全
  都不需要复制。
* **`start_` / `pos_` 双指针**：Lexer 的标准模式。`start_` 是当前
  token 的起点，`pos_` 是下一个待读字符；扫完 token 后拷贝
  `src_.substr(start_, pos_ - start_)` 就是它的 lexeme。
* **`line_` 与 `token_col_`**：列号有两个——当前正在读的字符的列
  号 `column_`，和当前 token **起点**的列号 `token_col_`。错误信
  息用的是后者。

## 2.4 扫描循环：scan_token

整个 Lexer 的"主循环"长这样：

```cpp
std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    while (!at_end()) {
        skip_whitespace_and_comments();
        if (at_end()) break;
        start_ = pos_;
        token_col_ = column_;
        out.push_back(scan_token());
    }
    out.push_back(make(TokenType::END_OF_FILE));
    return out;
}
```

`scan_token` 是一个大 switch，按"看见的第一个字符"分派：

```cpp
Token Lexer::scan_token() {
    char c = advance();

    if (c == '\n') {
        Token t = make(TokenType::NEWLINE);
        line_++;
        column_ = 1;
        return t;
    }

    if (std::isdigit(static_cast<unsigned char>(c))) return scan_number();
    if (c == '_' || std::isalpha(static_cast<unsigned char>(c))) {
        return scan_identifier();
    }
    if (c == '"') return scan_string();

    switch (c) {
        case '(': return make(TokenType::LPAREN);
        case ')': return make(TokenType::RPAREN);
        case '[': return make(TokenType::LBRACKET);
        case ']': return make(TokenType::RBRACKET);
        case '{': return make(TokenType::LBRACE);
        case '}': return make(TokenType::RBRACE);
        case ',': return make(TokenType::COMMA);
        case ':': return make(TokenType::COLON);
        case '+': return make(TokenType::PLUS);
        case '-': return make(TokenType::MINUS);
        case '*': return make(TokenType::STAR);
        case '/': return make(TokenType::SLASH);
        case '%': return make(TokenType::PERCENT);

        case '=':
            return make(match('=') ? TokenType::EQUAL_EQUAL : TokenType::EQUAL);
        case '!':
            if (match('=')) return make(TokenType::BANG_EQUAL);
            return error("unexpected '!' (did you mean '!=' or 'not'?)");
        case '<':
            return make(match('=') ? TokenType::LESS_EQUAL : TokenType::LESS);
        case '>':
            return make(match('=') ? TokenType::GREATER_EQUAL : TokenType::GREATER);
        case '.':
            if (match('.')) return make(TokenType::DOT_DOT);
            return error("unexpected '.'  (did you mean '..'?)");
    }

    return error(std::string("unexpected character: '") + c + "'");
}
```

一些"可读性 > 一切"的小习惯：

* **`std::isdigit` 包一层 `unsigned char` cast**：避免 `char` 是
  signed 时传入负值导致 UB；
* **错误消息里给出"是不是想打 X"**：用户最常打错 `!` `.`，我们
  友善一点；
* **运算符 lookahead 1 字符就够**：Mini 故意没设计 `<<=` `>>>` 这
  种三字符运算符，所以 `match()` 判断一次完事。

## 2.5 跳过空白与注释

```cpp
void Lexer::skip_whitespace_and_comments() {
    while (!at_end()) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
                advance();
                break;
            case '#':
                // 行注释：吃到行末，但不消费 '\n' 本身
                while (!at_end() && peek() != '\n') advance();
                break;
            default:
                return;
        }
    }
}
```

注意几件事：

* **`\n` 不算空白**——它被 scan_token 当作 NEWLINE token 吐出来；
* **`\r` 算空白**——这样 Windows 的 CRLF 文件不会把每行结尾都解
  析成"NEWLINE NEWLINE"；
* **行注释结束在 `\n` 之前**，不消费 `\n`，让 NEWLINE 由主循环统
  一处理。

如果将来要支持块注释 `#[ ... ]#`，加一个状态分支即可，本章先省。

## 2.6 数字字面量

Mini 区分整数和浮点：`1` 是 int，`1.0` / `1e3` / `1.5e-2` 是 float。
这是为了让"整数除整数 = 整数"（避免 Python 2 vs 3 的历史包袱），
同时不丢失 IEEE754 的精度。

```cpp
Token Lexer::scan_number() {
    bool is_float = false;
    while (std::isdigit(static_cast<unsigned char>(peek()))) advance();

    // 小数点：必须后跟数字才认（否则 "0..10" 会被吃成 "0." + "." + "10"）
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        is_float = true;
        advance(); // '.'
        while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
    }

    // 指数
    if (peek() == 'e' || peek() == 'E') {
        is_float = true;
        advance();
        if (peek() == '+' || peek() == '-') advance();
        if (!std::isdigit(static_cast<unsigned char>(peek()))) {
            return error("invalid float literal: missing exponent digits");
        }
        while (std::isdigit(static_cast<unsigned char>(peek()))) advance();
    }

    std::string text(src_.substr(start_, pos_ - start_));
    if (is_float) {
        return make(TokenType::NUMBER, std::stod(text));
    } else {
        try {
            return make(TokenType::NUMBER,
                        static_cast<std::int64_t>(std::stoll(text)));
        } catch (const std::out_of_range&) {
            return error("integer literal out of range: " + text);
        }
    }
}
```

最关键的一行是 `peek() == '.' && std::isdigit(peek(1))`：

> **`0..10` 必须切成 `[0] [..] [10]`，不能切成 `[0.] [.] [10]`。**

这就是为什么 `peek()` 要支持 lookahead 1 字符。如果不做这个判断，
我们的 `for i in 0..10` 语法就崩了——一个看似无关的语法决策，会
反作用到 Lexer 的实现细节上。

## 2.7 字符串字面量

字符串支持 `\n` `\t` `\\` `\"` `\0` 几种常见转义，再加一个 Unicode
`\u{XXXX}`：

```cpp
Token Lexer::scan_string() {
    std::string value;
    while (!at_end() && peek() != '"') {
        char c = peek();
        if (c == '\n') {
            // 不允许跨行字符串（避免漏写 " 的整段文件被吃掉）
            return error("unterminated string (newline inside string)");
        }
        if (c == '\\') {
            advance();
            char esc = advance();
            switch (esc) {
                case 'n': value.push_back('\n'); break;
                case 't': value.push_back('\t'); break;
                case 'r': value.push_back('\r'); break;
                case '0': value.push_back('\0'); break;
                case '"': value.push_back('"');  break;
                case '\\': value.push_back('\\'); break;
                case 'u': {
                    if (advance() != '{') return error("expected '{' after \\u");
                    std::string hex;
                    while (!at_end() && peek() != '}') hex.push_back(advance());
                    if (advance() != '}') return error("unterminated \\u{...}");
                    auto cp = static_cast<std::uint32_t>(
                        std::stoul(hex, nullptr, 16));
                    encode_utf8(cp, value);
                    break;
                }
                default:
                    return error(std::string("invalid escape: \\") + esc);
            }
        } else {
            value.push_back(advance());
        }
    }
    if (at_end()) return error("unterminated string");
    advance(); // 吃掉收尾的 "
    return make(TokenType::STRING, std::move(value));
}
```

`encode_utf8` 是个 30 行小函数，把 `uint32_t` 码点编成 1~4 字节的
UTF-8，这里略去——脚本语言的源码字符串内部存 UTF-8 是事实标准。

注意我们**禁止字符串里直接换行**——这是经验之谈：用户写代码 90%
的"卡住了不报错"都来自漏了一个引号，把后面整个文件吃成字符串。
显式禁止跨行后，错误能在第一时间报在出问题的那一行。

需要长字符串？用 `..` 拼接：

```python
let s = "first line\n"
     .. "second line\n"
```

这是个语法学家会皱眉、用户毫不在意的小取舍。

## 2.8 标识符与关键字

```cpp
Token Lexer::scan_identifier() {
    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
        advance();
    }
    std::string text(src_.substr(start_, pos_ - start_));

    // 关键字查表
    static const std::unordered_map<std::string, TokenType> keywords = {
        {"let",    TokenType::LET},
        {"fn",     TokenType::FN},
        {"if",     TokenType::IF},
        {"elif",   TokenType::ELIF},
        {"else",   TokenType::ELSE},
        {"then",   TokenType::THEN},
        {"end",    TokenType::END},
        {"while",  TokenType::WHILE},
        {"do",     TokenType::DO},
        {"for",    TokenType::FOR},
        {"in",     TokenType::IN},
        {"return", TokenType::RETURN},
        {"and",    TokenType::AND},
        {"or",     TokenType::OR},
        {"not",    TokenType::NOT},
        {"true",   TokenType::TRUE},
        {"false",  TokenType::FALSE},
        {"nil",    TokenType::NIL},
    };

    auto it = keywords.find(text);
    if (it != keywords.end()) {
        // 关键字也保留原文 lexeme，方便错误信息
        Token t = make(it->second);
        t.lexeme = std::move(text);
        return t;
    }
    return make(TokenType::IDENT, std::move(text));
}
```

这里用 `unordered_map` 已经够快（关键字数 < 30）。如果你追求极致，
可以做"完美哈希"或按长度分桶——但实测在普通项目里完全无感，没
必要折腾。

更值得说的是**关键字数组里没有的"假关键字"**：比如 `print`、
`len`、`type`——它们在 Mini 里就是普通的全局变量，不是关键字。
这样写：

```python
let print = my_print   # 合法：覆盖 print
print("hi")            # 调用新的 print
```

这是 Lua / JavaScript 的传统。Python 在 3.0 把 `print` 从语句变函
数也是这个原因——**关键字越少，语言扩展性越好**。

## 2.9 错误处理：吐 ERROR token 而不是抛异常

我们不在 Lexer 里 throw。原因：

* 编辑器/REPL 希望"一次性看到所有词法错误"，不希望第一个错就停；
* 异常携带的栈对终端用户没意义；
* 字符串/注释这种状态，错误恢复策略很自然——"跳到下一个安全字符
  重新开始"。

```cpp
Token Lexer::error(const std::string& msg) {
    Token t;
    t.type = TokenType::ERROR;
    t.lexeme = msg;
    t.line = line_;
    t.column = token_col_;
    return t;
}
```

下游 parser 只要看到 `ERROR` token 就把 `lexeme` 当错误消息打出
来，并跳过它继续。这种"错误也是 token"的模式在 GDScript、Rust
（早期 libsyntax）都用过。

## 2.10 第一组单元测试

到这一步可以写第一组真正能跑的测试了。我们用 GoogleTest 做骨架，
但只用最基础的 `EXPECT_EQ`，不依赖任何高级 mock：

`tests/lex_test.cpp`：

```cpp
#include "lexer.h"
#include <gtest/gtest.h>

using namespace mini;

static std::vector<Token> lex(std::string_view src) {
    return Lexer(src).tokenize();
}

TEST(Lexer, Empty) {
    auto t = lex("");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[0].type, TokenType::END_OF_FILE);
}

TEST(Lexer, IntLiteral) {
    auto t = lex("42");
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0].type, TokenType::NUMBER);
    EXPECT_EQ(std::get<std::int64_t>(t[0].literal), 42);
}

TEST(Lexer, FloatLiteral) {
    auto t = lex("3.14");
    EXPECT_EQ(t[0].type, TokenType::NUMBER);
    EXPECT_DOUBLE_EQ(std::get<double>(t[0].literal), 3.14);
}

TEST(Lexer, RangeNotMisparsedAsFloat) {
    auto t = lex("0..10");
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t[0].type, TokenType::NUMBER);
    EXPECT_EQ(std::get<std::int64_t>(t[0].literal), 0);
    EXPECT_EQ(t[1].type, TokenType::DOT_DOT);
    EXPECT_EQ(t[2].type, TokenType::NUMBER);
    EXPECT_EQ(std::get<std::int64_t>(t[2].literal), 10);
}

TEST(Lexer, StringWithEscape) {
    auto t = lex(R"("hello\n\tworld")");
    ASSERT_EQ(t[0].type, TokenType::STRING);
    EXPECT_EQ(std::get<std::string>(t[0].literal), "hello\n\tworld");
}

TEST(Lexer, KeywordVsIdent) {
    auto t = lex("let letter = 1");
    EXPECT_EQ(t[0].type, TokenType::LET);
    EXPECT_EQ(t[1].type, TokenType::IDENT);
    EXPECT_EQ(t[1].lexeme, "letter");
    EXPECT_EQ(t[2].type, TokenType::EQUAL);
    EXPECT_EQ(t[3].type, TokenType::NUMBER);
}

TEST(Lexer, UnterminatedString) {
    auto t = lex(R"("oops)");
    EXPECT_EQ(t[0].type, TokenType::ERROR);
}

TEST(Lexer, LineColumnTracking) {
    auto t = lex("let x =\n  42");
    // x 在第 1 行第 5 列
    EXPECT_EQ(t[1].line, 1);
    EXPECT_EQ(t[1].column, 5);
    // 42 在第 2 行第 3 列
    auto num = std::find_if(t.begin(), t.end(),
        [](const Token& tk){ return tk.type == TokenType::NUMBER; });
    EXPECT_EQ(num->line, 2);
    EXPECT_EQ(num->column, 3);
}
```

跑通这些测试，Lexer 就完工了。注意最后一组**列号测试**——很多人
忘了写它，结果 Parser 报错时给出的列号永远偏移几个字符，调试时
很折磨。Lexer 的列号要在你写 Parser 之前就完全可信。

## 2.11 性能小贴士（先记下来，第三部分再回来调）

我们这章不调性能，但提前列几个常见瓶颈，方便你日后回头优化时知
道往哪里看：

1. **关键字 hash 查表**：现在每次 `scan_identifier` 都构造一次
   `std::unordered_map`？不，`static const` 只构造一次。但
   `unordered_map` 的 hash 比线性搜索 30 个 case 还慢——优化时改
   成"按长度分桶 + memcmp"会快 2~3 倍。
2. **`std::string` 拷贝**：每个 IDENT 都 `substr` 拷一次。优化时
   改成 `std::string_view` 指向 `src_` 即可，前提是源码生命周期长
   于 token。
3. **字符分类**：`std::isdigit` 在 MSVC 上有 locale 检查，比手写
   `c >= '0' && c <= '9'` 慢一截。

性能这件事**不要现在做**——你还没跑通解释器，调它毫无意义。

---

下一章我们把 Token 流喂给 Parser，构造 AST。会用到一种叫 **Pratt
parsing** 的小技巧来优雅地处理 `1 + 2 * 3 ** 4` 这种带优先级和结
合性的表达式——它比传统的"每个优先级一层递归函数"短得多，也好
理解得多。
