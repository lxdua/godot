# ObjectPool 对象池系统

> 为 Godot 引擎新增内置对象池机制，通过预创建和复用节点来避免频繁 `instantiate()` / `queue_free()` 带来的性能开销。

---

## 一、问题分析

### 当前 Godot 的节点创建/销毁成本

每次 `instantiate()` 一个 PackedScene：

```
1. 内存分配（malloc）
2. 构造函数链（Node → Node3D → CharacterBody3D → ...）
3. 递归创建所有子节点
4. 属性赋值（.tscn 中保存的所有属性）
5. add_child() → 进入场景树
6. NOTIFICATION_ENTER_TREE → 递归所有子节点
7. NOTIFICATION_READY → _ready() 链
8. 信号连接（脚本中 connect 的、编辑器中连的）
9. 物理体注册到 PhysicsServer
10. 渲染实例注册到 RenderingServer
```

每次 `queue_free()`：

```
1. 标记为待删除
2. 帧末：NOTIFICATION_EXIT_TREE → 递归
3. 信号断连
4. 物理体从 PhysicsServer 注销
5. 渲染实例从 RenderingServer 注销
6. 析构函数链
7. 内存释放（free）
```

**一个典型的子弹场景**（MeshInstance3D + CollisionShape3D + Area3D + GPUParticles3D）做一次完整创建/销毁周期约 **0.02-0.05ms**。弹幕游戏一帧 200 发 → **4-10ms**，占满 60fps 帧预算的 25-60%。

### 对象池如何解决

```
创建: 一次性预创建 → 全部隐藏、禁用
获取: 从空闲列表取出 → show + enable（~0.001ms）
回收: hide + disable + 移回空闲列表（~0.001ms）

200发/帧: 0.2ms vs 4-10ms，提升 20-50 倍
```

---

## 二、UE 中的做法

UE 没有官方内置 ObjectPool 类，但有成熟的社区模式和引擎层支持：

### 标准模式

```cpp
// 池管理器
UCLASS()
class UActorPool : public UObject {
    TArray<AActor*> FreePool;
    TSubclassOf<AActor> PooledClass;

public:
    AActor* Acquire() {
        if (FreePool.Num() > 0) {
            AActor* Actor = FreePool.Pop();
            Actor->SetActorHiddenInGame(false);
            Actor->SetActorEnableCollision(true);
            Actor->SetActorTickEnabled(true);
            return Actor;
        }
        // 池空 → 新建
        return GetWorld()->SpawnActor(PooledClass);
    }

    void Release(AActor* Actor) {
        Actor->SetActorHiddenInGame(true);
        Actor->SetActorEnableCollision(false);
        Actor->SetActorTickEnabled(false);
        FreePool.Add(Actor);
    }
};
```

### UE 的 IPoolable 接口

```cpp
class IPoolable {
public:
    virtual void OnAcquiredFromPool() = 0;   // 取出时调用
    virtual void OnReleasedToPool() = 0;     // 回收时调用
};
```

### Niagara 粒子系统

Niagara 内部全自动池化，粒子"死亡"后 slot 直接复用，不做内存操作。

---

## 三、设计方案

### 3.1 总体架构

```
┌─────────────────────────────────────────────┐
│                 ObjectPool                   │
│            (extends Node)                    │
│                                              │
│  scene: PackedScene     ← 池中对象的模板     │
│  initial_size: int      ← 预创建数量         │
│  max_size: int          ← 最大容量           │
│  auto_expand: bool      ← 池空时自动扩容     │
│                                              │
│  acquire() → Node       ← 从池中取出         │
│  release(node)          ← 回收到池中         │
│  warm_up(count)         ← 手动预热           │
│  clear()                ← 清空池             │
│                                              │
│  ┌──────────────┐  ┌──────────────┐         │
│  │ Active List  │  │  Free List   │         │
│  │  (在用)      │  │  (空闲)      │         │
│  │  bullet_1 ●  │  │  bullet_3 ○  │         │
│  │  bullet_2 ●  │  │  bullet_4 ○  │         │
│  │              │  │  bullet_5 ○  │         │
│  └──────────────┘  └──────────────┘         │
└─────────────────────────────────────────────┘
```

### 3.2 节点生命周期

```
[预创建] instantiate() → add_child() → _ready() → _on_pool_release() → 隐藏+禁用
                                                          ↑
                                                          │ 首次入池
[获取]   acquire() → _on_pool_acquire() → 显示+启用 ─────→ 游戏使用中
                                                          │
[回收]   release() → _on_pool_release() → 隐藏+禁用 ─────→ 空闲列表
                          ↑                               │
                          └───────────────────────────────┘
                                     循环复用
```

关键：节点**始终在场景树中**，只是通过 visible/process 控制活跃状态。这样避免了 `add_child()` / `remove_child()` 的开销。

### 3.3 自动禁用清单

`release()` 时引擎自动执行：

| 操作 | 目的 | API |
|------|------|-----|
| `set_visible(false)` | 不渲染 | `Node3D::hide()` / `CanvasItem::hide()` |
| `set_process(false)` | 不调用 `_process` | `Node::set_process(false)` |
| `set_physics_process(false)` | 不调用 `_physics_process` | `Node::set_physics_process(false)` |
| `set_process_mode(DISABLED)` | 递归禁用所有子节点 | `Node::set_process_mode()` |
| 碰撞禁用 | 不参与物理检测 | `CollisionObject3D::set_collision_layer(0)` + 保存原值 |

`acquire()` 时引擎自动执行上述操作的逆操作，恢复到原始状态。

### 3.4 用户自定义重置：虚函数回调

在 Node 基类中新增两个可选虚函数：

```cpp
// scene/main/node.h
GDVIRTUAL0(_on_pool_acquire)    // 从池中取出时调用
GDVIRTUAL0(_on_pool_release)    // 回收到池中时调用
```

用户在 GDScript 中 override：

```gdscript
# bullet.gd
extends Area3D

var velocity := Vector3.ZERO
var damage := 10.0
var _lifetime := 0.0

func _on_pool_acquire():
    # 重置状态 — 每次从池中取出时调用
    velocity = Vector3.ZERO
    damage = 10.0
    _lifetime = 0.0

func _on_pool_release():
    # 清理 — 回收时调用
    # 停止粒子、音效等
    $TrailParticles.emitting = false
    $HitSound.stop()
```

### 3.5 对比：虚函数 vs 信号

| | 虚函数 `_on_pool_acquire/release` | 信号 `pool_acquired` / `pool_released` |
|---|---|---|
| 性能 | ✅ 直接调用，无分发开销 | ❌ 信号分发有开销 |
| 使用体验 | ✅ override 即可 | 需要手动 connect |
| 一致性 | ✅ 和 `_ready()` / `_process()` 风格一致 | 和 Node 其他回调风格不一致 |
| 子节点通知 | ❌ 需要手动传播 | ✅ 可以多个对象 connect |

**方案：两者都提供**。虚函数作为主要接口，同时 emit 信号方便外部监听。

---

## 四、完整 API 设计

### 4.1 ObjectPool 类

```cpp
class ObjectPool : public Node {
    GDCLASS(ObjectPool, Node);

public:
    // ==================== 配置 ====================

    // 池中对象的模板场景
    void set_scene(const Ref<PackedScene> &p_scene);
    Ref<PackedScene> get_scene() const;

    // 预创建数量（进入场景树时自动创建）
    void set_initial_size(int p_size);
    int get_initial_size() const;

    // 最大容量（0 = 无限制）
    void set_max_size(int p_size);
    int get_max_size() const;

    // 池空时是否自动扩容
    void set_auto_expand(bool p_enable);
    bool is_auto_expand() const;

    // ==================== 核心操作 ====================

    // 从池中获取一个对象，返回 nullptr 如果池空且不自动扩容
    Node *acquire();

    // 回收一个对象到池中
    void release(Node *p_node);

    // 手动预热：额外创建 p_count 个对象
    void warm_up(int p_count);

    // 清空池，销毁所有对象（包括活跃的）
    void clear();

    // ==================== 状态查询 ====================

    // 当前空闲对象数量
    int get_free_count() const;

    // 当前活跃（在用）对象数量
    int get_active_count() const;

    // 总对象数量（空闲 + 活跃）
    int get_total_count() const;

signals:
    // 当池需要扩容但达到 max_size 时触发
    void pool_exhausted();
};
```

### 4.2 Node 基类新增

```cpp
// scene/main/node.h 新增：

// 虚函数回调
GDVIRTUAL0(_on_pool_acquire)
GDVIRTUAL0(_on_pool_release)

// 通知常量
NOTIFICATION_POOL_ACQUIRE = 2100,  // 从池中取出
NOTIFICATION_POOL_RELEASE = 2101,  // 回收到池中

// 池归属
ObjectPool *_pool_owner = nullptr;  // 指向所属的 ObjectPool（null = 不在池中）

public:
// 获取所属对象池（如果有的话）
ObjectPool *get_pool() const;

// 便捷方法：回收自身到池中
void release_to_pool();
```

---

## 五、改动文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `scene/main/object_pool.h` | **新建** | ObjectPool 类声明 |
| `scene/main/object_pool.cpp` | **新建** | ObjectPool 实现 |
| `scene/main/node.h` | **修改** | 新增 NOTIFICATION 常量、虚函数声明、`_pool_owner` |
| `scene/main/node.cpp` | **修改** | 虚函数绑定、`release_to_pool()` 实现 |
| `scene/register_scene_types.cpp` | **修改** | 注册 ObjectPool 类 |
| `doc/classes/ObjectPool.xml` | **新建** | API 文档 |
| `doc/classes/Node.xml` | **修改** | 新增 pool 相关文档 |

---

## 六、核心实现伪代码

### acquire()

```cpp
Node *ObjectPool::acquire() {
    Node *node = nullptr;

    if (!free_list.is_empty()) {
        // 从空闲列表取出
        node = free_list.back()->get();
        free_list.pop_back();
    } else if (auto_expand && (max_size == 0 || get_total_count() < max_size)) {
        // 池空 → 扩容
        node = _create_instance();
    } else {
        // 池空且不扩容
        emit_signal(SNAME("pool_exhausted"));
        return nullptr;
    }

    // 激活
    active_list.push_back(node);
    _activate_node(node);

    // 通知
    node->notification(NOTIFICATION_POOL_ACQUIRE);
    // → 触发 _on_pool_acquire() 虚函数

    return node;
}
```

### release()

```cpp
void ObjectPool::release(Node *p_node) {
    ERR_FAIL_NULL(p_node);
    ERR_FAIL_COND(p_node->_pool_owner != this);

    // 从活跃列表移到空闲列表
    active_list.erase(p_node);
    free_list.push_back(p_node);

    // 通知（在禁用之前，让用户有机会做清理）
    p_node->notification(NOTIFICATION_POOL_RELEASE);
    // → 触发 _on_pool_release() 虚函数

    // 禁用
    _deactivate_node(p_node);
}
```

### _activate_node() / _deactivate_node()

```cpp
void ObjectPool::_activate_node(Node *p_node) {
    p_node->set_process_mode(Node::PROCESS_MODE_INHERIT);

    // 恢复可见性
    Node3D *n3d = Object::cast_to<Node3D>(p_node);
    if (n3d) {
        n3d->show();
    }
    CanvasItem *ci = Object::cast_to<CanvasItem>(p_node);
    if (ci) {
        ci->show();
    }

    // 恢复碰撞层
    CollisionObject3D *co3d = Object::cast_to<CollisionObject3D>(p_node);
    if (co3d && _saved_collision_layers.has(p_node)) {
        co3d->set_collision_layer(_saved_collision_layers[p_node].first);
        co3d->set_collision_mask(_saved_collision_layers[p_node].second);
    }
    CollisionObject2D *co2d = Object::cast_to<CollisionObject2D>(p_node);
    if (co2d && _saved_collision_layers_2d.has(p_node)) {
        co2d->set_collision_layer(_saved_collision_layers_2d[p_node].first);
        co2d->set_collision_mask(_saved_collision_layers_2d[p_node].second);
    }
}

void ObjectPool::_deactivate_node(Node *p_node) {
    p_node->set_process_mode(Node::PROCESS_MODE_DISABLED);

    // 隐藏
    Node3D *n3d = Object::cast_to<Node3D>(p_node);
    if (n3d) {
        n3d->hide();
    }
    CanvasItem *ci = Object::cast_to<CanvasItem>(p_node);
    if (ci) {
        ci->hide();
    }

    // 保存并清除碰撞层
    CollisionObject3D *co3d = Object::cast_to<CollisionObject3D>(p_node);
    if (co3d) {
        _saved_collision_layers[p_node] = { co3d->get_collision_layer(), co3d->get_collision_mask() };
        co3d->set_collision_layer(0);
        co3d->set_collision_mask(0);
    }
    CollisionObject2D *co2d = Object::cast_to<CollisionObject2D>(p_node);
    if (co2d) {
        _saved_collision_layers_2d[p_node] = { co2d->get_collision_layer(), co2d->get_collision_mask() };
        co2d->set_collision_layer(0);
        co2d->set_collision_mask(0);
    }
}
```

---

## 七、使用示例

### 7.1 基础用法：子弹

```gdscript
# 场景树:
# Game
# ├── Player
# ├── BulletPool (ObjectPool)
# │   ├── Bullet_0 (预创建, 隐藏)
# │   ├── Bullet_1
# │   └── ...
# └── EnemyPool (ObjectPool)

@onready var bullet_pool: ObjectPool = $BulletPool

func _shoot():
    var bullet = bullet_pool.acquire()
    if bullet == null:
        push_warning("Bullet pool exhausted!")
        return
    bullet.global_position = $Muzzle.global_position
    bullet.velocity = -transform.basis.z * 50.0
```

```gdscript
# bullet.gd
extends Area3D

var velocity := Vector3.ZERO

func _on_pool_acquire():
    velocity = Vector3.ZERO

func _on_pool_release():
    $Trail.emitting = false

func _physics_process(delta):
    global_position += velocity * delta

func _on_body_entered(body):
    if body.has_method("take_damage"):
        body.take_damage(10)
    release_to_pool()  # 便捷方法，等于 get_pool().release(self)
```

### 7.2 Inspector 配置

```
▼ ObjectPool
   Scene                    [res://bullet.tscn]
   Initial Size             50
   Max Size                 200
   Auto Expand              ☑

▼ Status (只读, 运行时显示)
   Free                     47
   Active                   3
   Total                    50
```

### 7.3 延迟回收

```gdscript
# 子弹命中后播放爆炸动画，动画结束再回收
func _on_body_entered(body):
    body.take_damage(10)
    velocity = Vector3.ZERO
    $ExplosionAnim.play("explode")
    await $ExplosionAnim.animation_finished
    release_to_pool()
```

### 7.4 2D 场景同样适用

```gdscript
# coin.gd — 2D 金币
extends Area2D

func _on_pool_acquire():
    $AnimatedSprite2D.play("spin")
    $CollectSound.stop()

func _on_pool_release():
    $AnimatedSprite2D.stop()

func _on_body_entered(body):
    $CollectSound.play()
    $AnimatedSprite2D.play("collect")
    await $AnimatedSprite2D.animation_finished
    release_to_pool()
```

---

## 八、边界情况处理

### 8.1 池空策略

| `auto_expand` | `max_size` | 行为 |
|:---:|:---:|------|
| `true` | `0` | 无限扩容，永远返回有效对象 |
| `true` | `200` | 扩容到 200 后返回 null + emit `pool_exhausted` |
| `false` | 任意 | 池空直接返回 null + emit `pool_exhausted` |

### 8.2 release 已经释放的对象

```cpp
void ObjectPool::release(Node *p_node) {
    ERR_FAIL_NULL(p_node);
    ERR_FAIL_COND_MSG(p_node->_pool_owner != this, 
        "Node does not belong to this pool.");
    ERR_FAIL_COND_MSG(free_list.find(p_node) != -1, 
        "Node is already in the free list (double release).");
    // ...
}
```

### 8.3 ObjectPool 被销毁时

```cpp
ObjectPool::~ObjectPool() {
    // 销毁所有池中对象（包括活跃的和空闲的）
    for (Node *node : active_list) {
        node->_pool_owner = nullptr;
        node->queue_free();
    }
    for (Node *node : free_list) {
        node->_pool_owner = nullptr;
        node->queue_free();
    }
}
```

### 8.4 acquire 后节点被外部 queue_free()

如果用户不小心对池中节点调用了 `queue_free()`：
- 在节点的 `NOTIFICATION_PREDELETE` 中检测 `_pool_owner != nullptr`
- 自动从池的 active_list 中移除
- 打印 warning：`"Pooled node was freed directly. Use release_to_pool() instead."`

---

## 九、性能分析

### 内存模型

```
ObjectPool (initial_size=100, bullet.tscn)
├── 100 × Bullet 节点（每个约 2-5 KB）
├── active_list: Vector<Node*>（指针数组，100×8=800 bytes）
├── free_list: Vector<Node*>（指针数组）
└── _saved_collision_layers: HashMap（回收时保存的碰撞层数据）

总预分配：约 200-500 KB（对于100个简单子弹）
```

### 性能对比

| 操作 | 无池 (instantiate + queue_free) | 有池 (acquire + release) | 倍率 |
|------|------|------|------|
| 单个子弹获取 | 0.02-0.05ms | 0.001-0.002ms | **20-50x** |
| 单个子弹回收 | 0.01-0.03ms | 0.0005-0.001ms | **20-30x** |
| 100个/帧获取 | 2-5ms | 0.1-0.2ms | **20-25x** |
| 内存碎片 | 高（反复 malloc/free） | 零（预分配） | ∞ |

### 最大受益场景

| 场景 | 预估池大小 | 帧节省 |
|------|-----------|--------|
| 弹幕射击 | 500-2000 | 5-20ms/帧 |
| RTS 单位 | 200-500 | 2-10ms/帧 |
| 掉落物/金币 | 50-100 | 0.5-2ms/帧 |
| 粒子替代（Mesh 粒子） | 1000+ | 10-50ms/帧 |
| 伤害数字 UI | 30-50 | 0.3-1ms/帧 |

---

## 十、兼容性考虑

| 方面 | 分析 |
|------|------|
| **向后兼容** | ObjectPool 是新增类，不影响现有项目 |
| **Node 改动** | 新增的 `_pool_owner`、虚函数、NOTIFICATION 都是新增，不破坏现有 API |
| **2D / 3D 通用** | `_activate/_deactivate` 通过 `cast_to` 自动检测节点类型 |
| **多线程** | 池操作应在主线程进行（和 SceneTree 一致），不需要加锁 |
| **网络同步** | 池中节点的 MultiplayerSynchronizer 在 deactivate 时应暂停，activate 时恢复 |
| **编辑器安全** | `acquire()` / `release()` 应标记 `PROPERTY_USAGE_NONE` 避免编辑器中误触 |

---

## 十一、TODO

- [ ] 实现 `ObjectPool` 节点类（`.h` / `.cpp`）
- [ ] Node 基类新增 `_on_pool_acquire` / `_on_pool_release` 虚函数
- [ ] Node 基类新增 `NOTIFICATION_POOL_ACQUIRE` / `NOTIFICATION_POOL_RELEASE`
- [ ] Node 基类新增 `_pool_owner` + `get_pool()` + `release_to_pool()`
- [ ] `_activate_node()` — 自动 show / enable process / restore collision
- [ ] `_deactivate_node()` — 自动 hide / disable process / save+clear collision
- [ ] 防御性检查：double release、外部 queue_free 检测
- [ ] warm_up() 预热方法
- [ ] Inspector 运行时状态显示（free/active/total 计数）
- [ ] 注册到 `register_scene_types.cpp`
- [ ] API 文档 `doc/classes/ObjectPool.xml`
- [ ] GDScript 测试脚本
