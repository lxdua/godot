# Godot 导航系统改进方案：Tile 化增量更新

> **目标**：将 Godot 当前的全量重建导航架构改为 Tile 化增量更新，并引入交错偏移（Staggered Tile）布局以进一步减少每次更新涉及的 Tile 数量。

---

## 1. 现状分析

### 1.1 Godot 当前架构

Godot 的导航地图（`NavMap3D`）采用 **全量重建** 策略：

- 地图由多个 `NavRegion3D` 组成，每个 Region 包含一组多边形
- 任何一个 Region 发生变化 → 标记整个 Map 为 `iteration_dirty`
- 重建时收集 **所有** Region 的 **所有** 多边形，重新做边连接
- 双缓冲（ping-pong）机制保证查询不阻塞，但重建本身是全量的

**核心代码路径**：
```
NavMap3D::sync()
  → _sync_dirty_map_update_requests()     // 遍历所有 dirty region
      → iteration_dirty = true            // 任何 region 变了就标记整个 map
  → _build_iteration()
      → NavMapBuilder3D::build_navmap_iteration()
          → _build_step_gather_region_polygons()       // 收集【全部】多边形
          → _build_step_find_edge_connection_pairs()   // 重建【全部】边连接
          → _build_step_merge_edge_connection_pairs()
          → _build_step_edge_connection_margin_connections()
          → _build_step_navlink_connections()
          → _build_update_map_iteration()
```

### 1.2 UE 的 Tile 架构（参考）

UE 的 NavigationSystem 集成了 Recast-Detour 的 **Tile-based NavMesh**：

- 世界按固定大小的 Tile 网格划分
- 变化只标记受影响的 Tile（通过 Dirty Area AABB）
- 只重建被标记的 Tile，其他 Tile 保持不变
- 支持分帧渐进式重建（每帧处理若干个 Tile）
- 支持 NavMesh Carving（障碍物在 NavMesh 上打洞）
- 支持 Tile 级别的流式加载/卸载

### 1.3 异步更新策略对比：双缓冲 vs Tile 原子替换

Godot 和 UE 都支持异步重建，但异步策略截然不同：

#### Godot：异步重建 + 双缓冲无锁读取

```
主线程                          工作线程
  │                               │
  ├─ sync(): 发现 dirty           │
  ├─ 提交任务给 WorkerThreadPool ──► 开始全量重建（往 slot B 写）
  ├─ 立刻返回（不卡）              │
  │                               │
  ├─ Agent 查路径 → 读 slot A     │   还在建...
  ├─ Agent 查路径 → 读 slot A     │   还在建...
  │                               │
  ├─ sync(): 检查完成 ◄────────── ├─ 建完了！
  ├─ 切换 slot: A → B             │
  │                               │
  ├─ Agent 查路径 → 读 slot B     │
```

- 两个 `NavMapIteration3D` 槽（`iteration_slots[2]`），通过 `iteration_slot_index` 原子切换
- 查询线程读当前活跃槽，完全不需要任何锁
- 必须等全部重建完成才能切换 → **延迟较高**
- 两份完整数据副本 → **内存 ×2**

#### UE：异步 Tile 重建 + 单数据原子替换

```
主线程                          工作线程
  │                               │
  ├─ 标记 dirty tiles             │
  ├─ 提交 Tile 重建任务 ─────────► 重建 Tile A, B, C
  │                               │
  ├─ 下一帧 sync                  │
  │   检查完成的 Tile ◄──────────── Tile A, B 完成（C 还在建）
  │   短暂加锁，替换 Tile A, B    │
  │   到 NavMesh 主数据中          │
  │                               │
  ├─ Agent 查路径                  │
  │   → 读到 A,B 已更新，C 是旧的  │
```

- 只有一份 NavMesh 主数据
- Tile 重建完一个就替换一个 → **延迟低**（不用等全部完成）
- 替换 Tile 时需要短暂加锁 → 查询可能等几微秒
- 查询可能看到"部分更新"状态（A,B 新 + C 旧）→ **短暂不一致**
- 只有正在重建的 Tile 有临时副本 → **内存开销小**

#### 对比总结

| 维度 | Godot（双缓冲） | UE（Tile 原子替换） |
|------|-----------------|-------------------|
| **缓冲数量** | 2 份完整副本 | 1 份主数据 + dirty tile 临时副本 |
| **查询时加锁** | ❌ 完全不需要 | ⚠️ 替换 Tile 时短暂锁 |
| **数据一致性** | ✅ 查到的永远是完整一致的版本 | ⚠️ 可能看到部分新+部分旧 |
| **内存开销** | 较大（整张图 ×2） | 较小（仅 dirty tile ×2） |
| **更新粒度** | 全部完成才切换 | 逐 Tile 生效 |
| **更新延迟** | 高（等全量重建完） | 低（Tile 建完即可用） |
| **多线程查询性能** | ✅ 极佳（零锁竞争） | 良好（锁粒度小，竞争少） |

#### 本方案的策略：双缓冲 + Tile 增量（两者结合）

本方案保留 Godot 的双缓冲架构的零锁查询优势，同时引入 Tile 化增量重建：

```
  前台 slot（Agent 在读）          后台 slot（正在重建）
  ┌──┬──┬──┬──┐                   ┌──┬──┬──┬──┐
  │  │  │  │  │                   │  │██│  │  │
  ├──┼──┼──┼──┤    只重建 dirty   ├──┼──┼──┼──┤
  │  │  │  │  │    ──────────►    │  │██│██│  │  ██ = 重建的 Tile
  ├──┼──┼──┼──┤    tile           ├──┼──┼──┼──┤
  │  │  │  │  │                   │  │  │  │  │  其他 Tile 共享 Ref
  └──┴──┴──┘──┘                   └──┴──┴──┴──┘
                                        │
                建完 → 原子切换 slot ◄───┘
```

- **查询零锁**：保留双缓冲，查询端完全无锁
- **内存不翻倍**：未修改的 Tile 在两个 slot 间通过 `Ref<NavTileData3D>` 共享
- **增量重建**：后台 slot 只重建 dirty tile + 更新跨 Tile 连接
- **完全一致**：切换是原子的，查询端看到的永远是完整版本
- **延迟折中**：需等所有 dirty tile 建完才切换（但只建几个 Tile，远快于全量重建）

### 1.4 差距总结

| 维度 | Godot | UE |
|------|-------|-----|
| 最小重建单元 | 整张地图 | 单个 Tile |
| 改一个 Region | 重建全部 | 只重建覆盖的 Tile |
| 大世界支持 | 不支持流式 | Tile 流式加载 |
| 动态障碍物 | 不影响寻路图 | Carving 打洞 |
| 更新延迟 | 全量重建耗时长 | 增量更新快 |
| 异步策略 | 双缓冲无锁（高内存） | Tile 原子替换（低延迟） |
| 查询时的锁 | 无 | 短暂锁 |

---

## 2. 设计方案

### 2.1 Tile 网格划分

#### 2.1.1 基本概念

将导航世界的 XZ 平面按固定尺寸 `tile_size` 划分为二维网格。每个 Tile 是一个独立的导航子图，包含落在其范围内的所有多边形。

```
参数：
  tile_size: float = 32.0    // 每个 Tile 的边长（世界单位）
  tile_origin: Vector3       // Tile 网格的原点（通常为地图 AABB 最小角）
```

#### 2.1.2 交错偏移布局（Staggered Tile）★ 改进点

**标准对齐布局**的问题：

在标准网格中，四个 Tile 共享一个角点。当一个障碍物（或变化区域）恰好落在这个角点附近时，会同时影响 4 个 Tile：

```
标准对齐布局：

    col 0    col 1    col 2
   ┌────────┬────────┬────────┐
   │        │        │        │  row 0
   │   T00  │  T10   │  T20   │
   │        │●───────●        │   ← 障碍物落在角点
   ├────────┼────────┼────────┤      影响 4 个 Tile:
   │        │        │        │  row 1   T00, T10, T01, T11
   │   T01  │  T11   │  T21   │
   │        │        │        │
   ├────────┼────────┼────────┤
   │        │        │        │  row 2
   │   T02  │  T12   │  T22   │
   │        │        │        │
   └────────┴────────┴────────┘

   四 Tile 交叉点（worst case = 4 tiles）：
   每个内部顶点被 4 个 Tile 共享
```

**交错偏移布局**（本方案采用）：

将奇数行（或偶数行）在 X 方向偏移 `tile_size / 2`，形成类似砖墙的排列：

```
交错偏移布局：

    ┌────────┬────────┬────────┐
    │        │        │        │  row 0 （无偏移）
    │  T00   │  T10   │  T20   │
    │        │        │        │
    ├───┬────┴───┬────┴───┬────┤
    │   │        │        │    │  row 1 （偏移 tile_size/2）
    │   │  T01   │  T11   │   │
    │   │        │        │    │
    ├───┴────┬───┴────┬───┴────┤
    │        │        │        │  row 2 （无偏移）
    │  T02   │  T12   │  T22   │
    │        │        │        │
    └────────┴────────┴────────┘

    三 Tile 交叉点（worst case = 3 tiles）：
    每个内部顶点被 3 个 Tile 共享，而不是 4 个
```

#### 2.1.3 几何证明：为什么最多 3 个

在标准对齐网格中，水平线和垂直线同时在同一点相交，形成 **十字交叉**，一个点被 4 个 Tile 围住。

在交错布局中，相邻行的垂直分割线 **错开了半个 Tile**，不再同时交汇于一点。任何一个分割线的交汇处，只有 **T 字形交叉**（三条边交汇），而不是十字形（四条边交汇）：

```
标准布局 — 十字交叉（4 tiles）：    交错布局 — T字交叉（3 tiles）：

      │                                    │
  ────┼────                            ────┤
      │                                    │
   4 tiles meet                         3 tiles meet
```

**量化收益**：对于一个不超过约 `tile_size / 2` 大小的障碍物或变化区域：

| 布局 | 最坏情况影响 Tile 数 | 平均情况 |
|------|---------------------|---------|
| 标准对齐 | 4 | ~2.5 |
| 交错偏移 | 3 | ~2.2 |
| **减少** | **25%** | **~12%** |

对于更大的变化区域（接近 `tile_size`），两种布局差异缩小。但对于常见的小型动态障碍物（门、箱子、可破坏物），这个优化很有意义，因为它们通常远小于一个 Tile。

#### 2.1.4 Tile 坐标计算

```cpp
// 标准对齐（参考）：
int tile_x = (int)Math::floor((world_x - origin_x) / tile_size);
int tile_z = (int)Math::floor((world_z - origin_z) / tile_size);

// 交错偏移：
int tile_z = (int)Math::floor((world_z - origin_z) / tile_size);
float offset_x = (tile_z % 2 == 1) ? tile_size * 0.5f : 0.0f;
int tile_x = (int)Math::floor((world_x - origin_x - offset_x) / tile_size);
```

**查询"一个 AABB 覆盖了哪些 Tile"**：

```cpp
// 输入：dirty_aabb (变化区域的包围盒)
// 输出：affected_tiles (需要重建的 Tile 列表)

void get_affected_tiles(const AABB &dirty_aabb, LocalVector<TileCoord> &affected_tiles) {
    // 计算 Z 方向覆盖的行范围
    int min_row = Math::floor((dirty_aabb.position.z - origin_z) / tile_size);
    int max_row = Math::floor((dirty_aabb.get_end().z - origin_z) / tile_size);

    for (int row = min_row; row <= max_row; row++) {
        // 根据行号计算 X 偏移
        float offset_x = (row % 2 == 1) ? tile_size * 0.5f : 0.0f;

        int min_col = Math::floor((dirty_aabb.position.x - origin_x - offset_x) / tile_size);
        int max_col = Math::floor((dirty_aabb.get_end().x - origin_x - offset_x) / tile_size);

        for (int col = min_col; col <= max_col; col++) {
            affected_tiles.push_back(TileCoord{col, row});
        }
    }
}
```

---

### 2.2 核心数据结构

#### 2.2.1 NavTile3D

```cpp
struct NavTile3D {
    TileCoord coord;                    // {col, row}
    AABB bounds;                        // 该 Tile 的世界空间包围盒
    
    // 内部多边形数据
    LocalVector<NavPolygon3D> polygons;
    LocalVector<NavEdge3D> internal_edges;    // Tile 内部的边
    LocalVector<NavEdge3D> border_edges;      // Tile 边界上的边（需要跨 Tile 连接）
    
    // 跨 Tile 连接
    LocalVector<NavConnection3D> cross_tile_connections;
    
    // 状态
    bool dirty = false;
    uint32_t build_version = 0;
};
```

#### 2.2.2 NavTileGrid3D

```cpp
class NavTileGrid3D {
    float tile_size = 32.0f;
    bool use_staggered_layout = true;    // 是否使用交错偏移
    Vector3 origin;
    
    HashMap<TileCoord, NavTile3D *, TileCoordHasher> tiles;
    
    // 从世界坐标获取 Tile
    NavTile3D *get_tile_at(const Vector3 &world_pos) const;
    
    // 获取 AABB 覆盖的所有 Tile
    void get_affected_tiles(const AABB &aabb, LocalVector<NavTile3D *> &result) const;
    
    // 获取相邻 Tile（用于跨 Tile 边连接）
    void get_neighbor_tiles(const TileCoord &coord, LocalVector<NavTile3D *> &neighbors) const;
};
```

#### 2.2.3 交错布局的邻居关系

标准网格中每个 Tile 有 4 个邻居（上下左右），交错布局中每个 Tile 有 **6 个邻居**：

```
偶数行的 Tile (col, row) 的邻居：

  (col-1, row-1)  (col, row-1)
          ╲        ╱
    (col-1, row) ─ SELF ─ (col+1, row)
          ╱        ╲
  (col-1, row+1)  (col, row+1)

奇数行的 Tile (col, row) 的邻居：

  (col, row-1)  (col+1, row-1)
          ╲        ╱
    (col-1, row) ─ SELF ─ (col+1, row)
          ╱        ╲
  (col, row+1)  (col+1, row+1)
```

这比标准 4 邻居稍复杂，但实现上只是查表差异，不增加算法复杂度。

---

### 2.3 增量更新流程

#### 2.3.1 整体流程（替换现有的全量重建）

```
NavMap3D::sync()
  │
  ├─ _sync_dirty_map_update_requests()
  │    │
  │    ├─ 遍历 dirty regions
  │    │   对每个 dirty region:
  │    │     计算变化区域的 AABB (dirty_aabb)
  │    │     调用 tile_grid.get_affected_tiles(dirty_aabb, affected)
  │    │     标记 affected tiles 为 dirty
  │    │
  │    └─ 遍历 dirty links（同理）
  │
  ├─ _build_dirty_tiles()                    ★ 替换原来的 _build_iteration()
  │    │
  │    ├─ 收集所有 dirty tiles → dirty_list
  │    │
  │    ├─ 分帧预算（可选）：
  │    │   max_tiles_per_frame = 4
  │    │   取 dirty_list 前 N 个处理，其余留到下一帧
  │    │
  │    ├─ 对每个 dirty tile:
  │    │   ├─ 1. 收集落在该 Tile 范围内的多边形
  │    │   ├─ 2. 重建 Tile 内部边连接
  │    │   ├─ 3. 标识边界边（border_edges）
  │    │   └─ 4. tile.dirty = false; tile.build_version++
  │    │
  │    └─ 重建受影响的跨 Tile 连接：
  │        对每个刚重建的 Tile:
  │          获取其 6 个邻居 Tile
  │          重新匹配 border_edges ↔ 邻居的 border_edges
  │          更新 cross_tile_connections
  │
  └─ _sync_iteration()                      // ping-pong 切换（保留）
```

#### 2.3.2 分帧渐进式重建

```cpp
// nav_map_3d.h 新增
int max_tiles_per_sync = 4;              // 每帧最多处理的 Tile 数
LocalVector<NavTile3D *> pending_dirty_tiles;  // 待处理队列

// nav_map_3d.cpp
void NavMap3D::_build_dirty_tiles() {
    // 将新标记的 dirty tiles 加入队列
    for (auto &[coord, tile] : tile_grid.tiles) {
        if (tile->dirty && !pending_dirty_tiles.has(tile)) {
            pending_dirty_tiles.push_back(tile);
        }
    }
    
    if (pending_dirty_tiles.is_empty()) {
        return;
    }
    
    // 每帧只处理有限数量
    int count = MIN(max_tiles_per_sync, pending_dirty_tiles.size());
    
    for (int i = 0; i < count; i++) {
        NavTile3D *tile = pending_dirty_tiles[i];
        _rebuild_single_tile(tile);
    }
    
    // 移除已处理的
    pending_dirty_tiles.remove_at(0, count);  // 批量移除前 count 个
    
    // 重建跨 Tile 连接（只针对刚重建的 Tile）
    _rebuild_cross_tile_connections(rebuilt_tiles);
    
    // 合并到 iteration
    _update_iteration_from_tiles();
    
    iteration_ready = true;
}
```

#### 2.3.3 多边形分配到 Tile

一个多边形可能跨越多个 Tile。处理策略：

**方案 A：按质心归属（推荐）**
```cpp
// 简单高效，每个多边形只属于一个 Tile
TileCoord tile_for_polygon(const NavPolygon3D &poly) {
    Vector3 centroid = poly.get_centroid();
    return tile_grid.world_to_tile(centroid);
}
```

**方案 B：按 AABB 归属到所有覆盖的 Tile**
- 一个多边形可能被多个 Tile 引用
- 更精确但增加了复杂度和内存

推荐先用方案 A，足以满足大部分场景。

---

### 2.4 跨 Tile 边连接

这是 Tile 化方案中最关键的部分。当两个多边形分属不同 Tile 时，它们的共享边需要通过 **跨 Tile 连接** 来维护。

```
Tile A                    Tile B
┌──────────────────┐┌──────────────────┐
│         ╱╲       ││       ╱╲         │
│        ╱  ╲      ││      ╱  ╲        │
│       ╱ P1 ╲     ││     ╱ P2 ╲       │
│      ╱      ╲    ││    ╱      ╲      │
│     ╱────────●===●●===●────────╲     │
│              ▲  border edges  ▲      │
│              │                │       │
└──────────────┘                └───────┘
         Tile A 的边界边    Tile B 的边界边
         通过 edge merge 连接
```

#### 跨 Tile 连接重建规则

当 Tile X 被重建时：
1. 清除所有涉及 Tile X 的 cross_tile_connections
2. 获取 Tile X 的 6 个邻居
3. 对每个邻居 Tile Y：
   - 将 X 的 border_edges 与 Y 的 border_edges 做匹配（复用现有的 edge merge 算法）
   - 生成新的 cross_tile_connections
4. **不需要重建邻居 Tile 的内部结构**，只更新连接

这意味着重建 1 个 Tile 时，需要读取（但不重建）其邻居的 border_edges，开销很小。

---

### 2.5 与现有双缓冲机制的集成

保留现有的 ping-pong 双缓冲，但修改其粒度：

```
当前架构：
  iteration_slots[2] → 每个 slot 存整张地图的完整副本

改进架构：
  iteration_slots[2] → 每个 slot 存 tile_grid 的引用
  
  Tile 重建时：
    1. 在后台 slot 中替换被重建的 Tile 数据
    2. 未变化的 Tile 在两个 slot 间共享引用（Ref<NavTileData3D>）
    3. 切换 slot 时只交换 index，大部分 Tile 数据被共享

  好处：内存不会翻倍（只有被修改的 Tile 需要两份副本）
```

```cpp
struct NavMapIteration3D {
    // 改前：
    // LocalVector<Ref<NavRegionIteration3D>> region_iterations;
    
    // 改后：
    Ref<NavTileGrid3D> tile_grid;    // 包含所有 Tile 的引用
    // 每个 Tile 是 Ref 计数的，两个 slot 可以共享未修改的 Tile
};
```

---

## 3. 交错偏移的额外考量

### 3.1 Debug 可视化

交错布局的 Tile 网格线不再是简单的等距直线，Debug 绘制时需要处理偏移：

```cpp
void draw_tile_grid_debug() {
    for (auto &[coord, tile] : tile_grid.tiles) {
        float offset_x = (coord.row % 2 == 1) ? tile_size * 0.5f : 0.0f;
        
        Vector3 min_corner(
            origin.x + coord.col * tile_size + offset_x,
            0,
            origin.z + coord.row * tile_size
        );
        Vector3 max_corner = min_corner + Vector3(tile_size, 0, tile_size);
        
        draw_rect(min_corner, max_corner, Color::CYAN);
    }
}
```

### 3.2 边界情况：世界边缘的半 Tile

交错偏移会导致奇数行的左右两端各出现一个 **半宽 Tile**：

```
    ┌────────┬────────┬────────┐
    │ full   │ full   │ full   │  row 0
    ├───┬────┴───┬────┴───┬────┤
    │ ½ │  full  │  full  │ ½  │  row 1  ← 左右各有半个
    ├───┴────┬───┴────┬───┴────┤
    │ full   │ full   │ full   │  row 2
    └────────┴────────┴────────┘
```

处理方案：
- 用 HashMap 存储 Tile（而非固定二维数组），半 Tile 自然存在
- 或者将世界范围在 X 方向各扩展 `tile_size / 2`，保证所有 Tile 都是完整的

### 3.3 配置选项

```cpp
// NavigationServer3D 新增设置
"navigation/world/tile_size"             // float, 默认 32.0
"navigation/world/use_staggered_tiles"   // bool, 默认 true
"navigation/world/max_tiles_per_sync"    // int, 默认 4
```

用户可以选择关闭交错偏移（回退到标准网格），以简化调试或适配特殊需求。

---

## 4. 实现路线图

### Phase 1：Tile 基础设施（不改变外部行为）

**目标**：引入 Tile 网格，但重建逻辑仍为全量，用于验证 Tile 划分的正确性。

| 步骤 | 工作内容 | 涉及文件 |
|------|---------|---------|
| 1.1 | 定义 `NavTile3D`、`NavTileGrid3D` 数据结构 | 新建 `nav_tile_3d.h/cpp` |
| 1.2 | 在 `NavMapBuilder3D` 中加入 Tile 划分逻辑 | `nav_map_builder_3d.cpp` |
| 1.3 | 实现交错偏移坐标计算 | `nav_tile_3d.cpp` |
| 1.4 | Debug 绘制 Tile 网格 | `nav_map_3d.cpp` 或 debug 渲染 |
| 1.5 | 验证：所有多边形正确归属到 Tile，跨 Tile 边连接正确 | 测试 |

### Phase 2：增量重建

**目标**：只重建 dirty Tile 而非整张图。

| 步骤 | 工作内容 | 涉及文件 |
|------|---------|---------|
| 2.1 | 实现 dirty area → affected tiles 映射 | `nav_tile_3d.cpp` |
| 2.2 | 实现 `_rebuild_single_tile()` | `nav_map_builder_3d.cpp` |
| 2.3 | 实现跨 Tile 边连接增量更新 | `nav_map_builder_3d.cpp` |
| 2.4 | 替换 `_build_iteration()` 为 `_build_dirty_tiles()` | `nav_map_3d.cpp` |
| 2.5 | 修改双缓冲机制支持 Tile 级别共享 | `nav_map_iteration_3d.h` |

### Phase 3：分帧渐进 + 性能优化

**目标**：平滑重建开销，优化大地图性能。

| 步骤 | 工作内容 |
|------|---------|
| 3.1 | 实现分帧 Tile 队列（`max_tiles_per_sync`） |
| 3.2 | Tile 优先级排序（距离玩家近的优先） |
| 3.3 | 性能对比测试：全量 vs 增量 vs 增量+交错 |

### Phase 4（可选）：NavMesh Carving

**目标**：让动态障碍物能在 NavMesh 上打洞。

| 步骤 | 工作内容 |
|------|---------|
| 4.1 | 障碍物 AABB → 标记覆盖的 Tile 为 dirty |
| 4.2 | 在 Tile 重建时裁剪掉障碍物区域的多边形 |
| 4.3 | 与 `impl_dynamic_obstacle_pathfinding.md` 方案整合 |

---

## 5. 性能预估

### 测试场景假设

- 地图大小：256m × 256m
- Tile 大小：32m
- 总 Tile 数（标准）：8 × 8 = 64
- 总 Tile 数（交错）：约 64-68（含边缘半 Tile）
- 多边形总数：~5000

### 重建开销对比

| 场景 | 全量重建 | Tile 增量（标准） | Tile 增量（交错） |
|------|---------|-----------------|-----------------|
| 移动 1 个小障碍物 | 重建 5000 多边形 | 重建 ~4 个 Tile ≈ 320 多边形 | 重建 ~3 个 Tile ≈ 240 多边形 |
| 打开一扇门 | 重建 5000 多边形 | 重建 ~1-2 个 Tile ≈ 80-160 多边形 | 重建 ~1-2 个 Tile ≈ 80-160 多边形 |
| 大规模地形变化 | 重建 5000 多边形 | 重建 ~10-16 个 Tile ≈ 800-1300 多边形 | 重建 ~10-14 个 Tile ≈ 800-1100 多边形 |
| **加速比** | 1x | **~6-15x** | **~7-20x** |

### 交错偏移的额外收益

对于"小障碍物落在角点"这个最坏情况：

```
标准：  4 tiles × ~80 polygons/tile = 320 polygons 需要重建
交错：  3 tiles × ~80 polygons/tile = 240 polygons 需要重建
节省：  25% 的重建工作量
```

这个 25% 虽然不是革命性的，但它是 **免费的**——只需要在坐标计算时加一个偏移量，几乎零运行时开销。

---

## 6. 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 跨 Tile 边连接的正确性 | 路径不连通或出现缝隙 | 复用现有 edge merge 算法；充分的自动化测试 |
| 交错布局增加坐标计算复杂度 | 代码可读性下降 | 封装在 `NavTileGrid3D` 内部，对外暴露统一接口 |
| 多边形跨 Tile 边界的归属 | 边界多边形的连接可能不稳定 | 方案 A（质心归属）+ 扩展 border_edges 搜索范围 |
| 分帧重建导致短暂不一致 | 相邻 Tile 版本不同步 | 跨 Tile 连接在最后一个相关 Tile 重建完后统一刷新 |
| 与现有 Region 系统的兼容 | Region 跨多个 Tile | Region 提供多边形，Tile 是 Map 层面的组织方式，两者正交 |

---

## 7. 总结

本方案通过两个层次的改进来解决 Godot 导航系统的全量重建瓶颈：

1. **Tile 化增量更新**（核心价值）：将 O(N) 全量重建降为 O(k) 局部重建（k = 受影响的 Tile 数），对于典型的小变化场景可获得 6-20 倍加速
2. **交错偏移布局**（锦上添花）：通过消除四 Tile 交叉点，将最坏情况下的影响 Tile 数从 4 降为 3，额外减少 25% 重建工作量，且实现成本接近为零

两者结合，使 Godot 的导航系统能够高效处理动态场景变化，为后续的 NavMesh Carving 和大世界流式加载打下基础。
