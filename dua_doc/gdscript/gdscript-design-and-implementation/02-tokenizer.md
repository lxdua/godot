# 第 2 章　词法分析：`GDScriptTokenizer`

> 本章对应源码：
> `modules/gdscript/gdscript_tokenizer.h`、`gdscript_tokenizer.cpp`、
> `gdscript_tokenizer_buffer.h`、`gdscript_tokenizer_buffer.cpp`。

从本章开始我们进入编译流水线的第一站——**词法分析（Lexing / Tokenizing）**。这一站的唯一任务是：把一串原始字符（`char32_t`）切分成一个个有语义标签的 **Token**，供 Parser 在下一阶段"按语法规则拼装"。

别看它任务简单，GDScript 的 Tokenizer 有几件事做得相当精致，值得单独成章：

1. 它要处理 **Python 风格的缩进语法**——`INDENT` / `DEDENT` 不是字符，而是由 Tokenizer "合成"出来的虚拟 Token。
2. 它要同时支持**两种输入源**：文本源码（`.gd`）和二进制 Token 缓存（`.gdc`）。
3. 它要和 **Lambda** 合作——Lambda 可能"嵌在"一个表达式内部，但表达式内部要支持多行；这与 Python 的缩进规则存在冲突，GDScript 用"缩进栈的栈"解决了它。
4. 它要为编辑器保留**注释、光标位置、错误队列**等额外信息。

本章按职责拆成四节：Token 结构 → 文本 Tokenizer 的主循环 → 缩进与行延续 → 二进制 Tokenizer。

## 2.1　Token：词法输出的最小单元

### 2.1.1　Token 结构体

`gdscript_tokenizer.h` 中 `GDScriptTokenizer::Token` 的定义非常紧凑：

```cpp
struct Token {
    enum Type { EMPTY, ANNOTATION, IDENTIFIER, LITERAL, /* ... TK_EOF, TK_MAX */ };

    Type type = EMPTY;
    Variant literal;                 // 字面量值（LITERAL / IDENTIFIER 时有意义）
    int start_line = 0;
    int start_column = 0;
    int end_line = 0;
    int end_column = 0;
    CursorPlace cursor_place = CURSOR_NONE;
    String source;                   // 原始源串切片（调试/错误提示用）

    const char *get_name() const;
    String get_debug_name() const;
    bool can_precede_bin_op() const; // 关键：给 Parser 判断二元运算上下文
    bool is_identifier() const;
    bool is_node_name() const;
    StringName get_identifier() const { return literal; }
};
```

几个关键点：

- **位置信息粒度到列**：`start_line/start_column` + `end_line/end_column` 让 Parser/Analyzer 在报错时能精确指出"从第几行第几列到第几行第几列"。
- **`literal` 用 `Variant` 复用**：标识符的字符串、数字的 `int`/`float`、字符串字面量、布尔常量、`null` ——统统塞进同一个 `Variant`。这也是为什么 `get_identifier()` 可以只 `return literal`：`StringName` 从 `Variant` 的隐式转换会取出里面存的字符串。
- **`cursor_place`**：编辑器用的"光标落在本 Token 的什么位置"。`CURSOR_BEGINNING`、`CURSOR_MIDDLE`、`CURSOR_END` 对于自动补全判断至关重要（例如光标在 `prin|` 这种"前缀位置"时要触发补全）。
- **`source`**：`Token` 的原始文本片段。调试模式下用来生成可读的错误消息。

### 2.1.2　Token 类型枚举与 ABI 约束

`Token::Type` 的枚举值超过 100 个，`gdscript_tokenizer.h:49` 起按类别分块排列：基础 / 比较 / 逻辑 / 位运算 / 算术 / 赋值 / 控制流 / 关键字 / 标点 / 空白 / 常量 / 错误改进 / 特殊。

这个枚举上面有一行至关重要的注释：

```cpp
// If this enum changes, please increment the TOKENIZER_VERSION in gdscript_tokenizer_buffer.h
```

为什么？因为**枚举值是 `.gdc` 文件里直接存储的字节**。`.gdc` 是 GDScript 的"预编译词法缓存"，部署到游戏里的 `.gd` 其实可以被换成 `.gdc`，由 `GDScriptTokenizerBuffer` 直接反序列化成 Token，跳过文本扫描。如果枚举顺序变了，旧 `.gdc` 会全部错位。

这里体现了 GDScript 工程的一个常见模式：**枚举即 ABI**。除了 `Token::Type`，后续章节里还会遇到：

- `GDScriptFunction::Opcode`（字节码版本）；
- `Variant::Type`（跨语言共用）；
- `ScriptInstance` 的 RPC 索引。

改动这些枚举时，都必须改对应的 VERSION。

### 2.1.3　`can_precede_bin_op()`：词法与语法的一个微妙契约

Token 上有个看起来很奇怪的方法：

```cpp
bool can_precede_bin_op() const;
```

这是给 Parser 用的。GDScript 的某些 Token 既可能出现在表达式开头（前缀），也可能出现在运算符后（中缀）。比如 `-` 既是一元取反，也是二元减法；`.` 既是成员访问，也可以是 `..` 区间（Range）的一部分。Parser 在决定下一 Token 如何解释时，经常要问一句："上一个 Token 是能作为'左侧操作数'的那种吗？" 这就由 `can_precede_bin_op()` 回答——实现上就是一个白名单：标识符、字面量、右括号、`self`、`super`、`DEDENT` 之类"可以作为值"的 Token 返回 `true`。

这是词法与语法之间**极轻微的耦合**。在一个纯 LR 或纯 LL 解析器里，这种信息通常在 Parser 里自己维护；但 GDScript 选择让 Token 自带这个谓词，代价是一点耦合，收益是 Parser 的前缀/中缀判定变成"读一个标志位"——契合它"只前瞻一个 Token"的设计。

## 2.2　两个实现：`GDScriptTokenizerText` 与 `GDScriptTokenizerBuffer`

`GDScriptTokenizer` 是抽象基类，对外只暴露一个核心 API：

```cpp
virtual Token scan() = 0;
```

它有两个具体子类，分别对应两种输入源：

| 子类 | 输入 | 用途 |
|---|---|---|
| `GDScriptTokenizerText` | 源码 `String` | 编辑器、热重载、从 `.gd` 文件加载 |
| `GDScriptTokenizerBuffer` | 二进制缓冲区 `Vector<uint8_t>` | 从 `.gdc` 预编译缓存加载（导出包中可选） |

两者的**输出流完全兼容**——Parser 根本不关心 Token 是从哪儿来的；它只调用 `scan()` 拿下一个 Token。这种"多产线，同下游"的设计为 Godot 的"部署即预编译"提供了可能：游戏发布时可以把所有 `.gd` 预先 tokenize 成 `.gdc`，启动更快、体积更小，甚至在一定程度上对源码做了混淆。

具体选择哪一种，发生在 `GDScriptParser::parse()`（见第 3 章）加载资源时。

## 2.3　`scan()`：文本 Tokenizer 的主循环

`GDScriptTokenizerText::scan()`（`gdscript_tokenizer.cpp:1355`）是整个词法分析的"心脏"。它的骨架是：

```
Token scan() {
    if (has_error()) return pop_error();        // 错误优先
    _skip_whitespace();                          // 吃空白、注释，同时可能触发缩进检查
    if (pending_newline) return last_newline;    // 有待发的 NEWLINE 先发
    if (has_error()) return pop_error();
    if (pending_indents != 0) return INDENT or DEDENT;  // 有待发的缩进变化先发
    if (_is_at_end()) return TK_EOF;
    char32_t c = _advance();
    if (c == '\\') { /* 行延续 */ return scan(); }
    if (is_digit(c))                return number();
    if (c == 'r' && quotechar)      return string();     // raw string 前缀
    if (is_unicode_identifier_start(c)) return potential_identifier();
    switch (c) { case '"'/'\'': return string();
                 case '@':      return annotation();
                 /* 单字符标点 */
                 /* 多字符运算符，依赖 _peek() 一瞥后决定 */ }
}
```

这里能读出 GDScript 词法器的几个关键设计选择：

1. **错误缓冲、延迟返回**：错误不是抛出异常，而是 `push_error()` 入队，下次 `scan()` 时 `pop_error()` 作为特殊 Token 返回。这样 Parser 可以像对待普通 Token 一样处理错误，并决定是"吞掉继续"还是"立即中止"。
2. **NEWLINE 与 INDENT/DEDENT 是"挂起队列"而非即时返回**：`pending_newline`、`pending_indents` 让 Tokenizer 在一次 `scan()` 调用里只生产一个 Token，却又能把"一次实际换行"对应的多个虚拟 Token（一个 NEWLINE + 若干 DEDENT）逐个吐给 Parser。
3. **行延续通过递归调用 `scan()`**：见到 `\ + \n`，置 `line_continuation = true`、跳过空白、`return scan()`——**整条反斜杠行从 Token 流中彻底消失**。这和 Python 完全一致。
4. **单 Token 前瞻**：需要区分 `+` 和 `+=` 时用 `_peek()` 瞥一眼下一字符即可；从不回溯，从不预读多字符。

下面几节把其中最有料的几个点展开。

### 2.3.1　标识符识别与关键字表

一旦首字符落入 `is_unicode_identifier_start(c)` 的范围，进入 `potential_identifier()`（`gdscript_tokenizer.cpp:563`）。它的逻辑非常整洁：

```cpp
while (is_unicode_identifier_continue(_peek())) {
    char32_t c = _advance();
    only_ascii = only_ascii && c < 128;
}
int len = _current - _start;

if (len == 1 && _peek(-1) == '_') {
    // 单独的下划线是独立 Token（用于 match 的通配）
    return make_token(Token::UNDERSCORE);
}

String name = String::utf32(Span(_start, len));
if (len < MIN_KEYWORD_LENGTH || len > MAX_KEYWORD_LENGTH) {
    return make_identifier(name);   // 长度不可能是关键字
}
if (!only_ascii) {
    // 关键字都是 ASCII，有非 ASCII 字符一定不是关键字
    return make_identifier(name);   // 另外会做 "visually similar" 警告
}

switch (_start[0]) {
    default:
        KEYWORDS(KEYWORD_GROUP_CASE, KEYWORD)   // X-Macro 展开
        break;
}
// true/false/null 作为字面量单独处理
if (len == 4 && name == "true") return make_literal(true);
if (len == 4 && name == "null") return make_literal(Variant());
if (len == 5 && name == "false") return make_literal(false);

return make_identifier(name);
```

有四个非常值得学习的工程细节：

#### 细节 ①　长度剪枝

任何关键字的长度都在 `[MIN_KEYWORD_LENGTH, MAX_KEYWORD_LENGTH]` 区间内（两个常量由 `KEYWORDS` 宏扫一遍算出）。只要读完的标识符长度超出这个区间，`memcmp` 都不用做——直接当作普通标识符返回。

#### 细节 ②　ASCII 剪枝

所有 GDScript 关键字都是纯 ASCII。如果标识符中出现任何非 ASCII 字符，也可以立刻走"非关键字"分支，顺便在 Debug 模式下跑一次"视觉混淆检测"（`TextServer::is_confusable`）：像 `а` (Cyrillic) 和 `a` (Latin) 字面完全一样，却是两个字符——如果一个标识符看起来像 `class` 但其实藏着一个西里尔字母，会触发警告。

#### 细节 ③　首字母分组的 X-Macro

真正的关键字识别通过一个 **X-Macro** 展开完成。`gdscript_tokenizer.cpp:486` 附近定义：

```cpp
#define KEYWORDS(KEYWORD_GROUP, KEYWORD) \
    KEYWORD_GROUP('a') \
      KEYWORD("as", Token::AS) \
      KEYWORD("and", Token::AND) \
      KEYWORD("assert", Token::ASSERT) \
      KEYWORD("await", Token::AWAIT) \
    KEYWORD_GROUP('b') \
      KEYWORD("break", Token::BREAK) \
      KEYWORD("breakpoint", Token::BREAKPOINT) \
    /* ... 一直到 PI / TAU / INF / NAN ... */
```

然后用不同的替换把它"实例化"两次：

- 在 `scan()` 内部：`KEYWORD_GROUP` 展开为 `break; case 'x':`，`KEYWORD` 展开为 `if (len==N && name==kw) return make_token(T);`。这样整个关键字列表变成一张**按首字母分派的 `switch`**，大多数情况下首字母一不对就立刻 `break`，对 KB 级源码的扫描非常友好。
- 在 `make_keyword_list()`：`KEYWORD_GROUP` 展开为空，`KEYWORD` 展开为 `list.push_back(kw)`，收集所有关键字字符串，调试模式下用于混淆检测和编辑器补全。

这是一个教科书级别的 X-Macro 运用——**同一份关键字表只写一遍，被多处消费**，增减关键字只改一处。

#### 细节 ④　`_` 是一个独立 Token

单独的下划线走 `Token::UNDERSCORE`，而不是一个名为 `_` 的 IDENTIFIER。这是为 `match` 语句的通配符模式准备的：

```gdscript
match value:
    1: print("one")
    _: print("other")   # 这里的 _ 要在语法层特判
```

如果 `_` 走普通标识符，就得在语义层对"名为 `_` 的标识符"做特判，显得别扭。把它升级成词法级别的 Token 才干净。

### 2.3.2　数字、字符串、注解

这三类都是"吃进一段连续字符，一次性产出一个 Token"的套路，细节不同但结构雷同。我们只强调 GDScript 特有的点：

- **`number()`**（`gdscript_tokenizer.cpp:670`）支持 `0x`/`0o`/`0b` 进制前缀、下划线分隔符（`1_000_000`）、科学计数法、后缀 `.`（`1.`）。解析时会构造 `int64_t` 或 `double`，存进 `Token::literal`。
- **`string()`**（`gdscript_tokenizer.cpp:849`）处理单引号、双引号、三引号多行字符串，支持 `r"..."` 原始字符串（不转义反斜杠）。它也负责**字符串节点路径 `$...`** 的部分工作（`$"path"` 的引号部分走 string）。
- **`annotation()`**（`gdscript_tokenizer.cpp:471`）处理 `@tool`、`@export`、`@onready` 等。注意它**只负责把 `@name` 扫成一个 Token**，注解的参数（括号内的表达式）由 Parser 用普通表达式规则解析，之后 Analyzer 再按名字分发到各个 `AnnotationNode` 处理器。

### 2.3.3　括号栈 `paren_stack`

`GDScriptTokenizerText` 持有一个 `List<char32_t> paren_stack`，进入 `(` / `[` / `{` 时 `push_paren()`，遇到 `)` / `]` / `}` 时 `pop_paren(expected)`。作用有二：

1. **配对检查**：若 `pop_paren('(')` 发现栈顶不是 `(`，立即产出 `make_paren_error(c)`，Parser 看到这个错误 Token 就知道括号不配对。
2. **决定 `multiline_mode`**：实际上 `multiline_mode` 和 `paren_stack` 并不完全耦合，但二者共同决定了"此时换行是真的换行，还是表达式内部的无意义换行"。见下一节。

## 2.4　缩进的合成：`INDENT` / `DEDENT` 从哪里来

GDScript 的语法是缩进敏感的：

```gdscript
if a > 0:
    print("positive")
    print("still inside if")
print("outside")
```

Parser 看到的 Token 流长这样：

```
IF IDENTIFIER(a) GREATER LITERAL(0) COLON NEWLINE
INDENT
  IDENTIFIER(print) PARENTHESIS_OPEN LITERAL("positive") PARENTHESIS_CLOSE NEWLINE
  IDENTIFIER(print) PARENTHESIS_OPEN LITERAL("still inside if") PARENTHESIS_CLOSE NEWLINE
DEDENT
IDENTIFIER(print) PARENTHESIS_OPEN LITERAL("outside") PARENTHESIS_CLOSE NEWLINE
TK_EOF
```

请注意：**源码里根本不存在 `INDENT` / `DEDENT` 这两个字符**，它们是 Tokenizer 在检测到缩进层级变化时"凭空"产出的虚拟 Token。Python 家族所有语言都有这套机制，GDScript 的实现放在 `check_indent()`（`gdscript_tokenizer.cpp:1124`）。

### 2.4.1　缩进栈 `indent_stack`

每次缩进加深，都把当前行的缩进字符数 `push_back()` 到 `List<int> indent_stack`；每次缩进变浅，把栈顶逐层弹出，每弹一层就"欠一个 DEDENT"。这部分逻辑的核心片段如下：

```cpp
int previous_indent = 0;
if (indent_level() > 0) {
    previous_indent = indent_stack.back()->get();
}
if (indent_count == previous_indent) {
    return;                         // 缩进未变
}
if (indent_count > previous_indent) {
    indent_stack.push_back(indent_count);
    pending_indents++;              // 欠一个 INDENT
} else {
    while (indent_level() > 0 && indent_stack.back()->get() > indent_count) {
        indent_stack.pop_back();
        pending_indents--;          // 欠一个 DEDENT
    }
    if ((indent_level() > 0 && indent_stack.back()->get() != indent_count)
        || (indent_level() == 0 && indent_count != 0)) {
        push_error("Unindent doesn't match the previous indentation level.");
        indent_stack.push_back(indent_count);    // 宽容处理，继续
    }
}
```

然后在 `scan()` 顶端：

```cpp
if (pending_indents != 0) {
    if (pending_indents > 0) {
        pending_indents--;
        return make_token(Token::INDENT);
    } else {
        pending_indents++;
        return make_token(Token::DEDENT);
    }
}
```

`pending_indents` 就是"还欠 Parser 几个缩进 Token"的净值，正数欠 INDENT、负数欠 DEDENT。因为 `scan()` 一次只产一个 Token，所以"一行回退三个缩进层级"对应 3 次 `scan()` 调用，每次吐一个 DEDENT。

### 2.4.2　特殊情况 ①：空行与注释行不触发缩进变化

`check_indent()` 在处理缩进前先跳过连续的空行和注释行：

```cpp
if (_peek() == '\n') { _advance(); newline(false); continue; }
if (_peek() == '#') { /* 吃到行尾 */ continue; }
```

这样 `if a:` 后面即使连写几个空行或只写注释，也不会提前产出 DEDENT，符合直觉。

### 2.4.3　特殊情况 ②：文件末尾补发 DEDENT

当读到 EOF 时，`indent_stack` 可能还有几层没 DEDENT。`check_indent()` 一开始有专门一段：

```cpp
if (_is_at_end()) {
    pending_indents -= indent_level();
    indent_stack.clear();
    return;
}
```

把"还欠多少 DEDENT"一次性记入 `pending_indents`，让最后几次 `scan()` 把它们一个个吐完，再吐 `TK_EOF`。这保证所有层级的块都能被 Parser 正常 "关闭"。

### 2.4.4　特殊情况 ③：Tab 与 Space 混用

```cpp
if (mixed && !line_continuation && !multiline_mode) {
    Token error = make_error("Mixed use of tabs and spaces for indentation.");
    push_error(error);
}
// ...
if (indent_char == '\0') {
    indent_char = current_indent_char;     // 第一次遇到的字符作为基准
} else if (current_indent_char != indent_char) {
    push_error(vformat("Used %s character for indentation instead of %s as used before in the file.", ...));
}
```

GDScript 采取了 **"文件级一致性"** 策略：一个 `.gd` 文件要么全用 Tab，要么全用空格，不能混用。第一行出现哪种字符就锁定为 `indent_char`，其后不一致就报错。这个约束比 Python 更严格（Python 允许不同块用不同字符，只要内部一致），换来的是 Tokenizer 逻辑的简化与错误提示的清晰。

## 2.5　行延续与 `multiline_mode`

缩进敏感语法和"表达式跨行"是天生冲突的：

```gdscript
result = (1 + 2
          + 3 + 4)        # 这里的"缩进"是表达式内的对齐，不应产生 INDENT
var x = 1 + \
        2                 # 反斜杠行延续
```

GDScript 用两个标志位协同解决这个问题：

- `line_continuation`：见到 `\\\n` 时置 `true`，下一行不触发缩进检查，处理完"下一行首个 Token"后被 `scan()` 显式清除。
- `multiline_mode`：当 Parser 在解析某些内置允许跨行的表达式时，调用 `set_multiline_mode(true)` 告诉 Tokenizer "接下来的 NEWLINE 不要吐、缩进变化也不要吐"。最典型的例子就是括号里的内容——但注意 GDScript **不是自动**根据 `paren_stack` 进入 multiline，而是由 Parser 主动控制。

在 `check_indent()` 中，这两个标志都会让函数提前 `return`：

```cpp
if (line_continuation || multiline_mode) {
    return;
}
```

在 `newline()` 中，它们会阻止 NEWLINE Token 的合成：

```cpp
if (p_make_token && !pending_newline && !line_continuation) {
    /* ... create NEWLINE token ... */
}
```

### 2.5.1　Lambda 的特殊处理：缩进栈的栈

GDScript 支持 Lambda：

```gdscript
var callback = func(x):
    return x * 2
print("hello")
```

Lambda 的函数体是**一个表达式内部的缩进块**。这带来一个尴尬的问题：Lambda 体内部用的是"相对于 Lambda 的缩进"，而 Lambda 之外继续用"原来的缩进"。仅用单个 `indent_stack` 根本处理不了这种嵌套——Lambda 结束时如何"回到"外层的缩进上下文？

GDScript 的答案极其巧妙：**把整个缩进栈压进另一个栈**。

```cpp
List<List<int>> indent_stack_stack;  // 每个元素本身就是一个缩进栈

void GDScriptTokenizerText::push_expression_indented_block() {
    indent_stack_stack.push_back(indent_stack);
    // （内部再初始化一个新缩进栈，给 Lambda 体使用）
}
void GDScriptTokenizerText::pop_expression_indented_block() {
    indent_stack = indent_stack_stack.back()->get();
    indent_stack_stack.pop_back();
}
```

Parser 在看到 `func` 后进入表达式级 Lambda 解析时调用 `push_expression_indented_block()`；Lambda 体解析完毕后调用 `pop_expression_indented_block()`。这样 Lambda 体内部就像一段独立的小程序，拥有自己的缩进上下文，互不干扰。

这种"栈的栈"的写法在其他 Python 风格语言里并不常见（大多数干脆禁止 Lambda 体内出现复杂缩进），GDScript 选择正面处理，是用户体验好但实现复杂的一个典型例子。

## 2.6　注释、光标：编辑器友好的小细节

`check_indent()` 和 `_skip_whitespace()` 中都有这段：

```cpp
#ifdef TOOLS_ENABLED
String comment;
while (_peek() != '\n' && !_is_at_end()) {
    comment += _advance();
}
comments[line] = CommentData(comment, is_bol);
#else
while (_peek() != '\n' && !_is_at_end()) {
    _advance();
}
#endif
```

- **运行时构建（游戏）**：注释直接被丢弃，省内存。
- **编辑器构建**：注释以 `line → CommentData` 存入 `HashMap<int, CommentData> comments`，供文档生成（`GDScriptDocGen`，第 20 章）从 `##` 风格注释里提取类/成员文档。

此外，`GDScriptTokenizerText` 还维护 `cursor_line`、`cursor_column`、`is_past_cursor()`——这些都是为 **编辑器的自动补全** 准备的。编辑器在请求补全前调用 `set_cursor_position()`，Tokenizer 在扫到光标所在位置的 Token 时给它打上 `CursorPlace`。Parser 据此在对应 AST 节点上留下"这是补全候选位置"的标记（`GDScriptParser::completion_context`），供 `gdscript_editor.cpp` 在补全查询时按位置分发。

这是 GDScript 编辑器集成能做得相当流畅的一个底层原因——**补全不是事后"从光标位置搜索 AST"，而是在词法/语法阶段就把光标位置原地标好**。

## 2.7　二进制 Tokenizer：`GDScriptTokenizerBuffer`

`GDScriptTokenizerBuffer` 不做真正意义上的"词法分析"——它从一份 `Vector<uint8_t>` 中解码出已经切好的 Token。`gdscript_tokenizer_buffer.h` 的核心字段：

```cpp
static constexpr uint32_t TOKENIZER_VERSION = 101;
static constexpr uint32_t TOKEN_BYTE_MASK   = 0x80;
static constexpr uint32_t TOKEN_BITS        = 8;
static constexpr uint32_t TOKEN_MASK        = (1 << (TOKEN_BITS - 1)) - 1;

Vector<StringName> identifiers;   // 字符串池
Vector<Variant>    constants;     // 字面量池
Vector<int>        continuation_lines;
HashMap<int, int>  token_lines;
HashMap<int, int>  token_columns;
Vector<Token>      tokens;        // 解码后的 Token 数组
int current = 0;
```

编码格式（见 `GDScriptTokenizerBuffer::parse_code_string` 与 `_token_to_binary`）的核心思路：

1. **字符串/常量分离**：源码里出现的所有标识符和字面量去重后分别放入 `identifiers`、`constants` 两个池；Token 里只存池索引，而不是字符串/`Variant` 本身。这使得 Token 流里"同一个标识符出现 1000 次"只占 1000 个小整数 + 1 份字符串。
2. **变长编码**：当 Token 类型 `< 128` 时用 1 字节；需要带一个池索引时高位 `TOKEN_BYTE_MASK = 0x80` 置位，后面跟变长索引。这是 protobuf 风格的 varint。
3. **zstd 可选压缩**：`CompressMode { COMPRESS_NONE, COMPRESS_ZSTD }`。文本 `.gd` 压缩后通常比 `.gdc` 还小，但 `.gdc` 无需再跑 Tokenizer，启动更快——在嵌入式/移动平台有实际意义。
4. **版本号守门**：`TOKENIZER_VERSION = 101`。`set_code_buffer()` 会校验 header 的版本号，不匹配直接返回错误。**这就是前面强调过的"枚举即 ABI"契约的落地**。

`GDScriptTokenizerBuffer` 仍然实现了同一个 `scan()` 接口：它内部只是按 `current` 指针递增地从 `tokens` 里取下一个 Token。INDENT / DEDENT 的合成逻辑在编码阶段就已经跑过，解码时自然地流出来，**和文本 Tokenizer 产出的序列完全一致**——这也是 Parser 能够"统一对待两种来源"的根本。

## 2.8　错误处理：错误也是 Token

GDScript 词法错误不抛异常。`push_error()` 把错误封装成一个 `Token(ERROR)` 压进 `error_stack`，下次 `scan()` 开头直接 `pop_error()` 返回：

```cpp
Token GDScriptTokenizerText::scan() {
    if (has_error()) return pop_error();
    _skip_whitespace();
    /* 可能又 push 错误 */
    if (has_error()) return pop_error();
    // ... 正常扫描 ...
}
```

这使得 Parser 不需要理解"扫描失败"是怎么回事——它看到一个 `ERROR` 类型的 Token 就把它当成普通的"不合法 Token"处理，继续尝试往下恢复解析。这种**"错误也是 Token"** 的写法在工业级编译器里非常常见（例如 Clang 的 `tok::unknown`），它让错误恢复和正常流程共享同一套代码。

## 2.9　完整流程示例

最后给一个小例子把本章所有要点串起来。源码：

```gdscript
func greet(name):
    if name:
        print("hi, " + name)
```

Tokenizer 会依次产生（省略列号）：

| # | Token | 触发代码 |
|---|---|---|
| 1 | `FUNC` | `potential_identifier()` 走到 `KEYWORD("func", FUNC)` |
| 2 | `IDENTIFIER("greet")` | `make_identifier()` |
| 3 | `PARENTHESIS_OPEN` | `push_paren('(')` |
| 4 | `IDENTIFIER("name")` | |
| 5 | `PARENTHESIS_CLOSE` | `pop_paren('(')` |
| 6 | `COLON` | |
| 7 | `NEWLINE` | `newline(true)` → `pending_newline` |
| 8 | `INDENT` | `check_indent()`：`indent_count=4 > previous=0`，`pending_indents=+1` |
| 9 | `IF` | |
| 10 | `IDENTIFIER("name")` | |
| 11 | `COLON` | |
| 12 | `NEWLINE` | |
| 13 | `INDENT` | `indent_count=8 > previous=4` |
| 14 | `IDENTIFIER("print")` | |
| 15 | `PARENTHESIS_OPEN` | `push_paren('(')` |
| 16 | `LITERAL("hi, ")` | `string()` |
| 17 | `PLUS` | |
| 18 | `IDENTIFIER("name")` | |
| 19 | `PARENTHESIS_CLOSE` | `pop_paren('(')` |
| 20 | `NEWLINE` | |
| 21 | `DEDENT` | EOF 触发：栈里还剩 [4, 8]，先弹 8 |
| 22 | `DEDENT` | 再弹 4 |
| 23 | `TK_EOF` | |

请特别注意 21、22 号——文件结尾处 Tokenizer 一次性把缩进栈全部弹空，吐出两个 DEDENT 才吐 EOF。Parser 因此能像处理普通块一样把 `if` 体和 `func` 体都"收尾"。

## 本章小结

- `Token` 是词法输出的最小单元，携带类型、字面量、位置、光标信息；Token 类型枚举是 `.gdc` 二进制格式的 ABI 契约。
- `GDScriptTokenizer` 提供单一抽象 `scan()`，有文本和二进制两个实现；两者产出的 Token 流完全兼容，是 GDScript 可以预编译部署的基础。
- 文本 Tokenizer 的 `scan()` 是一个带挂起队列（`pending_newline` / `pending_indents` / `error_stack`）的字符驱动状态机；错误也被当作 Token 返回。
- 关键字识别用"长度剪枝 + ASCII 剪枝 + 首字母 switch + X-Macro"的组合拳，既快又易维护。
- Python 风格缩进由 `check_indent()` + `indent_stack` 合成 INDENT/DEDENT 虚拟 Token；Lambda 用"缩进栈的栈"（`indent_stack_stack`）处理嵌套。
- `line_continuation` 和 `multiline_mode` 两个标志与 `paren_stack` 配合，允许表达式跨行而不产生假的缩进 Token。
- 二进制 Tokenizer 把标识符、常量分别放入池，Token 流里只存索引，用 `TOKENIZER_VERSION` 守门保证兼容。

下一章我们把 Tokenizer 吐出的 Token 流送给 Parser，看看 `GDScriptParser` 如何在"只前瞻一个 Token"的严格约束下构造出完整的 AST，并在过程中就地恢复一些常见的语法错误。

---

[← 目录](./README.md) · [下一章：第 3 章 语法分析：`GDScriptParser` 与 AST →](./03-parser.md)
