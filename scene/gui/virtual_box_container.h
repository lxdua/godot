/**************************************************************************/
/*  virtual_box_container.h                                               */
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

#include "scene/gui/container.h"
#include "scene/resources/packed_scene.h"

class ScrollContainer;

// Virtualized vertical list container.
//
// Only instantiates Control nodes for items currently inside the viewport
// (plus a small buffer), recycling nodes through an internal pool as the
// viewport scrolls. Designed for large homogeneous data sets (1000+ items)
// where a plain `BoxContainer` would create one full Control subtree per
// item and become memory / CPU bound.
//
// Expected usage:
//   ScrollContainer
//     └── VirtualBoxContainer
//
// The container queries its first ancestor `ScrollContainer` for the
// current scroll offset; if no `ScrollContainer` is found it degrades to
// instantiating every item (no virtualization), which preserves
// correctness but loses the performance benefit.
class VirtualBoxContainer : public Container {
	GDCLASS(VirtualBoxContainer, Container);

public:
	enum ScrollAlign {
		ALIGN_VISIBLE,
		ALIGN_START,
		ALIGN_CENTER,
		ALIGN_END,
	};

private:
	// --- Template & data scale ---
	Ref<PackedScene> item_scene;
	int item_count = 0;

	// --- Layout ---
	// If true (default), items stride along the Y axis and the container
	// expects to be hosted inside a vertically-scrolling ScrollContainer.
	// If false, items stride along the X axis (horizontal list) and we
	// subscribe to the parent's HScrollBar instead.
	//
	// Semantics of `item_min_size` follows BoxContainer:
	//   vertical=true  -> x = cross-axis width, y = main-axis item height
	//   vertical=false -> x = main-axis item width, y = cross-axis height
	bool vertical = true;
	Vector2 item_min_size = Vector2(0, 48);

	// --- Buffer / pool ---
	int buffer_items = 2;
	int pool_size_limit = 32;

	// --- Theme ---
	struct ThemeCache {
		int separation = 4;
	} theme_cache;

	// --- Runtime state ---
	double scroll_offset = 0.0;
	HashMap<int, Control *> active_nodes;
	LocalVector<Control *> pool;

	// Cached parent ScrollContainer (refreshed on enter/exit tree and on reparent).
	ScrollContainer *tracked_scroll_container = nullptr;

	// Re-entrancy guard for _sort_children (add_child during sort triggers queue_sort).
	bool in_sort_children = false;

	// --- Internal helpers (placeholders for now, implemented in later steps) ---
	void _update_tracked_scroll_container();
	void _disconnect_tracked_scroll_container();
	void _on_scroll_changed(double p_value);
	void _on_viewport_resized();
	void _compute_visible_range(int &r_first, int &r_last) const;
	real_t _item_stride() const;
	real_t _content_main_size() const;

	Control *_acquire_node();
	void _recycle_node(Control *p_node);
	void _release_all_nodes();

	void _resort();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// --- Properties ---
	void set_item_scene(const Ref<PackedScene> &p_scene);
	Ref<PackedScene> get_item_scene() const;

	void set_item_count(int p_count);
	int get_item_count() const;

	void set_item_min_size(const Vector2 &p_size);
	Vector2 get_item_min_size() const;

	void set_vertical(bool p_vertical);
	bool is_vertical() const;

	void set_buffer_items(int p_buffer);
	int get_buffer_items() const;

	void set_pool_size_limit(int p_limit);
	int get_pool_size_limit() const;

	// --- Data change notifications (implemented in Step 6) ---
	void refresh();
	void refresh_item(int p_index);

	// --- Queries ---
	int get_bound_index(Control *p_node) const;
	Control *get_node_at_index(int p_index) const;
	bool is_item_visible(int p_index) const;
	int get_first_visible_index() const;
	int get_last_visible_index() const;

	// --- Scrolling ---
	void scroll_to_index(int p_index, ScrollAlign p_align = ALIGN_VISIBLE);

	// --- Container overrides ---
	virtual Size2 get_minimum_size() const override;

	VirtualBoxContainer();
	~VirtualBoxContainer();
};

VARIANT_ENUM_CAST(VirtualBoxContainer::ScrollAlign);
