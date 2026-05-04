# CanvasPostProcess 2D 后处理系统

> 为 Godot 引擎新增 `CanvasPostProcess` 节点，提供开箱即用的 2D 全屏后处理效果。

---

## 一、修改目的

Godot 的 3D 渲染有完整的后处理栈（Tonemap、Glow、SSAO 等），但 2D 渲染完全没有内置后处理。用户要实现 2D 全屏效果（晕影、色差、像素化、调色、模糊等）必须手动搭建 `SubViewport + BackBufferCopy + ColorRect + ShaderMaterial` 的组合，非常繁琐。

`CanvasPostProcess` 将这些常用后处理效果封装为一个节点，放到场景树即可生效，支持 8 种效果。

---

## 二、改动文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `scene/main/canvas_post_process.h` | **新建** | 节点类声明，8 组效果的属性和方法 |
| `scene/main/canvas_post_process.cpp` | **新建** | 节点实现：内嵌 Raw String Literal shader、属性绑定、绘制逻辑 |
| `scene/register_scene_types.cpp` | **修改** | 注册 `CanvasPostProcess` 类（+2 行） |
| `doc/classes/CanvasPostProcess.xml` | **新建** | 编辑器悬浮提示文档（英文） |
| `dua_doc/CanvasPostProcess后处理.md` | **新建** | 本文件（中文改动记录） |

---

## 三、新增属性

> **Inspector 面板的分组顺序与 Shader 处理顺序完全一致**，序号越小越先处理。

### 1. Pixelation（像素化）

| 属性名 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `pixelation_enabled` | bool | false | — | 像素化开关 |
| `pixelation_size` | int | 4 | 1 ~ 64 | 像素块大小，值越大像素越粗 |

### 2. Chromatic Aberration（色差）

| 属性名 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `chromatic_aberration_enabled` | bool | false | — | 色差开关 |
| `chromatic_aberration_strength` | float | 1.0 | 0.0 ~ 20.0 | 色差强度，RGB 通道径向偏移量 |

### 3. Gaussian Blur（高斯模糊）

| 属性名 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `blur_enabled` | bool | false | — | 模糊开关 |
| `blur_strength` | float | 2.0 | 0.0 ~ 10.0 | 模糊半径/强度 |
| `blur_iterations` | int | 1 | 1 ~ 5 | 模糊迭代次数，越多越平滑但越慢 |

### 4. Radial Blur（径向模糊）

| 属性名 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `radial_blur_enabled` | bool | false | — | 径向模糊开关 |
| `radial_blur_strength` | float | 0.02 | 0.0 ~ 0.2 | 径向模糊强度 |
| `radial_blur_samples` | int | 16 | 4 ~ 64 | 采样次数，越多越平滑但越慢 |
| `radial_blur_center` | Vector2 | (0.5, 0.5) | — | 模糊中心点（UV 坐标，0~1） |

### 5. Color Adjustment（色调调整）

| 属性名 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `adjustment_enabled` | bool | false | — | 调色开关 |
| `adjustment_brightness` | float | 1.0 | 0.0 ~ 3.0 | 亮度倍率 |
| `adjustment_contrast` | float | 1.0 | 0.0 ~ 3.0 | 对比度 |
| `adjustment_saturation` | float | 1.0 | 0.0 ~ 3.0 | 饱和度，0=灰度 |

### 6. Scanlines（扫描线）

| 属性名 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `scanlines_enabled` | bool | false | — | 扫描线开关 |
| `scanlines_density` | float | 1.0 | 0.1 ~ 10.0 | 扫描线密度，值越大线越密 |
| `scanlines_opacity` | float | 0.3 | 0.0 ~ 1.0 | 扫描线不透明度 |
| `scanlines_speed` | float | 0.0 | 0.0 ~ 20.0 | 扫描线滚动速度，0=静止 |

### 7. Film Grain（胶片颗粒）

| 属性名 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `grain_enabled` | bool | false | — | 颗粒开关 |
| `grain_intensity` | float | 0.1 | 0.0 ~ 1.0 | 颗粒强度 |
| `grain_speed` | float | 15.0 | 0.0 ~ 60.0 | 颗粒变化速度，值越大闪动越快 |

### 8. Vignette（晕影）

| 属性名 | 类型 | 默认值 | 范围 | 说明 |
|--------|------|--------|------|------|
| `vignette_enabled` | bool | false | — | 晕影开关 |
| `vignette_intensity` | float | 0.4 | 0.0 ~ 1.0 | 晕影范围，值越大暗角越向中心延伸 |
| `vignette_softness` | float | 2.0 | 0.0 ~ 10.0 | 晕影过渡柔和度，值越大过渡越平滑 |
| `vignette_color` | Color | 黑色(0,0,0,1) | — | 晕影颜色，alpha 控制整体透明度 |

---

## 四、实现原理

### 架构

`CanvasPostProcess` 继承自 `Control`，内部自动创建一个 `Shader` + `ShaderMaterial`：

1. **进入场景树时** (`NOTIFICATION_ENTER_TREE`)：
   - 设置为全屏铺满 (`PRESET_FULL_RECT`)
   - 设置鼠标穿透 (`MOUSE_FILTER_IGNORE`)
   - 通过 `canvas_item_set_copy_to_backbuffer` 开启 backbuffer 拷贝
   - 使用 C++ Raw String Literal (`R"(...)"`) 内嵌完整的 canvas_item shader 代码并编译
   - 将 `ShaderMaterial` 应用到自身

2. **绘制时** (`NOTIFICATION_DRAW`)：
   - 绘制一个全屏白色矩形，shader 在 fragment 阶段读取 `screen_texture`（`hint_screen_texture`）并应用后处理

3. **属性变化时**：
   - 通过 `set_shader_parameter` 更新 uniform 值
   - 调用 `queue_redraw()` 触发重绘

### Shader 处理顺序

Inspector 面板的分组顺序 = Shader fragment 函数中的执行顺序：

```
screen_texture
  → 1. Pixelation (UV 量化为像素块中心，nearest 采样)
  → 2. Chromatic Aberration (RGB 通道径向偏移采样)
  → 3. Gaussian Blur (11×11 2D 高斯核，可多次迭代)
  → 4. Radial Blur (从中心向外逐步采样)
  → 5. Color Adjustment (亮度 × 对比度 × 饱和度)
  → 6. Scanlines (正弦波明暗条纹叠加)
  → 7. Film Grain (hash 噪点叠加)
  → 8. Vignette (边缘暗角混合)
  → COLOR 输出
```

### 双 sampler 设计

Shader 中声明了两个 screen texture sampler：
- `screen_texture`：`filter_linear_mipmap`，用于大多数效果（线性插值平滑）
- `screen_texture_nearest`：`filter_nearest`，专用于像素化（最近邻采样，保证像素块边缘锐利）

当像素化开启时，采样自动切换到 `screen_texture_nearest`。

### 层级控制

后处理影响其所在 Viewport 中 **渲染顺序在它之前** 的所有 2D 内容。

要排除 UI（如对话框）不受后处理影响，推荐使用 SubViewport 隔离：

```
Main
├── SubViewportContainer          ← 全屏
│   └── SubViewport
│       ├── CanvasPostProcess     ← 后处理只影响 SubViewport 内容
│       ├── Camera2D
│       ├── Player
│       └── TileMap
└── CanvasLayer (layer=1)         ← UI 不受影响
    └── DialogBox
```

`CanvasPostProcess` 默认 `z_index=4096`，确保在同层其他节点之后绘制。

---

## 五、参数调节指南

| 想要的效果 | 推荐设置 |
|-----------|---------|
| 电影感暗角 | Vignette: intensity=0.4, softness=2.0, 黑色 |
| 强烈暗角（恐怖） | Vignette: intensity=0.8, softness=1.0, 黑色 |
| 红色血边 | Vignette: intensity=0.6, color=红色 |
| 轻微色差（氛围） | CA: strength=1.0~3.0 |
| 强烈色差（受击） | CA: strength=8.0~15.0 |
| 老电影/褪色 | Adjustment: saturation=0.3, contrast=1.2 |
| 梦幻/过曝 | Adjustment: brightness=1.3, saturation=0.8 |
| 复古像素风 | Pixelation: size=4~8 |
| 暂停/背景模糊 | Blur: strength=3.0, iterations=2 |
| 毛玻璃 | Blur: strength=5.0, iterations=3 |
| CRT 显示器 | Scanlines: density=1.0, opacity=0.3, speed=0 + Vignette + Pixelation |
| 电影质感 | Grain: intensity=0.08, speed=15 + Vignette: intensity=0.3 |
| 老式录像带 | Scanlines: density=0.5, opacity=0.2, speed=2.0 + Grain: intensity=0.15 |
| 速度感/冲击波 | Radial Blur: strength=0.05, samples=32 |
| 聚焦中心 | Radial Blur: strength=0.03, center=(0.5,0.5) + Vignette |
| 受击全套 | CA: strength=10 + Radial Blur: strength=0.04 + Vignette: color=红色 |