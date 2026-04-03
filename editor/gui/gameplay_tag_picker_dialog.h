/**************************************************************************/
/*  gameplay_tag_picker_dialog.h                                          */
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

#include "scene/gui/dialogs.h"

class LineEdit;
class Tree;
class TreeItem;

/**
 * A reusable dialog for selecting gameplay tags from the registered tag tree.
 * Supports single-select and multi-select modes, with a search filter.
 *
 * Used by:
 * - GameplayTagSettingsEditor (Project Settings panel)
 * - EditorPropertyGameplayTagContainer (Inspector property editor)
 */
class GameplayTagPickerDialog : public ConfirmationDialog {
	GDCLASS(GameplayTagPickerDialog, ConfirmationDialog);

	bool multi_select = false;

	LineEdit *search_box = nullptr;
	Tree *tag_tree = nullptr;

	// Maps full tag name -> TreeItem for quick lookup.
	HashMap<StringName, TreeItem *> tag_items;

	void _update_tree();
	void _search_text_changed(const String &p_text);
	void _item_activated();
	bool _filter_tree(TreeItem *p_item, const String &p_filter);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_multi_select(bool p_multi_select);
	bool is_multi_select() const;

	// Get the selected tag name (single-select mode).
	StringName get_selected_tag() const;

	// Get all checked tag names (multi-select mode).
	PackedStringArray get_selected_tags() const;

	// Pre-select tags (for multi-select mode).
	void set_selected_tags(const PackedStringArray &p_tags);

	// Refresh the tree from GameplayTagManager.
	void refresh();

	GameplayTagPickerDialog();
};
