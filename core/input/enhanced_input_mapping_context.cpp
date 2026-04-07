
/**************************************************************************/
/*  enhanced_input_mapping_context.cpp                                    */
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

#include "enhanced_input_mapping_context.h"

#include "core/input/input_event.h"

// ============================================================================
// InputActionMapping
// ============================================================================

void InputActionMapping::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_action", "action"), &InputActionMapping::set_action);
	ClassDB::bind_method(D_METHOD("get_action"), &InputActionMapping::get_action);

	ClassDB::bind_method(D_METHOD("set_input_event", "event"), &InputActionMapping::set_input_event);
	ClassDB::bind_method(D_METHOD("get_input_event"), &InputActionMapping::get_input_event);

	ClassDB::bind_method(D_METHOD("set_modifiers", "modifiers"), &InputActionMapping::set_modifiers);
	ClassDB::bind_method(D_METHOD("get_modifiers"), &InputActionMapping::get_modifiers);
	ClassDB::bind_method(D_METHOD("add_modifier", "modifier"), &InputActionMapping::add_modifier);

	ClassDB::bind_method(D_METHOD("set_triggers", "triggers"), &InputActionMapping::set_triggers);
	ClassDB::bind_method(D_METHOD("get_triggers"), &InputActionMapping::get_triggers);
	ClassDB::bind_method(D_METHOD("add_trigger", "trigger"), &InputActionMapping::add_trigger);

	ClassDB::bind_method(D_METHOD("set_consume_input", "consume"), &InputActionMapping::set_consume_input);
	ClassDB::bind_method(D_METHOD("get_consume_input"), &InputActionMapping::get_consume_input);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "action", PROPERTY_HINT_RESOURCE_TYPE, "InputAction"), "set_action", "get_action");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "input_event", PROPERTY_HINT_RESOURCE_TYPE, "InputEvent"), "set_input_event", "get_input_event");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "modifiers", PROPERTY_HINT_ARRAY_TYPE, "InputModifier"), "set_modifiers", "get_modifiers");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "triggers", PROPERTY_HINT_ARRAY_TYPE, "InputTrigger"), "set_triggers", "get_triggers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "consume_input"), "set_consume_input", "get_consume_input");
}

void InputActionMapping::set_action(const Ref<InputAction> &p_action) {
	action = p_action;
}

Ref<InputAction> InputActionMapping::get_action() const {
	return action;
}

void InputActionMapping::set_input_event(const Ref<InputEvent> &p_event) {
	input_event = p_event;
}

Ref<InputEvent> InputActionMapping::get_input_event() const {
	return input_event;
}

void InputActionMapping::set_modifiers(const TypedArray<InputModifier> &p_modifiers) {
	modifiers = p_modifiers;
}

TypedArray<InputModifier> InputActionMapping::get_modifiers() const {
	return modifiers;
}

void InputActionMapping::add_modifier(const Ref<InputModifier> &p_modifier) {
	modifiers.push_back(p_modifier);
}

void InputActionMapping::set_triggers(const TypedArray<InputTrigger> &p_triggers) {
	triggers = p_triggers;
}

TypedArray<InputTrigger> InputActionMapping::get_triggers() const {
	return triggers;
}

void InputActionMapping::add_trigger(const Ref<InputTrigger> &p_trigger) {
	triggers.push_back(p_trigger);
}

void InputActionMapping::set_consume_input(bool p_consume) {
	consume_input = p_consume;
}

bool InputActionMapping::get_consume_input() const {
	return consume_input;
}

// ============================================================================
// InputMappingContext
// ============================================================================

void InputMappingContext::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_context_name", "name"), &InputMappingContext::set_context_name);
	ClassDB::bind_method(D_METHOD("get_context_name"), &InputMappingContext::get_context_name);

	ClassDB::bind_method(D_METHOD("set_mappings", "mappings"), &InputMappingContext::set_mappings);
	ClassDB::bind_method(D_METHOD("get_mappings"), &InputMappingContext::get_mappings);

	ClassDB::bind_method(D_METHOD("add_mapping", "mapping"), &InputMappingContext::add_mapping);
	ClassDB::bind_method(D_METHOD("remove_mapping", "index"), &InputMappingContext::remove_mapping);
	ClassDB::bind_method(D_METHOD("map_action", "action", "event"), &InputMappingContext::map_action);

	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "context_name"), "set_context_name", "get_context_name");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "mappings", PROPERTY_HINT_ARRAY_TYPE, "InputActionMapping"), "set_mappings", "get_mappings");
}

void InputMappingContext::set_context_name(const StringName &p_name) {
	context_name = p_name;
}

StringName InputMappingContext::get_context_name() const {
	return context_name;
}

void InputMappingContext::set_mappings(const TypedArray<InputActionMapping> &p_mappings) {
	mappings = p_mappings;
}

TypedArray<InputActionMapping> InputMappingContext::get_mappings() const {
	return mappings;
}

void InputMappingContext::add_mapping(const Ref<InputActionMapping> &p_mapping) {
	mappings.push_back(p_mapping);
}

void InputMappingContext::remove_mapping(int p_index) {
	ERR_FAIL_INDEX(p_index, mappings.size());
	mappings.remove_at(p_index);
}

Ref<InputActionMapping> InputMappingContext::map_action(const Ref<InputAction> &p_action, const Ref<InputEvent> &p_event) {
	Ref<InputActionMapping> mapping;
	mapping.instantiate();
	mapping->set_action(p_action);
	mapping->set_input_event(p_event);
	mappings.push_back(mapping);
	return mapping;
}
