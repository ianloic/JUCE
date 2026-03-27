/*
  ==============================================================================

   This file is part of the JUCE framework.

   Copyright (c) 2026 by Ian McKellar

   You may use this under the terms of the GNU AGPLv3 or later:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

#include <EGL/egl.h>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

extern "C" {
#include "xdg-shell-client-protocol.h"
#include "xdg-shell-protocol.c"
}

namespace juce {

class WaylandComponentPeer;

class WaylandDisplay {
public:
  static void registry_handle_global(void *data, struct wl_registry *registry,
                                     uint32_t name, const char *interface,
                                     uint32_t version) {
    auto *wd = static_cast<WaylandDisplay *>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0)
      wd->compositor = static_cast<wl_compositor *>(
          wl_registry_bind(registry, name, &wl_compositor_interface, version));
    else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      wd->xdg_wm_base = static_cast<struct xdg_wm_base *>(
          wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
      static const struct xdg_wm_base_listener xdg_wm_base_listener = {
          [](void *, struct xdg_wm_base *xdg_wm_base, uint32_t serial) {
            xdg_wm_base_pong(xdg_wm_base, serial);
          }};
      xdg_wm_base_add_listener(wd->xdg_wm_base, &xdg_wm_base_listener, wd);
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0)
      wd->shm = static_cast<wl_shm *>(
          wl_registry_bind(registry, name, &wl_shm_interface, 1));
    else if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      wd->seat = static_cast<wl_seat *>(
          wl_registry_bind(registry, name, &wl_seat_interface, 1));
      wd->bindSeat(wd->seat);
    }
  }

  static void registry_handle_global_remove(void *, struct wl_registry *,
                                            uint32_t) {}

  WaylandDisplay() {
    xkbContext = xkb_context_new (XKB_CONTEXT_NO_FLAGS);
    display = wl_display_connect(nullptr);
    if (display != nullptr) {
      registry = wl_display_get_registry(display);

      static const struct wl_registry_listener registry_listener = {
          WaylandDisplay::registry_handle_global,
          WaylandDisplay::registry_handle_global_remove};

      wl_registry_add_listener(registry, &registry_listener, this);
      wl_display_roundtrip(display);

      int fd = wl_display_get_fd(display);
      LinuxEventLoop::registerFdCallback(
          fd, [this](int) { wl_display_dispatch(display); }, 1 /* POLLIN */);
    }
  }

  ~WaylandDisplay() {
    if (display != nullptr) {
      LinuxEventLoop::unregisterFdCallback(wl_display_get_fd(display));
      if (xkbState != nullptr) xkb_state_unref (xkbState);
      if (xkbKeymap != nullptr) xkb_keymap_unref (xkbKeymap);
      if (xkbContext != nullptr) xkb_context_unref (xkbContext);
      if (xdg_wm_base != nullptr)
        xdg_wm_base_destroy(xdg_wm_base);
      if (shm != nullptr)
        wl_shm_destroy(shm);
      if (keyboard != nullptr)
        wl_keyboard_destroy(keyboard);
      if (seat != nullptr)
        wl_seat_destroy(seat);
      if (compositor != nullptr)
        wl_compositor_destroy(compositor);
      wl_registry_destroy(registry);
      wl_display_disconnect(display);
    }
  }

  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  wl_compositor *compositor = nullptr;
  struct xdg_wm_base *xdg_wm_base = nullptr;
  wl_shm *shm = nullptr;
  wl_seat *seat = nullptr;

  void bindSeat(wl_seat *s);
  void handleSeatCapabilities(wl_seat *s, uint32_t caps);

  wl_pointer *pointer = nullptr;
  WaylandComponentPeer *pointerFocus = nullptr;
  Point<float> pointerPos;
  ModifierKeys currentModifiers;

  xkb_context* xkbContext = nullptr;
  xkb_keymap* xkbKeymap = nullptr;
  xkb_state* xkbState = nullptr;
  wl_keyboard* keyboard = nullptr;
  WaylandComponentPeer* keyboardFocus = nullptr;
  
  int getJUCEKeyCodeFromXKB (xkb_keysym_t sym, juce_wchar& unicodeChar);

  JUCE_DECLARE_SINGLETON_INLINE(WaylandDisplay, false)
};

//==============================================================================
static int createAnonymousFile(off_t size) {
  int fd = fileno(tmpfile());
  if (fd >= 0) {
    if (ftruncate(fd, size) < 0) {
      close(fd);
      return -1;
    }
  }
  return fd;
}

class WaylandComponentPeer final : public ComponentPeer, private AsyncUpdater {
public:
  WaylandComponentPeer(Component &comp, int windowStyleFlags,
                       void * /*parentToAddTo*/)
      : ComponentPeer(comp, windowStyleFlags) {
    auto *wd = WaylandDisplay::getInstance();
    if (wd != nullptr && wd->display != nullptr) {
      surface = wl_compositor_create_surface(wd->compositor);
      wl_surface_set_user_data(surface, this);
      xdgSurface = xdg_wm_base_get_xdg_surface(wd->xdg_wm_base, surface);
      xdgToplevel = xdg_surface_get_toplevel(xdgSurface);

      static const struct xdg_surface_listener surface_listener = {
          [](void *data, struct xdg_surface *xdg_surf, uint32_t serial) {
            xdg_surface_ack_configure(xdg_surf, serial);
            auto *peer = static_cast<WaylandComponentPeer *>(data);
            peer->configured = true;
            peer->triggerAsyncUpdate();
            wl_display_flush(WaylandDisplay::getInstance()->display);
          }};
      xdg_surface_add_listener(xdgSurface, &surface_listener, this);

      static const struct xdg_toplevel_listener toplevel_listener = {
          [](void *data, struct xdg_toplevel *, int32_t w, int32_t h,
             struct wl_array *) {
            auto *peer = static_cast<WaylandComponentPeer *>(data);
            if (w > 0 && h > 0)
              peer->setBounds(Rectangle<int>(0, 0, w, h), false);
          },
          [](void *data, struct xdg_toplevel *) {
            auto *peer = static_cast<WaylandComponentPeer *>(data);
            peer->handleUserClosingWindow();
          },
          [](void *, struct xdg_toplevel *, int32_t, int32_t) {},
          [](void *, struct xdg_toplevel *, struct wl_array *) {}};
      xdg_toplevel_add_listener(xdgToplevel, &toplevel_listener, this);

      wl_surface_commit(surface);
      wl_display_flush(wd->display);
    }
  }

  ~WaylandComponentPeer() override {
    if (buffer != nullptr)
      wl_buffer_destroy(buffer);
    if (xdgToplevel != nullptr)
      xdg_toplevel_destroy(xdgToplevel);
    if (xdgSurface != nullptr)
      xdg_surface_destroy(xdgSurface);
    if (surface != nullptr)
      wl_surface_destroy(surface);
  }

  void *getNativeHandle() const override { return nullptr; }

  void setBounds(const Rectangle<int> &newBounds,
                 bool isNowFullScreen) override {
    bounds = newBounds;
    fullScreen = isNowFullScreen;
    handleMovedOrResized();
  }

  Rectangle<int> getBounds() const override { return bounds; }
  Point<float> localToGlobal(Point<float> relativePosition) override {
    return relativePosition + bounds.getPosition().toFloat();
  }
  Point<float> globalToLocal(Point<float> screenPosition) override {
    return screenPosition - bounds.getPosition().toFloat();
  }

  void setVisible(bool) override {}
  void setTitle(const String &) override {}
  void setMinimised(bool) override {}
  bool isMinimised() const override { return false; }
  void setFullScreen(bool shouldBeFullScreen) override {
    fullScreen = shouldBeFullScreen;
  }
  bool isFullScreen() const override { return fullScreen; }
  bool contains(Point<int>, bool) const override { return true; }
  void toFront(bool) override {}
  void toBehind(ComponentPeer *) override {}
  bool isFocused() const override { return true; }
  void grabFocus() override {}
  void textInputRequired(Point<int>, TextInputTarget &) override {}

  void repaint(const Rectangle<int> &area) override {
    if (WaylandDisplay::getInstance()->display == nullptr)
      return;
    if (!bounds.isEmpty())
      regionsNeedingRepaint.add(area.getIntersection(bounds.withZeroOrigin()));
    else
      regionsNeedingRepaint.add(area);
    
    if (configured)
      triggerAsyncUpdate();
  }

  void handleAsyncUpdate() override { performAnyPendingRepaintsNow(); }

  void performAnyPendingRepaintsNow() override {
    if (!configured || bounds.isEmpty() ||
        WaylandDisplay::getInstance()->display == nullptr)
      return;

    auto originalRepaintRegion = regionsNeedingRepaint;
    regionsNeedingRepaint.clear();

    if (!originalRepaintRegion.isEmpty()) {
      if (image.isNull() || image.getWidth() < bounds.getWidth() ||
          image.getHeight() < bounds.getHeight())
        image = Image(Image::ARGB, bounds.getWidth(), bounds.getHeight(), true);

      for (auto &i : originalRepaintRegion)
        image.clear(i);

      {
        auto context = getComponent().getLookAndFeel().createGraphicsContext(
            image, Point<int>(), originalRepaintRegion);
        handlePaint(*context);
      }

      blitToWayland(image, originalRepaintRegion);
    }
  }

  void blitToWayland(const Image &img,
                     const RectangleList<int> &changedRegions) {
    int width = bounds.getWidth();
    int height = bounds.getHeight();
    int stride = width * 4;
    int size = stride * height;

    int fd = createAnonymousFile(size);
    if (fd < 0)
      return;

    void *data = mmap(nullptr, static_cast<size_t>(size),
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
      close(fd);
      return;
    }

    Image::BitmapData srcData(img, Image::BitmapData::readOnly);

    for (int y = 0; y < height; ++y)
      std::memcpy(static_cast<uint8_t *>(data) + y * stride,
                  srcData.getLinePointer(y), static_cast<size_t>(stride));

    auto *pool =
        wl_shm_create_pool(WaylandDisplay::getInstance()->shm, fd, size);
    auto *new_buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                 WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    munmap(data, static_cast<size_t>(size));
    close(fd);

    wl_surface_attach(surface, new_buffer, 0, 0);
    for (auto &rect : changedRegions)
      wl_surface_damage_buffer(surface, rect.getX(), rect.getY(),
                               rect.getWidth(), rect.getHeight());

    wl_surface_commit(surface);
    wl_display_flush(WaylandDisplay::getInstance()->display);

    if (buffer)
      wl_buffer_destroy(buffer);
    buffer = new_buffer;
  }
  void setAlpha(float) override {}
  bool setAlwaysOnTop(bool) override { return false; }
  void setIcon(const Image &) override {}

  bool isShowing() const override { return true; }
  OptionalBorderSize getFrameSizeIfPresent() const override { return {}; }
  BorderSize<int> getFrameSize() const override { return {}; }
  StringArray getAvailableRenderingEngines() override {
    return StringArray("Software Renderer");
  }

private:
  Rectangle<int> bounds{0, 0, 800, 600};
  bool fullScreen = false;
  bool configured = false;
  RectangleList<int> regionsNeedingRepaint;
  Image image;

  wl_surface *surface = nullptr;
  xdg_surface *xdgSurface = nullptr;
  xdg_toplevel *xdgToplevel = nullptr;
  wl_buffer *buffer = nullptr;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaylandComponentPeer)
};

//==============================================================================
ComponentPeer *Component::createNewPeer(int styleFlags,
                                        void *nativeWindowToAttachTo) {
  return new WaylandComponentPeer(*this, styleFlags, nativeWindowToAttachTo);
}

//==============================================================================
JUCE_API bool JUCE_CALLTYPE Process::isForegroundProcess() { return true; }
JUCE_API void JUCE_CALLTYPE Process::makeForegroundProcess() {}
JUCE_API void JUCE_CALLTYPE Process::hide() {}

//==============================================================================
void Desktop::setKioskComponent(Component *, bool, bool) {}
void Displays::findDisplays(const Desktop &) {
  Display d;
  d.isMain = true;
  d.totalArea = Rectangle<int>(0, 0, 1920, 1080);
  d.userArea = Rectangle<int>(0, 0, 1920, 1080);
  d.safeAreaInsets = BorderSize<int>();
  d.keyboardInsets = BorderSize<int>();
  d.topLeftPhysical = Point<int>(0, 0);
  d.scale = 1.0;
  d.dpi = 96.0; // Avoid Desktop::getInstance() here!
  d.verticalFrequencyHz = 60.0;
  displays.add(d);
  updateToLogical();
}

bool Desktop::canUseSemiTransparentWindows() noexcept { return true; }
struct Desktop::NativeDarkModeChangeDetectorImpl {};
std::unique_ptr<Desktop::NativeDarkModeChangeDetectorImpl>
Desktop::createNativeDarkModeChangeDetectorImpl() {
  return nullptr;
}
bool Desktop::isDarkModeActive() const { return true; }
void Desktop::setScreenSaverEnabled(bool) {}
bool Desktop::isScreenSaverEnabled() { return true; }
double Desktop::getDefaultMasterScale() { return 1.0; }
Desktop::DisplayOrientation Desktop::getCurrentOrientation() const {
  return upright;
}
void Desktop::allowedOrientationsChanged() {}

//==============================================================================
bool detail::MouseInputSourceList::addSource() {
  if (sources.isEmpty()) {
    addSource(0, MouseInputSource::InputSourceType::mouse);
    return true;
  }
  return false;
}
bool detail::MouseInputSourceList::canUseTouch() const { return false; }
Point<float> MouseInputSource::getCurrentRawMousePosition() {
  return Point<float>();
}
void MouseInputSource::setRawMousePosition(Point<float>) {}

//==============================================================================
class MouseCursor::PlatformSpecificHandle {
public:
  explicit PlatformSpecificHandle(const MouseCursor::StandardCursorType) {}
  explicit PlatformSpecificHandle(const detail::CustomMouseCursorInfo &) {}
  ~PlatformSpecificHandle() {}
  static void showInWindow(PlatformSpecificHandle *, ComponentPeer *) {}

private:
  JUCE_DECLARE_NON_COPYABLE(PlatformSpecificHandle)
  JUCE_DECLARE_NON_MOVEABLE(PlatformSpecificHandle)
};

//==============================================================================
bool DragAndDropContainer::performExternalDragDropOfFiles(
    const StringArray &, bool, Component *, std::function<void()>) {
  return false;
}
bool DragAndDropContainer::performExternalDragDropOfText(
    const String &, Component *, std::function<void()>) {
  return false;
}
void SystemClipboard::copyTextToClipboard(const String &) {}
String SystemClipboard::getTextFromClipboard() { return {}; }
bool KeyPress::isKeyCurrentlyDown(int) { return false; }
void LookAndFeel::playAlertSound() {}

//==============================================================================
namespace Keys {
static constexpr int extendedKeyModifier = 0x10000000;
}

// Use standard XKB keysym values (which are identical to X11 keysyms)
const int KeyPress::spaceKey = ' ';
const int KeyPress::returnKey = 0x0d;
const int KeyPress::escapeKey = 0x1b;
const int KeyPress::backspaceKey = 0x08;
const int KeyPress::leftKey = 0x51 | Keys::extendedKeyModifier;
const int KeyPress::rightKey = 0x53 | Keys::extendedKeyModifier;
const int KeyPress::upKey = 0x52 | Keys::extendedKeyModifier;
const int KeyPress::downKey = 0x54 | Keys::extendedKeyModifier;
const int KeyPress::pageUpKey = 0x55 | Keys::extendedKeyModifier;
const int KeyPress::pageDownKey = 0x56 | Keys::extendedKeyModifier;
const int KeyPress::endKey = 0x57 | Keys::extendedKeyModifier;
const int KeyPress::homeKey = 0x50 | Keys::extendedKeyModifier;
const int KeyPress::insertKey = 0x63 | Keys::extendedKeyModifier;
const int KeyPress::deleteKey = 0xff | Keys::extendedKeyModifier;
const int KeyPress::tabKey = '\t';
const int KeyPress::F1Key = 0xbe | Keys::extendedKeyModifier;
const int KeyPress::F2Key = 0xbf | Keys::extendedKeyModifier;
const int KeyPress::F3Key = 0xc0 | Keys::extendedKeyModifier;
const int KeyPress::F4Key = 0xc1 | Keys::extendedKeyModifier;
const int KeyPress::F5Key = 0xc2 | Keys::extendedKeyModifier;
const int KeyPress::F6Key = 0xc3 | Keys::extendedKeyModifier;
const int KeyPress::F7Key = 0xc4 | Keys::extendedKeyModifier;
const int KeyPress::F8Key = 0xc5 | Keys::extendedKeyModifier;
const int KeyPress::F9Key = 0xc6 | Keys::extendedKeyModifier;
const int KeyPress::F10Key = 0xc7 | Keys::extendedKeyModifier;
const int KeyPress::F11Key = 0xc8 | Keys::extendedKeyModifier;
const int KeyPress::F12Key = 0xc9 | Keys::extendedKeyModifier;
const int KeyPress::F13Key = 0xca | Keys::extendedKeyModifier;
const int KeyPress::F14Key = 0xcb | Keys::extendedKeyModifier;
const int KeyPress::F15Key = 0xcc | Keys::extendedKeyModifier;
const int KeyPress::F16Key = 0xcd | Keys::extendedKeyModifier;
const int KeyPress::F17Key = 0xce | Keys::extendedKeyModifier;
const int KeyPress::F18Key = 0xcf | Keys::extendedKeyModifier;
const int KeyPress::F19Key = 0xd0 | Keys::extendedKeyModifier;
const int KeyPress::F20Key = 0xd1 | Keys::extendedKeyModifier;
const int KeyPress::F21Key = 0xd2 | Keys::extendedKeyModifier;
const int KeyPress::F22Key = 0xd3 | Keys::extendedKeyModifier;
const int KeyPress::F23Key = 0xd4 | Keys::extendedKeyModifier;
const int KeyPress::F24Key = 0xd5 | Keys::extendedKeyModifier;
const int KeyPress::F25Key = 0xd6 | Keys::extendedKeyModifier;
const int KeyPress::F26Key = 0xd7 | Keys::extendedKeyModifier;
const int KeyPress::F27Key = 0xd8 | Keys::extendedKeyModifier;
const int KeyPress::F28Key = 0xd9 | Keys::extendedKeyModifier;
const int KeyPress::F29Key = 0xda | Keys::extendedKeyModifier;
const int KeyPress::F30Key = 0xdb | Keys::extendedKeyModifier;
const int KeyPress::F31Key = 0xdc | Keys::extendedKeyModifier;
const int KeyPress::F32Key = 0xdd | Keys::extendedKeyModifier;
const int KeyPress::F33Key = 0xde | Keys::extendedKeyModifier;
const int KeyPress::F34Key = 0xdf | Keys::extendedKeyModifier;
const int KeyPress::F35Key = 0xe0 | Keys::extendedKeyModifier;
const int KeyPress::numberPad0 = 0xb0 | Keys::extendedKeyModifier;
const int KeyPress::numberPad1 = 0xb1 | Keys::extendedKeyModifier;
const int KeyPress::numberPad2 = 0xb2 | Keys::extendedKeyModifier;
const int KeyPress::numberPad3 = 0xb3 | Keys::extendedKeyModifier;
const int KeyPress::numberPad4 = 0xb4 | Keys::extendedKeyModifier;
const int KeyPress::numberPad5 = 0xb5 | Keys::extendedKeyModifier;
const int KeyPress::numberPad6 = 0xb6 | Keys::extendedKeyModifier;
const int KeyPress::numberPad7 = 0xb7 | Keys::extendedKeyModifier;
const int KeyPress::numberPad8 = 0xb8 | Keys::extendedKeyModifier;
const int KeyPress::numberPad9 = 0xb9 | Keys::extendedKeyModifier;
const int KeyPress::numberPadAdd = 0xab | Keys::extendedKeyModifier;
const int KeyPress::numberPadSubtract = 0xad | Keys::extendedKeyModifier;
const int KeyPress::numberPadMultiply = 0xaa | Keys::extendedKeyModifier;
const int KeyPress::numberPadDivide = 0xaf | Keys::extendedKeyModifier;
const int KeyPress::numberPadSeparator = 0xac | Keys::extendedKeyModifier;
const int KeyPress::numberPadDecimalPoint = 0xae | Keys::extendedKeyModifier;
const int KeyPress::numberPadEquals = 0xbd | Keys::extendedKeyModifier;
const int KeyPress::numberPadDelete = 0x9f | Keys::extendedKeyModifier;
const int KeyPress::playKey = ((int)0xffeeff00) | Keys::extendedKeyModifier;
const int KeyPress::stopKey = ((int)0xffeeff01) | Keys::extendedKeyModifier;
const int KeyPress::fastForwardKey =
    ((int)0xffeeff02) | Keys::extendedKeyModifier;
const int KeyPress::rewindKey = ((int)0xffeeff03) | Keys::extendedKeyModifier;

//==============================================================================
static const struct wl_pointer_listener pointer_listener = {
    [](void *data, struct wl_pointer *, uint32_t, struct wl_surface *surface,
       wl_fixed_t fx, wl_fixed_t fy) {
      auto *d = static_cast<WaylandDisplay *>(data);
      if (surface)
        d->pointerFocus = static_cast<WaylandComponentPeer *>(
            wl_surface_get_user_data(surface));
      d->pointerPos = {(float)wl_fixed_to_double(fx),
                       (float)wl_fixed_to_double(fy)};
      if (d->pointerFocus)
        d->pointerFocus->handleMouseEvent(
            MouseInputSource::InputSourceType::mouse, d->pointerPos,
            d->currentModifiers, 0.0f, 0.0f,
            Time::getCurrentTime().toMilliseconds(), {}, false);
    },
    [](void *data, struct wl_pointer *, uint32_t, struct wl_surface *) {
      auto *d = static_cast<WaylandDisplay *>(data);
      if (d->pointerFocus) {
        d->pointerFocus->handleMouseEvent(
            MouseInputSource::InputSourceType::mouse, d->pointerPos,
            d->currentModifiers, 0.0f, 0.0f,
            Time::getCurrentTime().toMilliseconds(), {}, false);
        d->pointerFocus = nullptr;
      }
    },
    [](void *data, struct wl_pointer *, uint32_t, wl_fixed_t fx,
       wl_fixed_t fy) {
      auto *d = static_cast<WaylandDisplay *>(data);
      d->pointerPos = {(float)wl_fixed_to_double(fx),
                       (float)wl_fixed_to_double(fy)};
      if (d->pointerFocus)
        d->pointerFocus->handleMouseEvent(
            MouseInputSource::InputSourceType::mouse, d->pointerPos,
            d->currentModifiers, 0.0f, 0.0f,
            Time::getCurrentTime().toMilliseconds(), {}, false);
    },
    [](void *data, struct wl_pointer *, uint32_t, uint32_t, uint32_t button,
       uint32_t state) {
      auto *d = static_cast<WaylandDisplay *>(data);
      int modifier = 0;
      if (button == 0x110)
        modifier = ModifierKeys::leftButtonModifier;
      else if (button == 0x111)
        modifier = ModifierKeys::rightButtonModifier;
      else if (button == 0x112)
        modifier = ModifierKeys::middleButtonModifier;

      if (state == 1)
        d->currentModifiers = d->currentModifiers.withFlags(modifier);
      else
        d->currentModifiers = d->currentModifiers.withoutFlags(modifier);

      if (d->pointerFocus)
        d->pointerFocus->handleMouseEvent(
            MouseInputSource::InputSourceType::mouse, d->pointerPos,
            d->currentModifiers, 1.0f, 0.0f,
            Time::getCurrentTime().toMilliseconds(), {}, false);
    },
    [](void *, struct wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {},
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr};

static const struct wl_keyboard_listener keyboard_listener = {
    [] (void* data, struct wl_keyboard*, uint32_t format, int32_t fd, uint32_t size) {
        auto* d = static_cast<WaylandDisplay*> (data);
        if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
            char* map_str = (char*) mmap (nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            if (map_str != MAP_FAILED) {
                if (d->xkbState) xkb_state_unref (d->xkbState);
                if (d->xkbKeymap) xkb_keymap_unref (d->xkbKeymap);
                d->xkbKeymap = xkb_keymap_new_from_string (d->xkbContext, map_str, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
                if (d->xkbKeymap) d->xkbState = xkb_state_new (d->xkbKeymap);
                munmap (map_str, size);
            }
        }
        close (fd);
    },
    [] (void* data, struct wl_keyboard*, uint32_t, struct wl_surface* surface, struct wl_array*) {
        auto* d = static_cast<WaylandDisplay*> (data);
        if (surface) d->keyboardFocus = static_cast<WaylandComponentPeer*> (wl_surface_get_user_data (surface));
    },
    [] (void* data, struct wl_keyboard*, uint32_t, struct wl_surface*) {
        auto* d = static_cast<WaylandDisplay*> (data);
        d->keyboardFocus = nullptr;
    },
    [] (void* data, struct wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
        auto* d = static_cast<WaylandDisplay*> (data);
        if (d->xkbState && d->keyboardFocus) {
            xkb_keysym_t sym = xkb_state_key_get_one_sym (d->xkbState, key + 8);
            juce_wchar unicodeChar = 0;
            int keyCode = d->getJUCEKeyCodeFromXKB (sym, unicodeChar);
            bool isDown = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
            d->keyboardFocus->handleKeyUpOrDown (isDown);
            if (isDown && (keyCode != 0 || unicodeChar != 0)) {
                KeyPress kp (keyCode, d->currentModifiers, unicodeChar);
                d->keyboardFocus->handleKeyPress (kp);
            }
        }
    },
    [] (void* data, struct wl_keyboard*, uint32_t, uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked, uint32_t group) {
        auto* d = static_cast<WaylandDisplay*> (data);
        if (d->xkbState) {
            xkb_state_update_mask (d->xkbState, mods_depressed, mods_latched, mods_locked, 0, 0, group);
            int modifier = 0;
            if (xkb_state_mod_name_is_active (d->xkbState, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE))
                modifier |= ModifierKeys::shiftModifier;
            if (xkb_state_mod_name_is_active (d->xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE))
                modifier |= ModifierKeys::ctrlModifier;
            if (xkb_state_mod_name_is_active (d->xkbState, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE))
                modifier |= ModifierKeys::altModifier;
            int mouseMods = d->currentModifiers.getRawFlags() & (ModifierKeys::leftButtonModifier | ModifierKeys::rightButtonModifier | ModifierKeys::middleButtonModifier);
            d->currentModifiers = ModifierKeys (mouseMods | modifier);
            if (d->keyboardFocus) d->keyboardFocus->handleModifierKeysChange();
        }
    },
    [] (void*, struct wl_keyboard*, int32_t, int32_t) {}
};

static const struct wl_seat_listener seat_listener = {
    [](void *data, struct wl_seat *seat, uint32_t caps) {
      static_cast<WaylandDisplay *>(data)->handleSeatCapabilities(seat, caps);
    },
    [](void *, struct wl_seat *, const char *) {}};

void WaylandDisplay::bindSeat(wl_seat *s) {
  wl_seat_add_listener(s, &seat_listener, this);
}

void WaylandDisplay::handleSeatCapabilities(wl_seat *s, uint32_t caps) {
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
    pointer = wl_seat_get_pointer(s);
    wl_pointer_add_listener(pointer, &pointer_listener, this);
  }
  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
    keyboard = wl_seat_get_keyboard(s);
    wl_keyboard_add_listener(keyboard, &keyboard_listener, this);
  }
}

int WaylandDisplay::getJUCEKeyCodeFromXKB (xkb_keysym_t sym, juce_wchar& unicodeChar)
{
    unicodeChar = (juce_wchar) xkb_keysym_to_utf32 (sym);
    int keyCode = (int) unicodeChar;
    if (keyCode < 0x20) keyCode = (int) sym;
    if ((sym & 0xff00) == 0xff00 || keyCode == XKB_KEY_ISO_Left_Tab)
    {
        switch (sym) {
            case XKB_KEY_KP_Add: keyCode = XKB_KEY_plus; break;
            case XKB_KEY_KP_Subtract: keyCode = XKB_KEY_hyphen; break;
            case XKB_KEY_KP_Divide: keyCode = XKB_KEY_slash; break;
            case XKB_KEY_KP_Multiply: keyCode = XKB_KEY_asterisk; break;
            case XKB_KEY_KP_Enter: keyCode = XKB_KEY_Return; break;
            case XKB_KEY_KP_Insert: keyCode = XKB_KEY_Insert; break;
            case XKB_KEY_Delete: case XKB_KEY_KP_Delete: keyCode = XKB_KEY_Delete; break;
            case XKB_KEY_KP_Left: keyCode = XKB_KEY_Left; break;
            case XKB_KEY_KP_Right: keyCode = XKB_KEY_Right; break;
            case XKB_KEY_KP_Up: keyCode = XKB_KEY_Up; break;
            case XKB_KEY_KP_Down: keyCode = XKB_KEY_Down; break;
            case XKB_KEY_KP_Home: keyCode = XKB_KEY_Home; break;
            case XKB_KEY_KP_End: keyCode = XKB_KEY_End; break;
            case XKB_KEY_KP_Page_Down: keyCode = XKB_KEY_Page_Down; break;
            case XKB_KEY_KP_Page_Up: keyCode = XKB_KEY_Page_Up; break;
            case XKB_KEY_KP_0: keyCode = XKB_KEY_0; break;
            case XKB_KEY_KP_1: keyCode = XKB_KEY_1; break;
            case XKB_KEY_KP_2: keyCode = XKB_KEY_2; break;
            case XKB_KEY_KP_3: keyCode = XKB_KEY_3; break;
            case XKB_KEY_KP_4: keyCode = XKB_KEY_4; break;
            case XKB_KEY_KP_5: keyCode = XKB_KEY_5; break;
            case XKB_KEY_KP_6: keyCode = XKB_KEY_6; break;
            case XKB_KEY_KP_7: keyCode = XKB_KEY_7; break;
            case XKB_KEY_KP_8: keyCode = XKB_KEY_8; break;
            case XKB_KEY_KP_9: keyCode = XKB_KEY_9; break;
        }
        switch (keyCode) {
            case XKB_KEY_Left: case XKB_KEY_Right: case XKB_KEY_Up: case XKB_KEY_Down:
            case XKB_KEY_Page_Up: case XKB_KEY_Page_Down: case XKB_KEY_End: case XKB_KEY_Home:
            case XKB_KEY_Delete: case XKB_KEY_Insert:
                keyCode = (keyCode & 0xff) | Keys::extendedKeyModifier; break;
            case XKB_KEY_Tab: case XKB_KEY_Return: case XKB_KEY_Escape: case XKB_KEY_BackSpace:
                keyCode &= 0xff; break;
            case XKB_KEY_ISO_Left_Tab:
                keyCode = XKB_KEY_Tab & 0xff; break;
            default:
                if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F35)
                    keyCode = static_cast<int> ((sym & 0xff) | Keys::extendedKeyModifier);
                break;
        }
    }
    return keyCode;
}

extern "C" __attribute__((visibility("default"))) int
juce_gtkWebkitMain(int, const char *const *) {
  return 0;
}
Image detail::WindowingHelpers::createIconForFile(const File &) { return {}; }
void juce_LinuxAddRepaintListener(ComponentPeer *, Component *);
void juce_LinuxAddRepaintListener(ComponentPeer *, Component *) {}
void juce_LinuxRemoveRepaintListener(ComponentPeer *, Component *);
void juce_LinuxRemoveRepaintListener(ComponentPeer *, Component *) {}

} // namespace juce
