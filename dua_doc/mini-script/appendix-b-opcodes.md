# 附录 B　Opcode 速查表

Mini VM 的全部 35 条 opcode。指令编码 32 bit：

```
| Opcode (8 bit) | Argument / Offset (24 bit) |
```

`Argument` 字段对不同 op 有不同含义：

* **uarg** 表示无符号 24 位整数（slot 下标、常量池下标、参数个
  数等）；
* **sarg** 表示有符号 24 位整数（jump offset、立即数）。

对没用到 argument 的 op，低 24 位忽略。

## B.1 栈操作

| Op | 参数 | 栈效果 (前→后) | 说明 |
|----|------|--------------|------|
| `OP_PUSH_NIL`     | -     | `... → ..., nil`     | 压入 nil |
| `OP_PUSH_TRUE`    | -     | `... → ..., true`    | 压入 true |
| `OP_PUSH_FALSE`   | -     | `... → ..., false`   | 压入 false |
| `OP_PUSH_INT`     | sarg  | `... → ..., sarg`    | 压入小整数（24 位） |
| `OP_LOAD_CONST`   | uarg  | `... → ..., k[u]`    | 压入常量池第 u 项 |
| `OP_POP`          | -     | `..., x → ...`       | 丢弃栈顶 |
| `OP_DUP`          | -     | `..., x → ..., x, x` | 复制栈顶 |

## B.2 局部变量

| Op | 参数 | 栈效果 | 说明 |
|----|------|--------|------|
| `OP_LOAD_LOCAL`     | uarg | `... → ..., L[u]`     | 读 local u |
| `OP_STORE_LOCAL`    | uarg | `..., x → ..., x`     | 写 local u（保留栈顶） |
| `OP_STORE_LOCAL_POP`| uarg | `..., x → ...`        | 写 local u 并 pop |
| `OP_CLOSE_UPVALUE`  | uarg | `... → ...`           | 关闭 local u 的 open upvalue |

## B.3 全局变量

| Op | 参数 | 栈效果 | 说明 |
|----|------|--------|------|
| `OP_LOAD_GLOBAL`    | uarg | `... → ..., G[k[u]]` | 按名读全局 |
| `OP_STORE_GLOBAL`   | uarg | `..., x → ..., x`    | 按名写全局（保留栈顶） |
| `OP_DEFINE_GLOBAL`  | uarg | `..., x → ...`       | 定义新全局并 pop |

`k[u]` 表示常量池第 u 项（一定是 string）。

## B.4 Upvalue（闭包）

| Op | 参数 | 栈效果 | 说明 |
|----|------|--------|------|
| `OP_LOAD_UPVALUE`   | uarg | `... → ..., U[u]`    | 读 upvalue u |
| `OP_STORE_UPVALUE`  | uarg | `..., x → ..., x`    | 写 upvalue u（保留栈顶） |
| `OP_CLOSURE`        | uarg | `... → ..., closure` | 用 k[u]（FunctionProto）实例化闭包 |

## B.5 算术与字符串

| Op | 参数 | 栈效果 | 说明 |
|----|------|--------|------|
| `OP_ADD`    | - | `..., a, b → ..., a+b` | 数值加 |
| `OP_SUB`    | - | `..., a, b → ..., a-b` | |
| `OP_MUL`    | - | `..., a, b → ..., a*b` | |
| `OP_DIV`    | - | `..., a, b → ..., a/b` | |
| `OP_MOD`    | - | `..., a, b → ..., a%b` | |
| `OP_NEG`    | - | `..., a → ..., -a`     | 一元负 |
| `OP_CONCAT` | - | `..., a, b → ..., a..b`| 字符串拼接 |

整型/浮点之间的提升规则在 VM 里实现，bytecode 不区分。

## B.6 比较与逻辑

| Op | 参数 | 栈效果 | 说明 |
|----|------|--------|------|
| `OP_EQ`  | - | `..., a, b → ..., a==b` | |
| `OP_NE`  | - | `..., a, b → ..., a!=b` | |
| `OP_LT`  | - | `..., a, b → ..., a<b`  | |
| `OP_LE`  | - | `..., a, b → ..., a<=b` | |
| `OP_GT`  | - | `..., a, b → ..., a>b`  | |
| `OP_GE`  | - | `..., a, b → ..., a>=b` | |
| `OP_NOT` | - | `..., a → ..., !a`      | 逻辑非（`a` 按 truthy 取反） |

`and` / `or` **没有专门 opcode**——编译器用 JUMP_IF_FALSE +
POP 模式实现短路。

## B.7 跳转

| Op | 参数 | 栈效果 | 说明 |
|----|------|--------|------|
| `OP_JUMP`              | sarg | `... → ...`     | ip += sarg |
| `OP_JUMP_IF_FALSE`     | sarg | `..., x → ..., x` | x falsy 时跳，**保留栈顶** |
| `OP_JUMP_IF_FALSE_POP` | sarg | `..., x → ...`    | x falsy 时跳，无论如何都 pop |

跳转 offset 是相对当前 ip 的有符号偏移（执行 fetch++ 之后）。

## B.8 表

| Op | 参数 | 栈效果 | 说明 |
|----|------|--------|------|
| `OP_NEW_TABLE` | -    | `... → ..., {}`        | 创建新空 table |
| `OP_GET_INDEX` | -    | `..., obj, k → ..., v` | obj[k] |
| `OP_SET_INDEX` | -    | `..., obj, k, v → ..., v` | obj[k] = v，留 v 在栈上 |

`obj.field` 在编译期翻译成 `OP_LOAD_CONST "field" + OP_GET_INDEX`
——没有专门的 GET_FIELD（简化指令集）。如果做 inline cache 优
化可以加一条 OP_GET_FIELD_CACHED。

## B.9 函数调用

| Op | 参数 | 栈效果 | 说明 |
|----|------|--------|------|
| `OP_CALL`        | uarg | `..., fn, a0..a_{u-1} → ..., result` | 调用 fn，传 u 个参数 |
| `OP_RETURN`      | -    | `..., x → ...` (跨帧)     | 返回栈顶值 |
| `OP_RETURN_VOID` | -    | `... → ...` (跨帧)        | 返回 nil |

调用约定：caller 先 push 函数对象，再 push 参数，发 `OP_CALL u`。
返回时 callee 那个槽被返回值替换。

## B.10 调试

| Op | 参数 | 栈效果 | 说明 |
|----|------|--------|------|
| `OP_LINE`  | sarg | `... → ...` | 设置 current_line（无运行作用） |
| `OP_HALT`  | -    | `... → x`   | 跳出 main_loop（main 函数末尾） |

`OP_LINE` 在 release 构建可移除，行号信息走单独的 line table
（见 20.6 节）。

## B.11 完整 Opcode 编号

```cpp
enum Opcode : std::uint8_t {
    // 栈操作
    OP_PUSH_NIL = 0,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_PUSH_INT,
    OP_LOAD_CONST,
    OP_POP,
    OP_DUP,

    // 局部变量
    OP_LOAD_LOCAL,
    OP_STORE_LOCAL,
    OP_STORE_LOCAL_POP,
    OP_CLOSE_UPVALUE,

    // 全局变量
    OP_LOAD_GLOBAL,
    OP_STORE_GLOBAL,
    OP_DEFINE_GLOBAL,

    // Upvalue
    OP_LOAD_UPVALUE,
    OP_STORE_UPVALUE,
    OP_CLOSURE,

    // 算术
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NEG, OP_CONCAT,

    // 比较与逻辑
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE, OP_NOT,

    // 跳转
    OP_JUMP, OP_JUMP_IF_FALSE, OP_JUMP_IF_FALSE_POP,

    // 表
    OP_NEW_TABLE, OP_GET_INDEX, OP_SET_INDEX,

    // 调用
    OP_CALL, OP_RETURN, OP_RETURN_VOID,

    // 调试
    OP_LINE, OP_HALT,

    OPCODE_COUNT,
};
```

35 条——和 Lox 同量级，比 Lua 5.4 的 81 条少一半（Lua 多在
register-based + specialized op）。

## B.12 反汇编输出格式参考

回顾第 20 章的 disassembler 输出格式：

```
0000  OP_PUSH_INT          5
0001  OP_LOAD_CONST        2  ; "hello"
0002  OP_LOAD_LOCAL        0
0003  OP_CALL              argc=2
0004  OP_JUMP_IF_FALSE_POP +4  -> 9
0005  OP_LOAD_GLOBAL       1  ; "print"
...
```

格式约定：

* ip 用 4 位 0 填充十进制；
* opcode 名左对齐占 20 字符；
* 注释 `;` 后面给出常量值/字符串值；
* jump 同时给相对偏移和绝对目标 ip。

