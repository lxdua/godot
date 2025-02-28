#ifndef DUA_DOCK_H
#define DUA_DOCK_H

#include "scene/gui/box_container.h"

class Button;
class EditorInspector;
class Label;
class LineEdit;

class DuaDock : public VBoxContainer {
	GDCLASS(DuaDock, VBoxContainer);

	EditorInspector *todo_ei = nullptr;
	VBoxContainer *todo_vb = nullptr;
	Label *online_time_label = nullptr;
	Label *todo_title_label = nullptr;
	HBoxContainer *add_todo_hb = nullptr;
	LineEdit *add_todo_line_edit = nullptr;
	Button *add_todo_button = nullptr;

	int online_time = 0;

	void _on_online_timer_timeout();
	void _on_add_button_pressed();

	void update_dua_label(const String &p_text);

public:
	DuaDock();
};


class DuaTodoHB : public HBoxContainer {
	GDCLASS(DuaTodoHB, HBoxContainer);

	Label *todo_label = nullptr;
	Button *finish_button = nullptr;

	void _on_completed_button_pressed();

public:
	DuaTodoHB();
	void set_todo_text(const String& p_text);
};

#endif // DUA_DOCK_H
