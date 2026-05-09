# 第 9 章　可执行函数：`GDScriptFunction`

经过第 6–8 章的讲解，我们已经了解编译器如何驱动 `GDScriptByteCodeGenerator`
把语法树写成线性字节码。但“字节码”本身只是一串 `Vector<int>`，它必须
被封装进一个可以与 `Object::callp`、`Callable`、`Script::get_method` 等
上层机制对接的对象里，才能真正被 Godot 引擎当作“方法”来调用。这个对象
就是 `GDScriptFunction`（定义在 `modules/gdscript/gdscript_function.h`）。

本章聚焦以下三个主题：

1. **函数对象的字段布局**——GDScriptFunction 为什么拥有如此多的 `Vector`
   以及对应的 `_xxx_ptr` 热指针？
2. **栈帧的组织方式**——VM 调用时 alloca 出的内存如何被切分成“stack /
   instruction_args / 固定地址”三块区域？
3. **调试信息与暂停态**——`StackDebug` 和 `CallState` 分别在什么场景下
   发挥作用？

理解这一章之后，再去看第 10 章的主循环会十分轻松——主循环对栈帧和指针
数组的使用几乎是本章介绍的布局的直接后果。

---

## 9.1 `GDScriptFunction` 的角色

在 `GDScriptByteCodeGenerator::write_end()` 中，编译器会把累积的所有
`Vector` 交给一个 `memnew(GDScriptFunction)` 所得的实例，然后把它挂到
`GDScript::member_functions[name]` 上。从那一刻起，这个函数对象就开始
扮演三种身份：

* **Variant 调用入口**：`GDScriptInstance::callp` 会根据方法名查表，最终调用
  `function->call(...)`。字节码方法与 C++ MethodBind 在 `Object::callp` 视角
  下是等价的。
* **Bytecode 容器**：所有 `OPCODE_XXX` 操作码、常量池、辅助符号表都属于
  它的字段，VM 主循环从这些字段里读取数据执行。
* **调试元数据提供者**：断点、变量监视、性能分析器都要求把运行期的状态
  映射回源码。`StackDebug` 和 profile 字段正是为这一点服务。

---

## 9.2 为什么同一份数据要存两份？`Vector<T>` 与 `_xxx_ptr`

`gdscript_function.h` 中可以看到一个非常惹眼的现象：几乎每一个 `Vector`
成员旁边都会声明一个对应的原始指针和一个 `_xxx_count`。例如：

```cpp
Vector<int> code;
Vector<Variant> constants;
Vector<Variant::ValidatedOperatorEvaluator> operator_funcs;
// ... 十余种特化函数指针向量 ...

int _code_size = 0;
int _constant_count = 0;
int _operator_funcs_count = 0;
// ...

int *_code_ptr = nullptr;
mutable Variant *_constants_ptr = nullptr;
const Variant::ValidatedOperatorEvaluator *_operator_funcs_ptr = nullptr;
// ...
```

### 目的：把热路径的数据访问降到“一次数组下标”

VM 主循环是一个巨大的 `switch` / `goto *table[op]`，它会用极高的频率去
读取这些字段。如果每次都走 `Vector<T>::ptr()`，那么意味着：

1. 多一次 `_cowdata` 级别的间接寻址（Godot 自家的 CoW 容器）；
2. 多一次条件分支（判断是否需要 detach）；
3. 编译器更难把向量指针提升到寄存器。

因此 Godot 采用了一个常见的“打包/冻结”做法：在 `write_end()` 里，把
`Vector<T>` 视作“构建期数据结构”，而把 `_xxx_ptr` / `_xxx_count` 视作
“运行期只读视图”。一旦函数对象构建完成，主循环就只访问后者。

> **实现细节**：`_constants_ptr` 被声明为 `mutable Variant *`，这是因为
> GDScript 的 const 常量（例如一个字典）语义上是可变的——Variant 底层是
> CoW 的，我们无法强制 `const`，因此必须通过 `mutable` 维持指针形式的
> 只读视图。

### 字段清单：特化路径就住在这里

第 7、8 章反复提到的 `OPCODE_XXX_VALIDATED` 家族，其“验证过的 C++ 函数
指针”本质上就住在这张表里：

| 字段 | 内容 | 对应 OPCODE |
| --- | --- | --- |
| `operator_funcs` | `Variant::evaluate` 的特化版本 | `OPCODE_OPERATOR_VALIDATED` |
| `setters` / `getters` | `Variant::ValidatedSetter/Getter` | `OPCODE_SET_NAMED_VALIDATED` / `OPCODE_GET_NAMED_VALIDATED` |
| `keyed_setters` / `keyed_getters` | 键值访问特化 | `OPCODE_SET/GET_KEYED_VALIDATED` |
| `indexed_setters` / `indexed_getters` | 整数下标访问特化 | `OPCODE_SET/GET_INDEXED_VALIDATED` |
| `builtin_methods` | `Variant::ValidatedBuiltInMethod` | `OPCODE_CALL_BUILTIN_TYPE_VALIDATED` |
| `constructors` | `Variant::ValidatedConstructor` | `OPCODE_CONSTRUCT_VALIDATED` |
| `utilities` | `Variant::ValidatedUtilityFunction` | `OPCODE_CALL_UTILITY_VALIDATED` |
| `gds_utilities` | `GDScriptUtilityFunctions::FunctionPtr` | `OPCODE_CALL_GDSCRIPT_UTILITY` |
| `methods` | `MethodBind *` | `OPCODE_CALL_METHOD_BIND_*` |
| `lambdas` | 嵌套的 `GDScriptFunction *` | `OPCODE_CREATE_LAMBDA` |

换句话说，VM 的一条“特化指令”执行逻辑等同于：

```cpp
int func_index = _code_ptr[ip + N];
_operator_funcs_ptr[func_index](lhs, rhs, out, r_valid);
```

一切只是一次间接函数调用。这也是 GDScript 能达到解释执行性能瓶颈的关键。

---

## 9.3 关键元数据字段

### 9.3.1 签名相关

```cpp
StringName name;            // 函数名
StringName source;          // 源文件路径（纯文本，用于栈跟踪）
bool _static = false;       // 是否 static 函数
Vector<GDScriptDataType> argument_types;
GDScriptDataType return_type;
MethodInfo method_info;     // 对外暴露给编辑器/反射的 Godot 标准描述
Variant rpc_config;         // @rpc 注解解析后的配置
int _argument_count = 0;
int _vararg_index = -1;     // 若有 vararg，其在栈中的位置
int _default_arg_count = 0; // default_arguments.size()
```

其中 `MethodInfo` 结构是 Godot 跨脚本语言的通用签名描述，它让 GDScript
方法可以和 C# 方法、原生方法在 `Object` 层面混用。`rpc_config` 则来自
注解阶段（见第 5 章），最终会被 `MultiplayerAPI` 读取。

### 9.3.2 栈尺寸

```cpp
int _stack_size = 0;                // Variant 槽总数
int _instruction_args_size = 0;     // Variant* 数组槽数
TightLocalVector<Pair<int, Variant::Type>> temporary_slots;
```

这三个字段完整描述了一帧调用栈的内存布局。在第 10 章我们将看到它们
如何被用来计算 `alloca_size`；而 `temporary_slots` 则记录“必须在进入
函数时被特定类型构造”的槽位——典型场景是硬类型的本地变量或者特化 for
循环所需要的计数器（第 8 章中提到的 `OPCODE_TYPE_ADJUST_*` 家族也会
在运行期触及这些槽）。

### 9.3.3 脚本反向指针

```cpp
GDScript *_script = nullptr;
SelfList<GDScriptFunction> function_list{ this };
mutable Variant nil;
```

* `_script` 让函数在运行时随时拿回自己的宿主 `GDScript`，用于类型检查、
  `self` 构造、常量查询等。
* `function_list` 把所有活着的 `GDScriptFunction` 串成一条链表——
  `GDScriptLanguage` 在热重载、Reload 操作时会遍历它来替换指针。
* `nil` 是一个“常驻空 Variant”，某些 OPCODE 在需要一个 null 目标时不
  想触碰栈就可以指向它。

---

## 9.4 栈帧布局

`GDScriptFunction::call` 在入口用 `alloca` 一次性取出一整块内存，然后
人为把它切成三段。这块连续内存的布局是：

```
+--------------------------------------------------+  <-- stack (aptr)
| Variant stack[_stack_size]                       |
|   [0..FIXED_ADDRESSES_MAX)   固定地址（self/class/nil）
|   [FIXED_ADDRESSES_MAX..)    参数 + 局部 + 临时
+--------------------------------------------------+
| Variant* instruction_args[_instruction_args_size]|
+--------------------------------------------------+
```

对应源代码（`gdscript_vm.cpp`，简化版）：

```cpp
alloca_size =
    sizeof(Variant *) * FIXED_ADDRESSES_MAX +   // 兼容 32/64 位对齐
    sizeof(Variant *) * _instruction_args_size +
    sizeof(Variant)   * _stack_size;

uint8_t *aptr = (uint8_t *)alloca(alloca_size);
stack = (Variant *)aptr;
instruction_args = (Variant **)&aptr[sizeof(Variant) * _stack_size];
```

### 9.4.1 固定地址：`FIXED_ADDRESSES_MAX = 3`

栈的最前面三个槽位有特殊含义：

```cpp
ADDR_STACK_SELF  = 0,   // self
ADDR_STACK_CLASS = 1,   // 所属脚本（GDScript*）
ADDR_STACK_NIL   = 2,   // 只读 nil
FIXED_ADDRESSES_MAX = 3
```

这一小节保留在栈头而不是放到一个独立的“寄存器文件”里，有几个实际好处：

* **统一寻址**：所有地址都可以通过 `variant_addresses[type][index]` 访问，
  `self`、`class`、`nil` 不需要额外的分支。编译器端也只需要把
  `GDScriptCodeGenerator::Address` 的 `SELF/CLASS/NIL` 三种模式翻译成
  `ADDR_TYPE_STACK + 0/1/2`。
* **调用栈快照**：`OPCODE_AWAIT` 需要把整个栈序列化到堆上，若这三个槽
  在普通栈之外还要特殊处理就会非常复杂；作为栈内元素，直接走同一条
  路径就行。
* **空值安全**：`ADDR_NIL` 是一个合法的、已构造的 Variant，指向它总是
  安全的；编译器可以把“丢弃值”的 `write_xxx` 输出目标直接设为 NIL。

### 9.4.2 参数/局部/临时

`FIXED_ADDRESSES_MAX` 之后依次是：参数（按声明顺序）、普通局部变量、
`TEMPORARY` 临时槽。第 7 章讲过 `GDScriptByteCodeGenerator::temporaries`
的对象池如何复用这段空间——只要类型兼容，后写入的临时可以落在已腾出
的槽里。

如果函数是 **vararg**，那么在这段区域内会有一个专门留给 `Array vararg`
的槽，位置为 `_vararg_index`：

```cpp
if (is_vararg()) {
    Array vararg;
    stack[_vararg_index] = vararg;
    if (p_argcount > _argument_count) {
        vararg.resize(p_argcount - _argument_count);
        for (int i = 0; i < p_argcount - _argument_count; i++) {
            vararg[i] = *p_args[i + _argument_count];
        }
    }
}
```

### 9.4.3 `instruction_args[]`：变长参数的“跳板”

除了 `stack` 外，alloca 还分出一块 `Variant *` 数组叫做 `instruction_args`。
它的作用是承载那些 **参数个数可变** 的指令（构造数组、构造字典、
调用方法等）的实参指针表。

第 7 章介绍过 `GDScriptByteCodeGenerator` 会维护 `max_instruction_args`，
`write_end` 会把它写入 `_instruction_args_size`。主循环里有两个对应的
宏负责填表和取数据：

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

这一设计的精髓在于：**无论参数是 Constant/Stack/Member 哪种池，解出来
的都是 `Variant *`**；指令里随后的语义（调用、构造、甚至模式匹配）
只需要把这组指针当作一个普通 C 函数的 argv 使用即可。

### 9.4.4 `variant_addresses[]`：地址解码的基础

真正的“把 24 位索引翻译成 Variant 指针”的核心代码是这两行（`DEBUG_ENABLED`
关闭时的精简版）：

```cpp
int variant_addresses_limits[ADDR_TYPE_MAX] = {
    _stack_size, _constant_count,
    p_instance ? (int)p_instance->members.size() : 0
};
Variant *variant_addresses[ADDR_TYPE_MAX] = {
    stack, _constants_ptr,
    p_instance ? p_instance->members.ptrw() : nullptr
};
```

配合 `ADDR_TYPE_MASK / ADDR_MASK` 就可以用一次数组下标直接得到 Variant
指针。这里值得注意的是：

* **常量池是函数级别的，而成员池是实例级别的**。两个池都挂在 VM 一层的
  `variant_addresses[]` 上，统一了“从地址拿值”的抽象。
* **成员访问需要实例存在**；`GET_VARIANT_PTR` 在 DEBUG 模式下会针对
  “在静态方法里访问成员”给出清晰的错误消息。
* **常量池是全函数共享、只读的**——这使得 `GDScriptFunction` 之间即使
  是同一个 GDScript 的成员也不会互相干扰。

---

## 9.5 调试信息：`StackDebug`

```cpp
struct StackDebug {
    int line;
    int pos;
    bool added;
    StringName identifier;
};

List<StackDebug> stack_debug;
```

`stack_debug` 列表按源码顺序记录了“在第 `line` 行，栈槽 `pos` 被加入/
移除一个名为 `identifier` 的变量”。第 7 章提到过，字节码生成器在
`add_local` 与 `pop_temporary` 时会顺手把一条 `StackDebug` 记录插入这里。

这个数据结构服务于两个场合：

* **`debug_get_stack_member_state`**：调试器需要在断点处拿到“此时当前
  行能看到的所有变量名及其在栈里的位置”，才能把值回填到 IDE 的变量
  监视窗。它通过按行号遍历 `stack_debug` 构建出一个 `StringName -> 栈下标`
  的映射。
* **Reload 一致性检查**：当 GDScript 重新编译时，老的 `GDScriptFunction`
  可能仍然被 `Callable` / `Signal` 持有；`stack_debug` 使得比较新旧签名
  时可以更精确地判断“是否是同一个函数”。

---

## 9.6 继续执行的载体：`CallState`

```cpp
struct CallState {
    Signal completed;
    GDScript *script = nullptr;
    GDScriptInstance *instance = nullptr;
#ifdef DEBUG_ENABLED
    StringName function_name;
    String script_path;
#endif
    Vector<uint8_t> stack;
    int stack_size = 0;
    int ip = 0;
    int line = 0;
    int defarg = 0;
    Variant result;
};
```

`CallState` 是“await 中断态”的全部内容。当主循环执行到 `OPCODE_AWAIT`
且需要异步等待信号时，会执行以下关键步骤：

1. 新建一个 `GDScriptFunctionState`（`RefCounted`），其内部就包裹着一个
   `CallState`；
2. 把当前 `stack[FIXED_ADDRESSES_MAX..]` 的 Variant 按位复制到
   `state.stack`（一个 `Vector<uint8_t>`，用作裸字节缓冲区）；
3. 记录 `ip + 2`、`line`、`defarg` 以便恢复；
4. 把 `gdfs` 连接到所等待的 `Signal`，信号触发后调用 `resume` 恢复执行
   —— `GDScriptFunction::call` 在这种情况下会走 `p_state != nullptr`
   的分支，直接把 `state.stack` 当作新的栈继续跑。

这一机制使得 `await` 在用户眼中是一句“看似挂起”的普通语句，但在 VM
眼中只是“退出主循环 → 复原主循环”的对称操作，无需特殊线程或协程栈。

> `completed` 是一个 Godot `Signal`，用来让 `await somefunc()` 这种
> “对另一个函数本身 await”的写法成立：如果被 await 的返回值是一个
> `GDScriptFunctionState`，就转换成 `Signal(obj, "completed")`，再次走
> 普通信号等待路径（参见 `OPCODE_AWAIT` 的实现）。

---

## 9.7 Profile 字段

在 `DEBUG_ENABLED` 下，`GDScriptFunction` 还会嵌入一个 `Profile` 子结构
来收集性能数据：

```cpp
struct Profile {
    StringName signature;
    SafeNumeric<uint64_t> call_count;
    SafeNumeric<uint64_t> self_time;
    SafeNumeric<uint64_t> total_time;
    // ... 帧级别副本 ...
    HashMap<String, NativeProfile> native_calls;
    HashMap<String, NativeProfile> last_native_calls;
} profile;
```

`self_time` 与 `total_time` 的区分是 VM 在调用其它函数时会单独累加
`function_call_time`，返回时从 `total_time` 里减掉，就得到了“除去嵌套
调用后，本函数自己花的时间”。`native_calls` 则专门统计 `MethodBind`
调用（通过 `_profile_native_call` 写入），让性能分析工具可以把“慢”定位
到具体的 C++ 方法上。

---

## 9.8 对外 API：`call()`、`get_constant()`、`disassemble()`

```cpp
Variant call(GDScriptInstance *p_instance, const Variant **p_args,
             int p_argcount, Callable::CallError &r_err,
             CallState *p_state = nullptr);
void debug_get_stack_member_state(int p_line,
             List<Pair<StringName, int>> *r_stackvars) const;
#ifdef DEBUG_ENABLED
void disassemble(const Vector<String> &p_code_lines) const;
#endif
Variant get_constant(int p_idx) const;
StringName get_global_name(int p_idx) const;
```

* `call()` 是主入口，下一章会深入拆解它。
* `debug_get_stack_member_state` 是调试器查询变量状态的唯一桥梁。
* `disassemble()` 就是第 8 章那份反汇编器的入口。
* `get_constant` / `get_global_name` 允许外部（例如 LSP）通过索引读取
  常量或全局名，用于提示与调试。

---

## 9.9 函数对象的生命周期

合在一起看，`GDScriptFunction` 的生命周期大致是：

1. **编译期**：`GDScriptCompiler::_parse_function` 创建
   `GDScriptByteCodeGenerator`，生成器里持有一个**尚未填好的**
   `GDScriptFunction`。
2. **收尾期**：`write_end()` 调用 `_set_pointers()` 把 `Vector` 打包到
   `_xxx_ptr` / `_xxx_count`，并把函数加入 `GDScriptLanguage` 的全局
   `function_list`。
3. **运行期**：任意数量的 `Callable` / 直接调用共享同一个
   `GDScriptFunction`；调用时 alloca 出临时栈帧，不产生任何堆分配
   （除非遇到 `await`）。
4. **销毁期**：宿主 `GDScript` 析构时调用 `memdelete` 删除函数对象；
   `function_list` 自动断链；若该函数还有挂起的 `GDScriptFunctionState`，
   `pending_func_states` 会把它们也一并清理。

> **热重载（reload）** 是这个生命周期里最复杂的环节：为了替换仍被
> 外部 Callable 引用的旧函数，编译器维护 `FunctionLambdaInfo` /
> `ScriptLambdaInfo` 结构以识别函数身份，再用
> `_get_function_ptr_replacements` 打出一张旧指针→新指针的映射表，
> 由 `GDScript` 全局替换。这一机制说明：`GDScriptFunction` 的“指针等同
> 于身份”假设并不总成立，而 `name + parent + line + arg_count` 才是
> 真正的身份签名。

---

## 小结

`GDScriptFunction` 是一个看起来“字段堆叠”但实际经过精心优化的对象。
它有两大主设计原则：

1. **构建期数据 / 运行期视图分离**——用 `_xxx_ptr` 冻结向量，让主循环
   只做一次间接寻址；
2. **一切地址统一**——self、class、nil 都放进栈头，成员、常量、栈槽
   统一走 `variant_addresses[][]`，使得主循环成为一个纯粹的“指针分派
   机”。

下一章我们将正式进入 `GDScriptFunction::call` 的主循环，看看这些精心
布置的数据是如何被一百多个 OPCODE 消费的。
