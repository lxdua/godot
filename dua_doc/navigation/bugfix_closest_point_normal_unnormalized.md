# Bug 修复记录：closest_point_normal 返回未归一化向量

> **发现日期**：2025-04-20  
> **发现方式**：BVH 空间索引测试脚本 (Test Group 2: get_closest_point_normal)  
> **严重性**：Medium — 功能性错误，影响所有使用 `map_get_closest_point_normal()` 的用户代码  
> **分类**：Godot 原有 Bug（非 BVH 改动引入）

---

## 现象

测试脚本在水平 NavMesh 上查询法线时：

```
❌ FAIL: 2.1 水平 NavMesh 法线 → (0, 1, 0) | expected=(0.0, 1.0, 0.0) actual=(0.0, 400.0, 0.0)
❌ FAIL: 2.2 法线长度 = 1.0 | expected=1.0 actual=400.0
❌ FAIL: 2.3 Elevated Region 法线 → (0, 1, 0) | expected=(0.0, 1.0, 0.0) actual=(0.0, 50.0, 0.0)
```

法线方向正确（Y 轴正方向），但 **长度不为 1.0**，而是与三角形面积成正比。

## 根因分析

### 叉积的数学性质

```cpp
Vector3 plane_normal = (v1 - v0).cross(v2 - v0);
```

叉积 `a × b` 的长度 = `|a| * |b| * sin(θ)` = **2 × 三角形面积**。

| NavMesh | 三角形顶点 | 面积 | 叉积长度 | 返回的"法线"长度 |
|---------|-----------|------|---------|----------------|
| Main Region (20×20) | `(-10,0,-10), (-10,0,10), (10,0,10)` | 200 | 400 | **400.0** |
| Elevated Region (10×5) | `(-5,3,15), (-5,3,20), (5,3,20)` | 25 | 50 | **50.0** |

### 代码错误位置

文件：`modules/navigation_3d/3d/nav_mesh_queries_3d.cpp`

函数中先计算了归一化法线用于距离投影，但在赋值给 `result.normal` 时却用了**未归一化**的原始叉积：

```cpp
// 第 1121 行 —— 正确计算了归一化法线
Vector3 plane_normalized = plane_normal.normalized();
real_t distance = plane_normalized.dot(point - polygon->vertices[0]);
// ...
// 第 1127 行 —— ❌ 赋值时用了未归一化的 plane_normal
result.normal = plane_normal;  // BUG：长度 ≠ 1.0
```

### 受影响的代码路径

共 **4 处**，分布在 2 个函数中：

| 序号 | 函数 | 行号（修复前） | 分支 | 说明 |
|------|------|--------------|------|------|
| 1 | `map_iteration_get_closest_point_info` (BVH 版) | 1127 | `inside == true` | 点在多边形内部投影 |
| 2 | `map_iteration_get_closest_point_info` (BVH 版) | 1139 | `inside == false` | 点在多边形外部，最近点在边上 |
| 3 | `polygons_get_closest_point_info` (暴力遍历版) | 1357 | `inside == true` | 同 #1 |
| 4 | `polygons_get_closest_point_info` (暴力遍历版) | 1369 | `inside == false` | 同 #2 |

> **注意**：`polygons_get_closest_point_info` 是 Godot 原有的暴力遍历实现，说明这个 bug **在 BVH 改动之前就已存在**。BVH 版本的代码是参照原版逻辑编写的，因此继承了同样的错误。

## 修复方案

### inside == true 分支

该分支中已有 `plane_normalized` 变量（用于距离计算），直接复用：

```cpp
// 修复前
result.normal = plane_normal;

// 修复后
result.normal = plane_normalized;  // ✅ 复用已有的归一化结果，零额外开销
```

### inside == false 分支

该分支中没有预计算的归一化向量，需要显式调用：

```cpp
// 修复前
result.normal = plane_normal;

// 修复后
result.normal = plane_normal.normalized();  // ✅ 新增一次归一化调用
```

### 性能影响

| 分支 | 修复方式 | 额外开销 |
|------|---------|---------|
| `inside == true` | 复用 `plane_normalized` | **零**（变量已存在） |
| `inside == false` | `plane_normal.normalized()` | 1 次 `sqrt` + 3 次除法（≈ 5 ns） |

整体性能影响**可忽略**。

## 影响范围

### 受影响的公开 API

| API | 说明 |
|-----|------|
| `NavigationServer3D.map_get_closest_point_normal(map, point)` | 直接返回 `result.normal` |
| `NavigationServer3D.region_get_closest_point_normal(region, point)` | 调用暴力遍历版 |

### 对用户代码的影响

任何依赖 `map_get_closest_point_normal()` 返回**单位向量**的代码都会受到影响。常见用例：

```gdscript
# 用法 1：计算反射方向 —— ❌ 结果错误（法线放大导致反射向量异常）
var normal = NavigationServer3D.map_get_closest_point_normal(map, point)
var reflection = velocity.reflect(normal)

# 用法 2：沿法线偏移 —— ❌ 偏移量被放大数百倍
var offset_point = closest_point + normal * 0.1

# 用法 3：判断坡度 —— ✅ 方向正确，但需要先手动归一化
var slope_angle = normal.angle_to(Vector3.UP)  # 如果未归一化则结果不确定
```

### 为什么之前没被发现

1. **法线方向始终正确** —— 未归一化不影响方向，只影响长度
2. **很少有用户直接调用这个 API** —— 大多数用户使用 `NavigationAgent3D`，不直接操作 `NavigationServer3D`
3. **单位网格掩盖问题** —— 如果 NavMesh 的三角形面积恰好为 0.5（如边长为 1 的直角三角形），叉积长度 = 1.0，看起来像是归一化的

## 验证

修复后测试结果：

```
--- Test Group 2: get_closest_point_normal ---
  ✅ PASS: 2.1 水平 NavMesh 法线 → (0, 1, 0)
  ✅ PASS: 2.2 法线长度 = 1.0
  ✅ PASS: 2.3 Elevated Region 法线 → (0, 1, 0)
```
