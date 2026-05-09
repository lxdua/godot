# 第 18 章　脚本资源加载：`ResourceFormatLoaderGDScript`

到目前为止我们一直默认 `Ref<GDScript>` 已经在手——但它是怎么从磁盘
上的一个 `res://foo.gd` 变成内存里的对象的？这条路径看似简单，实际
牵涉到 Godot 的资源系统、二进制 token 缓存、依赖收集，以及与
`GDScriptCache` 的紧密协作。本章拆解“**一行 `load("res://foo.gd")`
背后发生了什么**”。

我们关注的问题：

1. GDScript 怎么注册成 Godot 资源系统能识别的格式？
2. 普通 `.gd` 与编译后的 `.gdc` 在加载时如何分流？
3. 为什么 `ResourceFormatLoaderGDScript::load()` 只有寥寥几行？真正
   的工作在哪里？
4. 引擎的依赖系统（编辑器“Find references”、构建依赖图）需要 GDScript
   提供什么？`get_dependencies` / `get_classes_used` 是怎么做到不真
   正执行脚本就枚举出依赖的？
5. `ResourceFormatSaverGDScript` 在保存时为什么要触发 `reload_tool_script`？

涉及的核心文件：

* `modules/gdscript/gdscript_resource_format.h/.cpp`
* `modules/gdscript/register_types.cpp`：注册到 ResourceLoader/Saver
* `modules/gdscript/gdscript_cache.cpp`：真正的“脏活”都在这里

---

## 18.1 注册：让 ResourceLoader 看见 `.gd` 与 `.gdc`

`register_types.cpp` 中初始化阶段会把两个 ResourceFormat 单例注册
到全局表：

```cpp
resource_loader_gd.instantiate();
ResourceLoader::add_resource_format_loader(resource_loader_gd);

resource_saver_gd.instantiate();
ResourceSaver::add_resource_format_saver(resource_saver_gd);
```

`ResourceLoader` 在 `load()` 时会按注册顺序询问每个
`ResourceFormatLoader::handles_type` / `get_resource_type` /
`get_recognized_extensions`——只要一家声称“认识”，就交给它处理。

GDScript 的 Loader 这样回答：

```cpp
void ResourceFormatLoaderGDScript::get_recognized_extensions(List<String> *p_extensions) const {
    p_extensions->push_back("gd");
    p_extensions->push_back("gdc");
}

bool ResourceFormatLoaderGDScript::handles_type(const String &p_type) const {
    return (p_type == "Script" || p_type == "GDScript");
}

String ResourceFormatLoaderGDScript::get_resource_type(const String &p_path) const {
    String el = p_path.get_extension().to_lower();
    if (el == "gd" || el == "gdc") return "GDScript";
    return "";
}
```

要点：

* **两种扩展名**：`.gd`（源码）与 `.gdc`（二进制 token）。整个 Loader
  对它们使用同一条入口，由下游的 `GDScriptCache` 根据后缀分流。
* **`handles_type("Script")`**：返回 `true` 是为了让代码里写
  `ResourceLoader::load(path, "Script")` 也能命中——脚本作为抽象
  Resource 类型的别名。
* **`get_resource_type`** 在不读文件内容的情况下、纯靠扩展名给出资源
  类型，是 `EditorFileSystem` 扫描项目时必走的一步。

---

## 18.2 `load()`：一个 4 行的转发函数

```cpp
Ref<Resource> ResourceFormatLoaderGDScript::load(const String &p_path,
        const String &p_original_path, Error *r_error,
        bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
    Error err;
    bool ignoring = p_cache_mode == CACHE_MODE_IGNORE
                 || p_cache_mode == CACHE_MODE_IGNORE_DEEP;
    Ref<GDScript> scr = GDScriptCache::get_full_script(p_original_path, err, "", ignoring);

    if (err && scr.is_valid()) {
        ERR_PRINT_ED(vformat(R"(Failed to load script "%s" with error "%s".)",
                             p_original_path, error_names[err]));
    }
    if (r_error) {
        // Don't fail loading because of parsing error.
        *r_error = scr.is_valid() ? OK : err;
    }
    return scr;
}
```

`load()` 本身没有任何解析、编译、缓存逻辑——它把所有重活都委托给
`GDScriptCache::get_full_script`。这种设计带来三个好处：

1. **统一入口**：通过 `ResourceLoader::load` 和直接调用
   `GDScriptCache::get_full_script` 得到的 `Ref<GDScript>` **是同一个
   对象**，避免“同一脚本两份对象”的灾难。
2. **复用循环依赖处理**：`GDScriptCache` 内部的浅/全双层缓存
   （第 19 章）天然能解决 `A.gd` preload `B.gd`，`B.gd` 又 preload
   `A.gd` 的循环——`load()` 端不需要重复实现。
3. **CacheMode 透传**：`CACHE_MODE_IGNORE*` 让用户能强制重新读取磁盘
   （编辑器的“revert”就是用它）。

注意一个微妙的细节：**“即使有错误，仍然返回 script 实例”**。这是为
了：

* 避免 `r_error` 把上层 `ResourceLoader` 整个调用链失败掉；
* 使编辑器仍能打开有语法错误的脚本进行修复（脚本对象是有效的，只
  是 `valid == false`）。

---

## 18.3 `.gd` 与 `.gdc` 的分流

二进制 token 文件 `.gdc` 是 Godot 在导出项目时产生的——把 Tokenizer
的结果直接序列化到磁盘，省去运行期再做一次词法分析。两者在缓存层
的具体分流发生在 `get_shallow_script`：

```cpp
script.instantiate();
script->set_path_cache(p_path);
if (remapped_path.has_extension("gdc")) {
    Vector<uint8_t> buffer = get_binary_tokens(remapped_path);
    if (buffer.is_empty()) r_error = ERR_FILE_CANT_READ;
    script->set_binary_tokens_source(buffer);
} else {
    r_error = script->load_source_code(remapped_path);
}
```

* `.gdc`：读为字节缓冲并塞进 `GDScript::binary_tokens`；
  Tokenizer 阶段会用 `GDScriptTokenizerBuffer`（第 2 章）反序列化。
* `.gd`：调 `GDScript::load_source_code(path)`，把文件读为 UTF-8
  字符串放进 `GDScript::source`。

往后所有“parse → analyze → compile”的流程对两者**完全统一**——这是
为什么 `.gdc` 能无缝替换 `.gd`。在 release 构建中，`.gd` 文件根本
不会进入安装包；编辑器导出时会自动转成 `.gdc`，文件名仍是
`res://*.gd`，但 `path_remap` 会把它指到 `.gdc`。

---

## 18.4 `path_remap`：导出时的隐形转换

注意 `load()` 与 `get_shallow_script` 中频繁出现：

```cpp
const String remapped_path = ResourceLoader::path_remap(p_path);
```

这是 Godot 的“路径重定向”机制。它解决：

* 导出后 `.gd` 被替换为 `.gdc`，但脚本中的 `preload("res://foo.gd")`
  字符串没变；
* 多语言资源（图片、声音）在不同地区使用不同文件；
* 修补包（patch）覆盖原始资源。

`path_remap` 接受“原始路径”，按当前运行环境配置（`*.translation`
表 / 导出转换表）返回“真实物理路径”。GDScript Loader **始终用这个
重定向后的路径去读文件**，但**始终用原始路径作为 `set_path_cache`
的 key**——这样上层缓存看到的还是用户写的字符串，没有歧义。

---

## 18.5 `get_dependencies`：不执行就要枚举依赖

引擎的依赖图（编辑器“Show in dependencies”、构建系统的“增量打包”）
需要知道：**这个脚本依赖哪些其它资源？** GDScript 必须能在不真正
**执行**脚本的前提下回答这个问题：

```cpp
void ResourceFormatLoaderGDScript::get_dependencies(const String &p_path,
        List<String> *p_dependencies, bool p_add_types) {
    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
    String source = file->get_as_utf8_string();
    if (source.is_empty()) return;

    GDScriptParser parser;
    if (OK != parser.parse(source, p_path, false)) return;

    for (const String &E : parser.get_dependencies()) {
        p_dependencies->push_back(E);
    }
}
```

要点：

* **只跑到 Parser 这一步**——不分析、不编译、不实例化；
* **`parser.parse(..., false)`** 第三个参数 `for_completion = false`，
  让 Parser 走正常分支，但不需要构造完整的 AST 类型注解；
* **`parser.get_dependencies()`** 返回 Parser 在解析过程中累积的所
  有 `preload(...)` 路径与 `extends "..."` 路径——这两类是 GDScript
  里**字面静态写出**的依赖。

GDScript 的依赖收集**严格只覆盖编译期可见的字符串**。运行时通过变
量传入的 `load(some_var_path)` 永远不会出现在依赖列表中——这是与
Godot 整个静态依赖图模型一致的取舍：动态依赖只能由用户自己负责打
包。

---

## 18.6 `get_classes_used`：词法级反向索引

这个函数用于编辑器的“Find references”——给定一个脚本，问“它都用
到了哪些引擎类？”。这个能力不能用编译产物回答，因为编译产物已经把
`Node` 这种名字解析成了具体类型；要回答语言级别的“用到哪些类名”
必须回到源码。

```cpp
void ResourceFormatLoaderGDScript::get_classes_used(const String &p_path,
        HashSet<StringName> *r_classes) {
    Ref<GDScript> scr = ResourceLoader::load(p_path);
    if (scr.is_null()) return;

    const String source = scr->get_source_code();
    GDScriptTokenizerText tokenizer;
    tokenizer.set_source_code(source);
    GDScriptTokenizer::Token current = tokenizer.scan();
    while (current.type != GDScriptTokenizer::Token::TK_EOF) {
        if (!current.is_identifier()) {
            current = tokenizer.scan();
            continue;
        }
        // ... 在 token 位置插入光标，调用 lookup_code(...) 解析符号 ...
        ScriptLanguage::LookupResult result;
        if (scr->get_language()->lookup_code(source_with_cursor,
                current.get_identifier(), p_path, nullptr, result) == OK) {
            if (!result.class_name.is_empty() &&
                ClassDB::class_exists(result.class_name)) {
                r_classes->insert(result.class_name);
            }
            // 进一步收集属性/方法返回类型涉及的类
            // ...
        }
        current = tokenizer.scan();
    }
}
```

设计思路：

1. **逐 token 扫描**——只关心标识符；
2. 对每个标识符**伪造一个光标位置**，复用 `ScriptLanguage::lookup_code`
   （编辑器代码补全那一套）来解析它的语义；
3. 把解析结果中提到的所有引擎类名收集起来。

这个实现复用了 LSP 路径的现成能力，避免重新实现一套“源码 → 类名”
的解析。它的代价是：每个 token 都要走一次 `lookup_code`（包含完整
的 parse + analyze），所以这是个**慢操作**——只在编辑器请求依赖图
时调用，不在运行时路径上。

---

## 18.7 `save()`：源码即真相 + 自动重载

`ResourceFormatSaverGDScript` 同样精简：

```cpp
Error ResourceFormatSaverGDScript::save(const Ref<Resource> &p_resource,
        const String &p_path, uint32_t p_flags) {
    Ref<GDScript> sqscr = p_resource;
    String source = sqscr->get_source_code();

    Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &err);
    file->store_string(source);
    // ...

    if (ScriptServer::is_reload_scripts_on_save_enabled()) {
        GDScriptLanguage::get_singleton()->reload_tool_script(p_resource, true);
    }
    return OK;
}
```

两条关键决策：

### 18.7.1 “源码就是真相”

GDScript 在保存时只把 `source` 字符串原样写回——不会写出字节码、不
会规范化空白、不会重排成员。这与 Godot 把 `.gd` 视为 “human source”
的定位一致：用户编辑的就是这个文件，引擎不应擅自改动。

字节码 `.gdc` 不在 `save()` 路径上产生——它由“项目导出”流程批量生
成，与编辑期保存解耦。

### 18.7.2 保存时触发 `@tool` 脚本热重载

```cpp
if (ScriptServer::is_reload_scripts_on_save_enabled()) {
    GDScriptLanguage::get_singleton()->reload_tool_script(p_resource, true);
}
```

如果脚本带 `@tool` 注解（即在编辑器中也运行），每次保存都立刻替换
内存中的版本——让用户在编辑器里立刻看到改动效果。`true` 参数表示
“soft reload”——尽量保留实例状态。

非 `@tool` 脚本不需要这一步——它们只在游戏运行时才被加载，编辑器
里本来就没有运行实例。

第 23 章会展开 reload 的具体机制，这里只关注**触发点**位于 save
路径上。

---

## 18.8 一次完整的 `load("res://enemy.gd")`

让我们把所有部件串起来，看一次完整的加载：

```
用户代码:
  var script = load("res://enemy.gd")

ResourceLoader::load("res://enemy.gd", "")
└── 查 ResourceCache → miss
└── 找到 ResourceFormatLoaderGDScript（handles_type / extension 命中）
└── ResourceFormatLoaderGDScript::load("res://enemy.gd", ...)
     └── GDScriptCache::get_full_script("res://enemy.gd", err, "", ignoring=false)
          ├── 查 full_gdscript_cache → miss
          ├── get_shallow_script("res://enemy.gd")
          │    ├── 查 shallow_gdscript_cache → miss
          │    ├── path_remap → "res://enemy.gd"（开发期不重定向）
          │    ├── memnew GDScript()
          │    ├── set_path_cache("res://enemy.gd")
          │    ├── load_source_code(path)        ← 读 UTF-8 文件
          │    ├── get_parser(path, PARSED, ...) ← 走到 Parsed 状态
          │    └── GDScriptCompiler::make_scripts(...) ← 仅创建空壳子脚本
          ├── script->reload(true)               ← 触发 analyze + compile
          ├── full_gdscript_cache[path] = script
          ├── shallow_gdscript_cache.erase(path)
          └── set_path(path, true)               ← 加入 ResourceCache
└── 返回 Ref<GDScript>

用户代码:
  var instance = script.new()                   ← 走第 15 章的 _create_instance
```

每一步的失败都不会让整个 `load()` 抛错——而是把脚本对象（可能
`valid == false`）原样返回，让用户/编辑器有机会介入修复。

---

## 18.9 设计回顾

GDScript 的资源加载模块遵循三条原则：

1. **薄 Loader / 厚 Cache**：`ResourceFormatLoaderGDScript` 只是一
   层壳子，所有真正的工作（解析、编译、缓存、循环依赖处理）都收敛
   到 `GDScriptCache`。这避免了 `load()` 路径与 `preload()` 路径走
   两套不同代码。
2. **统一原始路径，重定向到物理路径**：`path_remap` 在内部使用，对
   外仍以 `res://*.gd` 为唯一标识——这是导出/补丁/本地化对脚本透明
   的关键。
3. **失败容忍，错误延迟**：解析或编译失败不阻断 `load()`，让对象
   仍能被编辑器/调试器获取。这与 Godot 的“编辑器优先体验”理念一
   致——再差的脚本也要能被打开。

依赖系统的两个查询函数（`get_dependencies` / `get_classes_used`）
则体现了 GDScript 在工具链协作中的角色：**前者只跑到 Parser，后者
复用 LSP 路径**——不为了效率重新实现一套，也不为了功能而牺牲性能
分层。

---

## 小结

* GDScript 通过 `ResourceFormatLoaderGDScript` / `ResourceFormatSaverGDScript`
  接入 Godot 的资源系统，识别 `.gd` 与 `.gdc` 两种扩展名；
* `load()` 只是个委托——真正的加载、解析、编译都在
  `GDScriptCache::get_full_script` 中完成；
* `.gd` / `.gdc` 在 `get_shallow_script` 处分流，往后路径完全一致；
* `path_remap` 让导出时的 `.gd → .gdc` 替换对上层透明；
* `get_dependencies` 通过仅跑到 Parser 的 `parser.get_dependencies()`
  实现静态依赖收集；
* `get_classes_used` 利用 Tokenizer + `lookup_code` 复用 LSP 能力枚
  举源码中用到的引擎类；
* `save()` 写回原始 source 字符串，并在 `@tool` 脚本场景触发热重
  载；
* 加载失败时仍返回脚本对象，把诊断与修复留给上层，不打断 `load()`
  调用链。

下一章我们正式拆解 `GDScriptCache` 这个“厚后端”——它是怎么用浅/全
双层缓存破解 GDScript 类间循环依赖的。
