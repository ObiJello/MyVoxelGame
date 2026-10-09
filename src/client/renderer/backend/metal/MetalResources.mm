// File: src/client/renderer/backend/metal/MetalResources.mm
//
// The Metal backend's buffers, textures (and their batched updates and
// per-frame copies), samplers, meshes and render targets.
#ifdef HAS_METAL

#import "MetalBackend.hpp"

#include "common/core/Log.hpp"
#include "common/core/Profiling_Tracy.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace Render {

    namespace {
        struct FormatInfo {
            MTLPixelFormat format;
            uint32_t bytesPerPixel;
        };
        FormatInfo ToMetalFormat(TextureFormat format) {
            switch (format) {
                case TextureFormat::RGBA8:           return {MTLPixelFormatRGBA8Unorm, 4};
                case TextureFormat::SRGB8_A8:        return {MTLPixelFormatRGBA8Unorm_sRGB, 4};
                // Apple GPUs have no 24-bit depth; every Mac GPU has D32F + S8.
                case TextureFormat::Depth24Stencil8: return {MTLPixelFormatDepth32Float_Stencil8, 8};
                case TextureFormat::RGBA16F:         return {MTLPixelFormatRGBA16Float, 8};
                case TextureFormat::RGBA32F:         return {MTLPixelFormatRGBA32Float, 16};
                case TextureFormat::RGBA16:          return {MTLPixelFormatRGBA16Unorm, 8};
                case TextureFormat::RGBA16UI:        return {MTLPixelFormatRGBA16Uint, 8};
                case TextureFormat::R11G11B10F:      return {MTLPixelFormatRG11B10Float, 4};
            }
            return {MTLPixelFormatRGBA8Unorm, 4};
        }
        bool IsDepthFormat(MTLPixelFormat f) {
            return f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatDepth32Float;
        }
    }

    void MetalBackend::TrackAlloc(size_t& bucket, size_t bytes, size_t& count) {
        bucket += bytes;
        m_memStats.totalAllocated += bytes;
        ++count;
        m_memStats.peakUsage = std::max(m_memStats.peakUsage, m_memStats.totalAllocated);
    }

    // ========================================================================
    // UPLOADS OUTSIDE THE FRAME
    // ========================================================================

    id MetalBackend::UploadBlit() {
        // One command buffer of uploads at a time, committed by FlushUploads
        // before the frame's own: Metal runs a queue's command buffers in
        // commit order, so every upload made while recording a frame lands
        // before that frame's draws. Nothing waits for it.
        if (m_metal4) return M4UploadEncoder();
        if (!m_uploadCmd) m_uploadCmd = MakeCommandBuffer(@"Uploads", /*watchErrors=*/true);
        if (!m_uploadBlit) m_uploadBlit = MakeBlitEncoder(m_uploadCmd, @"Uploads");
        return m_uploadBlit;
    }

    void MetalBackend::FlushUploads() {
        if (m_metal4) {
            M4FlushUploads();
            return;
        }
        if (m_uploadBlit) {
            [m_uploadBlit endEncoding];
            m_uploadBlit = nil;
        }
        if (m_uploadCmd) {
            [m_uploadCmd commit];
            m_uploadCmd = nil;
        }
    }

    // ========================================================================
    // BUFFERS
    // ========================================================================

    BufferHandle MetalBackend::CreateBuffer(BufferUsage usage, size_t size, const void* data, BufferAccess access) {
        if (!m_device) return INVALID_BUFFER;
        @autoreleasepool {
            const size_t length = std::max<size_t>(size, 16);
            BufferInfo info;
            info.size = size;
            info.usage = usage;
            if (access == BufferAccess::Static && data != nullptr) {
                // GPU-only: private storage, filled by a blit from a staging
                // copy on the upload command buffer — no wait (Vulkan's old
                // drain here was 5-6 ms per mid-game sign or held-item mesh).
                PROFILE_ZONE_N("Mtl.CreateBuffer.StaticUpload");
                info.buffer = [m_device newBufferWithLength:length options:MTLResourceStorageModePrivate];
                id<MTLBuffer> staging = [m_device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
                if (!info.buffer || !staging) {
                    Log::Error("MetalBackend: failed to create a %zu-byte buffer", size);
                    return INVALID_BUFFER;
                }
                staging.label = @"Upload staging";
                m_counters.uploadBytes += size;
                M4Resident(info.buffer);
                M4ResidentTransient(staging);
                M4Release(staging);   // gone once the uploads ran
                [UploadBlit() copyFromBuffer:staging sourceOffset:0 toBuffer:info.buffer destinationOffset:0 size:size];
            } else {
                // CPU-written (Dynamic / Streaming, or Static without data):
                // persistently visible; UpdateBuffer is a memcpy.
                info.buffer = [m_device newBufferWithLength:length options:m_hostVisibleOptions];
                if (!info.buffer) {
                    Log::Error("MetalBackend: failed to create a %zu-byte host-visible buffer", size);
                    return INVALID_BUFFER;
                }
                info.hostVisible = true;
                M4Resident(info.buffer);
                if (data != nullptr) {
                    std::memcpy(info.buffer.contents, data, size);
                    if (m_managedBuffers) [info.buffer didModifyRange:NSMakeRange(0, size)];
                }
            }
            const uint32_t handle = AllocHandle();
            // A default name for the tools until SetDebugLabel gives a real
            // one; the driver's allocation is what the memory stats count.
            static const char* const kUsage[] = {"Vertex", "Index", "Uniform", "Storage", "Indirect"};
            static const char* const kAccess[] = {"Static", "Dynamic", "Streaming"};
            const int u = static_cast<int>(usage), a = static_cast<int>(access);
            info.label = "Buf#" + std::to_string(handle) + " " +
                         (u >= 0 && u < 5 ? kUsage[u] : "?") + " " + (a >= 0 && a < 3 ? kAccess[a] : "?") + " " +
                         std::to_string((size + 1023) / 1024) + " KB";
            info.buffer.label = [NSString stringWithUTF8String:info.label.c_str()];
            info.allocated = info.buffer.allocatedSize;
            m_buffers[handle] = info;
            TrackAlloc(m_memStats.bufferMemory, info.allocated, m_memStats.bufferCount);
            return handle;
        }
    }

    void MetalBackend::UpdateBuffer(BufferHandle handle, size_t offset, size_t size, const void* data) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end() || size == 0 || data == nullptr) return;
        BufferInfo& info = it->second;
        if (offset + size > info.size) {
            Log::Error("MetalBackend: UpdateBuffer range [%zu, %zu) exceeds buffer size %zu",
                       offset, offset + size, info.size);
            return;
        }
        if (info.hostVisible) {
            // The write is the whole job — the same contract as Vulkan's
            // mapped buffers: a caller that rewrites a range a queued frame
            // still reads must version it.
            std::memcpy(static_cast<uint8_t*>(info.buffer.contents) + offset, data, size);
            if (m_managedBuffers) [info.buffer didModifyRange:NSMakeRange(offset, size)];
            return;
        }
        // A private (Static) buffer: staged and blitted, ordered after the
        // frames already committed by Metal's hazard tracking. Never meant to
        // be updated — said once, as Vulkan says it.
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            Log::Warning("MetalBackend: UpdateBuffer on a Static buffer - staged copy; create it as Dynamic instead");
        }
        @autoreleasepool {
            id<MTLBuffer> staging = [m_device newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
            M4ResidentTransient(staging);
            M4Release(staging);
            [UploadBlit() copyFromBuffer:staging sourceOffset:0 toBuffer:info.buffer destinationOffset:offset size:size];
        }
    }

    void MetalBackend::UpdateBufferUnsynchronized(BufferHandle handle, size_t offset, size_t size, const void* data) {
        // UpdateBuffer is already a bare memcpy into visible memory.
        UpdateBuffer(handle, offset, size, data);
    }

    void MetalBackend::DestroyBuffer(BufferHandle handle) {
        auto it = m_buffers.find(handle);
        if (it == m_buffers.end()) return;
        if (m_boundUniformBuffer == handle) m_boundUniformBuffer = INVALID_BUFFER;
        // A command buffer still using it keeps its own reference: destroying
        // now is safe, whatever is in flight.
        m_memStats.bufferMemory -= std::min(m_memStats.bufferMemory, it->second.size);
        m_memStats.totalAllocated -= std::min(m_memStats.totalAllocated, it->second.size);
        if (m_memStats.bufferCount > 0) --m_memStats.bufferCount;
        M4Release(it->second.buffer);   // Metal 4 command buffers hold no reference: after its frames
        m_buffers.erase(it);
    }

    void MetalBackend::BindUniformBuffer(BufferHandle handle, size_t offset, size_t /*size*/) {
        if (m_buffers.find(handle) == m_buffers.end()) {
            m_boundUniformBuffer = INVALID_BUFFER;
            return;
        }
        m_boundUniformBuffer = handle;
        m_boundUniformOffset = offset;
    }

    const void* MetalBackend::DebugGetMappedBufferPtr(BufferHandle handle) const {
        auto it = m_buffers.find(handle);
        return it != m_buffers.end() && it->second.hostVisible ? it->second.buffer.contents : nullptr;
    }

    // ========================================================================
    // TEXTURES
    // ========================================================================

    namespace {
        // A private, sampled texture — the shape every owned texture here
        // has, and what a reallocation (ReserveTextureMipLevels) or a
        // per-frame copy (PromoteToFrameCopies) must reproduce exactly: a
        // 2D array stays a 2D array.
        MTLTextureDescriptor* SampledTextureDescriptor(MTLPixelFormat format, int width, int height,
                                                       uint32_t mipLevels, uint32_t layers) {
            MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                             width:static_cast<NSUInteger>(width)
                                                                                            height:static_cast<NSUInteger>(height)
                                                                                         mipmapped:NO];
            desc.mipmapLevelCount = mipLevels;
            if (layers > 1) {
                desc.textureType = MTLTextureType2DArray;
                desc.arrayLength = layers;
            }
            desc.storageMode = MTLStorageModePrivate;
            desc.usage = MTLTextureUsageShaderRead;
            return desc;
        }
    }

    TextureHandle MetalBackend::CreateTexture2D(int width, int height, TextureFormat format, const void* data) {
        return CreateTextureImpl(width, height, format, data, 1);
    }

    TextureHandle MetalBackend::CreateTexture2DArray(int width, int height, int layers, int mipLevels,
                                                    TextureFormat format) {
        if (!m_device || width <= 0 || height <= 0 || layers <= 0 || mipLevels <= 0) return INVALID_TEXTURE;
        if (format != TextureFormat::RGBA8) {
            Log::Error("MetalBackend::CreateTexture2DArray: only RGBA8 arrays are supported");
            return INVALID_TEXTURE;
        }
        PROFILE_ZONE_N("Mtl.CreateTexture2DArray");
        @autoreleasepool {
            const FormatInfo f = ToMetalFormat(format);
            MTLTextureDescriptor* desc = SampledTextureDescriptor(f.format, width, height,
                                                                  static_cast<uint32_t>(mipLevels),
                                                                  static_cast<uint32_t>(layers));
            TextureInfo info;
            info.texture = M4HeapTexture(desc) ?: [m_device newTextureWithDescriptor:desc];
            if (!info.texture) {
                Log::Error("MetalBackend: failed to create a %dx%dx%d texture array", width, height, layers);
                return INVALID_TEXTURE;
            }
            info.width = width;
            info.height = height;
            info.mipLevels = static_cast<uint32_t>(mipLevels);
            info.layers = static_cast<uint32_t>(layers);
            info.format = f.format;
            info.bytesPerPixel = f.bytesPerPixel;
            M4Resident(info.texture);
            size_t bytes = 0;
            for (uint32_t level = 0; level < info.mipLevels; ++level) {
                bytes += static_cast<size_t>(std::max(1, width >> level)) * static_cast<size_t>(std::max(1, height >> level)) *
                         f.bytesPerPixel;
            }
            bytes *= info.layers;
            info.memorySize = bytes;
            // The whole chain reachable from the start, as a 2D texture's
            // SetTextureFilter would set it.
            info.lodMax = static_cast<float>(info.mipLevels - 1);
            UpdateSampler(info);
            info.lastUsedFrame = m_frameNumber;
            const uint32_t handle = AllocHandle();
            info.label = "TexArr#" + std::to_string(handle) + " " + std::to_string(width) + "x" + std::to_string(height) +
                         "x" + std::to_string(layers) + " L" + std::to_string(mipLevels);
            info.texture.label = [NSString stringWithUTF8String:info.label.c_str()];
            m_textures[handle] = info;
            TrackAlloc(m_memStats.textureMemory, bytes, m_memStats.textureCount);
            return handle;
        }
    }

    void MetalBackend::UploadTextureArrayLevel(TextureHandle handle, int level, int width, int height, int layers,
                                               const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !data || !it->second.texture) return;
        TextureInfo& tex = it->second;
        if (level < 0 || static_cast<uint32_t>(level) >= tex.mipLevels) return;
        if (width <= 0 || height <= 0 || layers <= 0 || static_cast<uint32_t>(layers) > tex.layers) return;
        tex.lastUsedFrame = m_frameNumber;
        @autoreleasepool {
            // One staging buffer of every layer's image back to back, one
            // blit copy per slice out of it (a buffer-to-texture copy
            // addresses a single slice), all queued ahead of the frame's
            // draws like UploadTextureRegionNow's.
            const size_t rowBytes = static_cast<size_t>(width) * tex.bytesPerPixel;
            const size_t imageBytes = rowBytes * static_cast<size_t>(height);
            const size_t totalBytes = imageBytes * static_cast<size_t>(layers);
            id<MTLBuffer> staging = [m_device newBufferWithBytes:data length:totalBytes
                                                         options:MTLResourceStorageModeShared];
            staging.label = @"Upload staging (array)";
            m_counters.uploadBytes += totalBytes;
            M4ResidentTransient(staging);
            M4Release(staging);
            id blit = UploadBlit();
            auto copyInto = [&](id<MTLTexture> target) {
                for (int layer = 0; layer < layers; ++layer) {
                    [blit copyFromBuffer:staging
                            sourceOffset:imageBytes * static_cast<size_t>(layer)
                       sourceBytesPerRow:rowBytes
                     sourceBytesPerImage:imageBytes
                              sourceSize:MTLSizeMake(static_cast<NSUInteger>(width), static_cast<NSUInteger>(height), 1)
                               toTexture:target
                        destinationSlice:static_cast<NSUInteger>(layer)
                        destinationLevel:static_cast<NSUInteger>(level)
                       destinationOrigin:MTLOriginMake(0, 0, 0)];
                }
            };
            if (tex.frameCopies.empty()) copyInto(tex.texture);
            for (const TextureInfo::FrameCopy& c : tex.frameCopies) copyInto(c.texture);
        }
    }

    TextureHandle MetalBackend::CreateEmptyTexture2D(int width, int height, TextureFormat format, int maxLevel) {
        return CreateTextureImpl(width, height, format, nullptr, static_cast<uint32_t>(std::max(0, maxLevel)) + 1u);
    }

    TextureHandle MetalBackend::CreateTextureImpl(int width, int height, TextureFormat format, const void* data,
                                                  uint32_t mipLevels) {
        if (!m_device || width <= 0 || height <= 0) return INVALID_TEXTURE;
        PROFILE_ZONE_N("Mtl.CreateTexture2D");
        @autoreleasepool {
            const FormatInfo f = ToMetalFormat(format);
            MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f.format
                                                                                             width:static_cast<NSUInteger>(width)
                                                                                            height:static_cast<NSUInteger>(height)
                                                                                         mipmapped:NO];
            desc.mipmapLevelCount = mipLevels;
            desc.storageMode = MTLStorageModePrivate;
            desc.usage = MTLTextureUsageShaderRead;
            if (IsDepthFormat(f.format)) desc.usage |= MTLTextureUsageRenderTarget;
            TextureInfo info;
            info.texture = M4HeapTexture(desc) ?: [m_device newTextureWithDescriptor:desc];
            if (!info.texture) {
                Log::Error("MetalBackend: failed to create a %dx%d texture", width, height);
                return INVALID_TEXTURE;
            }
            info.width = width;
            info.height = height;
            info.mipLevels = mipLevels;
            info.format = f.format;
            info.bytesPerPixel = f.bytesPerPixel;
            M4Resident(info.texture);
            size_t bytes = 0;
            for (uint32_t level = 0; level < mipLevels; ++level) {
                bytes += static_cast<size_t>(std::max(1, width >> level)) * static_cast<size_t>(std::max(1, height >> level)) *
                         f.bytesPerPixel;
            }
            info.memorySize = bytes;
            if (data && !IsDepthFormat(f.format)) {
                const size_t rowBytes = static_cast<size_t>(width) * f.bytesPerPixel;
                const size_t imageBytes = rowBytes * static_cast<size_t>(height);
                id<MTLBuffer> staging = [m_device newBufferWithBytes:data length:imageBytes
                                                             options:MTLResourceStorageModeShared];
                staging.label = @"Upload staging";
                m_counters.uploadBytes += imageBytes;
                M4ResidentTransient(staging);
                M4Release(staging);
                [UploadBlit() copyFromBuffer:staging
                                sourceOffset:0
                           sourceBytesPerRow:rowBytes
                         sourceBytesPerImage:imageBytes
                                  sourceSize:MTLSizeMake(static_cast<NSUInteger>(width), static_cast<NSUInteger>(height), 1)
                                   toTexture:info.texture
                            destinationSlice:0
                            destinationLevel:0
                           destinationOrigin:MTLOriginMake(0, 0, 0)];
            }
            UpdateSampler(info);
            info.lastUsedFrame = m_frameNumber;
            const uint32_t handle = AllocHandle();
            info.label = "Tex#" + std::to_string(handle) + " " + std::to_string(width) + "x" + std::to_string(height) +
                         " L" + std::to_string(mipLevels);
            info.texture.label = [NSString stringWithUTF8String:info.label.c_str()];
            m_textures[handle] = info;
            TrackAlloc(m_memStats.textureMemory, bytes, m_memStats.textureCount);
            return handle;
        }
    }

    void MetalBackend::EraseTextureEntry(TextureHandle handle) {
        for (uint32_t slot = 0; slot < kMaxTextureSlots; ++slot) {
            if (m_boundTextures[slot] == handle) {
                m_boundTextures[slot] = INVALID_TEXTURE;
                m_boundTextureInfo[slot] = nullptr;
            }
        }
        m_textures.erase(handle);
    }

    TextureHandle MetalBackend::WrapTexture(id<MTLTexture> texture, MTLSamplerMinMagFilter filter) {
        // A texture entry over an image owned elsewhere (a render target's
        // colour, the frame's depth, an OIT image): sampled, never freed here.
        TextureInfo info;
        info.texture = texture;
        info.width = static_cast<int>(texture.width);
        info.height = static_cast<int>(texture.height);
        info.format = texture.pixelFormat;
        info.owned = false;
        info.minFilter = filter;
        info.magFilter = filter;
        info.mipFilter = MTLSamplerMipFilterNotMipmapped;
        UpdateSampler(info);
        const uint32_t handle = AllocHandle();
        m_textures[handle] = info;
        return handle;
    }

    id<MTLSamplerState> MetalBackend::SamplerFor(MTLSamplerMinMagFilter minF, MTLSamplerMinMagFilter magF,
                                                 MTLSamplerMipFilter mipF, MTLSamplerAddressMode s,
                                                 MTLSamplerAddressMode t, float aniso, float lodMax) {
        const uint32_t anisoInt = static_cast<uint32_t>(std::clamp(aniso, 1.0f, m_maxAnisotropy));
        const uint32_t lodInt = static_cast<uint32_t>(std::clamp(lodMax, 0.0f, 255.0f));
        const uint64_t key = static_cast<uint64_t>(minF) | (static_cast<uint64_t>(magF) << 2) |
                             (static_cast<uint64_t>(mipF) << 4) | (static_cast<uint64_t>(s) << 8) |
                             (static_cast<uint64_t>(t) << 12) | (static_cast<uint64_t>(anisoInt) << 16) |
                             (static_cast<uint64_t>(lodInt) << 24);
        auto it = m_samplers.find(key);
        if (it != m_samplers.end()) return it->second;
        @autoreleasepool {
            MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
            d.minFilter = minF;
            d.magFilter = magF;
            d.mipFilter = mipF;
            d.sAddressMode = s;
            d.tAddressMode = t;
            d.rAddressMode = t;
            d.maxAnisotropy = anisoInt;
            d.lodMinClamp = 0.0f;
            d.lodMaxClamp = static_cast<float>(lodInt);
            d.normalizedCoordinates = YES;
            d.label = [NSString stringWithFormat:@"Sampler %s/%s mip %s %s aniso %u lod %u",
                                                 minF == MTLSamplerMinMagFilterLinear ? "linear" : "nearest",
                                                 magF == MTLSamplerMinMagFilterLinear ? "linear" : "nearest",
                                                 mipF == MTLSamplerMipFilterNotMipmapped ? "none"
                                                 : mipF == MTLSamplerMipFilterLinear ? "linear" : "nearest",
                                                 s == MTLSamplerAddressModeRepeat ? "repeat" : "clamp", anisoInt, lodInt];
            id<MTLSamplerState> sampler = [m_device newSamplerStateWithDescriptor:d];
            m_samplers[key] = sampler;
            return sampler;
        }
    }

    void MetalBackend::UpdateSampler(TextureInfo& tex) {
        tex.sampler = SamplerFor(tex.minFilter, tex.magFilter, tex.mipFilter, tex.wrapS, tex.wrapT,
                                 tex.maxAnisotropy, tex.lodMax);
    }

    void MetalBackend::SetTextureFilter(TextureHandle handle, TextureFilter min, TextureFilter mag) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;
        TextureInfo& tex = it->second;
        // The *_MIPMAP_* modes carry both the in-level filter and the
        // between-level one (as on Vulkan).
        switch (min) {
            case TextureFilter::NearestMipmapNearest:
                tex.minFilter = MTLSamplerMinMagFilterNearest; tex.mipFilter = MTLSamplerMipFilterNearest; break;
            case TextureFilter::NearestMipmapLinear:
                tex.minFilter = MTLSamplerMinMagFilterNearest; tex.mipFilter = MTLSamplerMipFilterLinear;  break;
            case TextureFilter::LinearMipmapNearest:
                tex.minFilter = MTLSamplerMinMagFilterLinear;  tex.mipFilter = MTLSamplerMipFilterNearest; break;
            case TextureFilter::LinearMipmapLinear:
                tex.minFilter = MTLSamplerMinMagFilterLinear;  tex.mipFilter = MTLSamplerMipFilterLinear;  break;
            case TextureFilter::Linear:
                tex.minFilter = MTLSamplerMinMagFilterLinear;  tex.mipFilter = MTLSamplerMipFilterNearest; break;
            default:
                tex.minFilter = MTLSamplerMinMagFilterNearest; tex.mipFilter = MTLSamplerMipFilterNearest; break;
        }
        tex.magFilter = mag == TextureFilter::Linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        // From here the sampler reaches the whole chain (a single-level
        // texture keeps sampling level 0).
        tex.lodMax = static_cast<float>(tex.mipLevels - 1);
        UpdateSampler(tex);
    }

    void MetalBackend::SetTextureWrap(TextureHandle handle, TextureWrap s, TextureWrap t) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;
        auto toMtl = [](TextureWrap w) {
            switch (w) {
                case TextureWrap::Repeat:         return MTLSamplerAddressModeRepeat;
                case TextureWrap::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
                default:                          return MTLSamplerAddressModeClampToEdge;
            }
        };
        it->second.wrapS = toMtl(s);
        it->second.wrapT = toMtl(t);
        it->second.lodMax = static_cast<float>(it->second.mipLevels - 1);
        UpdateSampler(it->second);
    }

    void MetalBackend::SetTextureAnisotropy(TextureHandle handle, float maxAnisotropy) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;
        const float value = std::clamp(maxAnisotropy, 1.0f, m_maxAnisotropy);
        if (it->second.maxAnisotropy == value) return;
        it->second.maxAnisotropy = value;
        UpdateSampler(it->second);
    }

    void MetalBackend::GenerateMipmaps(TextureHandle /*handle*/) {
        // Intentionally a no-op, as on Vulkan: the block atlas — the only
        // texture whose mips matter — authors its chain on the CPU with MC's
        // algorithm (ReserveTextureMipLevels + UploadTextureMipLevel); a
        // driver box filter would be wrong for cutout sprites.
    }

    void MetalBackend::ReserveTextureMipLevels(TextureHandle handle, int maxLevel) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !it->second.owned) return;
        TextureInfo& tex = it->second;
        const uint32_t wanted = static_cast<uint32_t>(std::max(0, maxLevel)) + 1u;
        if (wanted == tex.mipLevels) return;
        @autoreleasepool {
            // A texture's mip count is fixed at creation: a new one, contents
            // undefined (every level is uploaded after reserving).
            MTLTextureDescriptor* desc = SampledTextureDescriptor(tex.format, tex.width, tex.height, wanted, tex.layers);
            id<MTLTexture> texture = M4HeapTexture(desc) ?: [m_device newTextureWithDescriptor:desc];
            if (!texture) {
                Log::Error("MetalBackend: failed to reallocate a texture for %u mip levels", wanted);
                return;
            }
            M4Resident(texture);
            M4Release(tex.texture);
            for (const TextureInfo::FrameCopy& c : tex.frameCopies) {
                if (c.texture != tex.texture) M4Release(c.texture);
            }
            // Anything queued against the old image is for the old chain.
            m_pendingTextureUpdates.erase(
                std::remove_if(m_pendingTextureUpdates.begin(), m_pendingTextureUpdates.end(),
                               [&](const PendingTextureUpdate& u) { return u.texture == handle; }),
                m_pendingTextureUpdates.end());
            const size_t copyBytes = tex.frameCopies.empty() ? 0 : tex.memorySize * (tex.frameCopies.size() - 1);
            tex.frameCopies.clear();
            tex.carried.clear();
            m_frameCopyTextures.erase(std::remove(m_frameCopyTextures.begin(), m_frameCopyTextures.end(), handle),
                                      m_frameCopyTextures.end());
            tex.texture = texture;
            tex.mipLevels = wanted;
            tex.lodMax = static_cast<float>(wanted - 1);
            if (!tex.label.empty()) texture.label = [NSString stringWithUTF8String:tex.label.c_str()];
            UpdateSampler(tex);
            size_t bytes = 0;
            for (uint32_t level = 0; level < wanted; ++level) {
                bytes += static_cast<size_t>(std::max(1, tex.width >> level)) *
                         static_cast<size_t>(std::max(1, tex.height >> level)) * tex.bytesPerPixel;
            }
            bytes *= std::max(1u, tex.layers);
            const size_t old = tex.memorySize + copyBytes;
            m_memStats.textureMemory = m_memStats.textureMemory - std::min(m_memStats.textureMemory, old) + bytes;
            m_memStats.totalAllocated = m_memStats.totalAllocated - std::min(m_memStats.totalAllocated, old) + bytes;
            m_memStats.peakUsage = std::max(m_memStats.peakUsage, m_memStats.totalAllocated);
            tex.memorySize = bytes;
        }
    }

    void MetalBackend::UploadTextureMipLevel(TextureHandle handle, int level, int width, int height, const void* data) {
        UploadTextureRegionNow(handle, level, 0, 0, width, height, data);
    }

    void MetalBackend::UploadTextureRegionNow(TextureHandle handle, int level, int x, int y, int width, int height,
                                              const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !data || !it->second.texture) return;
        TextureInfo& tex = it->second;
        if (level < 0 || static_cast<uint32_t>(level) >= tex.mipLevels) return;
        if (width <= 0 || height <= 0 || x < 0 || y < 0) return;
        tex.lastUsedFrame = m_frameNumber;
        @autoreleasepool {
            // The pixels are copied out now (the caller's buffer is free) and
            // the copy is queued ahead of the frame's draws; a texture with
            // per-frame copies takes them in every copy.
            const size_t rowBytes = static_cast<size_t>(width) * tex.bytesPerPixel;
            const size_t imageBytes = rowBytes * static_cast<size_t>(height);
            id<MTLBuffer> staging = [m_device newBufferWithBytes:data length:imageBytes
                                                         options:MTLResourceStorageModeShared];
            staging.label = @"Upload staging";
            m_counters.uploadBytes += imageBytes;
            M4ResidentTransient(staging);
            M4Release(staging);
            id blit = UploadBlit();
            auto copyInto = [&](id<MTLTexture> target) {
                [blit copyFromBuffer:staging
                        sourceOffset:0
                   sourceBytesPerRow:rowBytes
                 sourceBytesPerImage:imageBytes
                          sourceSize:MTLSizeMake(static_cast<NSUInteger>(width), static_cast<NSUInteger>(height), 1)
                           toTexture:target
                    destinationSlice:0
                    destinationLevel:static_cast<NSUInteger>(level)
                   destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(x), static_cast<NSUInteger>(y), 0)];
            };
            if (tex.frameCopies.empty()) copyInto(tex.texture);
            for (const TextureInfo::FrameCopy& c : tex.frameCopies) copyInto(c.texture);
        }
    }

    void MetalBackend::UpdateTexture2D(TextureHandle handle, int x, int y, int width, int height, const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !data) return;
        it->second.lastUsedFrame = m_frameNumber;
        QueueTextureUpdate(handle, 0, x, y, width, height, data);
    }

    void MetalBackend::UpdateTexture2DLevel(TextureHandle handle, int level, int x, int y, int width, int height,
                                            const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !data) return;
        if (level < 0 || static_cast<uint32_t>(level) >= it->second.mipLevels) return;
        it->second.lastUsedFrame = m_frameNumber;
        // Animated sprites call this mid-frame: the copy rides the next
        // frame's command buffer rather than anything waiting.
        QueueTextureUpdate(handle, static_cast<uint32_t>(level), x, y, width, height, data);
    }

    uint8_t* MetalBackend::ReserveStaging(size_t bytes, size_t& outOffset) {
        // The slot the NEXT BeginFrame flushes (see VKBackend::m_texStaging
        // for why writing here, before that frame's wait, is safe).
        StagingSlot& slot = m_staging[static_cast<size_t>((m_frameNumber + 1) % kStagingSlots)];
        const size_t required = slot.used + bytes;
        if (!slot.buffer || required > slot.capacity) {
            // Grow, keeping what this slot already holds for the coming flush.
            const size_t capacity = std::max(required, std::max(slot.capacity * 2, static_cast<size_t>(256 * 1024)));
            id<MTLBuffer> buffer = [m_device newBufferWithLength:capacity
                                                         options:MTLResourceStorageModeShared |
                                                                 MTLResourceCPUCacheModeWriteCombined];
            if (!buffer) {
                Log::Error("MetalBackend: failed to grow texture staging to %zu bytes", capacity);
                return nullptr;
            }
            if (slot.buffer && slot.used > 0) std::memcpy(buffer.contents, slot.buffer.contents, slot.used);
            M4Release(slot.buffer);
            M4Resident(buffer);
            buffer.label = [NSString stringWithFormat:@"Texture staging [%zu] %zu KB",
                                                      static_cast<size_t>((m_frameNumber + 1) % kStagingSlots), capacity / 1024];
            slot.buffer = buffer;
            slot.capacity = capacity;
        }
        outOffset = slot.used;
        slot.used += bytes;
        m_counters.stagingBytes += bytes;
        ++m_counters.texUpdates;
        return static_cast<uint8_t*>(slot.buffer.contents) + outOffset;
    }

    void MetalBackend::QueueTextureUpdate(uint32_t texture, uint32_t mipLevel, int x, int y, int width, int height,
                                          const void* data, uint32_t layer) {
        if (width <= 0 || height <= 0) return;
        auto it = m_textures.find(texture);
        if (it == m_textures.end()) return;
        const uint32_t bpp = it->second.bytesPerPixel;
        const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * bpp;
        size_t offset = 0;
        uint8_t* dst = ReserveStaging(bytes, offset);
        if (!dst) return;
        std::memcpy(dst, data, bytes);
        m_pendingTextureUpdates.push_back(
            {texture, mipLevel, x, y, width, height, offset, bytes, static_cast<uint32_t>(width) * bpp, layer});
    }

    void MetalBackend::UpdateTextureArrayLevel(TextureHandle handle, int layer, int level, int x, int y,
                                               int width, int height, const void* data) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !data) return;
        if (level < 0 || static_cast<uint32_t>(level) >= it->second.mipLevels) return;
        if (layer < 0 || static_cast<uint32_t>(layer) >= it->second.layers) return;
        it->second.lastUsedFrame = m_frameNumber;
        // The animated sprites' layers: the next frame's copy, as
        // UpdateTexture2DLevel's (per-frame copies once drawn with).
        QueueTextureUpdate(handle, static_cast<uint32_t>(level), x, y, width, height, data,
                           static_cast<uint32_t>(layer));
    }

    bool MetalBackend::PromoteToFrameCopies(id blit, TextureInfo& tex) {
        // The texture's own image stays with the previous frame's slot — that
        // frame may be sampling it right now, and it holds every update
        // flushed so far. The other slots get new images seeded from it (the
        // one copy that waits for the previous frame, once).
        const uint32_t n = kFramesInFlight;
        const uint32_t previous = (m_currentFrame + n - 1) % n;
        std::vector<TextureInfo::FrameCopy> made(n);
        made[previous] = {tex.texture, m_frameNumber - 1};
        @autoreleasepool {
            for (uint32_t s = 0; s < n; ++s) {
                if (s == previous) continue;
                MTLTextureDescriptor* desc = SampledTextureDescriptor(tex.format, tex.width, tex.height,
                                                                      tex.mipLevels, tex.layers);
                made[s].texture = M4HeapTexture(desc) ?: [m_device newTextureWithDescriptor:desc];
                made[s].syncedFrame = m_frameNumber - 1;
                M4Resident(made[s].texture);
                if (made[s].texture) {
                    made[s].texture.label = [NSString stringWithFormat:@"%s copy[%u]",
                                                                       tex.label.empty() ? "Texture" : tex.label.c_str(), s];
                }
                if (!made[s].texture) {
                    static bool s_warned = false;
                    if (!s_warned) {
                        s_warned = true;
                        Log::Warning("MetalBackend: could not create per-frame texture copies - updates stay in place "
                                     "(frames will not overlap on the GPU)");
                    }
                    return false;
                }
                [blit copyFromTexture:tex.texture toTexture:made[s].texture];
            }
        }
        tex.frameCopies = std::move(made);
        const size_t extra = tex.memorySize * (n - 1);
        m_memStats.textureMemory += extra;
        m_memStats.totalAllocated += extra;
        m_memStats.peakUsage = std::max(m_memStats.peakUsage, m_memStats.totalAllocated);
        Log::Info("[MetalBackend] %dx%d texture (%u levels) updated while in use: %u per-frame copies (+%.1f MB)",
                  tex.width, tex.height, tex.mipLevels, n, static_cast<double>(extra) / (1024.0 * 1024.0));
        return true;
    }

    void MetalBackend::FlushPendingTextureUpdates() {
        // BeginFrame advanced m_frameNumber before calling: this is the slot
        // writers were targeting as "pending" until that moment.
        const uint32_t stagingIndex = static_cast<uint32_t>(m_frameNumber % kStagingSlots);
        StagingSlot& slot = m_staging[stagingIndex];
        const uint32_t frameSlot = m_currentFrame;

        id blit = nil;
        auto encoder = [&]() {
            if (!blit) blit = MakeBlitEncoder(m_cmd, @"Texture flush", /*afterFragment=*/false);
            return blit;
        };

        // A texture that has been drawn with gets its per-frame copies on its
        // first queued update: writing its one image in place would touch
        // what the frame still on the GPU samples, and Metal would hold this
        // whole frame until that one finished.
        for (const PendingTextureUpdate& u : m_pendingTextureUpdates) {
            auto it = m_textures.find(u.texture);
            if (it == m_textures.end()) continue;
            TextureInfo& tex = it->second;
            if (tex.frameCopies.empty() && tex.everBound && tex.owned && !tex.bufferTexture &&
                PromoteToFrameCopies(encoder(), tex)) {
                m_frameCopyTextures.push_back(u.texture);
            }
        }
        if (m_pendingTextureUpdates.empty() && m_frameCopyTextures.empty()) {
            slot.used = 0;
            if (blit) EndBlitEncoder(blit);
            return;
        }

        auto copy = [&](id<MTLBuffer> buffer, size_t offset, uint32_t bytesPerRow, uint32_t level,
                        int x, int y, int w, int h, id<MTLTexture> target, uint32_t layer) {
            [encoder() copyFromBuffer:buffer
                         sourceOffset:offset
                    sourceBytesPerRow:bytesPerRow
                  sourceBytesPerImage:static_cast<NSUInteger>(bytesPerRow) * static_cast<NSUInteger>(h)
                           sourceSize:MTLSizeMake(static_cast<NSUInteger>(w), static_cast<NSUInteger>(h), 1)
                            toTexture:target
                     destinationSlice:layer
                     destinationLevel:level
                    destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(x), static_cast<NSUInteger>(y), 0)];
        };

        // 1. Each per-frame texture's copy for this slot replays the frames it
        //    missed while the other slot was current — oldest first, before
        //    this frame's own updates land on top.
        for (uint32_t handle : m_frameCopyTextures) {
            auto it = m_textures.find(handle);
            if (it == m_textures.end()) continue;
            TextureInfo& tex = it->second;
            TextureInfo::FrameCopy& own = tex.frameCopies[frameSlot];
            for (const auto& [frame, list] : tex.carried) {
                if (frame <= own.syncedFrame) continue;
                if (frame + kStagingSlots - 1 - kFramesInFlight < m_frameNumber) {
                    static bool s_warned = false;
                    if (!s_warned) {
                        s_warned = true;
                        Log::Error("MetalBackend: per-frame texture copy fell %llu frames behind - its staged updates "
                                   "are gone", static_cast<unsigned long long>(m_frameNumber - frame));
                    }
                    continue;
                }
                for (const auto& c : list) {
                    copy(m_staging[c.stagingSlot].buffer, c.stagingOffset,
                         static_cast<uint32_t>(c.width) * tex.bytesPerPixel, c.mipLevel, c.x, c.y, c.width, c.height,
                         own.texture, c.layer);
                }
            }
        }

        // 2. This frame's updates: into the slot's copy of a per-frame texture
        //    (and carried for the other), in place otherwise.
        for (const PendingTextureUpdate& u : m_pendingTextureUpdates) {
            auto it = m_textures.find(u.texture);
            if (it == m_textures.end() || !it->second.texture) continue;
            TextureInfo& tex = it->second;
            if (tex.frameCopies.empty()) {
                copy(slot.buffer, u.stagingOffset, u.bytesPerRow, u.mipLevel, u.x, u.y, u.width, u.height, tex.texture,
                     u.layer);
                continue;
            }
            copy(slot.buffer, u.stagingOffset, u.bytesPerRow, u.mipLevel, u.x, u.y, u.width, u.height,
                 tex.frameCopies[frameSlot].texture, u.layer);
            if (tex.carried.empty() || tex.carried.back().first != m_frameNumber) tex.carried.push_back({m_frameNumber, {}});
            tex.carried.back().second.push_back({stagingIndex, u.stagingOffset, u.mipLevel, u.x, u.y, u.width, u.height,
                                                 u.layer});
        }

        // The slot's copy now holds everything through this frame; records
        // every copy has taken are dropped.
        for (uint32_t handle : m_frameCopyTextures) {
            auto it = m_textures.find(handle);
            if (it == m_textures.end()) continue;
            TextureInfo& tex = it->second;
            tex.frameCopies[frameSlot].syncedFrame = m_frameNumber;
            uint64_t oldest = m_frameNumber;
            for (const auto& c : tex.frameCopies) oldest = std::min(oldest, c.syncedFrame);
            while (!tex.carried.empty() && tex.carried.front().first <= oldest) tex.carried.erase(tex.carried.begin());
        }

        m_pendingTextureUpdates.clear();
        slot.used = 0;
        if (blit) EndBlitEncoder(blit);
    }

    void MetalBackend::DestroyTexture(TextureHandle handle) {
        auto it = m_textures.find(handle);
        if (it == m_textures.end()) return;
        // Anything still queued against it would copy into a released image.
        m_pendingTextureUpdates.erase(
            std::remove_if(m_pendingTextureUpdates.begin(), m_pendingTextureUpdates.end(),
                           [&](const PendingTextureUpdate& u) { return u.texture == handle; }),
            m_pendingTextureUpdates.end());
        m_frameCopyTextures.erase(std::remove(m_frameCopyTextures.begin(), m_frameCopyTextures.end(), handle),
                                  m_frameCopyTextures.end());
        if (it->second.owned) {
            const size_t bytes = it->second.memorySize * std::max<size_t>(1, it->second.frameCopies.size());
            m_memStats.textureMemory -= std::min(m_memStats.textureMemory, bytes);
            m_memStats.totalAllocated -= std::min(m_memStats.totalAllocated, bytes);
            if (m_memStats.textureCount > 0) --m_memStats.textureCount;
            M4Release(it->second.texture);
            for (const TextureInfo::FrameCopy& c : it->second.frameCopies) {
                if (c.texture != it->second.texture) M4Release(c.texture);
            }
        }
        EraseTextureEntry(handle);
    }

    void MetalBackend::BindTexture(TextureHandle handle, uint32_t slot) {
        auto it = m_textures.find(handle);
        if (slot < kMaxTextureSlots) {
            m_boundTextures[slot] = handle;
            m_boundTextureInfo[slot] = it != m_textures.end() ? &it->second : nullptr;
        }
        if (it != m_textures.end()) {
            it->second.lastUsedFrame = m_frameNumber;
            it->second.everBound = true;
        }
    }

    TextureHandle MetalBackend::CreateBufferTexture(BufferHandle buffer, TextureFormat format) {
        auto bit = m_buffers.find(buffer);
        if (bit == m_buffers.end()) return INVALID_TEXTURE;
        const FormatInfo f = ToMetalFormat(format);
        if (format != TextureFormat::RGBA8 && format != TextureFormat::RGBA16 && format != TextureFormat::RGBA16UI &&
            format != TextureFormat::RGBA16F && format != TextureFormat::RGBA32F) {
            Log::Error("MetalBackend::CreateBufferTexture: unsupported format");
            return INVALID_TEXTURE;
        }
        @autoreleasepool {
            // The buffer's bytes as texels (texture_buffer in the shader),
            // sharing its storage — the terrain face map.
            const NSUInteger texels = bit->second.size / f.bytesPerPixel;
            const NSUInteger alignment = [m_device minimumTextureBufferAlignmentForPixelFormat:f.format];
            const NSUInteger rowBytes = ((texels * f.bytesPerPixel + alignment - 1) / alignment) * alignment;
            if (rowBytes > bit->second.buffer.length) {
                Log::Error("MetalBackend::CreateBufferTexture: buffer too small for an aligned texel view");
                return INVALID_TEXTURE;
            }
            MTLTextureDescriptor* desc =
                [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:f.format
                                                                       width:texels
                                                             resourceOptions:bit->second.buffer.resourceOptions
                                                                       usage:MTLTextureUsageShaderRead];
            id<MTLTexture> view = [bit->second.buffer newTextureWithDescriptor:desc offset:0 bytesPerRow:rowBytes];
            if (!view) {
                Log::Error("MetalBackend::CreateBufferTexture: %lu texels could not be viewed",
                           static_cast<unsigned long>(texels));
                return INVALID_TEXTURE;
            }
            TextureInfo info;
            info.texture = view;
            info.width = static_cast<int>(texels);
            info.height = 1;
            info.format = f.format;
            info.bytesPerPixel = f.bytesPerPixel;
            info.bufferTexture = true;
            info.owned = false;   // the buffer's storage
            const uint32_t handle = AllocHandle();
            info.label = "TexBuf#" + std::to_string(handle) + " over " + bit->second.label;
            view.label = [NSString stringWithUTF8String:info.label.c_str()];
            m_textures[handle] = info;
            ++m_memStats.textureCount;
            return handle;
        }
    }

    uintptr_t MetalBackend::GetNativeTextureID(TextureHandle handle) const {
        auto it = m_textures.find(handle);
        if (it == m_textures.end() || !it->second.texture) return 0;
        // ImGui draws with it this frame: this slot's copy. ImTextureID is
        // the id<MTLTexture> itself (imgui_impl_metal).
        const_cast<TextureInfo&>(it->second).everBound = true;
        return reinterpret_cast<uintptr_t>((__bridge void*)FrameTexture(it->second));
    }

    // ========================================================================
    // MESHES
    // ========================================================================

    MeshHandle MetalBackend::CreateMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer, const VertexLayout& layout) {
        const uint32_t handle = AllocHandle();
        MeshInfo info;
        info.vertexBuffer = vertexBuffer;
        info.indexBuffer = indexBuffer;
        info.layout = layout;
        m_meshes[handle] = std::move(info);
        ++m_memStats.meshCount;
        return handle;
    }

    MeshHandle MetalBackend::CreateInstancedMesh(BufferHandle vertexBuffer, BufferHandle indexBuffer,
                                                 BufferHandle instanceBuffer, const VertexLayout& vertexLayout,
                                                 const VertexLayout& instanceLayout) {
        if (vertexBuffer == INVALID_BUFFER || instanceBuffer == INVALID_BUFFER) return INVALID_MESH;
        const uint32_t handle = AllocHandle();
        MeshInfo info;
        info.vertexBuffer = vertexBuffer;
        info.indexBuffer = indexBuffer;
        info.layout = vertexLayout;
        info.instanceBuffer = instanceBuffer;
        info.instanceLayout = instanceLayout;
        m_meshes[handle] = std::move(info);
        ++m_memStats.meshCount;
        return handle;
    }

    void MetalBackend::DestroyMesh(MeshHandle handle) {
        if (m_meshes.erase(handle) > 0 && m_memStats.meshCount > 0) --m_memStats.meshCount;
    }

    // ========================================================================
    // RENDER TARGETS
    // ========================================================================

    bool MetalBackend::CreateTargetImages(TargetInfo& rt) {
        @autoreleasepool {
            // The frame's formats, so every pipeline that draws into the frame
            // draws here unchanged.
            MTLTextureDescriptor* color = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kColorFormat
                                                                                              width:static_cast<NSUInteger>(rt.width)
                                                                                             height:static_cast<NSUInteger>(rt.height)
                                                                                          mipmapped:NO];
            color.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            color.storageMode = MTLStorageModePrivate;
            M4Release(rt.color);   // a resize: the old images after their frames
            M4Release(rt.depth);
            rt.color = [m_device newTextureWithDescriptor:color];
            M4Resident(rt.color);
            rt.depth = nil;
            if (rt.hasDepth) {
                MTLTextureDescriptor* depth = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kDepthFormat
                                                                                                  width:static_cast<NSUInteger>(rt.width)
                                                                                                 height:static_cast<NSUInteger>(rt.height)
                                                                                              mipmapped:NO];
                depth.usage = MTLTextureUsageRenderTarget;
                depth.storageMode = MTLStorageModePrivate;
                rt.depth = [m_device newTextureWithDescriptor:depth];
                M4Resident(rt.depth);
            }
            if (!rt.color || (rt.hasDepth && !rt.depth)) return false;
            if (!rt.label.empty()) ApplyTargetLabels(rt);

            // Cleared on the upload command buffer — the colour to transparent
            // black, so a target sampled before it is ever drawn reads
            // nothing; the depth to far.
            if (m_metal4) {
                M4UploadEncoder();      // the upload command buffer exists
                M4EndUploadEncoder();   // a render encoder follows its copies
            } else {
                if (m_uploadBlit) {
                    [m_uploadBlit endEncoding];
                    m_uploadBlit = nil;
                }
                if (!m_uploadCmd) m_uploadCmd = MakeCommandBuffer(@"Uploads", /*watchErrors=*/true);
            }
            MTLRenderPassDescriptor* clear = [MTLRenderPassDescriptor renderPassDescriptor];
            clear.colorAttachments[0].texture = rt.color;
            clear.colorAttachments[0].loadAction = MTLLoadActionClear;
            clear.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
            clear.colorAttachments[0].storeAction = MTLStoreActionStore;
            if (rt.depth) {
                clear.depthAttachment.texture = rt.depth;
                clear.depthAttachment.loadAction = MTLLoadActionClear;
                clear.depthAttachment.clearDepth = 1.0;
                clear.depthAttachment.storeAction = MTLStoreActionStore;
                clear.stencilAttachment.texture = rt.depth;
                clear.stencilAttachment.loadAction = MTLLoadActionClear;
                clear.stencilAttachment.storeAction = MTLStoreActionStore;
            }
            id clearEnc = m_metal4 ? M4MakeRenderEncoder(m_uploadCmd4, clear, static_cast<uint32_t>(rt.width),
                                                         static_cast<uint32_t>(rt.height), /*resumed=*/false)
                                   : [m_uploadCmd renderCommandEncoderWithDescriptor:clear];
            [clearEnc setLabel:[NSString stringWithFormat:@"Clear %s", rt.label.empty() ? "render target" : rt.label.c_str()]];
            [clearEnc endEncoding];
        }
        // The colour as an ordinary texture: same entry, new image.
        auto texIt = m_textures.find(rt.colorTexture);
        if (texIt != m_textures.end()) {
            texIt->second.texture = rt.color;
            texIt->second.width = rt.width;
            texIt->second.height = rt.height;
            texIt->second.lastUsedFrame = m_frameNumber;
        }
        return true;
    }

    RenderTargetHandle MetalBackend::CreateRenderTarget(const RenderTargetDesc& desc) {
        if (desc.width <= 0 || desc.height <= 0 || !m_device) return INVALID_RENDER_TARGET;
        // Colour is always the frame's format (8-bit BGRA), as on Vulkan.
        if (desc.colorFormat != TextureFormat::RGBA8) {
            Log::Warning("MetalBackend: render target format %d not supported - using the frame's 8-bit format",
                         static_cast<int>(desc.colorFormat));
        }
        TargetInfo rt;
        rt.width = desc.width;
        rt.height = desc.height;
        // OBEY_RT_DEPTH=1: every target keeps a depth attachment (the A/B
        // kill switch for RenderTargetDesc::depth = false).
        static const bool s_forceDepth = std::getenv("OBEY_RT_DEPTH") != nullptr;
        rt.hasDepth = desc.depth || s_forceDepth;
        const RenderTargetHandle handle = AllocHandle();
        rt.label = "RT#" + std::to_string(handle);   // until SetDebugLabel names it ("Rain half-res")
        if (!CreateTargetImages(rt)) {
            Log::Error("MetalBackend: render target %dx%d could not be created", desc.width, desc.height);
            return INVALID_RENDER_TARGET;
        }
        // Sampled linear, clamped (post passes sample between texels).
        rt.colorTexture = WrapTexture(rt.color, MTLSamplerMinMagFilterLinear);
        ApplyTargetLabels(rt);
        const size_t bytes = static_cast<size_t>(rt.width) * static_cast<size_t>(rt.height) * 9u;
        TrackAlloc(m_memStats.textureMemory, bytes, m_memStats.textureCount);
        m_targets[handle] = rt;
        return handle;
    }

    void MetalBackend::DestroyRenderTarget(RenderTargetHandle handle) {
        auto it = m_targets.find(handle);
        if (it == m_targets.end()) return;
        if (m_activeTarget == handle) BindRenderTarget(INVALID_RENDER_TARGET);
        const size_t bytes = static_cast<size_t>(it->second.width) * static_cast<size_t>(it->second.height) * 9u;
        m_memStats.textureMemory -= std::min(m_memStats.textureMemory, bytes);
        m_memStats.totalAllocated -= std::min(m_memStats.totalAllocated, bytes);
        if (m_memStats.textureCount > 0) --m_memStats.textureCount;
        EraseTextureEntry(it->second.colorTexture);
        m_targets.erase(it);
    }

    void MetalBackend::ResizeRenderTarget(RenderTargetHandle handle, int w, int h) {
        if (w <= 0 || h <= 0) return;
        auto it = m_targets.find(handle);
        if (it == m_targets.end()) return;
        TargetInfo& rt = it->second;
        if (rt.width == w && rt.height == h) return;
        if (m_activeTarget == handle) BindRenderTarget(INVALID_RENDER_TARGET);
        const size_t oldBytes = static_cast<size_t>(rt.width) * static_cast<size_t>(rt.height) * 9u;
        rt.width = w;
        rt.height = h;
        if (!CreateTargetImages(rt)) {
            Log::Error("MetalBackend: render target resize to %dx%d failed", w, h);
            return;
        }
        const size_t newBytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 9u;
        m_memStats.textureMemory = m_memStats.textureMemory - std::min(m_memStats.textureMemory, oldBytes) + newBytes;
        m_memStats.totalAllocated = m_memStats.totalAllocated - std::min(m_memStats.totalAllocated, oldBytes) + newBytes;
    }

    TextureHandle MetalBackend::GetRenderTargetColorTexture(RenderTargetHandle handle) const {
        auto it = m_targets.find(handle);
        return it != m_targets.end() ? it->second.colorTexture : INVALID_TEXTURE;
    }

} // namespace Render

#endif // HAS_METAL
