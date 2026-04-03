extends SceneTree

func _init():
	print("===== GameplayTag 系统测试 =====\n")

	# --- 1. 基础标签创建 ---
	print(">> 1. 基础标签创建")
	var tag = GameplayTag.create("Damage.Fire.DoT")
	print("  标签名: ", tag.get_tag_name())
	print("  深度: ", tag.get_depth())  # 应该是 3
	print("  是否有效: ", tag.is_valid())  # true
	print("")

	# --- 2. 层级匹配 ---
	print(">> 2. 层级匹配")
	var parent = GameplayTag.create("Damage.Fire")
	var root = GameplayTag.create("Damage")
	var other = GameplayTag.create("Damage.Ice")

	print("  DoT matches Fire: ", tag.matches_tag(parent))     # true
	print("  DoT matches Damage: ", tag.matches_tag(root))     # true
	print("  DoT matches Ice: ", tag.matches_tag(other))       # false
	print("  Fire matches DoT: ", parent.matches_tag(tag))     # false（父不匹配子）
	print("  DoT exact Fire: ", tag.matches_tag_exact(parent)) # false
	print("  DoT exact DoT: ", tag.matches_tag_exact(GameplayTag.create("Damage.Fire.DoT")))  # true
	print("")

	# --- 3. 父标签 & 祖先 ---
	print(">> 3. 父标签 & 祖先")
	var p = tag.get_parent_tag()
	print("  DoT 的父标签: ", p.get_tag_name() if p else "null")  # Damage.Fire
	var ancestors = tag.get_ancestor_tags()
	print("  DoT 的祖先数量: ", ancestors.size())  # 2
	for a in ancestors:
		print("    - ", a.get_tag_name())
	print("")

	# --- 4. TagContainer ---
	print(">> 4. TagContainer 基础操作")
	var container = GameplayTagContainer.new()
	container.add_tag_name("Damage.Fire.DoT")
	container.add_tag_name("Status.Buff.SpeedUp")
	container.add_tag_name("Ability.Skill.Fireball")
	print("  标签数量: ", container.get_tag_count())  # 3
	print("  所有标签: ", container.get_tag_names())
	print("")

	# --- 5. Container 层级查询 ---
	print(">> 5. Container 层级查询")
	print("  has Damage.Fire: ", container.has_tag_name("Damage.Fire"))       # true（层级匹配）
	print("  has Damage: ", container.has_tag_name("Damage"))                 # true（层级匹配）
	print("  has Status: ", container.has_tag_name("Status"))                 # true
	print("  has Status.Debuff: ", container.has_tag_name("Status.Debuff"))   # false
	print("  has_exact Damage.Fire: ", container.has_tag_exact_name("Damage.Fire"))  # false
	print("  has_exact Damage.Fire.DoT: ", container.has_tag_exact_name("Damage.Fire.DoT"))  # true
	print("")

	# --- 6. Container 集合查询 ---
	print(">> 6. Container 集合查询 (has_any / has_all / has_none)")
	var query_any = GameplayTagContainer.create_from_array(PackedStringArray(["Damage.Ice", "Status.Buff.SpeedUp"]))
	var query_all = GameplayTagContainer.create_from_array(PackedStringArray(["Damage.Fire.DoT", "Status.Buff.SpeedUp"]))
	var query_none = GameplayTagContainer.create_from_array(PackedStringArray(["Damage.Ice", "Status.Debuff.Poison"]))

	print("  has_any [Ice, SpeedUp]: ", container.has_any(query_any))    # true（SpeedUp 匹配）
	print("  has_all [DoT, SpeedUp]: ", container.has_all(query_all))    # true
	print("  has_none [Ice, Poison]: ", container.has_none(query_none))  # true
	print("")

	# --- 7. GameplayTagManager 单例 ---
	print(">> 7. GameplayTagManager")
	GameplayTagManager.register_tag("Enemy.Type.Undead")
	GameplayTagManager.register_tag("Enemy.Type.Dragon")
	GameplayTagManager.register_tag("Enemy.Rank.Boss")
	print("  注册标签数: ", GameplayTagManager.get_tag_count())
	print("  所有标签: ", GameplayTagManager.get_all_tag_names())
	print("  Enemy 的子标签: ", GameplayTagManager.get_children_of("Enemy"))
	print("  Enemy 的所有后代: ", GameplayTagManager.get_descendants_of("Enemy"))
	print("  是否注册 Enemy.Type.Undead: ", GameplayTagManager.is_tag_registered("Enemy.Type.Undead"))  # true
	print("  是否注册 Enemy.Type.Human: ", GameplayTagManager.is_tag_registered("Enemy.Type.Human"))    # false
	print("")

	# --- 8. 标签名验证 ---
	print(">> 8. 标签名验证")
	print("  'Damage.Fire' 合法: ", GameplayTagManager.is_valid_tag_name("Damage.Fire"))      # true
	print("  'Damage..Fire' 合法: ", GameplayTagManager.is_valid_tag_name("Damage..Fire"))    # false
	print("  '.Damage' 合法: ", GameplayTagManager.is_valid_tag_name(".Damage"))              # false
	print("  'Damage Fire' 合法: ", GameplayTagManager.is_valid_tag_name("Damage Fire"))      # false
	print("  'My_Tag.V2' 合法: ", GameplayTagManager.is_valid_tag_name("My_Tag.V2"))          # true
	print("")

	print("===== 全部测试完成 =====")
	quit()
