# 第 12 章　`await`、信号与协程

GDScript 的协程系统在用户层面看起来非常朴素——只多了一个 `await` 关键字
就能把一段同步代码变成可中断的异步代码。但在实现层面，它牵动了
**Tokenizer / Parser / Analyzer / Compiler / VM / Signal / Callable** 六
大模块。本章把这条贯穿前后端的脉络一次性串起来。

读完后你会明白：

* GDScript 的 `await` 为什么**不需要专门的协程栈**，也不需要操作系统级
  线程或纤程？
* `func _ready() -> void: await get_tree().process_frame` 这样一句
  用户代码，在编译期和运行期分别经历了什么？
* `GDScriptFunctionState` 这个对象到底**装着什么**，又是被谁拽着活到
  下一帧的？
* `REDUNDANT_AWAIT` / `MISSING_AWAIT` 两条警告是怎么由分析器生成的？

---

## 12.1 GDScript 协程的设计哲学

协程在脚本语言里有多种实现路线：

| 风格 | 代表 | 特点 |
| --- | --- | --- |
| **栈协程** | Lua coroutine | 给每个协程开一段独立栈，`yield/resume` 切栈 |
| **生成器函数** | Python generator | `yield` 把函数变成一个迭代器对象 |
| **CPS 变换** | Scheme call/cc、JS async/await（V8） | 编译期把函数切成多个连续段 |
| **状态机** | C# async/await | 编译器把函数改写成有限状态机 |
| **挂起点 + 信号回调** | **GDScript** | 把每个 `await` 编成“退出 → 信号回调 → 重入”的对称操作 |

GDScript 选择了最后一种：**“挂起点”同时是“退出点”和“重入点”**。这种
做法的关键在于**栈是临时的**——主循环用 `alloca` 分配栈帧，一旦 `await`
要挂起，就把栈整体复制到堆上的 `Vector<uint8_t>` 里；恢复时再复制回
另一段 alloca 栈帧。这样整个协程系统：

* 不需要专门的协程栈或纤程；
* 不需要编译期把函数切成片段；
* 复用了 Godot 已有的 `Signal` / `Callable` 机制；
* 与 RefCounted 引用计数自然契合，不存在协程对象“生死不明”的问题。

代价是每次 `await` 都要复制一份栈，但 GDScript 的栈通常很小（几十个
Variant），且 `await` 在游戏脚本里不是热路径——这是一种非常划算的取舍。

---

## 12.2 Tokenizer 端：`await` 是关键字

`gdscript_tokenizer.cpp` 中：

```cpp
KEYWORD("await", Token::AWAIT)
```

`await` 在词法层面就是一个普通关键字，对 INDENT/DEDENT 没有任何特殊
影响。它和 `if`、`for` 在 Tokenizer 眼中是平等的。

---

## 12.3 Parser 端：`await` 是一元前缀表达式

在 `gdscript_parser.h` 中，`await` 被赋予一个独立的优先级：

```cpp
enum Precedence {
    // ...
    PREC_AWAIT,
    // ...
};
```

而在 Pratt 表的 `parse_rules` 中：

```cpp
{ &GDScriptParser::parse_await, nullptr, PREC_NONE },  // AWAIT
```

也就是说：

* `await` 只能用作**前缀**（`prefix`），不能出现在中缀位置；
* 它所约束的子表达式优先级是 `PREC_AWAIT`，比一般运算高，比成员访问与
  调用低——这意味着 `await foo.bar()` 解析为 `await (foo.bar())`，
  而不是 `(await foo).bar()`。

`AwaitNode` 的结构非常简单：

```cpp
class AwaitNode : public ExpressionNode {
public:
    ExpressionNode *to_await = nullptr;
    // ...
    AwaitNode() { type = AWAIT; }
};
```

它只有一个孩子节点 `to_await`，表示“等待什么”。

---

## 12.4 Analyzer 端：类型推断与两条警告

`GDScriptAnalyzer::reduce_await` 是协程语义检查的核心。它做三件事：

```cpp
void GDScriptAnalyzer::reduce_await(GDScriptParser::AwaitNode *p_await) {
    if (p_await->to_await == nullptr) {
        // 容错：解析失败
        await_type.kind = DataType::VARIANT;
        p_await->set_datatype(await_type);
        return;
    }

    // 1. 子表达式按"调用允许返回 coroutine"的方式 reduce
    if (p_await->to_await->type == Node::CALL) {
        reduce_call(static_cast<CallNode *>(p_await->to_await), true);
    } else {
        reduce_expression(p_await->to_await);
    }

    // 2. 推断 await 表达式自身的类型
    DataType await_type = p_await->to_await->get_datatype();
    if (await_type.is_hard_type() &&
        await_type.kind == DataType::BUILTIN &&
        await_type.builtin_type == Variant::SIGNAL) {
        // 等待信号：返回值不可推断
        await_type.kind = DataType::VARIANT;
        await_type.type_source = DataType::UNDETECTED;
    } else if (p_await->to_await->is_constant) {
        // 等待常量：直接传递常量
        p_await->is_constant = p_await->to_await->is_constant;
        p_await->reduced_value = p_await->to_await->reduced_value;
    }
    await_type.is_coroutine = false;
    p_await->set_datatype(await_type);

    // 3. 警告：等待一个非协程、非信号、非 Variant 的表达式
#ifdef DEBUG_ENABLED
    DataType to_await_type = p_await->to_await->get_datatype();
    if (!to_await_type.is_coroutine &&
        !to_await_type.is_variant() &&
        to_await_type.builtin_type != Variant::SIGNAL) {
        parser->push_warning(p_await, GDScriptWarning::REDUNDANT_AWAIT);
    }
#endif
}
```

### 12.4.1 类型推断规则

* **等待 Signal**：返回值类型未知（信号回调可以带任意参数），降为
  `VARIANT`；
* **等待协程函数（带 `is_coroutine` 标记的调用）**：使用函数声明的
  返回类型，但去掉 `is_coroutine` 标记——`await` 之后拿到的就是“真正
  的返回值”而不是协程对象本身；
* **等待普通同步表达式**：透传类型；如果是常量则进一步把常量也传过来。

### 12.4.2 `is_coroutine` 标记的来源

`is_coroutine` 是 `DataType` 上的一个布尔字段，在分析函数体时设置：
**只要函数体里出现过 `await`**，那个函数返回类型上的 `is_coroutine`
就是 true。它通过 `FunctionNode::is_coroutine` 在编译期被传递。

### 12.4.3 两条专有警告

* **`REDUNDANT_AWAIT`**：用在“无意义的 await”，例如
  `await some_int_function()`。运行期不会出错，但会产生一个
  `OPCODE_AWAIT` 同步路径的额外开销，更重要的是**通常意味着写错了
  代码**。
* **`MISSING_AWAIT`**：当用户**没写** `await`，却调用了一个 coroutine
  函数时触发：

```cpp
// 在 reduce_call 里
if (returned_type.is_coroutine && !called_with_await) {
    parser->push_warning(p_call, GDScriptWarning::MISSING_AWAIT);
}
```

这一条几乎是 GDScript 用户最常见的脚本错误来源——忘了写 `await` 会让
返回的 `GDScriptFunctionState` 被丢弃，整个异步链断裂。

---

## 12.5 Compiler 端：两个生成模式

`GDScriptCompiler` 在两个位置处理 await 相关逻辑。

### 12.5.1 `awaited_node` 标志

```cpp
GDScriptParser::ExpressionNode *awaited_node = nullptr;  // gdscript_compiler.h
```

这个字段在 `_parse_expression` 处理 `AWAIT` 节点时被临时设上：

```cpp
case Node::AWAIT: {
    ...
    GDScriptParser::ExpressionNode *previous_awaited_node = awaited_node;
    awaited_node = await->to_await;            // 标记"我是被 await 的子表达式"
    Address argument = _parse_expression(codegen, r_error, await->to_await);
    awaited_node = previous_awaited_node;
    ...
    gen->write_await(result, argument);
    ...
}
```

它是一种“对底下一层子表达式的提示”：当 `_parse_expression` 在解析
**调用表达式**时，会检查 `is_awaited = (p_expression == awaited_node)`：

```cpp
bool is_awaited = p_expression == awaited_node;
// ...
if (is_call_to_method_with_self) {
    if (is_awaited) {
        gen->write_call_async(result, self, call->function_name, arguments);
    } else {
        gen->write_call_self(result, call->function_name, arguments);
    }
}
```

也就是说，**生成器会区分 `await foo()` 与 `foo()`**：前者发
`OPCODE_CALL_ASYNC`，后者发 `OPCODE_CALL`。这两个 OPCODE 在主循环里
共享实现，但 `CALL_ASYNC` 会在 DEBUG 下抑制“返回值是
GDScriptFunctionState 但没 await”的诊断。

### 12.5.2 `write_await`：两条 OPCODE 的成对发出

`GDScriptByteCodeGenerator::write_await` 极简：

```cpp
void GDScriptByteCodeGenerator::write_await(
        const Address &p_target, const Address &p_operand) {
    append_opcode(OPCODE_AWAIT);
    append(p_operand);            // 等待的对象（信号或非信号）
    append_opcode(OPCODE_AWAIT_RESUME);
    append(p_target);             // 恢复后存放结果的目标地址
}
```

最终字节码长这样：

```
ip+0:  AWAIT
ip+1:  [operand 24-bit address]
ip+2:  AWAIT_RESUME
ip+3:  [target 24-bit address]
```

`AWAIT` 与 `AWAIT_RESUME` **物理相邻**——这是 VM 同步路径里
`ip += 4` 跳过 `AWAIT_RESUME` 的依据。

---

## 12.6 VM 端：同步路径 vs 异步路径

`OPCODE_AWAIT` 的实现（精简版）：

```cpp
OPCODE(OPCODE_AWAIT) {
    GET_VARIANT_PTR(argobj, 0);

    Signal sig;
    bool is_signal = true;
    {
        Variant result = *argobj;

        // 1. 如果是 Object 且是 GDScriptFunctionState，转成它的 completed 信号
        if (argobj->get_type() == Variant::OBJECT) {
            Object *obj = argobj->get_validated_object_with_check(was_freed);
            if (obj && obj->is_class_ptr(GDScriptFunctionState::get_class_ptr_static())) {
                result = Signal(obj, SNAME("completed"));
            }
        }

        // 2. 如果不是信号，走同步路径
        if (result.get_type() != Variant::SIGNAL) {
            GET_VARIANT_PTR(target, 2);   // 直接读 AWAIT_RESUME 后的目标地址
            *target = result;
            ip += 4;                       // 跳过 AWAIT_RESUME
            is_signal = false;
        } else {
            sig = result;
        }
    }

    if (is_signal) {
        // 3. 异步路径：保存栈、连接信号、退出主循环
        Ref<GDScriptFunctionState> gdfs = memnew(GDScriptFunctionState);
        gdfs->function = this;
        gdfs->state.stack.resize(alloca_size);

        // 复制栈（跳过 SELF/CLASS/NIL 三个固定槽）
        for (int i = FIXED_ADDRESSES_MAX; i < _stack_size; i++) {
            memnew_placement(&gdfs->state.stack.write[sizeof(Variant) * i],
                             Variant(stack[i]));
        }
        gdfs->state.stack_size = _stack_size;
        gdfs->state.ip = ip + 2;           // 让 resume 从 AWAIT_RESUME 继续
        gdfs->state.line = line;
        gdfs->state.script = _script;
        gdfs->state.instance = p_instance;
        gdfs->state.defarg = defarg;

        // 注册到 script / instance 的 pending 列表
        _script->pending_func_states.add(&gdfs->scripts_list);
        if (p_instance) p_instance->pending_func_states.add(&gdfs->instances_list);

        // 决定 completed 信号
        if (p_state) {
            gdfs->state.completed = p_state->completed;   // 嵌套 await：传递信号
        } else {
            gdfs->state.completed = Signal(gdfs.ptr(), SNAME("completed"));
        }

        retvalue = gdfs;

        // 一次性连接：信号触发后调用 _signal_callback
        Error err = sig.connect(
            Callable(gdfs.ptr(), "_signal_callback").bind(retvalue),
            Object::CONNECT_ONE_SHOT);

        awaited = true;
        OPCODE_BREAK;          // 退出主循环
    }
}
DISPATCH_OPCODE;

OPCODE(OPCODE_AWAIT_RESUME) {
    GET_VARIANT_PTR(result, 0);
    *result = p_state->result;
    ip += 2;
}
DISPATCH_OPCODE;
```

### 12.6.1 三条路径并存

VM 里这条 `OPCODE_AWAIT` 实际上**编码了三种执行路径**：

| 路径 | 触发条件 | 行为 |
| --- | --- | --- |
| 同步直通 | `argobj` 不是 Signal、不是 GDScriptFunctionState | 直接读 `AWAIT_RESUME` 的目标槽，`ip += 4` 继续 |
| 跨函数 await | `argobj` 是 GDScriptFunctionState | 把它转换为 `Signal(state, "completed")`，再走异步路径 |
| 信号 await | `argobj` 是 Signal | 创建 `GDScriptFunctionState`、序列化栈、连接信号、退出 |

> 注意“同步直通”与第 11 章的 REDUNDANT_AWAIT 警告是一致的——分析器
> 提示用户“你这里的 await 没有意义”，VM 层面也确实只是多一次 4 字段
> 跳过的开销，不会出错。

### 12.6.2 GDScriptFunctionState 的栈快照

把栈复制到 `state.stack` 用了一招技巧：

```cpp
gdfs->state.stack.resize(alloca_size);   // 注意是 alloca_size，不是 _stack_size
```

`alloca_size` 包含了原始栈的 **Variant 段 + instruction_args 指针段**
两块。但实际复制的只有 `[FIXED_ADDRESSES_MAX, _stack_size)` 范围里的
Variant；`instruction_args` 那段并不复制——因为恢复时它会被
`call(p_state)` 路径重新指向 `state.stack` 里同一个偏移：

```cpp
instruction_args = (Variant **)&p_state->stack.ptr()[
                       sizeof(Variant) * p_state->stack_size];
```

也就是说：**`instruction_args` 的内存位置在 `state.stack` 内是固定的**，
但它存的指针在恢复时不再有意义。这没关系，因为
`OPCODE_AWAIT_RESUME` 之后的下一条指令一定是新的 `LOAD_INSTRUCTION_ARGS`，
会重新填表。

固定地址 `SELF/CLASS/NIL` 也不复制——`call(p_state)` 入口处会重新构造
它们：

```cpp
if (p_instance) {
    memnew_placement(&stack[ADDR_STACK_SELF], Variant(p_instance->owner));
}
memnew_placement(&stack[ADDR_STACK_CLASS], Variant);
memnew_placement(&stack[ADDR_STACK_NIL], Variant);
```

这里再次体现了**“固定地址 = 函数入口的环境”** 的设计——它们不属于
“算到一半被中断”的状态，而属于“环境”，每次进入函数重建即可。

### 12.6.3 `pending_func_states` 链表

```cpp
_script->pending_func_states.add(&gdfs->scripts_list);
p_instance->pending_func_states.add(&gdfs->instances_list);
```

每个挂起的 `GDScriptFunctionState` 同时被挂到**脚本**和**实例**两条
全局链表上。这是一种“弱引用”机制：

* 当 GDScript 资源被销毁/重载时，链表里的所有挂起态需要被失效；
* 当 `Object` 实例被释放时，依赖该实例的挂起态需要被失效；
* `is_valid(true)` 通过检查 `scripts_list.in_list()` 与
  `instances_list.in_list()` 来回答“恢复是否安全”。

`GDScriptFunctionState::~GDScriptFunctionState` 析构时会从两个链表里
自动摘除，这是 `SelfList` 模板的语义。

---

## 12.7 唤醒：`_signal_callback` → `resume` → `call(p_state)`

### 12.7.1 信号回调的桥梁

```cpp
Error err = sig.connect(
    Callable(gdfs.ptr(), "_signal_callback").bind(retvalue),
    Object::CONNECT_ONE_SHOT);
```

`bind(retvalue)` 把 `gdfs` 自己作为最后一个参数绑定到 Callable 上。
这是为了让 `_signal_callback` 在被调用时**通过自己的最后一个参数拿回
自己**——避免 `gdfs` 被引用计数回收。

`_signal_callback` 的实现非常巧妙地处理了“信号带 0/1/多个参数”三种
情形：

```cpp
Variant GDScriptFunctionState::_signal_callback(
        const Variant **p_args, int p_argcount,
        Callable::CallError &r_error) {
    Variant arg;
    if (p_argcount == 0) {            // 不可能，因为至少有 bind 进来的 self
        r_error.error = CALL_ERROR_TOO_FEW_ARGUMENTS;
        return Variant();
    } else if (p_argcount == 1) {     // 信号无参，await 的结果是 null
        // arg 保持为 NIL Variant
    } else if (p_argcount == 2) {     // 信号一个参数，作为 await 的结果
        arg = *p_args[0];
    } else {                          // 信号多参，打包成 Array
        Array extra_args;
        for (int i = 0; i < p_argcount - 1; i++) {
            extra_args.push_back(*p_args[i]);
        }
        arg = extra_args;
    }

    // 最后一个参数是被 bind 的 self
    Ref<GDScriptFunctionState> self = *p_args[p_argcount - 1];

    return resume(arg);
}
```

所以**`await some_signal` 的语义**根据信号参数个数分为三种：

| 信号参数 | `await` 表达式的值 |
| --- | --- |
| 0 个 | `null` |
| 1 个 | 该参数本身 |
| ≥2 个 | 一个 Array，包含所有参数 |

这是为什么 `await body.body_entered`（`body` 是 `Node`）能直接拿到
进入的 body 对象，而 `await timer.timeout`（无参数）什么也拿不到。

### 12.7.2 `resume()`：状态校验 + 重入主循环

```cpp
Variant GDScriptFunctionState::resume(const Variant &p_arg) {
    {
        MutexLock lock(GDScriptLanguage::singleton->mutex);

        if (!scripts_list.in_list()) {
            ERR_FAIL_V_MSG(Variant(), "Resumed function ... but script is gone.");
        }
        if (state.instance && !instances_list.in_list()) {
            ERR_FAIL_V_MSG(Variant(), "Resumed function ... but class instance is gone.");
        }
        scripts_list.remove_from_list();
        instances_list.remove_from_list();
    }

    state.result = p_arg;
    Callable::CallError err;
    Variant ret = function->call(nullptr, nullptr, 0, err, &state);

    function = nullptr;          // 单次性消费
    state.result = Variant();
    return ret;
}
```

要点：

1. **生死检查**：脚本/实例已销毁就放弃恢复，避免野指针。
2. **链表摘除**：在 `call` 之前完成，防止下一次 `await` 时再次添加产生
   循环。
3. **`state.result = p_arg`**：把信号回调的“一个参数”塞进 `CallState`，
   `OPCODE_AWAIT_RESUME` 会从这里读出来写到目标槽。
4. **`function->call(nullptr, nullptr, 0, err, &state)`**：第二三个参数
   故意是 nullptr 与 0——因为参数已经在原始 alloca 栈里被拷贝到
   `state.stack` 了，不需要再传。`call(p_state != nullptr)` 路径会自动
   走恢复分支。
5. **`function = nullptr`**：明示这次 state 已被消费，二次 `resume` 会
   被 `ERR_FAIL_NULL_V` 截住。

### 12.7.3 嵌套 await 的“最后一棒”机制

考虑下面场景：

```python
func a() -> void:
    print("a-1")
    await b()          # 内层 await
    print("a-2")

func b() -> void:
    await get_tree().process_frame    # 外层 await
```

执行 `a()` 时：

1. `a` 调用 `b`，`b` 立刻 `await process_frame`，VM 挂起 `b`，返回
   `gdfs_b`；
2. `a` 调用 `await gdfs_b`——VM 检测到这是 `GDScriptFunctionState`，
   把它转成 `Signal(gdfs_b, "completed")`，挂起 `a`，返回 `gdfs_a`；
3. 帧切换，`process_frame` 触发 → `gdfs_b._signal_callback` →
   `gdfs_b.resume()` → 重入 `b()` → `b` 走完 → VM 走 OPSOUT 的
   `p_state->completed.emit(retvalue)`；
4. 这条 `completed` 信号触发 `gdfs_a._signal_callback` → `a()` 恢复 →
   走完 → 同样 emit `gdfs_a.completed`（如果有更外层等着）。

VM 的代码里有这样一段：

```cpp
if (p_state) {
    // 把 completed 信号"传染"给嵌套 state
    gdfs->state.completed = p_state->completed;
} else {
    gdfs->state.completed = Signal(gdfs.ptr(), SNAME("completed"));
}
```

——这段确保整个嵌套链上**只有最外层的 state 拥有真正的 completed
信号**。其他层都共享这个信号，因为它们的“完成”也意味着外层完成。

OPSOUT 阶段的 `emit(completed)` 也有对应规则：

```cpp
if (p_state && !awaited) {
    // 已恢复并最终走完，emit 让外层继续
    p_state->completed.emit(args, 1);
}
```

> 注释说这是为了 “preserve async call stack”——如果在错误的时机 emit，
> 调试器看到的调用链就会断裂。

---

## 12.8 RefCounted 引用计数与生命周期

`GDScriptFunctionState` 继承自 `RefCounted`，意味着它的生命由 `Ref<T>`
托管。挂起期间它至少有两条引用：

1. **VM 的 retvalue**：第一次 `await` 挂起后，`retvalue = gdfs` 被传回
   调用者（也许是上层 `await`，也许是用户代码持有的变量）。
2. **Signal 的连接绑定**：`sig.connect(Callable(gdfs.ptr(), ...).bind(retvalue))`
   把 `gdfs` bind 进了 Callable，从而被 Signal 持有。

任意一条引用都足以让 `gdfs` 活下去。如果用户主动丢弃 `gdfs`（比如忘了
写 `await`），还有 Signal 那条引用——直到信号触发后断开为止。

### 唯一的“漏挂起”场景

* 信号永远不触发；
* Signal 所属 Object 被销毁，CONNECT_ONE_SHOT 没机会执行；
* 用户也丢掉了 `gdfs`；

此时 `gdfs` 引用计数归零，析构时 `_clear_stack` 释放保存的 Variant，
`_clear_connections` 把已建立的连接断开。**所有资源都正确清理**——
GDScript 的协程系统永远不会泄漏，只会“静悄悄地什么也没发生”。

---

## 12.9 与引擎主循环的衔接

GDScript 的 `await` 不和操作系统线程打交道，它的“时间流逝”完全由
**Godot 引擎主循环驱动的信号**触发。最常见的几条：

| 信号 | 用途 |
| --- | --- |
| `SceneTree.process_frame` | 等到下一帧处理 |
| `SceneTree.physics_frame` | 等到下一物理帧 |
| `Timer.timeout` | 等待计时器到期 |
| `Tween.finished` | 等待补间动画结束 |
| `HTTPRequest.request_completed` | 等待 HTTP 请求完成 |
| 任意自定义信号 | `await my_event` |

无论哪种，本质都只是 Signal——VM 不需要知道它来自定时器、IO 还是用户
事件。这种解耦让 `await` 在生态层面成为了一个统一的“事件驱动同步点”。

> **特殊情况**：`await` 一个 GDScriptFunctionState 时不会创建额外信号，
> 而是复用它自己的 `completed`，原因见 12.7.3。

---

## 12.10 一次完整 await 的端到端时间线

把前面所有内容汇成一张图，假设代码是：

```python
func _ready() -> void:
    print("before")
    var t := await create_tween().tween_property(...).finished
    print("after")
```

```
[编译期]
  ├─ Tokenizer:  AWAIT 关键字
  ├─ Parser:     AwaitNode { to_await = <member access> }
  ├─ Analyzer:
  │    ├─ 子表达式类型 = SIGNAL
  │    └─ AwaitNode 类型 = VARIANT, is_coroutine = false
  └─ Compiler:
       └─ OPCODE_AWAIT [signal_addr]
          OPCODE_AWAIT_RESUME [t_addr]

[运行期 - 第一次]
  ├─ ... 之前指令 ...
  ├─ OPCODE_AWAIT
  │    ├─ 取得 signal Variant
  │    ├─ memnew GDScriptFunctionState gdfs
  │    ├─ 复制 stack[FIXED..]_stack_size 到 gdfs.state.stack
  │    ├─ gdfs.state.ip = ip + 2  (指向 AWAIT_RESUME)
  │    ├─ gdfs.state.completed = Signal(gdfs, "completed")
  │    ├─ 注册到 script/instance 的 pending 链表
  │    ├─ sig.connect(_signal_callback.bind(gdfs))
  │    ├─ retvalue = gdfs
  │    └─ awaited = true; OPCODE_BREAK
  └─ OPSOUT
       ├─ exit_function()
       ├─ 析构原始 alloca 栈
       └─ return gdfs

[Godot 主循环]
  ├─ Tween 完成 → emit "finished"
  └─ Signal 触发 → Callable._signal_callback(...) → gdfs._signal_callback

[运行期 - 第二次]
  ├─ resume(arg)
  │    ├─ scripts/instances 链表生死检查 + 摘除
  │    ├─ state.result = arg
  │    └─ function->call(nullptr, nullptr, 0, err, &state)
  └─ call(p_state != nullptr)
       ├─ 用 state.stack 重建 stack/instruction_args
       ├─ 重新构造 SELF/CLASS/NIL
       ├─ ip = state.ip (指向 AWAIT_RESUME)
       ├─ 进入主循环
       ├─ OPCODE_AWAIT_RESUME: *t_addr = state.result; ip += 2
       ├─ ... 后续指令 ("print after") ...
       ├─ OPCODE_RETURN
       └─ OPSOUT
            ├─ p_state->completed.emit(retvalue)   // 通知更外层
            ├─ exit_function()
            └─ 析构第二段 alloca 栈
```

整个流程没有任何线程切换、没有任何额外栈，所有“暂停”都通过把栈复制
到堆上的 Vector 来表示。

---

## 12.11 设计回顾：为什么这种设计合适？

回到本章开头的对比表，可以总结 GDScript 选择“挂起点 + 信号回调”路线
的几个判断：

1. **游戏脚本的 await 频率不高**——一帧里通常只有少量挂起点，把栈
   完整复制一份的开销可以承受。
2. **栈足够小**——典型 GDScript 函数的 `_stack_size` 在十几到几十
   Variant 量级，复制不是瓶颈。
3. **复用 Signal/Callable 减少新概念**——不引入额外语言机制，所有
   异步唤醒都走 Godot 已有的事件系统。
4. **天然支持嵌套 await**——只要让嵌套层共享外层的 `completed`，外层
   感知就完整。
5. **配合 RefCounted 自动 GC**——挂起态的生命由引用计数自动管理，
   不需要单独的协程注册表。

代价是“挂起栈复制”的常数开销，以及不能像 `Lua coroutine` 那样把协程
当作通用迭代器用。但对游戏脚本场景而言，这是一笔非常合算的取舍。

---

## 小结

* GDScript 的 `await` 在语言层面只是一个一元前缀表达式，但实现上
  贯穿 Tokenizer → Parser → Analyzer → Compiler → VM → Signal 全链路；
* VM 中 `OPCODE_AWAIT` / `OPCODE_AWAIT_RESUME` 两条指令物理相邻，
  分别承载“挂起”与“恢复”两个时刻；
* 挂起态被封装到 `GDScriptFunctionState`，通过栈复制 + 信号回调实现
  “退出 → 重入”的对称操作；
* `pending_func_states` 双链表配合 RefCounted 引用计数，确保资源不
  泄漏、脚本重载/对象销毁时挂起态能正确失效；
* 嵌套 `await` 通过共享 `completed` 信号实现 async 栈的连续展开；
* 警告系统从分析器层面拦截 `REDUNDANT_AWAIT` 与 `MISSING_AWAIT`，是
  实战中最有用的两条 GDScript 警告之一。

下一章我们将聚焦另一个看似简单却同样横跨多模块的语言特性——
**Lambda** 与 `GDScriptLambdaCallable`。
