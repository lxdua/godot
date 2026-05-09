# 第 13 章　Lambda 与 `GDScriptLambdaCallable`

GDScript 的 lambda 看起来很轻量——一句 `var f = func(x): return x * 2`
就能在脚本里得到一个可以四处传递、保存到信号、塞进 Tween 回调的可调用
对象。但要让它真的“工作”，需要回答一系列棘手的问题：

* lambda 是 `Callable`，但 GDScript 函数对象是 `GDScriptFunction`——
  谁把它们粘起来？
* lambda 内部能用 `self` 吗？如果用了，会不会让宿主对象“僵尸化”？
* lambda 能引用外层局部变量（捕获）吗？是按值还是按引用？
* 如果脚本被热重载（reload），那些“漂浮在外”的 lambda Callable 会不会
  失效？

这一章给出每一个问题的实现层答案。涉及的核心文件：

* `modules/gdscript/gdscript_parser.{h,cpp}`：`LambdaNode`、`parse_lambda`
* `modules/gdscript/gdscript_analyzer.{h,cpp}`：`reduce_lambda`、
  `mark_lambda_use_self`、`resolve_pending_lambda_bodies`
* `modules/gdscript/gdscript_compiler.cpp`：`AWAIT/LAMBDA` 节点编译
* `modules/gdscript/gdscript_byte_codegen.cpp`：`write_lambda`
* `modules/gdscript/gdscript_vm.cpp`：`OPCODE_CREATE_LAMBDA` /
  `OPCODE_CREATE_SELF_LAMBDA`
* `modules/gdscript/gdscript_lambda_callable.{h,cpp}`：本章主角
* `modules/gdscript/gdscript.cpp`：`GDScript::UpdatableFuncPtr`

---

## 13.1 lambda 在 Godot 类型体系中的位置

GDScript 把 lambda 编译成 Godot 的 `Callable`。`Callable` 自己有两种
形态：

* **静态形态**：`Callable(Object*, StringName)`——绑到一个对象的某个
  名字方法。
* **自定义形态**：`Callable(CallableCustom*)`——把任何继承自
  `CallableCustom` 的子类塞进去，由它自己实现 `call()`。

GDScript lambda 走的是第二种，提供两个 `CallableCustom` 实现：

| 类 | 用途 | 持有 |
| --- | --- | --- |
| `GDScriptLambdaCallable` | 不使用 self 的 lambda | `Ref<GDScript>` + `GDScriptFunction*` + 捕获 |
| `GDScriptLambdaSelfCallable` | 使用 self 的 lambda | self（`Ref<RefCounted>` 或裸 `Object*`）+ `GDScriptFunction*` + 捕获 |

为什么要拆两个类？因为两者对**对象生命周期**的处理截然不同——这是本章
最核心的设计点之一。

---

## 13.2 Parser 端：`LambdaNode` 与作用域上下文

### 13.2.1 `LambdaNode` 结构

```cpp
struct LambdaNode : public ExpressionNode {
    FunctionNode *function = nullptr;          // 函数体的 AST
    LambdaNode *parent_lambda = nullptr;       // 嵌套外层
    Vector<IdentifierNode *> captures;         // 捕获列表（IdentifierNode）
    bool use_self = false;                     // 由 Analyzer 填充
    LambdaNode() { type = LAMBDA; }
};
```

注意 `captures` 在 Parser 阶段是空的——Parser 只知道“这是一个 lambda”，
不知道它捕获了什么。`captures` 的填充发生在 Analyzer 阶段（13.3）。

### 13.2.2 `parse_lambda`

`parse_lambda` 注册在 Pratt 表里 `func` 关键字的前缀位置：

```cpp
{ &GDScriptParser::parse_lambda, nullptr, PREC_NONE },  // FUNC
```

也就是说，**`func(x): return x` 与 `func foo(x): return x` 在 Parser 看来
是同一个起点**——都是 `func` 关键字。Parser 通过当前是否在表达式位置来
区分：

* 顶层 `func` → `parse_function` 走声明路径，写入 `current_class->members`；
* 表达式中的 `func` → `parse_lambda` 走表达式路径，返回一个 `LambdaNode`。

`parse_lambda` 内做了几件特殊处理：

```cpp
function->is_static = current_function != nullptr ?
                      current_function->is_static : false;
// ...
push_multiline(false);
if (multiline_context) {
    tokenizer->push_expression_indented_block();
}
```

* **静态继承**：lambda 自己没有 `static` 关键字，但它必须沿用宿主函数
  的 `is_static`，否则在静态函数里写 lambda 就会拿到一个非法的 self。
* **缩进栈切换**：见第 2 章——表达式内的多行 lambda 必须借助
  `tokenizer->push_expression_indented_block()` 让 Tokenizer 把 lambda
  的缩进作为一段独立块计算，不要和宿主表达式所在行的缩进混淆。

```cpp
LambdaNode *previous_lambda = current_lambda;
current_lambda = lambda;
// ... 解析 body ...
current_lambda = previous_lambda;
```

`current_lambda` 是 Parser/Analyzer 共享的“当前所在 lambda”指针——它形成
一条 `parent_lambda` 链表，在分析 self 使用时会被用来回溯。

```cpp
function->source_lambda = lambda;
```

`FunctionNode` 上新增的 `source_lambda` 反向指针让编译器可以从
`FunctionNode` 找回外面那层 `LambdaNode`，进而拿到捕获列表。

---

## 13.3 Analyzer 端：捕获、self 检测、延迟 body 解析

`reduce_lambda` 看似简单，背后却包含三条复杂规则：

```cpp
void GDScriptAnalyzer::reduce_lambda(GDScriptParser::LambdaNode *p_lambda) {
    // 1. lambda 表达式的类型一定是 Callable
    GDScriptParser::DataType lambda_type;
    lambda_type.type_source = GDScriptParser::DataType::ANNOTATED_INFERRED;
    lambda_type.kind = GDScriptParser::DataType::BUILTIN;
    lambda_type.builtin_type = Variant::CALLABLE;
    p_lambda->set_datatype(lambda_type);

    if (p_lambda->function == nullptr) return;

    // 2. 立刻 resolve 签名（参数、返回类型），但不 resolve body
    GDScriptParser::LambdaNode *previous_lambda = current_lambda;
    current_lambda = p_lambda;
    resolve_function_signature(p_lambda->function, p_lambda, true);
    current_lambda = previous_lambda;

    // 3. 把 body 推迟到 resolve_pending_lambda_bodies()
    pending_body_resolution_lambdas.push_back(p_lambda);
}
```

### 13.3.1 为什么 body 要延迟解析？

在 `reduce_*` 流程中，分析器会按 AST 顺序处理表达式。当遇到一个嵌入在
赋值右侧的 lambda 时，**当前外层函数的本地变量符号表还在动态构建中**，
立刻去解析 lambda 内部对外层变量的引用容易拿到“尚未声明完毕”的版本，
甚至触发循环解析。

延迟一下，等外层函数的整个 body 都跑完 `reduce_*`，再统一处理所有
lambda 的 body，问题就消失了：

```cpp
void GDScriptAnalyzer::resolve_pending_lambda_bodies() {
    if (pending_body_resolution_lambdas.is_empty()) return;

    List<GDScriptParser::LambdaNode *> lambdas = std::move(pending_body_resolution_lambdas);
    pending_body_resolution_lambdas.clear();

    for (GDScriptParser::LambdaNode *lambda : lambdas) {
        current_lambda = lambda;
        static_context = lambda->function->is_static;

        resolve_function_body(lambda->function, true);

        // 把捕获作为额外参数前置到形参列表
        int captures_amount = lambda->captures.size();
        if (captures_amount > 0) {
            int param_count = lambda->function->parameters.size();
            lambda->function->parameters.resize(param_count + captures_amount);
            for (int i = param_count - 1; i >= 0; i--) {
                lambda->function->parameters.write[i + captures_amount]
                    = lambda->function->parameters[i];
                // ...
            }
            // 此处省略：把 captures 合成为 ParameterNode 写入前面
        }
    }
}
```

注意最后那段——**捕获被改写成 lambda 的额外参数**。也就是说，对编译器而
言，`func(x): return x + a + b`（捕获 `a`、`b`）会被视作：

```python
func(a, b, x):
    return x + a + b
```

只是这两个“前置参数”在调用时不由用户填，而是由 `LambdaCallable.call()`
从 `captures` 数组里自动塞进来。这样**lambda 函数体内不需要任何特殊的
“捕获访问”指令**——它们就是普通参数，复用所有现有的参数访问 OPCODE。

### 13.3.2 `mark_lambda_use_self`

```cpp
void GDScriptAnalyzer::mark_lambda_use_self() {
    GDScriptParser::LambdaNode *lambda = current_lambda;
    while (lambda != nullptr) {
        lambda->use_self = true;
        lambda = lambda->parent_lambda;
    }
}
```

每当 Analyzer 在 lambda 体内发现 `self`、`super`、隐式成员引用、隐式方法
调用等需要实例的操作时就调用这个函数。它做两件事：

* 把当前 lambda 标记为 `use_self = true`；
* **同时把所有外层 lambda 也标记**——这是因为内层 lambda 想用 self，
  外层 lambda 必须先“传递”self 才能让内层拿到。

`use_self` 的最终用途是让编译器决定发 `OPCODE_CREATE_LAMBDA` 还是
`OPCODE_CREATE_SELF_LAMBDA`，进而决定运行期构造哪一种 `CallableCustom`。

`mark_lambda_use_self` 在分析器里被调用的位置（节选）：

```cpp
void GDScriptAnalyzer::reduce_self(...)        { mark_lambda_use_self(); }
// 隐式 self.method() / self.member 的若干 case
// super 调用、@onready 节点引用 等
```

可以看到，**任何隐式或显式触及 self 的位置都会触发标记**。这是 GDScript
设计的保守策略：宁可多走一次 SelfCallable 路径，也不能让 lambda 在
无 self 状态下错误地访问成员。

---

## 13.4 Compiler 端：捕获的求值与 `write_lambda`

回到 `GDScriptCompiler::_parse_expression` 的 LAMBDA 分支：

```cpp
case Node::LAMBDA: {
    const LambdaNode *lambda = static_cast<const LambdaNode *>(p_expression);
    Address result = codegen.add_temporary(_gdtype_from_datatype(...));

    // 1. 在外层作用域里求值每一个捕获表达式
    Vector<Address> captures;
    captures.resize(lambda->captures.size());
    for (int i = 0; i < lambda->captures.size(); i++) {
        captures.write[i] = _parse_expression(codegen, r_error, lambda->captures[i]);
        if (r_error) return Address();
    }

    // 2. 递归编译 lambda 函数体（p_for_lambda = true）
    GDScriptFunction *function = _parse_function(
        r_error, codegen.script, codegen.class_node,
        lambda->function, false, true);

    // 3. 记录 lambda_info（供热重载比对使用）
    codegen.script->lambda_info.insert(function,
        { (int)lambda->captures.size(), lambda->use_self });

    // 4. 写出 OPCODE
    gen->write_lambda(result, function, captures, lambda->use_self);

    // 5. 释放捕获用的临时槽
    for (int i = 0; i < captures.size(); i++) {
        if (captures[i].mode == Address::TEMPORARY) gen->pop_temporary();
    }

    return result;
}
```

### 13.4.1 捕获是“按值快照”

`_parse_expression(captures[i])` 是普通表达式求值——结果落在某个 Variant
槽里。后面 `write_lambda` 会把这些槽地址写入指令，运行时
`OPCODE_CREATE_LAMBDA` 把 Variant **拷贝**到 `captures` 向量。

也就是说：

```python
var x = 10
var f = func(): return x
x = 20
print(f.call())   # 输出 10，不是 20
```

这是和 Python 闭包不同的行为——Python 捕获的是 cell 引用，GDScript 捕获
的是值快照。这一选择避免了“让局部变量在函数返回后还活着”的复杂逃逸分析，
**完全与 GDScript 栈帧 alloca 化的策略契合**。

> **特例**：如果捕获的是 `Object` 引用、`Array`、`Dictionary` 等共享
> 类型，那么“按值”指的也只是那个引用本身——通过 lambda 修改容器内容
> 仍然可以影响外层。这是 Variant 共享语义的自然结果，并非 lambda 特有
> 行为。

### 13.4.2 `_parse_function(... p_for_lambda = true)`

`_parse_function` 是普通函数编译入口，多带一个 `p_for_lambda` 标志做
两件细微调整：

* 不向 `script->member_functions` 里注册（lambda 没有名字暴露给类）；
* 把生成的 `GDScriptFunction*` 加入到 `script->lambdas` 数组里——这就是
  第 9 章 `_lambdas_ptr[lambda_index]` 的来源。

### 13.4.3 `write_lambda`：一条 OPCODE

```cpp
void GDScriptByteCodeGenerator::write_lambda(
        const Address &p_target, GDScriptFunction *p_function,
        const Vector<Address> &p_captures, bool p_use_self) {
    append_opcode_and_argcount(
        p_use_self ? OPCODE_CREATE_SELF_LAMBDA : OPCODE_CREATE_LAMBDA,
        1 + p_captures.size());
    for (int i = 0; i < p_captures.size(); i++) append(p_captures[i]);
    CallTarget ct = get_call_target(p_target);
    append(ct.target);
    append(p_captures.size());
    append(p_function);          // 写入 lambda_index（见下）
    ct.cleanup();
}
```

`append(GDScriptFunction*)` 内部会通过 `lambdas_map` 把指针映射成索引：

```cpp
int get_lambda_function_pos(GDScriptFunction *p_lambda_function) {
    if (lambdas_map.has(p_lambda_function)) return lambdas_map[p_lambda_function];
    int pos = lambdas.size();
    lambdas.push_back(p_lambda_function);
    lambdas_map[p_lambda_function] = pos;
    return pos;
}
```

最终字节码长这样：

```
CREATE_LAMBDA / CREATE_SELF_LAMBDA
  <instr_arg_count = 1 + N>
  [capture_0] [capture_1] ... [capture_{N-1}] [target]
  <captures_count = N>
  <lambda_index>
```

---

## 13.5 VM 端：构造 `Callable`

`OPCODE_CREATE_LAMBDA` 的实现已经在第 11 章贴过，这里再贴一次并加注释：

```cpp
OPCODE(OPCODE_CREATE_LAMBDA) {
    LOAD_INSTRUCTION_ARGS                         // 把 N+1 个地址展平到 instruction_args
    CHECK_SPACE(2 + instr_arg_count);

    ip += instr_arg_count;
    int captures_count = _code_ptr[ip + 1];
    int lambda_index   = _code_ptr[ip + 2];
    GDScriptFunction *lambda = _lambdas_ptr[lambda_index];

    // 把捕获从 Variant* 拷贝成 Variant
    Vector<Variant> captures;
    captures.resize(captures_count);
    for (int i = 0; i < captures_count; i++) {
        GET_INSTRUCTION_ARG(arg, i);
        captures.write[i] = *arg;
    }

    // 构造 CallableCustom 并包成 Callable Variant
    GDScriptLambdaCallable *callable = memnew(
        GDScriptLambdaCallable(Ref<GDScript>(script), lambda, captures));

    GET_INSTRUCTION_ARG(result, captures_count);
    *result = Callable(callable);

    ip += 3;
}
DISPATCH_OPCODE;
```

`OPCODE_CREATE_SELF_LAMBDA` 多了一个分支：

```cpp
GDScriptLambdaSelfCallable *callable;
if (Object::cast_to<RefCounted>(p_instance->owner)) {
    callable = memnew(GDScriptLambdaSelfCallable(
        Ref<RefCounted>(Object::cast_to<RefCounted>(p_instance->owner)),
        lambda, captures));
} else {
    callable = memnew(GDScriptLambdaSelfCallable(
        p_instance->owner, lambda, captures));
}
```

* **如果 self 是 `RefCounted`**：用 `Ref<RefCounted>` 持有，引用计数 +1。
* **如果 self 是普通 `Object`（如 `Node`）**：只持有裸指针，**不增加
  引用计数**——因为 Node 没有引用计数，强行持有反而会导致循环引用
  无法释放。

这种二选一的策略正是“lambda 不会让宿主 Node 僵尸化”的实现。代价是：
当 Node 被 `queue_free` 后，lambda 持有的裸指针会失效。Godot 通过
`get_validated_object_with_check` + `was_freed` 检查在 lambda 内部
处理这种情况，下面会展开。

---

## 13.6 `GDScriptLambdaCallable::call`：参数前置 + 错误偏移

`call()` 的核心逻辑：

```cpp
void GDScriptLambdaCallable::call(
        const Variant **p_arguments, int p_argcount,
        Variant &r_return_value, Callable::CallError &r_call_error) const {
    int captures_amount = captures.size();

    if (function == nullptr) {                    // 见 13.7：热重载失效
        r_return_value = Variant();
        r_call_error.error = CALL_ERROR_INSTANCE_IS_NULL;
        return;
    }

    if (captures_amount > 0) {
        // 1. alloca 出一个新的 argv
        const int total_argcount = p_argcount + captures_amount;
        const Variant **args = (const Variant **)alloca(
            sizeof(Variant *) * total_argcount);

        // 2. 前置捕获参数
        for (int i = 0; i < captures_amount; i++) {
            args[i] = &captures[i];
            // 检查捕获的 Object 是否被释放
            if (captures[i].get_type() == Variant::OBJECT) {
                bool was_freed = false;
                captures[i].get_validated_object_with_check(was_freed);
                if (was_freed) {
                    ERR_PRINT(...);
                    static Variant nil;
                    args[i] = &nil;
                }
            }
        }

        // 3. 拼接用户传入的参数
        for (int i = 0; i < p_argcount; i++) {
            args[i + captures_amount] = p_arguments[i];
        }

        // 4. 调用 GDScriptFunction
        r_return_value = function->call(nullptr, args, total_argcount, r_call_error);

        // 5. 把错误中的索引/期望值"减去"捕获数，让用户看到的位置对得上
        switch (r_call_error.error) {
            case CALL_ERROR_INVALID_ARGUMENT:
                r_call_error.argument -= captures_amount;
                break;
            case CALL_ERROR_TOO_MANY_ARGUMENTS:
            case CALL_ERROR_TOO_FEW_ARGUMENTS:
                r_call_error.expected -= captures_amount;
                break;
            default: break;
        }
    } else {
        r_return_value = function->call(nullptr, p_arguments, p_argcount, r_call_error);
    }
}
```

四个要点：

1. **alloca 拼装 argv**：避免堆分配，符合 lambda 调用是热路径的假设。
2. **被释放对象的兜底**：对每一个 Object 类型捕获，运行期都做一次
   `was_freed` 检查；若已被释放，**用 NIL 替代**并打印错误。这避免了
   Node 在 `queue_free` 后被 lambda 误用造成段错误。
3. **错误索引偏移**：用户看到的“第 0 个参数错误”应该是用户的第 0 个，
   而不是“包含捕获在内的第 0 个”。call 结束后做一次反偏移修正。
4. **传 `nullptr` 作为 instance**：lambda 不属于任何实例，`function->call`
   会走静态函数路径——所有 `self` / 成员访问要么走 `SelfCallable`（带
   self），要么是编译期错误。

---

## 13.7 `GDScriptLambdaSelfCallable::call`：实例校验

`SelfCallable::call` 与上面几乎一样，只多了两件事：

```cpp
#ifdef DEBUG_ENABLED
if (object->get_script_instance() == nullptr ||
    object->get_script_instance()->get_language() != GDScriptLanguage::get_singleton()) {
    ERR_PRINT("Trying to call a lambda with an invalid instance.");
    r_call_error.error = CALL_ERROR_INSTANCE_IS_NULL;
    return;
}
#endif
// ...
r_return_value = function->call(
    static_cast<GDScriptInstance *>(object->get_script_instance()),
    args, total_argcount, r_call_error);
```

* **DEBUG 校验**：宿主对象是否仍然挂着 GDScript 实例——若某种重载操作
  把脚本换成了非 GDScript（极少见），lambda 直接拒绝执行。
* **传入 `GDScriptInstance*`**：让 `function->call` 在序言阶段把
  `ADDR_STACK_SELF` 设为 `p_instance->owner`，使得 lambda 体内 `self`
  是合法的。

这一步把“self 持有”的责任和“self 调用”的责任彻底解耦：
`SelfCallable` 负责保活/校验，`GDScriptFunction::call` 负责使用。

---

## 13.8 热重载与 `UpdatableFuncPtr`

这是本章最容易忽视、却最体现 GDScript 工程考虑的细节。

### 13.8.1 问题

考虑一个长存场景：

```python
extends Node
var f
func _ready() -> void:
    f = func(): print("hi")
    get_tree().create_timer(1.0).timeout.connect(f)
```

定时器一秒后触发 → 调用 `f` → 调用 lambda 的 `GDScriptFunction*`。

但**如果用户在这一秒内热重载了脚本**（编辑器 reload），原来的
`GDScriptFunction*` 会被销毁，新的 `GDScriptFunction*` 出现。`f` 这个
`Callable` 仍然挂在场景树上，里面持有的旧指针就成了野指针——再调用一
次就崩。

### 13.8.2 `UpdatableFuncPtr`：可被替换的函数指针

GDScript 用一个小巧的 RAII 结构解决这个问题：

```cpp
// gdscript.h
class UpdatableFuncPtr {
    friend class GDScript;
    GDScriptFunction *ptr = nullptr;
    GDScript *script = nullptr;
    List<UpdatableFuncPtr *>::Element *list_element = nullptr;

public:
    UpdatableFuncPtr(GDScriptFunction *p_function);
    ~UpdatableFuncPtr();
    GDScriptFunction *operator->() const { return ptr; }
    operator bool() const { return ptr != nullptr; }
    operator GDScriptFunction *() const { return ptr; }
};
```

构造时把自己挂到 `script->func_ptrs_to_update` 链表上：

```cpp
UpdatableFuncPtr::UpdatableFuncPtr(GDScriptFunction *p_function) {
    if (p_function == nullptr) return;
    ptr = p_function;
    script = ptr->get_script();
    MutexLock script_lock(script->func_ptrs_to_update_mutex);
    list_element = script->func_ptrs_to_update.push_back(this);
}
```

析构时从链表上摘掉。

`GDScriptLambdaCallable` 的 `function` 字段就是 `UpdatableFuncPtr` 类型
（不是裸 `GDScriptFunction*`），所以**每一个 lambda Callable 都被自动
注册到了它所属脚本的“可更新指针列表”里**。

### 13.8.3 重载时遍历替换

热重载流程会走到：

```cpp
void GDScript::_recurse_replace_function_ptrs(
        const HashMap<GDScriptFunction *, GDScriptFunction *> &p_replacements) const {
    MutexLock lock(func_ptrs_to_update_mutex);
    for (UpdatableFuncPtr *updatable : func_ptrs_to_update) {
        auto replacement = p_replacements.find(updatable->ptr);
        if (replacement) {
            updatable->ptr = replacement->value;       // 替换为新指针
        } else {
            // 旧 lambda 在新版本里不存在了，置 nullptr
            updatable->ptr = nullptr;
        }
    }
    // 递归到内部类
    for (auto subscript : subclasses) {
        subscript->value->_recurse_replace_function_ptrs(p_replacements);
    }
}
```

这就解释了为什么 `LambdaCallable::call` 里要先判断 `function == nullptr`——
它代表“热重载后这个 lambda 已经不存在了”。VM 会优雅地返回
`CALL_ERROR_INSTANCE_IS_NULL` 而不是崩溃。

`p_replacements` 这张表来自第 6 章 `GDScriptCompiler::_get_function_ptr_replacements`，
它配合 `lambda_info`（编译时记录的 capture_count 与 use_self）来比较新旧
lambda 是否“同身份”：

```cpp
bool GDScriptCompiler::_do_function_infos_match(
        const FunctionLambdaInfo &p_old_info, const FunctionLambdaInfo *p_new_info) {
    if (p_new_info->capture_count != p_old_info.capture_count ||
        p_new_info->use_self != p_old_info.use_self) {
        return false;
    }
    // ... 还会比对 name、line、index 等 ...
}
```

只有 capture 数量、use_self 标志、源码位置都一致的 lambda 才会被
认作“同一个函数的新版本”。

### 13.8.4 `clear()`：脚本被销毁时

如果整个 GDScript 资源被销毁：

```cpp
void GDScript::clear() {
    // ...
    {
        MutexLock lock(func_ptrs_to_update_mutex);
        for (UpdatableFuncPtr *updatable : func_ptrs_to_update) {
            updatable->ptr = nullptr;     // 全部置空
        }
    }
    // ... 其余清理
}
```

所有挂在外的 lambda Callable 都会“静默失效”——下次调用时返回错误而
不是崩溃。这同样是 13.4 中 `function == nullptr` 检查的另一种触发路径。

---

## 13.9 等价 / 哈希：lambda 永远只与自己相等

```cpp
bool GDScriptLambdaCallable::compare_equal(
        const CallableCustom *p_a, const CallableCustom *p_b) {
    return p_a == p_b;     // 引用比较
}

uint32_t GDScriptLambdaCallable::hash() const {
    return h;              // 构造时设为 hash_murmur3_one_64((uint64_t)this)
}
```

* **每次 `OPCODE_CREATE_LAMBDA` 都生成一个新对象**，地址不同 → hash 不同
  → 不相等。
* 这是有意为之：lambda 的 capture 是 Variant 值，写一个内容相等性比较
  没有意义（捕获里可能有 Object 引用、Array 等共享类型）；用引用相等
  足够支撑“信号断开”等典型场景——用户保留 Callable 引用即可断开它。

负面后果：

```python
var f1 = func(): return 1
var f2 = func(): return 1
f1 == f2  # false，即使语义相同
```

但这个语义和 Python、JS 等绝大多数语言的 lambda 一致，不会让用户惊讶。

---

## 13.10 设计回顾

把所有线索拎到一起，GDScript lambda 的设计可以总结为四条原则：

1. **lambda = 普通函数 + 前置参数**——通过把捕获改写成额外参数，函数体
   不需要新指令，复用全部现有 OPCODE。
2. **按值快照捕获**——避开闭包逃逸分析，与 alloca 栈帧策略契合。
3. **`use_self` 二选一**——分别用 `LambdaCallable` 与 `SelfCallable`
   处理“纯捕获”与“持有 self”两种场景，保证 Node 不会被 lambda 持续
   保活。
4. **`UpdatableFuncPtr` 与热重载协议**——所有 lambda Callable 自动注册
   到脚本的更新链表，重载时统一替换或失效，确保已逃逸到外部世界的
   Callable 永远安全。

第 1 条让 lambda 在指令集层面几乎透明；第 2 条让运行时极简；
第 3 条让生命周期符合 Godot 的对象模型；第 4 条让长生命周期场景下的
开发体验得以保留。

---

## 小结

* GDScript lambda 在 Parser 阶段是 `LambdaNode`，在 Analyzer 阶段先 reduce
  签名再延迟 reduce body；
* 捕获最终被改写成 lambda 函数的“前置参数”，运行时 `LambdaCallable`
  通过 alloca + 错误偏移把捕获和用户参数合并；
* `use_self` 标志由 `mark_lambda_use_self` 沿外层链向上传播；
* 编译期生成 `OPCODE_CREATE_LAMBDA` 或 `OPCODE_CREATE_SELF_LAMBDA`，
  VM 据此构造 `GDScriptLambdaCallable` 或 `GDScriptLambdaSelfCallable`；
* 通过 `UpdatableFuncPtr` 链表，所有 lambda Callable 在脚本热重载时
  会被同步替换或安全失效，避免野指针；
* lambda 的等价性是引用相等，与主流脚本语言保持一致。

下一章我们来看另一种特殊 Callable——`GDScriptRPCCallable`，它把
GDScript 函数与 Godot 网络层无缝衔接。
