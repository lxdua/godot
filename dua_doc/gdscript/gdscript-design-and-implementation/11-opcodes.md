# 第 11 章　Opcode 详解

到这一章，我们已经学完了**字节码是怎么生成的**（第 7 章）、**字节码长
什么样**（第 8 章）和**字节码是怎么被分派的**（第 10 章）。剩下的最后
一块拼图就是：**每一类 OPCODE 究竟在做什么？**

GDScript 在 `enum Opcode` 中定义了大约 160 个操作码，其中绝大部分都是
“同一家族的特化版本”。本章不会逐条复述源码——那会变成一份厚重的
reference manual——而是把这 160 个 OPCODE **按职能聚成 12 个家族**，
对每个家族讲清楚：

* **它解决什么问题？**
* **典型的指令布局是什么样？**
* **特化版本相对通用版本快在哪？**
* **生成端（第 7 章）什么时候会选择哪一个？**

读完后，再去看任意一段 `disassemble()` 输出或者 `gdscript_vm.cpp` 实现
都能立刻对号入座。

> 本章约定：用 `[a]`、`[b]`、`[dst]` 表示 24 位地址槽，`<n>` 表示一个
> 索引/数量，`{ptr}` 表示一个被打包成若干 `int` 的 C++ 指针。

---

## 11.1 全景图：从枚举到分组

把第 9 章贴出的 `enum Opcode` 按“最小变种数”归类，可以得到下面 12 类：

| 家族 | 代表 OPCODE | 数量 | 说明 |
| --- | --- | --- | --- |
| 1. 算术与逻辑 | `OPERATOR`、`OPERATOR_VALIDATED` | 2 | 二元 / 一元运算 |
| 2. 类型测试 | `TYPE_TEST_BUILTIN/ARRAY/DICTIONARY/NATIVE/SCRIPT` | 5 | `is` / `is_instance_of` 的实现 |
| 3. 容器 set/get | `SET_KEYED`、`SET_NAMED`、`GET_KEYED`、`GET_NAMED` 及其 VALIDATED/INDEXED 变种 | 12 | 下标、键、命名属性访问 |
| 4. 成员访问 | `SET_MEMBER`、`GET_MEMBER`、`SET/GET_STATIC_VARIABLE` | 4 | 类成员、静态变量 |
| 5. 赋值与字面量 | `ASSIGN`、`ASSIGN_NULL/TRUE/FALSE`、`ASSIGN_TYPED_*` | 9 | 普通赋值、类型化赋值 |
| 6. 类型转换 | `CAST_TO_BUILTIN/NATIVE/SCRIPT` | 3 | `as` 运算符 |
| 7. 构造 | `CONSTRUCT*`、`CONSTRUCT_ARRAY/DICTIONARY` 及类型化变种 | 6 | Variant 构造、字面量 Array/Dict |
| 8. 调用 | `CALL` 系列、`CALL_METHOD_BIND_*`、`CALL_BUILTIN_*`、`CALL_UTILITY_*` | 14 | 各种函数/方法调用 |
| 9. 协程 | `AWAIT`、`AWAIT_RESUME` | 2 | 暂停/恢复（详见第 12 章）|
| 10. Lambda | `CREATE_LAMBDA`、`CREATE_SELF_LAMBDA` | 2 | 闭包构造（详见第 13 章）|
| 11. 控制流 | `JUMP*`、`RETURN*`、`ITERATE*` | 38 | 跳转、返回、迭代 |
| 12. 杂项 | `STORE_GLOBAL`、`STORE_NAMED_GLOBAL`、`TYPE_ADJUST_*`、`ASSERT`、`BREAKPOINT`、`LINE`、`END` | 32 | 全局加载、类型修正、调试 |

> **观察一**：表里 “6 + 9 + 38 + 32 = 85” 个 OPCODE 来自“特化”——它们
> 是普通版本的快速路径。**核心 OPCODE 其实只有 70 多个**，剩下的全是
> 性能优化。
>
> **观察二**：被特化最多的家族是 **迭代器**（`ITERATE_BEGIN_*` 与
> `ITERATE_*` 各有 18 个），因为 `for x in arr` 是脚本中最热的循环模式
> 之一。

接下来按家族展开。

---

## 11.2 家族 1：算术与逻辑（`OPERATOR` 系列）

### 指令布局

```
OPERATOR            [a] [b] [dst] <op> <op_signature> <ret_type> {ValidatedEvaluator}
OPERATOR_VALIDATED  [a] [b] [dst] <op_func_index>
```

`OPERATOR` 是“通用 + 内联缓存”的形态，第 10.5 节已经详细分析过它的三
路分支（首次查表 → 写回缓存 → 命中 / 未命中）。

`OPERATOR_VALIDATED` 是“静态特化”的形态：编译期就已经确定了 a、b 类型
和操作符，因此可以直接查 `_operator_funcs_ptr[op_func_index]` 拿到一个
`Variant::ValidatedOperatorEvaluator` 函数指针。

### 何时生成哪一个？

`GDScriptByteCodeGenerator::write_binary_operator` 的判定大致是：

```cpp
if (a 与 b 都是已知 BUILTIN 且不是 Variant && operator 有 validated 实现) {
    write OPERATOR_VALIDATED;
} else {
    write OPERATOR;
}
```

也就是说**只要类型在分析阶段已经被收紧成 BUILTIN**，就走特化版本；否则
退化成 OPERATOR + 内联缓存。这条规则解释了 GDScript 性能优化的一个朴素
事实：**给变量加上类型注解，往往能让一段表达式从两条 OPERATOR 变成两条
OPERATOR_VALIDATED**。

### 一元运算

GDScript 把一元运算（`-x`、`not x`、`~x`）归到 `Variant::Operator` 的
枚举里，复用同样的两条 OPCODE，只是指令布局上 `[b]` 槽指向一个
“ADDR_NIL”占位。这是“不为一元单独造 OPCODE”的设计省下了一套实现。

---

## 11.3 家族 2：类型测试（`TYPE_TEST_*`）

GDScript 的 `is` 运算符在编译期会被分派到下面五条之一：

| OPCODE | 测试对象类型 |
| --- | --- |
| `TYPE_TEST_BUILTIN` | 内建 Variant 类型（int、String、Vector2…） |
| `TYPE_TEST_ARRAY` | 类型化 Array，需要比较元素类型 |
| `TYPE_TEST_DICTIONARY` | 类型化 Dictionary，需要比较键/值类型 |
| `TYPE_TEST_NATIVE` | C++ 原生类（Node、Sprite2D…） |
| `TYPE_TEST_SCRIPT` | GDScript / 其他脚本类 |

之所以要拆五条，是因为它们的“证据”取数方式各不相同：

* `BUILTIN` 只比较 `Variant::get_type()`；
* `ARRAY/DICTIONARY` 还要比对容器内部记录的元素类型；
* `NATIVE` 调用 `ClassDB::is_parent_class()`；
* `SCRIPT` 沿着 `Script::get_base_script()` 链向上查。

### 指令布局示例

```
TYPE_TEST_BUILTIN   [dst] [value] <Variant::Type>
TYPE_TEST_NATIVE    [dst] [value] [type_addr]
TYPE_TEST_SCRIPT    [dst] [value] [script_addr]
TYPE_TEST_ARRAY     [dst] [value] [script_type] <builtin_type> <native_type_str_idx>
```

注意 `BUILTIN` 把目标类型直接编码成立即数，而 `NATIVE/SCRIPT` 因为类
对象本身是个 Variant，所以放在常量池里、用地址引用。

---

## 11.4 家族 3：容器 set/get

这是 OPCODE 设计上最“规整”的家族——每一种访问方式都有 4 个变种：

| 访问方式 | 通用 set | 通用 get | 特化 set | 特化 get |
| --- | --- | --- | --- | --- |
| 字符串键 | `SET_NAMED` | `GET_NAMED` | `SET_NAMED_VALIDATED` | `GET_NAMED_VALIDATED` |
| 任意键 | `SET_KEYED` | `GET_KEYED` | `SET_KEYED_VALIDATED` | `GET_KEYED_VALIDATED` |
| 整数下标 | —— | —— | `SET_INDEXED_VALIDATED` | `GET_INDEXED_VALIDATED` |

### 三种语义对比

* **NAMED**：`obj.name`。键必须是 StringName 字面量，编译期就把它存入
  常量池。
* **KEYED**：`obj[key]`，键是任意 Variant，运行期才知道。
* **INDEXED**：`vec.x`、`arr[i]`。仅当编译期能证明“访问目标是某种内建
  容器，下标是个 int”时才能走，例如 `Vector2.x`、`PackedByteArray[i]`。

### 特化版本如何加速？

以 `OPCODE_GET_NAMED_VALIDATED` 为例，它的指令布局多了一个
`<getter_index>`，运行期直接：

```cpp
_getters_ptr[getter_index](src, dst);
```

而通用 `OPCODE_GET_NAMED` 必须走 `Variant::get_named` —— 那是一长串
`switch (variant.type)` 的分发。两者的差距在“String 拼接 + 一次哈希查找”
的代价上。GDScript 的“访问 Vector2.x”这种最常见的字段访问就是借此达到
近似 C++ 的速度。

---

## 11.5 家族 4：成员与静态变量

```
SET_MEMBER  [value] <name_idx>
GET_MEMBER  [dst]   <name_idx>
SET_STATIC_VARIABLE  [value] [class_addr] <slot_index>
GET_STATIC_VARIABLE  [dst]   [class_addr] <slot_index>
```

* `SET_MEMBER / GET_MEMBER` 走 `GDScriptInstance::set/get`，而不是
  `variant_addresses[ADDR_TYPE_MEMBER]` 路径——前者会触发 setter/getter，
  后者只是裸内存访问。生成器只在“无 setter/getter 的简单成员”路径上才会
  采用更高效的 `ADDR_TYPE_MEMBER` 直接寻址。
* `SET/GET_STATIC_VARIABLE` 把目标 GDScript 类传进来，因为 `static var`
  存放在 `GDScript::static_variables` 数组里，需要跨实例访问。

---

## 11.6 家族 5：赋值与字面量

```
ASSIGN          [dst] [src]
ASSIGN_NULL     [dst]
ASSIGN_TRUE     [dst]
ASSIGN_FALSE    [dst]
ASSIGN_TYPED_BUILTIN     [dst] [src] <Variant::Type>
ASSIGN_TYPED_ARRAY       [dst] [src] [script_type] <builtin> <native_idx>
ASSIGN_TYPED_DICTIONARY  [dst] [src] [key_script] [value_script] ...
ASSIGN_TYPED_NATIVE      [dst] [src] [type_addr]
ASSIGN_TYPED_SCRIPT      [dst] [src] [script_addr]
```

### 设计要点

* **`ASSIGN_NULL/TRUE/FALSE` 单独成 OPCODE**——这三类常量出现得太频繁
  （初始化、return false、循环条件）。每多一条 OPCODE 就省一个常量池
  槽位、省一次 `GET_VARIANT_PTR`。这种“为常用立即数特化 OPCODE”的取舍
  在很多解释器（CPython、Lua）里也常见。
* **`ASSIGN_TYPED_*` 家族一定带类型转换**：当左值是硬类型而右值不一定
  匹配时，编译器必须保证赋值后左值仍合法。VM 在这里调用
  `Variant::construct(dst_type, src)` 而不是简单的 `*dst = *src`。
* **类型化容器赋值的额外槽位**：`ASSIGN_TYPED_ARRAY` 必须比较元素类型
  以判断“能否原样赋值”还是“需要重建一个新容器”。这就是为什么它的指令
  布局要带 `script_type/builtin/native_idx` 三个槽——能在不查询 Array
  内部的情况下完成验证。

---

## 11.7 家族 6：类型转换（`CAST_*`）

```
CAST_TO_BUILTIN  [src] [dst] <Variant::Type>
CAST_TO_NATIVE   [src] [dst] [type_addr]
CAST_TO_SCRIPT   [src] [dst] [script_addr]
```

`as` 与 `ASSIGN_TYPED_*` 的区别在于：**`as` 失败时返回 null，赋值失败时
报错**。所以 `CAST_*` 家族的实现：

```cpp
if (matches) {
    *dst = *src;
} else if (Object 类) {
    *dst = (Variant)nullptr;
} else {
    err_text = "Trying to assign value of type ...";
    OPCODE_BREAK;
}
```

`CAST_TO_NATIVE` 和 `CAST_TO_SCRIPT` 在“失败但目标是 Object”的情况下
返回 null 而非报错，这正是 GDScript `obj as Node` 的语义。

---

## 11.8 家族 7：构造（`CONSTRUCT_*`）

```
CONSTRUCT             [args...] [dst] <argc> <Variant::Type>
CONSTRUCT_VALIDATED   [args...] [dst] <argc> <constructor_idx>
CONSTRUCT_ARRAY       [args...] [dst] <argc>
CONSTRUCT_TYPED_ARRAY [args...] [script_type] [dst] <argc> <builtin> <native_idx>
CONSTRUCT_DICTIONARY  [k0,v0,k1,v1,...] [dst] <pair_count>
CONSTRUCT_TYPED_DICTIONARY [k,v,...] [k_script] [v_script] [dst] ...
```

### 关键点

* **`CONSTRUCT` 与 `CONSTRUCT_VALIDATED` 的关系**和算术运算家族对应——
  一个走 `Variant::construct`，一个直接调用预查的
  `Variant::ValidatedConstructor`。当编译器能在静态阶段确定参数类型
  组合时，会发 `CONSTRUCT_VALIDATED`。
* **字典的实参顺序是 `key, value, key, value, …`**，配对储存。生成端
  在 `write_construct_dictionary` 里就是按这种顺序把 `instruction_args`
  压进去的。

---

## 11.9 家族 8：调用（`CALL_*`）

调用家族是整个指令集里最庞大也最分散的部分，把它分成 5 个子家族会清晰
得多：

### 11.9.1 通用调用三兄弟

```
CALL          [args..., base, ?ret] <argc> <method_name_idx>
CALL_RETURN   同上，但要求把返回值写入 [ret]
CALL_ASYNC    同上，标记“此调用预期返回 GDScriptFunctionState”
```

三者共享 VM 实现，仅由“是否需要 ret”和“是否标记异步”区分。它们都走
`base->callp(method, argv, argc, ...)`——也就是 Godot 通用方法分派路径。

### 11.9.2 MethodBind 直连

```
CALL_METHOD_BIND      [args..., base] <argc> {MethodBind*}
CALL_METHOD_BIND_RET  [args..., base, ret] <argc> {MethodBind*}
```

当编译器知道 base 是某个具体的 C++ 类、且 method 已经在 ClassDB 里注册
时，可以直接把 `MethodBind*` 编进字节码。运行期不再做名字查找，直接
`method->call(...)`。

### 11.9.3 类型化的 MethodBind（最快路径）

```
CALL_METHOD_BIND_VALIDATED_RETURN     [args..., base, ret] <argc> {MethodBind*}
CALL_METHOD_BIND_VALIDATED_NO_RETURN  [args..., base]      <argc> {MethodBind*}
```

`VALIDATED` 意味着每个参数都已经过类型检查——VM 可以跳过参数类型适配
直接调用 `MethodBind::validated_call(base, argv, ret)`。这是 GDScript
能与 C# 性能持平的关键路径之一。

### 11.9.4 内建类型方法

```
CALL_BUILTIN_TYPE_VALIDATED [args..., base, ret] <argc> <method_idx>
CALL_BUILTIN_STATIC         [args..., ret] <argc> <Variant::Type> <name_idx>
```

* `BUILTIN_TYPE_VALIDATED` 处理 `vec.normalized()`、`str.to_upper()`
  这种内建类型实例方法，预查的 `Variant::ValidatedBuiltInMethod` 存在
  `_builtin_methods_ptr` 表里。
* `BUILTIN_STATIC` 处理 `Vector2.from_angle(...)` 这种内建类型静态方法。

### 11.9.5 工具与原生静态

```
CALL_UTILITY                              [args..., ret] <argc> <name_idx>
CALL_UTILITY_VALIDATED                    [args..., ret] <argc> <util_idx>
CALL_GDSCRIPT_UTILITY                     [args..., ret] <argc> {GDScriptUtilFunc*}
CALL_NATIVE_STATIC                        [args..., ret] <argc> <class_name_idx> <method_name_idx>
CALL_NATIVE_STATIC_VALIDATED_RETURN       [args..., ret] <argc> {MethodBind*}
CALL_NATIVE_STATIC_VALIDATED_NO_RETURN    [args...]      <argc> {MethodBind*}
```

* `UTILITY` 调用的是 Godot 全局函数（如 `lerp`、`min`、`abs`）；
  `_VALIDATED` 走 `Variant::ValidatedUtilityFunction`，否则走 `callp` 风格。
* `GDSCRIPT_UTILITY` 调用的是 GDScript 专属工具函数（`print`、`type_string`
  等，定义于 `GDScriptUtilityFunctions`）。
* `NATIVE_STATIC` 调用 ClassDB 注册的某个类的 static 方法
  （`OS.get_name()` 之类）。

### 11.9.6 self / super

```
CALL_SELF_BASE  [args..., ret] <argc> <name_idx>
```

`super.foo()` 编成此 OPCODE。它会沿着脚本继承链查找方法，与普通
`base.callp` 不同。

---

## 11.10 家族 9：协程（`AWAIT`、`AWAIT_RESUME`）

`AWAIT` 与 `AWAIT_RESUME` 在生成器里是**成对**写出的：

```cpp
void GDScriptByteCodeGenerator::write_await(
        const Address &p_target, const Address &p_operand) {
    append_opcode(OPCODE_AWAIT);
    append(p_operand);
    append_opcode(OPCODE_AWAIT_RESUME);
    append(p_target);
}
```

布局：

```
AWAIT          [operand]
AWAIT_RESUME   [target]
```

VM 行为：

* **同步路径**：`operand` 不是 Signal——VM 直接读取 `AWAIT_RESUME` 的
  `[target]` 槽，把结果写过去，`ip += 4` 跳过 `AWAIT_RESUME` 字节段。
* **异步路径**：`operand` 是 Signal——VM 创建 `GDScriptFunctionState`、
  连接信号、`OPCODE_BREAK` 退出主循环。等到信号触发时由
  `GDScriptFunctionState::resume()` 重新进入主循环，从
  `AWAIT_RESUME` 处继续，把信号回调参数写入 `[target]`。

详细的语义、唤醒机制和编辑器警告（REDUNDANT/MISSING）请见第 12 章。

---

## 11.11 家族 10：Lambda（`CREATE_LAMBDA`、`CREATE_SELF_LAMBDA`）

```
CREATE_LAMBDA       [captures...] [dst] <captures_count> <lambda_index>
CREATE_SELF_LAMBDA  [captures...] [dst] <captures_count> <lambda_index>
```

VM 行为是“拿到 `_lambdas_ptr[lambda_index]` 这个 `GDScriptFunction*`，
配合捕获列表，构造一个 `GDScriptLambdaCallable` Variant 写入 `dst`”。

`CREATE_SELF_LAMBDA` 比普通版本多做一件事：**把 self 也作为隐式捕获
绑定到 Callable 上**。这样 lambda 即便在 self 析构后被调用，也会通过
`ObjectID` 检查避免野指针。

更深入的捕获机制、生命周期控制留到第 13 章。

---

## 11.12 家族 11：控制流

控制流家族包含三个子类：跳转、返回、迭代。

### 11.12.1 跳转（5 种）

```
JUMP                 <to>
JUMP_IF              [test] <to>
JUMP_IF_NOT          [test] <to>
JUMP_TO_DEF_ARGUMENT
JUMP_IF_SHARED       [val] <to>
```

前三种与 C 风格控制流一致。需要专门讲的是后两个：

* **`JUMP_TO_DEF_ARGUMENT`**：从 `_default_arg_ptr[defarg]` 取目标地址
  跳过去。配合编译期生成的“默认值构造段”实现“按缺省参数个数选择跳点”
  的开关。
* **`JUMP_IF_SHARED`**：当 `val` 是“共享语义”的 Variant（Array、
  Dictionary、Object 引用）时跳转。这是为 `for` 循环临时变量与“按值”
  传递语义服务的——若变量是共享类型，`for` 块退出时不能假设它已被
  析构，需要走特殊的清理路径。

### 11.12.2 返回（5 种）

```
RETURN                  [value]
RETURN_TYPED_BUILTIN    [value] <Variant::Type>
RETURN_TYPED_ARRAY      [value] [script_type] <builtin> <native_idx>
RETURN_TYPED_DICTIONARY [value] [k_script] [v_script] ...
RETURN_TYPED_NATIVE     [value] [type_addr]
RETURN_TYPED_SCRIPT     [value] [script_addr]
```

`RETURN_TYPED_*` 与 `ASSIGN_TYPED_*` 完全对称——都做“返回前再确认一次
类型契约”。生成端的判定也一致：硬类型函数 → 走 TYPED 版本；变体
返回类型 → 走通用 `RETURN`。

### 11.12.3 迭代（18 + 18 种）

GDScript 把 `for` 循环编译成两条 OPCODE 的循环：`ITERATE_BEGIN_*` 在
进入循环前调用一次（初始化迭代状态、若容器为空就跳到循环末端），
`ITERATE_*` 在每次循环末尾调用（推进迭代器、若没有更多元素就跳出）。

| 通用 | 特化族 |
| --- | --- |
| `ITERATE_BEGIN` / `ITERATE` | `_INT`、`_FLOAT`（仅范围 `for i in N`）<br>`_VECTOR2`、`_VECTOR2I`、`_VECTOR3`、`_VECTOR3I`、`_STRING`<br>`_DICTIONARY`、`_ARRAY`<br>`_PACKED_BYTE_ARRAY` … `_PACKED_VECTOR4_ARRAY`<br>`_OBJECT`（用户自定义 `_iter_init/_iter_next/_iter_get`）<br>`_RANGE`（`for x in range(...)`）|

特化版本的核心收益是**省掉 `Variant::iter_init/iter_get` 的多态分发**，
直接按已知容器类型走 C++ 索引。例如 `OPCODE_ITERATE_BEGIN_INT` 实现
仅需：

```cpp
int64_t size = *VariantInternal::get_int(container);
VariantInternal::initialize(counter, Variant::INT);
*VariantInternal::get_int(counter) = 0;
if (size > 0) {
    GET_VARIANT_PTR(iterator, 2);
    VariantInternal::initialize(iterator, Variant::INT);
    *VariantInternal::get_int(iterator) = 0;
    ip += 5; // 跳过紧随其后的 ITERATE
} else {
    ip = _code_ptr[ip + 4]; // 跳到循环末
}
```

这一段不会触碰 `Variant::iter_*`，几乎就是裸 C 整数循环。

> **`for x in range(...)` 走 `_RANGE`**：分析器会识别出
> `range(N) / range(a, b) / range(a, b, c)` 这一组特殊调用，编译器以
> `OPCODE_ITERATE_BEGIN_RANGE` 取代“先构造 PackedInt32Array 再迭代”的
> 通用流程，进一步消除中间分配。

---

## 11.13 家族 12：杂项

### `STORE_GLOBAL` / `STORE_NAMED_GLOBAL`

```
STORE_GLOBAL         [dst] <global_index>
STORE_NAMED_GLOBAL   [dst] <name_idx>
```

* `STORE_GLOBAL` 直接从 `GDScriptLanguage::singleton->global_array`
  取值（`GDScriptLanguage` 在引擎启动时把 `Vector2`、`Color`、`Engine`、
  `OS` 等全局符号注册进这个数组，索引在编译期就固定下来）。
* `STORE_NAMED_GLOBAL` 用名字查 `ProjectSettings`/`autoload` 风格的
  命名全局，慢但灵活。

### `TYPE_ADJUST_*`

`TYPE_ADJUST_*` 家族针对每种 Variant 类型各有一条 OPCODE。它的指令
布局只有 `[dst]`，作用是“把 `dst` 槽强制重置为该类型的默认值”。

它的存在是为了配合**类型化局部变量**：

```python
var pos: Vector2     # 编译期会先发 TYPE_ADJUST_VECTOR2 [pos]
pos.x = 3            # 后续 SET_NAMED_VALIDATED 假设 pos 已经是 Vector2
```

如果跳过 `TYPE_ADJUST_*`，`pos` 在第一次出现时是 NIL，后续的特化访问
就会因为类型不匹配直接走慢路径。第 9 章提到过 `temporary_slots` 也会
触发同样的“按类型预初始化”——两者本质是同一种思想，只是触发时机不同
（`temporary_slots` 是函数入口一次性，`TYPE_ADJUST_*` 是局部块入口
按需）。

### `ASSERT`、`BREAKPOINT`、`LINE`、`END`

```
ASSERT      [test] [message]
BREAKPOINT
LINE        <line_no>
END
```

* `ASSERT` 在 DEBUG 构建下激活，运行 `test`，若为假打印 `message` 并
  触发调试器中断；Release 构建下编译器根本不发这条 OPCODE。
* `BREAKPOINT` 是 `breakpoint` 关键字（GDScript 内置的硬断点）。
* `LINE` 在第 10 章已经分析。
* `END` 是函数末尾的“安全网”——若执行到这里说明走到了字节码末端
  仍未 RETURN，`exit_ok = true` 后退出主循环，返回值是 NIL。

---

## 11.14 一段示例的反汇编对照

为了把上述家族串起来，看下面这段 GDScript：

```python
func _ready() -> void:
    var v := Vector2(1, 2)
    var sum := 0
    for i in 10:
        sum += int(v.x) * i
    print(sum)
```

类型完全确定，分析器收紧后，编译器倾向选用最特化的 OPCODE。反汇编
结果（精简版）大致如下：

```
0:  CONSTRUCT_VALIDATED [const(1.0)] [const(2.0)] [v]   2  <Vector2_ctor>
6:  ASSIGN_TYPED_BUILTIN [sum] [const(0)] <INT>
10: ITERATE_BEGIN_INT [counter] [const(10)] [i]  end_addr=40
15: GET_NAMED_VALIDATED  [v] [tmp_x] <Vector2.x_getter>
19: CALL_BUILTIN_STATIC  [tmp_x] [tmp_int] 1 <INT> <int_ctor>
26: OPERATOR_VALIDATED [tmp_int] [i] [tmp_mul] <op_mul_int_int>
31: OPERATOR_VALIDATED [sum] [tmp_mul] [sum] <op_add_int_int>
36: ITERATE_INT [counter] [const(10)] [i]  jump_back=15
40: CALL_GDSCRIPT_UTILITY [sum] [_] 1 {print}
46: RETURN [nil]
```

可以看到：构造 / 赋值 / 迭代 / 字段读取 / 二元运算 / 内建调用 / 返回
全部都走了**特化路径**。如果去掉 `: int / : Vector2` 的类型注解，
同一段代码会被编译成全是非特化版本：`ASSIGN`、`OPERATOR`、`ITERATE_BEGIN`、
`GET_NAMED`、`CALL`，运行速度会显著下降。这就是 GDScript 性能调优的
最简单准则的**实现层依据**。

---

## 11.15 设计观察：为什么 GDScript 有这么多 OPCODE？

读完这一章，你也许会问：与 Lua（约 40 个 OPCODE）相比，GDScript 的
160 个 OPCODE 是不是“过度设计”？

事实上，这正是 GDScript 三个独特定位带来的必然：

1. **渐进式类型 + 值类型 Variant**——同一种动作（赋值、构造、调用）
   在“动态”和“静态”两个语义下表现不同，所以每个动作都至少需要两条
   OPCODE。
2. **与 Variant/ClassDB 紧绑定**——内建类型方法、原生 MethodBind 都
   是高频路径，不能落到通用 callp，必须有专门 OPCODE。
3. **以游戏循环为目标**——`for` 与 `print`、`Vector2` 与 `Object` 这种
   组合在脚本里出现得太多，特化收益远超指令集复杂度的代价。

简而言之：**OPCODE 的数量是 Variant 类型数 × 操作种类的笛卡尔积的
结果**，每一个特化都换来了真实的性能提升。这种取舍与 CPython 用
adaptive specialization（PEP 659）做的事情非常相似——只是 GDScript 在
**编译期**就完成了大部分特化决策，不再依赖运行期 profile。

---

## 小结

* GDScript 的 ~160 个 OPCODE 可以归成 **12 个家族**；
* 大约一半 OPCODE 都是“特化版本”，目的明确：**把多态分发提前到编译期**；
* 控制流家族中迭代器占比最高，反映了游戏脚本的典型负载；
* 一段类型完整的代码几乎可以全部走特化路径，性能与 C++ 差距极小；
* 设计哲学：**“能用 Variant 类型 × 操作种类的笛卡尔积换性能”**。

下一章我们将聚焦协程家族——`await` 看似只占 2 条 OPCODE，背后却串起
了 Tokenizer、Parser、Analyzer、Compiler、VM 与 Signal 的完整链路。
