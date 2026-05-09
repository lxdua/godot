# 第 17 章　与 Variant、ClassDB 的互操作

GDScript 不是一门“独立”的语言——它的每一个值都是 Godot 的
`Variant`，每一次 `obj.method()` 都最终走到 `MethodBind`/`ClassDB`
或 `Variant` 的某个成员函数。可以说，**GDScript 是 Godot 反射系统的
一个高级前端**：编译器把语法翻译成对反射 API 的调用，VM 只负责按
反射的回参把值放回栈。

本章把这条 “GDScript ↔ 引擎” 的桥梁拆开，看看：

1. `GDScriptDataType` 是怎么把 Godot 的多套类型系统（Variant 内建、
   引擎 NativeClass、Script、自指 GDScript）统一表达的；
2. 类型校验 `is_type` 在赋值/参数传递/返回值中的具体角色；
3. VM 中四组互操作 Opcode：`CONSTRUCT*`、`CALL_METHOD_BIND*`、
   `CALL_BUILTIN*`、`CALL_UTILITY*`，以及它们背后的 ClassDB / Variant
   接口；
4. `GDScriptUtilityFunctions` 与 `Variant::utility` 的双轨工具函数
   注册——一个隐藏在 GDScript 内部、一个由整个引擎共享。

涉及的核心文件：

* `modules/gdscript/gdscript_function.h`：`GDScriptDataType`
* `modules/gdscript/gdscript_vm.cpp`：互操作 Opcode 的实现
* `modules/gdscript/gdscript_utility_functions.cpp`：GDScript 专属
  工具函数表
* `core/variant/variant_utility.cpp`（仅引用）：`Variant::utility`
* `modules/gdscript/gdscript_utility_callable.cpp`：`Callable` 形态
  的工具函数封装

---

## 17.1 类型系统总览：GDScript 站在三套类型之上

Godot 内部至少存在三套独立的“类型系统”：

| 系统 | 表示 | 例子 |
|------|------|------|
| Variant 内建类型 | `Variant::Type` 枚举 | `INT`、`STRING`、`VECTOR2`、`PACKED_INT32_ARRAY` |
| 引擎 NativeClass | `StringName` + `ClassDB` | `Node`、`CharacterBody2D`、`Resource` |
| 脚本类 | `Ref<Script>`（GDScript / C# / GDExtension） | `res://enemy.gd`、`Behavior` 内部类 |

GDScript 用户从不区分这三者——`var x: Node` 和 `var y: int` 写法
完全一样，类型注解可以是任何身份的“类型符号”。要让编译器、VM、
反射 API 都能以同一种方式处理这些类型，就需要一个统一表示——这就
是 `GDScriptDataType`：

```cpp
class GDScriptDataType {
public:
    enum Kind {
        VARIANT,   // 任意类型（无注解）
        BUILTIN,   // Variant 内建类型
        NATIVE,    // 引擎 NativeClass
        SCRIPT,    // 任意 Script（C# / GDExtension）
        GDSCRIPT,  // GDScript 类（特化优化）
    };

    Kind          kind            = VARIANT;
    Variant::Type builtin_type    = Variant::NIL;  // BUILTIN/NATIVE: 总是 OBJECT
    StringName    native_type;                     // NATIVE/SCRIPT: 引擎类名
    Script       *script_type     = nullptr;       // SCRIPT/GDSCRIPT: 指针引用
    Ref<Script>   script_type_ref;                 // SCRIPT/GDSCRIPT: Ref 持有

    Vector<GDScriptDataType> container_element_types; // Array[T]/Dictionary[K, V]
};
```

* `kind == VARIANT`：未声明类型——用户完全没写类型注解、动态行为。
* `kind == BUILTIN`：值就是 `Variant::int`、`String`、`Vector2` 这种
  内建类型，`builtin_type` 给出具体枚举。
* `kind == NATIVE`：值是 `Object*`（即 `builtin_type == OBJECT`）且
  必须 is-a `native_type` 指定的引擎类。
* `kind == SCRIPT` / `GDSCRIPT`：再进一步——必须 is-a 某个具体
  Script。`GDSCRIPT` 是 `SCRIPT` 的子情况，单独抽出便于热路径快速
  判断。
* `container_element_types`：用于 `Array[int]` / `Dictionary[String,
  Vector2]` 这种泛型容器的元素类型——因为 Variant 的 Array/Dictionary
  本身没有元素类型槽，GDScript 需要在外层把它补回来。

> 把 `script_type` 同时存裸指针**和** `Ref<>` 是个细节：**热路径**
> （类型判断、参数传递）走裸指针，避免引用计数原子操作；**生命期
> 保活**走 `Ref<>`，保证脚本不会在被作为类型注解使用期间被释放。

---

## 17.2 `is_type`：类型校验的统一入口

```cpp
bool GDScriptDataType::is_type(const Variant &p_variant,
                               bool p_allow_implicit_conversion = false) const;
```

实现要点（精简版逻辑）：

| `kind`     | 校验                                                         |
|------------|--------------------------------------------------------------|
| `VARIANT`  | 总是返回 `true`                                              |
| `BUILTIN`  | `p_variant.get_type() == builtin_type`，可选数值隐式转换     |
| `NATIVE`   | `Object::cast_to<>` 到 `native_type`                         |
| `SCRIPT`   | `obj->get_script_instance()->get_script()` 是 `script_type` 或其后代 |
| `GDSCRIPT` | 同上，但只走 `Ref<GDScript>` 的快路径                        |

这个函数被无数地方调用：

* `GDScriptInstance::set` 在写入字段前做类型校验（第 16 章）；
* 函数调用前对每个实参做检查（VM 中的 `OPCODE_CALL_*_TYPED_*` 系列）；
* `OPCODE_RETURN_TYPED_*` 在返回前最后校验一次；
* Analyzer 在常量折叠期检查字面量是否符合声明类型。

`p_allow_implicit_conversion` 控制 “int → float” 这种 GDScript 允
许的隐式 Variant 转换是否参与判断——绝大多数热路径上是 `true`，
保持 GDScript 的弱类型亲和性；只有在 Analyzer 做严格检查时才传
`false`。

---

## 17.3 互操作 Opcode 全景

VM 中所有“离开纯 Variant 计算、调引擎反射或构造对象”的指令大致分
为四组：

```
CONSTRUCT*            ── 构造内建/对象类型
CALL_METHOD_BIND*     ── 调 Object 上的 ClassDB MethodBind
CALL_BUILTIN_*        ── 调 Variant 类型的成员/静态方法
CALL_NATIVE_STATIC*   ── 调 ClassDB 注册的静态/工厂方法
CALL_UTILITY*         ── 调 Variant::utility / GDScript 工具函数
```

每组又细分出几条变体，按“是否经过类型验证”“是否需要返回值”决定。
下面分别看典型代表。

---

## 17.4 `OPCODE_CONSTRUCT*`：构造的两条路径

GDScript 中 `Vector2(1, 2)`、`Color.from_rgba(...)` 这样的“类型名直接
调用”实际上对应 Variant 的 **构造器分发**：

* `OPCODE_CONSTRUCT(type, argc, args, dst)`
  通用慢路径，编译器没法在编译期确定具体重载时使用。运行时调
  `Variant::construct(type, dst, argv, argc, err)`，由 Variant 内部
  根据签名找匹配的构造器。

* `OPCODE_CONSTRUCT_VALIDATED(constructor_idx, args, dst)`
  快路径——编译期已经选定具体构造函数指针（`Variant::ValidatedConstructor`），
  运行时直接调用，**不查表、不报错**。

* `OPCODE_CONSTRUCT_ARRAY` / `OPCODE_CONSTRUCT_DICTIONARY`
  专门处理 `[a, b, c]` / `{k: v}` 字面量，比通用 CONSTRUCT 更紧凑。

* `OPCODE_CONSTRUCT_TYPED_ARRAY` / `OPCODE_CONSTRUCT_TYPED_DICTIONARY`
  对应 `Array[int]` / `Dictionary[String, Vector2]`——构造完后再调
  `set_typed(...)` 把元素类型钉死，使后续插入会自动校验。

“通用慢路径 + 验证版快路径” 这种二元结构会在所有 CALL_* 指令里反
复出现——它的本质是：**Analyzer 能确定类型时把查表步骤搬到编译
期**，运行时省掉一次 hash 查询和参数校验。

---

## 17.5 `OPCODE_CALL_METHOD_BIND*`：经由 ClassDB 调引擎方法

这是和引擎对话最重要的指令。看核心实现：

```cpp
OPCODE(OPCODE_CALL_METHOD_BIND)
OPCODE(OPCODE_CALL_METHOD_BIND_RET) {
    bool call_ret = (_code_ptr[ip]) == OPCODE_CALL_METHOD_BIND_RET;
    LOAD_INSTRUCTION_ARGS
    CHECK_SPACE(3 + instr_arg_count);
    ip += instr_arg_count;

    int argc = _code_ptr[ip + 1];
    MethodBind *method = _methods_ptr[_code_ptr[ip + 2]];

    GET_INSTRUCTION_ARG(base, argc);

#ifdef DEBUG_ENABLED
    bool freed = false;
    Object *base_obj = base->get_validated_object_with_check(freed);
    if (freed)         { err_text = METHOD_CALL_ON_FREED_INSTANCE_ERROR(method); OPCODE_BREAK; }
    else if (!base_obj){ err_text = METHOD_CALL_ON_NULL_VALUE_ERROR(method);     OPCODE_BREAK; }
#else
    Object *base_obj = base->operator Object *();
#endif

    Variant **argptrs = instruction_args;
    Variant temp_ret;
    Callable::CallError err;
    if (call_ret) {
        GET_INSTRUCTION_ARG(ret, argc + 1);
        temp_ret = method->call(base_obj, (const Variant **)argptrs, argc, err);
        *ret = temp_ret;
    } else {
        temp_ret = method->call(base_obj, (const Variant **)argptrs, argc, err);
    }
    // ... DEBUG 错误格式化 ...
    ip += 3;
}
DISPATCH_OPCODE;
```

几个关键点：

### 17.5.1 `MethodBind` 是预先索引好的

```cpp
MethodBind *method = _methods_ptr[_code_ptr[ip + 2]];
```

`_methods_ptr` 是 `GDScriptFunction` 在编译期就构建好的查找表
（第 9 章的常量池家族之一）：每个被调用的 `ClassDB::class_get_method`
结果会被存进函数本地的 `methods` 数组，字节码里只携带一个 16/24 位
索引。这样**所有 ClassDB 的 hash 查找都在编译期一次性完成**，运行
时只是数组下标。

### 17.5.2 `freed` 检查与开发期诊断

`get_validated_object_with_check(freed)` 是 Godot 4 引入的“被释放
对象侦测”——通过 `ObjectDB` 检查 ObjectID 是否仍然有效。`DEBUG_ENABLED`
时如果对象已被释放，会给出明确错误信息；release 构建中走裸指针
直接调用，最大化性能。

### 17.5.3 `call` vs `free` 的特殊化提示

```cpp
if (methodstr == "call") {
    if (argc >= 1) { methodstr = String(*argptrs[0]) + " (via call)"; ... }
} else if (methodstr == "free") {
    if (err.error == CALL_ERROR_INVALID_METHOD) {
        if (base->is_ref_counted()) err_text = "Attempted to free a RefCounted object.";
        else if (...) err_text = "Attempted to free a locked object (...).";
    }
}
```

这是给两个常用 API 加的“贴心错误信息”——遇到错误时识别出是
`call(method, args)` 或 `free()`，给出比通用错误更明确的提示。属于
**用户体验向的硬编码**，可以理解为对“高频踩坑点”的针对性优化。

### 17.5.4 验证版：`CALL_METHOD_BIND_VALIDATED_*`

```
OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN
OPCODE_CALL_METHOD_BIND_VALIDATED_NO_RETURN
```

如果 Analyzer 确定了所有参数类型且与 MethodBind 签名完全匹配，编译
器会发出验证版指令。它直接走 `MethodBind::validated_call(...)`——
跳过参数类型检查与转换，直接 push 到原生函数。这是 GDScript 在静态
类型场景下能逼近 C++ 性能的关键。

---

## 17.6 `OPCODE_CALL_BUILTIN_TYPE_VALIDATED`：Variant 自身的方法

`a_string.length()`、`array.push_back(x)` 这种调用对应的目标不是
`Object` 上的 `MethodBind`，而是 `Variant` 自带的“内建方法”。

```cpp
OPCODE(OPCODE_CALL_BUILTIN_TYPE_VALIDATED) {
    LOAD_INSTRUCTION_ARGS
    // ...
    Variant::ValidatedBuiltInMethod method
            = _builtin_methods_ptr[_code_ptr[ip + 2]];
    // 调用：method(base, args, argc, ret);
    ip += 3;
}
```

`Variant::ValidatedBuiltInMethod` 是个函数指针，由
`Variant::get_validated_builtin_method(type, name)` 在编译期取得。
所有 Variant 类型的成员方法都注册在 Variant 内部表里，VM 这里只是
取出索引并直接调用——和 ClassDB 的 MethodBind 路径几乎对称，但调
用目标不是 `Object`，而是 `Variant` 自身的“类型成员函数”。

`OPCODE_CALL_BUILTIN_STATIC` 是它的“无 base”版本——对应 `Color.html(...)`
这种类型上的静态方法。

---

## 17.7 `OPCODE_CALL_NATIVE_STATIC*`：ClassDB 静态方法

引擎里有一些方法是 `static_function` 注册的，例如 `Time.get_unix_time_from_system()`。
它们没有 `self`，不能用 `OPCODE_CALL_METHOD_BIND`（那个要求第一个
参数是 Object）。所以专门有：

```
OPCODE_CALL_NATIVE_STATIC
OPCODE_CALL_NATIVE_STATIC_VALIDATED_RETURN
OPCODE_CALL_NATIVE_STATIC_VALIDATED_NO_RETURN
```

实现上同样是 `MethodBind* method = _methods_ptr[idx]`，但调用时
`base` 传 `nullptr`。验证版按是否需要返回值分两条——和 17.5.4 的
分裂规则一致，都是为了让指令解码时少一次条件分支。

---

## 17.8 工具函数的双轨：`Variant::utility` vs `GDScriptUtilityFunctions`

GDScript 用户能直接写 `print(...)`、`load(...)`、`range(...)`、
`is_instance_of(...)`——这些“没有 `self`”的全局函数实际上分布在
**两个独立的注册表**里：

### 17.8.1 引擎共享：`Variant::utility`

`core/variant/variant_utility.cpp` 中通过
`Variant::register_utility_function(...)` 注册了大量数学/类型工具：
`abs`、`sin`、`cos`、`min`、`max`、`weakref`、`type_string`、
`bytes_to_var`……这些是 **所有脚本语言都能用的** 工具，C# / GDExtension
也能调它们。

GDScript 编译器看到 `abs(x)` 时调用：

```cpp
Variant::has_utility_function(name)   // 是否注册过
Variant::call_utility_function(name, ret, args, argc, err)
```

### 17.8.2 GDScript 专属：`GDScriptUtilityFunctions`

`modules/gdscript/gdscript_utility_functions.cpp` 里维护另一张表：

```cpp
static AHashMap<StringName, GDScriptUtilityFunctionInfo> utility_function_table;
static List<StringName>                                  utility_function_name_table;
```

通过宏 `REGISTER_FUNC` 注册一组**专属于 GDScript** 的工具：

```cpp
void GDScriptUtilityFunctions::register_functions() {
    REGISTER_FUNC( _char,          true,  RET(STRING),        ARGS(...), false, varray());
    REGISTER_FUNC( ord,            true,  RET(INT),           ARGS(...), false, varray());
    REGISTER_FUNC( range,          false, RET(ARRAY),         NOARGS,    true,  varray());
    REGISTER_FUNC( load,           false, RETCLS("Resource"), ARGS(...), false, varray());
    REGISTER_FUNC( print_debug,    false, RET(NIL),           NOARGS,    true,  varray());
    REGISTER_FUNC( print_stack,    false, RET(NIL),           NOARGS,    false, varray());
    REGISTER_FUNC( get_stack,      false, RET(ARRAY),         NOARGS,    false, varray());
    REGISTER_FUNC( len,            true,  RET(INT),           ARGS(...), false, varray());
    REGISTER_FUNC( is_instance_of, true,  RET(BOOL),          ARGS(...), false, varray());
}
```

为什么这些不放进 `Variant::utility`？

* **`load` / `print_stack` / `get_stack`** 依赖 GDScript 调用栈或脚本
  缓存，与运行时强耦合，不适合作为通用 Variant 工具。
* **`range`** 返回的是 `Array`，但语义上 GDScript 里允许 `for i in
  range(...)`，需要按 GDScript 的语义实现，与其它语言可能不同。
* **`is_instance_of`** 在 GDScript 中要识别“`TYPE_*` 常量 / 引擎类
  / 脚本类”三种参数，依赖 GDScript 类型系统的全部细节。
* **`len`** 因 GDScript 的容器语义而存在，行为微妙，不适合泛化到
  其它语言。

也就是说：**第二张表是承接“与 GDScript 语义紧耦合”的工具函数**，
它们必须在 GDScript 模块内实现。

### 17.8.3 编译器的查找顺序

`GDScriptAnalyzer` 与 `GDScriptCompiler` 检查函数名时按顺序查：

```cpp
if (Variant::has_utility_function(name)) {
    // 走 Variant utility 路径
} else if (GDScriptUtilityFunctions::function_exists(name)) {
    // 走 GDScript utility 路径
}
```

—— 引擎共享的优先级更高。这避免了 GDScript 模块意外覆盖通用工具的
名字。

### 17.8.4 VM 的双重 Opcode

对应到字节码层面也是双轨的：

```
OPCODE_CALL_UTILITY              ← GDScript utility，慢路径，按名字查
OPCODE_CALL_UTILITY_VALIDATED    ← GDScript utility，快路径，按指针调

(Variant utility 走的是另一组：见 OPCODE_CALL_GDSCRIPT_UTILITY 注释 / 类似指令)
```

VM 里取出函数指针的逻辑：

```cpp
GDScriptUtilityFunctions::FunctionPtr function = _gds_utilities_ptr[_code_ptr[ip + 2]];
function(ret, argv, argc, err);
```

`_gds_utilities_ptr` 是 `GDScriptFunction` 的另一个常量池——
`gds_utilities` 数组，编译期填充。和 `_methods_ptr` 同构。

---

## 17.9 `Callable` 形态的工具函数：`GDScriptUtilityCallable`

工具函数有时候要被当作一等公民传递，例如 `Array.map(my_func)`、
`callable.bind(...)`。`my_func` 必须是个 `Callable`，但
`GDScriptUtilityFunctions` 注册的只是函数指针——所以专门有
`GDScriptUtilityCallable`：

```cpp
class GDScriptUtilityCallable : public CallableCustom {
    GDScriptUtilityFunctions::FunctionPtr gdscript_function = nullptr;
    StringName function_name;
    // ...
public:
    void call(const Variant **p_arguments, int p_argcount,
              Variant &r_return_value, Callable::CallError &r_call_error) const override {
        if (gdscript_function) {
            gdscript_function(&r_return_value, p_arguments, p_argcount, r_call_error);
        }
    }
    int get_argument_count(bool &r_is_valid) const override {
        return GDScriptUtilityFunctions::get_function_argument_count(function_name);
    }
    GDScriptUtilityCallable(const StringName &p_function_name) {
        function_name = p_function_name;
        if (GDScriptUtilityFunctions::function_exists(p_function_name)) {
            gdscript_function = GDScriptUtilityFunctions::get_function(p_function_name);
        }
    }
};
```

它继承 `CallableCustom`（与第 13 章的 `GDScriptLambdaCallable`、
第 14 章的 `GDScriptRPCCallable` 是同一族），把 GDScript 工具函数
适配成可绑定、可序列化、可比较的 `Callable`。

`Variant::utility` 那一边因为已经是引擎共享接口，直接由
`Callable::create_for_callable_function` 处理——不需要 GDScript 自
己包一层。

---

## 17.10 类型注解如何落到 ClassDB

最后一个值得说的链路：用户写 `func foo(node: Node) -> Resource:`，
这套类型注解最终是怎么被 ClassDB 看到的？

1. **Parser** 把 `Node` 与 `Resource` 解析成 `IdentifierNode`；
2. **Analyzer** 在 `resolve_datatype()` 中查全局类型表，将其转换成
   `GDScriptDataType{ kind = NATIVE, native_type = "Node" }`；
3. **Compiler** 把 `GDScriptDataType` 写入 `GDScriptFunction::argument_types`
   与 `return_type`（第 9 章的字段）；
4. **`GDScriptFunction::get_method_info()`** 在被引擎反射查询时，把
   这些 `DataType` 转换成 `PropertyInfo`：

   * `kind == NATIVE`： `PropertyInfo(Variant::OBJECT, name,
     PROPERTY_HINT_RESOURCE_TYPE, native_type)`
   * `kind == BUILTIN`：`PropertyInfo(builtin_type, name)`
   * `kind == SCRIPT/GDSCRIPT`：和 NATIVE 类似，但 hint 字符串拼上
     脚本路径

5. 引擎通过 `Object::get_method_list()` → `ScriptInstance::get_method_list()`
   → `Script::get_script_method_list()` 把这些 `MethodInfo` 暴露给
   inspector / 其它语言 / 文档生成器。

也就是说：**GDScript 的类型注解只对自己生成的字节码有意义**（用于
`is_type` 校验），但**通过 PropertyInfo 反射出去后，可以让任何
Godot 工具理解 GDScript 函数的签名**——这是“GDScript 是引擎一等公
民”的具体形式。

---

## 17.11 设计回顾

GDScript 与 Variant/ClassDB 互操作的设计可总结为四点：

1. **统一类型表示 `GDScriptDataType`**：把三套独立类型系统折叠成一
   个枚举驱动的结构，使 Analyzer / Compiler / VM / 反射四端都能用同
   一种语言谈类型。
2. **慢路径 + 验证路径的二元 Opcode**：所有反射调用都按“是否能在
   编译期确定签名”分裂出两条指令，验证路径直接调函数指针，把 hash
   与类型校验全压到编译期。
3. **MethodBind/Constructor/UtilityFunc 全数索引化**：字节码里只携
   带函数本地池中的小整数索引，避免运行时按名字查表。
4. **工具函数的双轨注册**：引擎共享工具走 `Variant::utility`，与
   GDScript 强耦合的工具走 `GDScriptUtilityFunctions`——按职责切分
   而非按“用户感知”切分，避免功能漂移。

这四个设计共同保证了：GDScript 既能在脚本语言层面保持简洁，又能在
反射/类型校验/序列化/文档生成上享受引擎的全部能力——而所有 “反射”
层的调用都通过编译期索引化压平到了 O(1) 的常数开销。

---

## 小结

* `GDScriptDataType` 用 5 种 `Kind`（VARIANT / BUILTIN / NATIVE /
  SCRIPT / GDSCRIPT）统一表达 GDScript 的类型系统；
* `is_type` 是类型校验的统一入口，被 `set` / 参数传递 / 返回值 /
  常量折叠等场景调用；
* 互操作 Opcode 按目标分四组：`CONSTRUCT*` / `CALL_METHOD_BIND*` /
  `CALL_BUILTIN_*` / `CALL_NATIVE_STATIC*` / `CALL_UTILITY*`；
* 每组都有 “通用慢路径 + 验证版快路径” 的二元结构——验证版直接调
  函数指针，跳过类型检查；
* 引擎共享工具走 `Variant::utility`，GDScript 紧耦合工具走
  `GDScriptUtilityFunctions`，编译器优先级前者高于后者；
* `GDScriptUtilityCallable` 把 GDScript 工具函数适配成 `CallableCustom`，
  使其可被 `Array.map`、`bind` 等 API 当一等公民传递；
* 类型注解最终通过 `PropertyInfo` 反射出去，让整个引擎的工具链都能
  理解 GDScript 函数签名。

至此第五部分（类型与实例）全部完成。下一部分将进入“资源与缓存”：
第 18 章看 `.gd` 文件如何被 `ResourceLoader` 加载，第 19 章看
`GDScriptCache` 如何用浅/全两级缓存破解循环依赖。
