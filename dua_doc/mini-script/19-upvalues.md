# 第 19 章　Upvalue 与闭包

第 18 章我们解决了"函数能调用"。现在解决另一个更难的：**函数
能捕获外层局部变量**——闭包。

```python
fn make_counter()
    let n = 0
    fn step()
        n = n + 1
        return n
    end
    return step
end

let c = make_counter()
print(c())  -- 1
print(c())  -- 2
```

`step` 引用了 `make_counter` 的局部 `n`。但 `make_counter` 已经
return 了，按第 18 章的规则，`n` 那个 stack 槽早被截断。`step`
怎么访问？

这就是 Lua **upvalue** 机制要解决的问题，也是这一章的全部主题。

## 19.1 三种闭包实现路线

闭包的本质是"延长被捕获变量的生命周期"。三种路线：

| 路线 | 代表 | 做法 |
|------|------|------|
| 全部上堆 | Tree-walking 解释器 | 每个变量都在堆上的 Environment 里——闭包白送 |
| 整 frame 上堆 | 旧 JavaScript | 函数生命周期内整个 frame 不释放 |
| 按需迁移（upvalue） | Lua、CPython 部分 | 变量先在栈上，闭包退栈时把被捕获的迁移到堆 |

我们之前的树遍历实现（第 10 章）走的是路线 1——简单但慢。VM
现在用了栈帧，**路线 3 是最高效且最优雅**的方案。

## 19.2 Open / Closed upvalue

Lua 的核心 idea 分两态：

* **Open upvalue**：变量还在 caller 栈上活着——upvalue 存一个
  指向那个栈槽的指针 `Value*`；
* **Closed upvalue**：caller return 了，栈槽要被截断——把那个
  Value 拷贝到 upvalue 自己的存储里，指针重定向到自己内部。

```cpp
// src/upvalue.h
struct Upvalue {
    Value* location;     // 当前指向的位置（栈或自己 closed_value）
    Value  closed_value; // open 时不用，close 时存放
    int    stack_index;  // open 状态下记录原栈下标，便于 close 检查

    bool is_open(const std::vector<Value>& stack) const {
        return location != &closed_value;
    }

    void close() {
        closed_value = std::move(*location);
        location = &closed_value;
    }
};
```

`Closure` 持有 `vector<shared_ptr<Upvalue>>`。**用 `shared_ptr`**
是因为多个 closure 可能共享同一个 upvalue（同一个 outer 局部变
量被两个 inner func 同时捕获）：

```python
fn make()
    let n = 0
    fn inc() n = n + 1 return n end
    fn get() return n end
    return [inc, get]
end
```

这里 `inc` 和 `get` 必须看到**同一个** `n`——`inc` 改了，`get`
要能读到新值。所以 `n` 对应的 Upvalue 在两个 closure 里是同一
个 shared_ptr。

## 19.3 Closure 结构

```cpp
// src/closure.h
struct Closure {
    std::shared_ptr<FunctionProto> proto;
    std::vector<std::shared_ptr<Upvalue>> upvalues;

    explicit Closure(std::shared_ptr<FunctionProto> p) : proto(std::move(p)) {}
};
```

之前 Value 里只存 `proto`。现在 `Value::function` 改成 `Closure*`
（shared_ptr 包装）。

## 19.4 编译期：识别 upvalue

Compiler 解析函数时，遇到引用一个变量名要分三类：

1. 本函数的 local（含参数）→ `OP_LOAD_LOCAL slot`；
2. 外层函数的 local → **upvalue**，发射 `OP_LOAD_UPVALUE idx`；
3. 都不是 → global，`OP_LOAD_GLOBAL name_const`。

引入 `Compiler::resolve_upvalue`：

```cpp
struct UpvalueDescriptor {
    bool is_local;   // true: 直接捕获父帧的 local；false: 捕获父帧的 upvalue
    int  index;
};

class FunctionCompiler {
    FunctionCompiler* enclosing;
    std::vector<Local>            locals;
    std::vector<UpvalueDescriptor> upvalues;

    int resolve_local(const std::string& name) {
        for (int i = (int)locals.size() - 1; i >= 0; --i)
            if (locals[i].name == name) return i;
        return -1;
    }

    int add_upvalue(bool is_local, int index) {
        // 去重
        for (size_t i = 0; i < upvalues.size(); ++i) {
            const auto& uv = upvalues[i];
            if (uv.is_local == is_local && uv.index == index) return (int)i;
        }
        upvalues.push_back({is_local, index});
        return (int)upvalues.size() - 1;
    }

    int resolve_upvalue(const std::string& name) {
        if (!enclosing) return -1;
        int local = enclosing->resolve_local(name);
        if (local >= 0) {
            enclosing->locals[local].is_captured = true;
            return add_upvalue(true, local);
        }
        int upv = enclosing->resolve_upvalue(name);
        if (upv >= 0) {
            return add_upvalue(false, upv);
        }
        return -1;
    }
};
```

精妙的是 **递归 resolve_upvalue**：

```python
fn outer()
    let a = 1
    fn middle()
        fn inner()
            return a    -- 跨两层
        end
        return inner
    end
end
```

`inner` 找 `a` 时：

1. inner 自己 local 没 `a`；
2. 调 `enclosing(middle).resolve_upvalue("a")`；
   - middle 自己 local 也没；
   - 调 `enclosing(outer).resolve_local("a")` → 找到，下标 0；
   - middle 添加 upvalue `{is_local=true, index=0}`，返回 idx；
3. inner 添加 upvalue `{is_local=false, index=that_idx}`。

`is_local=true` 表示"父函数对应的 local 槽"；`is_local=false`
表示"父函数对应的 upvalue 槽"——递归冒泡完成跨层捕获。

## 19.5 OP_CLOSURE：实例化闭包

Compiler 把内部函数编译成 `FunctionProto`，存进**外层函数**的
constants 池。然后发射 `OP_CLOSURE proto_const_idx`，其后紧跟
**N 条捕获描述指令**（pseudo-instruction）：

```
OP_CLOSURE  k       ; k = constants 中 proto 的下标
[is_local | index]  ; 第 0 个 upvalue
[is_local | index]  ; 第 1 个 upvalue
...
```

每条 pseudo-instruction 复用一条 32-bit 槽——`is_local` 占高位
1 bit，`index` 占低位 16 bit（足够大）。

VM 执行 OP_CLOSURE：

```cpp
case OP_CLOSURE: {
    int k = get_uarg(inst);
    auto proto = fr.constants[k].as_proto();   // Value 支持 proto 类型
    auto closure = std::make_shared<Closure>(proto);
    closure->upvalues.reserve(proto->upvalue_descs.size());

    for (auto& desc : proto->upvalue_descs) {
        if (desc.is_local) {
            // 捕获当前帧的 local
            int slot = fr.local_base + desc.index;
            closure->upvalues.push_back(capture_upvalue(slot));
        } else {
            // 复用当前 closure 的 upvalue
            closure->upvalues.push_back(fr.closure->upvalues[desc.index]);
        }
    }
    stack_.push_back(Value(closure));
    break;
}
```

`upvalue_descs` 是 `FunctionProto` 编译期就已经定下的，所以不需
要在 bytecode 里再放 pseudo-instruction（更干净）。我们 Mini 用
这种简化方案——少一层指令编码。

## 19.6 capture_upvalue：开链表

如果同一个 stack 槽被多个 closure 捕获，必须返回**同一个**
Upvalue 对象。VM 用一个**开 upvalue 链表**追踪：

```cpp
class VM {
    std::vector<std::shared_ptr<Upvalue>> open_upvalues_;
    // 按 stack_index 升序

    std::shared_ptr<Upvalue> capture_upvalue(int slot) {
        // 二分 / 线性查找——栈上活的 upvalue 通常很少
        for (auto& uv : open_upvalues_) {
            if (uv->stack_index == slot) return uv;
        }
        auto uv = std::make_shared<Upvalue>();
        uv->location = &stack_[slot];
        uv->stack_index = slot;
        open_upvalues_.push_back(uv);
        return uv;
    }
};
```

性能版本（Lua）维护按 slot 降序的链表，O(深度) 查找；我们 Mini
线性扫——栈上同时活着的 upvalue 一般 < 10 个，无所谓。

## 19.7 close_upvalues：return 时的迁移

`do_return` 里调：

```cpp
void VM::close_upvalues(int from_slot) {
    for (auto it = open_upvalues_.begin(); it != open_upvalues_.end(); ) {
        auto& uv = *it;
        if (uv->stack_index >= from_slot) {
            uv->close();           // 把 stack 上的 Value 拷到 closed_value
            it = open_upvalues_.erase(it);
        } else {
            ++it;
        }
    }
}
```

`from_slot = fr.local_base` ——本帧的 local 全部即将被截断。所
以"`stack_index >= local_base`"的 upvalue 都得 close。

被 close 后 `Upvalue::location` 指向自己内部 `closed_value`，
后续读写都打到 heap 内存——不再依赖那块即将无效的栈空间。

**bug 防御**：上面 `uv->location = &stack_[slot]` 是个**裸指
针**，指向 vector 内部。如果 stack `vector` 在 capture 之后被
push/pop 触发 reallocation，这个指针就悬空了。

解决方案：

* 方案 A：stack 用 fixed-size 数组（启动时 `resize(1<<20)`，永
  不 reallocate）；
* 方案 B：upvalue 不存 `Value*`，而存 `(stack_index)`，每次访
  问都 `&stack_[index]`——多一次 vector 索引但不会失效。

Lua 用方案 A（栈预分配）。Mini 教学用方案 B 更安全：

```cpp
struct Upvalue {
    bool   is_closed = false;
    int    stack_index = -1;
    Value  closed_value;

    Value& get(std::vector<Value>& stack) {
        return is_closed ? closed_value : stack[stack_index];
    }
    void close(std::vector<Value>& stack) {
        closed_value = std::move(stack[stack_index]);
        is_closed = true;
    }
};
```

OP_LOAD_UPVALUE / OP_STORE_UPVALUE 都通过 `uv->get(stack_)`
访问。无悬空隐患。

## 19.8 OP_LOAD_UPVALUE / OP_STORE_UPVALUE

```cpp
case OP_LOAD_UPVALUE: {
    int idx = get_uarg(inst);
    stack_.push_back(fr.closure->upvalues[idx]->get(stack_));
    break;
}
case OP_STORE_UPVALUE: {
    int idx = get_uarg(inst);
    fr.closure->upvalues[idx]->get(stack_) = stack_.back();
    break;
}
```

简单到出乎意料——所有复杂度都在 capture / close 那一步。

## 19.9 但是 OP_CLOSE_UPVALUE 干嘛用

考虑：

```python
fn outer()
    for i in range(10) do
        let item = i * 2
        callbacks.push(fn() return item end)
    end
end
```

每轮循环 `item` 都是同一个 stack 槽吗？取决于编译器实现。如果
我们把 `item` 这个 local 每次循环复用同一个 slot，那 10 个 closure
共享同一个 Upvalue——它们最终都看到 `item = 18`（最后一轮的值）。
这就是 JavaScript 的著名 `var` 闭包陷阱。

正确做法：每轮循环结束时，把 `item` 这个 local 对应的 upvalue
**强制 close**，下一轮再开新的：

```cpp
case OP_CLOSE_UPVALUE: {
    int slot = fr.local_base + get_uarg(inst);
    close_upvalues(slot);
    // 槽里的值还在，但不再有 open upvalue 指着它
    break;
}
```

Compiler 在循环体结束的位置，给本轮新创建的 local 发射
`OP_CLOSE_UPVALUE`。这样下一轮迭代时，`item` 槽上的新值不会回
塞给上一轮的 closure。

JavaScript 在 ES6 `let` 引入时正式修复了这个问题——`let` 的语
义就是"每次迭代是新 binding"，跟 Lua/Mini 的 `OP_CLOSE_UPVALUE`
是一回事。

## 19.10 验证：counter

跑开头那段 make_counter：

```python
fn make_counter()
    let n = 0
    fn step()
        n = n + 1
        return n
    end
    return step
end

let c = make_counter()
print(c())  -- 1
print(c())  -- 2
print(c())  -- 3
```

验证流程：

1. compile 时 step 识别出 `n` 是父帧 local，记 upvalue
   `{is_local=true, index=0}`；
2. make_counter return 时，`step` 已经 push 到栈顶；do_return
   触发 close_upvalues(local_base=0)——`n` 那个 open upvalue
   close 成 closed_value=0；
3. 主程序得到 `c`（带闭包的 step）；
4. 第一次 `c()` 调用：OP_LOAD_UPVALUE 0 读到 0；OP_PUSH_INT 1；
   OP_ADD 得 1；OP_STORE_UPVALUE 0 写回；返回 1；
5. 第二次：upvalue 现在是 1，加 1 得 2；
6. ……

闭包工作了。

## 19.11 第 19 章小结

* **Upvalue** 是"延长被捕获变量寿命"的载体——open 时指向栈，
  closed 时把值搬到自己内部；
* `Closure` 持有一组 `shared_ptr<Upvalue>`——多个 closure 共
  享同一变量时是同一 shared_ptr；
* Compiler 通过 `resolve_local → resolve_upvalue → resolve_global`
  三级 fallback 决定一个 identifier 编译成什么；`resolve_upvalue`
  递归冒泡跨多层捕获；
* VM 维护 `open_upvalues_` 链表，capture 时去重；frame return
  时 `close_upvalues(local_base)` 把本帧的 upvalue 全部 close；
* 循环里捕获要发射 `OP_CLOSE_UPVALUE`，避免 JavaScript var 闭
  包陷阱。

第三部分的 VM 主体逻辑到这就完整了。剩下两块拼图：

* **第 20 章**：反汇编器与调试支持——能 dump 指令流、设断点、
  单步；
* **第 21 章**：原生函数注册与 Mini ↔ C++ 互操作；
* **第 22 章**：从引用计数 GC 演进到三色标记。

下一章 **第 20 章 反汇编器与调试**。
