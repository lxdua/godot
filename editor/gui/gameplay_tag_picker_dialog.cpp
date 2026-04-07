/**************************************************************************/
/*  gameplay_tag_picker_dialog.cpp                                        */
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

#include "gameplay_tag_picker_dialog.h"

#include "core/gameplay_tag/gameplay_tag_manager.h"
#include "core/object/callable_mp.h"
#include "scene/gui/box_container.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/tree.h"

void GameplayTagPickerDialog::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_VISIBILITY_CHANGED: {
			if (is_visible()) {
				refresh();

				// Restore pending selection after tree rebuild.
				if (has_pending_selection) {
					set_selected_tags(pending_selected_tags);
					has_pending_selection = false;
				}

				search_box->clear();
				search_box->grab_focus();
			}
		} break;
	}
}

void GameplayTagPickerDialog::_update_tree() {
	tag_tree->clear();
	tag_items.clear();

	TreeItem *root = tag_tree->create_item();
	tag_tree->set_hide_root(true);

	GameplayTagManager *manager = GameplayTagManager::get_singleton();
	if (!manager) {
		return;
	}

	PackedStringArray all_tags = manager->get_all_tag_names();

	// Build tree items in sorted order.
	// Since tags are sorted, parents always come before children.
	for (int i = 0; i < all_tags.size(); i++) {
		const String &tag_name = all_tags[i];
		StringName tag_sn = StringName(tag_name);

		// Find parent TreeItem.
		TreeItem *parent_item = root;
		int last_dot = tag_name.rfind(".");
		if (last_dot != -1) {
			String parent_name = tag_name.substr(0, last_dot);
			StringName parent_sn = StringName(parent_name);
			if (tag_items.has(parent_sn)) {
				parent_item = tag_items[parent_sn];
			}
		}

		TreeItem *item = tag_tree->create_item(parent_item);

		// In multi-select mode, set cell mode BEFORE setting text
		// (set_cell_mode clears existing text).
		if (multi_select) {
			item->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
			item->set_editable(0, true);
		}

		// Display only the last segment as label.
		String short_name = (last_dot != -1) ? tag_name.substr(last_dot + 1) : tag_name;
		item->set_text(0, short_name);
		item->set_metadata(0, tag_name);
		item->set_tooltip_text(0, tag_name);

		tag_items[tag_sn] = item;
	}
}

void GameplayTagPickerDialog::_search_text_changed(const String &p_text) {
	if (p_text.is_empty()) {
		// Show all items.
		TreeItem *root = tag_tree->get_root();
		if (root) {
			_filter_tree(root, "");
		}
		return;
	}

	TreeItem *root = tag_tree->get_root();
	if (root) {
		_filter_tree(root, p_text.to_lower());
	}
}

bool GameplayTagPickerDialog::_filter_tree(TreeItem *p_item, const String &p_filter) {
	bool any_visible = false;

	TreeItem *child = p_item->get_first_child();
	while (child) {
		bool child_visible = _filter_tree(child, p_filter);

		if (p_filter.is_empty()) {
			child->set_visible(true);
			child->set_collapsed(false);
			any_visible = true;
		} else {
			String full_name = child->get_metadata(0);
			bool matches = full_name.to_lower().contains(p_filter);

			if (matches || child_visible) {
				child->set_visible(true);
				child->set_collapsed(false);
				any_visible = true;
			} else {
				child->set_visible(false);
			}
		}

		child = child->get_next();
	}

	return any_visible;
}

void GameplayTagPickerDialog::_item_activated() {
	if (!multi_select) {
		// In single-select, double-click confirms.
		emit_signal(SNAME("confirmed"));
		hide();
	}
}

void GameplayTagPickerDialog::set_multi_select(bool p_multi_select) {
	multi_select = p_multi_select;
}

bool GameplayTagPickerDialog::is_multi_select() const {
	return multi_select;
}

StringName GameplayTagPickerDialog::get_selected_tag() const {
	TreeItem *selected = tag_tree->get_selected();
	if (selected) {
		return StringName(String(selected->get_metadata(0)));
	}
	return StringName();
}

PackedStringArray GameplayTagPickerDialog::get_selected_tags() const {
	PackedStringArray result;

	TreeItem *root = tag_tree->get_root();
	if (!root) {
		return result;
	}

	// Traverse all items and collect checked ones.
	TreeItem *item = root->get_first_child();
	while (item) {
		if (item->is_checked(0)) {
			result.push_back(String(item->get_metadata(0)));
		}

		// Depth-first traversal.
		if (item->get_first_child()) {
			item = item->get_first_child();
		} else if (item->get_next()) {
			item = item->get_next();
		} else {
			// Go back up to find next sibling.
			while (item) {
				item = item->get_parent();
				if (item == root) {
					item = nullptr;
					break;
				}
				if (item && item->get_next()) {
					item = item->get_next();
					break;
				}
			}
		}
	}

	return result;
}

void GameplayTagPickerDialog::set_selected_tags(const PackedStringArray &p_tags) {
	// First, uncheck everything.
	for (KeyValue<StringName, TreeItem *> &kv : tag_items) {
		kv.value->set_checked(0, false);
	}

	// Check the specified tags and expand their ancestor nodes.
	for (int i = 0; i < p_tags.size(); i++) {
		StringName tag_sn = StringName(p_tags[i]);
		if (tag_items.has(tag_sn)) {
			TreeItem *item = tag_items[tag_sn];
			item->set_checked(0, true);

			// Expand all ancestor nodes so the checked item is visible.
			TreeItem *parent = item->get_parent();
			while (parent && parent != tag_tree->get_root()) {
				parent->set_collapsed(false);
				parent = parent->get_parent();
			}
		}
	}
}

void GameplayTagPickerDialog::set_pending_selected_tags(const PackedStringArray &p_tags) {
	pending_selected_tags = p_tags;
	has_pending_selection = true;
}

void GameplayTagPickerDialog::refresh() {
	_update_tree();
}

void GameplayTagPickerDialog::_bind_methods() {
}

GameplayTagPickerDialog::GameplayTagPickerDialog() {
	set_title(TTR("Select Gameplay Tag"));
	set_min_size(Size2(400, 500));

	VBoxContainer *vbox = memnew(VBoxContainer);
	add_child(vbox);

	search_box = memnew(LineEdit);
	search_box->set_placeholder(TTR("Filter Tags..."));
	search_box->set_clear_button_enabled(true);
	search_box->connect(SceneStringName(text_changed), callable_mp(this, &GameplayTagPickerDialog::_search_text_changed));
	vbox->add_child(search_box);

	tag_tree = memnew(Tree);
	tag_tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	tag_tree->set_hide_root(true);
	tag_tree->connect("item_activated", callable_mp(this, &GameplayTagPickerDialog::_item_activated));
	vbox->add_child(tag_tree);
}
