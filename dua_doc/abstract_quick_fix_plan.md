
# GDScript 抽象类 Quick Fix —— 详细实现方案

## 目标

在 Godot 编辑器的错误面板中，当检测到"非抽象类未实现继承的抽象方法"时，提供两个可点击的快速修复按钮：

- **`[Implement All]`**：自动在脚本末尾插入所有未实现抽象方法的存根代码
- **`[Mark Abstract]`**：自动在类声明处插入 `@abstract` 注解

---

## 一、数据流全景

```
GDScriptAnalyzer::resolve_class_body()    ← 检测到未实现的抽象方法
        ↓ push_error()
GDScriptParser::ParserError                ← 携带修复数据（新增字段）
        ↓ validate()
ScriptLanguage::ScriptError                ← 携带修复数据（新增字段）
        ↓ _validate_script()
ScriptTextEditor::errors                   ← 存储到成员变量
        ↓ _update_errors()
errors_panel (RichTextLabel)               ← 渲染 [Implement All] / [Mark Abstract] 按钮
        ↓ meta_clicked 信号
ScriptTextEditor::_error_clicked()         ← 分发修复逻辑
        ↓
_implement_abstract_methods()              ← 在脚本末尾插入存根代码
_mark_class_abstract()                     ← 在类声明处插入 @abstract
```

---

## 二、逐文件改动详解

### 改动 1：`modules/gdscript/gdscript_parser.h` — 扩展 ParserError

**位置**：第 261-276 行，`struct ParserError`

**改动**：新增 `fix_id` 和 `fix_data` 字段。

```cpp
struct ParserError {
    String message;
    int start_line = 0;
    int start_column = 0;
    int end_line = 0;
    int end_column = 0;
    // === 新增 ===
    String fix_id;              // 修复标识，如 "implement_abstract_methods"
    PackedStringArray fix_data; // 修复所需数据，如方法存根代码行数组
};
```

**设计决策**：用 `PackedStringArray` 而非 `Dictionary`，因为：
- 这里只需要传递方法存根的代码字符串列表
- `PackedStringArray` 更轻量，拷贝成本低
- 避免在 parser 层引入 Dictionary 依赖

---

### 改动 2：`core/object/script_language.h` — 扩展 ScriptError

**位置**：第 229-234 行，`struct ScriptError`

**改动**：新增对应字段，与 ParserError 保持一致。

```cpp
struct ScriptError {
    String path;
    int line = -1;
    int column = -1;
    String message;
    // === 新增 ===
    String fix_id;
    PackedStringArray fix_data;
};
```

---

### 改动 3：`modules/gdscript/gdscript_analyzer.cpp` — 收集抽象方法并生成存根

**位置**：第 1532-1567 行，`resolve_class_body()` 中的抽象方法检查逻辑

**当前逻辑**（简化）：
```
遍历继承链 → 发现第一个未实现的抽象方法 → push_error → break
```

**问题**：当前逻辑只报告**第一个**未实现的方法就 break 了，不会收集全部。

**改造后逻辑**：
```
遍历继承链 → 收集所有未实现的抽象方法 → 生成存根字符串 → 附带到 push_error
```

**详细伪代码**：

```cpp
// 第 1532-1567 行替换为：
if (!p_class->is_abstract) {
    HashSet<StringName> implemented_funcs;
    Vector<GDScriptParser::FunctionNode *> unimplemented_abstracts; // 新增
    bool has_own_abstract = false; // 自身定义了抽象方法的情况
    const GDScriptParser::ClassNode *base_class = p_class;

    while (base_class != nullptr) {
        if (!base_class->is_abstract && base_class != p_class) {
            break;
        }
        for (GDScriptParser::ClassNode::Member member : base_class->members) {
            if (member.type == GDScriptParser::ClassNode::Member::FUNCTION) {
                if (member.function->is_abstract) {
                    if (base_class == p_class) {
                        has_own_abstract = true;
                    } else if (!implemented_funcs.has(member.function->identifier->name)) {
                        unimplemented_abstracts.push_back(member.function);
                    }
                } else {
                    implemented_funcs.insert(member.function->identifier->name);
                }
            }
        }
        // 继承链向上（与原代码相同）
        if (base_class->base_type.kind == GDScriptParser::DataType::CLASS) {
            base_class = base_class->base_type.class_type;
        } else if (base_class->base_type.kind == GDScriptParser::DataType::SCRIPT) {
            Ref<GDScriptParserRef> base_parser_ref = parser->get_depended_parser_for(base_class->base_type.script_path);
            ERR_BREAK(base_parser_ref.is_null());
            base_class = base_parser_ref->get_parser()->head;
        } else {
            break;
        }
    }

    const String class_name = p_class->identifier == nullptr ? p_class->fqcn.get_file() : String(p_class->identifier->name);

    if (has_own_abstract) {
        // 自身定义了抽象方法，不可能实现（和之前一样）
        push_error(vformat(R"*(Class "%s" is not abstract but contains abstract methods. Mark the class as "@abstract" or remove "@abstract" from all methods in this class.)*", class_name), p_class);
    } else if (!unimplemented_abstracts.is_empty()) {
        // 继承了未实现的抽象方法 → 生成存根 + 附加到错误
        PackedStringArray stubs;
        for (const GDScriptParser::FunctionNode *func : unimplemented_abstracts) {
            stubs.push_back(_generate_abstract_method_stub(func));
        }

        // 错误信息保持友好，列出第一个方法名
        const String base_class_name = ...; // 从第一个 unimplemented 获取
        String error_msg = vformat(
            R"*(Class "%s" must implement "%s.%s()" and other inherited abstract methods or be marked as "@abstract".)*",
            class_name, base_class_name, unimplemented_abstracts[0]->identifier->name
        );

        // 用新的 push_error 重载传递修复数据
        push_error_with_fix(error_msg, p_class, "implement_abstract_methods", stubs);
    }
}
```

**新增辅助函数** `_generate_abstract_method_stub()`：

```cpp
String GDScriptAnalyzer::_generate_abstract_method_stub(const GDScriptParser::FunctionNode *p_function) const {
    // 生成如 "func draw(x: int, y: int) -> void:\n\tpass"
    String stub = "func " + String(p_function->identifier->name) + "(";

    // 参数
    for (int i = 0; i < p_function->parameters.size(); i++) {
        if (i > 0) stub += ", ";
        const GDScriptParser::ParameterNode *param = p_function->parameters[i];
        stub += String(param->identifier->name);
        if (param->datatype_specifier != nullptr) {
            stub += ": " + param->get_datatype().to_string();
        }
    }
    stub += ")";

    // 返回类型
    GDScriptParser::DataType return_type = p_function->get_datatype();
    bool is_void = return_type.is_hard_type() && return_type.kind == GDScriptParser::DataType::BUILTIN && return_type.builtin_type == Variant::NIL;
    bool has_return_type = p_function->return_type != nullptr;

    if (has_return_type) {
        if (is_void) {
            stub += " -> void";
        } else {
            stub += " -> " + return_type.to_string();
        }
    }

    stub += ":\n";

    // 函数体（存根）
    if (!has_return_type || is_void) {
        stub += "\tpass # TODO: Override abstract method.";
    } else {
        stub += "\treturn " + _get_default_value_literal(return_type) + " # TODO: Override abstract method.";
    }

    return stub;
}
```

**新增辅助函数** `_get_default_value_literal()`：

```cpp
String GDScriptAnalyzer::_get_default_value_literal(const GDScriptParser::DataType &p_type) const {
    if (p_type.kind == GDScriptParser::DataType::BUILTIN) {
        switch (p_type.builtin_type) {
            case Variant::BOOL: return "false";
            case Variant::INT: return "0";
            case Variant::FLOAT: return "0.0";
            case Variant::STRING:
            case Variant::STRING_NAME: return "\"\"";
            case Variant::VECTOR2: return "Vector2()";
            case Variant::VECTOR2I: return "Vector2i()";
            case Variant::VECTOR3: return "Vector3()";
            case Variant::VECTOR3I: return "Vector3i()";
            case Variant::VECTOR4: return "Vector4()";
            case Variant::VECTOR4I: return "Vector4i()";
            case Variant::RECT2: return "Rect2()";
            case Variant::RECT2I: return "Rect2i()";
            case Variant::COLOR: return "Color()";
            case Variant::ARRAY: return "[]";
            case Variant::DICTIONARY: return "{}";
            case Variant::NODE_PATH: return "^\"\"";
            case Variant::TRANSFORM2D: return "Transform2D()";
            case Variant::TRANSFORM3D: return "Transform3D()";
            default: {
                String type_name = Variant::get_type_name(p_type.builtin_type);
                return type_name + "()";
            }
        }
    }
    return "null";
}
```

---

### 改动 4：`modules/gdscript/gdscript_analyzer.h` — 声明新方法

**位置**：private 区域（约第 150 行附近）

```cpp
// === 新增 ===
String _generate_abstract_method_stub(const GDScriptParser::FunctionNode *p_function) const;
String _get_default_value_literal(const GDScriptParser::DataType &p_type) const;
void push_error_with_fix(const String &p_message, const GDScriptParser::Node *p_origin, const String &p_fix_id, const PackedStringArray &p_fix_data);
```

---

### 改动 5：`modules/gdscript/gdscript_analyzer.cpp` — 实现 push_error_with_fix

**位置**：在 `push_error()` 方法（第 6468 行）附近

```cpp
void GDScriptAnalyzer::push_error_with_fix(const String &p_message, const GDScriptParser::Node *p_origin, const String &p_fix_id, const PackedStringArray &p_fix_data) {
    mark_node_unsafe(p_origin);
    parser->push_error(p_message, p_origin);
    // 给刚刚添加的最后一条错误附加修复数据
    if (!parser->errors.is_empty()) {
        parser->errors.back()->get().fix_id = p_fix_id;
        parser->errors.back()->get().fix_data = p_fix_data;
    }
}
```

**注意**：需要确认 `parser->errors` 的访问权限。当前 `GDScriptParser::errors` 是 `List<ParserError>`，`GDScriptAnalyzer` 通过 `parser->` 指针访问。如果是 private，需要：
- 方案 A：给 `GDScriptParser::push_error()` 增加重载，接受 fix_id + fix_data 参数（**推荐**）
- 方案 B：给 GDScriptParser 增加 friend 声明

**推荐方案 A**：

在 `gdscript_parser.h` 中：
```cpp
void push_error(const String &p_message, const Node *p_origin = nullptr);
// === 新增重载 ===
void push_error(const String &p_message, const Node *p_origin, const String &p_fix_id, const PackedStringArray &p_fix_data);
```

在 `gdscript_parser.cpp` 中：
```cpp
void GDScriptParser::push_error(const String &p_message, const Node *p_origin, const String &p_fix_id, const PackedStringArray &p_fix_data) {
    push_error(p_message, p_origin);  // 调用原有逻辑
    if (!errors.is_empty()) {
        errors.back()->get().fix_id = p_fix_id;
        errors.back()->get().fix_data = p_fix_data;
    }
}
```

那么 `GDScriptAnalyzer` 中就不需要 `push_error_with_fix` 了，直接：
```cpp
push_error(error_msg, p_class, "implement_abstract_methods", stubs);
```

因为 `GDScriptAnalyzer::push_error` 会调用 `parser->push_error`，需要增加对应的重载。

在 `gdscript_analyzer.h` 中：
```cpp
void push_error(const String &p_message, const GDScriptParser::Node *p_origin, const String &p_fix_id, const PackedStringArray &p_fix_data);
```

在 `gdscript_analyzer.cpp` 中：
```cpp
void GDScriptAnalyzer::push_error(const String &p_message, const GDScriptParser::Node *p_origin, const String &p_fix_id, const PackedStringArray &p_fix_data) {
    mark_node_unsafe(p_origin);
    parser->push_error(p_message, p_origin, p_fix_id, p_fix_data);
}
```

---

### 改动 6：`modules/gdscript/gdscript_editor.cpp` — 传递修复数据到 ScriptError

**位置**：第 170-177 行（`validate()` 方法中 ParserError → ScriptError 的转换）

```cpp
for (const GDScriptParser::ParserError &pe : parser.get_errors()) {
    ScriptLanguage::ScriptError e;
    e.path = p_path;
    e.line = pe.start_line;
    e.column = pe.start_column;
    e.message = pe.message;
    // === 新增 ===
    e.fix_id = pe.fix_id;
    e.fix_data = pe.fix_data;
    r_errors->push_back(e);
}
```

同样处理第 181-187 行的 depended parser 错误。

---

### 改动 7：`editor/script/script_text_editor.h` — 声明新方法

**位置**：第 174 行 `_error_clicked` 附近

```cpp
void _error_clicked(const Variant &p_line);
// === 新增 ===
void _implement_abstract_methods(const PackedStringArray &p_stubs);
void _mark_class_abstract(int p_line);
```

---

### 改动 8：`editor/script/script_text_editor.cpp` — 修改 _update_errors()

**位置**：第 964-987 行

**当前**：错误面板只渲染"行号 + 消息"两列表格。

**改造后**：检测到 `fix_id` 时，在消息后面追加可点击的修复按钮。

```cpp
void ScriptTextEditor::_update_errors() {
    code_editor->set_error_count(errors.size());

    errors_panel->clear();
    errors_panel->push_table(2);
    for (const ScriptLanguage::ScriptError &err : errors) {
        // --- 第一列：行号（不变） ---
        errors_panel->push_cell();
        errors_panel->push_meta(err.line - 1);
        errors_panel->push_color(warnings_panel->get_theme_color(SNAME("error_color"), EditorStringName(Editor)));
        errors_panel->add_text(vformat(TTR("Line %d:"), err.line));
        errors_panel->pop(); // Color.
        errors_panel->pop(); // Meta goto.
        errors_panel->pop(); // Cell.

        // --- 第二列：消息 + Quick Fix 按钮 ---
        errors_panel->push_cell();
        errors_panel->add_text(err.message);

        // === 新增：Quick Fix 按钮 ===
        if (err.fix_id == "implement_abstract_methods" && !err.fix_data.is_empty()) {
            errors_panel->add_newline();

            // [Implement All] 按钮
            Dictionary impl_meta;
            impl_meta["fix_id"] = "implement_abstract_methods";
            impl_meta["fix_data"] = err.fix_data;
            errors_panel->push_meta(impl_meta);
            errors_panel->push_color(
                errors_panel->get_theme_color(SNAME("accent_color"), EditorStringName(Editor)));
            errors_panel->add_text(TTR("[Implement All]"));
            errors_panel->pop(); // Color.
            errors_panel->pop(); // Meta.

            errors_panel->add_text("  ");

            // [Mark Abstract] 按钮
            Dictionary mark_meta;
            mark_meta["fix_id"] = "mark_class_abstract";
            mark_meta["line"] = err.line;
            errors_panel->push_meta(mark_meta);
            errors_panel->push_color(
                errors_panel->get_theme_color(SNAME("accent_color"), EditorStringName(Editor)));
            errors_panel->add_text(TTR("[Mark Abstract]"));
            errors_panel->pop(); // Color.
            errors_panel->pop(); // Meta.
        }

        errors_panel->add_newline();
        errors_panel->pop(); // Cell.
    }
    errors_panel->pop(); // Table

    // ... 后续 depended_errors 渲染（不变） ...
}
```

---

### 改动 9：`editor/script/script_text_editor.cpp` — 修改 _error_clicked()

**位置**：第 412-440 行

**改造**：在处理 Dictionary meta 时，先检查是否是 fix_id，是则分发到修复方法。

```cpp
void ScriptTextEditor::_error_clicked(const Variant &p_line) {
    if (p_line.get_type() == Variant::INT) {
        goto_line_centered(p_line.operator int64_t());
    } else if (p_line.get_type() == Variant::DICTIONARY) {
        Dictionary meta = p_line.operator Dictionary();

        // === 新增：Quick Fix 分发 ===
        if (meta.has("fix_id")) {
            String fix_id = meta["fix_id"];
            if (fix_id == "implement_abstract_methods") {
                _implement_abstract_methods(meta["fix_data"]);
                return;
            }
            if (fix_id == "mark_class_abstract") {
                _mark_class_abstract(meta["line"]);
                return;
            }
        }

        // --- 原有逻辑（行跳转）不变 ---
        const String path = meta["path"].operator String();
        const int line = meta["line"].operator int64_t();
        const int column = meta["column"].operator int64_t();
        // ...
    }
}
```

---

### 改动 10：`editor/script/script_text_editor.cpp` — 实现修复方法

**位置**：在 `_error_clicked()` 方法之后添加

#### `_implement_abstract_methods()`

```cpp
void ScriptTextEditor::_implement_abstract_methods(const PackedStringArray &p_stubs) {
    CodeEdit *te = code_editor->get_text_editor();
    te->begin_complex_operation(); // 支持 Ctrl+Z 一键撤销

    int last_line = te->get_line_count() - 1;
    String last_line_text = te->get_line(last_line);

    // 拼接所有存根代码
    String code_to_insert;
    // 确保末尾有空行分隔
    if (!last_line_text.strip_edges().is_empty()) {
        code_to_insert += "\n";
    }
    for (const String &stub : p_stubs) {
        code_to_insert += "\n" + stub + "\n";
    }

    te->insert_text(code_to_insert, last_line, last_line_text.length());

    te->end_complex_operation();

    // 重新验证脚本，刷新错误面板
    _validate_script();
}
```

#### `_mark_class_abstract()`

```cpp
void ScriptTextEditor::_mark_class_abstract(int p_line) {
    CodeEdit *te = code_editor->get_text_editor();
    te->begin_complex_operation();

    // p_line 是 1-indexed（来自错误信息），转成 0-indexed
    int target_line = p_line - 1;

    // 找到需要插入 @abstract 的位置
    // 通常错误指向 class_name 或 extends 所在行，或脚本第一行
    // 在该行之前插入 @abstract
    String line_text = te->get_line(target_line);

    if (line_text.strip_edges().begins_with("class_name") || line_text.strip_edges().begins_with("extends")) {
        // 在 class_name/extends 行之前插入
        te->insert_line_at(target_line, "@abstract");
    } else if (line_text.strip_edges().begins_with("class ")) {
        // 内部类的情况：在 class 行之前插入（注意缩进）
        int indent_level = line_text.length() - line_text.lstrip("\t ").length();
        String indent = line_text.substr(0, indent_level);
        te->insert_line_at(target_line, indent + "@abstract");
    } else {
        // 脚本顶层（没有 class_name/extends），在第一行插入
        te->insert_line_at(0, "@abstract");
    }

    te->end_complex_operation();
    _validate_script();
}
```

---

## 三、缩进处理的关键细节

生成存根代码时，缩进需要与用户设置一致。但这里有个矛盾：

- **分析器层**（`gdscript_analyzer.cpp`）不知道编辑器的缩进设置
- **编辑器层**（`script_text_editor.cpp`）知道缩进设置

**解决方案**：分析器中的存根统一使用 `\t`（单个 Tab）作为缩进，在编辑器层插入代码时，根据用户设置将 `\t` 替换为实际缩进：

```cpp
// 在 _implement_abstract_methods() 中
String indent;
if (te->is_indent_using_spaces()) {
    indent = String(" ").repeat(te->get_indent_size());
} else {
    indent = "\t";
}

for (String &stub : stubs_copy) {
    stub = stub.replace("\t", indent);
}
```

---

## 四、内部类的特殊处理

GDScript 支持内部类（inner class），内部类也可以是抽象的：

```gdscript
class Outer:
    @abstract class Inner:
        @abstract func foo() -> void

    class InnerImpl extends Inner:
        # 这里需要实现 foo()
        pass
```

**当前分析器已经正确处理**了内部类的抽象检查（因为 `resolve_class_body` 是递归调用的）。

对于 Quick Fix，需要注意：
- 存根代码的缩进需要匹配内部类的层级
- `[Mark Abstract]` 需要在正确的 class 行前插入 `@abstract`

**处理方式**：错误的 `start_line` 指向的就是内部类声明的行号，`_mark_class_abstract()` 中已经处理了 `class ` 开头的情况。对于存根插入，内部类的情况更复杂——需要找到内部类体的末尾而不是文件末尾。

**简化处理（Phase 1）**：先只处理顶层类（非内部类）的情况，在文件末尾插入存根。后续可以迭代支持内部类。

---

## 五、完整改动文件清单

| # | 文件 | 改动类型 | 改动内容 |
|---|------|---------|---------|
| 1 | `modules/gdscript/gdscript_parser.h` | 修改 | `ParserError` 新增 `fix_id` + `fix_data` 字段；`push_error` 新增重载声明 |
| 2 | `modules/gdscript/gdscript_parser.cpp` | 修改 | 实现 `push_error` 新重载 |
| 3 | `core/object/script_language.h` | 修改 | `ScriptError` 新增 `fix_id` + `fix_data` 字段 |
| 4 | `modules/gdscript/gdscript_analyzer.h` | 修改 | 声明 `push_error` 重载、`_generate_abstract_method_stub`、`_get_default_value_literal` |
| 5 | `modules/gdscript/gdscript_analyzer.cpp` | 修改 | 改造抽象方法检查循环（收集全部未实现方法）；实现存根生成；实现 `push_error` 重载 |
| 6 | `modules/gdscript/gdscript_editor.cpp` | 修改 | `validate()` 中传递 `fix_id` + `fix_data` 到 ScriptError |
| 7 | `editor/script/script_text_editor.h` | 修改 | 声明 `_implement_abstract_methods`、`_mark_class_abstract` |
| 8 | `editor/script/script_text_editor.cpp` | 修改 | `_update_errors()` 渲染 Quick Fix 按钮；`_error_clicked()` 分发修复；实现两个修复方法 |

共 **8 个文件**，预计新增约 **150-200 行**代码。

---

## 六、测试用例

### 用例 1：基本 Quick Fix

```gdscript
# abstract_base.gd
@abstract class_name AbstractBase
@abstract func foo() -> void
@abstract func bar(x: int) -> float
```

```gdscript
# child.gd
extends AbstractBase
# 此时错误面板应显示错误 + [Implement All] + [Mark Abstract]
```

点击 `[Implement All]` 后 `child.gd` 应变为：

```gdscript
extends AbstractBase

func foo() -> void:
	pass # TODO: Override abstract method.

func bar(x: int) -> float:
	return 0.0 # TODO: Override abstract method.
```

### 用例 2：多层继承

```gdscript
# a.gd
@abstract class_name A
@abstract func a_method() -> String

# b.gd（中间层，部分实现）
@abstract class_name B extends A
@abstract func b_method() -> int
# a_method 未实现

# c.gd
extends B
# 应需要实现 a_method 和 b_method
```

### 用例 3：Mark Abstract

点击 `[Mark Abstract]` 后 `child.gd` 应变为：

```gdscript
@abstract
extends AbstractBase
```

### 用例 4：Ctrl+Z 撤销

- 点击 `[Implement All]` 后，Ctrl+Z 应一次性撤销所有插入的存根
- 点击 `[Mark Abstract]` 后，Ctrl+Z 应撤销 `@abstract` 的插入

### 用例 5：自身定义抽象方法

```gdscript
# bad.gd — 自身有 @abstract func 但类不是 abstract
extends Node
@abstract func test() -> void
# 此错误只显示 [Mark Abstract]，不显示 [Implement All]
```
