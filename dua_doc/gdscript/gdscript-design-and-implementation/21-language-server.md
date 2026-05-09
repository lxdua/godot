# 第 21 章　语言服务器（LSP）：`language_server/`

第 20 章把编辑器内的服务讲完了，但 GDScript 还要服务**编辑器之外**
的客户端：VSCode、Neovim、Sublime 这些通用编辑器希望以标准 LSP 协
议消费 GDScript 的能力。`modules/gdscript/language_server/` 就是这
个适配层。

它的核心立意非常清晰：**“不重写一遍补全/跳转/校验，把它们包装成
JSON-RPC”**。这一章拆解：

1. LSP 进程在 Godot 编辑器中的位置——它不是独立 daemon，而是编辑器
   的 `EditorPlugin`；
2. 三个核心对象 `GDScriptLanguageProtocol` / `GDScriptTextDocument`
   / `GDScriptWorkspace` 各自的职责；
3. `ExtendGDScriptParser` 如何从 `GDScriptParser` 派生出 LSP 友好
   的符号树与诊断；
4. LSP 的请求是如何最终落到第 20 章那些 `complete_code` / `lookup_code`
   函数上的；
5. `SceneCache` 这种“看起来不属于语言服务器”的子模块为什么会出现
   在这里。

涉及的核心文件：

* `language_server/gdscript_language_server.{h,cpp}`：作为
  `EditorPlugin` 的入口
* `language_server/gdscript_language_protocol.{h,cpp}`：JSON-RPC
  / TCP 连接管理 + 方法注册
* `language_server/gdscript_text_document.{h,cpp}`：每文件级 LSP
  方法的接收端
* `language_server/gdscript_workspace.{h,cpp}`：工程级状态、符号
  索引、跨文件操作
* `language_server/gdscript_extend_parser.{h,cpp}`：把 GDScript AST
  翻译成 LSP `DocumentSymbol`
* `language_server/godot_lsp.h`：本地的 LSP 数据结构（避免依赖外部
  库）
* `language_server/scene_cache.{h,cpp}`：节点路径补全用的场景缓存

---

## 21.1 进程模型：插在编辑器里的 TCP 服务器

GDScript LSP **不是**独立进程。它通过 `GDScriptLanguageServer`（一
个 `EditorPlugin`）在编辑器启动时创建一个 TCP 服务器，监听 `127.0.0.1`
某端口（默认 6005，可配置）。客户端（VSCode 插件等）连上来发 LSP
请求，服务器在编辑器进程内跑 GDScript 自己的 Parser/Analyzer 给出
答复。

```cpp
class GDScriptLanguageServer : public EditorPlugin {
    // 维护一个 GDScriptLanguageProtocol 实例 + 一个监听 thread
};
```

注册位置：

```cpp
GDREGISTER_CLASS(GDScriptLanguageProtocol);
GDREGISTER_CLASS(GDScriptTextDocument);
GDREGISTER_CLASS(GDScriptWorkspace);
```

三个核心 RefCounted 类都被注册到 `ClassDB`——这样 LSP 协议方法可
以直接通过 `ClassDB::bind_method` 暴露成 RPC 端点（见 21.3）。

为什么这种**“宿主进程内的 LSP”** 设计？

* 编辑器本身就是 GDScript 的“住处”——已经持有 `GDScriptCache`、
  `EditorFileSystem`、加载好的资源，直接复用零成本；
* 跨进程把 AST/符号信息序列化反序列化的开销很大，对于大型项目无意
  义；
* 编辑器里的更改与 LSP 客户端的更改可以无锁同步（同进程同 mutex）。

代价是：**编辑器没开就没 LSP**。Godot 项目的“启动 LSP”等同于“启
动编辑器”。VSCode 插件做的其实是**自动启动一个无窗口的 godot 编辑
器**，再连上它的 LSP 端口。

---

## 21.2 三件套：Protocol / TextDocument / Workspace

LSP 规范把所有方法分成两个命名空间——`textDocument/*` 与
`workspace/*`。GDScript 严格按照这个划分，把代码组织成两个对象 +
一个总线：

```
                       ┌───────────────────────────────┐
                       │ GDScriptLanguageProtocol       │
                       │  (JSONRPC + TCPServer)         │
                       │  · 方法分发                     │
                       │  · 客户端连接管理                │
                       │  · 通知发送                     │
                       └────────┬───────────────┬──────┘
                                │               │
                  textDocument/*│               │workspace/*
                                ▼               ▼
                ┌──────────────────────┐  ┌────────────────────┐
                │ GDScriptTextDocument │  │ GDScriptWorkspace  │
                │  · didOpen           │  │  · 跨文件符号索引   │
                │  · completion        │  │  · 全局重命名       │
                │  · hover             │  │  · scene/api        │
                │  · definition        │  │  · 诊断发布          │
                │  · rename            │  │                    │
                └──────────────────────┘  └────────────────────┘
```

### 21.2.1 `GDScriptLanguageProtocol`：底层管道

```cpp
class GDScriptLanguageProtocol : public JSONRPC {
    static GDScriptLanguageProtocol *singleton;
    Ref<TCPServer> server;
    HashMap<int, Client> clients;       // peer_id → 连接

    Ref<GDScriptTextDocument> text_document;
    Ref<GDScriptWorkspace>    workspace;
    // ...
};
```

继承自 `JSONRPC`（Godot 自带的通用 JSON-RPC 框架），自动处理：

* JSON 报文的解析与组包；
* `id/method/params` 的字段路由；
* 错误码的封装。

GDScript LSP 在 `_bind_methods()` 中把上面那些方法暴露给 JSONRPC：

```cpp
ClassDB::bind_method(D_METHOD("on_client_connected"),
                     &GDScriptLanguageProtocol::on_client_connected);
```

这样客户端发来 `{"method": "on_client_connected", ...}` 时，JSONRPC
基类会自动调到 `GDScriptLanguageProtocol::on_client_connected`。
`GDScriptTextDocument` 与 `GDScriptWorkspace` 的方法注册同理。

`notify_client(method, params, client_id)` 是反方向的“服务端推送”
——用于发送 `textDocument/publishDiagnostics`、`window/showMessage`
等通知。

### 21.2.2 `GDScriptTextDocument`：单文件级 LSP

```cpp
class GDScriptTextDocument : public RefCounted {
public:
    void didOpen(const Variant &p_param);
    void didClose(const Variant &p_param);
    void didChange(const Variant &p_param);
    void willSaveWaitUntil(const Variant &p_param);
    void didSave(const Variant &p_param);

    void reload_script(Ref<GDScript> p_to_reload_script);
    void show_native_symbol_in_editor(const String &p_symbol_id);

    Variant   nativeSymbol(const Dictionary &p_params);
    Array     documentSymbol(const Dictionary &p_params);
    Array     documentHighlight(const Dictionary &p_params);
    Array     completion(const Dictionary &p_params);
    Dictionary resolve(const Dictionary &p_params);
    Dictionary rename(const Dictionary &p_params);
    Variant   prepareRename(const Dictionary &p_params);
    Array     references(const Dictionary &p_params);
    Array     foldingRange(const Dictionary &p_params);
    Array     codeLens(const Dictionary &p_params);
    Array     documentLink(const Dictionary &p_params);
    Array     colorPresentation(const Dictionary &p_params);
    Variant   hover(const Dictionary &p_params);
    Array     definition(const Dictionary &p_params);
    Variant   declaration(const Dictionary &p_params);
    Variant   signatureHelp(const Dictionary &p_params);
};
```

每个方法对应 LSP 规范里的一个标准请求。它们大多很短——把入参翻译
成 GDScript 内部数据结构，调到 `GDScriptWorkspace` 里的某个查询函
数，再把结果包回 LSP `Dictionary`/`Array`。

### 21.2.3 `GDScriptWorkspace`：工程级状态

```cpp
class GDScriptWorkspace : public RefCounted {
public:
    String root;              // 项目根
    String root_uri;          // file:// 形式

    HashMap<StringName, ClassMembers> native_members;     // 引擎类成员索引

    Error initialize();
    void  publish_diagnostics(const String &p_path);
    void  completion(const LSP::CompletionParams &p_params,
                     List<ScriptLanguage::CodeCompletionOption> *r_options);

    const LSP::DocumentSymbol *resolve_symbol(
        const LSP::TextDocumentPositionParams &p_doc_pos,
        const String &p_symbol_name = "", bool p_func_required = false);
    const LSP::DocumentSymbol *resolve_native_symbol(
        const LSP::NativeSymbolInspectParams &p_params);
    void resolve_document_links(const String &p_uri,
                                List<LSP::DocumentLink> &r_list);
    Dictionary generate_script_api(const String &p_path);
    Error      resolve_signature(const LSP::TextDocumentPositionParams &p_doc_pos,
                                 LSP::SignatureHelp &r_signature);
    Dictionary rename(const LSP::TextDocumentPositionParams &p_doc_pos,
                      const String &new_name);
    bool can_rename(const LSP::TextDocumentPositionParams &p_doc_pos,
                    LSP::DocumentSymbol &r_symbol, LSP::Range &r_range);
    Vector<LSP::Location> find_usages_in_file(const LSP::DocumentSymbol &p_symbol,
                                              const String &p_file_path);
    Vector<LSP::Location> find_all_usages(const LSP::DocumentSymbol &p_symbol);
};
```

它的两大职责：

1. **维护工程级符号索引**：`native_symbols` / `native_members` 缓存
   引擎所有类的成员，启动时通过遍历 ClassDB 一次性建立——LSP 客户
   端 hover 一个 `Vector2.length` 时，能 O(1) 查到。
2. **跨文件协作**：rename 操作要修改所有引用方，find references 要
   遍历整个项目；它们都通过 Workspace 调度。

`absolute_res_paths` 这个看似冗余的 HashSet 解决一个细节：客户端发
来的可能是绝对路径（`file:///c:/proj/foo.gd`），需要被映射回
`res://foo.gd`——`absolute_res_paths` 缓存已知映射，避免每次都查
ProjectSettings。

---

## 21.3 `ExtendGDScriptParser`：把 AST 转成 LSP 符号树

LSP 用 `DocumentSymbol` 表示符号树（用于大纲面板、面包屑导航等）。
`ExtendGDScriptParser` 通过**继承** `GDScriptParser`、在
`parse(...)` 之后再做一次遍历来生成这棵树：

```cpp
class ExtendGDScriptParser : public GDScriptParser {
    String              path;
    Vector<String>      lines;

    LSP::DocumentSymbol     class_symbol;
    Vector<LSP::Diagnostic> diagnostics;
    List<LSP::DocumentLink> document_links;
    ClassMembers            members;
    HashMap<String, ClassMembers> inner_classes;

    LSP::Range range_of_node(const Node *p_node) const;

    void update_diagnostics();
    void update_symbols();
    void update_document_links(const String &p_code);
    void parse_class_symbol(const ClassNode *p_class, LSP::DocumentSymbol &r_symbol);
    void parse_function_symbol(const FunctionNode *p_func, LSP::DocumentSymbol &r_symbol);
    // ...
public:
    void parse(const String &p_code, const String &p_path);
    // ... 各种 LSP 友好的查询接口 ...
};
```

为什么继承而不是组合？

* 直接获得 Parser 的所有内部访问权限——`get_tree()`、内部 AST 节点
  类型都不需要再暴露 public 接口；
* 复用 Parser 的 `clear()` / 错误收集机制；
* `parse(...)` 是个**新签名**（少一个 `for_completion` 参数），让 LSP
  调用更简洁。

`ExtendGDScriptParser::parse` 内部会先调基类 `GDScriptParser::parse`，
然后顺次执行 `update_symbols()` / `update_diagnostics()` /
`update_document_links()`——这三个方法把内部 AST 翻译成 LSP 数据结
构，并维护 `members` / `inner_classes` 这种平铺索引以加速后续查询。

### 21.3.1 `ClassMembers`：把树扁平化方便查找

```cpp
ClassMembers                         members;       // 当前文件类的成员
HashMap<String, ClassMembers>        inner_classes; // 内部类名 → 成员表
```

`ClassMembers` 通常是个 `HashMap<String, const DocumentSymbol *>`。
扁平化的目的是：用户通过 LSP 请求 `Foo.bar` 时，能 O(1) 拿到 `bar`
对应的 `DocumentSymbol`，而不用每次都遍历 AST。

### 21.3.2 `range_of_node`：行列定位

LSP 的所有符号位置都用 `{line, character}` 表示，但 GDScript Parser
只记录 `(start_line, start_column, end_line, end_column)`。
`range_of_node` 就是这个翻译适配器，把 1-based 列号转成 LSP 的
0-based。

### 21.3.3 双 ID 策略

注意 `ExtendGDScriptParser` 与 `GDScriptParserRef`（第 19 章）**不
是同一个东西**：

* `GDScriptParserRef` 用于编译流水线，按状态机推进；
* `ExtendGDScriptParser` 是 LSP 专用，每次 `didChange` 都重新跑一遍
  完整 parse（不维护增量）。

这两套各有一个 Parser 实例，**互不共享**——LSP 的修改只在 LSP 端
临时存在，不污染编译路径。直到用户保存文件，编译路径才看到改动
（通过 `didSave` 触发的 reload）。

---

## 21.4 一次完整的 LSP 请求：`textDocument/completion`

让我们追一次最常见的请求：

```
[VSCode] → JSON-RPC TCP 包：
  {"id":42,"method":"textDocument/completion",
   "params":{"textDocument":{"uri":"file:///c:/proj/enemy.gd"},
             "position":{"line":10,"character":4}}}

[Godot 编辑器进程]
GDScriptLanguageProtocol::JSONRPC 解析 method
└── 路由到 GDScriptTextDocument::completion
     ├── 参数转 LSP::CompletionParams（uri / position）
     ├── workspace->completion(params, &options)
     │    ├── uri → res://enemy.gd
     │    ├── 取出 ExtendGDScriptParser 实例（didChange 时已建好）
     │    ├── get_text_for_completion(cursor)  ← 在光标位置插入触发符
     │    └── GDScriptLanguage::complete_code(text, path, owner, &options, ...)
     │         └── 走第 20 章 _find_identifiers 那条多层查找
     └── 把 List<CodeCompletionOption> 翻译成 LSP CompletionList JSON
└── JSONRPC 回包给客户端
```

LSP 适配层在中间所做的事其实只有：

1. 协议格式转换（`Dictionary` ↔ `LSP::*` 结构 ↔ JSON）；
2. URI ↔ 文件系统路径；
3. 位置坐标 0-based ↔ GDScript 内部 1-based；
4. 触发符注入（用 `0xFFFF` 字符标记光标位置，让 Parser 识别）。

**核心引擎逻辑都在 `GDScriptLanguage::complete_code`——就是第 20 章
讲过的那个函数。** 这就是“LSP 是个适配层”这句话的具体体现。

---

## 21.5 诊断发布：`publishDiagnostics`

LSP 里编辑器不会主动“拉”错误，而是服务端主动“推”——`workspace`
在每次脚本被修改/保存时调：

```cpp
void publish_diagnostics(const String &p_path);
```

实现思路：

1. 在 `GDScriptCache` 中找到对应 `ExtendGDScriptParser`；
2. 取出它的 `diagnostics` 字段（在 `update_diagnostics()` 中已生成）；
3. 通过 `protocol->notify_client("textDocument/publishDiagnostics",
   {uri, diagnostics})` 推送给客户端。

`update_diagnostics` 把 GDScript 的 `ParserError` + `GDScriptWarning`
统一翻译成 LSP `Diagnostic`：

* `severity`：error / warning / information / hint
* `range`：错误的行列范围
* `message`：人类可读的错误描述
* `source`：固定为 "gdscript" 让客户端能区分错误来源

这种**主动推送**模型和编辑器内置的“validate 后立刻渲染”模型在最
终效果上是一致的，但走的是不同的协议路径。

---

## 21.6 `SceneCache`：为节点路径补全服务

`scene_cache.{h,cpp}` 看起来与“语言服务器”关系不大——它实际为
`$Node` / `%UniqueNode` / `get_node("...")` 这类**节点路径补全**服
务。

```cpp
class SceneCache {
    // 监听 EditorFileSystemDirectory，缓存所有 .tscn / .scn
    // 维护 节点路径 → 节点类型 的索引
    HashMap<String, Vector<String>> scene_node_paths;
    // ...
};
```

为什么需要它？因为 `$Player/Sprite2D` 这种引用要给出补全候选，必须
**预先解析所有场景文件**——不能每次补全都现去扫场景。SceneCache
监听 EditorFileSystem 的 `filesystem_changed` 信号，增量地维护这张
表。

把 SceneCache 放在 `language_server/` 而不是 `editor/`，是因为它的
**唯一消费者**就是 LSP（编辑器内置补全直接走另一条路径，能从内存
中正在编辑的 Scene Tree 拿数据；只有 LSP 必须依赖磁盘上的场景文件
做缓存）。

---

## 21.7 `generate_script_api`：导出脚本 API 文档

```cpp
Dictionary generate_script_api(const String &p_path);
```

这是一个 GDScript LSP 的**专属扩展**（不在标准 LSP 规范里），用于
生成机器可读的脚本 API 描述（信号、方法、属性、内部类）。它的输出
被一些 GDScript 文档工具消费，作为生成静态文档站点的中间格式。

实现走 `ExtendGDScriptParser::generate_api`，再次复用 AST 遍历，
不重新解析。这种“在 LSP 端口上加私有方法”的扩展模式 LSP 规范本身
是允许的——客户端只要识别这个方法名就能调。

---

## 21.8 设计回顾

GDScript LSP 实现的设计可总结为四点：

1. **薄适配层**：`text_document` / `workspace` 加起来不到 2000 行
   代码，几乎没有“真正的语言能力”——所有重活都委托回
   `GDScriptLanguage` / `GDScriptParser`。
2. **同进程，少序列化**：在编辑器进程内监听 TCP，避免跨进程序列化
   AST/符号；与编辑器内置编辑面板共用 `GDScriptCache`。
3. **`ExtendGDScriptParser` 派生模式**：通过继承 Parser 添加 LSP 视
   图，避免侵入 Parser 自身。Parser 仍是“纯编译器组件”，LSP 视图
   是“扩展”。
4. **JSONRPC 通用框架托底**：把 “TCP / JSON / 方法分发” 收敛到
   `core/io/json_rpc.h`，GDScript 模块只关注语义层。这种分层让其他
   脚本语言（C# / GDExtension）若要做 LSP，可以走同一条管道。

四点合起来的本质是：**“LSP 是个翻译，不是重写”**。这与第 20 章里
“编辑器集成是契约 + 复用”的结论一脉相承——只是这次把契约从“函数
表”换成了“JSON-RPC”。

---

## 小结

* GDScript LSP 不是独立 daemon，而是 `GDScriptLanguageServer`
  EditorPlugin 在编辑器进程内开 TCP 服务器；
* 三件套 `GDScriptLanguageProtocol` / `GDScriptTextDocument` /
  `GDScriptWorkspace` 分别承担 RPC 总线、文件级 LSP 方法、工程级
  状态；
* `ExtendGDScriptParser` 通过继承 `GDScriptParser` 增加 LSP 友好的
  符号树、诊断、document links 字段；
* 大多数 LSP 方法只做协议适配，最终调到 `GDScriptLanguage::*`（第
  20 章）的函数完成实际工作；
* `publishDiagnostics` 主动推送实现“即时错误反馈”；
* `SceneCache` 通过监听 EditorFileSystem 维护节点路径补全所需的索
  引；
* `generate_script_api` 是 LSP 私有扩展方法，输出机器可读的 API 描
  述供文档工具消费；
* 整套设计严格遵循“**薄适配 + 重复用**”——LSP 端只做协议翻译，语
  言能力不重写。

下一章我们将看 GDScript 的警告系统：`GDScriptWarning` 是如何在
Analyzer 中嵌入、`@warning_ignore` 如何抑制特定警告、警告与编辑器
工具链怎样配合呈现给用户。
