# 单一 NavMesh 上的可变体型寻路 — 技术设计文档

> **版本**: v0.1 Draft  
> **日期**: 2025-01  
> **参考文献**:  
> - *Efficient Triangulation-Based Pathfinding* (Demyen, 2006) — Section 3.1, Algorithm 3  
> - *StarCraft 2 GDC: "Pathing — It's Not a Solved Problem"* (James Anhalt)  

---

## 目录

1. [问题陈述与目标](#1-问题陈述与目标)
2. [现有系统分析](#2-现有系统分析)
3. [方案总览](#3-方案总览)
4. [Part 1 — Corridor Width 预计算](#4-part-1--corridor-width-预计算)
5. [Part 2 — Expanded Vertices 路径偏移](#5-part-2--expanded-vertices-路径偏移)
6. [API 设计](#6-api-设计)
7. [数据结构变更](#7-数据结构变更)
8. [文件改动清单与伪代码](#8-文件改动清单与伪代码)
9. [工作量评估](#9-工作量评估)
10. [风险分析与边界情况](#10-风险分析与边界情况)
11. [未来扩展](#11-未来扩展)

---

## 1. 问题陈述与目标

### 现状痛点

当前 Godot 导航系统中，每种 Agent 体型（不同 `agent_radius`）都需要烘焙一份**独立的 NavMesh**。这意味着：

- **内存开销**：N 种体型 = N 份 NavMesh 数据
- **烘焙时间**：每次场景变更需要重新烘焙所有体型的 NavMesh
- **维护成本**：设计师需要管理多份导航资源
- **运行时灵活性差**：无法动态调整 Agent 体型

### 目标

在**单一 NavMesh** 上支持**可变体型寻路**，使不同 `agent_radius` 的 Agent 共享同一份导航数据，同时保证：

1. 大体型 Agent 不会穿越过窄的通道
2. 路径自动远离墙壁/障碍物边缘 `agent_radius` 的距离
3. 对现有零半径查询（`agent_radius = 0`）保持完全兼容

---

## 2. 现有系统分析

### 2.1 核心数据结构

**`nav_utils_3d.h` — `Nav3D` 命名空间**

```
Polygon {
    uint32_t id;                          // 区域内局部 ID
    const NavBaseIteration3D *owner;      // 所属 Region/Link
    LocalVector<Vector3> vertices;        // 凸多边形顶点（最多6个）
    real_t surface_area;
}

Connection {
    Polygon *polygon;        // 连接目标多边形
    int edge;                // 源多边形的边索引
    Vector3 pathway_start;   // 通道起点
    Vector3 pathway_end;     // 通道终点
}

NavigationPoly {
    const Polygon *poly;
    int back_navigation_poly_id;
    int back_navigation_edge;
    Vector3 back_navigation_edge_pathway_start;
    Vector3 back_navigation_edge_pathway_end;
    Vector3 entry;
    real_t traveled_distance;    // g cost
    real_t distance_to_destination; // h cost
}
```

### 2.2 关键流程

查询入口：`NavMeshQueries3D::query_task_map_iteration_get_path()`

```
1. _query_task_find_start_end_positions()   → 找起终点所在多边形
2. _query_task_build_path_corridor()        → A* 搜索，构建多边形走廊
   └─ _query_task_search_polygon_connections() → 邻居展开（内部+外部连接）
3. _query_task_post_process_corridorfunnel() → 漏斗算法，生成最终路径点
```

### 2.3 连接拓扑构建

**区域内部** (`nav_region_builder_3d.cpp`)：
- 遍历每个多边形的每条边，用 `EdgeKey` 做哈希
- 共享边的两个多边形互相记入 `internal_connections[poly.id]`

**区域之间** (`nav_map_builder_3d.cpp`)：
- 相同 EdgeKey 的边直接合并
- 自由边通过 `edge_connection_margin` 距离匹配
- NavLink 创建合成多边形桥接

### 2.4 A* 邻居展开的关键代码位置

`_query_task_build_path_corridor()` 中（`nav_mesh_queries_3d.cpp:366-482`）：

```cpp
// 内部连接
const LocalVector<Connection> &polygon_connections =
    navbase_polygons_to_connections[navbase_local_polygon_id];
for (const Connection &connection : polygon_connections) {
    _query_task_search_polygon_connections(..., connection, ...);
}

// 外部连接
for (const Connection &connection :
     navbases_polygons_external_connections[least_cost_navbase][navbase_local_polygon_id]) {
    _query_task_search_polygon_connections(..., connection, ...);
}
```

`_query_task_search_polygon_connections()` 计算新的 `traveled_distance` 并更新堆，**此处即为插入宽度过滤的最佳位置**。

---

## 3. 方案总览

整体方案分为两个正交的部分，可以独立开发、逐步集成：

```
┌─────────────────────────────────────────────────────────────────┐
│                     NavMesh (单次烘焙, agent_radius=0)          │
│  ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐    │
│  │ Polygon A │───│ Polygon B │───│ Polygon C │───│ Polygon D │   │
│  └──────────┘   └──────────┘   └──────────┘   └──────────┘    │
│       │              │              │              │             │
│  Part 1: 预计算 corridor_width per connection                   │
│  (构建时一次性完成，存入 Connection 结构)                         │
│       │              │              │              │             │
│       ▼              ▼              ▼              ▼             │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │ A* 搜索时：skip if corridor_width < agent_diameter      │    │
│  │ (在 _query_task_search_polygon_connections 中过滤)       │    │
│  └─────────────────────────────────────────────────────────┘    │
│       │                                                         │
│       ▼                                                         │
│  Part 2: 走廊确定后, 对漏斗算法的输入做顶点扩展                   │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │ Expanded Vertices: 每个 portal 顶点向内偏移 agent_radius │    │
│  │ → 新 portal → 标准漏斗算法 → 安全路径                     │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

| 部分 | 解决的问题 | 时机 | 复杂度 |
|------|-----------|------|--------|
| Part 1: Corridor Width | Agent 能不能过 | 地图构建时预计算 + A* 查询时过滤 | 中等 |
| Part 2: Expanded Vertices | Agent 过去后路径离墙多远 | 路径后处理时 | 较高 |

---

## 4. Part 1 — Corridor Width 预计算

### 4.1 概念：凸多边形的通道宽度

对于三角形 NavMesh，Demyen 论文定义 "triangle width" 为：从一条边到对顶点的最短距离（即三角形在该方向上的"厚度"）。

Godot 使用**凸多边形**（最多 6 顶点），需要推广此概念。

**定义**：对于一个 Connection（从多边形 P 的边 E_src 连接到相邻多边形 Q 的边 E_dst），`corridor_width` 定义为：

> **从 E_src 到 E_dst 之间，多边形 P 内部可通过的最小宽度。**

对于凸多边形，这等价于：E_src 和 E_dst 所界定的"带状区域"的最窄处。

### 4.2 凸多边形通道宽度计算算法

#### 情况 1：两条边共享一个顶点（最常见）

```
        v_shared
       / \
      /   \
E_src/     \E_dst
    /  Poly  \
   v1────────v2
```

宽度 = 共享顶点到对边的距离。具体地：
- 设 E_src = (v_shared, v1)，E_dst = (v_shared, v2)
- `corridor_width = distance_point_to_segment(v_shared, v1, v2)`
  - 但更准确地说，是 min(E_src.length, E_dst.length, dist(v_shared, line(v1,v2)))

实际上，对共享顶点的情况，corridor_width 就是以 v_shared 为顶点、E_src 和 E_dst 为两腰的三角形的最小高。

更简洁的定义：

```python
def corridor_width_shared_vertex(v_shared, v1, v2):
    """v_shared 是两条边的公共顶点, v1 是 E_src 的另一端, v2 是 E_dst 的另一端"""
    # 宽度 = 两条边形成的张角处的"瓶颈"
    # 等于 min(从 v_shared 出发，沿两条边方向，能放下的圆的直径)
    edge1 = (v1 - v_shared).normalized()
    edge2 = (v2 - v_shared).normalized()
    sin_half_angle = edge1.cross(edge2).length()  # |sin(angle)|
    # 对于锐角通道，瓶颈在 v_shared 处
    # corridor_width ≈ 2 * min(len(E_src), len(E_dst)) * sin(angle/2)
    # 但简化为：
    return v1.distance_to(v2)  # E_src 远端到 E_dst 远端的距离（portal 宽度的下界）

    # 更精确版本：
    # return min(
    #     distance_point_to_line(v1, v_shared, v2),
    #     distance_point_to_line(v2, v_shared, v1),
    #     v1.distance_to(v2)
    # )
```

#### 情况 2：两条边不共享顶点（较少见，多边形 ≥ 4 顶点时可能出现）

```
   v_a1 ──── v_a2    (E_src)
   |              |
   |    Poly      |
   |              |
   v_b1 ──── v_b2    (E_dst)
```

此时通道宽度 = 两条边之间的最短距离：

```python
def corridor_width_non_adjacent(E_src_start, E_src_end, E_dst_start, E_dst_end):
    return min_distance_between_segments(
        E_src_start, E_src_end,
        E_dst_start, E_dst_end
    )
```

#### 情况 3：Connection 的 pathway 是边的子段

对于外部连接（edge connection margin 产生的），`pathway_start/end` 可能不覆盖整条边。此时直接用 pathway 的长度作为宽度上界：

```python
corridor_width = min(
    pathway_start.distance_to(pathway_end),
    polygon_internal_width  # 前两种情况计算的值
)
```

### 4.3 在 Connection 上存储 corridor_width

**核心思想**：每条 Connection 代表从当前多边形穿越到邻居的一个通道。该通道的宽度取决于：

1. **pathway 本身的长度**：`pathway_start.distance_to(pathway_end)` — 这是通道"入口"的宽度
2. **多边形内部的几何约束** — 这是通过多边形内部时的瓶颈

最终 `corridor_width = min(portal_width, internal_constraint)`。

> **简化方案（推荐初版实现）**：
> 
> 仅使用 `pathway_start.distance_to(pathway_end)` 作为 `corridor_width`。
> 
> **理由**：
> - 凸多边形的 NavMesh 中，portal（共享边或 margin 连接）通常就是瓶颈
> - 实现极其简单，只需一行距离计算
> - 对于大多数实际场景已经足够准确
> - 后续可以增加更精确的多边形内部约束计算

### 4.4 A* 过滤逻辑

在 `_query_task_search_polygon_connections()` 的**最前面**添加宽度检查：

```cpp
void NavMeshQueries3D::_query_task_search_polygon_connections(
    NavMeshPathQueryTask3D &p_query_task,
    const Connection &p_connection, ...) {

    // ★ 新增: 宽度过滤
    if (p_query_task.agent_radius > 0.0f) {
        real_t agent_diameter = p_query_task.agent_radius * 2.0f;
        real_t corridor_width = p_connection.pathway_start.distance_to(
            p_connection.pathway_end);
        if (corridor_width < agent_diameter) {
            return; // 通道太窄，该体型无法通过
        }
    }

    // ... 原有逻辑不变 ...
}
```

---

## 5. Part 2 — Expanded Vertices 路径偏移

### 5.1 目标

A* + 宽度过滤保证了走廊足够宽，但标准漏斗算法产生的路径会紧贴顶点（墙角），不考虑 Agent 半径。我们需要将路径从墙壁偏移 `agent_radius` 距离。

### 5.2 算法概述（基于 Demyen 2006, Section 3.1 的推广）

```
输入：多边形走廊 + portal 序列 + agent_radius
输出：安全路径点序列

1. 从走廊中提取 portal 序列 (left[], right[])
2. 对每个 portal 顶点 v:
   a. 计算 v 的"角平分线方向"（v 相邻两条边的法线平均）
   b. 将 v 沿角平分线方向向多边形内部偏移 agent_radius → v_expanded
   c. 如果偏移后的点超出多边形范围，clamp 到安全位置
3. 用 expanded 顶点重建 portal 边
4. 在新 portal 上运行标准漏斗算法
```

### 5.3 顶点扩展详细算法

#### Step 1：提取 Portal 顶点

当前漏斗算法 (`_query_task_post_process_corridorfunnel`) 已经在遍历走廊时提取了 `left` 和 `right` portal 端点。我们需要在同一遍历中收集完整的顶点序列。

```
Portal 序列 (从 end 到 begin 回溯):
  portal[0]: (left_0, right_0) = (pathway_start, pathway_end) of last poly
  portal[1]: (left_1, right_1) = ... of second-to-last
  ...
  portal[N]: (begin_point, begin_point) = 起点（退化为点）
```

#### Step 2：计算每个顶点的偏移方向

对于 portal 顶点 `v`（假设它是多边形的一个角点），其偏移方向由相邻的两条多边形边决定：

```python
def compute_offset_direction(v, edge_prev, edge_next, polygon_normal):
    """
    v: 当前顶点
    edge_prev: v 到前一个顶点的方向
    edge_next: v 到后一个顶点的方向
    polygon_normal: 多边形面法线 (map_up)
    """
    # 每条边的内法线 = 边方向 × 面法线（指向多边形内部）
    normal_prev = edge_prev.cross(polygon_normal).normalized()
    normal_next = edge_next.cross(polygon_normal).normalized()

    # 角平分线
    bisector = (normal_prev + normal_next).normalized()

    # 偏移距离补偿：在角平分线方向上，需要偏移
    # agent_radius / sin(half_angle) 才能保证与两条边都距离 agent_radius
    half_angle = acos(clamp(normal_prev.dot(normal_next), -1, 1)) / 2.0
    offset_distance = agent_radius / max(sin(half_angle), 0.1)  # 防止除零
    offset_distance = min(offset_distance, agent_radius * 3.0)  # cap 防止过大偏移

    return v + bisector * offset_distance
```

#### Step 3：重建 Portal

```python
expanded_portals = []
for i in range(len(portals)):
    left_expanded = expand_vertex(portals[i].left, ...)
    right_expanded = expand_vertex(portals[i].right, ...)

    # 确保 expanded portal 宽度 > 0
    if left_expanded.distance_to(right_expanded) < EPSILON:
        # portal 太窄，使用中点
        mid = (left_expanded + right_expanded) / 2
        left_expanded = mid
        right_expanded = mid

    expanded_portals.append((left_expanded, right_expanded))
```

#### Step 4：在新 Portal 上运行漏斗

直接复用现有 `_query_task_post_process_corridorfunnel()` 的逻辑，只是输入改为 expanded portals。

### 5.4 特殊情况处理

| 情况 | 处理方式 |
|------|---------|
| Portal 顶点不在多边形顶点上（margin 连接的中点）| 使用 portal 边的法线方向偏移 |
| 偏移后 portal 宽度 < 0 | 使用 portal 中点（退化为点 portal）|
| 起点/终点的 portal | 不偏移，保持原始位置 |
| NavLink 合成多边形 | 不偏移（link 已经是点到点） |
| agent_radius = 0 | 跳过整个扩展步骤，走原有路径 |

### 5.5 实现策略

**推荐分阶段实现**：

- **Phase 2a**：简化版 — 仅对 portal 端点做固定方向偏移（沿 portal 边向内缩短 agent_radius），不做角平分线计算
- **Phase 2b**：完整版 — 角平分线 + 距离补偿

Phase 2a 的伪代码：

```python
def shrink_portal(portal_start, portal_end, agent_radius):
    """将 portal 两端各向内收缩 agent_radius"""
    direction = (portal_end - portal_start).normalized()
    length = portal_start.distance_to(portal_end)
    shrink = min(agent_radius, length / 2.0 - EPSILON)
    if shrink <= 0:
        mid = (portal_start + portal_end) / 2.0
        return (mid, mid)
    return (portal_start + direction * shrink,
            portal_end - direction * shrink)
```

这个简化版已经能避免路径紧贴墙角的大部分情况。

---

## 6. API 设计

### 6.1 查询参数扩展

**文件**: `servers/navigation_3d/navigation_path_query_parameters_3d.h`

```cpp
class NavigationPathQueryParameters3D : public RefCounted {
    // ... existing members ...

    // ★ 新增
    float agent_radius = 0.0;  // 默认 0 = 不做体型过滤/偏移

public:
    // ★ 新增
    void set_agent_radius(float p_radius);
    float get_agent_radius() const;
};
```

### 6.2 绑定注册

**文件**: `servers/navigation_3d/navigation_path_query_parameters_3d.cpp`

```cpp
void NavigationPathQueryParameters3D::_bind_methods() {
    // ... existing bindings ...

    ClassDB::bind_method(D_METHOD("set_agent_radius", "agent_radius"),
        &NavigationPathQueryParameters3D::set_agent_radius);
    ClassDB::bind_method(D_METHOD("get_agent_radius"),
        &NavigationPathQueryParameters3D::get_agent_radius);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "agent_radius", PROPERTY_HINT_RANGE,
        "0.0,10.0,0.01,or_greater"), "set_agent_radius", "get_agent_radius");
}
```

### 6.3 内部查询任务传递

**文件**: `modules/navigation_3d/3d/nav_mesh_queries_3d.h`

```cpp
struct NavMeshPathQueryTask3D {
    // ... existing members ...

    // ★ 新增
    float agent_radius = 0.0;
};
```

**文件**: `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` — `map_query_path()`

```cpp
query_task.agent_radius = p_query_parameters->get_agent_radius();
```

### 6.4 向后兼容

- `agent_radius = 0.0`（默认值）：所有行为与修改前完全一致
- Part 1 宽度过滤和 Part 2 顶点扩展均由 `agent_radius > 0` 条件守护
- 不改变任何现有 API 的签名或行为

---

## 7. 数据结构变更

### 7.1 Connection 结构扩展（可选，用于预计算宽度）

> 初版方案使用运行时计算 `pathway_start.distance_to(pathway_end)`，无需修改 Connection 结构。
>
> 如果后续需要更精确的预计算宽度，可以添加：

```cpp
struct Connection {
    Polygon *polygon = nullptr;
    int edge = -1;
    Vector3 pathway_start;
    Vector3 pathway_end;
    // ★ 可选新增
    real_t corridor_width = FLT_MAX;  // 预计算的通道宽度
};
```

### 7.2 无需修改的结构

| 结构 | 原因 |
|------|------|
| `Polygon` | 通道宽度是 Connection 级别的属性 |
| `NavigationPoly` | A* 状态不需要额外字段 |
| `PathQuerySlot` | 槽位结构不变 |
| `EdgeConnectionPair` | 连接配对逻辑不变 |

---

## 8. 文件改动清单与伪代码

### 8.1 改动文件总览

| 文件 | 改动类型 | 改动量 | Part |
|------|---------|--------|------|
| `servers/navigation_3d/navigation_path_query_parameters_3d.h` | API 新增 | ~5 行 | 共用 |
| `servers/navigation_3d/navigation_path_query_parameters_3d.cpp` | 绑定注册 | ~10 行 | 共用 |
| `modules/navigation_3d/3d/nav_mesh_queries_3d.h` | 任务结构 | ~2 行 | 共用 |
| `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | **核心改动** | ~80 行 | 1 + 2 |
| `modules/navigation_3d/nav_utils_3d.h` | 可选扩展 | ~2 行 | 1 (可选) |

### 8.2 详细伪代码

#### 8.2.1 `navigation_path_query_parameters_3d.h` — 新增属性

```diff
 private:
     float path_search_max_distance = 0.0;
+    float agent_radius = 0.0;

 public:
+    void set_agent_radius(float p_radius);
+    float get_agent_radius() const;
```

#### 8.2.2 `navigation_path_query_parameters_3d.cpp` — 绑定

```diff
 void NavigationPathQueryParameters3D::_bind_methods() {
     // ... existing ...
+    ClassDB::bind_method(D_METHOD("set_agent_radius", "agent_radius"),
+        &NavigationPathQueryParameters3D::set_agent_radius);
+    ClassDB::bind_method(D_METHOD("get_agent_radius"),
+        &NavigationPathQueryParameters3D::get_agent_radius);
+    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "agent_radius",
+        PROPERTY_HINT_RANGE, "0.0,10.0,0.01,or_greater"),
+        "set_agent_radius", "get_agent_radius");
 }
+
+void NavigationPathQueryParameters3D::set_agent_radius(float p_radius) {
+    agent_radius = MAX(p_radius, 0.0f);
+}
+
+float NavigationPathQueryParameters3D::get_agent_radius() const {
+    return agent_radius;
+}
```

#### 8.2.3 `nav_mesh_queries_3d.h` — 任务结构

```diff
 struct NavMeshPathQueryTask3D {
     float path_search_max_distance = 0.0;
+    float agent_radius = 0.0;
```

#### 8.2.4 `nav_mesh_queries_3d.cpp` — map_query_path 传递参数

```diff
 void NavMeshQueries3D::map_query_path(...) {
     // ... existing parameter setup ...
     query_task.path_search_max_distance = p_query_parameters->get_path_search_max_distance();
+    query_task.agent_radius = p_query_parameters->get_agent_radius();
     query_task.status = NavMeshPathQueryTask3D::TaskStatus::QUERY_STARTED;
```

#### 8.2.5 `nav_mesh_queries_3d.cpp` — Part 1: A* 宽度过滤

```diff
 void NavMeshQueries3D::_query_task_search_polygon_connections(
     NavMeshPathQueryTask3D &p_query_task,
     const Connection &p_connection, ...) {

+    // Part 1: Width filtering — skip connections too narrow for agent.
+    if (p_query_task.agent_radius > 0.0f) {
+        real_t corridor_width = p_connection.pathway_start.distance_to(
+            p_connection.pathway_end);
+        if (corridor_width < p_query_task.agent_radius * 2.0f) {
+            return;
+        }
+    }
+
     const NavBaseIteration3D *connection_owner = p_connection.polygon->owner;
     // ... rest unchanged ...
 }
```

#### 8.2.6 `nav_mesh_queries_3d.cpp` — Part 2: Portal 收缩

**新增静态辅助函数：**

```cpp
static void _shrink_portal(const Vector3 &p_start, const Vector3 &p_end,
                           real_t p_agent_radius,
                           Vector3 &r_start, Vector3 &r_end) {
    Vector3 dir = p_end - p_start;
    real_t len = dir.length();
    if (len < CMP_EPSILON) {
        r_start = p_start;
        r_end = p_end;
        return;
    }
    dir /= len;
    real_t shrink = MIN(p_agent_radius, len * 0.5f - CMP_EPSILON);
    if (shrink <= 0.0f) {
        Vector3 mid = (p_start + p_end) * 0.5f;
        r_start = mid;
        r_end = mid;
        return;
    }
    r_start = p_start + dir * shrink;
    r_end = p_end - dir * shrink;
}
```

**修改漏斗算法，在读取 portal 时应用收缩：**

```diff
 void NavMeshQueries3D::_query_task_post_process_corridorfunnel(
     NavMeshPathQueryTask3D &p_query_task) {
     // ... existing setup ...

     while (p) {
         // Set left and right points of the pathway between polygons.
-        Vector3 left = p->back_navigation_edge_pathway_start;
-        Vector3 right = p->back_navigation_edge_pathway_end;
+        Vector3 left, right;
+        if (p_query_task.agent_radius > 0.0f) {
+            _shrink_portal(
+                p->back_navigation_edge_pathway_start,
+                p->back_navigation_edge_pathway_end,
+                p_query_task.agent_radius,
+                left, right);
+        } else {
+            left = p->back_navigation_edge_pathway_start;
+            right = p->back_navigation_edge_pathway_end;
+        }
         if (THREE_POINTS_CROSS_PRODUCT(apex_point, left, right).dot(p_map_up) < 0) {
             SWAP(left, right);
         }
         // ... rest unchanged ...
```

#### 8.2.7 完整新增代码量统计

| 位置 | 新增行数 | 修改行数 |
|------|---------|---------|
| `navigation_path_query_parameters_3d.h` | 3 | 0 |
| `navigation_path_query_parameters_3d.cpp` | 15 | 0 |
| `nav_mesh_queries_3d.h` | 1 | 0 |
| `nav_mesh_queries_3d.cpp` — 参数传递 | 1 | 0 |
| `nav_mesh_queries_3d.cpp` — 宽度过滤 | 7 | 0 |
| `nav_mesh_queries_3d.cpp` — portal 收缩函数 | 18 | 0 |
| `nav_mesh_queries_3d.cpp` — 漏斗修改 | 8 | 2 |
| **总计** | **~53** | **~2** |

---

## 9. 工作量评估

### 9.1 分阶段计划

| 阶段 | 内容 | 预估工时 | 风险 |
|------|------|---------|------|
| **Phase 0** | API 新增 (`agent_radius` 属性 + 传递) | 0.5 天 | 低 |
| **Phase 1** | Part 1 宽度过滤（portal 宽度） | 1 天 | 低 |
| **Phase 1-test** | 单元测试 + 场景验证 | 1 天 | 低 |
| **Phase 2a** | Part 2 简化版 portal 收缩 | 1.5 天 | 中 |
| **Phase 2a-test** | 漏斗算法回归测试 | 1 天 | 中 |
| **Phase 2b** | Part 2 完整版角平分线偏移（可选） | 2 天 | 高 |
| **Phase 2b-test** | 边界情况测试 | 1.5 天 | 高 |
| **Phase 3** | Connection.corridor_width 预计算优化（可选） | 1.5 天 | 中 |
| **总计（必要部分 0+1+2a）** | | **5 天** | |
| **总计（含完整版 + 优化）** | | **10 天** | |

### 9.2 最小可行产品 (MVP)

Phase 0 + Phase 1 + Phase 2a = **约 4-5 个工作日**

这已经能够：
- ✅ 大体型 Agent 自动避开窄通道
- ✅ 路径离开墙角 `agent_radius` 距离
- ✅ 零半径查询完全兼容

---

## 10. 风险分析与边界情况

### 10.1 技术风险

| 风险 | 严重度 | 概率 | 缓解措施 |
|------|--------|------|---------|
| Portal 收缩导致漏斗算法退化 | 高 | 中 | 收缩后宽度 < 0 时退化为中点；添加断言检查 |
| 过窄走廊被全部过滤，导致无路可走 | 中 | 低 | 保留现有 `reachable_end` 回退机制；agent_radius 过大时返回最近可达点 |
| 多线程安全 | 高 | 低 | agent_radius 是查询参数（栈上），不修改共享数据；PathQuerySlot 已有互斥机制 |
| 外部连接 (margin) 的 pathway 长度不代表真实通道宽度 | 中 | 中 | Phase 3 可添加更精确的预计算；初版使用 pathway 距离已是合理近似 |
| 2D vs 3D: portal 收缩在斜面上的行为 | 低 | 中 | 收缩沿 portal 边方向进行，与平面无关；但角平分线偏移需要考虑 map_up |

### 10.2 边界情况清单

| 情况 | 预期行为 | 需要测试 |
|------|---------|---------|
| `agent_radius = 0` | 完全不触发新逻辑，行为不变 | ✅ |
| `agent_radius` 大于所有 portal | A* 找不到路径，回退到最近可达点 | ✅ |
| 起点/终点在窄通道内 | 起终点多边形不受宽度过滤影响（它们是直接定位的） | ✅ |
| 两个 portal 完全重叠（NavLink） | NavLink 的 pathway_start == pathway_end，宽度=0，但 NavLink 应豁免宽度过滤 | ✅ |
| Portal 收缩后左右反转 | 退化为中点，漏斗算法仍可处理 | ✅ |
| 非常大的凸多边形（6顶点）| 多个出边的 corridor_width 各自独立，互不影响 | ✅ |
| 动态障碍物改变 NavMesh | 障碍物不在本方案范围内（它们影响 NavMesh 重建） | - |

### 10.3 NavLink 特殊处理

NavLink 创建的合成多边形是退化的（4 个顶点但面积为 0），其 pathway_start 和 pathway_end 通常是相同的点（宽度=0）。

**必须豁免 NavLink 的宽度过滤**，否则所有 Link 都会被过滤掉。

实现方式：在宽度过滤中检查 `connection.edge == -1`（NavLink 的标志）：

```cpp
if (p_query_task.agent_radius > 0.0f && p_connection.edge != -1) {
    real_t corridor_width = p_connection.pathway_start.distance_to(
        p_connection.pathway_end);
    if (corridor_width < p_query_task.agent_radius * 2.0f) {
        return;
    }
}
```

---

## 11. 未来扩展

### 11.1 预计算 Corridor Width 传播

类似 Demyen 论文中的 "width propagation"，可以在地图构建时预计算每对相邻多边形之间的精确通道宽度，考虑多边形内部几何。这可以避免运行时的距离计算。

### 11.2 分层宽度标注

预计算多个宽度阈值（如 0.5m, 1.0m, 2.0m），在 A* 中使用二分查找快速判断，避免浮点比较。

### 11.3 Agent Height 支持

类似方案可以扩展到垂直维度，在 Connection 上存储 `corridor_height`，过滤高度不足的通道。

### 11.4 动态宽度查询

允许在运行时根据动态障碍物临时缩小 corridor_width，实现更精确的动态避障。

### 11.5 可视化调试

在 NavigationServer 的调试绘制中，用颜色编码显示每条连接的 corridor_width，帮助设计师理解 NavMesh 的通行性。

---

## 附录 A：代码引用索引

| 代码符号 | 文件 | 行号 |
|---------|------|------|
| `Nav3D::Polygon` | `modules/navigation_3d/nav_utils_3d.h` | 98-107 |
| `Nav3D::Connection` | `modules/navigation_3d/nav_utils_3d.h` | 84-96 |
| `Nav3D::NavigationPoly` | `modules/navigation_3d/nav_utils_3d.h` | 109-150 |
| `NavMeshPathQueryTask3D` | `modules/navigation_3d/3d/nav_mesh_queries_3d.h` | 57-123 |
| `_query_task_build_path_corridor` | `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | 319-512 |
| `_query_task_search_polygon_connections` | `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | 279-317 |
| `_query_task_post_process_corridorfunnel` | `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | 735-828 |
| `NavigationPathQueryParameters3D` | `servers/navigation_3d/navigation_path_query_parameters_3d.h` | 37-131 |
| `NavMapBuilder3D::build_navmap_iteration` | `modules/navigation_3d/3d/nav_map_builder_3d.cpp` | 54-74 |
| `NavRegionBuilder3D::_build_step_find_edge_connection_pairs` | `modules/navigation_3d/3d/nav_region_builder_3d.cpp` | 167-223 |
| `NavBaseIteration3D` | `modules/navigation_3d/3d/nav_base_iteration_3d.h` | 38-68 |
| `NavMap3D::query_path` | `modules/navigation_3d/nav_map_3d.cpp` | 147-182 |

---

## 附录 B：与参考文献的对应关系

| 本方案概念 | Demyen 2006 | StarCraft 2 GDC |
|-----------|-------------|-----------------|
| corridor_width per Connection | Triangle Width (Section 3.1) | "annotated edge width" |
| A* 宽度过滤 | Algorithm 3: Width-based search | "skip edges below agent size" |
| Portal 收缩 | Expanded Vertices (Section 3.1) | "offset portals by radius" |
| 角平分线偏移 | Vertex expansion direction | — |
| 凸多边形适配 | 论文仅讨论三角形 | SC2 使用三角形 |

**关键差异**：Demyen 论文和 SC2 都假设三角形 NavMesh。本方案需要处理 Godot 的凸多边形（最多 6 顶点），这使得 "triangle width" 的概念需要推广为 "connection corridor width"，但核心思想一致。
