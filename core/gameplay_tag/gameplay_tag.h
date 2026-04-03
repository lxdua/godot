/**************************************************************************/
/*  gameplay_tag.h                                                        */
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

#include "core/object/ref_counted.h"
#include "core/string/string_name.h"
#include "core/variant/typed_array.h"

/**
 * GameplayTag represents a single hierarchical tag, e.g. "Damage.Fire.DoT".
 *
 * Tags are registered in the global GameplayTagManager and use StringName
 * internally for fast comparison. The hierarchical structure allows parent
 * matching: an object with tag "Damage.Fire.DoT" will match a query for
 * "Damage.Fire" or "Damage".
 */
class GameplayTag : public RefCounted {
	GDCLASS(GameplayTag, RefCounted);

	StringName tag_name; // Full tag path, e.g. "Damage.Fire.DoT"

protected:
	static void _bind_methods();

public:
	// --- Property accessors ---

	void set_tag_name(const StringName &p_tag_name);
	StringName get_tag_name() const;

	// --- Matching ---

	// Returns true if this tag matches p_other, considering hierarchy.
	// e.g. "Damage.Fire.DoT".matches_tag("Damage.Fire") -> true
	// e.g. "Damage.Fire.DoT".matches_tag("Damage") -> true
	// e.g. "Damage.Fire".matches_tag("Damage.Fire.DoT") -> false
	bool matches_tag(const Ref<GameplayTag> &p_other) const;

	// Returns true if this tag is exactly the same as p_other.
	bool matches_tag_exact(const Ref<GameplayTag> &p_other) const;

	// --- Hierarchy ---

	// Returns the parent tag. e.g. "Damage.Fire.DoT" -> "Damage.Fire"
	// Returns null if this is a root-level tag.
	Ref<GameplayTag> get_parent_tag() const;

	// Returns the tag depth (number of levels).
	// e.g. "Damage" -> 1, "Damage.Fire" -> 2, "Damage.Fire.DoT" -> 3
	int get_depth() const;

	// Returns all ancestor tags (not including self).
	// e.g. "Damage.Fire.DoT" -> ["Damage.Fire", "Damage"]
	TypedArray<GameplayTag> get_ancestor_tags() const;

	// Check if this tag is a direct or indirect child of p_parent.
	bool is_child_of(const Ref<GameplayTag> &p_parent) const;

	// --- Utility ---

	bool is_valid() const;
	String _to_string() const;

	// --- Operators ---

	bool operator==(const GameplayTag &p_other) const;
	bool operator!=(const GameplayTag &p_other) const;
	bool operator<(const GameplayTag &p_other) const;

	_FORCE_INLINE_ uint32_t hash() const { return tag_name.hash(); }

	// --- Static creation ---

	static Ref<GameplayTag> create(const StringName &p_tag_name);

	GameplayTag() {}
	GameplayTag(const StringName &p_tag_name);
};