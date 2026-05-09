# 第 21 章　Native 函数与 FFI

VM 跑通了，但 Mini 此刻还活在沙箱里——没有 print、没有 io、没
有 math。这一章我们打通"Mini ↔ C++"的双向接口：

1. **C++ → Mini**：注册一个 native 函数，让 Mini 能调用；
2. **Mini → C++**：传 Mini 表/函数给 C++ 代码，C++ 反过来回调；
3. 处理类型转换、错误传播、生命周期。

## 21.1 Native 函数签名

最简单的统一签名：

```cpp
// src/native.h
using NativeFn = std::function<Value(VM&, const std::vector<Value>&)>;

struct NativeFunction {
    std::string name;
    NativeFn    fn;
    int         arity = -1;  // -1 表示可变参数

    Value call(VM& vm, const std::vector<Value>& args) {
        if (arity >= 0 && (int)args.size() != arity) {
            throw RuntimeError(name + " expects " + std::to_string(arity)
                + " args, got " + std::to_string(args.size()));
        }
        return fn(vm, args);
    }
};
```

为什么传 `VM&`？

* 让 native 函数能反过来调用 Mini 函数（`pcall`、`map` 之类）；
* 让 native 能访问 globals、抛 RuntimeError 时拿到 line。

`arity = -1` 是 `print` 这种"可变参数"的占位；具体校验交给 fn
内部。

## 21.2 注册 API

```cpp
// src/builtins.h
void register_builtins(VM& vm);

// src/builtins.cpp
static void register_native(VM& vm, const std::string& name, NativeFn fn, int arity = -1) {
    auto nf = std::make_shared<NativeFunction>();
    nf->name = name;
    nf->fn = std::move(fn);
    nf->arity = arity;
    vm.globals()->define(name, Value(nf));
}

void register_builtins(VM& vm) {
    // -- IO --
    register_native(vm, "print", [](VM&, const std::vector<Value>& args) -> Value {
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) std::cout << "\t";
            std::cout << args[i].to_string();
        }
        std::cout << "\n";
        return Value();
    });

    // -- 类型 --
    register_native(vm, "type", [](VM&, const std::vector<Value>& a) -> Value {
        if (a.size() != 1) throw RuntimeError("type expects 1 arg");
        return Value(std::string(a[0].type_name()));
    }, 1);

    register_native(vm, "tostring", [](VM&, const std::vector<Value>& a) -> Value {
        if (a.size() != 1) throw RuntimeError("tostring expects 1 arg");
        return Value(a[0].to_string());
    }, 1);

    register_native(vm, "tonumber", [](VM&, const std::vector<Value>& a) -> Value {
        if (a.size() != 1) throw RuntimeError("tonumber expects 1 arg");
        const auto& v = a[0];
        if (v.is_int() || v.is_float()) return v;
        if (v.is_string()) {
            try {
                size_t pos = 0;
                std::int64_t i = std::stoll(v.as_string(), &pos);
                if (pos == v.as_string().size()) return Value(i);
                pos = 0;
                double d = std::stod(v.as_string(), &pos);
                if (pos == v.as_string().size()) return Value(d);
            } catch (...) {}
        }
        return Value();
    }, 1);

    // -- 数学 --
    register_native(vm, "sqrt", [](VM&, const std::vector<Value>& a) -> Value {
        if (a.size() != 1 || !(a[0].is_int() || a[0].is_float()))
            throw RuntimeError("sqrt expects number");
        double x = a[0].is_int() ? (double)a[0].as_int() : a[0].as_float();
        return Value(std::sqrt(x));
    }, 1);

    register_native(vm, "floor", [](VM&, const std::vector<Value>& a) -> Value {
        double x = a[0].is_int() ? (double)a[0].as_int() : a[0].as_float();
        return Value((std::int64_t)std::floor(x));
    }, 1);

    // -- 表 --
    register_native(vm, "len", [](VM&, const std::vector<Value>& a) -> Value {
        const auto& v = a[0];
        if (v.is_string()) return Value((std::int64_t)v.as_string().size());
        if (v.is_table())  return Value((std::int64_t)v.as_table()->size());
        throw RuntimeError("len expects string or table");
    }, 1);

    register_native(vm, "keys", [](VM&, const std::vector<Value>& a) -> Value {
        if (!a[0].is_table()) throw RuntimeError("keys expects table");
        auto t = std::make_shared<Table>();
        std::int64_t i = 0;
        for (auto& kv : a[0].as_table()->entries()) {
            t->set(Value(i++), kv.first);
        }
        return Value(t);
    }, 1);

    // -- 元函数 --
    register_native(vm, "pcall", [](VM& vm, const std::vector<Value>& a) -> Value {
        if (a.empty() || !(a[0].is_closure() || a[0].is_native()))
            throw RuntimeError("pcall expects function as first arg");
        std::vector<Value> args(a.begin() + 1, a.end());
        auto result = std::make_shared<Table>();
        try {
            Value rv = vm.call_function(a[0], args);
            result->set(Value(std::int64_t(0)), Value(true));
            result->set(Value(std::int64_t(1)), rv);
        } catch (const RuntimeError& e) {
            result->set(Value(std::int64_t(0)), Value(false));
            result->set(Value(std::int64_t(1)), Value(std::string(e.what())));
        }
        return Value(result);
    });
}
```

`pcall` 是关键的"元函数"——在 Mini 代码里捕获 RuntimeError，
让脚本能写自己的错误处理。需要 `vm.call_function(func, args)` 这
个 helper：在 C++ 里调一个 Mini 函数。

## 21.3 vm.call_function：C++ 调 Mini 函数

这是 FFI 的反向：让 native 能回调 Mini closure。

```cpp
Value VM::call_function(const Value& fn, const std::vector<Value>& args) {
    if (fn.is_native()) {
        return fn.as_native()->call(*this, args);
    }
    if (!fn.is_closure()) {
        throw RuntimeError("call_function: not callable");
    }

    // 模拟 OP_CALL：push fn, push args, 调 do_call, 跑到本帧 return
    int saved_top = (int)stack_.size();
    int saved_frames = (int)frames_.size();

    stack_.push_back(fn);
    for (auto& a : args) stack_.push_back(a);
    do_call((int)args.size());

    // 跑直到我们这一层 frame 弹出
    while ((int)frames_.size() > saved_frames) {
        // 复用 main_loop 但给个退出条件
        step_until_frame_popped(saved_frames);
    }

    if ((int)stack_.size() <= saved_top) return Value();
    Value rv = std::move(stack_.back());
    stack_.pop_back();
    return rv;
}
```

`step_until_frame_popped` 就是把 main_loop 抽出，加判断
"frames_ 缩到 saved_frames 时退出"。

注意**栈/帧基线保存**：异常时也要恢复——否则 Mini 函数抛出后
stack_ 里残留垃圾会污染下一次调用。包一层 try/catch：

```cpp
try {
    while ((int)frames_.size() > saved_frames) step_once();
} catch (...) {
    stack_.resize(saved_top);
    while ((int)frames_.size() > saved_frames) frames_.pop_back();
    throw;
}
```

## 21.4 让 native 持有 host 资源

实际项目里 native 经常代表"一个文件句柄""一个网络连接""一
个游戏引擎里的实体"——要给 Mini 暴露成"可调用对象"，又要在
没人引用时自动释放。

最简单方案：用 Table 的 `__handle` 槽存一个 `shared_ptr<void>`：

```cpp
// 打开文件
register_native(vm, "fopen", [](VM&, const std::vector<Value>& a) -> Value {
    auto path = a[0].as_string();
    auto fp = std::shared_ptr<FILE>(std::fopen(path.c_str(), "r"), [](FILE* f) {
        if (f) std::fclose(f);
    });
    if (!fp) throw RuntimeError("fopen failed: " + path);
    auto t = std::make_shared<Table>();
    t->set(Value(std::string("__handle")), Value(std::make_shared<NativeRef>(fp)));
    return Value(t);
});

// 读一行
register_native(vm, "fread_line", [](VM&, const std::vector<Value>& a) -> Value {
    auto t = a[0].as_table();
    auto h = t->get(Value(std::string("__handle")));
    if (!h.is_native_ref()) throw RuntimeError("fread_line: not a file");
    auto fp = std::static_pointer_cast<FILE>(h.as_native_ref()->ptr);
    char buf[1024];
    if (!std::fgets(buf, sizeof(buf), fp.get())) return Value();
    return Value(std::string(buf));
});
```

`NativeRef` 是 Value 里加的一个新 alternative：

```cpp
struct NativeRef {
    std::shared_ptr<void> ptr;
    std::string type_tag;
    explicit NativeRef(std::shared_ptr<void> p, std::string tag = {})
        : ptr(std::move(p)), type_tag(std::move(tag)) {}
};
```

`type_tag` 用于运行时检查"这个 native ref 真的是 FILE 吗"——
否则用户拿一个 socket ref 当 FILE 用会爆 segfault。

## 21.5 注册类（OOP）

如果想让 Mini 用上 C++ 的"类"风格 API，可以构造一个 Table 当
"namespace"：

```cpp
auto math = std::make_shared<Table>();
math->set(Value(std::string("pi")), Value(3.14159265358979));
math->set(Value(std::string("sqrt")), Value(std::make_shared<NativeFunction>(/* ... */)));
math->set(Value(std::string("sin")), Value(std::make_shared<NativeFunction>(/* ... */)));
vm.globals()->define("math", Value(math));
```

Mini 里：

```python
print(math.pi)
print(math.sqrt(2))
```

`math.sqrt(2)` 在 Mini 编译期变成 `math.sqrt(2)` → `OP_LOAD_GLOBAL
"math"; OP_LOAD_CONST "sqrt"; OP_GET_INDEX; OP_PUSH_INT 2; OP_CALL`。
没有专门的"方法调用"语法——一切是 table + 函数值。Lua 也这样
开始，后来加 `:` 语法糖才把"self"自动塞进第一个参数。

## 21.6 GDScript 是怎么做的（对照）

GDScript 的 native 接口比 Mini 复杂得多，因为它要兼容整个
Godot 引擎：

* `Variant` 替代 Value，能装 60+ 种类型（含 Vector3、NodePath
  ……）；
* 每个 `Callable` 包了 ObjectID + method name；调用时 binder
  根据 method 的 `Vector<Variant::Type>` 做参数 marshaling；
* 错误用 `Callable::CallError` 而不是异常——返回值代码（INVALID_METHOD、
  TOO_FEW_ARGUMENTS……）由 caller 检查；
* native 由 `ClassDB::bind_method` 在 Godot 启动时一次性注册到一
  个全局表，GDScript 编译期可以查名字 + 参数类型做静态校验。

我们 Mini 把这套大刀阔斧砍掉——`std::function` + `std::variant`
就够教学。但**思路是一样的**：

| 概念 | Mini | GDScript |
|------|------|----------|
| 动态值 | Value | Variant |
| native fn | NativeFunction | Callable + ClassDB::bind_method |
| 错误 | C++ exception | Callable::CallError |
| 类型校验 | runtime + arity | 编译期 (Analyzer) + runtime |

## 21.7 安全：sandbox 边界

如果你打算让 Mini 跑用户代码（modding、配置脚本、热补丁），
默认就**不要注册 fopen / system / load**——这些都是越狱口子。

Lua 的 sandbox 模式就是把 `os.execute`、`io.open`、`require`
全部不注册到 environment，再禁用 `loadstring`。Mini 里同样：
`register_safe_builtins(vm)` 只注册纯函数，不碰 IO/系统。

## 21.8 第 21 章小结

* 用 `std::function<Value(VM&, args)>` 统一 native 签名；
* 注册 = 把 NativeFunction 包进 Value 塞 globals；
* `pcall` 给 Mini 用户错误处理能力——内部依赖
  `vm.call_function(fn, args)` 这个 C++ 调 Mini 的 helper；
* 实际工程要给 native 持有的资源用 `NativeRef` + 类型 tag 包
  装，避免 segfault；
* 模块/类用 Table 表达——Mini 不引入新语法；
* GDScript 用 ClassDB + Variant 把 native 注册做到引擎级，
  Mini 简化版能教清楚思路。

下一章 **第 22 章 GC**：从 shared_ptr 引用计数升级到三色标记，
讨论循环引用、内存预算与增量回收。
