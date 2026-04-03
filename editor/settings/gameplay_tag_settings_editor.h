/**************************************************************************/
/*  gameplay_tag_settings_editor.h                                        */
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

#include "scene/gui/box_container.h"

class Button;
class ConfirmationDialog;
class Label;
class LineEdit;
class Tree;
class TreeItem;

/**
 * Editor panel for managing gameplay tags in Project Settings.
 * Embedded as a tab in the Globals section alongside Groups, Shader Globals, etc.
 *
 * Provides:
 * - Add new tags (with dot-separated hierarchy)
 * - Tree view of all registered tags
 * - Delete tags (leaf-only)
 * - Inline comment editing
 */
class GameplayTagSettingsEditor : public VBoxContainer {
	GDCLASS(GameplayTagSettingsEditor, VBoxContainer);

	bool updating = false;

	LineEdit *tag_name_edit = nullptr;
	LineEdit *tag_comment_edit = nullptr;
	Button *add_button = nullptr;
	Tree *tag_tree = nullptr;

	ConfirmationDialog *remove_dialog = nullptr;
	Label *remove_label = nullptr;
	StringName pending_remove_tag;

	// Button IDs for tree item buttons.
	enum {
		BUTTON_DELETE = 0,
	};

	void _add_tag();
	void _add_tag_text_submitted(const String &p_text);
	void _tag_name_text_changed(const String &p_text);

	void _item_button_pressed(Object *p_item, int p_column, int p_id, MouseButton p_button);
	void _item_edited();

	void _confirm_delete();

	void _save_tags();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	LineEdit *get_name_box() const;
	void update_tags();

	GameplayTagSettingsEditor();
};
