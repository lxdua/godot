
# Object + ClassDB + MethodBind —— Godot 反射系统深度解析

> 📂 核心源码：`core/object/object.h`, `core/object/class_db.h`, `core/object/method_bind.h`, `core/object/gdtype.h`

---

## 一、为什么需要自研反射系统？

C++ 原生几乎没有反射能力——你无法在运行时通过字符串查找一个类的方法、创建一个类的实例、或者遍历一个对象的所有属性。但游戏引擎需要这些能力：

- **编辑器**需要知道每个类有哪些属性，才能在 Inspector 中显示它们
- **脚本语言**（GDScript、C#）需要能调用 C++ 类的方法
- **序列化系统**需要能通过字符串名字读写对象的属性
- **信号系统**需要能通过字符串名字调用连接的方法

Godot 的解决方案是：**用宏 + 模板，在编译期生成反射信息，在运行时存储到全局数据库 ClassDB 中**。这比 Qt 的 MOC（需要额外的预处理工具）更轻量，比 C++ RTTI（只有 `typeid` 和 `dynamic_cast`）功能强大得多。

---

## 二、Object —— 万物之基

`Object` 是 Godot 中几乎所有类的基类。它提供了以下核心能力：

### 2.1 Object 的内存布局

```cpp
// object.h (简化)
class Object {
private:
    ObjectGDExtension *_extension = nullptr;          // GDExtension 扩展信息
    GDExtensionClassInstancePtr _extension_instance;  // 扩展实例

    // 信号系统
    HashMap<StringName, SignalData> signal_map;        // 信号名 → 信号数据（包括 slot 列表）
    List<Connection> connections;                       // 出站连接列表

    ObjectID _instance_id;                             // 全局唯一 ID
    uint32_t _ancestry : 15;                           // 祖先类位域（快速类型判断）
    bool _block_signals : 1;                           // 是否阻塞信号
    bool _can_translate : 1;                           // 是否可以翻译
    bool _emitting : 1;                                // 是否正在发射信号

    ScriptInstance *script_instance = nullptr;          // 脚本实例
    HashMap<StringName, Variant> metadata;              // Meta 键值对
    mutable const GDType *_gdtype_ptr = nullptr;       // 类型信息缓存
    // ...
};
```

### 2.2 ObjectID 与 ObjectDB

每个 `Object` 实例在创建时会被注册到全局 `ObjectDB` 中，分配一个 **64 位的 `ObjectID`**：

```
┌──────────────────────────────────────────────────────────────────┐
│  bit 63  │  bit 62~24 (39 bits)  │  bit 23~0 (24 bits)          │
│ ref_bit  │    validator           │    slot_index                │
└──────────────────────────────────────────────────────────────────┘
```

- **slot_index**（24 bit）：在 `object_slots` 数组中的位置，最多 ~1600 万个对象
- **validator**（39 bit）：分代计数器，当 slot 被回收重用时 validator 会递增，确保旧 ID 不会访问到新对象
- **ref_bit**（1 bit）：标记该对象是否是 `RefCounted`

```cpp
// ObjectDB::get_instance 的核心逻辑
static Object *get_instance(ObjectID p_instance_id) {
    uint64_t id = p_instance_id;
    uint32_t slot = id & OBJECTDB_SLOT_MAX_COUNT_MASK;
    uint64_t validator = (id >> OBJECTDB_SLOT_MAX_COUNT_BITS) & OBJECTDB_VALIDATOR_MASK;

    spin_lock.lock();
    if (object_slots[slot].validator != validator) {
        spin_lock.unlock();
        return nullptr;  // 对象已被销毁，旧 ID 失效
    }
    Object *object = object_slots[slot].object;
    spin_lock.unlock();
    return object;
}
```

这个设计让你可以安全地持有 `ObjectID` 而不用担心悬垂指针——如果对象被销毁了，`get_instance()` 会返回 `nullptr`。

### 2.3 AncestralClass —— 快速类型判断

传统的 `is_class` 需要遍历继承链或查哈希表，Godot 用一个 15 位的位域做**零成本类型判断**：

```cpp
enum class AncestralClass : unsigned int {
    REF_COUNTED         = 1 << 0,
    NODE                = 1 << 1,
    RESOURCE            = 1 << 2,
    SCRIPT              = 1 << 3,
    CANVAS_ITEM         = 1 << 4,
    CONTROL             = 1 << 5,
    NODE_2D             = 1 << 6,
    COLLISION_OBJECT_2D = 1 << 7,
    AREA_2D             = 1 << 8,
    NODE_3D             = 1 << 9,
    VISUAL_INSTANCE_3D  = 1 << 10,
    GEOMETRY_INSTANCE_3D= 1 << 11,
    COLLISION_OBJECT_3D = 1 << 12,
    PHYSICS_BODY_3D     = 1 << 13,
    MESH_INSTANCE_3D    = 1 << 14,
};
```

每个类在构造时通过 `_define_ancestry()` 设置自己的位。判断类型时：

```cpp
_FORCE_INLINE_ bool is_ref_counted() const { return _ancestry & (uint32_t)AncestralClass::REF_COUNTED; }
```

这是一个 **单条 AND 指令**，比 `dynamic_cast` 快几个数量级。当然它只覆盖了 15 个"热门"基类，其他类仍然走 `is_class_ptr` 指针比较。

### 2.4 cast_to —— 比 dynamic_cast 更快的类型转换

```cpp
template <typename T, typename O>
static T *cast_to(O *p_object) {
    return p_object && p_object->template derives_from<T, O>()
        ? static_cast<T *>(p_object) : nullptr;
}
```

`derives_from<T, O>` 的判断策略是**分层的**：

1. **编译期**：如果 `O` 就是 `T` 的子类（`is_base_of_v<T, O>`），直接返回 `true`，零运行时开销
2. **AncestralClass 位域**：如果 `T` 有设置 `static_ancestral_class`，用位 AND 判断
3. **指针比较**：最后才用 `is_class_ptr(T::get_class_ptr_static())`，遍历虚继承链

```cpp
template <typename T, typename O>
bool Object::derives_from() const {
    if constexpr (std::is_base_of_v<T, O>) {
        return true;                                     // 编译期确定
    } else {
        if constexpr (T::static_ancestral_class != T::super_type::static_ancestral_class) {
            return _has_ancestry(T::static_ancestral_class);  // 位域判断
        } else {
            return is_class_ptr(T::get_class_ptr_static());   // 指针遍历
        }
    }
}
```

---

## 三、GDCLASS 宏 —— 反射的入口

每个要注册到引擎的类都必须写一个 `GDCLASS(ClassName, ParentClass)` 宏。这个宏展开后大约 100 行代码，做了以下事情：

### 3.1 宏展开分析

```cpp
#define GDCLASS(m_class, m_inherits)
    GDSOFTCLASS(m_class, m_inherits)   // ← 第一层：基础类型信息
    // ← 第二层：ClassDB 注册能力
```

#### GDSOFTCLASS 提供的能力

```cpp
// 1. 类型别名
using self_type = m_class;
using super_type = m_inherits;

// 2. 静态指针标识（每个类一个唯一的静态 int 地址）
static void *get_class_ptr_static() {
    static int ptr;    // 每个类的这个 static int 地址是唯一的
    return &ptr;
}

// 3. is_class_ptr 虚函数 —— 遍历继承链比较指针
virtual bool is_class_ptr(void *p_ptr) const override {
    return (p_ptr == get_class_ptr_static()) || m_inherits::is_class_ptr(p_ptr);
}

// 4. _setv / _getv / _notification_forwardv / _notification_backwardv
//    这些虚函数实现了"沿继承链依次调用"的机制
```

**继承链调用的关键技巧**——函数指针比较：

```cpp
// 判断子类是否覆写了 _notification
_FORCE_INLINE_ void (Object::*_get_notification() const)(int) {
    return (void (Object::*)(int)) & m_class::_notification;
}
virtual void _notification_forwardv(int p_notification) override {
    m_inherits::_notification_forwardv(p_notification);      // 先调父类
    if (m_class::_get_notification() != m_inherits::_get_notification()) {
        _notification(p_notification);                        // 再调自己（仅当覆写了才调）
    }
}
```

这就是为什么你在 Godot 中只需要写 `void _notification(int p_what)` 而**不需要手动调用 `super()`** 的原因——宏自动生成的虚函数会沿继承链正确分发。

#### GDCLASS 在 GDSOFTCLASS 基础上额外提供的

```cpp
// 5. GDType 静态类型信息（延迟初始化，线程安全）
static GDType &get_gdtype_static_mutable() {
    static GDType *gdtype = nullptr;
    static bool initialized = false;
    if (likely(initialized)) return *gdtype;

    static BinaryMutex __init_mutex;
    MutexLock lock(__init_mutex);
    if (initialized) return *gdtype;           // double-checked locking
    gdtype = memnew(GDType(&super_type::get_gdtype_static(), StringName(#m_class)));
    initialized = true;
    return *gdtype;
}

// 6. 静态类名
static const StringName &get_class_static() {
    return get_gdtype_static().get_name();     // 从 GDType 获取
}

// 7. initialize_class —— 把自己注册到 ClassDB
static void initialize_class() {
    static bool initialized = false;
    if (likely(initialized)) return;

    m_inherits::initialize_class();            // 先确保父类已初始化
    _add_class_to_classdb(get_gdtype_static_mutable(), &super_type::get_gdtype_static());
    get_gdtype_static_mutable().initialize();
    if (m_class::_get_bind_methods() != m_inherits::_get_bind_methods()) {
        _bind_methods();                        // 调用 _bind_methods()，注册方法/属性/信号
    }
    initialized = true;
}
```

### 3.2 GDCLASS 为什么用函数指针比较？

注意 `_get_bind_methods()` / `_get_notification()` 等函数返回的是**成员函数指针**。比较两个函数指针相等，本质是判断"子类是否覆写了这个函数"。如果子类没有覆写（函数指针与父类相同），就跳过调用。

这避免了在每一层都调用空函数的开销，也避免了强制要求每个类都实现 `_bind_methods()`。

---

## 四、GDType —— 类型的元数据

`GDType` 是 Godot 4.x 新增的类型描述对象，存储**一个类的静态元信息**：

```cpp
class GDType {
    const GDType *super_type;              // 父类型指针
    StringName name;                        // 类名
    Vector<StringName> name_hierarchy;      // 继承链名字列表 [自己, 父, 祖父, ..., Object]

    AHashMap<StringName, int64_t> constant_map;           // 常量（含继承）
    AHashMap<StringName, int64_t> self_constant_map;      // 常量（仅自己）
    AHashMap<StringName, const EnumInfo *> enum_map;       // 枚举（含继承）
    AHashMap<StringName, const EnumInfo *> self_enum_map;  // 枚举（仅自己）
    AHashMap<StringName, const MethodInfo *> signal_map;   // 信号（含继承）
    AHashMap<StringName, const MethodInfo *> self_signal_map; // 信号（仅自己）
};
```

`GDType` 的 `initialize()` 方法会**合并父类的常量、枚举、信号到自己的 map 中**，实现继承语义。这使得查询"某个类的所有信号"只需要查一个 map，而不需要沿继承链逐级查找。

每个 C++ 类通过 `GDCLASS` 宏在静态初始化期间创建自己的 `GDType` 实例（线程安全的 double-checked locking 初始化）。

---

## 五、ClassDB —— 全局类数据库

`ClassDB` 是一个**纯静态类**（没有实例），维护了一个全局的 `HashMap<StringName, ClassInfo>`：

```cpp
class ClassDB {
    static HashMap<StringName, ClassInfo> classes;  // 核心！所有类信息都在这
    // ...
};
```

### 5.1 ClassInfo —— 一个类的完整档案

```cpp
struct ClassInfo {
    APIType api = API_NONE;                                 // 属于哪个 API 层
    ClassInfo *inherits_ptr = nullptr;                      // 父类 ClassInfo 指针
    void *class_ptr = nullptr;                              // get_class_ptr_static() 的地址
    GDType *gdtype = nullptr;                               // GDType 指针

    ObjectGDExtension *gdextension = nullptr;               // GDExtension 信息

    HashMap<StringName, MethodBind *> method_map;           // 方法名 → MethodBind
    HashMap<StringName, LocalVector<MethodBind *>> method_map_compatibility; // 兼容方法

    List<PropertyInfo> property_list;                        // 属性列表（有序）
    HashMap<StringName, PropertyInfo> property_map;          // 属性名 → PropertyInfo

    AHashMap<StringName, PropertySetGet> property_setget;   // 属性名 → setter/getter

    bool disabled = false;
    bool exposed = false;                                    // 是否暴露给脚本
    bool is_virtual = false;                                 // 是否是虚拟类（不可实例化但可继承）
    Object *(*creation_func)(bool) = nullptr;               // 工厂函数
};
```

### 5.2 类的注册流程

```cpp
// 第一步：在 register_core_types() / register_scene_types() 等函数中调用
ClassDB::register_class<Node>();

// register_class 的实现
template <typename T>
static void register_class(bool p_virtual = false) {
    T::initialize_class();              // ← 触发 GDCLASS 宏生成的 initialize_class()
    ClassInfo *t = classes.getptr(T::get_class_static());
    t->creation_func = &creator<T>;     // 设置工厂函数
    t->exposed = true;                  // 标记为暴露给脚本
    t->class_ptr = T::get_class_ptr_static();
    t->api = current_api;
}

// creator 模板 —— 实际创建对象的工厂
template <typename T>
static Object *creator(bool p_notify_postinitialize) {
    Object *ret = new ("") T;
    ret->_initialize();
    if (p_notify_postinitialize) {
        ret->_postinitialize();
    }
    return ret;
}
```

这样，`ClassDB::instantiate("Node")` 就能通过字符串名字创建对象了。

### 5.3 注册变体

| 注册方法 | `creation_func` | `exposed` | 用途 |
|---|---|---|---|
| `register_class<T>()` | ✅ 有 | ✅ true | 普通类，脚本可见可实例化 |
| `register_abstract_class<T>()` | ❌ 无 | ✅ true | 抽象类，脚本可见但不可实例化 |
| `register_internal_class<T>()` | ✅ 有 | ❌ false | 内部类，脚本不可见 |
| `register_runtime_class<T>()` | ✅ 有 | ✅ true | 运行时注册的类 |

---

## 六、MethodBind —— 方法绑定的核心

`MethodBind` 是 Godot 反射系统中最精巧的部分。它将一个 **C++ 成员函数** 包装成统一的可调用接口。

### 6.1 三种调用路径

```cpp
class MethodBind {
    virtual Variant call(Object *p_object, const Variant **p_args, int p_arg_count,
                         Callable::CallError &r_error) const = 0;

    virtual void validated_call(Object *p_object, const Variant **p_args,
                                Variant *r_ret) const = 0;

    virtual void ptrcall(Object *p_object, const void **p_args, void *r_ret) const = 0;
};
```

| 调用路径 | 参数类型 | 类型检查 | 性能 | 使用场景 |
|---|---|---|---|---|
| `call` | `Variant**` | 运行时校验 + 自动转换 | ⭐ | GDScript 调用、`Object::callp()` |
| `validated_call` | `Variant**` | 已提前验证，跳过检查 | ⭐⭐ | GDScript 编译器已确认类型安全时 |
| `ptrcall` | `void**` | 无检查，直接指针传递 | ⭐⭐⭐ | GDExtension、C# 绑定 |

### 6.2 MethodBind 的模板特化家族

根据方法签名的不同（是否 const、是否有返回值、是否静态、是否可变参数），Godot 用模板生成了多种 MethodBind 子类：

```
MethodBind（抽象基类）
├── MethodBindT<T, P...>           ── 无返回值，非 const
├── MethodBindTC<T, P...>          ── 无返回值，const
├── MethodBindTR<T, R, P...>       ── 有返回值，非 const
├── MethodBindTRC<T, R, P...>      ── 有返回值，const
├── MethodBindTS<R, P...>          ── 静态方法，有返回值
├── MethodBindT (static variant)   ── 静态方法，无返回值
├── MethodBindVarArgT<T>           ── 可变参数，无返回值
└── MethodBindVarArgTR<T, R>       ── 可变参数，有返回值
```

### 6.3 以 MethodBindT 为例

```cpp
template <typename T, typename... P>
class MethodBindT : public MethodBind {
    void (T::*method)(P...);   // 存储的 C++ 成员函数指针

    // call —— Variant 路径
    virtual Variant call(Object *p_object, const Variant **p_args, int p_arg_count,
                         Callable::CallError &r_error) const override {
        // 从 Variant 提取参数，调用 C++ 函数
        call_with_variant_args_dv(
            static_cast<T *>(p_object), method,
            p_args, p_arg_count, r_error,
            get_default_arguments()         // 支持默认参数
        );
        return Variant();
    }

    // validated_call —— 已验证路径（跳过类型检查）
    virtual void validated_call(Object *p_object, const Variant **p_args,
                                Variant *r_ret) const override {
        call_with_validated_object_instance_args(
            static_cast<T *>(p_object), method, p_args
        );
    }

    // ptrcall —— 原始指针路径（零开销）
    virtual void ptrcall(Object *p_object, const void **p_args, void *r_ret) const override {
        call_with_ptr_args<T, P...>(
            static_cast<T *>(p_object), method, p_args
        );
    }
};
```

**创建过程**：

```cpp
// create_method_bind 是一个模板函数，自动推导参数类型
template <typename T, typename... P>
MethodBind *create_method_bind(void (T::*p_method)(P...)) {
    MethodBind *a = memnew((MethodBindT<T, P...>)(p_method));
    a->set_instance_class(T::get_class_static());   // 记住属于哪个类
    return a;
}
```

### 6.4 bind_method 的完整流程

```cpp
// 用户写的绑定代码
ClassDB::bind_method(D_METHOD("set_position", "position"), &Node2D::set_position);
```

展开后发生的事情：

```
1. D_METHOD("set_position", "position")
   → 创建 MethodDefinition，记录方法名和参数名（仅 DEBUG_ENABLED）

2. ClassDB::bind_method(name, &Node2D::set_position)
   → 调用 create_method_bind(&Node2D::set_position)
     → 编译器推导出 T=Node2D, P=(Vector2)
     → 创建 MethodBindT<Node2D, Vector2>，存储成员函数指针
   → 调用 bind_methodfi(flags, bind, false, name, defaults, default_count)
     → 在 ClassInfo::method_map 中注册：method_map["set_position"] = bind

3. 运行时调用 node2d->callp("set_position", args, 1, error)
   → ClassDB::get_method("Node2D", "set_position")  → 找到 MethodBind
   → method->call(node2d, args, 1, error)
     → MethodBindT::call()
       → 从 Variant 提取 Vector2 参数
       → 调用 node2d->set_position(extracted_vector2)
```

---

## 七、Object::callp —— 方法调用分发

`callp` 是 Object 上**最核心的调用入口**，所有动态方法调用最终都经过它：

```cpp
Variant Object::callp(const StringName &p_method, const Variant **p_args,
                       int p_argcount, Callable::CallError &r_error) {
    r_error.error = Callable::CallError::CALL_OK;

    // 1. 特殊处理 free()
    if (p_method == CoreStringName(free_)) {
        memdelete(this);
        return Variant();
    }

    // 2. 先尝试脚本实例
    if (script_instance) {
        Variant ret = script_instance->callp(p_method, p_args, p_argcount, r_error);
        if (r_error.error == Callable::CallError::CALL_OK) {
            return ret;     // 脚本处理了，直接返回
        }
        if (r_error.error != Callable::CallError::CALL_ERROR_INVALID_METHOD) {
            return ret;     // 脚本有这个方法但调用出错，返回错误
        }
        // 脚本没有这个方法，fallthrough 到 C++ 层
    }

    // 3. 从 ClassDB 查找 MethodBind
    MethodBind *method = ClassDB::get_method(get_class_name(), p_method);

    if (method) {
        return method->call(this, p_args, p_argcount, r_error);
    } else {
        r_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
    }

    return Variant();
}
```

**调用优先级：`free()` > 脚本方法 > C++ MethodBind 方法**

这个设计使得：
- 脚本可以"覆写"C++ 方法（脚本先执行，如果找到就返回）
- C++ 方法始终作为兜底存在
- `free()` 永远不会被脚本覆写（安全保证）

---

## 八、_bind_methods —— 类的"API 清单"

每个类通过 `_bind_methods()` 静态函数声明自己暴露给反射系统的所有内容。以 `Object` 自身为例：

```cpp
void Object::_bind_methods() {
    // === 绑定普通方法 ===
    ClassDB::bind_method(D_METHOD("get_class"), &Object::get_class);
    ClassDB::bind_method(D_METHOD("is_class", "class"), &Object::is_class);
    ClassDB::bind_method(D_METHOD("set", "property", "value"), &Object::_set_bind);
    ClassDB::bind_method(D_METHOD("get", "property"), &Object::_get_bind);

    // === 绑定带默认值的方法 ===
    ClassDB::bind_method(D_METHOD("notification", "what", "reversed"),
                         &Object::notification, DEFVAL(false));
    //                                           ↑ reversed 默认为 false

    // === 绑定可变参数方法 ===
    MethodInfo mi;
    mi.name = "emit_signal";
    mi.arguments.push_back(PropertyInfo(Variant::STRING_NAME, "signal"));
    ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "emit_signal",
                                &Object::_emit_signal, mi, varray(), false);

    // === 绑定信号 ===
    ADD_SIGNAL(MethodInfo("script_changed"));
    ADD_SIGNAL(MethodInfo("property_list_changed"));

    // === 绑定虚方法（供脚本覆写）===
    BIND_OBJ_CORE_METHOD(MethodInfo("_init"));
    BIND_OBJ_CORE_METHOD(MethodInfo(Variant::STRING, "_to_string"));
    // ...
}
```

### 8.1 API 声明的几种形式

| 用法 | 含义 |
|---|---|
| `ClassDB::bind_method(D_METHOD("name", "arg1"), &Class::method)` | 绑定普通方法 |
| `ClassDB::bind_method(..., DEFVAL(value))` | 绑定带默认参数的方法 |
| `ClassDB::bind_static_method(class, D_METHOD("name"), &func)` | 绑定静态方法 |
| `ClassDB::bind_vararg_method(flags, "name", &method, info)` | 绑定可变参数方法 |
| `ADD_PROPERTY(PropertyInfo(...), "setter", "getter")` | 注册属性（setter/getter 对） |
| `ADD_GROUP("GroupName", "prefix_")` | 属性分组（编辑器显示用） |
| `ADD_SUBGROUP("SubName", "prefix_")` | 属性子分组 |
| `ADD_SIGNAL(MethodInfo("signal_name"))` | 注册信号 |
| `BIND_ENUM_CONSTANT(ENUM_VALUE)` | 注册枚举值 |
| `GDVIRTUAL_BIND(method_name)` | 注册虚方法（供脚本覆写） |

---

## 九、属性系统 —— set/get 的分发链

当你在编辑器中修改一个属性，或在脚本中写 `node.position = Vector2(10, 20)` 时，调用链是：

```
Object::set("position", value)
    │
    ├── 1. ClassDB::set_property(this, "position", value)
    │       → 查找 property_setget["position"]
    │       → 找到 setter = "set_position"
    │       → 找到对应 MethodBind，调用 method->call(this, &value, 1, error)
    │       → 如果成功，返回 true
    │
    ├── 2. _setv("position", value)  // 虚函数链
    │       → 沿继承链依次调用每一层的 _set()
    │       → 如果某一层返回 true，停止
    │
    ├── 3. 尝试 metadata（以 "metadata/" 开头的属性）
    │
    └── 4. 尝试 script_instance->set()
```

属性的注册方式：

```cpp
// 在 _bind_methods 中
ADD_PROPERTY(
    PropertyInfo(Variant::VECTOR2, "position", PROPERTY_HINT_NONE, "suffix:px"),
    "set_position",   // setter
    "get_position"    // getter
);
```

这会在 ClassInfo 的 `property_setget` 中创建一条记录，把属性名映射到 setter/getter 的 MethodBind。

---

## 十、Notification 系统 —— 继承链正确分发

Notification 是一种通过**整数码**传递的轻量级事件：

```cpp
node->notification(NOTIFICATION_READY);
```

内部调用链：

```
Object::notification(NOTIFICATION_READY, reversed=false)
    → _notification_forward(NOTIFICATION_READY)
        → Object::_notification_forwardv()
            → Node::_notification_forwardv()     // GDCLASS 生成
                → Node::_notification_forwardv 先调 Object 的
                → 再判断 Node 是否覆写了 _notification，是则调用 Node::_notification()
                    → Node3D::_notification_forwardv()
                        → ...
```

**Forward（正序）**：从 Object → 最终子类，先基后子
**Backward（逆序）**：从最终子类 → Object，先子后基

这个机制由 `GDSOFTCLASS` 宏自动生成，开发者只需要写：

```cpp
void MyNode::_notification(int p_what) {
    switch (p_what) {
        case NOTIFICATION_READY: {
            // 你的逻辑
        } break;
    }
}
```

不需要手动调用 `super._notification()`，因为宏生成的分发代码**自动**保证了继承链上每一层的 `_notification` 都会被调用。

---

## 十一、完整的调用关系图

```
┌─────────────────────────────────────────────────────────────────┐
│                    GDScript / C# / GDExtension                  │
│  node.set_position(Vector2(10, 20))                            │
└──────────────┬──────────────────────────────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────────────────────────────┐
│  Object::callp("set_position", args, 1, error)                  │
│  ├── 1. script_instance->callp()     ← 脚本优先                 │
│  └── 2. ClassDB::get_method("Node2D", "set_position")           │
│         → MethodBind* bind                                       │
│         → bind->call(this, args, 1, error)                       │
└──────────────┬───────────────────────────────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────────────────────────────┐
│  MethodBindTR<Node2D, void, Vector2>::call(...)                  │
│  ├── call 路径：Variant → Vector2 提取 → node2d->set_position() │
│  ├── validated_call 路径：跳过类型检查，直接提取                  │
│  └── ptrcall 路径：void** → 直接转 Vector2*，零开销              │
└──────────────────────────────────────────────────────────────────┘
```

---

## 十二、设计亮点总结

### ✅ 编译期生成，运行时查询
所有反射信息在 `initialize_class()` 时注册到 `ClassDB`，之后是**纯运行时查表**，没有编译期元编程的运行时开销。

### ✅ 三级调用路径
`call` / `validated_call` / `ptrcall` 三条路径让不同场景（动态脚本 vs 已验证调用 vs 原生绑定）都能获得最佳性能。

### ✅ 无需外部工具
不像 Qt 需要 MOC 预处理器，Godot 纯靠 C++ 宏和模板实现反射，构建流程更简单。

### ✅ 函数指针比较避免空调用
`_get_notification() != parent::_get_notification()` 这个技巧让没有覆写 `_notification()` 的类完全零开销。

### ✅ AncestralClass 位域加速热路径
对最常用的 15 个基类，类型判断只需一条位运算指令。

### ✅ ObjectDB 分代 ID 确保安全
不持有裸指针，通过 `ObjectID` + validator 确保永远不会访问已销毁的对象。

---

## 十三、动手实验建议

1. **追踪一次 `_bind_methods` 调用**：选一个简单的类（如 `Timer`），找到它的 `_bind_methods`，看看注册了哪些方法、属性、信号

2. **在 ClassDB 打断点**：在 `ClassDB::bind_methodfi` 打断点，观察 `MethodBind` 是如何被创建和注册的

3. **跟踪一次 callp**：在 GDScript 中写 `node.set_position(Vector2(1,2))`，在 `Object::callp` 打断点，观察调用如何分发到 `MethodBind::call`

4. **观察 initialize_class 的递归**：在 `Node::initialize_class` 打断点，观察它如何先调用 `Object::initialize_class`，再注册自己的方法

5. **尝试自己绑定一个方法**：给一个现有类添加一个简单的 C++ 方法，在 `_bind_methods` 中注册它，然后从 GDScript 调用
