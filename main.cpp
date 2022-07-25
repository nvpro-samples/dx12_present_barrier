// Copyright 2020-2021 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0

#include <RenderThread.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

#include <optional>

class Sample : public nvh::AppWindowProfiler, private WindowCallback
{
public:
  Sample();

  bool begin() override;
  void think(double time) override;
  void end() override;

  void contextInit() override {}
  void contextSync() override {}
  void contextDeinit() override {}

  void swapPrepare() override {}
  void swapBuffers() override {}

  void swapVsync(bool state) override;

private:
  struct PosAndSize
  {
    int x, y, width, height;
  };

  RenderThread              m_renderThread;
  Configuration             m_initialConfig;
  bool                      m_showCursor = true;
  std::mutex                m_mutex;
  std::optional<bool>       m_windowDecorated;
  std::optional<PosAndSize> m_windowPosAndSize;

  HWND        getWindowHandle() override { return glfwGetWin32Window(m_internal); }
  GLFWwindow* getGlfwWindow() override { return m_internal; }
  void        setDecorated(bool decorated) override;
  void        setPosAndSize(int x, int y, int width, int height) override;
};

Sample::Sample()
{
  // Register various command-line options to change behavior of this sample
  m_parameterList.add("output|Monitor index to render on", &m_initialConfig.m_outputIndex);
  m_parameterList.add("o|Same as -output", &m_initialConfig.m_outputIndex);
  m_parameterList.add(
      "displaymode|Select the startup display mode: (b)orderless (default), (f)ullscreen, or (w)indowed.",
      &m_initialConfig.m_startupDisplayMode);
  m_parameterList.add("dm|Same as -displaymode", &m_initialConfig.m_startupDisplayMode);
  m_parameterList.add("adapter|Adapter index to render on", &m_renderThread.contextInfo().compatibleAdapterIndex);
  m_parameterList.add("a|Same as -adapter", &m_renderThread.contextInfo().compatibleAdapterIndex);
  m_parameterList.add("afr|Alternate frame rendering when SLI is enabled", &m_initialConfig.m_alternateFrameRendering);
  m_parameterList.add("listadapters|Print available adapters", &m_renderThread.contextInfo().verboseCompatibleAdapters);
  m_parameterList.add("stereo|Stereoscopic rendering", &m_initialConfig.m_stereo);

  m_parameterList.add("lines|Set number of scrolling lines to show", &m_initialConfig.m_numLines);
  m_parameterList.add("linesize|Size of the scrolling lines in pixels (first value is main size, second for variation)",
                      m_initialConfig.m_lineSizeInPixels, nullptr, 2);
  m_parameterList.add("linespeed|Speed of the scrolling lines in pixels per frame", &m_initialConfig.m_lineSpeedInPixels);
  m_parameterList.add("verticallines|Show vertical scrolling lines", &m_initialConfig.m_showVerticalLines);
  m_parameterList.add("horizontallines|Show horizontal scrolling lines", &m_initialConfig.m_showHorizontalLines);

  m_parameterList.add("cursor|Show or hide mouse cursor of the operating system", &m_showCursor);
  m_parameterList.add("sleepinterval|Specifies a sleep interval in milliseconds that is added between present calls",
                      &m_initialConfig.m_sleepIntervalInMilliseconds);
  m_parameterList.add(
      "synctimeout|Specifies a sync timeout in milliseconds that is used when waiting for all gpu work to finish (e.g. "
      "when transitioning display modes or toggling present barrier, default: 1000",
      &m_initialConfig.m_syncTimeoutMillis);
  m_parameterList.add(
      "testmode|Start app in test mode: (n)o test mode (default), (i)flipflipex transition: simulates windows key "
      "presses in fixed intervals, (f)ullscreen transition: transitions between fullscreen and windowed in fixed "
      "intervals, (b)orderless transition: transitions between borderless and windowed in fixed intervals",
      &m_initialConfig.m_testMode);
  m_parameterList.add("t|Same as -testmode", &m_initialConfig.m_testMode);
  m_parameterList.add("testmodeinterval|The framecount interval for -testmode, default: 120", &m_initialConfig.m_testModeInterval);
  m_parameterList.add("framecounterfile|Frame counters of the Quadro Sync device will be logged into this file.",
                      &m_initialConfig.m_frameCounterFilePath);
}

bool Sample::begin()
{
  // Hide cursor when requested
  if(!m_showCursor)
  {
    ShowCursor(FALSE);
  }

  LOGI(
      "\n"
      "Keyboard shortcuts:\n"
      " V          - Toggle vsync\n"
      " S          - Toggle scrolling of the lines\n"
      " T          - Toggle present barrier\n"
      " Q          - Toggle usage of the Quadro Sync frame counter\n"
      " R          - Reset frame counter (only works from timing server)\n"
      " W          - Increase sleep interval between presets by 1ms\n"
      " Alt + W    - Reset sleep interval between presents to zero (effectively disabling it)\n"
      " Shift + W  - Decrease sleep interval between presents by 1ms\n"
      //" F          - Toggle fullscreen mode\n"
      //" B          - Toggle borderless window fullscreen mode\n"
      " 2          - Toggle stereoscopic rendering\n"
      "\n"
      "The bar at the top of the window indicates the present barrier status:\n"
      " red        - The swap chain is not in present barrier sync\n"
      " yellow     - The swap chain is in present barrier sync with other clients on the local system\n"
      " green      - The swap chain is in present barrier sync across systems through framelock\n"
      "\n");
  return m_renderThread.start(m_initialConfig, this, getWidth(), getHeight());
}

void Sample::think(double time)
{
  // Handle keyboard shortcuts that affect state
  if(m_windowState.onPress(KEY_W))
  {
    if(m_windowState.m_keyPressed[KEY_LEFT_ALT])
    {
      m_renderThread.setSleepInterval(0);
    }
    else if(m_windowState.m_keyPressed[KEY_LEFT_SHIFT])
    {
      m_renderThread.changeSleepInterval(-1);
    }
    else
    {
      m_renderThread.changeSleepInterval(1);
    }
  }
  if(m_windowState.onPress(KEY_2))
  {
    m_renderThread.toggleStereo();
  }

  if(m_windowState.onPress(KEY_F))
  {
    //m_renderThread.requestFullscreenStateChange();
  }
  if(m_windowState.onPress(KEY_B))
  {
    //m_renderThread.requestBorderlessStateChange();
  }
  if(m_windowState.onPress(KEY_S))
  {
    m_renderThread.toggleScrolling();
  }
  if(m_windowState.onPress(KEY_Q))
  {
    m_renderThread.toggleQuadroSync();
  }
  if(m_windowState.onPress(KEY_R))
  {
    m_renderThread.requestResetFrameCount();
  }
  if(m_windowState.onPress(KEY_T) && !m_renderThread.requestPresentBarrierChange(1000))
  {
    m_renderThread.forcePresentBarrierChange();
  }
  {
    std::lock_guard guard(m_mutex);
    if(m_windowDecorated.has_value())
    {
      glfwSetWindowAttrib(m_internal, GLFW_DECORATED, m_windowDecorated.value() ? 1 : 0);
      m_windowDecorated.reset();
    }
    if(m_windowPosAndSize.has_value())
    {
      glfwSetWindowPos(m_internal, m_windowPosAndSize.value().x, m_windowPosAndSize.value().y);
      glfwSetWindowSize(m_internal, m_windowPosAndSize.value().width, m_windowPosAndSize.value().height);
      m_windowPosAndSize.reset();
    }
  }
  //Sleep(100);
}

void Sample::swapVsync(bool state)
{
  m_renderThread.setVsync(state);
}

void Sample::end()
{
  // Show cursor again when it was hidden
  if(!m_showCursor)
  {
    ShowCursor(TRUE);
  }
  m_renderThread.interruptAndJoin();
}

void Sample::setDecorated(bool decorated)
{
  std::lock_guard guard(m_mutex);
  m_windowDecorated = decorated;
}

void Sample::setPosAndSize(int x, int y, int width, int height)
{
  std::lock_guard guard(m_mutex);
  m_windowPosAndSize = {x, y, width, height};
}

int main(int argc, const char** argv)
{
  NVPSystem system(PROJECT_NAME);

  Sample sample;
  return sample.run(PROJECT_NAME, argc, argv, SAMPLE_WINDOWED_WIDTH, SAMPLE_WINDOWED_HEIGHT, false);
}