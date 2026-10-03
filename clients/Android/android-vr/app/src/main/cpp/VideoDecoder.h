// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct AHardwareBuffer;

namespace oxr
{

/**
 * PyroWave decoder on a Vulkan device of its own.
 *
 * A decode thread decodes the newest submitted frame into three R8 planes (Y, Cb, Cr) backed by
 * AHardwareBuffers and waits for the GPU before publishing them, so the GLES renderer can import
 * the planes as EGLImages and sample them with no further synchronisation.
 */
class VideoDecoder
{
public:
    static constexpr uint32_t PlaneCount = 3;

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    bool Initialize(uint32_t width, uint32_t height, EGLDisplay display);
    void Shutdown();

    // Queues one PyroWave frame; a frame not yet decoded is replaced by a newer one.
    bool SubmitFrame(const uint8_t* data, size_t size, int64_t presentationTimeUs,
                     int64_t receiveTimeNs, bool alphaBlend);

    struct DecodedFrame
    {
        AHardwareBuffer* planes[PlaneCount] = {};
        int64_t presentationTimeUs = 0;
        uint32_t bufferWidth = 0;
        uint32_t bufferHeight = 0;
        uint32_t bufferStride = 0;
        int32_t cropLeft = 0;
        int32_t cropTop = 0;
        int32_t cropRight = 0;
        int32_t cropBottom = 0;
        int64_t localReceiveTimeNs = 0;
        int64_t localSubmitTimeNs = 0;
        int64_t localAcquireTimeNs = 0;
        uint32_t skippedFramesBeforeAcquire = 0;
        bool alphaBlend = false;
    };

    // Called on the GL thread with the context current. The planes stay unwritten until the
    // GL commands issued before the next successful AcquireFrame have completed.
    bool AcquireFrame(DecodedFrame* outFrame);

    bool IsInitialized() const { return running_.load(); }

    uint32_t GetWidth() const { return width_; }
    uint32_t GetHeight() const { return height_; }
    uint32_t GetSkippedFramesBeforeAcquire() const { return skippedFramesBeforeAcquire_.load(); }

    // Width and height from a PyroWave frame's sequence header, or false without one.
    static bool FrameSize(const uint8_t* data, size_t size, uint32_t* width, uint32_t* height);

private:
    struct Gpu;

    struct FrameMetadata
    {
        int64_t presentationTimeUs = 0;
        int64_t receiveTimeNs = 0;
        int64_t submitTimeNs = 0;
        bool alphaBlend = false;
    };

    struct Slot
    {
        FrameMetadata metadata;
        uint32_t skippedBefore = 0;
        EGLSyncKHR glDone = EGL_NO_SYNC_KHR; // set when the renderer lets go of the slot
    };

    static constexpr int SlotCount = 3;

    void DecodeThreadMain();
    void DestroySyncs();

    std::unique_ptr<Gpu> gpu_;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR_ = nullptr;
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR_ = nullptr;
    PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR_ = nullptr;

    uint32_t width_ = 0;
    uint32_t height_ = 0;

    std::thread decodeThread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<uint8_t> pending_;
    FrameMetadata pendingMetadata_;
    bool hasPending_ = false;
    uint32_t pendingReplaced_ = 0;

    Slot slots_[SlotCount];
    int heldSlot_ = -1;   // sampled by the renderer
    int readySlot_ = -1;  // decoded, not yet acquired
    std::atomic<uint32_t> skippedFramesBeforeAcquire_{0};
};

} // namespace oxr
