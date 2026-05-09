# 第 14 章　RPC 与 `GDScriptRPCCallable`

GDScript 用一个看似平淡的注解 `@rpc` 就能把一个普通函数“变成”可以跨
网络调用的远程过程调用（RPC）。但和 lambda 一样，背后牵动的模块远比
表面上多——不仅有 GDScript 自身的 Tokenizer/Parser/Analyzer/Compiler，
还要和 `modules/multiplayer` 中的 `SceneRPCInterface`、`Node::rpcp`、
`MultiplayerPeer` 协作。

本章把这条链路一次讲清楚：

1. `@rpc` 注解如何被 GDScript 解析、校验、转译成一个 `Dictionary`？
2. 为什么 `GDScriptFunction` 与 `GDScript` 上都各自存了一份
   `rpc_config`？
3. 在 `obj.method` 这种属性访问的代码路径里，GDScript 在哪个分支判定
   该走 `Callable(obj, method)` 还是 `GDScriptRPCCallable`？
4. `GDScriptRPCCallable` 与普通 `Callable` 的关键差异是什么？为什么
   它要专门继承 `CallableCustom` 重写一个 `rpc()`？
5. 一次 `node.rpc("foo", 1, 2)` 在 GDScript 端到 MultiplayerAPI 端的
   完整数据流是怎样的？

涉及的核心文件：

* `modules/gdscript/gdscript_parser.cpp`：`rpc_annotation` 与默认值
* `modules/gdscript/gdscript_compiler.cpp`：把 `rpc_config` 写入
  `GDScript` 与 `GDScriptFunction`
* `modules/gdscript/gdscript_byte_codegen.cpp`：`write_start` 中的
  `rpc_config` 字段绑定
* `modules/gdscript/gdscript.cpp`：`_get` / `_get_member` 路径里
  `GDScriptRPCCallable` 的构造点
* `modules/gdscript/gdscript_rpc_callable.{h,cpp}`：本章主角
* `modules/multiplayer/scene_rpc_interface.cpp`：另一端的接收/分派
* `scene/main/node.cpp`：`Node::rpcp` 桥梁

---

## 14.1 RPC 在 Godot 网络模型中的位置

Godot 4 把所有“跨节点同步”都收敛到 `MultiplayerAPI` 抽象上。其默认实现
是 `SceneMultiplayer`，配套的 `SceneRPCInterface` 负责：

* 收集每个节点上注册的 RPC 配置；
* 把本地的 RPC 调用打包成网络包发送；
* 反过来在收到包时解码、校验权限、就地执行函数。

GDScript 的角色是“配置提供者”：通过 `@rpc` 注解告诉
`SceneRPCInterface` —— **某个函数在网络层的可见模式、传输模式、通道**
等元信息。GDScript 不参与序列化与传输，只负责让“我有哪些函数可被调
用”这一信息流出。

这种解耦使得：

* C# / GDExtension 脚本也可以通过 `Object::get_rpc_config()` 暴露相同
  形态的配置，被 `SceneRPCInterface` 一视同仁；
* 配置变更时不需要改动协议层；
* GDScript 可以保持脚本语言的简洁性，没有任何与“socket/peer”相关的
  概念硬编码。

---

## 14.2 词法/语法：`@rpc` 是注解，不是关键字

GDScript 没有把 `rpc` 做成关键字（与 Godot 3 中废弃的 `remote/sync/master`
不同）。它走的是统一的注解机制（第 5 章），在 `GDScriptParser::register_annotations()`
中注册：

```cpp
register_annotation(
    MethodInfo("@rpc",
        PropertyInfo(Variant::STRING, "mode"),
        PropertyInfo(Variant::STRING, "sync"),
        PropertyInfo(Variant::STRING, "transfer_mode"),
        PropertyInfo(Variant::INT,    "transfer_channel")),
    AnnotationInfo::FUNCTION,
    &GDScriptParser::rpc_annotation,
    varray("authority", "call_remote", "reliable", 0));
```

要点：

* **注解形态**：四个参数分别是模式、同步性、传输模式、通道号；
* **作用范围**：`AnnotationInfo::FUNCTION`——只能用于函数定义；
* **回调函数**：`rpc_annotation`（见下）；
* **默认参数**：`("authority", "call_remote", "reliable", 0)`，与
  `SceneRPCInterface::_parse_rpc_config()` 端的默认值严格一致——
  这是 GDScript 与多人模块之间的**契约**。

> 注意 `SceneRPCInterface` 端的源码注释里也写着
> *“Default values should match GDScript `@rpc` annotation registration
> and `rpc_annotation()`”*。这条契约出现在两个独立模块的注释中，是
> Godot 工程上保持跨模块一致性的常用做法。

### 旧关键字的废止

GDScript 3 有 `remote/sync/master/puppet/...` 一系列关键字，Parser
会显式拒绝它们并提示迁移：

```cpp
push_error(R"(The "remote" keyword was removed in Godot 4.
              Use the "@rpc" annotation with "any_peer" instead.)");
```

把 RPC 配置从语法层降到注解层，让“RPC”从语言特性变成了普通元数据，
减少了语言的关键字占用——这种简化是 Godot 4 多处特性都在做的事情。

---

## 14.3 注解的求值：`rpc_annotation`

```cpp
bool GDScriptParser::rpc_annotation(AnnotationNode *p_annotation,
                                    Node *p_target, ClassNode *p_class) {
    ERR_FAIL_COND_V_MSG(p_target->type != Node::FUNCTION, false,
        vformat(R"("%s" annotation can only be applied to functions.)",
                p_annotation->name));

    FunctionNode *function = static_cast<FunctionNode *>(p_target);
    if (function->rpc_config.get_type() != Variant::NIL) {
        push_error(R"(RPC annotations can only be used once per function.)",
                   p_annotation);
        return false;
    }

    Dictionary rpc_config;
    rpc_config["rpc_mode"] = MultiplayerAPI::RPC_MODE_AUTHORITY;
    if (!p_annotation->resolved_arguments.is_empty()) {
        unsigned char locality_args = 0;
        unsigned char permission_args = 0;
        unsigned char transfer_mode_args = 0;

        for (int i = 0; i < p_annotation->resolved_arguments.size(); i++) {
            if (i == 3) {
                rpc_config["channel"] = p_annotation->resolved_arguments[i].operator int();
                continue;
            }
            String arg = p_annotation->resolved_arguments[i].operator String();
            if (arg == "call_local")          { rpc_config["call_local"] = true;  locality_args++; }
            else if (arg == "call_remote")    { rpc_config["call_local"] = false; locality_args++; }
            else if (arg == "any_peer")       { rpc_config["rpc_mode"]  = MultiplayerAPI::RPC_MODE_ANY_PEER;   permission_args++; }
            else if (arg == "authority")      { rpc_config["rpc_mode"]  = MultiplayerAPI::RPC_MODE_AUTHORITY;  permission_args++; }
            else if (arg == "reliable")       { rpc_config["transfer_mode"] = MultiplayerPeer::TRANSFER_MODE_RELIABLE;            transfer_mode_args++; }
            else if (arg == "unreliable")     { rpc_config["transfer_mode"] = MultiplayerPeer::TRANSFER_MODE_UNRELIABLE;          transfer_mode_args++; }
            else if (arg == "unreliable_ordered") { rpc_config["transfer_mode"] = MultiplayerPeer::TRANSFER_MODE_UNRELIABLE_ORDERED; transfer_mode_args++; }
        }
        // ... 校验 locality_args/permission_args/transfer_mode_args 都不超过 1
    }
    function->rpc_config = rpc_config;
    return true;
}
```

可以看到几条关键设计：

1. **结果是一个 Dictionary**，最终落到 `function->rpc_config`（一个
   Variant）。这与 Godot 多人模块期望的输入类型完全一致——
   `SceneRPCInterface::_parse_rpc_config()` 拿到的就是这种格式。
2. **位置参数自由组合**：`@rpc("any_peer", "call_local", "reliable")`
   与 `@rpc("call_local", "any_peer")` 等价。Parser 通过“按字面值识别”
   而非“按位置严格对应”来解释参数，体现注解系统的灵活性。
3. **互斥校验**：`locality_args`、`permission_args`、`transfer_mode_args`
   各自最多出现一次，重复时报错——避免 `@rpc("any_peer", "authority")`
   这种语义混乱的写法。
4. **第 4 个参数特殊处理**：`channel` 是个整数，不参与字符串识别，按
   位置严格对应到第 4 个参数。

---

## 14.4 编译期：`rpc_config` 的双重写入

`rpc_config` 这个字典最终会出现在两个地方：`GDScriptFunction::rpc_config`
和 `GDScript::rpc_config`。两份配置承担不同职责。

### 14.4.1 写入 `GDScriptFunction`

`GDScriptByteCodeGenerator::write_start` 接收 `Variant p_rpc_config`，把它
直接挂到 `function->rpc_config`：

```cpp
void GDScriptByteCodeGenerator::write_start(GDScript *p_script,
        const StringName &p_function_name, bool p_static,
        Variant p_rpc_config, const GDScriptDataType &p_return_type) {
    // ...
    function->rpc_config = p_rpc_config;
    // ...
}
```

这是函数自己的“身份证”——任何持有 `GDScriptFunction*` 的代码（例如
`Object::get_method_info()`、调试器、文档生成器）都能立刻问出
“这个函数是不是 RPC？”。

`GDScriptCompiler::_parse_function` 在调用 `write_start` 时把
`function_n->rpc_config` 透传过去：

```cpp
Variant rpc_config;
if (p_func->rpc_config.get_type() != Variant::NIL) {
    rpc_config = p_func->rpc_config;
}
codegen.generator->write_start(p_script, func_name, is_static,
                               rpc_config, return_type);
```

### 14.4.2 写入 `GDScript`

而 `GDScript::rpc_config` 是一个 `Dictionary`（注意类型不同——单个函数
的是 Variant 但内容也是 Dictionary，类的是直接 Dictionary），按函数名
索引：

```cpp
case ClassNode::Member::FUNCTION: {
    const FunctionNode *function_n = member.function;
    Variant config = function_n->rpc_config;
    if (config.get_type() != Variant::NIL) {
        p_script->rpc_config[function_n->identifier->name] = config;
    }
} break;
```

这是脚本类的“RPC 函数总览表”——`SceneRPCInterface` 在一个 Node 入树
时会通过 `script->get_rpc_config()` 一次性取到这个 Dictionary，
建立局部缓存（见 14.7）。

### 14.4.3 继承的合并

```cpp
if (p_script->base.is_valid()) {
    p_script->rpc_config = p_script->base->rpc_config.duplicate();
}
// 之后再合并自身定义的 rpc 函数
```

子类继承父类的 RPC 注册表——这意味着子类中**未重写的父类 RPC 函数仍
然是 RPC**，与多数语言的方法继承语义一致。

> **注意**：如果子类重写了父类的 RPC 函数但**没有重新加 `@rpc`**，
> 子类版本仍会被视作 RPC（因为子类基底里带着父类的注册）。这是 Godot
> 当前的实现行为，文档里也建议显式重写时再次加上 `@rpc`，避免疑惑。

---

## 14.5 取属性：什么时候构造 `GDScriptRPCCallable`

GDScript 用户访问“函数属性”有两种典型写法：

```python
var c = some_node.foo                # 把方法当属性取出
some_node.foo.connect(...)           # 隐式当属性
some_node.foo.rpc(...)               # 同上，再调用 rpc()
```

GDScript 的 `_get` 路径会走到这一段（`gdscript.cpp` 简化版）：

```cpp
if (likely(top->valid)) {
    HashMap<StringName, GDScriptFunction *>::ConstIterator E
            = top->member_functions.find(p_name);
    if (E && E->value->is_static()) {
        if (top->rpc_config.has(p_name)) {
            r_ret = Callable(memnew(GDScriptRPCCallable(
                const_cast<GDScript *>(top), E->key)));
        } else {
            r_ret = Callable(const_cast<GDScript *>(top), E->key);
        }
        return true;
    }
}
```

以及实例侧 `_get_member`：

```cpp
if (sptr->rpc_config.has(p_name)) {
    r_ret = Callable(memnew(GDScriptRPCCallable(owner, E->key)));
}
```

判定规则非常简洁：**`rpc_config` 字典里有这个名字 → 返回 RPC Callable，
否则返回普通 Callable。** 这意味着 GDScript 用户根本不需要主动选择，
`some_node.foo` 取出来的就已经是“正确类型”的 Callable，可以直接 `.rpc()`。

> 普通 `Callable(object, method)` 也是有 `rpc()` 方法的（继承自基类的
> `Callable::rpc(...)`），但它走的是 `Object::callp(method, ...)` —— 不
> 进入 MultiplayerAPI 的远程分派路径。**`GDScriptRPCCallable::rpc()`
> 才是真正的远程触发**。

---

## 14.6 `GDScriptRPCCallable` 的实现

```cpp
class GDScriptRPCCallable : public CallableCustom {
    Object *object = nullptr;
    Node *node = nullptr;          // cast 后缓存
    StringName method;
    uint32_t h = 0;
    // ...
public:
    void call(...) const override;
    Error rpc(int p_peer_id, ...) const override;
    // ...
};
```

### 14.6.1 构造时锁定为 Node

```cpp
GDScriptRPCCallable::GDScriptRPCCallable(Object *p_object, const StringName &p_method) {
    ERR_FAIL_NULL(p_object);
    object = p_object;
    method = p_method;
    h = method.hash();
    h = hash_murmur3_one_64(object->get_instance_id(), h);
    node = Object::cast_to<Node>(object);
    ERR_FAIL_NULL_MSG(node, "RPC can only be defined on class that extends Node.");
}
```

构造时就完成 `Node*` cast 并缓存——RPC 必须挂在 Node 上（因为
`MultiplayerAPI` 的路由完全基于 `NodePath`），这里直接做硬性约束，
避免运行期每次 `rpc()` 都重新 cast。

### 14.6.2 `call()`：本地直接调用

```cpp
void GDScriptRPCCallable::call(const Variant **p_arguments, int p_argcount,
                               Variant &r_return_value,
                               Callable::CallError &r_call_error) const {
    r_return_value = object->callp(method, p_arguments, p_argcount, r_call_error);
}
```

如果用户用普通 `f.call(args)` 调用 RPC Callable，**它退化为本地直调**——
不发网络包。这条等价规则保证：

* `GDScriptRPCCallable` 与普通 `Callable` 在 `call()` 语义上一致；
* 用户必须显式写 `f.rpc(args)` 才会触发远程行为。

### 14.6.3 `rpc()`：转发到 Node

```cpp
Error GDScriptRPCCallable::rpc(int p_peer_id, const Variant **p_arguments,
                               int p_argcount,
                               Callable::CallError &r_call_error) const {
    if (unlikely(!node)) {
        r_call_error.error = CALL_ERROR_INSTANCE_IS_NULL;
        return ERR_UNCONFIGURED;
    }
    r_call_error.error = CALL_OK;
    return node->rpcp(p_peer_id, method, p_arguments, p_argcount);
}
```

转身就把任务交给 `Node::rpcp`：

```cpp
Error Node::rpcp(int p_peer_id, const StringName &p_method,
                 const Variant **p_arg, int p_argcount) {
    Ref<MultiplayerAPI> api = get_multiplayer();
    return api->rpcp(this, p_peer_id, p_method, p_arg, p_argcount);
}
```

这就把 GDScript 完全抛在身后，进入 `MultiplayerAPI` 的世界。

### 14.6.4 等价与哈希

```cpp
bool GDScriptRPCCallable::compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
    return p_a->hash() == p_b->hash();
}
uint32_t GDScriptRPCCallable::hash() const { return h; }
```

`h` 由 `method` 名字哈希 + `object` 实例 ID 复合而成。也就是说：

* 同一个 `(node, method)` 多次取属性得到的 RPC Callable **互相相等**——
  即便它们是不同的 `memnew` 出来的对象。
* 这与 `GDScriptLambdaCallable` 的“引用相等”设计形成对比——RPC Callable
  是“值语义”的，而 lambda Callable 是“引用语义”的。

为什么差别这么大？

* lambda 的标识就是它在源码中的位置 + 一组 capture，无法用一个稳定的
  hash 表达；
* RPC Callable 的标识就是“哪个对象的哪个方法”，刚好和 hash 完美对应。

这种“值相等性”让 `signal.disconnect(node.foo)` 之类的写法能正确工作，
不会因为每次取属性都生成新对象而断不开。

---

## 14.7 接收侧：`SceneRPCInterface` 如何使用 `rpc_config`

### 14.7.1 配置缓存

每个 Node 入树时，`SceneRPCInterface::_get_node_config` 会把脚本的
`rpc_config` 解析进一个 `RPCConfigCache`：

```cpp
const RPCConfigCache &SceneRPCInterface::_get_node_config(const Node *p_node) {
    const ObjectID oid = p_node->get_instance_id();
    if (rpc_cache.has(oid)) return rpc_cache[oid];

    RPCConfigCache cache;
    _parse_rpc_config(p_node->get_node_rpc_config(), true, cache);
    if (p_node->get_script_instance()) {
        _parse_rpc_config(p_node->get_script_instance()->get_rpc_config(),
                          false, cache);
    }
    rpc_cache[oid] = cache;
    return rpc_cache[oid];
}
```

`get_script_instance()->get_rpc_config()` 最终走到
`GDScriptInstance::get_rpc_config() → script->get_rpc_config()`，拿回的
就是 14.4.2 里写入的字典。

`_parse_rpc_config` 把字典里每个函数项转成一个 `RPCConfig` 结构，并
**为它分配一个 16 位 ID**：

```cpp
uint16_t id = ((uint16_t)i);
if (p_for_node) id |= (1 << 15);     // 高位区分 native vs script
r_cache.configs[id] = cfg;
r_cache.ids[name]   = id;
```

字典在缓存前先按 key 排序——这样**只要双端脚本一致，分配出的 ID 也一致**，
网络包就可以只发 ID 而不发函数名，省掉大量字符串传输。

### 14.7.2 发送：`rpcp → _send_rpc`

```cpp
Error SceneRPCInterface::rpcp(Object *p_obj, int p_peer_id,
                              const StringName &p_method,
                              const Variant **p_arg, int p_argcount) {
    // ...
    const RPCConfigCache &config_cache = _get_node_config(node);
    uint16_t rpc_id = config_cache.ids.has(p_method)
                      ? config_cache.ids[p_method] : UINT16_MAX;
    ERR_FAIL_COND_V_MSG(rpc_id == UINT16_MAX, ERR_INVALID_PARAMETER, ...);
    const RPCConfig &config = config_cache.configs[rpc_id];

    // ...本地权限检查（call_local 等）...

    if (p_peer_id != caller_id) {
        _send_rpc(node, p_peer_id, rpc_id, config, p_method, p_arg, p_argcount);
    }
    if (call_local_native || call_local_script) {
        node->callp(p_method, p_arg, p_argcount, ce);
    }
    // ...
}
```

`_send_rpc` 把 `node_path_cache_id + rpc_id + 序列化 args` 拼成数据包，
按 `transfer_mode/channel` 通过 `MultiplayerPeer` 发出去。

### 14.7.3 接收：`process_rpc → _process_rpc`

接收端从包里读出 `node_target` 找到 Node，再读出 `rpc_id` 从配置缓存
里查到 `RPCConfig`，最后调用 `_process_rpc(...)` 执行权限检查并最终
`node->callp(method_name, ...)`——回到了普通 Godot 方法分派，对 GDScript
来说就是 `GDScriptInstance::callp → GDScriptFunction::call`，与本地调用
没有区别。

---

## 14.8 端到端时序：`node.rpc("foo", 1, 2)` 的完整生命周期

```
[GDScript 源码]
  some_node.foo.rpc(1, 2)

[Parser/Analyzer/Compiler]
  → 普通方法访问 + Callable.rpc(...) 调用，没有特殊指令

[运行期 - 本地侧]
  ├─ OPCODE_GET_NAMED [some_node] [tmp] <"foo">
  │    └─ GDScriptInstance::get("foo") 走到 14.5 的判断
  │         └─ rpc_config.has("foo") → 构造 GDScriptRPCCallable
  │
  ├─ OPCODE_CALL [tmp] [const(1)] [const(2)] [_] <"rpc">
  │    └─ Callable::callp("rpc", ...)
  │         └─ GDScriptRPCCallable::rpc(0, [1,2], 2)
  │              └─ Node::rpcp(0, "foo", [1,2], 2)
  │                   └─ MultiplayerAPI::rpcp(...)
  │                        └─ SceneRPCInterface::rpcp(...)
  │                             ├─ _get_node_config(node) 取得 rpc_id
  │                             ├─ 权限检查、call_local 判断
  │                             ├─ 若 call_local: node->callp("foo", ...) 本地调用一遍
  │                             └─ _send_rpc(node, peer, rpc_id, ...)
  │                                  └─ MultiplayerPeer::send_packet(buf, ...)

[网络层传输]
  ... 数据包流向远端 ...

[运行期 - 远端]
  ├─ MultiplayerPeer::poll() → packet_received
  ├─ SceneMultiplayer 收包 → SceneRPCInterface::process_rpc(...)
  │    ├─ 解析 node_id → 找到 Node
  │    ├─ 解析 rpc_id  → 查 RPCConfig
  │    └─ _process_rpc(node, ...)
  │         ├─ 权限校验（rpc_mode：authority / any_peer）
  │         ├─ 反序列化参数
  │         └─ node->callp("foo", argv, argc, ce)
  │              └─ GDScriptInstance::callp("foo", ...)
  │                   └─ GDScriptFunction::call(instance, argv, ...)
  │                        └─ 主循环执行 foo() 的字节码
```

每一步都在调用现成的子系统，没有任何模块为 “GDScript 的 RPC” 做特例。
GDScript 自己提供的全部责任只有：

1. 把 `@rpc(...)` 解析为 `Dictionary`；
2. 在 `_get` 时区分构造哪种 Callable；
3. `GDScriptRPCCallable::rpc()` 转发到 `Node::rpcp`。

剩下的事情完全交给 Godot 的网络层。这种**“窄接口、宽实现”** 的边界设计
是 GDScript 与引擎其它模块协作的范本。

---

## 14.9 `GDScriptFunction::rpc_config` 的别样用途

第 14.4.1 节提到 `GDScriptFunction` 自身也持有一份 `rpc_config`——其实
`SceneRPCInterface` 走的是 `GDScript::rpc_config`，那么函数对象上的
副本是给谁用的？

答案散落在几个地方：

* **`GDScriptFunction::get_rpc_config()`** 是公共 API，被反射、文档生成
  器、LSP 用来查询某个函数是否是 RPC。
* **调试器**：在挂起的栈上展示某个函数的 RPC 元数据。
* **未来拓展**：如果将来 GDScript 引入“匿名 RPC”或“lambda RPC”，函数
  对象级别的配置就成为唯一可靠的来源（脚本级字典只存有名字的成员
  函数）。

把 `rpc_config` 同时挂在两个层级看似冗余，但在 Godot 这种长期演进的工程
里，给“函数”也留出独立元数据槽是常见的健壮性投资。

---

## 14.10 设计回顾

GDScript 的 RPC 实现可以总结为三条原则：

1. **元数据而非语法**：`@rpc` 是普通注解，不污染关键字表，便于演进。
2. **分层落地**：注解 → 函数对象 → 脚本类 → 网络层缓存，每一层都有
   明确职责。
3. **Callable 的两条 `call` 路径**：`call()` 退化为本地直调，`rpc()`
   才进入 MultiplayerAPI——让用户用同一种符号同时表达本地与远程行为。

第 1 条是 Godot 4 的语言哲学；第 2 条让网络层与脚本层得以独立演进；
第 3 条让 GDScript 的多人编程有一个非常符合直觉的入口。

---

## 小结

* `@rpc` 在 GDScript 中是注解，不是关键字；它通过 `rpc_annotation`
  解析为 `Dictionary` 形态的配置；
* 编译器把这个字典分别落到 `GDScriptFunction::rpc_config` 和
  `GDScript::rpc_config`，各有用途；
* 取属性时根据脚本级 `rpc_config` 是否包含该名字，选择构造
  `GDScriptRPCCallable` 还是普通 `Callable`；
* `GDScriptRPCCallable` 的 `call()` 退化为本地直调，`rpc()` 才转发到
  `Node::rpcp` → `MultiplayerAPI::rpcp` → `SceneRPCInterface`；
* 远端走 `process_rpc` 解码、权限校验后，最终回到普通方法分派，对
  GDScript 透明；
* 整个链路里 GDScript 自己的责任非常窄：注解解析、Callable 选型、
  方法转发——其余完全复用 Godot 网络层。

至此第四部分（运行时）全部完成。下一章我们将进入第五部分，从“函数与
调用”切换到“类与实例”，先看 GDScript 类对象、内部类与继承的实现。
