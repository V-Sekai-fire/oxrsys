// SPDX-License-Identifier: MPL-2.0

// PyroWaveDecoder.swift — Decodes the runtime's PyroWave frames on the GPU into the
// NV12 video-range pixel buffers StereoRenderer draws.

import CoreMedia
import CoreVideo
import CPyroWave
import Foundation
import Metal

public final class PyroWaveDecoder: @unchecked Sendable {
    public typealias OnFrame = @Sendable (CVPixelBuffer, CMTime) -> Void

    private let lock = NSLock()
    private var onFrame: OnFrame?
    private var onDecodeErrorCallback: (@Sendable () -> Void)?
    private var decodeErrorCount = 0
    private var framesDecoded = 0

    private var mtl: MTLDevice?
    private var queue: MTLCommandQueue?
    private var packPipeline: MTLComputePipelineState?
    private var textureCache: CVMetalTextureCache?
    private var pool: CVPixelBufferPool?
    private var device: OpaquePointer?
    private var decoder: OpaquePointer?
    private var planes: [MTLTexture] = []
    private var width = 0
    private var height = 0

    /// Debug control: OXRSYS_PYROWAVE_CORRUPT_EVERY=N flips the first byte of every Nth frame,
    /// so the decode-error path can be seen end to end.
    private let corruptEvery = Int(ProcessInfo.processInfo.environment["OXRSYS_PYROWAVE_CORRUPT_EVERY"] ?? "") ?? 0

    public var onDecodeError: (@Sendable () -> Void)? {
        get { locked { onDecodeErrorCallback } }
        set { locked { onDecodeErrorCallback = newValue } }
    }

    public var totalDecodeErrors: Int { locked { decodeErrorCount } }

    public init() {}

    public func configure(callback: @escaping OnFrame) {
        locked { onFrame = callback }
    }

    public func decode(frameData: Data, presentationTimeNs: Int64) {
        var data = frameData
        let frameNumber = locked { () -> Int in
            framesDecoded += 1
            return framesDecoded
        }
        if corruptEvery > 0 && frameNumber % corruptEvery == 0 && !data.isEmpty {
            data[data.startIndex] ^= 0x01
        }
        guard let (frameWidth, frameHeight) = Self.frameSize(data) else {
            fail("no PyroWave sequence header")
            return
        }
        // 4:2:0 needs even sizes, so an odd one is a damaged header: the current decoder refuses it below.
        let validSize = frameWidth % 2 == 0 && frameHeight % 2 == 0
        if (frameWidth != width || frameHeight != height) && (validSize || decoder == nil) {
            guard setup(width: frameWidth, height: frameHeight) else {
                fail("cannot create a \(frameWidth)x\(frameHeight) decoder")
                return
            }
        }
        guard let decoder, let queue, let pool, let textureCache, let packPipeline else { return }

        let start = DispatchTime.now().uptimeNanoseconds
        pyrowave_decoder_clear(decoder)
        let pushed = data.withUnsafeBytes { raw in
            pyrowave_decoder_push_packet(decoder, raw.baseAddress, raw.count)
        }
        guard pushed == PYROWAVE_SUCCESS else {
            fail("corrupt bitstream (\(String(cString: pyrowave_result_to_string(pushed))))")
            return
        }
        guard pyrowave_decoder_decode_is_ready(decoder, false) else {
            fail("incomplete frame")
            return
        }

        var pixelBuffer: CVPixelBuffer?
        guard CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixelBuffer) == kCVReturnSuccess,
              let pixelBuffer,
              let lumaOut = Self.texture(textureCache, pixelBuffer, plane: 0, format: .r8Unorm),
              let chromaOut = Self.texture(textureCache, pixelBuffer, plane: 1, format: .rg8Unorm),
              let commandBuffer = queue.makeCommandBuffer() else {
            fail("no output buffer")
            return
        }

        var buffers = pyrowave_gpu_buffers(planes: (
            Unmanaged.passUnretained(planes[0] as AnyObject).toOpaque(),
            Unmanaged.passUnretained(planes[1] as AnyObject).toOpaque(),
            Unmanaged.passUnretained(planes[2] as AnyObject).toOpaque()))
        let decoded = pyrowave_decoder_decode_gpu_buffer(
            decoder, Unmanaged.passUnretained(commandBuffer as AnyObject).toOpaque(), &buffers)
        guard decoded == PYROWAVE_SUCCESS, let encoder = commandBuffer.makeComputeCommandEncoder() else {
            fail("decode submit failed")
            return
        }
        encoder.setComputePipelineState(packPipeline)
        for (index, texture) in (planes + [lumaOut, chromaOut]).enumerated() {
            encoder.setTexture(texture, index: index)
        }
        let group = MTLSize(width: 16, height: 16, depth: 1)
        encoder.dispatchThreadgroups(
            MTLSize(width: (width / 2 + 15) / 16, height: (height / 2 + 15) / 16, depth: 1),
            threadsPerThreadgroup: group)
        encoder.endEncoding()
        commandBuffer.commit()
        commandBuffer.waitUntilCompleted()

        let decodeMs = Double(DispatchTime.now().uptimeNanoseconds - start) / 1_000_000
        if frameNumber <= 5 || frameNumber % 600 == 0 {
            print("[PyroWave] frame \(frameNumber) \(data.count) bytes \(width)x\(height) decode \(String(format: "%.2f", decodeMs)) ms")
        }
        let callback = locked { onFrame }
        callback?(pixelBuffer, CMTime(value: presentationTimeNs, timescale: 1_000_000_000))
    }

    public func invalidate() {
        locked {
            if let decoder { pyrowave_decoder_destroy(decoder) }
            if let device { pyrowave_device_destroy(device) }
            decoder = nil
            device = nil
            planes = []
            pool = nil
            width = 0
            height = 0
        }
    }

    // MARK: - Setup

    private func setup(width frameWidth: Int, height frameHeight: Int) -> Bool {
        guard let mtl = mtl ?? MTLCreateSystemDefaultDevice() else { return false }
        self.mtl = mtl
        if queue == nil { queue = mtl.makeCommandQueue() }
        if packPipeline == nil {
            guard let library = try? mtl.makeLibrary(source: Self.packSource, options: nil),
                  let function = library.makeFunction(name: "pyrowave_pack_nv12"),
                  let pipeline = try? mtl.makeComputePipelineState(function: function) else { return false }
            packPipeline = pipeline
        }
        if textureCache == nil {
            var cache: CVMetalTextureCache?
            CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, mtl, nil, &cache)
            textureCache = cache
        }

        var createInfo = pyrowave_device_create_info()
        createInfo.mtl_device = Unmanaged.passUnretained(mtl as AnyObject).toOpaque()
        var newDevice: OpaquePointer?
        guard pyrowave_device_create(&createInfo, &newDevice) == PYROWAVE_SUCCESS, let newDevice else { return false }
        var decoderInfo = pyrowave_decoder_create_info(
            device: newDevice, width: Int32(frameWidth), height: Int32(frameHeight),
            chroma: PYROWAVE_CHROMA_SUBSAMPLING_420)
        var newDecoder: OpaquePointer?
        guard pyrowave_decoder_create(&decoderInfo, &newDecoder) == PYROWAVE_SUCCESS, let newDecoder else {
            pyrowave_device_destroy(newDevice)
            return false
        }

        let discard = {
            pyrowave_decoder_destroy(newDecoder)
            pyrowave_device_destroy(newDevice)
        }
        var newPlanes: [MTLTexture] = []
        for (planeWidth, planeHeight) in [(frameWidth, frameHeight), (frameWidth / 2, frameHeight / 2), (frameWidth / 2, frameHeight / 2)] {
            let desc = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .r8Unorm, width: planeWidth, height: planeHeight, mipmapped: false)
            desc.usage = [.shaderRead, .shaderWrite]
            desc.storageMode = .private
            guard let texture = mtl.makeTexture(descriptor: desc) else {
                discard()
                return false
            }
            newPlanes.append(texture)
        }

        let attributes: [String: Any] = [
            kCVPixelBufferWidthKey as String: frameWidth,
            kCVPixelBufferHeightKey as String: frameHeight,
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
            kCVPixelBufferMetalCompatibilityKey as String: true,
        ]
        var newPool: CVPixelBufferPool?
        guard CVPixelBufferPoolCreate(kCFAllocatorDefault, nil, attributes as CFDictionary, &newPool) == kCVReturnSuccess else {
            discard()
            return false
        }

        let (oldDecoder, oldDevice) = locked { (decoder, device) }
        if let oldDecoder { pyrowave_decoder_destroy(oldDecoder) }
        if let oldDevice { pyrowave_device_destroy(oldDevice) }
        locked {
            device = newDevice
            decoder = newDecoder
            planes = newPlanes
            pool = newPool
            width = frameWidth
            height = frameHeight
        }
        print("[PyroWave] decoder \(frameWidth)x\(frameHeight)")
        return true
    }

    private func fail(_ reason: String) {
        let (count, callback) = locked { () -> (Int, (@Sendable () -> Void)?) in
            decodeErrorCount += 1
            return (decodeErrorCount, onDecodeErrorCallback)
        }
        if count <= 10 || count % 100 == 0 {
            print("[PyroWave] decode error #\(count): \(reason)")
        }
        callback?()
    }

    private func locked<T>(_ body: () throws -> T) rethrows -> T {
        lock.lock()
        defer { lock.unlock() }
        return try body()
    }

    /// Luma size from the frame's leading sequence header (14-bit width and height minus one).
    static func frameSize(_ data: Data) -> (Int, Int)? {
        guard data.count >= 8 else { return nil }
        let word = data.withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }.littleEndian
        guard word >> 31 == 1 else { return nil }
        return (Int(word & 0x3FFF) + 1, Int((word >> 14) & 0x3FFF) + 1)
    }

    private static func texture(_ cache: CVMetalTextureCache, _ buffer: CVPixelBuffer, plane: Int,
                                format: MTLPixelFormat) -> MTLTexture? {
        var cvTexture: CVMetalTexture?
        let attributes = [kCVMetalTextureUsage as String: MTLTextureUsage([.shaderRead, .shaderWrite]).rawValue] as CFDictionary
        guard CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cache, buffer, attributes, format,
            CVPixelBufferGetWidthOfPlane(buffer, plane), CVPixelBufferGetHeightOfPlane(buffer, plane),
            plane, &cvTexture) == kCVReturnSuccess, let cvTexture else { return nil }
        return CVMetalTextureGetTexture(cvTexture)
    }

    // One thread per chroma sample: copies its 2x2 luma block and interleaves Cb and Cr.
    private static let packSource = """
    #include <metal_stdlib>
    using namespace metal;
    kernel void pyrowave_pack_nv12(texture2d<float, access::read> y [[texture(0)]],
                                   texture2d<float, access::read> cb [[texture(1)]],
                                   texture2d<float, access::read> cr [[texture(2)]],
                                   texture2d<float, access::write> lumaOut [[texture(3)]],
                                   texture2d<float, access::write> chromaOut [[texture(4)]],
                                   uint2 gid [[thread_position_in_grid]])
    {
        if (gid.x >= chromaOut.get_width() || gid.y >= chromaOut.get_height()) { return; }
        for (uint dy = 0; dy < 2; dy++)
            for (uint dx = 0; dx < 2; dx++)
            {
                uint2 p = gid * 2 + uint2(dx, dy);
                lumaOut.write(y.read(p), p);
            }
        chromaOut.write(float4(cb.read(gid).r, cr.read(gid).r, 0.0, 0.0), gid);
    }
    """
}
