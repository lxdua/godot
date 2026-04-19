# Godot WorkerThreadPool 深度解析

> 源码位置：`core/object/worker_thread_pool.h` / `core/object/worker_thread_pool.cpp`

## 目录

1. [前置知识：多线程基础概念](#1-前置知识多线程基础概念)
2. [WorkerThreadPool 是什么](#2-workerthreadpool-是什么)
3. [整体架构](#3-整体架构)
4. [核心数据结构](#4-核心数据结构)
5. [任务的生命周期](#5-任务的生命周期)
6. [两种任务模型](#6-两种任务模型)
7. [线程调度机制](#7-线程调度机制)
8. [协作等待机制](#8-协作等待机制)
9. [死锁防御](#9-死锁防御)
10. [Pump Task（泵任务）](#10-pump-task泵任务)
11. [Yield 机制](#11-yield-机制)
12. [可解锁互斥量区域](#12-可解锁互斥量区域)
13. [Runlevel 生命周期管理](#13-runlevel-生命周期管理)
14. [GDScript API](#14-gdscript-api)
15. [实际应用场景](#15-实际应用场景)
16. [总结](#16-总结)

---

## 1. 前置知识：多线程基础概念

在深入 WorkerThreadPool 之前，我们需要先理解几个多线程编程中的核心概念。

### 1.1 线程（Thread）

**线程**是操作系统能够调度的最小执行单元。一个进程可以包含多个线程，它们共享同一块内存空间。

```
进程（Godot 编辑器）
├── 主线程 (Main Thread)      → 处理 UI、场景树
├── 渲染线程 (Render Thread)   → 提交 GPU 命令
├── 工作线程 0 (Worker 0)      → 执行任务
├── 工作线程 1 (Worker 1)      → 执行任务
└── 工作线程 N (Worker N)      → 执行任务
```

**为什么需要多线程？** 现代 CPU 有多个核心（如 8 核 16 线程），单线程只能用 1 个核心，多线程可以同时利用所有核心，成倍提升性能。

### 1.2 互斥量（Mutex）

当多个线程同时读写同一块数据时，会产生**数据竞争**（Data Race），导致不可预测的结果。

```cpp
// 危险！两个线程同时执行这段代码
int counter = 0;

// 线程 A                    // 线程 B
counter++;                   counter++;
// 读取 counter = 0          // 读取 counter = 0
// 计算 0 + 1 = 1            // 计算 0 + 1 = 1
// 写回 counter = 1          // 写回 counter = 1
// 最终结果：counter = 1，而不是期望的 2！
```

**互斥量（Mutex）** 就是一把"锁"，同一时刻只有一个线程能持有它：

```cpp
Mutex mutex;
// 线程 A                    // 线程 B
mutex.lock();                mutex.lock();    // 被阻塞，等待 A 释放
counter++;                   // ...等待中...
mutex.unlock();              counter++;       // A 释放后才能执行
                             mutex.unlock();
// 最终结果：counter = 2 ✓
```

Godot 中使用 `BinaryMutex`（轻量级互斥量）和 `MutexLock`（RAII 自动解锁包装器）。

### 1.3 信号量（Semaphore）

信号量是一个**带计数器的等待/通知机制**：

- `wait()`：计数器 > 0 则减 1 继续执行；计数器 = 0 则阻塞等待
- `post()`：计数器加 1，唤醒一个等待中的线程

```
信号量 (初始值 = 0)

生产者线程:                     消费者线程:
  准备好数据                      sem.wait()  ← 阻塞，因为计数=0
  sem.post()  → 计数变为1  →     被唤醒，继续执行
```

在 WorkerThreadPool 中，信号量用于：
- 通知"任务完成"（`done_semaphore`）
- 通知"Group 任务全部完成"（`group->done_semaphore`）

### 1.4 条件变量（Condition Variable）

条件变量比信号量更灵活，它允许线程在**持有锁的情况下**等待某个条件成立：

```cpp
// 等待方:
mutex.lock();
while (!condition_met) {
    cond_var.wait(mutex);  // 自动释放锁 → 睡眠 → 被唤醒后重新获取锁
}
// 条件满足，继续执行
mutex.unlock();

// 通知方:
mutex.lock();
condition_met = true;
cond_var.notify_one();  // 唤醒一个等待的线程
mutex.unlock();
```

在 WorkerThreadPool 中，每个工作线程都有自己的条件变量，用于"有新任务时唤醒"。

### 1.5 原子操作（Atomic Operation）

原子操作是**不可被中断的操作**，不需要锁就能保证线程安全：

```cpp
SafeNumeric<uint32_t> counter;  // Godot 的原子整数

counter.increment();     // 原子地 +1，多线程安全
counter.postincrement(); // 原子地返回旧值并 +1
counter.get();           // 原子地读取
```

比 Mutex 更快（纳秒级 vs 微秒级），但只适用于简单的整数操作。

### 1.6 线程池（Thread Pool）

创建和销毁线程是**昂贵的操作**（微秒到毫秒级），如果每次需要并行工作都创建新线程，开销很大。

**线程池**的思路是：启动时创建一组线程，让它们循环等待任务。有任务时唤醒线程执行，执行完后回到等待状态，而不是销毁。

```
线程池 (4个工作线程)
┌─────────────────────────────────────────────┐
│  Worker 0: 睡眠 → 执行任务A → 睡眠 → ...    │
│  Worker 1: 睡眠 → 执行任务B → 执行任务D → ...│
│  Worker 2: 执行任务C → 睡眠 → ...            │
│  Worker 3: 睡眠 → 睡眠 → 执行任务E → ...     │
│                                              │
│  任务队列: [F] [G] [H] ...                   │
└─────────────────────────────────────────────┘
```

WorkerThreadPool 就是 Godot 的线程池实现。

---

## 2. WorkerThreadPool 是什么

`WorkerThreadPool` 是 Godot 引擎中的 **全局线程池单例**（Singleton），负责管理所有并行任务的调度和执行。它是一个继承自 `Object` 的类，可以通过 `WorkerThreadPool::get_singleton()` 获取（GDScript 中直接用 `WorkerThreadPool`）。

### 设计目标

1. **统一的并行入口**：引擎内部所有需要并行的地方（渲染、物理、场景处理等）都通过它提交任务
2. **简单的 API**：提交任务、等待完成，仅此而已
3. **安全性**：内建死锁检测和防御机制
4. **效率**：避免频繁创建/销毁线程的开销

### 一句话总结

> 你把一段代码（任务）丢给 WorkerThreadPool，它会找一个空闲的工作线程来执行，你可以选择等它做完，也可以不管它。

---

## 3. 整体架构

```
                        ┌─────────────────────────────────────────────┐
                        │           WorkerThreadPool (单例)            │
                        │                                             │
  ┌──────────┐          │   ┌──────────────┐   ┌───────────────────┐  │
  │ 主线程   │──add_task──→│  高优先级队列  │   │  低优先级队列     │  │
  │ 渲染线程 │          │   │  task_queue   │   │  low_priority_    │  │
  │ 物理线程 │          │   │  (SelfList)   │   │  task_queue       │  │
  │ ...      │          │   └──────┬───────┘   └──────┬────────────┘  │
  └──────────┘          │          │                   │               │
                        │          ▼                   │ (有空位时提升) │
                        │   ┌──────────────────────────▼──┐           │
                        │   │        工 作 线 程 组         │           │
                        │   │                              │           │
                        │   │  Thread 0 ──→ _process_task  │           │
                        │   │  Thread 1 ──→ _process_task  │           │
                        │   │  Thread 2 ──→ _process_task  │           │
                        │   │  ...                         │           │
                        │   │  Thread N ──→ _process_task  │           │
                        │   └──────────────────────────────┘           │
                        │                                             │
                        │   HashMap<TaskID, Task*> tasks               │
                        │   HashMap<GroupID, Group*> groups             │
                        │   HashMap<Thread::ID, int> thread_ids        │
                        └─────────────────────────────────────────────┘
```

**关键设计点：**
- 两个队列（高/低优先级）通过 `SelfList`（侵入式链表）实现，入队/出队 O(1)
- 工作线程数默认等于 CPU 核心数，通过 `OS::get_default_thread_pool_size()` 获取
- 低优先级任务有使用线程数上限（`max_low_priority_threads`），防止低优先级任务占满所有线程
- 所有任务和线程的状态都由 `task_mutex` 这一把锁保护

---

## 4. 核心数据结构

### 4.1 Task（任务）

`Task` 是最核心的数据结构，代表一个待执行的工作单元：

```cpp
struct Task {
    TaskID self = -1;                        // 唯一 ID（int64_t）

    // === 三种执行方式（三选一）===
    Callable callable;                        // GDScript / 通用 Callable
    void (*native_func)(void *) = nullptr;    // C++ 原生函数指针
    void (*native_group_func)(void *, uint32_t) = nullptr;  // 组任务的函数指针
    void *native_func_userdata = nullptr;     // 传给函数的自定义数据

    String description;                       // 调试用的描述文字

    // === 同步机制 ===
    Semaphore done_semaphore;                 // 给外部线程（非池线程）等待用
    bool completed : 1;                       // 是否已完成
    uint32_t waiting_pool = 0;                // 有多少池内线程在等这个任务
    uint32_t waiting_user = 0;                // 有多少外部线程在等这个任务

    // === 组任务关联 ===
    Group *group = nullptr;                   // 如果是组任务的一部分，指向所属组

    // === 队列节点 ===
    SelfList<Task> task_elem;                 // 侵入式链表节点（用于 task_queue）

    // === 其他 ===
    bool low_priority = false;                // 是否低优先级
    bool is_pump_task : 1;                    // 是否是泵任务（长期运行）
    bool pending_notify_yield_over : 1;       // yield 恢复通知是否待处理
    int pool_thread_index = -1;               // 正在哪个线程上执行（-1 表示未执行）
    BaseTemplateUserdata *template_userdata;   // 模板任务的用户数据
};
```

**三种执行方式的优先级**（源码 `_process_task` 中的判断顺序）：
1. `native_func` / `native_group_func`：最快，直接函数指针调用
2. `template_userdata`：通过虚函数 `callback()` 调用，用于 C++ 模板接口
3. `callable`：GDScript Callable，最灵活但最慢

> **什么是侵入式链表（SelfList）？**
>
> 普通链表是在节点外面再包一层 `ListNode<T>`；侵入式链表是把链表节点 **嵌入到数据结构内部**（`SelfList<Task> task_elem` 就是 Task 自己身上的链表节点）。好处是：不需要额外内存分配，入队/出队只是改指针，非常快。

### 4.2 Group（组）

`Group` 管理一批 "并行 for" 风格的任务：

```cpp
struct Group {
    GroupID self = -1;                       // 唯一 ID
    SafeNumeric<uint32_t> index;             // 下一个待领取的工作元素索引（原子）
    SafeNumeric<uint32_t> completed_index;   // 已完成的元素数（原子）
    uint32_t max = 0;                        // 总元素数
    Semaphore done_semaphore;                // 等待组完成的信号量
    SafeFlag completed;                      // 是否全部完成（原子布尔）
    SafeNumeric<uint32_t> finished;          // 已结束的线程数（用于内存释放）
    uint32_t tasks_used = 0;                 // 分配了多少个 Task 来处理这个组
};
```

**工作窃取模型**：多个线程原子地对 `index` 做 `postincrement()`，谁拿到就谁做，直到 `index >= max`。这是一种无锁的工作分配方式。

### 4.3 ThreadData（线程数据）

每个工作线程有一个对应的 `ThreadData`：

```cpp
struct ThreadData {
    static Task *const YIELDING;        // 特殊标记值，表示线程在 yield 状态

    uint32_t index = 0;                 // 线程在 threads 数组中的索引
    Thread thread;                      // 实际的 OS 线程对象
    bool signaled : 1;                  // 是否已被通知（避免重复唤醒）
    bool yield_is_over : 1;             // yield 是否结束
    bool pre_exited_languages : 1;      // 是否已预退出脚本语言
    bool exited_languages : 1;          // 是否已退出脚本语言
    bool has_pump_task : 1;             // 是否正在运行泵任务
    Task *current_task = nullptr;       // 当前正在执行的任务
    Task *awaited_task = nullptr;       // 当前正在等待的任务（协作等待用）
    ConditionVariable cond_var;         // 用于线程的睡眠/唤醒
    WorkerThreadPool *pool = nullptr;   // 所属的线程池
};
```

### 4.4 内存管理

```cpp
PagedAllocator<Task, false, 1024> task_allocator;   // Task 的分页分配器
PagedAllocator<Group, false, 256> group_allocator;  // Group 的分页分配器
```

> **什么是 PagedAllocator（分页分配器）？**
>
> 普通的 `new`/`delete` 每次分配都可能触发系统调用，而且分配出来的内存散落在各处（内存碎片）。PagedAllocator 一次性分配一大页（比如 1024 个 Task 的空间），然后从这一页中逐个分配。回收时也不释放给系统，而是标记为可复用。
>
> 好处：
> 1. 分配/释放极快（O(1)，不需要系统调用）
> 2. 内存局部性好（同一页的 Task 在内存中连续，对 CPU 缓存友好）
> 3. 减少碎片

---

## 5. 任务的生命周期

一个普通任务从提交到销毁的完整流程：

```
 ① 提交           ② 入队            ③ 被线程取走        ④ 执行           ⑤ 完成通知         ⑥ 回收
add_task() ──→ task_queue ──→ _thread_function ──→ _process_task ──→ 唤醒等待者 ──→ task_allocator.free()
                                取出队首任务         执行用户代码        设 completed=true
                                                                     post 信号量
```

### 详细步骤

**① 提交（`_add_task`）**
```cpp
TaskID _add_task(...) {
    MutexLock lock(task_mutex);           // 加锁
    Task *task = task_allocator.alloc();  // 从分页分配器获取一个 Task
    TaskID id = last_task++;              // 分配自增 ID
    task->self = id;
    // ... 设置 callable / native_func 等字段 ...
    tasks.insert(id, task);              // 注册到 HashMap
    _post_tasks(&task, 1, high_priority, lock, pump_task);  // 入队
    return id;
}
```

**② 入队（`_post_tasks`）**
```
如果是高优先级 或 低优先级线程还有余量：
    → 放入 task_queue（高优先级队列）
    → 如果是低优先级任务，low_priority_threads_used++

否则（低优先级线程已满）：
    → 放入 low_priority_task_queue（低优先级等待队列）

最后调用 _notify_threads() 唤醒工作线程
```

**③ 被线程取走（`_thread_function`）**
```cpp
while (true) {
    MutexLock lock(task_mutex);
    while (true) {
        // 检查是否该退出
        if (_handle_runlevel(...)) return;
        // 尝试从队列取任务
        if (task_queue.first()) {
            task = task_queue.first()->self();
            task_queue.remove(task_queue.first());  // 从队列移除
            break;
        }
        // 没有任务，休眠等待通知
        cond_var.wait(lock);
    }
    // 锁已释放，执行任务
    _process_task(task);
}
```

**④ 执行（`_process_task`）**

根据任务类型（普通/组）调用对应的函数，这里先讲普通任务：
```cpp
if (task->native_func) {
    task->native_func(task->native_func_userdata);   // C++ 函数指针
} else if (task->template_userdata) {
    task->template_userdata->callback();             // 模板方式
} else {
    task->callable.call();                           // GDScript Callable
}
```

**⑤ 完成通知**
```cpp
task_mutex.lock();
task->completed = true;
task->pool_thread_index = -1;

// 通知外部线程（通过信号量）
if (task->waiting_user) {
    task->done_semaphore.post(task->waiting_user);
}

// 通知池内线程（通过条件变量）
for (每个线程 thread) {
    if (thread.awaited_task == task) {
        thread.cond_var.notify_one();
    }
}
```

**⑥ 回收**

当所有等待者都完成等待后（`waiting_pool == 0 && waiting_user == 0`），在 `wait_for_task_completion()` 中释放：
```cpp
tasks.erase(id);
task_allocator.free(task);
```

---

## 6. 两种任务模型

### 6.1 普通任务（Single Task）

提交一个独立的工作单元，由一个线程执行。

```cpp
// C++ 端 —— 三种提交方式

// 方式1：原生函数指针（最快）
TaskID id = pool->add_native_task(my_function, my_data, true, "MyTask");

// 方式2：模板方式（类型安全）
TaskID id = pool->add_template_task(this, &MyClass::my_method, my_data, true, "MyTask");

// 方式3：Callable（最灵活）
TaskID id = pool->add_task(callable_mp(this, &MyClass::my_method), true, "MyTask");
```

```gdscript
# GDScript 端
var id = WorkerThreadPool.add_task(my_callable, true, "MyTask")
WorkerThreadPool.wait_for_task_completion(id)
```

### 6.2 组任务（Group Task） —— 并行 for

将 N 个元素的处理工作分配到多个线程并行执行。这是 **最常用的并行模式**。

**概念图**：
```
add_group_task(process_item, elements=1000, tasks=4)

Group { max=1000, index=0 }

线程 0: index=0 → process(0), index=4 → process(4), index=8 → process(8), ...
线程 1: index=1 → process(1), index=5 → process(5), index=9 → process(9), ...
线程 2: index=2 → process(2), index=6 → process(6), index=10 → process(10), ...
线程 3: index=3 → process(3), index=7 → process(7), index=11 → process(11), ...

注意：实际分配不是轮询，而是原子竞争（谁先 postincrement 谁先拿到）
```

**源码关键逻辑（`_process_task` 中组任务部分）：**
```cpp
while (true) {
    uint32_t work_index = group->index.postincrement();  // 原子取下一个索引
    if (work_index >= group->max) break;                 // 没有更多工作了

    // 执行用户函数，传入工作索引
    native_group_func(userdata, work_index);

    // 原子递增完成计数
    uint32_t completed = group->completed_index.increment();
    if (completed == group->max) {
        // 我是最后一个完成的，通知等待者
        group->done_semaphore.post();
        group->completed.set_to(true);
    }
}
```

> **为什么用原子 postincrement 而不是预分配范围？**
>
> 假设 1000 个元素分给 4 个线程，预分配是 [0-249], [250-499], [500-749], [750-999]。
> 问题是：如果某些元素处理得快、某些慢，提前完成的线程就闲着了。
>
> 原子竞争的方式让每个线程 **做完一个立刻取下一个**，自动实现负载均衡——快的线程自然多做几个。

**使用示例：**

```cpp
// 并行处理 1000 个物体的可见性检测
GroupID gid = pool->add_template_group_task(
    this,
    &SceneCull::_cull_instance,  // 处理函数
    &cull_data,                  // 传入的数据
    1000,                        // 元素总数
    -1,                          // 线程数（-1 = 自动，等于 CPU 核数）
    true,                        // 高优先级
    "VisibilityCull"             // 描述
);
pool->wait_for_group_task_completion(gid);  // 等待全部完成
```

### 6.3 两种模型对比

| 特性 | 普通任务 | 组任务 |
|------|---------|--------|
| 执行线程数 | 1 个 | 多个（自动分配） |
| 适用场景 | 一个独立的异步操作 | 对 N 个元素做相同操作 |
| 结果获取 | `wait_for_task_completion(id)` | `wait_for_group_task_completion(gid)` |
| 负载均衡 | 不涉及 | 原子竞争自动均衡 |
| 有 TaskID？ | ✅ 有，可查询/等待 | 组内单个任务无 ID |
| 典型例子 | 异步加载资源 | 并行剔除、并行物理 |

---

## 7. 线程调度机制

### 7.1 初始化

```cpp
void WorkerThreadPool::init(int p_thread_count, float p_low_priority_task_ratio) {
    // 默认线程数 = OS 建议值（通常等于 CPU 逻辑核心数）
    if (p_thread_count < 0)
        p_thread_count = OS::get_singleton()->get_default_thread_pool_size();

    // 低优先级线程上限 = 总线程数 × 0.3（至少 1，至多 N-1）
    max_low_priority_threads = CLAMP(p_thread_count * 0.3, 1, p_thread_count - 1);

    // 预留 5 个额外位置（2D物理 + 3D物理 + 渲染 + GPU纹理压缩 + 其他）
    threads.reserve(5);
    threads.resize(p_thread_count);

    // 启动所有工作线程
    for (int i = 0; i < p_thread_count; i++) {
        threads[i].index = i;
        threads[i].pool = this;
        threads[i].thread.start(&_thread_function, &threads[i]);
    }
}
```

### 7.2 双队列与优先级

```
                ┌─────────────────────────────────┐
                │         _post_tasks()            │
                │                                  │
                │  if (高优先级 || 低优先级有余量)    │
                │     → task_queue（主队列）         │
                │     → low_priority_threads_used++ │
                │  else                            │
                │     → low_priority_task_queue     │
                │       （等待队列，等有线程释放时提升）│
                └─────────────────────────────────┘
```

**低优先级限流机制**：
- `max_low_priority_threads` = 线程数 × 30%
- 超过上限的低优先级任务被放入等待队列
- 当一个低优先级任务完成时，`low_priority_threads_used--`，然后尝试从等待队列提升一个任务（`_try_promote_low_priority_task`）

**为什么要限流？** 防止大量低优先级任务（如后台资源加载）占满所有线程，导致高优先级任务（如渲染、物理）无线程可用。

### 7.3 线程唤醒策略（`_notify_threads`）

提交任务后需要唤醒工作线程。但唤醒是有开销的（涉及内核态切换），所以 Godot 有精心设计的策略：

**第一轮扫描**：
1. 对于需要 **处理** 的任务：优先唤醒 **空闲线程**（`current_task == null`），栈最浅
2. 对于需要 **提升** 的低优先级任务：寻找 **正在等待且执行低优先级任务的线程**

**第二轮扫描**（第一轮没找够时）：
- 唤醒任何 **正在等待其他任务的线程**（它们可以在协作等待中帮忙处理新任务）

**轮转索引**（`notify_index`）：使用轮转的方式选择从哪个线程开始扫描，避免总是唤醒同一个线程，起到简单的负载均衡作用。

```cpp
// 跳过已经被通知过的线程
if (th.signaled) continue;

// 跳过正在忙且没在等待的线程
if (th.current_task && !th.awaited_task) continue;

// 唤醒！
th.cond_var.notify_one();
th.signaled = true;
```

---

## 8. 协作等待机制

这是 WorkerThreadPool 最精妙的设计之一。

### 问题：池内线程等待会死锁

假设只有 2 个工作线程：

```
线程 0：执行任务 A，任务 A 提交了任务 C 并等待 C 完成
线程 1：执行任务 B，任务 B 提交了任务 D 并等待 D 完成

此时任务 C 和 D 在队列中，但没有空闲线程来执行它们 → 死锁！
```

### 解决方案：协作等待（Collaborative Waiting）

当池内线程需要等待另一个任务时，**不是傻等**，而是趁等待的空隙去执行其他任务：

```cpp
void _wait_collaboratively(ThreadData *caller, Task *awaited_task) {
    while (true) {
        MutexLock lock(task_mutex);

        // 检查等待是否结束
        if (awaited_task->completed) break;

        // 队列中有其他任务吗？
        if (task_queue.first()) {
            // 拿一个来做！
            Task *other_task = task_queue.first()->self();
            task_queue.remove(task_queue.first());
            lock.unlock();
            _process_task(other_task);  // 递归执行其他任务
            continue;
        }

        // 队列也空了，设置 awaited_task 并休眠
        caller->awaited_task = awaited_task;
        caller->cond_var.wait(lock);
        caller->awaited_task = nullptr;
    }
}
```

**形象比喻**：
> 你（线程 0）在排队等外卖（任务 C）。与其干站着，不如帮隔壁柜台处理一下其他订单。等你的外卖到了，别人会拍你一下（`notify_one`），你就回去取外卖。

### 递归深度

协作等待可能导致任务处理递归：

```
线程 0 的栈：
  _thread_function()              ← 主循环
    _process_task(A)              ← 执行任务 A
      wait_for_task_completion(C) ← A 等 C
        _wait_collaboratively()   ← 开始协作等待
          _process_task(E)        ← 趁机执行任务 E
            ...                   ← 可能继续递归
```

这就是为什么 `_process_task` 中有 `prev_task` 备份——为了递归返回时恢复上下文。

---

## 9. 死锁防御

### 问题：为什么等"老任务"会死锁？

任务 ID 是自增的。如果一个池线程等待 ID 比自己 **更小**（更早）的任务，可能出现：

```
任务 A (ID=1) 等待 任务 B (ID=2)
任务 B (ID=2) 等待 任务 A (ID=1)    ← 循环依赖！
```

或者更隐蔽的情况：

```
线程 0 执行任务 A (ID=5)，A 协作等待时执行了任务 C (ID=7)
C 又想等待任务 A (ID=5) —— 但 A 在栈深处，C 在栈顶，不可能回去执行 A
```

### Godot 的策略：禁止等待更老的任务

```cpp
Error wait_for_task_completion(TaskID p_task_id) {
    // ...
    if (caller_pool_thread && p_task_id <= caller_pool_thread->current_task->self) {
        // 要等的任务 ID ≤ 当前任务 ID → 拒绝！
        return ERR_BUSY;
    }
    // ...
}
```

**规则**：池线程只能等待 ID 比自己当前任务 **更大**（更新）的任务。这从根本上防止了循环等待。

返回 `ERR_BUSY` 而不是崩溃，给调用方机会处理这种情况（比如改为同步执行）。

> **注意**：这个限制只针对池内线程。外部线程（如主线程）等待任何任务都是安全的，因为它们不占用池内资源。

---

## 10. Pump Task（泵任务）

### 什么是泵任务？

泵任务是一种 **长期运行的专属任务**。普通任务执行完就结束了，但泵任务会持续运行（通常跨越多帧），通过 yield/resume 机制与引擎主循环同步。

**典型使用者**：
- `PhysicsServer2D`（2D 物理线程）
- `PhysicsServer3D`（3D 物理线程）
- `RenderingServer`（渲染线程）

### 为什么需要专门的泵任务？

这些系统需要 **每帧执行一次**，但又要在 **独立线程上运行** 以避免阻塞主线程。泵任务就是为此设计的：

```
主线程:  帧1 ──→ 帧2 ──→ 帧3 ──→ ...
                  │         │
                  ▼         ▼
物理线程: [yield] → 执行物理 → [yield] → 执行物理 → [yield] → ...
         等主线程     完成       等主线程     完成
         通知                   通知
```

### 自动扩容

当泵任务数量接近线程数时，WorkerThreadPool 会自动创建新线程，确保始终有空闲线程可用：

```cpp
if (pump_task_count >= thread_count) {
    // "需要的专属线程太多了！自动增加一个工作线程。"
    threads.resize(thread_count + 1);
    // ... 初始化并启动新线程 ...
}
```

> 这就是 `init()` 中 `threads.reserve(5)` 的原因——预留空间给可能动态添加的泵任务线程。

### 泵任务的调度限制

协作等待中，如果当前线程已经在执行泵任务（或正在 yield），它 **不会去取另一个泵任务来执行**：

```cpp
if ((p_task == ThreadData::YIELDING || caller->has_pump_task) && task_to_process->is_pump_task) {
    task_to_process = nullptr;  // 不取这个泵任务
    _notify_threads(...);       // 让别的线程来处理
}
```

原因：泵任务通常是长期运行的，如果一个泵任务线程在 yield 等待时又去执行另一个泵任务，可能导致栈深度不断增加。

---

## 11. Yield 机制

### 工作原理

Yield 让泵任务 **暂停执行并让出线程**，等待被外部唤醒后继续：

```cpp
// 泵任务内部调用
void WorkerThreadPool::yield() {
    // 进入协作等待，但等待的"任务"是特殊标记 YIELDING
    _wait_collaboratively(&threads[current_index], ThreadData::YIELDING);
}

// 外部（通常是主线程）调用来唤醒
void WorkerThreadPool::notify_yield_over(TaskID p_task_id) {
    ThreadData &td = threads[task->pool_thread_index];
    td.yield_is_over = true;    // 设置标记
    td.cond_var.notify_one();   // 唤醒线程
}
```

### 完整流程

```
物理线程:
  1. 执行物理模拟的一帧
  2. 调用 yield() → 进入协作等待
  3. 在等待期间可能帮忙处理其他任务
  4. 主线程调用 notify_yield_over() → 唤醒
  5. yield() 返回，继续执行下一帧物理模拟
  6. 回到步骤 1
```

### YIELDING 标记

`ThreadData::YIELDING` 是一个特殊的 `Task*` 指针值（`(Task*)1`），不指向任何真实的任务。它被用在 `_wait_collaboratively` 中来区分"等待特定任务完成"和"yield 等待被唤醒"：

```cpp
if (p_task == ThreadData::YIELDING) {
    // 检查 yield_is_over 标志
    if (caller->yield_is_over) break;
} else {
    // 检查任务是否完成
    if (p_task->completed) break;
}
```

---

## 12. 可解锁互斥量区域

### 问题背景

Godot 的某些系统（如 `MessageQueue`）在工作线程上持有自己的互斥锁。当工作线程进入协作等待时，它可能一直持有这些锁，导致其他线程无法访问这些资源。

### 解决方案

`thread_enter_unlock_allowance_zone` / `thread_exit_unlock_allowance_zone` 机制允许注册最多 2 个 "可解锁互斥量"。当线程进入协作等待的休眠状态时，这些锁会被 **临时释放**，唤醒后重新获取：

```cpp
// 注册一个可解锁的互斥量
uint32_t zone = WorkerThreadPool::thread_enter_unlock_allowance_zone(my_lock);

// ... 在这个区域内，如果线程进入协作等待，my_lock 会被临时释放 ...

// 取消注册
WorkerThreadPool::thread_exit_unlock_allowance_zone(zone);
```

在 `_wait_collaboratively` 中的体现：

```cpp
if (!task_to_process) {
    // 即将休眠，释放可解锁的互斥量
    _unlock_unlockable_mutexes();
    cond_var.wait(lock);
    // 唤醒后重新获取
    _lock_unlockable_mutexes();
}
```

> 这个机制使用引用计数（`rc`），同一个锁可以嵌套注册多次，只在引用计数降为 0 时才真正取消注册。

---

## 13. Runlevel 生命周期管理

WorkerThreadPool 有 4 个运行级别，控制引擎关闭时的有序退出：

```
RUNLEVEL_NORMAL
    │  正常运行，处理所有任务
    ▼
RUNLEVEL_PRE_EXIT_LANGUAGES
    │  停止接受新任务，等待所有任务完成
    │  所有线程报告"空闲"后进入下一级
    ▼
RUNLEVEL_EXIT_LANGUAGES
    │  所有工作线程调用 ScriptServer::thread_exit()
    │  与脚本语言（GDScript、C#等）断开连接
    │  这个阶段会阻止新任务的提交
    ▼
RUNLEVEL_EXIT
    │  所有线程退出主循环，准备销毁
    ▼
  finish() → thread.wait_to_finish() → 线程结束
```

### 为什么需要分步退出？

脚本语言（特别是 C#/Mono）需要在线程上做清理工作。如果直接杀线程，可能导致脚本引擎崩溃。分步退出确保：
1. 先处理完所有待执行的任务
2. 再安全地与脚本系统断开
3. 最后才真正停止线程

### 切换 Runlevel

```cpp
void _switch_runlevel(Runlevel p_runlevel) {
    runlevel = p_runlevel;
    memset(&runlevel_data, 0, sizeof(runlevel_data));
    // 唤醒所有线程，让它们检查新的 runlevel
    for (auto &thread : threads) {
        thread.cond_var.notify_one();
        thread.signaled = true;
    }
    control_cond_var.notify_all();
}
```

---

## 14. GDScript API

WorkerThreadPool 通过 `_bind_methods()` 暴露了以下 API 给 GDScript：

```gdscript
# ===== 普通任务 =====

# 提交一个任务
var task_id = WorkerThreadPool.add_task(my_callable, false, "描述")
# 参数：callable, 是否高优先级, 描述

# 查询任务是否完成
var done = WorkerThreadPool.is_task_completed(task_id)

# 等待任务完成（阻塞当前线程）
var err = WorkerThreadPool.wait_for_task_completion(task_id)
# 返回 OK 或 ERR_BUSY（死锁风险时）

# 获取当前任务的 ID（在任务内部调用）
var my_id = WorkerThreadPool.get_caller_task_id()


# ===== 组任务 =====

# 提交一个组任务
var group_id = WorkerThreadPool.add_group_task(
    my_callable,   # 接受一个 int 参数（元素索引）的 callable
    100,           # 元素总数
    -1,            # 线程数（-1 = 自动）
    false,         # 是否高优先级
    "描述"
)

# 查询已处理的元素数
var count = WorkerThreadPool.get_group_processed_element_count(group_id)

# 查询组任务是否完成
var done = WorkerThreadPool.is_group_task_completed(group_id)

# 等待组任务完成（阻塞当前线程）
WorkerThreadPool.wait_for_group_task_completion(group_id)

# 获取当前组任务的 ID（在组任务内部调用）
var my_gid = WorkerThreadPool.get_caller_group_id()
```

### GDScript 使用示例

```gdscript
# 并行处理数组
var data = range(1000)
var results = []
results.resize(1000)

func _process_element(index: int):
    results[index] = heavy_computation(data[index])

func _ready():
    var gid = WorkerThreadPool.add_group_task(
        _process_element, 1000, -1, false, "ProcessData"
    )
    WorkerThreadPool.wait_for_group_task_completion(gid)
    print("全部处理完成！", results)
```

> ⚠️ **注意**：GDScript 中使用多线程时要小心：
> - 不要在工作线程中访问场景树节点（除非 `set_thread_safe_for_nodes(true)`）
> - 不要在多个线程中同时修改同一个对象
> - 使用 `Mutex` 保护共享数据

---

## 15. 实际应用场景

### 引擎内部使用

| 系统 | 使用方式 | 任务类型 |
|------|---------|----------|
| **场景剔除** (`RendererSceneCull`) | 并行检测物体可见性 | Group Task |
| **Shader 编译** (`ShaderRD`) | 并行编译多个 Shader 变体 | Group Task |
| **2D 物理** (`PhysicsServer2DWrapMT`) | 独立物理线程 | Pump Task |
| **3D 物理** (`PhysicsServer3DWrapMT`) | 独立物理线程 | Pump Task |
| **渲染服务器** (`RenderingServerDefault`) | 独立渲染线程 | Pump Task |
| **GPU 粒子碰撞** (`GPUParticlesCollision3D`) | 并行烘焙 SDF | Group Task |
| **纹理压缩** | 并行压缩多个纹理 | Group Task |

### Named Pool（命名池）

除了全局单例之外，可以创建 **命名线程池**：

```cpp
// 获取（或创建）一个名为 "my_pool" 的独立线程池
WorkerThreadPool *pool = WorkerThreadPool::get_named_pool("my_pool");
```

命名池有自己独立的线程和任务队列，适用于需要隔离的场景（如避免与引擎内部任务竞争线程）。

---

## 16. 总结

### WorkerThreadPool 的设计哲学

```
简单 > 复杂
安全 > 极致性能
显式 > 隐式
```

1. **简单的 API**：只有 add_task / wait / add_group_task 三个核心操作
2. **安全第一**：内建死锁防御（禁止等待老任务）、协作等待（避免线程饥饿）
3. **显式控制**：没有自动依赖推断，顺序关系由调用方通过 wait 显式管理

### 架构全景

```
┌─────────────────────────────────────────────────────────────────────┐
│                        调用方                                       │
│  (主线程 / 渲染线程 / 物理线程 / GDScript / ...)                     │
└────────┬──────────────────┬──────────────────┬──────────────────────┘
         │ add_task()       │ add_group_task()  │ wait_for_*()
         ▼                 ▼                   ▼
┌─────────────────────────────────────────────────────────────────────┐
│                    WorkerThreadPool (单例)                          │
│                                                                     │
│  ┌────────────┐  ┌──────────────┐  ┌─────────────────────────────┐ │
│  │ 任务分配器  │  │ 高/低优先级  │  │        工作线程              │ │
│  │ PagedAlloc  │  │   双队列     │  │  ┌────┐┌────┐┌────┐┌────┐  │ │
│  └────────────┘  │  SelfList    │  │  │ T0 ││ T1 ││ T2 ││ T3 │  │ │
│                  └──────────────┘  │  └──┬─┘└──┬─┘└──┬─┘└──┬─┘  │ │
│                                    │     │     │     │     │     │ │
│  ┌─────────────────────────────┐   │  _process_task() 循环执行    │ │
│  │ 死锁防御 + 协作等待          │   │                              │ │
│  │ + yield/resume + runlevel   │   └─────────────────────────────┘ │
│  └─────────────────────────────┘                                   │
└─────────────────────────────────────────────────────────────────────┘
```

### 与 UE TaskGraph 的关键差异

| | Godot WorkerThreadPool | UE TaskGraph |
|---|---|---|
| **核心思想** | "给我代码，我找线程跑" | "声明依赖关系，自动调度" |
| **依赖管理** | 手动 wait（屏障） | 声明式 DAG |
| **优势** | 简单、安全、易理解 | 自动优化、高吞吐 |
| **劣势** | 无法自动并行化依赖链 | 复杂、学习曲线陡 |

### 如果你想在 Godot 上构建 TaskGraph...

WorkerThreadPool 提供了坚实的基础：
- ✅ 成熟的线程池和任务执行机制
- ✅ 协作等待避免线程浪费
- ✅ 完成通知机制可用于触发下游任务
- ❌ 缺少的是：依赖声明 + 拓扑排序 + 自动调度

在它之上封装一层依赖图管理是完全可行的，这也是我们后续 TaskGraph 设计方案的基础。
