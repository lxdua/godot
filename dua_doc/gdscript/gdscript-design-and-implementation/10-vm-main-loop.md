# 第 10 章　虚拟机主循环：`GDScriptFunction::call`

如果说第 9 章介绍的是“运行期数据结构”，那么本章要讲的就是这些数据被
如何**驱动**——`GDScriptFunction::call` 是整个 GDScript 解释器跳动的
心脏，定义在 `modules/gdscript/gdscript_vm.cpp`（约 4000 行单函数）。

本章不逐条讲解一百多个 OPCODE——那已经由反汇编器（第 8 章）和第 11 章
“OPCODE 家族”覆盖。这里的目标是让你理解**分派机制**本身：

1. 一个 4000 行的单函数为什么可以在两种编译器下保持可读、可维护？
2. 主循环在启动时做了哪些“一次性”准备工作？
3. 寻址宏 `GET_VARIANT_PTR` / `GET_INSTRUCTION_ARG` 是如何把 24 位索引
   翻译成 Variant 指针的？
4. `OPCODE_OPERATOR` 这种带有“内联缓存”的自修改字节码是怎么工作的？
5. 异常、`await`、`return` 的退出路径是怎么收束到同一个出口的？

看完后，你再读任意一条 OPCODE 实现，都能立即抓住它的“上下文契约”。

---

## 10.1 为什么整段 VM 写在一个函数里？

GDScript VM 的主循环选择“单函数 + 宏分派”的写法，而不是拆成虚函数、
拆成小函数的 trampoline。原因有三：

* **热路径内联**：VM 的性能几乎完全取决于“取指 → 分派 → 执行 → 再取指”
  这个循环能否保持在指令缓存里。把它放在一个函数里让编译器得以对
  `stack` / `instruction_args` / `_code_ptr` 等变量做充分的寄存器分配，
  不会因为函数调用而被迫溢出。
* **跨平台编译器差异**：`goto *table[op]`（通常称为 computed goto）
  只存在于 GCC/Clang，MSVC 和其他编译器不支持。通过宏定义两套分派
  实现，可以在保持源码一份的前提下双平台编译。
* **全局状态集中**：`ip`、`line`、`stack`、`p_state`、`err_text`、
  `retvalue` 等有十几个状态变量需要在所有 OPCODE 之间共享，拆成多个
  函数会被迫用参数/结构体来传递，可读性反而更差。

作为代价，我们需要忍受一个巨型函数——但它被严格组织成“序言 → 分派表
→ 各 OPCODE 块 → 异常/退出尾声”这四段，每一段都可以独立阅读。

---

## 10.2 双路分派：Computed Goto 与 Switch/Case

### 10.2.1 宏层定义

`gdscript_vm.cpp` 开头定义了两套互斥的宏：

```cpp
#if defined(__GNUC__) || defined(__clang__)
    // 路径 A：computed goto
    #define OPCODES_TABLE  static const void *switch_table_ops[] = { ... };
    #define OPCODE(m_op)   m_op:
    #define OPCODES_END    OPSEXIT:
    #define OPCODES_OUT    OPSOUT:
    #define OPCODE_SWITCH(m_test)   goto *switch_table_ops[m_test];
    #define DISPATCH_OPCODE         goto *switch_table_ops[_code_ptr[ip]]
    #define OPCODE_BREAK            goto OPSEXIT
    #define OPCODE_OUT              goto OPSOUT
#else
    // 路径 B：switch/case
    #define OPCODES_TABLE
    #define OPCODE(m_op)           case m_op:
    #define OPCODES_END
    #define OPCODES_OUT
    #define OPCODE_SWITCH(m_test)  switch (m_test)
    #define DISPATCH_OPCODE        continue
    #define OPCODE_BREAK           break
    #define OPCODE_OUT             break
    #define OPCODE_WHILE(m_test)   while (m_test)
#endif
```

> `_MSC_VER` 下的 `OPCODE_SWITCH` 还加了 `__assume(m_test <= OPCODE_END)`，
> 给 MSVC 一个承诺让它生成更紧凑的跳转表。

两套宏的目标完全一致：**对同一段 OPCODE 实现代码，在 GCC/Clang 下
展开成 `label: ... goto *table[...]`；在 MSVC 下展开成
`case XXX: ... continue;`。**

于是整个主循环的骨架长成这样：

```cpp
OPCODES_TABLE;                       // GCC/Clang 下是一张跳转表
OPCODE_WHILE(ip < _code_size) {      // MSVC 下是 while(ip < _code_size)
    OPCODE_SWITCH(_code_ptr[ip]) {
        OPCODE(OPCODE_OPERATOR) { /* ... */ } DISPATCH_OPCODE;
        OPCODE(OPCODE_ASSIGN)   { /* ... */ } DISPATCH_OPCODE;
        // ...一百多条分支...
        OPCODE(OPCODE_END)      { /* ... */ } OPCODE_BREAK;
    }
    OPCODES_END                      // GCC/Clang 下是 OPSEXIT:
    /* 错误处理 */
    OPCODE_OUT;
}
OPCODES_OUT                          // GCC/Clang 下是 OPSOUT:
/* profile、清理、return 尾声 */
```

### 10.2.2 为什么 computed goto 更快？

传统 `switch/case` 的生成结果是：

```
loop:
    opcode = code[ip];
    jump table[opcode];       // 间接跳转 A
    ...
case X:
    ... 执行 ...
    goto loop;                // 直接跳回 loop
```

每条 OPCODE 结尾的 `continue` 都会回到同一个间接跳转点，CPU 的分支
预测器只能对这个点做一个“全局历史”预测，命中率低。

Computed goto 的结构是：

```
OPCODE_X:
    ... 执行 ...
    goto *table[code[ip]];    // 间接跳转 B
OPCODE_Y:
    ...
    goto *table[code[ip]];    // 间接跳转 C
```

每条指令都有**自己**的间接跳转点，CPU 可以针对“在 OPCODE_X 之后常见
的下一条指令”单独学习历史（例如在 for 循环中 `ITERATE` 之后几乎总是
`JUMP`）。根据 Ertl & Gregg 的经典论文，这种写法在解释器场景下能带来
15%-40% 的性能提升，所以 Godot 愿意为此引入宏层抽象。

### 10.2.3 分派表的生成

`OPCODES_TABLE` 宏展开后就是第 9 章图中提到的指针数组：

```cpp
static const void *switch_table_ops[] = {
    &&OPCODE_OPERATOR,
    &&OPCODE_OPERATOR_VALIDATED,
    &&OPCODE_TYPE_TEST_BUILTIN,
    /* ... 所有 OPCODE_XXX 标签地址 ... */
    &&OPCODE_END,
};
```

这是 GCC 的 Labels as Values 扩展：`&&label` 取一个 label 的地址。表
在函数内部是 `static` 的，只会初始化一次。其**下标**必须严格对应
`Opcode` 枚举值，否则 VM 会跳到错误的地方——这也是为什么第 7 章
`GDScriptByteCodeGenerator` 在写出 OPCODE 时要用枚举常量而不是裸数字。

---

## 10.3 序言：启动一次调用

`call()` 入口并不是直接跳到分派表。函数前半段做了大量**准备工作**，
分成若干步：

### 10.3.1 栈溢出防线

```cpp
static thread_local int call_depth = 0;
if (unlikely(++call_depth > MAX_CALL_DEPTH)) {   // 2048
    call_depth--;
    // 打印 "Stack overflow. Check for infinite recursion..."
    return _get_default_variant_for_data_type(return_type);
}
```

GDScript 的函数调用会通过 `OPCODE_CALL` 再次进入 `call()`，递归地消耗
C 栈（这也是为什么后文用 `alloca`）。`MAX_CALL_DEPTH` 是一个线程本地
的软上限，目的是**在真的发生 C 栈溢出前**把用户的递归截断，避免整个
进程崩溃。

### 10.3.2 两种入场模式：新调用 vs 恢复调用

```cpp
if (p_state) {
    // 恢复被 await 暂停的调用
    stack = (Variant *)p_state->stack.ptr();
    instruction_args = (Variant **)&p_state->stack.ptr()[
                           sizeof(Variant) * p_state->stack_size];
    line = p_state->line;
    ip = p_state->ip;
    alloca_size = p_state->stack.size();
    script = p_state->script;
    p_instance = p_state->instance;
    defarg = p_state->defarg;

    // 转移所有权，防止 GDScriptFunctionState 析构时再销毁一次栈
    p_state->stack_size = 0;
} else {
    // 正常新建栈帧（见 10.3.3）
}
```

恢复路径和新建路径的栈**布局是完全一致的**——恢复路径只是把当时
堆上保存的一整块原样拷回（在 `OPCODE_AWAIT` 分支里做过复制），所以
“恢复”对主循环来说完全透明：设好 `ip`、`stack`、`instruction_args`
之后就跟第一次调用没区别。

### 10.3.3 参数校验、类型转换与栈分配

新调用路径做以下事情（简化展示）：

1. **参数个数检查**：
   ```cpp
   if (p_argcount != _argument_count) {
       if (p_argcount > _argument_count && !is_vararg()) {
           r_err.error = CALL_ERROR_TOO_MANY_ARGUMENTS;
           return ...;
       }
       if (p_argcount < _argument_count - _default_arg_count) {
           r_err.error = CALL_ERROR_TOO_FEW_ARGUMENTS;
           return ...;
       }
       defarg = _argument_count - p_argcount;   // 将跳到默认值段
   }
   ```
   `defarg` 是“还需要走多少个默认参数槽”的计数，稍后由
   `OPCODE_JUMP_TO_DEF_ARGUMENT` 配合 `default_arguments` 数组把 ip 跳
   到对应位置。

2. **alloca 一整块栈**：
   ```cpp
   alloca_size = sizeof(Variant*) * FIXED_ADDRESSES_MAX
               + sizeof(Variant*) * _instruction_args_size
               + sizeof(Variant)  * _stack_size;
   uint8_t *aptr = (uint8_t *)alloca(alloca_size);
   stack = (Variant *)aptr;
   ```
   `alloca` 分配在 C 栈上——这意味着**只要不 await，就不会有堆分配**。
   这对于高频调用（例如 `_process`）至关重要。

3. **参数构造与类型转换**：逐个参数判断 `argument_types[i]` 是否匹配；
   若硬类型需要转型，则用 `Variant::construct` 重新构造一份。特别地：
   * `Array` / `Dictionary` 有容器元素类型时，会用“带元素类型约束”
     的构造器复制一份，避免把未约束的 Variant 存到强类型变量里。
   * 未传入的参数（由默认值补齐的槽位）这里不会初始化，留给
     `OPCODE_JUMP_TO_DEF_ARGUMENT` 与 `OPCODE_ASSIGN_DEFAULT` 之类的
     字节码做。

4. **剩余槽位 placement new**：
   ```cpp
   for (int i = non_vararg_arg_count + FIXED_ADDRESSES_MAX; i < _stack_size; i++) {
       memnew_placement(&stack[i], Variant);
   }
   ```
   每一个栈槽都得是合法构造过的 Variant，否则 `GET_VARIANT_PTR` 返回
   的指针解引用就是未定义行为。

5. **vararg 装袋**：见 9.4.2 所述。

6. **特化临时槽的类型初始化**：
   ```cpp
   for (const Pair<int, Variant::Type> &E : temporary_slots) {
       type_init_function_table[E.second](&stack[E.first]);
   }
   ```
   `temporary_slots` 是 `GDScriptByteCodeGenerator::temporaries` 的快照，
   里面只保留了“必须以特定 Variant::Type 初始化”的槽。这样
   `OPCODE_TYPE_ADJUST_*` 与特化 OPCODE 就能假设目标槽已经是正确类型。

### 10.3.4 固定地址 + 通知 GDScriptLanguage

```cpp
if (p_instance) {
    memnew_placement(&stack[ADDR_STACK_SELF], Variant(p_instance->owner));
    script = p_instance->script.ptr();
} else {
    memnew_placement(&stack[ADDR_STACK_SELF], Variant);
    script = _script;
}
memnew_placement(&stack[ADDR_STACK_CLASS], Variant);
VariantInternal::object_assign_without_ref_unsafe(&stack[ADDR_STACK_CLASS], script);
memnew_placement(&stack[ADDR_STACK_NIL], Variant);

GDScriptLanguage::CallLevel call_level;
GDScriptLanguage::get_singleton()->enter_function(
    &call_level, p_instance, this, stack, &ip, &line);
```

* `ADDR_STACK_SELF` 被赋成 `p_instance->owner`（Object*），这样
  `self` 在任何 OPCODE 里都只是一个普通的栈槽。
* `ADDR_STACK_CLASS` 存的是**当前 GDScript***，但是用
  `object_assign_without_ref_unsafe` 避免 refcount 操作——这对应
  第 9 章结尾那句“故意不调用 `ADDR_STACK_CLASS` 的析构”。
* `enter_function` 把本次调用推入 `GDScriptLanguage` 的调用栈，
  暴露 `&ip`、`&line` 给调试器——当调试器把当前脚本挂起时，可以
  通过这些指针实时读出“当前正在跑到哪”。

---

## 10.4 寻址宏：24 位地址 → `Variant *`

主循环体里最频繁出现的两个宏是 `GET_VARIANT_PTR` 和 `GET_INSTRUCTION_ARG`。
它们是整个 VM 的“寻址单元”。

### 10.4.1 `GET_VARIANT_PTR`

Release 版本的实现：

```cpp
#define GET_VARIANT_PTR(m_v, m_code_ofs) \
    Variant *m_v; \
    { \
        int address = _code_ptr[ip + 1 + (m_code_ofs)]; \
        m_v = &variant_addresses[(address & ADDR_TYPE_MASK) >> ADDR_BITS] \
                                [address & ADDR_MASK]; \
        if (unlikely(!m_v)) OPCODE_BREAK; \
    }
```

配合：

```cpp
Variant *variant_addresses[ADDR_TYPE_MAX] = {
    stack,                                   // ADDR_TYPE_STACK    = 0
    _constants_ptr,                          // ADDR_TYPE_CONSTANT = 1
    p_instance ? p_instance->members.ptrw()  // ADDR_TYPE_MEMBER   = 2
               : nullptr,
};
```

一次寻址的开销只有两件事：**一次位运算 + 一次数组索引**。注意
`variant_addresses` 是 `Variant *[3]`，**不是** `Variant **`——它在栈上
作为普通局部变量，编译器可以把三个指针都 hoisted 到寄存器。

DEBUG 版本额外做两件事：检查 `address_type` 范围、检查 `address_index`
是否越过 `variant_address_limits[type]`，并且对“静态方法里访问成员”
给出一条定向错误消息（“Cannot access member without instance.”）。

### 10.4.2 `LOAD_INSTRUCTION_ARGS` / `GET_INSTRUCTION_ARG`

对于 `CALL`、`CONSTRUCT_ARRAY` 这种“实参数量可变”的 OPCODE，VM 不
能为每一条都写一份访问代码——于是引入了 `instruction_args` 这条
**跳板数组**：

```cpp
#define LOAD_INSTRUCTION_ARGS \
    int instr_arg_count = _code_ptr[ip + 1]; \
    for (int i = 0; i < instr_arg_count; i++) { \
        GET_VARIANT_PTR(v, i + 1); \
        instruction_args[i] = v; \
    } \
    ip += 1;

#define GET_INSTRUCTION_ARG(m_v, m_idx) \
    Variant *m_v = instruction_args[m_idx]
```

`LOAD_INSTRUCTION_ARGS` 把变长的实参 24 位地址全部解析成 `Variant *`
写进 `instruction_args[]`，然后剩余的 OPCODE 实现就只认
`GET_INSTRUCTION_ARG(ret, argc + 1)` 这种下标形式。这样，无论实参是
来自常量池、成员池还是栈槽，OPCODE 实现都不需要关心。

一个典型的 `OPCODE_CALL` 长这样：

```cpp
OPCODE(OPCODE_CALL) {
    bool call_ret = (_code_ptr[ip]) != OPCODE_CALL;
    LOAD_INSTRUCTION_ARGS                           // 装填实参指针
    CHECK_SPACE(3 + instr_arg_count);

    ip += instr_arg_count;                          // 跳过地址段
    int argc          = _code_ptr[ip + 1];
    int methodname_idx= _code_ptr[ip + 2];
    const StringName *methodname = &_global_names_ptr[methodname_idx];

    GET_INSTRUCTION_ARG(base, argc);                // base 是最后一个参数位
    Variant **argptrs = instruction_args;

    Variant temp_ret;
    Callable::CallError err;
    if (call_ret) {
        GET_INSTRUCTION_ARG(ret, argc + 1);
        base->callp(*methodname, (const Variant **)argptrs, argc, temp_ret, err);
        *ret = temp_ret;
    } else {
        base->callp(*methodname, (const Variant **)argptrs, argc, temp_ret, err);
    }
    /* DEBUG: 诊断 await 漏用、返回类型为 void 等 */
    ip += 3;
}
DISPATCH_OPCODE;
```

可以看到整条指令的结构：

1. `_code_ptr[ip + 0]` = opcode；
2. `_code_ptr[ip + 1]` = `instr_arg_count`；
3. 接下来 `instr_arg_count` 个 24 位地址（被 `LOAD_INSTRUCTION_ARGS` 消化）；
4. 再往后 `argc`、`methodname_idx`，最后 `ip += 3` 迈过这三个常规字段。

这一套“**LOAD → CHECK → pullout fields → do work → ip +=**”的模板
在几乎所有 OPCODE 中重复出现，只是字段数量和语义不同。

---

## 10.5 自修改字节码：`OPCODE_OPERATOR` 的内联缓存

VM 里**最花哨**的一段是 `OPCODE_OPERATOR`。它是非特化的二元运算符，
但又想接近 `OPCODE_OPERATOR_VALIDATED` 的速度。做法是：在运行期把
字节码**改写**成带缓存的版本，形成一个简易的单态内联缓存（monomorphic
inline cache）。

### 10.5.1 指令布局

`OPCODE_OPERATOR` 的指令槽包括：

```
[ip+0]  opcode = OPCODE_OPERATOR
[ip+1]  address(a)
[ip+2]  address(b)
[ip+3]  address(dst)
[ip+4]  operator (Variant::Operator)
[ip+5]  op_signature            (初始为 0，会被改写)
[ip+6]  ret_type                (被改写)
[ip+7..]Variant::ValidatedOperatorEvaluator  (被改写)
```

`sizeof(Variant::ValidatedOperatorEvaluator)` 会被按 `sizeof(int)` 展平
占用若干个槽。`_pointer_size` 就是这个数目。

### 10.5.2 三条路径

```cpp
uint32_t op_signature     = _code_ptr[ip + 5];
uint32_t actual_signature = (a->get_type() << 8) | (b->get_type());

if (unlikely(op_signature == 0)) {
    // —— 第一次运行：查表并写回缓存 ——
    Variant::ValidatedOperatorEvaluator op_func =
        Variant::get_validated_operator_evaluator(op, a_type, b_type);
    if (op_func) {
        Variant::Type ret_type = Variant::get_operator_return_type(op, a_type, b_type);
        VariantInternal::initialize(dst, ret_type);
        op_func(a, b, dst);

        if (_code_ptr[ip + 5] == 0) {
            _code_ptr[ip + 5] = actual_signature;
            _code_ptr[ip + 6] = (int)ret_type;
            *reinterpret_cast<ValidatedOperatorEvaluator*>(&_code_ptr[ip + 7]) = op_func;
        }
    }
} else if (likely(op_signature == actual_signature)) {
    // —— 快速路径：签名匹配，直接调用缓存函数指针 ——
    Variant::Type ret_type = (Variant::Type)_code_ptr[ip + 6];
    auto op_func = *reinterpret_cast<ValidatedOperatorEvaluator*>(&_code_ptr[ip + 7]);
    VariantInternal::initialize(dst, ret_type);
    op_func(a, b, dst);
} else {
    // —— 慢速路径：类型变了，退回到通用 Variant::evaluate ——
    Variant::evaluate(op, *a, *b, *dst, valid);
}
```

三点细节：

* **线程安全**：初始化路径用一个 `static Mutex initializer_mutex`
  锁住写回——Godot 里可能有多个线程同时跑同一段字节码（例如共享的
  脚本资源），不加锁会让不同线程写出不一致的指针。
* **DIV/MOD 例外**：除零错误只有“慢路径”`Variant::evaluate` 会做
  诊断，所以这两类运算被强行把 `op_signature` 置为 `0xFFFF`，永远走
  慢路径，牺牲性能换正确性。
* **“自修改”仍是常量时间**：这是单态缓存，不是多态缓存。如果一条
  语句真的会跑两种类型组合（例如同一个 `+` 既处理 int+int 又处理
  String+String），那么这条指令会在两种类型间反复命中慢路径。因为
  GDScript 是静态/渐进类型的，实际工程中类型切换很少，单态缓存足够
  覆盖 99% 的场景。

---

## 10.6 控制流样板：跳转、循环、默认参数

大部分控制流指令与表达式执行的模式不同——**它们直接修改 `ip`**。

### 10.6.1 无条件/条件跳转

```cpp
OPCODE(OPCODE_JUMP)        { int to = _code_ptr[ip+1];
                             ip = to; } DISPATCH_OPCODE;

OPCODE(OPCODE_JUMP_IF)     { GET_VARIANT_PTR(test, 0);
                             if (test->booleanize()) ip = _code_ptr[ip+2];
                             else                    ip += 3;
                           } DISPATCH_OPCODE;
```

注意 `DISPATCH_OPCODE` 永远在 `ip` 被正确更新之后才执行。**跳转不是
主循环的一等公民**——对主循环来说，它只负责“读 `_code_ptr[ip]`、
分派、让 OPCODE 自己把 ip 推进”。这种解耦让第 7 章“回填（backpatching）”
的做法极其自然：只要 `OPCODE_JUMP` 的目标字段最终填上正确值，主循环
什么都不用知道。

### 10.6.2 `OPCODE_JUMP_TO_DEF_ARGUMENT`

进入函数时，如果有默认参数没被传入，就需要让 `ip` 跳到生成器预先
准备好的“默认值构造段”：

```cpp
OPCODE(OPCODE_JUMP_TO_DEF_ARGUMENT) {
    ip = _default_arg_ptr[defarg];
} DISPATCH_OPCODE;
```

`_default_arg_ptr`（即 `default_arguments`）是一个 `int` 数组，第 `i`
项存着“如果缺 i+1 个参数，应该从哪里开始执行”的代码偏移。第 6 章
讲过编译器如何生成这段。

### 10.6.3 `for` 循环：`ITERATE_BEGIN` 与 `ITERATE`

```cpp
OPCODE(OPCODE_ITERATE_BEGIN) {
    GET_VARIANT_PTR(counter, 0);
    GET_VARIANT_PTR(container, 1);

    *counter = Variant();
    if (!container->iter_init(*counter, valid)) {
        ip = _code_ptr[ip + 4];         // 跳到循环末尾
    } else {
        GET_VARIANT_PTR(iterator, 2);
        *iterator = container->iter_get(*counter, valid);
        ip += 5;                        // 跳过紧随其后的 ITERATE
    }
} DISPATCH_OPCODE;
```

值得注意的是：`ITERATE_BEGIN` 在容器**非空**时直接 `ip += 5` 跳过了
紧挨着的 `ITERATE`——生成器就是利用这个约定，把 `ITERATE` 紧跟在
`ITERATE_BEGIN` 之后排布。这样一份字节码同时编码了“第一次进入”和
“循环回跳”两条路径，省掉一次 JUMP。

特化版本 `OPCODE_ITERATE_BEGIN_INT/FLOAT/STRING/…` 则把 `iter_init`
+ `iter_get` 的 Variant 多态分派替换成类型直连访问，进一步加速。

---

## 10.7 运行时调试支持：`OPCODE_LINE` 与 `OPCODE_BREAKPOINT`

```cpp
OPCODE(OPCODE_LINE) {
    line = _code_ptr[ip + 1];
    ip += 2;

    if (EngineDebugger::is_active()) {
        // Step 执行：每遇到一条 LINE 递减 lines_left
        if (EngineDebugger::get_script_debugger()->get_lines_left() > 0) { ... }

        // 断点命中
        if (EngineDebugger::get_script_debugger()->is_breakpoint(line, source)) {
            do_break = true;
        }
        if (do_break) {
            GDScriptLanguage::get_singleton()->debug_break("Breakpoint", true);
        }
        EngineDebugger::get_singleton()->line_poll();
    }
}
DISPATCH_OPCODE;
```

第 7 章讲过每一行源码会生成一条 `OPCODE_LINE`。它在运行时：

* 把当前行号同步到 `line` 变量——别忘了 `enter_function` 已经把
  `&line` 交给了调试器，这里一更新调试器立即可见；
* 做“单步”与“断点”判定，调用 `debug_break` 把线程挂起；
* 调用 `line_poll` 让调试器有机会响应 UI 消息。

Release 构建下，`OPCODE_LINE` 依然存在（除非 strip 了调试符号），但
`is_active` 为假时所有检查都被跳过，只剩一次 `line = ...`，开销可忽略。

---

## 10.8 退出路径：异常、返回、`await`

主循环的终点并不是一个，它用 `OPCODE_BREAK` / `OPCODE_OUT` 两个
语义编码了三种退出方式。

### 10.8.1 OPSEXIT：错误或 break

所有类似“检查失败就退出”的宏（`OPCODE_BREAK`、`GD_ERR_BREAK`）都
展开为 `goto OPSEXIT`。在 OPSEXIT 标签处，主循环做错误报告：

```cpp
OPCODES_END      // = OPSEXIT:
#ifdef DEBUG_ENABLED
    if (exit_ok) {
        OPCODE_OUT;          // 正常退出（RETURN/OPCODE_END/AWAIT 挂起）
    }
    // 否则组装错误信息、打印、调试器打断、回落到默认返回值
    retvalue = _get_default_variant_for_data_type(return_type);
#endif
    OPCODE_OUT;
```

`exit_ok` 布尔旗标是 DEBUG 下区分“主动退出”与“错误退出”的手段：

* `OPCODE_RETURN*`、`OPCODE_END`、`OPCODE_AWAIT`（挂起时）都会把
  `exit_ok = true` 再 `OPCODE_BREAK`；
* 真正出错的分支（`CHECK_SPACE` 失败、类型不匹配、方法找不到）不会
  设置 `exit_ok`，于是在 OPSEXIT 里被识别为错误并打印。

Release 构建没有这一层保护——一切错误直接跳 OPSEXIT → OPSOUT，返回
默认值。这是 GDScript 对“不中断游戏运行”的传统取舍。

### 10.8.2 OPSOUT：共同的收尾

```cpp
OPCODES_OUT      // = OPSOUT:
#ifdef DEBUG_ENABLED
    if (profiling) {
        profile.total_time.add(time_taken);
        profile.self_time.add(time_taken - function_call_time);
    }
#endif

    // 若不是 await 挂起，则通知 GDScriptLanguage 本帧函数已退出
    if (!p_state || awaited) {
        GDScriptLanguage::get_singleton()->exit_function();
    }

    stack[ADDR_STACK_SELF].~Variant();
    stack[ADDR_STACK_NIL].~Variant();
    // 故意不析构 ADDR_STACK_CLASS（见 9.3.3）
    for (int i = FIXED_ADDRESSES_MAX; i < _stack_size; i++) {
        stack[i].~Variant();
    }
    call_depth--;

    if (p_state && !awaited) {
        // 这是一条被 await 过的函数、现在走完了。
        // 把 retvalue 发给 completed 信号，让挂起者继续。
        const Variant *args[1] = { &retvalue };
        p_state->completed.emit(args, 1);
        GDScriptLanguage::get_singleton()->exit_function();
    }
    return retvalue;
```

这段收尾有三个极其关键的设计点：

1. **显式析构 Variant**：因为栈是 `alloca` 出来的原始内存，Variant 是
   通过 placement new 构造的，所以必须手动调用 `~Variant()`。漏掉任何
   一个都会泄漏引用计数。
2. **Profile 中的 self_time**：`function_call_time` 是在 `OPCODE_CALL`
   里累加子调用所用时间的，用 `total_time - function_call_time` 得到
   的就是“刨除子函数后本函数自身开销”，这正是 Profiler 需要的数值。
3. **异步栈的完整性**：当 `p_state && !awaited` 时，说明本次是“被 await
   过、最终正常返回”，此时要：先 `emit(p_state->completed)`，让因
   `await` 挂起的调用者恢复；再调用 `exit_function()`。注释中强调这
   个顺序是为了“保留 async 调用栈”——调试器能在 completed 信号传播
   过程中看到完整的调用链。

### 10.8.3 `OPCODE_AWAIT`：延迟退出

第 9 章已经剖析过 `OPCODE_AWAIT` 里 `GDScriptFunctionState` 的创建
过程。它与主循环的关系只有两条：

* **同步退出**：如果等待的对象不是 Signal（例如一个立即可用的
  Variant），主循环直接读 `OPCODE_AWAIT_RESUME` 把结果写入目标槽，
  像什么都没发生一样继续；
* **异步挂起**：创建 `gdfs`、连接信号、把 `awaited = true`，然后
  `OPCODE_BREAK`。到 OPSOUT 时 `awaited && p_state == nullptr` 或
  `awaited && p_state != nullptr` 都会被正确处理：

  - 顶层 await（`p_state == nullptr`，`awaited == true`）：直接
    `exit_function()`，返回 `gdfs`（`retvalue = gdfs`），调用者拿到
    一个 `GDScriptFunctionState` 引用；
  - 嵌套 await（`p_state != nullptr`，`awaited == true`）：不
    `emit(completed)`——让最外层那次最终的 resume 去通知外部。

  这种“最后一个 await 才 emit”的安排，正是注释里说的“preserve async
  call stack”。

---

## 10.9 汇总：主循环的执行时间线

把前面几节收拢成一张时间线图：

```
call() 入口
  ├─ call_depth 检查
  ├─ if p_state: 恢复栈        ─┐
  │  else:                      │
  │     ├─ 参数校验             │  序言阶段
  │     ├─ alloca + 初始化栈    │
  │     ├─ vararg 装袋          │
  │     └─ 初始化 temporary_slots
  ├─ 固定地址 (SELF/CLASS/NIL)  ─┘
  ├─ enter_function 注册调用

  ├──────────────────────────────────┐
  │ while (ip < _code_size) {        │
  │   OPCODE_SWITCH(_code_ptr[ip]) {  │
  │      OPCODE: ...                  │
  │      DISPATCH_OPCODE              │  分派阶段
  │   }                               │
  │ }                                 │
  └──────────────────────────────────┘
            │
            ▼
  OPSEXIT: 错误诊断（DEBUG）       ─┐
            │                      │
            ▼                      │  退出阶段
  OPSOUT:                          │
    ├─ profile 结算                │
    ├─ 析构栈上 Variant            │
    ├─ exit_function / completed   │
    └─ return retvalue             ─┘
```

序言负责“把一团乱麻的参数变成整齐的栈”、分派负责“把栈翻译成结果”、
退出负责“让栈和外部世界恢复整洁”。把每个 OPCODE 视作一条从“栈某槽”
流到“另一个栈/成员/常量槽”的小水管，而主循环只是个路由表，那么整个
VM 就变得非常直观了。

---

## 小结

本章的重点不是“能做什么”，而是“怎么快地分派”：

1. **双宏分派**让 GDScript VM 在 GCC/Clang 上使用 computed goto 享受
   15%–40% 的额外性能，同时在 MSVC 上回落到标准 switch。
2. **序言/尾声一次性付费**让 alloca 栈、固定地址、参数构造只在 call
   入口发生一次，主循环内部再也不做堆分配或类型协商。
3. **GET_VARIANT_PTR 的二元表访问**把“常量/栈/成员”三个池统一到一次
   数组下标，所以 OPCODE 实现里看不到 `if (is_constant) ... else ...`
   这种分支。
4. **内联缓存（OPCODE_OPERATOR）**把运行期的类型组合写回字节码，使得
   即使是非特化指令也能达到接近特化指令的速度。
5. **异常路径与 await 路径共用 OPSOUT**，简化了退出点的数量，并以
   `exit_ok`、`awaited`、`p_state` 三个标志区分不同语义。

下一章我们会切换视角，从“主循环怎么跑”回到“指令集有什么”，系统性
地梳理 OPCODE 家族及其语义，把第 7、8、10 三章的线索串起来。
