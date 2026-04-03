/**************************************************************************/
/*  gameplay_tag_editor_plugin.cpp                                        */
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

#include "gameplay_tag_editor_plugin.h"

#include "core/gameplay_tag/gameplay_tag_container.h"
#include "core/object/callable_mp.h"
#include "editor/editor_string_names.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"

// --- GameplayTagContainerEditor ---

void GameplayTagContainerEditor::_update_tag_list() {
	// Remove old tag widgets.
	while (tag_list->get_child_count() > 0) {
		Node *child = tag_list->get_child(0);
		tag_list->remove_child(child);
		child->queue_free();
	}

	if (container.is_null()) {
		return;
	}

	PackedStringArray tag_names = container->get_tag_names();
	for (int i = 0; i < tag_names.size(); i++) {
		HBoxContainer *row = memnew(HBoxContainer);

		Label *label = memnew(Label);
		label->set_text(tag_names[i]);
		label->set_h_size_flags(Control::SIZE_EXPAND_FILL);
		label->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
		row->add_child(label);

		Button *remove_btn = memnew(Button);
		remove_btn->set_flat(true);
		remove_btn->set_button_icon(get_theme_icon(SNAME("Remove"), EditorStringName(EditorIcons)));
		remove_btn->set_tooltip_text(TTR("Remove Tag"));
		StringName tag_sn = StringName(tag_names[i]);
		remove_btn->connect(SceneStringName(pressed), callable_mp(this, &GameplayTagContainerEditor::_remove_tag).bind(tag_sn));
		row->add_child(remove_btn);

		tag_list->add_child(row);
	}

	if (tag_names.is_empty()) {
		Label *empty_label = memnew(Label);
		empty_label->set_text(TTR("No tags. Click 'Add Tag' to add one."));
		empty_label->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
		empty_label->add_theme_color_override(SceneStringName(font_color), Color(0.5, 0.5, 0.5));
		tag_list->add_child(empty_label);
	}
}

void GameplayTagContainerEditor::_add_tag_pressed() {
	picker_dialog->set_multi_select(true);
	if (container.is_valid()) {
		picker_dialog->set_selected_tags(container->get_tag_names());
	}
	picker_dialog->popup_centered();
}

void GameplayTagContainerEditor::_picker_confirmed() {
	if (container.is_null()) {
		return;
	}

	PackedStringArray selected = picker_dialog->get_selected_tags();

	// Replace all tags with the selection.
	container->set_tag_names(selected);
	_update_tag_list();
}

void GameplayTagContainerEditor::_remove_tag(const StringName &p_tag_name) {
	if (container.is_valid()) {
		container->remove_tag_name(p_tag_name);
		_update_tag_list();
	}
}

void GameplayTagContainerEditor::set_container(const Ref<GameplayTagContainer> &p_container) {
	container = p_container;
	_update_tag_list();

	if (container.is_valid()) {
		container->connect_changed(callable_mp(this, &GameplayTagContainerEditor::_update_tag_list));
	}
}

GameplayTagContainerEditor::GameplayTagContainerEditor() {
	tag_list = memnew(VBoxContainer);
	add_child(tag_list);

	Button *add_btn = memnew(Button);
	add_btn->set_text(TTR("Add Tag..."));
	add_btn->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	add_btn->connect(SceneStringName(pressed), callable_mp(this, &GameplayTagContainerEditor::_add_tag_pressed));
	add_child(add_btn);

	picker_dialog = memnew(GameplayTagPickerDialog);
	picker_dialog->connect(SceneStringName(confirmed), callable_mp(this, &GameplayTagContainerEditor::_picker_confirmed));
	add_child(picker_dialog);
}

// --- EditorInspectorPluginGameplayTag ---

bool EditorInspectorPluginGameplayTag::can_handle(Object *p_object) {
	return Object::cast_to<GameplayTagContainer>(p_object) != nullptr;
}

void EditorInspectorPluginGameplayTag::parse_begin(Object *p_object) {
	Ref<GameplayTagContainer> container = Ref<GameplayTagContainer>(Object::cast_to<GameplayTagContainer>(p_object));
	if (container.is_null()) {
		return;
	}

	GameplayTagContainerEditor *editor = memnew(GameplayTagContainerEditor);
	editor->set_container(container);
	add_custom_control(editor);
}

// --- GameplayTagEditorPlugin ---

GameplayTagEditorPlugin::GameplayTagEditorPlugin() {
	Ref<EditorInspectorPluginGameplayTag> plugin;
	plugin.instantiate();
	add_inspector_plugin(plugin);
}
