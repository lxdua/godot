# Inverted Hull 屏幕空间等宽描边

> 改进 Godot 现有的 `BaseMaterial3D.grow` 描边机制，新增**屏幕空间等宽**模式，使描边线宽不受透视距离影响，解决近大远小的经典问题。同时用于改进 `STENCIL_MODE_OUTLINE` 的描边质量。

---

## 一、问题分析

### 现有实现

Godot 当前的 Grow 描边在 `scene/resources/material.cpp:1463-1467` 生成如下 Vertex Shader 代码：

```glsl
// Grow: Enabled
VERTEX += NORMAL * grow;
```

当 `STENCIL_MODE_OUTLINE` 启用时（`material.cpp:3188-3201`），引擎自动创建一个 Next Pass 材质，开启 Grow + Stencil NOT_EQUAL 测试来实现描边。

### 核心缺陷

1. **近大远小**：`VERTEX += NORMAL * grow` 是在模型空间（Object Space）操作的，膨胀量是固定的世界空间距离（单位：米）。离摄像机近时描边占很多像素，远时几乎看不到。
2. **透视畸变**：在透视投影下，屏幕边缘的物体描边宽度与中心不同。
3. **硬边断裂**：法线不连续处（硬边、UV 接缝）描边会断开——此问题本方案不解决，但后续可通过"平滑法线烘焙"补充。

### UE 中的做法

UE 社区的改进版 Inverted Hull 通常在 **Clip Space（裁剪空间）** 操作：

```hlsl
float4 clipPos = mul(float4(worldPos, 1), ViewProjection);
float3 clipNormal = mul(normal, (float3x3)ViewProjection);
clipNormal.z = 0;  // 保留 XY 方向
clipPos.xy += normalize(clipNormal.xy) * OutlineWidth * clipPos.w;
```

关键点：乘以 `clipPos.w` 抵消透视除法，使得最终 NDC 空间中偏移量恒定 → 屏幕上等宽。

---

## 二、设计方案

### 改动范围

在 `BaseMaterial3D` 中新增一个 **Grow Mode** 枚举，控制 grow 的膨胀模式：

| 枚举值 | 名称 | 行为 |
|--------|------|------|
| `GROW_MODE_OBJECT_SPACE` | 模型空间（默认） | 现有行为：`VERTEX += NORMAL * grow` |
| `GROW_MODE_SCREEN_SPACE` | 屏幕空间等宽 | 在裁剪空间操作，描边像素宽度恒定 |

### Shader 代码生成

#### 模型空间模式（不变）
```glsl
// Grow: Object Space
VERTEX += NORMAL * grow;
```

#### 屏幕空间等宽模式（新增）
```glsl
// Grow: Screen Space
{
    vec4 clip_vertex = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
    vec3 clip_normal = mat3(PROJECTION_MATRIX) * mat3(MODELVIEW_MATRIX) * NORMAL;
    clip_normal.z = 0.0;
    vec2 offset = normalize(clip_normal.xy) * grow * clip_vertex.w;
    clip_vertex.xy += offset;
    vec4 new_vertex = inverse(MODELVIEW_MATRIX) * inverse(PROJECTION_MATRIX) * clip_vertex;
    VERTEX = new_vertex.xyz / new_vertex.w;
}
```

> **注意**：`grow` 值在屏幕空间模式下的含义从"世界空间距离（米）"变为"NDC 空间偏移量"。0.01 约等于屏幕宽度的 1%。

#### 更优的实现（避免矩阵求逆）

Godot Shader Language 的 `vertex()` 函数可以直接修改 `POSITION`（裁剪空间输出），无需逆变换：

```glsl
// Grow: Screen Space (Optimized)
{
    vec4 clip_vertex = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
    vec3 view_normal = mat3(MODELVIEW_MATRIX) * NORMAL;
    vec2 proj_normal = mat2(PROJECTION_MATRIX) * view_normal.xy;
    proj_normal = normalize(proj_normal);
    clip_vertex.xy += proj_normal * grow * clip_vertex.w;
    POSITION = clip_vertex;
}
```

直接写入 `POSITION` 跳过引擎内置的 MVP 变换，性能更好，也避免了矩阵求逆的精度问题。

### grow 参数含义对照

| 模式 | `grow` 值 | 物理含义 | 示例 |
|------|----------|---------|------|
| Object Space | 0.01 | 沿法线膨胀 0.01 米 | 近处粗远处细 |
| Screen Space | 0.005 | NDC 偏移 0.005（约屏幕 0.5%） | 恒定宽度 |

---

## 三、改动文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `scene/resources/material.h` | **修改** | 新增 `GrowMode` 枚举、`grow_mode` 字段、getter/setter 声明 |
| `scene/resources/material.cpp` | **修改** | 枚举绑定、shader 代码生成分支、属性注册、stencil outline 联动 |
| `doc/classes/BaseMaterial3D.xml` | **修改** | 文档更新 |

### 不新增文件

本功能完全在现有的 `BaseMaterial3D` 类内完成，无需新建文件。

---

## 四、具体代码修改点

### 4.1 material.h — 新增枚举和字段

```cpp
// 在 BaseMaterial3D 类的 public 区域，DistanceFadeMode 枚举附近：
enum GrowMode {
    GROW_MODE_OBJECT_SPACE,
    GROW_MODE_SCREEN_SPACE,
    GROW_MODE_MAX
};

// 在 MaterialKey 中新增：
uint64_t grow_mode : Math::get_num_bits(GROW_MODE_MAX - 1);

// 在 _compute_key() 中新增：
mk.grow_mode = grow_mode;

// 在成员变量区域：
GrowMode grow_mode = GROW_MODE_OBJECT_SPACE;

// 在 public 方法区域：
void set_grow_mode(GrowMode p_mode);
GrowMode get_grow_mode() const;
```

### 4.2 material.cpp — Shader 代码生成

```cpp
// 替换现有的 grow_enabled 代码生成块（约 L1463）:
if (grow_enabled) {
    if (grow_mode == GROW_MODE_SCREEN_SPACE) {
        code += R"(
    // Grow: Screen Space
    {
        vec4 clip_vertex = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
        vec3 view_normal = mat3(MODELVIEW_MATRIX) * NORMAL;
        vec2 proj_normal = mat2(PROJECTION_MATRIX) * view_normal.xy;
        proj_normal = normalize(proj_normal);
        clip_vertex.xy += proj_normal * grow * clip_vertex.w;
        POSITION = clip_vertex;
    }
)";
    } else {
        code += R"(
    // Grow: Object Space
    VERTEX += NORMAL * grow;
)";
    }
}
```

### 4.3 material.cpp — 属性注册

```cpp
// _bind_methods() 中：
ClassDB::bind_method(D_METHOD("set_grow_mode", "mode"), &BaseMaterial3D::set_grow_mode);
ClassDB::bind_method(D_METHOD("get_grow_mode"), &BaseMaterial3D::get_grow_mode);

// 在 grow_amount 属性之后：
ADD_PROPERTY(PropertyInfo(Variant::INT, "grow_mode", PROPERTY_HINT_ENUM, "Object Space,Screen Space"),
    "set_grow_mode", "get_grow_mode");

// 枚举绑定：
BIND_ENUM_CONSTANT(GROW_MODE_OBJECT_SPACE);
BIND_ENUM_CONSTANT(GROW_MODE_SCREEN_SPACE);
```

### 4.4 material.cpp — Stencil Outline 联动

```cpp
// _prepare_stencil_effect() 中 STENCIL_MODE_OUTLINE 分支，增加：
case STENCIL_MODE_OUTLINE:
    // ... 现有代码 ...
    stencil_next_pass->set_grow_mode(grow_mode); // 继承主材质的 grow mode
    break;
```

### 4.5 material.cpp — 属性隐藏逻辑

```cpp
// _validate_property() 中，在 grow_amount 的隐藏逻辑附近：
if (p_property.name == "grow_mode" && !grow_enabled) {
    p_property.usage = PROPERTY_USAGE_NO_EDITOR;
}
```

---

## 五、数学原理

### 为什么乘以 `clip_vertex.w`？

透视投影后，GPU 会执行**透视除法**：

```
ndc.xy = clip.xy / clip.w
```

如果我们在 clip space 做偏移 `clip.xy += offset * clip.w`，那么透视除法后：

```
ndc.xy = (clip.xy + offset * clip.w) / clip.w
       = clip.xy / clip.w + offset
       = original_ndc.xy + offset
```

无论 `clip.w`（约等于 view-space 深度）多大，NDC 偏移都是常量 `offset` → 屏幕像素宽度恒定。

### mat2(PROJECTION_MATRIX) 提取的是什么？

```
PROJECTION_MATRIX = | f/aspect  0     0    0  |
                    | 0         f     0    0  |
                    | 0         0     A    B  |
                    | 0         0    -1    0  |

mat2(PROJECTION_MATRIX) = | f/aspect  0 |
                          | 0         f |
```

这恰好是将 View Space XY 投影到 Clip Space XY 的 2D 缩放矩阵，包含了长宽比修正。

---

## 六、Inspector 面板效果

启用 Grow 后，面板显示：

```
▼ Grow                          ☑
   Grow Amount                  0.005
   Grow Mode                    [Screen Space ▾]
```

当 `grow_enabled = false` 时，`Grow Amount` 和 `Grow Mode` 都隐藏。

Stencil Outline 模式下：

```
▼ Stencil
   Stencil Mode                 [Outline ▾]
   Stencil Color                ■ (0, 0, 0, 1)
   Stencil Outline Thickness    0.005
```

描边厚度的含义自动跟随 `Grow Mode`。

---

## 七、兼容性考虑

| 方面 | 分析 |
|------|------|
| **向后兼容** | 默认值为 `GROW_MODE_OBJECT_SPACE`，现有项目行为不变 |
| **Forward Mobile** | `POSITION` 输出在移动渲染器中同样支持，无兼容问题 |
| **Shadow Pass** | 屏幕空间模式下 Shadow Pass 的投影矩阵是光源视角的，描边会按光源视角等宽——这实际是期望行为（避免阴影中描边忽大忽小） |
| **MaterialKey** | 新增 `grow_mode` 位域，改变 shader 变体总数。由于只有 2 种模式（1 bit），影响极小 |
| **性能** | Screen Space 模式在顶点着色器中多了 1 次 mat4×vec4 乘法 + 1 次 normalize + 直接输出 POSITION，开销可忽略 |

---

## 八、未来扩展

本方案为 Grow 系统打下模式化基础，后续可扩展更多模式：

| 可能的未来模式 | 描述 |
|-------------|------|
| `GROW_MODE_PIXEL_PERFECT` | 根据屏幕分辨率自动换算，指定精确像素宽度 |
| `GROW_MODE_SMOOTH_NORMAL` | 使用第二套法线（COLOR/UV2 通道存储的平滑法线）做膨胀，解决硬边断裂问题 |

这些可以用 `GrowMode` 枚举自然扩展，不破坏现有 API。

---

## 九、测试计划

### GDScript 快速验证

```gdscript
# 创建一个带屏幕空间描边的 Mesh
var mesh_instance = MeshInstance3D.new()
mesh_instance.mesh = SphereMesh.new()

var mat = StandardMaterial3D.new()
mat.stencil_mode = BaseMaterial3D.STENCIL_MODE_OUTLINE
mat.stencil_color = Color.BLACK
mat.stencil_outline_thickness = 0.005
mat.grow_mode = BaseMaterial3D.GROW_MODE_SCREEN_SPACE

mesh_instance.material_override = mat
add_child(mesh_instance)
```

### 视觉验证要点

1. **距离测试**：将摄像机拉远/推近，描边像素宽度应保持不变
2. **屏幕位置测试**：物体在画面中心和边缘时，描边宽度一致
3. **正交投影测试**：切换到正交投影，两种模式效果应该相同（正交无透视除法）
4. **多物体测试**：不同距离的物体描边宽度一致
