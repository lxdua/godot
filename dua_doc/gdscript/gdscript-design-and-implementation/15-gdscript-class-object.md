# 第 15 章　`GDScript` 类对象、内部类与继承

前面的所有章节都把 `GDScript` 当作一个“黑箱容器”来引用：编译器把字
节码塞进它，VM 从它取函数指针，Callable 从它构造实例……本章把这个
盒子彻底打开，看看它到底装了什么、为什么这样装，以及一个 `.gd` 文件
里嵌套的 `class Foo: ...` 是如何在内存里铺开的。

`GDScript` 这个类的特别之处在于它**同时承担多重身份**：

1. **资源**（继承自 `Resource`）：它是 `.gd` 文件在 ResourceLoader
   下的具体形态，可以被 `load()`、可以被引用计数。
2. **类（class object）**：它定义了一组成员变量、方法、信号、常量、
   静态变量——是面向对象意义上的“类”。
3. **类作用域**：它是字节码里所有 `static`/常量/内部类符号查找的容
   器。
4. **可调用对象**：`GDScript._new()` 让脚本类本身就是个工厂，可以被
   GDScript 用户像 `MyClass.new()` 一样使用。
5. **节点（subclass tree）**：通过 `subclasses` 和 `_owner` 形成一棵
   嵌套的内部类树。

把这五件事塞进同一个 C++ 类，是 GDScript 与 Godot 资源系统紧密耦合
的产物，也是后续章节谈“资源加载”“热重载”绕不开的前提。

涉及的核心文件：

* `modules/gdscript/gdscript.h`：`GDScript` 类定义
* `modules/gdscript/gdscript.cpp`：实例创建、继承查找、热重载入口

---

## 15.1 一个 `.gd` 文件 = 一个 `GDScript`（再加若干内部 `GDScript`）

来看一段最常见的脚本：

```python
# res://enemy.gd
class_name Enemy
extends CharacterBody2D

const MAX_HP := 100

class Behavior:
    var name: String = ""
    func tick() -> void:
        pass

class Aggressive extends Behavior:
    func tick() -> void:
        # ...
        pass

var hp := MAX_HP

func _ready() -> void:
    pass
```

加载完成后，内存里的对象图大致是：

```
GDScript (root)
├── path                  = "res://enemy.gd"
├── local_name            = &"Enemy"
├── global_name           = &"Enemy"      ← class_name
├── fully_qualified_name  = "res://enemy.gd"
├── _owner                = nullptr        ← 根脚本
├── native                = Ref<CharacterBody2D 的 Native 包装>
├── base                  = Ref<CharacterBody2D 对应的 GDScript 包装> 或 null
├── members               = { "hp" }
├── member_indices        = { "hp": {idx=0, ...} }
├── constants             = { "MAX_HP": 100 }
├── member_functions      = { "_ready": GDScriptFunction*,
│                              "@implicit_new": GDScriptFunction*, ... }
└── subclasses
    ├── "Behavior" → GDScript (inner)
    │     ├── _owner   = root
    │     ├── local_name = &"Behavior"
    │     ├── fully_qualified_name = "res://enemy.gd::Behavior"
    │     ├── native  = Ref<RefCounted>     ← 内部类默认基类
    │     └── ...
    └── "Aggressive" → GDScript (inner)
          ├── _owner = root
          ├── base   = Ref<Behavior 对应的 inner GDScript>
          └── ...
```

几个关键点：

* **每个内部类都是一个独立的 `GDScript` 对象**，与外层脚本一起被
  `Ref<>` 持有。这意味着内部类的字节码、成员表、构造函数和外层完全
  对等；所谓“内部”只是符号查找上的归属。
* **`_owner` 指向词法/源码上的外层 `GDScript`**，构成一棵纯结构性的
  树，**不是**继承关系。
* **`base` 指向继承的父类 `GDScript`**（可能是同一文件内的内部类，
  也可能是别的脚本，也可能为空），构成另外一棵继承图。
* `_owner` 与 `base` 是两个完全独立的关系——这是 GDScript 区别于很多
  其它脚本语言的地方。Java 之类的“内部类自动持有外层引用”在 GDScript
  里**不存在**：内部类实例完全不知道外层实例。
* **`native`** 是这条 `base` 链最终通向的引擎原生类（如
  `CharacterBody2D`），它是一切非脚本继承的“锚点”。

---

## 15.2 字段速览：`GDScript` 内部都装了什么

按用途分组，挑出最关键的字段（精简版）：

```cpp
class GDScript : public Script {
    // —— 继承与归属 ——
    Ref<GDScriptNativeClass> native;       // 引擎侧原生基类
    Ref<GDScript>            base;         // 脚本侧直接父类
    GDScript                *_owner;       // 词法外层（内部类用）
    HashMap<StringName, Ref<GDScript>> subclasses; // 内部类表

    // —— 成员变量布局 ——
    HashMap<StringName, MemberInfo> member_indices; // 含所有基类的成员（用于实例数组寻址）
    HashSet<StringName>             members;        // 仅本类成员
    HashMap<StringName, MemberInfo> static_variables_indices;
    Vector<Variant>                 static_variables;

    // —— 函数、常量、信号、RPC ——
    HashMap<StringName, Variant>            constants;
    HashMap<StringName, GDScriptFunction *> member_functions;
    HashMap<StringName, MethodInfo>         _signals;
    Dictionary                              rpc_config;

    // —— 特殊函数捷径 ——
    GDScriptFunction *initializer;          // 用户写的 _init
    GDScriptFunction *implicit_initializer; // @implicit_new（成员默认值赋值）
    GDScriptFunction *implicit_ready;       // @implicit_ready（@onready 处理）
    GDScriptFunction *static_initializer;   // @static_initializer

    // —— Lambda 信息 ——
    HashMap<GDScriptFunction *, LambdaInfo> lambda_info;

    // —— 实例追踪 / 热重载 ——
    RBSet<Object *>                instances;
    List<UpdatableFuncPtr *>       func_ptrs_to_update;
    SelfList<GDScriptFunctionState>::List pending_func_states;

    // —— 元数据 ——
    String     source;            // 源代码字符串
    Vector<uint8_t> binary_tokens;// 二进制 .gdc 缓冲
    String     path;              // res:// 路径
    StringName local_name;        // 内部类名 或 class_name
    StringName global_name;       // class_name
    String     fully_qualified_name; // "<path>::Inner1::Inner2"
};
```

大致可以分作三层：**结构层**（前 4 项）、**语义层**（成员/函数/常量/
信号）、**运行层**（实例集合、热重载链、协程链）。这种把“类的元数据”
和“运行期状态”混合在同一个对象上的设计是 Godot 资源风格的延续——
`Resource` 自身也常常这样。

---

## 15.3 `member_indices` 与 `members`：为什么要两份

这是初看代码最容易困惑的地方：

```cpp
HashMap<StringName, MemberInfo> member_indices; // 含所有基类
HashSet<StringName>             members;        // 仅本类
```

* **`members`** 用于回答“这个类自己定义了哪些字段”——例如 `_set` /
  `_get` 的属性列表生成（第 16 章会讲）。
* **`member_indices`** 用于回答“某个名字对应实例数组里的哪个槽位”，
  并且**包含从所有基类继承下来的成员**——因为 GDScriptInstance 里
  `members` 是个**扁平 `Vector<Variant>`**：

```cpp
class GDScriptInstance : public ScriptInstance {
    Vector<Variant> members;
    // ...
};
```

无论变量在哪一层基类定义，运行期都共用这一个数组、按 `index` 直接
寻址。这种“扁平继承”是**字节码访问 O(1) 的关键**——对应的指令
`OPCODE_SET_MEMBER` / `OPCODE_GET_MEMBER` 只需要一个整型索引就能拿到
变量，不用走任何 hash 查表。

代价是：编译期必须在 `_prepare_compilation()` 时**把基类 ABI 完全确定
下来**，这也是为什么 `GDScriptCompiler` 的两阶段编译（第 6 章）必不
可少。

---

## 15.4 三个隐式构造函数

`GDScript` 上挂着三个用户写不出来的“合成函数”：

```cpp
GDScriptFunction *initializer          = nullptr; // 用户 _init
GDScriptFunction *implicit_initializer = nullptr; // @implicit_new
GDScriptFunction *implicit_ready       = nullptr; // @implicit_ready
GDScriptFunction *static_initializer   = nullptr; // @static_initializer
```

它们各自的职责：

| 函数 | 何时被调用 | 干了什么 |
|------|------------|----------|
| `@implicit_new` | 实例创建早期 | 给所有成员变量赋默认值（包括类型化字段的零值与字面量初值） |
| `_init`         | `@implicit_new` 之后 | 用户写的初始化逻辑 |
| `@implicit_ready` | 节点 `_ready` 触发时 | 处理所有 `@onready var` 的右值求值 |
| `@static_initializer` | 脚本首次加载时 | 给所有 `static var` 赋默认值 |

为什么要把成员默认值单独拆成 `@implicit_new`，而不是塞进 `_init`？

* **基类先于子类完成默认值赋值**——`_super_implicit_constructor` 会
  递归调用基类的 `@implicit_new`，然后才执行子类的；这与其他 OO 语
  言中“父类成员先初始化”的语义一致。
* **`_init` 是用户函数，可能根本不存在**；强制要求用户写 `_init` 才
  能初始化字段是反直觉的。
* **静态分析、IDE 跳转**可以直接看 `_init` 的源码而不被默认值赋值代
  码污染。

类似地，`@onready` 必须等到节点入场景树才能解引用 `$Node`，所以单独
做成 `@implicit_ready`，由 `Node` 在 `_ready` 通知里调起。

---

## 15.5 实例创建链：`_create_instance`

```cpp
GDScriptInstance *GDScript::_create_instance(const Variant **p_args, int p_argcount,
                                             Object *p_owner, Callable::CallError &r_error) {
    // STEP 1, CREATE
    GDScriptInstance *instance = memnew(GDScriptInstance);
    instance->members.resize(member_indices.size());     // 一次性分配扁平数组
    instance->script = Ref<GDScript>(this);
    instance->owner  = p_owner;
    instance->owner_id = p_owner->get_instance_id();
    instance->owner->set_script_instance(instance);

    // STEP 2, INITIALIZE AND CONSTRUCT
    instances.insert(instance->owner);                   // 加入跟踪集合（热重载需要）

    _super_implicit_constructor(this, instance, r_error); // 沿 base 链递归赋默认值
    if (r_error.error != Callable::CallError::CALL_OK) { /* 回滚并返回 */ }

    if (p_argcount < 0) return instance;                 // 仅创建、不调 _init

    GDScriptFunction *applicable_initializer = _super_constructor(this);
    if (applicable_initializer != nullptr) {
        applicable_initializer->call(instance, p_args, p_argcount, r_error);
        if (r_error.error != Callable::CallError::CALL_OK) { /* 回滚 */ }
    }
    return instance;
}
```

注意几个细节：

* **`members.resize(member_indices.size())`**：扁平数组的容量等于含基
  类的成员总数——一次分配，零增长。
* **`set_script_instance(instance)` 必须在调用 `@implicit_new` 之前**
  ——因为默认值表达式里可能包含 `self.something = expr`，必须先让
  `Object` 知道脚本实例。
* **`instances.insert(instance->owner)`** 是为了热重载时能枚举到所有
  在用实例并迁移它们的状态。
* **`_super_constructor`** 与 `_super_implicit_constructor` 不一样：
  `_init` 只调用**最派生类自己的版本**（如果没有就沿 `base` 找一个），
  这是 GDScript 用户能感知到的 OO 行为；而 `@implicit_new` 是从根到
  叶**全部都执行**——因为每一层都要给自己那部分成员赋默认值。

```cpp
GDScriptFunction *GDScript::_super_constructor(GDScript *p_script) {
    if (likely(p_script->valid) && p_script->initializer) {
        return p_script->initializer;            // 找到第一个有 _init 的就停
    }
    GDScript *base_src = p_script->base.ptr();
    return base_src ? _super_constructor(base_src) : nullptr;
}

void GDScript::_super_implicit_constructor(GDScript *p_script,
        GDScriptInstance *p_instance, Callable::CallError &r_error) {
    if (p_script->base.is_valid()) {
        _super_implicit_constructor(p_script->base.ptr(), p_instance, r_error);
        if (r_error.error != Callable::CallError::CALL_OK) return;
    }
    p_script->implicit_initializer->call(p_instance, nullptr, 0, r_error);
}
```

这两个函数的差异精确反映了上面那个表。

---

## 15.6 继承查找：`base` 链的三个用途

`base` 是个 `Ref<GDScript>`，构成一条**自下而上**的链表。它在三个地
方被频繁遍历：

### 15.6.1 类型判断：`inherits_script`

```cpp
bool GDScript::inherits_script(const Ref<Script> &p_script) const {
    Ref<GDScript> gd = p_script;
    if (gd.is_null()) return false;
    const GDScript *s = this;
    while (s) {
        if (s == p_script.ptr()) return true;
        s = s->base.ptr();
    }
    return false;
}
```

支持 `is`、`as` 这类语言级类型判断的最终落点。注意它只比较指针——
两个加载自同一文件的 `GDScript` 实例必须是同一个对象。这条不变量
由 `GDScriptCache`（第 19 章）维护。

### 15.6.2 信号查找：`has_script_signal`

```cpp
bool GDScript::has_script_signal(const StringName &p_signal) const {
    if (_signals.has(p_signal)) return true;
    if (base.is_valid()) return base->has_script_signal(p_signal);
    return false;
}
```

很多“查 X 是否存在”的接口都是这种递归形式：先看自己，再问基类。

### 15.6.3 成员/方法解析

虽然 `member_indices` 已经把基类成员铺平了，但**方法**没有铺平：

```cpp
HashMap<StringName, GDScriptFunction *> member_functions; // 仅本类
```

调用一个方法时，VM 从最派生类的 `member_functions` 开始找，找不到就
沿 `base` 上溯。原因有两个：

1. **方法可能被覆盖**——铺平表里只能保留一个版本，但 VM 在 `super`
   调用时还想取到父类版本，需要保留分层结构。
2. **方法的查找频率比变量低得多**——多一次 hash 查找的代价可以接受。

---

## 15.7 内部类导航：`find_class` 与 `_owner`

`find_class` 解决一个问题：编译器在生成 `extends Behavior` 这种引用
时，留下的是一个**全限定名**字符串，例如 `"res://enemy.gd::Behavior"`，
运行时怎么找到对应的 `GDScript`？

```cpp
GDScript *GDScript::find_class(const String &p_qualified_name) {
    String first = p_qualified_name.get_slice("::", 0);

    Vector<String> class_names;
    GDScript *result = nullptr;

    if (first.is_empty() || first == global_name) {
        // 以本类为根，直接展开
        class_names = p_qualified_name.split("::");
        result = this;
    } else if (p_qualified_name.begins_with(get_root_script()->path)) {
        // 名字是 path::Inner::... 的形式，从根脚本展开
        class_names = p_qualified_name.trim_prefix(get_root_script()->path).split("::");
        result = get_root_script();
    } else if (auto E = subclasses.find(first)) {
        // 直接在自己的内部类表里找到了
        class_names = p_qualified_name.split("::");
        result = E->value.ptr();
    } else if (_owner != nullptr) {
        // 沿 _owner 链向外查
        return _owner->find_class(p_qualified_name);
    }

    for (int i = 1; result != nullptr && i < class_names.size(); i++) {
        if (auto E = result->subclasses.find(class_names[i])) {
            result = E->value.ptr();
        } else {
            return nullptr;
        }
    }
    return result;
}
```

这个函数体现了内部类的**词法作用域**：

* 内部类可以直接看到同一文件中其它内部类（沿 `_owner` 上溯到根，再
  沿 `subclasses` 下探）；
* 它**不能**看到别的脚本里的内部类——那种引用必须经由 `class_name`
  全局符号或 `preload()` 建立；
* `fully_qualified_name` 本质上是 `path::Outer::Inner` 的字符串
  路径，是这棵树的**唯一坐标**。

`get_root_script()` 是 `_owner` 链的封顶操作：

```cpp
GDScript *GDScript::get_root_script() {
    GDScript *result = this;
    while (result->_owner) result = result->_owner;
    return result;
}
```

被多个查找函数复用——任何"以文件为单位"的操作（路径解析、热重
载、缓存查找）都要先升到根。

---

## 15.8 静态变量：另一套小型实例

```cpp
HashMap<StringName, MemberInfo> static_variables_indices;
Vector<Variant>                 static_variables;
GDScriptFunction               *static_initializer = nullptr;
```

注意 `static_variables` 直接挂在 `GDScript`（也就是“类对象”）上——
`GDScriptInstance` 不存它们。这与“静态变量是类级别的”语义匹配。
访问时通过 `OPCODE_GET_STATIC_VARIABLE` / `OPCODE_SET_STATIC_VARIABLE`
按索引读写，与实例成员几乎是一套机制的镜像。

`@static_initializer` 在脚本首次有效化时由 `_static_init` 调用：

```cpp
Error _static_init();           // 调用 @static_initializer
void  _static_default_init();   // 给类型化静态变量赋零值
```

热重载时还会保留一份 `old_static_variables` 做迁移——这与第 23 章
讨论的状态保留密切相关。

---

## 15.9 Lambda 和 Updatable 函数指针：与本章的关系

第 13 章已经讨论过 `GDScriptLambdaCallable`，它持有的 `function` 指
针实际上是个 `UpdatableFuncPtr`：

```cpp
class GDScript::UpdatableFuncPtr {
    GDScriptFunction *ptr;
    GDScript         *script;
    List<UpdatableFuncPtr *>::Element *list_element;
};

List<UpdatableFuncPtr *>  func_ptrs_to_update;   // 挂在 GDScript 上
Mutex                     func_ptrs_to_update_mutex;
```

每个活跃的 `UpdatableFuncPtr` 在构造时把自己注册到所属脚本的
`func_ptrs_to_update` 链表里，析构时摘除。脚本热重载时执行：

```cpp
void GDScript::_recurse_replace_function_ptrs(
        const HashMap<GDScriptFunction *, GDScriptFunction *> &p_replacements) const;
```

按照 `subclasses` 树递归地把所有旧的函数指针替换为新版本。这就是 13
章里那个“lambda 在 reload 后仍然有效”的具体机制——它建立在
`subclasses` 这棵树上，所以也属于本章话题。

---

## 15.10 协程的脚本归属：`pending_func_states`

```cpp
SelfList<GDScriptFunctionState>::List pending_func_states;
```

第 12 章里挂起的 `GDScriptFunctionState` 也会把自己注册到
`pending_func_states`。这条链的作用是：

* **脚本析构 / 热重载时**，必须先把所有挂起的协程中断
  （`cancel_pending_functions`），否则它们恢复时会引用到已经失效的字
  节码。
* **诊断**：调试器可以遍历这个链来展示“当前还有哪些 `await` 在等”。

这条链与 `func_ptrs_to_update` 联手保证了 GDScript 在“运行期被替换
代码”这种激进操作下仍能保持一致性。

---

## 15.11 路径与命名：`path` / `local_name` / `global_name` / `fully_qualified_name`

四个字段经常被新读者搞混：

| 字段 | 含义 | 示例 |
|------|------|------|
| `path` | 文件系统路径（仅根脚本有） | `"res://enemy.gd"` |
| `local_name` | 类自身名字（内部类用，根类不一定有） | `"Behavior"` 或 `"Enemy"` |
| `global_name` | `class_name` 声明的全局名，仅根脚本可有 | `"Enemy"` |
| `fully_qualified_name` | 在 `find_class` 等处使用的唯一坐标 | `"res://enemy.gd::Behavior"` |

`path_valid` 这个 bool 标记当前 `path` 是不是“真路径”——内存中临时
构造的脚本（例如编辑器里没保存的 buffer）也会有 `path`，但仅作为
ID 用，不能用来 `ResourceLoader::load`。

---

## 15.12 设计回顾

回看 `GDScript` 这个类的设计：

1. **资源 + 类两位一体**：让 `.gd` 文件天然融入 Godot 的资源/引用计
   数体系，省掉了一套独立的“类管理器”。
2. **扁平实例 + 分层方法**：成员变量铺平到大数组以加速访问，方法保
   留分层结构以支持覆盖与 `super` 调用——空间效率与查找效率的不同
   折衷。
3. **结构 (`_owner`) 与继承 (`base`) 解耦**：内部类的词法归属和运行
   时继承彼此独立，避免“内部类隐式持有外层引用”这种容易引发循环引用
   的设计。
4. **隐式函数三件套**：`@implicit_new` / `@implicit_ready` /
   `@static_initializer` 把“非用户编写但必须存在”的初始化代码独立成
   函数，统一走 VM——既不用在引擎侧硬写 C++ 初始化逻辑，又能享受字
   节码带来的灵活性。
5. **运行期可变性**：`func_ptrs_to_update`、`pending_func_states`、
   `instances` 三条链让脚本类**在运行中被原地替换**成为可能，是热重
   载体验的根基。

---

## 小结

* `GDScript` 类是一个**资源 + 类对象 + 作用域 + 工厂**的复合体；
* 一个 `.gd` 文件对应一个根 `GDScript`，每个内部 `class` 对应一个
  独立的内部 `GDScript`，由 `_owner` 串成树、由 `subclasses` 索引；
* 继承通过 `base` 单链表表达，`native` 是这条链最终连接的引擎类；
* `member_indices` 包含所有基类成员、用于扁平数组寻址；`members` 仅
  含本类成员，用于属性枚举；
* `_create_instance` 完成 `new ScriptInstance + resize members +
  super_implicit + super_init` 四步，是所有实例诞生的唯一入口；
* `find_class` 沿 `_owner`/`subclasses` 双向遍历，实现内部类的词法
  作用域查找；
* `pending_func_states` 与 `func_ptrs_to_update` 让协程和 lambda 在
  脚本被热重载时不至于野指针。

下一章我们将进入 `GDScriptInstance`，看看实例侧那个扁平 `members`
数组到底如何与 `Object`、`Script`、字节码三方协作。
