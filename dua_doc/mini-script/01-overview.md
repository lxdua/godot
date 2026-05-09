# 第 1 章　总览：我们要做什么 & 项目骨架

写一门脚本语言听起来吓人，其实拆开来只有几个固定模块。这一章不
写一行求值代码，但会把整个项目的**目录、依赖、命名约定、第一次
能跑的 main 函数**全部敲定。后续每一章都在这个骨架上往前堆。

## 1.1 一门脚本语言由什么组成

在最朴素的视角下，"运行一段源代码"就是这个流水线：

```
"a = 1 + 2"  ──┐
               ▼
           ┌────────┐
           │ Lexer  │   把字符流切成 token：[IDENT(a)] [=] [NUM(1)] [+] [NUM(2)]
           └───┬────┘
               ▼
           ┌────────┐
           │ Parser │   按文法构造 AST：Assign(a, BinOp(+, 1, 2))
           └───┬────┘
               ▼
       ┌────────────────┐
       │  Interpreter   │   求值：把 AST "跑"成结果
       │  或 Compiler   │   或编译：把 AST "翻译"成字节码
       └───┬────────────┘
           ▼
        结果 / 输出
```

这条流水线的每一段都可以独立写、独立测，这也是我们安排目录的依据。

## 1.2 Mini 是什么样

为了不让这本书变成"什么都讲一点但什么都没做完"的状态，我们先把
目标语言冻结。语法上 Mini 取自 Lua + Python 的中间状态：

```python
# 注释从 # 开始，一直到行末

let x = 10              # 变量声明
let pi = 3.14
let name = "mini"
let ok = true
let nothing = nil

# 算术、比较、逻辑
let y = (x + 2) * 3
let big = x > 5 and not ok

# 控制流
if x > 0 then
    print("positive")
elif x == 0 then
    print("zero")
else
    print("negative")
end

while x > 0 do
    x = x - 1
end

for i in 0..10 do          # 半开区间
    print(i)
end

# 函数与闭包
fn make_counter()
    let n = 0
    fn inc()
        n = n + 1
        return n
    end
    return inc
end

let c = make_counter()
print(c())   # 1
print(c())   # 2

# table：兼用作数组与字典
let arr = [1, 2, 3]
let dict = {"a": 1, "b": 2}
print(arr[0])
print(dict["a"])

# 字符串拼接
print("hello, " .. name)
```

故意做的几个选择：

* **`then`/`do`/`end` 而不是大括号**：避免 token 表里再加 `{ }`，
  也让 parser 写起来更直观——end 一律收尾；
* **`let` 关键字**：和赋值区分，第一次出现的变量必须 `let`，再次
  赋值不加 `let`。这避免了"作用域偷偷创建"的隐式问题，也让 parser
  能直接判断"这是声明还是写"；
* **`..` 字符串拼接**：抄 Lua，避开 `+` 在数值/字符串间的歧义；
* **`0..10` 半开区间 for**：抄 Rust，写起来比 Python 的 `range`
  少敲几个字。

完整 BNF 见附录 A，写 parser 时再展开。

## 1.3 项目骨架

```
mini/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── main.cpp            # 入口：REPL / run file
│   ├── common.h            # 共享类型别名、宏
│   ├── token.h             # TokenType 枚举 + Token 结构
│   ├── lexer.h / .cpp
│   ├── ast.h               # AST 节点定义
│   ├── parser.h / .cpp
│   ├── value.h             # 第二部分加入
│   ├── env.h               # 第二部分加入
│   ├── interpreter.h/.cpp  # 第二部分：树遍历解释器
│   ├── opcode.h            # 第三部分：字节码 VM
│   ├── compiler.h/.cpp     # 第三部分
│   └── vm.h / .cpp         # 第三部分
├── tests/
│   ├── lex_test.cpp
│   ├── parse_test.cpp
│   ├── interp_test.cpp
│   └── vm_test.cpp
└── examples/
    ├── fib.mini
    ├── closure.mini
    └── repl_demo.mini
```

注意几件小事：

* **`.h` / `.cpp` 配对，不写 header-only**：编译时间友好，错误信
  息可读；
* **每个模块单测**：手写脚本语言最容易"前面错后面爆"，分层测试是
  唯一能保命的方式；
* **examples/ 收集真能跑的脚本**：每写完一个特性就往里加一个例子，
  自动跑 examples/ 是低成本的回归测试。

## 1.4 一个能编译过的第一版 main

我们先把**整个项目能 build、能跑、能打印**这件事先解决，之后每章
都是往里填模块。

`CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.16)
project(mini CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_executable(mini
    src/main.cpp
)
```

`src/main.cpp`：

```cpp
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace mini {

static int run_source(const std::string& source, const std::string& name) {
    // TODO(ch.2): lexer
    // TODO(ch.3): parser
    // TODO(ch.8/17): evaluate or execute
    std::cout << "[" << name << "] " << source.size()
              << " bytes (run not implemented yet)\n";
    return 0;
}

static int run_file(const char* path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return run_source(ss.str(), path);
}

static int run_repl() {
    std::string line;
    std::cout << "Mini REPL. Ctrl+D to exit.\n";
    while (true) {
        std::cout << ">>> ";
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            return 0;
        }
        if (line.empty()) {
            continue;
        }
        run_source(line, "<repl>");
    }
}

}  // namespace mini

int main(int argc, char** argv) {
    if (argc == 1) {
        return mini::run_repl();
    }
    if (argc == 2) {
        return mini::run_file(argv[1]);
    }
    std::cerr << "usage: mini [file.mini]\n";
    return 64;
}
```

```bash
$ cmake -S . -B build && cmake --build build
$ ./build/mini examples/fib.mini
[examples/fib.mini] 142 bytes (run not implemented yet)
```

到这里你已经有一个能跑、能加模块、能写测试的项目了。

## 1.5 命名约定与几条编码原则

后续代码里我们会一直遵循这几条小约定，提前说清省得后面反复解释：

1. **所有公开类型放 `mini` 命名空间**——避免污染全局；
2. **AST/Token 节点用值类型 + `std::unique_ptr` 拥有子节点**——别
   一上来就 `shared_ptr`，会把所有权关系搅成一锅；
3. **错误处理用 `throw mini::Error`**——脚本语言的错误数量有限、
   层级深，异常比错误码更适合；后面我们会加上行号/列号；
4. **`size_t` 处理索引、`int` 处理"行号、列号"这种带 `-1` 哨兵的
   值**——不要让 unsigned 偷偷绕回来；
5. **不要早优化**——第一版能跑就行，第三部分再回头谈性能。

---

下一章我们正式开始写 Lexer：把 `let x = 1 + 2` 这样的字符串切成
`[LET] [IDENT(x)] [EQUAL] [NUMBER(1)] [PLUS] [NUMBER(2)]`，并配上
最早的一组单元测试。
