// Copyright 2020-2021 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <d3dx12.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

#include <nvh/fileoperations.hpp>
#include <nvh/appwindowprofiler.hpp>
#include <nvdx12/base_dx12.hpp>
#include <nvdx12/context_dx12.hpp>

#include <nvapi.h>

enum class DisplayMode
{
  WINDOWED,
  BORDERLESS,
  FULLSCREEN,
};

struct Configuration
{
  std::string   m_startupDisplayMode          = "b";
  std::string   m_testMode                    = "n";
  std::string   m_frameCounterFilePath        = "";
  bool          m_disablePresentBarrier       = false;
  bool          m_stereo                      = false;
  bool          m_showVerticalLines           = true;
  bool          m_showHorizontalLines         = true;
  bool          m_scrolling                   = true;
  bool          m_quadroSync                  = false;
  std::uint32_t m_testModeInterval            = 120;
  std::uint32_t m_numLines                    = 4;
  std::uint32_t m_lineSpeedInPixels           = 1;
  std::uint32_t m_sleepIntervalInMilliseconds = 0;
  std::uint32_t m_lineSizeInPixels[2]         = {1, 54};
  std::uint32_t m_syncTimeoutMillis           = 1000;
  std::int32_t  m_outputIndex                 = -1;
  std::uint32_t m_winSize[2];
};

// window attributes can only be changed from window-owning thread
class WindowCallback
{
public:
  virtual void               setDecorated(bool decorated)                       = 0;
  virtual void               setPosAndSize(int x, int y, int width, int height) = 0;
  virtual HWND               getWindowHandle()                                  = 0;
  virtual struct GLFWwindow* getGlfwWindow()                                    = 0;
};

class RenderThread
{
public:
  RenderThread();
  ~RenderThread() {}

  bool        start(Configuration const& initialConfig, WindowCallback* windowCallback);
  void        togglePaused();
  void        interruptAndJoin();
  DisplayMode trySetDisplayMode(DisplayMode displayMode);
  void        setSleepInterval(std::uint32_t millis);
  void        changeSleepInterval(std::int32_t deltaMillis);
  void        toggleStereo();
  void        toggleQuadroSync();
  void        setVsync(bool enabled);
  void        requestBorderlessStateChange();
  void        requestFullscreenStateChange();
  bool        requestPresentBarrierChange(std::uint32_t maxWaitMillis);
  void        requestResetFrameCount();
  void        forcePresentBarrierChange();

  nvdx12::ContextCreateInfo& contextInfo() { return m_contextInfo; }

private:
  enum class Status
  {
    CREATED,
    INITIAZATION_ERROR,
    RUNNING,
    PAUSED,
    INTERRUPTED,
  };

  std::thread             m_thread;
  Status                  m_status = Status::CREATED;
  std::mutex              m_mutex;
  std::condition_variable m_conVar;
  WindowCallback*         m_windowCallback = nullptr;

  Configuration m_config;
  NvU32         m_linesPosOffset         = 0;
  bool          m_requestToggleStereo    = false;
  bool          m_requestResetFrameCount = false;
  bool          m_skipNextSwap           = false;
  std::ofstream m_frameCounterFile;

  nvdx12::ContextCreateInfo m_contextInfo;
  nvdx12::Context           m_context;

  ComPtr<ID3D12Fence> m_presentBarrierFence;
  ComPtr<ID3D12Fence> m_frameFence;
  ComPtr<ID3D12Fence> m_guiFence;
  UINT64              m_frameIdx = 0;
  HANDLE              m_syncEvt  = NULL;

  ComPtr<IDXGISwapChain3>             m_swapChain;
  std::vector<ComPtr<ID3D12Resource>> m_backBufferResources;
  ComPtr<ID3D12Resource>              m_guiTexture;
  ComPtr<ID3D12DescriptorHeap>        m_rtvHeap;
  ComPtr<ID3D12DescriptorHeap>        m_cbvSrvUavHeap;

  ComPtr<ID3D12GraphicsCommandList>           m_graphicsCommandList;
  std::vector<ComPtr<ID3D12CommandAllocator>> m_graphicsCommandAllocators;
  ComPtr<ID3D12GraphicsCommandList>           m_guiCommandList;
  std::vector<ComPtr<ID3D12CommandAllocator>> m_guiCommandAllocators;
  std::vector<UINT64>                         m_allocatorFrameIndices;

  NvPresentBarrierClientHandle m_presentBarrierClient = nullptr;

  ComPtr<ID3D12PipelineState> m_linesPipeline;
  ComPtr<ID3D12PipelineState> m_indicatorPipeline;
  ComPtr<ID3D12PipelineState> m_guiPipeline;
  ComPtr<ID3D12RootSignature> m_rootSignature;

  DisplayMode                         m_displayMode                   = DisplayMode::WINDOWED;
  DisplayMode                         m_requestedDisplayMode          = DisplayMode::WINDOWED;
  bool                                m_presentBarrierChangeRequested = false;
  bool                                m_presentBarrierJoined          = false;
  NvU32                               m_frameCount                    = 0;
  NvU32                               m_syncInterval                  = 0;
  NV_PRESENT_BARRIER_FRAME_STATISTICS m_presentBarrierFrameStats      = {};

  bool isInterrupted();

  bool init(unsigned int initialWidth, unsigned int initialHeight);
  void setStatus(Status newStatus);
  void waitIfPaused();
  void renderFrame();
  void swapResize(int width, int height, bool stereo, bool force);
  void swapBuffers();
  bool sync();
  void end();
  void releasePresentBarrier();

  void drawLines(ComPtr<ID3D12GraphicsCommandList> commandList, uint32_t offset = 0);
  void drawSyncIndicator(ComPtr<ID3D12GraphicsCommandList> commandList);
  void drawGui(ComPtr<ID3D12GraphicsCommandList> commandList);
  void prepareGui();
};
