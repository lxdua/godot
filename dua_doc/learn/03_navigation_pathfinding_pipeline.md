# Godot 导航寻路管线完整解析

> 本文详细拆解 Godot 4.x 导航系统从用户调用 `map_get_path()` 到最终返回路径点数组的完整流程，涵盖 7 个阶段。

---

## 一、总体架构

一次寻路请求的完整调用链：

```
用户 API 层
  NavigationServer3D.map_get_path()
  NavigationServer3D.query_path()
      ↓
实现层
  GodotNavigationServer3D::map_get_path()    // 包装旧接口
  GodotNavigationServer3D::query_path()      // 新接口
      ↓
查询桥接层
  NavMeshQueries3D::map_query_path()         // 参数打包
      ↓
地图访问层
  NavMap3D::query_path()                     // 分配 PathQuerySlot
      ↓
核心寻路管线（7 个阶段）
  NavMeshQueries3D::query_task_map_iteration_get_path()
```

### 管线 7 阶段一览

```
Phase 1  _query_task_find_start_end_positions     找起终点最近多边形
Phase 2  平凡情况检查                               null → 空路径; 同多边形 → 直连
Phase 3  _query_task_build_path_corridor          A* 搜索，构建 path corridor
Phase 4  后处理（三选一）                            CORRIDORFUNNEL / EDGECENTERED / NONE
Phase 5  path_reverse                             反转（路径是反向构建的）
Phase 6  _query_task_simplified_path_points       可选 Ramer-Douglas-Peucker 简化
Phase 7  _query_task_process_path_result_limits   可选裁剪（max_length / max_radius）
```

---

## 二、入口层

### 2.1 `map_get_path` — 简洁旧接口

```cpp
Vector<Vector3> map_get_path(RID p_map, Vector3 p_origin, Vector3 p_destination,
                             bool p_optimize, uint32_t p_navigation_layers)
```

- `p_optimize = true` → 使用 `CORRIDORFUNNEL` 后处理（平滑路径）
- `p_optimize = false` → 使用 `EDGECENTERED` 后处理（边中点路径）
- 内部创建临时 `NavigationPathQueryParameters3D`，调用 `query_path()`

### 2.2 `query_path` — 完整新接口

```cpp
void query_path(const Ref<NavigationPathQueryParameters3D> &p_query_parameters,
                Ref<NavigationPathQueryResult3D> p_query_result,
                const Callable &p_callback)
```

支持所有高级功能：区域过滤、搜索限制、路径简化、异步回调等。

### 2.3 `map_query_path` — 参数打包

将 `NavigationPathQueryParameters3D`（Godot 对象）的所有字段拷贝到轻量的 `NavMeshPathQueryTask3D` 结构体中：

| 参数类别 | 字段 |
|---------|------|
| 基础 | `start_position`, `target_position`, `navigation_layers` |
| 区域过滤 | `excluded_regions`, `included_regions` |
| 算法选择 | `pathfinding_algorithm`（当前仅 ASTAR） |
| 后处理 | `path_postprocessing`（CORRIDORFUNNEL / EDGECENTERED / NONE） |
| 搜索限制 | `path_search_max_polygons`, `path_search_max_distance` |
| 路径限制 | `path_return_max_length`, `path_return_max_radius` |
| 简化 | `simplify_path`, `simplify_epsilon` |
| 元数据 | `metadata_flags`（控制收集 types / rids / owners） |

### 2.4 `NavMap3D::query_path` — Slot 分配

```
1. semaphore.wait()           // 等待可用 slot（限制并发查询数）
2. 遍历 slots 找到 in_use == false 的
3. 标记 in_use = true，挂到 query_task.path_query_slot
4. 执行 query_task_map_iteration_get_path()
5. 释放 slot，semaphore.post()
```

**PathQuerySlot 的设计目的**：预分配所有 A* 搜索需要的内存，运行时零动态分配。

---

## 三、核心数据结构

### 3.1 NavMeshPathQueryTask3D

A* 搜索的"工单"，包含输入参数、中间状态和输出结果：

```cpp
struct NavMeshPathQueryTask3D {
    // 输入
    Vector3 start_position, target_position;
    uint32_t navigation_layers;
    // ... 过滤、限制、简化等参数 ...

    // 中间状态（Phase 1 填充）
    const Polygon *begin_polygon, *end_polygon;  // 起终点所在多边形
    Vector3 begin_position, end_position;         // 投影到 NavMesh 上的实际起终点
    uint32_t least_cost_id;                       // A* 终点在 corridor 中的 ID

    // 中间状态（管线共享）
    PathQuerySlot *path_query_slot;               // 当前使用的查询槽
    Vector3 map_up;                               // 地图"上"方向

    // 输出
    LocalVector<Vector3> path_points;             // 最终路径点
    LocalVector<int32_t> path_meta_point_types;   // 每个点的类型
    LocalVector<RID> path_meta_point_rids;         // 每个点所属 RID
    LocalVector<int64_t> path_meta_point_owners;   // 每个点所属节点
    float path_length;
    TaskStatus status;
};
```

### 3.2 PathQuerySlot

```cpp
struct PathQuerySlot {
    LocalVector<NavigationPoly> path_corridor;  // 大小 = 地图总多边形数（预分配）
    Heap<...> traversable_polys;                // A* open list（最小堆）
    AHashMap<const Polygon*, uint32_t> poly_to_id; // Polygon* → corridor 索引
    bool in_use;
};
```

- `path_corridor` 的大小 = 所有 region 的多边形数 + navlink 的多边形数
- 每个 Polygon 在 corridor 中有固定位置，通过 `poly_to_id` 查找
- 这是**"数组直接索引"替代传统 open/closed list** 的设计——空间换时间

### 3.3 NavigationPoly — A* 搜索的工作节点

```cpp
struct NavigationPoly {
    const Polygon *poly;                        // 指向实际 Polygon 数据

    // 回溯链（从终点反向追踪到起点）
    int back_navigation_poly_id;                // 父节点在 corridor 中的 ID（-1 = 无）
    int back_navigation_edge;                   // 通过哪条边进入
    Vector3 back_navigation_edge_pathway_start; // 穿越边（portal）的起点
    Vector3 back_navigation_edge_pathway_end;   // 穿越边（portal）的终点

    Vector3 entry;                              // 进入此多边形的点
    real_t traveled_distance;                   // g 值：起点到此的累计代价
    real_t distance_to_destination;             // h 值：到终点的启发距离

    real_t total_travel_cost() { return g + h; } // f = g + h

    uint32_t traversable_poly_index;            // 在 Heap 中的位置（用于 decrease-key）
};
```

### 3.4 Connection — 多边形间的连接

```cpp
struct Connection {
    Polygon *polygon;       // 此连接通向的多边形
    int edge;               // 源多边形上的边索引
    Vector3 pathway_start;  // 通道（portal）起点
    Vector3 pathway_end;    // 通道（portal）终点
};
```

### 3.5 数据结构关系图

```
NavMapIteration3D（只读快照，查询线程使用）
  │
  ├── polygon_bvh (DynamicBVH)              ← Phase 1 起终点查找
  ├── polygon_bvh_data (Polygon*[])         ← BVH 叶节点 → Polygon*
  │
  ├── region_iterations[]
  │     └── NavRegionIteration3D
  │           ├── navmesh_polygons[]         ← Polygon 数据（顶点、owner、id）
  │           ├── internal_connections[][]   ← [poly_id] → Connection[]
  │           ├── travel_cost / enter_cost
  │           └── enabled / navigation_layers
  │
  ├── navlink_polygons[]                    ← Link 的两个虚拟多边形
  │
  ├── navbases_polygons_external_connections
  │     └── [NavBase*][poly_id] → Connection[]  ← 跨区域连接
  │
  └── path_query_slots[]                    ← 预分配的查询槽池
        ├── path_corridor[total_poly_count] ← NavigationPoly 数组
        ├── traversable_polys (Heap)        ← A* open list
        └── poly_to_id (HashMap)            ← Polygon* → corridor 索引
```

---

## 四、Phase 1：起终点查找

```cpp
void _query_task_find_start_end_positions(task, map_iteration) {
    task.begin_polygon = _find_closest_polygon_to_point(..., task.start_position, task.begin_position);
    task.end_polygon   = _find_closest_polygon_to_point(..., task.target_position, task.end_position);
}
```

使用 BVH 渐进式搜索（详见 `impl_polygon_spatial_index.md`），核心流程：

```
1. search_radius = 1.0, closest_d = FLT_MAX
2. 循环：
   a. 构建 AABB(point ± radius)
   b. BVH aabb_query → 回调检查每个候选多边形
      - 过滤：enabled / navigation_layers / exclude/include regions
      - 拆为三角形扇，Face3::get_closest_point_to() 计算距离
      - 维护全局最近
   c. 找到候选 → 精炼（用 closest_d + ε 再查一次）→ break
   d. 未找到 → radius *= 4.0（最大 10000）
3. 输出：begin/end_polygon（指针）+ begin/end_position（投影点）
```

---

## 五、Phase 2：平凡情况检查

```cpp
if (begin_polygon == nullptr || end_polygon == nullptr) {
    // 起点或终点不在任何 NavMesh 上 → 空路径
    return;
}

if (begin_polygon == end_polygon) {
    // 起终点在同一个多边形 → 直接返回 [begin_position, end_position]
    // 跳过 A*、后处理等所有后续阶段
    return;
}
```

---

## 六、Phase 3：A* 搜索

### 6.1 初始化

```
1. traversable_polys.clear()
2. 所有 NavigationPoly 的 traveled_distance = FLT_MAX
3. 起点多边形：
   - poly = begin_poly
   - entry = begin_point
   - traveled_distance = 0
4. least_cost_id = poly_to_id[begin_poly]
5. found_route = false
6. reachable_end = nullptr（追踪最远可达多边形）
```

### 6.2 主循环

```
while (true):
    least_cost_poly = navigation_polys[least_cost_id]

    ─── 展开邻居 ───

    (A) 内部连接：同一个 region/link 内的多边形间连接
        internal_connections[local_polygon_id] → Connection[]
        对每个 Connection 调用松弛操作

    (B) 外部连接：跨 region 的边合并连接 + navlink 连接
        external_connections[navbase][local_polygon_id] → Connection[]
        对每个 Connection 调用松弛操作

    ─── 搜索限制检查 ───

    if processed_polygon_count >= path_search_max_polygons → 提前终止
    if begin_point 到 entry 的距离 > path_search_max_distance → 提前终止

    ─── 取下一个节点 ───

    if traversable_polys.is_empty():
        // 堆空了但没找到终点 → 不可达，进入 fallback
        ...
    else:
        least_cost_id = poly_to_id[traversable_polys.pop()->poly]

        // 追踪最远可达点
        if distance_to_target < distance_to_reachable_end:
            reachable_end = 当前多边形

        // 到达终点？
        if navigation_polys[least_cost_id].poly == end_poly:
            found_route = true; break;

        // 跨 owner 时的进入代价
        if 新 poly 的 owner != 当前 owner:
            poly_enter_cost = 当前 owner 的 enter_cost
```

### 6.3 松弛操作（expand neighbor）

```cpp
_query_task_search_polygon_connections(task, connection, least_cost_id, least_cost_poly, enter_cost, end_point)
{
    // 1. 检查目标 owner 是否可用
    if (!_query_task_is_connection_owner_usable(owner)) return;

    // 2. 计算 new_entry：当前 entry → 连接通道(portal) 的最近点
    new_entry = closest_point_to_segment(entry, pathway_start, pathway_end);

    // 3. 计算新 g 值
    new_g = (entry → new_entry 距离) × travel_cost
          + enter_cost                          // 跨 owner 时的固定代价
          + old_g;

    // 4. 计算新 h 值
    new_h = (new_entry → end_point 距离) × target_owner.travel_cost;

    // 5. 松弛（如果新距离更短）
    neighbor = navigation_polys[poly_to_id[connection.polygon]];
    if (new_g < neighbor.traveled_distance) {
        更新 neighbor 的所有字段（父节点、穿越边、entry、g、h）

        if neighbor 已在堆中:
            traversable_polys.shift(index);  // decrease-key，O(log n)
        else:
            traversable_polys.push(&neighbor);
    }
}
```

**代价模型解析：**

| 参数 | 含义 | 默认值 | 影响 |
|------|------|--------|------|
| `travel_cost` | 区域的行走代价倍率 | 1.0 | g 和 h 都乘以此值，使某些区域"更贵" |
| `enter_cost` | 进入新区域的固定代价 | 0.0 | 跨 owner 时一次性加到 g 上 |

例如：沼泽区域 `travel_cost = 3.0`，路径会倾向绕过沼泽，即使距离更远。

### 6.4 内部连接 vs 外部连接

| 类型 | 存储位置 | 来源 | 说明 |
|------|---------|------|------|
| **内部连接** | `NavBaseIteration3D::internal_connections` | Region 解析阶段 | 同一个 region 内多边形间的共享边连接 |
| **外部连接** | `NavMapIteration3D::navbases_polygons_external_connections` | 地图构建阶段 | 跨 region 的边合并 + NavLink 连接 |

**边合并（Edge Merge）**：不同 region 的自由边如果足够接近（在 `edge_connection_margin` 内），会被自动合并为外部连接。

### 6.5 不可达时的 Fallback

当 `traversable_polys` 变空但未找到终点：

```
1. 标记 is_reachable = false
2. 如果 reachable_end == nullptr → 完全无路径，结束
3. 否则：
   a. 将终点重定向为 reachable_end（离 target 最近的已展开多边形）
   b. 在 reachable_end 和 begin_poly 上找距 target 最近的点
   c. 如果最近点在起始多边形上 → 返回 [begin_point, 最近点]
   d. 否则 → 重置所有 NavigationPoly，以 reachable_end 为终点重新跑 A*
```

---

## 七、Phase 4：路径后处理

### 7.1 CORRIDORFUNNEL — Funnel 算法

经典的 **Simple Stupid Funnel Algorithm**，从终点反向遍历 corridor 到起点，在穿越边（portal）上收紧"漏斗"。

**核心概念：**

```
        apex（漏斗顶点）
       / \
      /   \
     /     \
    /       \
   L ─────── R
  left      right
  portal    portal
```

- `apex_point` — 当前最后确认的路径拐点
- `left_portal` / `right_portal` — 漏斗的左右边界
- 每次迭代尝试用新的 portal 边收紧漏斗
- 如果新的 left 越过了 right（或反之），说明需要"转弯"——apex 移到被越过的那一侧

**方向判断**：

```cpp
#define THREE_POINTS_CROSS_PRODUCT(A, B, C) ((C - A).cross(B - A))
```

`cross_product.dot(map_up)` 的正负决定 B→C 相对于 A 是左转还是右转。

**算法流程：**

```
1. apex = end_point，push end_point 到路径
2. left_portal = right_portal = end_point
3. 反向遍历 corridor (p = 终点poly → 起点poly)：
   a. 取 p 的 back_edge 的 left/right 端点
   b. 确保 left 在左、right 在右（用 cross product 检查，必要时 swap）

   c. 收紧左边界：
      if (apex → left_portal → new_left) 没有左转:
          if left_portal == apex 或 (apex → new_left → right_portal) 是左转:
              // 收紧成功
              left_portal = new_left
          else:
              // new_left 越过了 right_portal → 转弯！
              新的 apex = right_portal
              clip_path()          // 裁剪中间的 portal 交点
              push apex 到路径
              重置漏斗

   d. 收紧右边界（对称逻辑）

4. 如果最后一个点 != begin_point → push begin_point
```

**注意**：路径是**反向构建**的（end → begin），Phase 5 会反转。

### 7.2 `_query_task_clip_path` — 路径裁剪

当漏斗发生"转弯"时，中间可能穿越了多个 portal，需要在这些 portal 上计算交点：

```
1. from = 上一个路径点
2. 构建切割平面：normal = (from - to_point) × map_up
3. 从 from_poly 反向遍历到 to_poly：
   对每个中间 poly 的 portal：
     如果切割平面与 portal 线段相交 → push 交点到路径
```

### 7.3 EDGECENTERED — 边中点模式

最简单的后处理——每个穿越边取中点：

```
1. push end_point
2. 反向遍历 corridor：
   对每个 poly：
     push (pathway_start + pathway_end) / 2  // 穿越边中点
3. push begin_point
```

路径是"锯齿形"的，但实现简单、性能好。

### 7.4 NONE — 无后处理

直接使用 A* 搜索时记录的 entry 点：

```
1. push end_point
2. 反向遍历 corridor：push 每个 poly 的 entry
3. push begin_point
```

---

## 八、Phase 5：路径反转

```cpp
path_points.reverse();
path_meta_point_types.reverse();
path_meta_point_rids.reverse();
path_meta_point_owners.reverse();
```

因为 corridor 是从终点反向追踪到起点的，所以构建出的路径也是反向的。

---

## 九、Phase 6：路径简化

使用 **Ramer-Douglas-Peucker** 算法递归简化路径：

```
1. 保留起点和终点
2. 在 [起点, 终点] 之间找距离线段最远的点 P
3. 如果 P 的距离 > epsilon：
   a. 保留 P
   b. 递归简化 [起点, P] 和 [P, 终点]
4. 否则：丢弃所有中间点
```

**参数**：`simplify_epsilon`（容差），值越大路径越简洁，但偏离原始路径越多。

简化后会同步裁剪所有元数据数组（types / rids / owners），保持对齐。

---

## 十、Phase 7：路径限制

两种限制可同时使用，以先触发的为准：

### 10.1 `path_return_max_length` — 最大长度

```
逐段累加路径长度
当 accumulated + edge_length > max_length 时：
  remaining = max_length - accumulated
  截断点 = vertex1 + direction × remaining
  丢弃后续所有点
```

### 10.2 `path_return_max_radius` — 最大半径

```
检查每个路径点到起点的距离
当 distance > max_radius 时：
  用 segment_intersects_sphere() 求线段与球的交点
  截断点 = 交点
  丢弃后续所有点
```

---

## 十一、路径元数据

每添加一个路径点时，根据 `metadata_flags` 收集对应的元数据：

| 数组 | 标志位 | 含义 | 值 |
|------|--------|------|----|
| `path_meta_point_types` | `PATH_INCLUDE_TYPES` | 路径点所在区域类型 | `0` = Region, `1` = Link |
| `path_meta_point_rids` | `PATH_INCLUDE_RIDS` | 路径点所属 region/link 的 RID | RID |
| `path_meta_point_owners` | `PATH_INCLUDE_OWNERS` | 路径点所属场景节点的 ObjectID | int64 |

用途举例：判断路径中哪些段经过了 NavLink（跳跃/传送），可以在这些段播放特殊动画。

---

## 十二、线程安全模型

```
                        sync() 时构建
                    ┌──────────────────┐
                    │ NavMapIteration3D │ ← 只读快照（Slot A / Slot B 交替）
                    │  polygon_bvh      │
                    │  region_iterations│
                    │  connections      │
                    │  path_query_slots │
                    └──────────────────┘
                           ↑ 读取（无锁）
              ┌────────────┼────────────┐
              │            │            │
         Thread 1     Thread 2     Thread 3
         Agent A 寻路  Agent B 寻路  Agent C 寻路
              │            │            │
              ↓            ↓            ↓
         Slot 0        Slot 1        Slot 2
         (独占)        (独占)        (独占)
```

- **NavMapIteration3D** 是只读的，多线程可以同时读取，无锁
- **PathQuerySlot** 通过信号量保护并发数量，每个 slot 同一时间只有一个线程使用
- 双缓冲（ping-pong）：`sync()` 时构建新的 iteration 到备用 slot，构建完毕后原子切换
- 查询永远不会阻塞 `sync()`，`sync()` 永远不会阻塞查询

---

## 十三、完整流水线图

```
用户调用 map_get_path(origin, destination, optimize)
  │
  ▼
map_query_path(): 打包参数 → NavMeshPathQueryTask3D
  │
  ▼
NavMap3D::query_path(): 获取 PathQuerySlot（信号量保护）
  │
  ▼
query_task_map_iteration_get_path():
  │
  ├─ Phase 1: 起终点查找
  │   _find_closest_polygon_to_point() × 2
  │   BVH 渐进扩展 + 精炼 → begin/end polygon + position
  │
  ├─ Phase 2: 平凡检查
  │   null → 空路径; 同多边形 → 直连 [begin, end]
  │
  ├─ Phase 3: A* 搜索
  │   展开 internal + external connections
  │   松弛：g = distance × travel_cost + enter_cost
  │   Heap pop → 最小 f 值
  │   搜索限制 → 提前终止
  │   不可达 → fallback（reachable_end → 重新 A*）
  │
  ├─ Phase 4: 后处理（三选一）
  │   ├─ CORRIDORFUNNEL: 漏斗算法（apex/left/right portal 收紧）
  │   ├─ EDGECENTERED: 穿越边中点
  │   └─ NONE: 直接用 entry 点
  │
  ├─ Phase 5: 反转路径（end→begin 变为 begin→end）
  │
  ├─ Phase 6: Ramer-Douglas-Peucker 简化（可选）
  │
  └─ Phase 7: 长度/半径裁剪（可选）
  │
  ▼
返回 path_points[]
```
