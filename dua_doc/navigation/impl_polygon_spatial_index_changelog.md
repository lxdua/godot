# BVH 空间索引 —— 落地过程文档

> **目标**：为 Godot 导航系统的多边形查询引入 BVH 空间索引，将起终点查找等操作从 O(N) 暴力遍历优化为 O(log N)。

---

## 涉及的类/结构体说明

| 类/结构体 | 所在文件 | 职责 |
|-----------|---------|------|
| `NavMap3D` | `nav_map_3d.h/cpp` | 导航地图的核心管理器。持有所有 Region、Link、Agent、Obstacle 的引用，负责每帧 `sync()`（检测脏标记、触发重建、切换双缓冲槽） |
| `NavMapIteration3D` | `nav_map_iteration_3d.h` | 地图的 **只读快照**（双缓冲的一个槽）。存储某一时刻所有多边形、连接关系、查询槽等数据，供路径查询线程安全读取，不需要加锁 |
| `NavMapIterationBuild3D` | `nav_map_iteration_3d.h` | 地图重建时的 **临时工作区**。收集脏数据、边连接对、多边形计数等中间结果，重建完成后写入 `NavMapIteration3D` |
| `NavMapBuilder3D` | `nav_map_builder_3d.h/cpp` | **地图构建器**（全部是静态方法）。负责执行重建的 5 个步骤：收集多边形 → 边配对 → 边合并 → margin 连接 → navlink 连接，最后调用 `_build_update_map_iteration()` 将结果写入快照 |
| `NavMeshQueries3D` | `nav_mesh_queries_3d.h/cpp` | **路径查询引擎**（全部是静态方法）。提供 A* 寻路、漏斗算法后处理、最近点查询、线段查询等所有运行时查询功能，从 `NavMapIteration3D` 中读取数据 |
| `NavRegionIteration3D` | `nav_region_iteration_3d.h` | 单个导航区域（Region）的 **只读快照**。存储该区域的多边形数组、变换矩阵、边界 AABB、外部边等 |
| `NavBaseIteration3D` | `nav_base_iteration_3d.h` | Region 和 Link 的公共基类。持有 `navmesh_polygons`（多边形数组）、`navigation_layers`（层掩码）、`enter_cost` / `travel_cost`（寻路代价）等共享属性 |
| `Nav3D::Polygon` | `nav_utils_3d.h` | 导航多边形。包含 `id`（在所属 Region 内的编号）、`owner`（指向所属的 `NavBaseIteration3D`）、`vertices`（顶点数组）、`surface_area`（面积） |
| `Nav3D::Connection` | `nav_utils_3d.h` | 多边形之间的连接。记录目标多边形指针、边索引、通行路径的起止点（`pathway_start` / `pathway_end`），用于 A* 搜索时的邻居遍历 |
| `DynamicBVH` | `core/math/dynamic_bvh.h/cpp` | Godot 引擎的通用 **动态包围盒层次树**（基于 Bullet 的 Dbvt）。支持 insert/remove/update，提供 AABB/凸体/射线查询，模板化回调 |
| `NavMeshPathQueryTask3D` | `nav_mesh_queries_3d.h` | 一次路径查询的 **任务描述**。包含起终点坐标、navigation_layers 过滤条件、查询结果（begin/end polygon 和 position）等 |
| `PathQuerySlot` | `nav_mesh_queries_3d.h` | A* 寻路的 **预分配工作槽**。每个槽包含 path_corridor（路径走廊数组）、traversable_polys（开放列表堆）、poly_to_id（多边形→索引映射），避免查询时动态分配内存 |

---

## 改动总览

| 序号 | 文件 | 改动类型 | 说明 |
|------|------|---------|------|
| 1 | `modules/navigation_3d/3d/nav_map_iteration_3d.h` | 修改 | 在 `NavMapIteration3D` 中新增 `DynamicBVH polygon_bvh` 和 `polygon_bvh_data` 成员 |
| 2 | `modules/navigation_3d/3d/nav_map_builder_3d.cpp` | 修改 | 新增 `_polygon_to_aabb()` 辅助函数；在 `_build_update_map_iteration()` 末尾构建 BVH |
| 3 | `modules/navigation_3d/3d/nav_mesh_queries_3d.h` | 修改 | 新增 `_find_closest_polygon_to_point()` 函数声明 |
| 4 | `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | 修改 | 新增 `_find_closest_polygon_to_point()` 实现；重写 `_query_task_find_start_end_positions()`；BVH 加速 `map_iteration_get_closest_point_info()` 和 `map_iteration_get_closest_point_to_segment()` |

---

## Step 1：NavMapIteration3D 新增 BVH 成员

**文件**：`modules/navigation_3d/3d/nav_map_iteration_3d.h`

**改动内容**：
- `#include "core/math/dynamic_bvh.h"`
- 在 `NavMapIteration3D` 结构体中新增：
  - `DynamicBVH polygon_bvh` —— BVH 树实例
  - `LocalVector<const Nav3D::Polygon *> polygon_bvh_data` —— BVH 叶子 userdata 索引到多边形指针的映射数组
- 在 `clear()` 方法中新增：
  - `polygon_bvh.clear()`
  - `polygon_bvh_data.clear()`

**设计决策**：
- 使用 `LocalVector<const Polygon *>` 而非 `HashMap`，因为多边形 ID 是连续分配的（0, 1, 2, ...），数组直接索引比哈希查找更快
- BVH 的 `void *p_userdata` 存储多边形在 `polygon_bvh_data` 数组中的索引（转为 `uintptr_t`）

---

## Step 2：构建 BVH

**文件**：`modules/navigation_3d/3d/nav_map_builder_3d.cpp`

**改动位置**：`_build_update_map_iteration()` 函数末尾（约第 430 行）

**改动内容**：
在 path_query_slots 设置完成后，遍历所有多边形，为每个多边形计算 AABB 并插入 BVH，最后调用 `optimize_top_down()` 优化树结构。

**伪代码**：
```cpp
// 1. 清空旧 BVH
map_iteration->polygon_bvh.clear();
map_iteration->polygon_bvh_data.clear();
map_iteration->polygon_bvh_data.reserve(total_polygon_count);

// 2. 遍历所有 region 的多边形
for (region : map_iteration->region_iterations) {
    for (polygon : region->navmesh_polygons) {
        // 2a. 计算多边形 AABB
        AABB poly_aabb = compute_polygon_aabb(polygon.vertices);
        // 2b. 记录多边形指针
        uint32_t idx = polygon_bvh_data.size();
        polygon_bvh_data.push_back(&polygon);
        // 2c. 插入 BVH，userdata = 索引
        polygon_bvh.insert(poly_aabb, (void *)(uintptr_t)idx);
    }
}

// 3. 遍历 navlink 多边形（同理）
for (polygon : map_iteration->navlink_polygons) { ... }

// 4. 优化树结构
map_iteration->polygon_bvh.optimize_top_down();
```

---

## Step 3：用 BVH 加速查询

### 新增函数：`_find_closest_polygon_to_point()`

**文件**：`modules/navigation_3d/3d/nav_mesh_queries_3d.h/cpp`

独立的通用查询函数，给定一个点，返回 NavMesh 上离它最近的多边形。

```cpp
static const Nav3D::Polygon *_find_closest_polygon_to_point(
    const NavMapIteration3D &p_map_iteration,  // 地图快照（含 BVH）
    const NavMeshPathQueryTask3D &p_query_task, // 用于 enabled/layers/exclude 过滤
    const Vector3 &p_point,                     // 查询点
    Vector3 &r_closest_point                    // 输出：多边形上最近的点
);
// 返回值：最近的多边形指针，未找到时返回 nullptr
```

**设计决策**：
- 用返回值而非 void + 引用参数，因为这是独立工具函数而非 `_query_task_*` 流水线的一环
- 每次查询以查询点为中心独立构造搜索 AABB，不和其他点混在一起
- Refinement 用自己的 `closest_d`，精确到自身需要的范围

**查询流程**：
1. 以 `p_point` 为中心、初始半径 1.0 构造搜索 AABB
2. BVH `aabb_query()` 返回候选多边形
3. 对每个候选做 `_query_task_is_connection_owner_usable()` 过滤（enabled / layers / exclude / include）
4. 对通过过滤的候选做 `Face3::get_closest_point_to()` 精确距离计算
5. 若无候选，扩大搜索半径（×4）重试，最大 10000
6. 找到候选后做 Refinement：用 `closest_d` 作为精确半径再查一轮，确保全局最优
7. 输出最近多边形指针和投影点坐标

### 3a. `_query_task_find_start_end_positions()`

**文件**：`modules/navigation_3d/3d/nav_mesh_queries_3d.cpp`

重构后简化为两次独立调用 `_find_closest_polygon_to_point()`：

```cpp
void _query_task_find_start_end_positions(task, map_iteration) {
    task.begin_polygon = _find_closest_polygon_to_point(map_iteration, task,
                              task.start_position, task.begin_position);
    task.end_polygon   = _find_closest_polygon_to_point(map_iteration, task,
                              task.target_position, task.end_position);
}
```

起点和终点各自独立搜索，互不影响，Refinement 精度各自独立。

### 3b. `map_iteration_get_closest_point_info()`

**文件**：`modules/navigation_3d/3d/nav_mesh_queries_3d.cpp`

**策略**：渐进式 BVH AABB 查询 + Refinement。回调内保留原版的凸多边形点-边距离计算逻辑（inside/outside 分支）。当点恰好在多边形表面时（distance ≈ 0），回调返回 `true` 提前终止搜索。

### 3c. `map_iteration_get_closest_point_to_segment()`

**文件**：`modules/navigation_3d/3d/nav_mesh_queries_3d.cpp`

**策略**：
1. 用线段（`p_from` + `p_to`）的 AABB 加搜索边距构造查询范围
2. BVH 返回候选后，保留原版的三种距离计算：面交点、端点到面距离、边到线段距离

### 辅助函数：`_polygon_to_aabb()`

**文件**：`modules/navigation_3d/3d/nav_map_builder_3d.cpp`（文件顶部 static 函数）

```cpp
static AABB _polygon_to_aabb(const Polygon &p_polygon);
```

从多边形顶点计算 AABB，返回值加 `.grow(CMP_EPSILON)` 防止零厚度（NavLink 合成多边形等退化情况）。含 `DEV_ASSERT(p_polygon.vertices.size() >= 1)` 防空顶点。放在 builder 文件内作为文件级 static 函数，因为目前只有构建时使用。

---

## 改动记录

| 时间 | 步骤 | 状态 | 备注 |
|------|------|------|------|
| 2026-04-20 | Step 1: NavMapIteration3D 新增 BVH 成员 | ✅ 完成 | 新增 `polygon_bvh` + `polygon_bvh_data`，`clear()` 中清理 |
| 2026-04-20 | Step 2: 构建 BVH | ✅ 完成 | 在 `_build_update_map_iteration()` 末尾遍历所有多边形插入 BVH + `optimize_top_down()` |
| 2026-04-20 | Step 3a: 加速 _query_task_find_start_end_positions | ✅ 完成 | 渐进式扩展 AABB 搜索 + BVH 回调过滤 + navigation_layers 保留 |
| 2026-04-20 | Step 3b: 加速 map_iteration_get_closest_point_info | ✅ 完成 | 同 3a 策略，保留完整的点-多边形距离计算逻辑 |
| 2026-04-20 | Step 3c: 加速 map_iteration_get_closest_point_to_segment | ✅ 完成 | 线段 AABB + 渐进扩展 + 保留 face intersection / edge distance 逻辑 |
| 2026-04-20 | Review 修复 #8: 恢复完整过滤条件 | ✅ 完成 | BVH 回调中用 `_query_task_is_connection_owner_usable()` 替代仅 navigation_layers 检查，恢复 enabled / exclude / include 过滤 |
| 2026-04-20 | Review 修复 #3: 退化 AABB 防护 | ✅ 完成 | `_polygon_to_aabb()` 返回值加 `.grow(CMP_EPSILON)`，防止零厚度 AABB；加 `DEV_ASSERT` 防空顶点 |
| 2026-04-20 | Review 修复 #9: 渐进搜索精确化 | ✅ 完成 | 三个函数找到候选后均增加 refinement pass，用已找到的最短距离重新查询确保全局最优 |
| 2026-04-20 | Review 修复 #2-2: closest_point_info early return | ✅ 完成 | 点恰好在多边形表面时（distance==0）返回 true 提前终止搜索 |
| 2026-04-20 | 重构: 抽取 `_find_closest_polygon_to_point` | ✅ 完成 | 将"找某点最近多边形"拆为独立函数，`_query_task_find_start_end_positions` 改为两次独立调用，Refinement 精度从 `MAX(begin_d, end_d)` 优化为各自独立的 `closest_d` |

---

## 技术细节备忘

### BVH userdata 映射

- `DynamicBVH::insert()` 的 `void *p_userdata` 存储的是 `polygon_bvh_data` 数组的索引（`uint32_t` 转为 `uintptr_t`）
- 回调中通过 `(uint32_t)(uintptr_t)p_data` 还原索引，再从 `polygon_bvh_data[idx]` 取多边形指针

### 渐进式搜索半径

所有 BVH 查询都采用了渐进式扩展策略：
- 初始半径：1.0 单位
- 增长因子：4.0x
- 最大半径：10000.0 单位
- 如果初始半径内有候选 → 一次查询即完成（绝大多数情况）
- 如果查询点离 NavMesh 很远 → 最多约 7 次迭代（1 → 4 → 16 → 64 → 256 → 1024 → 4096 → 10000）

### const_cast 说明

`DynamicBVH::aabb_query()` 是非 const 方法（模板参数无法标记 const），但实际上不修改树结构。
由于 `NavMapIteration3D` 在查询路径中是 const 引用传入的，需要 `const_cast` 来调用 `aabb_query()`。
这是安全的，因为 `aabb_query()` 只读遍历树节点。

### 未修改的 polygons_get_* 函数

`polygons_get_closest_point_to_segment()`、`polygons_get_closest_point_info()` 等以 `polygons_` 开头的函数
接受的是 `LocalVector<Polygon>` 参数（单个 Region 的多边形数组），不是全地图查询，数据量小。
这些函数暂不需要 BVH 加速，保持原有的 O(N) 遍历即可。

---

## 已知限制

| 编号 | 描述 | 影响 | 优先级 |
|------|------|------|--------|
| 1 | `DynamicBVH` 无拷贝构造函数，`NavMapIteration3D` 如被意外拷贝会导致双重释放 | 当前 ping-pong 不会拷贝，暂安全。未来可对 `NavMapIteration3D` 禁用拷贝 | LOW |
| 2 | `DynamicBVH::aabb_query()` 非 const，需 `const_cast` 调用 | 安全但不优雅。需改动 `core/math/dynamic_bvh.h` 核心头文件，影响范围超出导航模块 | LOW |
| 3 | `SEARCH_RADIUS_MAX = 10000.0` 硬编码 | 超大世界可能不够。可后续改为从地图 AABB 计算或暴露为配置项 | LOW |
| 4 | `_find_closest_polygon_to_point` 未找到时 `r_closest_point` 被设为默认值 `(0,0,0)` | 调用方收到 `nullptr` 后不会使用该值，暂安全 | LOW |

---

## TODO：将 BVH 搜索硬编码常量暴露为地图参数

### 背景

当前 `_find_closest_polygon_to_point()` 以及其他 BVH 查询函数中有 3 个硬编码常量：

| 常量 | 当前值 | 含义 |
|------|--------|------|
| `search_radius`（初始值） | `1.0` | 渐进式搜索的起始 AABB 半径（单位：世界坐标） |
| `SEARCH_RADIUS_MAX` | `10000.0` | 搜索半径的上限，超过后放弃搜索并返回 nullptr |
| `SEARCH_RADIUS_GROW_FACTOR` | `4.0` | 每次未命中时搜索半径的倍增因子 |

这三个值只影响 **性能**（搜索几次 BVH 才能命中候选），不影响 **正确性**（最终找到的多边形一定是全局最近的）。但在不同规模的项目中，最优值可能不同：

- **小场景**（室内、关卡）：默认 `1.0` 起始半径足够，几乎总是一次命中
- **超大世界**（开放世界、太空）：`SEARCH_RADIUS_MAX = 10000.0` 可能不够，导致远处查询返回 nullptr
- **稀疏 NavMesh**（大量空白区域）：`SEARCH_RADIUS_GROW_FACTOR = 4.0` 可能太保守，增大到 `8.0` 或 `16.0` 可减少迭代次数

### 计划

将这 3 个常量作为 `NavigationServer3D` 的**地图级参数**暴露给用户，类似现有的 `map_set_cell_size()` / `map_set_edge_connection_margin()` 等接口。

#### 新增 API

```cpp
// NavigationServer3D
void map_set_bvh_search_radius_initial(RID p_map, real_t p_radius);     // 默认 1.0
real_t map_get_bvh_search_radius_initial(RID p_map) const;

void map_set_bvh_search_radius_max(RID p_map, real_t p_radius);         // 默认 10000.0
real_t map_get_bvh_search_radius_max(RID p_map) const;

void map_set_bvh_search_radius_grow_factor(RID p_map, real_t p_factor); // 默认 4.0
real_t map_get_bvh_search_radius_grow_factor(RID p_map) const;
```

#### 涉及的修改文件

| 文件 | 改动 |
|------|------|
| `servers/navigation_server_3d.h/cpp` | 新增 3 对 getter/setter 虚方法 + `ClassDB::bind_method` |
| `modules/navigation_3d/3d/nav_map_3d.h/cpp` | `NavMap3D` 新增 3 个成员变量 + getter/setter 实现 |
| `modules/navigation_3d/3d/nav_map_iteration_3d.h` | `NavMapIteration3D` 新增 3 个只读字段（从 NavMap3D 同步过来） |
| `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | 将 `_find_closest_polygon_to_point()` 等函数中的硬编码替换为从 `p_map_iteration` 读取的参数 |
| `doc/classes/NavigationServer3D.xml` | 新增 API 文档 |

#### 注意事项

- 参数值在 `sync()` 时从 `NavMap3D` 复制到 `NavMapIteration3D` 快照中，查询时从快照读取，保证线程安全
- 需要加合法性校验：`initial > 0`、`max >= initial`、`grow_factor > 1.0`
- 可考虑在编辑器 Inspector 中显示这些参数（通过 `NavigationServer3D` 的 ProjectSettings 或 Map 属性）

---

## Review 总结

共进行 3 轮 Review，发现并修复 5 个问题：

| 轮次 | 发现 | 修复 |
|------|------|------|
| 第 1 轮 | #8 丢失 enabled/exclude/include 过滤（HIGH）、#3 退化 AABB（MEDIUM）、#9 Refinement 可能遗漏最优（MEDIUM） | 全部修复 |
| 第 2 轮 | #2-2 `closest_point_info` 丢失 early break 优化、Refinement AABB 起终点联合导致偏大 | 修复 early break；联合 AABB 问题通过重构拆分函数彻底解决 |
| 第 3 轮 | 无功能性 bug | 确认所有改动正确，更新文档 |
