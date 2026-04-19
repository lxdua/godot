# Godot 导航系统源码深度解析

> 基于 Godot 引擎源码逐函数分析 | 2026-04-19
> 不含 RVO 避障部分

---

## 目录

1. [整体架构与类关系](#1-整体架构与类关系)
2. [NavMesh 烘焙（Recast 集成）](#2-navmesh-烘焙)
3. [地图构建与多边形连接](#3-地图构建与多边形连接)
4. [A* 寻路算法](#4-a-寻路算法)
5. [漏斗算法与路径后处理](#5-漏斗算法与路径后处理)
6. [NavigationAgent3D 路径跟随](#6-navigationagent3d-路径跟随)
7. [完整调用链总结](#7-完整调用链总结)

---

## 1. 整体架构与类关系

### 1.1 分层架构

Godot 导航系统分为**场景层**和**服务器层**两层：

```
场景层 (用户直接使用的节点)
├── NavigationRegion3D        持有 NavigationMesh，触发烘焙，向 Server 提交网格数据
├── NavigationAgent3D         路径跟随 + 避障的高层封装，用户最常交互的节点
├── NavigationLink3D          跨区域连接点（楼梯、跳台、传送门）
└── NavigationObstacle3D      动态障碍物（仅影响 RVO）

服务器层 (底层实现)
├── NavigationServer3D        公共 API 接口（虚基类）
├── GodotNavigationServer3D   具体实现
│   └── NavMap3D              导航地图容器
│       ├── NavRegion3D       区域内部数据
│       ├── NavLink3D         链接内部数据
│       ├── NavAgent3D        代理内部数据
│       └── NavObstacle3D     障碍物内部数据
├── NavMeshGenerator3D        Recast 烘焙器
├── NavMeshQueries3D          A* + 漏斗算法（静态方法集合）
├── NavRegionBuilder3D        Region 级多边形构建
└── NavMapBuilder3D           Map 级跨区域连接构建
```

### 1.2 核心源文件清单

| 文件 | 路径 | 角色 |
|------|------|------|
| `navigation_mesh.h/cpp` | `scene/resources/` | NavigationMesh 资源类，所有烘焙参数定义 |
| `navigation_region_3d.h/cpp` | `scene/3d/navigation/` | 场景节点，触发烘焙，向 Server 提交网格 |
| `navigation_agent_3d.h/cpp` | `scene/3d/navigation/` | 场景节点，路径跟随 + 信号系统 |
| `nav_mesh_generator_3d.h/cpp` | `modules/navigation_3d/3d/` | Recast 烘焙 pipeline（12 阶段） |
| `nav_region_builder_3d.h/cpp` | `modules/navigation_3d/3d/` | Region 内部多边形构建 + 内部连接 |
| `nav_map_builder_3d.h/cpp` | `modules/navigation_3d/3d/` | Map 级跨区域连接 + NavLink 连接 |
| `nav_mesh_queries_3d.h/cpp` | `modules/navigation_3d/3d/` | A* 搜索 + 漏斗算法 + 路径后处理 |
| `nav_map_3d.h/cpp` | `modules/navigation_3d/` | NavMap 同步、双缓冲、查询槽池 |
| `nav_region_3d.h/cpp` | `modules/navigation_3d/` | Region 同步、异步构建管理 |
| `nav_utils_3d.h` | `modules/navigation_3d/` | Polygon、Connection、NavigationPoly 等核心数据结构 |
| `nav_map_iteration_3d.h` | `modules/navigation_3d/3d/` | NavMapIteration3D 迭代快照 |
| `nav_heap.h` | `servers/` | 自定义 Heap（支持 decrease-key） |

### 1.3 核心数据结构一览

**`Nav3D::Polygon`** — 导航多边形（`nav_utils_3d.h` L98-107）

```cpp
struct Polygon {
    uint32_t id = UINT32_MAX;                    // 在所属 region 中的局部索引
    const NavBaseIteration3D *owner = nullptr;   // 所属 region 或 link 的迭代快照
    LocalVector<Vector3> vertices;               // 凸多边形顶点（世界坐标）
    real_t surface_area = 0.0;                   // 面积
};
```

**`Nav3D::Connection`** — 多边形之间的连接 / Portal 边（L84-96）

```cpp
struct Connection {
    Polygon *polygon = nullptr;  // 连接指向的目标多边形
    int edge = -1;               // 源多边形上的边索引
    Vector3 pathway_start;       // Portal 边起点
    Vector3 pathway_end;         // Portal 边终点
};
```

**`Nav3D::NavigationPoly`** — A* 搜索节点（L109-150）

```cpp
struct NavigationPoly {
    const Polygon *poly = nullptr;
    uint32_t traversable_poly_index = UINT32_MAX;  // 在堆中的索引
    int back_navigation_poly_id = -1;               // 父节点（路径回溯）
    int back_navigation_edge = -1;                  // 经过哪条边到达
    Vector3 back_navigation_edge_pathway_start;     // 回溯用 Portal 端点
    Vector3 back_navigation_edge_pathway_end;
    Vector3 entry;                                  // 进入此多边形的坐标
    real_t traveled_distance = 0.0;                 // g: 已旅行距离
    real_t distance_to_destination = 0.0;           // h: 到终点的启发估计
    
    real_t total_travel_cost() const {              // f = g + h
        return traveled_distance + distance_to_destination;
    }
    
    void reset() {
        poly = nullptr;
        traversable_poly_index = UINT32_MAX;
        back_navigation_poly_id = -1;
        traveled_distance = FLT_MAX;  // 关键：初始化为最大值
    }
};
```

**`Nav3D::PointKey`** — 栅格化顶点键（L45-53）

```cpp
union PointKey {
    struct {
        int64_t x : 21;  // ±100 万个格子
        int64_t y : 22;
        int64_t z : 21;
    };
    uint64_t key = 0;  // 合并为 64 位用于哈希
};
```

**`Nav3D::EdgeKey`** — 无向边键（L55-74）

```cpp
struct EdgeKey {
    PointKey a, b;
    // 构造时自动 SWAP 保证 a.key <= b.key → (A,B) == (B,A)
};
```

**`PathQuerySlot`** — 查询槽（`nav_mesh_queries_3d.h` L49-55）

```cpp
struct PathQuerySlot {
    LocalVector<NavigationPoly> path_corridor;     // 预分配 = 全部多边形总数
    Heap<NavigationPoly*, ...> traversable_polys;  // A* 开放列表
    AHashMap<const Polygon*, uint32_t> poly_to_id; // Polygon* → 数组索引
    bool in_use = false;
};
```

---

## 2. NavMesh 烘焙

### 2.1 烘焙参数（NavigationMesh 资源）

定义在 `scene/resources/navigation_mesh.h` L80-106，默认值在 `.cpp` 中设置：

| 参数 | 默认值 | Recast 映射 | 转换公式 |
|------|--------|-------------|----------|
| `cell_size` | 0.25m | `cfg.cs` | 直接赋值 |
| `cell_height` | 0.25m | `cfg.ch` | 直接赋值 |
| `agent_height` | 1.5m | `cfg.walkableHeight` | `ceil(val / ch)` |
| `agent_radius` | 0.5m | `cfg.walkableRadius` | `ceil(val / cs)` |
| `agent_max_climb` | 0.25m | `cfg.walkableClimb` | `floor(val / ch)` |
| `agent_max_slope` | 45° | `cfg.walkableSlopeAngle` | 直接赋值 |
| `region_min_size` | 2.0 | `cfg.minRegionArea` | `(int)(val * val)` |
| `region_merge_size` | 20.0 | `cfg.mergeRegionArea` | `(int)(val * val)` |
| `edge_max_length` | 0.0 | `cfg.maxEdgeLen` | `(int)(val / cs)` |
| `edge_max_error` | 1.3 | `cfg.maxSimplificationError` | 直接赋值 |
| `vertices_per_polygon` | 6 | `cfg.maxVertsPerPoly` | `(int)val` |
| `detail_sample_distance` | 6.0 | `cfg.detailSampleDist` | `MAX(cs * val, 0.1)` |
| `detail_sample_max_error` | 1.0 | `cfg.detailSampleMaxError` | `ch * val` |
| `partition_type` | Watershed | — | 决定分区算法 |

> `agent_radius` 和 `agent_height` 在烘焙时就固定了。不同体型需要不同的 NavigationMesh。

### 2.2 烘焙触发调用链

用户在编辑器点击"Bake"或代码中调用 `NavigationRegion3D.bake_navigation_mesh()`：

```
NavigationRegion3D::bake_navigation_mesh(p_on_thread)
    [scene/3d/navigation/navigation_region_3d.cpp L227]
│
├── 1. 创建 NavigationMeshSourceGeometryData3D                    [L231]
│
├── 2. 收集源几何数据
│      NavigationServer3D::parse_source_geometry_data()            [L234]
│      └→ NavMeshGenerator3D::generator_parse_source_geometry_data [L281]
│          ├── 根据 source_geometry_mode 决定遍历哪些节点           [L284]
│          │   ├── ROOT_NODE_CHILDREN → 根节点及子树
│          │   └── GROUPS_* → 指定组内的节点
│          ├── 计算 root_node_transform = root.global_transform.inverse [L291]
│          └── 对每个节点调用 generator_parse_geometry_node()       [L298]
│              └── 遍历注册的解析器回调(NavMeshGeometryParser3D)    [L265]
│                  各模块(MeshInstance3D, StaticBody3D等)提取 mesh → 
│                  source_data.add_mesh() / add_faces()
│
├── 3. 烘焙
│   ├── 同步模式 (p_on_thread=false):
│   │   NavigationServer3D::bake_from_source_geometry_data()       [L239]
│   │   └→ NavMeshGenerator3D::generator_bake_from_source_geometry_data [L305]
│   │       └── (12 步 Recast Pipeline，见下文)
│   │
│   └── 异步模式 (p_on_thread=true):
│       NavigationServer3D::bake_from_source_geometry_data_async() [L237]
│       └→ 创建 WorkerThread 任务 → generator_thread_bake()       [L251]
│           └→ generator_bake_from_source_geometry_data()           [L254]
│
├── 4. 烘焙完成回调
│      _bake_finished()                                            [L243]
│      └→ emit_signal("bake_finished")                             [L249]
│
└── 5. 网格变更通知
       _navigation_mesh_changed()                                  [L332]
       └→ NavigationServer3D::region_set_navigation_mesh()         [L335]
           (将烘焙结果提交给导航服务器，触发地图重建)
```

### 2.3 Recast 烘焙 12 步 Pipeline

全部在 `nav_mesh_generator_3d.cpp` 的 `generator_bake_from_source_geometry_data()` 函数中（L305-588）：

```
Step 1  [L333-407] CONFIGURATION
│  从 NavigationMesh 读取参数，转换为 rcConfig
│  rcCalcBounds() 计算几何体 AABB
│  精度损失警告（agent_radius < cell_size 等）
│
Step 2  [L409-421] CALC_GRID_SIZE
│  rcCalcGridSize() 计算体素网格尺寸 (width × height)
│  安全检查：超过 ~30M 体素会中断
│
Step 3  [L423-427] CREATE_HEIGHTFIELD
│  rcAllocHeightfield() + rcCreateHeightfield()
│  创建实心体素高度场
│
Step 4  [L429-450] MARK_WALKABLE + RASTERIZE + FILTER
│  rcMarkWalkableTriangles()   按坡度角标记可行走三角形
│  rcRasterizeTriangles()      光栅化到高度场
│  [可选] rcFilterLowHangingWalkableObstacles()
│  [可选] rcFilterLedgeSpans()
│  [可选] rcFilterWalkableLowHeightSpans()
│
Step 5  [L452-477] COMPACT_HEIGHTFIELD + 非carve障碍物
│  rcBuildCompactHeightfield()  转为紧凑高度场
│  遍历 projected_obstructions (carve=false):
│    rcMarkConvexPolyArea(RC_NULL_AREA)
│  ↑ 在 erode 之前标记 → 会被 agent_radius 侵蚀影响
│
Step 6  [L479-498] ERODE + carve障碍物
│  rcErodeWalkableArea(walkableRadius)  按 agent 半径侵蚀
│  遍历 projected_obstructions (carve=true):
│    rcMarkConvexPolyArea(RC_NULL_AREA)
│  ↑ 在 erode 之后标记 → 精确切割，不受 agent_radius 影响
│
Step 7  [L500-509] PARTITION
│  根据 partition_type 选择：
│  ├── Watershed: rcBuildDistanceField() + rcBuildRegions()
│  ├── Monotone: rcBuildRegionsMonotone()
│  └── Layers:   rcBuildLayerRegions()
│
Step 8  [L511-516] CONTOURS
│  rcBuildContours()  从紧凑高度场生成简化轮廓线
│
Step 9  [L518-531] POLYMESH + DETAIL
│  rcBuildPolyMesh()        生成凸多边形网格
│  rcBuildPolyMeshDetail()  生成高精度细节网格
│
Step 10 [L533-578] CONVERT TO GODOT FORMAT
│  顶点去重 (HashMap<Vector3, int>)
│  三角形索引翻转 (Recast 与 Godot 绕序相反)
│  NavigationMesh::set_data(vertices, polygons)
│
Step 11 [L580-585] CLEANUP
│  释放 rcPolyMesh / rcPolyMeshDetail
│
Step 12 [L587] FINISHED
```

### 2.4 投影障碍物的两阶段处理

这是一个容易忽略但很重要的设计：

```
                    非 carve (Step 5)          carve (Step 6)
标记时机             erode 之前                 erode 之后
agent_radius 影响    ✅ 会被侵蚀扩大           ❌ 精确切割
适用场景             普通障碍物                  需要精确边界的障碍物
```

非 carve 障碍物在侵蚀前标记，所以最终的不可行走区域会比障碍物本身**大一圈**（agent_radius）。carve 障碍物在侵蚀后标记，不受 radius 影响，切割边界精确对齐。

---

## 3. 地图构建与多边形连接

烘焙完成后，NavigationMesh 的数据被提交到 NavigationServer。接下来需要把原始的顶点/多边形数据转换为运行时的导航图（多边形 + 连接关系）。

这个过程分为两个层级：**Region 级**（单个区域内部）和 **Map 级**（跨区域连接）。

### 3.1 双缓冲迭代架构

在深入构建流程之前，先理解 NavMap 的并发设计：

```
NavMap3D 维护两个迭代槽 iteration_slots[2]（ping-pong 双缓冲）

Frame N:
  slot[0] = 当前活跃 (iteration_slot_index=0)
            查询线程正在读取
  slot[1] = 空闲 (users==0)
            构建线程可以写入

  sync() → 检查 slot[1].users == 0 → 在 slot[1] 上构建新数据

  构建完成 → write_lock → iteration_slot_index = 1 → write_unlock
  slot[1] 变为活跃，新查询读 slot[1]
  slot[0] 等 users 降为 0 后可被下次构建使用
```

**关键数据结构** (`nav_map_3d.h` L143-145):

```cpp
uint32_t iteration_slot_index = 0;
LocalVector<NavMapIteration3D> iteration_slots;  // 固定 2 个
mutable RWLock iteration_slot_rwlock;
```

**读端 RAII 守卫** (`nav_map_iteration_3d.h` L109-122):

```cpp
class NavMapIterationRead3D {
    NavMapIterationRead3D(const NavMapIteration3D &p) : map_iteration(p) {
        map_iteration.rwlock.read_lock();
        map_iteration.users.increment();    // 原子 +1
    }
    ~NavMapIterationRead3D() {
        map_iteration.users.decrement();    // 原子 -1
        map_iteration.rwlock.read_unlock();
    }
};
```

### 3.2 NavMap3D::sync() 主流程

同步入口在 `nav_map_3d.cpp` L430-472：

```
NavMap3D::sync()
│
├── 1. _sync_async_tasks()                              [L437]
│      检查异步 Region 构建是否完成
│
├── 2. _sync_dirty_map_update_requests()                [L439]
│      ├── if map_settings_dirty:
│      │   所有 region->scratch_polygons()  强制重建
│      ├── 遍历脏 Region: region->sync()
│      │   ├── if dirty → _build_iteration()  触发 Region 构建
│      │   └── if ready → _sync_iteration()   交换迭代数据
│      │       → requires_map_update = true
│      └── 遍历脏 Link: link->sync()
│
├── 3. 判断是否需要 Map 级构建                           [L441-457]
│      if iteration_dirty && !building && !ready:
│      │   _build_iteration()                           [L442]
│      │   ├── 检查下一个 slot.users == 0               [L344]
│      │   ├── 收集所有 region/link 的迭代快照          [L381]
│      │   ├── NavMapBuilder3D::build_navmap_iteration() [L396/398]
│      │   └── (可同步或异步)
│      │
│      if 构建完成:
│         _sync_iteration()                             [L454]
│         ├── iteration_id++
│         ├── write_lock
│         ├── iteration_slot_index = (index+1) % 2     ← ping-pong 切换
│         ├── write_unlock
│         └── emit_signal("map_changed")
│
└── 4. _sync_avoidance()                                [L461]
       (RVO 相关，此文档不展开)
```

### 3.3 Region 级构建：三步流程

入口：`NavRegionBuilder3D::build_iteration()` (`nav_region_builder_3d.cpp` L39-55)

**Step 1: 处理网格数据** (`_build_step_process_navmesh_data`, L57-147)

```
输入：NavigationMesh 的 vertices + polygon indices + region_transform
输出：世界空间的 Polygon 数组

对每个多边形：
  1. 设置 polygon.id = i, polygon.owner = region_iteration
  2. 跳过顶点数 < 3 的退化多边形
  3. 对每个顶点执行 region_transform.xform(vertex) → 世界坐标
  4. 三角扇分解计算面积：sum(Face3(v[0], v[j-1], v[j]).get_area())
  5. 累积 AABB bounds
```

**Step 2: 查找边连接对** (`_build_step_find_edge_connection_pairs`, L167-223)

```
对每个多边形的每条边：
  1. 计算 EdgeKey：
     point_key = floor(vertex / cell_size) → 打包到 PointKey (21+22+21=64bit)
     edge_key = EdgeKey(point_key_a, point_key_b)  // 自动排序保证无向
  
  2. 在 HashMap<EdgeKey, EdgeConnectionPair> 中查找/插入
  
  3. pair.size < 2: 添加 Connection 到 pair
     pair.size == 2: 这条边被两个多边形共享 → 后续会变成内部连接
     pair.size > 2: 异常！超过 2 个多边形映射到同一边 → edge_merge_error
```

**Step 3: 合并边连接对** (`_build_step_merge_edge_connection_pairs`, L225-257)

```
遍历 connection_pairs_map：
  pair.size == 2:
    → 内部连接！互相添加到 internal_connections[polygon_id]
    → polygon A 的连接列表加上 polygon B，反之亦然
  
  pair.size == 1:
    → 外部边！构建 ConnectableEdge，加入 external_edges 列表
    → 等待 Map 级构建做跨区域匹配
```

**构建结果**：每个 Region 得到一个 `NavRegionIteration3D` 快照，包含：
- `navmesh_polygons[]` — 多边形数组
- `internal_connections[][]` — 内部连接（[polygon_id] → connections）
- `external_edges[]` — 暴露给 Map 级别的自由边

### 3.4 Map 级构建：五步流程

入口：`NavMapBuilder3D::build_navmap_iteration()` (`nav_map_builder_3d.cpp` L54-74)

**Step 1: 收集区域多边形** (`_build_step_gather_region_polygons`, L76-101)

```
遍历所有 region_iterations：
  累计 polygon_count
  为每个 region 初始化空的外部连接容器
  初始化 navbases_polygons_external_connections HashMap
```

**Step 2-3: 跨区域精确边匹配** (L103-188)

```
Step 2: 收集所有 region 的 external_edges
        用全局 EdgeKey HashMap 分组
        不同 Region 的边映射到同一个 EdgeKey → 可连接

Step 3: pair.size == 2 → 跨 Region 连接！
          navbases_polygons_external_connections[owner][polygon_id].push_back(对方)
        pair.size == 1 → 自由边，加入 free_edges 列表
```

**Step 4: 边距模糊匹配** (`_build_step_edge_connection_margin_connections`, L190-269) ⭐

当两个 Region 的边没有精确对齐（比如手动拼接的地图），用距离匹配：

```
对所有自由边做 O(n²) 两两比较（跳过同 Region 的）：

  边 i: P1→P2 (来自 Region A)
  边 j: Q1→Q2 (来自 Region B)

  1. 将 Q1、Q2 投影到 P1→P2 的参数空间 [0,1]:
     ratio_1 = dot(P1P2, Q1-P1) / |P1P2|²
     ratio_2 = dot(P1P2, Q2-P1) / |P1P2|²

  2. 快速剔除：两个 ratio 都 < 0 或都 > 1 → 完全不重叠，跳过

  3. 计算通道起点：
     self1 = P1 + P1P2 * CLAMP(ratio_1, 0, 1)    // P 边上的对应点
     other1 = 反向插值得到 Q 边上的对应点
     检查 distance(self1, other1) < edge_connection_margin

  4. 同理计算通道终点 self2、other2

  5. 创建连接：
     pathway_start = (self1 + other1) / 2    // 两边中点作为 Portal
     pathway_end   = (self2 + other2) / 2
```

**Step 5: NavLink 连接** (`_build_step_navlink_connections`, L271-393)

```
对每个 NavLink：
  1. 创建合成多边形：4 个顶点 [start, start, end, end]（退化四边形）
  
  2. 遍历所有 Region 的所有多边形：
     用三角扇 Face3::get_closest_point_to() 找：
     - closest_start_polygon：距 link 起点最近的多边形（距离 < link_connection_radius）
     - closest_end_polygon：距 link 终点最近的多边形
  
  3. 创建前向连接链：
     start_polygon → [entry_connection] → link合成多边形 → [exit_connection] → end_polygon
  
  4. 如果 bidirectional，创建反向连接链
```

**最后：更新查询槽** (`_build_update_map_iteration`, L395-431)

```
对每个 PathQuerySlot：
  path_corridor.resize(总多边形数)
  poly_to_id.clear()
  遍历所有 region 多边形 + navlink 多边形
  建立 Polygon* → global_id 映射
```

### 3.5 三层连接体系总结

```
┌─────────────────────────────────────────────────────┐
│ Region 内部连接                                      │
│   同一 Region 内两多边形共享边 (EdgeKey 相同)         │
│   存储：region_iteration->internal_connections       │
│   时机：Region 构建 Step 2-3                         │
├─────────────────────────────────────────────────────┤
│ 跨 Region 精确连接                                   │
│   不同 Region 的外部边 EdgeKey 完全匹配               │
│   存储：navbases_polygons_external_connections        │
│   时机：Map 构建 Step 2-3                            │
├─────────────────────────────────────────────────────┤
│ 跨 Region 模糊连接                                   │
│   自由边在 edge_connection_margin 距离内              │
│   Portal = 两边对应点的中点                           │
│   存储：navbases_polygons_external_connections        │
│        + external_region_connections                  │
│   时机：Map 构建 Step 4                              │
├─────────────────────────────────────────────────────┤
│ NavLink 连接                                         │
│   合成退化多边形 [start,start,end,end]                │
│   entry/exit 连接链                                  │
│   时机：Map 构建 Step 5                              │
└─────────────────────────────────────────────────────┘
```

---

## 4. A* 寻路算法

地图构建完成后，多边形之间的连接图就绑了。当用户请求寻路时，A* 算法在这个图上搜索最短路径。

### 4.1 查询入口与整体流程

入口函数：`NavMeshQueries3D::query_task_map_iteration_get_path()`
位置：`nav_mesh_queries_3d.cpp` L514-581

```
query_task_map_iteration_get_path()
│
├── 1. path_clear()                           清空上次结果
│
├── 2. _query_task_find_start_end_positions()  找到起/终点所在多边形 [L237]
│      暴力遍历所有 Region 的所有多边形
│      对每个凸多边形做 fan 三角化
│      用 Face3::get_closest_point_to() 找最近点
│      过滤条件：navigation_layers 位与 + region include/exclude
│
├── 3. 平凡情况检查
│      ├── 无起/终点多边形 → 返回空路径
│      └── 起 == 终 → 直接返回 [begin_point, end_point]
│
├── 4. _query_task_build_path_corridor()      核心 A* 搜索 [L319]
│
├── 5. 路径后处理（根据 path_postprocessing 选择）
│      ├── CORRIDORFUNNEL → _query_task_post_process_corridorfunnel() [L735]
│      ├── EDGECENTERED   → _query_task_post_process_edgecentered()  [L830]
│      └── NONE           → _query_task_post_process_nopostprocessing() [L859]
│
├── 6. path_reverse()                         路径是倒序构建的，需要反转
│
├── 7. [可选] _query_task_simplified_path_points()  Douglas-Peucker 简化 [L693]
│
└── 8. _query_task_process_path_result_limits()     长度/半径裁剪 [L607]
```

### 4.2 查询槽池管理

A* 搜索需要大量临时数据（开放列表、走廊数组等）。Godot 使用**预分配的查询槽池**来避免运行时内存分配：

位置：`nav_map_3d.cpp` L147-182

```cpp
void NavMap3D::query_path(NavMeshPathQueryTask3D &p_query_task) {
    // 1. 获取信号量（限制并发数量）
    map_iteration.path_query_slots_semaphore.wait();
    
    // 2. 加锁找一个空闲槽
    map_iteration.path_query_slots_mutex.lock();
    for (PathQuerySlot &slot : map_iteration.path_query_slots) {
        if (!slot.in_use) {
            slot.in_use = true;
            p_query_task.path_query_slot = &slot;
            break;
        }
    }
    map_iteration.path_query_slots_mutex.unlock();
    
    // 3. 执行查询
    NavMeshQueries3D::query_task_map_iteration_get_path(p_query_task, map_iteration);
    
    // 4. 归还槽
    slot.in_use = false;
    map_iteration.path_query_slots_semaphore.post();
}
```

每个 `PathQuerySlot` 在地图构建时就预分配了：
- `path_corridor`：大小 = 全部多边形总数的 `NavigationPoly` 数组
- `poly_to_id`：`Polygon*` → 数组索引的哈希表
- `traversable_polys`：自定义 Heap（开放列表）

### 4.3 Heap 实现（开放列表）

位置：`servers/nav_heap.h`

Godot 使用自定义 Heap 而非 STL priority_queue，关键原因是**支持 decrease-key 操作**：

```cpp
// 实例化类型
Heap<NavigationPoly*, NavPolyTravelCostGreaterThan, NavPolyHeapIndexer>
```

**比较器** (`nav_utils_3d.h` L152-163):

```cpp
struct NavPolyTravelCostGreaterThan {
    bool operator()(const NavigationPoly *a, const NavigationPoly *b) const {
        real_t f_a = a->total_travel_cost();  // f = g + h
        real_t f_b = b->total_travel_cost();
        if (f_a != f_b) return f_a > f_b;    // f 小的优先
        return a->distance_to_destination > b->distance_to_destination;  // f 相同时 h 小的优先
    }
};
```

> 这是一个 max-heap + 反向比较器 = 效果等同于按 f 值的最小堆。f 相同时，h 更小的（更靠近终点的）优先出堆。

**索引器** (`nav_utils_3d.h` L165-169):

```cpp
struct NavPolyHeapIndexer {
    void operator()(NavigationPoly *p_poly, uint32_t p_heap_index) const {
        p_poly->traversable_poly_index = p_heap_index;
    }
};
```

堆内每次元素位置变化时，通过索引器回写新索引到 `NavigationPoly::traversable_poly_index`。这使得 `shift()` 方法（decrease-key）可以 O(log n) 完成：

```cpp
// nav_heap.h
void push(const T &elem) {                    // 插入 + 上浮
    _buffer.push_back(elem);
    _indexer(elem, _buffer.size() - 1);
    _shift_up(_buffer.size() - 1);
}

T pop() {                                      // 弹出堆顶 + 下沉
    T value = _buffer[0];
    _indexer(value, INVALID_INDEX);            // 标记已移出堆
    // swap with last, remove last, shift_down(0)
    ...
}

void shift(uint32_t p_index) {                 // decrease-key
    if (!_shift_up(p_index)) _shift_down(p_index);
}
```

### 4.4 A* 核心搜索

位置：`_query_task_build_path_corridor()` (`nav_mesh_queries_3d.cpp` L319-512)

**初始化**（L319-365）:

```
1. 清空开放列表：traversable_polys.clear()
2. 重置所有搜索节点：navigation_polys[*].reset() → traveled_distance = FLT_MAX
3. 初始化起点节点：
   begin_nav_poly.poly = begin_polygon
   begin_nav_poly.entry = begin_point
   begin_nav_poly.traveled_distance = 0     // g(start) = 0
4. least_cost_id = poly_to_id[begin_polygon]
5. 状态变量：
   found_route = false
   reachable_end = nullptr          // 不可达时的回退目标
   distance_to_reachable_end = FLT_MAX
   is_reachable = true
```

**主循环**（L366-482）:

```
while (true):
    current = navigation_polys[least_cost_id]
    processed_count += 1
    
    //====== 步骤 1: 展开邻居 ======
    
    // 1a. 内部连接（同 Region 内的相邻多边形）
    for connection in internal_connections[current.poly.owner][current.poly.id]:
        _query_task_search_polygon_connections(connection, ...)
    
    // 1b. 外部连接（跨 Region 边合并 / NavLink）
    for connection in navbases_polygons_external_connections[current.poly.owner][current.poly.id]:
        _query_task_search_polygon_connections(connection, ...)
    
    //====== 步骤 2: 搜索限制检查 ======
    
    if processed_count >= path_search_max_polygons:
        traversable_polys.clear()  // 强制终止
    elif begin_point.distance²(current.entry) > max_distance²:
        traversable_polys.clear()
    
    //====== 步骤 3: 终止判断 ======
    
    if 开放列表为空:
        is_reachable = false
        if reachable_end == nullptr:
            break  // 完全不可达（起点是孤岛）
        
        // 回退到最近可达多边形作为新终点
        end_poly = reachable_end
        重新计算 end_point
        
        // 完全重置搜索，以新终点重新搜索
        reset all navigation_polys
        least_cost_id = begin_poly 的索引
        continue
    
    else:
        // 弹出 f 值最小的节点
        best = traversable_polys.pop()
        least_cost_id = poly_to_id[best.poly]
        
        // 跟踪最近可达多边形（防不可达时兜底）
        if best.entry 距 target 更近:
            reachable_end = best.poly
        
        // 到达终点？
        if best.poly == end_polygon:
            found_route = true
            break
        
        // 计算跨 Region 进入代价
        if owner 变了:
            poly_enter_cost = current.poly.owner.enter_cost
```

### 4.5 邻居松弛（Relax）

位置：`_query_task_search_polygon_connections()` (`nav_mesh_queries_3d.cpp` L279-317)

这是 A* 的核心——对每个邻居计算新代价并更新：

```
function search_polygon_connections(connection, least_cost_id, 
                                     least_cost_poly, poly_enter_cost, end_point):
    
    // 1. 前置检查：连接所属者是否可用
    if !is_connection_owner_usable(connection.polygon.owner):
        return   // 不可用（禁用、层不匹配、被 exclude）
    
    // 2. 计算入口点
    //    将当前节点的 entry 投影到 Portal 边 [pathway_start, pathway_end] 上
    new_entry = Geometry3D::get_closest_point_to_segment(
        least_cost_poly.entry,
        connection.pathway_start, connection.pathway_end)
    
    // 3. 计算新的 g 代价
    poly_travel_cost = least_cost_poly.poly.owner.travel_cost
    new_g = least_cost_poly.traveled_distance                    // 父节点的 g
          + dist(least_cost_poly.entry, new_entry) * poly_travel_cost  // 移动距离 × 权重
          + poly_enter_cost                                      // 跨 Region 进入代价
    
    // 4. 松弛
    neighbor = navigation_polys[poly_to_id[connection.polygon]]
    if new_g < neighbor.traveled_distance:           // 找到更短路径
        neighbor.back_navigation_poly_id = least_cost_id
        neighbor.back_navigation_edge = connection.edge
        neighbor.back_navigation_edge_pathway_start = connection.pathway_start
        neighbor.back_navigation_edge_pathway_end = connection.pathway_end
        neighbor.traveled_distance = new_g                           // 更新 g
        neighbor.distance_to_destination =                           // 更新 h
            dist(new_entry, end_point) * connection.polygon.owner.travel_cost
        neighbor.entry = new_entry
        
        if neighbor.traversable_poly_index != INVALID_INDEX:
            traversable_polys.shift(neighbor.traversable_poly_index) // decrease-key
        else:
            neighbor.poly = connection.polygon
            traversable_polys.push(&neighbor)                        // 首次入堆
```

**关键设计要点**：

| 要点 | 说明 |
|------|------|
| **入口点投影** | 不是走到 Portal 边中点，而是投影到最近点 → 路径更短 |
| **区域权重** | `travel_cost` 同时乘在 g 和 h 上；`enter_cost` 只加到 g 上 |
| **无显式 closed set** | `traveled_distance` 初始为 `FLT_MAX`，新代价更小才更新，天然避免重复展开 |
| **decrease-key** | 已在堆中的节点被松弛时用 `shift()` 更新位置，不需要重新入堆 |
| **启发函数一致性** | h = 欧氏距离 × travel_cost，当 travel_cost ≥ 1 时是 admissible 的 |

### 4.6 不可达回退机制

当终点不可达时（开放列表清空），搜索不会直接失败：

```
搜索过程中持续跟踪：
  reachable_end = 距目标最近的已展开多边形

开放列表清空时：
  1. is_reachable = false
  2. 将 reachable_end 作为新的 end_polygon
  3. 完全重置所有搜索状态
  4. 以新终点重新搜索（保证找到最优路径到替代终点）
```

> 最坏情况下搜索两遍。第一遍发现不可达，第二遍搜到最近可达点的最优路径。

---

## 5. 漏斗算法与路径后处理

A* 搜索的输出是一个**多边形通道（corridor）**——一系列相邻多边形的链。但玩家需要的是**路径点**，这就是后处理要做的事。

### 5.1 三种后处理模式

| 模式 | 函数位置 | 路径点来源 | 适用场景 |
|------|---------|-----------|---------|
| **CorridorFunnel**（默认） | L735 | 漏斗算法拐点 | 最短路径，大多数场景 |
| **EdgeCentered** | L830 | Portal 边中点 | 需要居中行走的场景 |
| **None** | L859 | A* 的 entry 投影点 | 调试或自定义后处理 |

### 5.2 漏斗算法详解

位置：`_query_task_post_process_corridorfunnel()` (`nav_mesh_queries_3d.cpp` L735-828)

漏斗算法（Simple Stupid Funnel Algorithm）在 Portal 边序列上维护一个"漏斗"：apex（尖端）+ 左边界 + 右边界。

**核心叉积宏** (L42):

```cpp
#define THREE_POINTS_CROSS_PRODUCT(a, b, c) (((c) - (a)).cross((b) - (a)))
```

`THREE_POINTS_CROSS_PRODUCT(apex, left, right).dot(map_up)` 判断从 apex 看 left 和 right 的方向：
- `> 0`：left 在 right 的左侧（正确的漏斗方向）
- `< 0`：left 在 right 的右侧（需要 swap）

**关键特点**：路径从**终点到起点反向构建**，最后整体 `path_reverse()`。

**完整流程**:

```
初始化（L735-766）：
  apex_point = end_point
  left_portal = apex_point
  right_portal = apex_point
  push(end_point)    // 第一个路径点 = 终点
  
  // 特殊处理：如果终点恰好在 Portal 边上，跳到上一个多边形
  if end_point ≈ closest_on_segment(end_point, portal_start, portal_end):
      apex_poly = 上一个多边形

主循环（L767-822）：
  从终点多边形沿 back_navigation_poly_id 反向遍历到起点
  
  for 每个多边形 p:
      left = p.back_navigation_edge_pathway_start
      right = p.back_navigation_edge_pathway_end
      
      // 确保左右方向正确
      if cross(apex→left, apex→right) · map_up < 0:
          swap(left, right)
      
      //=== 检查左边界 ===
      if left 在漏斗内（没越过左边界）:
          if left 没越过右边界:
              收窄左边界：left_portal = left
          else:
              // left 越过右边界 → right_portal 是拐点！
              clip_path(apex_poly → right_portal)
              push(right_portal)       // 添加拐点
              重置漏斗：apex = right_portal
      
      //=== 检查右边界 ===（对称逻辑）
      if right 在漏斗内:
          if right 没越过左边界:
              收窄右边界：right_portal = right
          else:
              // left_portal 是拐点
              clip_path(apex_poly → left_portal)
              push(left_portal)
              重置漏斗：apex = left_portal
  
  // 补上起点
  if 最后的路径点 != begin_point:
      push(begin_point)

path_reverse()  // 翻转为 起点→终点 顺序
```

**图示一次完整的漏斗收窄和拐点检测**:

```
初始状态：
     apex ●
    ╱      ╲
   ╱ 漏斗  ╲
  left    right

Portal 边来了新的 left'：

情况1：left' 在漏斗内 → 收窄
     apex ●
    ╱  ╱   ╲
   ╱  left'  ╲      ← 左边界收窄了
  left     right

情况2：left' 越过右边界 → right 是拐点
     apex ●
    ╱         ╲
   ╱    right ●────  ← 拐点！添加到路径
  left      left'╲   
            
  重置漏斗：apex = right，继续
```

### 5.3 clip_path 路径裁剪

位置：`_query_task_clip_path()` (`nav_mesh_queries_3d.cpp` L1246-1279)

当漏斗检测到拐点时，从上一个路径点到拐点之间可能跨越了多个 Portal 边。在 3D 地形中这些 Portal 边可能有高度差。`clip_path` 确保路径贴合地形：

```
function clip_path(from_poly, to_point, to_poly):
    from = path_points.back()   // 上一个路径点
    
    // 构造切割平面：法线 = (from - to_point) × map_up
    cut_plane.normal = (from - to_point).cross(map_up).normalized()
    cut_plane.d = cut_plane.normal.dot(from)
    
    // 从 from_poly 反向遍历到 to_poly
    while from_poly != to_poly:
        portal_start = from_poly.pathway_start
        portal_end = from_poly.pathway_end
        from_poly = navigation_polys[from_poly.back_id]
        
        // 切割平面与 Portal 边求交
        if cut_plane.intersects_segment(portal_start, portal_end, &intersection):
            if intersection != to_point && intersection != path_points.back():
                push(intersection)   // 添加中间路径点
```

**效果**：在平坦地形上 clip_path 通常不产生额外点。但在阶梯、坡道等高度变化的地形上，它会在 Portal 边与切割平面的交点处插入中间路径点，使路径不会"飘"在空中。

### 5.4 EdgeCentered 后处理

位置：L830-857

```
push(end_point)

从终点多边形反向遍历：
  if 有 back_navigation_edge:
      // 计算多边形上该边的中点
      edge_start = poly.vertices[edge]
      edge_end = poly.vertices[(edge+1) % vertex_count]
      push((edge_start + edge_end) / 2)
  else:
      push(entry)   // NavLink 连接无边索引，用 entry 点

push(begin_point)
```

路径经过每条 Portal 边的中点，远离墙壁。适合需要角色在通道中间行走的场景。

### 5.5 路径简化（Douglas-Peucker）

位置：`_query_task_simplified_path_points()` L693，核心在 L1332-1369

```
function simplify(start, end, points, epsilon):
    // 找 start→end 线段上距离最远的点
    max_dist = 0
    max_idx = 0
    for i in (start, end):
        d = distance²(points[i], closest_on_segment(points[i], points[start], points[end]))
        if d > max_dist:
            max_dist = d
            max_idx = i
    
    if max_dist > epsilon²:
        simplify(start, max_idx, ...)
        result.push(max_idx)
        simplify(max_idx, end, ...)
```

简化后保留关键拐点，删除近似共线的中间点。元数据（path_types、path_rids、path_owner_ids）同步重排。

### 5.6 路径长度/半径裁剪

位置：`_query_task_process_path_result_limits()` L607-691

```
if path_return_max_length > 0:
    // 从起点沿路径累积距离
    // 超过 max_length 时，在当前段上插值一个截断点
    // 删除后续所有点

if path_return_max_radius > 0:
    // 用以起点为圆心、max_radius 为半径的球体
    // 与每段路径线段求交
    // 交点作为新的末端点，删除后续
```

---

## 6. NavigationAgent3D 路径跟随

`NavigationAgent3D` 是面向用户的高层封装节点，封装了寻路请求、路径跟随、信号通知。

位置：`scene/3d/navigation/navigation_agent_3d.h/cpp`

### 6.1 路径请求触发

```cpp
// 用户设置目标
navigation_agent.set_target_position(Vector3(100, 0, 200))
    [navigation_agent_3d.cpp L709]
    └→ target_position_submitted = true
    └→ _request_repath()   // 清空旧路径状态

// 每帧获取下一个路径点
var next = navigation_agent.get_next_path_position()
    [L723]
    └→ _update_navigation()  // 内部自动重新寻路（如果需要）
    └→ return navigation_path[navigation_path_index]
```

### 6.2 _update_navigation() 核心流程

位置：`navigation_agent_3d.cpp` L803-884

```
_update_navigation():
│
├── 1. 前置检查
│      无父节点 / 不在树中 / 无目标 → 返回
│
├── 2. 获取当前世界位置 origin
│
├── 3. 判断是否需要重新寻路 (reload_path):
│      ├── 地图已变更 (agent_is_map_changed)
│      ├── 当前路径为空
│      └── 偏离路径超过 path_max_distance
│
├── 4. 重新查询路径
│      ├── 设置查询参数:
│      │   navigation_query.start_position = origin
│      │   navigation_query.target_position = target
│      │   navigation_query.navigation_layers = layers
│      │   navigation_query.map = map_rid
│      │   navigation_query.path_postprocessing = postprocessing
│      │   navigation_query.simplify_path = simplify
│      ├── NavigationServer3D::query_path(navigation_query, navigation_result)
│      ├── navigation_path_index = 0
│      └── emit_signal("path_changed")
│
├── 5. 到达目标检查
│      distance(origin, target) < target_desired_distance
│      → _advance_waypoints()
│      → emit_signal("target_reached")
│      → navigation_finished = true
│
└── 6. 推进路点
       _advance_waypoints(origin)
```

### 6.3 路点推进

位置：`_advance_waypoints()` L886-902

```cpp
void _advance_waypoints(const Vector3 &p_origin) {
    if (last_waypoint_reached) return;
    
    while (_is_within_waypoint_distance(p_origin)) {
        // origin 距当前路点 < path_desired_distance
        
        _trigger_waypoint_reached();
        // → emit_signal("waypoint_reached")
        // → 如果是 NavLink 路点 → emit_signal("link_reached")
        
        if (_is_last_waypoint()) {
            last_waypoint_reached = true;
            break;
        }
        
        _move_to_next_waypoint();
        // → navigation_path_index += 1
    }
}
```

### 6.4 关键属性

| 属性 | 默认值 | 含义 |
|------|--------|------|
| `path_desired_distance` | 1.0m | 路点到达判定距离 |
| `target_desired_distance` | 1.0m | 最终目标到达判定距离 |
| `path_max_distance` | 5.0m | 偏离路径触发重寻阈值 |
| `navigation_layers` | 1 | 导航层过滤掩码 |
| `path_postprocessing` | CorridorFunnel | 后处理模式 |
| `simplify_path` | false | 是否 Douglas-Peucker 简化 |
| `path_height_offset` | 0.0m | 路径点 Y 轴偏移 |
| `path_metadata_flags` | ALL | 路径元数据标志（types/rids/owner_ids） |

---

## 7. 完整调用链总结

从用户代码到最终路径点的完整调用链：

```
用户代码
  agent.set_target_position(pos)
  agent.get_next_path_position()
│
▼ scene/3d/navigation/navigation_agent_3d.cpp

NavigationAgent3D::get_next_path_position()                    [L723]
  └→ _update_navigation()                                      [L803]
      └→ NavigationServer3D::query_path(params, result)         [虚函数]
│
▼ servers/navigation_3d/godot_navigation_server_3d.cpp

GodotNavigationServer3D::query_path()                          [L1450]
  └→ NavMeshQueries3D::map_query_path()                        [L148]
│
▼ modules/navigation_3d/nav_map_3d.cpp

NavMap3D::query_path()                                         [L147]
  ├── semaphore.wait() + mutex.lock()                          // 获取查询槽
  ├── NavMeshQueries3D::query_task_map_iteration_get_path()    // 执行查询
  └── mutex.unlock() + semaphore.post()                        // 归还查询槽
│
▼ modules/navigation_3d/3d/nav_mesh_queries_3d.cpp

query_task_map_iteration_get_path()                            [L514]
│
├── _query_task_find_start_end_positions()                     [L237]
│   遍历所有多边形，fan 三角化，找最近点
│   输出：begin_polygon/point, end_polygon/point
│
├── _query_task_build_path_corridor()                          [L319]
│   A* 搜索主循环：
│   ├── 初始化 path_corridor, 开放列表, 起点
│   └── while (true):
│       ├── 展开内部/外部邻居连接
│       │   └→ _query_task_search_polygon_connections()        [L279]
│       │       入口投影到 Portal 边
│       │       new_g = g + dist * travel_cost + enter_cost
│       │       new_h = dist_to_end * travel_cost
│       │       if new_g < neighbor.g → 松弛（入堆或 decrease-key）
│       ├── 搜索限制检查
│       ├── 开放列表空 → 不可达回退
│       └── 弹出最小 f 节点，到达终点则 break
│   输出：least_cost_id（终点节点索引），back 链路径走廊
│
├── 路径后处理（以默认 CorridorFunnel 为例）
│   _query_task_post_process_corridorfunnel()                  [L735]
│   从终点沿 back 链反向遍历：
│   ├── 维护 apex + 左右 Portal 边界
│   ├── 叉积判断边界收窄/拐点
│   ├── 拐点处调用 _query_task_clip_path()                     [L1246]
│   │   切割平面与中间 Portal 边求交 → 贴合 3D 地形
│   └── push 路径点（反向，终点→起点）
│
├── path_reverse()                                             // 翻转为起点→终点
│
├── [可选] _query_task_simplified_path_points()                [L693]
│   Douglas-Peucker 路径简化
│
└── _query_task_process_path_result_limits()                   [L607]
    路径长度/半径裁剪
│
▼ 返回到 NavigationAgent3D

navigation_result 包含最终路径点
navigation_path_index = 0
emit_signal("path_changed")

每帧：
  get_next_path_position() → 返回 path[index]
  _advance_waypoints() → 距当前路点 < desired_distance 时 index++
  到达最后路点 → emit "target_reached"
```

### 数据流总结

```
NavigationMesh (资源)
  ↓ 烘焙 (Recast 12 步)
vertices + polygon_indices
  ↓ Region 构建 (3 步)
Polygon[] + internal_connections + external_edges
  ↓ Map 构建 (5 步)
全局连接图 (internal + external + margin + navlink)
  ↓ 预分配 PathQuerySlot
path_corridor + poly_to_id + heap
  ↓ A* 搜索
多边形通道 (back 链)
  ↓ 漏斗算法
路径点序列
  ↓ 简化 + 裁剪
最终路径 → NavigationAgent3D → 用户
```
