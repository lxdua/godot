
# GDScript 抽象类 Quick Fix —— [Implement All] 一键实现抽象方法

> 为 Godot 编辑器的错误面板新增 Quick Fix 功能：当非抽象类继承了抽象类但未实现抽象方法时，点击 `[Implement All]` 按钮自动在脚本末尾生成所有未实现抽象方法的存根代码。

---

## 一、修改目的

GDScript 已支持 `@abstract` 注解来声明抽象类和抽象方法，但当用户创建子类时，如果忘了实现抽象方法，只会得到一条编译错误：

```
Class "Circle" must implement "Shape.draw()" and other inherited abstract methods or be marked as "@abstract".
```

用户需要**手动查看**父类有哪些抽象方法、手动写出所有函数签名和默认返回值。对于多层继承链或父类有大量抽象方法的场景，这个过程繁琐且容易出错。

本改动在错误面板中为此类错误新增可点击的 `[Implement All]` 按钮，一键自动生成所有缺失的抽象方法存根。

**这是引擎中第一个针对 ParserError（编译错误）的 Quick Fix。** 此前引擎只有 Warning（警告）面板的 `[Ignore]` 按钮（插入 `@warning_ignore` 注解来压制警告），从未有过针对编译错误的自动修复功能。

---

## 二、改动文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `core/object/script_language.h` | **修改** | `ScriptError` 结构体新增 `fix_id` + `fix_data` 字段 |
| `modules/gdscript/gdscript_parser.h` | **修改** | `ParserError` 结构体新增 `fix_id` + `fix_data` 字段；`push_error` 新增重载声明 |
| `modules/gdscript/gdscript_parser.cpp` | **修改** | 实现 `push_error` 重载（附带修复数据） |
| `modules/gdscript/gdscript_analyzer.h` | **修改** | 声明 `push_error` 重载、`_generate_abstract_method_stub`、`_get_default_value_literal` |
| `modules/gdscript/gdscript_analyzer.cpp` | **修改** | 改造抽象方法检查循环（收集全部未实现方法）；实现存根生成和默认值生成 |
| `modules/gdscript/gdscript_editor.cpp` | **修改** | `validate()` 中传递 `fix_id` + `fix_data` 到 ScriptError |
| `editor/script/script_text_editor.h` | **修改** | 声明 `_implement_abstract_methods` 方法 |
| `editor/script/script_text_editor.cpp` | **修改** | 错误面板渲染 `[Implement All]` 按钮；点击分发逻辑；存根代码插入实现 |

共 **8 个文件**。

---

## 三、用户体验

### 改动前

```
错误面板：
  Line 1:  Class "Circle" must implement "Shape.draw()" and other inherited
           abstract methods or be marked as "@abstract".
```

用户只能看到报错，需要手动去父类查函数签名、手动写出每个方法。

### 改动后

```
错误面板：
  Line 1:  Class "Circle" must implement "Shape.draw()" and other inherited
           abstract methods or be marked as "@abstract".
           [Implement All]    ← 可点击，accent 颜色高亮
```

点击 `[Implement All]` 后，自动在脚本末尾插入：

```gdscript
func draw() -> void:
    pass # TODO: Override abstract method.

func get_area() -> float:
    return 0.0 # TODO: Override abstract method.
```

- 支持 **Ctrl+Z 一键撤销**（所有插入的存根作为一个整体操作）
- 自动匹配用户的**缩进设置**（Tab 或 Space）
- 插入后**自动重新验证**脚本，错误消失

---

## 四、实现原理

### 4.1 数据流全景

```
GDScriptAnalyzer::resolve_class_body()    ← Step 1: 检测到未实现的抽象方法
        ↓ push_error(msg, node, fix_id, fix_data)
GDScriptParser::ParserError                ← Step 2: 错误结构体携带修复数据
        ↓ validate()
ScriptLanguage::ScriptError                ← Step 3: 转换为编辑器可用的错误结构体
        ↓ _validate_script()
ScriptTextEditor::errors                   ← Step 4: 存储到编辑器成员变量
        ↓ _update_errors()
errors_panel (RichTextLabel)               ← Step 5: 渲染 [Implement All] 按钮
        ↓ meta_clicked 信号
ScriptTextEditor::_error_clicked()         ← Step 6: 点击分发
        ↓
ScriptTextEditor::_implement_abstract_methods()  ← Step 7: 在脚本末尾插入存根
```

### 4.2 Step 1-2：分析器层 —— 收集抽象方法并生成存根

**原有逻辑**：在 `resolve_class_body()` 中遍历继承链，发现**第一个**未实现的抽象方法就立即 `push_error` 并 `break`。

**改造后**：遍历继承链时**收集全部**未实现的抽象方法，为每个方法调用 `_generate_abstract_method_stub()` 生成存根代码字符串，最后将存根数组作为 `fix_data` 附加到错误中。

```cpp
// 伪代码
Vector<FunctionNode *> unimplemented_abstracts;
while (遍历继承链) {
    for (每个成员) {
        if (是抽象方法 && 没有被子类实现) {
            unimplemented_abstracts.push_back(该方法);
        }
    }
}
PackedStringArray stubs;
for (每个未实现方法) {
    stubs.push_back(_generate_abstract_method_stub(方法));
}
push_error(消息, 节点, "implement_abstract_methods", stubs);
```

**存根生成** `_generate_abstract_method_stub()` 会根据 `FunctionNode` 的信息拼出完整的函数签名：

- 函数名从 `identifier->name` 获取
- 参数列表从 `parameters` 数组获取（名称 + 类型标注）
- 返回类型从 `return_type` 和 `get_datatype()` 获取
- 函数体：void 或无返回类型用 `pass`，其他类型用 `return 默认值`

**默认值生成** `_get_default_value_literal()` 为各种内置类型返回合适的字面量：

| 类型 | 默认值 | 类型 | 默认值 |
|------|--------|------|--------|
| `bool` | `false` | `int` | `0` |
| `float` | `0.0` | `String` | `""` |
| `Vector2` | `Vector2()` | `Vector3` | `Vector3()` |
| `Color` | `Color()` | `Array` | `[]` |
| `Dictionary` | `{}` | 其他对象 | `null` |

### 4.3 Step 2-3：数据管道 —— ParserError → ScriptError

`ParserError` 和 `ScriptError` 都新增了两个字段：

```cpp
String fix_id;              // 修复标识符，如 "implement_abstract_methods"
PackedStringArray fix_data; // 修复数据，如方法存根代码字符串数组
```

这两个字段**默认为空**，不影响现有的任何错误。只有分析器在特定错误时主动填充。

在 `gdscript_editor.cpp` 的 `validate()` 方法中，`ParserError` 转换为 `ScriptError` 时原样传递这两个字段。

**为什么不用 Dictionary 而用 PackedStringArray？**

- 这里只需传递存根代码字符串列表，`PackedStringArray` 更轻量
- 避免在 parser 层引入复杂的 Dictionary 构造
- `PackedStringArray` 存入 `Variant`（Dictionary 的值类型）和取出时都有内置的隐式转换支持

### 4.4 Step 4-5：编辑器层 —— 渲染 Quick Fix 按钮

在 `_update_errors()` 中，遍历 errors 渲染表格时，检测 `err.fix_id`：

```cpp
if (err.fix_id == "implement_abstract_methods" && !err.fix_data.is_empty()) {
    Dictionary impl_meta;
    impl_meta["fix_id"] = err.fix_id;
    impl_meta["fix_data"] = err.fix_data;
    errors_panel->push_meta(impl_meta);   // 绑定点击元数据
    errors_panel->add_text(TTR("[Implement All]"));
    errors_panel->pop();
}
```

按钮使用 RichTextLabel 的 `push_meta` 机制实现——这与现有 warnings 面板的 `[Ignore]` 按钮采用完全相同的模式。按钮用 `accent_color` 高亮显示。

### 4.5 Step 6-7：编辑器层 —— 点击处理与代码插入

`_error_clicked()` 检测到 meta Dictionary 中有 `fix_id` 时，分发到对应的处理方法：

```cpp
if (meta.has("fix_id")) {
    String fix_id = meta["fix_id"];
    if (fix_id == "implement_abstract_methods") {
        _implement_abstract_methods(meta["fix_data"]);
    }
    return; // fix meta 没有 path/line/column，不要 fall through
}
```

`_implement_abstract_methods()` 的实现：

1. `begin_complex_operation()` —— 标记撤销组的开始
2. 获取用户的缩进设置，将存根中的 `\t` 替换为实际缩进
3. 在文件末尾拼接所有存根代码，用 `insert_text()` 插入
4. `end_complex_operation()` —— 标记撤销组的结束（Ctrl+Z 一步回退）
5. `_validate_script()` —— 重新验证脚本，刷新错误面板

---

## 五、与现有 [Ignore] 按钮的对比

| 方面 | `[Ignore]`（Warning） | `[Implement All]`（Error） |
|------|----------------------|---------------------------|
| 触发源 | 警告（Warning） | 编译错误（ParserError） |
| 修复本质 | 压制警告（插入 `@warning_ignore`） | 真正修复代码（生成方法实现） |
| 数据来源 | `Warning.string_code` 原有字段 | 新增的 `fix_id` + `fix_data` |
| 需要的数据 | 警告码字符串 | 多个方法的完整签名和存根代码 |
| 撤销支持 | 未用 `complex_operation`（单步操作，影响不大） | 使用 `complex_operation` 包裹 |

---

## 六、扩展性

本改动建立了一套通用的 Quick Fix 管道（`fix_id` + `fix_data`），后续可以复用此机制为其他编译错误添加 Quick Fix，只需：

1. 在分析器中给对应的 `push_error` 调用附加 `fix_id` 和 `fix_data`
2. 在 `_update_errors()` 中添加一个 `if (err.fix_id == "xxx")` 分支渲染按钮
3. 在 `_error_clicked()` 中添加一个分发分支
4. 实现对应的修复方法

可能的后续 Quick Fix：

- 修复 override 函数签名不匹配
- 为缺少返回值的函数添加 return 语句
- 给未使用的变量加 `_` 前缀
