# 第 24 章　LSP 服务器：接入 VSCode

到这里 Mini 已经能跑、能 dump、能报漂亮错误。但用户写 Mini 代
码还在记事本里裸打——没有补全、没有跳转定义、没有 hover 文档。
是时候上 **LSP（Language Server Protocol）** 了。

LSP 的好处：写一遍 server，自动支持 VSCode、Vim/NeoVim、Emacs、
Sublime、Helix、IntelliJ……所有主流编辑器都有 LSP 客户端。

这一章我们：

1. 解释 LSP 的 JSON-RPC 模型；
2. 写一个最小的 mini-lsp server（hover + diagnostics + goto-def）；
3. 写一个 VSCode extension 把 server 拉起来。

代码完整版约 800 行 C++，本章只讲核心结构。

## 24.1 LSP 通讯协议

LSP 基于 **JSON-RPC 2.0**，运行在 **stdio**（也支持 socket）上。
client 和 server 通过这种格式互发消息：

```
Content-Length: 175\r\n
\r\n
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{...}}
```

每条消息：

* HTTP 风格 header（必须包含 `Content-Length`）；
* 空行分隔；
* JSON 正文。

Server 启动时读 stdin、解析消息、处理、把响应写 stdout。日志走
stderr（不能污染 stdout 协议流）。

## 24.2 JSON-RPC 消息分三类

| 类别 | 方向 | 说明 |
|------|------|------|
| Request | client → server，需要 response | initialize、textDocument/hover、textDocument/definition |
| Notification | 单向，没有 response | textDocument/didOpen、textDocument/didChange |
| Response | server → client，回复 Request | 结果或错误 |

Server 也能反过来主动发 Notification（如 `textDocument/publishDiagnostics`）
告诉 client"这文件这几行有错"。

## 24.3 最小 server 骨架

```cpp
// src/lsp/server.h
class LspServer {
public:
    int run(std::istream& in, std::ostream& out);

private:
    std::unordered_map<std::string, std::string> open_docs_;  // uri → text

    json handle_request(const std::string& method, const json& params);
    void handle_notification(const std::string& method, const json& params);

    // handlers
    json on_initialize(const json& params);
    json on_hover(const json& params);
    json on_definition(const json& params);
    json on_completion(const json& params);

    void on_did_open(const json& params);
    void on_did_change(const json& params);
    void on_did_close(const json& params);

    void publish_diagnostics(const std::string& uri);

    void send(const json& msg);

    std::ostream* out_ = nullptr;
};
```

JSON 用 [nlohmann/json](https://github.com/nlohmann/json) 这个
header-only 库——加 `#include <nlohmann/json.hpp>` 就能用，没有
其他依赖。

## 24.4 主循环：消息派发

```cpp
int LspServer::run(std::istream& in, std::ostream& out) {
    out_ = &out;
    while (true) {
        // 1. 读 header
        int content_length = 0;
        std::string line;
        while (std::getline(in, line)) {
            if (line == "\r" || line.empty()) break;
            if (line.rfind("Content-Length: ", 0) == 0) {
                content_length = std::stoi(line.substr(16));
            }
        }
        if (content_length == 0) return 0;  // EOF

        // 2. 读 body
        std::string body(content_length, '\0');
        in.read(&body[0], content_length);

        // 3. parse JSON
        json msg = json::parse(body);
        std::string method = msg.value("method", "");

        if (msg.contains("id")) {
            // request
            json response;
            response["jsonrpc"] = "2.0";
            response["id"] = msg["id"];
            try {
                response["result"] = handle_request(method, msg.value("params", json{}));
            } catch (const std::exception& e) {
                response["error"] = {{"code", -32603}, {"message", e.what()}};
            }
            send(response);
        } else if (!method.empty()) {
            // notification
            handle_notification(method, msg.value("params", json{}));
        }

        if (method == "exit") return 0;
    }
}

void LspServer::send(const json& msg) {
    std::string body = msg.dump();
    *out_ << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out_->flush();
}
```

整个 LSP server 的"骨架"就这 30 行。所有的智能都在 handlers
里。

## 24.5 initialize：宣告能力

client 启动 server 后第一条消息是 `initialize`，server 必须返
回自己**能做什么**：

```cpp
json LspServer::on_initialize(const json& params) {
    return json{
        {"capabilities", {
            {"textDocumentSync", {
                {"openClose", true},
                {"change", 1},  // 1 = full document, 2 = incremental
            }},
            {"hoverProvider", true},
            {"definitionProvider", true},
            {"completionProvider", {
                {"triggerCharacters", json::array({".", ":"})},
            }},
            {"diagnosticProvider", {
                {"interFileDependencies", false},
                {"workspaceDiagnostics", false},
            }},
        }},
        {"serverInfo", {
            {"name", "mini-lsp"},
            {"version", "0.1.0"},
        }},
    };
}
```

声明了：

* 完整文档同步（每次 didChange 发整篇）；
* 提供 hover、goto-def、补全；
* 主动 publish diagnostics。

完整文档同步实现简单但大文件抖；增量同步要解析 `range + text`
patch 局部应用——MVP 阶段不值得。

## 24.6 didOpen / didChange：缓存源码并跑诊断

```cpp
void LspServer::on_did_open(const json& params) {
    auto& doc = params["textDocument"];
    std::string uri = doc["uri"];
    open_docs_[uri] = doc["text"];
    publish_diagnostics(uri);
}

void LspServer::on_did_change(const json& params) {
    auto& doc = params["textDocument"];
    std::string uri = doc["uri"];
    // full sync 模式：取最后一个 contentChanges
    auto changes = params["contentChanges"];
    if (!changes.empty()) {
        open_docs_[uri] = changes.back()["text"];
    }
    publish_diagnostics(uri);
}

void LspServer::publish_diagnostics(const std::string& uri) {
    const std::string& source = open_docs_[uri];

    auto compile_result = compile(source);

    json diags = json::array();
    for (auto& d : compile_result.diagnostics) {
        diags.push_back({
            {"range", {
                {"start", {{"line", d.primary_span.start_line - 1},
                           {"character", d.primary_span.start_col - 1}}},
                {"end",   {{"line", d.primary_span.end_line - 1},
                           {"character", d.primary_span.end_col - 1}}},
            }},
            {"severity", d.severity == Severity::Error ? 1 :
                         d.severity == Severity::Warning ? 2 : 3},
            {"code", d.code},
            {"source", "mini"},
            {"message", d.message},
        });
    }

    send(json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params", {
            {"uri", uri},
            {"diagnostics", diags},
        }},
    });
}
```

LSP 的行/列 **从 0 开始**——我们 Mini 内部从 1 开始，每次发出
要 `-1`。新手最容易掉这个坑：行号显示对了但都偏 1。

## 24.7 hover：把鼠标悬停做成"看类型/文档"

```cpp
json LspServer::on_hover(const json& params) {
    std::string uri = params["textDocument"]["uri"];
    int line = (int)params["position"]["line"] + 1;     // 转回 1-based
    int col  = (int)params["position"]["character"] + 1;

    const std::string& source = open_docs_[uri];
    auto compile_result = compile(source);
    if (!compile_result.proto) return nullptr;

    // 在 AST/symbol table 里找 (line, col) 处的标识符
    auto symbol = lookup_symbol(*compile_result.proto, line, col);
    if (!symbol) return nullptr;

    std::string md;
    md += "```mini\n";
    md += symbol->signature();      // "fn fib(n)" / "let x: int" 等
    md += "\n```\n";
    if (!symbol->doc.empty()) md += "\n" + symbol->doc;

    return json{
        {"contents", {
            {"kind", "markdown"},
            {"value", md},
        }},
    };
}
```

`lookup_symbol(proto, line, col)` 是一个工具函数：扫 AST/symbol
table 找包含 `(line, col)` 位置的最小 identifier 节点。这要求
我们在 compile 阶段保留一份"symbol → declared-at + type +
doc"的索引——大多数 LSP 服务器实现都需要它。

GDScript 在 `extend_gdscript_parser.cpp` 里维护 `members`、
`symbols` 这种结构，专门给 LSP 用。Mini 教学版可以从最简单的
"identifier → 第一个 LetStmt 位置"做起。

## 24.8 definition：跳转到定义

```cpp
json LspServer::on_definition(const json& params) {
    std::string uri = params["textDocument"]["uri"];
    int line = (int)params["position"]["line"] + 1;
    int col  = (int)params["position"]["character"] + 1;

    auto symbol = lookup_symbol_at(uri, line, col);
    if (!symbol || symbol->declaration_uri.empty()) return nullptr;

    return json{
        {"uri", symbol->declaration_uri},
        {"range", {
            {"start", {{"line", symbol->declaration_line - 1},
                       {"character", symbol->declaration_col - 1}}},
            {"end",   {{"line", symbol->declaration_line - 1},
                       {"character", symbol->declaration_col - 1 + (int)symbol->name.size()}}},
        }},
    };
}
```

只要 symbol table 记录了"在哪个文件哪一行声明的"，这个 handler
是 5 行 JSON 装配。

## 24.9 completion：基础补全

```cpp
json LspServer::on_completion(const json& params) {
    std::string uri = params["textDocument"]["uri"];
    int line = (int)params["position"]["line"] + 1;
    int col  = (int)params["position"]["character"] + 1;

    json items = json::array();

    // 1. 当前 scope 可见 identifier
    auto candidates = collect_visible_identifiers_at(uri, line, col);
    for (auto& sym : candidates) {
        items.push_back({
            {"label", sym.name},
            {"kind", sym.kind == SymbolKind::Function ? 3 :     // Function
                     sym.kind == SymbolKind::Variable ? 6 :     // Variable
                                                        14},    // Keyword
            {"detail", sym.signature()},
            {"documentation", sym.doc},
        });
    }

    // 2. 关键字
    for (auto kw : {"if", "else", "elif", "end", "while", "for",
                    "let", "fn", "return", "true", "false", "nil"}) {
        items.push_back({{"label", kw}, {"kind", 14}});
    }

    return json{{"isIncomplete", false}, {"items", items}};
}
```

更精致的版本会区分 `obj.` 后只补 `obj` 的字段、函数调用括号内
补 parameter name——但需要更深的语境分析。Mini 的 MVP 做"全
scope identifier + 关键字"已经能覆盖 80% 场景。

## 24.10 VSCode extension

只需要一个 `package.json` 和一个 `extension.ts`：

```json
// package.json
{
  "name": "mini-lang",
  "displayName": "Mini Language",
  "version": "0.0.1",
  "engines": { "vscode": "^1.75.0" },
  "activationEvents": ["onLanguage:mini"],
  "main": "./out/extension.js",
  "contributes": {
    "languages": [{
      "id": "mini",
      "extensions": [".mini"],
      "aliases": ["Mini"]
    }],
    "grammars": [{
      "language": "mini",
      "scopeName": "source.mini",
      "path": "./syntaxes/mini.tmLanguage.json"
    }]
  },
  "dependencies": {
    "vscode-languageclient": "^9.0.1"
  }
}
```

```typescript
// src/extension.ts
import * as vscode from 'vscode';
import { LanguageClient, ServerOptions, TransportKind }
    from 'vscode-languageclient/node';

let client: LanguageClient;

export function activate(context: vscode.ExtensionContext) {
    const serverOptions: ServerOptions = {
        command: 'mini-lsp',         // 你编译出的 LSP server 二进制
        args: [],
        transport: TransportKind.stdio,
    };
    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'mini' }],
    };
    client = new LanguageClient('mini-lsp', 'Mini Language Server',
        serverOptions, clientOptions);
    client.start();
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}
```

`vscode-languageclient` 自动处理 stdio 启动、消息发送、断线重
连。你的 C++ server 只管 read/write，不需要懂 VSCode 内部。

## 24.11 调试 LSP server

LSP 跑在 stdio 上，没法用 VSCode 的"启动调试器附加"——server
进程已经被 client 拉起。两个常用技巧：

* **`MINI_LSP_LOG=1` 环境变量** → server 把所有收到的请求 dump
  到 `/tmp/mini-lsp.log`，对照看哪条没处理；
* **Trace 协议层** → VSCode `settings.json` 加
  `"mini.trace.server": "verbose"`，VSCode 输出面板会显示每一
  条 JSON-RPC 消息——立刻看出 client 期望什么、server 给了什么。

## 24.12 第 24 章小结

* LSP 是 JSON-RPC over stdio，header-body 协议；
* 三类消息：Request（要回复）、Notification（不回复）、Response；
* Server 在 `initialize` 宣告 capabilities，client 据此发后续请
  求；
* `textDocument/publishDiagnostics` 是 server → client 的主动通
  知，每次 didChange 重新跑 compiler 把诊断推过去；
* hover / definition / completion 都依赖一份**编译期建立的 symbol
  table**——AST 节点要能反向查"位置 → 标识符"；
* VSCode extension 用 `vscode-languageclient` 包，C++ server 只
  写 stdio。

写完这一章 Mini 就有了"现代脚本语言基本工程能力"：

* 编译器、运行时、GC ✓
* 反汇编、调试 trace、错误恢复 ✓
* IDE 集成 ✓

剩下两章把性能与扩展方向收个尾。

下一章 **第 25 章 性能：你能做的几件小事**——内联缓存、字符串
intern、栈预分配。
