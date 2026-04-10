
# Godot 2D 物理 —— CharacterBody2D 的 Slide 与 Sweep 深度解析

> 📂 核心源码：
> - `scene/2d/physics/character_body_2d.h/cpp` —— CharacterBody2D 主逻辑
> - `scene/2d/physics/physics_body_2d.h/cpp` —— PhysicsBody2D::move_and_collide
> - `servers/physics_2d/physics_server_2d.h` —— MotionParameters / MotionResult 结构体
> - `modules/godot_physics_2d/godot_space_2d.cpp` —— body_test_motion 底层实现（Sweep + Recovery）

---

## 一、全景概览

CharacterBody2D 的核心就是一个函数：**`move_and_slide()`**。它不参与物理模拟（不是刚体），而是每帧主动调用，通过**扫掠测试（Sweep）+ 滑动（Slide）+ 地面吸附（Snap）**来实现角色移动。

```
你写的代码                       引擎内部
─────────                       ──────────
velocity += gravity * delta      ← 你设好速度
move_and_slide()                 ← 调用一次，引擎帮你做完所有事
  │
  ├── 1. 平台速度处理
  ├── 2. 根据 motion_mode 分发
  │     ├── GROUNDED → _move_and_slide_grounded()   ← 平台跳跃类游戏
  │     └── FLOATING → _move_and_slide_floating()   ← 俯视角/太空类游戏
  │         │
  │         └── 循环最多 max_slides 次：
  │             ├── move_and_collide()               ← 单次扫掠 + 移动
  │             │     └── body_test_motion()          ← Server 层：Recovery + Sweep + Rest Info
  │             ├── _set_collision_direction()         ← 判断碰到的是地板/墙/天花板
  │             └── slide() 计算剩余运动              ← 沿碰撞面滑动
  │
  ├── 3. _snap_on_floor()                            ← 地面吸附
  └── 4. 计算 real_velocity
```

---

## 二、底层核心：body_test_motion —— 三步走

在讲 CharacterBody2D 之前，必须先理解它依赖的底层 API。所有运动检测最终都调到 `GodotSpace2D::test_body_motion()`。

这个函数的源码注释写得很坦诚：

```cpp
//give me back regular physics engine logic
//this is madness
//and most people using this function will think
//what it does is simpler than using physics
//this took about a week to get right..
//but is it right? who knows at this point..
```

它分三步：

### Step 1：Recovery（恢复/脱困）

**问题**：角色可能因为上一帧的移动或外力，已经嵌入了其他碰撞体内部。必须先把它"推出来"。

```cpp
// STEP 1, FREE BODY IF STUCK
int recover_attempts = 4;  // 最多尝试 4 次

do {
    // 1. 对每个 body shape × 每个场景中的碰撞体，做重叠检测
    GodotCollisionSolver2D::solve(body_shape, body_shape_xform,
        Vector2(),         // 不施加运动，只检测当前位置的重叠
        against_shape, col_obj_shape_xform, Vector2(),
        cbkres, cbkptr, nullptr, margin);

    // 2. 收集所有穿透点对 (a, b)
    //    a = body 上的最近点, b = 碰撞体上的最近点

    // 3. 计算恢复向量
    for (int i = 0; i < cbk.amount; i++) {
        Vector2 a = sr[i * 2 + 0];  // body 上的点
        Vector2 b = sr[i * 2 + 1];  // 碰撞体上的点

        Vector2 n = (a - b).normalized();       // 推出方向
        real_t depth = n.dot(a + recover_motion) - n.dot(b);

        if (depth > min_contact_depth + CMP_EPSILON) {
            // 按 40% 的比例推出（渐进式，避免震荡）
            // 同时考虑碰撞优先级权重
            recover_motion -= n * (depth - min_contact_depth) * 0.4 * priority_weight;
        }
    }

    // 4. 应用恢复位移
    body_transform.columns[2] += recover_motion;

} while (recover_attempts--);
```

**关键点**：
- **每次只推出 40%**（`* 0.4`），分 4 次迭代。这是为了处理多个碰撞体同时嵌入的情况——一次推太多可能会推进另一个碰撞体。
- **考虑碰撞优先级**（`collision_priority`），权重高的碰撞体推得更多。
- **One-Way Collision**：单向碰撞平台在 Recovery 阶段也要考虑方向，只在正确方向上推出。

### Step 2：Sweep（扫掠 / 二分查找安全距离）

**问题**：Recovery 后角色不再嵌入任何东西，现在要沿着 `motion` 方向移动。需要找到"最远能安全走到哪里"。

```cpp
// STEP 2 ATTEMPT MOTION

// 对每个碰撞体做二分查找
real_t low = 0.0;   // 安全比例（确定不碰撞）
real_t hi = 1.0;    // 不安全比例（确定碰撞）

for (int k = 0; k < 8; k++) {  // 8 次二分
    real_t fraction = low + (hi - low) * fraction_coeff;

    // 在 motion * fraction 处做碰撞检测
    bool collided = GodotCollisionSolver2D::solve(
        body_shape, body_shape_xform,
        p_parameters.motion * fraction,    // ← 部分运动
        against_shape, col_obj_shape_xform,
        Vector2(), nullptr, nullptr, &sep, 0);

    if (collided) {
        hi = fraction;           // 碰撞了，缩小上界
        fraction_coeff = ...;    // 自适应步长
    } else {
        low = fraction;          // 没碰撞，扩大下界
        fraction_coeff = ...;
    }
}
```

**输出**：
- `safe`：最大安全比例（0~1），乘以 motion 就是可以安全移动的距离
- `unsafe`：最小不安全比例，用于后续获取碰撞信息

**自适应步长**的技巧：
- 如果连续碰撞，`fraction_coeff = 0.25`，更快收敛到 low 侧（碰撞发生在起始端附近）
- 如果连续不碰撞，`fraction_coeff = 0.75`，更快收敛到 hi 侧

```
motion 方向 ─────────────────────────────────────────→

|←── safe ──→|←── unsafe ──→|
             ↑               ↑
        最后安全位置     首次碰撞位置
```

### Step 3：Rest Info（碰撞信息收集）

在 `unsafe` 位置（刚好碰撞的位置），再做一次精确的碰撞检测，收集：

```cpp
r_result->collision_normal  = rcd.best_normal;     // 碰撞法线
r_result->collision_point   = rcd.best_contact;     // 碰撞点
r_result->collision_depth   = rcd.best_len;         // 穿透深度
r_result->collider_velocity = ...;                  // 碰撞体速度（含角速度）
r_result->travel            = safe * motion;        // 安全移动量
r_result->remainder         = motion - travel;      // 剩余运动量
r_result->collision_safe_fraction   = safe;
r_result->collision_unsafe_fraction = unsafe;
```

### MotionResult 完整结构

```cpp
struct MotionResult {
    Vector2 travel;              // 实际安全移动的位移
    Vector2 remainder;           // 碰撞后剩余的位移

    Vector2 collision_point;     // 碰撞点（世界坐标）
    Vector2 collision_normal;    // 碰撞法线（从碰撞体指向角色）
    Vector2 collider_velocity;   // 碰撞体的速度
    real_t  collision_depth;     // 穿透深度
    real_t  collision_safe_fraction;    // 安全移动比例 [0, 1]
    real_t  collision_unsafe_fraction;  // 不安全移动比例 [0, 1]
    int     collision_local_shape;     // 角色的哪个 shape 碰撞了
    ObjectID collider_id;              // 碰撞体的 ObjectID
    RID     collider;                  // 碰撞体的 RID
    int     collider_shape;            // 碰撞体的哪个 shape

    real_t get_angle(Vector2 p_up_direction) const {
        return Math::acos(collision_normal.dot(p_up_direction));
    }
};
```

---

## 三、move_and_collide —— 单次移动

`PhysicsBody2D::move_and_collide` 是对 `body_test_motion` 的封装，加了一个重要功能：**Cancel Sliding（消除恢复导致的滑动）**。

```cpp
bool PhysicsBody2D::move_and_collide(params, result, test_only, cancel_sliding) {
    // 1. 调用底层扫掠测试
    bool colliding = PhysicsServer2D::body_test_motion(get_rid(), params, &result);

    // 2. Cancel Sliding（关键！）
    if (cancel_sliding) {
        // Recovery 阶段可能把角色沿非运动方向推了一下，
        // 导致 result.travel 不完全沿着 motion 方向。
        // 这里把 travel 投影回 motion 方向，消除横向偏移。

        Vector2 motion_normal = params.motion.normalized();
        real_t projected_length = result.travel.dot(motion_normal);
        Vector2 recovery = result.travel - motion_normal * projected_length;

        if (recovery.length() < params.margin + precision) {
            result.travel = motion_normal * projected_length;
            result.remainder = params.motion - result.travel;
        }
    }

    // 3. 如果不是仅测试，实际移动角色
    if (!test_only) {
        Transform2D gt = params.from;
        gt.columns[2] += result.travel;
        set_global_transform(gt);
    }

    return colliding;
}
```

**Cancel Sliding 解决什么问题？**

想象角色站在斜坡上不动（velocity = 0）。Recovery 阶段可能因为浮点误差把角色沿斜面法线推了一点点，这个推力有一个沿斜面向下的分量。如果不消除这个分量，角色就会在斜坡上缓慢滑动。Cancel Sliding 把 travel 投影回 motion 方向（此时是零向量），就消除了这个横向偏移。

---

## 四、move_and_slide —— 完整的一帧移动

### 4.1 入口

```cpp
bool CharacterBody2D::move_and_slide() {
    double delta = Engine::is_in_physics_frame()
        ? get_physics_process_delta_time()
        : get_process_delta_time();

    // 1. 获取当前站立平台的速度
    Vector2 current_platform_velocity = ...;

    // 2. 先随平台移动（如果站在移动平台上）
    if (!current_platform_velocity.is_zero_approx()) {
        PhysicsServer2D::MotionParameters params(get_global_transform(),
            current_platform_velocity * delta, margin);
        params.exclude_bodies.insert(platform_rid);  // 排除平台自身！

        if (move_and_collide(params, result, false, false)) {
            _set_collision_direction(result);  // 可能碰到天花板等
        }
    }

    // 3. 重置碰撞状态
    on_floor = false;
    on_ceiling = false;
    on_wall = false;

    // 4. 根据模式分发
    if (motion_mode == MOTION_MODE_GROUNDED) {
        _move_and_slide_grounded(delta, was_on_floor);
    } else {
        _move_and_slide_floating(delta);
    }

    // 5. 计算真实速度
    real_velocity = get_position_delta() / delta;

    // 6. 离开平台时继承平台速度
    if (platform_on_leave != DO_NOTHING && !on_floor && !on_wall) {
        velocity += current_platform_velocity;
    }
}
```

### 4.2 平台速度的获取

注意平台速度的获取方式——不是用存的值，而是**实时从 PhysicsServer 查**：

```cpp
PhysicsDirectBodyState2D *bs = PhysicsServer2D::body_get_direct_state(platform_rid);
Vector2 local_position = my_position - platform_position;
current_platform_velocity = bs->get_velocity_at_local_position(local_position);
```

`get_velocity_at_local_position` 考虑了平台的**线速度 + 角速度**在角色位置的合成速度。这就是为什么旋转平台上的角色也能正确跟随。

---

## 五、_move_and_slide_grounded —— 地面模式核心

这是最复杂的部分，处理平台跳跃游戏中的所有边界情况。

### 5.1 主循环结构

```cpp
void CharacterBody2D::_move_and_slide_grounded(double p_delta, bool p_was_on_floor) {
    Vector2 motion = velocity * p_delta;
    Vector2 motion_slide_up = motion.slide(up_direction);  // 运动在水平面的投影

    bool sliding_enabled = !floor_stop_on_slope;  // 第一次迭代是否允许滑动
    bool first_slide = true;
    bool vel_dir_facing_up = velocity.dot(up_direction) > 0;  // 速度是否朝上（跳跃中）

    for (int iteration = 0; iteration < max_slides; ++iteration) {
        // ① 扫掠测试 + 移动
        bool collided = move_and_collide(params, result, false, !sliding_enabled);

        if (collided) {
            _set_collision_direction(result);  // 判断碰到了什么

            // ② 各种碰撞情况处理（见下文）
            // ③ 计算剩余运动的滑动方向
        }

        sliding_enabled = true;  // 从第二次迭代开始允许滑动
        first_slide = false;

        if (!collided || motion.is_zero_approx()) break;
    }

    // ④ 地面吸附
    _snap_on_floor(p_was_on_floor, vel_dir_facing_up);

    // ⑤ 靠墙时的速度修正
    // ⑥ 着地时消除重力累积
}
```

### 5.2 碰撞方向判定

```cpp
void CharacterBody2D::_set_collision_direction(const MotionResult &result) {
    real_t angle = result.get_angle(up_direction);  // = acos(normal · up)

    if (angle <= floor_max_angle + THRESHOLD) {
        on_floor = true;                            // 地板（法线接近 up）
        floor_normal = result.collision_normal;
    } else if (result.get_angle(-up_direction) <= floor_max_angle + THRESHOLD) {
        on_ceiling = true;                          // 天花板（法线接近 -up）
    } else {
        on_wall = true;                             // 墙壁（其他角度）
        wall_normal = result.collision_normal;
    }
}
```

```
        up_direction (0, -1)
             ↑
             │
         ╱───┼───╲  floor_max_angle (默认 45°)
        ╱ FLOOR  ╲
       ╱          ╲
──────╱────────────╲──────  WALL 区域
      ╲            ╱
       ╲          ╱
        ╲CEILING ╱
         ╲───┼──╱
             │
             ↓
        -up_direction (0, 1)
```

### 5.3 Slide（滑动）—— Vector2::slide 的含义

这是整个系统最核心的数学操作：

```cpp
// Vector2::slide 的定义
Vector2 Vector2::slide(const Vector2 &p_normal) const {
    return *this - p_normal * this->dot(p_normal);
}
```

**几何含义**：将向量沿法线方向的分量去掉，只保留沿碰撞面的分量。

```
        motion (原始运动)
          ╲
           ╲
            ╲───→ slide(normal) = 沿碰撞面的分量
             ╲  ↗
              ╲╱
    ──────────●──────────── 碰撞面
              ↑
            normal
```

在代码中的使用：

```cpp
// 碰撞后，剩余运动沿碰撞面滑动
Vector2 slide_motion = result.remainder.slide(result.collision_normal);
if (slide_motion.dot(velocity) > 0.0) {
    motion = slide_motion;   // 只有滑动方向与原速度同向时才滑
} else {
    motion = Vector2();       // 否则停下（避免反弹回去）
}
```

### 5.4 六种碰撞情况的处理

Grounded 模式的碰撞后处理非常复杂，我画个决策树：

```
碰到东西了
    │
    ├── 碰到天花板平台，且平台在向下压？
    │     → 用平台速度覆盖角色的垂直速度（apply_ceiling_velocity）
    │
    ├── 碰到地板 + floor_stop_on_slope + 速度方向朝下？
    │     → 停住！velocity = 0，撤销微小位移
    │     → （站在斜坡上不滑动）
    │
    ├── floor_block_on_wall + 碰到墙 + 运动方向撞墙？
    │     ├── 之前在地上，现在不在了（走到悬崖边撞墙）？
    │     │     → 撤销位移 + snap 回地面 + 停住
    │     ├── 在空中？
    │     │     → 只保留垂直分量的滑动（不让角色爬墙）
    │     └── 在地上？
    │           → 正常使用 remainder 继续
    │
    ├── floor_constant_speed + 在地上 + 碰到上坡？
    │     → 恒速滑动：保持原水平速度大小，只改方向
    │     → motion = slide_norm * (原水平距离 - 已走水平距离)
    │
    ├── 正常滑动（sliding_enabled + 不是特殊情况）？
    │     → motion = remainder.slide(collision_normal)
    │     → 如果 slide 方向与 velocity 反向，停住
    │
    └── 第一次迭代 + floor_stop_on_slope（不允许滑动）？
          → motion = remainder（不 slide，保持原方向重试）
          → 如果碰到天花板，消除垂直分量
```

### 5.5 Floor Constant Speed（恒速上坡）

这个功能保证角色**上斜坡时水平速度不变**。

没有 constant speed 时，角色上 45° 斜坡，水平速度只剩 ~70%（因为 slide 分解了速度）。开启后：

```cpp
Vector2 motion_slide_norm = result.remainder.slide(result.collision_normal).normalized();
// 用"原始水平距离 - 已经走的水平距离"来保持恒速
motion = motion_slide_norm * (motion_slide_up.length()
    - result.travel.slide(up_direction).length()
    - last_travel.slide(up_direction).length());
```

### 5.6 第一次不滑动的技巧

```cpp
bool sliding_enabled = !floor_stop_on_slope;  // 如果开了 stop_on_slope，第一次不滑
```

第一次 `move_and_collide` 传 `cancel_sliding = !sliding_enabled`，也就是 `true`。这意味着第一次移动会 Cancel Sliding，把 Recovery 产生的横向偏移消除掉。

**为什么？** 站在斜坡上时，Recovery 会沿法线推出角色，产生沿斜面向下的分量。如果第一次就允许滑动，角色会在斜坡上抖动。第一次不滑动（Cancel Sliding），后续迭代再正常滑动，就稳定了。

---

## 六、Floor Snap（地面吸附）

### 6.1 为什么需要 Snap？

角色从斜坡顶端走下来时，由于速度有水平分量，移动结果可能让角色"飞离"斜面：

```
    角色原位置
        ○
       ╱  ╲  velocity 水平向右
      ╱    ╲→→→→
     ╱      ╲
    ╱   斜坡  ○ ← 移动后角色离开了地面！
   ╱──────────╲
```

Snap 就是在移动完成后，**向下做一次短距离的扫掠测试**，如果能碰到地板就把角色"吸"下去。

### 6.2 实现

```cpp
void CharacterBody2D::_snap_on_floor(bool p_was_on_floor, bool p_vel_dir_facing_up, bool p_wall_as_floor) {
    // 三种情况不 snap：
    if (on_floor) return;          // 已经在地板上了
    if (!p_was_on_floor) return;   // 上一帧也不在地板上（不是从地板走出去的）
    if (p_vel_dir_facing_up) return; // 正在向上跳（不应该吸回来）

    _apply_floor_snap(p_wall_as_floor);
}

void CharacterBody2D::_apply_floor_snap(bool p_wall_as_floor) {
    real_t length = MAX(floor_snap_length, margin);  // snap 距离

    // 向 -up_direction（即向下）做一次扫掠
    PhysicsServer2D::MotionParameters params(
        get_global_transform(),
        -up_direction * length,    // ← 向下探测
        margin);
    params.collide_separation_ray = true;  // 允许 SeparationRay 碰撞

    PhysicsServer2D::MotionResult result;
    if (move_and_collide(params, result, true /*test_only*/, false)) {
        // 检查碰到的面是否是地板（角度 ≤ floor_max_angle）
        if (result.get_angle(up_direction) <= floor_max_angle + THRESHOLD) {
            on_floor = true;
            floor_normal = result.collision_normal;

            // 只沿 up 方向移动，避免横向偏移
            if (result.travel.length() > margin) {
                result.travel = up_direction * up_direction.dot(result.travel);
            } else {
                result.travel = Vector2();
            }

            // 应用吸附位移
            params.from.columns[2] += result.travel;
            set_global_transform(params.from);
        }
    }
}
```

**关键细节**：
- `collide_separation_ray = true`：允许 SeparationRayShape2D 参与碰撞。这个特殊形状是一条射线，专门用于地面检测。
- Snap 只沿 up 方向移动（`up_direction * up_direction.dot(result.travel)`），避免 Recovery 产生的横向偏移。
- `floor_snap_length` 默认 1 像素，可以调大以适应更陡的下坡。

### 6.3 _on_floor_if_snapped —— 假设性检测

在 Constant Speed 逻辑中，需要知道"如果做了 snap，角色是否还在地面上？"但不能真的移动。这就是 `_on_floor_if_snapped`：

```cpp
bool CharacterBody2D::_on_floor_if_snapped(bool p_was_on_floor, bool p_vel_dir_facing_up) {
    // 和 _apply_floor_snap 一模一样的扫掠测试
    // 但用 move_and_collide(..., true /*test_only*/, ...) 
    // 只测试不移动
    ...
    if (move_and_collide(params, result, true, false)) {
        if (result.get_angle(up_direction) <= floor_max_angle + THRESHOLD) {
            return true;  // 如果 snap 的话会在地面上
        }
    }
    return false;
}
```

---

## 七、_move_and_slide_floating —— 浮动模式

浮动模式比 Grounded 简单得多，因为没有地面/天花板/重力的概念：

```cpp
void CharacterBody2D::_move_and_slide_floating(double p_delta) {
    Vector2 motion = velocity * p_delta;

    for (int iteration = 0; iteration < max_slides; ++iteration) {
        bool collided = move_and_collide(params, result, false, false);

        if (collided) {
            _set_collision_direction(result);

            if (result.remainder.is_zero_approx()) {
                break;
            }

            // wall_min_slide_angle 检查：
            // 如果碰撞角度太小（几乎正面撞墙），不滑动，直接停
            if (wall_min_slide_angle != 0 &&
                result.get_angle(-velocity.normalized()) < wall_min_slide_angle) {
                motion = Vector2();
            }
            // 第一次碰撞：恒速滑动（保持总速度大小）
            else if (first_slide) {
                Vector2 motion_slide_norm = result.remainder.slide(result.collision_normal).normalized();
                motion = motion_slide_norm * (motion.length() - result.travel.length());
            }
            // 后续碰撞：普通滑动
            else {
                motion = result.remainder.slide(result.collision_normal);
            }

            // 防止滑动方向与原速度反向
            if (motion.dot(velocity) <= 0.0) {
                motion = Vector2();
            }
        }

        if (!collided || motion.is_zero_approx()) break;
        first_slide = false;
    }
}
```

---

## 八、完整的一帧时序图

```
              move_and_slide() 调用
                      │
     ┌────────────────┴────────────────┐
     │  1. 获取平台速度                  │
     │  2. 随平台移动 (move_and_collide) │
     │  3. 重置 on_floor/wall/ceiling   │
     └────────────────┬────────────────┘
                      │
                      ▼
     ┌─────── _move_and_slide_grounded ───────┐
     │                                         │
     │  for iteration in 0..max_slides:        │
     │    │                                    │
     │    ├── move_and_collide(motion)          │
     │    │     │                               │
     │    │     ├── body_test_motion()          │
     │    │     │     ├── Step1: Recovery       │
     │    │     │     ├── Step2: Sweep (二分)   │
     │    │     │     └── Step3: Rest Info      │
     │    │     │                               │
     │    │     └── Cancel Sliding (可选)       │
     │    │                                    │
     │    ├── _set_collision_direction()        │
     │    │     └── 判断 floor/wall/ceiling     │
     │    │                                    │
     │    └── 计算下一次迭代的 motion           │
     │          ├── stop_on_slope → 停住        │
     │          ├── block_on_wall → 阻止/吸附   │
     │          ├── constant_speed → 恒速滑动   │
     │          └── 普通 slide → remainder.slide │
     │                                         │
     └──────────────────┬──────────────────────┘
                        │
                        ▼
     ┌─── _snap_on_floor ───┐
     │  向下扫掠探测          │
     │  如果碰到地板 → 吸附   │
     └──────────┬───────────┘
                │
                ▼
     ┌─── 后处理 ───────────────────┐
     │  real_velocity = delta_pos/dt │
     │  靠墙时速度修正               │
     │  着地时消除重力累积           │
     │  离开平台时继承平台速度       │
     └──────────────────────────────┘
```

---

## 九、关键参数速查表

| 参数 | 默认值 | 含义 |
|---|---|---|
| `velocity` | (0,0) | 角色速度，你设置，引擎修改 |
| `up_direction` | (0,-1) | "上"方向，用于判断 floor/wall/ceiling |
| `motion_mode` | GROUNDED | GROUNDED（平台跳跃）或 FLOATING（俯视角） |
| `max_slides` | 4 | 最大滑动迭代次数 |
| `margin` | 0.08px | 碰撞安全边距 |
| `floor_max_angle` | 45° | 法线与 up 的夹角 ≤ 此值视为地板 |
| `floor_stop_on_slope` | true | 站在斜坡上时不滑动 |
| `floor_constant_speed` | false | 上/下坡时保持水平速度不变 |
| `floor_block_on_wall` | true | 防止在悬崖边缘撞墙后飘出去 |
| `floor_snap_length` | 1px | 向下吸附的最大距离 |
| `slide_on_ceiling` | true | 碰到天花板时是否滑动 |
| `wall_min_slide_angle` | 15° | 浮动模式下，碰撞角度小于此值不滑动 |
| `platform_on_leave` | ADD_VELOCITY | 离开移动平台时如何处理平台速度 |
| `platform_floor_layers` | 0xFFFFFFFF | 哪些碰撞层视为地板平台 |
| `platform_wall_layers` | 0 | 哪些碰撞层视为墙壁平台 |

---

## 十、常见问题与原理

### Q: 为什么角色在斜坡上抖动？
**A:** 通常是 `floor_stop_on_slope` 没开，或者 `margin` 太小。`margin` 太小导致 Recovery 不够，角色反复嵌入/推出。

### Q: 为什么角色下坡时会"飞"起来？
**A:** `floor_snap_length` 太小。增大它，或者在下坡时手动给一个朝下的速度。

### Q: max_slides = 4 是什么意思？
**A:** 角色一帧内最多碰撞/滑动 4 次。通常够用。想象一下：碰到地板 → 碰到墙 → 碰到另一面墙 → 碰到天花板，4 次就处理完了。

### Q: move_and_slide 和 move_and_collide 的区别？
**A:** `move_and_collide` 只做一次移动，碰到就停。`move_and_slide` 是多次 `move_and_collide` + 滑动 + 吸附的完整封装。绝大多数情况下用 `move_and_slide` 就够了。

### Q: Cancel Sliding 到底消除了什么？
**A:** Recovery（脱困）阶段可能沿碰撞法线推出角色，这个推力在碰撞面切线方向有一个分量。Cancel Sliding 把这个切线分量去掉，只保留沿运动方向的分量。防止角色在斜坡上静止时因为 Recovery 而缓慢滑动。

---

## 十一、设计亮点总结

### ✅ 三步走的 body_test_motion
Recovery → Sweep → Rest Info 三步分离，每步职责清晰。Recovery 处理"已经嵌入"的情况，Sweep 处理"将要碰撞"的情况。

### ✅ 二分查找 + 自适应步长
8 次二分就能精确到 1/256 的运动精度，自适应 `fraction_coeff` 让起始端碰撞和末端碰撞都能快速收敛。

### ✅ 渐进式 Recovery
每次只推出 40%，4 次迭代处理多碰撞体嵌入，比一次推到位稳定得多。

### ✅ Grounded 模式的完备性
`floor_stop_on_slope`、`floor_constant_speed`、`floor_block_on_wall`、`slide_on_ceiling` 四个开关组合覆盖了平台跳跃游戏的几乎所有边界情况。

### ✅ 平台速度的精确追踪
先随平台移动（排除平台自身碰撞），再做角色自身移动，避免了平台和角色移动互相干扰。离开平台时继承速度的三种策略也很实用。
