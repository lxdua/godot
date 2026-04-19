# 单一 NavMesh 上的可变体型寻路方案

> 技术调研文档 | 2026-04-17  
> 参考来源：jdtec《RTS Pathfinding 3》、星际争霸2 GDC 演讲、Demyen 2006 论文  
> 目标引擎：Godot Engine

---

## 目录

1. [背景与动机](#1-背景与动机)
2. [现有方案对比：多份 NavMesh vs 单份 NavMesh](#2-现有方案对比)
3. [核心算法详解](#3-核心算法详解)
   - 3.1 [三角形宽度（Triangle Width）](#31-三角形宽度)
   - 3.2 [膨胀顶点（Expanded Vertices）](#32-膨胀顶点)
   - 3.3 [完整流程](#33-完整流程)
4. [Godot 引擎导航系统现状](#4-godot-引擎导航系统现状)
5. [移植可行性分析](#5-移植可行性分析)
6. [实现方案](#6-实现方案)
7. [相关知识：缺页中断与内存管理](#7-相关知识缺页中断与内存管理)
8. [相关知识：BSP 树](#8-相关知识bsp-树)
9. [参考资料](#9-参考资料)

---

## 1. 背景与动机

在 RTS 游戏中，不同体型的单位（小型步兵、中型车辆、大型巨兽）需要在同一张地图上寻路。小单位能走的窄路，大单位可能过不去；大单位需要自动选择宽阔的路线。

### 问题

传统方案（Unity / Recast-Detour / Godot）通过为每种体型**烘焙一份独立的 NavMesh** 来解决这个问题。但在 RTS 场景下，这带来了严重问题：

- **体型种类多**：5-10 种不同大小的单位 → 需要 5-10 份 NavMesh → 内存翻倍
- **地图动态变化**：建筑建造/拆除会改变地形 → 每份 NavMesh 都要重建 → 性能不可接受
- **连续可变体型**：某些单位可能动态改变大小 → 多份离散 NavMesh 无法覆盖

### 目标

在**单一 NavMesh** 上支持任意大小的单位寻路，运行时根据单位半径动态判断通道可通过性并生成安全路径。

---

## 2. 现有方案对比

| 方案 | 内存 | 构建时间 | 动态更新 | 路径精度 | 适用场景 |
|------|------|---------|---------|---------|---------|
| **多份 NavMesh**（Recast 原生） | ×N | ×N | 每份都要更新 | 完美 | 体型少、静态地图 |
| **单份 + 边长判断** | ×1 | ×1 | 只更新一份 | ❌ 不可靠 | 不推荐 |
| **单份 + 三角形宽度 + 膨胀顶点**（SC2/本文） | ×1 | ×1（+预计算） | 只更新一份 | 完美 | **RTS、体型多、动态地图** |
| **单份 + 运行时宽度检查** | ×1 | ×1 | 只更新一份 | 中等 | 简单场景 |

### 为什么"用边长判断"不可靠？

早期方案通过三角形共享边的长度来判断单位能否通过，但边长 ≠ 通道实际宽度：

```
共享边 AB 很长（看起来能过）：

      A ─────────────── B
     / \               /
    /   \   三角形    /
   /     \           /
  /       \         /
 C ─────── D ──── E

但实际通道在三角形内部可能被对面的顶点"掐"得很窄。
边长只反映边界尺寸，不反映内部通行能力。
```

---

## 3. 核心算法详解

### 3.1 三角形宽度（Triangle Width）

> 来源：《Efficient Triangulation-Based Pathfinding》(Demyen, 2006) Section 3.1 & Algorithm 3

#### 概念

对 NavMesh 中的每个三角形，每一对可穿越的边之间都有一个**三角形宽度**值——即一个圆形单位从一条边进入、从另一条边离开时，能通过的**最大直径**。

```
一个三角形有 3 条边：e0, e1, e2
有 3 种穿越方式，各有一个宽度值：

  e0 → e1：width_01
  e0 → e2：width_02
  e1 → e2：width_12
```

#### 直觉理解

想象推一个**圆形单位**穿过三角形。三角形宽度就是能推过去的最大圆的直径：

```
宽三角形：                    窄三角形：

    e0                           e0
  ─────────                  ─────────
  \         /                \       /
   \  ●→  / e1                \ ●→ / e1
    \    /                     \  /
     \ /                        \/  ← 对面顶点很近，通道窄
      V                          V
    e2                          e2

width = 大                   width = 小
```

#### 关键点

三角形宽度不仅取决于边长，还取决于**三角形的形状（角度）**。具体计算方法见论文 Algorithm 3。核心思想是：通道的瓶颈在对面的顶点处，宽度由该顶点到两条穿越边的几何关系决定。

#### 在 A* 中使用

```
预处理：
  对每个三角形预计算 3 个宽度值
  triangleWidth[triIndex][edgePairIndex]

A* 搜索时：
  当前三角形 T_curr，从边 e_in 进来
  邻居三角形 T_next，从边 e_out 出去

  corridorWidth = triangleWidth[T_curr][pair(e_in, e_out)]

  if (corridorWidth < unit.radius * 2) {
      // 通道太窄，跳过这个邻居
      continue;
  }
```

效果：

```
┌──── 宽通道（width=3.0）────┐
│ 大单位(r=1.2) ✅ 能过       │
│ 小单位(r=0.3) ✅ 能过       │
├──── 窄通道（width=0.8）────┤
│ 大单位(r=1.2) ❌ A*跳过     │ → 自动绕路走宽通道
│ 小单位(r=0.3) ✅ 走捷径     │
```

---

### 3.2 膨胀顶点（Expanded Vertices）

> 来源：星际争霸2 GDC 演讲《Pathing: It's Not a Solved Problem》(James Anhalt)

#### 问题

A* 找到了一条三角形通道后，路径点不能贴着 NavMesh 边界走，否则大单位的边缘会穿墙：

```
墙 ████████████
    ○          ← 单位中心在边界上，但"身体"已经穿墙
墙 ████████████
```

需要将路径点向通道**内部**偏移 `agentRadius` 的距离。

#### 详细步骤

**Step 1：提取通道左右顶点列表**

从 A* 输出的三角形通道中，提取两侧的边界顶点序列：

```
通道（三角形序列）：

  左侧顶点：L0, L1, L2, L3
  右侧顶点：R0, R1, R2, R3

     L0 ──── L1 ──── L2 ──── L3
    / portal / portal / portal /
   /   边   /   边   /   边   /
  R0 ──── R1 ──── R2 ──── R3

起点 S 加入两个列表的开头
终点 E 加入两个列表的末尾
不允许重复顶点
```

**Step 2：计算每个顶点的相邻边方向**

对每个边界顶点，找到它连接的前一条边和后一条边的方向向量：

```
以 L1 为例：

  L0 ────→ L1 ────→ L2

  dir_prev = normalize(L1 - L0)
  dir_next = normalize(L2 - L1)
```

**Step 3：求平均方向**

```
avg_dir = normalize(dir_prev + dir_next)
```

这给出了顶点处通道边界的"平均朝向"。

**Step 4：取垂线，按半径偏移**

```
offset_dir = perpendicular(avg_dir)  // 垂直方向，指向通道内部
expanded_L1 = L1 + offset_dir * agentRadius
```

对拐角处的效果：

```
直线段处：偏移量 = agentRadius

墙角处：偏移量 > agentRadius（自然被推离墙角更远）
  因为 avg_dir 在拐角处两个方向的平均会"撑开"
  perpendicular 后偏移距离自然增大
  → 大单位不会"削"到墙角
```

**Step 5：用膨胀顶点构建新的 Portal 边**

```
原始 Portal：L0─R0, L1─R1, L2─R2, L3─R3
膨胀 Portal：L0'─R0', L1'─R1', L2'─R2', L3'─R3'

通道两侧各向内缩了 agentRadius，有效通道变窄。
```

**Step 6：在新 Portal 边上跑漏斗算法（Funnel Algorithm）**

标准的 Simple Stupid Funnel Algorithm：
- 输入：膨胀后的 portal 边列表
- 输出：最终路径点序列

漏斗算法在缩小后的通道内找到最短路径，路径点自然与墙壁保持 `agentRadius` 距离。

---

### 3.3 完整流程

```
输入：起点 S，终点 E，单位半径 R

┌─────────────────────────────────────┐
│ 1. 预处理（NavMesh 构建/更新时）      │
│    对每个多边形计算通道宽度            │
└──────────────┬──────────────────────┘
               ▼
┌─────────────────────────────────────┐
│ 2. A* 搜索（带宽度过滤）             │
│    遍历多边形邻接图                   │
│    通道宽度 < R×2 的边直接跳过        │
│    → 输出：多边形通道序列              │
└──────────────┬──────────────────────┘
               ▼
┌─────────────────────────────────────┐
│ 3. 提取左右顶点列表                   │
│    从通道中提取两侧边界顶点           │
└──────────────┬──────────────────────┘
               ▼
┌─────────────────────────────────────┐
│ 4. 膨胀顶点                          │
│    每个边界顶点向内偏移 R             │
│    （平均相邻边方向 → 取垂线 → 偏移） │
└──────────────┬──────────────────────┘
               ▼
┌─────────────────────────────────────┐
│ 5. 构建新 Portal 边                  │
│    用膨胀顶点替换原始顶点             │
└──────────────┬──────────────────────┘
               ▼
┌─────────────────────────────────────┐
│ 6. 漏斗算法                          │
│    在缩小的通道内找最短路径            │
│    → 输出：最终路径点                  │
└──────────────┬──────────────────────┘
               ▼
        单位沿路径移动，边缘不碰墙 ✅
```

---

## 4. Godot 引擎导航系统现状

### 架构概览

```
场景层 (Scene)
  └─ NavigationAgent3D (Node)
  └─ NavigationRegion3D (Node)
  └─ NavigationLink3D (Node)
  └─ NavigationObstacle3D (Node)
     │
服务器层 (Servers)
  └─ NavigationServer3D (Public API)
  └─ GodotNavigationServer3D (实现)
     ├─ NavMap3D (导航地图)
     │  ├─ NavRegion3D[] (导航区域)
     │  ├─ NavLink3D[] (区域连接)
     │  ├─ NavAgent3D[] (移动代理)
     │  └─ NavObstacle3D[] (障碍物)
     ├─ NavMeshQueries3D (寻路查询)
     │  ├─ A* 算法 (多边形图)
     │  └─ Funnel 算法 (路径平滑)
     └─ NavMeshGenerator3D (Recast 集成)
```

### 关键文件

| 文件 | 角色 |
|------|------|
| `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` | A* 搜索 + 漏斗算法实现 |
| `modules/navigation_3d/nav_map_3d.cpp` | 导航地图管理，区域连接构建 |
| `modules/navigation_3d/nav_utils_3d.h` | Polygon、Connection 等核心数据结构 |
| `modules/navigation_3d/3d/nav_mesh_generator_3d.cpp` | Recast 集成，NavMesh 烘焙 |
| `scene/resources/navigation_mesh.h` | NavigationMesh 资源类 |
| `servers/navigation_3d/navigation_server_3d.h` | 公共 API 接口 |

### 当前多体型支持

- **烘焙时**：`agent_radius` 和 `agent_height` 在烘焙时确定，用于 Recast 侵蚀
- **运行时**：`NavAgent3D` 的 radius 仅用于 RVO 碰撞避免，不影响寻路
- **多体型**：需要为每种体型创建独立的 `NavigationMesh` + `NavigationRegion3D`
- **不支持**：运行时通道宽度判断、路径膨胀

### 当前的 NavMesh 数据结构

```cpp
// NavigationMesh 资源
Vector<Vector3> vertices;          // 所有顶点
Vector<Vector<int>> polygons;      // 凸多边形（非三角形，默认最多6顶点）

// 内部运行时结构
struct Polygon {
    uint32_t id;
    LocalVector<Vector3> vertices;
    real_t surface_area;
};

struct Connection {
    Polygon *polygon;
    int edge;
    Vector3 pathway_start;     // Portal 边起点
    Vector3 pathway_end;       // Portal 边终点
};
```

---

## 5. 移植可行性分析

### 技术可行性：✅ 完全可行

**有利因素：**

1. **管线高度对齐**：Godot 已有 A* + 漏斗算法，只需在中间插入宽度过滤和膨胀步骤
2. **Portal 边数据已有**：`Connection` 结构中的 `pathway_start/end` 就是 Portal 边，膨胀后直接替换即可
3. **凸多边形兼容**：Godot 用凸多边形而非三角形，但通道宽度概念完全适用（甚至更简单，因为凸多边形内任意两点连线不穿出多边形）
4. **改动面小**：核心修改集中在 2-3 个文件

### 具体挑战与对策

| 挑战 | 说明 | 对策 |
|------|------|------|
| 凸多边形 vs 三角形 | 论文算法针对三角形 | 方案A：先三角化再算；方案B：直接对凸多边形算（更优） |
| API 扩展 | 当前 `query_path()` 无 agent_radius 参数 | 扩展 `NavigationPathQueryParameters3D` 加入 radius |
| 确定性（联机同步） | 膨胀涉及 normalize（开方） | 如需 lockstep，改用定点数实现 |
| 边缘情况 | 通道宽度恰好等于单位直径时的处理 | 添加小 epsilon 容差 |

### 工作量估算

| 任务 | 预计工时 |
|------|---------|
| 通道宽度预计算 | 2-3 天 |
| A* 宽度过滤 | 0.5 天 |
| 膨胀顶点计算 | 2-3 天 |
| 漏斗算法适配 | 1 天 |
| API 扩展 + 测试 | 2-3 天 |
| **合计** | **约 1-2 周** |

---

## 6. 实现方案

### 推荐路径：先做 GDExtension 插件验证，再考虑合入引擎

#### 阶段一：GDExtension 插件

```
优点：
  - 不修改引擎源码，风险最低
  - 可以直接读取 NavigationMesh 的顶点和多边形数据
  - 自己实现完整的 A* + 宽度过滤 + 膨胀 + 漏斗
  - 快速验证效果

缺点：
  - 无法复用引擎内部的数据结构和优化
  - 性能可能略低于引擎内实现
```

#### 阶段二：引擎源码修改（验证通过后）

关键修改点：

**1. `nav_utils_3d.h` — 数据结构扩展**

```cpp
struct Polygon {
    // ... 现有字段 ...

    // 新增：每对可穿越边之间的通道宽度
    // corridor_widths[edge_pair_index] = max passable diameter
    LocalVector<real_t> corridor_widths;
};
```

**2. `nav_map_3d.cpp` — 预计算通道宽度**

在 NavMap 同步/构建连接关系时，对每个多边形预计算通道宽度。

**3. `nav_mesh_queries_3d.cpp` — A* 宽度过滤**

```cpp
// 在 A* 邻居展开循环中添加：
real_t corridor_width = get_corridor_width(current_poly, neighbor_poly, entry_edge, exit_edge);
if (corridor_width < query_agent_radius * 2.0f) {
    continue; // 通道太窄，跳过
}
```

**4. `nav_mesh_queries_3d.cpp` — 膨胀顶点 + 漏斗**

在现有漏斗算法 `_query_task_post_process_corridorfunnel()` 之前，插入膨胀步骤：

```cpp
void NavMeshQueries3D::_query_task_expand_portals(
    NavMeshPathQueryTask3D &p_query_task, real_t agent_radius) {
    // 1. 提取左右顶点列表
    // 2. 计算每个顶点的平均相邻边方向
    // 3. 取垂线偏移 agent_radius
    // 4. 替换 portal 端点
}
```

**5. `navigation_server_3d.h` — API 扩展**

```cpp
// NavigationPathQueryParameters3D 新增属性
real_t pathfinding_agent_radius = 0.0; 
// 0 = 不启用宽度过滤（向后兼容）
// > 0 = 启用通道宽度过滤 + 路径膨胀
```

### 测试建议

1. **迷宫地图测试**：多条宽窄不同的路线，验证不同体型单位自动选择合适路线
2. **极端情况测试**：通道宽度恰好等于/略大于/略小于单位直径
3. **性能测试**：对比多份 NavMesh 方案与单份方案在 100/500/1000 单位时的寻路性能
4. **动态更新测试**：NavMesh 局部重建后，通道宽度正确重算

---

## 7. 相关知识：缺页中断与内存管理

> 本节记录了在讨论游戏开发中资源加载时涉及的操作系统内存管理知识。

### 缺页中断（Page Fault）

当 CPU 访问虚拟地址，而该地址对应的页面不在物理内存中时，MMU 触发缺页中断，由操作系统处理。

### 硬缺页（Major Page Fault）

页面**不在物理内存中**，需要从磁盘读取。耗时毫秒级。

**游戏场景举例：**
- 开放世界玩家跑到新区域 → mmap 映射的地形数据首次访问 → 硬缺页 → 磁盘 I/O → 卡顿
- 角色纹理首次加载 → 从 .dds 文件读入 → 硬缺页

### 软缺页（Minor Page Fault）

页面**已在物理内存中**但当前进程页表没有映射。不涉及磁盘 I/O，耗时微秒级。

**游戏场景举例：**
- 对象池（bulletPool）延迟分配 → 首次写入触发软缺页 → 分配零页
- 共享库（如 DLL）已被另一个进程加载 → 当前进程首次访问 → 建立映射即可
- fork() 后写时复制 → 软缺页 → 复制页面

### 判断机制

- **CPU（MMU）**：只看页表 Present 位。P=1 直接翻译，P=0 触发中断
- **操作系统**：维护 Page Cache（文件页）、Swap Cache（交换页）等数据结构，判断页面在不在物理内存

### 游戏开发中的应对

| 策略 | 目的 |
|------|------|
| 预加载（Loading Screen） | 避免运行时硬缺页 |
| 内存预热（memset） | 避免运行时软缺页 |
| 流式加载（Streaming） | 分散硬缺页到多帧 |
| Huge Pages（2MB 大页） | 减少缺页总次数 |

---

## 8. 相关知识：BSP 树

> BSP 树在早期游戏引擎中承担渲染排序和碰撞检测的核心角色，其空间划分思想与 NavMesh 寻路密切相关。

### 概念

BSP（Binary Space Partitioning）树用平面递归地将空间一分为二。每个内部节点存一个分割平面，叶子节点存凸空间区域。

### 游戏应用

| 应用 | 说明 |
|------|------|
| 渲染排序（Doom/Quake） | 无 GPU 深度缓冲时代，用 BSP 确定正确的前后渲染顺序 |
| 碰撞检测 | O(log n) 的射线/点查询，用于子弹检测、穿墙检查 |
| PVS 预计算 | 配合 BSP 叶子节点预计算可见性集合，大量剔除不可见几何 |
| 空间音效 / AI 感知 | 利用 BSP 区域划分判断声音传播和视线遮挡 |

### 现代地位

BSP 在现代引擎中逐渐被 BVH（包围体层次）、Octree（八叉树）等更灵活的结构取代，但 Source 引擎等至今仍在使用。其核心思想——**用平面递归二分空间来加速空间查询**——是永不过时的。

---

## 9. 参考资料

1. **Demyen, Douglas Jon.** *Efficient Triangulation-Based Pathfinding.* University of Alberta, 2006.  
   - Section 3.1: Width Calculation  
   - Algorithm 3: Triangle Width 伪代码实现

2. **Anhalt, James.** *Pathing: It's Not a Solved Problem.* GDC, Blizzard Entertainment (StarCraft 2).  
   - 膨胀顶点方法的来源

3. **jdtec.** *RTS Pathfinding 3: Variable agent size, smoke tests & Navmesh fixes.* December 7, 2021.  
   - 本文档的直接参考，实际实现了 SC2 方案

4. **Mikko Mononen.** *Recast & Detour Navigation Toolkit.*  
   - Godot 引擎集成的 NavMesh 生成库  
   - https://github.com/recastnavigation/recastnavigation

5. **Godot Engine 源码**  
   - `modules/navigation_3d/` — 导航系统核心实现
   - `modules/navigation_3d/3d/nav_mesh_queries_3d.cpp` — A* + Funnel 算法

---

> **文档结论**：单一 NavMesh + 三角形宽度 + 膨胀顶点的方案，技术上完全适合移植进 Godot。推荐先以 GDExtension 插件验证效果，再考虑合入引擎源码。核心改动集中在 `nav_mesh_queries_3d.cpp` 和 `nav_map_3d.cpp`，工作量约 1-2 周。
