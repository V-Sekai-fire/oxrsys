// SPDX-License-Identifier: MPL-2.0
//
// Linux streaming encoder: the packed eye image LinuxVulkanInterop.cpp exports from the app's
// device is imported by PyroWave on its own Vulkan device on the same GPU and encoded after the
// exported timeline semaphore, so the frame never leaves the GPU. Without the interop it streams
// an opaque black frame.

#include "Config.h"
#include "LinuxVulkanInterop.h"
#include "VideoEncoder.h"

#include <vulkan/vulkan.h>
#include <pyrowave.h>

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

double ToMilliseconds(Clock::duration duration)
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

constexpr size_t PacketBoundary = 64 * 1024;
constexpr size_t ImportSlots = 8;

struct ImportedImage
{
    uint64_t interopId = 0;
    uint64_t generation = 0;
    pyrowave_image image = nullptr;
    pyrowave_image_view view = {};
};

struct LinuxPyroWaveState
{
    pyrowave_device device = nullptr;
    pyrowave_encoder encoder = nullptr;
    bool sameGpu = false; // the device is on the app's GPU, so its frames can be imported

    pyrowave_sync_object sync = nullptr;
    uint64_t syncInteropId = 0;
    std::array<ImportedImage, ImportSlots> images = {};

    std::vector<pyrowave_packet> packets;
    std::vector<uint8_t> bitstream;

    ~LinuxPyroWaveState()
    {
        for (ImportedImage& imported : images)
        {
            if (imported.image != nullptr)
                pyrowave_image_destroy(imported.image);
        }
        if (sync != nullptr)
            pyrowave_sync_object_destroy(sync);
        if (encoder != nullptr)
            pyrowave_encoder_destroy(encoder);
        if (device != nullptr)
            pyrowave_device_destroy(device);
    }

    bool ImportSync(const LinuxFrameImage& frame)
    {
        if (sync != nullptr && syncInteropId == frame.interopId)
            return true;
        if (sync != nullptr)
        {
            pyrowave_sync_object_destroy(sync);
            sync = nullptr;
        }
        const int fd = LinuxExportTimelineFd(frame);
        if (fd < 0)
            return false;
        pyrowave_sync_object_create_info info = {};
        info.device = device;
        info.external_handle = static_cast<pyrowave_os_handle>(fd);
        info.handle_type = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
        info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;
        if (pyrowave_sync_object_create(&info, &sync) != PYROWAVE_SUCCESS)
        {
            close(fd);
            sync = nullptr;
            return false;
        }
        syncInteropId = frame.interopId;
        return true;
    }

    // The import of frame's packed image, made once per slot and generation.
    const ImportedImage* ImportImage(const LinuxFrameImage& frame)
    {
        if (frame.slotIndex >= images.size())
            return nullptr;
        ImportedImage& imported = images[frame.slotIndex];
        if (imported.image != nullptr && imported.interopId == frame.interopId &&
            imported.generation == frame.slotGeneration)
            return &imported;
        if (imported.image != nullptr)
            pyrowave_image_destroy(imported.image);
        imported = {};

        const int fd = LinuxExportPackedImageFd(frame);
        if (fd < 0)
            return nullptr;
        VkImageCreateInfo imageInfo = LinuxPackedImageCreateInfo(frame.format, frame.width, frame.height);
        pyrowave_image_create_info info = {};
        info.device = device;
        info.external_handle = static_cast<pyrowave_os_handle>(fd);
        info.handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        info.image_create_info = &imageInfo;
        if (pyrowave_image_create(&info, &imported.image) != PYROWAVE_SUCCESS)
        {
            close(fd);
            imported = {};
            return nullptr;
        }
        if (pyrowave_image_get_image_view(imported.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_USAGE_SAMPLED_BIT,
                                          &imported.view) != PYROWAVE_SUCCESS)
        {
            pyrowave_image_destroy(imported.image);
            imported = {};
            return nullptr;
        }
        imported.interopId = frame.interopId;
        imported.generation = frame.slotGeneration;
        return &imported;
    }
};

LinuxPyroWaveState* State(void* opaque)
{
    return static_cast<LinuxPyroWaveState*>(opaque);
}

const LinuxFrameImage* Frame(const FrameImageSource& source)
{
    return source.IsValid() ? static_cast<const LinuxFrameImage*>(source.GetImage()) : nullptr;
}

// Opaque black in full-range YUV 4:2:0.
pyrowave_result EncodeBlack(pyrowave_encoder encoder, uint32_t width, uint32_t height,
                            const pyrowave_rate_control& rate)
{
    const size_t luma = static_cast<size_t>(width) * height;
    std::vector<uint8_t> planes[3] = {std::vector<uint8_t>(luma, 0), std::vector<uint8_t>(luma / 4, 128),
                                      std::vector<uint8_t>(luma / 4, 128)};
    pyrowave_cpu_buffer buffer = {};
    buffer.width = static_cast<int>(width);
    buffer.height = static_cast<int>(height);
    buffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    for (int i = 0; i < 3; ++i)
    {
        buffer.data[i] = planes[i].data();
        buffer.row_stride_in_bytes[i] = i == 0 ? width : width / 2;
        buffer.plane_size_in_bytes[i] = planes[i].size();
    }
    return pyrowave_encoder_encode_cpu_synchronous(encoder, &buffer, &rate);
}

} // namespace

VideoEncoder::VideoEncoder() = default;

VideoEncoder::~VideoEncoder()
{
    Shutdown();
}

bool VideoEncoder::SupportsFoveatedEncoding(const GraphicsContext& /*graphicsContext*/)
{
    return false;
}

oxr::protocol::VideoCodec VideoEncoder::StreamCodec()
{
    return oxr::protocol::VideoCodec::PyroWave;
}

bool VideoEncoder::Initialize(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateMbps,
                              const GraphicsContext& graphicsContext)
{
    Shutdown();

    width_ = width & ~3u;
    height_ = height & ~1u;
    eyeWidth_ = width_ / 2;
    fps_ = std::max(fps, 1u);
    bitrateMbps_ = bitrateMbps;
    graphicsContext_ = graphicsContext;
    frameCount_ = 0;
    forceKeyframe_.store(false);
    shuttingDown_.store(false);
    droppedFrameCount_.store(0);
    inFlightFrameCount_.store(0);
    frameNumberCounter_.store(0);

    std::unique_ptr<LinuxPyroWaveState> state = std::make_unique<LinuxPyroWaveState>();
    pyrowave_uuid deviceUuid = {};
    pyrowave_uuid driverUuid = {};
    if (graphicsContext.api == GraphicsApi::Vulkan &&
        LinuxGetDeviceUuids(graphicsContext.vulkan, deviceUuid.uuid, driverUuid.uuid) &&
        pyrowave_create_device_by_compat(0, 0, &deviceUuid, &driverUuid, nullptr, &state->device) ==
            PYROWAVE_SUCCESS)
    {
        state->sameGpu = true;
    }
    else
    {
        spdlog::warn("PyroWave: no Vulkan device matches the app's GPU UUIDs; streaming black frames");
        state->device = nullptr;
        if (pyrowave_create_default_device(&state->device) != PYROWAVE_SUCCESS)
        {
            state->device = nullptr;
            spdlog::error("PyroWave: no Vulkan device");
            return false;
        }
    }

    pyrowave_encoder_create_info info = {};
    info.device = state->device;
    info.width = static_cast<int>(width_);
    info.height = static_cast<int>(height_);
    info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    if (pyrowave_encoder_create(&info, &state->encoder) != PYROWAVE_SUCCESS)
    {
        state->encoder = nullptr;
        spdlog::error("PyroWave: encoder {}x{} creation failed", width_, height_);
        return false;
    }
    linuxState_ = state.release();
    spdlog::info("PyroWave: {}x{} @ {}Hz {}Mbps ({} bytes per frame)", width_, height_, fps_, bitrateMbps_,
                 static_cast<size_t>(bitrateMbps_) * 1'000'000 / 8 / fps_);
    return true;
}

void VideoEncoder::Shutdown()
{
    shuttingDown_.store(true);
    delete State(linuxState_);
    linuxState_ = nullptr;
    inFlightFrameCount_.store(0);
}

bool VideoEncoder::Encode(FrameImageSource imageSource, int64_t timestampNs, OnNalUnitCallback callback,
                          OnFrameEncodedCallback frameCallback)
{
    FrameSource frameSource = {};
    frameSource.left = std::move(imageSource);
    return EncodeInternal(std::move(frameSource), false, timestampNs, std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeStereo(FrameSource frameSource, int64_t timestampNs, OnNalUnitCallback callback,
                                OnFrameEncodedCallback frameCallback)
{
    return EncodeInternal(std::move(frameSource), true, timestampNs, std::move(callback), std::move(frameCallback));
}

bool VideoEncoder::EncodeInternal(FrameSource frameSource, bool /*stereo*/, int64_t timestampNs,
                                  OnNalUnitCallback callback, OnFrameEncodedCallback frameCallback)
{
    static std::atomic_bool loggedBlack{false};
    static std::atomic_bool loggedImport{false};

    LinuxPyroWaveState* state = State(linuxState_);
    if (state == nullptr)
    {
        return false;
    }
    Clock::time_point encodeStart = Clock::now();
    inFlightFrameCount_.fetch_add(1);
    VideoEncoder::FrameMetrics metrics = {};
    metrics.frameNumber = frameNumberCounter_.fetch_add(1) + 1;
    metrics.timestampNs = timestampNs;
    metrics.keyframe = true;

    pyrowave_rate_control rate = {};
    rate.maximum_bitstream_size = static_cast<size_t>(bitrateMbps_) * 1'000'000 / 8 / fps_;

    // Both eyes point at the packed image once LinuxPackVulkanFrame has run.
    const LinuxFrameImage* frame = Frame(frameSource.left);
    const ImportedImage* imported = nullptr;
    if (frame != nullptr && frame->packed && state->sameGpu)
    {
        imported = state->ImportSync(*frame) ? state->ImportImage(*frame) : nullptr;
        if (imported == nullptr && !loggedImport.exchange(true))
        {
            spdlog::warn("PyroWave: importing the packed eye image or its timeline failed; streaming black frames");
        }
    }
    else if (!loggedBlack.exchange(true))
    {
        spdlog::warn("PyroWave: the app's frames are not exported; streaming black frames");
    }

    Clock::time_point submitStart = Clock::now();
    pyrowave_result result = PYROWAVE_SUCCESS;
    if (imported != nullptr)
    {
        pyrowave_gpu_external_reference reference = {imported->image, VK_QUEUE_FAMILY_EXTERNAL};
        pyrowave_gpu_sync_operation acquire = {};
        acquire.images = &reference;
        acquire.num_images = 1;
        acquire.sync = {pyrowave_sync_object_get_semaphore(state->sync), frame->timelineValue};
        pyrowave_gpu_sync_operation release = {};
        release.images = &reference;
        release.num_images = 1;

        pyrowave_scaled_encode_info scaled = {};
        scaled.view = imported->view;
        scaled.input_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        scaled.output_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        scaled.intermediate_plane_format = VK_FORMAT_R8_UNORM;
        scaled.ycbcr_chroma_midpoint = 0.5f;
        result = pyrowave_encoder_encode_gpu_scaled_synchronous(state->encoder, &acquire, &release, &scaled, &rate);
    }
    else
    {
        result = EncodeBlack(state->encoder, width_, height_, rate);
    }
    size_t packetCount = 0;
    if (result == PYROWAVE_SUCCESS)
    {
        result = pyrowave_encoder_compute_num_packets(state->encoder, PacketBoundary, &packetCount);
    }
    size_t written = 0;
    if (result == PYROWAVE_SUCCESS)
    {
        state->packets.resize(packetCount);
        state->bitstream.resize(rate.maximum_bitstream_size + PacketBoundary);
        result = pyrowave_encoder_packetize(state->encoder, state->packets.data(), PacketBoundary, &written,
                                            state->bitstream.data(), state->bitstream.size());
    }
    metrics.encodeSubmitMs = ToMilliseconds(Clock::now() - submitStart);

    const bool emitted = result == PYROWAVE_SUCCESS && written > 0;
    if (emitted)
    {
        const pyrowave_packet& last = state->packets[written - 1];
        const size_t bytes = last.offset + last.size;
        if (metrics.frameNumber < 5 || metrics.frameNumber % 600 == 0)
        {
            spdlog::info("PyroWave: frame {} {} bytes (budget {}), {}, encode {:.2f} ms", metrics.frameNumber, bytes,
                         rate.maximum_bitstream_size, imported != nullptr ? "app frame" : "black",
                         metrics.encodeSubmitMs);
        }
        forceKeyframe_.store(false); // every PyroWave frame is intra-only
        if (callback)
        {
            callback(state->bitstream.data(), bytes, true, timestampNs);
        }
    }
    else
    {
        spdlog::warn("PyroWave: encode failed ({})", static_cast<int>(result));
    }
    frameCount_ += emitted ? 1 : 0;
    metrics.totalLatencyMs = ToMilliseconds(Clock::now() - encodeStart);
    inFlightFrameCount_.fetch_sub(1);
    if (!emitted)
    {
        droppedFrameCount_.fetch_add(1);
        metrics.frameDropped = true;
    }
    if (frameCallback)
    {
        frameCallback(metrics);
    }
    return emitted;
}

void VideoEncoder::ForceKeyframe()
{
    forceKeyframe_.store(true);
}

void VideoEncoder::SetBitrate(uint32_t bitrateMbps)
{
    bitrateMbps_ = bitrateMbps;
}

bool VideoEncoder::AcquireSlot(size_t& outSlotIndex)
{
    outSlotIndex = 0;
    return false;
}

void VideoEncoder::ReleaseSlot(size_t /*slotIndex*/) {}

void VideoEncoder::DestroySlots() {}
