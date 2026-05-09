# 第 23 章　调试器与 Profiler

调试器（Debugger）与性能采集器（Profiler）是 GDScript 工具链上最后
也最“侵入式”的一块拼图：它们要在不改变虚拟机正常语义的前提下，
让每一行 GDScript 代码都能：

* **暂停**——遇到断点时主线程停下来等用户继续；
* **观察**——把当前调用栈、局部变量、成员变量、全局表搬给编辑器；
* **单步**——逐行/逐过程推进；
* **统计**——记录每个函数被调用的次数、自身耗时、累计耗时、native
  方法耗时；
* **热重载**——脚本源码改动后，旧实例不丢状态地切换到新版字节码。

它们之所以能放到本书最后一章讲，是因为前面所有篇幅积累的概念
（OPCODE_LINE 指令、`CallLevel` 调用栈、`enter_function/exit_function`
钩子、`GDScriptCache` 反向依赖图）几乎都会被这一章重新组合起来用一遍。

涉及的核心文件：

* `modules/gdscript/gdscript.h`：`CallLevel` / `enter_function` /
  `exit_function` / `track_call_stack`
* `modules/gdscript/gdscript_byte_codegen.cpp`：`write_newline` /
  `write_breakpoint`
* `modules/gdscript/gdscript_vm.cpp`：`OPCODE_LINE` / `OPCODE_BREAKPOINT`
  分发、profile 采样
* `modules/gdscript/gdscript_editor.cpp`：`debug_break_*` /
  `debug_get_stack_*`
* `modules/gdscript/gdscript.cpp`：`profiling_start/stop` / `reload`

---

## 23.1 一切从一条“看不见的指令”开始

GDScript 的字节码里有一条专门为调试器服务的指令：`OPCODE_LINE`。
它没有任何业务语义，只携带一个整数——当前源码行号。

```cpp
// gdscript_byte_codegen.cpp
void GDScriptByteCodeGenerator::write_newline(int p_line) {
    if (GDScriptLanguage::get_singleton()->should_track_call_stack()) {
        // Add newline for debugger and stack tracking if enabled in
        // the project settings.
        append_opcode(GDScriptFunction::OPCODE_LINE);
        append(p_line);
        current_line = p_line;
    }
}
```

`should_track_call_stack()` 这层判断决定了这条指令**根本不会被发射**：

```cpp
// gdscript.cpp::GDScriptLanguage::init()
track_call_stack = GLOBAL_DEF_RST(
    "debug/settings/gdscript/always_track_call_stacks", false);
track_locals = GLOBAL_DEF_RST(
    "debug/settings/gdscript/always_track_local_variables", false);

#ifdef DEBUG_ENABLED
track_call_stack = true;
track_locals = track_locals || EngineDebugger::is_active();
#endif
```

也就是说：

* **release 构建**默认连 `OPCODE_LINE` 都不存在。脚本字节码体积更
  小、VM 主循环跳转更密集（少一类 case），分支预测更友好；
* **debug 构建**强制 `track_call_stack = true`，每个语句前都会有一
  条 `OPCODE_LINE`，于是 VM 就拥有了一根天然的“断点检查节拍器”；
* 用户在 release 里也可以打开 `always_track_call_stacks`，专门为线
  上崩溃栈或第三方 profiler 做准备。

这是 Godot 设计中很典型的“**功能 = 编译期开关 + 运行期开关**”：
取舍既不在头文件里硬编码、又不必每帧检查 `is_debugger_active()`。

## 23.2 编译器在哪里发射 `OPCODE_LINE`

`OPCODE_LINE` 不是“每条字节码前一条”，而是“**每条 AST 语句前一
条**”。来看 `GDScriptCompiler::_parse_block`：

```cpp
// gdscript_compiler.cpp
gen->write_newline(s->start_line);   // 每个语句节点
// ...
gen->write_newline(branch->start_line);  // match 的每个分支
// ...
codegen.generator->write_newline(field->start_line);  // 字段初始化
```

为什么是“每个语句”而不是“每个表达式”？

* 调试器面向的是**用户**而不是字节码——用户能看到的最小单位是源码
  的一行；
* 一条复杂表达式可能产生几十条字节码（操作符、临时变量、隐式转换
  等），如果都打 `OPCODE_LINE`，运行时检查会爆炸；
* GDScript 的语义本身**以语句为分界点**：单步时跳到下一条语句而不
  是下一个临时变量。

`write_breakpoint` 则只在遇到 GDScript 关键字 `breakpoint` 时发射
一次：

```cpp
void GDScriptByteCodeGenerator::write_breakpoint() {
    append_opcode(GDScriptFunction::OPCODE_BREAKPOINT);
}
```

这条指令是用户主动埋的“源码级断点”——比 IDE 设置的断点更可靠，
因为它已经被烤进字节码，不会被热重载或编辑器重启打散。

## 23.3 VM 主循环里的“断点节拍器”

来看 VM 中这两条指令的处理。先看 `OPCODE_BREAKPOINT`，它是最简单
的版本——直接把控制权交给 `EngineDebugger`：

```cpp
// gdscript_vm.cpp
OPCODE(OPCODE_BREAKPOINT) {
#ifdef DEBUG_ENABLED
    if (EngineDebugger::is_active()) {
        GDScriptLanguage::get_singleton()->debug_break(
            "Breakpoint Statement", true);
    }
#endif
    ip += 1;
}
DISPATCH_OPCODE;
```

再看 `OPCODE_LINE`，逻辑稍多——它身兼“**单步控制器**”+“**行断点
匹配器**”+“**调试事件轮询器**”三职：

```cpp
OPCODE(OPCODE_LINE) {
    CHECK_SPACE(2);
    line = _code_ptr[ip + 1];
    ip += 2;

    if (EngineDebugger::is_active()) {
        bool do_break = false;

        // (1) 单步控制：lines_left > 0 时倒计时
        if (unlikely(EngineDebugger::get_script_debugger()
                     ->get_lines_left() > 0)) {
            if (EngineDebugger::get_script_debugger()
                    ->get_depth() <= 0) {
                EngineDebugger::get_script_debugger()->set_lines_left(
                    EngineDebugger::get_script_debugger()
                        ->get_lines_left() - 1);
            }
            if (EngineDebugger::get_script_debugger()
                    ->get_lines_left() <= 0) {
                do_break = true;
            }
        }

        // (2) 行断点匹配
        if (EngineDebugger::get_script_debugger()
                ->is_breakpoint(line, source)) {
            do_break = true;
        }

        // (3) 触发暂停
        if (unlikely(do_break)) {
            GDScriptLanguage::get_singleton()->debug_break(
                "Breakpoint", true);
        }

        // (4) 让 EngineDebugger 处理网络消息（继续/单步/查变量等）
        EngineDebugger::get_singleton()->line_poll();
    }
}
DISPATCH_OPCODE;
```

四个分支的语义对应了用户在编辑器里能按下的所有按钮：

| 用户操作    | EngineDebugger 状态    | 触发分支              |
| ----------- | ---------------------- | --------------------- |
| Step Over   | `lines_left=1, depth≥0`| (1) depth 内倒计时    |
| Step Into   | `lines_left=1, depth=0`| (1) 任意层级倒计时    |
| Continue    | `lines_left=0`         | 仅看 (2)              |
| 行断点      | `is_breakpoint=true`   | (2)                   |
| 暂停按钮    | 主动设 `lines_left=1`  | (1)                   |
| 后台变量刷新| 不暂停                 | (4) `line_poll`       |

`get_depth()` 与 `enter_function/exit_function` 中的 `set_depth(±1)`
对应——这是 Step Over 区别于 Step Into 的关键：

```cpp
// gdscript.h::enter_function
ScriptDebugger *script_debugger = EngineDebugger::get_script_debugger();
if (script_debugger != nullptr
    && script_debugger->get_lines_left() > 0
    && script_debugger->get_depth() >= 0) {
    script_debugger->set_depth(script_debugger->get_depth() + 1);
}
```

* Step Over 调用前会把 `depth` 设为 0，进入新函数后 `depth=1`，此
  时 `OPCODE_LINE` 看到 `depth>0` **不**消耗 `lines_left`，于是单
  步“跨越”了整个被调函数；
* Step Into 把 `depth` 设为 -1，`enter_function` 那段 `if` 条件
  `depth >= 0` 不成立，`depth` 保持负数，单步时不再判断 `depth<=0`
  的分支语义即“立刻匹配”。

## 23.4 触发暂停：`debug_break` 的细节

`OPCODE_LINE` 真正决定要停下来时，调用的是脚本语言层的
`debug_break`：

```cpp
// gdscript_editor.cpp
bool GDScriptLanguage::debug_break(const String &p_error,
                                   bool p_allow_continue) {
    if (EngineDebugger::is_active()) {
        _debug_parse_err_line = -1;
        _debug_parse_err_file = "";
        _debug_error = p_error;
        bool is_error_breakpoint = p_error != "Breakpoint";
        EngineDebugger::get_script_debugger()->debug(
            this, p_allow_continue, is_error_breakpoint);
        // Because this is thread local, clear the memory afterwards.
        _debug_parse_err_file = String();
        _debug_error = String();
        return true;
    } else {
        return false;
    }
}
```

它做了两件事：

1. 把 `_debug_error`（thread_local）写成 `"Breakpoint"` 或具体错误
   字符串——这是后续 `debug_get_error()` 给编辑器看的“暂停理由”；
2. 调用 `EngineDebugger::debug()`，这是引擎层的同步阻塞点：在主线
   程里，它会**把控制权交给调试器**直到收到 continue 命令。

注意这里有一个孪生 API `debug_break_parse`：

```cpp
bool GDScriptLanguage::debug_break_parse(const String &p_file,
                                         int p_line,
                                         const String &p_error) {
    if (EngineDebugger::is_active()
        && Thread::get_caller_id() == Thread::get_main_id()) {
        _debug_parse_err_line = p_line;
        _debug_parse_err_file = p_file;
        _debug_error = p_error;
        EngineDebugger::get_script_debugger()->debug(this, false, true);
        // ...
    }
}
```

`reload()` 在解析或类型检查失败时会调它，让编辑器**直接跳到出错的
脚本/行**——这是一种“伪栈帧”：脚本还没运行，所以没有真正的调用
栈，于是把 `_debug_parse_err_line ≥ 0` 当作一个 sentinel，`debug_get_stack_level_count()` 在这种情况下返回 1，制造一个假的“第 0 层
栈帧”给编辑器显示。

## 23.5 调用栈快照：`CallLevel` 反向链表

`debug_get_stack_level_*` 这一组方法是 ScriptLanguage 接口的核心实
现，用来回答编辑器“当前栈是什么样子”这个问题。底层全部基于
`_call_stack` 这条**线程本地反向链表**：

```cpp
// gdscript.h
struct CallLevel {
    Variant *stack = nullptr;
    GDScriptFunction *function = nullptr;
    GDScriptInstance *instance = nullptr;
    int *ip = nullptr;
    int *line = nullptr;
    CallLevel *prev = nullptr; // Reverse linked list (stack).
};

static thread_local CallLevel *_call_stack;
static thread_local uint32_t _call_stack_size;
```

链表节点的内存**不在堆上分配**——`enter_function` 接受一个外部传
入的 `CallLevel *`，它的真实存储是 VM 主循环栈帧上的局部变量。这种
设计避免了热路径上的 `new/delete`，代价是栈结构必须严格 LIFO。

`_call_stack_size` 的存在让 `debug_get_stack_level_line(p_level)` 这
种“按索引查”的接口能在 O(level) 内完成；而 `_call_stack` 头指针
让 `enter_function/exit_function` 是 O(1)。

`debug_get_stack_level_locals` 在调用栈节点的基础上又叠了一层
**反向索引**——`debug_get_stack_member_state`：

```cpp
// gdscript_editor.cpp
void GDScriptLanguage::debug_get_stack_level_locals(int p_level,
        List<String> *p_locals, List<Variant> *p_values, ...) {
    // ...
    CallLevel *cl = _get_stack_level(p_level);
    GDScriptFunction *f = cl->function;

    List<Pair<StringName, int>> locals;
    f->debug_get_stack_member_state(*cl->line, &locals);

    for (const Pair<StringName, int> &E : locals) {
        p_locals->push_back(E.first);
        if (f->constant_map.has(E.first)) {
            p_values->push_back(f->constant_map[E.first]);
        } else {
            p_values->push_back(cl->stack[E.second]);
        }
    }
}
```

`debug_get_stack_member_state(line, ...)` 拿当前 IP 对应的源码行号
去查“这一行**作用域内可见的变量名 → 栈槽**”——这是编译期就建好
的索引（`GDScriptFunction::stack_debug`），只在 `DEBUG_ENABLED` 下
存在。

成员变量则简单很多，直接通过 `GDScript::debug_get_member_indices()`
拿到 `name → MemberInfo`：

```cpp
const HashMap<StringName, GDScript::MemberInfo> &mi =
    scr->debug_get_member_indices();

for (const KeyValue<StringName, GDScript::MemberInfo> &E : mi) {
    p_members->push_back(E.key);
    p_values->push_back(instance->debug_get_member_by_index(E.value.index));
}
```

至此**编辑器上的“变量”面板**所需要的全部数据都已就绪——而它的代
价仅仅是 VM 在每条语句前多跑一条 `OPCODE_LINE`。

## 23.6 表达式求值：`debug_parse_stack_level_expression`

调试器的“监视/REPL”面板允许用户在暂停态输入任意表达式
（`my_node.position.x + 1`）即时求值。这个能力靠
`Expression`（`core/math/expression.h`）实现：

```cpp
String GDScriptLanguage::debug_parse_stack_level_expression(
        int p_level, const String &p_expression, ...) {
    // 收集当前栈的 locals + members 作为 Expression 的输入符号表
    debug_get_stack_level_locals(p_level, &names, &values, ...);
    // ...
    ScriptInstance *instance = debug_get_stack_level_instance(p_level);
    // ...
}
```

注意它**不是**让 GDScript Parser/Compiler/VM 再跑一遍——那样代价
太高且会触发副作用（修改成员变量、调用 setter）。它走的是 Godot
的通用 `Expression` 类：一个独立的、只读的、面向 Variant 的小型表
达式求值器，没有控制流，也不能写状态。

代价是“调试器表达式”的语法是 GDScript 的**子集**——不能用
`await`、不能定义 lambda，但对绝大多数“看一眼变量值”的场景已经
够用，且**绝对安全**。

## 23.7 Profiler 采样：环绕在 VM 入口/出口的钩子

Profiler 不需要新增字节码，它只在 VM 主循环的**入口和出口**插了两
个时间戳：

```cpp
// gdscript_vm.cpp::call() 入口
#ifdef DEBUG_ENABLED
uint64_t function_start_time = 0;
uint64_t function_call_time = 0;

if (GDScriptLanguage::get_singleton()->profiling) {
    function_start_time = OS::get_singleton()->get_ticks_usec();
    function_call_time = 0;
    profile.call_count.increment();
    profile.frame_call_count.increment();
}
#endif

// ... 主循环 ...

// 出口
#ifdef DEBUG_ENABLED
if (GDScriptLanguage::get_singleton()->profiling) {
    uint64_t time_taken =
        OS::get_singleton()->get_ticks_usec() - function_start_time;
    profile.total_time.add(time_taken);
    profile.self_time.add(time_taken - function_call_time);
    profile.frame_total_time.add(time_taken);
    profile.frame_self_time.add(time_taken - function_call_time);
    if (Thread::get_caller_id() == Thread::get_main_id()) {
        GDScriptLanguage::get_singleton()->script_frame_time
            += time_taken - function_call_time;
    }
}
#endif
```

每个 `GDScriptFunction` 上都挂了一个 `Profile` 结构：

```cpp
// gdscript_function.h
struct Profile {
    StringName signature;
    SafeNumeric<uint64_t> call_count;
    SafeNumeric<uint64_t> self_time;
    SafeNumeric<uint64_t> total_time;
    SafeNumeric<uint64_t> frame_call_count;
    SafeNumeric<uint64_t> frame_self_time;
    SafeNumeric<uint64_t> frame_total_time;
    uint64_t last_frame_call_count = 0;
    uint64_t last_frame_self_time = 0;
    uint64_t last_frame_total_time = 0;
    typedef struct NativeProfile {
        uint64_t call_count;
        uint64_t total_time;
        String signature;
    } NativeProfile;
    HashMap<String, NativeProfile> native_calls;
    HashMap<String, NativeProfile> last_native_calls;
};
```

几个值得注意的设计：

1. **`SafeNumeric` 而不是 mutex**——调用计数和时间累加是**纯加
   法**，原子操作就够，不需要锁；多线程脚本调用同一个
   `GDScriptFunction` 时不会互相阻塞。
2. **total_time vs self_time 的分离**：`total_time` 是函数从入口到
   出口的全部耗时，`self_time = total_time - function_call_time`，
   后者由子调用累加得到，剥离了内部嵌套调用的耗时。这就是 Profiler
   面板里能区分“总耗时”和“自身耗时”的原因。
3. **frame_* vs 普通累计的双轨**：`frame_*` 字段每帧清零（参见
   `frame()` 钩子），用于显示“最近一帧”的热点；普通字段则是从
   `profiling_start` 起累计的总量，用于排序“整体热点”。
4. **last_frame_* 三件套**：因为 Profiler 数据采集和编辑器拉取**不
   同步**，主线程 `frame()` 把 `frame_*` 拷贝到 `last_frame_*` 后清
   零，编辑器再读 `last_frame_*` 就不会读到“正在累计中”的半成品。

## 23.8 native 调用的子项采样

GDScript 的代码热点经常不在自己身上，而在它频繁调用的引擎方法上
（`Node.get_node`、`Vector2.distance_to` 等）。Profiler 把这一层也
单独采集了：

```cpp
// gdscript_vm.cpp 里散落多次
if (GDScriptLanguage::get_singleton()->profiling
    && GDScriptLanguage::get_singleton()->profile_native_calls) {
    uint64_t t = OS::get_singleton()->get_ticks_usec();
    // ... 调用 native 方法 ...
    function_call_time +=
        OS::get_singleton()->get_ticks_usec() - t;
}
```

`profile_native_calls` 是一个**独立**于 `profiling` 的开关，因为这
项采集成本要明显高一点（每个 `CALL_METHOD_BIND` 都要取两次时间
戳）。`function_call_time` 同时被用来支撑前一节的 `self_time` 计
算，这也解释了为什么 Profiler 不开 native 采集时 `self_time` 会
**等于** `total_time`——它没法剥离子调用耗时。

native 调用的细分结果存在 `profile.native_calls` 这个 hashmap 里，
key 是 native 方法的签名字符串，value 是 `NativeProfile{call_count,
total_time, signature}`。Profiler 面板展开一行 GDScript 函数时，看
到的“被调用的引擎方法列表”就是这个 hashmap 的内容。

## 23.9 `profiling_start` / `profiling_stop`：全量清零

Profiler 的开/关只是简单的“清零 + 翻一个布尔位”：

```cpp
// gdscript.cpp
void GDScriptLanguage::profiling_start() {
#ifdef DEBUG_ENABLED
    MutexLock lock(mutex);

    SelfList<GDScriptFunction> *elem = function_list.first();
    while (elem) {
        elem->self()->profile.call_count.set(0);
        elem->self()->profile.self_time.set(0);
        elem->self()->profile.total_time.set(0);
        elem->self()->profile.frame_call_count.set(0);
        elem->self()->profile.frame_self_time.set(0);
        elem->self()->profile.frame_total_time.set(0);
        elem->self()->profile.last_frame_call_count = 0;
        elem->self()->profile.last_frame_self_time = 0;
        elem->self()->profile.last_frame_total_time = 0;
        elem->self()->profile.native_calls.clear();
        elem->self()->profile.last_native_calls.clear();
        elem = elem->next();
    }

    profiling = true;
#endif
}
```

注意它遍历的是 `function_list`——一个挂在 `GDScriptLanguage` 上的
**所有已编译 `GDScriptFunction` 的全局链表**。每个
`GDScriptFunction` 的 `SelfList` 节点在编译完成时被链入，析构时自
动摘除。这样 Profiler 不用知道脚本的层级关系，也能统计到每一个函
数。

`profiling_stop` 只是 `profiling = false`——已经记到 `Profile` 里
的数据保留着，等编辑器后续拉取。

## 23.10 热重载：`GDScript::reload(p_keep_state=true)`

Godot 编辑器“运行游戏的同时编辑代码”的能力，最终落到
`GDScript::reload(true)` 这个调用上。它的难点在于：

* 旧脚本可能有**正在运行的实例**（节点上挂着的脚本不能销毁）；
* 旧实例的成员字段值不能丢；
* 静态变量、内部类常量、export 默认值都要重新求值；
* 子类、依赖该脚本的其它脚本都要级联刷新。

```cpp
Error GDScript::reload(bool p_keep_state) {
    if (reloading) {
        return OK; // 防递归
    }
    reloading = true;

    bool has_instances;
    {
        MutexLock lock(GDScriptLanguage::singleton->mutex);
        has_instances = instances.size();
    }

    // 不允许在 keep_state=false 时重载有活动实例的脚本
    if (!p_keep_state && has_instances) {
        reloading = false;
        ERR_FAIL_V_MSG(ERR_ALREADY_IN_USE,
                       "Cannot reload script while instances exist.");
    }

    // ... 准备 basedir、刷新 GDScriptCache ...

    // 关键：保存旧的静态数据
#ifdef TOOLS_ENABLED
    if (p_keep_state && can_run && is_valid()) {
        _save_old_static_data();
    }
#endif

    valid = false;
    GDScriptParser parser;
    Error err = parser.parse(source, path, false);
    if (err) {
        // 解析失败：让编辑器跳到出错行
        if (EngineDebugger::is_active()) {
            GDScriptLanguage::get_singleton()->debug_break_parse(
                _get_debug_path(), parser.get_errors().front()->get().start_line,
                "Parser Error: " + ...);
        }
        reloading = false;
        return ERR_PARSE_ERROR;
    }

    GDScriptAnalyzer analyzer(&parser);
    err = analyzer.analyze();
    // ... 编译、安装新字节码、_restore_old_static_data ...
}
```

可以看到 reload 实质上是**全套编译流水线再走一遍**——Parser →
Analyzer → Compiler，把得到的新 `GDScriptFunction` 替换进
`member_functions`。`_save_old_static_data / _restore_old_static_data`
负责把所有 `static var` 的旧值搬过来：

```cpp
void GDScript::_save_old_static_data() {
    // 把 static_variables 拷贝到一份临时 map
    for (KeyValue<StringName, GDScript> &inner : subclasses) {
        inner.value->_save_old_static_data();
    }
}
```

注意它**递归处理 inner class**——`class Foo: static var x` 这种嵌
套结构在 reload 后也能保留旧值。

实例字段则是另一条路径：`instances` 集合里的每个
`GDScriptInstance` 已经持有自己的 `members` 数组，reload 不会动它，
新版字节码访问时按 `member_indices` 名字索引匹配——只要新版没把
某个 `var` 改名/删除，旧实例的状态就自动“迁移”过来。

最后由 `GDScriptCache::reload_scripts` 触发依赖该脚本的所有其它脚
本也走一次 reload，形成级联——这就是为什么修改一个 `class_name`
脚本后，整个项目的相关脚本都能感知到新的 API。

## 23.11 把整条工具链串起来

回顾这一章的内容，可以发现调试与性能采集**不是一个独立子系统**，
而是把前 22 章里所有部件再排列组合了一遍：

| 能力        | 重用的前章组件                                   |
| ----------- | ------------------------------------------------ |
| 行断点      | 编译器 `write_newline`（第 7 章） + VM 主循环 dispatch（第 10 章） |
| 单步        | `enter/exit_function` 的 depth 维护（第 9 章） |
| 调用栈      | `CallLevel` 反向链表（第 9 章）                   |
| 局部变量    | `GDScriptFunction::stack_debug` 索引（第 9 章）   |
| 成员变量    | `GDScript::member_indices`（第 15 章）            |
| 监视表达式  | 通用 `Expression` 求值器（独立模块）              |
| Profiler    | `function_list` 全局链表 + `SafeNumeric`        |
| native 采样 | VM 中所有 `CALL_METHOD_BIND_*` 周围的时间戳     |
| 热重载      | Parser+Analyzer+Compiler 重跑 + `static_data` 保存 / `GDScriptCache` 级联（第 19 章） |

GDScript 的设计哲学也在这里体现得最清楚：

* **零开销原则**：release 构建里 `OPCODE_LINE` 直接消失、`Profile`
  字段 `#ifdef DEBUG_ENABLED` 排除，调试能力只在该开的时候开；
* **组合而非新增**：调试器没有自己的“字节码”、Profiler 没有自己
  的“调用栈”——它们直接复用 VM 已经存在的语义；
* **同步阻塞而非异步事件**：`debug_break` 是阻塞的，所有“变量探
  查”都在主线程上完成，省掉了跨线程同步的复杂度，代价是调试时游
  戏会真的卡住；
* **线程本地存储（TLS）的妙用**：`_call_stack`、`_debug_error`、
  `_debug_parse_err_line` 全是 `thread_local`，多线程脚本互不干扰，
  也无须额外加锁。

至此，本书正文部分到此结束。从 Tokenizer 接收第一个字符开始，到
Profiler 在编辑器面板里画出热点条形图，GDScript 走过的全部路径都
已展开。后续附录将以**速查表**的形式整理关键字、Opcode 和常用调
试技巧，便于读者在阅读源码时随时回查。
