# 第 6 章　编译器主流程：`GDScriptCompiler`

> 本章对应源码：
> `modules/gdscript/gdscript_compiler.h`、`gdscript_compiler.cpp`，
> 以及 `gdscript.h` 中的 `GDScript::reload()`、`gdscript_codegen.h` 中的 `GDScriptCodeGenerator` 抽象基类。

Analyzer 把 AST 从"结构正确"升级到"语义完备"之后，离运行时还差关键一步：把 AST 翻译成字节码，并把 `GDScript` 这个运行时类对象装配起来。这一步由 `GDScriptCompiler` 负责。

本章讨论 Compiler 的 **主流程**——它怎么组织工作、怎么避免循环依赖、怎么跟运行时对象打交道。真正"**翻译一条表达式到多少字节码**"的机械活留到第 7 章 `GDScriptByteCodeGenerator` 再讲。这样拆开读更清楚。

## 6.1　Compiler 的职责

`GDScriptCompiler` 的对外 API 很简单：

```cpp
Error GDScriptCompiler::compile(const GDScriptParser *p_parser,
                                GDScript *p_script,
                                bool p_keep_state = false);
```

- **输入**：一个已经被 Analyzer 处理完毕的 `GDScriptParser`（持有完整 AST），以及一个"目标" `GDScript` 对象（可能是空壳，也可能是热重载场景下的旧实例）。
- **产出**：把 AST 的所有信息翻译进 `p_script`——包括成员变量槽位、常量池、信号定义、子类、每个 `GDScriptFunction` 的字节码——使之成为运行时可用的脚本。
- **`p_keep_state`**：热重载场景置 `true`，表示"尽可能保留已有实例的成员值、捕获 Lambda 等"。

从这个接口可以看出两件事：

1. **Parser 和 `GDScript` 分离**：Parser/Analyzer 只关心 AST，一个字节都不碰 `GDScript`。Compiler 才是连接两个世界的桥。
2. **Compiler 不拥有 `GDScript`**：它的职责是**填充**，不是创建。`GDScript` 对象本身由 `GDScriptCache` 或 `GDScript::reload()` 的调用者持有。

## 6.2　两阶段编译：接口 vs 实现

第 1 章和第 4 章反复强调过"**两阶段**"以避免循环依赖。Compiler 把这条纪律贯彻到最严：

```cpp
Error GDScriptCompiler::_prepare_compilation(GDScript *p_script,
                                             const GDScriptParser::ClassNode *p_class,
                                             bool p_keep_state);

Error GDScriptCompiler::_compile_class(GDScript *p_script,
                                       const GDScriptParser::ClassNode *p_class,
                                       bool p_keep_state);
```

按 README 的说法：

> The compilation process of a class ... _cannot_ depend on information obtained by calling `_compile_class()` on another class, for the same cyclic dependency reasons.

翻译成实现约束：

- **阶段 ①　`_prepare_compilation`**：只做"把 AST 上 Analyzer 已经算好的接口信息搬进 `GDScript` 对象壳"这件事——成员变量列表、成员常量、信号、函数签名、导出属性、内部类的空壳……**不生成任何字节码，不处理任何函数体**。
- **阶段 ②　`_compile_class`**：真正编译函数体、生成字节码、把 `GDScriptFunction` 对象塞进 `GDScript::member_functions`。**此阶段允许且仅允许查询已经 `_prepare_compilation` 过的其它脚本的接口信息**。

`_compile_class` 之间**必须互不调用**。任何需要"看别的类的东西"的操作都必须在阶段 ① 准备好，阶段 ② 只看本脚本的 AST + 其他脚本的已备好接口。

这条纪律落到代码里就是 `parsing_classes` / `parsed_classes` 两个集合：

```cpp
HashSet<GDScript *> parsed_classes;
HashSet<GDScript *> parsing_classes;
```

`_compile_class(x)` 入口处先 `parsing_classes.insert(x)`，出口处 `parsing_classes.erase(x); parsed_classes.insert(x)`。如果在编译过程中以任何方式递归回到 `_compile_class(x)`，立即中止并报错。这是第 4 章 `RESOLVING` 哨兵在编译期的对应物。

## 6.3　`compile()` 的总体流程

简化后的骨架：

```cpp
Error GDScriptCompiler::compile(const GDScriptParser *p_parser,
                                GDScript *p_script, bool p_keep_state) {
    parser = p_parser;
    main_script = p_script;
    const ClassNode *root = parser->get_tree();

    // 0) 为整棵类树预创建 GDScript 空壳（含内部类）
    make_scripts(p_script, root, p_keep_state);

    // 1) 接口阶段：把所有类（含内部）都准备好
    _prepare_compilation(p_script, root, p_keep_state);

    // 2) 实现阶段：逐类编译函数体，产生字节码
    _compile_class(p_script, root, p_keep_state);

    // 3) 热重载下的 Lambda 引用替换
    if (p_keep_state) {
        /* 计算 old → new 的 GDScriptFunction* 映射
         * 更新已有 GDScriptLambdaCallable 的持有引用 */
    }

    return OK;
}
```

下面把几个关键步骤分开看。

### 6.3.1　`make_scripts`：为内部类预建空壳

`GDScriptCompiler::make_scripts(GDScript *, const ClassNode *, bool)` 是一个静态方法。它递归遍历 ClassNode 树，为每个 `ClassNode` 在对应的 `GDScript` 对象里创建"内部 `GDScript` 子对象"的壳并建立 `subclasses[name] -> GDScript *` 映射。

```gdscript
class Outer:
    class Inner:
        class Leaf:
            pass
```

这段代码对应三个嵌套的 `GDScript` 对象。**壳先建好**的意义在于：阶段 ② 里编译 `Outer.foo()` 时如果需要引用 `Outer.Inner` 作为类型或类字面量，能直接拿到 Inner 对应的 `GDScript *`；而无需此时此刻去 `_compile_class(Inner)`——那会立刻引入循环。

### 6.3.2　`_prepare_compilation`：填充所有接口字段

它的工作量相当可观，主要任务按顺序大致是：

1. **基类链**：读 `class_node->base_class`（Analyzer 已填），把 `GDScript::base` / `native_class` / `base_type` 填好。
2. **继承成员合并**：向上遍历基类 `GDScript`，把成员表复制进本 `GDScript::member_indices`（然后再被自身成员覆盖）。这一步复刻 C++ 的继承内存布局逻辑。
3. **成员变量槽位分配**：按 `ClassNode::members` 顺序，给每个 `Type == VARIABLE` 的成员在 `GDScript::members` 里分配一个槽位索引，同时填充 `PropertyInfo`（包括 export 信息）。
4. **成员常量**：把 Analyzer 已经 `reduced_value` 好的 `CONSTANT` 直接塞进 `GDScript::constants`。
5. **信号**：依据 `SignalNode` 构造 `MethodInfo` 放进 `GDScript::signals`。
6. **函数签名占位**：对每个 `FunctionNode` 先插一个空的 `GDScriptFunction` 占位到 `member_functions`，签名填好，但 `code` 数组为空。这样阶段 ② 里别的类就能"看到"本函数存在。
7. **RPC 配置**：从 `FunctionNode::rpc_config` 复制到 `GDScript::rpc_config`（供 `GDScriptRPCCallable` 在运行时查）。
8. **递归内部类**：对每个子 `ClassNode` 递归调用 `_prepare_compilation`。

做完阶段 ①，整棵 `GDScript` 对象树就已"长得完整"——它有正确的基类链、成员表、常量表、信号表、函数签名——仅仅是每个函数的字节码是空的。此时任何外部脚本都可以安全地查询它的接口。

### 6.3.3　`_compile_class`：真正生成字节码

阶段 ② 的工作：

1. 把当前 `p_script` 放进 `parsing_classes`；
2. 对每个 `FunctionNode` 调用 `_parse_function` 编译函数体，产出一个 `GDScriptFunction`，替换阶段 ① 填的占位；
3. 合成若干"隐式方法"（见下一节）；
4. 对每个 `VariableNode` 处理 setter/getter：
   - 若 setter/getter 是内联 Lambda（`var x: int: set(v): x = v`），合成独立的 `GDScriptFunction`；
   - 若是引用已有成员函数（`var x: int: set = _set_x`），只记录函数名；
5. 递归编译内部类；
6. 把 `p_script` 从 `parsing_classes` 移到 `parsed_classes`。

## 6.4　隐式合成方法

`_compile_class` 会为每个类"凭空"多造几个方法。这些方法在源码里不存在，但在 `member_functions` 里和普通方法一样被注册。

### 6.4.1　`@implicit_new` / `_init` / `new` 合成

脚本实例化时引擎会调 `_init()`（如果用户定义）或一个隐式的初始化路径。Compiler 对每个类合成一个 `@implicit_new` 函数，它按以下顺序执行：

1. 对每个成员变量赋其默认值或初始化表达式（Analyzer 已帮忙 reduce 过的 `reduced_value` 直接用，否则生成运行时表达式代码）；
2. 调用用户 `_init`（若存在）。

这样 `_init` 里用户**不需要**手动初始化每个变量——Compiler 已经在 `_init` 之前把所有声明时的初始化代码挤进隐式方法里。

### 6.4.2　`@implicit_ready`：`@onready` 的落点

第 5 章讲过，`@onready var x = $Node` 的语义是"`_ready()` 被调用前执行"。Compiler 把所有 `@onready` 变量的初始化表达式收集起来，合成一个 `@implicit_ready` 方法，`GDScript` 实例被 `Node` 通知 `_ready` 时先调这个方法。

这比"在 `_ready` 里手写 `x = $Node`"好的地方在于：**用户写的 `_ready` 体完全可替换**——override 它都不会把 onready 绑定弄丢。隐式方法是 GDScript "约定优于语法"的典型例子。

### 6.4.3　静态初始化器：`_make_static_initializer`

```cpp
GDScriptFunction *_make_static_initializer(Error &, GDScript *, const ClassNode *);
```

类里所有 `static var`（类级静态变量）的初始化组合成一个 `@static_initializer` 函数。`has_static_data = true` 的类会在首次被引用时调用这个函数完成静态字段初始化——类似 C# 的 static constructor 语义。

### 6.4.4　setter / getter

```cpp
Error _parse_setter_getter(GDScript *, const ClassNode *,
                           const VariableNode *, bool p_is_setter);
```

对于下面这种"内联 setter / getter"：

```gdscript
var health: int:
    set(v):
        health = clamp(v, 0, max_health)
    get:
        return health
```

它们的 body 是 `FunctionNode` 风格的 `SuiteNode`，Compiler 把它们当成独立方法来编译，命名通常是 `@xxx_setter` / `@xxx_getter`，放进 `member_functions`。`VariableNode::setter` / `getter` 字段会持有对应函数指针，运行时访问成员时走 `ScriptInstance::set_fallback` / `get_fallback` 路径，由 `GDScriptInstance`（第 16 章）调这些隐式方法。

## 6.5　`CodeGen`：单个函数的编译上下文

来看 Compiler 内部为**每一个函数**的编译所创建的上下文对象：

```cpp
struct CodeGen {
    GDScript *script = nullptr;
    const ClassNode    *class_node    = nullptr;
    const FunctionNode *function_node = nullptr;
    StringName          function_name;
    GDScriptCodeGenerator *generator = nullptr;

    HashMap<StringName, GDScriptCodeGenerator::Address> parameters;
    HashMap<StringName, GDScriptCodeGenerator::Address> locals;
    List<HashMap<StringName, GDScriptCodeGenerator::Address>> locals_stack;
    bool is_static = false;

    Address add_local(...);
    Address add_local_constant(...);
    Address add_temporary(...);
    Address add_constant(...);
    void start_block();
    void end_block();
};
```

这段结构体就是 Compiler 与 ByteCodeGenerator 之间的 **"桥"**。我们逐项看：

### 6.5.1　`generator`：字节码生成器

`GDScriptCodeGenerator` 是一个抽象基类（`gdscript_codegen.h`），当前仅有一个具体实现 `GDScriptByteCodeGenerator`（第 7 章详讲）。Compiler 通过这个抽象接口调用 `write_*` 族函数把 Opcode 写进 `GDScriptFunction::code`——这个抽象层意味着 GDScript 理论上可以**未来增加另一种 CodeGen 后端**（比如 C++ 代码生成或 JIT），Compiler 不需要大改。

### 6.5.2　`Address`：统一的"值位置"

`GDScriptCodeGenerator::Address` 是 Compiler/CodeGen 交互的核心数据类型：

```cpp
struct Address {
    enum AddressMode {
        SELF, CLASS, MEMBER, CONSTANT, LOCAL_VARIABLE, FUNCTION_PARAMETER,
        TEMPORARY, STATIC_VARIABLE, NIL
    };
    AddressMode mode;
    uint32_t address;       // 按 mode 解释的索引
    GDScriptDataType type;
};
```

Compiler 里，任何一个"值在哪儿"的问题都用 `Address` 回答：

- 局部变量 → `LOCAL_VARIABLE` + 栈索引；
- 参数 → `FUNCTION_PARAMETER` + 参数位；
- 成员 → `MEMBER` + 成员槽；
- 常量 → `CONSTANT` + 常量池索引；
- 临时寄存器 → `TEMPORARY` + 寄存器号；
- `self` → `SELF`；
- 类对象 → `CLASS`；
- `static var` → `STATIC_VARIABLE`；
- `null` → `NIL`。

一条表达式的 `_parse_expression` 返回值就是一个 `Address`，告诉调用者"我的结果放在这里"。调用者如果只是想把这个结果再作为下一指令的操作数，直接把 `Address` 塞进 `generator->write_xxx(dst, src1, src2)` 即可。

### 6.5.3　`locals` + `locals_stack`：嵌套作用域的实现

GDScript 允许块级作用域里声明局部变量，但又不像 C++ 那样为每个块分配独立栈帧——所有局部共享同一片 `GDScriptFunction` 栈。Compiler 的处理是：

- **进入块**（`if` / `for` / `while` 体等）调用 `start_block()`：把当前 `locals` 快照压进 `locals_stack`；
- **离开块**：`end_block()`：从 `locals_stack` 弹出快照恢复。

这样块内新声明的变量名在块结束后从 `locals` 消失（再用同名会被视为新变量），但它们**占用的栈索引**仍然被 CodeGen 的 `temporaries` / `locals_index` 计算覆盖——CodeGen 有自己的重用策略。Compiler 只管名字作用域，CodeGen 管栈物理布局。

### 6.5.4　`add_constant` 的类型推断

注意 `add_constant(Variant)` 的实现里有一段特殊处理：

```cpp
if (type.builtin_type == Variant::OBJECT) {
    Object *obj = p_constant;
    if (obj) {
        type.kind = GDScriptDataType::NATIVE;
        type.native_type = obj->get_class_name();
        Ref<Script> scr = obj->get_script();
        if (scr.is_valid()) {
            type.script_type = scr.ptr();
            Ref<GDScript> gdscript = scr;
            if (gdscript.is_valid()) {
                type.kind = GDScriptDataType::GDSCRIPT;
            } else {
                type.kind = GDScriptDataType::SCRIPT;
            }
        }
    } else {
        type.builtin_type = Variant::NIL;
    }
}
```

当把一个 `Object *` 作为常量塞进常量池时，Compiler 会"透视"它：原生 C++ 类？绑定了脚本？是 GDScript 还是别的脚本？这些信息都会被写到 Address 的 `type` 上。**这是 GDScript 之所以能对常量 base 生成 specialized Opcode 的关键一步**——比如 `MyClass.FOO` 里的 `MyClass` 如果被预先 reduce 成 `Ref<GDScript>` 常量，Compiler 能生成 `OPCODE_GET_STATIC` 直接从 GDScript 常量表取 `FOO`，省掉运行时类型判定。

## 6.6　`GDScriptDataType`：Parser 到运行时的类型转换

注意这里出现了一个新类型 **`GDScriptDataType`**（来自 `gdscript_function.h`），和我们第 4 章见过的 `GDScriptParser::DataType` **不是同一个**。这是 GDScript 实现里很重要的一对并行类型：

| `GDScriptParser::DataType` | `GDScriptDataType` |
|---|---|
| 编译期用 | 运行时用 |
| 信息更丰富：容器元素类型、类 AST 指针、TypeSource... | 信息更紧凑：只保留运行时分发必需的 |
| Analyzer 阶段填 | 保存在 `GDScriptFunction` 内、`GDScript::member_info` 里 |
| 仅 Parser/Analyzer/Compiler 知道 | VM、Inspector、ScriptInstance 都要用 |

转换函数就是 `GDScriptCompiler::_gdtype_from_datatype`：

```cpp
GDScriptDataType _gdtype_from_datatype(const GDScriptParser::DataType &p_datatype,
                                       GDScript *p_owner,
                                       bool p_handle_metatype = true);
```

它丢弃大部分 Analyzer 才关心的字段（比如 `type_source`、`is_meta_type`、AST 指针），保留运行时类型判定需要的：`kind` / `builtin_type` / `native_type` / `script_type` / 容器元素类型等。

这个分离的意义：**运行时不需要也不应该知道"这个类型是用户显式注解的还是推断的"** —— 那是编译期的关心。`GDScriptFunction` 里存的只是"参数应该是什么类型"，不是"用户到底信不信"。**这个边界划得干净**，是第 4 章讲的"reduce/resolve 与 GDScript 对象间的墙"在运行时对应的另一面墙。

## 6.7　`_parse_function`：一次函数编译的全流程

```cpp
GDScriptFunction *_parse_function(Error &r_error, GDScript *p_script,
                                  const ClassNode *p_class, const FunctionNode *p_func,
                                  bool p_for_ready = false, bool p_for_lambda = false);
```

一次调用大致做：

1. 新建 `GDScriptFunction` 对象；
2. 构造 `CodeGen codegen`，附上 `ByteCodeGenerator`；
3. 注册参数到 `codegen.parameters` 并告诉 generator 栈布局；
4. 如果是实例方法（非 `p_for_lambda` 时若非 static），保留 `self` slot；
5. 调用 `_parse_block(codegen, func->body)` 编译函数体；
6. 如果函数声明了返回类型却缺少 `return`，补一条 "隐式 return null"；
7. 调用 `codegen.generator->end_function(...)` 让 CodeGen 把栈大小、常量池、字节码数组 "烘焙" 进 `GDScriptFunction`；
8. 返回该 `GDScriptFunction`。

`p_for_ready` 和 `p_for_lambda` 是两个旗标：

- `p_for_ready`：这次编译其实是合成 `@implicit_ready`，body 是一个人造的 SuiteNode（按 `@onready` 变量拼出来的）；
- `p_for_lambda`：这次编译对应用户源码里的一个 `LambdaNode`，需要额外处理捕获（captures）——捕获的外层变量会转化为 Lambda 函数对象上的"捕获槽"，并在调用时通过 `GDScriptLambdaCallable` 传入。

### 6.7.1　`_parse_block`：块级递归

```cpp
Error _parse_block(CodeGen &codegen, const SuiteNode *p_block,
                   bool p_add_locals = true, bool p_clear_locals = true);
```

它按顺序走 `p_block->statements`，逐条语句分派到各个 `_parse_*` 帮助函数。核心 switch 大致是：

```cpp
for (Node *stmt : p_block->statements) {
    switch (stmt->type) {
        case Node::VARIABLE:    /* 走 _parse_expression 生成初始化字节码 */
        case Node::IF:          /* codegen.generator->write_if(...); 递归 _parse_block */
        case Node::FOR:         /* codegen.generator->write_for(...); 递归 */
        case Node::WHILE:       /* 类似 */
        case Node::MATCH:       /* 复杂，见下节 */
        case Node::RETURN:      /* _parse_expression 求值 → write_return */
        case Node::CALL:        /* 表达式语句：_parse_expression p_root=true */
        case Node::ASSIGNMENT:  /* 左值生成 + 右值生成 + write_assign */
        ...
    }
}
```

`_parse_block` 本身非常薄——真正翻译一种结构到多少 Opcode 的决定权都在 CodeGen 的 `write_if` / `write_for` 等方法。第 7 章会看 CodeGen 是怎么做跳转回填的。

### 6.7.2　`_parse_match_pattern`：match 的单独处理

`match` 是 GDScript 里语义最复杂的控制流：它支持字面量、类型、数组、字典、变量绑定、通配符 `_`、嵌套模式等。Compiler 对 match 走了一个专门函数：

```cpp
Address _parse_match_pattern(CodeGen &codegen, Error &r_error,
                             const PatternNode *p_pattern,
                             const Address &p_value_addr,
                             const Address &p_type_addr,
                             const Address &p_previous_test,
                             bool p_is_first, bool p_is_nested);
```

它递归编译一种模式，产出一个**布尔临时 Address**，表示"这个 pattern 是否匹配"。`_parse_block` 对整个 `match` 语句的处理就变成："对每个 branch 生成 pattern 测试字节码 → 若 true 则跳转到 branch 体 → 否则继续下一个 branch"。

复杂的 pattern（如 `[head, ..tail]`）在这里被递归展开成大量 Opcode——这是 GDScript 字节码可能很长的少数几个地方之一。

## 6.8　热重载：`p_keep_state` 与 Lambda 替换

游戏运行中保存 `.gd` 文件触发热重载时，`GDScript::reload()` 会带上 `p_keep_state = true`。此时：

1. 旧的 `GDScriptFunction *` 很快要被新的替换掉；
2. 但场景里可能已经存在一堆 `GDScriptLambdaCallable`——它们持有"老的 `GDScriptFunction *`"。如果旧函数对象被 `memdelete`，这些 Callable 会变成悬垂引用。

Compiler 的解法是：编译完成后构造一张 `HashMap<GDScriptFunction *, GDScriptFunction *>` 的 **old → new 映射**，然后遍历所有已存在的 `GDScriptLambdaCallable` 把它们持有的指针就地更新：

```cpp
FunctionLambdaInfo _get_function_replacement_info(GDScriptFunction *p_func,
                                                  int p_index = -1,
                                                  int p_depth = 0,
                                                  GDScriptFunction *p_parent_func = nullptr);

Vector<FunctionLambdaInfo> _get_function_lambda_replacement_info(
    GDScriptFunction *p_func, int p_depth = 0, GDScriptFunction *p_parent_func = nullptr);

ScriptLambdaInfo _get_script_lambda_replacement_info(GDScript *p_script);

bool _do_function_infos_match(const FunctionLambdaInfo &old, const FunctionLambdaInfo *p_new);

void _get_function_ptr_replacements(HashMap<GDScriptFunction *, GDScriptFunction *> &r,
                                    const FunctionLambdaInfo &old,
                                    const FunctionLambdaInfo *p_new);
// ... 同名重载 ...
```

思路是：

1. 编译前，先对旧 `GDScript` 做 `_get_script_lambda_replacement_info()`，把"每个 Lambda 位置的身份证"（所在函数、index、depth、use_self、capture_count、arg_count 等）记下来（`FunctionLambdaInfo` / `ScriptLambdaInfo`）。
2. 编译后，对新 `GDScript` 做同样的遍历。
3. 对两棵"身份证树"做同构匹配——`_do_function_infos_match` 会逐项比较身份证字段，只有完全相同才认为是"同一 Lambda 的新旧版本"。
4. 匹配上的旧 `GDScriptFunction *` 映射到新的；构建 `replacements` 表。
5. 遍历所有 `GDScriptLambdaCallable` 把指针替换掉。

这套机制让"改一个 Lambda 内部代码"的热重载**不必让玩家重建任何回调注册**。但如果你增删/重排了 Lambda，身份证匹配不上，Compiler 会选择安全地失效那个 Callable（它下一次被调用时会变成空操作）。

## 6.9　Compiler 与 `GDScriptParser::CompletionContext`

Compiler 在编辑器场景下也有特殊行为：如果 Parser 标记过完整性（`p_for_completion`），Compiler 在编译过程中**不会阻塞于某些错误**——只要函数体有一条"看起来像"正确的 Opcode 就往下走。这让编辑器即使在写一半的代码上也能得到"尽量完整"的 `GDScript`，供跳转、悬浮提示、部分补全等功能使用。

这也是为什么 `_compile_class` 返回 `Error` 而非 `void`——许多失败都被吞掉变成 `OK`，真正致命的才会返回 `ERR_COMPILATION_FAILED`。

## 6.10　一次完整调用：从 `GDScript::reload()` 看 Compiler

我们追一次热保存场景。源码在 `gdscript.cpp` 里 `GDScript::reload()` 大致做：

```cpp
Error GDScript::reload(bool p_keep_state) {
    // 1. 分析器
    GDScriptParser parser;
    Error err = parser.parse(source, path, /*for_completion*/ false);
    if (err) return err;

    GDScriptAnalyzer analyzer(&parser);
    err = analyzer.analyze();
    if (err) return err;

    // 2. 编译器
    GDScriptCompiler compiler;
    err = compiler.compile(&parser, this, p_keep_state);
    if (err) return err;

    // 3. 注册 & 通知实例热重载
    valid = true;
    GDScriptLanguage::get_singleton()->script_reloaded(this);
    return OK;
}
```

只要 Parser 成功、Analyzer 成功，`compile(&parser, this, true)` 就会：

1. `make_scripts(this, root, true)`；
2. `_prepare_compilation(this, root, true)` 填接口；
3. `_compile_class(this, root, true)` 产出字节码；
4. 做 Lambda 替换映射；
5. 返回 `OK`；

`GDScript::reload` 在 3 步之后被通知所有活动实例"重载自己"——这时 `GDScriptInstance` 会重新执行 `@implicit_new` 与 `@implicit_ready`（如有保留），再把旧状态 merge 回来。

## 本章小结

- `GDScriptCompiler::compile` 是 Analyzer 与运行时的桥。它只**填充**给定的 `GDScript` 对象树，不拥有。
- Compiler 严格遵守**两阶段分离**：`_prepare_compilation`（接口）只搬 Analyzer 算好的成员/签名信息，`_compile_class`（实现）才产字节码。两阶段之间由 `parsing_classes` / `parsed_classes` 守护。
- `make_scripts` 为内部类预建空壳，保证跨类引用在阶段 ② 能用 `GDScript *` 安全标记。
- 每个函数一次编译对应一个 `CodeGen` 上下文，里面有 `parameters` / `locals` / `locals_stack` / 抽象 `generator`。所有"值在哪儿"的问题用统一的 `Address` 表达。
- 两个并行的类型系统：**`GDScriptParser::DataType`**（编译期）和 **`GDScriptDataType`**（运行时），通过 `_gdtype_from_datatype` 单向转换。运行时不再知道"用户是否承诺了这个类型"。
- Compiler 自动合成若干隐式方法：`@implicit_new`、`@implicit_ready`、`@static_initializer`，让 onready、static、字段初始化等语法糖对用户透明。
- setter/getter、match pattern、Lambda 等复杂结构都有专门的 `_parse_*` 子例程处理。
- 热重载场景下，Compiler 通过 `FunctionLambdaInfo` 树做新旧 Lambda 的同构匹配，构建 `GDScriptFunction *` 替换表，让已注册的回调能无缝继续工作。

下一章我们下潜到 Compiler 调用的那个抽象 `generator`——`GDScriptByteCodeGenerator`。那里才是"一条 `if` 会翻成几条 Opcode、跳转如何回填、栈槽位如何分配"的真正战场。

---

[← 上一章：第 5 章 注解系统](./05-annotations.md) · [目录](./README.md) · [下一章：第 7 章 字节码生成：`GDScriptByteCodeGenerator` →](./07-bytecode-generator.md)
