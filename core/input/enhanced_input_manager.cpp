
/**************************************************************************/
/*  enhanced_input_manager.cpp                                            */
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

#include "enhanced_input_manager.h"

#include "core/input/input_event.h"

EnhancedInputManager *EnhancedInputManager::singleton = nullptr;

// ============================================================================
// Bind methods
// ============================================================================

void EnhancedInputManager::_bind_methods() {
	// Context management.
	ClassDB::bind_method(D_METHOD("push_mapping_context", "context", "priority"), &EnhancedInputManager::push_mapping_context, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("pop_mapping_context", "context"), &EnhancedInputManager::pop_mapping_context);
	ClassDB::bind_method(D_METHOD("clear_all_contexts"), &EnhancedInputManager::clear_all_contexts);
	ClassDB::bind_method(D_METHOD("has_mapping_context", "context"), &EnhancedInputManager::has_mapping_context);
	ClassDB::bind_method(D_METHOD("get_active_contexts"), &EnhancedInputManager::get_active_contexts);

	// Query API.
	ClassDB::bind_method(D_METHOD("get_action_value", "action"), &EnhancedInputManager::get_action_value);
	ClassDB::bind_method(D_METHOD("is_action_triggered", "action"), &EnhancedInputManager::is_action_triggered);
	ClassDB::bind_method(D_METHOD("get_action_trigger_state", "action"), &EnhancedInputManager::get_action_trigger_state);
	ClassDB::bind_method(D_METHOD("get_action_elapsed_time", "action"), &EnhancedInputManager::get_action_elapsed_time);

	// Per-action binding.
	ClassDB::bind_method(D_METHOD("bind_action", "action", "event_type", "callable"), &EnhancedInputManager::bind_action);
	ClassDB::bind_method(D_METHOD("unbind_action", "action", "event_type", "callable"), &EnhancedInputManager::unbind_action);

	// Signals.
	ADD_SIGNAL(MethodInfo("action_triggered",
			PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "InputAction"),
			PropertyInfo(Variant::OBJECT, "value", PROPERTY_HINT_RESOURCE_TYPE, "InputActionValue"),
			PropertyInfo(Variant::INT, "state")));
	ADD_SIGNAL(MethodInfo("action_started",
			PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "InputAction"),
			PropertyInfo(Variant::OBJECT, "value", PROPERTY_HINT_RESOURCE_TYPE, "InputActionValue")));
	ADD_SIGNAL(MethodInfo("action_ongoing",
			PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "InputAction"),
			PropertyInfo(Variant::OBJECT, "value", PROPERTY_HINT_RESOURCE_TYPE, "InputActionValue")));
	ADD_SIGNAL(MethodInfo("action_completed",
			PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "InputAction"),
			PropertyInfo(Variant::OBJECT, "value", PROPERTY_HINT_RESOURCE_TYPE, "InputActionValue")));
	ADD_SIGNAL(MethodInfo("action_canceled",
			PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "InputAction"),
			PropertyInfo(Variant::OBJECT, "value", PROPERTY_HINT_RESOURCE_TYPE, "InputActionValue")));
	ADD_SIGNAL(MethodInfo("context_pushed",
			PropertyInfo(Variant::OBJECT, "context", PROPERTY_HINT_RESOURCE_TYPE, "InputMappingContext")));
	ADD_SIGNAL(MethodInfo("context_popped",
			PropertyInfo(Variant::OBJECT, "context", PROPERTY_HINT_RESOURCE_TYPE, "InputMappingContext")));
}

// ============================================================================
// Constructor / Destructor / Singleton
// ============================================================================

EnhancedInputManager::EnhancedInputManager() {
	singleton = this;
}

EnhancedInputManager::~EnhancedInputManager() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

EnhancedInputManager *EnhancedInputManager::get_singleton() {
	return singleton;
}

// ============================================================================
// Context management
// ============================================================================

void EnhancedInputManager::_sort_context_stack() {
	// Simple insertion sort by priority descending (context stacks are small).
	for (int i = 1; i < context_stack.size(); i++) {
		ContextEntry key = context_stack[i];
		int j = i - 1;
		while (j >= 0 && context_stack[j].priority < key.priority) {
			context_stack.write[j + 1] = context_stack[j];
			j--;
		}
		context_stack.write[j + 1] = key;
	}
}

void EnhancedInputManager::push_mapping_context(const Ref<InputMappingContext> &p_context, int p_priority) {
	ERR_FAIL_COND(p_context.is_null());

	// Don't add duplicates.
	for (int i = 0; i < context_stack.size(); i++) {
		if (context_stack[i].context == p_context) {
			context_stack.write[i].priority = p_priority;
			_sort_context_stack();
			_rebuild_mapping_states();
			return;
		}
	}

	ContextEntry entry;
	entry.context = p_context;
	entry.priority = p_priority;
	context_stack.push_back(entry);
	_sort_context_stack();
	_rebuild_mapping_states();
	emit_signal("context_pushed", p_context);
}

void EnhancedInputManager::pop_mapping_context(const Ref<InputMappingContext> &p_context) {
	ERR_FAIL_COND(p_context.is_null());

	for (int i = 0; i < context_stack.size(); i++) {
		if (context_stack[i].context == p_context) {
			context_stack.remove_at(i);
			_rebuild_mapping_states();
			emit_signal("context_popped", p_context);
			return;
		}
	}
}

void EnhancedInputManager::clear_all_contexts() {
	context_stack.clear();
	_rebuild_mapping_states();
}

bool EnhancedInputManager::has_mapping_context(const Ref<InputMappingContext> &p_context) const {
	for (int i = 0; i < context_stack.size(); i++) {
		if (context_stack[i].context == p_context) {
			return true;
		}
	}
	return false;
}

TypedArray<InputMappingContext> EnhancedInputManager::get_active_contexts() const {
	TypedArray<InputMappingContext> result;
	for (int i = 0; i < context_stack.size(); i++) {
		result.push_back(context_stack[i].context);
	}
	return result;
}

// ============================================================================
// Internal: rebuild mapping states
// ============================================================================

void EnhancedInputManager::_rebuild_mapping_states() {
	all_mapping_states.clear();
	action_states.clear();

	// Walk contexts in priority order (already sorted descending).
	for (int ci = 0; ci < context_stack.size(); ci++) {
		Ref<InputMappingContext> ctx = context_stack[ci].context;
		TypedArray<InputActionMapping> ctx_mappings = ctx->get_mappings();

		for (int mi = 0; mi < ctx_mappings.size(); mi++) {
			Ref<InputActionMapping> mapping = ctx_mappings[mi];
			if (mapping.is_null() || mapping->get_action().is_null()) {
				continue;
			}

			MappingState ms;
			ms.mapping = mapping;
			ms.raw_value = Vector3();
			ms.value_active = false;

			// Initialize per-trigger state.
			TypedArray<InputTrigger> triggers = mapping->get_triggers();
			int trigger_count = triggers.size();
			if (trigger_count == 0) {
				trigger_count = 1; // Default trigger (Down).
			}
			ms.trigger_states.resize(trigger_count);
			ms.trigger_elapsed.resize(trigger_count);
			for (int ti = 0; ti < trigger_count; ti++) {
				ms.trigger_states.write[ti] = TRIGGER_NONE;
				ms.trigger_elapsed.write[ti] = 0.0f;
			}

			all_mapping_states.push_back(ms);
		}
	}

	// Build action states and link mapping states.
	for (int i = 0; i < all_mapping_states.size(); i++) {
		MappingState &ms = all_mapping_states.write[i];
		StringName action_name = ms.mapping->get_action()->get_action_name();

		if (!action_states.has(action_name)) {
			ActionState as;
			as.action = ms.mapping->get_action();
			as.accumulated_value = Vector3();
			as.trigger_state = TRIGGER_NONE;
			as.last_trigger_state = TRIGGER_NONE;
			as.elapsed_time = 0.0f;
			action_states.insert(action_name, as);
		}

		action_states[action_name].mapping_states.push_back(&ms);
	}
}

// ============================================================================
// Internal: event matching
// ============================================================================

bool EnhancedInputManager::_event_matches_mapping(const Ref<InputEvent> &p_incoming, const Ref<InputEvent> &p_mapping_event) const {
	if (p_incoming.is_null() || p_mapping_event.is_null()) {
		return false;
	}

	// InputEventKey matching.
	Ref<InputEventKey> incoming_key = p_incoming;
	Ref<InputEventKey> mapping_key = p_mapping_event;
	if (incoming_key.is_valid() && mapping_key.is_valid()) {
		return incoming_key->get_keycode() == mapping_key->get_keycode() ||
				(incoming_key->get_physical_keycode() != Key::NONE &&
						incoming_key->get_physical_keycode() == mapping_key->get_physical_keycode());
	}

	// InputEventMouseButton matching.
	Ref<InputEventMouseButton> incoming_mb = p_incoming;
	Ref<InputEventMouseButton> mapping_mb = p_mapping_event;
	if (incoming_mb.is_valid() && mapping_mb.is_valid()) {
		return incoming_mb->get_button_index() == mapping_mb->get_button_index();
	}

	// InputEventJoypadButton matching.
	Ref<InputEventJoypadButton> incoming_jb = p_incoming;
	Ref<InputEventJoypadButton> mapping_jb = p_mapping_event;
	if (incoming_jb.is_valid() && mapping_jb.is_valid()) {
		return incoming_jb->get_button_index() == mapping_jb->get_button_index();
	}

	// InputEventJoypadMotion matching.
	Ref<InputEventJoypadMotion> incoming_jm = p_incoming;
	Ref<InputEventJoypadMotion> mapping_jm = p_mapping_event;
	if (incoming_jm.is_valid() && mapping_jm.is_valid()) {
		return incoming_jm->get_axis() == mapping_jm->get_axis();
	}

	// InputEventMouseMotion — any mouse motion matches any mouse motion mapping.
	Ref<InputEventMouseMotion> incoming_mm = p_incoming;
	Ref<InputEventMouseMotion> mapping_mm = p_mapping_event;
	if (incoming_mm.is_valid() && mapping_mm.is_valid()) {
		return true;
	}

	return false;
}

// ============================================================================
// Internal: extract value from event
// ============================================================================

Vector3 EnhancedInputManager::_extract_value_from_event(const Ref<InputEvent> &p_event) const {
	// Key press → 1.0 or 0.0
	Ref<InputEventKey> key_event = p_event;
	if (key_event.is_valid()) {
		return key_event->is_pressed() ? Vector3(1.0f, 0.0f, 0.0f) : Vector3();
	}

	// Mouse button → 1.0 or 0.0
	Ref<InputEventMouseButton> mb_event = p_event;
	if (mb_event.is_valid()) {
		return mb_event->is_pressed() ? Vector3(1.0f, 0.0f, 0.0f) : Vector3();
	}

	// Joypad button → 1.0 or 0.0 (with pressure)
	Ref<InputEventJoypadButton> jb_event = p_event;
	if (jb_event.is_valid()) {
		return jb_event->is_pressed() ? Vector3(jb_event->get_pressure(), 0.0f, 0.0f) : Vector3();
	}

	// Joypad axis → axis value
	Ref<InputEventJoypadMotion> jm_event = p_event;
	if (jm_event.is_valid()) {
		return Vector3(jm_event->get_axis_value(), 0.0f, 0.0f);
	}

	// Mouse motion → delta
	Ref<InputEventMouseMotion> mm_event = p_event;
	if (mm_event.is_valid()) {
		Vector2 rel = mm_event->get_relative();
		return Vector3(rel.x, rel.y, 0.0f);
	}

	return Vector3();
}

// ============================================================================
// Internal: run modifier chain
// ============================================================================

Vector3 EnhancedInputManager::_run_modifiers(MappingState &p_state, double p_delta) {
	Vector3 value = p_state.raw_value;
	TypedArray<InputModifier> modifiers = p_state.mapping->get_modifiers();

	for (int i = 0; i < modifiers.size(); i++) {
		Ref<InputModifier> mod = modifiers[i];
		if (mod.is_valid()) {
			value = mod->modify(value, p_delta);
		}
	}

	return value;
}

// ============================================================================
// process_input_event — Stage 1-3: Match events, extract values
// ============================================================================

void EnhancedInputManager::process_input_event(const Ref<InputEvent> &p_event) {
	if (p_event.is_null()) {
		return;
	}

	// Track whether this physical event has been consumed.
	// Once consumed, no lower-priority mapping for the same physical key should match.
	bool event_consumed = false;

	// Walk mapping states (they are in context priority order since we
	// built them by iterating contexts in descending priority).
	for (int i = 0; i < all_mapping_states.size(); i++) {
		MappingState &ms = all_mapping_states.write[i];
		Ref<InputActionMapping> mapping = ms.mapping;

		// Check if this event matches the mapping's input_event.
		if (!_event_matches_mapping(p_event, mapping->get_input_event())) {
			continue;
		}

		// Skip if this physical event was already consumed by a higher-priority mapping.
		if (event_consumed) {
			continue;
		}

		// Extract raw value.
		Vector3 value = _extract_value_from_event(p_event);
		ms.raw_value = value;
		ms.value_active = (value.length_squared() > 0.0f);

		// Consume: once a mapping with consume_input matches, all subsequent
		// mappings for the same physical key (in lower-priority contexts) are blocked.
		if (mapping->get_consume_input()) {
			event_consumed = true;
		}
	}
}

// ============================================================================
// tick — Stage 4-8: Modifiers, Accumulation, Triggers, Signals
// ============================================================================

void EnhancedInputManager::tick(double p_delta) {
	// --- Stage 4: Run modifiers and accumulate per action ---

	// Reset accumulated values.
	for (KeyValue<StringName, ActionState> &kv : action_states) {
		kv.value.accumulated_value = Vector3();
	}

	// For each mapping state, run modifiers and accumulate.
	for (int i = 0; i < all_mapping_states.size(); i++) {
		MappingState &ms = all_mapping_states.write[i];
		Ref<InputAction> action = ms.mapping->get_action();
		StringName action_name = action->get_action_name();

		if (!action_states.has(action_name)) {
			continue;
		}

		ActionState &as = action_states[action_name];

		// Run modifier chain.
		Vector3 modified_value = _run_modifiers(ms, p_delta);

		// Accumulate.
		if (action->get_accumulation_mode() == InputAction::CUMULATIVE) {
			as.accumulated_value += modified_value;
		} else {
			// TAKE_HIGHEST: keep the one with greater length.
			if (modified_value.length_squared() > as.accumulated_value.length_squared()) {
				as.accumulated_value = modified_value;
			}
		}
	}

	// --- Stage 5-6: Run triggers and detect state changes ---

	for (KeyValue<StringName, ActionState> &kv : action_states) {
		ActionState &as = kv.value;
		as.last_trigger_state = as.trigger_state;

		// Determine the final trigger state from all mapping states.
		TriggerState best_state = TRIGGER_NONE;

		for (int mi = 0; mi < as.mapping_states.size(); mi++) {
			MappingState *ms = as.mapping_states[mi];
			TypedArray<InputTrigger> triggers = ms->mapping->get_triggers();

			if (triggers.size() == 0) {
				// Default trigger: InputTriggerDown behavior.
				TriggerState ts = (as.accumulated_value.length_squared() > 0.0f) ? TRIGGER_TRIGGERED : TRIGGER_NONE;
				if (ts > best_state) {
					best_state = ts;
				}
			} else {
				// ANY mode: any trigger reaching TRIGGERED means triggered.
				for (int ti = 0; ti < triggers.size(); ti++) {
					Ref<InputTrigger> trigger = triggers[ti];
					if (trigger.is_null()) {
						continue;
					}

					// Ensure state arrays are big enough.
					if (ti >= ms->trigger_states.size()) {
						ms->trigger_states.resize(ti + 1);
						ms->trigger_elapsed.resize(ti + 1);
						ms->trigger_states.write[ti] = TRIGGER_NONE;
						ms->trigger_elapsed.write[ti] = 0.0f;
					}

					TriggerState new_ts = trigger->update_state(
							ms->trigger_states[ti],
							as.accumulated_value,
							p_delta,
							ms->trigger_elapsed.write[ti]);

					ms->trigger_states.write[ti] = new_ts;

					if (new_ts > best_state) {
						best_state = new_ts;
					}
				}
			}
		}

		as.trigger_state = best_state;

		// Track elapsed time.
		if (best_state != TRIGGER_NONE) {
			as.elapsed_time += (float)p_delta;
		} else {
			as.elapsed_time = 0.0f;
		}

		// --- Stage 7-8: Detect state transitions and emit signals ---

		Ref<InputActionValue> value_obj;
		value_obj.instantiate();
		value_obj->set_value_type(as.action->get_value_type());
		value_obj->set_raw(as.accumulated_value);

		TriggerState prev = as.last_trigger_state;
		TriggerState curr = as.trigger_state;

		if (prev == TRIGGER_NONE && (curr == TRIGGER_ONGOING || curr == TRIGGER_TRIGGERED)) {
			// Started.
			emit_signal("action_started", as.action, value_obj);
			_emit_action_event(kv.key, TRIGGER_EVENT_STARTED, as.action, value_obj);
		}

		if (curr == TRIGGER_ONGOING) {
			emit_signal("action_ongoing", as.action, value_obj);
			_emit_action_event(kv.key, TRIGGER_EVENT_ONGOING, as.action, value_obj);
		}

		if (curr == TRIGGER_TRIGGERED) {
			emit_signal("action_triggered", as.action, value_obj, (int)curr);
			_emit_action_event(kv.key, TRIGGER_EVENT_TRIGGERED, as.action, value_obj);
		}

		if ((prev == TRIGGER_TRIGGERED) && curr == TRIGGER_NONE) {
			// Completed.
			emit_signal("action_completed", as.action, value_obj);
			_emit_action_event(kv.key, TRIGGER_EVENT_COMPLETED, as.action, value_obj);
		}

		if (prev == TRIGGER_ONGOING && curr == TRIGGER_NONE) {
			// Canceled.
			emit_signal("action_canceled", as.action, value_obj);
			_emit_action_event(kv.key, TRIGGER_EVENT_CANCELED, as.action, value_obj);
		}
	}
}

// ============================================================================
// Query API
// ============================================================================

Ref<InputActionValue> EnhancedInputManager::get_action_value(const Ref<InputAction> &p_action) const {
	ERR_FAIL_COND_V(p_action.is_null(), Ref<InputActionValue>());

	StringName name = p_action->get_action_name();
	if (!action_states.has(name)) {
		return Ref<InputActionValue>();
	}

	const ActionState &as = action_states[name];
	Ref<InputActionValue> val;
	val.instantiate();
	val->set_value_type(p_action->get_value_type());
	val->set_raw(as.accumulated_value);
	return val;
}

bool EnhancedInputManager::is_action_triggered(const Ref<InputAction> &p_action) const {
	ERR_FAIL_COND_V(p_action.is_null(), false);

	StringName name = p_action->get_action_name();
	if (!action_states.has(name)) {
		return false;
	}

	return action_states[name].trigger_state == TRIGGER_TRIGGERED;
}

int EnhancedInputManager::get_action_trigger_state(const Ref<InputAction> &p_action) const {
	ERR_FAIL_COND_V(p_action.is_null(), (int)TRIGGER_NONE);

	StringName name = p_action->get_action_name();
	if (!action_states.has(name)) {
		return (int)TRIGGER_NONE;
	}

	return (int)action_states[name].trigger_state;
}

float EnhancedInputManager::get_action_elapsed_time(const Ref<InputAction> &p_action) const {
	ERR_FAIL_COND_V(p_action.is_null(), 0.0f);

	StringName name = p_action->get_action_name();
	if (!action_states.has(name)) {
		return 0.0f;
	}

	return action_states[name].elapsed_time;
}

// ============================================================================
// Per-action binding
// ============================================================================

void EnhancedInputManager::bind_action(const Ref<InputAction> &p_action, TriggerEvent p_event_type, const Callable &p_callable) {
	ERR_FAIL_COND(p_action.is_null());

	ActionBinding binding;
	binding.action_name = p_action->get_action_name();
	binding.event_type = p_event_type;
	binding.callable = p_callable;
	action_bindings.push_back(binding);
}

void EnhancedInputManager::unbind_action(const Ref<InputAction> &p_action, TriggerEvent p_event_type, const Callable &p_callable) {
	ERR_FAIL_COND(p_action.is_null());

	StringName action_name = p_action->get_action_name();

	for (int i = action_bindings.size() - 1; i >= 0; i--) {
		const ActionBinding &b = action_bindings[i];
		if (b.action_name == action_name && b.event_type == p_event_type && b.callable == p_callable) {
			action_bindings.remove_at(i);
			return;
		}
	}
}

void EnhancedInputManager::_emit_action_event(const StringName &p_action_name, TriggerEvent p_event_type, const Ref<InputAction> &p_action, const Ref<InputActionValue> &p_value) {
	for (int i = 0; i < action_bindings.size(); i++) {
		const ActionBinding &b = action_bindings[i];
		if (b.action_name == p_action_name && b.event_type == p_event_type) {
			Callable::CallError ce;
			Variant args[2] = { p_action, p_value };
			const Variant *argp[2] = { &args[0], &args[1] };
			Variant ret;
			b.callable.callp(argp, 2, ret, ce);
		}
	}
}
