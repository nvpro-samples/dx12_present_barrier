# dx12_present_barrier

This sample demonstrates the usage of the new NvAPI interface to synchronize
present calls between windows on the same system as well as on distributed
systems. It can also be used to check if systems are configured to support
synchronized present through DirectX 12 present barrier. A general overview of
the interface can be found on the [NVIDIA developer blog](https://developer.nvidia.com/blog/).

## System Requirements
* NVIDIA RTX / Quadro series card (Geforce is NOT supported)
* 472.12 driver or newer
* NVAPI R470 or newer (can be downloaded from [here](https://developer.nvidia.com/nvapi))

## Sample Features

The app draws a couple of moving bars and lines to visually check the
synchronization quality. In the upper left corner a window with present barrier
statistics is shown. There are several keyboard shortcuts that can be used at
runtime.
* V          - Toggle vsync
* S          - Toggle scrolling of the lines
* T          - Toggle present barrier
* Q          - Toggle usage of the Quadro Sync frame counter
* W          - Increase sleep interval between presets by 1ms
* Alt + W    - Reset sleep interval between presents to zero (effectively disabling it)
* Shift + W  - Decrease sleep interval between presents by 1ms
* 2          - Toggle stereoscopic rendering

A bar at the top of the window indicates the present barrier status.
* red     - The swap chain is not in present barrier sync
* yellow  - The swap chain is in present barrier sync with other clients on the local system
* green   - The swap chain is in present barrier sync across systems through framelock
 
## Build and Run

Clone https://github.com/nvpro-samples/nvpro_core.git
next to this repository (or pull latest `master` if you already have it):

`mkdir build && cd build && cmake .. # Or use CMake GUI`

If there are missing dependencies (e.g. glfw), run `git submodule
update --init --recursive --checkout --force` in the `nvpro_core`
repository.

Then start the generated `.sln` in VS or run `make -j`.

Run `dx12_present_barrier` or `../../bin_x64/Release/dx12_present_barrier.exe`

You are advised not to run the debug build unless you have the
required validation layers.

## LICENSE

Copyright 2022 NVIDIA CORPORATION. Released under Apache License,
Version 2.0. See "LICENSE" file for details.