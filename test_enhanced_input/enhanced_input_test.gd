
# enhanced_input_test.gd
# 挂在任意 Node 上即可运行测试
extends Node

# === Actions ===
var move_action: InputAction
var jump_action: InputAction
var interact_action: InputAction
var exit_vehicle_action: InputAction

# === Contexts ===
var on_foot_ctx: InputMappingContext
var vehicle_ctx: InputMappingContext

# === State ===
var in_vehicle: bool = false

func _ready():
	print("=== Enhanced Input Test ===")
	print("Controls:")
	print("  WASD  - Move (check Vector2 output)")
	print("  Space - Jump (Pressed trigger, fires once)")
	print("  Tab   - Toggle vehicle context")
	print("  E     - Interact (on foot) / Exit vehicle (in vehicle)")
	print("")

	_setup_actions()
	_setup_on_foot_context()
	_setup_vehicle_context()

	# Start with on-foot context
	EnhancedInput.push_mapping_context(on_foot_ctx, 0)
	print("[Context] Pushed: OnFoot (priority 0)")

	# Connect signals
	EnhancedInput.action_triggered.connect(_on_action_triggered)
	EnhancedInput.action_started.connect(_on_action_started)
	EnhancedInput.action_completed.connect(_on_action_completed)
	EnhancedInput.action_canceled.connect(_on_action_canceled)

	# Also test per-action binding
	EnhancedInput.bind_action(jump_action, "triggered", _on_jump)

func _setup_actions():
	# Move: Vector2, WASD cumulative
	move_action = InputAction.new()
	move_action.action_name = "Move"
	move_action.value_type = InputActionValue.VECTOR2
	move_action.accumulation_mode = InputAction.CUMULATIVE

	# Jump: Bool, only on press
	jump_action = InputAction.new()
	jump_action.action_name = "Jump"
	jump_action.value_type = InputActionValue.BOOL

	# Interact: Bool, on press (on foot)
	interact_action = InputAction.new()
	interact_action.action_name = "Interact"
	interact_action.value_type = InputActionValue.BOOL
	interact_action.consume_input = true  # Block lower-priority E mappings

	# Exit Vehicle: Bool, on press (in vehicle)
	exit_vehicle_action = InputAction.new()
	exit_vehicle_action.action_name = "ExitVehicle"
	exit_vehicle_action.value_type = InputActionValue.BOOL
	exit_vehicle_action.consume_input = true

func _setup_on_foot_context():
	on_foot_ctx = InputMappingContext.new()
	on_foot_ctx.context_name = "OnFoot"

	# W = Move forward (0, -1) — negative Y is up in 2D
	var w_key = InputEventKey.new()
	w_key.keycode = KEY_W
	var w_map = on_foot_ctx.map_action(move_action, w_key)
	var w_scalar = InputModifierScalar.new()
	w_scalar.scale = Vector3(0, -1, 0)
	w_map.add_modifier(w_scalar)

	# S = Move backward (0, 1)
	var s_key = InputEventKey.new()
	s_key.keycode = KEY_S
	var s_map = on_foot_ctx.map_action(move_action, s_key)
	var s_scalar = InputModifierScalar.new()
	s_scalar.scale = Vector3(0, 1, 0)
	s_map.add_modifier(s_scalar)

	# A = Move left (-1, 0)
	var a_key = InputEventKey.new()
	a_key.keycode = KEY_A
	var a_map = on_foot_ctx.map_action(move_action, a_key)
	var a_scalar = InputModifierScalar.new()
	a_scalar.scale = Vector3(-1, 0, 0)
	a_map.add_modifier(a_scalar)

	# D = Move right (1, 0)
	var d_key = InputEventKey.new()
	d_key.keycode = KEY_D
	var d_map = on_foot_ctx.map_action(move_action, d_key)
	var d_scalar = InputModifierScalar.new()
	d_scalar.scale = Vector3(1, 0, 0)
	d_map.add_modifier(d_scalar)

	# Space = Jump (Pressed trigger: only fires once on press)
	var space_key = InputEventKey.new()
	space_key.keycode = KEY_SPACE
	var jump_map = on_foot_ctx.map_action(jump_action, space_key)
	jump_map.add_trigger(InputTriggerPressed.new())

	# E = Interact (Pressed trigger)
	var e_key = InputEventKey.new()
	e_key.keycode = KEY_E
	var interact_map = on_foot_ctx.map_action(interact_action, e_key)
	interact_map.add_trigger(InputTriggerPressed.new())

func _setup_vehicle_context():
	vehicle_ctx = InputMappingContext.new()
	vehicle_ctx.context_name = "Vehicle"

	# E = Exit Vehicle (Pressed trigger) — same key, different action
	var e_key = InputEventKey.new()
	e_key.keycode = KEY_E
	var exit_map = vehicle_ctx.map_action(exit_vehicle_action, e_key)
	exit_map.add_trigger(InputTriggerPressed.new())

func _input(event):
	# Tab to toggle vehicle context
	if event is InputEventKey and event.keycode == KEY_TAB and event.pressed and not event.echo:
		in_vehicle = !in_vehicle
		if in_vehicle:
			EnhancedInput.push_mapping_context(vehicle_ctx, 50)
			print("[Context] Pushed: Vehicle (priority 50) — E key is now 'Exit Vehicle'")
		else:
			EnhancedInput.pop_mapping_context(vehicle_ctx)
			print("[Context] Popped: Vehicle — E key is now 'Interact'")

func _process(_delta):
	# Poll move value every frame
	var move_val = EnhancedInput.get_action_value(move_action)
	if move_val and move_val.is_non_zero():
		var dir = move_val.get_vector2()
		print("[Move] direction: ", dir)

# === Signal callbacks ===

func _on_action_triggered(action: InputAction, value: InputActionValue, state: int):
	if action == move_action:
		return  # Don't spam move logs here, handled in _process
	print("[Triggered] ", action.action_name, " = ", value.get_float(), " state=", state)

func _on_action_started(action: InputAction, value: InputActionValue):
	if action == move_action:
		return
	print("[Started] ", action.action_name)

func _on_action_completed(action: InputAction, value: InputActionValue):
	if action == move_action:
		return
	print("[Completed] ", action.action_name)

func _on_action_canceled(action: InputAction, value: InputActionValue):
	if action == move_action:
		return
	print("[Canceled] ", action.action_name)

# === Per-action binding callback ===

func _on_jump(action: InputAction, value: InputActionValue):
	print("[bind_action] Jump fired! value=", value.get_bool())
