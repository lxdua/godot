# 第 16 章　`GDScriptInstance` 与运行时实例

第 15 章介绍了 `GDScript` 这个“类对象”，但它本身不存放任何用户实例
的状态——所有 `var foo := 0` 的运行期值都活在 `GDScriptInstance`
里。这一章把焦点从“类”切到“实例”：

* 一个 `GDScriptInstance` 与 `Object`、`GDScript` 三方的关系；
* 字节码、`get_property_list`、引擎反射调用对实例侧的不同访问路径；
* `set` / `get` 的多级回退（成员 → 静态 → 信号 → `_set` / `_get`）；
* `notification` 为什么不是“最派生类覆盖”而是“逐层都调”；
* `_ready` 触发时 `@implicit_ready` 怎么递归激活；
* 析构期为什么要先“清空挂起的 `await`”再放回脚本的实例集合。

涉及的核心文件：

* `modules/gdscript/gdscript.h`：`GDScriptInstance` 类声明
* `modules/gdscript/gdscript.cpp`：`set` / `get` / `callp` /
  `notification` / `~GDScriptInstance` 的具体实现

---

## 16.1 三个对象，一组指针

```
┌──────────────┐  set_script_instance     ┌────────────────────────┐
│   Object     │ ───────────────────────▶ │  GDScriptInstance       │
│ (engine)     │ ◀─── get_owner ──────── │   owner :  Object*      │
└──────────────┘                          │   owner_id: ObjectID    │
        ▲                                 │   members: Vector<Var>  │
        │  set_script                     │   script : Ref<GDScript>│
        ▼                                 └────────────────────────┘
┌──────────────┐                                       │
│   Script*    │                                       │ holds Ref
│ = GDScript   │ ◀──────────────── get_script ─────────┘
└──────────────┘
```

* **`Object`**：引擎侧的“宿主”。所有 Godot 对象天生就有 set/get/call
  接口，但默认实现只查 ClassDB。
* **`Object::set_script_instance`** 把一个 `GDScriptInstance*` 挂到
  Object 上，于是 Object 的 `set/get/call` 在没找到原生属性时会自动
  转发给 ScriptInstance。
* **`GDScriptInstance::script`** 持有一个 `Ref<GDScript>`，这是脚本
  实例的“类”。
* **`GDScriptInstance::owner`** 与 `owner_id` 双重持有 `Object*`，
  一份是裸指针（性能），一份是 ID（用于在 hot reload / 销毁竞争中
  做有效性检查）。

`GDScriptInstance` 不继承 `Object`——它是个**轻量结构体**，本身没有
信号、没有 RTTI、不占用 `ObjectDB` 槽位。它只是 `ScriptInstance`
（Godot 抽象基类）的一个实现。

```cpp
class GDScriptInstance : public ScriptInstance {
    ObjectID        owner_id;
    Object         *owner = nullptr;
    Ref<GDScript>   script;
#ifdef DEBUG_ENABLED
    HashMap<StringName, int> member_indices_cache;
#endif
    Vector<Variant> members;     // 扁平成员数组
    SelfList<GDScriptFunctionState>::List pending_func_states;
    // ... 方法 ...
};
```

成员只有 5 个字段——这是 GDScript 的实例足以达到“可与原生对象比拼
内存占用”的关键。

---

## 16.2 `members` 数组的内存布局

`members` 是 `Vector<Variant>`，长度等于 `script->member_indices.size()`
——也就是“包含所有基类的字段总数”。

```cpp
instance->members.resize(member_indices.size());
```

布局示意（接 15.1 的 `Enemy extends CharacterBody2D` 例子）：

```
members 数组           来自                         索引
┌──────────────┐
│  hp          │   ← Enemy.hp                       0
└──────────────┘
```

如果 `Enemy` 还有一个父 GDScript（比如 `Character extends CharacterBody2D`），
布局就会变成：

```
┌──────────────┐
│  health      │   ← Character.health               0
│  speed       │   ← Character.speed                1
│  hp          │   ← Enemy.hp                       2
└──────────────┘
```

这个排列由编译器在 `_prepare_compilation` 的“成员索引分配”阶段决定，
**子类的字段一定排在父类后面**——这样：

1. 给父类预留的槽位永远不会被子类挤占；
2. 子类指令直接用更大的索引就能拿到自己的成员；
3. **变更继承关系才会引起索引洗牌**，这正是 `reload_members` 处理的
   场景（见 16.7）。

---

## 16.3 `set`：逐层回退的写入路径

```cpp
bool GDScriptInstance::set(const StringName &p_name, const Variant &p_value) {
    // (1) 实例成员（含基类，扁平表）
    if (auto E = script->member_indices.find(p_name)) {
        const auto *member = &E->value;
        Variant value = p_value;
        // 类型校验/转换
        if (!member->data_type.is_type(value)) {
            const Variant *args = &p_value;
            Callable::CallError err;
            Variant::construct(member->data_type.builtin_type, value, &args, 1, err);
            if (err.error != CALL_OK || !member->data_type.is_type(value)) return false;
        }
        if (likely(script->valid) && member->setter) {
            // 用户写了 setter，走 setter 函数
            const Variant *args = &value;
            Callable::CallError err;
            callp(member->setter, &args, 1, err);
            return err.error == CALL_OK;
        } else {
            members.write[member->index] = value;
            return true;
        }
    }

    // (2) 静态变量 + (3) _set 回调（沿 base 链上溯）
    GDScript *sptr = script.ptr();
    while (sptr) {
        // 静态变量
        if (auto E = sptr->static_variables_indices.find(p_name)) { /* ... */ }

        // 用户的 _set
        if (likely(sptr->valid)) {
            if (auto E = sptr->member_functions.find(strings._set)) {
                Variant name = p_name;
                const Variant *args[2] = { &name, &p_value };
                Callable::CallError err;
                Variant ret = E->value->call(this, args, 2, err);
                if (err.error == CALL_OK && ret.get_type() == BOOL && ret.operator bool())
                    return true;
            }
        }
        sptr = sptr->base.ptr();
    }
    return false;
}
```

写入路径的优先级：

1. **实例字段**（最常见）→ 直接索引写入；
2. **类型化字段** → 自动 `Variant::construct` 转换；
3. **声明了 setter** → 转去调用 `set_xxx(value)`，不直接写 `members`；
4. **静态变量**（沿基类链）→ 写 `sptr->static_variables`；
5. **用户 `_set(name, value)`** 回调 → 返回 `true` 才视作处理过。

第 4 与第 5 步在每一层基类上都要试一次，所以是个**双重循环**：内
层 hash 查，外层 base 链遍历。但因为绝大多数情况第 1 步就命中，热
路径仍然是 O(1)。

> 注意第 3 步的语义：**setter 优先于直接写**。如果用户为字段声明
> 了 `set foo(value):`，那么从外部 `obj.foo = ...` 也会走 setter，
> 与从内部赋值的语义一致——这点和很多语言的“property accessor”
> 一致。

---

## 16.4 `get`：更复杂的多级查找

`get` 不仅要返回成员变量值，还要承担“访问类常量”“访问信号”“访问
方法引用”等多种语义，因此查找层次更多：

```cpp
bool GDScriptInstance::get(const StringName &p_name, Variant &r_ret) const {
    // (1) 实例字段（含 getter 转发）
    if (auto E = script->member_indices.find(p_name)) {
        if (likely(script->valid) && E->value.getter) {
            Callable::CallError err;
            const Variant ret = const_cast<GDScriptInstance *>(this)
                                    ->callp(E->value.getter, nullptr, 0, err);
            r_ret = (err.error == CALL_OK) ? ret : Variant();
            return true;
        }
        r_ret = members[E->value.index];
        return true;
    }

    const GDScript *sptr = script.ptr();
    while (sptr) {
        // (2) 类常量
        if (auto E = sptr->constants.find(p_name))           { r_ret = E->value; return true; }
        // (3) 静态变量（含 getter）
        if (auto E = sptr->static_variables_indices.find(p_name)) { /* ... */ }
        // (4) 信号 → 构造 Signal(owner, name)
        if (auto E = sptr->_signals.find(p_name))            { r_ret = Signal(owner, E->key); return true; }
        // (5) 方法 → 构造 Callable，按 rpc_config 选 RPC/普通
        if (likely(sptr->valid)) {
            if (auto E = sptr->member_functions.find(p_name)) {
                if (sptr->rpc_config.has(p_name)) {
                    r_ret = Callable(memnew(GDScriptRPCCallable(owner, E->key)));
                } else {
                    r_ret = Callable(owner, E->key);
                }
                return true;
            }
        }
        // (6) 内部子类
        if (auto E = sptr->subclasses.find(p_name))          { r_ret = E->value; return true; }
        // (7) 用户 _get(name) 回调
        if (likely(sptr->valid)) {
            if (auto E = sptr->member_functions.find(strings._get)) { /* 调用，nil 表示未处理 */ }
        }
        sptr = sptr->base.ptr();
    }
    return false;
}
```

七级回退构成 GDScript 的“属性访问语义”，理解了它就理解了第 14 章
里 `some_node.foo` 取出 RPC Callable 的真正落点（第 5 级）。

特别值得拆出来谈的几条：

### 16.4.1 信号也是“属性”

`signal hit` 在用户视角下是个独立的语言概念，但在 `_get` 阶段它
就是个名字——遇到就构造一个 `Signal(owner, name)` 返回。因此
`obj.hit.connect(...)` 与 `obj.foo.call(...)` 走的是同一条访问机制。

### 16.4.2 内部类作为属性

`Enemy.Behavior` 这种写法在 `_get` 阶段返回 `Ref<GDScript>`——内部
类本来就是个 Resource，可以当作普通值传递、`new()` 实例化，正是
这种统一性使内部类不需要任何特殊语法支持。

### 16.4.3 `_get` 回调与“透明属性”

第 7 级是用户自定义的 `_get(name)` 钩子。Godot 的 `Object::get` 在
找不到属性时会询问 ScriptInstance，ScriptInstance 又会问 `_get`
——这条链让 GDScript 用户能模拟“动态属性”，最常见的用法就是封装
一个字典作为对外 schema。

---

## 16.5 方法分发：`callp`

```cpp
Variant GDScriptInstance::callp(const StringName &p_method,
        const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
    GDScript *sptr = script.ptr();
    if (unlikely(p_method == SceneStringName(_ready))) {
        // 触发 @implicit_ready，递归到根
        _call_implicit_ready_recursively(sptr);
    }
    while (sptr) {
        if (likely(sptr->valid)) {
            if (auto E = sptr->member_functions.find(p_method)) {
                return E->value->call(this, p_args, p_argcount, r_error);
            }
        }
        sptr = sptr->base.ptr();
    }
    r_error.error = CALL_ERROR_INVALID_METHOD;
    return Variant();
}
```

主体非常朴素——沿 base 链找第一个有该方法的脚本，取出
`GDScriptFunction*` 直接调用。但它做了一件“额外的事”：

### 16.5.1 `_ready` 的拦截

任何对 `_ready` 的调用都会**先触发 `@implicit_ready`**：

```cpp
void GDScriptInstance::_call_implicit_ready_recursively(GDScript *p_script) {
    if (p_script->base.ptr())
        _call_implicit_ready_recursively(p_script->base.ptr());
    if (likely(p_script->valid) && p_script->implicit_ready) {
        Callable::CallError err;
        p_script->implicit_ready->call(this, nullptr, 0, err);
    }
}
```

递归方向是**从根到叶**（先调用基类的 `@implicit_ready`，再调用子
类的）——这样基类的 `@onready` 会先被赋值，符合“先父后子”的初
始化顺序。

为什么要把 `@onready` 处理放在 `_ready` 触发时而不是入树通知里？因
为节点入树（`NOTIFICATION_ENTER_TREE`）发生在 `_ready` 之前，但有
些 `@onready var x = $Child` 依赖的子节点可能要等子节点自己 `_ready`
才完整就绪。把时机定在 `_ready` 进场前做最后一次默认值赋值，是最
安全的折衷。

> Godot 的 `Node` 在收到 `NOTIFICATION_READY` 时主动调用脚本的
> `_ready` 方法，于是这条拦截就能稳定地为每个节点跑一次。

### 16.5.2 “找不到”的语义

`callp` 找不到方法时返回 `CALL_ERROR_INVALID_METHOD` 而不是直接
crash——上层（`Object::callp` 或 `Callable::call`）要据此决定是
返回 nil 还是抛错。这与 GDScript 用户层面 `obj.method_does_not_exist()`
报错的体验一致。

---

## 16.6 `notification`：与 `callp` 不同的分发策略

```cpp
void GDScriptInstance::notification(int p_notification, bool p_reversed) {
    if (unlikely(!script->valid)) return;

    Variant value = p_notification;
    const Variant *args[1] = { &value };
    const StringName &notification_str = strings._notification;

    LocalVector<GDScript *> script_stack;
    uint32_t script_count = 0;
    for (GDScript *sptr = script.ptr(); sptr; sptr = sptr->base.ptr(), ++script_count)
        script_stack.push_back(sptr);

    const int start = p_reversed ? 0 : script_count - 1;
    const int end   = p_reversed ? script_count : -1;
    const int step  = p_reversed ? 1 : -1;

    for (int idx = start; idx != end; idx += step) {
        GDScript *sc = script_stack[idx];
        if (likely(sc->valid)) {
            if (auto E = sc->member_functions.find(notification_str)) {
                Callable::CallError err;
                E->value->call(this, args, 1, err);
            }
        }
    }
}
```

注意它**和方法调用的语义完全不同**：

* `callp` 找到第一个匹配的方法后**就返回**——子类覆盖父类。
* `notification` **逐层都调用**——和 C++ 中 `_notification(int what)`
  的“each layer handles its own”行为对齐。

这背后的设计选择是：通知是“事件分发”而非“方法调用”，每一层基类
都有权对 `NOTIFICATION_READY` 之类的事件做响应；如果只调最底层版本，
基类的初始化逻辑就丢了。

`p_reversed` 参数让某些通知（如 `NOTIFICATION_PREDELETE`）按相反
顺序传播——析构通常希望从子到父。

`script_stack` 这个本地数组的存在是为了避免在循环中反复遍历 base
链（第二次遍历需要倒序）。LocalVector 是栈上的小数组，无堆分配。

---

## 16.7 热重载与 `reload_members`

热重载场景下，新版本脚本可能添加/删除字段，导致 `member_indices`
索引发生变动。`reload_members` 负责把旧实例的成员**按名字而非索引**
迁移到新布局：

```cpp
void GDScriptInstance::reload_members() {
#ifdef DEBUG_ENABLED
    Vector<Variant> new_members;
    new_members.resize(script->member_indices.size());

    // 按名字重映射
    for (KeyValue<StringName, GDScript::MemberInfo> &E : script->member_indices) {
        if (member_indices_cache.has(E.key)) {
            Variant value = members[member_indices_cache[E.key]];
            new_members.write[E.value.index] = value;
        }
    }

    members.resize(new_members.size());
    members = new_members;

    // 更新缓存为新索引
    member_indices_cache.clear();
    for (const KeyValue<StringName, GDScript::MemberInfo> &E : script->member_indices) {
        member_indices_cache[E.key] = E.value.index;
    }
#endif
}
```

* 使用了 `#ifdef DEBUG_ENABLED`——release 构建里没有 `member_indices_cache`，
  也就没有热重载支持，符合“运行时热重载是开发期能力”的定位。
* `member_indices_cache` 是上次重载（或创建时）的“旧→索引”表，与
  `script->member_indices` 的“新→索引”表配合就能完成迁移。
* 新增字段会得到默认 nil（用户脚本应在 `_init` 或属性赋值里给出值）；
* 删除字段对应的旧值就此丢失——这是设计上的取舍，避免引入“字段坟
  墓”累积内存。

热重载的整体流程会在第 23 章详细谈，这里只关注实例侧的承接。

---

## 16.8 析构：清理协程与脱离脚本

```cpp
GDScriptInstance::~GDScriptInstance() {
    MutexLock lock(GDScriptLanguage::get_singleton()->mutex);

    while (SelfList<GDScriptFunctionState> *E = pending_func_states.first()) {
        // 顺序很重要：清栈可能导致 state 自身被释放并从链表里移除
        pending_func_states.remove(E);
        GDScriptFunctionState *state = E->self();
        ObjectID state_id = state->get_instance_id();
        state->_clear_connections();
        if (ObjectDB::get_instance(state_id)) {
            state->_clear_stack();
        }
    }

    if (script.is_valid() && owner) {
        script->instances.erase(owner);
    }
}
```

两件事：

### 16.8.1 中断挂起的协程

实例上挂起的所有 `await`（每个对应一个 `GDScriptFunctionState`）必须
被显式中止：

* `_clear_connections()` 解除信号绑定，避免别的对象稍后唤醒它；
* `_clear_stack()` 释放栈快照里的 `Variant`，保证不会泄露引用。

代码里那条注释“**Order matters**”指出一个很微妙的问题：清栈过程中，
`GDScriptFunctionState` 自身可能被释放（因为它的引用计数归零），从
而**自动**从链表中摘掉这个 `SelfList` 元素。如果先 `_clear_stack`
再 `pending_func_states.remove(E)`，`E` 已经悬空了。所以代码刻意先
摘除节点再清理。

### 16.8.2 从脚本的实例集合里登记注销

```cpp
script->instances.erase(owner);
```

这是 15.5 节 `_create_instance` 里 `instances.insert(...)` 的对偶
操作——脚本对象始终维护一份“当前在用实例集合”，热重载、脚本卸载
时会用到。

**全程持有 `GDScriptLanguage::mutex`**：因为这两条链是跨线程共享的
（多人开发场景下脚本可能在主线程加载、在 worker 线程释放），不加
锁会破坏链表完整性。

---

## 16.9 属性枚举：`get_property_list`

引擎的 inspector、序列化系统都需要知道“一个对象有哪些属性”。这条
路径走 `GDScriptInstance::get_property_list`：

```cpp
void GDScriptInstance::get_property_list(List<PropertyInfo> *p_properties) const {
    const GDScript *sptr = script.ptr();
    List<PropertyInfo> props;
    // ... 顺着 base 链收集每一层的成员、调用 _get_property_list 钩子，
    //     最终拼接出一个有序列表 ...
}
```

它会按“**最派生类在前，基类在后**”的顺序拼装属性表，并在每一层调
用用户的 `_get_property_list()` 钩子追加自定义条目。这个顺序与 C++
节点的属性显示顺序一致，使得 inspector 中“子类自定义字段”出现在
顶部，便于编辑。

`validate_property` 与 `property_can_revert` / `property_get_revert`
也走类似的“沿 base 链问每个 `_validate_property` / `_property_can_revert`”
模式——不再展开。

---

## 16.10 设计回顾

GDScriptInstance 的设计可以总结为四点：

1. **轻量结构 + 重量委托**：实例本身只有 5 个字段，所有“类语义”都
   委托给 `script`；所有“宿主语义”都委托给 `owner`。
2. **扁平成员数组 + 多级回退访问**：常见路径 O(1)；语言特性需要的
   多级回退（setter/static/_set/_get/_validate_property）都按基类链
   线性遍历，复杂度可接受。
3. **方法分发 vs 通知分发的语义分离**：前者“查找第一个”，后者
   “逐层都调”。这是 OO 与事件系统的两种范式，混在同一对象里通过
   两个独立函数实现，互不干扰。
4. **生命周期与协程共生**：实例销毁时主动收尾自己挂起的协程；脚本
   销毁时枚举实例集合主动 `clear`——这两条链让 GDScript 的对象图
   始终“可被关闭”。

---

## 小结

* `GDScriptInstance` 是一个**轻量结构体**，仅持有 owner、script、
  扁平成员数组与挂起协程链表；
* 写入走 `set` 的多级回退（成员 → 静态 → `_set`），读取走 `get` 的
  七级回退（成员 → 常量 → 静态 → 信号 → 方法 → 子类 → `_get`）；
* `callp` 在 base 链上找到第一个匹配的方法并执行；`_ready` 会被
  专门拦截以触发 `@implicit_ready`；
* `notification` **不沿 base 链停下来**，每一层都调用——支持 OO
  式事件分发；
* `reload_members` 通过名字重映射在热重载时迁移成员；
* 析构时按特定顺序清理挂起的 `GDScriptFunctionState`，并从脚本的
  实例集合里注销自己；
* 整套设计将“字段访问的常见 O(1) 路径”与“多级语言特性回退”天然
  分离，使得复杂语义不会拖慢热路径。

下一章我们把视野从“GDScript 自己”再扩大一圈，看 GDScript 如何通过
`Variant` 与 `ClassDB` 与整个 Godot 引擎做互操作。
