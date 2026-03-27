# JUCE Wayland Backend - Remaining Tasks

This document outlines the pending steps to transition the experimental Wayland connection into a fully functional graphical backend for JUCE on Linux.

## ~~1. Window Rendering~~ (Completed)
~~Currently, the `WaylandComponentPeer` maps a basic `xdg_surface` and attaches a static red `wl_shm` buffer. To render actual UI components:~~
* ~~**Software Rendering Pipeline**: During `ComponentPeer::repaint()`, invoke JUCE's software renderer to draw the component hierarchy into a `juce::Image`. Copy the `juce::Image` ARGB pixels into the memory-mapped anonymous file representing the `wl_shm_pool` buffer, then submit via `wl_surface_damage_buffer` and `wl_surface_commit`.~~
* ~~**Buffer Management**: Implement double-buffering or a circular pool of `wl_shm` buffers to prevent overwriting a frame that the compositor is currently reading. Wait for `wl_buffer::release` events before reusing memory.~~
* ~~**OpenGL / EGL Contexts**: For hardware-accelerated components, properly implement `OpenGLContext::NativeContext`. Initialize EGL, create a `wl_egl_window` using the `wl_surface`, and bind an `eglCreateWindowSurface()`.~~

## ~~2. Input Handling (`wl_seat`)~~ (Completed)
~~The `wl_seat` global provides access to input peripherals.~~
* ~~**Pointer Events (`wl_pointer`)**: Add listeners for `enter`, `leave`, `motion`, `button`, and `axis` events. Map coordinate data to JUCE's `MouseInputSource` and dispatch events to the component framework via `handleMouseEvent()`.~~
* ~~**Keyboard Events (`wl_keyboard`)**: Utilize `libxkbcommon` to decode the `keymap` event file descriptor provided by the compositor. Translate Wayland keycodes into JUCE key presses and modifiers, then dispatch via `handleKeyPress()` and `handleModifierKeysChange()`.~~
* ~~**Focus Management**: Track pointer focus and keyboard focus as surfaces emit `enter` and `leave` events.~~

## 3. Window Sizing and State Management
* **Dynamic Resizing**: The `xdg_toplevel::configure` event dictates window sizing from the compositor (e.g., maximizing or tiling). `WaylandComponentPeer` must respond by resizing its JUCE `Component` bounds, allocating a new `wl_shm` buffer of the correct dimensions, and acknowledging the configure serial before committing the new buffer.
* **Window Properties**: Expose JUCE window controls to Wayland via `xdg_toplevel_set_title`, `xdg_toplevel_set_app_id`, `xdg_toplevel_set_fullscreen`, `xdg_toplevel_set_maximized`, and `xdg_toplevel_set_minimized`.
* **Window Hierarchy**: Support popups (menus, dropdowns, tooltips) by creating `xdg_popup` surfaces parented to the main `xdg_toplevel`.

## 4. Multi-Monitor and HiDPI (`wl_output`)
* **Display Enumeration**: Add a `wl_output` listener to the global registry to detect connected monitors.
* **Resolution and Scaling**: Populate JUCE's `Displays::findDisplays` with real output geometry, refresh rates, and integer scale factors provided by the `wl_output::geometry` and `wl_output::scale` events.
* **Fractional Scaling**: (Optional/Advanced) Support the `wp_fractional_scale_v1` protocol to allow crisp rendering on compositors that support non-integer scaling factors.

## 5. Pointer Cursors
* **Themes and Rendering**: Bind `wayland-cursor` to load the system cursor theme.
* **Cursor Control**: When a JUCE component requests a custom cursor type, look up the appropriate cursor buffer from the theme and attach it using `wl_pointer_set_cursor()`.

## 6. Clipboard and Drag & Drop
* **Data Device Integration**: Bind to `wl_data_device_manager`.
* **System Clipboard**: Implement `SystemClipboard::copyTextToClipboard` and `getTextFromClipboard` by responding to `wl_data_source` and `wl_data_offer` selection events.
* **Drag and Drop**: Fully flesh out `DragAndDropContainer` external interaction by listening to Wayland `enter`, `motion`, `drop`, and `leave` data device events.
