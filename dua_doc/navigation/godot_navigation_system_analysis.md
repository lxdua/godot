# Godot 引擎导航寻路系统深度解析

> 基于 Godot 源码分析 | 2026-04-19  
> 源码位置：`modules/navigation_3d/`、`scene/3d/navigation/`、`servers/navigation_3d/`

---

## 目录

1. [整体架构](#1-整体架构)
2. [NavMesh 生成（Recast 集成）](#2-navmesh-生成recast-集成)
3. [地图构建与多边形连接](#3-地图构建与多边形连接)
4. [A* 寻路算法](#4-a-寻路算法)
5. [路径后处理](#5-路径后处理)
6. [NavigationAgent3D 路径跟随](#6-navigationagent3d-路径跟随)
7. [RVO 碰撞避免](#7-rvo-碰撞避免)
8. [优缺点与可改进之处](#8-优缺点与可改进之处)

---

## 1. 整体架构

Godot 的导航系统采用**分层架构**，场景层通过服务器 API 与底层实现交互：

```
场景层 (Scene Nodes)
  ├─ NavigationAgent3D      — 自动路径跟随 + 避障的高层封装
  ├─ NavigationRegion3D     — 持有 NavigationMesh 资源的区域节点
  ├─ NavigationLink3D       — 跨区域/跨层的连接点（楼梯、传送门等）
  └─ NavigationObstacle3D   — 动态障碍物（仅影响 RVO 避障）
       │
       ▼
服务器层 (NavigationServer3D)
  └─ GodotNavigationServer3D (实现)
       ├─ NavMap3D            — 导航地图容器
       │   ├─ NavRegion3D[]   — 区域数据（持有多边形）
       │   ├─ NavLink3D[]     — 链接数据
       │   ├─ NavAgent3D[]    — 避障代理
       │   └─ NavObstacle3D[] — 障碍物数据
       ├─ NavMeshQueries3D    — A* 搜索 + 漏斗算法
       └─ NavMeshGenerator3D  — Recast 集成烘焙器
```

### 关键设计：双缓冲迭代

NavMap 使用 **ping-pong 双缓冲** 架构（`iteration_slots[2]`）：
- 查询线程读取当前 slot 中的快照数据
- 构建线程写入另一个 slot
- 构建完成后原子切换索引

这保证了寻路查询和地图重建可以并发执行，互不阻塞。

### 核心文件清单

| 文件 | 角色 |
|------|------|
| `nav_mesh_queries_3d.cpp` | A* 搜索 + 漏斗算法 + 路径后处理 |
| `nav_map_3d.cpp` | 地图同步、区域连接构建、RVO 仿真调度 |
| `nav_utils_3d.h` | Polygon、Connection、NavigationPoly 等核心数据结构 |
| `nav_mesh_generator_3d.cpp` | Recast 烘焙 pipeline |
| `nav_region_3d.cpp` | 区域内部多边形构建和内部连接 |
| `nav_agent_3d.cpp` | Agent 避障数据管理 |
| `navigation_agent_3d.cpp` | 场景节点：路径跟随 + 信号系统 |

---

## 2. NavMesh 生成（Recast 集成）

Godot 使用 **Recast** 库生成导航网格。整个烘焙流程在 `NavMeshGenerator3D::generator_bake_from_source_geometry_data()` 中实现，共 12 个阶段：

### 2.1 烘焙 Pipeline

```
源几何收集 (parse_source_geometry_data)
    │
    ▼
[1] 配置 rcConfig
    │  从 NavigationMesh 资源读取所有参数
    │  计算几何包围盒 bmin/bmax
    ▼
[2] rcCalcGridSize
    │  计算体素网格尺寸 (width × height)
    │  安全检查：超过 ~30M 体素会中断烘焙
    ▼
[3] rcCreateHeightfield
    │  创建实心体素高度场
    ▼
[4] 标记可行走三角形 + 体素化
    │  rcMarkWalkableTriangles (按坡度角)
    │  rcRasterizeTriangles (光栅化到高度场)
    │  可选过滤器：LowHanging / Ledge / LowHeight
    ▼
[5] rcBuildCompactHeightfield
    │  转换为紧凑高度场
    │  添加非 carve 类型障碍物
    ▼
[6] rcErodeWalkableArea
    │  以 walkableRadius 收缩可行走区域
    │  添加 carve 类型障碍物（绕过 agent_radius 侵蚀）
    ▼
[7] 区域分割
    │  Watershed（默认）/ Monotone / Layers
    ▼
[8] rcBuildContours
    │  从紧凑高度场生成简化轮廓线
    ▼
[9] rcBuildPolyMesh + rcBuildPolyMeshDetail
    │  生成凸多边形网格 + 高精度细节网格
    ▼
[10] 转换为 Godot 格式
    │  去重顶点 + 反转三角形绕序（Recast 与 Godot 相反）
    ▼
[11-12] 清理 + 完成
```

### 2.2 关键烘焙参数

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `cell_size` | 0.25m | XZ 平面体素尺寸，精度与性能的核心权衡 |
| `cell_height` | 0.25m | Y 轴体素高度 |
| `agent_height` | 1.5m | Agent 站立高度，低于此值的通道不可通行 |
| `agent_radius` | 0.5m | Agent 半径，用于侵蚀可行走区域边缘 |
| `agent_max_climb` | 0.25m | 最大攀爬高度（如台阶） |
| `agent_max_slope` | 45° | 最大可行走坡度角 |
| `region_min_size` | 2.0 | 最小区域面积，过小区域被删除 |
| `region_merge_size` | 20.0 | 区域合并阈值面积 |
| `vertices_per_polygon` | 6 | 每个多边形最大顶点数 |
| `partition_type` | Watershed | 分区算法选择 |

> **注意**：`agent_radius` 和 `agent_height` 在烘焙时就固定了。不同体型的单位需要烘焙不同的 NavMesh。

---

## 3. 地图构建与多边形连接

### 3.1 核心数据结构

```cpp
// 导航多边形（凸多边形，非三角形）
struct Polygon {
    uint32_t id;                      // 局部索引
    const NavBaseIteration3D *owner;  // 所属 Region 或 Link
    LocalVector<Vector3> vertices;    // 凸多边形顶点序列
    real_t surface_area;              // 面积
};

// 多边形之间的连接（Portal 边）
struct Connection {
    Polygon *polygon;       // 通向的目标多边形
    int edge;               // 源多边形上的边索引
    Vector3 pathway_start;  // Portal 边起点
    Vector3 pathway_end;    // Portal 边终点
};
```

### 3.2 构建流程

构建分为 **Region 级别**和 **Map 级别**两个阶段：

**Region 级别**（每个 Region 独立构建）：

```
NavRegionBuilder3D::build_iteration()
  │
  ├─ Step 1: 从 NavigationMesh 读取顶点和多边形索引
  │          用 region_transform 变换到世界坐标
  │          生成 Polygon 数组
  │
  ├─ Step 2: 对每条边生成 EdgeKey（栅格化的顶点对哈希）
  │          同一 EdgeKey 最多匹配 2 条边 → EdgeConnectionPair
  │
  └─ Step 3: pair.size == 2 → 内部连接（同 Region 内两多边形共享边）
             pair.size == 1 → 外部边（暴露给 Map 级别做跨区域连接）
```

**Map 级别**（跨 Region 连接）：

```
NavMapBuilder3D::build_navmap_iteration()
  │
  ├─ Step 1: 收集所有 Region 的外部边
  │
  ├─ Step 2-3: EdgeKey 精确匹配
  │            不同 Region 的外部边 EdgeKey 相同 → 建立连接
  │
  ├─ Step 4: 边距模糊匹配（free edges）
  │          对未匹配的自由边做 O(n²) 距离检查
  │          距离 < edge_connection_margin → 建立连接
  │          Portal 端点 = 两边对应点的中点
  │
  ├─ Step 5: NavLink 连接
  │          为每个 NavLink 创建合成多边形
  │          搜索最近的起点/终点多边形
  │          建立 entry → link → exit 的连接链
  │
  └─ Step 6: 更新路径查询槽的索引映射
```

### 3.3 EdgeKey 栅格化

```cpp
// PointKey：顶点按 cell_size 量化到整数格子
union PointKey {
    struct { int64_t x:21; int64_t y:22; int64_t z:21; };
    uint64_t key = 0;
};

// EdgeKey：两个 PointKey 组合，保证 (A,B) == (B,A)
EdgeKey(PointKey a, PointKey b) {
    if (a.key > b.key) SWAP(a, b);
}
```

### 3.4 三层连接体系

| 层级 | 连接方式 | 时机 |
|------|----------|------|
| Region 内部 | 同一 Region 内两多边形共享边 | Region 构建时 |
| 跨 Region 精确 | 不同 Region 的外部边 EdgeKey 完全匹配 | Map 构建 |
| 跨 Region 模糊 | 自由边在 edge_connection_margin 内 | Map 构建 |

### 3.5 NavLink 工作机制

NavLink 不携带自己的 NavMesh，而是动态生成一个**合成多边形**（4 个顶点的退化薄片）。A* 搜索时路径可以经过：`起点多边形 → link 合成多边形 → 终点多边形`，实现跳跃、传送等跨区域移动。

---

## 4. A* 寻路算法

### 4.1 核心数据结构

```cpp
// A* 搜索节点
struct NavigationPoly {
    const Polygon *poly;                // 指向的导航多边形
    int back_navigation_poly_id;        // 父节点索引（用于回溯路径）
    int back_navigation_edge;           // 通过哪条边到达
    Vector3 back_navigation_edge_pathway_start; // 回溯用 Portal 端点
    Vector3 back_navigation_edge_pathway_end;
    Vector3 entry;                      // 进入此多边形的坐标
    real_t traveled_distance;           // g: 已行走距离
    real_t distance_to_destination;     // h: 到终点的启发估计
    
    real_t total_travel_cost() { return traveled_distance + distance_to_destination; }
};
```

**PathQuerySlot**（搜索上下文，可复用槽位池）：

```cpp
struct PathQuerySlot {
    LocalVector<NavigationPoly> path_corridor;  // 预分配 = 全部多边形总数
    Heap<NavigationPoly*> traversable_polys;    // 开放列表（最小堆）
    AHashMap<const Polygon*, uint32_t> poly_to_id; // Polygon* → 索引映射
};
```

> **关键设计**：`path_corridor` 在地图构建时就预分配了等于所有多边形总数的大小。A* 搜索时**零内存分配**——只需将 `traveled_distance` 重置为 `FLT_MAX`。

### 4.2 搜索流程

```
入口：query_task_map_iteration_get_path()

1. 清空上次结果
2. 找到起点/终点所在的多边形（遍历所有多边形做最近点投影）
3. 若起终点在同一多边形 → 直接返回两点路径
4. A* 核心搜索 (build_path_corridor)
5. 路径后处理（漏斗 / 边中心 / 原始）
6. 路径反转（路径是倒序构建的）
7. 可选：Douglas-Peucker 路径简化
8. 路径长度/半径裁剪
```

### 4.3 A* 核心搜索伪代码

```
function build_path_corridor(begin_poly, end_poly, begin_point, end_point):
    // 初始化
    reset all nav_polys (traveled_distance = FLT_MAX)
    begin_nav_poly.traveled_distance = 0
    begin_nav_poly.entry = begin_point
    least_cost_id = poly_to_id[begin_poly]
    
    while true:
        current = navigation_polys[least_cost_id]
        
        // ===== 展开邻居 =====
        for each connection in internal_connections[current]:
            relax_neighbor(connection, current)
        
        for each connection in external_connections[current]:
            relax_neighbor(connection, current)
        
        // ===== 终止判断 =====
        if 开放列表为空:
            // 目标不可达 → 回退到距目标最近的已展开多边形
            // 完全重置搜索，以新终点重新搜索
            ...
        else:
            best = 开放列表.pop()   // f 值最小的节点
            if best.poly == end_poly:
                found_route = true
                break
            least_cost_id = poly_to_id[best.poly]
```

### 4.4 邻居松弛（Relax）

```
function relax_neighbor(connection, current):
    // 入口点 = 当前 entry 投影到 Portal 边上的最近点
    new_entry = closest_point_on_segment(current.entry,
                                          connection.pathway_start,
                                          connection.pathway_end)
    
    // g 代价 = 已走距离 + 到新入口的距离 × 区域旅行权重 + 跨区域进入代价
    new_g = current.traveled_distance
          + dist(current.entry, new_entry) * owner.travel_cost
          + enter_cost
    
    // h 代价 = 到终点的直线距离 × 区域旅行权重
    new_h = dist(new_entry, end_point) * owner.travel_cost
    
    neighbor = navigation_polys[poly_to_id[connection.polygon]]
    if new_g < neighbor.traveled_distance:
        更新 neighbor 的所有字段
        if neighbor 已在堆中:
            heap.shift(neighbor.index)  // decrease-key, O(log n)
        else:
            heap.push(&neighbor)        // 首次入堆
```

### 4.5 优先级比较

```
f 值不同时：f 小的优先
f 值相同时：h 小的优先（更靠近终点的优先）
```

### 4.6 区域权重系统

| 参数 | 含义 | 影响 |
|------|------|------|
| `travel_cost` | 穿越该区域的代价系数 | 乘到 g 和 h 上，>1 表示"不想走" |
| `enter_cost` | 进入该区域的一次性代价 | 跨 Region 时额外加到 g 上 |

例如：沼泽区域 `travel_cost = 3.0`，寻路会倾向绕开它。

### 4.7 搜索限制

| 限制 | 参数 | 行为 |
|------|------|------|
| 最大多边形数 | `path_search_max_polygons` | 展开数量超限 → 强制结束，回退到最近可达点 |
| 最大搜索距离 | `path_search_max_distance` | entry 到起点距离超限 → 强制结束 |
| 不可达回退 | — | 持续跟踪距目标最近的已展开多边形，不可达时以它为新终点重新搜索 |

---

## 5. 路径后处理

A* 搜索输出的是多边形通道（corridor），需要后处理转换为实际路径点。

### 5.1 三种模式

| 模式 | 路径点来源 | 特点 |
|------|-----------|------|
| **CorridorFunnel**（默认） | 漏斗算法拐点 | 最短路径，贴着拐角走 |
| **EdgeCentered** | 每条 Portal 边的中点 | 路径居中，远离墙壁 |
| **None** | A* 的 entry 点 | 最原始，用于调试或自定义 |

### 5.2 漏斗算法（Funnel Algorithm）

漏斗算法将多边形通道转换为最短路径。它维护一个"漏斗"结构：apex（尖端）+ 左边界 + 右边界。

**关键特点**：路径从终点到起点**反向构建**，最后整体翻转。

```
function corridorfunnel(end_point, begin_point, corridor):
    apex_point = end_point
    left_portal = apex_point
    right_portal = apex_point
    
    push(end_point)  // 添加终点
    
    // 从终点沿 corridor 向起点遍历
    for each polygon p in corridor (reverse):
        left, right = p 的 Portal 边端点
        
        // 确保左右方向正确（用叉积判断）
        if cross(apex→left, apex→right) · map_up < 0:
            swap(left, right)
        
        // 处理左边界：
        if left 在漏斗内:
            if left 没越过右边界:
                收窄左边界
            else:
                right_portal 是拐点！
                push(right_portal)
                重置漏斗
        
        // 处理右边界（对称逻辑）
    
    push(begin_point)  // 添加起点
    path_reverse()     // 翻转为起点→终点顺序
```

**3D 地形处理**：漏斗检测到拐点时，通过 `clip_path` 用切割平面与中间 Portal 边求交，确保路径在高低不平的地形上也紧贴 NavMesh 表面。

### 5.3 路径简化

最终路径可选地经过 **Douglas-Peucker** 算法简化：递归找距线段最远的点，超过 epsilon 阈值则保留该点，否则删除。

---

## 6. NavigationAgent3D 路径跟随

NavigationAgent3D 是面向用户的高层封装，提供自动寻路 + 路径跟随 + 避障一体化。

### 6.1 路径更新触发条件

每次调用 `get_next_path_position()` 时检查是否需要重新寻路：

- 地图发生变化
- 当前路径为空
- Agent 偏离路径超过 `path_max_distance`（默认 5m）

### 6.2 路径跟随流程

```
_update_navigation():
  │
  ├─ 需要重新寻路？
  │   → 创建查询参数，调用 NavigationServer3D.query_path()
  │   → 重置路点索引，发出 "path_changed" 信号
  │
  ├─ 检查是否到达目标
  │   → 距离 < target_desired_distance → 触发 "target_reached"
  │
  └─ 推进路点 (_advance_waypoints)
      → while 距当前路点 < path_desired_distance:
          触发 "waypoint_reached"
          如果是 NavLink 路点 → 额外触发 "link_reached"
          前进到下一路点
```

### 6.3 避障集成

```
每物理帧：
  1. 更新 Agent 位置到 NavigationServer
  2. 提交期望速度（velocity → RVO 的 prefVelocity）
  3. NavMap3D::step() 执行 RVO 仿真
  4. 回调 _avoidance_done(safe_velocity)
  5. 发出 "velocity_computed" 信号
  6. 用户在信号回调中用 safe_velocity 移动角色
```

### 6.4 关键属性

| 属性 | 默认值 | 含义 |
|------|--------|------|
| `path_desired_distance` | 1.0m | 路点到达判定距离 |
| `target_desired_distance` | 1.0m | 目标到达判定距离 |
| `path_max_distance` | 5.0m | 偏离路径重寻阈值 |
| `navigation_layers` | 1 | 寻路层过滤掩码 |
| `path_postprocessing` | CorridorFunnel | 路径后处理模式 |
| `simplify_path` | false | 是否简化路径 |
| `keep_y_velocity` | true | 2D 避障时保持 Y 速度 |

---

## 7. RVO 碰撞避免

### 7.1 双模式架构

| 特性 | RVO 2D（默认） | RVO 3D |
|------|----------------|--------|
| 坐标 | XZ 平面运算，Y 作为 elevation | 完整 3D |
| 静态障碍物 | ✅ 支持 | ❌ 不支持 |
| 适用场景 | 平面/低起伏地形 | 多层/飞行 |

### 7.2 ORCA 算法流程

```
NavMap3D::step(delta):
  对每个活跃 Agent（可多线程并行）:
    1. computeNeighbors()      — KdTree 查找附近 Agent + 障碍物
    2. computeNewVelocity()    — 对每个邻居构造 ORCA 半平面约束
                                  在约束下做线性规划，找最接近 prefVelocity 的可行速度
    3. update()                — 应用新速度，受 maxSpeed 限制
    4. dispatch_callback()     — 回调通知场景节点
```

### 7.3 避障层系统

32 位位掩码，类似物理层：

```
Agent A 是否避开 Agent B？
→ (A.avoidance_mask & B.avoidance_layers) != 0

avoidance_layers：声明"我属于哪些层"（被谁看到）
avoidance_mask：  声明"我关注哪些层"（我看谁）
avoidance_priority（0.0-1.0）：越低越不让路
```

### 7.4 重要注意事项

- **障碍物不修改 NavMesh**：`NavObstacle3D` 只参与 RVO 仿真，不影响多边形连接和 A* 搜索
- **2D 模式 Y 轴保持**：提交避障前保存 Y 速度，计算后恢复，避免穿透地形
- **不要直接设置 velocity**：应设置 prefVelocity，让 ORCA 计算安全速度

---

## 8. 优缺点与可改进之处

### ✅ 优点

**1. 工程质量高**
- 预分配搜索节点数组，A* 搜索时零内存分配
- 自定义堆支持 O(log n) decrease-key，避免重复入堆
- PathQuerySlot 槽位池 + 信号量实现多线程并发寻路
- 双缓冲迭代架构，查询与构建互不阻塞

**2. Recast 集成完整**
- 完整的 12 阶段烘焙 pipeline
- 支持 Watershed / Monotone / Layers 三种分区算法
- 支持 ProjectedObstruction（carve / 非 carve 两种模式）
- 参数精度检查和警告机制

**3. 功能丰富**
- 三种路径后处理模式（漏斗 / 边中心 / 原始）
- 区域权重系统（travel_cost + enter_cost）
- NavLink 跨区域连接（楼梯、跳跃、传送）
- navigation_layers 32 位过滤 + include/exclude 列表
- 搜索限制（最大多边形数、最大距离、最大返回路径长度/半径）
- 不可达时智能回退到最近可达点
- Douglas-Peucker 路径简化

**4. 避障系统完善**
- RVO 2D + 3D 双模式
- KdTree 加速邻居查询
- avoidance_layers / mask / priority 选择性避让
- 多线程并行仿真

### ❌ 缺点

**1. 不支持运行时可变体型寻路**
- agent_radius 在烘焙时就固定了
- 不同体型需要烘焙多份 NavMesh（内存 ×N，构建时间 ×N）
- 没有三角形宽度（triangle width）或通道宽度概念
- 没有路径膨胀（expanded vertices）机制
- **改进方案**：参考星际争霸 2 的方案——单份 NavMesh + 预计算通道宽度 + A* 宽度过滤 + 膨胀顶点

**2. 障碍物不影响寻路**
- NavObstacle3D 只参与 RVO 避障，不修改 NavMesh 多边形
- 如果一个大障碍物挡住了通道，A* 仍然会规划穿过它的路径
- 单位会走到障碍物面前才被 RVO "推"开，行为不自然
- **改进方案**：支持动态障碍物对 NavMesh 的局部修改（如 Detour 的 TempObstacle），或在 A* 中集成障碍物感知

**3. 起点/终点查找是暴力遍历**
- `_query_task_find_start_end_positions()` 遍历所有 Region 的所有多边形
- 对每个凸多边形做 fan 三角化 + 最近点投影
- 多边形数量大时（数万+）这是一个性能瓶颈
- **改进方案**：为多边形构建空间索引（如 BVH 或 KdTree），将 O(n) 查找降为 O(log n)

**4. 跨 Region 模糊匹配是 O(n²)**
- `_build_step_edge_connection_margin_connections()` 对所有自由边做 O(n²) 两两比较
- 地图 Region 切片多时可能较慢
- **改进方案**：用空间哈希或 R-Tree 加速自由边的近邻查找

**5. 没有内置的分层 / 分区寻路**
- 大型开放世界地图（几十万多边形）A* 搜索空间可能很大
- 没有 HPA*（Hierarchical Pathfinding A*）或类似的分层加速机制
- **改进方案**：引入分层导航图——先在粗粒度 Region 级别 A*，再在细粒度多边形级别 A*

**6. 不支持流场寻路**
- 大量同目标单位（RTS 场景）每个都要独立 A*，开销大
- 没有流场（Flow Field）生成能力
- **改进方案**：在 NavMesh 多边形图上实现流场生成，一次 Dijkstra 生成全局方向场，所有同目标单位共享

**7. 漏斗算法不感知 Agent 半径**
- 漏斗算法直接在原始 Portal 边上运行
- 生成的路径贴着拐角走，不考虑单位的物理半径
- 大单位可能"削"到墙角
- **改进方案**：在漏斗算法前对 Portal 边做膨胀（向内缩 agentRadius），再跑漏斗

**8. 不可达回退会重新搜索**
- 目标不可达时，先搜一遍发现不可达，再以最近可达点为新终点完全重来
- 最坏情况下搜索两遍
- **改进方案**：可以在第一遍搜索时直接记录到每个已展开节点的最优路径，回退时直接回溯，不重搜

### 🔧 可以补充的功能

| 功能 | 优先级 | 说明 |
|------|--------|------|
| 可变体型寻路 | ⭐⭐⭐ | 单份 NavMesh + 通道宽度 + 膨胀顶点（SC2 方案） |
| 动态障碍物影响寻路 | ⭐⭐⭐ | NavObstacle 能局部修改 NavMesh 或 A* 代价 |
| 多边形空间索引 | ⭐⭐ | BVH/KdTree 加速起终点查找 |
| 分层寻路（HPA*） | ⭐⭐ | 大地图上减少 A* 搜索空间 |
| 流场寻路 | ⭐⭐ | RTS 大量同目标单位场景 |
| 漏斗算法半径感知 | ⭐⭐ | Portal 膨胀，大单位不削墙角 |
| 异步分帧寻路 | ⭐ | 单次寻路跨多帧执行，避免单帧卡顿 |
| 路径预测缓存 | ⭐ | 相近起终点复用已有路径的部分结果 |

---

> **总结**：Godot 的导航系统是一套工程质量很高的 **Recast + A* + Funnel + RVO** 标准管线。对于大部分 RPG、FPS、动作游戏场景完全够用。但如果你要做 **RTS（大量单位、多体型、动态地图）** 或 **大型开放世界**，现有系统在可变体型、流场、分层寻路等方面还有提升空间。
