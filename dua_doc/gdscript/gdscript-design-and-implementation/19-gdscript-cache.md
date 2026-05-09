# 第 19 章　`GDScriptCache`：浅/全双层缓存与循环依赖

GDScript 在编译期面对一个棘手的问题：**两个脚本互相 `preload` 怎么
办？**

```python
# a.gd
const B = preload("res://b.gd")
class_name A
func use_b() -> void: var b: B = B.new()

# b.gd
const A = preload("res://a.gd")
class_name B
extends A
```

如果按朴素的“加载 a → 看到 preload b → 加载 b → 看到 preload a →
…”流程，会立刻陷入死循环。

GDScript 解决这个问题的核心机制是 `GDScriptCache`——一个用**浅缓存
+ 全缓存** + **状态机化的 `GDScriptParserRef`** 的组合实现的二级
缓存系统。本章把这套机制完全拆开。

涉及的核心文件：

* `modules/gdscript/gdscript_cache.h`：缓存数据结构
* `modules/gdscript/gdscript_cache.cpp`：状态机与缓存查找/失效
* `modules/gdscript/gdscript_compiler.cpp`：`make_scripts` 的浅
  编译入口

---

## 19.1 三个缓存表 + 一个解析器表

```cpp
class GDScriptCache {
    HashMap<String, GDScriptParserRef *>   parser_map;
    HashMap<String, Vector<ObjectID>>      abandoned_parser_map;
    HashMap<String, Ref<GDScript>>         shallow_gdscript_cache;
    HashMap<String, Ref<GDScript>>         full_gdscript_cache;
    HashMap<String, Ref<GDScript>>         static_gdscript_cache;
    HashMap<String, HashSet<String>>       dependencies;
    HashMap<String, HashSet<String>>       parser_inverse_dependencies;
    static GDScriptCache *singleton;
    static SafeBinaryMutex<BINARY_MUTEX_TAG> mutex;
};
```

按用途分组：

| 表 | 含义 |
|----|------|
| `parser_map` | 路径 → `GDScriptParserRef*`，正在用的 Parser/Analyzer 对象 |
| `abandoned_parser_map` | 路径 → 已被 “abandon” 但仍在调用栈中的 Parser，避免在迭代过程中被销毁 |
| `shallow_gdscript_cache` | 路径 → 只有“类骨架”的 GDScript（无字节码） |
| `full_gdscript_cache` | 路径 → 完整编译完成、可调用的 GDScript |
| `static_gdscript_cache` | 全限定名 → 全局 `class_name` 脚本，让 `class_name` 引用 不必走文件系统 |
| `dependencies` | 反向：编译时记录“`p_owner` 依赖了哪些路径” |
| `parser_inverse_dependencies` | 路径 → 哪些 owner 引用了它，用于失效传播 |

它们共用一把 `SafeBinaryMutex`——可重入二元 mutex，允许同一线程多次
锁定，配合下文的“unlock 区域”使用。

---

## 19.2 `GDScriptParserRef`：把 Parser 状态机化

```cpp
class GDScriptParserRef : public RefCounted {
public:
    enum Status {
        EMPTY,
        PARSED,
        INHERITANCE_SOLVED,
        INTERFACE_SOLVED,
        FULLY_SOLVED,
    };
private:
    GDScriptParser   *parser   = nullptr;
    GDScriptAnalyzer *analyzer = nullptr;
    Status            status   = EMPTY;
    Error             result   = OK;
    String            path;
    uint32_t          source_hash = 0;
    bool              clearing  = false;
    bool              abandoned = false;
};
```

这个类的**核心创新**是把 Parser/Analyzer 的工作切成**5 个递增状态**：

| 状态 | 含义 | 触发函数 |
|------|------|----------|
| `EMPTY` | 刚创建，还没读过源码 | — |
| `PARSED` | 词法 + 语法树构造完毕 | `GDScriptParser::parse` |
| `INHERITANCE_SOLVED` | 解析出 `extends` 链，类间继承关系确定 | `Analyzer::resolve_inheritance` |
| `INTERFACE_SOLVED` | 解析出函数签名、成员类型，可被外部安全引用 | `Analyzer::resolve_interface` |
| `FULLY_SOLVED` | 解析完函数体，准备好生成字节码 | `Analyzer::resolve_body` |

`raise_status` 把状态从“目前到了哪一步”推进到“需要到哪一步”：

```cpp
Error GDScriptParserRef::raise_status(Status p_new_status) {
    if (p_new_status < status) return OK;

    while (result == OK && p_new_status > status) {
        switch (status) {
            case EMPTY: {
                get_parser()->clear();
                status = PARSED;
                String remapped_path = ResourceLoader::path_remap(path);
                if (remapped_path.has_extension("gdc")) {
                    Vector<uint8_t> tokens = GDScriptCache::get_binary_tokens(remapped_path);
                    source_hash = hash_djb2_buffer(tokens.ptr(), tokens.size());
                    result = get_parser()->parse_binary(tokens, path);
                } else {
                    String source = GDScriptCache::get_source_code(remapped_path);
                    source_hash = source.hash();
                    result = get_parser()->parse(source, path, false);
                }
            } break;
            case PARSED:              status = INHERITANCE_SOLVED; result = get_analyzer()->resolve_inheritance(); break;
            case INHERITANCE_SOLVED:  status = INTERFACE_SOLVED;   result = get_analyzer()->resolve_interface();   break;
            case INTERFACE_SOLVED:    status = FULLY_SOLVED;       result = get_analyzer()->resolve_body();        break;
            case FULLY_SOLVED:        return result;
        }
    }
    return result;
}
```

为什么要这样切？因为**循环依赖只在某些状态下才是问题**：

* 编译 `a.gd` 需要 `b.gd` 的**接口**（`B.new()` 的签名、`B` 是不是
  类）——只需要 `INTERFACE_SOLVED`；
* 不需要 `b.gd` 的**函数体**——`resolve_body` 可以稍后做。

这种“按需推进”使得 `a.gd` 编译期可以让 `b.gd` 只走到接口阶段就停
下，`b.gd` 反过来引用 `a.gd` 的接口，**两边都拿到对方的接口而无需
进入对方的函数体**——循环就此化解。

> 注意 `case PARSED` 与 `case EMPTY` 中状态赋值都**先于** Analyzer/
> Parser 的实际工作完成。这是有意为之：当 Analyzer 在 `resolve_*`
> 中递归请求别的脚本时，那个脚本可能反过来又请求当前脚本——此时
> 把 `status` 提前置为目标状态，能避免再次跑同一阶段；如果引发循环，
> 那么递归调用会看到“已经在工作中”的状态并安全退出。

---

## 19.3 `get_parser`：第一道屏障

```cpp
Ref<GDScriptParserRef> GDScriptCache::get_parser(const String &p_path,
        Status p_status, Error &r_error, const String &p_owner) {
    MutexLock lock(singleton->mutex);
    Ref<GDScriptParserRef> ref;

    if (!p_owner.is_empty() && p_path != p_owner) {
        singleton->dependencies[p_owner].insert(p_path);
        singleton->parser_inverse_dependencies[p_path].insert(p_owner);
    }

    if (singleton->parser_map.has(p_path)) {
        ref = Ref<GDScriptParserRef>(singleton->parser_map[p_path]);
        if (ref.is_null()) { r_error = ERR_INVALID_DATA; return ref; }
    } else {
        String remapped_path = ResourceLoader::path_remap(p_path);
        if (!FileAccess::exists(remapped_path)) {
            r_error = ERR_FILE_NOT_FOUND;
            return ref;
        }
        ref.instantiate();
        ref->path = p_path;
        singleton->parser_map[p_path] = ref.ptr();
    }
    r_error = ref->raise_status(p_status);
    return ref;
}
```

设计要点：

### 19.3.1 双向依赖记账

```cpp
singleton->dependencies[p_owner].insert(p_path);
singleton->parser_inverse_dependencies[p_path].insert(p_owner);
```

每次“**owner 要求 path 推进到某状态**”都被双向登记：

* `dependencies`：owner → 它依赖的所有路径，用于 `finish_compiling`
  时一路触发 owner 的依赖完成编译；
* `parser_inverse_dependencies`：path → 反过来谁依赖了它，用于失效
  时把所有依赖者一并失效（见 19.6）。

### 19.3.2 ParserRef 是“类内”单例

每个路径在 `parser_map` 中**只有一个** `GDScriptParserRef`——这是
循环依赖能正确收敛的前提。如果同时出现两个 ParserRef 各自跑到不同
状态，就会出现“Analyzer A 看到 B 的接口、Analyzer B 看到 A 的另一
份接口”的不一致。

### 19.3.3 `raise_status` 的同 mutex 调用

注意 `raise_status` 是**在 mutex 持有期间**调用的——也就是说递归
解析的全过程都在同一把锁内。这要求那把 mutex **可重入**（同一线程
能再次获取），就是 `SafeBinaryMutex<>` 设计的初衷。

---

## 19.4 `get_shallow_script`：制造类骨架

`get_shallow_script` 完成“**有 GDScript 对象，但还没编译字节码**”
的中间态：

```cpp
Ref<GDScript> GDScriptCache::get_shallow_script(const String &p_path,
        Error &r_error, const String &p_owner) {
    MutexLock lock(singleton->mutex);

    if (!p_owner.is_empty() && p_path != p_owner)
        singleton->dependencies[p_owner].insert(p_path);

    if (singleton->full_gdscript_cache.has(p_path))
        return singleton->full_gdscript_cache[p_path];   // (1)
    if (singleton->shallow_gdscript_cache.has(p_path))
        return singleton->shallow_gdscript_cache[p_path]; // (2)

    const String remapped_path = ResourceLoader::path_remap(p_path);

    Ref<GDScript> script;
    script.instantiate();
    script->set_path_cache(p_path);

    if (remapped_path.has_extension("gdc")) {
        script->set_binary_tokens_source(get_binary_tokens(remapped_path));
    } else {
        r_error = script->load_source_code(remapped_path);
    }
    if (r_error) return Ref<GDScript>();

    Ref<GDScriptParserRef> parser_ref = get_parser(p_path, GDScriptParserRef::PARSED, r_error);
    if (r_error == OK) {
        GDScriptCompiler::make_scripts(script.ptr(), parser_ref->get_parser()->get_tree(), true);
    }

    singleton->shallow_gdscript_cache[p_path] = script;
    return script;
}
```

关键步骤：

1. **优先看 full**——如果脚本已经完整编译完了，直接用；
2. **次看 shallow**——如果别的编译流程已经造出了骨架，复用它；
3. **真正构造**：实例化空 `GDScript` → 设临时路径 → 读源码/二进制
   → 取一个 `PARSED` 状态的 `ParserRef` → 调
   `GDScriptCompiler::make_scripts(..., p_keep_state=true)`。

**`make_scripts(..., true)`** 是关键的一步：它根据 AST 的内部类树
**递归创建所有内部 `GDScript` 对象**（设置 `_owner` / `subclasses`），
但**不生成任何字节码**——只搭骨架。这样：

* `script` 自己已经有 `Ref<GDScript>` 引用，可以被其它脚本作为类型
  引用（“B 是不是个类？是。”）；
* 内部类也都有 `Ref<GDScript>`，可以被外部按 `path::Inner` 寻址；
* **没有任何字节码**意味着不需要先解析函数体，循环可以中止。

“浅缓存”这个名字就来自这里——**有壳无肉**。

---

## 19.5 `get_full_script`：补完字节码

```cpp
Ref<GDScript> GDScriptCache::get_full_script(const String &p_path,
        Error &r_error, const String &p_owner, bool p_update_from_disk) {
    MutexLock lock(singleton->mutex);

    if (!p_owner.is_empty() && p_path != p_owner)
        singleton->dependencies[p_owner].insert(p_path);

    Ref<GDScript> script;
    r_error = OK;
    if (singleton->full_gdscript_cache.has(p_path)) {
        script = singleton->full_gdscript_cache[p_path];
        if (!p_update_from_disk) return script;          // 命中
    }

    if (script.is_null()) {
        script = get_shallow_script(p_path, r_error);
        if (script.is_null()) return script;
    }

    const String remapped_path = ResourceLoader::path_remap(p_path);

    if (p_update_from_disk) {
        // 重新读磁盘内容（CACHE_MODE_IGNORE 路径）
        // ...
    }

    {
        // 临时让出锁——见 19.5.1
        uint32_t allowance_id = WorkerThreadPool::thread_enter_unlock_allowance_zone(singleton->mutex);
        r_error = script->reload(true);
        WorkerThreadPool::thread_exit_unlock_allowance_zone(allowance_id);
    }

finish:
    singleton->full_gdscript_cache[p_path] = script;
    singleton->shallow_gdscript_cache.erase(p_path);

    // ResourceCache 注册
    script->set_path_cache(String());
    script->set_path(p_path, true);

    return script;
}
```

整套流程的核心一步是 `script->reload(true)`——它内部会触发 Analyzer
跑到 `FULLY_SOLVED` 然后调 `GDScriptCompiler::compile`，最终把字节码
塞进 `GDScript::member_functions` 等字段。完成后：

* 从 `shallow_gdscript_cache` 移到 `full_gdscript_cache`；
* 通过 `set_path(..., true)` 真正注册到 ResourceCache（`path_cache`
  期间是“假路径”，避免冲突）。

### 19.5.1 “unlock allowance zone”：死锁兜底

```cpp
uint32_t allowance_id = WorkerThreadPool::thread_enter_unlock_allowance_zone(singleton->mutex);
r_error = script->reload(true);
WorkerThreadPool::thread_exit_unlock_allowance_zone(allowance_id);
```

这是 Godot 的“可允许临时让出 mutex”机制。`script->reload` 内部可能
触发别的 ResourceLoader 调用（preload 链上的资源）。如果别的资源加
载本身又要拿 GDScriptCache mutex，就会死锁。

注释里说得很明白：

> Allowing lifting the lock might cause a script to be reloaded
> multiple times, which, as a last resort deadlock prevention
> strategy, is a good tradeoff.

也就是——**接受“极端情况下重复编译一次”作为代价，换不死锁**。

---

## 19.6 `remove_parser`：失效的级联传播

当一个脚本被 `remove_script`（例如热重载、文件被删除）时，所有依赖
它的脚本都必须被失效——否则它们持有的 `Ref<GDScript>` 会指向旧版本
的字节码。

```cpp
void GDScriptCache::remove_parser(const String &p_path) {
    MutexLock lock(singleton->mutex);

    if (singleton->parser_map.has(p_path)) {
        GDScriptParserRef *parser_ref = singleton->parser_map[p_path];
        parser_ref->abandoned = true;
        singleton->abandoned_parser_map[p_path].push_back(parser_ref->get_instance_id());
    }
    singleton->parser_map.erase(p_path);

    // 复制反向依赖再迭代——级联会修改这张表
    HashSet<String> ideps(singleton->parser_inverse_dependencies[p_path]);
    singleton->parser_inverse_dependencies.erase(p_path);
    for (String idep_path : ideps) {
        remove_parser(idep_path);
    }
}
```

两个细节：

### 19.6.1 abandon 而非立刻销毁

把 `parser_ref->abandoned = true` 并放进 `abandoned_parser_map`，
只是把它从 `parser_map` 摘掉，**没有销毁对象**。这是因为 `parser_ref`
此刻可能正在被某个调用栈使用（典型情况：A 的 Analyzer 正调用
`get_parser(b)` → 触发 b 的 reload → 触发 a 被 invalidated）。
立即销毁会让上层的 `Ref<>` 变野指针。

`abandoned_parser_map` 是个 `Vector<ObjectID>`——只存 ID，不存指针，
让 ObjectDB 充当“是否还活着”的真相源。等到清理时（`clear()` 或
`remove_script`），再用 ObjectID 查回去执行 `clear()`。

### 19.6.2 复制反向依赖再递归

```cpp
HashSet<String> ideps(singleton->parser_inverse_dependencies[p_path]);
```

为什么要先把 `ideps` 复制出来？因为 `remove_parser(idep_path)` 会
**反过来修改 `parser_inverse_dependencies`**（idep 失效会触发它的
依赖者也失效）。在迭代原 HashSet 时修改它会引发未定义行为，所以
显式拷贝。

---

## 19.7 `finish_compiling`：补全 owner 的依赖图

`get_full_script` 在编译时只编译当前文件——那么它依赖的其它脚本
什么时候被编译？答案是 `finish_compiling`：

```cpp
Error GDScriptCache::finish_compiling(const String &p_owner) {
    MutexLock lock(singleton->mutex);

    Ref<GDScript> script = get_cached_script(p_owner);
    singleton->full_gdscript_cache[p_owner] = script;
    singleton->shallow_gdscript_cache.erase(p_owner);

    HashSet<String> depends(singleton->dependencies[p_owner]);

    Error err = OK;
    for (const String &E : depends) {
        Error this_err = OK;
        get_full_script(E, this_err);              // 递归触发
        if (this_err != OK) err = this_err;
    }
    singleton->dependencies.erase(p_owner);
    return err;
}
```

这是个**“后向补完”** 步骤：

1. 编译 owner 时，由 `get_parser` / `get_shallow_script` 在
   `dependencies[owner]` 中累积出依赖路径；
2. owner 自己编译完后，调用 `finish_compiling(owner)` 让所有依赖
   都从 shallow 升到 full；
3. `dependencies[owner]` 清空，因为 owner 已“自给自足”。

为什么不能在编译过程中就让依赖编译到 full？因为那样会触发依赖去取
owner 的接口/字节码，而 owner 自己还没 full——又是循环。

“先全部 shallow 互相引用，最后再补 full”就是 GDScriptCache 化解循
环依赖的最终招式。

---

## 19.8 `static_gdscript_cache`：`class_name` 的快路径

`add_static_script` / `remove_static_script` 维护另一个完全独立的
表，键是脚本的**全限定名**（`class_name` 形态）：

```cpp
void GDScriptCache::add_static_script(Ref<GDScript> p_script) {
    ERR_FAIL_COND_MSG(!p_script->is_valid(), "Trying to cache non-compiled script as static.");
    singleton->static_gdscript_cache[p_script->get_fully_qualified_name()] = p_script;
}
```

它的存在是为了让 GDScript 中 `class_name MyClass` 声明的脚本在被
其它脚本以 `MyClass.new()` 引用时，能跳过 ResourceLoader 直接命中
——没有这一层，每次 `MyClass` 都要走文件系统/ResourceCache，开销
明显高于一次 hash 查找。

`add_static_script` 的前提条件是 `is_valid()`——只缓存已经完整编译
的脚本，避免 shallow 阶段就被人按 class_name 拿走。

---

## 19.9 设计回顾

`GDScriptCache` 体现了五条互相支撑的设计：

1. **状态机化的 Parser**：把“词法 → 继承 → 接口 → 函数体”切成 5 阶
   段，按需推进。这是循环依赖能化解的根本。
2. **浅 / 全双层缓存**：浅缓存提供 `Ref<GDScript>` 的稳定身份让循环
   引用先“引用得到”，全缓存最后才把字节码补上。
3. **双向依赖记账 + 级联失效**：编译期记“谁依赖谁”，失效时反向传
   播——保证热重载的一致性。
4. **abandon 而非立刻销毁**：把可能正被调用栈使用的 ParserRef 摘到
   `abandoned_parser_map`，依靠 ObjectID 检测真实存活情况，避免野
   指针。
5. **可重入 mutex + unlock 区域**：用 `SafeBinaryMutex` 让递归调用
   成为可能；用 `thread_enter_unlock_allowance_zone` 在调 `reload`
   时让出锁，以 “可能重复一次编译” 换 “永远不死锁”。

这套机制的本质是：**把“互相需要”的两个脚本拆成‘骨架引用 + 内容
引用’两个层次**。骨架是无内容的形状，可以无序构建；内容只在两边
骨架都搭好后才填入。这与编译原理中“符号表两遍扫描”的思路一脉相
承——只是 GDScript 把它推广到了**跨文件**与**运行时按需触发**的
场景。

---

## 小结

* `GDScriptCache` 用 3+1 个表（shallow / full / static + parser）+
  双向依赖图实现脚本资源的统一缓存；
* `GDScriptParserRef` 把 Parser/Analyzer 切成 5 个递增状态：EMPTY /
  PARSED / INHERITANCE_SOLVED / INTERFACE_SOLVED / FULLY_SOLVED；
* 循环依赖通过“**所有人先到 INTERFACE_SOLVED + shallow GDScript，
  再统一补 FULLY_SOLVED + full GDScript**”的两遍方式化解；
* `get_shallow_script` 用 `make_scripts(keep_state=true)` 创建“有壳
  无肉”的 GDScript，让循环引用立刻拿到稳定的 `Ref<>`；
* `get_full_script` 触发完整 reload，期间通过 `unlock allowance zone`
  让出 mutex 防止死锁，宁可重复编译也不死锁；
* `remove_parser` 用 `abandoned` 标记 + 反向依赖级联实现安全失效；
* `finish_compiling` 在 owner 编译完成后统一把依赖从 shallow 推到
  full；
* `static_gdscript_cache` 提供 `class_name` 的常量级查找快路径。

下一章我们将进入“**编辑器集成**”：从 LSP 服务、代码补全、文档生成，
到 EditorScript 的运行时机制——看 GDScript 在编辑器侧的工具角色。
