/**************************************************************************/
/*  gameplay_tag.cpp                                                      */
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

#include "gameplay_tag.h"

#include "core/object/class_db.h"

GameplayTag::GameplayTag(const StringName &p_tag_name) :
		tag_name(p_tag_name) {
}

void GameplayTag::set_tag_name(const StringName &p_tag_name) {
	tag_name = p_tag_name;
}

StringName GameplayTag::get_tag_name() const {
	return tag_name;
}

bool GameplayTag::matches_tag(const Ref<GameplayTag> &p_other) const {
	if (p_other.is_null() || !is_valid() || !p_other->is_valid()) {
		return false;
	}

	// Exact match is always a match.
	if (tag_name == p_other->tag_name) {
		return true;
	}

	// Check if this tag starts with p_other's tag followed by a dot.
	// e.g. "Damage.Fire.DoT" starts with "Damage.Fire."
	// Avoids heap allocation by comparing length + prefix + separator directly.
	const String this_str = String(tag_name);
	const String other_str = String(p_other->tag_name);

	if (this_str.length() <= other_str.length()) {
		return false; // this must be longer than other to be a child.
	}

	// Check the character right after other's length is a dot separator.
	if (this_str[other_str.length()] != '.') {
		return false;
	}

	// Check the prefix matches.
	for (int i = 0; i < other_str.length(); i++) {
		if (this_str[i] != other_str[i]) {
			return false;
		}
	}

	return true;
}

bool GameplayTag::matches_tag_exact(const Ref<GameplayTag> &p_other) const {
	if (p_other.is_null() || !is_valid() || !p_other->is_valid()) {
		return false;
	}
	return tag_name == p_other->tag_name;
}

Ref<GameplayTag> GameplayTag::get_parent_tag() const {
	if (!is_valid()) {
		return Ref<GameplayTag>();
	}

	const String tag_str = String(tag_name);
	int last_dot = tag_str.rfind(".");
	if (last_dot == -1) {
		// Root-level tag, no parent.
		return Ref<GameplayTag>();
	}

	String parent_str = tag_str.substr(0, last_dot);
	return GameplayTag::create(StringName(parent_str));
}

int GameplayTag::get_depth() const {
	if (!is_valid()) {
		return 0;
	}

	const String tag_str = String(tag_name);
	int depth = 1;
	for (int i = 0; i < tag_str.length(); i++) {
		if (tag_str[i] == '.') {
			depth++;
		}
	}
	return depth;
}

TypedArray<GameplayTag> GameplayTag::get_ancestor_tags() const {
	TypedArray<GameplayTag> ancestors;
	if (!is_valid()) {
		return ancestors;
	}

	Ref<GameplayTag> current = get_parent_tag();
	while (current.is_valid()) {
		ancestors.push_back(current);
		current = current->get_parent_tag();
	}
	return ancestors;
}

bool GameplayTag::is_child_of(const Ref<GameplayTag> &p_parent) const {
	return matches_tag(p_parent) && !matches_tag_exact(p_parent);
}

String GameplayTag::_to_string() const {
	return String(tag_name);
}

bool GameplayTag::is_valid() const {
	return !tag_name.is_empty();
}

bool GameplayTag::operator==(const GameplayTag &p_other) const {
	return tag_name == p_other.tag_name;
}

bool GameplayTag::operator!=(const GameplayTag &p_other) const {
	return tag_name != p_other.tag_name;
}

bool GameplayTag::operator<(const GameplayTag &p_other) const {
	return String(tag_name) < String(p_other.tag_name);
}

Ref<GameplayTag> GameplayTag::create(const StringName &p_tag_name) {
	Ref<GameplayTag> tag;
	tag.instantiate();
	tag->set_tag_name(p_tag_name);
	return tag;
}

void GameplayTag::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_tag_name", "tag_name"), &GameplayTag::set_tag_name);
	ClassDB::bind_method(D_METHOD("get_tag_name"), &GameplayTag::get_tag_name);

	ClassDB::bind_method(D_METHOD("matches_tag", "other"), &GameplayTag::matches_tag);
	ClassDB::bind_method(D_METHOD("matches_tag_exact", "other"), &GameplayTag::matches_tag_exact);
	ClassDB::bind_method(D_METHOD("get_parent_tag"), &GameplayTag::get_parent_tag);
	ClassDB::bind_method(D_METHOD("get_depth"), &GameplayTag::get_depth);
	ClassDB::bind_method(D_METHOD("get_ancestor_tags"), &GameplayTag::get_ancestor_tags);
	ClassDB::bind_method(D_METHOD("is_child_of", "parent"), &GameplayTag::is_child_of);
	ClassDB::bind_method(D_METHOD("is_valid"), &GameplayTag::is_valid);
	ClassDB::bind_method(D_METHOD("_to_string"), &GameplayTag::_to_string);

	ClassDB::bind_static_method("GameplayTag", D_METHOD("create", "tag_name"), &GameplayTag::create);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "tag_name"), "set_tag_name", "get_tag_name");
}
