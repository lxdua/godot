# Add `DisplayServer.screen_get_current_orientation()` to query the physical screen orientation

## Describe the project you are working on

Godot Android / DisplayServer API. Specifically, writing test scenes and
runtime logic that needs to react to the device's *current physical*
screen rotation (e.g. sensor pipelines, tutorials instructing the player
to tilt the device, orientation-adaptive HUDs).

## Describe the problem or limitation you are having in your project

`DisplayServer.screen_get_orientation()` returns the **requested**
orientation — that is, the value most recently passed to
`screen_set_orientation()` or the one configured in Project Settings
under `display/window/handheld/orientation`. It does **not** report
what direction the screen is physically facing right now.

On Android this is easy to verify in the source:

```cpp
// platform/android/display_server_android.cpp
DisplayServerEnums::ScreenOrientation DisplayServerAndroid::screen_get_orientation(int p_screen) const {
    ...
    const int orientation = godot_io_java->get_screen_orientation();
    ...
}
```

```java
// platform/android/java/.../GodotIO.java
public int getScreenOrientation() {
    ...
    int orientation = activity.getRequestedOrientation();  // <-- requested, not actual
    ...
}
```

The consequence: when the app is configured with `Orientation = Sensor`
(which is what you want for any game that supports all four device
orientations), `screen_get_orientation()` always returns
`SCREEN_SENSOR (6)`, and there is **no public GDScript API that reports
the actual physical rotation state** (`ROTATION_0 / 90 / 180 / 270`).

The only workaround today is to compare `get_window().size.x` and `.y`:
that tells you portrait vs. landscape, but cannot distinguish forward
from reverse (upright portrait vs. upside-down portrait produce the
same aspect ratio, as do the two landscape variants). So GDScript code
can observe at most 2 of the 4 Android `Surface.getRotation()` states.

This became a concrete blocker while working on
[#XXXXX](https://github.com/godotengine/godot/pull/XXXXX) (quaternion-aware
screen-rotation correction for the `TYPE_ROTATION_VECTOR` sensor):
reviewers want evidence that all four branches of the correction
`switch (cachedRotation) { ROTATION_0 / 90 / 180 / 270 }` have been
exercised on real devices, and there is no clean way to surface that
verification from a GDScript test scene.

## Describe the feature / enhancement and how it helps to overcome the problem or limitation

Add a new `DisplayServer` method that reports the *current physical*
screen orientation, without changing the semantics of the existing
`screen_get_orientation()` (which would be a breaking change for
anyone relying on it to read back their own setting):

```gdscript
# Reuses the existing ScreenOrientation enum.
# Returns one of: SCREEN_LANDSCAPE, SCREEN_PORTRAIT,
# SCREEN_REVERSE_LANDSCAPE, SCREEN_REVERSE_PORTRAIT.
DisplayServer.screen_get_current_orientation(screen := SCREEN_OF_MAIN_WINDOW) -> ScreenOrientation
```

Behavior:

- When orientation is locked (`Landscape`, `Portrait`, `Reverse_*`):
  returns the same value as `screen_get_orientation()`.
- When orientation is sensor-driven (`Sensor`, `Sensor_Landscape`,
  `Sensor_Portrait`): returns the one of the four cardinal values
  corresponding to the current physical rotation.
- On platforms without a physical rotation concept (typical desktops):
  returns the value of `screen_get_orientation()` as a safe default.

## Describe how your proposal will work, with code, pseudo-code, mock-ups, and/or diagrams

### Android

The physical rotation is *already computed* inside the engine for
sensor value remapping; it's cached in
`GodotInputHandler.cachedRotation` (a `Surface.ROTATION_*` value that's
kept in sync with display rotation events). A new JNI binding
`getCurrentScreenOrientation()` on `GodotIO` (or equivalent) would map
that value into the existing `SCREEN_*` enum:

```java
public int getCurrentScreenOrientation() {
    int rot = activity.getWindowManager().getDefaultDisplay().getRotation();
    switch (rot) {
        case Surface.ROTATION_0:   return SCREEN_PORTRAIT;
        case Surface.ROTATION_90:  return SCREEN_LANDSCAPE;
        case Surface.ROTATION_180: return SCREEN_REVERSE_PORTRAIT;
        case Surface.ROTATION_270: return SCREEN_REVERSE_LANDSCAPE;
        default:                   return SCREEN_PORTRAIT;
    }
}
```

Note that `ROTATION_*` to `SCREEN_*` mapping depends on whether the
device's natural orientation is portrait or landscape (tablets); the
implementation should consult the `Configuration.orientation` to
pick the right base mapping.

### iOS

Use `UIWindowScene.interfaceOrientation` (iOS 13+) or
`UIDevice.current.orientation`, mapped similarly.

### Desktop / Web / other

Physical rotation isn't typically meaningful on desktop; a simple
fallback to `screen_get_orientation()` is sufficient. If Windows tablet
mode / Wayland eventually expose a rotation signal, the corresponding
backend can be enhanced.

### Class reference additions

`DisplayServer` gets one new entry alongside `screen_get_orientation`,
cross-referenced in both directions so the distinction is documented
(the existing docs for `screen_get_orientation` don't currently spell
out that it returns the *setting*, which is also worth clarifying).

## If this enhancement will not be used often, can it be worked around with a few lines of script?

Partially. `get_window().size` inequality can distinguish landscape
from portrait, but not forward from reverse, so any logic that needs
all four states (e.g. sensor data validation, forced re-calibration
on 180° flips) has no scripting workaround. Requesting the orientation
via JNI from GDScript is not viable for normal users.

## Is there a reason why this should be core and not an add-on in the asset library?

- The information is already computed inside the engine for
  `GodotInputHandler`'s sensor axis remapping — exposing it is a matter
  of wiring existing data through JNI, not adding a new computation.
- An add-on cannot reach `getRotation()` without writing a custom Java
  plugin per Android target, which is well outside the reach of typical
  GDScript users.
- The companion method `screen_get_orientation()` is already a core
  `DisplayServer` API; its "physical state" counterpart belongs in the
  same place for discoverability.
