
# GameplayTag vs Group vs Meta —— 三种节点标注机制对比

## 一、概览

| 特性 | **Group** | **Meta** | **GameplayTag** |
|---|---|---|---|
| 本质 | 节点所属的"集合名"（扁平字符串） | 节点上的键值对 | **层级化标签**（以 `.` 分隔的树状路径） |
| 数据结构 | `HashSet<StringName>` | `HashMap<StringName, Variant>` | `GameplayTagContainer`（`Vector<StringName>` + `HashSet` + parent cache） |
| 层级关系 | ❌ 扁平，无层级 | ❌ 扁平，无层级 | ✅ 天然层级：`Damage.Fire.DoT` 自动包含 `Damage.Fire` 和 `Damage` |
| 查询方式 | `is_in_group("enemies")` | `get_meta("hp")` | `has_gameplay_tag("Damage.Fire")` 层级匹配 / `has_gameplay_tag_exact()` 精确匹配 |
| 集合运算 | ❌ | ❌ | ✅ `has_any` / `has_all` / `has_none` / `union_with` / `intersection_with` / `difference_with` |
| 全局管理 | SceneTree 级别 `get_nodes_in_group()` | 无全局管理 | `GameplayTagManager` 全局单例，统一注册 & 校验 |
| 编辑器支持 | Inspector → Groups 面板 | Inspector 自动显示 | 专属 Inspector 编辑器 + Project Settings 面板 |
| 可序列化 | ✅（随场景保存） | ✅（随场景保存） | ✅（`GameplayTagContainer` 继承 `Resource`，可作 `.tres` 保存） |

---

## 二、Group（组）

### 是什么
Group 是 Godot 节点原生的"分组"机制。一个节点可以属于多个 Group，Group 名就是一个扁平的 `StringName`。

### 核心 API
```gdscript
node.add_to_group("enemies")
node.is_in_group("enemies")  # -> true
get_tree().get_nodes_in_group("enemies")  # -> 全局查找
get_tree().call_group("enemies", "take_damage", 10)  # -> 批量调用
```

### 设计目的
- **场景树级别的全局广播**：通过 `call_group` / `notify_group` 对所有同组节点做批量操作。
- **简单分类**：比如标记哪些是 "enemies"、哪些是 "interactable"。

### 局限性
1. **无层级**：`enemies` 和 `enemies_ranged` 是两个完全独立的字符串，没有父子关系。
2. **无集合运算**：无法原生表达"同时属于 A 且属于 B"这类复合查询。
3. **缺乏中央管理**：Group 名是随用随写的字符串，容易拼写错误且没有校验。

---

## 三、Meta（元数据）

### 是什么
Meta 是 Godot 节点/对象原生的 **键值对** 存储。Key 是 `StringName`，Value 是 `Variant`（可以是任意类型）。

### 核心 API
```gdscript
node.set_meta("hp", 100)
node.get_meta("hp")       # -> 100
node.has_meta("hp")       # -> true
node.remove_meta("hp")
```

### 设计目的
- **附加任意自定义数据**：不需要继承或声明变量，直接往对象上贴数据。
- **编辑器辅助**：编辑器自己也用 Meta 存一些内部标记（以 `_` 开头的 Meta 会在编辑器中隐藏）。

### 局限性
1. **不是"标签"系统**：Meta 本质是 key-value 存储，不适合用来做"这个节点具有某种特征"的标注查询。
2. **无层级**、**无集合运算**。
3. **无全局查询**：没有 `get_nodes_with_meta("xxx")` 这样的全局 API。
4. **缺乏类型约束**：Value 是 `Variant`，容易出现类型不一致。

---

## 四、GameplayTag（游戏标签）

### 是什么
你在引擎层面实现的、灵感源自 UE 的 **层级化标签系统**。由三个核心类组成：

| 类 | 职责 |
|---|---|
| `GameplayTag` | 一个不可变的层级标签（如 `Damage.Fire.DoT`），RefCounted |
| `GameplayTagContainer` | 一组标签的集合，附带 parent cache 做快速层级匹配，继承 Resource |
| `GameplayTagManager` | 全局单例，维护标签树、注册/校验/查询 |

### 核心设计

#### 1. 层级匹配
```
"Damage.Fire.DoT".matches_tag("Damage.Fire")  → true  （DoT 是 Fire 的子标签）
"Damage.Fire.DoT".matches_tag("Damage")       → true  （DoT 是 Damage 的后代）
"Damage.Fire".matches_tag("Damage.Fire.DoT")  → false （父不匹配子）
```

这是 Group 和 Meta 完全做不到的。

#### 2. 容器级集合运算
```gdscript
container.has_any(other)            # 是否有任一交集
container.has_all(other)            # 是否完全包含
container.has_none(other)           # 是否完全不相交
container.union_with(other)         # 并集
container.intersection_with(other)  # 交集
container.difference_with(other)    # 差集
```

#### 3. 全局标签注册 & 校验
```gdscript
GameplayTagManager.register_tag("Damage.Fire.DoT", "持续火焰伤害")
GameplayTagManager.is_valid_tag_name("Damage.Fire")  # 格式校验
GameplayTagManager.get_all_tag_names()                # 枚举所有已注册标签
GameplayTagManager.get_children_of("Damage")          # 获取直接子标签
```

标签在 Project Settings 中统一注册（`gameplay_tags/tag_list`），避免了拼写错误和散乱管理。

#### 4. 节点集成
直接集成到了 `Node` 上：
```gdscript
node.add_gameplay_tag("Status.Burning")
node.has_gameplay_tag("Status")          # 层级匹配 → true
node.has_gameplay_tag_exact("Status")    # 精确匹配 → false
node.find_children_by_tag("Status.Burning", true)  # 递归查找子节点
```

#### 5. 编辑器支持
- **Inspector**：`GameplayTagContainerEditor` —— 点击按钮弹出 `GameplayTagPickerDialog`，用树状结构选择标签。
- **Project Settings**：`GameplayTagSettingsEditor` —— 集中管理所有项目标签，支持增删改和备注。

---

## 五、什么时候用什么？

| 场景 | 推荐 | 原因 |
|---|---|---|
| 批量通知/调用同类节点 | **Group** | `call_group` 是专门为此设计的广播机制 |
| 给节点/资源附加临时或杂项数据 | **Meta** | 键值对灵活，适合存非结构化的附加信息 |
| 给实体标注"属性/状态/类型"并做复杂查询 | **GameplayTag** | 层级匹配 + 集合运算 = 强大的特征查询能力 |

### 具体例子

**用 Group：**
```gdscript
# "所有敌人受到全屏伤害"
get_tree().call_group("enemies", "take_damage", 50)
```
这里不需要层级，不需要集合运算，Group 刚好。

**用 Meta：**
```gdscript
# 给门存一个钥匙 ID
door.set_meta("required_key_id", "key_003")
```
这里需要的是 key-value 存储，不是标签。

**用 GameplayTag：**
```gdscript
# 技能释放条件：目标必须有"状态.燃烧"中的任一子状态，但不能有"免疫.火焰"
var can_cast = target.get_gameplay_tags().has_tag_name("Status.Burning") \
            and target.get_gameplay_tags().has_none(immune_fire_tags)
```
这里需要层级匹配（`Status.Burning` 匹配 `Status.Burning.Small`）和集合运算（`has_none`），只有 GameplayTag 能优雅地做到。

---

## 六、内部实现差异总结

| | Group | Meta | GameplayTag |
|---|---|---|---|
| 存储位置 | `Node::Data::grouped` (`HashMap<StringName, GroupData>`) | `Object::metadata` (`HashMap<StringName, Variant>`) | `Node::Data::gameplay_tags` (`Ref<GameplayTagContainer>`) |
| 查找复杂度 | `is_in_group`: O(1) hash | `has_meta`: O(1) hash | `has_tag`: O(1) hash（parent cache）<br>`has_tag_exact`: O(1) hash |
| 全局查找 | `SceneTree::get_nodes_in_group` → O(n) | 无原生支持 | `Node::find_children_by_tag` → 递归遍历<br>`GameplayTagManager` 管理注册表（不维护节点索引） |
| 序列化 | 场景文件 `[node]` 的 `groups` 字段 | 场景文件 metadata 段 | `GameplayTagContainer` 作为 Resource 序列化 (`tag_names` 属性) |

---

**总结一句话：** Group 是"广播通道"，Meta 是"便签纸"，GameplayTag 是"结构化特征标注系统"。三者定位不同，互不替代。
