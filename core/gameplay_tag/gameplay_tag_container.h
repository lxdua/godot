/**************************************************************************/
/*  gameplay_tag_container.h                                              */
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
#include "core/io/resource.h"
#include "core/templates/hash_set.h"
#include "core/variant/typed_array.h"

/**
 * GameplayTagContainer holds a collection of GameplayTags.
 *
 * It automatically maintains a cached set of all parent (ancestor) tags
 * for fast hierarchical matching. For example, if the container holds
 * "Damage.Fire.DoT", it implicitly also holds "Damage.Fire" and "Damage"
 * for matching purposes.
 *
 * Inherits from Resource so it can be saved/loaded as .tres files and
 * used as an @export property in GDScript.
 */
class GameplayTagContainer : public Resource {
	GDCLASS(GameplayTagContainer, Resource);

	// Explicit tags added by the user (ordered, for serialization and iteration).
	Vector<StringName> explicit_tags;

	// Fast lookup set for explicit tags (for O(1) dedup and exact queries).
	HashSet<StringName> explicit_tags_set;

	// Cached set of all parent tags (for fast hierarchical queries).
	// Rebuilt whenever explicit_tags changes.
	HashSet<StringName> parent_tags_cache;

	// Rebuild the parent tags cache from explicit_tags.
	void _rebuild_parent_cache();

protected:
	static void _bind_methods();

public:
	// --- Tag manipulation ---

	// Add a tag to the container. No-op if already present.
	void add_tag(const Ref<GameplayTag> &p_tag);

	// Add a tag by StringName directly.
	void add_tag_name(const StringName &p_tag_name);

	// Remove a tag from the container.
	void remove_tag(const Ref<GameplayTag> &p_tag);

	// Remove a tag by StringName directly.
	void remove_tag_name(const StringName &p_tag_name);

	// Remove all tags.
	void clear();

	// --- Queries (hierarchical matching) ---

	// Returns true if the container has a tag that matches p_tag (hierarchy-aware).
	// e.g. If container has "Damage.Fire.DoT", has_tag("Damage.Fire") returns true.
	bool has_tag(const Ref<GameplayTag> &p_tag) const;

	// String-based shorthand.
	bool has_tag_name(const StringName &p_tag_name) const;

	// Returns true only if the exact tag is present (no hierarchy).
	bool has_tag_exact(const Ref<GameplayTag> &p_tag) const;

	// String-based shorthand.
	bool has_tag_exact_name(const StringName &p_tag_name) const;

	// Returns true if the container has ANY of the tags in p_other.
	bool has_any(const Ref<GameplayTagContainer> &p_other) const;

	// Returns true if the container has ALL of the tags in p_other.
	bool has_all(const Ref<GameplayTagContainer> &p_other) const;

	// Returns true if the container has NONE of the tags in p_other.
	bool has_none(const Ref<GameplayTagContainer> &p_other) const;

	// --- Accessors ---

	// Get the number of explicit tags.
	int get_tag_count() const;

	// Get all explicit tags as an array of GameplayTag.
	TypedArray<GameplayTag> get_tags() const;

	// Get all explicit tag names as StringName array.
	PackedStringArray get_tag_names() const;

	// Set tags from an array of StringNames (for serialization).
	void set_tag_names(const PackedStringArray &p_tag_names);

	// Check if the container is empty.
	bool is_empty() const;

	// --- Set operations ---

	// Return a new container with the union of this and p_other.
	Ref<GameplayTagContainer> union_with(const Ref<GameplayTagContainer> &p_other) const;

	// Return a new container with the intersection of this and p_other.
	Ref<GameplayTagContainer> intersection_with(const Ref<GameplayTagContainer> &p_other) const;

	// Return a new container with tags in this but not in p_other.
	Ref<GameplayTagContainer> difference_with(const Ref<GameplayTagContainer> &p_other) const;

	// Append all tags from p_other into this container.
	void append_tags(const Ref<GameplayTagContainer> &p_other);

	// --- Utility ---

	String _to_string() const;

	// Static creation helper.
	static Ref<GameplayTagContainer> create_from_array(const PackedStringArray &p_tag_names);

	GameplayTagContainer() {}
};
