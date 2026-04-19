# 动态障碍物影响寻路 — 技术设计文档

> **状态**: 草案 v1.0  
> **范围**: Godot Navigation3D 模块  
> **日期**: 2025-01

---

## 1. 背景与现状分析

### 1.1 导航系统架构概览

Godot 的 3D 导航系统采用 **双缓冲 ping-pong 架构**（`iteration_slots[2]`），将地图构建与路径查询解耦：

```
NavMap3D::sync()
  ├── _sync_dirty_map_update_requests()   // 同步脏数据
  ├── _build_iteration()                  // 构建新 iteration（可异步）
  │     └── NavMapBuilder3D::build_navmap_iteration()
  │           ├── _build_step_gather_region_polygons()
  │           ├── _build_step_find_edge_connection_pairs()
  │           ├── _build_step_merge_edge_connection_pairs()
  │           ├── _build_step_edge_connection_margin_connections()
  │           ├── _build_step_navlink_connections()
  │           └── _build_update_map_iteration()
  ├── _sync_iteration()                   // ping-pong 切换 slot
  └── _sync_avoidance()                   // RVO 避障（独立于寻路）
```

**关键数据流**: `NavRegion3D` → `NavRegionIteration3D`（快照）→ `NavMapIteration3D`（含多边形、连接图）→ `NavMeshQueries3D`（A* + 漏斗）

### 1.2 NavObstacle3D 现状

当前 `NavObstacle3D` **仅参与 RVO 碰撞避免**，不影响 NavMesh 多边形和 A* 搜索：

- **数据**: `position`, `vertices`, `radius`, `height`, `velocity`
- **用途**: 在 `_sync_avoidance()` → `_update_rvo_obstacles_tree_2d()` 中被送入 RVO2D/3D 仿真
- **与寻路的关系**: **零** — `_build_iteration()` 和 `query_path()` 完全不读取 obstacles 列表

### 1.3 ProjectedObstruction（静态烘焙时）

场景层 `NavigationObstacle3D` 有两个属性：
- `affect_navigation_mesh`: 在烘焙时将障碍物形状作为 `ProjectedObstruction` 加入 source geometry
- `carve_navigation_mesh`: 控制是在 `rcErodeWalkableArea` 之前还是之后标记 `RC_NULL_AREA`

这是**静态烘焙时**的 carve 机制，在 `nav_mesh_generator_3d.cpp` 中通过 `rcMarkConvexPolyArea` 实现。一旦烘焙完成，NavMesh 就固定了，运行时移动障碍物不会触发重新烘焙。

### 1.4 A* 搜索核心流程

路径查询在 `nav_mesh_queries_3d.cpp` 的 `_query_task_build_path_corridor` 中：

```
1. 初始化 begin_polygon 的 NavigationPoly，traveled_distance = 0
2. while (true):
   a. 取当前 least_cost_poly
   b. 遍历其 internal_connections（同 region 内部边邻接）
   c. 遍历其 external_connections（跨 region 边合并 / 边距连接 / NavLink）
   d. 对每个 connection 调用 _query_task_search_polygon_connections:
      - new_traveled_distance = entry→new_entry 距离 × poly_travel_cost + enter_cost + accumulated
      - 如果 new_traveled_distance < neighbor.traveled_distance → 更新并入堆
   e. 从堆中 pop 最小 f-cost 的 poly
   f. 如果 pop 出的是 end_poly → found_route, break
3. 后处理：漏斗算法 / 边中心 / 无处理
```

**代价模型**: `traveled_distance`（g）= Σ(段距离 × owner.travel_cost) + Σ(enter_cost)；`distance_to_destination`（h）= 到终点距离 × connection_owner.travel_cost

---

## 2. 需求定义

### 2.1 功能需求

| ID | 描述 |
|----|------|
| F1 | 运行时动态障碍物（`NavObstacle3D`）能够影响 A* 寻路结果 |
| F2 | Agent 寻路时能够绕开被障碍物覆盖的多边形或 portal 边 |
| F3 | 障碍物移动/移除后，后续寻路查询自动反映新状态 |
| F4 | 与现有 RVO 避障系统正交，可同时使用 |
| F5 | 支持通过属性控制障碍物对寻路的影响程度（完全阻断 vs 代价增加） |

### 2.2 非功能需求

| ID | 描述 |
|----|------|
| NF1 | 不应导致同步帧时间（`NavMap3D::sync()`）显著增加（< 0.5ms @ 1000 多边形 + 50 障碍物） |
| NF2 | 不应破坏双缓冲无锁读的架构 |
| NF3 | 路径查询的延迟增加应 < 20% |
| NF4 | 向后兼容：默认行为不变，需显式启用 |

---

## 3. 方案对比

### 方案 A：NavMesh 局部重建（Runtime Tile Re-bake）

**思路**: 将 NavMesh 分成固定大小的 tile，当障碍物进入/离开某个 tile 时，触发该 tile 的局部重新烘焙。

**流程**:
1. 在 `NavMap3D::sync()` 中检测 obstacles_dirty
2. 对每个脏障碍物，确定其影响的 tile 范围
3. 收集 tile 内原始 source geometry + 障碍物投影
4. 调用 Recast 局部 rebake → 生成新的 polygon 集合
5. 替换 `NavRegionIteration3D` 中对应 tile 的多边形
6. 重建连接图

| 维度 | 评估 |
|------|------|
| **准确性** | ★★★★★ — 物理上正确地切割 NavMesh |
| **实时性** | ★★☆☆☆ — Recast 烘焙耗时（即使只烘一个 tile，也在 ms 级别） |
| **实现复杂度** | ★☆☆☆☆ — 极高：需 tile 化 NavMesh、增量 source geometry 管理、tile 边界缝合 |
| **内存开销** | ★★☆☆☆ — 需保留原始 source geometry 和 tile 索引 |
| **对现有架构影响** | ★★☆☆☆ — 需大幅修改 NavRegion、NavMapBuilder、引入 tile 概念 |
| **向后兼容** | ★★★☆☆ — tile 化会改变 NavMesh 拓扑 |

**结论**: 工业级方案（Detour/Recast TileCache 做法），但在 Godot 当前架构下改动量极大，适合作为长期目标。

---

### 方案 B：A* 代价注入（Query-Time Cost Injection）— ✅ 推荐

**思路**: 不修改 NavMesh 拓扑，而是在 A* 查询时实时计算障碍物对多边形/portal 的覆盖，注入额外代价或直接跳过被阻断的连接。

**流程**:
1. 在 `NavMapIteration3D` 中维护障碍物快照列表（`obstacle_iterations`）
2. 在 `_build_iteration()` 中把当前活跃障碍物的几何信息快照进 iteration
3. **可选预计算**: 在 build 阶段预计算每个多边形/portal 边与障碍物的重叠关系（空间哈希加速）
4. 在 `_query_task_search_polygon_connections()` 中，检查 connection 的 portal 边是否被障碍物阻断/部分遮挡，据此增加 travel cost 或完全 skip
5. 在 `_query_task_find_start_end_positions()` 中，排除被完全覆盖的多边形作为起点/终点

| 维度 | 评估 |
|------|------|
| **准确性** | ★★★★☆ — 路径能绕开障碍物，但不会真正改变 NavMesh 几何 |
| **实时性** | ★★★★★ — 只在查询时做检测，障碍物移动即时生效 |
| **实现复杂度** | ★★★★☆ — 改动集中在查询层 + iteration 快照 |
| **内存开销** | ★★★★☆ — 仅额外存储障碍物快照（位置 + 形状） |
| **对现有架构影响** | ★★★★★ — 完美适配 ping-pong 架构，不改变 NavMesh 拓扑 |
| **向后兼容** | ★★★★★ — 默认关闭，通过 `affect_pathfinding` 属性启用 |

**结论**: 改动量可控，与现有架构高度兼容，实时性最佳。推荐作为 v1 实现。

---

### 方案 C：混合方案（Portal 裁剪 + 代价注入）

**思路**: 在 iteration build 阶段，对被障碍物部分遮挡的 portal 边进行裁剪（缩短 `pathway_start`/`pathway_end`），被完全遮挡的 portal 直接移除。

这是方案 B 的增强版，在 build 阶段做更多预计算以减少查询时开销，但需要修改连接图结构。

| 维度 | 评估 |
|------|------|
| **准确性** | ★★★★★ — Portal 裁剪 + 代价注入，接近方案 A 效果 |
| **实时性** | ★★★★☆ — build 阶段有额外开销，但查询更快 |
| **实现复杂度** | ★★★☆☆ — 需要 portal 裁剪算法 + 连接图修改 |
| **对现有架构影响** | ★★★☆☆ — 需修改 `NavMapBuilder3D` 和连接数据结构 |

**结论**: 方案 B 成熟后的自然演进方向。

---

### 方案对比总结

| | 方案 A (Tile Re-bake) | **方案 B (代价注入)** ✅ | 方案 C (Portal 裁剪) |
|---|---|---|---|
| 改动量 | ~3000 行 | **~800 行** | ~1500 行 |
| 上线周期 | 8-12 周 | **3-5 周** | 5-8 周 |
| 运行时开销 | build 阶段高 | 查询阶段低-中 | build 中 + 查询低 |
| 路径质量 | 最优 | 良好 | 优秀 |
| 风险 | 高 | **低** | 中 |

---

## 4. 推荐方案详细设计 — A* 代价注入

### 4.1 整体架构

```
                   NavMap3D::sync()
                         │
    ┌────────────────────┼────────────────────┐
    │                    │                    │
  regions sync      obstacles sync       avoidance sync
    │                    │                    │
    ▼                    ▼                    ▼
  _build_iteration()   障碍物快照 ──→ NavMapIteration3D.obstacle_snapshots
    │                                         │
    ▼                                         │
  NavMapBuilder3D                             │
  (构建连接图)                                │
    │                                         │
    ▼                                         │
  NavMapIteration3D ◄─────────────────────────┘
  (polygons + connections + obstacle_snapshots)
    │
    ▼
  NavMeshQueries3D::_query_task_build_path_corridor()
    │
    ├── _query_task_search_polygon_connections()
    │     └── _compute_obstacle_cost_for_connection()  ◄── 新增
    │           ├── 检查 portal 边是否被障碍物遮挡
    │           ├── 计算遮挡比例 → 额外代价
    │           └── 完全阻断 → skip connection
    │
    └── _query_task_find_start_end_positions()
          └── _is_polygon_blocked_by_obstacle()  ◄── 新增（可选）
```

### 4.2 障碍物快照机制

**设计原则**: 遵循现有 ping-pong 架构，障碍物数据以快照形式写入 `NavMapIteration3D`。

在 `_build_iteration()` 中（与 region/link iteration 同级），遍历当前 `obstacles` 列表，将每个启用了 `affect_pathfinding` 的障碍物的几何信息快照进 `NavMapIteration3D`。

快照在 `_build_iteration()` 中写入、在 `_sync_iteration()` 中随 slot 切换原子生效、在 `query_path()` 中只读访问。

### 4.3 障碍物-Portal 碰撞检测

核心问题：判断一个 **2D 投影的凸多边形/圆形**（障碍物）是否与一条 **3D 线段**（portal 边 `pathway_start` → `pathway_end`）相交。

**检测策略**:

1. 将障碍物和 portal 边投影到 XZ 平面（忽略 Y，或做高度范围检查）
2. 对圆形障碍物：计算 portal 线段到圆心的最近距离，与 radius 比较
3. 对凸多边形障碍物：使用 SAT（Separating Axis Theorem）检测线段与凸多边形的相交

**代价计算**:

```
如果 portal 完全在障碍物内部:
    → skip connection（相当于 cost = ∞）
如果 portal 部分被遮挡:
    → 计算遮挡比例 overlap_ratio ∈ [0, 1]
    → extra_cost = obstacle_cost_multiplier × overlap_ratio × segment_length
    → new_traveled_distance += extra_cost
如果 portal 不与障碍物相交:
    → 无额外代价
```

### 4.4 预计算优化（空间哈希）

为避免在每次 A* 扩展时对所有障碍物做碰撞检测：

1. 在 `_build_iteration()` 阶段，将障碍物按 AABB 插入空间哈希网格
2. 在 `_query_task_search_polygon_connections()` 中，只查询 portal 边 AABB 附近的障碍物
3. 空间哈希网格尺寸建议与 NavMesh 的 `cell_size` 对齐

**替代方案**: 如果障碍物数量 < 20，直接暴力遍历即可，空间哈希在障碍物 > 50 时才有意义。

### 4.5 属性设计

在 `NavObstacle3D` 上新增：

| 属性 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `affect_pathfinding` | `bool` | `false` | 是否影响 A* 寻路 |
| `pathfinding_cost_multiplier` | `real_t` | `FLT_MAX` | 障碍物区域的代价乘数，`FLT_MAX` = 完全阻断 |

在场景层 `NavigationObstacle3D` 上暴露对应属性。

---

## 5. 文件修改清单

### 5.1 服务端层（modules/navigation_3d）

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `nav_obstacle_3d.h` | 修改 | 新增 `affect_pathfinding`, `pathfinding_cost_multiplier` 字段及 getter/setter |
| `nav_obstacle_3d.cpp` | 修改 | 实现新属性的 setter，触发 `obstacle_dirty` |
| `nav_utils_3d.h` | 修改 | 新增 `ObstacleSnapshot` 结构体 |
| `nav_map_3d.h` | 修改 | 在 `_build_iteration()` 中增加障碍物快照收集逻辑 |
| `nav_map_3d.cpp` | 修改 | `_build_iteration()` 中快照 obstacles 到 iteration |
| `3d/nav_map_iteration_3d.h` | 修改 | `NavMapIteration3D` 新增 `obstacle_snapshots` 字段 |
| `3d/nav_map_builder_3d.h` | 修改 | `NavMapIterationBuild3D` 新增障碍物相关 build 上下文 |
| `3d/nav_map_builder_3d.cpp` | 修改 | 新增 `_build_step_obstacle_spatial_index()` 构建空间索引 |
| `3d/nav_mesh_queries_3d.h` | 修改 | 新增 `_compute_obstacle_cost_for_connection()` 声明 |
| `3d/nav_mesh_queries_3d.cpp` | **核心修改** | 在 `_query_task_search_polygon_connections()` 中注入障碍物代价 |

### 5.2 场景层（scene/3d/navigation）

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `navigation_obstacle_3d.h` | 修改 | 新增属性声明 |
| `navigation_obstacle_3d.cpp` | 修改 | 绑定新属性，调用 NavigationServer API |

### 5.3 服务端 API（servers/navigation_3d）

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `navigation_server_3d.h` | 修改 | 新增 `obstacle_set_affect_pathfinding()` 等虚方法 |
| `navigation_server_3d.cpp` | 修改 | 绑定方法 |

### 5.4 GodotNavigationServer3D 实现

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `3d/godot_navigation_server_3d.h` | 修改 | override 新方法 |
| `3d/godot_navigation_server_3d.cpp` | 修改 | 实现：转发到 `NavObstacle3D` |

---

## 6. 数据结构设计

### 6.1 ObstacleSnapshot（nav_utils_3d.h 中新增）

```cpp
namespace Nav3D {

struct ObstacleSnapshot {
    Vector3 position;                    // 世界坐标位置
    real_t radius = 0.0;                 // 圆形障碍物半径（radius > 0 时用圆形检测）
    real_t height = 0.0;                 // 高度范围
    LocalVector<Vector3> vertices;       // 凸多边形顶点（世界坐标）
    real_t cost_multiplier = FLT_MAX;    // 代价乘数，FLT_MAX = 完全阻断
    AABB aabb;                           // 预计算的 AABB，用于空间索引快速剔除
};

} // namespace Nav3D
```

### 6.2 NavMapIteration3D 扩展

```cpp
struct NavMapIteration3D {
    // ... existing fields ...

    // 动态障碍物快照（用于 A* 代价注入）
    LocalVector<Nav3D::ObstacleSnapshot> obstacle_snapshots;

    // 空间哈希索引：cell_key → obstacle_snapshot_indices
    HashMap<uint64_t, LocalVector<uint32_t>> obstacle_spatial_hash;
    Vector3 obstacle_hash_cell_size;  // 空间哈希网格尺寸

    void clear() {
        // ... existing clear ...
        obstacle_snapshots.clear();
        obstacle_spatial_hash.clear();
    }
};
```

### 6.3 NavObstacle3D 扩展

```cpp
class NavObstacle3D : public NavRid3D {
    // ... existing fields ...

    bool affect_pathfinding = false;
    real_t pathfinding_cost_multiplier = FLT_MAX;  // 默认完全阻断

public:
    void set_affect_pathfinding(bool p_enabled);
    bool get_affect_pathfinding() const { return affect_pathfinding; }

    void set_pathfinding_cost_multiplier(real_t p_cost);
    real_t get_pathfinding_cost_multiplier() const { return pathfinding_cost_multiplier; }
};
```

### 6.4 空间哈希 Key 计算

```cpp
// 复用 NavMap3D 已有的 PointKey 机制
static uint64_t _obstacle_hash_key(const Vector3 &p_pos, const Vector3 &p_cell_size) {
    int x = (int)Math::floor(p_pos.x / p_cell_size.x);
    int y = (int)Math::floor(p_pos.y / p_cell_size.y);
    int z = (int)Math::floor(p_pos.z / p_cell_size.z);
    return ((uint64_t)(x & 0x1FFFFF)) |
           ((uint64_t)(y & 0x3FFFFF) << 21) |
           ((uint64_t)(z & 0x1FFFFF) << 43);
}
```

---

## 7. 核心伪代码

### 7.1 障碍物快照收集（NavMap3D::_build_iteration）

```cpp
void NavMap3D::_build_iteration() {
    // ... existing code (regions, links) ...

    // ========== 新增：收集障碍物快照 ==========
    NavMapIteration3D &next_map_iteration = iteration_slots[(iteration_slot_index + 1) % 2];

    next_map_iteration.obstacle_snapshots.clear();
    for (NavObstacle3D *obstacle : obstacles) {
        if (!obstacle->get_affect_pathfinding()) {
            continue;
        }

        Nav3D::ObstacleSnapshot snapshot;
        snapshot.position = obstacle->get_position();
        snapshot.radius = obstacle->get_radius();
        snapshot.height = obstacle->get_height();
        snapshot.cost_multiplier = obstacle->get_pathfinding_cost_multiplier();

        const Vector<Vector3> &verts = obstacle->get_vertices();
        if (verts.size() >= 3) {
            snapshot.vertices.resize(verts.size());
            for (int i = 0; i < verts.size(); i++) {
                snapshot.vertices[i] = snapshot.position + verts[i];
            }
        }

        // 计算 AABB
        if (snapshot.radius > 0.0) {
            Vector3 half_ext(snapshot.radius, snapshot.height * 0.5, snapshot.radius);
            snapshot.aabb = AABB(snapshot.position - half_ext, half_ext * 2.0);
        } else if (snapshot.vertices.size() > 0) {
            snapshot.aabb = AABB(snapshot.vertices[0], Vector3());
            for (const Vector3 &v : snapshot.vertices) {
                snapshot.aabb.expand_to(v);
            }
            snapshot.aabb.position.y = snapshot.position.y;
            snapshot.aabb.size.y = snapshot.height;
        }

        next_map_iteration.obstacle_snapshots.push_back(snapshot);
    }

    // ========== 新增：构建空间哈希 ==========
    next_map_iteration.obstacle_hash_cell_size = merge_rasterizer_cell_size * 4.0;
    next_map_iteration.obstacle_spatial_hash.clear();

    for (uint32_t i = 0; i < next_map_iteration.obstacle_snapshots.size(); i++) {
        const AABB &aabb = next_map_iteration.obstacle_snapshots[i].aabb;
        Vector3 cell_size = next_map_iteration.obstacle_hash_cell_size;

        int min_x = (int)Math::floor(aabb.position.x / cell_size.x);
        int min_z = (int)Math::floor(aabb.position.z / cell_size.z);
        int max_x = (int)Math::floor((aabb.position.x + aabb.size.x) / cell_size.x);
        int max_z = (int)Math::floor((aabb.position.z + aabb.size.z) / cell_size.z);

        for (int x = min_x; x <= max_x; x++) {
            for (int z = min_z; z <= max_z; z++) {
                uint64_t key = _obstacle_hash_key(Vector3(x * cell_size.x, 0, z * cell_size.z), cell_size);
                next_map_iteration.obstacle_spatial_hash[key].push_back(i);
            }
        }
    }

    // ... existing build code ...
}
```

### 7.2 A* 代价注入（nav_mesh_queries_3d.cpp）

```cpp
// 新增：计算 portal 边上的障碍物额外代价
static real_t _compute_obstacle_cost_for_connection(
    const Nav3D::Connection &p_connection,
    const NavMapIteration3D &p_map_iteration
) {
    if (p_map_iteration.obstacle_snapshots.is_empty()) {
        return 0.0;
    }

    const Vector3 &seg_start = p_connection.pathway_start;
    const Vector3 &seg_end = p_connection.pathway_end;

    // 计算 portal 边中点用于空间哈希查询
    Vector3 seg_mid = (seg_start + seg_end) * 0.5;
    Vector3 cell_size = p_map_iteration.obstacle_hash_cell_size;
    uint64_t hash_key = _obstacle_hash_key(seg_mid, cell_size);

    const HashMap<uint64_t, LocalVector<uint32_t>>::ConstIterator it =
        p_map_iteration.obstacle_spatial_hash.find(hash_key);

    if (!it) {
        return 0.0;  // 该空间格子内无障碍物
    }

    real_t max_extra_cost = 0.0;
    real_t seg_length = seg_start.distance_to(seg_end);
    if (seg_length < CMP_EPSILON) {
        seg_length = CMP_EPSILON;
    }

    for (uint32_t obs_idx : it->value) {
        const Nav3D::ObstacleSnapshot &obs = p_map_iteration.obstacle_snapshots[obs_idx];

        // 高度范围检查
        real_t portal_y = seg_mid.y;
        if (portal_y < obs.aabb.position.y || portal_y > obs.aabb.position.y + obs.aabb.size.y) {
            continue;
        }

        real_t overlap_ratio = 0.0;

        if (obs.radius > 0.0) {
            // 圆形障碍物：计算线段到圆心的最近距离
            Vector3 closest = Geometry3D::get_closest_point_to_segment(
                obs.position, seg_start, seg_end);
            real_t dist_xz = Vector2(closest.x - obs.position.x, closest.z - obs.position.z).length();

            if (dist_xz < obs.radius) {
                // 估算遮挡比例
                real_t chord_half = Math::sqrt(obs.radius * obs.radius - dist_xz * dist_xz);
                overlap_ratio = MIN(1.0, (chord_half * 2.0) / seg_length);
            }
        } else if (obs.vertices.size() >= 3) {
            // 凸多边形障碍物：投影到 XZ 平面做 2D SAT 检测
            // 简化：检查线段端点是否在多边形内 + 线段是否与多边形边相交
            overlap_ratio = _compute_segment_polygon_overlap_xz(
                seg_start, seg_end, obs.vertices);
        }

        if (overlap_ratio > 0.0) {
            if (obs.cost_multiplier >= FLT_MAX * 0.5) {
                return FLT_MAX;  // 完全阻断
            }
            real_t cost = obs.cost_multiplier * overlap_ratio * seg_length;
            max_extra_cost = MAX(max_extra_cost, cost);
        }
    }

    return max_extra_cost;
}
```

### 7.3 修改 `_query_task_search_polygon_connections`

```cpp
void NavMeshQueries3D::_query_task_search_polygon_connections(
    NavMeshPathQueryTask3D &p_query_task,
    const Connection &p_connection,
    uint32_t p_least_cost_id,
    const NavigationPoly &p_least_cost_poly,
    real_t p_poly_enter_cost,
    const Vector3 &p_end_point
) {
    // ... existing owner usability check ...

    // ========== 新增：障碍物代价检查 ==========
    real_t obstacle_extra_cost = _compute_obstacle_cost_for_connection(
        p_connection, *p_query_task.current_map_iteration);

    if (obstacle_extra_cost >= FLT_MAX * 0.5) {
        return;  // 完全阻断，跳过此连接
    }
    // ========== 新增结束 ==========

    // ... existing code ...

    real_t new_traveled_distance =
        p_least_cost_poly.entry.distance_to(new_entry) * poly_travel_cost
        + p_poly_enter_cost
        + p_least_cost_poly.traveled_distance
        + obstacle_extra_cost;  // ◄── 新增：加上障碍物额外代价

    // ... rest of existing code ...
}
```

### 7.4 传递 map_iteration 引用到查询

需要在 `NavMeshPathQueryTask3D` 中新增一个指向当前 `NavMapIteration3D` 的只读指针：

```cpp
struct NavMeshPathQueryTask3D {
    // ... existing fields ...
    const NavMapIteration3D *current_map_iteration = nullptr;  // ◄── 新增
};
```

在 `query_task_map_iteration_get_path()` 入口处赋值：

```cpp
void NavMeshQueries3D::query_task_map_iteration_get_path(
    NavMeshPathQueryTask3D &p_query_task,
    const NavMapIteration3D &p_map_iteration
) {
    p_query_task.current_map_iteration = &p_map_iteration;  // ◄── 新增
    // ... existing code ...
}
```

---

## 8. 工作量评估

### 8.1 任务分解

| 阶段 | 任务 | 估时 | 优先级 |
|------|------|------|--------|
| **P0** | `NavObstacle3D` 新增属性 + setter | 0.5d | 高 |
| **P0** | `NavigationObstacle3D` 场景层属性绑定 | 0.5d | 高 |
| **P0** | `NavigationServer3D` API 层新增方法 | 0.5d | 高 |
| **P0** | `ObstacleSnapshot` 数据结构 | 0.5d | 高 |
| **P1** | `NavMap3D::_build_iteration()` 障碍物快照收集 | 1d | 高 |
| **P1** | `NavMapIteration3D` 存储扩展 | 0.5d | 高 |
| **P2** | 空间哈希索引实现 | 1d | 中 |
| **P2** | 圆形障碍物 - Portal 碰撞检测 | 1d | 高 |
| **P2** | 凸多边形障碍物 - Portal 碰撞检测 | 1.5d | 高 |
| **P3** | `_query_task_search_polygon_connections()` 代价注入 | 1d | 核心 |
| **P3** | `NavMeshPathQueryTask3D` 传递 map_iteration 引用 | 0.5d | 核心 |
| **P4** | 单元测试 | 2d | 高 |
| **P4** | 集成测试 + 性能基准 | 2d | 高 |
| **P5** | Debug 可视化（障碍物影响区域） | 1d | 中 |
| | **总计** | **~13d (≈ 3 周)** | |

### 8.2 里程碑

| 里程碑 | 时间 | 产出 |
|--------|------|------|
| M1 - 数据通路 | 第 1 周 | 属性 → 快照 → iteration 全链路打通 |
| M2 - 核心功能 | 第 2 周 | A* 代价注入 + 基本碰撞检测，圆形障碍物可用 |
| M3 - 完整功能 | 第 3 周 | 凸多边形支持 + 空间哈希 + 测试 + 性能调优 |

---

## 9. 风险分析与缓解

### 9.1 风险矩阵

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| **R1**: 查询性能退化 — 大量障碍物导致 A* 每次扩展变慢 | 中 | 高 | 空间哈希预筛选；当障碍物 = 0 时零开销 fast path；性能基准门控 |
| **R2**: 路径质量下降 — 代价注入只能让路径绕远，无法精确绕开障碍物边缘 | 低 | 中 | portal 裁剪（方案 C）作为 v2 增强；漏斗算法本身会拉紧路径 |
| **R3**: 线程安全 — 查询线程读取 obstacle_snapshots 时 build 线程正在写入 | 低 | 高 | ping-pong 架构天然保护：读写在不同 slot，slot 切换由 `iteration_slot_rwlock` 保护 |
| **R4**: 障碍物形状精度 — 圆形/凸多边形近似可能导致误判 | 低 | 低 | 提供 `pathfinding_cost_multiplier` 让用户调节严格程度 |
| **R5**: 与 excluded_regions / included_regions 的交互 | 低 | 中 | 障碍物代价注入在 region 过滤之后执行，正交不冲突 |
| **R6**: 障碍物移动频率过高导致 iteration 频繁 rebuild | 中 | 中 | 障碍物快照收集是轻量操作（只拷贝 position/vertices），不触发 NavMesh 重建；可加入脏检测只在变化时更新快照 |
| **R7**: 起终点在障碍物内部的退化场景 | 中 | 中 | `_query_task_find_start_end_positions()` 中加入障碍物检查，标记但不拒绝（允许路径从障碍物内部出发） |

### 9.2 向后兼容性保证

- `affect_pathfinding` 默认 `false` → 现有项目行为不变
- `pathfinding_cost_multiplier` 默认 `FLT_MAX` → 启用后默认完全阻断（最保守行为）
- 无 obstacle_snapshots 时，`_compute_obstacle_cost_for_connection()` 直接返回 0（零开销）
- 不改变 `NavMesh` 拓扑，不影响 `get_closest_point()` 等静态查询

### 9.3 已知限制

1. **路径不会贴着障碍物边缘走** — 代价注入只影响 portal 选择，漏斗算法不感知障碍物。需要方案 C 的 portal 裁剪来改善。
2. **不支持高度差复杂场景** — 当前的高度检查是简单的 Y 范围比较，不处理斜面。
3. **不自动触发 re-path** — 障碍物移动后，已在执行中的 agent 不会自动重新寻路（需要游戏逻辑层配合）。

---

## 10. 后续演进

### 10.1 短期（v1.1）
- **Portal 裁剪**（方案 C）：在 build 阶段裁剪 `pathway_start`/`pathway_end`，提升路径精度
- **自动 re-path 通知**：当障碍物状态变化时，通知正在使用受影响多边形的 agent 重新寻路

### 10.2 中期（v2）
- **Tile 化 NavMesh**（方案 A 的前置条件）：将 NavMesh 分 tile 管理，支持局部更新
- **NavMesh 局部 carve**：在运行时对受影响 tile 进行增量 re-bake

### 10.3 长期
- **动态 NavMesh 完整支持**：类似 Detour TileCache 的完整方案，支持任意形状障碍物的精确 carve + 重建
- **多层 NavMesh**：支持不同高度层的独立障碍物影响

---

*文档结束*
