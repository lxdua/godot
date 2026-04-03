/**************************************************************************/
/*  gameplay_tag_manager.cpp                                              */
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

#include "gameplay_tag_manager.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

GameplayTagManager *GameplayTagManager::singleton = nullptr;

GameplayTagManager::GameplayTagManager() {
	singleton = this;
	root = memnew(TagNode);
	root->short_name = StringName();
	root->full_name = StringName();
}

GameplayTagManager::~GameplayTagManager() {
	if (root) {
		memdelete(root);
		root = nullptr;
	}
	singleton = nullptr;
}

// --- Internal ---

GameplayTagManager::TagNode *GameplayTagManager::_ensure_tag_path(const StringName &p_tag_name) {
	ERR_FAIL_COND_V(p_tag_name == StringName(), nullptr);

	// If already exists, return it.
	if (tag_lookup.has(p_tag_name)) {
		return tag_lookup[p_tag_name];
	}

	const String tag_str = String(p_tag_name);
	Vector<String> segments = tag_str.split(".");

	ERR_FAIL_COND_V(segments.is_empty(), nullptr);

	TagNode *current = root;
	String accumulated_path;

	for (int i = 0; i < segments.size(); i++) {
		const String &segment = segments[i];
		ERR_FAIL_COND_V(segment.is_empty(), nullptr);

		StringName segment_sn = StringName(segment);

		if (i > 0) {
			accumulated_path += ".";
		}
		accumulated_path += segment;
		StringName full_path_sn = StringName(accumulated_path);

		if (!current->children.has(segment_sn)) {
			// Create new node.
			TagNode *new_node = memnew(TagNode);
			new_node->short_name = segment_sn;
			new_node->full_name = full_path_sn;
			new_node->parent = current;

			current->children[segment_sn] = new_node;
			tag_lookup[full_path_sn] = new_node;
			registered_tags.insert(full_path_sn);
		}

		current = current->children[segment_sn];
	}

	return current;
}

// --- Registration ---

void GameplayTagManager::register_tag(const StringName &p_tag_name, const String &p_comment) {
	ERR_FAIL_COND_MSG(p_tag_name == StringName(), "Cannot register an empty tag name.");
	ERR_FAIL_COND_MSG(!is_valid_tag_name(p_tag_name), vformat("Invalid tag name: '%s'. Tags must be alphanumeric segments separated by dots.", String(p_tag_name)));

	bool already_existed = tag_lookup.has(p_tag_name);

	TagNode *node = _ensure_tag_path(p_tag_name);
	if (node && !p_comment.is_empty()) {
		node->comment = p_comment;
	}

	if (!already_existed) {
		emit_signal(SNAME("tag_registered"), p_tag_name);
	}
}

void GameplayTagManager::register_tags(const PackedStringArray &p_tag_names) {
	for (int i = 0; i < p_tag_names.size(); i++) {
		register_tag(StringName(p_tag_names[i]));
	}
}

bool GameplayTagManager::unregister_tag(const StringName &p_tag_name) {
	if (!tag_lookup.has(p_tag_name)) {
		return false;
	}

	TagNode *node = tag_lookup[p_tag_name];

	// Can't remove if it has children.
	if (!node->children.is_empty()) {
		WARN_PRINT(vformat("Cannot unregister tag '%s' because it has child tags.", String(p_tag_name)));
		return false;
	}

	// Remove from parent's children.
	if (node->parent) {
		node->parent->children.erase(node->short_name);
	}

	// Remove from lookup and registry.
	tag_lookup.erase(p_tag_name);
	registered_tags.erase(p_tag_name);

	memdelete(node);
	emit_signal(SNAME("tag_unregistered"), p_tag_name);
	return true;
}

// --- Queries ---

bool GameplayTagManager::is_tag_registered(const StringName &p_tag_name) const {
	return registered_tags.has(p_tag_name);
}

Ref<GameplayTag> GameplayTagManager::get_tag(const StringName &p_tag_name) const {
	if (!registered_tags.has(p_tag_name)) {
		return Ref<GameplayTag>();
	}
	return GameplayTag::create(p_tag_name);
}

PackedStringArray GameplayTagManager::get_all_tag_names() const {
	PackedStringArray result;
	for (const StringName &tag : registered_tags) {
		result.push_back(String(tag));
	}
	// Sort for consistent ordering.
	result.sort();
	return result;
}

PackedStringArray GameplayTagManager::get_children_of(const StringName &p_parent_tag_name) const {
	PackedStringArray result;

	const TagNode *parent_node = nullptr;
	if (p_parent_tag_name == StringName()) {
		parent_node = root;
	} else if (tag_lookup.has(p_parent_tag_name)) {
		parent_node = tag_lookup[p_parent_tag_name];
	}

	if (parent_node) {
		for (const KeyValue<StringName, TagNode *> &kv : parent_node->children) {
			result.push_back(String(kv.value->full_name));
		}
		result.sort();
	}

	return result;
}

void GameplayTagManager::_collect_descendants(const TagNode *p_node, PackedStringArray &r_result) {
	for (const KeyValue<StringName, TagNode *> &kv : p_node->children) {
		r_result.push_back(String(kv.value->full_name));
		_collect_descendants(kv.value, r_result);
	}
}

PackedStringArray GameplayTagManager::get_descendants_of(const StringName &p_parent_tag_name) const {
	PackedStringArray result;

	const TagNode *parent_node = nullptr;
	if (p_parent_tag_name == StringName()) {
		parent_node = root;
	} else if (tag_lookup.has(p_parent_tag_name)) {
		parent_node = tag_lookup[p_parent_tag_name];
	}

	if (parent_node) {
		_collect_descendants(parent_node, result);
		result.sort();
	}

	return result;
}

String GameplayTagManager::get_tag_comment(const StringName &p_tag_name) const {
	if (!tag_lookup.has(p_tag_name)) {
		return String();
	}
	return tag_lookup[p_tag_name]->comment;
}

void GameplayTagManager::set_tag_comment(const StringName &p_tag_name, const String &p_comment) {
	if (tag_lookup.has(p_tag_name)) {
		tag_lookup[p_tag_name]->comment = p_comment;
	}
}

int GameplayTagManager::get_tag_count() const {
	return registered_tags.size();
}

// --- Validation ---

bool GameplayTagManager::is_valid_tag_name(const StringName &p_tag_name) {
	const String tag_str = String(p_tag_name);
	if (tag_str.is_empty()) {
		return false;
	}

	// Must not start or end with a dot.
	if (tag_str.begins_with(".") || tag_str.ends_with(".")) {
		return false;
	}

	// Must not have consecutive dots.
	if (tag_str.find("..") != -1) {
		return false;
	}

	// Each segment must be non-empty and contain only alphanumeric chars and underscores.
	Vector<String> segments = tag_str.split(".");
	for (int i = 0; i < segments.size(); i++) {
		const String &seg = segments[i];
		if (seg.is_empty()) {
			return false;
		}
		for (int j = 0; j < seg.length(); j++) {
			char32_t c = seg[j];
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
				return false;
			}
		}
	}

	return true;
}

// --- Convenience ---

Ref<GameplayTag> GameplayTagManager::request_tag(const StringName &p_tag_name) {
	if (!is_tag_registered(p_tag_name)) {
		register_tag(p_tag_name);
	}
	return GameplayTag::create(p_tag_name);
}

Ref<GameplayTagContainer> GameplayTagManager::request_tag_container(const PackedStringArray &p_tag_names) {
	for (int i = 0; i < p_tag_names.size(); i++) {
		StringName sn = StringName(p_tag_names[i]);
		if (!is_tag_registered(sn)) {
			register_tag(sn);
		}
	}
	return GameplayTagContainer::create_from_array(p_tag_names);
}

// --- Lifecycle ---

void GameplayTagManager::load_project_tags() {
	if (!ProjectSettings::get_singleton()) {
		return;
	}

	// Define the project setting if it doesn't exist.
	if (!ProjectSettings::get_singleton()->has_setting("gameplay_tags/tag_list")) {
		ProjectSettings::get_singleton()->set_setting("gameplay_tags/tag_list", PackedStringArray());
		ProjectSettings::get_singleton()->set_initial_value("gameplay_tags/tag_list", PackedStringArray());
	}

	// Hide from the General settings inspector (managed via the Gameplay Tags tab instead).
	ProjectSettings::get_singleton()->set_custom_property_info(PropertyInfo(Variant::PACKED_STRING_ARRAY, "gameplay_tags/tag_list", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE));

	PackedStringArray tags = ProjectSettings::get_singleton()->get_setting("gameplay_tags/tag_list", PackedStringArray());
	for (int i = 0; i < tags.size(); i++) {
		StringName sn = StringName(tags[i]);
		if (sn != StringName() && is_valid_tag_name(sn)) {
			_ensure_tag_path(sn);
		}
	}
}

void GameplayTagManager::save_project_tags() {
	if (!ProjectSettings::get_singleton()) {
		return;
	}

	PackedStringArray tags = get_all_tag_names();
	ProjectSettings::get_singleton()->set_setting("gameplay_tags/tag_list", tags);
}

// --- Bindings ---

void GameplayTagManager::_bind_methods() {
	// Registration.
	ClassDB::bind_method(D_METHOD("register_tag", "tag_name", "comment"), &GameplayTagManager::register_tag, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("register_tags", "tag_names"), &GameplayTagManager::register_tags);
	ClassDB::bind_method(D_METHOD("unregister_tag", "tag_name"), &GameplayTagManager::unregister_tag);

	// Queries.
	ClassDB::bind_method(D_METHOD("is_tag_registered", "tag_name"), &GameplayTagManager::is_tag_registered);
	ClassDB::bind_method(D_METHOD("get_tag", "tag_name"), &GameplayTagManager::get_tag);
	ClassDB::bind_method(D_METHOD("get_all_tag_names"), &GameplayTagManager::get_all_tag_names);
	ClassDB::bind_method(D_METHOD("get_children_of", "parent_tag_name"), &GameplayTagManager::get_children_of);
	ClassDB::bind_method(D_METHOD("get_descendants_of", "parent_tag_name"), &GameplayTagManager::get_descendants_of);
	ClassDB::bind_method(D_METHOD("get_tag_comment", "tag_name"), &GameplayTagManager::get_tag_comment);
	ClassDB::bind_method(D_METHOD("set_tag_comment", "tag_name", "comment"), &GameplayTagManager::set_tag_comment);
	ClassDB::bind_method(D_METHOD("get_tag_count"), &GameplayTagManager::get_tag_count);

	// Validation.
	ClassDB::bind_static_method("GameplayTagManager", D_METHOD("is_valid_tag_name", "tag_name"), &GameplayTagManager::is_valid_tag_name);

	// Convenience.
	ClassDB::bind_method(D_METHOD("request_tag", "tag_name"), &GameplayTagManager::request_tag);
	ClassDB::bind_method(D_METHOD("request_tag_container", "tag_names"), &GameplayTagManager::request_tag_container);

	// Lifecycle.
	ClassDB::bind_method(D_METHOD("load_project_tags"), &GameplayTagManager::load_project_tags);
	ClassDB::bind_method(D_METHOD("save_project_tags"), &GameplayTagManager::save_project_tags);

	// Signals.
	ADD_SIGNAL(MethodInfo("tag_registered", PropertyInfo(Variant::STRING_NAME, "tag_name")));
	ADD_SIGNAL(MethodInfo("tag_unregistered", PropertyInfo(Variant::STRING_NAME, "tag_name")));
}
