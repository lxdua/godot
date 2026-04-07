
/**************************************************************************/
/*  enhanced_input_manager.h                                              */
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

#include "core/object/object.h"
#include "core/object/class_db.h"
#include "core/variant/typed_array.h"
#include "core/templates/hash_map.h"
#include "core/input/enhanced_input_action_value.h"
#include "core/input/enhanced_input_action.h"
#include "core/input/enhanced_input_modifier.h"
#include "core/input/enhanced_input_trigger.h"
#include "core/input/enhanced_input_mapping_context.h"

class InputEvent;

class EnhancedInputManager : public Object {
	GDCLASS(EnhancedInputManager, Object);

private:
	static EnhancedInputManager *singleton;

	// === Context stack ===

	struct ContextEntry {
		Ref<InputMappingContext> context;
		int priority = 0;
	};

	Vector<ContextEntry> context_stack; // Sorted by priority descending.

	void _sort_context_stack();

	// === Per-mapping runtime state ===

	struct MappingState {
		Ref<InputActionMapping> mapping;
		Vector3 raw_value;         // Current raw value from input event.
		bool value_active = false; // Whether this mapping is currently receiving input.

		// Per-trigger state for this mapping.
		Vector<TriggerState> trigger_states;
		Vector<float> trigger_elapsed;
	};

	// === Per-action runtime state ===

	struct ActionState {
		Ref<InputAction> action;
		Vector3 accumulated_value; // After modifier + accumulation.
		TriggerState trigger_state = TRIGGER_NONE;
		TriggerState last_trigger_state = TRIGGER_NONE;
		float elapsed_time = 0.0f;

		// All mapping states that feed into this action.
		Vector<MappingState *> mapping_states;
	};

	// All mapping states (flat list, rebuilt when contexts change).
	Vector<MappingState> all_mapping_states;

	// Action name → ActionState.
	HashMap<StringName, ActionState> action_states;

	// Rebuild internal structures when contexts are added/removed.
	void _rebuild_mapping_states();

	// Extract raw value from an InputEvent.
	Vector3 _extract_value_from_event(const Ref<InputEvent> &p_event) const;

	// Check if a mapping's input_event matches the incoming event.
	bool _event_matches_mapping(const Ref<InputEvent> &p_incoming, const Ref<InputEvent> &p_mapping_event) const;

	// Run modifier chain on a value.
	Vector3 _run_modifiers(MappingState &p_state, double p_delta);

	// Per-action binding storage.
	struct ActionBinding {
		StringName action_name;
		TriggerEvent event_type;
		Callable callable;
	};
	Vector<ActionBinding> action_bindings;

	void _emit_action_event(const StringName &p_action_name, TriggerEvent p_event_type, const Ref<InputAction> &p_action, const Ref<InputActionValue> &p_value);

protected:
	static void _bind_methods();

public:
	static EnhancedInputManager *get_singleton();

	EnhancedInputManager();
	~EnhancedInputManager();

	// === Context management ===

	void push_mapping_context(const Ref<InputMappingContext> &p_context, int p_priority = 0);
	void pop_mapping_context(const Ref<InputMappingContext> &p_context);
	void clear_all_contexts();
	bool has_mapping_context(const Ref<InputMappingContext> &p_context) const;
	TypedArray<InputMappingContext> get_active_contexts() const;

	// === Input processing (called by Input system) ===

	void process_input_event(const Ref<InputEvent> &p_event);
	void tick(double p_delta);

	// === Query API ===

	Ref<InputActionValue> get_action_value(const Ref<InputAction> &p_action) const;
	bool is_action_triggered(const Ref<InputAction> &p_action) const;
	int get_action_trigger_state(const Ref<InputAction> &p_action) const;
	float get_action_elapsed_time(const Ref<InputAction> &p_action) const;

	// === Per-action binding ===

	void bind_action(const Ref<InputAction> &p_action, TriggerEvent p_event_type, const Callable &p_callable);
	void unbind_action(const Ref<InputAction> &p_action, TriggerEvent p_event_type, const Callable &p_callable);
};
