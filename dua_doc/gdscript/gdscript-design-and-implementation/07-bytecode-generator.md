# 第 7 章　字节码生成：`GDScriptByteCodeGenerator`

> 本章对应源码：
> `modules/gdscript/gdscript_codegen.h`（抽象基类 `GDScriptCodeGenerator`）、
> `gdscript_byte_codegen.h`、`gdscript_byte_codegen.cpp`（具体实现）。

上一章讲的 Compiler 相当于"导演"——它知道要把 AST 翻译成什么。真正动手"敲键盘"的是这一章的主角 `GDScriptByteCodeGenerator`：它决定一次 `+` 翻成几条 Opcode、一个 `if` 的跳转偏移怎么算出来、局部变量该放在栈的哪个槽位。

Compiler 与 CodeGen 的关系值得一再强调：

- **Compiler 面向 AST**。它关心"这是一个 `BinaryOpNode`、左操作数是 `IdentifierNode`、右操作数是 `LiteralNode`"。
- **CodeGen 面向字节码流**。它只关心"两个 `Address` 作为源、一个 `Address` 作为目标，对它们做一次二元运算"。

两者通过 `GDScriptCodeGenerator` 这个**抽象基类接口**交互，具体由 `GDScriptByteCodeGenerator` 实现。这个抽象层的存在不是"理论正确"——它实实在在为 GDScript 未来替换成其它后端（例如某种 JIT 或跨语言转译）留了口子。

## 7.1　`GDScriptCodeGenerator` 抽象接口概览

`gdscript_codegen.h` 中的 `GDScriptCodeGenerator` 是一个纯虚基类。它的方法数量多，但按职责能清晰分成五类：

### ① 资源池与槽位管理

```cpp
virtual uint32_t add_parameter(const StringName &, bool optional, const GDScriptDataType &);
virtual uint32_t add_local(const StringName &, const GDScriptDataType &);
virtual uint32_t add_local_constant(const StringName &, const Variant &);
virtual uint32_t add_or_get_constant(const Variant &);
virtual uint32_t add_or_get_name(const StringName &);
virtual uint32_t add_temporary(const GDScriptDataType &);
virtual void pop_temporary();
virtual void clear_temporaries();
virtual void clear_address(const Address &);
virtual bool is_local_dirty(const Address &) const;
```

这些决定了"一个名字/一个常量对应哪条 `Address`"。

### ② 函数生命周期与作用域

```cpp
virtual void start_parameters(); virtual void end_parameters();
virtual void start_block();       virtual void end_block();
virtual void write_start(GDScript *, const StringName &fn, bool p_static,
                         Variant p_rpc_config, const GDScriptDataType &p_return_type);
virtual GDScriptFunction *write_end();
virtual void set_initial_line(int);
```

`write_start` / `write_end` 包住整个函数；`start_block` / `end_block` 对应 Compiler 的嵌套作用域。

### ③ 表达式写字节码

```cpp
virtual void write_type_adjust(const Address &t, Variant::Type);
virtual void write_unary_operator(const Address &t, Variant::Operator, const Address &l);
virtual void write_binary_operator(const Address &t, Variant::Operator,
                                   const Address &l, const Address &r);
virtual void write_type_test(const Address &t, const Address &s, const GDScriptDataType &);
virtual void write_assign(const Address &t, const Address &s);
virtual void write_assign_with_conversion(...);
virtual void write_assign_null/true/false(...);
virtual void write_cast(...);
// 各种 set/get：按下标、按名、按成员、按静态变量
virtual void write_set(const Address &tgt, const Address &idx, const Address &src);
virtual void write_get_named(const Address &t, const StringName &, const Address &s);
virtual void write_set_member(const Address &v, const StringName &);
// ...
```

### ④ 调用的多种变体

GDScript 对"调用"做了非常细致的 **specialization**——根据静态类型信息把一个调用拆成十几个不同的字节码路径：

```cpp
virtual void write_call(...);                         // 最通用：运行时查找
virtual void write_call_async(...);
virtual void write_super_call(...);
virtual void write_call_utility(...);                 // Variant 全局函数 (sin, abs...)
virtual void write_call_gdscript_utility(...);        // @GDScript 内建 (print, len...)
virtual void write_call_builtin_type(...);            // 内建类型的成员方法 (Array.push_back)
virtual void write_call_builtin_type_static(...);     // Color.from_hsv 等静态方法
virtual void write_call_native_static(...);           // 原生类的静态方法
virtual void write_call_native_static_validated(...); // 同上但已绑 MethodBind
virtual void write_call_method_bind(...);             // 实例方法，未验证类型
virtual void write_call_method_bind_validated(...);   // 实例方法，已验证——生成特化 Opcode
virtual void write_call_self(...);                    // 自身方法
virtual void write_call_script_function(...);         // 脚本级已知 GDScriptFunction*
```

每一条对应 VM 里一条或多条不同 Opcode。关键规律是：**越是静态可确定的路径，生成的 Opcode 越"瘦"、越快**。Analyzer 提供的类型信息越硬，Compiler 越能调到这组中更右边的（specialized）方法。

### ⑤ 控制流

```cpp
virtual void write_if(const Address &cond); virtual void write_else(); virtual void write_endif();
virtual void write_jump_if_shared(const Address &);  // Array/Dict 参数写时共享检测
virtual void write_end_jump_if_shared();
virtual void start_for(const GDScriptDataType &it_type, const GDScriptDataType &list_type, bool is_range);
virtual void write_for_list_assignment(const Address &list);
virtual void write_for_range_assignment(const Address &from, const Address &to, const Address &step);
virtual void write_for(const Address &var, bool use_conv, bool is_range);
virtual void write_endfor(bool is_range);
virtual void start_while_condition();
virtual void write_while(const Address &cond); virtual void write_endwhile();
virtual void write_break(); virtual void write_continue();
virtual void write_breakpoint();
virtual void write_newline(int p_line);
virtual void write_return(const Address &, bool p_use_conversion);
virtual void write_assert(const Address &test, const Address &msg);
```

控制流 API 的一个特点是**成对**出现：`write_if` / `write_else` / `write_endif`，`start_for` / `write_for` / `write_endfor`。CodeGen 内部用栈式结构配对它们并做跳转回填——7.4 节细讲。

### 短路逻辑运算符的特殊处理

`and` / `or` 的求值需要短路——左操作数为 false/true 时要跳过右操作数。CodeGen 不用二元运算符的通用路径处理它们，而是把它们拆成四步接口：

```cpp
virtual void write_and_left_operand(const Address &);
virtual void write_and_right_operand(const Address &);
virtual void write_end_and(const Address &target);
virtual void write_or_left_operand(...); virtual void write_or_right_operand(...);
virtual void write_end_or(const Address &target);
```

Compiler 先写 `write_and_left_operand(L)`——CodeGen 插入一条"如果 L 为 false 就跳到 target=false"的条件跳转，并把跳转位置记在栈里；再写 `write_and_right_operand(R)`；最后 `write_end_and(target)` 把之前挂着的跳转回填到当前位置。三元运算也是类似的"多步 API"。

这种"拆分式 API"是因为**中间结果要参与短路跳转**，Compiler 必须精细控制 CodeGen 的状态机。下面马上会看到 CodeGen 内部怎么实现。

## 7.2　字节码的内存布局：`GDScriptFunction` 是什么样子

在动手"写字节码"之前，先看清楚目标容器——`GDScriptFunction` 的关键字段（见 `gdscript_function.h`）：

```cpp
class GDScriptFunction {
    // 主字节码数组：每个 int 要么是 Opcode，要么是操作数索引
    Vector<int> code;

    // 池化资源：字节码里的索引指向这些池里的项
    Vector<Variant>    constants;          // 常量池
    Vector<StringName> global_names;       // 名字池（标识符、方法名……）
    Vector<int>        default_arguments;  // 默认参数字节码入口点

    // Specialization 池
    Vector<Variant::ValidatedOperatorEvaluator> operator_funcs;
    Vector<Variant::ValidatedSetter>            setters;
    Vector<Variant::ValidatedGetter>            getters;
    Vector<Variant::ValidatedBuiltInMethod>     builtin_methods;
    Vector<Variant::ValidatedConstructor>       constructors;
    Vector<Variant::ValidatedUtilityFunction>   utilities;
    Vector<GDScriptUtilityFunctions::FunctionPtr> gds_utilities;
    Vector<MethodBind *>                         methods;
    Vector<GDScriptFunction *>                   lambdas;

    // 栈/类型/调试信息
    int stack_size = 0;                    // 栈总槽位数
    Vector<GDScriptDataType> argument_types;
    GDScriptDataType         return_type;
    Vector<StackDebug>       stack_debug;

    // 行号映射（用于调试/错误定位）
    Vector<int>              code_pos_to_line;
    ...
};
```

几个关键观察：

1. **`code` 是一个 `Vector<int>`**——所有操作数都扁平化为 `int`。Opcode 自身占一个 int，每个参数占一个 int。VM 执行循环取一条 Opcode、按 Opcode 已知的参数个数读取后续 int 作为操作数索引。
2. **所有"重对象"都池化**：常量是 `Variant` 拷贝昂贵 → 入 `constants` 池；方法绑定是一个 `MethodBind *` → 入 `methods` 池；等等。`code` 数组永远只存小整数索引。这让字节码数组紧凑、cache 友好，序列化也简单。
3. **Validated 系列**（`ValidatedSetter`、`ValidatedGetter`、`ValidatedBuiltInMethod`……）是**已绑定到具体 C++ 函数指针**的特化条目——当 Analyzer 确认左右两个操作数类型时，Compiler 可以直接把 "执行 Variant::add(int, int)" 的函数指针烘进 `operator_funcs` 池，VM 一条 Opcode 直接函数指针调用、不再走 Variant 的通用评估器。这是 GDScript 字节码在**类型化代码**上能比"动态 Variant 操作" 快许多倍的根本。

## 7.3　`GDScriptByteCodeGenerator` 的内部状态

在 `gdscript_byte_codegen.h` 里看到的字段，就是 CodeGen 用来完成上面所有事情的"工作台"。挑几个关键：

### 7.3.1　字节码缓冲

```cpp
Vector<int> opcodes;             // 正在写入的字节码，最后会 move 到 GDScriptFunction::code
int current_line = 0;            // 当前源码行（用于行号映射）
int instr_args_max = 0;          // 单条 Opcode 最大操作数个数（用于 VM 预分配临时数组）
```

`opcodes` 就是逐条 `push_back(opcode)` + `push_back(operand)` 累积出来的。`instr_args_max` 是一条信息——**VM 主循环在执行一条指令前需要准备一个至少这么大的 Variant 指针数组来"解引用操作数"**。Compiler 边写边更新它，`write_end()` 再把它存进 `GDScriptFunction`。

### 7.3.2　栈布局：locals + temporaries

```cpp
struct StackSlot {
    Variant::Type type = Variant::NIL;
    bool can_contain_object = true;
    Vector<int> bytecode_indices;     // 哪些字节码位置引用了这个槽（类型调整时用）
};

Vector<StackSlot> locals;              // 显式命名的局部变量槽位
HashSet<int>      dirty_locals;        // 哪些 local 自上次清零起被写过
Vector<StackSlot> temporaries;         // 匿名临时槽位
List<int>         used_temporaries;    // 当前在用的 temp 编号
RBMap<Variant::Type, List<int>> temporaries_pool;  // 按类型分的 temp 回收池
```

- **locals vs temporaries**：同一个函数的栈上两种槽位并存。Locals 有名字（`var x`），生命周期跟源码作用域走；Temporaries 无名，每次表达式求值可能临时借一个，用完归还。
- **按类型回收的临时池**：`temporaries_pool[Variant::INT]` 缓存着一批曾经用于整数、现已空闲的槽位编号。CodeGen 要新临时时先去池里查同类型空闲槽——避免反复扩栈。这种"类型感知的 slot 池"是 GDScript 能在局部把栈压到很小的关键。
- **`dirty_locals`**：每当写一个 local（如 `x = 1`）就把它加入 dirty 集合。某些 Opcode（比如 for 循环结束时）会批量把循环内脏局部回收——具体用于"循环外想再确认某值不是循环中遗留的"的少数场景。

### 7.3.3　池化映射

```cpp
HashMap<Variant, int>       constant_map;
RBMap<StringName, int>      name_map;

RBMap<Variant::ValidatedOperatorEvaluator, int> operator_func_map;
RBMap<Variant::ValidatedSetter, int>   setters_map;
RBMap<Variant::ValidatedGetter, int>   getters_map;
RBMap<Variant::ValidatedBuiltInMethod, int> builtin_method_map;
RBMap<Variant::ValidatedConstructor, int>   constructors_map;
RBMap<Variant::ValidatedUtilityFunction, int> utilities_map;
RBMap<GDScriptUtilityFunctions::FunctionPtr, int> gds_utilities_map;
RBMap<MethodBind *, int> method_bind_map;
RBMap<GDScriptFunction *, int> lambdas_map;
```

看这行：`HashMap<Variant, int> constant_map`——同一个常量在一个函数里出现 100 次，只会在常量池里占一份。`add_or_get_constant(v)` 先查 `constant_map` 看 `v` 是否已在池中，在就返回旧索引，否则 `constants.push_back(v)` 再记入 map。**`add_or_get_*` 这个前缀就是"去重入池"**。

同样的模式套在每种特化池上——同一个 `MethodBind *` 在函数里用 100 次也只占一个 `methods` 槽。

### 7.3.4　跳转回填所用的"待定地址"栈

```cpp
// Lists since these can be nested.
List<int>             if_jmp_addrs;
List<int>             for_jmp_addrs;
List<Address>         for_counter_variables;
List<Address>         for_container_variables;
List<Address>         for_range_from_variables;
...（还有若干 while / match 相关栈）
```

一条 `if (c): ... else: ...` 的字节码大概长这样：

```
  [N+0] OP_JUMP_IF_NOT  cond  →  ?       # else 开始地址，暂填 0
  [N+1] ...（if 体）
  [N+K] OP_JUMP         →  ?              # 条件分支末尾跳到 endif，暂填 0
  [N+K+1] ...（else 体）                  # 前面 OP_JUMP_IF_NOT 的目标是这里
  [N+K+M] ...（if 之后）                  # 前面 OP_JUMP 的目标是这里
```

问题是：`write_if(cond)` 时 else 开始地址还不知道；`write_else()` 时 endif 地址还不知道。做法是：

1. `write_if(cond)`：写入 `OP_JUMP_IF_NOT cond 0`，同时把这个"0"的位置（`opcodes.size() - 1`）压进 `if_jmp_addrs`。
2. `write_else()`：读栈顶得到那个 0 的位置，**用当前 `opcodes.size()` 回填它**——这就是 else 开头；然后写 `OP_JUMP 0`，把这个新"0"的位置再压进栈。
3. `write_endif()`：读栈顶那个位置，用当前 `opcodes.size()` 回填。

这就是经典的**单趟扫描 + 回填（backpatching）**。`List<int>` 让 if/for/while/and/or/ternary 等都能嵌套——每种结构有自己的栈。

## 7.4　跳转回填的细节：一个 if 的完整字节码

拿一小段实际 GDScript 演示：

```gdscript
var x = 0
if x > 0:
    x = 1
else:
    x = -1
print(x)
```

Compiler 调用 CodeGen 的序列（简化）：

```cpp
// var x = 0
auto x = codegen->add_local("x", INT);
codegen->write_assign(x, codegen->add_or_get_constant(0));

// if x > 0:
auto tmp_cond = codegen->add_temporary(BOOL);
codegen->write_binary_operator(tmp_cond, OP_GREATER, x, const_zero);
codegen->write_if(tmp_cond);                    // 写 OP_JUMP_IF_NOT，记地址 A
    // x = 1
    codegen->write_assign(x, const_one);
codegen->write_else();                           // 回填 A = 当前位置; 写 OP_JUMP，记地址 B
    // x = -1
    codegen->write_assign(x, const_minus_one);
codegen->write_endif();                          // 回填 B = 当前位置

// print(x)
codegen->write_call_gdscript_utility(unused, "print", {x});
```

产出的字节码（用伪 Opcode）：

```
 0: OP_ASSIGN             dst=x        src=const_zero
 3: OP_BINARY_OP_GT       dst=tmp_cond L=x R=const_zero
 8: OP_JUMP_IF_NOT_COND   src=tmp_cond target=15      # ← A 被回填
12: OP_ASSIGN             dst=x        src=const_one
15: OP_JUMP               target=19                   # ← B 被回填
18: OP_ASSIGN             dst=x        src=const_minus_one
21: OP_CALL_GDSCRIPT_UTIL dst=_        fn=print args=[x] argc=1
```

注意 CodeGen 边写边维护了几张表：`constants` 池里有 `{0, 1, -1}`；`name_map` 里注册了 `"print"`；`gds_utilities` 池里注册了 `print` 的 `FunctionPtr`；`stack_debug` 记录了 `x` 和 `tmp_cond` 分别对应哪段行号段的哪个槽。这些都会在 `write_end()` 里烘进最终的 `GDScriptFunction`。

## 7.5　表达式求值的"临时寄存器"逻辑

考虑 `a + b * c`。Compiler 递归下来先让 CodeGen 算 `b * c`，结果存到一个临时；再用这个临时和 `a` 算 `+`。流程：

```cpp
auto t1 = codegen->add_temporary(ret_type);           // borrow temp for b*c
codegen->write_binary_operator(t1, OP_MUL, b, c);
auto t2 = codegen->add_temporary(ret_type);           // borrow temp for a+(b*c)
codegen->write_binary_operator(t2, OP_ADD, a, t1);
codegen->pop_temporary();                              // 归还 t1（现已无用）
// ... t2 作为表达式结果返回给上层 ...
codegen->pop_temporary();                              // 外层用完也归还
```

注意几点：

1. **临时的"借-还"**：`add_temporary` / `pop_temporary` 是严格配对的。CodeGen 内部用 `used_temporaries` 栈保证先借的后还（LIFO）。
2. **分配器复用**：还掉的临时不会立刻"销毁"，而是丢回 `temporaries_pool[type]`——下一次 `add_temporary(type)` 命中池就复用。所以大表达式不会导致栈无限膨胀。
3. **`CallTarget` 的 RAII 封装**：`GDScriptByteCodeGenerator` 自己内部实现 `write_call_*` 时用了一个小 RAII 类 `CallTarget`（见 byte_codegen.h:51），它在析构里保证 `pop_temporary()` 被调对次数——**C++ 编译器在 DEV_ENABLED 构建下会 assert `cleaned` 这个标志位**，是字节码生成器里少见、但非常有用的防御性设计。

## 7.6　特化：`validated` 系列调用

第 7.1 节提到 `write_call_method_bind_validated` 等方法。什么叫 "validated"？就是 Compiler 在编译期**已经知道参数类型全部匹配签名**、能把"这次调用"收敛到一个 C++ 级别的函数指针。

### 7.6.1　从 `Variant::evaluate` 到 validated operator

举个最经典的例子——`a + b`。`write_binary_operator` 的非特化路径大致是：

```
OP_OPERATOR  op=ADD  dst  L  R
```

VM 执行时会调 `Variant::evaluate(ADD, L, R, result, valid)`，这是一个内部包含 `switch (l.type) switch (r.type)` 的大型双重分派函数——性能不差但仍不是机器码直调。

如果 Analyzer 告诉 Compiler 两边都是 `int`，Compiler 可以提前找出对应的 `ValidatedOperatorEvaluator` ——实际是一个 C 函数指针 `void fn(const Variant *l, const Variant *r, Variant *out)`，它跳过所有类型判断直接干整数加法。然后：

```cpp
int idx = codegen->operator_func_map.get_or_add(fn);   // 进 operator_funcs 池
opcodes.push_back(OP_OPERATOR_VALIDATED);
opcodes.push_back(dst_addr_encoded);
opcodes.push_back(L_addr_encoded);
opcodes.push_back(R_addr_encoded);
opcodes.push_back(idx);                                 // 池索引
```

VM 看到 `OP_OPERATOR_VALIDATED` 时直接 `function->operator_funcs[idx](L, R, out)`——一个间接函数指针调用，没有任何分派。

这就是为什么 GDScript 官方文档里反复讲："类型化 GDScript 可以比未类型化快数倍"——**快的本质就是 Analyzer 能让 Compiler 走到这些 validated 路径上**。

### 7.6.2　其它 validated 池

`setters` / `getters` / `builtin_methods` / `constructors` / `utilities` / `gds_utilities` 都是同一种"把运行时查找压到编译期"的池化：

- `v.position = x` 在 untyped 时走通用 `OP_SET_NAMED`；若 `v` 静态类型是 `Sprite2D`，Compiler 直接找到 `Sprite2D::set_position` 对应的 `ValidatedSetter` 函数指针，生成 `OP_SET_NAMED_VALIDATED idx` —— VM 一步到位。
- `Array.size()` 在 typed 时直接走 `OP_CALL_BUILTIN_TYPE_VALIDATED`，无需再跑方法名查找。

每种特化 Opcode 都需要：① 一个池 + 一张池索引的去重映射；② `write_call_*` 方法在编译期完成池注册并写索引；③ VM 里对应一条 Opcode 读索引直接跳到函数指针。这三件事在 `GDScriptByteCodeGenerator` / `GDScriptFunction` / `gdscript_vm.cpp` 里一一对应——改动时必须同步，否则字节码格式就乱了。

## 7.7　行号映射与调试信息

```cpp
virtual void write_newline(int p_line);
int current_line = 0;
// GDScriptFunction 里相关字段：
Vector<int>              code_pos_to_line;
Vector<StackDebug>       stack_debug;
```

`write_newline(line)` 由 Compiler 在每条语句前调用。CodeGen 写一条 `OP_LINE line`——运行时这条 Opcode 唯一的作用是**让调试器/Profiler 感知"现在执行第几行"**。非 DEBUG 构建会在编译期剥离这些 Opcode 以减少开销。

`stack_debug` 记录的是 "哪段字节码范围内、哪个栈槽对应源码里的哪个变量名"——让调试器在断点处能列出局部变量。Compiler 的 `start_block` / `end_block` 会触发 CodeGen 往 `stack_debug` 压相应区间条目。

## 7.8　`write_end`：烘焙成 `GDScriptFunction`

`write_end()` 是 CodeGen 的"收工"方法：

```cpp
GDScriptFunction *GDScriptByteCodeGenerator::write_end() {
    // 1. 把最后一条 OP_END/RETURN 补齐（如果没有）
    // 2. 计算 stack_size = max(parameters) + max(locals) + max(temporaries)
    // 3. 把所有池（constants/names/operator_funcs/.../lambdas）move 进 function
    // 4. 把 opcodes move 进 function->code
    // 5. 把 stack_debug、行号映射等 move 进 function
    // 6. 填写 function->instr_args_max、stack_size
    // 7. return function;
}
```

这里尤其要注意 **`stack_size` 的计算**——它不是三段的简单相加，而是"参数段紧跟局部段紧跟临时段，每段的大小取生命周期中的峰值"。VM 每次调用函数时用这个数字一次 `alloca` / 分配一段连续 `Variant` 缓冲区，参数、局部、临时都落在同一个数组里，按 `Address.mode` + `address` 在对应偏移段内寻址。

## 7.9　一个完整小例的字节码与池

用 7.4 节那段 `if/else` 代码走完 `write_end`：

- `code` 数组：`[OP_ASSIGN, x_slot, 0/*const #0*/, OP_BINARY_GT, tmp_slot, x_slot, 0, OP_JUMP_IF_NOT, tmp_slot, 15, OP_ASSIGN, x_slot, 1/*const #1*/, OP_JUMP, 19, OP_ASSIGN, x_slot, 2/*const #2*/, OP_CALL_GDSCRIPT_UTIL, _, 0/*gds_util#0*/, 1, x_slot, OP_RETURN, 0]`
- `constants` 池：`[0, 1, -1]`
- `global_names` 池：`[]`
- `gds_utilities` 池：`[&GDScriptUtilityFunctions::_print_func]`
- `stack_size`: 2（`x`、`tmp_cond`）
- `code_pos_to_line`: 根据 `write_newline` 记录的映射

打开 Godot 编辑器 → Debugger → Disassemble Script 可以看到几乎一模一样的反汇编输出——那就是第 8 章的主题。

## 7.10　小结：Compiler 与 CodeGen 的职责边界

| 关注点 | 归属 |
|---|---|
| 解释 AST 的语义（"这是 `if` 还是 `ternary`"） | Compiler |
| 维护符号作用域（`locals` 名字→Address） | Compiler |
| 维护类型（`GDScriptDataType`）与调用 specialization 决策 | Compiler（根据 Analyzer 结果） |
| 实际往字节码缓冲区里 push int | CodeGen |
| 栈布局、临时池、跳转回填、常量/方法池去重 | CodeGen |
| 烘焙 `GDScriptFunction` 的 `stack_size` / `instr_args_max` 等运行时元数据 | CodeGen |
| 写 `OP_LINE`、`stack_debug` 调试信息 | CodeGen（按 Compiler 驱动） |

记住这条边界能让你在调试字节码相关问题时**第一时间找对源码位置**：

- "生成的字节码里少了一条指令" → 大概率是 Compiler 漏调了某个 `write_*`。
- "指令顺序对但跳转目标错了" → 大概率是 CodeGen 的回填栈配对出问题。
- "常量/方法名池里出现重复" → CodeGen 的 `add_or_get_*` 有 bug。
- "参数类型对但运行时类型错" → Analyzer → Compiler 的 `_gdtype_from_datatype` 丢信息。

## 本章小结

- `GDScriptCodeGenerator` 是 GDScript 实现里**唯一**一个"为未来替换后端"留出的抽象层，当前只有 `GDScriptByteCodeGenerator` 一个实现。
- 字节码目标容器是 `GDScriptFunction`。`code` 数组是扁平 `Vector<int>`，所有重对象（常量、名字、MethodBind、validated 函数指针、Lambda）都**池化**进对应 `Vector`，字节码里只存索引。
- CodeGen 内部管理两大类栈槽位：显式 `locals` + 匿名 `temporaries`，后者带按类型的回收池。`dirty_locals`、`used_temporaries`、`temporaries_pool` 一起保证栈在长函数里不暴涨。
- 跳转回填是经典的**单趟扫描 + 回填**，由若干按结构分类的 `List<int>` 栈（`if_jmp_addrs` / `for_jmp_addrs` / ...）承载，支持任意深度嵌套。
- Compiler 利用 Analyzer 给的硬类型把调用"收敛"到 validated 系列方法；每个 validated Opcode 背后都有一个 `Variant::Validated*` 函数指针池——运行时直接函数指针调用。这是类型化 GDScript 性能显著高于未类型化的根本原因。
- `write_end()` 把所有池与字节码打包进 `GDScriptFunction`，还要计算 `stack_size`、`instr_args_max` 等运行时元数据。
- Compiler 与 CodeGen 的职责分界要记清：前者理解 AST 语义，后者只负责"往字节码缓冲区里写 int"。

下一章我们把视角切换到结果——**第 8 章 字节码格式与反汇编**：看看 `gdscript_disassembler.cpp` 是怎么把 `GDScriptFunction::code` 再翻回人类可读文本的。读完那一章，你就能在任何时候打开 Godot 看一段 GDScript 的 "asm 视图" 来自行验证 Compiler / CodeGen 的行为。

---

[← 上一章：第 6 章 Compiler](./06-compiler.md) · [目录](./README.md) · [下一章：第 8 章 字节码格式与反汇编 →](./08-disassembler.md)
