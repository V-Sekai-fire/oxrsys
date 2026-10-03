# Architecture

## Overview

OXRSys Runtime is a cross-platform OpenXR runtime in progress. macOS is the mature path, Linux is being added through Vulkan + PyroWave scaffolding, and Windows renders Vulkan apps into shared D3D11 textures and streams them with PyroWave. Shared platform, config, status, and socket helpers are kept portable so platform-specific backends can be added without spreading OS calls through the runtime. The runtime is discovered by the OpenXR loader through the generated `oxrsys-runtime.json` manifest.

## Repository Layout

- `runtime/`: runtime library, graphics integration, input, configuration, streaming server, tracking receiver, and video encoder.
- `common/protocol/include/oxrsys/protocol/`: canonical C++ protocol and FEC wire layout.
- `clients/Android/android-vr/`: Quest/Pico-oriented Android VR client for decode, display, and tracking return.
- `clients/Apple/`: Xcode workspace, native SwiftUI Home app, unified Apple simulator/viewer, visionOS viewer, and shared Swift packages.
- `clients/Qt/`: Qt Home app, Qt simulator app, and reusable Qt simulator widget.
- `tests/`: unit-style and loader-backed runtime tests.
- `cmake/`: CMake helpers, including the OpenXR-CTS lane.
- `docs/`: focused project documentation.

## Runtime Modes

The runtime operates in two modes:

- `Simulator`: local keyboard and mouse input, local debug rendering, no headset required.
- `Streaming`: a headset client connects, sends tracking, and receives encoded frames.

The simulator is useful for API validation and local debugging. Streaming is the path used for Quest-class headsets and future remote clients.

## Frame And Input Flow

At a high level:

1. The application drives the normal OpenXR frame loop.
2. The runtime maintains instance, session, space, action, swapchain, and hand-tracker state.
3. Input comes either from the simulator path or from the tracking receiver.
4. `xrEndFrame` validates and publishes the submitted composition data.
5. The local renderer presents to the debug window, and the streaming path can encode the latest frame for the client.

`Session::EndFrame()` must remain non-blocking. The streaming path uses `StreamingFrameQueue`, a latest-frame-only queue that replaces any not-yet-encoded frame and immediately releases the replaced `FrameSource` resources. `FrameSource` owns backend-native image references and per-image sync tokens so async encoders can safely outlive the OpenXR frame submission.

## Runtime Boundaries

The runtime keeps these internal boundaries explicit:

- `RuntimePlatform`: config/state roots, module directory detection, and process ids.
- `RuntimeSockets`: UDP/TCP socket creation, options, timeout, close, and best-effort send/receive wrappers.
- `GraphicsContext`: typed Metal/Vulkan session context passed from OpenXR graphics bindings into sessions, swapchains, streaming, and encoders.
- `FrameSource`: the pair of backend-native frame resources owned by the latest-frame queue until the encoder consumes or replaces them.
- `VulkanDispatch`: Vulkan function dispatch resolved from app-provided or already-loaded process entry points without linking the runtime to the Vulkan loader.

## Graphics Integration

### Metal

Metal is the native Apple rendering path. Applications provide an `MTLDevice` through `XR_KHR_metal_enable`, and swapchain textures are backed by native Metal resources.

For dynamic Metal swapchains, `xrReleaseSwapchainImage` snapshots the released slot into a staging texture using the app-provided `MTLCommandQueue`. The snapshot signals a `MTLSharedEvent`, and the encoder's staging blit waits on that event GPU-side before reading the staging texture. This prevents the streaming encoder from reading a swapchain slot after the app has released and reused it, while keeping `Session::EndFrame()` CPU-non-blocking. Staging slots are leased through `FrameSource`; if no slot is safe to reuse, the runtime skips that streaming frame instead of falling back to an unsafe live-slot read.

### Vulkan

Vulkan support is exposed through `XR_KHR_vulkan_enable` and `XR_KHR_vulkan_enable2`. The runtime does not link directly against Vulkan. Instead, it resolves Vulkan functions through the application-provided loader path to avoid dual-loader and dual-MoltenVK issues.

The v2 path stores the app's `pfnGetInstanceProcAddr`. The v1 path first reuses that dispatch if available, then looks for `vkGetInstanceProcAddr` only in already-loaded process modules: `dlsym(RTLD_DEFAULT, ...)` on POSIX and `GetModuleHandleW(L"vulkan-1.dll")` plus `GetProcAddress` on Windows. It intentionally does not load a Vulkan loader itself.

On Apple, Vulkan images can use `VK_EXT_metal_objects` to bridge Vulkan-backed images to Metal textures. On Linux, the first-pass Vulkan swapchain allocates Vulkan images directly and the PyroWave encoder is wired; it sends a black frame until it reads those images.

On macOS, the runtime streams only PyroWave, through its Metal port (MIT, fetched with FetchContent; a sibling `pyrowave-upstream` checkout is used in its place when present). The staged BGRA slot is converted to NV12 video range with BT.709 by `VTPixelTransferSession`, encoded with exact rate control at `bitrate_mbps / refresh_rate_hz` bytes per frame, and sent as one NAL unit per frame with `VideoCodec::PyroWave`. Every frame is intra-only, so `ForceKeyframe` changes nothing. `VTPixelTransferSession` is the only VideoToolbox use left in the runtime. The Apple simulator and the visionOS viewer decode the stream in `OXRSysStreaming`'s `PyroWaveDecoder` into NV12 video-range pixel buffers for `StereoRenderer`; Swift packages cannot reach outside their root, so the decoder half of the Metal port is copied into the package's `CPyroWave` target at the commit the runtime fetches. The Qt frontends are not built on Apple platforms, and `-DOXRSYS_BUILD_QT_FRONTENDS=ON` fails configure there.

On Windows and Linux the encoder is PyroWave's Vulkan C API (MIT), fetched with FetchContent with the Granite subset its `checkout_granite.sh` pins and linked statically, its symbols kept out of the Linux runtime's exports. On Windows the eyes are copied side by side into a shared D3D11 texture that PyroWave imports through its KMT handle, with a D3D11 fence imported as the timeline semaphore it waits on; each frame is encoded with exact rate control at `bitrate_mbps / refresh_rate_hz` bytes and sent as one NAL unit with `VideoCodec::PyroWave`. Every frame is intra-only, so `ForceKeyframe` changes nothing. The Qt simulator lends its own Vulkan device to PyroWave, converts the decoded planes with the Lean-authored `kernels/simulator` kernel, and blits the left eye into a swapchain on its preview.

## Input And Actions

The input system is profile-aware. The runtime currently supports:

- `KHR simple_controller`
- `oculus/touch_controller`
- Meta Quest Touch and Touch Plus controller profiles
- PICO Neo3 and PICO 4 controller profiles
- `ext/hand_interaction_ext`

The runtime also supports:

- `XR_EXT_hand_tracking`
- `XR_EXT_conformance_automation`
- `XR_EXT_debug_utils`
- `xrLocateSpacesKHR` as an alias for OpenXR 1.1 `xrLocateSpaces`

Reference spaces currently enumerate `VIEW`, `LOCAL`, `LOCAL_FLOOR`, and `STAGE`.

## Configuration

Runtime configuration is loaded from:

- macOS: `~/Library/Application Support/OXRSys/oxrsys-runtime.toml`
- Linux: `${XDG_CONFIG_HOME:-~/.config}/oxrsys/oxrsys-runtime.toml`
- Windows: `%APPDATA%/OXRSys/oxrsys-runtime.toml`
- fallback: `build/runtime/oxrsys-runtime.toml`

Runtime status and logs are written to the platform state directory. On Linux this is `${XDG_STATE_HOME:-~/.local/state}/oxrsys`; on Windows it currently shares `%APPDATA%/OXRSys`.

For terminal-launched applications, use `XR_RUNTIME_JSON`. On macOS, `scripts/oxrsys_runtime_default.sh` can register `build/runtime/oxrsys-runtime.json` as the user default runtime and restore `XR_RUNTIME_JSON` through a LaunchAgent.

The native Home app in `clients/Apple/oxrsys-home/` manages the macOS workflow. The Qt Home app in `clients/Qt/oxrsys-home/` owns Linux runtime registration and can manually launch apps with the user-selected `XR_RUNTIME_JSON` on other desktop platforms.

The runtime reloads config changes opportunistically when the file timestamp changes. `runtime_enabled` is enforced on subsequent `xrCreateInstance` calls, dynamic streaming values such as keyframe cadence update without a full process restart, while initialization-time resources such as the file logger sink still require a restart. Headset and simulator clients own their eye FOV and send it through tracking metadata; the runtime config only keeps an internal fallback for clients that omit it.
