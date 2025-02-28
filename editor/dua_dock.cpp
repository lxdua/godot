
#include "dua_dock.h"

#include "editor/editor_inspector.h"
#include "editor/editor_properties.h"

#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"


DuaDock::DuaDock() {
	set_name("DuaDock");

	online_time_label = memnew(Label);
	add_child(online_time_label);

	todo_title_label = memnew(Label);
	add_child(todo_title_label);
	todo_title_label->set_text(TTR("TodoList"));

	Timer *online_timer = memnew(Timer);
	online_timer->set_wait_time(1.0);
	online_timer->set_one_shot(false);
	online_timer->set_autostart(true);
	online_timer->connect("timeout", callable_mp(this, &DuaDock::_on_online_timer_timeout));
	add_child(online_timer);

	todo_ei = memnew(EditorInspector);
	todo_ei->set_h_size_flags(SIZE_EXPAND_FILL);
	todo_ei->set_v_size_flags(SIZE_EXPAND_FILL);
	add_child(todo_ei);

	todo_vb = memnew(VBoxContainer);
	todo_vb->set_h_size_flags(SIZE_EXPAND_FILL);
	todo_vb->set_v_size_flags(SIZE_EXPAND_FILL);
	todo_ei->add_child(todo_vb);

	add_todo_hb = memnew(HBoxContainer);
	add_todo_hb->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(add_todo_hb);

	add_todo_line_edit = memnew(LineEdit);
	add_todo_line_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	add_todo_hb->add_child(add_todo_line_edit);

	add_todo_button = memnew(Button);
	add_todo_button->connect(SceneStringName(pressed), callable_mp(this, &DuaDock::_on_add_button_pressed));
	add_todo_button->set_text(TTR("Add Todo"));
	add_todo_button->set_disabled(false);
	add_todo_hb->add_child(add_todo_button);
}

void DuaDock::_on_online_timer_timeout() {
	online_time++;
	update_dua_label(TTR("Online Time: ") + stringify_variants(online_time) + "s");
}

void DuaDock::update_dua_label(const String &p_text) {
	online_time_label->set_text(p_text);
}

void DuaDock::_on_add_button_pressed() {
	DuaTodoHB *todo_hb = memnew(DuaTodoHB);
	todo_hb->set_todo_text(add_todo_line_edit->get_text());
	todo_vb->add_child(todo_hb);
}

DuaTodoHB::DuaTodoHB() {

	set_h_size_flags(SIZE_EXPAND_FILL);

	todo_label = memnew(Label);
	todo_label->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(todo_label);

	finish_button = memnew(Button);
	finish_button->set_text(TTR("Completed Todo"));
	finish_button->connect(SceneStringName(pressed), callable_mp(this, &DuaTodoHB::_on_completed_button_pressed));
	add_child(finish_button);
}

void DuaTodoHB::set_todo_text(const String& p_text) {
	todo_label->set_text(p_text);
}

void DuaTodoHB::_on_completed_button_pressed() {
	queue_free();
}
