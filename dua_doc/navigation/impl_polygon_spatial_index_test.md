# BVH 空间索引 —— 测试文档

> **目标**：验证 BVH 空间索引的正确性、鲁棒性和性能优势，确保与原始 O(N) 暴力查询产生一致的结果。

---

## 测试范围

| 模块 | 文件 | 测试内容 |
|------|------|---------|
| AABB 计算 | `nav_map_builder_3d.cpp` | `_polygon_to_aabb()` 辅助函数 |
| BVH 构建 | `nav_map_builder_3d.cpp` | `_build_update_map_iteration()` 中的 BVH 构建循环 |
| 通用查询 | `nav_mesh_queries_3d.cpp` | `_find_closest_polygon_to_point()` |
| 起终点查找 | `nav_mesh_queries_3d.cpp` | `_query_task_find_start_end_positions()` |
| 最近点信息 | `nav_mesh_queries_3d.cpp` | `map_iteration_get_closest_point_info()` |
| 线段查询 | `nav_mesh_queries_3d.cpp` | `map_iteration_get_closest_point_to_segment()` |

---

## 1. AABB 计算测试（`_polygon_to_aabb`）

### 1.1 正常三角形多边形

**场景**：一个顶点为 `(0,0,0), (10,0,0), (5,5,0)` 的三角形。

**预期**：
- AABB min = `(-ε, -ε, -ε)`
- AABB max = `(10+ε, 5+ε, ε)`
- 所有三个顶点都被包含在 AABB 内

**验证方法**：构建包含该三角形的 Region，触发 `sync()`，检查 BVH 的叶节点 AABB 是否与预期一致。

### 1.2 退化多边形 —— NavLink 合成（两个重合顶点）

**场景**：NavLink 的合成多边形 `vertices = [P, P, P]`（三个顶点完全重合于同一个点 `(5, 3, 7)`）。

**预期**：
- 原始 AABB 的 size 为 `(0, 0, 0)`
- `.grow(CMP_EPSILON)` 后 AABB 的 size > 0，宽高深各约 `2 * CMP_EPSILON`
- BVH 可以查询到该叶节点

**关键点**：如果没有 `.grow(CMP_EPSILON)`，零厚度 AABB 会导致 BVH 的 AABB 相交测试失败，该多边形永远不会被任何查询命中。

### 1.3 水平平面多边形（零厚度 Y 轴）

**场景**：一个顶点全在 Y=5 平面上的多边形 `(0,5,0), (10,5,0), (10,5,10), (0,5,10)`。

**预期**：
- Y 轴方向的 AABB 厚度为 `2 * CMP_EPSILON`（而非 0）
- 查询点 `(5, 5, 5)` 或 `(5, 5.001, 5)` 均可命中该多边形

**关键点**：水平地面是 NavMesh 最常见的情况，Y 轴零厚度必须被正确处理。

### 1.4 顶点数 < 3 的退化多边形

**场景**：一个 `vertices.size() == 2` 的多边形（边而非面）。

**预期**：
- 该多边形被 BVH 构建循环的 `if (polygon.vertices.size() < 3) continue;` 跳过
- `polygon_bvh_data` 中不包含该多边形的指针
- `polygon_bvh` 的叶节点数 = 总多边形数 - 跳过的退化多边形数

---

## 2. BVH 构建测试

### 2.1 空地图

**场景**：没有任何 Region 或 Link 的空导航地图。

**预期**：
- `polygon_bvh.is_empty() == true`
- `polygon_bvh_data.size() == 0`
- 所有查询函数返回 `nullptr`（或等效的"未找到"结果）

### 2.2 单 Region 单多边形

**场景**：一个 Region 包含一个三角形。

**预期**：
- `polygon_bvh_data.size() == 1`
- `polygon_bvh_data[0]` 指向该三角形
- BVH 叶节点数 = 1

### 2.3 多 Region 多多边形

**场景**：3 个 Region，分别包含 100、200、50 个有效多边形（vertices.size() >= 3）。

**预期**：
- `polygon_bvh_data.size() == 350`
- `polygon_bvh_data` 中先排列 Region 1 的多边形，再排列 Region 2 的，最后 Region 3 的（按 `region_iterations` 遍历顺序）
- BVH 叶节点数 = 350

### 2.4 Region + NavLink 混合

**场景**：2 个 Region（共 10 个多边形）+ 3 个 NavLink（共 3 个合成多边形）。

**预期**：
- `polygon_bvh_data.size() == 13`
- 前 10 个指针来自 Region 多边形，后 3 个来自 NavLink 多边形
- NavLink 的合成多边形也被成功插入 BVH（归功于 `.grow(CMP_EPSILON)`）

### 2.5 重建（双缓冲切换）

**场景**：
1. 初始状态有 2 个 Region（20 个多边形）
2. 删除 1 个 Region
3. 触发 `sync()` → 重建 BVH

**预期**：
- 重建后 `polygon_bvh_data.size() == 10`（仅保留剩余 Region 的多边形）
- 旧的 BVH 叶节点被 `polygon_bvh.clear()` 完全清除
- 查询不再命中已删除 Region 的多边形

### 2.6 `optimize_top_down()` 调用

**场景**：插入 500 个多边形后调用 `optimize_top_down()`。

**预期**：
- 树的高度 ≤ 理论最大值 O(log N)（N=500 → 高度约 ≤ 15）
- 优化不改变查询结果，只改善查询性能
- `polygon_bvh.is_empty()` 为 `false` 时才调用 `optimize_top_down()`

---

## 3. `_find_closest_polygon_to_point()` 查询测试

### 3.1 点在多边形表面上

**场景**：一个水平三角形 `(0,0,0), (10,0,0), (5,10,0)`，查询点 `(5, 0, 3)`。

**预期**：
- 返回该三角形
- `r_closest_point == (5, 0, 3)`（点本身就在表面上）
- 距离 = 0

### 3.2 点在多边形正上方

**场景**：同上三角形，查询点 `(5, 10, 3)`（在三角形正上方 10 个单位）。

**预期**：
- 返回该三角形
- `r_closest_point == (5, 0, 3)`（投影到表面）
- 距离 = 10.0

### 3.3 点在多边形边缘外侧

**场景**：同上三角形，查询点 `(-1, 0, 0)`（在三角形外侧、靠近边缘）。

**预期**：
- 返回该三角形
- `r_closest_point` 在三角形的边上（接近 `(0, 0, 0)` 顶点）
- 距离 > 0

### 3.4 多个多边形竞争

**场景**：两个不相邻的三角形：
- 三角形 A：`(0,0,0), (10,0,0), (5,5,0)` 
- 三角形 B：`(100,0,0), (110,0,0), (105,5,0)` 
- 查询点：`(7, 0, 0)`

**预期**：
- 返回三角形 A（距离 = 0，点在 A 内部）
- 三角形 B 不在初始搜索 AABB（半径 1.0）内，不会被错误选为结果

### 3.5 Refinement Pass 验证

**场景**（关键场景，验证 Refinement 的必要性）：

设置两个多边形：
- 多边形 A：AABB 较大，在初始搜索 AABB 内，但多边形本体距离查询点较远（如距离 = 5.0）
- 多边形 B：AABB 的中心在初始搜索 AABB 外，但多边形本体上有一个角距离查询点更近（如距离 = 4.0）

**分析**：
- 初始搜索（半径 1.0）可能不命中任何多边形 → 扩展到半径 4.0
- 半径 4.0 可能只命中 A 的 AABB → `closest_d = 5.0`
- **Refinement**：用半径 `5.0 + CMP_EPSILON` 重新查询 → 命中 B 的 AABB → 发现 B 更近 → 更新 `closest_d = 4.0`

**预期**：最终返回多边形 B，`closest_d = 4.0`。

**无 Refinement 时的错误**：会错误返回多边形 A（`closest_d = 5.0`），不是全局最近。

### 3.6 渐进式搜索扩展

**场景**：查询点 `(0, 0, 0)`，最近的多边形在 `(50, 0, 50)` 附近。

**预期**：
- 初始半径 1.0 → 无命中
- 第 2 次：半径 4.0 → 无命中
- 第 3 次：半径 16.0 → 无命中
- 第 4 次：半径 64.0 → 命中
- 总共查询 4 次 BVH（+ 1 次 Refinement = 5 次）

### 3.7 搜索半径上限

**场景**：查询点 `(0, 0, 0)`，最近的多边形在 `(20000, 0, 0)`（距离 > 10000）。

**预期**：
- 搜索半径从 1.0 增长到超过 `SEARCH_RADIUS_MAX (10000.0)` 后终止
- 返回 `nullptr`
- `r_closest_point` 保持默认值（`Vector3()`，即 `(0,0,0)`）

### 3.8 空 BVH 查询

**场景**：`polygon_bvh.is_empty() == true`，调用 `_find_closest_polygon_to_point()`。

**预期**：
- 直接返回 `nullptr`
- 不进入渐进式搜索循环

---

## 4. 过滤条件测试

### 4.1 Disabled Region 过滤

**场景**：
- Region A（enabled = true）：多边形在 `(0,0,0)` 附近
- Region B（enabled = false）：多边形在查询点 `(5,0,5)` 最近的位置

**预期**：
- BVH 的 AABB 查询会命中两个 Region 的多边形（BVH 不存储 enabled 状态）
- 回调内 `_query_task_is_connection_owner_usable()` 过滤掉 B
- 最终返回 Region A 的多边形

### 4.2 Navigation Layers 过滤

**场景**：
- 查询任务的 `navigation_layers = 0b0001`
- Region A：`navigation_layers = 0b0001`（匹配）
- Region B：`navigation_layers = 0b0010`（不匹配），但更近

**预期**：
- Region B 的多边形被过滤，返回 Region A 的多边形

### 4.3 Exclude Regions 过滤

**场景**：
- 查询任务的 `exclude_regions` 包含 Region B 的 RID
- Region B 的多边形最近

**预期**：
- Region B 被排除，返回其他 Region 的多边形

### 4.4 所有多边形被过滤

**场景**：只有一个 Region（enabled = false），查询点在其多边形附近。

**预期**：
- BVH 命中候选多边形，但全部被过滤
- `closest_d` 保持 `FLT_MAX`
- 搜索半径扩展至超过 `SEARCH_RADIUS_MAX`
- 返回 `nullptr`

---

## 5. `_query_task_find_start_end_positions()` 集成测试

### 5.1 起终点在同一多边形

**场景**：起点 `(3,0,3)` 和终点 `(7,0,3)` 都在同一个三角形内。

**预期**：
- `begin_polygon == end_polygon`
- `begin_position == start_position`（点在表面上）
- `end_position == target_position`
- 路径为直线

### 5.2 起终点在不同多边形

**场景**：起点在多边形 A 上，终点在多边形 B 上，A 和 B 之间有连接。

**预期**：
- `begin_polygon != end_polygon`
- `begin_polygon` 是 A
- `end_polygon` 是 B
- 后续 A* 搜索可以找到从 A 到 B 的路径

### 5.3 起点有效、终点无效

**场景**：起点在 NavMesh 上，终点距离 NavMesh > 10000 单位。

**预期**：
- `begin_polygon != nullptr`
- `end_polygon == nullptr`
- 路径查询应返回失败状态

### 5.4 独立 Refinement 精度验证

**场景**：
- 起点非常接近最近多边形（距离 = 0.1）
- 终点距离较远（距离 = 50.0）

**预期**：
- 起点的 Refinement AABB 半径 ≈ 0.1（非常紧凑）
- 终点的 Refinement AABB 半径 ≈ 50.0（较大）
- 两者互不影响，各自产生精确结果

**对比旧实现**：旧实现使用 `MAX(begin_d, end_d)` 作为统一半径，起点的 Refinement 会被迫使用 50.0 的大半径，遍历更多无关节点。

---

## 6. `map_iteration_get_closest_point_info()` 测试

### 6.1 点在多边形内部（early return）

**场景**：查询点恰好在多边形表面上。

**预期**：
- 距离 ≈ 0
- 回调返回 `true`（提前终止 BVH 遍历）
- 性能：仅检查 1 个多边形

### 6.2 点在多边形外部

**场景**：查询点在多边形边缘外。

**预期**：
- 返回边上最近点
- 返回该多边形的 owner（NavBaseIteration3D 指针）

### 6.3 多边形法线方向验证

**场景**：查询一个倾斜的多边形，验证 `closest_point_normal` 的方向。

**预期**：
- 法线方向是多边形的 `Face3::get_plane().normal` 再 `.normalized()`
- 法线长度 = 1.0

---

## 7. `map_iteration_get_closest_point_to_segment()` 测试

### 7.1 线段穿过多边形

**场景**：一条竖直线段 `(5, -10, 5)` → `(5, 10, 5)` 穿过水平三角形 `(0,0,0), (10,0,0), (5,10,0)`。

**预期**：
- 返回交点（如 `(5, 0, 5)`，如果该点在三角形内）
- `use_collision == true` 时，返回面交点
- 距离 = 0（线段与面相交）

### 7.2 线段平行于多边形

**场景**：水平线段 `(0, 5, 0)` → `(10, 5, 0)` 在三角形正上方 5 个单位。

**预期**：
- 返回线段上离三角形最近的点 → 三角形上离该点最近的点
- 距离 ≈ 5.0

### 7.3 线段远离多边形

**场景**：线段在 `(1000, 0, 0)` 附近，多边形在原点附近。

**预期**：
- 渐进式搜索在多次扩展后找到最近多边形
- 返回正确的最近点对

---

## 8. 与原始 O(N) 实现的一致性测试

> 这是最重要的一组测试 —— 确保 BVH 加速不改变查询结果。

### 8.1 方法

对于每个测试场景：
1. 使用 BVH 加速的新实现运行查询
2. 使用暴力遍历的原始实现运行相同查询
3. 对比结果

### 8.2 对比项

| 对比项 | 精度要求 |
|--------|---------|
| 返回的多边形指针 | 完全一致 |
| `r_closest_point` 坐标 | `Math::is_equal_approx()` 级别 |
| `closest_distance` | `Math::is_equal_approx()` 级别 |
| `closest_point_normal` | `Math::is_equal_approx()` 级别 |
| `closest_point_owner` | 完全一致 |

### 8.3 测试场景列表

使用随机化策略：

```
for each test_iteration in [1..1000]:
    1. 随机生成 10~500 个多边形（随机位置、随机大小、随机形状）
    2. 随机设置 10%~30% 的 Region 为 disabled
    3. 随机设置 navigation_layers
    4. 随机生成查询点
    5. 分别用 BVH 和暴力遍历求解
    6. assert 结果一致
```

### 8.4 边界案例

| 序号 | 场景 | 风险点 |
|------|------|--------|
| 1 | 查询点恰好在两个多边形的交界线上 | 两种方法可能返回不同多边形（但 closest_point 应相同） |
| 2 | 多个多边形到查询点等距 | 返回哪一个取决于遍历顺序；接受任意一个均可 |
| 3 | 查询点在 `(0, 0, 0)` 且多边形也在原点 | 距离 = 0，测试 early return 路径 |
| 4 | 非常大的坐标（`1e6, 1e6, 1e6`） | 浮点精度问题 |
| 5 | 非常小的多边形（面积 < CMP_EPSILON²） | AABB 可能非常小，测试 grow 是否足够 |

---

## 9. 性能基准测试

### 9.1 多边形数量 vs 查询耗时

| 多边形数量 | 测试方法 | 预期 BVH 耗时 | 预期暴力耗时 | 加速比 |
|-----------|---------|-------------|-------------|--------|
| 100 | 单次 `_find_closest_polygon_to_point` | < 1 μs | ~5 μs | ~5x |
| 1,000 | 同上 | < 2 μs | ~50 μs | ~25x |
| 10,000 | 同上 | < 3 μs | ~500 μs | ~170x |
| 100,000 | 同上 | < 5 μs | ~5 ms | ~1000x |

> 注：以上数值为量级估计，实际结果取决于多边形分布、CPU 缓存等因素。

### 9.2 渐进式搜索迭代次数分布

**测试方法**：在具有 10,000 个多边形的场景中，随机生成 10,000 个查询点，统计每次查询的搜索迭代次数。

**预期分布**：
- 1 次迭代（半径 1.0 内命中）：> 90%（对于角色在 NavMesh 上行走的常见情况）
- 2 次迭代（半径 4.0 内命中）：~5-8%
- 3+ 次迭代：< 2%

### 9.3 BVH 构建耗时

| 多边形数量 | 预期构建时间 | 说明 |
|-----------|-------------|------|
| 1,000 | < 0.5 ms | 包含 insert + optimize_top_down |
| 10,000 | < 5 ms | 同上 |
| 100,000 | < 50 ms | 同上 |

### 9.4 内存开销

| 多边形数量 | `polygon_bvh_data` 大小 | BVH 树节点估算 | 总额外内存 |
|-----------|----------------------|---------------|-----------|
| 1,000 | 8 KB (1000 × 8B 指针) | ~48 KB | ~56 KB |
| 10,000 | 80 KB | ~480 KB | ~560 KB |
| 100,000 | 800 KB | ~4.8 MB | ~5.6 MB |

---

## 10. 回归测试清单

确保 BVH 改动不破坏现有功能：

| 序号 | 测试项 | 验证方法 |
|------|--------|---------|
| 1 | 基本寻路（A → B） | 设置导航场景，调用 `get_simple_path()`，验证路径非空且合理 |
| 2 | 跨 Region 寻路 | 两个 Region 通过边连接，路径能正确跨越 |
| 3 | NavLink 跳跃寻路 | 通过 NavLink 连接两个不相邻的 Region，路径经过 Link |
| 4 | 动态添加/删除 Region | 运行时增删 Region 后，路径查询结果正确更新 |
| 5 | Navigation Layers 过滤 | 不同层的 Region 不被路径查询使用 |
| 6 | `get_closest_point()` | 调用 `NavigationServer3D::map_get_closest_point()` 返回 NavMesh 上最近的点 |
| 7 | `get_closest_point_normal()` | 返回最近点处的法线方向 |
| 8 | `get_closest_point_owner()` | 返回最近点所属的 Region/Link RID |
| 9 | `get_closest_point_to_segment()` | 线段到 NavMesh 的最近点 |
| 10 | 多线程并发查询 | 多个 Agent 同时寻路，结果正确且无崩溃 |
| 11 | 空导航地图查询 | 没有 Region 时查询不崩溃，返回空路径 |
| 12 | 超大距离查询 | 查询点距离 NavMesh 非常远时不崩溃，合理返回 |

---

## 11. 手动场景测试

### 11.1 测试场景搭建步骤

1. 创建一个 Godot 场景，添加 `NavigationRegion3D` 节点
2. 使用 `NavigationMesh` 烘焙一个包含多种地形的导航网格（平面、斜坡、台阶）
3. 添加多个 `NavigationLink3D` 连接不连通的区域
4. 添加一个 `CharacterBody3D` 作为测试 Agent

### 11.2 测试用例

| 序号 | 操作 | 预期结果 |
|------|------|---------|
| 1 | Agent 在 NavMesh 上正常行走 | 路径平滑，Agent 沿路径移动 |
| 2 | 将 Agent 拉到 NavMesh 外再寻路 | 能找到 NavMesh 上最近的起点，路径从该点开始 |
| 3 | 目标点在 NavMesh 外 | 路径终点在 NavMesh 边缘最近位置 |
| 4 | 禁用中间 Region 后寻路 | 路径绕行或返回失败 |
| 5 | 通过 NavLink 的跳跃寻路 | Agent 路径经过 NavLink 的 start/end 点 |
| 6 | 运行时删除 Region 后立即寻路 | 下一帧 sync 后，BVH 重建，旧 Region 不再被查询命中 |

### 11.3 性能对比测试

**方法**：
1. 准备一个大型开放世界场景（多边形 > 10,000）
2. 同时生成 100 个 Agent，各自寻路到随机目标
3. 使用 Godot Profiler 记录 `_query_task_find_start_end_positions` 的帧耗时
4. 与未加 BVH 的版本对比

**预期**：
- BVH 版本在 > 5,000 多边形的场景中有明显优势
- 帧时间抖动更小（避免了暴力遍历的最坏情况）

---

## 12. 已知的测试限制

| 序号 | 限制 | 说明 |
|------|------|------|
| 1 | `_polygon_to_aabb` 是 `static` 函数，无法直接单元测试 | 需要通过集成测试间接验证，或临时改为可访问的函数 |
| 2 | `_find_closest_polygon_to_point` 是 `static` 成员函数 | 需要构造完整的 `NavMapIteration3D` + `NavMeshPathQueryTask3D` 才能测试 |
| 3 | 等距多边形的返回结果不确定 | 当两个多边形到查询点的距离完全相同时，BVH 和暴力遍历可能返回不同的多边形（均视为正确） |
| 4 | 搜索半径硬编码无法在测试中覆盖 | `1.0` / `4.0` / `10000.0` 是编译时常量，无法在运行时调整 |
| 5 | 浮点精度差异 | BVH 回调的遍历顺序与暴力遍历不同，累积的浮点误差可能导致极端情况下 `closest_point` 微小差异 |
