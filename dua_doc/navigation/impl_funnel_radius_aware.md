
# 漏斗算法半径感知（Portal 边膨胀）技术方案

> **目标**：在 Godot 导航系统的漏斗算法（Simple Stupid Funnel）执行前，对 Portal 边按 `agent_radius` 向通道内部收缩，使生成的路径与墙壁保持安全距离，避免大体型单位穿墙。

---

## 1. 问题描述

### 1.1 现状

当前漏斗算法在 `nav_mesh_queries_3d.cpp` → `_query_task_post_process_corridorfunnel()` 中实现。算法直接在原始 Portal 边（`pathway_start` / `pathway_end`）上运行，生成的路径会**贴着多边形拐角走**。

对于半径为 0 的点状 agent 这没有问题，但对于有体积的 agent（如 RTS 中的大型单位），路径点紧贴拐角意味着 agent 的**边缘会穿入障碍物**。

### 1.2 核心原因

```
Portal 边 (原始)
  A ●━━━━━━━━━━━━━● B
  
漏斗收束后路径点 P 可能恰好在 A 或 B 上 → agent 圆心在 A/B 时，
半径 r 的圆会超出 navmesh 边界。
```

### 1.3 参考方案：星际争霸 2 膨胀顶点（Expanded Vertices）

SC2 的做法是在漏斗算法之前，将每条 Portal 边的两个端点沿**通道内侧法线**偏移 `agentRadius`，形成一条更短的"膨胀 Portal"。漏斗算法在膨胀后的 Portal 上运行，自然产生与墙壁保持安全距离的路径。

---

## 2. 现有代码架构分析

### 2.1 关键数据结构

```
文件: modules/navigation_3d/nav_utils_3d.h
```

```cpp
struct Connection {
    Polygon *polygon = nullptr;   // 目标多边形
    int edge = -1;                // 源多边形边索引
    Vector3 pathway_start;        // Portal 边起点
    Vector3 pathway_end;          // Portal 边终点
};

struct NavigationPoly {
    const Polygon *poly = nullptr;
    int back_navigation_poly_id = -1;
    int back_navigation_edge = -1;
    Vector3 back_navigation_edge_pathway_start;  // 回溯时记录的 Portal 起点
    Vector3 back_navigation_edge_pathway_end;     // 回溯时记录的 Portal 终点
    Vector3 entry;
    real_t traveled_distance = 0.0;
    real_t distance_to_destination = 0.0;
    // ...
};
```

### 2.2 漏斗算法流程（当前）

```
文件: modules/navigation_3d/3d/nav_mesh_queries_3d.cpp
函数: _query_task_post_process_corridorfunnel()
行号: 735-828
```

**当前流程：**

```
1. 从 end_point 开始，反向遍历 path corridor
2. 对每个 NavigationPoly，读取 back_navigation_edge_pathway_start/end 作为 Portal 左右边界
3. 用叉积判断左右方向（THREE_POINTS_CROSS_PRODUCT 宏 + dot(map_up)）
4. 标准漏斗收束：
   - 左侧点在左 portal 同侧 → 收窄左边界
   - 左侧点越过右边界 → 右 portal 成为新 apex → 输出路径点
   - 对称处理右侧
5. 路径点通过 _query_task_push_back_point_with_metadata() 追加
6. 经 _query_task_clip_path() 处理 3D 地形高差
7. 最终 path_reverse() 得到正序路径
```

**关键观察：**
- Portal 边数据存储在 `NavigationPoly::back_navigation_edge_pathway_start/end` 中
- 漏斗函数仅读取这些值，不修改 `NavigationPoly` 本身
- `_query_task_clip_path()` 也读取同样的 Portal 边数据用于高差裁剪

### 2.3 查询参数传递链

```
NavigationPathQueryParameters3D (GDScript 可见)
    ↓ map_query_path()
NavMeshPathQueryTask3D (内部任务结构)
    ↓ query_task_map_iteration_get_path()
        ↓ _query_task_build_path_corridor()   -- A* 搜索，填充 NavigationPoly
        ↓ _query_task_post_process_corridorfunnel()  -- 漏斗后处理
        ↓ path_reverse()
        ↓ _query_task_simplified_path_points() (可选)
        ↓ _query_task_process_path_result_limits()
```

---

## 3. 总体设计

### 3.1 方案概述

在漏斗算法执行前，插入一个**Portal 边膨胀预处理步骤**：

```
_query_task_build_path_corridor()
    ↓
>>> _query_task_inflate_portals()  ← 新增：膨胀 Portal 边 <<<
    ↓
_query_task_post_process_corridorfunnel()
```

**核心思路**：遍历 path corridor 中每个 `NavigationPoly` 的 Portal 边，将 `pathway_start` 和 `pathway_end` 沿通道内侧方向各向内偏移 `agent_radius`。漏斗算法无需任何修改，天然在收缩后的 Portal 上运行。

### 3.2 膨胀算法详解

对于 corridor 中连续的 Portal 序列 `Portal[0], Portal[1], ..., Portal[N-1]`：

每条 Portal 有左端点 `L[i]` 和右端点 `R[i]`（基于 map_up 方向区分左右）。

**膨胀策略**：

```
对于 Portal[i] 的左端点 L[i]:
  1. 计算该端点处的"墙壁方向" = 从 L[i-1] 到 L[i] 再到 L[i+1] 的平均方向
  2. 取该方向的垂线（指向通道内部）
  3. 沿垂线偏移 agent_radius → 得到 L'[i]

对于 Portal[i] 的右端点 R[i]:
  对称处理 → 得到 R'[i]
```

**简化方案（推荐首版实现）**：

不需要多点平均，只需对单条 Portal 做垂直收缩：

```
Portal 边方向:  edge_dir = normalize(pathway_end - pathway_start)
Portal 边长度:  edge_len = length(pathway_end - pathway_start)

膨胀后:
  new_start = pathway_start + edge_dir * min(agent_radius, edge_len * 0.5)
  new_end   = pathway_end   - edge_dir * min(agent_radius, edge_len * 0.5)
```

这种"端点向中心收缩"的方式简单、稳定，且对所有几何情况都有明确行为。

### 3.3 为什么是沿 Portal 边方向而非法线方向

Portal 边是两个相邻多边形之间的通道口。墙壁拐角恰好在 Portal 边的端点处。agent 需要与**端点**保持距离，而端点就在 Portal 边的两头。因此，将 Portal 边从两端向中心缩短 `agent_radius` 就等价于让漏斗"看到"一个更窄的通道，路径自然远离拐角。

```
  墙壁                             墙壁
   │     原始 Portal               │
   │  A ●━━━━━━━━━━━━━━━━● B      │
   │     ↑ agent_radius ↑          │
   │     A'●━━━━━━━━━━●B'          │
   │     膨胀 Portal               │
   │                               │
```

---

## 4. 详细实现设计

### 4.1 API 扩展：传入 agent_radius

#### 4.1.1 NavigationPathQueryParameters3D（GDScript 暴露层）

```
文件: servers/navigation_3d/navigation_path_query_parameters_3d.h
```

**新增成员变量：**

```cpp
// 在 private: 段，path_search_max_distance 下方添加
float path_agent_radius = 0.0;
```

**新增 getter/setter 声明：**

```cpp
// 在 public: 段添加
void set_path_agent_radius(float p_radius);
float get_path_agent_radius() const;
```

#### 4.1.2 NavigationPathQueryParameters3D（实现）

```
文件: servers/navigation_3d/navigation_path_query_parameters_3d.cpp
```

**新增实现：**

```cpp
void NavigationPathQueryParameters3D::set_path_agent_radius(float p_radius) {
    path_agent_radius = MAX(0.0, p_radius);
}

float NavigationPathQueryParameters3D::get_path_agent_radius() const {
    return path_agent_radius;
}
```

**在 `_bind_methods()` 中注册：**

```cpp
ClassDB::bind_method(D_METHOD("set_path_agent_radius", "radius"), &NavigationPathQueryParameters3D::set_path_agent_radius);
ClassDB::bind_method(D_METHOD("get_path_agent_radius"), &NavigationPathQueryParameters3D::get_path_agent_radius);

ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "path_agent_radius"), "set_path_agent_radius", "get_path_agent_radius");
```

#### 4.1.3 NavMeshPathQueryTask3D（内部任务结构）

```
文件: modules/navigation_3d/3d/nav_mesh_queries_3d.h
```

**在 `NavMeshPathQueryTask3D` 结构体中新增：**

```cpp
// 在 float path_search_max_distance = 0.0; 下方
float path_agent_radius = 0.0;
```

#### 4.1.4 参数传递

```
文件: modules/navigation_3d/3d/nav_mesh_queries_3d.cpp
函数: map_query_path()
```

在已有的参数赋值区域（约第 213 行附近）添加：

```cpp
query_task.path_agent_radius = p_query_parameters->get_path_agent_radius();
```

### 4.2 Portal 膨胀预处理函数

#### 4.2.1 函数声明

```
文件: modules/navigation_3d/3d/nav_mesh_queries_3d.h
```

```cpp
// 在 _query_task_post_process_corridorfunnel 声明附近添加
static void _query_task_inflate_portals(NavMeshPathQueryTask3D &p_query_task);
```

#### 4.2.2 函数实现

```
文件: modules/navigation_3d/3d/nav_mesh_queries_3d.cpp
```

**完整伪代码/实现：**

```cpp
void NavMeshQueries3D::_query_task_inflate_portals(NavMeshPathQueryTask3D &p_query_task) {
    const float agent_radius = p_query_task.path_agent_radius;
    if (agent_radius <= 0.0f) {
        return; // 半径为 0，无需膨胀，保持原始行为
    }

    LocalVector<NavigationPoly> &navigation_polys = p_query_task.path_query_slot->path_corridor;
    uint32_t least_cost_id = p_query_task.least_cost_id;

    // 从终点多边形反向遍历 corridor 中所有 NavigationPoly，
    // 修改其 back_navigation_edge_pathway_start/end。
    NavigationPoly *p = &navigation_polys[least_cost_id];

    while (p) {
        Vector3 &pw_start = p->back_navigation_edge_pathway_start;
        Vector3 &pw_end = p->back_navigation_edge_pathway_end;

        // 计算 Portal 边向量和长度
        Vector3 edge_vec = pw_end - pw_start;
        real_t edge_len = edge_vec.length();

        if (edge_len > CMP_EPSILON) {
            // 计算实际可收缩量：每端最多收缩 agent_radius，
            // 但总收缩量不超过边长的一半（保留最小 Portal 宽度）
            real_t shrink = MIN(agent_radius, edge_len * 0.5f);
            
            Vector3 edge_dir = edge_vec / edge_len; // 归一化
            
            pw_start = pw_start + edge_dir * shrink;
            pw_end   = pw_end   - edge_dir * shrink;
        }
        // else: Portal 已退化为点，不做处理

        // 反向遍历
        if (p->back_navigation_poly_id != -1) {
            p = &navigation_polys[p->back_navigation_poly_id];
        } else {
            p = nullptr;
        }
    }
}
```

#### 4.2.3 调用位置

```
文件: modules/navigation_3d/3d/nav_mesh_queries_3d.cpp
函数: query_task_map_iteration_get_path()
行号: 约 540 行处（switch 语句前）
```

```cpp
// Post-Process path.
switch (p_query_task.path_postprocessing) {
    case PathPostProcessing::PATH_POSTPROCESSING_CORRIDORFUNNEL: {
        _query_task_inflate_portals(p_query_task);  // ← 新增：在漏斗前膨胀
        _query_task_post_process_corridorfunnel(p_query_task);
    } break;
    // ... 其他 case 不变
}
```

> **注意**：只在 `CORRIDORFUNNEL` 模式下膨胀。Edge-centered 和 None 模式不需要膨胀，因为它们不经过漏斗收束。

---

## 5. 边缘情况处理

### 5.1 Portal 边被膨胀到退化（长度归零）

**场景**：通道宽度 < 2 × agent_radius

**处理**：

```cpp
real_t shrink = MIN(agent_radius, edge_len * 0.5f);
```

当 `edge_len < 2 * agent_radius` 时，每端只收缩 `edge_len * 0.5`，Portal 退化为中心点。漏斗算法能正确处理"Portal 为单点"的情况（左右 portal 重合时，该点必然成为路径点）。

**结果**：路径会穿过通道中心——这是狭窄通道下的最优解。

### 5.2 第一条和最后一条 Portal

corridor 的起始 Portal（begin_polygon 的 back 指向）和最终 Portal 的 `pathway_start == pathway_end == begin_point / end_point`（退化为点）。膨胀函数中 `edge_len <= CMP_EPSILON` 会跳过这些，不会产生问题。

### 5.3 Link（导航链接）跨越的 Portal

Link 连接两个不相邻的区域，其 Portal 可能很长。膨胀逻辑仍然适用——Link Portal 同样从两端收缩 `agent_radius`，让 agent 不贴 Link 连接点的墙壁。

### 5.4 clip_path 兼容性

`_query_task_clip_path()` 也读取 `back_navigation_edge_pathway_start/end`。在膨胀后运行 clip_path 是安全的——它在膨胀后的 Portal 上做平面裁剪，裁剪出的高差过渡点也自然远离墙壁。

实际上这正是理想行为：clip_path 产生的中间路径点也应该尊重 agent_radius。

### 5.5 agent_radius = 0 时的向后兼容

```cpp
if (agent_radius <= 0.0f) {
    return;
}
```

默认值为 0，整个膨胀步骤被跳过，行为与修改前完全一致。**零破坏性**。

### 5.6 非常大的 agent_radius

如果 `agent_radius` 大于所有 Portal 边长的一半，所有 Portal 都退化为中点。路径将退化为沿 corridor 中线行走。这是合理的降级行为——如果 agent 太大以至于几乎填满通道，走中线是最安全的。

但应注意：如果 navmesh 本身没有针对大 agent 做 erosion，即使路径走中线，agent 仍可能穿墙。**建议在文档中提示用户：`path_agent_radius` 应 ≤ navmesh bake 时使用的 agent_radius。**

### 5.7 corridor 中 Portal 朝向不一致

漏斗函数内部已经通过叉积判断并 SWAP left/right：

```cpp
if (THREE_POINTS_CROSS_PRODUCT(apex_point, left, right).dot(p_map_up) < 0) {
    SWAP(left, right);
}
```

膨胀函数不需要关心左右，因为它是沿 Portal 边方向做对称收缩。

---

## 6. 文件修改清单

| # | 文件路径 | 修改类型 | 改动描述 |
|---|---------|---------|---------|
| 1 | `servers/navigation_3d/navigation_path_query_parameters_3d.h` | 新增成员 | `float path_agent_radius = 0.0;` + getter/setter 声明 |
| 2 | `servers/navigation_3d/navigation_path_query_parameters_3d.cpp` | 新增实现 | getter/setter 实现 + `_bind_methods()` 注册 + ADD_PROPERTY |
| 3 | `modules/navigation_3d/3d/nav_mesh_queries_3d.h` | 新增字段 + 声明 | `NavMeshPathQueryTask3D::path_agent_radius` + `_query_task_inflate_portals()` 声明 |
| 4 | `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | 核心改动 | ① `map_query_path()` 中传递参数 ② 新增 `_query_task_inflate_portals()` 函数实现 ③ `query_task_map_iteration_get_path()` 中在漏斗前调用膨胀 |

### 6.1 无需修改的文件

| 文件 | 原因 |
|------|------|
| `nav_utils_3d.h` | `Connection` / `NavigationPoly` 结构无需改动，膨胀直接修改已复制到 corridor 中的值 |
| `navigation_server_3d.h` | 公共 API 无需改动，`path_agent_radius` 通过 `NavigationPathQueryParameters3D` 传入 |
| `_query_task_post_process_corridorfunnel()` | **无需修改** — 这是设计核心优势 |
| `_query_task_clip_path()` | 自动兼容膨胀后的 Portal |

---

## 7. 完整伪代码

### 7.1 _query_task_inflate_portals

```
FUNCTION _query_task_inflate_portals(query_task):
    radius ← query_task.path_agent_radius
    IF radius ≤ 0:
        RETURN
    
    corridor ← query_task.path_query_slot.path_corridor
    current ← corridor[query_task.least_cost_id]
    
    WHILE current ≠ NULL:
        start ← current.back_navigation_edge_pathway_start  // 引用
        end   ← current.back_navigation_edge_pathway_end    // 引用
        
        edge_vec ← end - start
        edge_len ← length(edge_vec)
        
        IF edge_len > EPSILON:
            shrink ← MIN(radius, edge_len × 0.5)
            dir ← edge_vec / edge_len
            
            start ← start + dir × shrink
            end   ← end   - dir × shrink
        END IF
        
        IF current.back_navigation_poly_id ≠ -1:
            current ← corridor[current.back_navigation_poly_id]
        ELSE:
            current ← NULL
        END IF
    END WHILE
END FUNCTION
```

### 7.2 调用序列

```
FUNCTION query_task_map_iteration_get_path(query_task, map_iteration):
    query_task.path_clear()
    
    _query_task_find_start_end_positions(query_task, map_iteration)
    
    // 平凡情况检查（省略）...
    
    _query_task_build_path_corridor(query_task, map_iteration)
    
    // 状态检查（省略）...
    
    SWITCH query_task.path_postprocessing:
        CASE CORRIDORFUNNEL:
            _query_task_inflate_portals(query_task)    // ← 新增
            _query_task_post_process_corridorfunnel(query_task)
        
        CASE EDGECENTERED:
            _query_task_post_process_edgecentered(query_task)
        
        CASE NONE:
            _query_task_post_process_nopostprocessing(query_task)
    
    query_task.path_reverse()
    
    IF query_task.simplify_path:
        _query_task_simplified_path_points(query_task)
    
    _query_task_process_path_result_limits(query_task)
    
    query_task.status ← QUERY_FINISHED
END FUNCTION
```

---

## 8. GDScript 用户使用示例

```gdscript
var query_params = NavigationPathQueryParameters3D.new()
query_params.map = get_world_3d().navigation_map
query_params.start_position = agent.global_position
query_params.target_position = target.global_position
query_params.path_postprocessing = NavigationPathQueryParameters3D.PATH_POSTPROCESSING_CORRIDORFUNNEL

# 新增：设置 agent 半径，路径将与墙壁保持此距离
query_params.path_agent_radius = 0.5  # agent 碰撞半径

var query_result = NavigationPathQueryResult3D.new()
NavigationServer3D.query_path(query_params, query_result)

var path = query_result.get_path()
```

---

## 9. 进阶优化方向（非首版范围）

### 9.1 邻居感知膨胀（Neighbor-Aware Inflation）

首版使用简单的"沿 Portal 边方向两端对称收缩"。进阶方案可以考虑相邻 Portal 的几何关系：

```
对于 left_vertices 序列 [L0, L1, L2, ...]:
  对每个 Li，计算相邻线段 (Li-1→Li) 和 (Li→Li+1) 的平均方向
  取垂线（指向通道内部）偏移 agent_radius
```

这种方案在弯曲走廊中能生成更平滑的路径，但实现复杂度高，且需要正确区分左右顶点序列。**建议首版验证简单方案后，在第二版迭代。**

### 9.2 动态半径

某些游戏中不同单位有不同半径。当前设计已支持——每次查询通过 `path_agent_radius` 传入不同值即可。膨胀是查询时的实时计算，不修改共享的 navmesh 数据。

### 9.3 2D 导航系统同步

Godot 同时有 2D 导航系统。2D 漏斗算法在 `modules/navigation_2d/` 中有对应实现。本方案可平行移植，但不在首版范围内。

---

## 10. 工作量评估

| 任务 | 预估人天 | 说明 |
|------|---------|------|
| API 层扩展（NavigationPathQueryParameters3D） | 0.5 天 | 纯机械：加字段、getter/setter、bind |
| 内部参数传递（Task 结构 + map_query_path） | 0.25 天 | 沿用现有模式 |
| _query_task_inflate_portals 实现 | 1 天 | 核心算法，含边缘情况处理 |
| 调用点集成 | 0.25 天 | 在 switch 中添加一行调用 |
| 单元测试 | 1 天 | 需构造 corridor mock，验证退化/正常/窄通道场景 |
| 集成测试（场景验证） | 1 天 | 在实际 navmesh 场景中验证路径不穿墙 |
| Debug 可视化（可选） | 0.5 天 | 在 editor 中绘制膨胀前后 Portal 对比 |
| **合计** | **3.5 - 4.5 天** | |

---

## 11. 风险分析

### 11.1 高风险

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 膨胀后 Portal 退化导致路径质量下降 | 在极窄通道中路径可能出现锯齿或不必要的拐点 | `MIN(radius, edge_len * 0.5)` 保底；退化为中点是可接受的降级 |
| clip_path 在膨胀 Portal 上行为异常 | 3D 地形高差过渡点位置偏移 | clip_path 用平面裁剪 Portal 线段，膨胀后线段仍合法；需测试验证 |

### 11.2 中风险

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 性能影响：多一次 corridor 遍历 | 查询时间增加 | corridor 通常 < 100 个 poly，一次线性遍历可忽略（< 1μs） |
| 向后兼容性 | 默认行为变化可能影响现有项目 | `path_agent_radius` 默认 0，不膨胀，行为完全一致 |
| 简单膨胀在复杂凹角走廊中不够精确 | 路径离某些墙壁仍然太近 | 首版已是显著改善；进阶方案可在后续迭代中实现 |

### 11.3 低风险

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| API 命名与 Godot 约定冲突 | 代码审查时被要求改名 | 使用 `path_agent_radius` 与现有 `path_return_max_radius` 命名风格一致 |
| 用户传入负值 | undefined behavior | setter 中 `MAX(0.0, p_radius)` 保护 |
| 修改 NavigationPoly 的 pathway 字段影响后续重复查询 | 数据污染 | corridor 每次查询都会 reset，膨胀发生在 `_query_task_build_path_corridor` 之后、漏斗之前，不影响 A* 数据 |

---

## 12. 测试策略

### 12.1 单元测试用例

| 用例 | 输入 | 期望 |
|------|------|------|
| 正常膨胀 | Portal (0,0,0)→(10,0,0), radius=1 | 膨胀后 (1,0,0)→(9,0,0) |
| 恰好半宽 | Portal (0,0,0)→(4,0,0), radius=2 | 膨胀后 (2,0,0)→(2,0,0)（中点） |
| 超宽 radius | Portal (0,0,0)→(2,0,0), radius=5 | 膨胀后 (1,0,0)→(1,0,0)（中点，clamped） |
| 零长 Portal | Portal (3,0,3)→(3,0,3), radius=1 | 不变 (3,0,3)→(3,0,3) |
| radius=0 | 任意 Portal | 不变 |
| 3D 倾斜 Portal | Portal (0,0,0)→(3,4,0), radius=1 | 沿 (0.6,0.8,0)方向收缩 1 单位 |

### 12.2 集成测试场景

1. **L 形走廊**：验证路径在拐角处与墙壁保持距离
2. **窄门**：Portal 宽度 < 2×radius，验证路径穿过中点
3. **开阔区域**：radius 对宽 Portal 影响小，路径几乎不变
4. **多层高差**：验证 clip_path 兼容性
5. **Link 跨越**：验证 Link Portal 也被正确膨胀

---

## 13. 总结

本方案通过在漏斗算法前插入一个轻量级 Portal 边膨胀预处理步骤，以最小侵入性实现了半径感知的路径规划。核心优势：

1. **零修改漏斗算法**：漏斗函数完全不变，膨胀是纯预处理
2. **向后兼容**：默认 radius=0 时行为不变
3. **实现简洁**：核心函数 < 30 行 C++ 代码
4. **性能无感**：一次线性遍历 corridor，开销可忽略
5. **可扩展**：后续可升级为邻居感知膨胀，替换函数内部即可，接口不变
