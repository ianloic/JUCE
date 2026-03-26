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

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>

extern "C" {
#include "xdg-shell-client-protocol.h"
#include "xdg-shell-protocol.c"
}

namespace juce
{

class WaylandDisplay
{
public:
    static void registry_handle_global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version)
    {
        auto* wd = static_cast<WaylandDisplay*> (data);
        if (std::strcmp(interface, wl_compositor_interface.name) == 0)
            wd->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, version));
        else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
            wd->xdg_wm_base = static_cast<struct xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
            static const struct xdg_wm_base_listener xdg_wm_base_listener = {
                [] (void*, struct xdg_wm_base* xdg_wm_base, uint32_t serial) {
                    xdg_wm_base_pong(xdg_wm_base, serial);
                }
            };
            xdg_wm_base_add_listener(wd->xdg_wm_base, &xdg_wm_base_listener, wd);
        }
        else if (std::strcmp(interface, wl_shm_interface.name) == 0)
            wd->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        else if (std::strcmp(interface, wl_seat_interface.name) == 0)
            wd->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    }

    static void registry_handle_global_remove(void*, struct wl_registry*, uint32_t) {}

    WaylandDisplay()
    {
        display = wl_display_connect (nullptr);
        if (display != nullptr)
        {
            registry = wl_display_get_registry (display);

            static const struct wl_registry_listener registry_listener = {
                WaylandDisplay::registry_handle_global,
                WaylandDisplay::registry_handle_global_remove
            };

            wl_registry_add_listener (registry, &registry_listener, this);
            wl_display_roundtrip (display);

            int fd = wl_display_get_fd (display);
            LinuxEventLoop::registerFdCallback (fd, [this] (int) {
                wl_display_dispatch (display);
            }, 1 /* POLLIN */);
        }
    }

    ~WaylandDisplay()
    {
        if (display != nullptr)
        {
            LinuxEventLoop::unregisterFdCallback (wl_display_get_fd (display));
            if (xdg_wm_base != nullptr) xdg_wm_base_destroy (xdg_wm_base);
            if (shm != nullptr) wl_shm_destroy (shm);
            if (seat != nullptr) wl_seat_destroy (seat);
            if (compositor != nullptr) wl_compositor_destroy (compositor);
            wl_registry_destroy (registry);
            wl_display_disconnect (display);
        }
    }

    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    struct xdg_wm_base* xdg_wm_base = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;

    JUCE_DECLARE_SINGLETON_INLINE (WaylandDisplay, false)
};

//==============================================================================
static int createAnonymousFile (off_t size)
{
    int fd = fileno (tmpfile());
    if (fd >= 0) {
        if (ftruncate (fd, size) < 0) {
            close (fd);
            return -1;
        }
    }
    return fd;
}

class WaylandComponentPeer final : public ComponentPeer
{
public:
    WaylandComponentPeer (Component& comp, int windowStyleFlags, void* /*parentToAddTo*/)
        : ComponentPeer (comp, windowStyleFlags)
    {
        auto* wd = WaylandDisplay::getInstance();
        if (wd != nullptr && wd->display != nullptr)
        {
            surface = wl_compositor_create_surface (wd->compositor);
            xdgSurface = xdg_wm_base_get_xdg_surface (wd->xdg_wm_base, surface);
            xdgToplevel = xdg_surface_get_toplevel (xdgSurface);

            static const struct xdg_surface_listener surface_listener = {
                [] (void* data, struct xdg_surface* xdg_surf, uint32_t serial) {
                    xdg_surface_ack_configure (xdg_surf, serial);
                    auto* peer = static_cast<WaylandComponentPeer*> (data);
                    peer->configured = true;
                    peer->repaint (peer->bounds);
                }
            };
            xdg_surface_add_listener (xdgSurface, &surface_listener, this);

            static const struct xdg_toplevel_listener toplevel_listener = {
                [] (void* data, struct xdg_toplevel*, int32_t w, int32_t h, struct wl_array*) {
                    auto* peer = static_cast<WaylandComponentPeer*> (data);
                    if (w > 0 && h > 0)
                        peer->setBounds (Rectangle<int> (0, 0, w, h), false);
                },
                [] (void* data, struct xdg_toplevel*) {
                    auto* peer = static_cast<WaylandComponentPeer*> (data);
                    peer->handleUserClosingWindow();
                },
                [] (void*, struct xdg_toplevel*, int32_t, int32_t) {},
                [] (void*, struct xdg_toplevel*, struct wl_array*) {}
            };
            xdg_toplevel_add_listener (xdgToplevel, &toplevel_listener, this);

            wl_surface_commit (surface);
            wl_display_roundtrip (wd->display);
        }
    }

    ~WaylandComponentPeer() override
    {
        if (buffer != nullptr) wl_buffer_destroy (buffer);
        if (xdgToplevel != nullptr) xdg_toplevel_destroy (xdgToplevel);
        if (xdgSurface != nullptr) xdg_surface_destroy (xdgSurface);
        if (surface != nullptr) wl_surface_destroy (surface);
    }

    void* getNativeHandle() const override { return nullptr; }

    void setBounds (const Rectangle<int>& newBounds, bool isNowFullScreen) override
    {
        bounds = newBounds;
        fullScreen = isNowFullScreen;
        handleMovedOrResized();
    }

    Rectangle<int> getBounds() const override { return bounds; }
    Point<float> localToGlobal (Point<float> relativePosition) override { return relativePosition; }
    Point<float> globalToLocal (Point<float> screenPosition) override { return screenPosition; }

    void setVisible (bool) override {}
    void setTitle (const String&) override {}
    void setMinimised (bool) override {}
    bool isMinimised() const override { return false; }
    void setFullScreen (bool shouldBeFullScreen) override { fullScreen = shouldBeFullScreen; }
    bool isFullScreen() const override { return fullScreen; }
    bool contains (Point<int>, bool) const override { return true; }
    void toFront (bool) override {}
    void toBehind (ComponentPeer*) override {}
    bool isFocused() const override { return true; }
    void grabFocus() override {}
    void textInputRequired (Point<int>, TextInputTarget&) override {}

    void repaint (const Rectangle<int>&) override 
    {
        if (!configured || bounds.isEmpty() || WaylandDisplay::getInstance()->display == nullptr) return;

        int width = jmax (1, bounds.getWidth());
        int height = jmax (1, bounds.getHeight());
        int stride = width * 4;
        int size = stride * height;

        int fd = createAnonymousFile (size);
        if (fd < 0) return;

        void* data = mmap (nullptr, static_cast<size_t> (size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            close (fd);
            return;
        }

        // Fill with a visible color to prove it works (e.g. ARGB red)
        uint32_t* pixels = static_cast<uint32_t*> (data);
        for (int i = 0; i < width * height; ++i)
            pixels[i] = 0xFFFF0000;

        auto* pool = wl_shm_create_pool (WaylandDisplay::getInstance()->shm, fd, size);
        auto* new_buffer = wl_shm_pool_create_buffer (pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy (pool);

        munmap (data, static_cast<size_t> (size));
        close (fd);

        wl_surface_attach (surface, new_buffer, 0, 0);
        wl_surface_damage_buffer (surface, 0, 0, width, height);
        wl_surface_commit (surface);

        if (buffer) wl_buffer_destroy (buffer);
        buffer = new_buffer;
    }

    void performAnyPendingRepaintsNow() override {}
    void setAlpha (float) override {}
    bool setAlwaysOnTop (bool) override { return false; }
    void setIcon (const Image&) override {}

    bool isShowing() const override { return true; }
    OptionalBorderSize getFrameSizeIfPresent() const override { return {}; }
    BorderSize<int> getFrameSize() const override { return {}; }
    StringArray getAvailableRenderingEngines() override { return StringArray ("Software Renderer"); }

private:
    Rectangle<int> bounds {0, 0, 800, 600};
    bool fullScreen = false;
    bool configured = false;

    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* xdgToplevel = nullptr;
    wl_buffer* buffer = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaylandComponentPeer)
};

//==============================================================================
ComponentPeer* Component::createNewPeer (int styleFlags, void* nativeWindowToAttachTo)
{
    return new WaylandComponentPeer (*this, styleFlags, nativeWindowToAttachTo);
}

//==============================================================================
JUCE_API bool JUCE_CALLTYPE Process::isForegroundProcess()    { return true; }
JUCE_API void JUCE_CALLTYPE Process::makeForegroundProcess()  {}
JUCE_API void JUCE_CALLTYPE Process::hide()                   {}

//==============================================================================
void Desktop::setKioskComponent (Component*, bool, bool) {}
void Displays::findDisplays (const Desktop&) 
{
    Display d;
    d.isMain = true;
    d.totalArea = Rectangle<int> (0, 0, 1920, 1080);
    d.userArea = Rectangle<int> (0, 0, 1920, 1080);
    d.safeAreaInsets = BorderSize<int>();
    d.keyboardInsets = BorderSize<int>();
    d.topLeftPhysical = Point<int> (0, 0);
    d.scale = 1.0;
    d.dpi = 96.0; // Avoid Desktop::getInstance() here!
    d.verticalFrequencyHz = 60.0;
    displays.add (d);
    updateToLogical();
}

bool Desktop::canUseSemiTransparentWindows() noexcept { return true; }
struct Desktop::NativeDarkModeChangeDetectorImpl {};
std::unique_ptr<Desktop::NativeDarkModeChangeDetectorImpl> Desktop::createNativeDarkModeChangeDetectorImpl() { return nullptr; }
bool Desktop::isDarkModeActive() const { return true; }
void Desktop::setScreenSaverEnabled (bool) {}
bool Desktop::isScreenSaverEnabled() { return true; }
double Desktop::getDefaultMasterScale() { return 1.0; }
Desktop::DisplayOrientation Desktop::getCurrentOrientation() const { return upright; }
void Desktop::allowedOrientationsChanged() {}

//==============================================================================
bool detail::MouseInputSourceList::addSource()
{
    if (sources.isEmpty())
    {
        addSource (0, MouseInputSource::InputSourceType::mouse);
        return true;
    }
    return false;
}
bool detail::MouseInputSourceList::canUseTouch() const { return false; }
Point<float> MouseInputSource::getCurrentRawMousePosition() { return Point<float>(); }
void MouseInputSource::setRawMousePosition (Point<float>) {}

//==============================================================================
class MouseCursor::PlatformSpecificHandle
{
public:
    explicit PlatformSpecificHandle (const MouseCursor::StandardCursorType) {}
    explicit PlatformSpecificHandle (const detail::CustomMouseCursorInfo&) {}
    ~PlatformSpecificHandle() {}
    static void showInWindow (PlatformSpecificHandle*, ComponentPeer*) {}
private:
    JUCE_DECLARE_NON_COPYABLE (PlatformSpecificHandle)
    JUCE_DECLARE_NON_MOVEABLE (PlatformSpecificHandle)
};

//==============================================================================
bool DragAndDropContainer::performExternalDragDropOfFiles (const StringArray&, bool, Component*, std::function<void()>) { return false; }
bool DragAndDropContainer::performExternalDragDropOfText (const String&, Component*, std::function<void()>) { return false; }
void SystemClipboard::copyTextToClipboard (const String&) {}
String SystemClipboard::getTextFromClipboard() { return {}; }
bool KeyPress::isKeyCurrentlyDown (int) { return false; }
void LookAndFeel::playAlertSound() {}

//==============================================================================
namespace Keys { static constexpr int extendedKeyModifier = 0x10000000; }

// Use standard XKB keysym values (which are identical to X11 keysyms)
const int KeyPress::spaceKey              =  ' ';
const int KeyPress::returnKey             = 0x0d;
const int KeyPress::escapeKey             = 0x1b;
const int KeyPress::backspaceKey          = 0x08;
const int KeyPress::leftKey               = 0x51 | Keys::extendedKeyModifier;
const int KeyPress::rightKey              = 0x53 | Keys::extendedKeyModifier;
const int KeyPress::upKey                 = 0x52 | Keys::extendedKeyModifier;
const int KeyPress::downKey               = 0x54 | Keys::extendedKeyModifier;
const int KeyPress::pageUpKey             = 0x55 | Keys::extendedKeyModifier;
const int KeyPress::pageDownKey           = 0x56 | Keys::extendedKeyModifier;
const int KeyPress::endKey                = 0x57 | Keys::extendedKeyModifier;
const int KeyPress::homeKey               = 0x50 | Keys::extendedKeyModifier;
const int KeyPress::insertKey             = 0x63 | Keys::extendedKeyModifier;
const int KeyPress::deleteKey             = 0xff | Keys::extendedKeyModifier;
const int KeyPress::tabKey                = '\t';
const int KeyPress::F1Key                 = 0xbe | Keys::extendedKeyModifier;
const int KeyPress::F2Key                 = 0xbf | Keys::extendedKeyModifier;
const int KeyPress::F3Key                 = 0xc0 | Keys::extendedKeyModifier;
const int KeyPress::F4Key                 = 0xc1 | Keys::extendedKeyModifier;
const int KeyPress::F5Key                 = 0xc2 | Keys::extendedKeyModifier;
const int KeyPress::F6Key                 = 0xc3 | Keys::extendedKeyModifier;
const int KeyPress::F7Key                 = 0xc4 | Keys::extendedKeyModifier;
const int KeyPress::F8Key                 = 0xc5 | Keys::extendedKeyModifier;
const int KeyPress::F9Key                 = 0xc6 | Keys::extendedKeyModifier;
const int KeyPress::F10Key                = 0xc7 | Keys::extendedKeyModifier;
const int KeyPress::F11Key                = 0xc8 | Keys::extendedKeyModifier;
const int KeyPress::F12Key                = 0xc9 | Keys::extendedKeyModifier;
const int KeyPress::F13Key                = 0xca | Keys::extendedKeyModifier;
const int KeyPress::F14Key                = 0xcb | Keys::extendedKeyModifier;
const int KeyPress::F15Key                = 0xcc | Keys::extendedKeyModifier;
const int KeyPress::F16Key                = 0xcd | Keys::extendedKeyModifier;
const int KeyPress::F17Key                = 0xce | Keys::extendedKeyModifier;
const int KeyPress::F18Key                = 0xcf | Keys::extendedKeyModifier;
const int KeyPress::F19Key                = 0xd0 | Keys::extendedKeyModifier;
const int KeyPress::F20Key                = 0xd1 | Keys::extendedKeyModifier;
const int KeyPress::F21Key                = 0xd2 | Keys::extendedKeyModifier;
const int KeyPress::F22Key                = 0xd3 | Keys::extendedKeyModifier;
const int KeyPress::F23Key                = 0xd4 | Keys::extendedKeyModifier;
const int KeyPress::F24Key                = 0xd5 | Keys::extendedKeyModifier;
const int KeyPress::F25Key                = 0xd6 | Keys::extendedKeyModifier;
const int KeyPress::F26Key                = 0xd7 | Keys::extendedKeyModifier;
const int KeyPress::F27Key                = 0xd8 | Keys::extendedKeyModifier;
const int KeyPress::F28Key                = 0xd9 | Keys::extendedKeyModifier;
const int KeyPress::F29Key                = 0xda | Keys::extendedKeyModifier;
const int KeyPress::F30Key                = 0xdb | Keys::extendedKeyModifier;
const int KeyPress::F31Key                = 0xdc | Keys::extendedKeyModifier;
const int KeyPress::F32Key                = 0xdd | Keys::extendedKeyModifier;
const int KeyPress::F33Key                = 0xde | Keys::extendedKeyModifier;
const int KeyPress::F34Key                = 0xdf | Keys::extendedKeyModifier;
const int KeyPress::F35Key                = 0xe0 | Keys::extendedKeyModifier;
const int KeyPress::numberPad0            = 0xb0 | Keys::extendedKeyModifier;
const int KeyPress::numberPad1            = 0xb1 | Keys::extendedKeyModifier;
const int KeyPress::numberPad2            = 0xb2 | Keys::extendedKeyModifier;
const int KeyPress::numberPad3            = 0xb3 | Keys::extendedKeyModifier;
const int KeyPress::numberPad4            = 0xb4 | Keys::extendedKeyModifier;
const int KeyPress::numberPad5            = 0xb5 | Keys::extendedKeyModifier;
const int KeyPress::numberPad6            = 0xb6 | Keys::extendedKeyModifier;
const int KeyPress::numberPad7            = 0xb7 | Keys::extendedKeyModifier;
const int KeyPress::numberPad8            = 0xb8 | Keys::extendedKeyModifier;
const int KeyPress::numberPad9            = 0xb9 | Keys::extendedKeyModifier;
const int KeyPress::numberPadAdd          = 0xab | Keys::extendedKeyModifier;
const int KeyPress::numberPadSubtract     = 0xad | Keys::extendedKeyModifier;
const int KeyPress::numberPadMultiply     = 0xaa | Keys::extendedKeyModifier;
const int KeyPress::numberPadDivide       = 0xaf | Keys::extendedKeyModifier;
const int KeyPress::numberPadSeparator    = 0xac | Keys::extendedKeyModifier;
const int KeyPress::numberPadDecimalPoint = 0xae | Keys::extendedKeyModifier;
const int KeyPress::numberPadEquals       = 0xbd | Keys::extendedKeyModifier;
const int KeyPress::numberPadDelete       = 0x9f | Keys::extendedKeyModifier;
const int KeyPress::playKey               = ((int) 0xffeeff00)       | Keys::extendedKeyModifier;
const int KeyPress::stopKey               = ((int) 0xffeeff01)       | Keys::extendedKeyModifier;
const int KeyPress::fastForwardKey        = ((int) 0xffeeff02)       | Keys::extendedKeyModifier;
const int KeyPress::rewindKey             = ((int) 0xffeeff03)       | Keys::extendedKeyModifier;

extern "C" __attribute__ ((visibility ("default"))) int juce_gtkWebkitMain (int, const char* const*) { return 0; }
Image detail::WindowingHelpers::createIconForFile (const File&) { return {}; }
void juce_LinuxAddRepaintListener (ComponentPeer*, Component*);
void juce_LinuxAddRepaintListener (ComponentPeer*, Component*) {}
void juce_LinuxRemoveRepaintListener (ComponentPeer*, Component*);
void juce_LinuxRemoveRepaintListener (ComponentPeer*, Component*) {}

} // namespace juce
