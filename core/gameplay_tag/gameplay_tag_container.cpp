/**************************************************************************/
/*  gameplay_tag_container.cpp                                            */
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

#include "gameplay_tag_container.h"

#include "core/object/class_db.h"

void GameplayTagContainer::_rebuild_parent_cache() {
	parent_tags_cache.clear();

	for (int i = 0; i < explicit_tags.size(); i++) {
		const String tag_str = String(explicit_tags[i]);
		// Add all parent paths.
		// e.g. "Damage.Fire.DoT" -> adds "Damage.Fire" and "Damage"
		int pos = tag_str.length();
		while (true) {
			pos = tag_str.rfind(".", pos - 1);
			if (pos == -1) {
				break;
			}
			String parent = tag_str.substr(0, pos);
			parent_tags_cache.insert(StringName(parent));
		}
	}
}

// --- Tag manipulation ---

void GameplayTagContainer::add_tag(const Ref<GameplayTag> &p_tag) {
	ERR_FAIL_COND(p_tag.is_null());
	add_tag_name(p_tag->get_tag_name());
}

void GameplayTagContainer::add_tag_name(const StringName &p_tag_name) {
	ERR_FAIL_COND(p_tag_name == StringName());

	// O(1) dedup check.
	if (explicit_tags_set.has(p_tag_name)) {
		return;
	}

	explicit_tags.push_back(p_tag_name);
	explicit_tags_set.insert(p_tag_name);
	_rebuild_parent_cache();
	emit_changed();
}

void GameplayTagContainer::remove_tag(const Ref<GameplayTag> &p_tag) {
	ERR_FAIL_COND(p_tag.is_null());
	remove_tag_name(p_tag->get_tag_name());
}

void GameplayTagContainer::remove_tag_name(const StringName &p_tag_name) {
	if (!explicit_tags_set.has(p_tag_name)) {
		return;
	}

	for (int i = 0; i < explicit_tags.size(); i++) {
		if (explicit_tags[i] == p_tag_name) {
			explicit_tags.remove_at(i);
			break;
		}
	}
	explicit_tags_set.erase(p_tag_name);
	_rebuild_parent_cache();
	emit_changed();
}

void GameplayTagContainer::clear() {
	if (explicit_tags.is_empty()) {
		return;
	}
	explicit_tags.clear();
	explicit_tags_set.clear();
	parent_tags_cache.clear();
	emit_changed();
}

// --- Queries ---

bool GameplayTagContainer::has_tag(const Ref<GameplayTag> &p_tag) const {
	ERR_FAIL_COND_V(p_tag.is_null(), false);
	return has_tag_name(p_tag->get_tag_name());
}

bool GameplayTagContainer::has_tag_name(const StringName &p_tag_name) const {
	if (p_tag_name == StringName()) {
		return false;
	}

	// O(1) check in explicit tags set.
	if (explicit_tags_set.has(p_tag_name)) {
		return true;
	}

	// Check parent cache (handles hierarchical matching).
	// If "Damage.Fire" is in the parent cache, it means some child like
	// "Damage.Fire.DoT" is an explicit tag.
	return parent_tags_cache.has(p_tag_name);
}

bool GameplayTagContainer::has_tag_exact(const Ref<GameplayTag> &p_tag) const {
	ERR_FAIL_COND_V(p_tag.is_null(), false);
	return has_tag_exact_name(p_tag->get_tag_name());
}

bool GameplayTagContainer::has_tag_exact_name(const StringName &p_tag_name) const {
	return explicit_tags_set.has(p_tag_name);
}

bool GameplayTagContainer::has_any(const Ref<GameplayTagContainer> &p_other) const {
	ERR_FAIL_COND_V(p_other.is_null(), false);

	for (int i = 0; i < p_other->explicit_tags.size(); i++) {
		if (has_tag_name(p_other->explicit_tags[i])) {
			return true;
		}
	}
	return false;
}

bool GameplayTagContainer::has_all(const Ref<GameplayTagContainer> &p_other) const {
	ERR_FAIL_COND_V(p_other.is_null(), false);

	for (int i = 0; i < p_other->explicit_tags.size(); i++) {
		if (!has_tag_name(p_other->explicit_tags[i])) {
			return false;
		}
	}
	return true;
}

bool GameplayTagContainer::has_none(const Ref<GameplayTagContainer> &p_other) const {
	ERR_FAIL_COND_V(p_other.is_null(), false);

	for (int i = 0; i < p_other->explicit_tags.size(); i++) {
		if (has_tag_name(p_other->explicit_tags[i])) {
			return false;
		}
	}
	return true;
}

// --- Accessors ---

int GameplayTagContainer::get_tag_count() const {
	return explicit_tags.size();
}

TypedArray<GameplayTag> GameplayTagContainer::get_tags() const {
	TypedArray<GameplayTag> result;
	for (int i = 0; i < explicit_tags.size(); i++) {
		result.push_back(GameplayTag::create(explicit_tags[i]));
	}
	return result;
}

PackedStringArray GameplayTagContainer::get_tag_names() const {
	PackedStringArray result;
	for (int i = 0; i < explicit_tags.size(); i++) {
		result.push_back(String(explicit_tags[i]));
	}
	return result;
}

void GameplayTagContainer::set_tag_names(const PackedStringArray &p_tag_names) {
	explicit_tags.clear();
	explicit_tags_set.clear();
	for (int i = 0; i < p_tag_names.size(); i++) {
		StringName sn = StringName(p_tag_names[i]);
		if (sn != StringName() && !explicit_tags_set.has(sn)) {
			explicit_tags.push_back(sn);
			explicit_tags_set.insert(sn);
		}
	}
	_rebuild_parent_cache();
	emit_changed();
}

bool GameplayTagContainer::is_empty() const {
	return explicit_tags.is_empty();
}

// --- Set operations ---

Ref<GameplayTagContainer> GameplayTagContainer::union_with(const Ref<GameplayTagContainer> &p_other) const {
	Ref<GameplayTagContainer> result;
	result.instantiate();

	// Add all from this.
	for (int i = 0; i < explicit_tags.size(); i++) {
		result->add_tag_name(explicit_tags[i]);
	}

	// Add all from other.
	if (p_other.is_valid()) {
		for (int i = 0; i < p_other->explicit_tags.size(); i++) {
			result->add_tag_name(p_other->explicit_tags[i]);
		}
	}

	return result;
}

Ref<GameplayTagContainer> GameplayTagContainer::intersection_with(const Ref<GameplayTagContainer> &p_other) const {
	Ref<GameplayTagContainer> result;
	result.instantiate();

	if (p_other.is_null()) {
		return result;
	}

	for (int i = 0; i < explicit_tags.size(); i++) {
		if (p_other->has_tag_exact_name(explicit_tags[i])) {
			result->add_tag_name(explicit_tags[i]);
		}
	}

	return result;
}

Ref<GameplayTagContainer> GameplayTagContainer::difference_with(const Ref<GameplayTagContainer> &p_other) const {
	Ref<GameplayTagContainer> result;
	result.instantiate();

	for (int i = 0; i < explicit_tags.size(); i++) {
		if (p_other.is_null() || !p_other->has_tag_exact_name(explicit_tags[i])) {
			result->add_tag_name(explicit_tags[i]);
		}
	}

	return result;
}

void GameplayTagContainer::append_tags(const Ref<GameplayTagContainer> &p_other) {
	ERR_FAIL_COND(p_other.is_null());

	for (int i = 0; i < p_other->explicit_tags.size(); i++) {
		add_tag_name(p_other->explicit_tags[i]);
	}
}

// --- Utility ---

String GameplayTagContainer::_to_string() const {
	PackedStringArray names = get_tag_names();
	return String(", ").join(names);
}

Ref<GameplayTagContainer> GameplayTagContainer::create_from_array(const PackedStringArray &p_tag_names) {
	Ref<GameplayTagContainer> container;
	container.instantiate();
	container->set_tag_names(p_tag_names);
	return container;
}

void GameplayTagContainer::_bind_methods() {
	// Tag manipulation.
	ClassDB::bind_method(D_METHOD("add_tag", "tag"), &GameplayTagContainer::add_tag);
	ClassDB::bind_method(D_METHOD("add_tag_name", "tag_name"), &GameplayTagContainer::add_tag_name);
	ClassDB::bind_method(D_METHOD("remove_tag", "tag"), &GameplayTagContainer::remove_tag);
	ClassDB::bind_method(D_METHOD("remove_tag_name", "tag_name"), &GameplayTagContainer::remove_tag_name);
	ClassDB::bind_method(D_METHOD("clear"), &GameplayTagContainer::clear);

	// Queries.
	ClassDB::bind_method(D_METHOD("has_tag", "tag"), &GameplayTagContainer::has_tag);
	ClassDB::bind_method(D_METHOD("has_tag_name", "tag_name"), &GameplayTagContainer::has_tag_name);
	ClassDB::bind_method(D_METHOD("has_tag_exact", "tag"), &GameplayTagContainer::has_tag_exact);
	ClassDB::bind_method(D_METHOD("has_tag_exact_name", "tag_name"), &GameplayTagContainer::has_tag_exact_name);
	ClassDB::bind_method(D_METHOD("has_any", "other"), &GameplayTagContainer::has_any);
	ClassDB::bind_method(D_METHOD("has_all", "other"), &GameplayTagContainer::has_all);
	ClassDB::bind_method(D_METHOD("has_none", "other"), &GameplayTagContainer::has_none);

	// Accessors.
	ClassDB::bind_method(D_METHOD("get_tag_count"), &GameplayTagContainer::get_tag_count);
	ClassDB::bind_method(D_METHOD("get_tags"), &GameplayTagContainer::get_tags);
	ClassDB::bind_method(D_METHOD("get_tag_names"), &GameplayTagContainer::get_tag_names);
	ClassDB::bind_method(D_METHOD("set_tag_names", "tag_names"), &GameplayTagContainer::set_tag_names);
	ClassDB::bind_method(D_METHOD("is_empty"), &GameplayTagContainer::is_empty);

	// Set operations.
	ClassDB::bind_method(D_METHOD("union_with", "other"), &GameplayTagContainer::union_with);
	ClassDB::bind_method(D_METHOD("intersection_with", "other"), &GameplayTagContainer::intersection_with);
	ClassDB::bind_method(D_METHOD("difference_with", "other"), &GameplayTagContainer::difference_with);
	ClassDB::bind_method(D_METHOD("append_tags", "other"), &GameplayTagContainer::append_tags);

	// Static.
	ClassDB::bind_static_method("GameplayTagContainer", D_METHOD("create_from_array", "tag_names"), &GameplayTagContainer::create_from_array);

	// Property for serialization.
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "tag_names"), "set_tag_names", "get_tag_names");
}
