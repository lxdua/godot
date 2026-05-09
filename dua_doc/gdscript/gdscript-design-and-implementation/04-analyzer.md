# 第 4 章　语义分析：`GDScriptAnalyzer` 与类型系统

> 本章对应源码：
> `modules/gdscript/gdscript_analyzer.h`、`gdscript_analyzer.cpp`，
> 以及 `gdscript_parser.h` 中的 `DataType` 类。

Parser 把源码变成了一棵**结构正确但类型空白**的 AST。Analyzer 的任务是把这棵树"激活"：

1. 给每个 `ExpressionNode` 填上 `DataType`；
2. 把每个 `IdentifierNode` 指向它实际引用的符号（局部变量、参数、成员、类、全局函数……）；
3. 校验所有语义规则（参数个数、类型兼容、只读赋值、信号匹配……）；
4. 顺手做常量折叠（`1+2*3` → 直接变成 `LiteralNode(7)`）；
5. 对类型信息不足的地方标 `unsafe`，让"渐进类型"哲学落地。

Analyzer 是整个 GDScript 实现里**最复杂的一个模块**——`gdscript_analyzer.cpp` 动辄数千行。但如果抓对了视角，它的结构其实非常清晰：**两族函数（reduce / resolve）+ 四个阶段（inheritance / interface / body / dependencies）+ 一个哨兵（`RESOLVING`）**。本章就按这个骨架展开。

## 4.1　`DataType`：类型信息的统一载体

在看 Analyzer 代码之前，必须先看懂 `GDScriptParser::DataType`（`gdscript_parser.h:101`）。它是所有类型信息的载体，Analyzer 的几千行代码几乎都在读写它。

```cpp
class DataType {
public:
    enum Kind {
        BUILTIN,      // Variant 内建类型：int, float, String, Vector2, Array...
        NATIVE,       // 引擎 C++ 类：Node, Node2D, Resource...
        SCRIPT,       // 另一个脚本资源（可能是别的语言如 C#）
        CLASS,        // GDScript 定义的类（顶层或内部）
        ENUM,         // 枚举
        VARIANT,      // 任意类型（"Variant" 关键字）
        RESOLVING,    // 正在解析中（循环依赖哨兵）
        UNRESOLVED,   // 还没解析
    };
    Kind kind = UNRESOLVED;

    enum TypeSource {
        UNDETECTED,            // 无任何信息（等价于 Variant）
        INFERRED,              // 动态推断出的"软类型"
        ANNOTATED_EXPLICIT,    // 用户写了类型注解
        ANNOTATED_INFERRED,    // var x := expr 这种"从赋值推断的硬类型"
    };
    TypeSource type_source = UNDETECTED;

    // 各 Kind 下使用的附加字段
    Variant::Type builtin_type;       // BUILTIN 时使用
    StringName    native_type;        // NATIVE / ENUM 时使用
    StringName    enum_type;
    Ref<Script>   script_type;        // SCRIPT 时使用
    ClassNode    *class_type;         // CLASS 时使用
    Vector<DataType> container_element_types;     // Array[T] / Dictionary[K, V]

    // 重要谓词
    bool is_set() const        { return kind != RESOLVING && kind != UNRESOLVED; }
    bool is_resolving() const  { return kind == RESOLVING; }
    bool is_variant() const    { return kind == VARIANT || ... || UNRESOLVED; }
    bool is_hard_type() const  { return type_source > INFERRED; }

    // 其它标志
    bool is_constant;       // 用 const 声明
    bool is_read_only;      // 只读（如 enum 值）
    bool is_meta_type;      // 类对象本身（而非其实例）
    bool is_pseudo_type;    // 全局命名但不能单独使用（如某些命名空间）
    bool is_coroutine;      // 函数返回，标记是协程
    ...
};
```

几个关键观察：

### 4.1.1　`Kind` × `TypeSource` 才是完整的类型空间

单看 `Kind` 只知道"这是什么种类的类型"；单看 `TypeSource` 只知道"用户给了多少信息"；两者组合起来才能回答一个至关重要的问题：

> **这个类型是"硬类型"还是"软类型"？**

谓词 `is_hard_type()` 直接定义为 `type_source > INFERRED`——也就是说，**只有用户显式写了类型注解（`ANNOTATED_EXPLICIT`）或用了 `:=` 推断出硬类型（`ANNOTATED_INFERRED`）时，才叫硬类型**。普通的 `var x = 1` 虽然 Analyzer 能推出"`x` 看起来是 `int`"，但它的 `type_source = INFERRED`，是软类型，`x` 之后被赋值其它类型不会报错。

这就是"渐进类型"在实现上的核心体现：**不是类型有没有被推断出来，而是用户是否向编译器"承诺"了这个类型**。用户没承诺的，Analyzer 再聪明也不强加。

### 4.1.2　`operator==` 的极度宽容

```cpp
bool operator==(const DataType &p_other) const {
    if (type_source == UNDETECTED || p_other.type_source == UNDETECTED)
        return true;
    if (type_source == INFERRED || p_other.type_source == INFERRED)
        return true;
    ...
}
```

两个 `DataType` **只要有一边是软类型，就视为相等**。Analyzer 在做"赋值兼容吗？"检查时大量使用 `==`——这条宽容规则让"混合使用类型化和未类型化代码"成为可能：

```gdscript
var y = "some string"     # 软类型
var x: int = y            # 按硬规则应该报错
                          # 实际上：y 是 INFERRED，x == y 返回 true，不报错
                          # 运行时会爆
```

宽容是"trust the programmer"的默认语义。如果 Analyzer 希望触发硬校验，用的是 `can_reference()` / `is_hard_type()` 这类显式 API，而不是 `==`。

### 4.1.3　`container_element_types`：参数化类型的递归结构

```cpp
Vector<DataType> container_element_types;
```

`Array[Node]` 存成 `kind = BUILTIN, builtin_type = ARRAY, container_element_types = [ DataType{kind=NATIVE, native_type="Node"} ]`；`Dictionary[String, int]` 存两个元素类型。由于 `container_element_types` 本身就是 `Vector<DataType>`，**任意深度嵌套自然支持**——`Array[Array[Dictionary[String, Node]]]` 只是一棵四层的 DataType 树。

### 4.1.4　`RESOLVING` 是 Analyzer 的"暂占位"

`kind = RESOLVING` 不是某种语言类型，而是 Analyzer 的**内部哨兵值**：表示"这个节点的类型目前正在被另一个调用栈处理中"。它在循环依赖检测里起决定性作用——见 4.4 节。

## 4.2　两族核心函数：`reduce_*` 与 `resolve_*`

Analyzer 头文件里的函数可以整齐地分成两族：

```cpp
// reduce 族 —— 作用于表达式节点（ExpressionNode）
void reduce_expression(ExpressionNode *p_expression, bool p_is_root = false);
void reduce_array(ArrayNode *);
void reduce_assignment(AssignmentNode *);
void reduce_await(AwaitNode *);
void reduce_binary_op(BinaryOpNode *);
void reduce_call(CallNode *, bool p_is_await = false, bool p_is_root = false);
void reduce_cast(CastNode *);
void reduce_dictionary(DictionaryNode *);
void reduce_get_node(GetNodeNode *);
void reduce_identifier(IdentifierNode *, bool can_be_builtin = false);
void reduce_identifier_from_base(IdentifierNode *, DataType *p_base = nullptr);
void reduce_lambda(LambdaNode *);
void reduce_literal(LiteralNode *);
void reduce_preload(PreloadNode *);
void reduce_self(SelfNode *);
void reduce_subscript(SubscriptNode *, bool p_can_be_pseudo_type = false);
void reduce_ternary_op(TernaryOpNode *, bool p_is_root = false);
void reduce_type_test(TypeTestNode *);
void reduce_unary_op(UnaryOpNode *);

// resolve 族 —— 作用于语句/声明节点
void resolve_node(Node *p_node, bool p_is_root = true);
void resolve_suite(SuiteNode *);
void resolve_if(IfNode *);
void resolve_for(ForNode *);
void resolve_while(WhileNode *);
void resolve_match(MatchNode *);
void resolve_match_branch(MatchBranchNode *, ExpressionNode *p_match_test);
void resolve_match_pattern(PatternNode *, ExpressionNode *p_match_test);
void resolve_return(ReturnNode *);
void resolve_assert(AssertNode *);

void resolve_assignable(AssignableNode *, const char *p_kind);
void resolve_variable(VariableNode *, bool p_is_local);
void resolve_constant(ConstantNode *, bool p_is_local);
void resolve_parameter(ParameterNode *);

void resolve_class_inheritance(ClassNode *, ...);
void resolve_class_interface(ClassNode *, ...);
void resolve_class_body(ClassNode *, ...);
void resolve_class_member(ClassNode *, const StringName &, ...);
void resolve_function_signature(FunctionNode *, ..., bool p_is_lambda = false);
void resolve_function_body(FunctionNode *, bool p_is_lambda = false);
void resolve_annotation(AnnotationNode *);
```

两族函数的差别是本质性的：

| | `reduce_*` | `resolve_*` |
|---|---|---|
| 作用对象 | 表达式节点（有值） | 语句/声明节点 |
| 主要产出 | 给节点填 `datatype`；如果是常量，填 `reduced_value` | 推进控制流、声明作用域里的符号、递归调用子节点的 reduce/resolve |
| 能否改变"值" | 可以（常量折叠） | 不能（不产生值） |
| 典型例子 | `reduce_call`、`reduce_identifier` | `resolve_if`、`resolve_for` |

最容易混的点：**表达式节点上的类型推断叫 reduce，而不是 resolve**——因为 Analyzer 的设计者想强调一件事：**reduce 除了算出类型，还可能"把这个表达式化简为更小形式"**。常量折叠是一种化简，而"化简"这个动作不适用于语句。

### 4.2.1　`reduce_expression` 的分发骨架

```cpp
void GDScriptAnalyzer::reduce_expression(ExpressionNode *p_expression, bool p_is_root) {
    if (p_expression == nullptr) return;
    if (p_expression->reduced) return;               // 已处理过
    p_expression->reduced = true;

    switch (p_expression->type) {
        case Node::ARRAY:           reduce_array(...); break;
        case Node::ASSIGNMENT:      reduce_assignment(...); break;
        case Node::BINARY_OPERATOR: reduce_binary_op(...); break;
        case Node::CALL:            reduce_call(..., p_is_root); break;
        case Node::CAST:            reduce_cast(...); break;
        case Node::DICTIONARY:      reduce_dictionary(...); break;
        case Node::GET_NODE:        reduce_get_node(...); break;
        case Node::IDENTIFIER:      reduce_identifier(...); break;
        case Node::LAMBDA:          reduce_lambda(...); break;
        case Node::LITERAL:         reduce_literal(...); break;
        case Node::PRELOAD:         reduce_preload(...); break;
        case Node::SELF:            reduce_self(...); break;
        case Node::SUBSCRIPT:       reduce_subscript(...); break;
        case Node::TERNARY_OPERATOR: reduce_ternary_op(..., p_is_root); break;
        case Node::TYPE_TEST:       reduce_type_test(...); break;
        case Node::UNARY_OPERATOR:  reduce_unary_op(...); break;
        ...
    }
}
```

- **幂等性**：`p_expression->reduced` 标志位防止重复处理。在循环依赖、Lambda 延后解析等场景，同一个节点可能被多条路径访问到。
- **`p_is_root` 参数**：区分"这个表达式是不是一条语句的顶层表达式"。有些警告只在顶层触发，比如"表达式结果被丢弃"。`CallNode` 是唯一一类"顶层可用"的表达式——其他丢弃结果的都会触发警告。

## 4.3　Analyzer 的四阶段流水线

`GDScriptAnalyzer::analyze()` 是对外入口，它按固定顺序调用四个私有阶段：

```cpp
Error GDScriptAnalyzer::analyze() {
    parser->apply_pending_warnings();
    Error err = resolve_inheritance();   // ①
    if (err) return err;

    err = resolve_interface();           // ②
    if (err) return err;

    err = resolve_body();                // ③
    if (err) return err;

    err = resolve_dependencies();        // ④
    if (err) return err;

    return OK;
}
```

### 阶段 ①　`resolve_inheritance()`：先把 extends 链理顺

走一遍所有类，确定每个 `ClassNode::base_class` / `base_script` / `base_native` 指向什么。这是最脆弱、最容易循环的一步——`class A extends B, B extends A` 不可能被支持，必须在这一步就发现。

实现上，它调用 `resolve_class_inheritance(cls, recursive=true)`，对 `extends_used` 节点做标识符解析，找到目标类，设置 `base_class`。若目标类还没解析过自己的 inheritance，就递归进去；若递归进入时发现 `cls->base_class` 已经标记成"正在解析"，立即报循环继承错误。

### 阶段 ②　`resolve_interface()`：只填"对外接口"

```
resolve_class_interface(cls)
 ├── 对每个成员调用 resolve_class_member(cls, name)
 │    ├── 变量：resolve_variable（填 datatype，但不 reduce 初始化表达式）
 │    ├── 常量：resolve_constant（必须 reduce，因为是常量）
 │    ├── 函数：resolve_function_signature（参数类型、返回类型；不进函数体）
 │    ├── 枚举：推断每个枚举值
 │    ├── 信号：填参数类型
 │    └── 内部类：递归 resolve_class_interface
 └── cls->resolved_interface = true
```

这一步的关键约束是：**绝对不分析任何函数体**。函数体里随便写一个 `B.some_method()`，而 B 可能还在等着 A 的接口——如果此时下潜进去分析函数体，立刻循环死锁。

### 阶段 ③　`resolve_body()`：现在可以看函数体了

```
resolve_class_body(cls)
 ├── 对每个函数调用 resolve_function_body
 │    ├── 新建作用域，把参数塞进 locals
 │    ├── resolve_suite(function_body)
 │    │    ├── 对每条 statement 调用 resolve_node
 │    │    └── 对每个表达式调用 reduce_expression
 │    └── 收集"Lambda 延后解析"队列
 ├── 对每个内部类递归 resolve_class_body
 └── cls->resolved_body = true
```

进入阶段 ③ 时，所有涉及到的类接口都已在阶段 ② 完成——因此函数体里任何跨类引用（`MyOtherClass.foo()`、`some_member: OtherClass`）都只需要**查接口**而不需要再触发 `_body` 层的分析。循环依赖问题在这个分界上被彻底消解。

### 阶段 ④　`resolve_dependencies()`：最后的收尾

处理一些需要所有 body 都就绪后才能做的事，比如：

- 某些延后的 Lambda 体；
- `@onready`、`@export_placeholder` 等注解的后期校验；
- 跨脚本的常量引用是否真的是常量；
- `preload()` 目标的最终依赖关系。

这一步之后，AST 就完全"成熟"，可以交给 Compiler 了。

## 4.4　循环依赖：`RESOLVING` 哨兵与按需成员解析

GDScript 支持相当广泛的循环依赖：

```gdscript
# a.gd
class_name A
var b_ref: B             # A 里有 B
func work(b: B): pass

# b.gd
class_name B
extends A                # B 继承 A
var a_list: Array[A]     # B 里有 A
```

这套代码能正常编译。秘诀在两层：

1. **两阶段**：阶段 ② 里只要接口（成员 + 签名），所以 A 的接口完成时不依赖 B 函数体，B 的接口完成时不依赖 A 函数体。
2. **按需成员解析 + RESOLVING 哨兵**：即使在阶段 ② 里，也可能出现"A 要求解析 B.foo，B.foo 的类型又引用了 A.bar"这种局部循环。这时 Analyzer 用一个显式 sentinel：

```cpp
DataType resolving_datatype;
resolving_datatype.kind = DataType::RESOLVING;
member_node->datatype = resolving_datatype;     // 先插标记

// 开始真正解析...
reduce_expression(member_node->initializer);

// 正常解析完成
member_node->datatype = computed;
```

如果在这"真正解析"的过程中又递归到了**同一个成员**，它会看到 `datatype.kind == RESOLVING`——Analyzer 立刻停下并报告循环依赖，而不是继续递归到栈溢出。

看一下 `GDScriptAnalyzer::resolve_class_member` 的典型结构（简化）：

```cpp
void resolve_class_member(ClassNode *p_class, int p_index, const Node *p_source) {
    ClassNode::Member &member = p_class->members.write[p_index];

    if (member.get_datatype().is_set()) return;              // 已解析完
    if (member.get_datatype().is_resolving()) {
        push_error("Could not resolve member (cyclic reference)", p_source);
        return;
    }

    // 插哨兵
    DataType resolving; resolving.kind = DataType::RESOLVING;
    set_member_datatype(member, resolving);

    // 真正解析（可能递归）
    switch (member.type) {
        case CONSTANT: resolve_constant(member.constant, false); break;
        case VARIABLE: resolve_variable(member.variable, false); break;
        case FUNCTION: resolve_function_signature(member.function); break;
        ...
    }
    // 这时 member.datatype 已经被具体的 resolve_* 改成了最终类型
}
```

`is_set()` / `is_resolving()` 这两个谓词就是为此设计的。这是一种**"三色标记"**思维的轻量版：UNRESOLVED（白）→ RESOLVING（灰）→ resolved（黑），遇到灰色立刻报错。

## 4.5　`reduce_identifier`：Analyzer 最重的"魔法"函数

一个 `IdentifierNode("foo")` 出现在代码里，它可能指向：

1. 局部变量（当前 SuiteNode 的 `locals`）；
2. 函数参数（当前 `FunctionNode->parameters`）；
3. 当前类的成员（`current_class->members`）；
4. 继承链上的成员（`current_class->base_class->...`）；
5. 当前类的内部类（作为类对象使用）；
6. 全局类（通过 `class_name` 注册）；
7. 自动加载单例（Autoload）；
8. 原生引擎类（`Node2D`、`Vector2`、`Color`...）；
9. 枚举类型名、枚举值；
10. GDScript 内置函数（`print`、`len`、`type_string`）；
11. Variant 全局函数（`sin`、`abs`、`min`）；
12. 内建常量（`PI`、`TAU`、`INF`、`NAN`——这些在词法阶段已经是独立 Token，但某些路径仍走 identifier）；
13. ……

`reduce_identifier` 就是这个分发表。它的骨架大致是：

```cpp
void GDScriptAnalyzer::reduce_identifier(IdentifierNode *p_id, bool can_be_builtin) {
    const StringName &name = p_id->name;

    // 1. 当前 Suite 的 locals
    if (SuiteNode *suite = current_suite) {
        int idx = suite->locals_indices.get(name, -1);
        if (idx >= 0) { /* 设 p_id->source = LOCAL_VARIABLE 等, 设 datatype, return */ }
    }

    // 2. 当前 Function 的参数（其实已经进 suite.locals）
    // ...

    // 3. 当前类成员与继承链
    if (current_class && current_class->members_indices.has(name)) {
        // 可能需要触发 resolve_class_member
    }
    // 继承链：current_class->base_class 递归查找

    // 4. Global classes（class_name 注册表）
    if (ScriptServer::is_global_class(name)) { /* SCRIPT / CLASS */ }

    // 5. Autoloads
    if (ProjectSettings::has autoload ...) { /* NATIVE/SCRIPT */ }

    // 6. 原生类（ClassDB::class_exists）
    if (ClassDB::class_exists(name)) { /* NATIVE, is_meta_type=true */ }

    // 7. 内建类型名（Variant::get_type_by_name）
    // 8. @GDScript 工具函数 (GDScriptUtilityFunctions)
    // 9. Variant 全局函数 (Variant::has_utility_function)
    // 10. "Variant" 关键字自身

    if (all of the above failed) {
        push_error("Identifier \"%s\" not declared", ...);
        mark_node_unsafe(p_id);
    }
}
```

真实代码有上千行，分支更多更细，但核心就是这一张查找表。每个分支都会给 `IdentifierNode::source` 赋一个特定的 `Source`（见 `gdscript_parser.h:900`）：

```cpp
enum Source {
    UNDEFINED_SOURCE, FUNCTION_PARAMETER, LOCAL_VARIABLE, LOCAL_CONSTANT,
    LOCAL_ITERATOR, LOCAL_BIND, MEMBER_VARIABLE, MEMBER_CONSTANT, MEMBER_FUNCTION,
    MEMBER_CLASS, MEMBER_SIGNAL, INHERITED_VARIABLE, STATIC_VARIABLE, ...
};
```

Compiler 后续根据这个 `source` 值决定生成哪种字节码访问方式——**同一个语法形式 `foo`，能生成十几种不同 Opcode**，全靠 Analyzer 这一步选对了 `source`。

### 4.5.1　`reduce_identifier_from_base`：属性访问的特别版本

`a.b` 解析时，Analyzer 会先 reduce 出 `a` 的 DataType，然后对 `b` 调用：

```cpp
void reduce_identifier_from_base(IdentifierNode *, DataType *p_base = nullptr);
```

它比 `reduce_identifier` 少几类（不查 locals、不查全局类），多一类（在 `p_base` 的成员里查）。这是 GDScript 里"成员查找"的真正实现。第 7 章里会看到，Compiler 会对"能静态解析的成员访问"生成 `OPCODE_GET_NAMED_*`，对"不能静态解析的"回落到 `OPCODE_GET_NAMED`——分支依据就是这里 reduce 出的 source 与 datatype。

## 4.6　常量折叠

`reduce_*` 在返回时除了填 `datatype`，还可能填：

```cpp
p_expression->is_constant = true;
p_expression->reduced_value = value;
```

几乎所有 `reduce_*` 都会在末尾加一段 "两个操作数都是常量？那我也是常量" 的判断，比如 `reduce_binary_op`：

```cpp
if (p_bin_op->left_operand->is_constant && p_bin_op->right_operand->is_constant) {
    bool valid = false;
    Variant result;
    Variant::evaluate(op, left, right, result, valid);
    if (valid) {
        p_bin_op->is_constant = true;
        p_bin_op->reduced_value = result;
    }
}
```

常量折叠的威力来自**组合性**：

- `1 + 2 * 3` → `BinaryOp(1, +, BinaryOp(2, *, 3))` → `BinaryOp(1, +, 6)` → `7`。
- `const MAX = 100; var x = MAX / 4` → `x` 的初始化表达式直接被折为常量 `25`。
- 常量表达式里的 `typeof(MY_ENUM.RED)` 也可以折。

Compiler 见到 `is_constant = true` 时会直接把 `reduced_value` 进常量池，一条 `LOAD_CONST` 解决问题，省掉运行时计算。对游戏脚本这种"大量魔法数字、枚举、长文本模板"的场景，效果相当可观。

## 4.7　"渐进类型"的实际落地：`mark_node_unsafe` 与 `downgrade_node_type_source`

### 4.7.1　`mark_node_unsafe`

Analyzer 每当做不了某件事时——类型未知、方法可能不存在、参数个数对不上但对方是 Variant——就调一次：

```cpp
void mark_node_unsafe(const Node *p_node);
```

它把节点加入一个内部"unsafe 集合"。最终这些节点在编辑器里显示为**灰色行号**，明确告诉用户："这一行我没办法替你保证安全，请自求多福。"

### 4.7.2　`downgrade_node_type_source`

有时候本来以为有类型，结果在更深层分析后发现信息不足——比如 `a = b + c`，原以为 `a` 能推断出类型，后来发现 `c` 是 Variant。这时调：

```cpp
void downgrade_node_type_source(Node *p_node);
```

把节点的 `type_source` 从 `INFERRED` 降到 `UNDETECTED`。关键是**不能从 `ANNOTATED_*` 降级**——用户写了的承诺 Analyzer 不能偷偷撤销。

这种"只能降级软信息、绝不改写硬承诺"的纪律贯穿整个 Analyzer，是"类型注解到底意味着什么"的实际定义。

## 4.8　Lambda 的延后解析

Lambda 体内部可能引用了**外层函数还没走到**的局部变量。比如：

```gdscript
func foo():
    var callback = func(): return x    # 这时 x 还没声明
    var x = 42
    return callback.call()
```

GDScript 语义上 Lambda **按文本位置捕获**外层作用域——也就是说 `x` 必须先声明再用。但 Parser 生成 `LambdaNode` 时，Analyzer 未必已经看到 `var x`。

解决方案是：**Lambda 体的 reduce 被延后**。`reduce_lambda` 只解析参数与签名，然后把 Lambda 加入 `pending_body_resolution_lambdas`。等外层 suite 的所有语句都 resolve 完，Analyzer 再调用 `resolve_pending_lambda_bodies()` 反过来处理这些 Lambda 体——此时 `x` 已经在 outer suite 的 locals 里，可以正常解析。

这是 Analyzer 里少见的"逻辑上非线性"的地方，但为的是让 Lambda 语法**跟普通函数一样直观**。

## 4.9　与 `GDScriptCache` / `GDScriptParserRef` 的互动

Analyzer 经常需要分析**另一个脚本**——当前脚本 `preload("other.gd")` 或 `extends OtherClass` 时。它不能自己造一个新的 Parser/Analyzer，否则每次都要重做，也会导致环形调用混乱。

实际的做法是向 `GDScriptCache` 要一个 `GDScriptParserRef`。这个 Ref 是"分阶段缓存"的：

```
GDScriptParserRef 状态：
    EMPTY → PARSED → INHERITANCE_SOLVED → INTERFACE_SOLVED → FULLY_SOLVED
```

Analyzer 向 Ref 请求时只要 "最多到哪一阶段"。例如解析 `extends B` 只需要 B 的 INHERITANCE_SOLVED；查 `B.foo` 的类型只需要 INTERFACE_SOLVED；真正要进 `B.foo` 的函数体才需要 FULLY_SOLVED。

这保证了跨脚本分析**最小化地推进**——既避免不必要的重复工作，又在循环依赖下能安全停步。这套机制详见第 19 章。

## 4.10　警告系统与 shadow 检查

Analyzer 顺便也做了大量软性检查：

```cpp
void is_shadowing(IdentifierNode *p_identifier, const String &p_context, bool p_in_local_scope);
```

检查"这个名字是不是遮盖了外层的同名成员/类/全局名"。Shadow 一般不报错（GDScript 允许）但会发 `SHADOWED_VARIABLE`、`SHADOWED_VARIABLE_BASE_CLASS` 等警告，在编辑器里以黄色下划线呈现。

所有这些警告由 `parser->push_warning(...)` 登记，最终在 `apply_pending_warnings()` 里按 `@warning_ignore` 规则过滤后输出。警告系统见第 22 章。

## 4.11　一段代码走完 Analyzer

看最终的一段小例子：

```gdscript
class_name Player
extends Node2D

@export var speed: float = 200.0

func _process(delta: float) -> void:
    position += Vector2.RIGHT * speed * delta
```

Analyzer 阶段 ② 之后：

- `Player` 的 `DataType{ kind=CLASS, class_type=Player, type_source=ANNOTATED_EXPLICIT }`
- `speed` 的 DataType = `{ kind=BUILTIN, builtin_type=FLOAT, ANNOTATED_EXPLICIT }`
- `_process` 的签名 = `void (float)`；`delta` 参数 DataType = `float (ANNOTATED_EXPLICIT)`

Analyzer 阶段 ③ 解析 `_process` 函数体：

1. `position` 是 `IdentifierNode`——`reduce_identifier`：查当前类无此成员 → 查基类 `Node2D` → `ClassDB` 告诉它 `Node2D.position: Vector2` → `IdentifierNode.source = INHERITED_VARIABLE`，datatype 填 `Vector2`。
2. `Vector2.RIGHT` 是 `SubscriptNode(is_attribute=true)`——先 reduce 基体 `Vector2`（得到 `is_meta_type` 的 Vector2 DataType），再 `reduce_identifier_from_base("RIGHT", &base)`，查到它是静态常量 `Vector2(1,0)`；`is_constant=true, reduced_value=Vector2(1,0)`。
3. `Vector2.RIGHT * speed` 是 `BinaryOp`——`reduce_binary_op` 询问 `Variant::evaluate(MUL, Vector2, float)`，合法，类型 `Vector2`。其中 `Vector2.RIGHT` 是常量，`speed` 不是，整体不是常量。
4. `* delta` 再乘一次，类型还是 `Vector2`。
5. `position += ...` 是 `AssignmentNode`——`reduce_assignment` 检查左值可写（`INHERITED_VARIABLE` 可以），右值类型兼容左值，OK。
6. 返回类型是 `void`，最后一条没有 `return` 语句——合法。

整段代码没有任何 `mark_node_unsafe`，所以这一行在编辑器里是**绿色行号**。如果 `Vector2.RIGHT` 被写成 `Vector2.RIGT`（拼写错误），第 2 步 `reduce_identifier_from_base` 返回失败，Analyzer 报错 + `mark_node_unsafe`，行号变灰 + 红色错误提示。

## 本章小结

- **`DataType`** 是 Analyzer 的通用载体，它的 `Kind × TypeSource` 二维决定了"类型是什么 + 用户承诺了没有"。`is_hard_type()` 与 `operator==` 的宽容实现，是渐进类型哲学的落点。
- Analyzer 分两族函数：**`reduce_*`** 处理表达式（同时做常量折叠），**`resolve_*`** 处理语句/声明（推进作用域与控制流）。
- `analyze()` 按固定顺序跑四阶段：**inheritance → interface → body → dependencies**。阶段 ② 只做接口，阶段 ③ 才进函数体——这是解开跨脚本循环依赖的钥匙。
- 局部循环用 **`RESOLVING` 哨兵** 检测——一种三色标记思路的轻量实现。
- **`reduce_identifier`** 是 Analyzer 里最密集的"分发表"，决定了同一个标识符最终被视作局部、参数、成员、原生类、全局函数、Autoload 中的哪一种，直接影响 Compiler 生成的 Opcode 变体。
- 常量折叠贯穿所有 `reduce_*`，按组合性自动递进，常量表达式的计算被完全前移到编译期。
- **`mark_node_unsafe`** 与 **`downgrade_node_type_source`** 是渐进类型在工程上的两个闸门——允许 Analyzer "忍让"，但绝不偷偷撤销用户承诺。
- Lambda 延后解析、与 `GDScriptParserRef` 多阶段缓存协作、shadow 警告，都是 Analyzer 与外界协同的典型例子。

走到这里，AST 已经从"结构正确"升级到"语义完备"。下一章我们先补一小块——**注解系统（第 5 章）**，因为它横跨 Parser / Analyzer 并影响到 Compiler；之后便进入中端：**第 6 章 Compiler**。

---

[← 上一章：第 3 章 Parser & AST](./03-parser.md) · [目录](./README.md) · [下一章：第 5 章 注解系统 →](./05-annotations.md)
