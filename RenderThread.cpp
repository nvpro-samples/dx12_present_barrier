// Copyright 2020-2021 NVIDIA Corporation
// SPDX-License-Identifier: Apache-2.0

#include <RenderThread.h>
#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_glfw.h>
#include <nvdx12/error_dx12.hpp>

#ifndef NDEBUG
#define CHECK_NV(status)                                                                                               \
  do                                                                                                                   \
  {                                                                                                                    \
    auto _ret = status;                                                                                                \
    if(_ret != NVAPI_OK)                                                                                               \
    {                                                                                                                  \
      std::stringstream log;                                                                                           \
      log << _ret << ": " << #status << std::endl;                                                                     \
      LOGE(log.str().c_str());                                                                                         \
    }                                                                                                                  \
    assert(_ret == NVAPI_OK);                                                                                          \
  } while(0)
#else
#define CHECK_NV(status) status
#endif

//#define DISABLE_PRESENT_BARRIER

#define BACK_BUFFER_FORMAT DXGI_FORMAT_R8G8B8A8_UNORM

RenderThread::RenderThread()
    : m_interrupted(false)
{
}

bool RenderThread::start(Configuration const& initialConfig, WindowCallback* windowCallback, unsigned int initialWidth, unsigned int initialHeight)
{
  m_config         = initialConfig;
  m_windowCallback = windowCallback;

  bool             success = false;
  std::unique_lock lock(m_mutex);

  m_thread = std::thread([this, &success, initialWidth, initialHeight]() {
    {
      std::unique_lock lock(m_mutex);
      success = init(initialWidth, initialHeight);
      m_conVar.notify_all();
    }
    if(success)
    {
      run();
    }
  });

  m_conVar.wait(lock);
  return success;
}

void RenderThread::run()
{
  while(!isInterrupted())
  {
    renderFrame();
    swapBuffers();
  }
  sync();
  end();
}

void RenderThread::interruptAndJoin()
{
  {
    std::lock_guard guard(m_mutex);
    m_interrupted = true;
    if(m_displayMode == DisplayMode::FULLSCREEN)
    {
      setDisplayMode(DisplayMode::WINDOWED);
    }
  }
  m_thread.join();
}

bool RenderThread::isInterrupted()
{
  std::lock_guard guard(m_mutex);
  return m_interrupted;
}

bool RenderThread::init(unsigned int initialWidth, unsigned int initialHeight)
{

  if(m_config.m_testMode == "f" && m_config.m_startupDisplayMode == "b")
  {
    LOGE("Display mode must not be borderless when using fullscreen transition test mode.");
    return false;
  }
  else if(m_config.m_testMode == "b" && m_config.m_startupDisplayMode == "f")
  {
    LOGE("Display mode must not be fullscreen when using borderless transition test mode.");
    return false;
  }
  else if(m_config.m_testMode != "n" && m_config.m_testMode != "f" && m_config.m_testMode != "b"
          && m_config.m_testMode != "i")
  {
    LOGE("Test mode must be n, f, b, or i.");
    return false;
  }
  if(m_config.m_testModeInterval <= 1)
  {
    LOGE("Test mode interval must be greater than 1.");
    return false;
  }

  if(!m_config.m_frameCounterFilePath.empty())
  {
    m_frameCounterFile.open(m_config.m_frameCounterFilePath);
  }

  // Create device
  if(!m_context.init(m_contextInfo))
  {
    return false;
  }

  CHECK_NV(NvAPI_Initialize());
#ifndef DISABLE_PRESENT_BARRIER
  // Check whether the system supports present barrier (Quadro + driver with support)
  bool presentBarrierSupported = false;
  if(NvAPI_D3D12_QueryPresentBarrierSupport(m_context.m_device, &presentBarrierSupported) != NVAPI_OK || !presentBarrierSupported)
  {
    LOGE("Present barrier is not supported on this system\n");
    return false;
  }
#endif

  const UINT nodeCount   = m_context.m_device->GetNodeCount();
  const UINT nodeMaskAll = (1 << nodeCount) - 1;

  // Create command queue for each node in the device
  m_commandQueues.push_back(m_context.m_commandQueue);
  m_context.m_commandQueue->AddRef();
  if(m_config.m_alternateFrameRendering)
  {
    for(UINT i = 1; i < nodeCount; ++i)
    {
      D3D12_COMMAND_QUEUE_DESC desc;
      desc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
      desc.Priority = 0;
      desc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
      desc.NodeMask = 1 << i;

      ComPtr<ID3D12CommandQueue> queue;
      HR_CHECK(m_context.m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)));
      m_commandQueues.push_back(queue);
    }
  }

  // Create fence and event used for context synchronization
  HR_CHECK(m_context.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_frameFence)));
  HR_CHECK(m_context.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_presentBarrierFence)));
  HR_CHECK(m_context.m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_guiFence)));
  m_syncEvt = CreateEventA(NULL, FALSE, FALSE, "SyncEvent");
  if(m_syncEvt == NULL)
  {
    HR_CHECK(HRESULT_FROM_WIN32(GetLastError()));
  }

  // Create descriptor heaps
  m_rtvHeaps.resize(nodeCount, nullptr);
  m_cbvSrvUavHeaps.resize(nodeCount, nullptr);
  for(UINT i = 0; i < nodeCount; ++i)
  {
    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
    descriptorHeapDesc.Type                       = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    descriptorHeapDesc.NumDescriptors             = D3D12_SWAP_CHAIN_SIZE * 2 + 1;
    descriptorHeapDesc.Flags                      = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    descriptorHeapDesc.NodeMask                   = 1 << i;
    HR_CHECK(m_context.m_device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_rtvHeaps[i])));

    descriptorHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeapDesc.NumDescriptors = i == 0 ? 2 : 1;
    descriptorHeapDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR_CHECK(m_context.m_device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_cbvSrvUavHeaps[i])));
  }

  // Create swap chain
  swapResize(initialWidth, initialHeight, m_config.m_stereo, true);

  // Load compiled shader binaries from disk
  std::vector<std::string> searchDirectories;
  searchDirectories.push_back(NVPSystem::exePath());
  searchDirectories.push_back(NVPSystem::exePath() + PROJECT_RELDIRECTORY);
  searchDirectories.push_back(PROJECT_NAME);

  std::string lineVsData      = nvh::loadFile("line_vs.cso", true, searchDirectories);
  std::string indicatorVsData = nvh::loadFile("indicator_vs.cso", true, searchDirectories);
  std::string psData          = nvh::loadFile("ps.cso", true, searchDirectories);
  std::string guiVsData       = nvh::loadFile("gui_vs.cso", true, searchDirectories);
  std::string guiPsData       = nvh::loadFile("gui_ps.cso", true, searchDirectories);
  if(lineVsData.empty() || indicatorVsData.empty() || psData.empty() || guiPsData.empty())
  {
    LOGE(
        "Could not load required shader binaries 'line_vs.cso', 'indicator_vs.cso', 'ps.cso', gui_vs.cso, and "
        "gui_ps.cso\n");
    return false;
  }

  // Create command allocators and a single list which will be re-used every frame
  m_commandAllocators.resize(m_backBufferResources.size(), nullptr);
  m_guiCommandAllocators.resize(m_backBufferResources.size(), nullptr);
  m_allocatorFrameIndices.resize(m_backBufferResources.size(), 0);
  for(UINT i = 0; i < m_commandAllocators.size(); ++i)
  {
    HR_CHECK(m_context.m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocators[i])));
    HR_CHECK(m_context.m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_guiCommandAllocators[i])));
  }

  m_commandLists.resize(nodeCount);
  ID3D12Device4* device4 = nullptr;
  HR_CHECK(m_context.m_device->QueryInterface(&device4));
  for(UINT node = 0; node < nodeCount; ++node)
  {
    HR_CHECK(device4->CreateCommandList1(1 << node, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                         IID_PPV_ARGS(&m_commandLists[node])));
  }
  HR_CHECK(device4->CreateCommandList1(1, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE,
                                       IID_PPV_ARGS(&m_guiCommandList)));

  // Create root signature for line rendering shaders
  CD3DX12_DESCRIPTOR_RANGE guiDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
  CD3DX12_ROOT_PARAMETER   rootParameters[2] = {};
  rootParameters[0].InitAsConstants(11, 0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
  rootParameters[1].InitAsDescriptorTable(1, &guiDescriptorRange, D3D12_SHADER_VISIBILITY_PIXEL);

  CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);

  ComPtr<ID3DBlob> rootSignatureBlob;
  HR_CHECK(D3D12SerializeVersionedRootSignature(&rootSignatureDesc, &rootSignatureBlob, nullptr));
  HR_CHECK(m_context.m_device->CreateRootSignature(nodeMaskAll, rootSignatureBlob->GetBufferPointer(),
                                                   rootSignatureBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));

  // Create graphics pipeline for line rendering (using simple quads rendered from triangle strips)
  struct PipelineStateDesc
  {
    CD3DX12_PIPELINE_STATE_STREAM_ROOT_SIGNATURE        m_rootSig;
    CD3DX12_PIPELINE_STATE_STREAM_VS                    m_vs;
    CD3DX12_PIPELINE_STATE_STREAM_PS                    m_ps;
    CD3DX12_PIPELINE_STATE_STREAM_RASTERIZER            m_rasterizer;
    CD3DX12_PIPELINE_STATE_STREAM_BLEND_DESC            m_blendDesc;
    CD3DX12_PIPELINE_STATE_STREAM_DEPTH_STENCIL1        m_depthStencil;
    CD3DX12_PIPELINE_STATE_STREAM_PRIMITIVE_TOPOLOGY    m_primitiveTopology;
    CD3DX12_PIPELINE_STATE_STREAM_RENDER_TARGET_FORMATS m_renderTargets;
    CD3DX12_PIPELINE_STATE_STREAM_NODE_MASK             m_nodeMask;
  } pipelineStateDesc;
  D3D12_PIPELINE_STATE_STREAM_DESC pipelineStateStreamDesc = {sizeof(PipelineStateDesc), &pipelineStateDesc};

  CD3DX12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(CD3DX12_DEFAULT());
  rasterizerDesc.CullMode                = D3D12_CULL_MODE_NONE;

  CD3DX12_DEPTH_STENCIL_DESC1 depthStencilDesc = CD3DX12_DEPTH_STENCIL_DESC1(CD3DX12_DEFAULT());
  depthStencilDesc.DepthEnable                 = FALSE;

  D3D12_RT_FORMAT_ARRAY renderTargets = {};
  renderTargets.NumRenderTargets      = 1;
  renderTargets.RTFormats[0]          = BACK_BUFFER_FORMAT;

  pipelineStateDesc.m_rootSig           = m_rootSignature.Get();
  pipelineStateDesc.m_vs                = CD3DX12_SHADER_BYTECODE(lineVsData.data(), lineVsData.size());
  pipelineStateDesc.m_ps                = CD3DX12_SHADER_BYTECODE(psData.data(), psData.size());
  pipelineStateDesc.m_rasterizer        = rasterizerDesc;
  pipelineStateDesc.m_blendDesc         = CD3DX12_BLEND_DESC((CD3DX12_DEFAULT()));
  pipelineStateDesc.m_depthStencil      = depthStencilDesc;
  pipelineStateDesc.m_primitiveTopology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipelineStateDesc.m_renderTargets     = renderTargets;
  pipelineStateDesc.m_nodeMask          = nodeMaskAll;
  HR_CHECK(device4->CreatePipelineState(&pipelineStateStreamDesc, IID_PPV_ARGS(&m_linesPipeline)));

  // Create graphics pipeline for present barrier status indicator
  pipelineStateDesc.m_vs = CD3DX12_SHADER_BYTECODE(indicatorVsData.data(), indicatorVsData.size());
  HR_CHECK(device4->CreatePipelineState(&pipelineStateStreamDesc, IID_PPV_ARGS(&m_indicatorPipeline)));

  // create graphics pipeline for gui rendering
  CD3DX12_BLEND_DESC guiBlendDesc          = CD3DX12_BLEND_DESC(CD3DX12_DEFAULT());
  guiBlendDesc.RenderTarget[0].BlendEnable = TRUE;
  guiBlendDesc.RenderTarget[0].SrcBlend    = D3D12_BLEND_SRC_ALPHA;
  guiBlendDesc.RenderTarget[0].DestBlend   = D3D12_BLEND_INV_SRC_ALPHA;

  pipelineStateDesc.m_vs        = CD3DX12_SHADER_BYTECODE(guiVsData.data(), guiVsData.size());
  pipelineStateDesc.m_ps        = CD3DX12_SHADER_BYTECODE(guiPsData.data(), guiPsData.size());
  pipelineStateDesc.m_blendDesc = guiBlendDesc;
  HR_CHECK(device4->CreatePipelineState(&pipelineStateStreamDesc, IID_PPV_ARGS(&m_guiPipeline)));

  device4->Release();

  // Switch into fullscreen (which is required for present barrier to work)
  if(m_config.m_startupDisplayMode == "b" || m_config.m_startupDisplayMode == "borderless")
  {
    setDisplayMode(DisplayMode::BORDERLESS);
  }
  else if(m_config.m_startupDisplayMode == "f" || m_config.m_startupDisplayMode == "fullscreen")
  {
    setDisplayMode(DisplayMode::FULLSCREEN);
  }
  else if(m_config.m_startupDisplayMode != "w" && m_config.m_startupDisplayMode != "windowed")
  {
    LOGE("Display mode argument must be (b)orderless, (f)ullscreen, or (w)indowed.");
    return false;
  }
  forcePresentBarrierChange();
  m_presentBarrierFrameStats.dwVersion = NV_PRESENT_BARRIER_FRAME_STATICS_VER1;


  IMGUI_CHECKVERSION();
  if(!ImGui::CreateContext())
  {
    LOGE("ImGui::CreateContext() failed.\n");
    return false;
  }
  ImGui::StyleColorsDark();
  if(!ImGui_ImplGlfw_InitForOther(m_windowCallback->getGlfwWindow(), true))
  {
    LOGE("ImGui_ImplGlfw_InitForOther() failed.\n");
    return false;
  }
  auto guiCpuHandle = m_cbvSrvUavHeaps.front()->GetCPUDescriptorHandleForHeapStart();
  guiCpuHandle.ptr += m_context.m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto guiGpuHandle = m_cbvSrvUavHeaps.front()->GetGPUDescriptorHandleForHeapStart();
  guiGpuHandle.ptr += m_context.m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  if(!ImGui_ImplDX12_Init(m_context.m_device, static_cast<int>(m_commandAllocators.size()), BACK_BUFFER_FORMAT,
                          m_cbvSrvUavHeaps.front().Get(), guiCpuHandle, guiGpuHandle))
  {
    LOGE("ImGui_ImplDX12_Init() failed.\n");
    return false;
  }
  return true;
}

void RenderThread::renderFrame()
{
  if(m_frameCount % m_config.m_testModeInterval < 2 && m_config.m_testMode[0] != 'n')
  {
    INPUT input      = {};
    input.type       = INPUT_KEYBOARD;
    input.ki.dwFlags = m_frameCount % m_config.m_testModeInterval == 1 ? KEYEVENTF_KEYUP : 0;
    switch(m_config.m_testMode[0])
    {
      case 'i':
        input.ki.wVk = VK_LWIN;
        break;
      case 'f':
        input.ki.wVk = 'F';
        break;
      case 'b':
        input.ki.wVk = 'B';
        break;
    }

    if(SendInput(1, &input, sizeof(INPUT)) != 1)
    {
      HR_CHECK(HRESULT_FROM_WIN32(GetLastError()));
    }
  }

  if(!m_config.m_quadroSync)
  {
    m_frameCount++;
  }

  m_mutex.lock();
  std::uint32_t sleepIntervalMillis = m_config.m_sleepIntervalInMilliseconds;
  m_mutex.unlock();
  if(sleepIntervalMillis != 0)
  {
    Sleep(sleepIntervalMillis);
  }

  // wait for command allocator to finish its execution
  auto waitForFrameIdx = m_allocatorFrameIndices[m_swapChain->GetCurrentBackBufferIndex()];
  if(m_frameFence->GetCompletedValue() < waitForFrameIdx)
  {
    //auto begin = std::chrono::high_resolution_clock::now();
    if(!m_skipNextSwap)
    {
      m_frameFence->SetEventOnCompletion(waitForFrameIdx, m_syncEvt);
    }
    switch(WaitForSingleObject(m_syncEvt, m_config.m_syncTimeoutMillis))
    {
      case WAIT_OBJECT_0:
        break;
      case WAIT_TIMEOUT:
        LOGE("Wait for frame %d to finish timed out.\n", waitForFrameIdx);
        m_skipNextSwap = true;
        return;
      default:
        HR_CHECK(HRESULT_FROM_WIN32(GetLastError()));
    }
    //auto end = std::chrono::high_resolution_clock::now();
    //LOGI("frame %d: waited for %.3f ms.\n", m_frameIdx, std::chrono::duration<float, std::milli>(end - begin).count());
  }
  m_skipNextSwap = false;

  std::uint32_t currentNodeIdx = m_swapChain->GetCurrentBackBufferIndex() % m_context.m_device->GetNodeCount();

  // Begin recording command list
  ComPtr<ID3D12CommandAllocator>    commandAllocator = m_commandAllocators[m_swapChain->GetCurrentBackBufferIndex()];
  ComPtr<ID3D12GraphicsCommandList> commandList      = m_commandLists[currentNodeIdx];
  HR_CHECK(commandAllocator->Reset());
  HR_CHECK(commandList->Reset(commandAllocator.Get(), m_linesPipeline.Get()));
  auto cbvSrvUavHeap = m_cbvSrvUavHeaps[currentNodeIdx].Get();
  commandList->SetDescriptorHeaps(1, &cbvSrvUavHeap);

  ComPtr<ID3D12CommandAllocator> guiCommandAllocator = m_guiCommandAllocators[m_swapChain->GetCurrentBackBufferIndex()];
  HR_CHECK(guiCommandAllocator->Reset());
  HR_CHECK(m_guiCommandList->Reset(guiCommandAllocator.Get(), nullptr));
  prepareGui();

  ComPtr<ID3D12Resource> currentBackBuffer = m_backBufferResources[m_swapChain->GetCurrentBackBufferIndex()];

  const D3D12_RESOURCE_BARRIER presentToRenderTarget =
      nvdx12::transitionBarrier(currentBackBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
  commandList->ResourceBarrier(1, &presentToRenderTarget);

  const UINT rtvIndex     = m_config.m_alternateFrameRendering ?
                                m_swapChain->GetCurrentBackBufferIndex() / m_context.m_device->GetNodeCount() :
                                m_swapChain->GetCurrentBackBufferIndex();
  const UINT rtvIncrement = m_context.m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  // Clear background to black
  const float clearColor[4] = {0, 0, 0, 1};
  {
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeaps[currentNodeIdx]->GetCPUDescriptorHandleForHeapStart(), rtvIndex, rtvIncrement);
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
  }
  if(m_config.m_stereo)
  {
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeaps[currentNodeIdx]->GetCPUDescriptorHandleForHeapStart(),
                                            D3D12_SWAP_CHAIN_SIZE + rtvIndex, rtvIncrement);
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
  }

  // Draw scrolling lines and a simple present barrier status indicator bar to the window
  DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
  m_swapChain->GetDesc1(&swapChainDesc);
  CD3DX12_RECT     scissorRect(0, 0, static_cast<LONG>(swapChainDesc.Width), static_cast<LONG>(swapChainDesc.Height));
  CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(scissorRect.right), static_cast<float>(scissorRect.bottom));
  commandList->RSSetScissorRects(1, &scissorRect);
  commandList->RSSetViewports(1, &viewport);

  for(UINT eye = 0; eye < (m_config.m_stereo ? 2u : 1u); ++eye)
  {
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeaps[currentNodeIdx]->GetCPUDescriptorHandleForHeapStart(),
                                            rtvIndex + eye * D3D12_SWAP_CHAIN_SIZE, rtvIncrement);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    drawLines(commandList, eye);
    drawSyncIndicator(commandList);
    drawGui(currentNodeIdx);
  }

  const D3D12_RESOURCE_BARRIER renderTargetToPresent =
      nvdx12::transitionBarrier(currentBackBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
  commandList->ResourceBarrier(1, &renderTargetToPresent);

  // Finish recording and execute command lists
  HR_CHECK(m_guiCommandList->Close());
  ID3D12CommandList* rawGuiCommandList = m_guiCommandList.Get();
  m_commandQueues.front()->Wait(m_frameFence.Get(), m_frameIdx);
  m_commandQueues.front()->ExecuteCommandLists(1, &rawGuiCommandList);
  m_commandQueues.front()->Signal(m_guiFence.Get(), m_frameIdx + 1);
  HR_CHECK(commandList->Close());
  ID3D12CommandList* rawCommandList = commandList.Get();
  m_commandQueues[currentNodeIdx]->Wait(m_guiFence.Get(), m_frameIdx + 1);
  m_commandQueues[currentNodeIdx]->ExecuteCommandLists(1, &rawCommandList);
}

void RenderThread::swapBuffers()
{
  if(!m_skipNextSwap)
  {
    auto currentNodeIdx = getCurrentNodeIdx();  // must be called before Present()
    m_allocatorFrameIndices[m_swapChain->GetCurrentBackBufferIndex()] = ++m_frameIdx;
    m_swapChain->Present(m_syncInterval, 0);
    HR_CHECK(m_commandQueues[currentNodeIdx]->Signal(m_frameFence.Get(), m_frameIdx));

#ifndef DISABLE_PRESENT_BARRIER
    if(m_presentBarrierJoined)
    {
      CHECK_NV(NvAPI_QueryPresentBarrierFrameStatistics(m_presentBarrierClient, &m_presentBarrierFrameStats));

      if(m_config.m_quadroSync)
      {
        if(m_requestResetFrameCount)
        {
          m_requestResetFrameCount = false;
          CHECK_NV(NvAPI_D3D1x_ResetFrameCount(m_context.m_device));
        }
        CHECK_NV(NvAPI_D3D1x_QueryFrameCount(m_context.m_device, &m_frameCount));
      }

      if(m_frameCounterFile.is_open())
      {
        m_frameCounterFile << m_presentBarrierFrameStats.PresentCount << std::endl;
      }
    }
#endif
    std::lock_guard guard(m_mutex);
    if(m_requestToggleStereo)
    {
      DXGI_SWAP_CHAIN_DESC1 desc;
      m_swapChain->GetDesc1(&desc);
      swapResize(desc.Width, desc.Height, !m_config.m_stereo, false);
      m_requestToggleStereo = false;
    }
  }

  {
    std::lock_guard guard(m_mutex);
    setDisplayMode(m_requestedDisplayMode);
    if(m_presentBarrierChangeRequested)
    {
      // sync may cause a present barrier leave on its own
      bool before = m_presentBarrierJoined;
      sync();
      if(before == m_presentBarrierJoined)
      {
        forcePresentBarrierChange();
      }
      m_presentBarrierChangeRequested = false;
      m_conVar.notify_all();
    }
  }
}

bool RenderThread::sync()
{
  if(m_frameFence->GetCompletedValue() == m_frameIdx)
  {
    return true;
  }
  HR_CHECK(m_frameFence->SetEventOnCompletion(m_frameIdx, m_syncEvt));
  DWORD waitResult = WaitForSingleObject(m_syncEvt, m_config.m_syncTimeoutMillis);
  if(waitResult == WAIT_OBJECT_0)
  {
    return true;
  }
  else if(waitResult != WAIT_TIMEOUT)
  {
    HR_CHECK(HRESULT_FROM_WIN32(GetLastError()));
  }
  else if(m_presentBarrierJoined)
  {
    LOGW("CPU/GPU synchronization timeout. Forcing present barrier leave.\n");
    forcePresentBarrierChange();
    return sync();
  }
  return false;
}

void RenderThread::setSleepInterval(std::uint32_t millis)
{
  std::lock_guard guard(m_mutex);
  m_config.m_sleepIntervalInMilliseconds = millis;
  LOGI("Sleep interval set to %u ms\n", millis);
}

void RenderThread::changeSleepInterval(std::int32_t deltaMillis)
{
  std::lock_guard guard(m_mutex);
  std::int32_t    millis = m_config.m_sleepIntervalInMilliseconds + deltaMillis;
  if(0 <= millis)
  {
    m_config.m_sleepIntervalInMilliseconds = static_cast<std::uint32_t>(millis);
  }
}

void RenderThread::requestBorderlessStateChange()
{
  std::lock_guard guard(m_mutex);
  if(m_requestedDisplayMode != DisplayMode::FULLSCREEN)
  {
    if(m_requestedDisplayMode == DisplayMode::BORDERLESS)
    {
      m_requestedDisplayMode = DisplayMode::WINDOWED;
    }
    else
    {
      m_requestedDisplayMode = DisplayMode::BORDERLESS;
    }
  }
}

void RenderThread::requestFullscreenStateChange()
{
  std::lock_guard guard(m_mutex);
  if(m_requestedDisplayMode != DisplayMode::BORDERLESS)
  {
    if(m_requestedDisplayMode == DisplayMode::FULLSCREEN)
    {
      m_requestedDisplayMode = DisplayMode::WINDOWED;
    }
    else
    {
      m_requestedDisplayMode = DisplayMode::FULLSCREEN;
    }
  }
}

void RenderThread::requestResetFrameCount()
{
  std::lock_guard guard(m_mutex);
  m_requestResetFrameCount = true;
}

bool RenderThread::requestPresentBarrierChange(std::uint32_t maxWaitMillis)
{
  std::unique_lock lock(m_mutex);
  m_presentBarrierChangeRequested = true;
  return maxWaitMillis == 0 || m_conVar.wait_for(lock, std::chrono::milliseconds(maxWaitMillis)) == std::cv_status::no_timeout;
}

void RenderThread::forcePresentBarrierChange()
{
#ifndef DISABLE_PRESENT_BARRIER
  if(!m_presentBarrierJoined)
  {
    NV_JOIN_PRESENT_BARRIER_PARAMS params = {};
    params.dwVersion                      = NV_JOIN_PRESENT_BARRIER_PARAMS_VER1;
    CHECK_NV(NvAPI_JoinPresentBarrier(m_presentBarrierClient, &params));
    m_presentBarrierJoined = true;
  }
  else
  {
    CHECK_NV(NvAPI_LeavePresentBarrier(m_presentBarrierClient));
    m_presentBarrierJoined = false;
  }
#endif
}

void RenderThread::swapResize(int width, int height, bool stereo, bool force)
{
  if(!force && m_swapChain != nullptr)
  {
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
    m_swapChain->GetDesc1(&swapChainDesc);
    if(width == swapChainDesc.Width && height == swapChainDesc.Height && stereo == m_config.m_stereo)
    {
      return;
    }
  }

  sync();

  // Release back buffer resources before resizing swap chain
  m_backBufferResources.clear();

  // Create swap chain
  const UINT swapFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

  if(m_swapChain == nullptr || stereo != m_config.m_stereo)
  {
    m_swapChain.Reset();

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width       = width;
    swapChainDesc.Height      = height;
    swapChainDesc.Format      = BACK_BUFFER_FORMAT;
    swapChainDesc.Stereo      = stereo;
    swapChainDesc.SampleDesc  = {1, 0};
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = D3D12_SWAP_CHAIN_SIZE;
    swapChainDesc.Scaling     = DXGI_SCALING_NONE;
    swapChainDesc.SwapEffect  = stereo ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode   = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags       = swapFlags;

    ComPtr<IDXGISwapChain1> swapChain1;
    HR_CHECK(m_context.m_factory->CreateSwapChainForHwnd(m_commandQueues[0].Get(), m_windowCallback->getWindowHandle(),
                                                         &swapChainDesc, nullptr, nullptr, &swapChain1));
    HR_CHECK(swapChain1.As(&m_swapChain));

    m_config.m_stereo = stereo;

#ifndef DISABLE_PRESENT_BARRIER
    releasePresentBarrier();

    CHECK_NV(NvAPI_D3D12_CreatePresentBarrierClient(m_context.m_device, m_swapChain.Get(), &m_presentBarrierClient));
#endif
  }
  else if(!m_config.m_alternateFrameRendering)
  {
    HR_CHECK(m_swapChain->ResizeBuffers(D3D12_SWAP_CHAIN_SIZE, width, height, DXGI_FORMAT_UNKNOWN, swapFlags));
  }

  const UINT nodeCount = m_context.m_device->GetNodeCount();

  // Configure swap chain for alternative frame rendering (back buffer resources distributed across nodes in the device)
  if(m_config.m_alternateFrameRendering)
  {
    const UINT             bufferCount = D3D12_SWAP_CHAIN_SIZE * nodeCount;
    std::vector<UINT>      nodeMasks(bufferCount, 0);
    std::vector<IUnknown*> nodeQueues(bufferCount, nullptr);
    for(UINT i = 0; i < D3D12_SWAP_CHAIN_SIZE; ++i)
    {
      for(UINT k = 0; k < nodeCount; ++k)
      {
        nodeMasks[i * nodeCount + k]  = 1 << k;
        nodeQueues[i * nodeCount + k] = m_commandQueues[k].Get();
      }
    }
    HR_CHECK(m_swapChain->ResizeBuffers1(bufferCount, width, height, DXGI_FORMAT_UNKNOWN, swapFlags, nodeMasks.data(),
                                         nodeQueues.data()));
    m_backBufferResources.resize(bufferCount);
  }
  else
  {
    m_backBufferResources.resize(D3D12_SWAP_CHAIN_SIZE);
  }

  // get back buffers and create render target views
  const UINT rtvIncrement = m_context.m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for(UINT i = 0; i < m_backBufferResources.size(); ++i)
  {
    HR_CHECK(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBufferResources[i])));

    const UINT rtvIndex  = m_config.m_alternateFrameRendering ? i / nodeCount : i;
    const UINT nodeIndex = m_config.m_alternateFrameRendering ? i % nodeCount : 0;

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc;
    rtvDesc.Format = BACK_BUFFER_FORMAT;
    if(!m_config.m_stereo)
    {
      rtvDesc.ViewDimension        = D3D12_RTV_DIMENSION_TEXTURE2D;
      rtvDesc.Texture2D.MipSlice   = 0;
      rtvDesc.Texture2D.PlaneSlice = 0;
    }
    else
    {
      rtvDesc.ViewDimension                  = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
      rtvDesc.Texture2DArray.MipSlice        = 0;
      rtvDesc.Texture2DArray.FirstArraySlice = 0;
      rtvDesc.Texture2DArray.ArraySize       = 0;
      rtvDesc.Texture2DArray.PlaneSlice      = 0;
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeaps[nodeIndex]->GetCPUDescriptorHandleForHeapStart(), rtvIndex, rtvIncrement);
    m_context.m_device->CreateRenderTargetView(m_backBufferResources[i].Get(), nullptr, rtvHandle);

    if(m_config.m_stereo)
    {
      rtvDesc.Texture2DArray.FirstArraySlice = 1;
      CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandleRight(rtvHandle, D3D12_SWAP_CHAIN_SIZE, rtvIncrement);
      m_context.m_device->CreateRenderTargetView(m_backBufferResources[i].Get(), nullptr, rtvHandleRight);
    }
  }

  // set up gui texture
  m_guiTexture.Reset();
  CD3DX12_HEAP_PROPERTIES guiTexHeapProps(D3D12_HEAP_TYPE_DEFAULT, 1, (1 << nodeCount) - 1);
  CD3DX12_RESOURCE_DESC   guiTexDesc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                                     width, height, 1, 1, BACK_BUFFER_FORMAT, 1, 0, D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
  const FLOAT             black[] = {0.0f, 0.0f, 0.0f, 0.0f};
  CD3DX12_CLEAR_VALUE     guiTexClearValue(guiTexDesc.Format, black);
  HR_CHECK(m_context.m_device->CreateCommittedResource(&guiTexHeapProps, D3D12_HEAP_FLAG_NONE, &guiTexDesc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                       &guiTexClearValue, IID_PPV_ARGS(&m_guiTexture)));
  D3D12_SHADER_RESOURCE_VIEW_DESC guiTexSrvDesc = {};
  guiTexSrvDesc.Format                          = guiTexDesc.Format;
  guiTexSrvDesc.ViewDimension                   = D3D12_SRV_DIMENSION_TEXTURE2D;
  guiTexSrvDesc.Shader4ComponentMapping         = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  guiTexSrvDesc.Texture2D.MipLevels             = 1;
  for(auto& heap : m_cbvSrvUavHeaps)
  {
    m_context.m_device->CreateShaderResourceView(m_guiTexture.Get(), &guiTexSrvDesc, heap->GetCPUDescriptorHandleForHeapStart());
  }
  D3D12_RENDER_TARGET_VIEW_DESC guiTexRtvDesc = {};
  guiTexRtvDesc.Format                        = guiTexDesc.Format;
  guiTexRtvDesc.ViewDimension                 = D3D12_RTV_DIMENSION_TEXTURE2D;
  auto guiRtvCpuHandle                        = m_rtvHeaps.front()->GetCPUDescriptorHandleForHeapStart();
  guiRtvCpuHandle.ptr +=
      D3D12_SWAP_CHAIN_SIZE * 2 * m_context.m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  m_context.m_device->CreateRenderTargetView(m_guiTexture.Get(), &guiTexRtvDesc, guiRtvCpuHandle);

#ifndef DISABLE_PRESENT_BARRIER
  assert(m_presentBarrierClient && m_presentBarrierFence && !m_backBufferResources.empty());

  std::vector<ID3D12Resource*> rawBackBuffers(m_backBufferResources.size());
  std::transform(m_backBufferResources.begin(), m_backBufferResources.end(), rawBackBuffers.begin(),
                 [](auto b) { return b.Get(); });

  // Register the new back buffer resources
  CHECK_NV(NvAPI_D3D12_RegisterPresentBarrierResources(m_presentBarrierClient, m_presentBarrierFence.Get(),
                                                       rawBackBuffers.data(), static_cast<NvU32>(m_backBufferResources.size())));
#endif
}

void RenderThread::toggleStereo()
{
  std::lock_guard guard(m_mutex);
  m_requestToggleStereo = !m_requestToggleStereo;
}

void RenderThread::toggleScrolling()
{
  std::lock_guard guard(m_mutex);
  m_config.m_scrolling = !m_config.m_scrolling;
}

void RenderThread::toggleQuadroSync()
{
  std::lock_guard guard(m_mutex);
  m_config.m_quadroSync = !m_config.m_quadroSync;
}

void RenderThread::setVsync(bool enabled)
{
  std::lock_guard guard(m_mutex);
  m_syncInterval = enabled ? 1 : 0;
}

std::uint32_t RenderThread::getCurrentNodeIdx()
{
  return m_config.m_alternateFrameRendering ? m_swapChain->GetCurrentBackBufferIndex() % m_context.m_device->GetNodeCount() : 0;
}

void RenderThread::drawLines(ComPtr<ID3D12GraphicsCommandList> commandList, uint32_t offset)
{
  if(!m_config.m_showVerticalLines && !m_config.m_showHorizontalLines)
  {
    return;
  }

  // Update line constants
  struct LineConstants
  {
    float    verticalSizeA;
    float    verticalSizeB;
    float    horizontalSizeA;
    float    horizontalSizeB;
    float    verticalOffset;
    float    horizontalOffset;
    float    verticalSpacing;
    float    horizontalSpacing;
    uint32_t numLines;
    uint32_t firstHorizontalInstance;
    uint32_t extraOffset;
  } constants;
  static_assert((sizeof(LineConstants) % sizeof(uint32_t)) == 0, "Unexpected LineConstants size");

  constants.numLines = m_config.m_numLines;
  constants.firstHorizontalInstance =
      m_config.m_showHorizontalLines ? (m_config.m_showVerticalLines ? m_config.m_numLines / 2 : 0) : m_config.m_numLines;
  constants.extraOffset = offset;

  DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
  m_swapChain->GetDesc1(&swapChainDesc);
  const UINT width  = swapChainDesc.Width;
  const UINT height = swapChainDesc.Height;

  // Convert pixel size into interpolation values
  constants.verticalSizeA   = m_config.m_lineSizeInPixels[0] / static_cast<float>(width);
  constants.horizontalSizeA = m_config.m_lineSizeInPixels[0] / static_cast<float>(height);
  if(m_config.m_lineSizeInPixels[1] != 0)
  {
    constants.verticalSizeB   = m_config.m_lineSizeInPixels[1] / static_cast<float>(width);
    constants.horizontalSizeB = m_config.m_lineSizeInPixels[1] / static_cast<float>(height);
  }
  else
  {
    constants.verticalSizeB   = constants.verticalSizeA;
    constants.horizontalSizeB = constants.horizontalSizeA;
  }

  // Update line offset based on the current frame count and speed
  constants.verticalOffset = ((m_frameCount * m_config.m_lineSpeedInPixels) % width) / static_cast<float>(width);
  constants.verticalOffset += offset * constants.verticalSizeB;
  constants.horizontalOffset = ((m_frameCount * m_config.m_lineSpeedInPixels) % height) / static_cast<float>(height);
  constants.horizontalOffset += offset * constants.horizontalSizeB;

  // Calculate spacing between lines so that they appear as a grid of squares
  constants.verticalSpacing = (static_cast<float>(height) / constants.firstHorizontalInstance) / static_cast<float>(width);
  constants.horizontalSpacing =
      (static_cast<float>(height) / (m_config.m_numLines - constants.firstHorizontalInstance)) / static_cast<float>(height);

  commandList->SetPipelineState(m_linesPipeline.Get());
  commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  commandList->SetGraphicsRootSignature(m_rootSignature.Get());
  commandList->SetGraphicsRoot32BitConstants(0, sizeof(LineConstants) / sizeof(uint32_t), &constants, 0);

  // Render the lines via instancing (every instance is one line, where each line consists of a quad made of two triangles)
  commandList->DrawInstanced(4, m_config.m_numLines, 0, 0);
}

void RenderThread::drawSyncIndicator(ComPtr<ID3D12GraphicsCommandList> commandList)
{
  float color[3];
  switch(m_presentBarrierFrameStats.SyncMode)
  {
    default:
    case PRESENT_BARRIER_NOT_JOINED:  // red
      color[0] = 1.0f;
      color[1] = 0.0f;
      color[2] = 0.0f;
      break;
    case PRESENT_BARRIER_SYNC_CLIENT:  // yellow
      color[0] = 1.0f;
      color[1] = 1.0f;
      color[2] = 0.1f;
      break;
    case PRESENT_BARRIER_SYNC_SYSTEM:
    case PRESENT_BARRIER_SYNC_CLUSTER:  // green
      color[0] = 0.462f;
      color[1] = 0.725f;
      color[2] = 0.0f;
      break;
  }

  commandList->SetPipelineState(m_indicatorPipeline.Get());
  commandList->SetGraphicsRoot32BitConstants(0, 3, color, 0);
  commandList->DrawInstanced(8, 1, 0, 0);
}

void RenderThread::prepareGui()
{
  ImGui_ImplDX12_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  ImGui::Begin("Present barrier stats");
  ImGui::SetWindowSize({240, 120});
  ImGui::SetWindowPos({0, 0});
  ImGui::BeginTable("table", 2, ImGuiTableFlags_SizingStretchProp);
  ImGui::TableNextColumn();
  ImGui::Text("SyncMode");
  ImGui::TableNextColumn();
  switch(m_presentBarrierFrameStats.SyncMode)
  {
    case PRESENT_BARRIER_NOT_JOINED:
      ImGui::Text("NOT_JOINED");
      break;
    case PRESENT_BARRIER_SYNC_CLIENT:
      ImGui::Text("SYNC_CLIENT");
      break;
    case PRESENT_BARRIER_SYNC_SYSTEM:
      ImGui::Text("SYNC_SYSTEM");
      break;
    case PRESENT_BARRIER_SYNC_CLUSTER:
      ImGui::Text("SYNC_CLUSTER");
      break;
    default:
      ImGui::Text("0x%08x", m_presentBarrierFrameStats.SyncMode);
      break;
  }
  ImGui::TableNextColumn();
  ImGui::Text("PresentCount");
  ImGui::TableNextColumn();
  ImGui::Text("%d", m_presentBarrierFrameStats.PresentCount);
  ImGui::TableNextColumn();
  ImGui::Text("PresentInSyncCount");
  ImGui::TableNextColumn();
  ImGui::Text("%d", m_presentBarrierFrameStats.PresentInSyncCount);
  ImGui::TableNextColumn();
  ImGui::Text("FlipInSyncCount");
  ImGui::TableNextColumn();
  ImGui::Text("%d", m_presentBarrierFrameStats.FlipInSyncCount);
  ImGui::TableNextColumn();
  ImGui::Text("RefreshCount");
  ImGui::TableNextColumn();
  ImGui::Text("%d", m_presentBarrierFrameStats.RefreshCount);
  ImGui::EndTable();
  ImGui::End();

  ImGui::Render();

  auto cbvSrvUavHeap = m_cbvSrvUavHeaps.front().Get();
  m_guiCommandList->SetDescriptorHeaps(1, &cbvSrvUavHeap);
  auto guiRtvHandle = m_rtvHeaps.front()->GetCPUDescriptorHandleForHeapStart();
  guiRtvHandle.ptr +=
      D3D12_SWAP_CHAIN_SIZE * 2 * m_context.m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  FLOAT guiClearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};
  m_guiCommandList->ClearRenderTargetView(guiRtvHandle, guiClearColor, 0, nullptr);
  m_guiCommandList->OMSetRenderTargets(1, &guiRtvHandle, FALSE, nullptr);
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_guiCommandList.Get());
}

void RenderThread::drawGui(unsigned int currentNodeIdx)
{
  auto commandList = m_commandLists[currentNodeIdx];
  commandList->SetPipelineState(m_guiPipeline.Get());
  commandList->SetGraphicsRootDescriptorTable(1, m_cbvSrvUavHeaps[currentNodeIdx]->GetGPUDescriptorHandleForHeapStart());
  commandList->DrawInstanced(3, 1, 0, 0);
}

void RenderThread::setDisplayMode(DisplayMode displayMode)
{
  BOOL fullscreen;
  HR_CHECK(m_swapChain->GetFullscreenState(&fullscreen, nullptr));
  if(fullscreen != (m_displayMode == DisplayMode::FULLSCREEN))
  {
    displayMode = DisplayMode::WINDOWED;
  }
  if(displayMode == m_displayMode)
  {
    return;
  }

  ComPtr<IDXGIOutput> output;

  // Get output for borderless and fullscreen
  if(m_config.m_outputIndex == -1)
  {
    HR_CHECK(m_swapChain->GetContainingOutput(&output));
  }
  else
  {
    ComPtr<IDXGIAdapter> adapter;
    HR_CHECK(m_context.m_factory->EnumAdapterByLuid(m_context.m_device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
    HR_CHECK(adapter->EnumOutputs(static_cast<UINT>(m_config.m_outputIndex), &output));
  }

  assert(output != nullptr);

  sync();

  DXGI_MODE_DESC modeDesc = {};
  modeDesc.Format         = BACK_BUFFER_FORMAT;
  if(displayMode == DisplayMode::FULLSCREEN)
  {
    DXGI_OUTPUT_DESC desc;
    HR_CHECK(output->GetDesc(&desc));
    modeDesc.Width  = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    modeDesc.Height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    HR_CHECK(output->FindClosestMatchingMode(&modeDesc, &modeDesc, nullptr));
    HR_CHECK(m_swapChain->SetFullscreenState(TRUE, output.Get()));
    HR_CHECK(m_swapChain->ResizeTarget(&modeDesc));
  }
  else
  {
    if(m_displayMode == DisplayMode::FULLSCREEN)
    {
      HR_CHECK(m_swapChain->SetFullscreenState(FALSE, nullptr));
    }

    // Find the desktop coordinates of the output to position the window
    DXGI_OUTPUT_DESC outputDesc;
    HR_CHECK(output->GetDesc(&outputDesc));
    int x = static_cast<int>(outputDesc.DesktopCoordinates.left);
    int y = static_cast<int>(outputDesc.DesktopCoordinates.top);

    if(displayMode == DisplayMode::BORDERLESS)
    {
      // Borderless is just the window without any decoration; sized to match the output's desktop
      modeDesc.Width  = static_cast<UINT>(outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left);
      modeDesc.Height = static_cast<UINT>(outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top);
      m_windowCallback->setDecorated(false);
    }
    else
    {
      // Move the window a little away from the corner of the output
      x += 128;
      y += 160;
      modeDesc.Width  = SAMPLE_WINDOWED_WIDTH;
      modeDesc.Height = SAMPLE_WINDOWED_HEIGHT;
      m_windowCallback->setDecorated(true);
    }

    m_windowCallback->setPosAndSize(x, y, modeDesc.Width, modeDesc.Height);
  }

  m_displayMode          = displayMode;
  m_requestedDisplayMode = displayMode;

  // Some display mode transitions are not detected by GLFW, so force resize
  swapResize(modeDesc.Width, modeDesc.Height, m_config.m_stereo, true);
}

void RenderThread::releasePresentBarrier()
{
#ifndef DISABLE_PRESENT_BARRIER
  if(m_presentBarrierClient)
  {
    if(m_presentBarrierJoined)
    {
      CHECK_NV(NvAPI_LeavePresentBarrier(m_presentBarrierClient));
      m_presentBarrierJoined = false;
    }
    CHECK_NV(NvAPI_DestroyPresentBarrierClient(m_presentBarrierClient));
    m_presentBarrierClient = nullptr;
  }
#endif
}

void RenderThread::end()
{
  ImGui_ImplDX12_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  releasePresentBarrier();
  //CHECK_NV(NvAPI_Unload());

  m_guiTexture.Reset();
  m_guiPipeline.Reset();
  m_indicatorPipeline.Reset();
  m_linesPipeline.Reset();
  m_rootSignature.Reset();
  m_rtvHeaps.clear();
  m_cbvSrvUavHeaps.clear();
  m_guiCommandList.Reset();
  m_commandLists.clear();
  m_guiCommandAllocators.clear();
  m_commandAllocators.clear();
  CloseHandle(m_syncEvt);
  m_guiFence.Reset();
  m_frameFence.Reset();
  m_presentBarrierFence.Reset();
  m_backBufferResources.clear();
  m_commandQueues.clear();
  m_swapChain.Reset();
  m_context.deinit();
}