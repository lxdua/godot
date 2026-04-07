
/**************************************************************************/
/*  enhanced_input_mapping_context.h                                      */
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

#include "core/io/resource.h"
#include "core/object/class_db.h"
#include "core/variant/typed_array.h"
#include "core/input/enhanced_input_action.h"
#include "core/input/enhanced_input_modifier.h"
#include "core/input/enhanced_input_trigger.h"

class InputEvent;

// ============================================================================
// InputActionMapping — A single binding: InputEvent → Action + Modifiers + Triggers
// ============================================================================

class InputActionMapping : public Resource {
	GDCLASS(InputActionMapping, Resource);

private:
	Ref<InputAction> action;
	Ref<InputEvent> input_event;
	TypedArray<InputModifier> modifiers;
	TypedArray<InputTrigger> triggers;
	bool consume_input = true;

protected:
	static void _bind_methods();

public:
	void set_action(const Ref<InputAction> &p_action);
	Ref<InputAction> get_action() const;

	void set_input_event(const Ref<InputEvent> &p_event);
	Ref<InputEvent> get_input_event() const;

	void set_modifiers(const TypedArray<InputModifier> &p_modifiers);
	TypedArray<InputModifier> get_modifiers() const;
	void add_modifier(const Ref<InputModifier> &p_modifier);

	void set_triggers(const TypedArray<InputTrigger> &p_triggers);
	TypedArray<InputTrigger> get_triggers() const;
	void add_trigger(const Ref<InputTrigger> &p_trigger);

	void set_consume_input(bool p_consume);
	bool get_consume_input() const;
};

// ============================================================================
// InputMappingContext — A collection of mappings that can be pushed/popped
// ============================================================================

class InputMappingContext : public Resource {
	GDCLASS(InputMappingContext, Resource);

private:
	StringName context_name;
	TypedArray<InputActionMapping> mappings;

protected:
	static void _bind_methods();

public:
	void set_context_name(const StringName &p_name);
	StringName get_context_name() const;

	void set_mappings(const TypedArray<InputActionMapping> &p_mappings);
	TypedArray<InputActionMapping> get_mappings() const;

	void add_mapping(const Ref<InputActionMapping> &p_mapping);
	void remove_mapping(int p_index);

	// Convenience: create a mapping, add it, and return it for chaining.
	Ref<InputActionMapping> map_action(const Ref<InputAction> &p_action, const Ref<InputEvent> &p_event);
};
