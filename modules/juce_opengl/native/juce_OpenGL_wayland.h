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

namespace juce {

class OpenGLContext::NativeContext {
public:
  NativeContext(Component &, const OpenGLPixelFormat &, void *, bool,
                OpenGLVersion) {}
  ~NativeContext() {}

  InitResult initialiseOnRenderThread(OpenGLContext &) {
    return InitResult::success;
  }
  void shutdownOnRenderThread() {}
  bool makeActive() const noexcept { return false; }
  bool isActive() const noexcept { return false; }
  static void deactivateCurrentContext() {}
  void swapBuffers() {}
  void updateWindowPosition(Rectangle<int>) {}
  bool setSwapInterval(int) { return false; }
  int getSwapInterval() const { return 0; }
  bool createdOk() const noexcept { return false; }
  void *getRawContext() const noexcept { return nullptr; }
  GLuint getFrameBufferID() const noexcept { return 0; }

  struct Locker {
    Locker(NativeContext &) {}
  };
  void addListener(NativeContextListener &) {}
  void removeListener(NativeContextListener &) {}

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NativeContext)
};

bool OpenGLHelpers::isContextActive() { return false; }

} // namespace juce
