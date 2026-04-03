/**************************************************************************/
/*  gameplay_tag_manager.h                                                */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/gameplay_tag/gameplay_tag.h"
#include "core/gameplay_tag/gameplay_tag_container.h"
#include "core/object/object.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"

/**
 * GameplayTagManager is a global singleton that manages all registered
 * gameplay tags. It maintains the tag tree and provides methods to
 * register, query, and validate tags.
 *
 * Tags can be registered from:
 * 1. Project Settings (gameplay_tags/tag_list)
 * 2. C++ code via register_tag()
 * 3. GDScript via register_tag()
 *
 * Accessible as a singleton: GameplayTagManager.register_tag("Damage.Fire")
 */
class GameplayTagManager : public Object {
	GDCLASS(GameplayTagManager, Object);

private:
	// Tree node representing one level in the tag hierarchy.
	struct TagNode {
		StringName short_name;  // Just the last segment, e.g. "Fire"
		StringName full_name;   // Full path, e.g. "Damage.Fire"
		String comment;         // Developer comment/description.
		TagNode *parent = nullptr;
		HashMap<StringName, TagNode *> children;

		~TagNode() {
			for (KeyValue<StringName, TagNode *> &kv : children) {
				memdelete(kv.value);
			}
		}
	};

	static GameplayTagManager *singleton;

	// Root of the tag tree. Children of root are top-level tags.
	TagNode *root = nullptr;

	// Fast lookup: full tag name -> TagNode pointer.
	HashMap<StringName, TagNode *> tag_lookup;

	// All registered tag names (for quick enumeration).
	HashSet<StringName> registered_tags;

	// Internal: ensure a tag and all its ancestors exist in the tree.
	TagNode *_ensure_tag_path(const StringName &p_tag_name);

	// Internal: recursively collect all descendant tag names.
	static void _collect_descendants(const TagNode *p_node, PackedStringArray &r_result);

protected:
	static void _bind_methods();

public:
	static GameplayTagManager *get_singleton() { return singleton; }

	// --- Registration ---

	// Register a tag (and all its implicit parent tags).
	// e.g. register_tag("Damage.Fire.DoT") also registers "Damage.Fire" and "Damage".
	void register_tag(const StringName &p_tag_name, const String &p_comment = "");

	// Register multiple tags at once.
	void register_tags(const PackedStringArray &p_tag_names);

	// Unregister a tag. Only removes if it has no children.
	// Returns true if removed.
	bool unregister_tag(const StringName &p_tag_name);

	// --- Queries ---

	// Check if a tag is registered.
	bool is_tag_registered(const StringName &p_tag_name) const;

	// Get a tag by name. Returns a new GameplayTag instance.
	Ref<GameplayTag> get_tag(const StringName &p_tag_name) const;

	// Get all registered tag names (sorted).
	PackedStringArray get_all_tag_names() const;

	// Get direct children of a tag (or root-level tags if p_parent is empty).
	PackedStringArray get_children_of(const StringName &p_parent_tag_name) const;

	// Get all descendants of a tag (sorted).
	PackedStringArray get_descendants_of(const StringName &p_parent_tag_name) const;

	// Get the comment for a tag.
	String get_tag_comment(const StringName &p_tag_name) const;

	// Set the comment for a tag.
	void set_tag_comment(const StringName &p_tag_name, const String &p_comment);

	// Get the total number of registered tags.
	int get_tag_count() const;

	// --- Validation ---

	// Validate a tag name format (must be alphanumeric segments separated by dots).
	static bool is_valid_tag_name(const StringName &p_tag_name);

	// --- Convenience ---

	// Create a GameplayTag from a string, registering it if not already registered.
	Ref<GameplayTag> request_tag(const StringName &p_tag_name);

	// Create a GameplayTagContainer from an array of strings.
	Ref<GameplayTagContainer> request_tag_container(const PackedStringArray &p_tag_names);

	// --- Lifecycle ---

	// Load tags from project settings.
	void load_project_tags();

	// Save current tags to project settings.
	void save_project_tags();

	GameplayTagManager();
	~GameplayTagManager();
};