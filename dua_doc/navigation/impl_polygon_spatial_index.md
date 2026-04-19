# 导航多边形空间索引（BVH）加速起终点查找 — 技术设计文档

> **状态**: 设计草案  
> **作者**: Auto-generated  
> **涉及模块**: `modules/navigation_3d`  
> **核心引擎依赖**: `core/math/dynamic_bvh.h`

---

## 1. 背景与动机

### 1.1 问题描述

Godot 导航系统在寻路时，需要将用户给定的 **起点/终点世界坐标** 映射到导航网格上最近的多边形及其最近点。当前实现采用 **暴力遍历**：对地图中所有 Region 的所有多边形逐一做 fan 三角化，再对每个三角面调用 `Face3::get_closest_point_to()` 求最近点。

在大型场景中（数千～数万多边形），每次寻路查询的起终点定位开销为 **O(N)**（N = 总多边形数 × 平均三角面数），成为性能瓶颈。

### 1.2 目标

引入 **BVH（Bounding Volume Hierarchy）** 空间索引，将起终点查找的平均复杂度从 O(N) 降低到 **O(log N)**，同时保持结果的正确性（找到完全相同的最近点）。

### 1.3 附带收益

同一 BVH 索引还将加速以下查询接口：
- `map_iteration_get_closest_point()` / `_info()` / `_normal()` / `_owner()`
- `map_iteration_get_closest_point_to_segment()`
- `map_iteration_get_random_point()`（如果未来需要基于空间过滤）

---

## 2. 现状分析

### 2.1 入口函数

```
文件: modules/navigation_3d/3d/nav_mesh_queries_3d.cpp
函数: NavMeshQueries3D::_query_task_find_start_end_positions()
行号: 237
```

### 2.2 当前暴力遍历流程

```
对于 map_iteration 中的每个 region:
    跳过不可用的 region（enabled / navigation_layers / include-exclude 过滤）
    对于 region 中的每个 polygon:
        跳过 navigation_layers 不匹配的 polygon
        对于 polygon 的每个 fan 三角面 (vertex[0], vertex[i-1], vertex[i]):
            face = Face3(v0, v_{i-1}, v_i)
            point = face.get_closest_point_to(start_position)
            if distance < begin_d → 更新 begin_polygon / begin_position
            point = face.get_closest_point_to(target_position)
            if distance < end_d → 更新 end_polygon / end_position
```

**关键观察**:
1. 同一个 polygon 被三角化为 `vertices.size() - 2` 个三角面
2. 对每个三角面同时做 start 和 target 两次最近点查询
3. Region 级别有可用性过滤，Polygon 级别有 navigation_layers 过滤
4. 没有任何空间剪枝——即使查询点距某个 polygon 非常远，也会计算最近点

### 2.3 相关查询函数的相同模式

`map_iteration_get_closest_point_info()` (行 961) 和 `map_iteration_get_closest_point_to_segment()` (行 880) 都采用完全相同的双重循环暴力遍历模式。

### 2.4 地图构建流程

```
NavMap3D::_build_iteration()
  → 准备 next iteration slot（ping-pong 双缓冲的另一个槽）
  → 收集 region / link iterations
  → NavMapBuilder3D::build_navmap_iteration()
      → _build_step_gather_region_polygons()
      → _build_step_find_edge_connection_pairs()
      → _build_step_merge_edge_connection_pairs()
      → _build_step_edge_connection_margin_connections()
      → _build_step_navlink_connections()
      → _build_update_map_iteration()    ← 最后阶段，更新 path_query_slots
  → NavMap3D::_sync_iteration()
      → 切换 iteration_slot_index（ping-pong swap）
```

### 2.5 双缓冲（Ping-Pong）架构

`NavMap3D` 持有两个 `NavMapIteration3D` 槽位（`iteration_slots[2]`），通过 `iteration_slot_index` 控制当前活跃槽。查询线程始终读取当前活跃槽，构建线程写入另一个槽。构建完成后通过 `_sync_iteration()` 原子切换索引。

**关键约束**: BVH 索引必须存放在 `NavMapIteration3D` 中，随 ping-pong 一起切换，确保查询线程读取的 BVH 与多边形数据一致。

### 2.6 Godot 引擎已有的 BVH 实现

| 类 | 文件 | 特点 |
|---|---|---|
| `DynamicBVH` | `core/math/dynamic_bvh.h/.cpp` | 基于 Bullet dbvt，支持动态增删、AABB/凸体/射线查询，模板化回调 |
| `BVH_Manager` / `BVH_Tree` | `core/math/bvh.h` / `bvh_tree.h` | 更复杂，支持 pairing 系统，主要用于物理/渲染 |

**推荐使用 `DynamicBVH`**：API 简洁（`insert`/`aabb_query`/`clear`），支持批量构建后 `optimize_top_down()`，且已在引擎多处使用，稳定可靠。

### 2.7 UE (Detour) 的 BVH 实现参考

UE 使用 Detour 库内置的 BVH，设计理念与 Godot 的 `DynamicBVH` 截然不同。作为参考，其方案有以下值得关注的特点：

#### 2.7.1 两级加速结构：Tile 定位 + Tile 内 BVH

Detour 的 `findNearestPoly()` 分两步：

```
输入点 (x, y, z)
  │
  ├─ Step 1: Tile 定位 — O(1)
  │    坐标除法 → 直接定位到所在 Tile（+ 相邻 Tile 扩展搜索）
  │    搜索范围从全地图几千多边形缩小到一个 Tile 的几十~几百个
  │
  └─ Step 2: Tile 内 BVH 查询 — O(log M)
       M = 单 Tile 内多边形数
       用量化 BVH 快速过滤出候选多边形
       只对候选做精确距离计算
```

**效果对比**（5000 多边形地图）：

| | Godot（暴力） | Detour（Tile + BVH） |
|---|---|---|
| 距离计算次数 | ~5000 | ~14（6 次 BVH 比较 + 8 次精确计算） |
| 加速比 | 1x | **~350x** |

#### 2.7.2 量化压缩的 BVH 节点

Detour 不存浮点包围盒，而是将坐标 **量化到 unsigned short（16 位整数）**：

```cpp
// Detour/Include/DetourNavMesh.h
struct dtBVNode {
    unsigned short bmin[3];  // 量化后的 AABB 最小角（6 字节）
    unsigned short bmax[3];  // 量化后的 AABB 最大角（6 字节）
    int i;                   // ≥ 0: 叶子节点（多边形索引）
                             // < 0: 内部节点（escape index）
};
// 总共 16 字节/节点，极度 cache-friendly
```

量化公式：
```
quantized = (int)((float_value - tile_bmin) * quantFactor)
quantFactor = 65535.0f / (tile_bmax - tile_bmin)
```

精度：65535 级，对于一个 Tile 内的局部坐标，误差小于 0.05cm。

#### 2.7.3 扁平数组 + Escape Index 存储

Detour 的 BVH **不使用指针**，而是存成一个连续的 `dtBVNode[]` 扁平数组，用 escape index 跳过子树：

```
数组索引:  [0]    [1]    [2]   [3]   [4]   [5]   ...
内容:      Root   Left   L-L   P0    P1    L-R   ...
node.i:    -15    -7     -3    0     1     -3    ...
           ↑内部  ↑内部  ↑内部 ↑叶子 ↑叶子 ↑内部

规则:
  node.i >= 0  → 叶子节点，i 是多边形索引
  node.i < 0   → 内部节点，跳过子树 = 当前位置 + (-node.i)
```

遍历代码：**无递归、无指针、无栈，纯线性扫描**：

```cpp
// Detour/Source/DetourNavMesh.cpp (简化)
void dtNavMesh::queryPolygonsInTile(const dtMeshTile* tile,
                                     const float* qmin, const float* qmax, ...) {
    const dtBVNode* node = &tile->bvTree[0];
    const dtBVNode* end = &tile->bvTree[tile->header->bvNodeCount];

    while (node < end) {
        bool overlap = dtOverlapQuantBounds(qmin, qmax, node->bmin, node->bmax);
        bool isLeaf = (node->i >= 0);

        if (isLeaf && overlap) {
            result[n++] = node->i;    // 候选多边形
        }

        if (overlap || isLeaf) {
            node++;                    // 进入子树 / 下一个兄弟
        } else {
            node += -node->i;         // 跳过整棵子树！
        }
    }
}
```

#### 2.7.4 构建算法：自顶向下 + 最长轴中点分割

```cpp
// 构建伪代码 (Detour/Source/DetourNavMesh.cpp)
void subdivide(dtBVNode* nodes, int* items, int imin, int imax, int& curNode) {
    dtBVNode& node = nodes[curNode++];

    if (imax - imin == 1) {
        // 叶子节点：直接用该多边形的量化包围盒
        node.bmin = items[imin].bmin;
        node.bmax = items[imin].bmax;
        node.i = items[imin].polyIndex;  // ≥ 0，表示叶子
        return;
    }

    // 1) 计算所有子项的总包围盒
    calcExtends(items, imin, imax, node.bmin, node.bmax);

    // 2) 找最长轴（X/Y/Z 中跨度最大的）
    int axis = longestAxis(bmax[0]-bmin[0], bmax[1]-bmin[1], bmax[2]-bmin[2]);

    // 3) 沿最长轴中点分割（类似 nth_element）
    int isplit = imin + (imax - imin) / 2;
    partitionItems(items, imin, imax, isplit, axis);

    // 4) 递归构建左右子树
    subdivide(nodes, items, imin, isplit, curNode);
    subdivide(nodes, items, isplit, imax, curNode);

    // 5) 回填 escape index
    node.i = -(curNode - icur);  // 负数，表示内部节点
}
```

构建复杂度：O(M log M)，其中 M = Tile 内多边形数。

#### 2.7.5 Detour 方案的核心优势

| 特点 | 效果 |
|------|------|
| 16 字节/节点，量化坐标 | 节点刚好 1/4 缓存行，遍历时 cache miss 极少 |
| 扁平数组 + 线性遍历 | 无指针追踪，CPU 预取友好 |
| Escape index 跳过子树 | 不重叠的内部节点直接跳过，减少分支 |
| 每 Tile 一棵 BVH | Tile 重建时只重建该 Tile 的 BVH，其他不受影响 |
| 构建后只读 | 运行时零维护成本 |

#### 2.7.6 对本方案的启示

Godot 的 `DynamicBVH`（基于指针树）对于动态增删很灵活，但对最近邻查询不如 Detour 的扁平方案高效。考虑到 Godot 导航的 BVH 在每次重建后是只读的，有两种路径：

- **路径 A（推荐，本文档采用）**：先用 `DynamicBVH` + `optimize_top_down()` 快速落地，验证正确性和收益
- **路径 B（进阶优化）**：如果后续 profiling 发现 `DynamicBVH` 的指针追踪开销显著，可参考 Detour 实现一个轻量级的量化扁平 BVH（16 字节/节点 + escape index），作为导航模块专用的只读 BVH

---

## 3. 方案设计

### 3.1 架构总览

```
                    ┌──────────────────────────────────────┐
                    │        NavMapBuilder3D                │
                    │  build_navmap_iteration()             │
                    │    ...existing steps...               │
                    │    _build_step_polygon_bvh() ← NEW   │
                    │    _build_update_map_iteration()      │
                    └──────────────────┬───────────────────┘
                                       │ writes to
                                       ▼
                    ┌──────────────────────────────────────┐
                    │      NavMapIteration3D                │
                    │  DynamicBVH polygon_bvh;  ← NEW      │
                    │  LocalVector<const Nav3D::Polygon*>   │
                    │      polygon_bvh_data;    ← NEW      │
                    └──────────────────┬───────────────────┘
                                       │ read by
                                       ▼
                    ┌──────────────────────────────────────┐
                    │      NavMeshQueries3D                 │
                    │  _query_task_find_start_end_positions │
                    │  map_iteration_get_closest_point_info │
                    │  map_iteration_get_closest_point_to_  │
                    │      segment                          │
                    │  → 使用 BVH 做空间裁剪后精确查找     │
                    └──────────────────────────────────────┘
```

### 3.2 核心设计决策

| 决策项 | 选择 | 理由 |
|--------|------|------|
| BVH 实现 | `DynamicBVH` | 引擎内置，API 简洁，支持 AABB 查询 |
| 索引粒度 | **每个 Nav3D::Polygon 一个叶节点** | 多边形是查询的最小过滤单元（有 navigation_layers），且数量适中 |
| 索引存放位置 | `NavMapIteration3D` | 随 ping-pong 一起切换，线程安全 |
| 构建时机 | `_build_update_map_iteration()` 中 | 所有多边形数据已准备好，构建完成后才 ping-pong 切换 |
| 查询策略 | 优先级搜索（best-first nearest） | 先用 AABB 距离排序 BVH 节点，逐步收紧最优距离 |

### 3.3 为什么不用 `DynamicBVH::aabb_query` 直接查询？

`DynamicBVH::aabb_query()` 是一个简单的 AABB 交叉测试——它返回所有与给定 AABB 相交的叶节点。对于"找最近点"查询，我们需要的是 **最近邻搜索**（nearest neighbor），这不能直接用 AABB 交叉完成。

**方案选择：自定义栈式 BVH 遍历 + 距离剪枝**

我们将直接遍历 `DynamicBVH` 的内部节点树（通过 `bvh_root` 和 `Node` 结构），实现一个 **best-first 最近邻搜索**：

1. 从根节点开始
2. 对每个内部节点，计算查询点到其 AABB 的最小距离
3. 如果该距离 ≥ 当前最优距离，剪枝
4. 否则递归进入子节点（优先进入更近的子节点）
5. 到达叶节点时，对 polygon 做精确最近点计算

> **注意**: `DynamicBVH` 的 `Node` 结构和 `bvh_root` 在 private 区域。我们需要通过两种方式之一来访问：
> - **方案 A（推荐）**: 在 `DynamicBVH` 中添加一个 `nearest_point_query` 模板方法（类似已有的 `aabb_query`）
> - **方案 B**: 使用友元类或在导航模块中实现轻量级 BVH（仅包含必要功能）
>
> 推荐 **方案 A**，因为最近邻搜索是通用需求，添加到 `DynamicBVH` 对引擎其他模块也有价值。

---

## 4. 数据结构设计

### 4.1 BVH 叶节点用户数据

每个 BVH 叶节点的 `void* data` 指向一个 `Nav3D::Polygon` 指针。由于 `Polygon` 已经包含 `owner`（可获取 navigation_layers、enabled 等信息）和 `vertices`，不需要额外的包装结构。

```cpp
// 叶节点 data 直接存储 Polygon 指针
// 在 insert 时: bvh.insert(polygon_aabb, (void*)&polygon);
// 在查询回调中: const Polygon* poly = static_cast<const Polygon*>(node->data);
```

### 4.2 NavMapIteration3D 新增成员

```cpp
// 文件: modules/navigation_3d/3d/nav_map_iteration_3d.h

struct NavMapIteration3D {
    // ... 已有成员 ...

    /// BVH 空间索引，用于加速多边形最近点查询。
    /// 每个叶节点的 data 指向一个 Nav3D::Polygon*。
    DynamicBVH polygon_bvh;
};
```

### 4.3 DynamicBVH 新增查询接口

```cpp
// 文件: core/math/dynamic_bvh.h

// 在 DynamicBVH 类的 public 区域添加:

/// 最近邻搜索。对 BVH 进行 best-first 遍历，
/// 对每个叶节点调用 r_result(data, node_aabb)，
/// 回调返回当前已知最短距离，用于剪枝。
/// QueryResult 签名: real_t operator()(void* p_data, const AABB& p_aabb)
///   - 返回值 < 0 表示立即终止搜索
///   - 返回值 >= 0 表示当前已知最短距离（用于后续剪枝）
template <typename QueryResult>
_FORCE_INLINE_ void nearest_point_query(const Vector3 &p_point, QueryResult &r_result);
```

### 4.4 计算 Polygon 的 AABB

```cpp
// 伪代码
AABB compute_polygon_aabb(const Nav3D::Polygon &p_polygon) {
    AABB aabb(p_polygon.vertices[0], Vector3());
    for (uint32_t i = 1; i < p_polygon.vertices.size(); i++) {
        aabb.expand_to(p_polygon.vertices[i]);
    }
    return aabb;
}
```

---

## 5. 构建流程

### 5.1 构建时机

在 `NavMapBuilder3D::build_navmap_iteration()` 中，在 `_build_update_map_iteration()` **之前**添加新的构建步骤：

```cpp
void NavMapBuilder3D::build_navmap_iteration(NavMapIterationBuild3D &r_build) {
    // ... existing steps ...
    _build_step_navlink_connections(r_build);

    _build_step_polygon_bvh(r_build);     // ← 新增

    _build_update_map_iteration(r_build);
}
```

### 5.2 构建伪代码

```cpp
void NavMapBuilder3D::_build_step_polygon_bvh(NavMapIterationBuild3D &r_build) {
    NavMapIteration3D *map_iteration = r_build.map_iteration;

    // 清空旧的 BVH（这个 iteration slot 可能残留上一轮数据）
    map_iteration->polygon_bvh.clear();

    // 遍历所有 region 的所有 polygon，插入 BVH
    for (Ref<NavRegionIteration3D> &region : map_iteration->region_iterations) {
        for (Nav3D::Polygon &polygon : region->navmesh_polygons) {
            if (polygon.vertices.size() < 3) {
                continue;
            }
            AABB aabb = compute_polygon_aabb(polygon);
            map_iteration->polygon_bvh.insert(aabb, (void *)&polygon);
        }
    }

    // 批量优化 BVH 结构（top-down 重建，获得更优的树质量）
    map_iteration->polygon_bvh.optimize_top_down();
}
```

### 5.3 清理

在 `NavMapIteration3D::clear()` 中添加 BVH 清理：

```cpp
void clear() {
    // ... 已有清理 ...
    polygon_bvh.clear();   // ← 新增
}
```

---

## 6. 查询流程与伪代码

### 6.1 AABB 到点的最小距离

这是 BVH 剪枝的核心辅助函数：

```cpp
// 计算点到 AABB 的最小距离的平方
_FORCE_INLINE_ real_t aabb_min_distance_squared(const AABB &p_aabb, const Vector3 &p_point) {
    real_t dist_sq = 0.0;
    for (int i = 0; i < 3; i++) {
        real_t v = p_point[i];
        real_t lo = p_aabb.position[i];
        real_t hi = lo + p_aabb.size[i];
        if (v < lo) {
            dist_sq += (lo - v) * (lo - v);
        } else if (v > hi) {
            dist_sq += (v - hi) * (v - hi);
        }
    }
    return dist_sq;
}
```

### 6.2 DynamicBVH::nearest_point_query 实现

```cpp
template <typename QueryResult>
void DynamicBVH::nearest_point_query(const Vector3 &p_point, QueryResult &r_result) {
    if (!bvh_root) {
        return;
    }

    const Node **alloca_stack = (const Node **)alloca(ALLOCA_STACK_SIZE * sizeof(const Node *));
    const Node **stack = alloca_stack;
    stack[0] = bvh_root;
    int32_t depth = 1;
    int32_t threshold = ALLOCA_STACK_SIZE - 2;
    LocalVector<const Node *> aux_stack;

    while (depth > 0) {
        depth--;
        const Node *n = stack[depth];

        // 计算查询点到当前节点 AABB 的最小距离
        AABB node_aabb;
        node_aabb.position = n->volume.min;
        node_aabb.size = n->volume.max - n->volume.min;

        // 回调返回当前最优距离平方，用于判断是否剪枝
        // 如果节点 AABB 最小距离已超过当前最优，跳过
        if (!r_result.should_visit(node_aabb)) {
            continue;
        }

        if (n->is_internal()) {
            if (depth > threshold) {
                if (aux_stack.is_empty()) {
                    aux_stack.resize(ALLOCA_STACK_SIZE * 2);
                    memcpy(aux_stack.ptr(), alloca_stack, ALLOCA_STACK_SIZE * sizeof(const Node *));
                    alloca_stack = nullptr;
                } else {
                    aux_stack.resize(aux_stack.size() * 2);
                }
                stack = aux_stack.ptr();
                threshold = aux_stack.size() - 2;
            }
            // 优先遍历离查询点更近的子节点
            stack[depth++] = n->children[0];
            stack[depth++] = n->children[1];
        } else {
            // 叶节点：精确计算
            if (r_result(n->data)) {
                return;  // 回调要求提前终止
            }
        }
    }
}
```

### 6.3 改造 `_query_task_find_start_end_positions`

```cpp
void NavMeshQueries3D::_query_task_find_start_end_positions(
        NavMeshPathQueryTask3D &p_query_task,
        const NavMapIteration3D &p_map_iteration) {

    real_t begin_d_sq = FLT_MAX;  // 使用距离平方避免 sqrt
    real_t end_d_sq = FLT_MAX;

    const uint32_t nav_layers = p_query_task.navigation_layers;
    const Vector3 &start_pos = p_query_task.start_position;
    const Vector3 &target_pos = p_query_task.target_position;

    // 定义 BVH 查询回调（lambda 或 functor）
    struct FindStartEndCallback {
        const Vector3 &start_pos;
        const Vector3 &target_pos;
        uint32_t nav_layers;
        const NavMeshPathQueryTask3D &query_task;

        real_t best_start_dist_sq = FLT_MAX;
        real_t best_end_dist_sq = FLT_MAX;
        const Nav3D::Polygon *begin_polygon = nullptr;
        const Nav3D::Polygon *end_polygon = nullptr;
        Vector3 begin_position;
        Vector3 end_position;

        // 返回 true 表示节点 AABB 值得继续搜索
        bool should_visit(const AABB &p_aabb) const {
            // 两个查询点共享一个 BVH 遍历：
            // 只要任一查询点到 AABB 的距离小于其当前最优，就需要访问
            real_t d_start = aabb_min_distance_squared(p_aabb, start_pos);
            real_t d_end = aabb_min_distance_squared(p_aabb, target_pos);
            return (d_start < best_start_dist_sq || d_end < best_end_dist_sq);
        }

        // 叶节点回调：精确计算最近点
        bool operator()(void *p_data) {
            const Nav3D::Polygon *p = static_cast<const Nav3D::Polygon *>(p_data);

            // navigation_layers 过滤
            if ((nav_layers & p->owner->get_navigation_layers()) == 0) {
                return false;
            }

            // owner 可用性过滤（enabled, include/exclude）
            // 注意：region 级别的过滤在这里通过 polygon->owner 检查
            if (!p->owner->get_enabled()) {
                return false;
            }

            // Fan 三角化并求最近点（与原始逻辑一致）
            for (uint32_t point_id = 2; point_id < p->vertices.size(); point_id++) {
                const Face3 face(p->vertices[0], p->vertices[point_id - 1], p->vertices[point_id]);

                Vector3 point = face.get_closest_point_to(start_pos);
                real_t dist_sq = point.distance_squared_to(start_pos);
                if (dist_sq < best_start_dist_sq) {
                    best_start_dist_sq = dist_sq;
                    begin_polygon = p;
                    begin_position = point;
                }

                point = face.get_closest_point_to(target_pos);
                dist_sq = point.distance_squared_to(target_pos);
                if (dist_sq < best_end_dist_sq) {
                    best_end_dist_sq = dist_sq;
                    end_polygon = p;
                    end_position = point;
                }
            }
            return false;  // 继续搜索
        }
    };

    FindStartEndCallback callback{
        start_pos, target_pos, nav_layers, p_query_task
    };

    p_map_iteration.polygon_bvh.nearest_point_query(
        /* 需要同时处理两个点，使用 callback 联合剪枝 */
        start_pos,  // 用 start_pos 作为主遍历锚点
        callback
    );

    p_query_task.begin_polygon = callback.begin_polygon;
    p_query_task.begin_position = callback.begin_position;
    p_query_task.end_polygon = callback.end_polygon;
    p_query_task.end_position = callback.end_position;
}
```

### 6.4 双点联合查询优化

一个关键的设计点：起点和终点共享同一次 BVH 遍历。`should_visit` 中使用两个点到 AABB 的距离的 **最小值** 作为剪枝条件——只要任一查询点还可能从该节点中找到更优解，就继续遍历。这避免了两次独立的 BVH 遍历。

**替代方案**：如果起点和终点距离很远，联合查询可能导致剪枝效率降低。可以考虑分别做两次独立的 BVH 查询。在实现时可通过简单 benchmark 决定阈值。

---

## 7. 受益的其他查询接口

### 7.1 `map_iteration_get_closest_point_info()`

**当前实现** (行 961-1033): 暴力遍历所有 polygon，对每个 polygon 做边-点投影求最近点。

**改造思路**: 使用与 6.3 类似的 BVH nearest-point 查询，回调中执行原有的边-点投影逻辑。

```cpp
ClosestPointQueryResult NavMeshQueries3D::map_iteration_get_closest_point_info(
        const NavMapIteration3D &p_map_iteration, const Vector3 &p_point) {

    struct ClosestPointCallback {
        const Vector3 &query_point;
        real_t best_dist_sq = FLT_MAX;
        ClosestPointQueryResult result;

        bool should_visit(const AABB &p_aabb) const {
            return aabb_min_distance_squared(p_aabb, query_point) < best_dist_sq;
        }

        bool operator()(void *p_data) {
            const Polygon *polygon = static_cast<const Polygon *>(p_data);
            // ... 原有的边-点投影逻辑 ...
            // 更新 best_dist_sq 和 result
            return false;
        }
    };

    ClosestPointCallback callback{p_point};
    p_map_iteration.polygon_bvh.nearest_point_query(p_point, callback);
    return callback.result;
}
```

### 7.2 `map_iteration_get_closest_point_to_segment()`

**当前实现** (行 880-943): 暴力遍历所有 polygon，对每个三角面做线段交叉检测和最近点计算。

**改造思路**: 使用线段的 AABB 做初步 BVH 查询（`aabb_query`），获取候选 polygon 列表，再对候选做精确计算。或者实现一个 segment 版本的 nearest-point BVH 遍历。

```cpp
// 简单方案：先用线段 AABB 扩展 best_distance 做 aabb_query，再精确过滤
// 更优方案：自定义 BVH 遍历，计算线段到 AABB 的最小距离做剪枝
```

---

## 8. 文件修改清单

### 8.1 核心修改

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `core/math/dynamic_bvh.h` | **新增方法** | 添加 `nearest_point_query()` 模板方法 |
| `modules/navigation_3d/3d/nav_map_iteration_3d.h` | **新增成员** | `NavMapIteration3D` 添加 `DynamicBVH polygon_bvh` |
| `modules/navigation_3d/3d/nav_map_iteration_3d.h` | **修改** | `clear()` 中添加 `polygon_bvh.clear()` |
| `modules/navigation_3d/3d/nav_map_builder_3d.h` | **新增声明** | 添加 `_build_step_polygon_bvh()` 静态方法声明 |
| `modules/navigation_3d/3d/nav_map_builder_3d.cpp` | **新增实现** | 实现 `_build_step_polygon_bvh()`，在 `build_navmap_iteration()` 中调用 |
| `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | **重写** | `_query_task_find_start_end_positions()` 使用 BVH 查询 |
| `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | **重写** | `map_iteration_get_closest_point_info()` 使用 BVH 查询 |
| `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | **重写** | `map_iteration_get_closest_point_to_segment()` 使用 BVH 查询 |

### 8.2 辅助修改

| 文件 | 修改类型 | 说明 |
|------|----------|------|
| `modules/navigation_3d/3d/nav_mesh_queries_3d.h` | **新增** | 添加 `aabb_min_distance_squared()` 辅助函数声明（或放在 nav_utils_3d.h） |
| `modules/navigation_3d/nav_utils_3d.h` | **可选** | 添加 `compute_polygon_aabb()` 辅助函数 |

### 8.3 头文件依赖新增

```cpp
// nav_map_iteration_3d.h 需要新增:
#include "core/math/dynamic_bvh.h"
```

---

## 9. 性能分析

### 9.1 理论复杂度对比

| | 暴力遍历 | BVH 加速 |
|---|---|---|
| **构建** | 无 | O(N log N)（每次地图同步时构建） |
| **单次查询** | O(N × T) | O(log N × T)（平均），O(N × T)（最坏） |
| **内存** | 0 | O(N)（BVH 节点，约 2N 个内部节点） |

其中 N = 多边形总数，T = 每个多边形的平均三角面数（通常 1-5）。

### 9.2 实际场景估算

| 场景规模 | 多边形数 | 暴力遍历（估算） | BVH 查询（估算） | 加速比 |
|----------|---------|-----------------|-----------------|--------|
| 小型 | 100 | ~100 次精确计算 | ~10-20 次精确计算 | 5-10x |
| 中型 | 1,000 | ~1,000 次 | ~20-40 次 | 25-50x |
| 大型 | 10,000 | ~10,000 次 | ~30-60 次 | 160-330x |
| 超大型 | 100,000 | ~100,000 次 | ~40-80 次 | 1,250-2,500x |

### 9.3 构建开销

BVH 构建是 O(N log N)，对于 10,000 个多边形，典型耗时在 **亚毫秒级**。由于构建在后台线程完成（`_build_iteration_threaded`），不会阻塞主线程。

### 9.4 内存开销

`DynamicBVH` 每个节点约 40-64 字节（Volume + 指针）。对于 N 个叶节点，总节点数约 2N-1，因此：
- 1,000 个多边形：~128 KB
- 10,000 个多边形：~1.2 MB
- 100,000 个多边形：~12 MB

由于有两个 ping-pong 槽位，内存占用翻倍。这个开销在大多数场景下可接受。

---

## 10. 工作量评估

### 10.1 任务分解

| 任务 | 预估工时 | 优先级 | 说明 |
|------|---------|--------|------|
| T1: DynamicBVH 添加 nearest_point_query | 4h | P0 | 模仿 aabb_query 模板，添加距离剪枝 |
| T2: NavMapIteration3D 添加 BVH 成员 | 1h | P0 | 数据结构修改 + clear |
| T3: NavMapBuilder3D 构建 BVH | 3h | P0 | 实现 _build_step_polygon_bvh |
| T4: 改造 _query_task_find_start_end_positions | 6h | P0 | 核心查询重写 + 回调实现 |
| T5: 改造 map_iteration_get_closest_point_info | 4h | P1 | 类似 T4 的模式 |
| T6: 改造 map_iteration_get_closest_point_to_segment | 5h | P1 | 线段查询更复杂 |
| T7: 单元测试 | 4h | P0 | 对比新旧实现结果一致性 |
| T8: 性能测试 | 3h | P1 | 大规模场景 benchmark |
| T9: 代码审查 & 文档 | 2h | P1 | |
| **总计** | **~32h (4 人天)** | | |

### 10.2 里程碑建议

| 里程碑 | 内容 | 预计完成 |
|--------|------|---------|
| M1 | T1-T4: 核心功能（起终点查找 BVH 加速）| 第 1 周 |
| M2 | T5-T6: 扩展到其他查询接口 | 第 2 周 |
| M3 | T7-T9: 测试、优化、收尾 | 第 2 周末 |

---

## 11. 风险分析与缓解措施

### 11.1 正确性风险

| 风险 | 严重性 | 概率 | 缓解 |
|------|--------|------|------|
| BVH 查询遗漏最优多边形（距离剪枝有 bug） | **高** | 中 | 编写对照测试：对同一输入，暴力遍历和 BVH 结果必须完全一致 |
| Polygon 指针在 BVH 中失效 | **高** | 低 | Polygon 存储在 NavRegionIteration3D 的 LocalVector 中，lifetime 由 Ref 保证；只要 map_iteration 存活，指针有效 |
| 浮点精度导致 AABB 不包含 polygon 所有顶点 | 中 | 低 | AABB 计算使用精确的 min/max，无精度损失 |
| navigation_layers 过滤在 BVH 层面无法做 | 低 | 确定 | 设计上 BVH 只做空间剪枝，层过滤在叶节点回调中完成（与当前行为一致） |

### 11.2 性能风险

| 风险 | 严重性 | 概率 | 缓解 |
|------|--------|------|------|
| BVH 构建耗时过长影响地图同步 | 中 | 低 | 构建在后台线程，且 O(N log N) 对于 10K 多边形 < 1ms |
| BVH 树质量差导致查询退化 | 中 | 低 | 使用 `optimize_top_down()` 批量重建，保证树质量 |
| 联合双点查询剪枝效率差（起终点距离远） | 低 | 中 | 提供回退方案：距离超过阈值时改为两次独立查询 |
| DynamicBVH PagedAllocator 的内存碎片 | 低 | 低 | 每次构建时 clear + 重新 insert，不会持续碎片化 |

### 11.3 架构/兼容性风险

| 风险 | 严重性 | 概率 | 缓解 |
|------|--------|------|------|
| 修改 `DynamicBVH` 公共接口影响其他模块 | 中 | 低 | 仅添加新方法，不修改已有接口 |
| 2D 导航模块需要相同改造 | 低 | 确定 | 2D 导航系统架构类似但独立，可后续单独处理 |
| 与 Region include/exclude 过滤的交互 | 中 | 低 | BVH 不感知 region 过滤，过滤在叶节点回调中完成 |

### 11.4 线程安全风险

| 风险 | 严重性 | 概率 | 缓解 |
|------|--------|------|------|
| 查询线程读 BVH 时构建线程在写 | **高** | 无 | BVH 存放在 NavMapIteration3D 中，ping-pong 架构保证读写分离 |
| DynamicBVH 本身非线程安全（多线程并发只读查询） | 中 | 低 | 查询是只读操作（遍历树），DynamicBVH 无全局可变状态，多线程只读安全 |

---

## 12. 测试策略

### 12.1 正确性测试

1. **对照测试**: 对于随机生成的 NavMesh 和随机查询点，确保 BVH 加速后的结果与暴力遍历完全一致
   - 比较 `begin_polygon` / `end_polygon` 指针
   - 比较 `begin_position` / `end_position` 向量（允许浮点误差 ε = 1e-6）

2. **边界测试**:
   - 空地图（无 polygon）
   - 单个 polygon
   - 查询点恰好在 polygon 上
   - 查询点在 AABB 边界上
   - 所有 polygon 被 navigation_layers 过滤掉

3. **回归测试**:
   - 确保所有现有导航系统测试通过
   - 确保 `map_iteration_get_closest_point*` 系列函数结果不变

### 12.2 性能测试

1. **Benchmark**: 在不同规模的 NavMesh 上（100, 1K, 10K, 100K 多边形）对比查询耗时
2. **构建耗时**: 测量 `_build_step_polygon_bvh()` 在不同规模下的耗时
3. **内存**: 测量 BVH 的内存占用

---

## 13. 附录：关键源码参考

### 13.1 当前暴力查询 — `_query_task_find_start_end_positions()`

```
文件: modules/navigation_3d/3d/nav_mesh_queries_3d.cpp
行号: 237-277
```

### 13.2 DynamicBVH 接口

```
文件: core/math/dynamic_bvh.h
关键方法:
  - insert(const AABB&, void*) → ID           (行 293)
  - clear()                                    (行 288)
  - optimize_top_down(int bu_threshold = 128)  (行 291)
  - aabb_query(const AABB&, QueryResult&)      (行 308, 321)
  - ray_query(const Vector3&, const Vector3&, QueryResult&) (行 312)
```

### 13.3 DynamicBVH::Node 结构

```
文件: core/math/dynamic_bvh.h
行号: 180-220
关键字段:
  - Volume volume (min, max)
  - Node* parent
  - Node* children[2] / void* data (叶节点)
  - is_leaf() / is_internal()
```

### 13.4 NavMapIteration3D 结构

```
文件: modules/navigation_3d/3d/nav_map_iteration_3d.h
行号: 73-107
关键字段:
  - region_iterations: 所有 region 的快照
  - navlink_polygons: link 多边形
  - path_query_slots: A* 查询槽位
```

### 13.5 Nav3D::Polygon 结构

```
文件: modules/navigation_3d/nav_utils_3d.h
行号: 98-107
关键字段:
  - id: 在 region 中的局部 ID
  - owner: NavBaseIteration3D* (可获取 navigation_layers, enabled 等)
  - vertices: LocalVector<Vector3>
  - surface_area: real_t
```

### 13.6 构建入口

```
文件: modules/navigation_3d/3d/nav_map_builder_3d.cpp
函数: NavMapBuilder3D::build_navmap_iteration()
行号: 54-74
```

### 13.7 Ping-Pong 切换

```
文件: modules/navigation_3d/nav_map_3d.cpp
函数: NavMap3D::_build_iteration()    (行 338-403)
函数: NavMap3D::_sync_iteration()     (行 411-428)
```

