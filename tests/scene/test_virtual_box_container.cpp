/**************************************************************************/
/*  test_virtual_box_container.cpp                                        */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_virtual_box_container)

#ifndef ADVANCED_GUI_DISABLED

#include "scene/gui/label.h"
#include "scene/gui/scroll_container.h"
#include "scene/gui/virtual_box_container.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/packed_scene.h"

namespace TestVirtualBoxContainer {

// Helper: build a trivial PackedScene whose root is a Control with a Label
// child. Matches the "template scene" pattern end-users are expected to feed
// into `item_scene`.
static Ref<PackedScene> make_item_scene(const Size2 &p_min_size = Size2(100, 48)) {
	Control *root = memnew(Control);
	root->set_name("Item");
	root->set_custom_minimum_size(p_min_size);

	Label *label = memnew(Label);
	label->set_name("Label");
	root->add_child(label);
	label->set_owner(root);

	Ref<PackedScene> ps;
	ps.instantiate();
	const Error err = ps->pack(root);
	CHECK_MESSAGE(err == OK, "Failed to pack item template scene.");

	memdelete(root);
	return ps;
}

// Helper: count direct (non-internal) children, which is what the active
// binding set maps to.
static int count_visible_children(Node *p_parent) {
	int n = 0;
	for (int i = 0; i < p_parent->get_child_count(false); i++) {
		if (Object::cast_to<Control>(p_parent->get_child(i, false))) {
			n++;
		}
	}
	return n;
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Default property values") {
	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	SceneTree::get_singleton()->get_root()->add_child(vbc);

	CHECK(vbc->get_item_count() == 0);
	CHECK(vbc->is_vertical() == true);
	CHECK(vbc->get_item_min_size() == Vector2(0, 48));
	CHECK(vbc->get_buffer_items() == 2);
	CHECK(vbc->get_pool_size_limit() == 32);
	CHECK(vbc->get_item_scene().is_null());
	CHECK(vbc->get_first_visible_index() == -1);
	CHECK(vbc->get_last_visible_index() == -1);

	memdelete(vbc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Property set/get round-trips") {
	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	SceneTree::get_singleton()->get_root()->add_child(vbc);

	Ref<PackedScene> ps = make_item_scene();
	vbc->set_item_scene(ps);
	CHECK(vbc->get_item_scene() == ps);

	vbc->set_item_count(1234);
	CHECK(vbc->get_item_count() == 1234);

	// Negative counts clamp to 0.
	vbc->set_item_count(-5);
	CHECK(vbc->get_item_count() == 0);

	vbc->set_item_min_size(Vector2(80, 32));
	CHECK(vbc->get_item_min_size() == Vector2(80, 32));

	vbc->set_vertical(false);
	CHECK(vbc->is_vertical() == false);

	vbc->set_buffer_items(5);
	CHECK(vbc->get_buffer_items() == 5);
	vbc->set_buffer_items(-3);
	CHECK(vbc->get_buffer_items() == 0);

	vbc->set_pool_size_limit(64);
	CHECK(vbc->get_pool_size_limit() == 64);
	vbc->set_pool_size_limit(-1);
	CHECK(vbc->get_pool_size_limit() == 0);

	memdelete(vbc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Minimum size depends on item_count") {
	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	SceneTree::get_singleton()->get_root()->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	// Empty list has no main-axis length.
	vbc->set_item_min_size(Vector2(100, 48));
	vbc->set_item_count(0);
	MessageQueue::get_singleton()->flush();
	CHECK(vbc->get_minimum_size().y == 0);

	// Non-empty: at least N * item_main (separation is theme-dependent so we
	// only check the lower bound, which is N * item_main with 0 separation).
	vbc->set_item_count(100);
	MessageQueue::get_singleton()->flush();
	CHECK(vbc->get_minimum_size().y >= 100 * 48);
	// Cross axis follows item_min_size.x when vertical.
	CHECK(vbc->get_minimum_size().x == 100);

	// Horizontal layout swaps the axes.
	vbc->set_vertical(false);
	MessageQueue::get_singleton()->flush();
	CHECK(vbc->get_minimum_size().x >= 100 * 100);
	CHECK(vbc->get_minimum_size().y == 48);

	memdelete(vbc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Virtualization with ScrollContainer parent") {
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_size(Size2(200, 240));
	SceneTree::get_singleton()->get_root()->add_child(sc);

	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	vbc->set_item_scene(make_item_scene());
	vbc->set_item_min_size(Vector2(0, 48));
	vbc->set_buffer_items(0); // Disable buffer for deterministic bounds.
	vbc->set_item_count(1000);
	sc->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	const int first = vbc->get_first_visible_index();
	const int last = vbc->get_last_visible_index();

	SUBCASE("[VirtualBoxContainer] Visible range is a small slice, not the full data set") {
		CHECK(first == 0); // Start of viewport maps to first item.
		CHECK(last >= 0);
		CHECK(last < vbc->get_item_count());
		// 240 / 48 = 5 items in a 240px viewport. Allow a small margin for
		// stride (separation) and rounding.
		CHECK(last - first + 1 <= 10);
	}

	SUBCASE("[VirtualBoxContainer] Every visible index has a bound node, others don't") {
		for (int i = first; i <= last; i++) {
			CHECK(vbc->is_item_visible(i));
			CHECK(vbc->get_node_at_index(i) != nullptr);
		}
		CHECK_FALSE(vbc->is_item_visible(last + 1));
		CHECK(vbc->get_node_at_index(last + 1) == nullptr);
	}

	SUBCASE("[VirtualBoxContainer] Child count matches active binding count") {
		CHECK(count_visible_children(vbc) == last - first + 1);
	}

	SUBCASE("[VirtualBoxContainer] get_bound_index round-trips") {
		Control *node = vbc->get_node_at_index(first);
		REQUIRE(node != nullptr);
		CHECK(vbc->get_bound_index(node) == first);
	}

	memdelete(sc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Scrolling changes the visible range") {
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_size(Size2(200, 240));
	SceneTree::get_singleton()->get_root()->add_child(sc);

	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	vbc->set_item_scene(make_item_scene());
	vbc->set_item_min_size(Vector2(0, 48));
	vbc->set_buffer_items(0);
	vbc->set_item_count(1000);
	sc->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	const int first_before = vbc->get_first_visible_index();

	// Scroll far enough that the viewport is guaranteed to be past index 0.
	// 200 pixels / stride >= 200 / (48 + ~4) = ~3 items at least.
	sc->set_v_scroll(2000);
	MessageQueue::get_singleton()->flush();

	const int first_after = vbc->get_first_visible_index();
	const int last_after = vbc->get_last_visible_index();

	CHECK(first_after > first_before);
	CHECK(last_after >= first_after);
	CHECK(last_after < vbc->get_item_count());

	// Scrolling to the very end must not go past the last item.
	sc->set_v_scroll(1000000);
	MessageQueue::get_singleton()->flush();
	CHECK(vbc->get_last_visible_index() == vbc->get_item_count() - 1);

	memdelete(sc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Node pool bounds active child count") {
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_size(Size2(200, 240));
	SceneTree::get_singleton()->get_root()->add_child(sc);

	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	vbc->set_item_scene(make_item_scene());
	vbc->set_item_min_size(Vector2(0, 48));
	vbc->set_buffer_items(0);
	vbc->set_item_count(10000);
	sc->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	const int initial_children = count_visible_children(vbc);

	// Scroll through many different positions. Each time the visible window
	// shifts, nodes that left the viewport should be recycled rather than
	// accumulated. The invariant we care about: active-child-count stays of
	// the same order as the initial (viewport-sized) count.
	for (int step = 1; step <= 20; step++) {
		sc->set_v_scroll(step * 500);
		MessageQueue::get_singleton()->flush();
		const int n = count_visible_children(vbc);
		CHECK(n <= initial_children + 4); // Small slack for rounding.
	}

	memdelete(sc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Toggling vertical axis re-binds axis") {
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_size(Size2(240, 240));
	SceneTree::get_singleton()->get_root()->add_child(sc);

	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	vbc->set_item_scene(make_item_scene());
	vbc->set_buffer_items(0);

	// Start in vertical mode: main axis = Y, item_min_size.y must be non-zero.
	vbc->set_item_min_size(Vector2(0, 48));
	vbc->set_item_count(1000);
	sc->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	CHECK(vbc->get_first_visible_index() == 0);
	CHECK(vbc->get_last_visible_index() >= 0);
	const int v_last = vbc->get_last_visible_index();

	// Flip to horizontal. Also adjust item_min_size so the main (x) axis has
	// a non-zero stride -- otherwise the degenerate-stride guard kicks in and
	// the visible range collapses, which is correct behavior but not what
	// this subcase is checking.
	vbc->set_vertical(false);
	vbc->set_item_min_size(Vector2(120, 0));
	MessageQueue::get_singleton()->flush();

	CHECK(vbc->get_first_visible_index() == 0);
	CHECK(vbc->get_last_visible_index() >= 0);
	const int h_last = vbc->get_last_visible_index();

	// With a roughly square viewport and different per-item sizes (48 vs 120),
	// the visible counts are free to differ -- the important invariant is that
	// both orientations produced a finite visible window.
	CHECK(v_last >= 0);
	CHECK(h_last >= 0);

	memdelete(sc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] scroll_to_index makes the target visible") {
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_size(Size2(200, 240));
	SceneTree::get_singleton()->get_root()->add_child(sc);

	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	vbc->set_item_scene(make_item_scene());
	vbc->set_item_min_size(Vector2(0, 48));
	vbc->set_buffer_items(0);
	vbc->set_item_count(1000);
	sc->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	SUBCASE("[VirtualBoxContainer] ALIGN_START places index at viewport top") {
		vbc->scroll_to_index(500, VirtualBoxContainer::ALIGN_START);
		MessageQueue::get_singleton()->flush();

		CHECK(vbc->is_item_visible(500));
		CHECK(vbc->get_first_visible_index() == 500);
	}

	SUBCASE("[VirtualBoxContainer] ALIGN_CENTER keeps target visible") {
		vbc->scroll_to_index(500, VirtualBoxContainer::ALIGN_CENTER);
		MessageQueue::get_singleton()->flush();

		CHECK(vbc->is_item_visible(500));
		CHECK(vbc->get_first_visible_index() < 500);
		CHECK(vbc->get_last_visible_index() >= 500);
	}

	SUBCASE("[VirtualBoxContainer] ALIGN_END places index at viewport bottom") {
		vbc->scroll_to_index(500, VirtualBoxContainer::ALIGN_END);
		MessageQueue::get_singleton()->flush();

		CHECK(vbc->is_item_visible(500));
		CHECK(vbc->get_last_visible_index() == 500);
	}

	SUBCASE("[VirtualBoxContainer] ALIGN_VISIBLE is a no-op when index already in view") {
		// Index 1 is already visible from the top; scroll should not change.
		const int scroll_before = sc->get_v_scroll();
		vbc->scroll_to_index(1, VirtualBoxContainer::ALIGN_VISIBLE);
		MessageQueue::get_singleton()->flush();
		CHECK(sc->get_v_scroll() == scroll_before);
	}

	SUBCASE("[VirtualBoxContainer] Out-of-range indices clamp instead of crashing") {
		vbc->scroll_to_index(-10, VirtualBoxContainer::ALIGN_START);
		MessageQueue::get_singleton()->flush();
		CHECK(vbc->is_item_visible(0));

		vbc->scroll_to_index(999999, VirtualBoxContainer::ALIGN_END);
		MessageQueue::get_singleton()->flush();
		CHECK(vbc->is_item_visible(vbc->get_item_count() - 1));
	}

	memdelete(sc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Replacing item_scene releases all bindings") {
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_size(Size2(200, 240));
	SceneTree::get_singleton()->get_root()->add_child(sc);

	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	vbc->set_item_scene(make_item_scene());
	vbc->set_item_min_size(Vector2(0, 48));
	vbc->set_buffer_items(0);
	vbc->set_item_count(50);
	sc->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	CHECK(count_visible_children(vbc) > 0);
	const Control *first_node = vbc->get_node_at_index(0);
	REQUIRE(first_node != nullptr);

	// Swap in a fresh template. The existing pool + active bindings should be
	// torn down (the previous template produced Control+Label subtrees that
	// are now invalid) and a new set instantiated on the next sort.
	vbc->set_item_scene(make_item_scene(Size2(100, 64)));
	MessageQueue::get_singleton()->flush();

	// New bindings should exist...
	CHECK(count_visible_children(vbc) > 0);
	// ...and the previously cached node pointer must not be reused (it was
	// queue_free'd). We only check via the map: index 0 now maps to a fresh
	// node. Dereferencing `first_node` would be use-after-free.
	CHECK(vbc->get_node_at_index(0) != first_node);

	memdelete(sc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Works without a ScrollContainer ancestor") {
	// Without a ScrollContainer, virtualization falls back to the container's
	// own rect. Goal here is correctness (no crash, visible indices consistent),
	// not performance.
	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	vbc->set_item_scene(make_item_scene());
	vbc->set_item_min_size(Vector2(0, 48));
	vbc->set_buffer_items(0);
	vbc->set_size(Size2(200, 240));
	vbc->set_item_count(100);
	SceneTree::get_singleton()->get_root()->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	const int first = vbc->get_first_visible_index();
	const int last = vbc->get_last_visible_index();

	// Viewport extent = 240, so visible range is bounded regardless of
	// item_count.
	CHECK(first == 0);
	CHECK(last >= 0);
	CHECK(last < vbc->get_item_count());
	CHECK(count_visible_children(vbc) == last - first + 1);

	// scroll_to_index is a no-op without a ScrollContainer: must not crash.
	vbc->scroll_to_index(50, VirtualBoxContainer::ALIGN_CENTER);
	MessageQueue::get_singleton()->flush();

	memdelete(vbc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Zero-sized items do not explode") {
	// Degenerate but reachable state: item_min_size is (0, 0). Stride
	// collapses to (separation). Spec: we bail out cleanly and produce an
	// empty visible range instead of trying to instantiate item_count nodes.
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_size(Size2(200, 240));
	SceneTree::get_singleton()->get_root()->add_child(sc);

	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	vbc->set_item_scene(make_item_scene());
	vbc->set_item_min_size(Vector2(0, 0));
	vbc->set_item_count(1000);
	sc->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	// Degenerate case: main-axis size is 0, so stride may still be > 0 due to
	// separation. Whatever the case, we must not have instantiated 1000 nodes.
	CHECK(count_visible_children(vbc) < 100);

	memdelete(sc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] Empty list: no children bound") {
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_size(Size2(200, 240));
	SceneTree::get_singleton()->get_root()->add_child(sc);

	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	vbc->set_item_scene(make_item_scene());
	vbc->set_item_min_size(Vector2(0, 48));
	vbc->set_item_count(0);
	sc->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	CHECK(count_visible_children(vbc) == 0);
	CHECK(vbc->get_first_visible_index() == -1);
	CHECK(vbc->get_last_visible_index() == -1);
	CHECK_FALSE(vbc->is_item_visible(0));

	memdelete(sc);
}

TEST_CASE("[SceneTree][VirtualBoxContainer] item_count shrink evicts out-of-range bindings") {
	ScrollContainer *sc = memnew(ScrollContainer);
	sc->set_size(Size2(200, 240));
	SceneTree::get_singleton()->get_root()->add_child(sc);

	VirtualBoxContainer *vbc = memnew(VirtualBoxContainer);
	vbc->set_item_scene(make_item_scene());
	vbc->set_item_min_size(Vector2(0, 48));
	vbc->set_buffer_items(0);
	vbc->set_item_count(1000);
	sc->add_child(vbc);
	MessageQueue::get_singleton()->flush();

	// Scroll to the middle, so indices around 500 are bound.
	vbc->scroll_to_index(500, VirtualBoxContainer::ALIGN_START);
	MessageQueue::get_singleton()->flush();
	CHECK(vbc->is_item_visible(500));

	// Shrink the data set below the current visible window. Every previously
	// bound index is now out of range and must be evicted.
	vbc->set_item_count(10);
	MessageQueue::get_singleton()->flush();

	CHECK_FALSE(vbc->is_item_visible(500));
	CHECK(vbc->get_last_visible_index() <= 9);
	// ScrollContainer max value dropped, so v_scroll clamps too; we expect a
	// non-empty view of the surviving 10 items.
	sc->set_v_scroll(0);
	MessageQueue::get_singleton()->flush();
	CHECK(vbc->is_item_visible(0));

	memdelete(sc);
}

} // namespace TestVirtualBoxContainer

#endif // ADVANCED_GUI_DISABLED
