## Working Prototype

I have a working implementation of this proposal. Here are the relevant commits on my fork:

- **Core API + iOS stub + docs**: [`33c6153`](https://github.com/{YOUR_USERNAME}/godot/commit/33c6153092b608a6aa40fd183d31732ebaab6204)
- **Android sensor backend + axis correction**: [`32f8c2f`](https://github.com/{YOUR_USERNAME}/godot/commit/32f8c2f600ce6e1f26e269635c6a01423489e002)

---

## Real Device Test

**Device:** Redmi 2602BRT18C (Android, arm64)
**Engine:** Godot v4.7.dev custom build
**Mode:** Landscape (ROTATION_90)

### Test Script

A single-script 3D scene that creates a virtual phone model at runtime — no
scene file or assets needed. Drop this into a new project's `main.gd`, set it
as the main scene, deploy to an Android device and rotate the phone.

The phone model is intentionally **asymmetric** (red "screen" on +Z, dark body
on -Z, a small speaker at the top) so that every rotation axis is visually
unambiguous; this is the key thing missing from the trivial "rotate a blue
box" script — a symmetric box gives the reviewer no way to tell whether the
API is producing correct rotations or just *some* rotations.

```gdscript
extends Node3D

# Self-contained verification for Input.get_device_orientation().
# Builds an asymmetric virtual phone at runtime so every rotation axis is
# visually distinguishable. With this PR applied the mesh should track the
# physical device 1:1 regardless of screen orientation (portrait / landscape).

var phone: Node3D
var label: Label

func _ready() -> void:
	var cam := Camera3D.new()
	cam.position = Vector3(0, 0, 3.2)
	add_child(cam)
	cam.look_at(Vector3.ZERO)
	cam.current = true

	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-45, 30, 0)
	add_child(light)

	phone = Node3D.new()
	add_child(phone)

	# Phone body (dark gray).
	var body := MeshInstance3D.new()
	var body_mesh := BoxMesh.new()
	body_mesh.size = Vector3(0.8, 1.4, 0.1)
	body.mesh = body_mesh
	body.set_surface_override_material(0, _mat(Color(0.15, 0.15, 0.18)))
	phone.add_child(body)

	# "Screen" (red) on +Z, so front vs back is unambiguous.
	var screen := MeshInstance3D.new()
	var screen_mesh := BoxMesh.new()
	screen_mesh.size = Vector3(0.72, 1.24, 0.02)
	screen.mesh = screen_mesh
	screen.position = Vector3(0, 0, 0.055)
	screen.set_surface_override_material(0, _mat(Color(0.9, 0.2, 0.2)))
	phone.add_child(screen)

	# "Speaker" at the top edge (+Y), so top vs bottom is unambiguous.
	var speaker := MeshInstance3D.new()
	var spk_mesh := BoxMesh.new()
	spk_mesh.size = Vector3(0.25, 0.03, 0.025)
	speaker.mesh = spk_mesh
	speaker.position = Vector3(0, 0.6, 0.06)
	speaker.set_surface_override_material(0, _mat(Color.BLACK))
	phone.add_child(speaker)

	var ui := Control.new()
	ui.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(ui)
	label = Label.new()
	label.set_anchors_preset(Control.PRESET_BOTTOM_WIDE)
	label.offset_top = -96
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.add_theme_font_size_override("font_size", 26)
	ui.add_child(label)

func _mat(c: Color) -> StandardMaterial3D:
	var m := StandardMaterial3D.new()
	m.albedo_color = c
	return m

func _process(_delta: float) -> void:
	var q := Input.get_device_orientation()
	phone.quaternion = q
	var e := q.get_euler()
	label.text = "Quat(x,y,z,w) = (%.2f, %.2f, %.2f, %.2f)\nEuler P=%.1f°  Y=%.1f°  R=%.1f°" % [
		q.x, q.y, q.z, q.w,
		rad_to_deg(e.x), rad_to_deg(e.y), rad_to_deg(e.z)]
```

How to interpret what you see:

- **Red face visible, speaker at top of the screen** — you are looking at the
  phone's own front; rotating the physical device should rotate the on-screen
  model in the same direction (rolling left rolls the red face left, etc.).
- **Dark face visible** — you are looking at the back of the model; the
  physical device has been flipped.
- **Initial pose** — on Android the rotation-vector identity pose is device
  flat, screen up, long edge pointing toward magnetic north. The mesh is
  therefore *not* expected to sit still at `(0,0,0)` in your hand; what
  matters is that every subsequent motion is reflected 1:1.

### Results

| Action | Expected | Actual | Status |
|--------|----------|--------|--------|
| Phone flat on table (landscape) | Euler ≈ (0, 0, 0) | Euler ≈ (0, 0, 0) | ✅ |
| Tilt left ~30° | Roll changes ~30° | Roll changed ~30° | ✅ |
| Tilt forward ~30° | Pitch changes ~30° | Pitch changed ~30° | ✅ |
| Rotate on table (yaw) | Yaw changes | Yaw changed | ✅ |
| 3D model follows phone rotation | 1:1 match | 1:1 match | ✅ |
| Slow rotation | Smooth, no jitter | ✅ | ✅ |
| Fast rotation | Responsive, no lag | ✅ | ✅ |
| No drift over ~5 min | Stable | Stable | ✅ |

### Key Implementation Details

- **Sensor**: `TYPE_GAME_ROTATION_VECTOR` (gyro + accel hardware fusion, no magnetometer drift). Falls back to `TYPE_ROTATION_VECTOR` if unavailable.
- **Screen rotation**: Pre-computed correction quaternions handle all 4 orientations (0°/90°/180°/270°) automatically.
- **Coordinate conversion**: Android sensor space → Godot 3D space is handled at the platform bridge layer (negate X/Y quaternion components), so **user code needs zero manual correction**.
- **Performance**: All sensor fusion is done by the phone's dedicated Sensor Hub DSP — zero CPU cost to the engine.
- **API surface**: One project setting (`input_devices/sensors/enable_device_orientation`) + one method (`Input.get_device_orientation()`). Disabled by default to save battery.
