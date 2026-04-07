
/**************************************************************************/
/*  enhanced_input_trigger.h                                              */
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
#include "core/object/gdvirtual.gen.h"
#include "core/math/vector3.h"

class InputAction;

// ============================================================================
// Trigger enums
// ============================================================================

enum TriggerState {
	TRIGGER_NONE,
	TRIGGER_ONGOING,
	TRIGGER_TRIGGERED,
};

enum TriggerEvent {
	TRIGGER_EVENT_NONE = 0,
	TRIGGER_EVENT_STARTED = 1 << 0,
	TRIGGER_EVENT_ONGOING = 1 << 1,
	TRIGGER_EVENT_TRIGGERED = 1 << 2,
	TRIGGER_EVENT_COMPLETED = 1 << 3,
	TRIGGER_EVENT_CANCELED = 1 << 4,
};

VARIANT_ENUM_CAST(TriggerState);
VARIANT_BITFIELD_CAST(TriggerEvent);

// ============================================================================
// InputTrigger — Base class
// ============================================================================

class InputTrigger : public Resource {
	GDCLASS(InputTrigger, Resource);

protected:
	static void _bind_methods();
	GDVIRTUAL3R(int, _update_state, int, Vector3, double);

public:
	virtual TriggerState update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed);
};

// ============================================================================
// InputTriggerDown — Triggered every frame while value is non-zero
// ============================================================================

class InputTriggerDown : public InputTrigger {
	GDCLASS(InputTriggerDown, InputTrigger);

protected:
	static void _bind_methods();

public:
	virtual TriggerState update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) override;
};

// ============================================================================
// InputTriggerPressed — Triggered on the frame value becomes non-zero
// ============================================================================

class InputTriggerPressed : public InputTrigger {
	GDCLASS(InputTriggerPressed, InputTrigger);

protected:
	static void _bind_methods();

public:
	virtual TriggerState update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) override;
};

// ============================================================================
// InputTriggerReleased — Triggered on the frame value becomes zero
// ============================================================================

class InputTriggerReleased : public InputTrigger {
	GDCLASS(InputTriggerReleased, InputTrigger);

protected:
	static void _bind_methods();

public:
	virtual TriggerState update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) override;
};

// ============================================================================
// InputTriggerHold — Triggered after holding for a duration
// ============================================================================

class InputTriggerHold : public InputTrigger {
	GDCLASS(InputTriggerHold, InputTrigger);

private:
	float hold_time = 0.5f;
	bool one_shot = false;

protected:
	static void _bind_methods();

public:
	virtual TriggerState update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) override;

	void set_hold_time(float p_time);
	float get_hold_time() const;

	void set_one_shot(bool p_one_shot);
	bool get_one_shot() const;
};

// ============================================================================
// InputTriggerHoldAndRelease — Triggered when released after holding long enough
// ============================================================================

class InputTriggerHoldAndRelease : public InputTrigger {
	GDCLASS(InputTriggerHoldAndRelease, InputTrigger);

private:
	float hold_time = 0.5f;

protected:
	static void _bind_methods();

public:
	virtual TriggerState update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) override;

	void set_hold_time(float p_time);
	float get_hold_time() const;
};

// ============================================================================
// InputTriggerTap — Triggered on quick press-and-release
// ============================================================================

class InputTriggerTap : public InputTrigger {
	GDCLASS(InputTriggerTap, InputTrigger);

private:
	float tap_time = 0.3f;

protected:
	static void _bind_methods();

public:
	virtual TriggerState update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) override;

	void set_tap_time(float p_time);
	float get_tap_time() const;
};

// ============================================================================
// InputTriggerPulse — Repeatedly triggers at an interval while held
// ============================================================================

class InputTriggerPulse : public InputTrigger {
	GDCLASS(InputTriggerPulse, InputTrigger);

private:
	float interval = 0.5f;
	bool trigger_on_start = true;

protected:
	static void _bind_methods();

public:
	virtual TriggerState update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) override;

	void set_interval(float p_interval);
	float get_interval() const;

	void set_trigger_on_start(bool p_trigger);
	bool get_trigger_on_start() const;
};

// ============================================================================
// InputTriggerChordAction — Triggered only when another action is also active
// ============================================================================

class InputTriggerChordAction : public InputTrigger {
	GDCLASS(InputTriggerChordAction, InputTrigger);

private:
	Ref<InputAction> chord_action;

protected:
	static void _bind_methods();

public:
	virtual TriggerState update_state(TriggerState p_current_state, const Vector3 &p_value, double p_delta, float &r_elapsed) override;

	void set_chord_action(const Ref<InputAction> &p_action);
	Ref<InputAction> get_chord_action() const;
};
