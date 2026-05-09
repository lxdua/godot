# 附录 A　完整 Mini 语法 BNF

下面是 Mini 语言的完整文法定义，使用扩展 BNF 描述。读法：

* `[X]` 表示 0 或 1 次；
* `{X}` 表示 0 或多次；
* `X | Y` 表示二选一；
* 全大写如 `IDENT`、`NUMBER` 是终结符 token。

---

## A.1 程序结构

```ebnf
program        = { statement } ;

statement      = let_stmt
               | assign_stmt
               | if_stmt
               | while_stmt
               | for_stmt
               | return_stmt
               | break_stmt
               | continue_stmt
               | fn_decl
               | expr_stmt ;
```

## A.2 声明与赋值

```ebnf
let_stmt       = "let" IDENT "=" expression ;

assign_stmt    = lvalue "=" expression ;

lvalue         = IDENT
               | primary "." IDENT
               | primary "[" expression "]" ;
```

## A.3 控制流

```ebnf
if_stmt        = "if" expression "then" { statement }
                 { "elif" expression "then" { statement } }
                 [ "else" { statement } ]
                 "end" ;

while_stmt     = "while" expression "do" { statement } "end" ;

for_stmt       = "for" IDENT "in" expression "do" { statement } "end" ;

return_stmt    = "return" [ expression ] ;

break_stmt     = "break" ;
continue_stmt  = "continue" ;
```

## A.4 函数

```ebnf
fn_decl        = "fn" IDENT "(" [ param_list ] ")" { statement } "end" ;

param_list     = IDENT { "," IDENT } ;

fn_expr        = "fn" "(" [ param_list ] ")" { statement } "end" ;
```

## A.5 表达式

按优先级从低到高（Pratt parser 直接对应）：

```ebnf
expression     = or_expr ;

or_expr        = and_expr  { "or"  and_expr } ;
and_expr       = eq_expr   { "and" eq_expr  } ;
eq_expr        = cmp_expr  { ( "==" | "!=" ) cmp_expr } ;
cmp_expr       = add_expr  { ( "<" | "<=" | ">" | ">=" ) add_expr } ;
add_expr       = mul_expr  { ( "+" | "-" | ".." ) mul_expr } ;
mul_expr       = unary     { ( "*" | "/" | "%" ) unary } ;
unary          = ( "-" | "not" ) unary
               | call ;

call           = primary { call_suffix } ;

call_suffix    = "(" [ arg_list ] ")"
               | "." IDENT
               | "[" expression "]" ;

arg_list       = expression { "," expression } ;

primary        = NUMBER
               | STRING
               | "true" | "false" | "nil"
               | IDENT
               | "(" expression ")"
               | table_literal
               | fn_expr ;
```

## A.6 字面量

```ebnf
table_literal  = "{" [ table_entries ] "}" ;

table_entries  = table_entry { "," table_entry } [ "," ] ;

table_entry    = expression                       (* array-style *)
               | "[" expression "]" "=" expression  (* keyed *)
               | IDENT "=" expression ;             (* keyed shorthand *)

NUMBER         = digit { digit } [ "." digit { digit } ]
                 [ ("e"|"E") ["+"|"-"] digit { digit } ] ;

STRING         = '"' { ~'"' | '\\' any } '"'
               | "'" { ~"'" | "\\" any } "'" ;

IDENT          = letter { letter | digit | "_" } ;
                 (* 不能是关键字 *)

letter         = "a".."z" | "A".."Z" | "_" ;
digit          = "0".."9" ;
```

## A.7 关键字保留字

```
let     fn     return     end
if      elif   else       then
while   do     for        in
break   continue
true    false  nil
and     or     not
```

## A.8 注释

```ebnf
comment        = "#"  { ~"\n" }      (* 行注释 *)
               | "--" { ~"\n" } ;    (* 兼容 Lua 风格 *)
```

注释在 lexer 阶段直接丢弃，不出现在 token 流。

## A.9 运算符优先级与结合性总表

从低到高：

| 优先级 | 运算符 | 结合性 |
|--------|--------|--------|
| 1 | `or` | 左 |
| 2 | `and` | 左 |
| 3 | `==` `!=` | 左 |
| 4 | `<` `<=` `>` `>=` | 左 |
| 5 | `+` `-` `..` | 左 |
| 6 | `*` `/` `%` | 左 |
| 7 | 前缀 `-` `not` | 右 |
| 8 | 后缀 `(...)` `.field` `[expr]` | 左 |

赋值不是表达式（不像 C），所以没出现在表达式优先级表里——`x =
y = 0` 在 Mini 里是语法错误。

## A.10 语义保证

文法之外的几条静态约束（由 parser/analyzer 检查）：

* `break` / `continue` 只能出现在 `while` / `for` 内；
* `return` 只能出现在函数体内；
* 同名 local 在同 scope 内重复 `let` 是错误；
* `let x = x` 是错误（Mini 比 Rust 严格——RHS 不能引用 LHS 自
  己）；
* 函数最多 65535 个常量、255 个 local（受 24-bit 指令编码限制）。

