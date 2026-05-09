# 第 22 章　警告系统：`GDScriptWarning`

GDScript 的警告体系是一条**贯穿前端、抑制注解、项目设置、编辑器、
LSP** 的纵向通路。它要解决的核心问题是：

* **报得准**：在 Analyzer 这种唯一掌握全局类型信息的地方生成；
* **报得灵**：每条警告各自有项目级开关 + 行级抑制 + 文件级区域抑
  制；
* **报得稳**：Parser 报告顺序和源码行号严格一致，便于编辑器排序；
* **可升可降**：用户可以把任何一条警告“当作错误”阻止脚本通过编译。

本章把这条通路从 `enum Code` 一路拆到 `@warning_ignore_start /
restore`，看 GDScript 是怎么用相对节制的代码量做到这些的。

涉及的核心文件：

* `modules/gdscript/gdscript_warning.{h,cpp}`：警告枚举与默认级别
  表
* `modules/gdscript/gdscript_parser.cpp`：`push_warning` /
  `apply_pending_warnings` / `warning_ignore_*` 注解
* `modules/gdscript/gdscript_analyzer.cpp`：警告的发出现场
* `modules/gdscript/gdscript_editor.cpp`：警告同步到 ScriptLanguage 接口

---

## 22.1 全部隐藏在 `#ifdef DEBUG_ENABLED` 后面

```cpp
class GDScriptWarning {
#ifdef DEBUG_ENABLED
public:
    enum WarnLevel { IGNORE, WARN, ERROR };
    enum Code { /* ... 40+ 项 ... */ WARNING_MAX };
    // ...
#endif // DEBUG_ENABLED
};
```

`GDScriptWarning` 头文件**整体被 `#ifdef DEBUG_ENABLED` 包裹**。这
意味着：

* **release 构建里完全没有警告系统的开销**——不只是不发警告，连
  enum、字段、抑制表都不存在；
* `apply_pending_warnings` / `push_warning` 等所有调用点都被同样的
  `#ifdef DEBUG_ENABLED` 圈起来；
* 用户写 `@warning_ignore` 在 release 构建中只走一个空 stub。

这种“**警告 = 仅开发期能力**”的取舍很彻底——既不让脚本 release
体积膨胀，也避免警告生产代码意外影响运行时。

---

## 22.2 三级 `WarnLevel` + 默认级别表

```cpp
enum WarnLevel { IGNORE, WARN, ERROR };
constexpr static WarnLevel default_warning_levels[] = {
    WARN,   // UNASSIGNED_VARIABLE
    WARN,   // UNUSED_VARIABLE
    /* ... */
    IGNORE, // UNTYPED_DECLARATION
    IGNORE, // INFERRED_DECLARATION
    IGNORE, // UNSAFE_PROPERTY_ACCESS
    /* ... */
    ERROR,  // INFERENCE_ON_VARIANT
    ERROR,  // NATIVE_METHOD_OVERRIDE
    ERROR,  // GET_NODE_DEFAULT_WITHOUT_ONREADY
    ERROR,  // ONREADY_WITH_EXPORT
    /* ... */
};

static_assert(std_size(default_warning_levels) == WARNING_MAX, ...);
```

设计要点：

* **`IGNORE`**：完全不报告。
* **`WARN`**：写入警告列表，编辑器在行号边栏显示黄色标记。
* **`ERROR`**：警告被升级为错误，阻止脚本编译——用作“这种用法几
  乎肯定是 bug”的强约束。

注释里还能看出 GDScript 的“**实用主义**”定级原则：

* `UNTYPED_DECLARATION` / `INFERRED_DECLARATION` / `UNSAFE_*` 这种
  在“不写类型时几乎到处都是”的告警**默认 IGNORE**——因为 GDScript
  的渐进式类型理念允许用户完全不写类型，强制告警会形成噪声地狱；
* `INFERENCE_ON_VARIANT` / `NATIVE_METHOD_OVERRIDE` / `ONREADY_WITH_EXPORT`
  这种**“几乎不可能是有意写法”** 的情况默认 ERROR——避免用户在不
  知情的情况下写出运行期 bug。

`default_warning_levels` 用 C 数组而非 HashMap——`Code` 枚举本身就
是连续整数，直接下标查找最快；外加 `static_assert` 防止维护者新增
枚举时忘了同步默认级别。

---

## 22.3 `Code` 枚举：四十多种诊断的清单

按主题归类（`Code` 枚举里的 40+ 项）：

| 主题 | 警告 |
|------|------|
| 未使用 | UNUSED_VARIABLE / UNUSED_LOCAL_CONSTANT / UNUSED_PRIVATE_CLASS_VARIABLE / UNUSED_PARAMETER / UNUSED_SIGNAL |
| 未赋值 | UNASSIGNED_VARIABLE / UNASSIGNED_VARIABLE_OP_ASSIGN |
| 遮蔽 | SHADOWED_VARIABLE / SHADOWED_VARIABLE_BASE_CLASS / SHADOWED_GLOBAL_IDENTIFIER |
| 不可达 | UNREACHABLE_CODE / UNREACHABLE_PATTERN |
| 无副作用表达式 | STANDALONE_EXPRESSION / STANDALONE_TERNARY / INCOMPATIBLE_TERNARY |
| 类型注解 | UNTYPED_DECLARATION / INFERRED_DECLARATION / INFERENCE_ON_VARIANT |
| 不安全访问 | UNSAFE_PROPERTY_ACCESS / UNSAFE_METHOD_ACCESS / UNSAFE_CAST / UNSAFE_CALL_ARGUMENT / UNSAFE_VOID_RETURN |
| 数值安全 | INTEGER_DIVISION / NARROWING_CONVERSION |
| 枚举 | INT_AS_ENUM_WITHOUT_CAST / INT_AS_ENUM_WITHOUT_MATCH / ENUM_VARIABLE_WITHOUT_DEFAULT |
| 注解一致性 | MISSING_TOOL / REDUNDANT_STATIC_UNLOAD / ONREADY_WITH_EXPORT / GET_NODE_DEFAULT_WITHOUT_ONREADY |
| 协程 | REDUNDANT_AWAIT / MISSING_AWAIT |
| 断言 | ASSERT_ALWAYS_TRUE / ASSERT_ALWAYS_FALSE |
| 易混淆 | CONFUSABLE_IDENTIFIER / CONFUSABLE_LOCAL_DECLARATION / CONFUSABLE_LOCAL_USAGE / CONFUSABLE_CAPTURE_REASSIGNMENT |
| 杂项 | RETURN_VALUE_DISCARDED / STATIC_CALLED_ON_INSTANCE / EMPTY_FILE / DEPRECATED_KEYWORD / NATIVE_METHOD_OVERRIDE |
| 已弃用 | PROPERTY_USED_AS_FUNCTION / CONSTANT_USED_AS_FUNCTION / FUNCTION_USED_AS_PROPERTY（兼容用） |

每条警告在 enum 旁都有简短注释说明触发条件——这些注释本身是 GDScript
为开发者写的“规则书”，构成了 GDScript 的“**最低代码质量基线**”。

`FIRST_DEPRECATED_WARNING` 这个常量与 `#ifndef DISABLE_DEPRECATED`
配合，让历史警告项保留枚举位置（保证 .csettings 兼容性），同时允
许编译时彻底剥离。

---

## 22.4 警告的发出：`push_warning`

发出点全部集中在 `GDScriptParser::push_warning`：

```cpp
void GDScriptParser::push_warning(const Node *p_source,
        GDScriptWarning::Code p_code, const Vector<String> &p_symbols) {
    ERR_FAIL_NULL(p_source);
    ERR_FAIL_INDEX(p_code, GDScriptWarning::WARNING_MAX);

    if (is_project_ignoring_warnings || is_script_ignoring_warnings) return;

    const GDScriptWarning::WarnLevel warn_level = warning_levels[p_code];
    if (warn_level == GDScriptWarning::IGNORE) return;

    PendingWarning pw;
    pw.source           = p_source;
    pw.code             = p_code;
    pw.treated_as_error = warn_level == GDScriptWarning::ERROR;
    pw.symbols          = p_symbols;

    pending_warnings.push_back(pw);
}
```

四道闸门：

1. **`is_project_ignoring_warnings`** —— `ProjectSettings` 里的
   `debug/gdscript/warnings/enable` 开关，整个项目级关闭警告；
2. **`is_script_ignoring_warnings`** —— 脚本顶部 `# warnings-disable`
   或类似机制（细节在脚本侧由 Parser 设置）；
3. **`warning_levels[p_code] == IGNORE`** —— 单条警告项被项目级
   关闭；
4. 通过后 **不立即生成 GDScriptWarning，而是放入 `pending_warnings`**
   ——这是为了配合 22.5 节的“延后过滤”。

可变参数模板版本让调用点可以省去显式构造 `Vector<String>`：

```cpp
template <class... Symbols>
void push_warning(const Node *p_source, GDScriptWarning::Code p_code,
                  const Symbols &...p_symbols) {
    push_warning(p_source, p_code, Vector<String>{ p_symbols... });
}
```

调用点示例（来自 Analyzer）：

```cpp
parser->push_warning(p_function->parameters[i]->identifier,
        GDScriptWarning::UNUSED_PARAMETER,
        function_visible_name, p_function->parameters[i]->identifier->name);
```

`symbols` 数组是 `get_message()` 模板里要替换的占位符，让单条警告
能呈现具体上下文（例如 `"Parameter 'x' of function 'foo()' is never
used."`）。

---

## 22.5 `pending_warnings` 与延后应用

为什么 `push_warning` 不直接产出最终警告？因为 GDScript 必须在
**Parser 全部完成后**才能确定每条警告是否被 `@warning_ignore` 抑制
——如果先报出来再撤回，编辑器就会看到“先红后绿”的闪烁。所以采用
两阶段：

```cpp
void GDScriptParser::apply_pending_warnings() {
    for (const PendingWarning &pw : pending_warnings) {
        if (warning_ignored_lines[pw.code].has(pw.source->start_line)) continue;
        if (warning_ignore_start_lines[pw.code] <= pw.source->start_line) continue;

        GDScriptWarning warning;
        warning.code        = pw.code;
        warning.symbols     = pw.symbols;
        warning.start_line  = pw.source->start_line;
        warning.start_column = pw.source->start_column;
        warning.end_line    = pw.source->end_line;
        warning.end_column  = pw.source->end_column;

        if (pw.treated_as_error) {
            push_error(warning.get_message() + " (Warning treated as error.)", pw.source);
            continue;
        }
        // 按行号有序插入到 warnings 链表
        // ...
    }
}
```

两步设计的好处：

* **抑制注解可以前向**：用户写在脚本顶部的 `@warning_ignore_start("unused_variable")`
  会影响后续整个文件——但抑制注解可能在 Analyzer 还没分析到第 N
  行的时候就已经写在第 1 行；只有所有分析完成后再过滤 pending，才
  能正确消化这种“前向抑制”。
* **顺序稳定**：最后按 `start_line` 有序插入到 `warnings` 链表，
  使编辑器拿到的列表始终按行号升序，便于显示。
* **统一升级路径**：`treated_as_error` 走 `push_error`，与普通解析
  错误共用同一条管道——编辑器/LSP/CLI 看到的 ERROR 列表是一致的，
  无需区分“源自警告升级”和“源自语法错误”。

---

## 22.6 三种抑制注解：`@warning_ignore` / `_start` / `_restore`

GDScript 提供了**三个层级**的抑制粒度：

```python
# 1. 行级/语句级抑制——只对紧跟的目标生效
@warning_ignore("unused_variable")
var x = 0

# 2. 区域抑制——从注解所在行开始，到 _restore 或文件末尾
@warning_ignore_start("unused_variable")
# ...这里所有 unused_variable 警告被忽略...
@warning_ignore_restore("unused_variable")

# 3. 隐式整文件抑制——_start 不写 _restore，等价于一直关闭到文件末尾
```

注册位置：

```cpp
register_annotation(MethodInfo("@warning_ignore", PropertyInfo(STRING, "warning")),
    AnnotationInfo::CLASS_LEVEL | AnnotationInfo::STATEMENT,
    &GDScriptParser::warning_ignore_annotation, varray(), /*is_vararg=*/true);

register_annotation(MethodInfo("@warning_ignore_start", PropertyInfo(STRING, "warning")),
    AnnotationInfo::STANDALONE,
    &GDScriptParser::warning_ignore_region_annotations, varray(), true);

register_annotation(MethodInfo("@warning_ignore_restore", PropertyInfo(STRING, "warning")),
    AnnotationInfo::STANDALONE,
    &GDScriptParser::warning_ignore_region_annotations, varray(), true);
```

注意：

* `@warning_ignore` 同时标了 `CLASS_LEVEL | STATEMENT`——可以贴在
  类成员上也可以贴在语句上；
* `_start` / `_restore` 标了 `STANDALONE`——它们不依附任何节点，
  自成一个声明性指令；
* 三者都是 vararg：`@warning_ignore("unused_variable", "shadowed_variable")`
  一次抑制多类。

### 22.6.1 `@warning_ignore`：按节点结构计算行范围

```cpp
bool GDScriptParser::warning_ignore_annotation(AnnotationNode *p_annotation,
        Node *p_target, ClassNode *p_class) {
    for (const Variant &warning_name : p_annotation->resolved_arguments) {
        GDScriptWarning::Code warning_code = GDScriptWarning::get_code_from_name(
                String(warning_name).to_upper());
        if (warning_code == GDScriptWarning::WARNING_MAX) {
            push_error(vformat(R"(Invalid warning name: "%s".)", warning_name), p_annotation);
            continue;
        }
        int start_line = p_annotation->start_line;
        int end_line   = p_target->end_line;

        switch (p_target->type) {
            case Node::VARIABLE: { /* end = initializer 行 */ } break;
            case Node::FOR / IF / MATCH / WHILE: { /* end = 条件行 */ } break;
            case Node::CLASS:    { /* 包括所有 annotation */ } break;
            case Node::FUNCTION: { /* 包括所有 parameters */ } break;
            case Node::MATCH_BRANCH: { /* 包括所有 patterns */ } break;
            default: { /* 用 p_target->end_line */ } break;
        }
        end_line = MAX(start_line, end_line); // 防御
        for (int line = start_line; line <= end_line; line++) {
            warning_ignored_lines[warning_code].insert(line);
        }
    }
    return !has_error;
}
```

精妙之处：**`end_line` 不取整个语义体的尾行，而是只覆盖“声明头”
部分**。

* 对 `var x = expensive_call()`：只覆盖到 initializer，不延伸到
  函数体；
* 对 `func foo(x: int): ... 100 行 ...`：只覆盖到参数行，函数体内
  的警告不被抑制；
* 对 `if cond: ... 大段代码 ...`：只覆盖到 `cond` 行。

这样 `@warning_ignore("unused_parameter")` 贴在 `func` 上才不会
**意外抑制函数体内 30 行后某变量的 unused_variable**——抑制的“辐
射半径”精确控制在用户视觉上能看到的“声明部分”。

### 22.6.2 `@warning_ignore_start` / `_restore`：区域配对

```cpp
bool GDScriptParser::warning_ignore_region_annotations(AnnotationNode *p_annotation,
        Node *p_target, ClassNode *p_class) {
    const bool is_start = p_annotation->name == SNAME("@warning_ignore_start");
    for (const Variant &warning_name : p_annotation->resolved_arguments) {
        GDScriptWarning::Code warning_code = ...;
        if (is_start) {
            if (warning_ignore_start_lines[warning_code] != INT_MAX) {
                push_error(vformat(R"(Warning "%s" is already being ignored ...)", ...));
                continue;
            }
            warning_ignore_start_lines[warning_code] = p_annotation->start_line;
        } else { // _restore
            if (warning_ignore_start_lines[warning_code] == INT_MAX) {
                push_error(vformat(R"(Warning "%s" is not being ignored ...)", ...));
                continue;
            }
            const int start_line = warning_ignore_start_lines[warning_code];
            const int end_line   = MAX(start_line, p_annotation->start_line);
            for (int i = start_line; i <= end_line; i++) {
                warning_ignored_lines[warning_code].insert(i);
            }
            warning_ignore_start_lines[warning_code] = INT_MAX;
        }
    }
    return !has_error;
}
```

* `warning_ignore_start_lines[code]` 初始化为 `INT_MAX`（未开启），
  `_start` 时记录起始行，`_restore` 时把区间填进 `warning_ignored_lines`；
* 嵌套 `_start` 同名警告会报错（“already being ignored at line N”）
  ——避免“双开半关”导致状态丢失；
* `_restore` 之前没 `_start` 也会报错——杜绝悬空 restore；
* 文件末尾 `apply_pending_warnings` 时若仍有未配对的 `_start`，会
  在 `apply_pending_warnings` 那条 `start_lines[code] <= source_line`
  的判断里把后续所有警告无条件抑制——这就是“**省略 _restore = 抑
  制到文件末尾**”的具体落点。

抑制状态用两张表：`warning_ignored_lines[code] : HashSet<int>`
（已固化的行集合）+ `warning_ignore_start_lines[code] : int`（当前
打开的起点）。读时（`apply_pending_warnings`）两张表都查——这就让
配对完成的区域和未闭合的区域走的是同一套查询路径。

---

## 22.7 项目级配置：`ProjectSettings` 同步

`GDScriptParser` 的 `warning_levels[code]` 数组在每次构造时从
`ProjectSettings` 读取：

```cpp
String setting = GDScriptWarning::get_setting_path_from_code(code);
// 例如 "debug/gdscript/warnings/unused_variable"
warning_levels[code] = ProjectSettings::get_singleton()->get(setting);
```

`get_setting_path_from_code` 把枚举名转成
`debug/gdscript/warnings/<lower_snake_name>` 这种统一路径，让用户在
项目设置里逐项配置。同时还有：

* `debug/gdscript/warnings/enable`：项目级总开关；
* `debug/gdscript/warnings/exclude_addons`：是否在 `res://addons/`
  下的脚本里也报警（默认不报）。

这两个总开关对应 `is_project_ignoring_warnings` 和按文件路径前缀决
定的 `is_script_ignoring_warnings`，在 `push_warning` 入口处就把结
果切掉。

---

## 22.8 警告与编辑器/LSP 的桥接

警告最终通过两条路径暴露给用户：

1. **编辑器内置 ScriptEditor**：`GDScriptLanguage::validate` 把
   `parser.get_warnings()` 翻译成 `ScriptLanguage::Warning` 列表，
   交给 ScriptEditor 在行号边栏画黄三角（第 20 章 22 节）。
2. **LSP**：`ExtendGDScriptParser::update_diagnostics` 把警告同样
   翻译成 LSP `Diagnostic { severity = Warning, ... }`，由
   `publishDiagnostics` 推送给客户端（第 21 章）。

两条路径都从 `parser.get_warnings()` 取数据，**没有任何各自重新生
成警告的代码**——再次体现“前端是真相”原则。

---

## 22.9 设计回顾

GDScript 警告系统的核心抉择：

1. **完全 DEBUG-only**：`#ifdef DEBUG_ENABLED` 包裹整套机制，release
   构建零开销；
2. **三级 IGNORE/WARN/ERROR**：让“严重 bug 苗头”能直接当错误处理；
3. **延后应用 (`pending_warnings` → `apply_pending_warnings`)**：
   解决“前向抑制”和“顺序稳定”两个问题；
4. **三种抑制粒度**：
   * `@warning_ignore` 精准到声明头部（不污染函数体）；
   * `@warning_ignore_start/_restore` 显式区域；
   * 省略 `_restore` 等价于抑制到文件末尾；
5. **抑制状态双表**：`warning_ignored_lines` + `warning_ignore_start_lines`
   让“已闭合区域”和“正在打开区域”走同一查询路径；
6. **项目级 + 文件级 + 行级三层开关**：分别对应整体策略、按目录排
   除、按代码片段豁免——粒度从粗到细，每层各自独立失效；
7. **共享真相**：编辑器、LSP、CLI 都从同一份 `parser.get_warnings()`
   读数据，不重复实现。

这套设计的本质是：**把“规则”集中到 enum + 默认级别表，把“产生”
集中到 push_warning，把“过滤”集中到 apply_pending_warnings，把
“呈现”交给现有渠道**——四件事各自独立，加新警告时只动 enum 与具
体的 Analyzer 触发点，其余都不用改。

---

## 小结

* 整套警告系统封装在 `#ifdef DEBUG_ENABLED` 之后，release 零开销；
* `WarnLevel` 三级：IGNORE / WARN / ERROR——后者把警告升级为错误
  阻止编译；
* `default_warning_levels` C 数组与 `static_assert` 配合保证维护一致
  性；
* `push_warning` 把警告先放进 `pending_warnings`，待 Parser 完成后
  在 `apply_pending_warnings` 里统一过滤抑制并按行号有序插入；
* 三种抑制注解 `@warning_ignore` / `@warning_ignore_start` /
  `@warning_ignore_restore`，分别提供节点头部级、显式区域、整文件
  抑制；
* 注解参数支持 vararg，按警告名小写匹配 `get_code_from_name`；
* `warning_ignored_lines` 与 `warning_ignore_start_lines` 双表统一
  查询路径，让闭合与未闭合区域无缝衔接；
* 项目级总开关、单条 IGNORE、按目录排除（addons）、按文件抑制
  四级粒度；
* 编辑器与 LSP 都从同一 `parser.get_warnings()` 读取，两条呈现路径
  共享一个真相源。

下一章我们将看 GDScript 的最后一块拼图——调试器与 Profiler：断点、
单步、热重载、Profiler 采样在字节码层面是怎么实现的。
