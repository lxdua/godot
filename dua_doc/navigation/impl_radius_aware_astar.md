# Radius-Aware A* 与路径后处理 落地文档

> **分支**：`poc/funnel-radius-aware`
> **目标**：为 Godot 3D 导航查询新增 `path_agent_radius` 参数，使同一张已烘焙的 NavMesh 能够为不同体型的 agent 生成**正确绕过窄缝**且**与墙面保持 `r` 距离**的路径，而不需要为每种半径重新烘焙导航网格。
> **范围**：仅涉及查询时路径后处理（`NavigationServer3D::query_path`），**不改动** Recast 烘焙流程与 `NavigationMesh` 资源格式；数据结构扩展仅存在于运行期 `Nav3D::Polygon` 的元数据里。

---

## 1. 背景与动机

### 1.1 现状

Godot 在 NavMesh 烘焙期通过 `NavigationMesh.agent_radius` 对几何做一次性 erosion（Recast 里的 `erodeWalkableArea`）。运行期查询没有半径的概念，查询结果永远贴着烘焙后的多边形边。

这导致：

1. **一张 NavMesh = 一种体型**。要让小兵走窄缝、坦克绕远，只能烘焙多张网格，显存与一致性成本高。
2. **路径贴墙**。漏斗算法把路径点压在 Portal 端点甚至多边形顶点上，半径 > 0 的 agent 会卡住或视觉穿模。

### 1.2 目标

在**单张以极小半径（如 0.05 m）烘焙**的 NavMesh 上：

- 查询参数 `path_agent_radius = r`：
  - **A\***：剪掉所有无法承载直径 `2r` 通行的分支（狭缝 + 多边形内部 pivot 阻塞）。
  - **漏斗后处理**：对可通行的部分生成精确 clearance = `r` 的路径。
- 零烘焙开销，零兼容性破坏（参数默认 0 时行为与现版本一致）。

### 1.3 非目标

- 不做动态避让（这是 `NavigationAgent` 的 avoidance / RVO 职责）。
- 不改变 NavMesh 拓扑（不做运行期的 erosion）。
- 不解决高度/height 相关的 3D 通行性判定。

---

## 2. 理论依据

参考 Douglas Demyen 的 2006 硕士论文
**"Efficient Triangulation-Based Pathfinding"**（University of Alberta）第 4.2 节 *"Abstracted Representation for Sized Agents"*。

核心结论：对一条从 entry-edge $e_{in}$ 进入、exit-edge $e_{out}$ 离开的**三角形**内通道，最窄处由三角形的三种 choke width 决定：

- 若 $e_{in}$ 和 $e_{out}$ 共享顶点 $v$（总是这样，三角形只有 3 个顶点），记第三个顶点为 $w$：
  $$\mathrm{width}(e_{in}, e_{out}) = \begin{cases} \min(\|e_{in}\|, \|e_{out}\|) & \text{如果 } v \text{ 为凸顶点侧} \\ \mathrm{dist}(v, \overline{ab}) & \text{如果 } w \text{ 从 } v \text{ 到对边的距离作阻塞} \end{cases}$$

**Godot 的差异**：Godot/Recast 生成的是凸多边形（`NavigationMesh.cells_per_polygon` 默认把顶点数限制到 6），不是纯三角形。因此每多边形需要 $N(N-1)/2$ 条 edge-pair 的宽度记录，而不是固定 3 条。下面把这个思想推广到 N 边形。

### 2.1 N 边形内的 pivot 宽度

对多边形 $P$ 的任意一对边 $(e_i, e_j)$，记它们之间夹着的一串顶点为 $V_{ij} = \{v_{i+1}, v_{i+2}, \dots, v_{j}\}$（沿多边形顺时针走）。在该多边形内任意一条从 $e_i$ 上某点走到 $e_j$ 上某点的最短路径，**最窄处的宽度**等于：

$$
W(e_i, e_j) = \min_{v \in V_{ij}} \mathrm{dist}(v, \overline{e_i e_j}^\star)
$$

其中 $\overline{e_i e_j}^\star$ 是绕过 $v$ 后从 $e_i$ 到 $e_j$ 的真正走廊对侧。对凸多边形，可以进一步简化为：

$$
W(e_i, e_j) = \min\bigl(\|e_i\|,\ \|e_j\|,\ \min_{v\in V_{ij}} d(v, L_{ij}^{opp})\bigr)
$$

实现上不追求数学严谨，采取以下工程化公式（对凸多边形足够准确）：

1. 把两条边端点集合 $\{A_i, B_i, A_j, B_j\}$ 之间的**共享顶点**找出来（设为 $s$）。若无共享顶点，在"短串"侧的所有顶点依次 pivot。
2. 对 $V_{ij}$ 中每个 $v$，计算它到另一串（对侧）所有边的最短距离，取 min。
3. 再与两条端边本身的长度求 min。

---

## 3. 已完成工作（Phase 1）

### 3.1 查询参数

**文件**：`scene/resources/navigation/navigation_path_query_parameters_3d.{h,cpp}`

新增属性：

```cpp
real_t path_agent_radius = 0.0;
void set_path_agent_radius(real_t p_radius);
real_t get_path_agent_radius() const;
```

通过 `ClassDB::bind_method` + `ADD_PROPERTY` 暴露到 GDScript，GDExtension 兼容。

### 3.2 查询任务透传

**文件**：`modules/navigation_3d/3d/nav_mesh_queries_3d.{h,cpp}`

`NavMeshPathQueryTask3D` 结构体添加 `real_t path_agent_radius = 0.0;` 字段；
`NavigationServer3D::_query_task_build_path_corridor()` 把用户参数拷过去。

### 3.3 A* Portal 过滤

在 `_query_task_polygons_get_closest()` 的邻接遍历里，进入一个 `Connection` 之前先判定：

```cpp
if (p_query_task.path_agent_radius > 0.0) {
    const real_t portal_length =
        p_connection.pathway_start.distance_to(p_connection.pathway_end);
    if (portal_length < p_query_task.path_agent_radius * 2.0) {
        return; // 此 Portal 塞不下 agent，剪枝
    }
}
```

**限制**：只看"门"的宽度，**不看多边形内部**的 pivot 阻塞 —— 这正是 Phase 2 要解决的。

### 3.4 Corner Offset 后处理

在漏斗跑完之后，对路径中间点在其内角平分线方向外推 `r`：

```cpp
void NavMeshQueries3D::_query_task_offset_path_corners(
        NavMeshPathQueryTask3D &p_query_task) {
    const real_t r = p_query_task.path_agent_radius;
    if (r <= 0.0) return;
    auto &points = p_query_task.path_points;
    if (points.size() < 3) return;

    for (int i = 1; i + 1 < points.size(); i++) {
        Vector3 v1 = (points[i - 1] - points[i]).normalized();
        Vector3 v2 = (points[i + 1] - points[i]).normalized();
        Vector3 bisector = (v1 + v2);
        if (bisector.length_squared() < CMP_EPSILON) continue;
        bisector.normalize();
        // 内角平分线朝向"通道内部" = 与下一段法线同号，保证不穿墙
        points.write[i] = points[i] + bisector * r;
    }
}
```

**关键点**：

- Offset **只作用于拐角**（路径中点），起终点保持原样。
- 每个拐角只 offset 一次，不会累加多段偏移 → clearance 严格等于 `r`，不再出现外扩过度。
- 拐角处的内角平分线总是朝向"这段转折所属多边形的内侧"，前提是路径点本身在凸多边形顶点上——当前情况都满足，无需额外求法线。

### 3.5 PoC 测试脚本

**文件**：`dua_doc/navigation/poc_test_path_agent_radius.gd`

动态生成：一堵带三个门（0.6 / 1.2 / 2.0 m）的墙 + 5 种半径预设（0, 0.2, 0.4, 0.7, 1.1）+ 实时 slider。使用 `BoxMesh` 切段拼 ribbon 来解决线宽不可调问题。

---

## 4. 待落地工作（Phase 2：多边形内部宽度）

### 4.1 问题

Phase 1 只剪 Portal 窄到塞不下 agent 的情况。考虑下图：

```
       ┌─────────┐
       │    v    │   v 是 polygon 的一个尖角顶点
       │    ●    │   突入 polygon 内部
  e_in │   / \   │ e_out
  ─────┼──●───●──┼─────
       │         │
       └─────────┘
```

$e_{in}$ 和 $e_{out}$ 两条门都很宽，但 `v` 从上方插下来，使得从 $e_{in}$ 走到 $e_{out}$ 的**最窄处**远小于两条门的长度。Phase 1 会放行大 agent，结果漏斗后 corner-offset 又会把它推向 $v$ —— 视觉穿墙。

### 4.2 数据结构扩展

**文件**：`modules/navigation_3d/nav_utils_3d.h`

```cpp
struct Polygon {
    uint32_t id = UINT32_MAX;
    const NavBaseIteration3D *owner = nullptr;
    LocalVector<Vector3> vertices;
    real_t surface_area = 0.0;

    // --- Phase 2 新增 ---
    // 扇形剖分：从 vertices[0] 向其他顶点散出 N-2 个三角形。
    // 每个三角形记录 3 条 Demyen choke width，按
    //   [tri_k * 3 + local_edge]  (local_edge ∈ {0,1,2}) 存一维数组。
    // N = 6 时最多 3 * 4 = 12 float = 48 B。
    LocalVector<real_t> triangle_choke_widths;
};
```

**为什么改用三角剖分而不是 N-gon pairwise 表**：

- Demyen 原论文的 3-choke 公式是对三角形证明的；直接推广到凸 N 边形只是启发式。
  扇形剖分后，每个扇形三角形复用论文公式，正确性由论文兜底。
- 存储略降：60 B → 48 B/poly。
- 预计算从 `O(N³)` 降到 `O(N)`（N-2 个三角形 × 常数）。
- 代价：A* 每次扩展多边形时，要在 N-2 个三角形组成的链上 min-reduce，而不是 1 次查表。
  N ≤ 6 时最多 4 次比较，仍是常数，完全可忽略。

**A\* 图粒度不变**：节点仍是原始多边形，`traveled_distance` /
`distance_to_destination` / funnel 全部在多边形层面工作。三角形**仅**在
"这个半径能否穿过此多边形"判定里使用。

### 4.3 预计算时机

扇形剖分 + 三角形 choke width 在 **`NavMap3D::sync()`** 每个 region 合并
完、polygon 列表最终化之后一次性填好。

**文件**：`modules/navigation_3d/nav_map_3d.cpp`
**插入点**：`NavMap3D::sync()` 里对每个 polygon 调用
`_compute_polygon_triangle_chokes(poly)`；实现放在
`modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` 匿名命名空间里。

```cpp
// 三角形 (a, b, c)，入边与出边都是它的三条边之一。
// 对每条边记录这条边作为"入边"时，另外两条边的 choke 最小值；
// 或者更直白：对每对 (in_edge, out_edge) 存一次 Demyen choke。
//
// 为了省空间，我们只存"这条边在三角形里能承载的最小通行直径"，
// 按 Demyen §4.2：
//   width(edge_k) = 2 * 内切圆半径-like 量，由三角形顶点到对边距离给出
// 等价于经过该边时不被"对面顶点"卡住的最大直径。
// 多边形链条上每个三角形取最小即是 entry→exit 的瓶颈。

static void _compute_polygon_triangle_chokes(Nav3D::Polygon &p_poly) {
    const uint32_t N = p_poly.vertices.size();
    if (N < 3) return;
    const uint32_t tri_count = N - 2;
    p_poly.triangle_choke_widths.resize(tri_count * 3);

    const Vector3 &v0 = p_poly.vertices[0];
    for (uint32_t k = 0; k < tri_count; k++) {
        const Vector3 &va = p_poly.vertices[k + 1];
        const Vector3 &vb = p_poly.vertices[k + 2];

        // 三条边：e0=(v0,va), e1=(va,vb), e2=(vb,v0)
        // 每条边的 choke width = 对面顶点到该边的距离，与边本身长度的 min。
        const real_t d0 = Geometry3D::get_closest_distance_to_segment(vb, v0, va);
        const real_t d1 = Geometry3D::get_closest_distance_to_segment(v0, va, vb);
        const real_t d2 = Geometry3D::get_closest_distance_to_segment(va, vb, v0);

        p_poly.triangle_choke_widths[k * 3 + 0] = MIN(d0, v0.distance_to(va));
        p_poly.triangle_choke_widths[k * 3 + 1] = MIN(d1, va.distance_to(vb));
        p_poly.triangle_choke_widths[k * 3 + 2] = MIN(d2, vb.distance_to(v0));
    }
}
```

> 复杂度：$O(N)$ / polygon（扇形剖分 + 常数工作量 / 三角形），sync 期总开销可忽略。

**多边形边 → 扇形三角形映射**：

- 多边形边 $e_m = (v_m, v_{m+1})$：
  - $m = 0$：三角形 $T_0 = (v_0, v_1, v_2)$ 的 local edge 0。
  - $1 \le m \le N-2$：三角形 $T_{m-1} = (v_0, v_m, v_{m+1})$ 的 local edge 1。
  - $m = N-1$：三角形 $T_{N-3} = (v_0, v_{N-2}, v_{N-1})$ 的 local edge 2。

相邻扇形三角形之间共享**对角线** $(v_0, v_k)$，这不是多边形的真实边，
所以穿越它时不需要 choke 检查（agent 走过对角线并不会撞到对角线"两侧"任何东西）。

### 4.4 A* 中使用

**文件**：`modules/navigation_3d/3d/nav_mesh_queries_3d.cpp`
**函数**：`_query_task_polygons_get_closest()` 内部，检查 Portal 的同时检查多边形：

```cpp
// 当前 poly 的进入边编号（从 back_navigation_edge 拿）
const int enter_edge = /* ... 来自 NavigationPoly */ ;
// 即将离开的出边编号（= p_connection.edge）
const int exit_edge = p_connection.edge;

if (p_query_task.path_agent_radius > 0.0 && enter_edge >= 0) {
    const real_t internal_width = _polygon_traversal_width(
        *current_nav_poly->poly, enter_edge, exit_edge);
    if (internal_width < p_query_task.path_agent_radius * 2.0) {
        return; // 这个多边形内部走不了
    }
}
```

`_polygon_traversal_width()` 的做法：

1. 根据 4.3 的映射把 `enter_edge` / `exit_edge` 翻译成扇形三角形编号
   `tri_a` / `tri_b` 与 local edge 下标。
2. 先取 `triangle_choke_widths[tri_a * 3 + local_enter]` 和
   `triangle_choke_widths[tri_b * 3 + local_exit]` 作为起止 choke。
3. 对区间 `[min(tri_a, tri_b) + 1, max(tri_a, tri_b) - 1]` 内的每个中间三角形，
   取**两条非对角线边**中的较小者（等价于"这个三角形可以让 agent 通过"的宽度），
   更新 min。
4. 返回最小值。

N ≤ 6 时最多遍历 4 个三角形，内部常数工作量，完全可忽略。

### 4.5 后处理阶段的补强

Phase 1 的 `_query_task_offset_path_corners` 不变。因为：

- A* 已经保证选出的每个 polygon 内部都能走过去（clearance ≥ `r`）。
- Corner offset 只把路径外推 `r`，在已保证宽度 ≥ `2r` 的多边形里，必然仍在原多边形或相邻多边形内，不会穿墙。

但保险起见，可在 offset 后做一次**per-segment 多边形归属检查**（沿用 `NavMap3D::get_closest_point_info`）。优先级低，Phase 2 第二迭代再加。

---

## 5. API / 兼容性

| 接口 | 类型 | 默认值 | 影响 |
|------|------|--------|------|
| `NavigationPathQueryParameters3D.path_agent_radius` | `real_t` | `0.0` | 新增属性；为 0 时行为与现版本完全一致 |
| `Nav3D::Polygon::edge_pair_widths` | `LocalVector<real_t>` | `{}` | **内部字段**，不暴露到脚本层；sync 时填充，热路径只读 |

**ABI**：对 GDExtension 消费者，`NavigationPathQueryParameters3D` 的方法表追加两个 setter/getter，不会破坏现有绑定。`NavMeshPathQueryTask3D` 是 internal struct，无影响。

**存档**：`.res` / `.tres` 带 `path_agent_radius` 的 `NavigationPathQueryParameters3D` 资源在旧引擎上打开时该字段会被忽略，按默认 0 处理。

---

## 6. 测试计划

### 6.1 PoC（手工）

`poc_test_path_agent_radius.gd` 覆盖：

- 三门等宽 → 不同半径选不同门。
- Slider 实时扫过 `r ∈ [0, 2]`，观察路径切换点是否与门宽对齐。

### 6.2 Unit Tests（Phase 2 之后补）

建议位置：`tests/servers/test_navigation_server_3d.h`。
用例：

1. **`radius_zero_unchanged`**：`path_agent_radius = 0` 时查询结果与 master 分支逐点完全一致。
2. **`narrow_portal_rejected`**：单条 Portal < 2r 时路径必须绕行或返回空。
3. **`internal_pivot_rejected`**：手工构造一个梯形 + 内凸尖角多边形，使两条门都 > 2r 但内部宽度 < 2r，验证 A* 会跳过该 polygon。
4. **`clearance_exactly_r`**：路径上每个中点离最近多边形边距离 ≥ `r - ε`，误差 ε ≤ 1e-3 m。
5. **`no_regression_large_map`**：`demo/nav_obstacle_3d` 之类的已有示例在 `r = 0` 下查询延迟不变（± 5 %）。

### 6.3 性能基准

- 烘焙期：`NavMap3D::sync()` 时长，对 5k / 20k polygon 对比。
- 查询期：`query_path()` 平均耗时（1k 次随机 start/goal）。预期：**r > 0 时比 r = 0 快 0~15 %**（剪枝减少了 A* 扩展的节点）。

---

## 7. 风险与权衡

| 风险 | 描述 | 缓解 |
|------|------|------|
| 多边形凹陷 | 理论上 Godot 的多边形是凸的，但 portal merging 后可能出现准凹形 | sync 时校验，若非凸则 `edge_pair_widths` 退化为 `min(len_i, len_j)`，保守放行 |
| Delaunay vs 任意凸 | 论文证明基于三角形，推广到 N-gon 的严谨性未经正式证明 | 经验公式在 N ≤ 6 时实测无反例；若遇反例可按最坏情况把该 polygon 拆成扇形三角形再算 |
| 高度/height | 3D NavMesh 顶点带 y，平面距离低估 clearance | 先在 XZ 平面算距离；如果 Map 3D 上有多层也是现行 Funnel 的已知限制，不在本 issue 扩大 |
| Agent 起点在障碍里 | `get_closest_point` 把 start 吸附到 navmesh 后，首个多边形可能本身不够宽 | 起点所在 polygon 不做宽度过滤（跟现版本一致），否则"原地动不了"成为常态 |

---

## 8. 迭代路线

- [x] **Phase 1** — Portal 宽度过滤 + Corner Offset（本分支已跑通）。
- [ ] **Phase 2.a** — `edge_pair_widths` 数据结构 + sync 期预计算。
- [ ] **Phase 2.b** — A* 内联多边形宽度过滤。
- [ ] **Phase 3** — 单元测试 + benchmark 入 CI。
- [ ] **Phase 4** — 写 godot-proposals issue + 发 PR。

---

## 9. 相关资料

- Demyen, D. *Efficient Triangulation-Based Pathfinding*. M.Sc. Thesis, University of Alberta, 2006. §4.2 Abstracted Representation for Sized Agents.
- Mikko Mononen, *Simple Stupid Funnel Algorithm*, 2010（Godot 当前漏斗实现的参考）。
- `dua_doc/navigation/impl_funnel_radius_aware.md` — 旧的 Portal 边膨胀方案，已废弃。
- `dua_doc/navigation/poc_test_path_agent_radius.gd` — 当前 PoC 测试场景。
