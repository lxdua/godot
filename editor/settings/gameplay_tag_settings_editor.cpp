/**************************************************************************/
/*  gameplay_tag_settings_editor.cpp                                      */
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

#include "gameplay_tag_settings_editor.h"

#include "core/gameplay_tag/gameplay_tag_manager.h"
#include "core/object/callable_mp.h"
#include "editor/editor_string_names.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/button.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/tree.h"

void GameplayTagSettingsEditor::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_THEME_CHANGED: {
			add_button->set_button_icon(get_editor_theme_icon(SNAME("Add")));
			update_tags();
		} break;
	}
}

void GameplayTagSettingsEditor::_add_tag() {
	String tag_name = tag_name_edit->get_text().strip_edges();
	if (tag_name.is_empty()) {
		return;
	}

	StringName tag_sn = StringName(tag_name);
	if (!GameplayTagManager::is_valid_tag_name(tag_sn)) {
		return;
	}

	GameplayTagManager *manager = GameplayTagManager::get_singleton();
	if (!manager) {
		return;
	}

	String comment = tag_comment_edit->get_text().strip_edges();
	manager->register_tag(tag_sn, comment);

	tag_name_edit->clear();
	tag_comment_edit->clear();

	_save_tags();
	update_tags();
	emit_signal(SNAME("tags_changed"));
}

void GameplayTagSettingsEditor::_add_tag_text_submitted(const String &p_text) {
	_add_tag();
}

void GameplayTagSettingsEditor::_tag_name_text_changed(const String &p_text) {
	String stripped = p_text.strip_edges();
	bool valid = !stripped.is_empty() && GameplayTagManager::is_valid_tag_name(StringName(stripped));
	add_button->set_disabled(!valid);
}

void GameplayTagSettingsEditor::_item_button_pressed(Object *p_item, int p_column, int p_id, MouseButton p_button) {
	if (p_button != MouseButton::LEFT) {
		return;
	}

	TreeItem *ti = Object::cast_to<TreeItem>(p_item);
	if (!ti) {
		return;
	}

	if (p_id == BUTTON_DELETE) {
		String tag_name = ti->get_metadata(0);
		pending_remove_tag = StringName(tag_name);

		// Check if tag has children.
		GameplayTagManager *manager = GameplayTagManager::get_singleton();
		PackedStringArray children = manager->get_children_of(pending_remove_tag);
		if (!children.is_empty()) {
			remove_label->set_text(vformat(TTR("Cannot delete tag '%s' because it has %d child tag(s). Delete children first."), tag_name, children.size()));
			remove_dialog->get_ok_button()->set_disabled(true);
		} else {
			remove_label->set_text(vformat(TTR("Delete tag '%s'?"), tag_name));
			remove_dialog->get_ok_button()->set_disabled(false);
		}
		remove_dialog->popup_centered();
	}
}

void GameplayTagSettingsEditor::_item_edited() {
	// Comment column edited.
	TreeItem *ti = tag_tree->get_edited();
	if (!ti) {
		return;
	}

	String tag_name = ti->get_metadata(0);
	String new_comment = ti->get_text(1);

	GameplayTagManager *manager = GameplayTagManager::get_singleton();
	if (manager) {
		manager->set_tag_comment(StringName(tag_name), new_comment);
		_save_tags();
		emit_signal(SNAME("tags_changed"));
	}
}

void GameplayTagSettingsEditor::_confirm_delete() {
	GameplayTagManager *manager = GameplayTagManager::get_singleton();
	if (!manager) {
		return;
	}

	manager->unregister_tag(pending_remove_tag);
	pending_remove_tag = StringName();

	_save_tags();
	update_tags();
	emit_signal(SNAME("tags_changed"));
}

void GameplayTagSettingsEditor::_save_tags() {
	GameplayTagManager *manager = GameplayTagManager::get_singleton();
	if (manager) {
		manager->save_project_tags();
	}
}

void GameplayTagSettingsEditor::update_tags() {
	if (updating) {
		return;
	}
	updating = true;

	tag_tree->clear();

	GameplayTagManager *manager = GameplayTagManager::get_singleton();
	if (!manager) {
		updating = false;
		return;
	}

	TreeItem *root = tag_tree->create_item();

	PackedStringArray all_tags = manager->get_all_tag_names();
	HashMap<StringName, TreeItem *> items;

	for (int i = 0; i < all_tags.size(); i++) {
		const String &tag_name = all_tags[i];
		StringName tag_sn = StringName(tag_name);

		// Find parent TreeItem.
		TreeItem *parent_item = root;
		int last_dot = tag_name.rfind(".");
		if (last_dot != -1) {
			String parent_name = tag_name.substr(0, last_dot);
			StringName parent_sn = StringName(parent_name);
			if (items.has(parent_sn)) {
				parent_item = items[parent_sn];
			}
		}

		TreeItem *item = tag_tree->create_item(parent_item);

		// Column 0: Tag name (short, like "Fire" instead of "Damage.Fire").
		String short_name = (last_dot != -1) ? tag_name.substr(last_dot + 1) : tag_name;
		item->set_text(0, short_name);
		item->set_metadata(0, tag_name);
		item->set_tooltip_text(0, tag_name);

		// Column 1: Comment (editable).
		String comment = manager->get_tag_comment(tag_sn);
		item->set_text(1, comment);
		item->set_editable(1, true);

		// Delete button.
		item->add_button(0, get_editor_theme_icon(SNAME("Remove")), BUTTON_DELETE);

		items[tag_sn] = item;
	}

	updating = false;
}

LineEdit *GameplayTagSettingsEditor::get_name_box() const {
	return tag_name_edit;
}

void GameplayTagSettingsEditor::_bind_methods() {
	ADD_SIGNAL(MethodInfo("tags_changed"));
}

GameplayTagSettingsEditor::GameplayTagSettingsEditor() {
	// --- Top bar: input + add button ---
	HBoxContainer *hbox = memnew(HBoxContainer);
	add_child(hbox);

	tag_name_edit = memnew(LineEdit);
	tag_name_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	tag_name_edit->set_placeholder(TTR("Tag Name (e.g. Damage.Fire.DoT)"));
	tag_name_edit->connect(SceneStringName(text_submitted), callable_mp(this, &GameplayTagSettingsEditor::_add_tag_text_submitted));
	tag_name_edit->connect(SceneStringName(text_changed), callable_mp(this, &GameplayTagSettingsEditor::_tag_name_text_changed));
	hbox->add_child(tag_name_edit);

	tag_comment_edit = memnew(LineEdit);
	tag_comment_edit->set_custom_minimum_size(Size2(200 * EDSCALE, 0));
	tag_comment_edit->set_placeholder(TTR("Comment (optional)"));
	tag_comment_edit->connect(SceneStringName(text_submitted), callable_mp(this, &GameplayTagSettingsEditor::_add_tag_text_submitted));
	hbox->add_child(tag_comment_edit);

	add_button = memnew(Button);
	add_button->set_text(TTR("Add"));
	add_button->set_disabled(true);
	add_button->connect(SceneStringName(pressed), callable_mp(this, &GameplayTagSettingsEditor::_add_tag));
	hbox->add_child(add_button);

	// --- Tag tree ---
	tag_tree = memnew(Tree);
	tag_tree->set_v_size_flags(SIZE_EXPAND_FILL);
	tag_tree->set_hide_root(true);
	tag_tree->set_columns(2);
	tag_tree->set_column_titles_visible(true);
	tag_tree->set_column_title(0, TTR("Tag"));
	tag_tree->set_column_title(1, TTR("Comment"));
	tag_tree->set_column_expand_ratio(0, 3);
	tag_tree->set_column_expand_ratio(1, 2);
	tag_tree->connect("button_clicked", callable_mp(this, &GameplayTagSettingsEditor::_item_button_pressed));
	tag_tree->connect("item_edited", callable_mp(this, &GameplayTagSettingsEditor::_item_edited));
	add_child(tag_tree);

	// --- Delete confirmation dialog ---
	remove_dialog = memnew(ConfirmationDialog);
	remove_dialog->set_title(TTR("Delete Gameplay Tag"));
	remove_label = memnew(Label);
	remove_dialog->add_child(remove_label);
	remove_dialog->connect(SceneStringName(confirmed), callable_mp(this, &GameplayTagSettingsEditor::_confirm_delete));
	add_child(remove_dialog);
}
