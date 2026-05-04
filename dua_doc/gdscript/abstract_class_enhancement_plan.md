
# GDScript 抽象类开发体验增强 —— 规划文档

## 目标

提升 GDScript 中抽象类的开发体验，分两个子特性：

1. **创建子类脚本时自动生成抽象方法存根**
2. **错误面板中的 Quick Fix：一键实现所有抽象方法**

---

## 一、背景分析

### 1.1 当前抽象类的工作流程

```gdscript
# base.gd
@abstract class_name Shape
@abstract func draw() -> void
@abstract func get_area() -> float
func describe():
    print("Area: %s" % get_area())
```

当用户创建一个继承自 `Shape` 的子类脚本时：
- **现状**：新脚本只有默认模板（`extends Shape`），没有任何抽象方法的存根
- **编译后**才会报错：`Class "Circle" must implement "Shape.draw()" and other inherited abstract methods or be marked as "@abstract".`
- 用户需要**手动查看**父类有哪些抽象方法，手动写出所有签名
- **痛点**：效率低、容易遗漏或写错签名

### 1.2 现有可参考的机制

| 机制 | 位置 | 说明 |
|------|------|------|
| `[Ignore]` 警告按钮 | `script_text_editor.cpp:940` | 在 warnings_panel 中用 `push_meta` 实现可点击操作，点击后自动插入 `@warning_ignore` |
| Override 方法补全 | `gdscript_editor.cpp:3682` | 输入 `func ` 时列出可重写的父类方法（含抽象方法），但不区分优先级 |
| `make_template()` | `gdscript_editor.cpp:83` | 创建脚本时用模板生成代码，通过字符串替换 `_BASE_`、`_CLASS_` 等占位符 |
| 函数签名字符串 | `gdscript_parser.cpp:1765` | `FunctionNode::signature` 存储了如 `(x: int) -> void` 的签名文本 |
| 抽象方法检查 | `gdscript_analyzer.cpp:1532-1567` | 遍历继承链收集未实现的抽象方法，生成错误信息 |

---

## 二、特性一：创建子类脚本时自动生成抽象方法存根

### 2.1 设计方案

在 `GDScriptLanguage::make_template()` 的流程中，当检测到父类（`p_base_class_name`）是一个带有抽象方法的类时，自动在生成的脚本模板中附加所有未实现抽象方法的存根代码。

### 2.2 用户体验

**改进前**：创建继承自 `Shape` 的脚本 → 得到：
```gdscript
extends Shape
```

**改进后**：创建继承自 `Shape` 的脚本 → 得到：
```gdscript
extends Shape


func draw() -> void:
	pass # TODO: Override abstract method.


func get_area() -> float:
	return 0.0 # TODO: Override abstract method.
```

### 2.3 需要改动的文件

#### (a) `modules/gdscript/gdscript.h` / `gdscript.cpp`

新增静态辅助方法，用于从脚本路径或类名收集所有抽象方法的签名：

```cpp
// gdscript.h - GDScriptLanguage 类中新增
struct AbstractMethodStub {
    String name;
    String signature;      // 如 "(x: int) -> void"
    String return_type;    // 如 "float", "void", ""
    bool has_return_value; // 用于决定存根中是 `pass` 还是 `return 默认值`
};

Vector<AbstractMethodStub> _collect_abstract_methods(const String &p_base_class) const;
```

```cpp
// gdscript.cpp - 实现
// 逻辑参考 gdscript_analyzer.cpp:1532-1567 的抽象方法遍历模式
// 1. 解析父类脚本 → 获取 parser->head（ClassNode）
// 2. 沿继承链向上遍历，收集所有 is_abstract 的 FunctionNode
// 3. 排除已在中间层被实现的方法
// 4. 返回方法签名列表
```

#### (b) `modules/gdscript/gdscript_editor.cpp` — `make_template()`

在模板生成后、返回前，附加抽象方法存根：

```cpp
Ref<Script> GDScriptLanguage::make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
    // ... 现有模板处理逻辑 ...

    // === 新增：附加抽象方法存根 ===
    Vector<AbstractMethodStub> abstract_methods = _collect_abstract_methods(p_base_class_name);
    if (!abstract_methods.is_empty()) {
        String stubs = "\n";
        String indent = _get_indentation();
        for (const AbstractMethodStub &method : abstract_methods) {
            stubs += "\nfunc " + method.name + method.signature + ":\n";
            if (method.has_return_value) {
                stubs += indent + "return " + _get_default_value_for_type(method.return_type) + " # TODO: Override abstract method.\n";
            } else {
                stubs += indent + "pass # TODO: Override abstract method.\n";
            }
        }
        processed_template += stubs;
    }

    scr->set_source_code(processed_template);
    return scr;
}
```

#### (c) 辅助：`_get_default_value_for_type()`

根据返回类型生成合理的默认值：

| 返回类型 | 默认值 |
|----------|--------|
| `int` | `0` |
| `float` | `0.0` |
| `bool` | `false` |
| `String` | `""` |
| `Vector2` | `Vector2()` |
| `Array` | `[]` |
| `void` / 无 | (用 `pass`) |
| 其他对象类型 | `null` |

### 2.4 跨文件继承的处理

抽象方法可能来自多层继承链，且父类可能在不同文件中：

```
Entity (res://entity.gd)         → @abstract func get_name()
  └─ Character (res://char.gd)   → @abstract（未实现 get_name，新增 @abstract func get_speed()）
       └─ Player (新创建)         → 需要实现 get_name() 和 get_speed()
```

收集逻辑需要：
1. 处理 `class_name` 引用（通过 `ScriptServer::get_global_class_path()`）
2. 处理相对路径引用（`preload("./base.gd")`）
3. 跨文件解析（通过 `GDScriptParserRef`）

### 2.5 EditorSettings 配置项

新增编辑器设置项，让用户可以控制此行为：

```
editor/script/auto_generate_abstract_stubs = true  (默认开启)
```

---

## 三、特性二：错误面板 Quick Fix —— 一键实现抽象方法

### 3.1 设计方案

参考现有 warnings 面板的 `[Ignore]` 按钮模式（`script_text_editor.cpp:940`），在错误面板中为"未实现抽象方法"的错误添加一个 `[Implement]` 可点击按钮。

### 3.2 用户体验

**错误面板改进后**：

```
Line 1:  Class "Circle" must implement "Shape.draw()" and other inherited abstract methods
         or be marked as "@abstract".
         [Implement All]  [Mark Abstract]
```

点击 `[Implement All]` → 自动在脚本末尾插入所有未实现抽象方法的存根代码。
点击 `[Mark Abstract]` → 自动在 class 声明前插入 `@abstract` 注解。

### 3.3 需要改动的文件

#### (a) `core/object/script_language.h` — `ScriptError` 结构体扩展

```cpp
struct ScriptError {
    String path;
    int line = -1;
    int column = -1;
    String message;
    // === 新增 ===
    String fix_id;           // 修复标识符，如 "implement_abstract_methods"
    Dictionary fix_data;     // 修复所需的数据（如方法签名列表）
};
```

#### (b) `modules/gdscript/gdscript_analyzer.cpp` — 在报错时附带修复数据

在 `resolve_class_body()` 的抽象方法检查逻辑中（1532-1567行），收集未实现的抽象方法列表，附加到错误信息中：

```cpp
// 改造现有的抽象方法检查循环
if (!p_class->is_abstract) {
    HashSet<StringName> implemented_funcs;
    Vector<String> unimplemented_stubs;  // 新增：收集存根代码
    const GDScriptParser::ClassNode *base_class = p_class;

    while (base_class != nullptr) {
        // ... 现有遍历逻辑 ...
        for (member : base_class->members) {
            if (member.function->is_abstract && !implemented_funcs.has(name)) {
                // 收集方法签名用于 Quick Fix
                unimplemented_stubs.push_back(
                    _generate_method_stub(member.function)
                );
            }
        }
    }

    if (!unimplemented_stubs.is_empty()) {
        // push_error 时附带修复数据
        // 具体方式见 3.3(d)
    }
}
```

#### (c) `editor/script/script_text_editor.cpp` — 错误面板渲染 Quick Fix 按钮

在 `_update_errors()` 方法中，检测错误是否有 `fix_id`，如果有则渲染可点击按钮：

```cpp
void ScriptTextEditor::_update_errors() {
    // ... 现有逻辑 ...
    for (const ScriptLanguage::ScriptError &err : errors) {
        // ... 现有的行号和消息渲染 ...

        // === 新增：Quick Fix 按钮 ===
        if (!err.fix_id.is_empty()) {
            errors_panel->push_cell();
            Dictionary fix_meta;
            fix_meta["fix_id"] = err.fix_id;
            fix_meta["fix_data"] = err.fix_data;
            fix_meta["line"] = err.line;

            if (err.fix_id == "implement_abstract_methods") {
                errors_panel->push_meta(fix_meta);
                errors_panel->push_color(accent_color);
                errors_panel->add_text(TTR("[Implement All]"));
                errors_panel->pop(); // Color
                errors_panel->pop(); // Meta

                errors_panel->add_text("  ");

                Dictionary mark_meta;
                mark_meta["fix_id"] = "mark_class_abstract";
                mark_meta["line"] = err.line;
                errors_panel->push_meta(mark_meta);
                errors_panel->push_color(accent_color);
                errors_panel->add_text(TTR("[Mark Abstract]"));
                errors_panel->pop(); // Color
                errors_panel->pop(); // Meta
            }
            errors_panel->pop(); // Cell
        }
    }
}
```

#### (d) `editor/script/script_text_editor.cpp` — 处理 Quick Fix 点击

修改 `_error_clicked()` 方法，处理新的 fix_meta：

```cpp
void ScriptTextEditor::_error_clicked(const Variant &p_line) {
    if (p_line.get_type() == Variant::DICTIONARY) {
        Dictionary meta = p_line.operator Dictionary();

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

        // ... 现有的跳转逻辑 ...
    }
}
```

#### (e) `editor/script/script_text_editor.cpp` — 新增修复方法

```cpp
void ScriptTextEditor::_implement_abstract_methods(const Dictionary &p_fix_data) {
    CodeEdit *te = code_editor->get_text_editor();
    te->begin_complex_operation(); // 支持 Ctrl+Z 一键撤销

    PackedStringArray stubs = p_fix_data["stubs"];
    int insert_line = te->get_line_count() - 1;

    String indent = _get_indentation();
    String code_to_insert = "\n";
    for (const String &stub : stubs) {
        code_to_insert += "\n" + stub;
    }

    te->insert_text(code_to_insert, insert_line, te->get_line(insert_line).length());
    te->end_complex_operation();

    _validate_script(); // 重新验证，刷新错误面板
}

void ScriptTextEditor::_mark_class_abstract(int p_line) {
    CodeEdit *te = code_editor->get_text_editor();
    te->begin_complex_operation();

    // 在脚本第一行（或 class_name 行之前）插入 @abstract
    int target_line = p_line - 1; // 转为 0-indexed
    String existing = te->get_line(target_line);

    if (existing.begins_with("class_name") || existing.begins_with("extends")) {
        te->insert_line_at(target_line, "@abstract");
    } else {
        // 在当前行前面加上 @abstract
        te->set_line(target_line, "@abstract " + existing);
    }

    te->end_complex_operation();
    _validate_script();
}
```

### 3.4 信息传递方案

需要把分析器中收集到的抽象方法信息传递到编辑器层。有两种可选方案：

**方案 A：扩展 ScriptError 结构体**（推荐）
- 在 `ScriptError` 中新增 `fix_id` 和 `fix_data` 字段
- 分析器在报错时直接填充修复数据
- 优点：改动集中、数据传递自然
- 缺点：稍微扩大了 ScriptError 的职责

**方案 B：独立的 FixHint 系统**
- 新增 `ScriptFixHint` 结构体，与错误关联但独立存储
- 优点：职责分离
- 缺点：需要更多的管道代码

**选择方案 A**，原因：
1. 与现有的 Warning `[Ignore]` 模式一致（Warning 也是在同一结构体中处理交互）
2. 改动量更小
3. 不需要额外的关联/匹配逻辑

---

## 四、Override 方法补全增强（附加优化）

### 4.1 补全列表中优先展示抽象方法

在 `gdscript_editor.cpp:3682` 的 `COMPLETION_OVERRIDE_METHOD` 分支中：

```cpp
// 现有代码（约 3706-3709 行）
String display_name = member.function->identifier->name;
display_name += member.function->signature + ":";
ScriptLanguage::CodeCompletionOption option(display_name, ScriptLanguage::CODE_COMPLETION_KIND_FUNCTION);

// === 改进 ===
if (member.function->is_abstract) {
    option.location = ScriptLanguage::LOCATION_LOCAL; // 最高优先级，排在最前
    // 可选：修改 display_name 加前缀标记
}
```

**改动文件**：`modules/gdscript/gdscript_editor.cpp`

---

## 五、实现步骤（推荐顺序）

### Phase 1：错误面板 Quick Fix（核心价值最高）

| 步骤 | 文件 | 内容 |
|------|------|------|
| 1.1 | `core/object/script_language.h` | 扩展 `ScriptError` 结构体，新增 `fix_id`、`fix_data` |
| 1.2 | `modules/gdscript/gdscript_analyzer.cpp` | 在抽象方法检查中收集方法签名存根，填充到错误的 fix_data 中 |
| 1.3 | `editor/script/script_text_editor.h` | 声明新方法 `_implement_abstract_methods()`、`_mark_class_abstract()` |
| 1.4 | `editor/script/script_text_editor.cpp` | 实现 `_update_errors()` 中的 Quick Fix 按钮渲染 |
| 1.5 | `editor/script/script_text_editor.cpp` | 实现 `_error_clicked()` 中的 Quick Fix 分发逻辑 |
| 1.6 | `editor/script/script_text_editor.cpp` | 实现 `_implement_abstract_methods()` 和 `_mark_class_abstract()` |

### Phase 2：创建脚本时自动生成存根

| 步骤 | 文件 | 内容 |
|------|------|------|
| 2.1 | `modules/gdscript/gdscript.h` | 声明 `_collect_abstract_methods()` 和辅助结构体 |
| 2.2 | `modules/gdscript/gdscript.cpp` 或 `gdscript_editor.cpp` | 实现抽象方法收集逻辑 |
| 2.3 | `modules/gdscript/gdscript_editor.cpp` | 修改 `make_template()`，追加存根代码 |
| 2.4 | `editor/settings/editor_settings.cpp` | 新增配置项 `editor/script/auto_generate_abstract_stubs` |

### Phase 3：补全增强（锦上添花）

| 步骤 | 文件 | 内容 |
|------|------|------|
| 3.1 | `modules/gdscript/gdscript_editor.cpp` | 在 `COMPLETION_OVERRIDE_METHOD` 中给抽象方法设置更高优先级 |

---

## 六、测试计划

### 单元测试

- `modules/gdscript/tests/scripts/` 下新增测试脚本：
  - 单层继承的抽象方法存根生成
  - 多层继承链（部分实现）的存根生成
  - 跨文件继承的存根生成
  - 无抽象方法时不生成多余代码

### 手动测试用例

1. **创建脚本存根生成**
   - 新建脚本，继承一个有 2 个抽象方法的类 → 验证存根自动生成
   - 新建脚本，继承一个多层链（中间层已实现部分方法）的类 → 只生成未实现的
   - 新建脚本，继承一个非抽象类 → 不生成任何额外代码

2. **Quick Fix 按钮**
   - 写一个非抽象类继承抽象类 → 错误面板出现 `[Implement All]` 和 `[Mark Abstract]`
   - 点击 `[Implement All]` → 存根代码正确插入、错误消失
   - 点击 `[Mark Abstract]` → `@abstract` 正确插入、错误消失
   - Ctrl+Z 撤销 → 修改正确回退

3. **补全增强**
   - 在子类中输入 `func ` → 抽象方法排在补全列表最前面

---

## 七、风险与注意事项

1. **ScriptError 结构体变更**：这是 core 层的结构体，扩展字段时需确保不影响其他语言（C#、VisualScript 等）。新增字段都有默认值，应无破坏性。

2. **跨文件解析的性能**：收集抽象方法需要解析继承链上的所有脚本文件。在 `make_template()` 调用时是可接受的（一次性），在错误分析时也是可接受的（本就会做这个遍历）。

3. **缩进风格**：生成的存根代码需要尊重用户的缩进设置（Tab vs Spaces、缩进宽度）。使用 `_get_indentation()` 或 `EditorSettings` 中的配置。

4. **类型标注偏好**：用户可能开启/关闭 type hints（`text_editor/completion/add_type_hints`），存根生成时需要尊重此设置。

5. **LSP 兼容**：未来可以将 Quick Fix 信息也暴露给 LSP 的 `textDocument/codeAction`，但这不在本期范围内。
