/**************************************************************************/
/*  virtual_box_container.cpp                                             */
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

#include "virtual_box_container.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/gui/scroll_bar.h"
#include "scene/gui/scroll_container.h"
#include "scene/theme/theme_db.h"

// =====================================================================
// Internal helpers (stubs; concrete implementations land in later steps).
// =====================================================================

void VirtualBoxContainer::_disconnect_tracked_scroll_container() {
	if (!tracked_scroll_container) {
		return;
	}
	// Disconnect from whichever scrollbar we previously subscribed to. We
	// defensively check both axes because a set_vertical() call between
	// subscribe and unsubscribe could have changed our preferred axis.
	ScrollBar *bars[2] = {
		tracked_scroll_container->get_v_scroll_bar(),
		tracked_scroll_container->get_h_scroll_bar(),
	};
	for (ScrollBar *bar : bars) {
		if (bar && bar->is_connected(SceneStringName(value_changed), callable_mp(this, &VirtualBoxContainer::_on_scroll_changed))) {
			bar->disconnect(SceneStringName(value_changed), callable_mp(this, &VirtualBoxContainer::_on_scroll_changed));
		}
	}
	if (tracked_scroll_container->is_connected(SceneStringName(resized), callable_mp(this, &VirtualBoxContainer::_on_viewport_resized))) {
		tracked_scroll_container->disconnect(SceneStringName(resized), callable_mp(this, &VirtualBoxContainer::_on_viewport_resized));
	}
	tracked_scroll_container = nullptr;
}

void VirtualBoxContainer::_update_tracked_scroll_container() {
	// Always disconnect from the previous ScrollContainer first so the
	// caller never accidentally leaves dangling connections when the
	// parent chain changes.
	_disconnect_tracked_scroll_container();

	Node *p = get_parent();
	while (p) {
		ScrollContainer *sc = Object::cast_to<ScrollContainer>(p);
		if (sc) {
			tracked_scroll_container = sc;
			break;
		}
		p = p->get_parent();
	}

	if (tracked_scroll_container) {
		// Subscribe to the scrollbar along our main axis: vertical list -> v,
		// horizontal list -> h. The other axis is cross-axis and doesn't
		// affect visible range calculation.
		ScrollBar *bar = vertical
				? (ScrollBar *)tracked_scroll_container->get_v_scroll_bar()
				: (ScrollBar *)tracked_scroll_container->get_h_scroll_bar();
		if (bar) {
			bar->connect(SceneStringName(value_changed), callable_mp(this, &VirtualBoxContainer::_on_scroll_changed));
			// Seed scroll_offset with current scrollbar value so the first
			// sort sees the correct viewport.
			scroll_offset = bar->get_value();
		}
		// Viewport extent changes when the ScrollContainer itself resizes;
		// re-queue sort so the visible range is recomputed.
		tracked_scroll_container->connect(SceneStringName(resized), callable_mp(this, &VirtualBoxContainer::_on_viewport_resized));
	} else {
		scroll_offset = 0.0;
	}
}

void VirtualBoxContainer::_on_scroll_changed(double p_value) {
	if (Math::is_equal_approx(scroll_offset, p_value)) {
		return;
	}
	scroll_offset = p_value;
	queue_sort();
}

void VirtualBoxContainer::_on_viewport_resized() {
	// The ScrollContainer we are nested in just resized, which changes the
	// viewport extent used by _compute_visible_range. Recompute on next tick.
	queue_sort();
}

real_t VirtualBoxContainer::_item_stride() const {
	// Main-axis dimension of each item, plus the separation between items.
	const real_t item_main = MAX((real_t)0, vertical ? item_min_size.y : item_min_size.x);
	return item_main + (real_t)theme_cache.separation;
}

real_t VirtualBoxContainer::_content_main_size() const {
	if (item_count <= 0) {
		return 0.0;
	}
	// N items consume (N * item_main) + (N - 1) * separation.
	const real_t item_main = MAX((real_t)0, vertical ? item_min_size.y : item_min_size.x);
	return (real_t)item_count * item_main + (real_t)(item_count - 1) * theme_cache.separation;
}

void VirtualBoxContainer::_compute_visible_range(int &r_first, int &r_last) const {
	if (item_count <= 0) {
		r_first = 0;
		r_last = -1;
		return;
	}

	const real_t stride = _item_stride();
	if (stride <= 0.0) {
		// Degenerate configuration (zero-sized items): fall back to showing
		// nothing rather than instantiating the whole data set.
		r_first = 0;
		r_last = -1;
		return;
	}

	// Viewport extent along the main axis: prefer the ScrollContainer we're
	// nested in. If there is none, fall back to our own rect, which
	// effectively disables virtualization (all items are considered visible)
	// but keeps output correct.
	real_t viewport_ext = 0.0;
	if (tracked_scroll_container) {
		const Size2 sc = tracked_scroll_container->get_size();
		viewport_ext = vertical ? sc.y : sc.x;
	} else {
		const Size2 self_sz = get_size();
		viewport_ext = vertical ? self_sz.y : self_sz.x;
	}
	if (viewport_ext <= 0.0) {
		r_first = 0;
		r_last = -1;
		return;
	}

	// scroll_offset is kept as double because it mirrors Range::get_value()
	// (Range API returns double). Convert to real_t at the boundary.
	const real_t offset_start = MAX((real_t)0, (real_t)scroll_offset);
	const real_t offset_end = offset_start + viewport_ext;

	int first = (int)Math::floor(offset_start / stride) - buffer_items;
	int last = (int)Math::ceil(offset_end / stride) - 1 + buffer_items;

	first = CLAMP(first, 0, item_count - 1);
	last = CLAMP(last, 0, item_count - 1);
	if (last < first) {
		// Can happen if the viewport is entirely above index 0 or below the
		// last item; represent as an empty range.
		r_first = 0;
		r_last = -1;
		return;
	}

	r_first = first;
	r_last = last;
}

Control *VirtualBoxContainer::_acquire_node() {
	// Pool hit: reuse an existing node, avoiding instantiation cost.
	if (!pool.is_empty()) {
		Control *n = pool[pool.size() - 1];
		pool.remove_at(pool.size() - 1);
		return n;
	}

	if (item_scene.is_null()) {
		// Nothing we can instantiate; _resort simply won't populate the viewport.
		return nullptr;
	}

	Node *instance = item_scene->instantiate();
	Control *as_control = Object::cast_to<Control>(instance);
	if (!as_control) {
		ERR_PRINT("VirtualBoxContainer: item_scene root must inherit Control.");
		if (instance) {
			memdelete(instance);
		}
		return nullptr;
	}
	return as_control;
}

void VirtualBoxContainer::_recycle_node(Control *p_node) {
	if (!p_node) {
		return;
	}
	// remove_child triggers Container::remove_child_notify which disconnects
	// the child's minimum_size_changed / size_flags_changed / visibility_changed
	// signals from this container's queue_sort. We want that: pooled nodes
	// should not keep calling back into us.
	if (p_node->get_parent() == this) {
		remove_child(p_node);
	}

	if ((int)pool.size() < pool_size_limit) {
		pool.push_back(p_node);
	} else {
		p_node->queue_free();
	}
}

void VirtualBoxContainer::_release_all_nodes() {
	// Unbind + free every active node and every pooled node. Used when the
	// template changes (set_item_scene) and when the container is destroyed.
	for (KeyValue<int, Control *> &E : active_nodes) {
		if (E.value) {
			emit_signal(SNAME("unbind_item"), E.value, E.key);
			if (E.value->get_parent() == this) {
				remove_child(E.value);
			}
			E.value->queue_free();
		}
	}
	active_nodes.clear();

	for (Control *c : pool) {
		if (c) {
			c->queue_free();
		}
	}
	pool.clear();
}

void VirtualBoxContainer::_resort() {
	if (!is_inside_tree()) {
		return;
	}

	int first = 0;
	int last = -1;
	_compute_visible_range(first, last);

	// ---- Phase 1: evict active nodes that fell out of the new range ------
	// Collect first, mutate the map afterwards to avoid iterator invalidation.
	LocalVector<int> to_evict;
	for (const KeyValue<int, Control *> &E : active_nodes) {
		if (E.key < first || E.key > last) {
			to_evict.push_back(E.key);
		}
	}
	for (int idx : to_evict) {
		Control *n = active_nodes[idx];
		if (n) {
			emit_signal(SNAME("unbind_item"), n, idx);
			_recycle_node(n);
		}
		active_nodes.erase(idx);
	}

	// ---- Phase 2: acquire + bind nodes that entered the range -----------
	if (item_scene.is_valid() && first <= last) {
		for (int idx = first; idx <= last; ++idx) {
			if (active_nodes.has(idx)) {
				continue;
			}
			Control *n = _acquire_node();
			if (!n) {
				// No template or instantiation failed; skip quietly. The
				// viewport will have gaps, which is the correct behavior for
				// a misconfigured container.
				continue;
			}
			// First time this node appears in the scene tree: parent it.
			// NOTE: add_child triggers Container::add_child_notify which calls
			// queue_sort, but the in_sort_children guard in _notification
			// prevents immediate re-entry. The deferred sort will run on the
			// next tick and see a stable state, so no busy loop.
			if (n->get_parent() != this) {
				add_child(n, false, INTERNAL_MODE_DISABLED);
			}
			active_nodes[idx] = n;
			emit_signal(SNAME("bind_item"), n, idx);
		}
	}

	// ---- Phase 3: position every active node ----------------------------
	const real_t stride = _item_stride();
	const Size2 self_size = get_size();
	// Cross-axis size fills whatever room the container has (driven by the
	// ScrollContainer for the cross axis, or by item_min_size on the cross
	// axis if the user set one). Main-axis size is the per-item span.
	const real_t main_item = MAX((real_t)0, vertical ? item_min_size.y : item_min_size.x);
	const real_t cross_size = vertical ? self_size.x : self_size.y;
	for (KeyValue<int, Control *> &E : active_nodes) {
		Control *n = E.value;
		if (!n) {
			continue;
		}
		const real_t main_pos = (real_t)E.key * stride;
		Rect2 rect;
		if (vertical) {
			rect = Rect2(Vector2(0, main_pos), Vector2(cross_size, main_item));
		} else {
			rect = Rect2(Vector2(main_pos, 0), Vector2(main_item, cross_size));
		}
		fit_child_in_rect(n, rect);
	}
}

// =====================================================================
// Property accessors.
// =====================================================================

void VirtualBoxContainer::set_item_scene(const Ref<PackedScene> &p_scene) {
	if (item_scene == p_scene) {
		return;
	}
	item_scene = p_scene;
	// Template change invalidates the entire pool and all active bindings.
	_release_all_nodes();
	queue_sort();
}

Ref<PackedScene> VirtualBoxContainer::get_item_scene() const {
	return item_scene;
}

void VirtualBoxContainer::set_item_count(int p_count) {
	const int new_count = MAX(0, p_count);
	if (new_count == item_count) {
		return;
	}
	item_count = new_count;
	update_minimum_size();
	queue_sort();
}

int VirtualBoxContainer::get_item_count() const {
	return item_count;
}

void VirtualBoxContainer::set_item_min_size(const Vector2 &p_size) {
	if (p_size == item_min_size) {
		return;
	}
	item_min_size = p_size;
	update_minimum_size();
	queue_sort();
}

Vector2 VirtualBoxContainer::get_item_min_size() const {
	return item_min_size;
}

void VirtualBoxContainer::set_vertical(bool p_vertical) {
	if (vertical == p_vertical) {
		return;
	}
	vertical = p_vertical;
	// Main axis changed: re-subscribe to the ScrollContainer's scrollbar on
	// the new axis. _update_tracked_scroll_container() internally
	// disconnects the previous binding first.
	if (is_inside_tree()) {
		_update_tracked_scroll_container();
	}
	update_minimum_size();
	queue_sort();
}

bool VirtualBoxContainer::is_vertical() const {
	return vertical;
}

void VirtualBoxContainer::set_buffer_items(int p_buffer) {
	const int v = MAX(0, p_buffer);
	if (v == buffer_items) {
		return;
	}
	buffer_items = v;
	queue_sort();
}

int VirtualBoxContainer::get_buffer_items() const {
	return buffer_items;
}

void VirtualBoxContainer::set_pool_size_limit(int p_limit) {
	pool_size_limit = MAX(0, p_limit);
	// Trim pool if the new limit is smaller. Stub for now (pool empty).
	while ((int)pool.size() > pool_size_limit) {
		Control *c = pool[pool.size() - 1];
		pool.remove_at(pool.size() - 1);
		if (c) {
			c->queue_free();
		}
	}
}

int VirtualBoxContainer::get_pool_size_limit() const {
	return pool_size_limit;
}

// =====================================================================
// Data change notifications (stubs).
// =====================================================================

void VirtualBoxContainer::refresh() {
	// Data changed but template is the same: re-emit bind_item for every
	// active node so user code sees the new values. Pool is left intact so
	// pooled nodes can still be reused with the same template.
	for (const KeyValue<int, Control *> &E : active_nodes) {
		if (E.value) {
			emit_signal(SNAME("bind_item"), E.value, E.key);
		}
	}
	// Range may still need to shift if item_count changed elsewhere; be safe.
	queue_sort();
}

void VirtualBoxContainer::refresh_item(int p_index) {
	const HashMap<int, Control *>::Iterator it = active_nodes.find(p_index);
	if (it != active_nodes.end() && it->value) {
		emit_signal(SNAME("bind_item"), it->value, p_index);
	}
	// If index isn't in the viewport, nothing to do: it will be bound with
	// the new data the next time it enters the visible range.
}

// =====================================================================
// Queries (mostly stubs for first cut).
// =====================================================================

int VirtualBoxContainer::get_bound_index(Control *p_node) const {
	for (const KeyValue<int, Control *> &E : active_nodes) {
		if (E.value == p_node) {
			return E.key;
		}
	}
	return -1;
}

Control *VirtualBoxContainer::get_node_at_index(int p_index) const {
	const HashMap<int, Control *>::ConstIterator it = active_nodes.find(p_index);
	return it != active_nodes.end() ? it->value : nullptr;
}

bool VirtualBoxContainer::is_item_visible(int p_index) const {
	return active_nodes.has(p_index);
}

int VirtualBoxContainer::get_first_visible_index() const {
	int first = INT32_MAX;
	for (const KeyValue<int, Control *> &E : active_nodes) {
		if (E.key < first) {
			first = E.key;
		}
	}
	return first == INT32_MAX ? -1 : first;
}

int VirtualBoxContainer::get_last_visible_index() const {
	int last = -1;
	for (const KeyValue<int, Control *> &E : active_nodes) {
		if (E.key > last) {
			last = E.key;
		}
	}
	return last;
}

// =====================================================================
// Scrolling.
// =====================================================================

void VirtualBoxContainer::scroll_to_index(int p_index, ScrollAlign p_align) {
	if (!tracked_scroll_container) {
		// No parent ScrollContainer to scroll -- silently ignore. This is
		// consistent with other ScrollContainer-dependent APIs in Godot that
		// are no-ops outside the expected hierarchy.
		return;
	}
	if (item_count <= 0) {
		return;
	}

	const int idx = CLAMP(p_index, 0, item_count - 1);
	const real_t stride = _item_stride();
	const real_t item_main = MAX((real_t)0, vertical ? item_min_size.y : item_min_size.x);
	const real_t item_start = (real_t)idx * stride;
	const real_t item_end = item_start + item_main;

	// Viewport extent along the main axis.
	const Size2 sc_size = tracked_scroll_container->get_size();
	const real_t viewport_ext = vertical ? sc_size.y : sc_size.x;

	real_t target = (real_t)scroll_offset;
	switch (p_align) {
		case ALIGN_START: {
			target = item_start;
		} break;
		case ALIGN_END: {
			target = item_end - viewport_ext;
		} break;
		case ALIGN_CENTER: {
			target = item_start + (item_main - viewport_ext) * 0.5;
		} break;
		case ALIGN_VISIBLE:
		default: {
			// If the item is already fully visible, do nothing.
			if (item_start < (real_t)scroll_offset) {
				target = item_start;
			} else if (item_end > (real_t)scroll_offset + viewport_ext) {
				target = item_end - viewport_ext;
			} else {
				// Already visible; no scroll change needed.
				return;
			}
		} break;
	}

	// ScrollContainer clamps the value to [0, max] internally, so we can
	// hand it any number including negative without pre-checking.
	const int target_int = (int)Math::round(target);
	if (vertical) {
		tracked_scroll_container->set_v_scroll(target_int);
	} else {
		tracked_scroll_container->set_h_scroll(target_int);
	}
}

// =====================================================================
// Container overrides.
// =====================================================================

Size2 VirtualBoxContainer::get_minimum_size() const {
	// IMPORTANT: deliberately ignore child minimum sizes. The container's
	// main-axis length is determined exclusively by item_count * stride so
	// that recycled child nodes flickering in/out of the viewport never
	// cause the outer ScrollContainer to recompute its scrollbar range.
	const real_t main = _content_main_size();
	const real_t cross = MAX((real_t)0, vertical ? item_min_size.x : item_min_size.y);
	return vertical ? Size2(cross, main) : Size2(main, cross);
}

void VirtualBoxContainer::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			_update_tracked_scroll_container();
		} break;

		case NOTIFICATION_EXIT_TREE: {
			// Only disconnect; don't walk parent chain (we're being removed).
			_disconnect_tracked_scroll_container();
		} break;

		case NOTIFICATION_PARENTED: {
			// Parent chain changed while still in tree: rebind if needed.
			if (is_inside_tree()) {
				_update_tracked_scroll_container();
				queue_sort();
			}
		} break;

		case NOTIFICATION_THEME_CHANGED: {
			// theme_cache.separation is populated automatically by the
			// ThemeDB-registered lambda (see _bind_methods / BIND_THEME_ITEM).
			// Stride depends on separation, so both the cached content size
			// and the layout need to be refreshed.
			update_minimum_size();
			queue_sort();
		} break;

		case NOTIFICATION_SORT_CHILDREN: {
			if (in_sort_children) {
				return;
			}
			in_sort_children = true;
			_resort();
			in_sort_children = false;
		} break;
	}
}

// =====================================================================
// Bindings.
// =====================================================================

void VirtualBoxContainer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_item_scene", "scene"), &VirtualBoxContainer::set_item_scene);
	ClassDB::bind_method(D_METHOD("get_item_scene"), &VirtualBoxContainer::get_item_scene);

	ClassDB::bind_method(D_METHOD("set_item_count", "count"), &VirtualBoxContainer::set_item_count);
	ClassDB::bind_method(D_METHOD("get_item_count"), &VirtualBoxContainer::get_item_count);

	ClassDB::bind_method(D_METHOD("set_item_min_size", "size"), &VirtualBoxContainer::set_item_min_size);
	ClassDB::bind_method(D_METHOD("get_item_min_size"), &VirtualBoxContainer::get_item_min_size);

	ClassDB::bind_method(D_METHOD("set_vertical", "vertical"), &VirtualBoxContainer::set_vertical);
	ClassDB::bind_method(D_METHOD("is_vertical"), &VirtualBoxContainer::is_vertical);

	ClassDB::bind_method(D_METHOD("set_buffer_items", "buffer"), &VirtualBoxContainer::set_buffer_items);
	ClassDB::bind_method(D_METHOD("get_buffer_items"), &VirtualBoxContainer::get_buffer_items);

	ClassDB::bind_method(D_METHOD("set_pool_size_limit", "limit"), &VirtualBoxContainer::set_pool_size_limit);
	ClassDB::bind_method(D_METHOD("get_pool_size_limit"), &VirtualBoxContainer::get_pool_size_limit);

	ClassDB::bind_method(D_METHOD("refresh"), &VirtualBoxContainer::refresh);
	ClassDB::bind_method(D_METHOD("refresh_item", "index"), &VirtualBoxContainer::refresh_item);

	ClassDB::bind_method(D_METHOD("get_bound_index", "node"), &VirtualBoxContainer::get_bound_index);
	ClassDB::bind_method(D_METHOD("get_node_at_index", "index"), &VirtualBoxContainer::get_node_at_index);
	ClassDB::bind_method(D_METHOD("is_item_visible", "index"), &VirtualBoxContainer::is_item_visible);
	ClassDB::bind_method(D_METHOD("get_first_visible_index"), &VirtualBoxContainer::get_first_visible_index);
	ClassDB::bind_method(D_METHOD("get_last_visible_index"), &VirtualBoxContainer::get_last_visible_index);

	ClassDB::bind_method(D_METHOD("scroll_to_index", "index", "align"), &VirtualBoxContainer::scroll_to_index, DEFVAL(ALIGN_VISIBLE));

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "item_scene", PROPERTY_HINT_RESOURCE_TYPE, "PackedScene"), "set_item_scene", "get_item_scene");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "item_count", PROPERTY_HINT_RANGE, "0,1000000,1"), "set_item_count", "get_item_count");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "vertical"), "set_vertical", "is_vertical");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "item_min_size", PROPERTY_HINT_NONE, "suffix:px"), "set_item_min_size", "get_item_min_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "buffer_items", PROPERTY_HINT_RANGE, "0,32,1"), "set_buffer_items", "get_buffer_items");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "pool_size_limit", PROPERTY_HINT_RANGE, "0,1024,1"), "set_pool_size_limit", "get_pool_size_limit");

	ADD_SIGNAL(MethodInfo("bind_item", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_RESOURCE_TYPE, "Control"), PropertyInfo(Variant::INT, "index")));
	ADD_SIGNAL(MethodInfo("unbind_item", PropertyInfo(Variant::OBJECT, "node", PROPERTY_HINT_RESOURCE_TYPE, "Control"), PropertyInfo(Variant::INT, "index")));

	BIND_ENUM_CONSTANT(ALIGN_VISIBLE);
	BIND_ENUM_CONSTANT(ALIGN_START);
	BIND_ENUM_CONSTANT(ALIGN_CENTER);
	BIND_ENUM_CONSTANT(ALIGN_END);

	BIND_THEME_ITEM(Theme::DATA_TYPE_CONSTANT, VirtualBoxContainer, separation);
}

VirtualBoxContainer::VirtualBoxContainer() {
}

VirtualBoxContainer::~VirtualBoxContainer() {
	// Free pool entries that are not currently in the scene tree.
	for (Control *c : pool) {
		if (c && !c->is_inside_tree()) {
			memdelete(c);
		}
	}
	pool.clear();
}
