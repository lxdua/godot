# 第 20 章　编辑器集成：自动补全、跳转、文档生成

GDScript 不是“运行起来就完事”的语言——它在 Godot 编辑器里同时承
担**着色器、补全器、跳转、Docstring 生成器、翻译字符串提取器**等多
重角色。本章把 `gdscript_editor.cpp` 与 `editor/` 子目录里的几个
工具组件拆开，看 GDScript 如何复用前端（Tokenizer / Parser /
Analyzer）来支撑编辑期能力，而不需要为每个功能再写一套独立的解析
器。

涉及的核心文件：

* `modules/gdscript/gdscript_editor.cpp`：`GDScriptLanguage` 编辑器
  侧的服务实现（验证、补全、跳转、调试器接口等）
* `modules/gdscript/editor/gdscript_highlighter.{h,cpp}`：语法高亮
* `modules/gdscript/editor/gdscript_docgen.{h,cpp}`：文档生成
* `modules/gdscript/editor/gdscript_translation_parser_plugin.{h,cpp}`：
  翻译字符串提取

---

## 20.1 `ScriptLanguage` 的“编辑器服务面”

`GDScriptLanguage` 实现了 `ScriptLanguage` 的**几十个虚方法**，按
用途大致可分为：

| 类别 | 代表方法 | 用途 |
|------|----------|------|
| 验证 | `validate()` | 编辑器实时获取错误/警告/可达行 |
| 补全 | `complete_code()` | 光标位置候选项 |
| 跳转 | `lookup_code()` | F12 跳到定义 |
| 模板 | `make_template()` / `get_built_in_templates()` | 文件创建模板 |
| 缩进 | `auto_indent_code()` / `_get_indentation()` | 自动缩进、tab/空格选择 |
| 调试 | `debug_*` 一族 | 见第 23 章 |
| 反射 | `get_public_functions()` / `get_public_constants()` / `get_public_annotations()` | 文档与提示 |
| 模式 | `supports_documentation()` / `supports_builtin_mode()` / `is_using_templates()` | 能力声明 |

这些方法构成了**编辑器和脚本语言之间的契约**——任何脚本语言（C# /
GDExtension Script / 第三方语言）只要实现这套接口，就能在编辑器
里享受统一的体验。GDScript 的实现是这套契约最完整的范例。

---

## 20.2 `validate()`：实时错误的来源

`validate()` 是编辑器在“代码改动后”最常调用的方法——它返回错误、
警告、安全行集合，直接驱动行号边栏上那些红/黄标记。

```cpp
bool GDScriptLanguage::validate(const String &p_script, const String &p_path,
        List<String> *r_functions, List<ScriptLanguage::ScriptError> *r_errors,
        List<ScriptLanguage::Warning> *r_warnings,
        HashSet<int> *r_safe_lines) const {
    GDScriptParser parser;
    GDScriptAnalyzer analyzer(&parser);

    Error err = parser.parse(p_script, p_path, false);
    if (err == OK) err = analyzer.analyze();

#ifdef DEBUG_ENABLED
    if (r_warnings) {
        for (const GDScriptWarning &E : parser.get_warnings()) {
            ScriptLanguage::Warning w;
            w.start_line = E.start_line;
            w.end_line = E.end_line;
            w.code = (int)E.code;
            w.string_code = GDScriptWarning::get_name_from_code(E.code);
            w.message = E.get_message();
            r_warnings->push_back(w);
        }
    }
#endif
    if (err) {
        // 收集本文件错误 + 跨文件错误（preload 的依赖也可能报错）
        for (const GDScriptParser::ParserError &pe : parser.get_errors()) { /* ... */ }
        for (KeyValue<String, Ref<GDScriptParserRef>> E : parser.get_depended_parsers()) {
            for (const GDScriptParser::ParserError &pe : E.value->get_parser()->get_errors()) {
                /* 用 E.key 作为 path 报告依赖文件错误 */
            }
        }
        return false;
    } else if (r_functions) {
        // 收集函数列表（支持“go to function”下拉菜单）
        const ClassNode *cl = parser.get_tree();
        HashMap<int, String> funcs;
        get_function_names_recursively(cl, "", funcs);
        for (const auto &E : funcs)
            r_functions->push_back(E.value + ":" + itos(E.key));
    }
#ifdef DEBUG_ENABLED
    if (r_safe_lines) {
        // 把所有“没被 Analyzer 标记为 unsafe”的行加入 safe set
        const HashSet<int> &unsafe_lines = parser.get_unsafe_lines();
        for (int i = 1; i <= parser.get_last_line_number(); i++)
            if (!unsafe_lines.has(i)) r_safe_lines->insert(i);
    }
#endif
    return true;
}
```

几个值得关注的设计：

### 20.2.1 完整复用 Parser + Analyzer

编辑器里跑的就是和编译器一样的 Parser + Analyzer——**没有简化版的
“编辑器专用解析器”**。这意味着编辑器看到的错误与运行时看到的错误
**严格一致**——“能编辑器通过、运行时报错”这种情况不会发生。

### 20.2.2 跨文件错误传播

```cpp
for (KeyValue<String, Ref<GDScriptParserRef>> E : parser.get_depended_parsers()) { ... }
```

如果当前脚本 `preload` 的另一个脚本本身有错，那个错误也会被报告
回来——但用的是依赖文件自己的 path。编辑器会在那个文件的对应行
号上画红线，给出“你正在编辑的脚本依赖的文件出错了”的提示。

### 20.2.3 “Safe lines”——类型推导成功的视觉反馈

`r_safe_lines` 是一个**对编辑器有用、对运行无影响**的副产品。
Analyzer 会标记“无法静态确定类型的行”（如 `var x = something_dynamic`），
这些被称为 unsafe lines。编辑器把它们以稍暗的颜色显示，提示用户：

> 这一行在做动态类型操作，IDE 没法给你完整提示，运行时也会绕过验证
> 路径走慢路径。

这是 GDScript 的“渐进式类型”理念在编辑器层的可视化——它不强迫你
写类型，但会告诉你“写了类型的地方更值得信任”。

### 20.2.4 `r_functions` 喂养顶部函数下拉菜单

编辑器顶部那个“跳到函数”下拉就是从 `r_functions` 来的——格式
`name:line`，由 `get_function_names_recursively` 递归内部类生成
`Outer.Inner.func:line` 这种带点号的限定名，让用户能直接定位到内
部类方法。

---

## 20.3 `complete_code()`：补全的多维度查找

`complete_code()` 是 `gdscript_editor.cpp` 中最长的函数（800+ 行），
但骨架很清晰：

1. **触发解析**——把用户当前正在编辑的代码（含光标位置标记）跑一遍
   Parser。Parser 在遇到光标处时会**故意**进入“completion 模式”，
   把上下文信息封装成 `CompletionContext`。
2. **分发到不同的查找器**——按 `CompletionType`（标识符、属性访问、
   注解、字符串、路径等）选择走哪条路径。
3. **生成候选项 `CodeCompletionOption`**，由编辑器排序、显示。

主要查找器：

| 查找器 | 用途 |
|--------|------|
| `_find_identifiers_in_suite` | 当前作用域内的局部变量、参数 |
| `_find_identifiers_in_class` | 当前类的成员、方法、信号、内部类 |
| `_find_identifiers_in_base` | 沿 `base` 链补全继承自基类的成员 |
| `_find_identifiers` | 总入口，先 suite 后 class 再 globals |

它们形成**与运行时 `_get` 多级回退（第 16 章）严格对应**的查找链——
**“能在运行时被解析的名字，编辑器都能补全出来”** 这一不变量由
共享 Analyzer 的类型推导保证。

### 20.3.1 “Add braces” / “Expecting Callable” 这类微妙优化

在补全时机里反复出现的 `_guess_expecting_callable(completion_context)`：

```cpp
_find_identifiers(completion_context, is_function,
                  !_guess_expecting_callable(completion_context),
                  options, 0);
```

`p_add_braces` 控制是否给函数名后面自动加 `()`。当上下文期待的是
一个 `Callable`（例如 `arr.map(▏)`），就**不**加括号——直接把
方法名作为 Callable 引用。这种细节让 GDScript 的补全在“函数式风
格”代码中更加自然。

### 20.3.2 静态/实例上下文区分

```cpp
_find_identifiers_in_class(... ,
    /*p_static=*/(!p_context.current_function || p_context.current_function->is_static),
    ...);
```

如果光标位于 static 函数中，补全结果会过滤掉所有非 static 成员——
避免把不可访问的符号摆到用户面前，与 Analyzer 的访问性检查保持一
致。

---

## 20.4 `lookup_code()`：F12 跳到定义

`lookup_code()` 接收源码 + 光标位置 + 符号名，返回 `LookupResult`：

```cpp
struct LookupResult {
    enum Type { LOOKUP_RESULT_SCRIPT_LOCATION, LOOKUP_RESULT_CLASS,
                LOOKUP_RESULT_CLASS_METHOD, LOOKUP_RESULT_CLASS_PROPERTY,
                LOOKUP_RESULT_CLASS_SIGNAL, LOOKUP_RESULT_CLASS_CONSTANT,
                LOOKUP_RESULT_CLASS_ENUM, LOOKUP_RESULT_CLASS_ANNOTATION,
                LOOKUP_RESULT_LOCAL_CONSTANT, LOOKUP_RESULT_LOCAL_VARIABLE,
                ... };
    Type     type;
    String   class_name;
    String   class_member;
    String   script_path;     // 可被定位到具体文件 + 行号的话
    int      script_line;
    // ...
};
```

实现复用 `_lookup_symbol_from_base(...)`——这是与第 18 章
`get_classes_used` 共用的入口（那里的伪光标技术就是为了走这条路径）。

跳转结果分两类：

* **跳到文件 + 行号**（`LOCAL_*` / 用户脚本符号）：编辑器直接打开
  该文件并定位到行；
* **跳到引擎文档**（`CLASS_*`）：编辑器打开内置 ClassDB 文档面板
  并跳到对应条目——这种“跳引擎文档”的能力让 GDScript 用户能无缝
  穿梭于自己的脚本与 Godot 内置 API 之间。

---

## 20.5 `GDScriptSyntaxHighlighter`：在 token 级做颜色

语法高亮的实现路径与编译器**完全独立**——它不需要等 Parser/Analyzer
跑完就要给出颜色。`GDScriptSyntaxHighlighter` 直接使用一个**轻量
的、行级的、token 级的**扫描器：

```cpp
class GDScriptSyntaxHighlighter : public EditorSyntaxHighlighter {
    enum Type { NONE, REGION, NODE_PATH, NODE_REF, ANNOTATION,
                STRING_NAME, SYMBOL, NUMBER, FUNCTION, SIGNAL,
                KEYWORD, MEMBER, IDENTIFIER, TYPE };

    Vector<ColorRegion>           color_regions;     // 字符串/注释/region 的开闭对
    HashMap<int, int>             color_region_cache;// 行 → 区域类型缓存
    HashMap<StringName, Color>    class_names;       // ClassDB 类名 → 颜色
    HashMap<StringName, Color>    reserved_keywords; // 关键字
    HashMap<StringName, Color>    member_keywords;   // self/super/...
    HashSet<StringName>           global_functions;  // print/range/...
    // 颜色字段、注释 marker（CRITICAL/WARNING/NOTICE）
};
```

关键方法：

* `_update_cache()`：从 EditorSettings 读取所有用户配置的颜色，建立
  关键字/类名/全局函数的查找表；
* `_get_line_syntax_highlighting_impl(int p_line)`：返回 `Dictionary
  { col_offset → { color: Color } }`，由 TextEdit 控件按 offset 上色。

### 20.5.1 “行级 + 区域”双重扫描

为什么要 `color_region_cache`？因为多行字符串 `"""..."""` 与多行
注释会跨越多行，但编辑器一次只重绘改动的几行——不能每次都从文件
头扫描。`color_region_cache` 缓存每行起始时所处的“开放 region 类
型”，让局部刷新仍能正确判断“当前在不在多行字符串里”。

### 20.5.2 注释 Marker：高亮 TODO/FIXME

```cpp
enum CommentMarkerLevel {
    COMMENT_MARKER_CRITICAL,  // FIXME, ALERT
    COMMENT_MARKER_WARNING,   // TODO, HACK
    COMMENT_MARKER_NOTICE,    // NOTE, INFO
    COMMENT_MARKER_MAX,
};
Color comment_marker_colors[COMMENT_MARKER_MAX];
HashMap<String, CommentMarkerLevel> comment_markers;
```

在普通注释里识别一组特殊关键字（用户可在编辑器配置里自定义），按级
别上不同色——这是编辑器“热门特性”的一种内置实现，无需额外插件。

### 20.5.3 它没有用 GDScriptTokenizer

值得注意：`GDScriptSyntaxHighlighter` 内部**没有**直接复用
`GDScriptTokenizer`。原因是高亮需要**部分地容错**——即使是断行不
全的字符串、错误的 token，也要能给出大致正确的颜色。Tokenizer 在
错误处会立即停下，不能满足高亮“尽力而为”的需求。

但表里出现的 `class_names`、`reserved_keywords` 是从 `GDScriptParser`
/ `GDScriptTokenizer` / `ClassDB` 同步过来的——保持识别能力的一致
性，但走自己的扫描路径。

---

## 20.6 `GDScriptDocGen`：从源码生成 XML 文档

Godot 的内置文档面板能直接显示 `.gd` 脚本中带 `##` 注释的成员。这
是 `GDScriptDocGen` 在编译期完成的工作。

```cpp
class GDScriptDocGen {
    using GDP = GDScriptParser;
    using GDType = GDP::DataType;

    static HashMap<String, String> singletons; // 路径 → autoload 名

    static String _get_script_name(const String &p_path);
    static String _get_class_name(const GDP::ClassNode &p_class);
    static void   _doctype_from_gdtype(const GDType &p_gdtype, String &r_type,
                                       String &r_enum, bool p_is_return = false);
    static String _docvalue_from_variant(const Variant &p_variant, int recursion = 1);
    static void   _generate_docs(GDScript *p_script, const GDP::ClassNode *p_class);

public:
    static void generate_docs(GDScript *p_script, const GDP::ClassNode *p_class);
    static void doctype_from_gdtype(const GDType &p_gdtype, String &r_type,
                                    String &r_enum, bool p_is_return = false);
    static String docvalue_from_expression(const GDP::ExpressionNode *p_expression);
};
```

`generate_docs(script, class_node)` 在编译过程中被调用一次，它做四
件事：

1. **拉取 `##` 文档注释**：Tokenizer 已经把每个标识符前面的 `##`
   注释收集到 `ClassNode::doc_data`，DocGen 直接消费它们。
2. **由 `GDScriptDataType` 生成 `r_type` / `r_enum` 字符串**：例如
   `Array[int]` → "Array", `MyEnum.VALUE` → "int" + enum_name。这
   一步把内部类型表示翻译成文档里能显示的字符串。
3. **常量字面量值的反序列化**：`_docvalue_from_variant` 把 `var =
   Vector2(1, 2)` 这种字面值打印成 `"Vector2(1, 2)"`，作为文档中
   “默认值”的展示。
4. **写入 `GDScript::docs`**：`Vector<DocData::ClassDoc>` 字段（每
   个内部类一份），由文档面板加载时直接读取。

### 20.6.1 `singletons` 表

```cpp
static HashMap<String, String> singletons;
```

记录每个 autoload 脚本的“全局名”——这样 DocGen 在生成 `class_name`
为空但作为 autoload 的脚本时，也能给一个有意义的展示名。这个映射
由 GDScript 模块在引擎初始化时从 `ProjectSettings` 同步进来。

### 20.6.2 与 ClassDB 文档的统一

DocGen 输出的 `DocData::ClassDoc` 与 ClassDB 文档面板使用的格式
**完全相同**——这就是为什么用户可以用同一个文档浏览器看 `Node`、
`Vector2` 与自己写的 `Enemy`。Godot 把“文档”抽象成跨语言的统一数
据模型，GDScript 只是其中一种数据来源。

---

## 20.7 翻译字符串提取：`GDScriptEditorTranslationParserPlugin`

游戏本地化要求“源码中所有用 `tr("Hello")` 包起来的字符串”能被批量
导出成 `.po` / `.csv` 文件。GDScript 的实现走 AST 遍历：

```cpp
class GDScriptEditorTranslationParserPlugin : public EditorTranslationParserPlugin {
    void _traverse_class(const ClassNode *p_class);
    void _traverse_function(const FunctionNode *p_func);
    void _traverse_block(const SuiteNode *p_suite);
    void _assess_expression(const ExpressionNode *p_expression);
    void _assess_assignment(const AssignmentNode *p_assignment);
    void _assess_call(const CallNode *p_call);

    void _add_id(const String &p_id, int p_line);
    void _add_id_ctx_plural(const Vector<String> &p_id_ctx_plural, int p_line);
    // ...
};
```

设计要点：

* **跑 Parser 但不跑 Analyzer**——不需要类型信息，只关心字符串字
  面量和它们出现的语法上下文；
* **识别 `tr()` / `tr_n()`**：由 `_assess_call` 检查函数名，把字面
  量参数收集起来；
* **处理 `_is_constant_string`**：拒绝 `tr(some_var)` 这种动态字符
  串（不能在编译期提取）；
* **赋值与 FileDialog filter 特殊化**：`_assess_assignment` 与
  `_extract_fd_filter_*` 处理一些约定俗成的字符串槽位（FileDialog
  的 filter 字段、`filter_strings` 这类常见模式）。

这个插件再次体现了**复用前端**的设计哲学——既不重复造解析器，也
不依赖类型分析器，只用最小必要的中间产物（AST）来完成本职工作。

---

## 20.8 设计回顾

GDScript 的编辑器集成可以总结成三个层次：

1. **语言契约层**：通过 `ScriptLanguage` 几十个虚方法，让 GDScript
   全部能力都能被编辑器统一调用。这一层是“与 C# / GDExtension
   平级”的接口适配层。
2. **复用前端层**：所有编辑期能力（`validate` / `complete_code` /
   `lookup_code` / 翻译提取 / 文档生成）都**复用 GDScript 自己的
   Parser/Analyzer**——保证“IDE 看到的世界”和“运行时看到的世界”
   严格一致。
3. **轻量旁路层**：语法高亮这种**实时性 > 准确性**的能力走自己的
   行级扫描器，但识别表（关键字/类名/全局函数）从前端同步——
   保证高亮规则不会与语法语义脱节。

“**复用 + 旁路**”的双轨结构是 GDScript 编辑器集成的核心思想：能复
用就复用，必须独立时也保持识别一致。这种克制让模块代码量保持在可
控范围，又不牺牲编辑体验。

---

## 小结

* `GDScriptLanguage` 实现 `ScriptLanguage` 的几十个虚方法，构成
  “编辑器 ↔ 脚本语言”的统一契约；
* `validate()` 直接复用 Parser + Analyzer，跨文件错误也能被报告；
  通过 “safe lines” 给静态类型行视觉反馈；
* `complete_code()` 由 `_find_identifiers_in_suite/class/base` 三层
  查找器组成，与运行时 `_get` 的多级回退一一对应；
* `lookup_code()` 用 `_lookup_symbol_from_base` 实现跳转，跳引擎
  类时落到内置文档而不是源文件；
* `GDScriptSyntaxHighlighter` 走自己的轻量行级扫描器，用
  `color_region_cache` 支持多行字符串/注释的局部刷新；
* `GDScriptDocGen` 在编译期把 `##` 注释 + AST 类型信息翻译成
  `DocData::ClassDoc`，复用引擎统一文档模型；
* `GDScriptEditorTranslationParserPlugin` 仅跑 Parser，按 AST 节点
  类型识别 `tr()` 调用提取本地化字符串。

下一章我们将看 GDScript 的语言服务器（LSP）——它如何把编辑器集成
里的这些能力进一步包装成跨进程的 JSON-RPC 接口，让 VSCode 等外部
编辑器也能享受同样的体验。
