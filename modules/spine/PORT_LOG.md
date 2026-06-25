# Spine 3.8 → Godot 4.7 移植记录

## 概述

将 Spine 3.8 C++ 运行时与 spine-godot 4.1 的 Godot 4.x 模块包装层结合，使其在 Godot 4.7 中支持 Spine 3.8 骨骼动画。

## 源码来源

| 组件 | 来源 | 路径 |
|------|------|------|
| Godot 模块包装层 | spine-runtimes `4.1` 分支 | `spine-godot/spine_godot/` → `modules/spine/` |
| Spine 3.8 C++ 运行时 | spine-runtimes `3.8` 分支 | `spine-cpp/spine-cpp/` → `modules/spine/spine-cpp/` |

## 修改记录

### 1. SCsub - 修正 include 路径

**文件**: `modules/spine/SCsub`

**改动**: 
- `#../spine_godot/spine-cpp/include` → `#modules/spine/spine-cpp/include`（2 处）

**原因**: 原始 spine-godot 目录布局假设 Godot 源码在 `spine-godot/spine_godot/`，spine-cpp 在 `spine-godot/spine-cpp/`。我们的布局是 standard Godot module layout。

**状态**: ✅ 完成

---

### 2. AnimationStateData - 补 clear() 方法

**文件**: 
- `modules/spine/spine-cpp/include/spine/AnimationStateData.h`
- `modules/spine/spine-cpp/src/spine/AnimationStateData.cpp`

**改动**: 新增 `void clear()` 方法，调用 `_animationToMixTime.clear()`

**原因**: Spine 3.8 的 `AnimationStateData` 没有 `clear()` 方法，但 `SpineSkeletonDataResource::update_mixes()` 需要此方法重置所有 mix 设置。`HashMap` 本身有 `clear()`，故直接注入。

**状态**: ✅ 完成

---

### 3. SpineSkeletonFileResource - 移除 Version.h 依赖

**文件**: `modules/spine/SpineSkeletonFileResource.cpp`

**改动**:
- 移除 `#include <spine/Version.h>`（3.8 运行时中不存在此文件）
- `SPINE_VERSION_STRING` → `"3.8"` 硬编码
- `.utf8()` → `.utf8().get_data()`（MSVC CharString 兼容）

**原因**: `Version.h` 和 `SPINE_VERSION_STRING` 宏都只在 Spine 4.1 中存在。版本检查逻辑只检查 skeleton JSON 的 version 字段是否以 `"3.8"` 开头，直接硬编码即可。

**状态**: ✅ 完成

---

### 4. SpineCommon.h - 添加 callable_mp 头文件 + 修复 SPINE_STRING 宏

**文件**: `modules/spine/SpineCommon.h`

**改动**: 
- 添加 `#include "core/object/callable_mp.h"`
- `SPINE_STRING(x)` 宏: `.utf8()` → `.utf8().get_data()`

**原因**: 
- 使用了 `callable_mp` 宏但缺少对应头文件
- MSVC 中 `CharString` 不能隐式转换为 `const char*`，需要显式 `.get_data()`

**状态**: ✅ 完成

---

### 5. register_types - 修正函数名以匹配模块名 + Ref<> 管理

**文件**: 
- `modules/spine/register_types.h`
- `modules/spine/register_types.cpp`

**改动**: 
- `initialize_spine_godot_module` → `initialize_spine_module`
- `uninitialize_spine_godot_module` → `uninitialize_spine_module`
- 4 个 static FormatLoader/Saver 指针改为 `Ref<>`

**原因**: 
- Godot 构建系统按照模块目录名 `spine` 自动生成调用 `initialize_spine_module` / `uninitialize_spine_module`
- Godot 4.x `ResourceLoader::add_resource_format_loader()` 接受 `Ref<ResourceFormatLoader>`

**状态**: ✅ 完成

---

### 6. SpineConstant.h - PropertyId → TimelineTypeId

**文件**: `modules/spine/SpineConstant.h`

**改动**:
- 枚举 `PropertyId` → `TimelineTypeId`
- `VARIANT_ENUM_CAST(PropertyId)` → `VARIANT_ENUM_CAST(TimelineTypeId)`

**原因**: Spine 3.8 使用顺序枚举标识 Timeline 类型；Spine 4.1 改为位标志 PropertyId 系统。

**状态**: ✅ 完成

---

### 7. SpineAnimation - has_timeline 参数类型适配

**文件**: `modules/spine/SpineAnimation.h`, `modules/spine/SpineAnimation.cpp`

**改动**: `bool has_timeline(Array ids)` → `bool has_timeline(int id)`

**原因**: 3.8 `Animation::hasTimeline()` 接受单个 `int`，而 4.1 接受 `Vector<PropertyId>`。

**状态**: ✅ 完成

---

### 8. SpineTimeline - 移除 4.1 独占方法，适配 getPropertyId()

**文件**: `modules/spine/SpineTimeline.h`, `modules/spine/SpineTimeline.cpp`

**改动**:
- 移除: `get_frame_entries()`, `get_frame_count()`, `get_frames()`, `get_duration()`, `get_property_ids()`
- 新增: `get_property_id()` → 调用 3.8 `Timeline::getPropertyId()`（单数 int）

**原因**: 3.8 的 `Timeline` 基类只有 `apply()` 和 `getPropertyId()` 两个 virtual 方法。

**状态**: ✅ 完成

---

### 9. SpineTrackEntry - 移除 4.1 独占方法

**文件**: `modules/spine/SpineTrackEntry.h`, `modules/spine/SpineTrackEntry.cpp`

**改动**:
- 移除: `get_previous()`, `get_reverse()`, `set_reverse()`, `get_shortest_rotation()`, `set_shortest_rotation()`, `get_track_complete()`

**原因**: 这些方法仅在 4.1 的 `TrackEntry` 中存在，3.8 没有对应 API。

**状态**: ✅ 完成

---

### 10. SpineAnimationTrack - 移除 3.8 不支持的 TrackEntry 调用

**文件**: `modules/spine/SpineAnimationTrack.cpp`

**改动**:
- 移除 6 处 `entry->setReverse(reverse)` 和 `entry->setShortestRotation(shortest_rotation)` 调用
- `List<StringName>` → `LocalVector<StringName>` 适配 `get_animation_library_list()` API

**原因**: 
- 3.8 `spine::TrackEntry` 没有 setReverse/setShortestRotation 方法
- Godot 4.7 `AnimationMixer::get_animation_library_list()` 参数改为 `LocalVector`

**状态**: ✅ 完成

---

### 11. SpineBoneData - 颜色属性改为 wrapper 侧存储

**文件**: `modules/spine/SpineBoneData.h`, `modules/spine/SpineBoneData.cpp`

**改动**:
- 新增私有成员 `Color bone_color`
- `get_color()` / `set_color()` 改为读写 `bone_color`

**原因**: 3.8 的 `BoneData` 没有 `getColor()` / `setColor()` 方法（仅在 4.1 中新增）。

**状态**: ✅ 完成

---

### 12. SpineAtlasResource - 适配 3.8 Atlas API

**文件**: `modules/spine/SpineAtlasResource.cpp`

**改动**:
- `page.texture = renderer_object` → `page.setRendererObject((void *)renderer_object)`
- Atlas 构造函数 `.utf8()` → `.utf8().get_data()`

**原因**: 3.8 使用 `HasRendererObject` mixin 的 `setRendererObject()`，4.1 直接有 `texture` 成员。

**状态**: ✅ 完成

---

### 13. Transform/Path 约束 - Mix API 重命名

**文件**:
- `modules/spine/SpineTransformConstraint.cpp` / `.h`
- `modules/spine/SpineTransformConstraintData.cpp`
- `modules/spine/SpinePathConstraint.cpp`
- `modules/spine/SpinePathConstraintData.cpp`

**改动**: 3.8 统一方法名：
- `getMixRotate()` → `getRotateMix()`
- `getMixX()` / `getMixY()` → `getTranslateMix()`
- `getMixScaleX()` / `getMixScaleY()` → `getScaleMix()`
- `getMixShearY()` → `getShearMix()`

**原因**: 4.1 将 translation/scale 拆分为 X/Y 独立方法，3.8 使用统一方法。

**状态**: ✅ 完成

---

### 14. SpineSprite - API 适配（Attachment + RenderingServer）

**文件**: `modules/spine/SpineSprite.h`, `modules/spine/SpineSprite.cpp`

**改动**:
- 添加头文件: `servers/rendering/rendering_server.h`, `scene/resources/mesh.h`, `scene/main/scene_tree.h`
- `RS::SurfaceData` → `RenderingServerTypes::SurfaceData`
- `RS::PrimitiveType` → `RSE::PrimitiveType`
- `RS::ARRAY_VERTEX` 等 → `Mesh::ARRAY_VERTEX`
- `region->computeWorldVertices(*slot, ...)` → `region->computeWorldVertices(slot->getBone(), ...)`
- `region->getRegion()` → `region->getRendererObject()`
- `mesh->getRegion()` → `mesh->getRendererObject()`
- `.page->texture` → `.page->getRendererObject()`

**原因**: 
- Godot 4.7 将 `SurfaceData` 移至 `RenderingServerTypes`，`PrimitiveType` 移至 `RSE`
- 3.8 `RegionAttachment::computeWorldVertices()` 接受 `Bone&` 而非 `Slot&`
- 3.8 使用 `HasRendererObject` mixin 存储 renderer object

**状态**: ✅ 完成

---

### 15. SpineEditorPlugin - 适配 Godot 4.7 Editor API

**文件**: `modules/spine/SpineEditorPlugin.h`, `modules/spine/SpineEditorPlugin.cpp`

**改动**:
- `import()` 签名添加 `ResourceUID::ID p_source_id` 参数
- 参数重命名: `*platform_variants` → `*r_platform_variants`, `*gen_files` → `*r_gen_files`
- 头文件路径修正: `editor/editor_properties.h` → `editor/inspector/editor_properties.h`
- 移除 `get_name()` override（Godot 4.7 无此方法）
- `EditorPropertyFloat::setup()` 改用 `EditorPropertyRangeHint` 结构体
- 信号连接使用 `callable_mp`

**状态**: ✅ 完成

---

### 16. Include 路径修正

**文件**: `SpineAnimationTrack.cpp`, `SpineSprite.cpp`, `SpineBoneNode.cpp`

**改动**:
| 旧路径 | 新路径 |
|--------|--------|
| `editor/plugins/animation_player_editor_plugin.h` | `editor/animation/animation_player_editor_plugin.h` |
| `editor/plugins/animation_tree_editor_plugin.h` | `editor/animation/animation_tree_editor_plugin.h` |
| `editor/editor_plugin.h` | `editor/plugins/editor_plugin.h` |
| `servers/rendering_server.h` | `servers/rendering/rendering_server.h` |

**原因**: Godot 4.x 代码重组导致头文件路径变更。

**状态**: ✅ 完成

---

### 17. SpineSlotNode - 参数名避免 shadowing

**文件**: `modules/spine/SpineSlotNode.h`, `modules/spine/SpineSlotNode.cpp`

**改动**: `material` → `p_material`（4 处 set_*_material 函数）

**原因**: MSVC C4458: 参数名 `material` 遮蔽了 `CanvasItem::material` 成员。

**状态**: ✅ 完成

---

### 18. MSVC CharString 兼容性修复

**文件**: `SpineSkeletonDataResource.cpp`, `SpineEvent.cpp`, `SpineEventData.cpp`

**改动**: `.utf8()` → `.utf8().get_data()`（所有传给 `spine::String` 构造函数的位置）

**原因**: MSVC 中 `CharString` 不能隐式转换为 `const char*`。

**状态**: ✅ 完成

---

### 19. SpineSkeletonFileResource - 补充 GDScript 绑定 + 修复 3.8 二进制格式检测

**文件**: `modules/spine/SpineSkeletonFileResource.cpp`

**改动 1**: 添加 `_bind_methods()` 绑定：
- `ClassDB::bind_method(D_METHOD("load_from_file", "path"), ...)`
- `ClassDB::bind_method(D_METHOD("save_to_file", "path"), ...)`
- `ClassDB::bind_method(D_METHOD("is_binary"), ...)`

**改动 2**: 修复 `checkBinary()` 中的 3.8 二进制格式解析：
```cpp
// 旧 (4.1 格式): 跳过 8 字节 hash，读取 version 字符串
input.cursor += 8;
char *version = readString(&input);
// 新 (3.8 格式): 先读取 hash 字符串，再读取 version 字符串
spine::SpineExtension::free(readString(&input), __FILE__, __LINE__);
char *version = readString(&input);
```

**原因**: 
- GDScript 需要通过绑定才能调用 C++ 方法
- Spine 3.8 二进制格式使用长度前缀字符串存储 hash（第一个 string），version 是第二个 string；4.1 格式使用固定 8 字节 hash

**状态**: ✅ 完成

---

### 20. SpineAnimationState - 补 null 检查

**文件**: `modules/spine/SpineAnimationState.cpp`

**改动**: `set_animation()` 和 `add_animation()` 添加 `animation_state->getData()` 和 `getSkeletonData()` 的 null 检查

```cpp
auto data = animation_state->getData();
if (!data) return nullptr;           // 新增
auto skeleton_data = data->getSkeletonData();
if (!skeleton_data) return nullptr;  // 新增
```

**原因**: 在 `_get_property_list()` const 上下文中调用时，如果 skeleton data 未正确初始化，`getData()` 或 `getSkeletonData()` 可能返回 null，导致空指针解引用。

**状态**: ✅ 完成

---

### 21. SPINE_STRING_TMP - 修复 double-free 堆损坏

**文件**: `modules/spine/SpineCommon.h`

**改动**:
```cpp
// 旧: spine::String 析构时 free CharString 临时对象的内部 buffer → double-free
#define SPINE_STRING_TMP(x) spine::String((x).utf8().get_data(), true)
// 新: 不接管所有权，改为拷贝字符串
#define SPINE_STRING_TMP(x) spine::String((x).utf8().get_data())
```

**原因**: `spine::String(ptr, true)` 构造时 `_buffer = (char*)chars`（不拷贝），析构时 `SpineExtension::free(_buffer)`。但 `ptr` 来自 `CharString::get_data()` 的临时对象，`CharString` 析构时也会释放同一块内存 → **double-free → 堆损坏 (0xC0000374)**。

此 bug 影响所有 18 处 `SPINE_STRING_TMP` 调用点，之前被 `connect()` 崩溃掩盖（先崩在 connect，spine::String 析构未执行）。

**状态**: ✅ 完成

---

### 22. SpineObjectWrapper - 添加析构函数防止悬空信号连接

**文件**: `modules/spine/SpineCommon.h`

**改动**:
- 添加 `~SpineObjectWrapper()` 析构函数，若 `spine_owner` 仍有效则断开 `_internal_spine_objects_invalidated` 信号
- `spine_objects_invalidated()` 信号回调末尾添加 `spine_owner = nullptr;`

```cpp
~SpineObjectWrapper() {
    if (spine_owner) {
        spine_owner->disconnect(SNAME("_internal_spine_objects_invalidated"),
            callable_mp(this, &SpineObjectWrapper::spine_objects_invalidated));
    }
}
```

**原因**: 临时 wrapper 对象（如 `_get_property_list` 中创建的 `SpineAnimation`）在 `Ref<>` 出作用域后被销毁，但未断开信号连接。多次 inspector 刷新后累积的悬空连接可能损坏信号槽列表，导致后续 `connect()` 崩溃。

**状态**: ✅ 完成

---

### 23. get_animation_duration - 修复 const 上下文中的信号连接

**文件**:
- `modules/spine/SpineSkeletonDataResource.h`
- `modules/spine/SpineSkeletonDataResource.cpp`
- `modules/spine/SpineSprite.cpp`

**改动**:
- 新增 `get_animation_duration(name) const` 方法，直接查 spine-cpp `SkeletonData::findAnimation()` 获取 duration，不创建 `SpineAnimation` 包装对象
- `SpineSprite::_get_property_list()` const 方法改用 `get_animation_duration()` 代替 `find_animation()`

```cpp
// 旧: 创建 SpineAnimation wrapper → set_spine_object() → connect() — const 违规
auto animation = skeleton_data_res->find_animation(preview_animation);
if (animation.is_valid()) { animation_duration = animation->get_duration(); }

// 新: 直接查询，无 wrapper 无信号连接
animation_duration = skeleton_data_res->get_animation_duration(preview_animation);
```

**原因**: `_get_property_list()` 是 const 方法，原调用 `find_animation()` 内部会执行 `set_spine_object()` → `connect()` 修改 `SpineSkeletonDataResource` 的信号连接（const 违规）。加上 SPINE_STRING_TMP 的 double-free，两者叠加导致选择预览动画时闪退。

**状态**: ✅ 完成

---

## 构建命令

```bash
scons accesskit=no d3d12=no angle=no module_spine_enabled=yes target=editor -j20
```

## 构建结果

✅ **编译成功** — Godot 4.7 (MSVC) + Spine 3.8 C++ 运行时 + spine-godot 模块包装层

Warnings（非致命）:
- C4458: 参数名遮蔽基类成员（已知，不影响功能）
- C4838: 指针到 Variant 转换需要窄化（spine-cpp 内部，忽略）

## 运行时测试结果

✅ **全部通过** — 使用 Spine 3.8 导出的 `bg.skel` + `bg.atlas` 测试：

| 测试项 | 结果 | 说明 |
|--------|------|------|
| ClassDB 15 类注册 | ✅ | SpineSprite 等全部注册 |
| 加载 bg.skel (二进制) | ✅ | `load_from_file` 返回 OK |
| 加载 bg.atlas | ✅ | 纹理加载在 headless 模式预期报错 |
| SkeletonData 加载 | ✅ | version: 3.8.99, fps: 30.0 |
| SpineSprite 创建 | ✅ | Skeleton 创建成功 |
| 编辑器 Preview 动画选择 | ✅ | 不再闪退，动画正常预览 |

## 已知问题

- 编辑器退出时 "4 ObjectDB instances leaked" — spine 模块退出清理不完整，不影响功能
