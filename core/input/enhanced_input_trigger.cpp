
/**************************************************************************/
/*  enhanced_input_trigger.cpp                                            */
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

#include "enhanced_input_trigger.h"

#include "core/input/enhanced_input_action.h"
#include "core/input/enhanced_input_manager.h"

static bool _is_value_non_zero(const Vector3 &p_value) {
	return p_value.length_squared() > 0.0f;
}

// ============================================================================
// InputTrigger — Base class
// ============================================================================

void InputTrigger::_bind_methods() {
	GDVIRTUAL_BIND(_update_state, "current_state", "value", "delta");

	// Bind TriggerState enum.
	BIND_ENUM_CONSTANT(TRIGGER_NONE);
	BIND_ENUM_CONSTANT(TRIGGER_ONGOING);
	BIND_ENUM_CONSTANT(TRIGGER_TRIGGERED);

	// Bind TriggerEvent bitfield.
	BIND_ENUM_CONSTANT(TRIGGER_EVENT_NONE);
	BIND_ENUM_CONSTANT(TRIGGER_EVENT_STARTED);
	BIND_ENUM_CONSTANT(TRIGGER_EVENT_ONGOING);
	BIND_ENUM_CONSTANT(TRIGGER_EVENT_TRIGGERED);
	BIND_ENUM_CONSTANT(TRIGGER_EVENT_COMPLETED);
	BIND_ENUM_CONSTANT(TRIGGER_EVENT_CANCELED);
}

TriggerState InputTrigger::update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) {
	// If GDScript override exists, call it (without elapsed — GDScript API is simpler).
	int result = (int)p_current_state;
	if (GDVIRTUAL_CALL(_update_state, (int)p_current_state, p_value, p_delta, result)) {
		return (TriggerState)result;
	}
	// Default: behave like InputTriggerDown.
	return _is_value_non_zero(p_value) ? TRIGGER_TRIGGERED : TRIGGER_NONE;
}

// ============================================================================
// InputTriggerDown
// ============================================================================

void InputTriggerDown::_bind_methods() {
	// No properties.
}

TriggerState InputTriggerDown::update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) {
	return _is_value_non_zero(p_value) ? TRIGGER_TRIGGERED : TRIGGER_NONE;
}

// ============================================================================
// InputTriggerPressed
// ============================================================================

void InputTriggerPressed::_bind_methods() {
	// No properties.
}

TriggerState InputTriggerPressed::update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) {
	if (_is_value_non_zero(p_value)) {
		// Only trigger on the transition from NONE to non-zero.
		if (p_current_state == TRIGGER_NONE) {
			return TRIGGER_TRIGGERED;
		}
		// After the first frame, stay ONGOING so we don't re-trigger
		// (ONGOING != NONE, so next frame won't match the condition above).
		return TRIGGER_ONGOING;
	}
	return TRIGGER_NONE;
}

// ============================================================================
// InputTriggerReleased
// ============================================================================

void InputTriggerReleased::_bind_methods() {
	// No properties.
}

TriggerState InputTriggerReleased::update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) {
	if (!_is_value_non_zero(p_value)) {
		// Trigger on the transition from non-NONE to zero.
		if (p_current_state == TRIGGER_ONGOING || p_current_state == TRIGGER_TRIGGERED) {
			return TRIGGER_TRIGGERED;
		}
		return TRIGGER_NONE;
	}
	// While held, stay ONGOING (so we can detect release).
	return TRIGGER_ONGOING;
}

// ============================================================================
// InputTriggerHold
// ============================================================================

void InputTriggerHold::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_hold_time", "time"), &InputTriggerHold::set_hold_time);
	ClassDB::bind_method(D_METHOD("get_hold_time"), &InputTriggerHold::get_hold_time);

	ClassDB::bind_method(D_METHOD("set_one_shot", "one_shot"), &InputTriggerHold::set_one_shot);
	ClassDB::bind_method(D_METHOD("get_one_shot"), &InputTriggerHold::get_one_shot);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hold_time", PROPERTY_HINT_RANGE, "0.0,10.0,0.05,or_greater"), "set_hold_time", "get_hold_time");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "one_shot"), "set_one_shot", "get_one_shot");
}

TriggerState InputTriggerHold::update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) {
	if (_is_value_non_zero(p_value)) {
		r_elapsed += (float)p_delta;
		if (r_elapsed >= hold_time) {
			if (one_shot && p_current_state == TRIGGER_TRIGGERED) {
				// Already triggered once, stay NONE.
				return TRIGGER_NONE;
			}
			return TRIGGER_TRIGGERED;
		}
		return TRIGGER_ONGOING;
	}
	// Released — reset.
	r_elapsed = 0.0f;
	return TRIGGER_NONE;
}

void InputTriggerHold::set_hold_time(float p_time) { hold_time = MAX(p_time, 0.0f); }
float InputTriggerHold::get_hold_time() const { return hold_time; }
void InputTriggerHold::set_one_shot(bool p_one_shot) { one_shot = p_one_shot; }
bool InputTriggerHold::get_one_shot() const { return one_shot; }

// ============================================================================
// InputTriggerHoldAndRelease
// ============================================================================

void InputTriggerHoldAndRelease::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_hold_time", "time"), &InputTriggerHoldAndRelease::set_hold_time);
	ClassDB::bind_method(D_METHOD("get_hold_time"), &InputTriggerHoldAndRelease::get_hold_time);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hold_time", PROPERTY_HINT_RANGE, "0.0,10.0,0.05,or_greater"), "set_hold_time", "get_hold_time");
}

TriggerState InputTriggerHoldAndRelease::update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) {
	if (_is_value_non_zero(p_value)) {
		r_elapsed += (float)p_delta;
		return TRIGGER_ONGOING;
	}
	// Released.
	if (r_elapsed >= hold_time) {
		r_elapsed = 0.0f;
		return TRIGGER_TRIGGERED;
	}
	r_elapsed = 0.0f;
	return TRIGGER_NONE;
}

void InputTriggerHoldAndRelease::set_hold_time(float p_time) { hold_time = MAX(p_time, 0.0f); }
float InputTriggerHoldAndRelease::get_hold_time() const { return hold_time; }

// ============================================================================
// InputTriggerTap
// ============================================================================

void InputTriggerTap::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_tap_time", "time"), &InputTriggerTap::set_tap_time);
	ClassDB::bind_method(D_METHOD("get_tap_time"), &InputTriggerTap::get_tap_time);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tap_time", PROPERTY_HINT_RANGE, "0.0,2.0,0.05,or_greater"), "set_tap_time", "get_tap_time");
}

TriggerState InputTriggerTap::update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) {
	if (_is_value_non_zero(p_value)) {
		r_elapsed += (float)p_delta;
		// If held too long, it's not a tap.
		if (r_elapsed > tap_time) {
			return TRIGGER_NONE;
		}
		return TRIGGER_ONGOING;
	}
	// Released.
	if (r_elapsed > 0.0f && r_elapsed <= tap_time) {
		r_elapsed = 0.0f;
		return TRIGGER_TRIGGERED;
	}
	r_elapsed = 0.0f;
	return TRIGGER_NONE;
}

void InputTriggerTap::set_tap_time(float p_time) { tap_time = MAX(p_time, 0.01f); }
float InputTriggerTap::get_tap_time() const { return tap_time; }

// ============================================================================
// InputTriggerPulse
// ============================================================================

void InputTriggerPulse::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_interval", "interval"), &InputTriggerPulse::set_interval);
	ClassDB::bind_method(D_METHOD("get_interval"), &InputTriggerPulse::get_interval);

	ClassDB::bind_method(D_METHOD("set_trigger_on_start", "trigger"), &InputTriggerPulse::set_trigger_on_start);
	ClassDB::bind_method(D_METHOD("get_trigger_on_start"), &InputTriggerPulse::get_trigger_on_start);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interval", PROPERTY_HINT_RANGE, "0.01,10.0,0.01,or_greater"), "set_interval", "get_interval");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "trigger_on_start"), "set_trigger_on_start", "get_trigger_on_start");
}

TriggerState InputTriggerPulse::update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) {
	if (_is_value_non_zero(p_value)) {
		// First frame pressed.
		if (p_current_state == TRIGGER_NONE) {
			r_elapsed = 0.0f;
			return trigger_on_start ? TRIGGER_TRIGGERED : TRIGGER_ONGOING;
		}
		r_elapsed += (float)p_delta;
		if (r_elapsed >= interval) {
			r_elapsed -= interval; // Keep remainder for consistent timing.
			return TRIGGER_TRIGGERED;
		}
		return TRIGGER_ONGOING;
	}
	// Released.
	r_elapsed = 0.0f;
	return TRIGGER_NONE;
}

void InputTriggerPulse::set_interval(float p_interval) { interval = MAX(p_interval, 0.01f); }
float InputTriggerPulse::get_interval() const { return interval; }
void InputTriggerPulse::set_trigger_on_start(bool p_trigger) { trigger_on_start = p_trigger; }
bool InputTriggerPulse::get_trigger_on_start() const { return trigger_on_start; }

// ============================================================================
// InputTriggerChordAction
// ============================================================================

void InputTriggerChordAction::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_chord_action", "action"), &InputTriggerChordAction::set_chord_action);
	ClassDB::bind_method(D_METHOD("get_chord_action"), &InputTriggerChordAction::get_chord_action);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "chord_action", PROPERTY_HINT_RESOURCE_TYPE, "InputAction"), "set_chord_action", "get_chord_action");
}

TriggerState InputTriggerChordAction::update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) {
	if (!_is_value_non_zero(p_value)) {
		return TRIGGER_NONE;
	}

	// Check if the chord action is also active.
	if (chord_action.is_valid() && EnhancedInputManager::get_singleton()) {
		if (EnhancedInputManager::get_singleton()->is_action_triggered(chord_action)) {
			return TRIGGER_TRIGGERED;
		}
		// Chord action not active — stay ongoing (waiting for chord).
		return TRIGGER_ONGOING;
	}

	// No chord action configured — just check own value.
	return TRIGGER_TRIGGERED;
}

void InputTriggerChordAction::set_chord_action(const Ref<InputAction> &p_action) { chord_action = p_action; }
Ref<InputAction> InputTriggerChordAction::get_chord_action() const { return chord_action; }
