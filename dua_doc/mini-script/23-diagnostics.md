# 第 23 章　错误诊断与恢复

到目前为止 Mini 的错误信息都是 `runtime error: undefined global:
foo`——能用，但跟 Rust / Elm 那种"指着源码画红线 + 告诉你怎么
改"的诊断比起来差距很大。

这一章我们补两件事：

1. **Diagnostic 数据结构**：把"位置 + 主信息 + 注释 + 建议"
   结构化，让前端（CLI、LSP、IDE）能格式化呈现；
2. **panic-mode 同步与多错误报告**：parser 一次跑完报出所有
   语法错误，而不是见一个错就退出。

## 23.1 Diagnostic 结构

```cpp
// src/diagnostic.h
enum class Severity { Error, Warning, Note, Help };

struct SourceSpan {
    int start_line, start_col;
    int end_line,   end_col;
};

struct Annotation {
    SourceSpan span;
    std::string message;
};

struct Diagnostic {
    Severity            severity;
    std::string         code;          // "E001", "E002"...
    std::string         message;       // 主信息
    SourceSpan          primary_span;
    std::vector<Annotation> notes;     // 二级注释
    std::vector<std::string> helps;    // "did you mean..." 建议
};
```

每条诊断都自带一个**code**——`E001` 这种短码方便用户搜索详细
说明（很多语言会有 `--explain E001` 命令显示长 doc）。

## 23.2 渲染：带源码片段的 ASCII art

```cpp
std::string render(const Diagnostic& d, const std::string& source) {
    std::ostringstream out;

    // 主标题
    out << (d.severity == Severity::Error ? "error" :
            d.severity == Severity::Warning ? "warning" : "note")
        << "[" << d.code << "]: " << d.message << "\n";

    // 源码切片
    out << "  --> line " << d.primary_span.start_line
        << ":" << d.primary_span.start_col << "\n";

    auto line = get_source_line(source, d.primary_span.start_line);
    out << "   |\n";
    out << std::setw(3) << d.primary_span.start_line << "| " << line << "\n";

    int caret_start = d.primary_span.start_col;
    int caret_len   = d.primary_span.end_line == d.primary_span.start_line
                      ? d.primary_span.end_col - d.primary_span.start_col : 1;
    out << "   | " << std::string(caret_start - 1, ' ')
        << std::string(std::max(1, caret_len), '^') << "\n";

    for (auto& note : d.notes) {
        out << "   = note: " << note.message << "\n";
    }
    for (auto& help : d.helps) {
        out << "   = help: " << help << "\n";
    }
    return out.str();
}
```

输出长这样：

```
error[E007]: undefined variable 'fooo'
  --> line 5:9
   |
 5 | print(fooo)
   |        ^^^^
   = help: a similar identifier exists: 'foo'
```

caret `^^^^` 长度对齐 token 实际宽度，`help` 行给 levenshtein
距离最近的已知 identifier。这两个细节让 Rust/Elm 的错误信息广
受好评，Mini 也照搬。

## 23.3 找相似名字：Levenshtein

```cpp
int levenshtein(const std::string& a, const std::string& b) {
    int m = a.size(), n = b.size();
    std::vector<int> dp(n + 1);
    for (int j = 0; j <= n; ++j) dp[j] = j;
    for (int i = 1; i <= m; ++i) {
        int prev = dp[0]; dp[0] = i;
        for (int j = 1; j <= n; ++j) {
            int tmp = dp[j];
            dp[j] = std::min({
                dp[j] + 1,                        // 删除
                dp[j-1] + 1,                      // 插入
                prev + (a[i-1] == b[j-1] ? 0 : 1) // 替换
            });
            prev = tmp;
        }
    }
    return dp[n];
}

std::string suggest_similar(const std::string& target,
        const std::vector<std::string>& candidates) {
    std::string best;
    int best_dist = (int)target.size() / 2 + 1;  // 阈值
    for (const auto& c : candidates) {
        int d = levenshtein(target, c);
        if (d < best_dist) { best_dist = d; best = c; }
    }
    return best;
}
```

`undefined variable 'fooo'` 时遍历当前 scope 里所有 binding 找
最近的：

```cpp
auto candidates = collect_visible_identifiers();
auto similar = suggest_similar("fooo", candidates);
if (!similar.empty()) {
    diag.helps.push_back("a similar identifier exists: '" + similar + "'");
}
```

简单实现 O(n·m) 遍历——Mini 的全局/局部表通常只有几十~几百个
名字，开销可忽略。

## 23.4 Parser 的 panic-mode 同步

第一版 Parser 见到错误就 throw 退出——只能报第一条。专业实现
要 **panic-mode**：

```cpp
void Parser::synchronize() {
    advance();      // 跳过出错的 token
    while (!is_at_end()) {
        if (previous().type == TokenType::Semicolon) return;
        switch (current().type) {
            case TokenType::Fn:
            case TokenType::Let:
            case TokenType::If:
            case TokenType::While:
            case TokenType::For:
            case TokenType::Return:
            case TokenType::End:
                return;
            default: advance();
        }
    }
}
```

遇到错误后跳过 token 直到看到下一个**语句开始关键字**——重新
进入正常解析。这样一次 parse 能报出文件里所有语法错误。

```cpp
void Parser::parse_program() {
    while (!is_at_end()) {
        try {
            stmts.push_back(parse_statement());
        } catch (const ParseError& e) {
            diags_.push_back(e.diag);
            synchronize();
        }
    }
}
```

`ParseError` 不再退出整个 compile，而是记录到 `diags_`，同步
后继续。最后一次性返回 `diags_`。

但要注意一个反作用力：**一个真错误经常被同步打散成几个伪错
误**——解析器进入了不熟悉的状态会一连串报。GDScript 有专门
的"silent error"模式：第一个错误后到 synchronize 之间报告的
warnings/errors 都丢弃，只留第一个。Mini 教学版做最朴素的就
够了，先告诉用户"可能存在级联错误，先修第一个再看"。

## 23.5 编译器 vs 运行时：统一管道

不要把"编译期 error"和"runtime error"分两个类——它们都用同
一个 Diagnostic 结构、同一个 render：

```cpp
class CompileResult {
public:
    std::vector<Diagnostic> diagnostics;
    std::shared_ptr<FunctionProto> proto;  // null 表示编译失败

    bool ok() const {
        for (auto& d : diagnostics)
            if (d.severity == Severity::Error) return false;
        return true;
    }
};

class RuntimeError : public std::exception {
public:
    Diagnostic diag;
    std::string traceback;   // 多个调用帧的源码截图
};
```

CLI 入口统一处理：

```cpp
int run_file(const std::string& path) {
    std::string source = read_file(path);
    auto result = compile(source);
    for (auto& d : result.diagnostics) {
        std::cerr << render(d, source);
    }
    if (!result.ok()) return 1;

    try {
        VM vm;
        vm.run(result.proto);
    } catch (const RuntimeError& e) {
        std::cerr << render(e.diag, source);
        std::cerr << e.traceback;
        return 2;
    }
    return 0;
}
```

LSP 也共用：把 `result.diagnostics` 和 RuntimeError.diag 翻译
成 LSP `Diagnostic` 推 `publishDiagnostics`。**只有一份诊断生
成代码**，前端（CLI 文本、LSP JSON、IDE 红线）只负责展示。

## 23.6 Runtime 错误的 backtrace

第 18 章我们已经收集到 frames 序列。把它格式化进 `traceback`：

```cpp
std::string format_traceback(const std::vector<CallFrame>& frames,
        const std::string& source) {
    std::ostringstream out;
    out << "stack trace (most recent call last):\n";
    for (auto& fr : frames) {
        std::string name = fr.proto->name.empty() ? "<main>" : fr.proto->name;
        int line = lookup_line(*fr.proto, fr.ip - 1);
        out << "  at " << name << " (line " << line << ")\n";
        if (line > 0) {
            auto src_line = get_source_line(source, line);
            out << "       " << src_line << "\n";
        }
    }
    return out.str();
}
```

输出：

```
runtime error: attempt to call a nil value
  --> line 7:5
   |
 7 |     foo()
   |     ^^^

stack trace (most recent call last):
  at outer (line 12)
       outer()
  at <main> (line 18)
       outer()
```

每一帧都附源码片段——比"`outer:12`"硬核多了。

## 23.7 Try-fix 建议

更进一步：根据错误类型给**自动修复**建议。

| 错误 | 建议 |
|------|------|
| `undefined variable 'fooo'` | "did you mean 'foo'?" + 改写预览 |
| `expected ')' got 'end'` | "missing ')' on line N" |
| `attempt to call a nil value` | "did you forget to define '%s'?" |
| `division by zero` | "use `if x != 0 then ... end`" |

这些走 `Diagnostic.helps` 字段，前端 IDE 可以接收为 LSP
"code action"，让用户一键应用。

GDScript Analyzer 就有大量这种 hint：`X cannot be converted to
Y`. `Use 'as Y' to make the conversion explicit.` —— 我们 Mini
照学，给每条 error code 写一个对应的 help table。

## 23.8 错误"等级"与--werror

从 GDScript 借鉴：让 warning 可以被升级为 error 阻止编译：

```cpp
struct CompileOptions {
    bool werror = false;          // 全部 warning 当 error
    std::set<std::string> werror_codes;  // 部分 warning 当 error
    std::set<std::string> ignore_codes;  // 抑制
};

void emit(Diagnostic d) {
    if (opts_.ignore_codes.count(d.code)) return;
    if (d.severity == Severity::Warning &&
        (opts_.werror || opts_.werror_codes.count(d.code))) {
        d.severity = Severity::Error;
    }
    diags_.push_back(d);
}
```

CLI：

```
mini --werror file.mini
mini --werror=W001,W002 file.mini
mini --ignore=W003 file.mini
```

适合 CI——团队可以决定"未使用变量是 warning，但 unreachable
code 必须当 error"。

## 23.9 第 23 章小结

* 统一 `Diagnostic` 数据结构（severity、code、span、notes、helps）
  让 CLI/LSP/IDE 共享渲染逻辑；
* 渲染输出 caret + source line + help，参考 Rust 的 ASCII art 风
  格；
* "did you mean" 用 Levenshtein 距离从可见 identifier 集合里挑
  最近的；
* Parser 通过 panic-mode `synchronize` 在错误后跳到下一个语句
  关键字，一次跑出所有语法错误；
* 编译期错误和运行期错误**复用同一管道**——只是源不同；
* Runtime backtrace 附源码片段，定位无歧义；
* `--werror` / `--ignore` 让团队按需调严或放宽。

下一章 **第 24 章 LSP 服务器**：把 Mini 接入 VSCode 的智能提示
/跳转/重命名。

