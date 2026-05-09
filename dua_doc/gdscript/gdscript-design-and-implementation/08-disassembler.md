# 第 8 章　字节码格式与反汇编：`gdscript_disassembler.cpp`

> 本章对应源码：
> `modules/gdscript/gdscript_function.h`（`Opcode` 枚举与 `Address` 编码）、
> `modules/gdscript/gdscript_disassembler.cpp`（`GDScriptFunction::disassemble()`）。

前面两章我们跟着 Compiler 和 CodeGen 看了"字节码是怎么长出来的"。本章换个角度——当字节码已经摆在你面前，**怎么把它读回去？** 这就是反汇编器的工作。读得懂反汇编，你就能：

- 在性能分析时肉眼判断一段脚本究竟走到了哪条快路径；
- 在修改 Compiler/VM 时验证修改是否产生了预期的字节码变化；
- 在调试跑偏的行为时反向定位是 Analyzer 没打上硬类型、还是 CodeGen 漏了某条 Opcode。

本章分三部分：先讲 **字节码文件级格式**（Opcode 枚举 + Address 编码），再讲 **Disassembler 的实现思路**，最后给几段 **实际的反汇编输出** 练眼力。

## 8.1　字节码的文件级格式

### 8.1.1　`code` 数组：指令流

回顾第 7 章：每个 `GDScriptFunction` 持有一个 `Vector<int> code`。它是纯粹的 `int` 序列，Opcode 与操作数一视同仁。VM 以 PC（指令指针）为索引线性读取。

每条指令的格式是：

```
[ opcode ] [ operand1 ] [ operand2 ] ... [ operandN ]
```

`N` 随 Opcode 种类而定。Opcode 枚举看起来有 170+ 个（见 `gdscript_function.h:152`），但其实许多是"同一指令的类型特化族"：

| 家族 | 代表 Opcode | 家族成员 |
|---|---|---|
| 通用运算 | `OPCODE_OPERATOR` | 一条 |
| 类型特化运算 | `OPCODE_OPERATOR_VALIDATED` | 一条（指向 `operator_funcs` 池） |
| 类型测试 | `OPCODE_TYPE_TEST_*` | BUILTIN / ARRAY / DICTIONARY / NATIVE / SCRIPT 各一 |
| 键/下标读写 | `OPCODE_SET/GET_KEYED(_VALIDATED)` / `OPCODE_SET/GET_INDEXED_VALIDATED` | 6 条 |
| 命名读写 | `OPCODE_SET/GET_NAMED(_VALIDATED)` / `OPCODE_SET/GET_MEMBER` / `OPCODE_SET/GET_STATIC_VARIABLE` | 8 条 |
| 赋值与类型转换 | `OPCODE_ASSIGN` / `OPCODE_ASSIGN_NULL/TRUE/FALSE` / `OPCODE_ASSIGN_TYPED_*` | 8 条 |
| cast | `OPCODE_CAST_TO_*` | BUILTIN / NATIVE / SCRIPT |
| 构造 | `OPCODE_CONSTRUCT` / `OPCODE_CONSTRUCT_VALIDATED` / `OPCODE_CONSTRUCT_ARRAY` / `OPCODE_CONSTRUCT_DICTIONARY` / 带 TYPED 版本 | 6 条 |
| 调用 | `OPCODE_CALL*` | 13 条（见下详解） |
| 协程 | `OPCODE_AWAIT` / `OPCODE_AWAIT_RESUME` | 2 条 |
| Lambda 创建 | `OPCODE_CREATE_LAMBDA` / `OPCODE_CREATE_SELF_LAMBDA` | 2 条 |
| 跳转 | `OPCODE_JUMP` / `JUMP_IF` / `JUMP_IF_NOT` / `JUMP_TO_DEF_ARGUMENT` / `JUMP_IF_SHARED` | 5 条 |
| 返回 | `OPCODE_RETURN` / `OPCODE_RETURN_TYPED_*` | 6 条 |
| 迭代 | `OPCODE_ITERATE_BEGIN_*` / `OPCODE_ITERATE_*`（各 20 条） | 超过 40 条 |
| 类型调整 | `OPCODE_TYPE_ADJUST_*` | 按 Variant::Type 全家桶，约 35 条 |
| 全局存储 | `OPCODE_STORE_GLOBAL` / `OPCODE_STORE_NAMED_GLOBAL` | 2 条 |
| 其它 | `OPCODE_ASSERT` / `OPCODE_BREAKPOINT` / `OPCODE_LINE` / `OPCODE_END` | 4 条 |

Opcode 总数看似吓人，实际 VM 真正"独立"的分支不过 20 来种；其余都是类型特化变体，用于避免运行时判别类型。**读反汇编时只要记住"特化族"背后是同一类操作**，心智负担就小了。

### 8.1.2　调用 Opcode 的 13 条

调用最复杂，单独拎出来：

| Opcode | 场景 |
|---|---|
| `OPCODE_CALL` | 通用运行时调用（没有任何静态信息） |
| `OPCODE_CALL_RETURN` | 同上，但保留返回值 |
| `OPCODE_CALL_ASYNC` | 异步调用，生成 `Signal`，配合 `await` |
| `OPCODE_CALL_UTILITY` | Variant 全局工具函数（`sin`、`abs`），未验证参数 |
| `OPCODE_CALL_UTILITY_VALIDATED` | 同上，已验证（走函数指针） |
| `OPCODE_CALL_GDSCRIPT_UTILITY` | `@GDScript` 内置函数（`print`、`len`） |
| `OPCODE_CALL_BUILTIN_TYPE_VALIDATED` | 内建类型的成员方法（如 `Array.size()`） |
| `OPCODE_CALL_BUILTIN_STATIC` | 内建类型的静态方法（如 `Color.from_hsv`） |
| `OPCODE_CALL_SELF_BASE` | `super.xxx()` |
| `OPCODE_CALL_METHOD_BIND` / `_RET` | 原生 C++ 类实例方法（走 `MethodBind`） |
| `OPCODE_CALL_METHOD_BIND_VALIDATED_RETURN` / `_NO_RETURN` | 同上，参数已校验 |
| `OPCODE_CALL_NATIVE_STATIC` / `_VALIDATED_RETURN` / `_VALIDATED_NO_RETURN` | 原生 C++ 类的静态方法 |

**看一条调用 Opcode 大致能猜出 Analyzer 对这次调用的"把握程度"**——越往下、越窄越"validated"的，Analyzer 对类型掌握越充分。

### 8.1.3　`Address` 编码：32 位压位

`code` 数组里的每个"地址类"操作数都是一个 32 位整数，按以下方式分段：

```cpp
enum Address {
    ADDR_BITS      = 24,
    ADDR_MASK      = (1 << 24) - 1,      // 低 24 位：地址内索引
    ADDR_TYPE_MASK = ~ADDR_MASK,         // 高 8 位：地址类型
    ADDR_TYPE_STACK    = 0,
    ADDR_TYPE_CONSTANT = 1,
    ADDR_TYPE_MEMBER   = 2,
    ADDR_TYPE_MAX      = 3,
};
```

即：

- **高 8 位**：地址"属于哪种池"——栈（包含局部变量、参数、临时、self/class/nil）、常量池、成员变量槽位。
- **低 24 位**：在那个池里的索引。

VM 里经常看到形如：

```cpp
int addr = code[ip++];
int type = (addr & ADDR_TYPE_MASK) >> ADDR_BITS;
int idx  =  addr & ADDR_MASK;

const Variant *v;
switch (type) {
    case ADDR_TYPE_STACK:    v = &stack[idx]; break;
    case ADDR_TYPE_CONSTANT: v = &constants[idx]; break;
    case ADDR_TYPE_MEMBER:   v = &instance->members[idx]; break;
}
```

**这种"类型位 + 索引位"的压位表达让每条地址只花一个 int，同时 VM 解码只需一次按位运算**——是字节码紧凑又快的关键。

### 8.1.4　固定地址：`SELF` / `CLASS` / `NIL`

栈的前 3 个固定槽位是预留的：

```cpp
enum FixedAddresses {
    ADDR_STACK_SELF  = 0,     // self
    ADDR_STACK_CLASS = 1,     // 当前 GDScript 对象
    ADDR_STACK_NIL   = 2,     // 预置的 null 常值
    FIXED_ADDRESSES_MAX = 3,

    ADDR_SELF  = ADDR_STACK_SELF  | (ADDR_TYPE_STACK << ADDR_BITS),
    ADDR_CLASS = ADDR_STACK_CLASS | (ADDR_TYPE_STACK << ADDR_BITS),
    ADDR_NIL   = ADDR_STACK_NIL   | (ADDR_TYPE_STACK << ADDR_BITS),
};
```

这三个地址是"永远有效、每函数都预留"的。`self` 和 `class` 由 VM 在调用时写入第 0/1 槽；`nil` 是一个常驻 `Variant()` 空值，用于需要"显式 null 操作数"的场景。Compiler 生成涉及 `self` 的代码时直接引用 `ADDR_SELF` 这个常量而不用再查表——大大减少指令长度。

### 8.1.5　字节码是 ABI

和 `Token::Type` 类似，**`Opcode` 枚举也是一个 ABI**——尤其是对 `.gdc` 预编译和对"外部工具分析字节码"的场景。GDScript 社区不承诺 Opcode 值跨版本兼容，所以：

1. 开源工具（GDScript 反编译器之类）通常会为每个 Godot 版本分别维护一份 Opcode 表；
2. Godot 本身修改 Opcode 时，同步修改 VM 与 Disassembler，不需要版本号——因为字节码不是持久化格式（`.gdc` 只存 Token，不存字节码）。

## 8.2　`GDScriptFunction::disassemble()`：把字节码翻回文本

反汇编器的实现入口是一个 `GDScriptFunction` 的成员方法：

```cpp
#ifdef DEBUG_ENABLED
void GDScriptFunction::disassemble(const Vector<String> &p_code_lines) const;
#endif
```

它只在 DEBUG 构建里可用——反汇编器会把很多只在 DEBUG 构建里维护的"方法名/属性名/常量名"调试信息（见第 7 章 7.3.3 节的 `operator_names` / `setter_names` 等）印出来，Release 构建这些数组是空的。

### 8.2.1　整体结构

`gdscript_disassembler.cpp` 的核心是一个巨大的 `switch (code[ip])`，对每条 Opcode 手写打印逻辑。伪代码：

```cpp
int ip = 0;
while (ip < code.size()) {
    int line = get_line_for_pc(ip);
    print_header(ip, line);

    switch (code[ip]) {
        case OPCODE_OPERATOR: {
            int op   = code[ip + 1];
            Address dst(code[ip + 2]);
            Address  l (code[ip + 3]);
            Address  r (code[ip + 4]);
            print("OPERATOR %s %s := %s %s %s",
                  operator_name(op), format(dst), format(l),
                  operator_symbol(op), format(r));
            ip += 5;
            break;
        }
        case OPCODE_CALL_GDSCRIPT_UTILITY: {
            int argc = code[ip + 1];
            Address dst(code[ip + 2]);
            int util_idx = code[ip + 3];
            Vector<Address> args = read_n(ip + 4, argc);
            print("CALL_GDSCRIPT_UTILITY %s := %s(%s)",
                  format(dst), gds_utilities_names[util_idx],
                  join_args(args));
            ip += 4 + argc;
            break;
        }
        case OPCODE_JUMP_IF_NOT: {
            Address cond(code[ip + 1]);
            int target = code[ip + 2];
            print("JUMP_IF_NOT %s → %d", format(cond), target);
            ip += 3;
            break;
        }
        case OPCODE_END: {
            print("END");
            ip++;
            break;
        }
        /* ... 160 多个 case ... */
    }
}
```

两个关键辅助：

- **`format(Address)`**：把一个 32 位地址解码成可读字符串，如 `stack[5]:int`、`const[3]=42`、`member[1]:position`。它需要读 Address 的 `mode` 位和 `GDScriptFunction` 的 `stack_debug` / `constants` / `member_indices_cache` 才能给出含义。
- **`get_line_for_pc(ip)`**：用 `code_pos_to_line` 反查当前 PC 对应源码哪一行，可以把反汇编输出和源码对齐显示。

### 8.2.2　反汇编对"池化索引"的解码

看 `OPCODE_CALL_METHOD_BIND_RET` 反汇编时最长见的问题：从字节码只能看到 `method_idx = 3`，这"3"对应哪个方法？Disassembler 会查 `function->methods[3]` 取出 `MethodBind *`，再调它的 `get_name()` 打印出人类可读的方法名：

```
CALL_METHOD_BIND_RET stack[7]:Variant := stack[3]:Node2D.get_parent()
```

Debug 构建里**每个特化池都额外维护一份"_names"索引**（`builtin_methods_names`、`utilities_names`、`gds_utilities_names`……）——这样反汇编器不用在 Release 数据结构里"逆向拼凑"出名字。这也是前几章频繁提到"DEBUG 构建额外调试信息"的来源。

### 8.2.3　行号注释

反汇编器在每一条 Opcode 前输出当前源码行号（以及源码文本片段，如果调用者通过 `p_code_lines` 传入了整脚本的源码）：

```
# Line 12: if x > 0:
 15: OPERATOR stack[3]:bool := stack[2]:int > const[0]=0
 20: JUMP_IF_NOT stack[3]:bool → 27
# Line 13:     x = 1
 23: ASSIGN stack[2]:int := const[1]=1
 26: JUMP → 30
 ...
```

这让反汇编不仅告诉你"字节码长什么样"，还告诉你"每条字节码对应源码哪一行"——对性能分析和断点调试都极其友好。

### 8.2.4　触发反汇编

用户侧有两种触发反汇编的办法：

1. **开启 `debug/gdscript/warnings/exclude_addons`** 之类选项后，GDScript 加载某些脚本会自动打印 disasm（需 DEBUG 构建）；
2. **在代码里手动调** `func->disassemble(get_source_code_lines())`，通常写在测试里。

Godot 的 GDScript 测试框架（`modules/gdscript/tests/`）就用这套把某些脚本的 disasm 文本和黄金答案比对——你要修改 Opcode 或 CodeGen，跑一遍这些测试就能看到字节码是否改变了。

## 8.3　实战：读三段反汇编

### 例 ① 最简单的算术

源码：

```gdscript
func add(a: int, b: int) -> int:
    return a + b
```

反汇编（简化、行号略）：

```
FUNCTION add(int, int) -> int
 stack layout: [ self, class, nil, a:int, b:int ]

 0: OPERATOR_VALIDATED stack[5]:int := stack[3]:int + stack[4]:int
                       op_func=int_add
 5: RETURN_TYPED_BUILTIN stack[5]:int

END
```

读解：

- Analyzer 确认参数与返回都是 `int`，Compiler 走 `write_binary_operator` 的 **validated** 路径，Opcode 是 `OPERATOR_VALIDATED`——结尾第 5 个参数是 `operator_funcs` 池索引，`disassemble()` 解出是"int 加法的 C 函数指针"。
- 返回走 `OPCODE_RETURN_TYPED_BUILTIN` 而非泛用 `OPCODE_RETURN`，VM 在返回时顺便做一次类型校验（虽然编译期已保证，但硬类型返回需运行时再确认）。

对比**无类型版本**：

```gdscript
func add(a, b):
    return a + b
```

反汇编会变成：

```
 0: OPERATOR ADD stack[5]:Variant := stack[3]:Variant + stack[4]:Variant
 5: RETURN stack[5]:Variant
```

——用通用 `OPCODE_OPERATOR`（五个操作数的头一个是 `Variant::Operator` 值）和通用 `OPCODE_RETURN`。VM 要为每次加法走一次完整的类型分派 + 非特化加法。这就是"加上类型注解会变快"的 disasm 级证据。

### 例 ② 对象方法调用

源码：

```gdscript
func nudge(n: Node2D) -> void:
    n.position += Vector2(1, 0)
```

反汇编（略有简化）：

```
 # n.position 的取值
 0: GET_NAMED_VALIDATED stack[5]:Vector2 := stack[3]:Node2D["position"]
                        getter=Node2D::get_position

 # Vector2(1, 0) 的构造
 4: CONSTRUCT_VALIDATED stack[6]:Vector2 := Vector2(const[0]=1, const[1]=0)
                        ctor=Vector2(float, float)

 # 加法
 9: OPERATOR_VALIDATED stack[7]:Vector2 := stack[5]:Vector2 + stack[6]:Vector2
                        op_func=Vector2_add_Vector2

 # 写回 n.position
14: SET_NAMED_VALIDATED stack[3]:Node2D["position"] := stack[7]:Vector2
                        setter=Node2D::set_position

18: RETURN nil
```

读解：

- 每个"读/构造/运算/写"都走到了 validated 分支。Analyzer 能看到 `n: Node2D`、`position` 的类型是 `Vector2`、`Vector2 + Vector2 -> Vector2`，所以 Compiler 全部生成特化版本。
- 如果 `n` 是 Variant（没有类型注解），`GET_NAMED_VALIDATED` 会退化为 `GET_NAMED`——它的第 4 个操作数是一个 `name_map` 索引，VM 要运行时从 base 的类型反查 getter。

### 例 ③ 控制流

源码：

```gdscript
func fizz(n: int) -> String:
    if n % 3 == 0:
        return "fizz"
    return str(n)
```

反汇编：

```
 0: OPERATOR_VALIDATED stack[5]:int  := stack[3]:int % const[0]=3   op_func=int_mod
 5: OPERATOR_VALIDATED stack[6]:bool := stack[5]:int == const[1]=0  op_func=int_eq
10: JUMP_IF_NOT stack[6]:bool → 16
13: RETURN_TYPED_BUILTIN const[2]="fizz"
16: CALL_GDSCRIPT_UTILITY stack[7]:String := str(stack[3]:int)
21: RETURN_TYPED_BUILTIN stack[7]:String
END
```

读解：

- `if` 分支由 `JUMP_IF_NOT` 跳到 else 位置——第 16 条，也就是"return str(n)"。
- 没有 else 分支时没有额外 `JUMP`：第一个 return 后的流程直接落到第 16 条的位置，不需要回填跳转。这就是 Compiler/CodeGen 针对"无 else 的 if" 的一个小优化。
- `str` 是 `@GDScript` 内建函数，走 `CALL_GDSCRIPT_UTILITY`。

## 8.4　"类型特化族"的阅读要点

回看 Opcode 列表里的 `OPCODE_ITERATE_BEGIN_*` 和 `OPCODE_ITERATE_*`——它们几乎覆盖了 Variant 所有容器类型。原因是 `for` 循环的性能热度极高，每种容器的迭代器状态不同，VM 用类型特化避免任何运行时类型分支。

反汇编里看到 `OPCODE_ITERATE_BEGIN_PACKED_VECTOR3_ARRAY` 这种长名字**反而是好消息**——它意味着 Analyzer 精确知道数组元素是 `Vector3`、Compiler 生成了最快路径。若是 `OPCODE_ITERATE_BEGIN`（无后缀）配合 `OPCODE_ITERATE`，说明运行时还要走一次类型判别。

同样：

- `OPCODE_ASSIGN_TYPED_*` 族：赋值前校验源值是否满足目标类型（Typed GDScript 的安全赋值）。
- `OPCODE_TYPE_ADJUST_*` 族：把一个 Variant"就地调整"成声明的类型（如 `var x: int = 3.5` 会先把 `3.5` `TYPE_ADJUST_INT` 成 `3`）。
- `OPCODE_RETURN_TYPED_*` 族：类型化返回，返回前再校验一次。

这些特化族共同构成了 GDScript "**硬类型的边界**"——编译期与运行时共同守卫用户注解的承诺。

## 8.5　把反汇编当成调试工具

下面列几个实战场景，能让你意识到反汇编不仅是"玩具"，而是**第一手诊断手段**：

1. **性能怀疑**：一段 for 循环跑得比预期慢。看 disasm，如果 `ITERATE` 是无后缀的，说明 Analyzer 没推出容器元素类型——加一行 `Array[Vector3]` 注解，disasm 立刻变成 `_PACKED_VECTOR3_ARRAY` 特化。
2. **修改 Compiler**：你加了一个新的 validated 调用路径。跑一遍测试、看 disasm，若同样一段代码原来是 `CALL_METHOD_BIND_RET`，现在变成了 `CALL_METHOD_BIND_VALIDATED_RETURN`，说明修改成功。
3. **追踪 Opcode 行为**：VM 某条 Opcode 里怀疑有 bug。用 disasm 把整个函数打出来，定位到可疑 Opcode 的操作数含义、对照源码判断。
4. **逆向学习**：想搞清楚某种 GDScript 语法糖（如 Lambda 捕获）在运行时是怎么走的——写个最小例子，看 disasm 就知道了。

## 8.6　为什么字节码不持久化

和 Python 的 `.pyc`、Java 的 `.class` 不同，Godot 不在磁盘上存**字节码**——它只存 Token（`.gdc`）。原因：

1. **版本演进频繁**：Opcode 集合经常调整以加入新的特化路径。持久化字节码会强迫维持跨版本兼容，代价太高。
2. **Token 压缩效率更高**：`.gdc` 重新编译很快（Tokenizer 已经省掉最大的词法工作，Parser 是小头），存 Token 文件又小又"前向兼容"。
3. **与原生调试信息耦合**：字节码里引用了大量"运行期才确定"的 MethodBind 指针、GDScriptFunction 指针、类型指针——这些跨进程/跨版本都是无效的，持久化无意义。

这就是为什么**"字节码 ABI"对 GDScript 内核而言比"Token ABI"宽松得多**——字节码只是一次 Godot 启动范围内的"中间产物"。

## 本章小结

- `GDScriptFunction::code` 是一个扁平的 `Vector<int>`，Opcode 与操作数并列排布。操作数多为 32 位压位的 `Address`（高 8 位类型 + 低 24 位索引）。
- Opcode 总数超过 170，但大量是"类型特化族"（`ITERATE_*`、`TYPE_ADJUST_*`、`RETURN_TYPED_*`、各种 `*_VALIDATED`）。读 disasm 时把它们按族看待。
- 调用家族的 13 条 Opcode 覆盖从"完全动态"到"完全 validated"的谱，对应 Analyzer 掌握类型信息的多寡。
- **反汇编器是 DEBUG 构建专属工具**，靠 `GDScriptFunction` 里 DEBUG 构建才维护的若干 `*_names` 数组把索引翻译回人类可读的方法/属性名。
- 反汇编输出按 PC 线性走，每条指令一行，**常与源码行号对齐显示**——这让反汇编成为做性能分析、验证 Compiler 修改、理解语法糖落地的首选工具。
- 字节码不持久化：这是 Godot 的工程决策，保证 Opcode 集合可以自由演进，只有 Token（`.gdc`）承担持久化 ABI。

至此，GDScript 的**前端 + 中端**已经讲完。从下一章起我们进入**运行时**：字节码就绪之后，VM 是怎样把它执行起来的？调用栈长什么样？Opcode 是如何被分发的？我们将先看 `GDScriptFunction` 的数据结构（第 9 章），然后进入 `GDScriptFunction::call()` 这一 VM 主循环（第 10 章）。

---

[← 上一章：第 7 章 字节码生成](./07-bytecode-generator.md) · [目录](./README.md) · [下一章：第 9 章 可执行函数：`GDScriptFunction` →](./09-gdscript-function.md)
