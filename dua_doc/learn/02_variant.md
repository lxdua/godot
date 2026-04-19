
# Variant —— Godot 万能类型容器深度解析

> 📂 核心源码：`core/variant/variant.h`, `core/variant/variant.cpp`, `core/variant/variant_op.cpp`, `core/variant/variant_call.cpp`

---

## 一、Variant 是什么

Variant 是一个**万能盒子**——一个变量，能装下 Godot 引擎中**任意类型**的值。

在 GDScript 中，当你写：

```gdscript
var x = 42
x = "hello"
x = Vector2(1, 2)
x = some_node
```

每一次赋值，底层都是在操作同一个 `Variant`。`x` 的类型在运行时可以随时变，这就是 Variant 的能力。

C++ 是静态类型语言，不支持这种操作。Variant 就是 Godot 在 C++ 中模拟"动态类型"的核心基础设施。

---

## 二、支持的 38 种类型

```cpp
enum Type {
    NIL,                    // null

    // 原子类型（4 种）
    BOOL, INT, FLOAT, STRING,

    // 数学类型（15 种）
    VECTOR2, VECTOR2I, RECT2, RECT2I,
    VECTOR3, VECTOR3I, VECTOR4, VECTOR4I,
    TRANSFORM2D, PLANE, QUATERNION, AABB,
    BASIS, TRANSFORM3D, PROJECTION,

    // 杂项类型（9 种）
    COLOR, STRING_NAME, NODE_PATH, RID,
    OBJECT, CALLABLE, SIGNAL,
    DICTIONARY, ARRAY,

    // 类型化数组（10 种）
    PACKED_BYTE_ARRAY, PACKED_INT32_ARRAY, PACKED_INT64_ARRAY,
    PACKED_FLOAT32_ARRAY, PACKED_FLOAT64_ARRAY,
    PACKED_STRING_ARRAY, PACKED_VECTOR2_ARRAY, PACKED_VECTOR3_ARRAY,
    PACKED_COLOR_ARRAY, PACKED_VECTOR4_ARRAY,

    VARIANT_MAX  // = 38
};
```

这 38 种类型覆盖了游戏开发中几乎所有常用数据。每个 GDScript 变量、每个信号参数、每个属性值，底层都是一个 Variant。

---

## 三、内存布局 —— union + 类型标签

### 3.1 整体结构

Variant 的核心就两个字段：

```cpp
class Variant {
    Type type = NIL;           // 类型标签（4 字节）

    union {
        bool _bool;
        int64_t _int;
        double _float;
        Transform2D *_transform2d;     // 堆上分配（太大了放不下）
        ::AABB *_aabb;                 // 堆上分配
        Basis *_basis;                 // 堆上分配
        Transform3D *_transform3d;     // 堆上分配
        Projection *_projection;       // 堆上分配
        PackedArrayRefBase *packed_array;  // 引用计数的数组
        void *_ptr;                    // 通用指针
        uint8_t _mem[sizeof(ObjData) > (sizeof(real_t) * 4)
            ? sizeof(ObjData) : (sizeof(real_t) * 4)]{ 0 };
    } _data alignas(8);
};
```

### 3.2 大小

源码注释写得很清楚：

```
// Variant takes 24 bytes when real_t is float, and 40 bytes if double.
```

- **float 精度**（默认）：`type`（4 字节）+ padding（4 字节）+ `_data`（16 字节）= **24 字节**
- **double 精度**：`type`（4 字节）+ padding（4 字节）+ `_data`（32 字节）= **40 字节**

### 3.3 哪些类型内联，哪些在堆上

关键设计：**小类型直接塞进 union，大类型用指针指向堆**。

| 存储方式 | 类型 | 原因 |
|---|---|---|
| **内联在 union 中** | bool、int、float、Vector2、Vector2i、Vector3、Vector3i、Vector4、Vector4i、Rect2、Rect2i、Plane、Quaternion、Color、RID | 这些类型 ≤ 16 字节（float 精度），能塞进 `_mem` |
| **堆上分配（指针）** | Transform2D、AABB、Basis、Transform3D、Projection | 这些类型太大了（Projection = 64 字节），union 装不下 |
| **引用计数（PackedArrayRef）** | 所有 PackedArray | 数组可能很大，用引用计数避免拷贝 |
| **特殊处理** | String、StringName、NodePath、Object、Callable、Signal、Array、Dictionary | 放在 `_mem` 中用 placement new 构造 |

### 3.4 ObjData —— Object 的特殊处理

Object 类型不是简单存一个指针，而是存了 `ObjData`：

```cpp
struct ObjData {
    ObjectID id;         // 64 位身份证号
    Object *obj = nullptr;  // 指针
};
```

为什么要同时存 ID 和指针？

- **指针**用于快速访问（不需要查 ObjectDB）
- **ObjectID** 用于安全验证——通过 `get_validated_object()` 检查对象是否还活着

```cpp
Object *Variant::get_validated_object() const {
    if (type == OBJECT) {
        return ObjectDB::get_instance(_get_obj().id);  // 用 ID 验证
    }
    return nullptr;
}
```

如果对象已经被销毁，ObjectDB 会返回 `nullptr`，而不是让你访问一个悬垂指针。

---

## 四、needs_deinit —— 谁需要析构

不是所有类型都需要析构（释放内存、减少引用计数等）。Variant 用一个**编译期常量数组**来标记：

```cpp
static constexpr bool needs_deinit[VARIANT_MAX] = {
    false,  // NIL
    false,  // BOOL
    false,  // INT
    false,  // FLOAT
    true,   // STRING          ← 需要释放字符串内存
    false,  // VECTOR2         ← 纯值类型，不需要
    false,  // VECTOR2I
    // ...
    true,   // TRANSFORM2D     ← 需要 delete 堆上的指针
    // ...
    true,   // OBJECT          ← 如果是 RefCounted，需要减引用
    true,   // ARRAY           ← 需要释放
    true,   // PACKED_*_ARRAY  ← 需要减引用计数
};
```

`clear()` 函数利用这个数组做**快速路径**：

```cpp
_FORCE_INLINE_ void clear() {
    if (unlikely(needs_deinit[type])) {  // 大多数情况走 false 分支
        _clear_internal();                // 只有需要析构的类型才调
    }
    type = NIL;
}
```

`unlikely()` 提示 CPU：大多数情况不需要析构（数值类型最常见），跳过 `_clear_internal()` 的调用。

---

## 五、PackedArrayRef —— 引用计数的数组

PackedArray（如 `PackedByteArray`、`PackedVector2Array`）是 Godot 中高性能的类型化数组。在 Variant 中它们用**引用计数**管理：

```cpp
template <typename T>
struct PackedArrayRef : public PackedArrayRefBase {
    Vector<T> array;              // 实际的数据
    SafeRefCount refcount;        // 原子引用计数
};
```

### 为什么不直接用 `Vector<T>`？

因为 `Vector<T>` 有 COW（Copy-On-Write）语义——赋值时只拷贝指针，修改时才深拷贝。但 Variant 需要更细粒度的控制：

1. **Variant 之间赋值**：只增加 `PackedArrayRef` 的引用计数，不拷贝数据
2. **修改数组内容**：修改的是 `PackedArrayRef` 里的 `Vector<T>`，COW 在 Vector 层面处理
3. **Variant 销毁**：减少引用计数，引用计数归零时才释放

```cpp
// 赋值时
PackedArrayRefBase *reference_from(PackedArrayRefBase *p_base, PackedArrayRefBase *p_from) {
    if (p_base == p_from) return p_base;  // 同一个，不操作
    if (p_from->reference()) {             // 新的引用计数 +1
        if (p_base->refcount.unref()) {    // 旧的引用计数 -1
            memdelete(p_base);             // 旧的归零了，释放
        }
        return p_from;                     // 指向新的
    }
    return p_base;
}
```

---

## 六、运算符系统 —— 三维函数指针表

Godot 需要支持 `Vector2(1,2) + Vector2(3,4)` 这种运算，而且要支持**任意两种类型之间的任意运算符**。

### 6.1 问题

26 种运算符 × 38 种左操作数 × 38 种右操作数 = **37,544 种组合**。

用 switch-case 写？疯了。

### 6.2 解决方案：三维函数指针表

```cpp
// variant_op.cpp
static VariantEvaluatorFunction
    operator_evaluator_table[OP_MAX][VARIANT_MAX][VARIANT_MAX];

static Variant::ValidatedOperatorEvaluator
    validated_operator_evaluator_table[OP_MAX][VARIANT_MAX][VARIANT_MAX];

static Variant::PTROperatorEvaluator
    ptr_operator_evaluator_table[OP_MAX][VARIANT_MAX][VARIANT_MAX];
```

这是一个 `[运算符][左类型][右类型]` 的三维数组，每个格子是一个函数指针。

### 6.3 注册

引擎启动时，用模板批量注册：

```cpp
template <typename T>
void register_op(Variant::Operator p_op, Variant::Type p_type_a, Variant::Type p_type_b) {
    operator_return_type_table[p_op][p_type_a][p_type_b] = T::get_return_type();
    operator_evaluator_table[p_op][p_type_a][p_type_b] = T::evaluate;
    validated_operator_evaluator_table[p_op][p_type_a][p_type_b] = T::validated_evaluate;
    ptr_operator_evaluator_table[p_op][p_type_a][p_type_b] = T::ptr_evaluate;
}

// 注册示例
register_op<OperatorEvaluatorAdd<Vector2, Vector2, Vector2>>(OP_ADD, VECTOR2, VECTOR2);
register_op<OperatorEvaluatorMul<Vector2, Vector2, double>>(OP_MULTIPLY, VECTOR2, FLOAT);
// ... 几百条注册
```

### 6.4 运行时调用

```cpp
void Variant::evaluate(Operator p_op, const Variant &a, const Variant &b,
                       Variant &r_ret, bool &r_valid) {
    // 直接查表！O(1)
    VariantEvaluatorFunction func = operator_evaluator_table[p_op][a.type][b.type];
    if (func) {
        func(a, b, &r_ret, r_valid);
    } else {
        r_valid = false;  // 不支持的组合
    }
}
```

一次数组下标访问，就拿到了对应的函数指针。没有任何 switch-case、没有 if-else 链。

### 6.5 和 MethodBind 一样的三级路径

运算符也有三种调用路径，和 MethodBind 的设计完全一致：

| 路径 | 函数指针类型 | 用途 |
|---|---|---|
| `evaluate` | `VariantEvaluatorFunction` | GDScript 动态调用（带类型检查、错误报告） |
| `validated_evaluate` | `ValidatedOperatorEvaluator` | GDScript 已验证类型后（跳过检查） |
| `ptr_evaluate` | `PTROperatorEvaluator` | GDExtension / C# 绑定（原始指针） |

---

## 七、方法系统 —— 内置类型的方法

Variant 类型（非 Object）也有方法，比如 `Vector2.length()`、`Array.append()`、`String.split()`。

### 7.1 方法调用入口

```cpp
void Variant::callp(const StringName &p_method, const Variant **p_args,
                     int p_argcount, Variant &r_ret, Callable::CallError &r_error) {
    if (type == OBJECT) {
        // Object 走 Object::callp()（上一章讲的 ClassDB 路径）
        _get_obj().obj->callp(p_method, p_args, p_argcount, r_ret, r_error);
    } else {
        // 内置类型走 Variant 自己的方法表
        // ...
    }
}
```

### 7.2 方法也是函数指针表

和运算符类似，内置类型的方法也通过函数指针注册：

```cpp
// 类型定义
typedef void (*ValidatedBuiltInMethod)(Variant *base, const Variant **p_args,
                                       int p_argcount, Variant *r_ret);
typedef void (*PTRBuiltInMethod)(void *p_base, const void **p_args,
                                 void *r_ret, int p_argcount);
```

方法注册时，用模板自动包装 C++ 成员函数：

```cpp
// variant_call.cpp 中的模板包装器
template <typename R, typename T, typename... P>
static void vc_method_call(R (T::*method)(P...), Variant *base,
                           const Variant **p_args, int p_argcount,
                           Variant &r_ret, ...) {
    // 1. 从 Variant 中取出 T 类型的值（如 Vector2）
    // 2. 在这个值上调用 method
    // 3. 把返回值包回 Variant
    call_with_variant_args_ret_dv(&VariantInternalAccessor<T>::get(base),
                                   method, p_args, p_argcount, r_ret, ...);
}
```

**对外效果**：GDScript 写 `my_vector.length()`，引擎查方法表找到 `Vector2::length` 的函数指针，直接调用，不经过 ClassDB。

---

## 八、构造器系统

Variant 类型的构造也是函数指针表注册的：

```cpp
typedef void (*ValidatedConstructor)(Variant *r_base, const Variant **p_args);
typedef void (*PTRConstructor)(void *base, const void **p_args);
```

GDScript 中 `Vector2(1, 2)` 的调用链：

```
GDScript: Vector2(1, 2)
    → Variant::construct(VECTOR2, result, args, 2, error)
        → 查构造器表 → 找到 Vector2(float, float) 的构造函数指针
        → 调用，result 变成 Vector2(1, 2)
```

一种类型可以有多个构造器（重载），通过索引区分：

```
Vector2 的构造器：
  [0] Vector2()             — 无参
  [1] Vector2(Vector2)      — 拷贝
  [2] Vector2(Vector2i)     — 从整数版本转换
  [3] Vector2(float, float) — 两个分量
```

---

## 九、类型转换

Variant 支持在不同类型之间转换：

```cpp
// 自动转换表
static bool can_convert(Type p_from, Type p_to);
static bool can_convert_strict(Type p_from, Type p_to);  // 严格版（不丢精度）
```

转换是通过 `operator` 重载实现的。Variant 为每种目标类型都定义了 `operator T()`：

```cpp
operator bool() const;
operator int64_t() const;
operator double() const;
operator String() const;
operator Vector2() const;
operator Object*() const;
// ... 40+ 个 operator 重载
```

内部实现是 switch-case 按 `type` 分发：

```cpp
Variant::operator int64_t() const {
    switch (type) {
        case NIL: return 0;
        case BOOL: return _data._bool ? 1 : 0;
        case INT: return _data._int;
        case FLOAT: return (int64_t)_data._float;
        case STRING: return reinterpret_cast<const String*>(_data._mem)->to_int();
        default: return 0;
    }
}
```

这也就是为什么 GDScript 中 `int("42")` 能返回 42，`bool(1)` 能返回 true。

---

## 十、Variant 在引擎中的角色

### 10.1 GDScript 的"万能变量"

GDScript 中每个变量都是 Variant。即使你写了类型标注 `var x: int = 42`，底层仍然是 Variant，只不过编译器会在赋值时做类型检查。

### 10.2 MethodBind 的参数和返回值

上一章讲的 `MethodBind::call()` 路径，参数和返回值都是 `Variant**`。从 GDScript 到 C++ 方法的调用，参数需要经过：

```
GDScript 变量（Variant）
    → MethodBind::call() 接收 Variant**
    → 从 Variant 中提取具体类型（如 Vector2）
    → 调用 C++ 函数
    → 把返回值包回 Variant
    → 返回给 GDScript
```

### 10.3 信号参数

```gdscript
signal hit(damage: float, position: Vector2)
emit_signal("hit", 10.0, Vector2(1, 2))
```

`emit_signal` 的参数就是 `Variant` 数组。

### 10.4 属性系统

`Object::set("position", value)` 中的 `value` 是 Variant。
`Object::get("position")` 返回的也是 Variant。

### 10.5 序列化

场景文件（`.tscn`）中的属性值全部序列化为 Variant 的文本表示：

```
[node name="Player" type="CharacterBody2D"]
position = Vector2(100, 200)     ← Variant(VECTOR2)
velocity = Vector2(0, 0)        ← Variant(VECTOR2)
```

### 10.6 编辑器 Inspector

Inspector 中显示和编辑的每一个属性值，都是通过 Variant 读写的。

---

## 十一、性能设计总结

| 技巧 | 解决的问题 |
|---|---|
| **union 内联小类型** | Vector2、int 等常用类型零堆分配 |
| **指针存储大类型** | Transform3D、Projection 等太大的类型用堆，保持 Variant 自身紧凑 |
| **needs_deinit 常量数组** | 析构时快速跳过不需要清理的类型（大多数数值类型） |
| **三维函数指针表** | 运算符求值 O(1) 查表，避免巨型 switch-case |
| **方法函数指针表** | 内置类型方法调用 O(1) 查表 |
| **三级调用路径** | evaluate / validated / ptr，不同场景用不同性能级别 |
| **PackedArrayRef 引用计数** | 大数组赋值零拷贝 |
| **ObjData 双保险** | 指针快速访问 + ObjectID 安全验证 |

---

## 十二、大白话总结

**Variant 是什么？** 一个 24 字节的小盒子，里面塞了一个类型标签（"我现在装的是什么"）和一个 union（"东西本身"）。小东西直接塞进去，大东西存个指针指出去。

**为什么需要它？** 因为 GDScript 是动态类型语言，一个变量今天是数字明天是字符串。C++ 做不到这个，所以用 Variant 包一层。

**它怎么做到快的？** 
- 运算符和方法调用不是用 switch-case 去匹配的，而是用**函数指针表**直接查表——`table[运算符][左类型][右类型]`，一步到位
- 大多数类型不需要析构，`clear()` 里一个 `unlikely` 判断就跳过了
- 小类型零堆分配，union 内联存储

**它和 Object 的 callp 什么关系？** Variant 的 `callp` 会判断：如果装的是 Object，就走 `Object::callp()`（ClassDB 路径）；如果装的是内置类型（Vector2、Array……），就走自己的方法函数指针表。两条路最终都能通过字符串名字调用方法。

---

## 补充一：placement new

普通 `new` 做两件事：**分配内存** + **调用构造函数**。

```cpp
auto *p = new MyClass();   // 1. 从堆上分配内存  2. 调构造函数
```

placement new 只做第二件事：**在你指定的已有内存上调用构造函数**，不分配内存。

```cpp
uint8_t buffer[64];                       // 你自己准备好的一块内存
auto *p = new (buffer) MyClass();         // 在 buffer 上构造 MyClass
//            ^^^^^^^^
//            这就是 placement new 的语法：new (地址) 类型()
```

Variant 里怎么用的：

```cpp
// _data._mem 是 union 里的 uint8_t 数组，内存已经在 Variant 自身里了
memnew_placement(_data._mem, ObjData);
// 展开后就是 new (_data._mem) ObjData()
```

**好处：零堆分配**。ObjData、Vector2、Color 这些小类型全部原地构造在 Variant 自己的 union 里，不需要 malloc。

---

## 补充二：union 中的指针为什么有具名和通用两种

在 Variant 的 union 中：

```cpp
union {
    Transform2D *_transform2d;
    ::AABB *_aabb;
    Basis *_basis;
    Transform3D *_transform3d;
    Projection *_projection;
    void *_ptr;                   // ← 通用指针
    uint8_t _mem[...];
} _data;
```

这些**全部是同一块内存的不同名字**（union 的特性：所有成员共享同一块内存）。`_ptr` 和 `_basis` 占的是同一个 8 字节。

实际使用中：

```cpp
// Basis 的分配
case BASIS: {
    _data._ptr = VariantPools::alloc<Basis>();     // 用 _ptr 写入（void* 接 void*，类型匹配）
    memnew_placement(_data._basis, Basis(...));     // 用 _basis 读取（编译器知道类型）
} break;

// AABB 直接用具名指针
case AABB: {
    _data._aabb = VariantPools::alloc<::AABB>();   // 用 _aabb 写入
    memnew_placement(_data._aabb, ::AABB(...));     // 用 _aabb 读取
} break;
```

`_ptr` 是用来做"通用写入"的（`void*` 接任意指针类型方便），带类型名的指针是用来做"类型安全读取"的。本质完全一样，只是写法差异。

---

## 补充三：Object 在 Variant 中的完整存储机制

### 存什么

Object 不是存一个裸指针，而是存了 `ObjData`：

```cpp
struct ObjData {
    ObjectID id;            // 64 位身份证号
    Object *obj = nullptr;  // 裸指针
};
```

同时存**指针**（快）和 **ID**（安全）。

### 存在哪

放在 union 的 `_mem` 内联缓冲区里，用 placement new 原地构造：

```cpp
case OBJECT: {
    memnew_placement(_data._mem, ObjData);      // 在 _mem 里原地构造
    _get_obj().ref(p_variant._get_obj());        // 引用赋值
} break;
```

访问时直接 reinterpret_cast：

```cpp
_ALWAYS_INLINE_ ObjData &_get_obj() {
    return *reinterpret_cast<ObjData *>(&_data._mem[0]);
}
```

### RefCounted vs 普通 Object

- **普通 Object**（如 Node）：Variant 只记住指针和 ID，**不管理生命周期**。对象被 `free()` 后需要通过 ID 验证
- **RefCounted**（如 Resource）：Variant **参与引用计数**。赋值时 `refcount++`，Variant 销毁时 `refcount--`，归零时自动释放

### 安全访问

```cpp
Object *Variant::get_validated_object() const {
    if (type == OBJECT) {
        return ObjectDB::get_instance(_get_obj().id);  // 用 ID 去 ObjectDB 验证
    }
    return nullptr;
}
```

如果对象已销毁，返回 `nullptr`，不会访问悬垂指针。

---

## 补充四：ObjectDB —— 全局对象登记处

### 是什么

一个全局的静态数据结构，引擎里所有 Object 在创建时登记、销毁时注销：

```cpp
class ObjectDB {
    static SpinLock spin_lock;
    static ObjectSlot *object_slots;    // 核心：对象槽数组
    static uint32_t slot_count;         // 当前活对象数量
    static uint32_t slot_max;           // 数组容量
    static uint64_t validator_counter;  // 全局分代计数器

    static ObjectID add_instance(Object *p_object);     // 登记
    static void remove_instance(Object *p_object);      // 注销
    static Object *get_instance(ObjectID p_id);          // 查询
};
```

### ObjectSlot 结构

```cpp
struct ObjectSlot {
    Object *object;       // 对象指针（null 表示空闲）
    uint64_t validator;   // 分代计数
    uint32_t next_free;   // 空闲链：指向下一个可用 slot
    bool is_ref_counted;
};
```

### ObjectID 的 64 位布局

```
┌──────────────────────────────────────────────────────────────────┐
│  bit 63  │  bit 62~24 (39 bits)  │  bit 23~0 (24 bits)          │
│ ref_bit  │    validator           │    slot_index                │
└──────────────────────────────────────────────────────────────────┘
```

### 安全验证原理——分代计数（validator）

用一个具体例子走完整流程：

```
时间    object_slots[1]              你手里的 enemy_id           查询结果
────    ──────────────────           ───────────────────         ────────
T1      { obj:Enemy, validator:1 }  {slot:1, validator:1}       → Enemy ✅
T2      { obj:null,  validator:0 }  {slot:1, validator:1}       → null  ✅（敌人没了）
        （free 后 validator 清零，slot 被回收）
T3      { obj:Chest, validator:5 }  {slot:1, validator:1}       → null  ✅（不会错认成宝箱）
        （宝箱复用了同一个 slot，但 validator 已变成 5）
        宝箱自己的 ID:              {slot:1, validator:5}       → Chest ✅
```

验证逻辑：

```cpp
Object *ObjectDB::get_instance(ObjectID p_id) {
    uint32_t slot = p_id & 0xFFFFFF;            // 取 slot_index
    uint64_t validator = (p_id >> 24) & MASK;   // 取 validator

    if (object_slots[slot].validator != validator) {
        return nullptr;   // 不匹配 → 对象已不在了
    }
    return object_slots[slot].obj;  // 匹配 → 返回指针
}
```

**即使 slot 被新对象复用了，旧 ID 里的 validator 和新的不一样，不会认错人。**

### 空闲 slot 管理——隐式栈

ObjectDB 用 `slot_count` + `next_free` 字段做了一个**零额外内存开销的空闲栈**。

**添加对象时**（pop）：

```cpp
uint32_t slot = object_slots[slot_count].next_free;  // 从栈顶取一个空闲 slot
object_slots[slot].object = p_object;                 // 占用
slot_count++;                                          // 栈顶前进
```

**删除对象时**（push）：

```cpp
slot_count--;                                          // 栈顶回退
object_slots[slot_count].next_free = slot;            // 把释放的 slot 编号压入栈
object_slots[slot].object = nullptr;                   // 清空
object_slots[slot].validator = 0;                      // 失效
```

走一个完整例子：

```
=== 初始状态 ===
slot_count = 0
[0] { obj: null, next_free: 0 }    ← 初始化时 next_free = 自己的索引
[1] { obj: null, next_free: 1 }
[2] { obj: null, next_free: 2 }
[3] { obj: null, next_free: 3 }

=== 添加 Obj1 ===
slot = object_slots[0].next_free = 0 → Obj1 放进 slot 0, slot_count=1
[0] { obj: Obj1 }    [1] { next_free: 1 }    [2] { next_free: 2 }    [3] { next_free: 3 }

=== 添加 Obj2 ===
slot = object_slots[1].next_free = 1 → Obj2 放进 slot 1, slot_count=2
[0] { obj: Obj1 }    [1] { obj: Obj2 }    [2] { next_free: 2 }    [3] { next_free: 3 }

=== 添加 Obj3 ===
slot = object_slots[2].next_free = 2 → Obj3 放进 slot 2, slot_count=3
[0] { obj: Obj1 }    [1] { obj: Obj2 }    [2] { obj: Obj3 }    [3] { next_free: 3 }

=== 🔥 删除 Obj2（slot 1）===
slot_count-- → 2
object_slots[2].next_free = 1    ← 把 slot 1 压入栈
[0] { obj: Obj1 }    [1] { obj: null }    [2] { obj: Obj3, next_free: 1 }    [3] { next_free: 3 }

=== 添加 Obj4 ===
slot = object_slots[2].next_free = 1 → Obj4 放进 slot 1 !!!（复用了 Obj2 的位置）
slot_count++ → 3
[0] { obj: Obj1 }    [1] { obj: Obj4 }    [2] { obj: Obj3 }    [3] { next_free: 3 }
```

**先释放的后复用，后释放的先复用——LIFO 栈行为。**

### 扩容策略

```cpp
uint32_t new_slot_max = slot_max > 0 ? slot_max * 2 : 1;
```

- 初始 0 个 slot
- 第一次扩容：0 → 1
- 然后：1 → 2 → 4 → 8 → 16 → 32 → ...
- 经典的 **2 倍增长**，均摊 O(1) 插入
- 最大容量：2^24 ≈ 1600 万个对象（由 ObjectID 的 24 位 slot_index 决定）

### 外部怎么知道自己的 Object 在哪个 slot？

不需要查找——**Object 自己身上就存着 ObjectID**：

```cpp
class Object {
    ObjectID _instance_id;   // 每个 Object 都记住了自己的 ID
};
```

创建时 ObjectDB 把 slot 位置编码进 ID，ID 存在 Object 自身上：

```cpp
Object::Object() {
    _instance_id = ObjectDB::add_instance(this);
    // ID 里编码了 slot_index + validator
}
```

外部拿到 Object 指针后，直接从对象身上取 ID：

```cpp
ObjectID id = enemy->get_instance_id();   // 从对象身上取
uint32_t slot = id & 0xFFFFFF;            // 低 24 位就是 slot_index
```

**全程没有任何"搜索"操作。** 创建时分配 slot → 编码进 ID → ID 存在对象上 → 从 ID 取 slot → 去 ObjectDB 查。全是 O(1)。
