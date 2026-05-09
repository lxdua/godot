# 第 3 章　语法分析：`GDScriptParser` 与 AST

> 本章对应源码：
> `modules/gdscript/gdscript_parser.h`、`gdscript_parser.cpp`。

如果说 Tokenizer 是"把字符切成词"，那么 Parser 的任务就是"把词组合成句、把句组合成段、把段组合成章"。它接收 `GDScriptTokenizer` 产出的 Token 流，按语法规则拼装出一棵 **抽象语法树（AST）**，作为后续 Analyzer 与 Compiler 的唯一事实来源。

本章的目标是让你：

1. 理解 GDScript Parser 的总体架构与那个著名的"**只前瞻一个 Token**"设计约束。
2. 掌握 AST 节点家族与 `ClassNode` 的核心地位。
3. 看懂表达式解析用的 **Pratt（优先级爬升）** 框架。
4. 明白 Parser 如何与 Tokenizer 协同处理缩进（`push_multiline`）、Lambda 结束（`lambda_ended`）等"跨阶段"细节。
5. 理解 Parser 的错误恢复机制（`panic_mode` + `synchronize()`）。

## 3.1　Parser 的输入、输出与接口

`GDScriptParser` 的主入口是：

```cpp
Error GDScriptParser::parse(const String &p_source_code,
                            const String &p_script_path,
                            bool p_for_completion,
                            bool p_parse_body);
```

- **输入**：源码字符串 + 脚本路径。内部会构造一个 `GDScriptTokenizerText`（或从 `.gdc` 构造 `GDScriptTokenizerBuffer`）作为 Token 源。
- **输出**：一棵以 `head`（类型是 `ClassNode *`）为根的 AST，挂在 `GDScriptParser` 对象上。
- **`p_for_completion`**：编辑器请求补全时传 `true`。Parser 会保留补全上下文、忽略光标之后的部分错误，以便即使在代码不完整时也能给出尽可能多的信息。
- **`p_parse_body`**：允许只解析顶层声明（类名、`extends`、成员签名），跳过函数体。第 19 章"浅脚本"会用到这个开关。

注意 Parser **并不释放 AST**——所有 `alloc_node<T>()` 分配的节点被挂在单向链表 `list` 上，由 `GDScriptParser` 析构时统一 `memdelete`。这意味着 AST 的生命周期跟 Parser 对象绑定，拿到 `head` 之后不能让 Parser 先于 AST 销毁。Analyzer 和 Compiler 运行时，`GDScriptParser` 对象本身必须一直存活。

## 3.2　单 Token 前瞻的工程化实现

Parser 头文件里明确写了它的底层 API：

```cpp
GDScriptTokenizer::Token advance();
bool match(GDScriptTokenizer::Token::Type p_token_type);
bool check(GDScriptTokenizer::Token::Type p_token_type) const;
bool consume(GDScriptTokenizer::Token::Type p_token_type, const String &p_error_message);
bool is_at_end() const;
```

`GDScriptParser` 在任意时刻都只持有两个 Token：

- `previous`：上一个已经消费掉的 Token。
- `current`：下一个等待决策的 Token（所谓的"前瞻 1"）。

这四个方法把 Parser 可能做的所有事情都抽象成了一组极简的词法视图：

| 方法 | 语义 |
|---|---|
| `advance()` | `previous = current; current = tokenizer->scan();` 返回旧 `previous`。 |
| `check(T)` | `current.type == T`，不消费。 |
| `match(T)` | 如果 `current.type == T` 就 `advance()` 并返回 `true`；否则什么都不做返回 `false`。 |
| `consume(T, msg)` | 强制要求下一个是 `T`；不是就 `push_error(msg)`。常用来解析"这里必须是 `(`"这类强约束位置。 |

> **为什么不做更多前瞻**？README 里有专门的一段回答（第 1 章已引），本质是设计克制：限制 Parser 的能力反过来约束语言的复杂度。但这也带来了现实成本——GDScript Parser 里有若干"靠 1 个前瞻无法决断"的地方，它们被转嫁到 **Analyzer** 去推断。比如 `var x = foo()`：`foo` 可能是类、函数、自动属性、子图节点路径等；Parser 老老实实产一个 `CallNode`，由 Analyzer 在有类型信息时再决定具体语义。

### 3.2.1　"多行模式栈"：`push_multiline` / `pop_multiline`

GDScript 允许表达式跨行，比如：

```gdscript
var result = [
    1, 2,
    3, 4,
]
```

Tokenizer 默认会在 `]` 之前吐出 NEWLINE。若不作处理，Parser 解析数组字面量时会立刻被 NEWLINE 打断。`GDScriptParser` 的做法是 **主动控制 Tokenizer 的 `multiline_mode`**：

```cpp
void GDScriptParser::push_multiline(bool p_state) {
    multiline_stack.push_back(p_state);
    tokenizer->set_multiline_mode(p_state);
    if (p_state) {
        // 把已经读进来的 NEWLINE/INDENT/DEDENT 吃掉
        while (current.type == NEWLINE || current.type == INDENT || current.type == DEDENT) {
            current = tokenizer->scan();
        }
    }
}

void GDScriptParser::pop_multiline() {
    multiline_stack.pop_back();
    tokenizer->set_multiline_mode(multiline_stack.size() > 0
                                  ? multiline_stack.back()->get()
                                  : false);
}
```

有两个值得细看的实现细节：

1. **栈式而非布尔**：用 `multiline_stack` 支持嵌套。进入一个多行表达式之前保存当前模式，退出时恢复。`[` 里可能有 `func ...`（Lambda），Lambda 体内部又切回单行模式——栈是必要的。
2. **进入多行模式时要"吃掉已读的空白 Token"**：因为 `current` 在 `push_multiline` 之前就已被 Tokenizer 扫过；若那时恰好是 NEWLINE，必须立即丢弃，并且 **不能** 用 `advance()`（会弄脏 `previous`），而是直接 `tokenizer->scan()`。这是 Parser 与 Tokenizer 间一个非常细的契约。

数组、字典、函数参数列表、`match` 的 pattern 等所有"括号内跨行"的结构都会走 `push_multiline(true)` / `pop_multiline()`。

## 3.3　AST 节点家族

`gdscript_parser.h:300` 的 `struct Node` 是一切 AST 节点的基类：

```cpp
struct Node {
    enum Type { NONE, ANNOTATION, ARRAY, ASSERT, ASSIGNMENT, AWAIT, BINARY_OPERATOR,
                BREAK, BREAKPOINT, CALL, CAST, CLASS, CONSTANT, CONTINUE, DICTIONARY,
                ENUM, FOR, FUNCTION, GET_NODE, IDENTIFIER, IF, LAMBDA, LITERAL, MATCH,
                MATCH_BRANCH, PARAMETER, PASS, PATTERN, PRELOAD, RETURN, SELF, SIGNAL,
                SUBSCRIPT, SUITE, TERNARY_OPERATOR, TYPE, TYPE_TEST, UNARY_OPERATOR,
                VARIABLE, WHILE };

    Type type = NONE;
    int start_line = 0;
    int start_column = 0;
    int end_line = 0;
    int end_column = 0;
    Node *next = nullptr;                      // Parser 内部分配链表
    List<AnnotationNode *> annotations;        // 前缀注解
    DataType datatype;                         // Analyzer 填充的类型

    virtual DataType get_datatype() const { return datatype; }
    virtual void set_datatype(const DataType &p_datatype) { datatype = p_datatype; }
    virtual bool is_expression() const { return false; }
    virtual ~Node() {}
};
```

注意几点：

- **`Node::Type` 枚举 + 运行时字段 `type` + RTTI 替代**：Godot 不启用 C++ RTTI，GDScript 的 AST 也不用 `dynamic_cast`。要判断"这是不是 `CallNode`"，你会看到满代码库的 `node->type == Node::CALL` 然后 `static_cast<CallNode *>(node)`。这是 Godot 一贯的工程风格。
- **`annotations`**：挂在 Node 上的注解列表。Parser 负责把 `@tool`、`@export_range(...)` 这类前缀注解"绑定"到下一个语法结构（变量、函数、类）上。
- **`datatype`**：Parser **不填**这个字段；它由 Analyzer 阶段填。Parser 只管"结构正确"。
- **`next`**：不是语义上的"下一兄弟节点"，而是 Parser 所有节点组成的全局单链表，析构时统一释放。

### 3.3.1　`ExpressionNode`：有值的节点

任何可以出现在表达式位置的节点都继承自 `ExpressionNode`：

```cpp
struct ExpressionNode : public Node {
    bool reduced = false;
    bool is_constant = false;
    Variant reduced_value;      // 常量折叠后的结果
    virtual bool is_expression() const override { return true; }
};
```

三个额外字段都是给 Analyzer 的常量折叠准备的：

- `reduced = true` 表示 Analyzer 已经处理过这个表达式；
- `is_constant = true` 表示它的值在编译期可以完全确定；
- `reduced_value` 就是那个编译期结果。

Compiler 在生成字节码时会检查 `is_constant`——能直接 bake 进常量池的，就不再产出运行时计算指令。这是 GDScript 一项重要且几乎免费的性能优化（详见第 4、7 章）。

### 3.3.2　节点家族树一览

把所有 `struct XxxNode` 按父类分组：

```
Node
├── ExpressionNode                       "有值的节点，可参与表达式"
│    ├── ArrayNode          [1, 2, 3]
│    ├── AssignmentNode     a = b        (也是表达式——GDScript 里赋值可嵌入 match 的 pattern 等)
│    ├── AwaitNode          await sig
│    ├── BinaryOpNode       a + b, a and b
│    ├── CallNode           f(x, y)
│    ├── CastNode           x as Node2D
│    ├── DictionaryNode     {a: 1}
│    ├── GetNodeNode        $Player
│    ├── IdentifierNode     name
│    ├── LambdaNode         func(x): return x
│    ├── LiteralNode        42, "hi", true
│    ├── PreloadNode        preload("res://a.gd")
│    ├── SelfNode           self
│    ├── SubscriptNode      a[i], a.b   (含成员访问)
│    ├── TernaryOpNode      a if c else b
│    ├── TypeTestNode       x is Node
│    ├── UnaryOpNode        -a, not a
│    ├── AssignableNode（抽象基）
│    │    ├── ConstantNode  const FOO = 1
│    │    ├── ParameterNode func f(x: int = 0)
│    │    └── VariableNode  var x: int = 0, var x: int: get: ...
│    └── ……
├── ClassNode               "一个类（含顶层/内部类）"
├── FunctionNode            func foo(): ...
├── SuiteNode               "一段缩进块（含 locals）"
├── IfNode / ForNode / WhileNode / MatchNode / MatchBranchNode / PatternNode
├── ReturnNode / BreakNode / ContinueNode / PassNode / BreakpointNode
├── AssertNode              assert(x)
├── SignalNode              signal hit(by)
├── EnumNode                enum Color { RED, BLUE }
├── AnnotationNode          @onready, @export
└── TypeNode                类型标注: `int`, `Array[Node]`, `MyClass`
```

几个值得点名的设计：

- **`SubscriptNode` 同时表示 `a[i]` 和 `a.b`**。通过 `is_attribute` 字段区分。属性访问本质是"用一个标识符做下标"——合并起来能让 Analyzer / ByteCodeGenerator 里"解析左值链"的代码走统一路径。
- **`TypeNode` 是独立节点，不是 `DataType`**。前者是"用户代码里写的类型表达式"的 AST 表示（比如 `Array[Node]`），后者是 Analyzer 算出的"最终类型信息"。两者互有映射但不是一回事。
- **`AssignmentNode` 继承自 `ExpressionNode`**。赋值被视为表达式以便嵌套，但它的 `reduced` 永远是 `false`——GDScript 不会在编译期折叠赋值。

### 3.3.3　`ClassNode`：整棵 AST 的"根"

`parse_program()` 干的第一件事就是：

```cpp
void GDScriptParser::parse_program() {
    head = alloc_node<ClassNode>();
    head->start_line = 1;
    head->fqcn = GDScript::canonicalize_path(script_path);
    current_class = head;
    ...
}
```

一个 `.gd` 文件隐式地就是一个 `ClassNode`，它是整棵 AST 的根。文件顶层的 `var`、`const`、`func`、`signal` 都是这个"根 ClassNode"的成员。内部类 `class Inner: ...` 本身是一个子 `ClassNode`，**嵌套**在父 ClassNode 的 `members` 里——AST 在这一点上完全映射到 `GDScript` 运行时对象的嵌套关系（第 15 章）。

`ClassNode` 的核心字段（简化版）：

```cpp
struct ClassNode : public Node {
    struct Member {
        enum Type { UNDEFINED, CLASS, CONSTANT, FUNCTION, SIGNAL, VARIABLE, ENUM,
                    ENUM_VALUE, GROUP, UNDEFINED_LATER };
        Type type;
        // 一个 union-like 的结构，按 type 访问不同的节点指针
        ClassNode   *m_class;
        ConstantNode *constant;
        FunctionNode *function;
        SignalNode   *signal;
        VariableNode *variable;
        EnumNode     *enum_node;
        ...
    };

    StringName identifier;               // class_name（可选）
    String fqcn;                         // 全限定类名，= script_path
    Node *extends_used = nullptr;        // extends 列表（可能是标识符或字符串）
    IdentifierNode *icon_path = nullptr; // @icon

    Vector<Member> members;
    HashMap<StringName, int> members_indices;
    ClassNode *base_class = nullptr;     // 解析 extends 后 Analyzer 填
    bool resolved_interface = false;
    bool resolved_body = false;
    ...
};
```

两类布尔字段 `resolved_interface` / `resolved_body` 揭示了第 1 章说过的**循环依赖两阶段解析**：Analyzer 可能先只填 `resolved_interface`（接口阶段），等到真正需要函数体时才 `resolved_body = true`。这些都是 Parser 留下的占位字段，Parser 自己不赋值。

## 3.4　总体流程：`parse_program()`

`parse_program()`（`gdscript_parser.cpp:707`）的骨架非常平实：

```cpp
void GDScriptParser::parse_program() {
    head = alloc_node<ClassNode>();
    current_class = head;

    advance();                         // 抓第一个 Token 到 current
    bool can_have_class_or_extends = true;

    // 顶部可以有：文件级注解、class_name、extends、其余成员
    while (!check(TK_EOF)) {
        if (check(ANNOTATION)) {
            // 解析注解，挂到 "pending annotations" 队列
        } else if (can_have_class_or_extends && match(CLASS_NAME)) {
            parse_class_name();
        } else if (can_have_class_or_extends && match(EXTENDS)) {
            parse_extends();
        } else {
            can_have_class_or_extends = false;
            parse_class_body(/* multiline */ true);
            break;
        }
    }

    // 把文件末尾没用上的 annotation 报错
    clear_unused_annotations();
}
```

几个要点：

- **`class_name` / `extends` 必须出现在文件开头**——`can_have_class_or_extends` 一旦 `false` 就不再允许。
- **注解是"前缀挂载"**：Parser 不会在看到 `@tool` 时立即处理，而是塞进 `annotation_stack`（或 pending 队列），在下一个实际语法结构（类、成员、语句）诞生时把它挂到那个节点的 `annotations` 列表里。这样无论注解是写在 class_name 之前、函数之前还是变量之前，挂载逻辑都一致。
- **`parse_class_body(true)`** 才是真正解析类成员的循环。它会反复调用 `parse_class_member<FunctionNode>(...)`、`parse_signal`、`parse_enum`、`parse_variable`、`parse_constant` 等，一直吃到 DEDENT 或 EOF。

### 3.4.1　成员解析：`parse_class_member`

这是个模板函数，抽象了"所有类成员解析都要做的事"：

```cpp
template <typename T>
void GDScriptParser::parse_class_member(T *(GDScriptParser::*p_parse_function)(bool),
                                        AnnotationInfo::TargetKind p_target,
                                        const String &p_member_kind,
                                        bool p_is_static);
```

它负责：

1. 检查前缀是否有 `static`；
2. 调用对应的具体解析函数（如 `parse_function`、`parse_variable`）；
3. 把前面积累的注解挂到产出的节点上；
4. 校验注解的 `TargetKind` 是否匹配（例如 `@export` 不能放函数上）；
5. 把成员加入 `ClassNode::members` 并在 `members_indices` 里建索引。

这种"共性抽到模板"的做法在 GDScript Parser 里非常典型，单看一个 `parse_*` 函数可能觉得它"什么都没做"，其实 heavy lifting 都在模板/基础设施里。

## 3.5　表达式解析：Pratt 风格与 `ParseRule` 表

GDScript 的表达式文法有近 20 个优先级层。与其写 20 层递归下降函数（`parse_unary` → `parse_factor` → `parse_term` → ...），Parser 选用了著名的 **Pratt 优先级爬升** 算法，核心是一张 "Token → 解析规则" 的表：

```cpp
struct ParseRule {
    ParseFunction prefix = nullptr;         // 当这个 Token 出现在表达式开头时怎么解析
    ParseFunction infix  = nullptr;         // 当这个 Token 作为中缀/后缀时怎么解析
    Precedence    precedence = PREC_NONE;   // 中缀优先级
};

static ParseRule *get_rule(Token::Type p_token_type);
```

`Precedence` 枚举（`gdscript_parser.h:1431`）是显式的整数枚举，从低到高：

```
PREC_NONE → PREC_ASSIGNMENT → PREC_CAST → PREC_TERNARY
→ PREC_LOGIC_OR → PREC_LOGIC_AND → PREC_LOGIC_NOT → PREC_CONTENT_TEST
→ PREC_COMPARISON → PREC_BIT_OR → PREC_BIT_XOR → PREC_BIT_AND → PREC_BIT_SHIFT
→ PREC_ADDITION_SUBTRACTION → PREC_FACTOR → PREC_SIGN → PREC_BIT_NOT → PREC_POWER
→ PREC_TYPE_TEST → PREC_AWAIT → PREC_CALL → PREC_ATTRIBUTE → PREC_SUBSCRIPT → PREC_PRIMARY
```

> 注意 **`**`（幂）在 `PREC_POWER`、右结合**，`-`（一元）在 `PREC_SIGN`，"`is` / `as`" 比较在 `PREC_TYPE_TEST` / `PREC_CAST` ——这些"坑点"都能从枚举顺序直接读出来。

Pratt 主循环（概念上，函数名为 `parse_expression` / `parse_precedence`）长这样：

```cpp
ExpressionNode *parse_precedence(Precedence p_precedence, bool p_can_assign) {
    advance();                                           // 吃一个 Token
    ParseRule *prefix_rule = get_rule(previous.type);
    if (prefix_rule->prefix == nullptr) {                // 没有前缀规则 → 语法错误
        push_error("Expected expression.");
        return nullptr;
    }
    ExpressionNode *left = (this->*prefix_rule->prefix)(nullptr, p_can_assign);

    // 不断尝试应用 infix 规则，只要当前 Token 的优先级 ≥ 要求的优先级
    while (p_precedence <= get_rule(current.type)->precedence) {
        advance();
        ParseRule *infix_rule = get_rule(previous.type);
        left = (this->*infix_rule->infix)(left, p_can_assign);
    }
    return left;
}
```

读这段代码时要抓住的关键直觉：

1. **前缀规则**负责"吃一个操作数子表达式"，例如 `parse_unary_operator`、`parse_literal`、`parse_identifier`、`parse_array` 等。
2. **中缀规则**在已知一个左操作数 `left` 的情况下，决定"这个 Token 怎么把 left 接进来"。例如 `parse_binary_operator` 会再调用 `parse_precedence` 把右操作数解析出来，组合成 `BinaryOpNode`。
3. **优先级只在中缀规则生效**，前缀不需要比较，因为前缀 Token 天然在表达式起点。
4. **右结合**通过"递归时传 `precedence + 0`"实现，**左结合**通过"递归时传 `precedence + 1`"实现——传 `+1` 意味着"同级别的中缀 Token 请别再继续消耗我"。

整张 `ParseRule` 表定义在 `gdscript_parser.cpp` 的底部（按 Token 类型索引的大数组），每一行都是一条"Token 的语法行为卡片"，增减运算符只改这一行 + 加一个解析函数，改动范围极小。这也是 Pratt 被工业界广泛采用的核心优势：**表驱动**。

### 3.5.1　一个例子：`a + b * c`

| 步骤 | `previous` / `current` | 行为 |
|---|---|---|
| 进 `parse_precedence(PREC_ASSIGNMENT)` | `cur=a` | `advance` → `prev=a`；应用 `IDENTIFIER` 的 prefix → `left = IdentifierNode(a)` |
| 循环 | `cur=+` | `+` 中缀优先级 `PREC_ADDITION_SUBTRACTION > ASSIGNMENT`，进入 |
| | `advance` → `prev=+`；调用 `parse_binary_operator(left=a)` | |
| 进 binary op | | 递归 `parse_precedence(PREC_ADDITION_SUBTRACTION + 1)` 取右操作数 |
| 递归内 | `cur=b` | prefix → `IdentifierNode(b)` |
| 递归内循环 | `cur=*` | `*` 优先级 `PREC_FACTOR > ADDITION+1`，进入 |
| | 递归 `parse_precedence(PREC_FACTOR + 1)` 取右操作数 → `IdentifierNode(c)` | |
| 递归内 | `cur=EOF` | 无更多中缀，返回 `BinaryOp(b, *, c)` |
| 回到外层 | | 结合为 `BinaryOp(a, +, BinaryOp(b,*,c))` |

Parser 只用一个 Token 前瞻就完成了优先级正确的树构造。这正是 Pratt 的魅力。

## 3.6　语句与块：`SuiteNode` 与 locals 表

### 3.6.1　`SuiteNode` 是什么

GDScript 里一个"缩进块"由 `SuiteNode` 表示：

```cpp
struct SuiteNode : public Node {
    struct Local {
        enum Type { UNDEFINED, CONSTANT, VARIABLE, PARAMETER,
                    FOR_VARIABLE, PATTERN_BIND };
        Type type;
        union { ConstantNode *constant; VariableNode *variable; ... };
    };

    SuiteNode  *parent_block = nullptr;
    FunctionNode *parent_function = nullptr;
    bool is_in_loop = false;
    Vector<Node *> statements;
    Vector<Local>  locals;
    HashMap<StringName, int> locals_indices;
    ...
};
```

几个关键观察：

- **Suite ≠ 函数**。函数体是一个 Suite，但 `if`、`for`、`while`、`match branch` 也都各有一个 Suite。Suite 是 GDScript 里"独立作用域"的通用载体。
- **`locals` 表在 Parser 阶段就建立**。`var x = ...` 出现在块里时，Parser 立即把它加入当前 Suite 的 `locals`。Analyzer 后续做标识符解析时，可以直接从 Suite 的 `locals_indices` 查找，比"遍历 `statements`" 快得多。
- **`parent_block` / `parent_function` / `is_in_loop`** 让每个 Suite 都知道自己的上下文：`break` 语句合法吗？`return` 能在这里出现吗？这些都在 Suite 构造时就标好，而不是 Analyzer 再去"往上走"确认。

### 3.6.2　`parse_suite` 与 `end_statement`

`parse_suite` 负责从"我现在站在一个 `:` 之后"到"吃完整个块"的全过程：

```cpp
SuiteNode *parse_suite(const String &p_context, SuiteNode *p_suite = nullptr, bool p_for_lambda = false);
```

它的工作流程：

1. `consume(NEWLINE, ...)` + `consume(INDENT, ...)` —— 必须有缩进进入；
2. 反复调用 `parse_statement()`，直到 `DEDENT` 或 EOF；
3. 每条语句结束后调用 `end_statement()`。

`end_statement()`（`gdscript_parser.cpp:681`）专门处理"语句到底怎么算结束"这个细节：

```cpp
bool is_statement_end_token() const {
    return check(NEWLINE) || check(SEMICOLON) || check(TK_EOF);
}
bool is_statement_end() const {
    return lambda_ended || in_lambda || is_statement_end_token();
}
```

- `NEWLINE` / `SEMICOLON` / `TK_EOF` 显然是语句终止符。
- `in_lambda` 和 `lambda_ended` 才是精彩的部分——见下一节。

## 3.7　Lambda：跨缩进模式的精巧协同

看一段代码：

```gdscript
callback(func(x):
    return x * 2, other_arg)
```

`func(x): ...` 是一个 Lambda，它的函数体有自己的缩进层次；但整个 Lambda 又嵌在一个函数调用的参数表里，参数表本身是单行表达式。要正确解析这种写法，Parser 必须：

1. 临时为 Lambda 建立一个"独立的缩进上下文"；
2. 在 Lambda 体结束后回到外层上下文，让外层的 `,` 和 `)` 继续生效。

Parser 和 Tokenizer 在这一点上做了一个相当优雅的协同：

- Parser 调用 `tokenizer->push_expression_indented_block()` 把当前缩进栈压入"栈的栈"（第 2 章 2.5.1 节）；
- Parser 在 `parse_suite` 内设置 `in_lambda = true`；
- 当 Lambda 体遇到 `DEDENT` 回到外层缩进时，Parser 意识到 Lambda 结束，置 `lambda_ended = true` 并调用 `pop_expression_indented_block()`；
- `is_statement_end()` 特判 `lambda_ended`——使得外层看见 Lambda"从语句结束的角度"等价于一个 NEWLINE；
- `end_statement()` 发现 `lambda_ended` 时 "吃掉这个 token"（其实 lambda_ended 不是真实 Token，只是一个逻辑标志）并复位。

这段两层缩进上下文 + 虚拟"lambda 结束"信号的协作，**跨越 Tokenizer 与 Parser 两个模块**。它是本书里少数需要同时打开两份 `.cpp` 才能读懂的地方之一；但它换来的用户体验是："Lambda 可以写得像一个普通代码块"。

## 3.8　错误恢复：`panic_mode` + `synchronize()`

让 Parser 在错误代码面前不崩、不连锁爆炸是工程上的硬需求——尤其是编辑器场景，用户每敲一个字可能都是"暂时性错误代码"。GDScript Parser 的策略是**恐慌模式 + 同步点**，这也是工业级编译器的经典做法。

```cpp
bool panic_mode = false;

void push_error(const String &p_message, const Node *p_origin = nullptr) {
    if (panic_mode) {
        return;       // 已处于恐慌，别再叠错误
    }
    panic_mode = true;
    errors.push_back(...);
}

void synchronize() {
    panic_mode = false;
    while (!is_at_end()) {
        if (previous.type == NEWLINE || previous.type == SEMICOLON) return;
        switch (current.type) {
            case CLASS: case FUNC: case STATIC: case VAR: case TK_CONST:
            case SIGNAL: case FOR: case WHILE: case MATCH: case RETURN:
            case ANNOTATION:
                return;
            default: break;
        }
        advance();
    }
}
```

机制是这样的：

1. `push_error()` 报告第一个错误后立刻置 `panic_mode = true`；此后若同一条语句里再发生错误，**直接静默**——避免"一个错误引发 20 条误导性后续错误"。
2. 在 Parser 的语句级循环里，发现错误后主动调用 `synchronize()`，**把 Token 指针一直推进到下一个"看起来像新语句开头"的 Token**（`class` / `func` / `var` / `for` / ... / 或 NEWLINE 之后）才停下来。
3. `synchronize()` 返回时清掉 `panic_mode`，新一轮解析重新开始。

这就是为什么 GDScript 即使只写了一半的代码，也能给你一个结构基本正确、能补全的 AST。

### 3.8.1　错误恢复节点：`alloc_recovery_node`

看头文件里还有一个兄弟方法：

```cpp
template <typename T>
T *alloc_recovery_node();

SuiteNode *alloc_recovery_suite();
```

这些"恢复节点"**不跟踪 extents（行列信息）**，因为它们不对应任何真实源码位置——它们是 Parser 为了让 AST 保持结构完整而"补"出来的占位符。Analyzer 遍历 AST 时会跳过这些占位节点的大部分校验。例如 `if :` 后忘了写表达式，Parser 仍然会给你一个合法的 `IfNode`，只是它的条件挂一个恢复出的 `ConstantNode`。

这种"结构优先于细节"的 AST 哲学让**编辑器的补全/跳转/悬浮提示**在半残代码下依然可用，是 GDScript 编辑体验的底层保证。

## 3.9　为补全铺路：`CompletionContext` 与 `CompletionCall`

Parser 头文件里专门有一组为补全服务的数据结构：

```cpp
enum CompletionType {
    COMPLETION_NONE, COMPLETION_ANNOTATION, COMPLETION_ANNOTATION_ARGUMENTS,
    COMPLETION_ASSIGN, COMPLETION_ATTRIBUTE, COMPLETION_ATTRIBUTE_METHOD,
    COMPLETION_BUILT_IN_TYPE_CONSTANT_OR_STATIC_METHOD, COMPLETION_CALL_ARGUMENTS,
    COMPLETION_IDENTIFIER, COMPLETION_METHOD, COMPLETION_OVERRIDE_METHOD,
    COMPLETION_PROPERTY_DECLARATION, COMPLETION_PROPERTY_METHOD, ...
};

struct CompletionContext { CompletionType type; ClassNode *current_class;
                           FunctionNode *current_function; SuiteNode *current_suite;
                           Node *node; int current_argument; ... };

struct CompletionCall { Node *call; int argument; bool used = true; };
```

工作机制与第 2 章末尾提到的 Token 级 `CursorPlace` 呼应——**Parser 在解析到光标所在结构时主动打标**：

```cpp
void make_completion_context(CompletionType p_type, Node *p_node,
                             int p_argument = -1, bool p_force = true);
```

比如解析到 `a.` 之后发现 Token 就是光标位置，就 `make_completion_context(COMPLETION_ATTRIBUTE, a_node)`。之后 `gdscript_editor.cpp` 在补全请求时只需读这个上下文，就知道"要补的是属性，base 是 `a`"，无需自己再从 AST 推断。

同样的，`CompletionCall` 跟踪"光标落在哪个函数调用的第几个参数"。嵌套调用需要栈：`push_completion_call` / `pop_completion_call`。

**这种"解析时就地记录补全意图"的设计是 GDScript 编辑器能同时做到"快、稳、半残代码也能用"的核心原因**。我们会在第 20 章详细展开它如何被 `gdscript_editor.cpp` 消费。

## 3.10　`extents`：行列信息的精细簿记

AST 节点的 `start_line / start_column / end_line / end_column` 四个字段看似平凡，却要 Parser 在每一次 `alloc_node` / `advance` 时小心维护。相关 API：

```cpp
List<Node *> nodes_in_progress;         // 当前"尚未闭合"的节点栈
void complete_extents(Node *p_node);    // 用当前位置关上这个节点
void update_extents(Node *p_node);      // 延伸一点点
void reset_extents(Node *p_node, Token p_token);
void reset_extents(Node *p_node, Node *p_from);

template <typename T>
T *alloc_node() {
    T *node = memnew(T);
    node->next = list;
    list = node;
    reset_extents(node, previous);          // 起点对齐到上一个 Token
    nodes_in_progress.push_back(node);
    return node;
}
```

意图是：

- 新分配的节点起点一定 ≥ `previous` 的起点；
- 解析过程中每消费一个新 Token 都 `update_extents`，把节点终点往后推；
- 节点组装完后调 `complete_extents(node)`，从 `nodes_in_progress` 弹出。

这些 extents 最终被 **编辑器调试器用来高亮代码行、LSP 用来回答"这个变量在哪？" 、错误消息用来显示"错在第几列"** ——全都依赖 Parser 诚实而精细的记账。

## 3.11　一个小全景：源码 → AST

给个最小例子把全章要点串起来。源码：

```gdscript
class_name Greeter

func greet(name: String) -> void:
    print("hi, " + name)
```

Parser 产出的 AST（简化）：

```
ClassNode(head, fqcn="res://greeter.gd", identifier=Greeter)
├── members:
│   └── FunctionNode("greet")
│       ├── parameters: [ ParameterNode("name", type=TypeNode("String")) ]
│       ├── return_type: TypeNode("void")
│       └── body: SuiteNode
│            ├── locals: (empty)
│            └── statements:
│                └── CallNode(callee=IdentifierNode("print"),
│                             args=[ BinaryOpNode(
│                                      op=PLUS,
│                                      left=LiteralNode("hi, "),
│                                      right=IdentifierNode("name")) ])
```

注意此时所有 `DataType` 都还是默认值，`IdentifierNode("name")` 也不知道自己是"参数" ——这些都是 Analyzer 的活。

## 本章小结

- Parser 的唯一任务是在 **只前瞻 1 个 Token** 的约束下把 Token 流变成结构化 AST。它用 `previous / current / advance / match / check / consume` 五件套做词法视图，用 `push_multiline` / `pop_multiline` 协调与 Tokenizer 的缩进模式。
- AST 节点都继承自 `Node`，表达式节点继承自 `ExpressionNode`。`Node::Type` 枚举 + 手工 `static_cast` 替代 RTTI，是 Godot 风格。
- `ClassNode` 是一个文件的 AST 根，内部类嵌套映射到运行时的 `GDScript` 嵌套关系。`resolved_interface` / `resolved_body` 占位字段为 Analyzer 的两阶段解析留出空间。
- 表达式解析用 **Pratt** 框架：一张 `ParseRule[Token::Type]` 表 + `Precedence` 枚举，前缀/中缀/优先级完全表驱动，增减运算符非常轻量。
- `SuiteNode` 承载任何"独立作用域"，`locals` 表在 Parser 阶段就建好，Analyzer 直接查表即可。
- Lambda 通过 `in_lambda` + `lambda_ended` + Tokenizer 的"缩进栈的栈"实现跨缩进模式的嵌套解析。
- 错误恢复用 **`panic_mode` + `synchronize()`** 同步点机制；AST 上还有 `alloc_recovery_node<T>()` 产出的"占位节点"保持结构完整，让编辑器在半残代码下也能工作。
- **Parser 就地记录 `CompletionContext` 与 extents**，为后续的编辑器补全、LSP、错误定位奠定基础。

下一章我们进入 **`GDScriptAnalyzer`**：它会把 Parser 留下的那棵"结构正确但类型空白"的 AST 变成"每个节点都带 `DataType` 并经过语义校验"的加强版。在那一章里，我们会仔细看 Analyzer 如何用 `reduce_*` / `resolve_*` 两族函数、如何用 `RESOLVING` 哨兵处理循环依赖、以及它怎样在"渐进类型"哲学下对未类型化的代码做"trust the programmer"的让步。

---

[← 上一章：第 2 章 Tokenizer](./02-tokenizer.md) · [目录](./README.md) · [下一章：第 4 章 语义分析：`GDScriptAnalyzer` 与类型系统 →](./04-analyzer.md)
