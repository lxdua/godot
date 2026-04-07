# ShaderParameterCollection 着色器参数集合

> 分析 UE 的 Material Parameter Collection (MPC) 和 Godot 现有的 Global Shader Uniforms，评估是否需要新增系统以及如何扩展。

---

## 一、UE 的 Material Parameter Collection (MPC)

你说的"矩阵"就是 **Material Parameter Collection**，UE 中的核心材质管理工具。

### 是什么

一个**可保存为资产的全局参数表**。在蓝图/C++ 中修改任意一个参数值，所有引用了这个 MPC 的材质**同帧生效**。

```
┌─ MPC_ToonSettings ──────────────────────────┐
│  Name                  Type       Value      │
│  shadow_offset         float      0.1        │
│  shadow_smoothness     float      0.1        │
│  shadow_color          vec4       (0,0,0,1)  │
│  saturation            float      1.0        │
│  outline_width         float      0.005      │
└──────────────────────────────────────────────┘
         ↓ 引用               ↓ 引用
   [角色材质A]           [角色材质B]       ... 100个材质
   shadow_offset = MPC.shadow_offset    （自动同步）
```

### 用法

**Material Editor 中**：拖入 `CollectionParameter` 节点 → 选择 MPC 资产 → 选择参数名

**蓝图/C++ 中**：
```cpp
UMaterialParameterCollection* MPC = LoadObject<...>("MPC_ToonSettings");
UKismetMaterialLibrary::SetScalarParameterValue(World, MPC, "shadow_offset", 0.3);
// → 所有用了这个 MPC 的材质同帧更新
```

### GPU 实现

每个 MPC 对应一个 **Uniform Buffer Object (UBO)**：
- 标量和向量打包在一起
- 修改时只需更新 UBO 的内容
- 所有引用该 MPC 的 shader 共享同一个 UBO binding
- **零额外 Draw Call、零材质遍历开销**

### 限制

- 每个 MPC 最多 1024 个标量 + 1024 个向量
- 每个项目可以有多个 MPC
- 不支持纹理类型（UE 5.1+ 开始支持）

---

## 二、Godot 现有系统：Global Shader Uniforms

**Godot 其实已经有了一个非常类似的系统！** 就是你说的项目设置中的 "着色器全局量"（Shader Globals）。

### 源码位置

| 组件 | 文件 |
|------|------|
| API 定义 | `servers/rendering/rendering_server.h:928-941` |
| RD 后端实现 | `servers/rendering/renderer_rd/storage_rd/material_storage.cpp:1795-2020` |
| Shader 语言支持 | `servers/rendering/shader_language.cpp:9553` (`SCOPE_GLOBAL`) |
| Shader 编译器 | `servers/rendering/shader_compiler.cpp:596` |
| 枚举定义 | `servers/rendering/rendering_server_enums.h:865` |

### 使用方式

**1. 在项目设置中定义**：

`Project Settings → General → Shader Globals → Add`

可添加的类型完整列表（`GlobalShaderParameterType`，共 30 种）：
- bool, bvec2-4
- int, ivec2-4, rect2i
- uint, uvec2-4
- float, vec2-4, color, rect2
- mat2, mat3, mat4, transform_2d, transform
- sampler2D, sampler2DArray, sampler3D, samplerCube, samplerExternal

**2. 在 shader 中使用**：

```glsl
shader_type spatial;

// 用 "global" 关键字声明
global uniform float shadow_offset;
global uniform float shadow_smoothness;
global uniform vec4 shadow_color;

void light() {
    float shadow = lambert_shadow(NORMAL, LIGHT, ATTENUATION);
    shadow = smoothstep(shadow_offset - shadow_smoothness, 
                        shadow_offset + shadow_smoothness, shadow);
    // ...
}
```

**3. 在 GDScript 中运行时修改**：

```gdscript
RenderingServer.global_shader_parameter_set("shadow_offset", 0.3)
# → 所有使用了 global uniform shadow_offset 的 shader 同帧生效
```

### GPU 实现（已确认）

源码 `material_storage.cpp:1795-1831` 确认：

```cpp
void MaterialStorage::global_shader_parameter_add(...) {
    // 非纹理类型 → 分配到全局 UBO 中
    gv.buffer_index = _global_shader_uniform_allocate(gv.buffer_elements);
    _global_shader_uniform_store_in_buffer(gv.buffer_index, gv.type, gv.value);
    _global_shader_uniform_mark_buffer_dirty(gv.buffer_index, gv.buffer_elements);
}
```

在 shader 编译时（`shader_compiler.cpp:964`）：
```cpp
if (u.scope == ShaderLanguage::ShaderNode::Uniform::SCOPE_GLOBAL) {
    // 编译成 global_shader_uniforms.data[index] 访问
}
```

所有引用的 shader 共享一个 `global_shader_uniforms.data` UBO（`actions.global_buffer_array_variable`），和 UE 的 MPC 完全相同的原理。

---

## 三、对比分析

| 特性 | UE MPC | Godot Global Shader Uniforms |
|------|--------|------------------------------|
| GPU 实现 | UBO | ✅ UBO（完全相同） |
| 修改一次全局生效 | ✅ | ✅ |
| 运行时修改 | ✅ `SetScalarParameterValue` | ✅ `RenderingServer.global_shader_parameter_set` |
| 纹理支持 | ✅（5.1+） | ✅（sampler2D 等） |
| shader 中声明 | `CollectionParameter` 节点 | `global uniform float xxx;` |
| 编辑器管理界面 | 专用编辑器 | Project Settings 面板 |
| **可保存为资产文件** | ✅ .uasset | ❌ 只在 project.godot 中 |
| **多个集合** | ✅ 可创建多个 MPC | ❌ 只有一个全局池 |
| **分组/分类** | ✅ 每个 MPC 是独立分组 | ❌ 所有参数混在一起 |
| **资产引用** | ✅ 材质中引用特定 MPC | ❌ 全局名称匹配 |
| **override/继承** | ❌ | ✅ `global_shader_parameter_set_override` |

---

## 四、结论：Godot 已有 80% 的能力，缺的是上层组织

### Godot 已经有的 ✅

1. **UBO 共享机制**：`global_shader_uniforms.data` 数组，GPU 端高效
2. **shader 语法**：`global uniform` 关键字
3. **运行时 API**：`RenderingServer.global_shader_parameter_set/get`
4. **类型系统**：30 种类型完整支持
5. **编辑器管理**：Project Settings 面板

### Godot 缺少的 ❌

| 缺失项 | 影响 | UE 对应 |
|--------|------|---------|
| **不能保存为独立资源文件** | 无法在多个项目间共享、无法版本管理单个参数集 | MPC 是独立 .uasset |
| **只有一个全局池** | 参数多了管理混乱（三渲二参数和天气参数混在一起） | 每个 MPC 独立 |
| **无分组概念** | Inspector 中无法按"卡通渲染"/"天气"/"后处理"归类 | MPC 天然分组 |
| **无预设/切换** | 不能一键切换"白天参数集"/"夜晚参数集" | 蓝图中切换整个 MPC |

---

## 五、改进方案

### 方向：在现有 Global Shader Uniforms 之上加一层 Resource 封装

**不动渲染管线**（UBO 机制已经存在且高效），只在上层新增一个 Resource 类型来组织参数。

### 新增类型：ShaderParameterCollection

```
ShaderParameterCollection (Resource)
│  可保存为 .tres 文件
│  内含多个参数的名称、类型、默认值
│  提供 apply() 方法一键同步到全局 shader uniforms
│  提供 reset() 方法恢复默认值
```

### Shader 端：零改动

现有的 `global uniform` 语法完全不需要改。ShaderParameterCollection 只是在 GDScript/C++ 层批量调用 `RenderingServer.global_shader_parameter_set()`。

### 使用方式

```gdscript
# toon_params.tres — ShaderParameterCollection 资源
# 在编辑器中创建，定义参数列表和默认值

@export var toon_day: ShaderParameterCollection    # 白天参数
@export var toon_night: ShaderParameterCollection  # 夜晚参数

func switch_to_night():
    toon_night.apply()  # 一键切换所有相关 global uniform

func _process(delta):
    # 也可以单独改一个
    toon_day.set_value("shadow_offset", lerp(0.1, 0.3, time_of_day))
    toon_day.apply()
```

### 编辑器集成

1. **Inspector**：展开后显示所有参数，可以直接拖滑条实时预览
2. **资源文件**：保存为 `.tres`，可以版本管理、跨项目复用
3. **预设切换**：创建多个 `.tres` 文件，运行时 `apply()` 切换

### 实现复杂度

| 组件 | 工作量 | 说明 |
|------|--------|------|
| `ShaderParameterCollection` 资源类 | 小 | 继承 Resource，存储 Dict<name, {type, value}> |
| `apply()` 方法 | 小 | 遍历参数调用 `RS::global_shader_parameter_set` |
| Inspector 编辑器 | 中 | 自定义 EditorInspectorPlugin，按类型生成控件 |
| 与 ProjectSettings 同步 | 小 | `apply()` 时自动注册缺失的 global uniform |

**总工作量：1-2 天**（因为底层 UBO 机制已经完备，只需要上层封装）

---

## 六、为什么不需要改渲染管线

关键洞察：**Godot 的 Global Shader Uniforms 在 GPU 端和 UE 的 MPC 原理完全相同**（单个 UBO，改一次全局生效）。

```
UE 架构:
  MPC Asset → UBO → shader 引用

Godot 现有架构:
  ProjectSettings → UBO → global uniform 引用

我们要做的:
  ShaderParameterCollection (.tres)
       ↓ apply()
  RenderingServer.global_shader_parameter_set()
       ↓
  UBO (已有) → global uniform (已有)
```

不需要新的 shader 语法，不需要新的 GPU buffer，不需要改编译器。只是在上层加一个更好用的组织方式。

---

## 七、与 UE MPC 剩余差距

即使实现了 ShaderParameterCollection，和 UE MPC 相比还有一个架构差距：

| 差异 | 说明 |
|------|------|
| **UE 支持多个独立 UBO** | 每个 MPC 有自己的 UBO，材质中 binding 不同的 MPC |
| **Godot 只有一个全局 UBO** | 所有 global uniform 共享一个 buffer |

这意味着：
- 如果两个 ShaderParameterCollection 有**同名参数**，会冲突
- 解决办法：命名约定（如 `toon_shadow_offset`、`weather_wind_strength`）
- 或者未来真的实现多 UBO 支持（改动较大，需要改 shader 编译器）

对于当前需求来说，**命名约定足够了**。

---

## 八、TODO

- [ ] 实现 `ShaderParameterCollection` 资源类
- [ ] 实现 `apply()` / `reset()` 方法
- [ ] 实现 `set_value()` / `get_value()` 单参数修改
- [ ] Inspector 自定义编辑器（按参数类型生成控件）
- [ ] 编辑器中实时预览（修改参数时立即 apply）
- [ ] 与 ProjectSettings Shader Globals 面板联动
