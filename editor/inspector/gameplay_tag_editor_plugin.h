/**************************************************************************/
/*  gameplay_tag_editor_plugin.h                                          */
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

#include "editor/gui/gameplay_tag_picker_dialog.h"
#include "editor/inspector/editor_inspector.h"
#include "editor/plugins/editor_plugin.h"

/**
 * Custom container displayed in the Inspector when editing a GameplayTagContainer resource.
 * Shows a list of current tags with remove buttons, plus an "Add Tag" button
 * that opens the GameplayTagPickerDialog.
 */
class GameplayTagContainerEditor : public VBoxContainer {
	GDCLASS(GameplayTagContainerEditor, VBoxContainer);

	Ref<GameplayTagContainer> container;
	GameplayTagPickerDialog *picker_dialog = nullptr;
	VBoxContainer *tag_list = nullptr;

	void _update_tag_list();
	void _add_tag_pressed();
	void _picker_confirmed();
	void _remove_tag(const StringName &p_tag_name);

public:
	void set_container(const Ref<GameplayTagContainer> &p_container);

	GameplayTagContainerEditor();
};

/**
 * EditorInspectorPlugin that handles GameplayTagContainer objects.
 * When the Inspector is editing a GameplayTagContainer, this plugin
 * injects a GameplayTagContainerEditor at the top.
 */
class EditorInspectorPluginGameplayTag : public EditorInspectorPlugin {
	GDCLASS(EditorInspectorPluginGameplayTag, EditorInspectorPlugin);

public:
	virtual bool can_handle(Object *p_object) override;
	virtual void parse_begin(Object *p_object) override;
};

/**
 * EditorPlugin wrapper that registers the inspector plugin.
 */
class GameplayTagEditorPlugin : public EditorPlugin {
	GDCLASS(GameplayTagEditorPlugin, EditorPlugin);

public:
	virtual String get_plugin_name() const override { return "GameplayTag"; }

	GameplayTagEditorPlugin();
};
