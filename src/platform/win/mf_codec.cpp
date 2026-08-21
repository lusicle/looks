#include "platform/win/mf_codec.h"

#include <windows.h>

#include <initguid.h>   // instantiate CODECAPI_* GUIDs in this TU

#include <codecapi.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstring>
#include <deque>

#include "media/h264_util.h"
#include "platform/win/com_ptr.h"
#include "util/log.h"

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "ole32")
#pragma comment(lib, "d3d11")

namespace looks::platform {

namespace {

bool set_error(std::string* error, const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s (hr=0x%08lX)", what,
                  static_cast<unsigned long>(hr));
    if (error) *error = buf;
    log_error("mf: %s", buf);
    return false;
}

// Finds and activates a synchronous MFT for the category, matched by
// input and/or output type (either may be null). Candidates are tried
// in MFTEnumEx's sorted order until one activates - the first-ranked
// transform is not always usable on a given machine.
HRESULT create_sync_mft(const GUID& category,
                        const MFT_REGISTER_TYPE_INFO* input_info,
                        const MFT_REGISTER_TYPE_INFO* output_info,
                        IMFTransform** out) {
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(category,
                           MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                           input_info, output_info, &activates, &count);
    if (FAILED(hr)) return hr;
    hr = MF_E_TOPO_CODEC_NOT_FOUND;
    for (UINT32 i = 0; i < count && FAILED(hr); ++i)
        hr = activates[i]->ActivateObject(IID_PPV_ARGS(out));
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    return hr;
}

// Wraps bytes into an IMFSample.
HRESULT make_sample(const uint8_t* data, size_t size, int64_t pts,
                    int64_t duration, IMFSample** out) {
    Com<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), buffer.put());
    if (FAILED(hr)) return hr;
    BYTE* dst = nullptr;
    DWORD max_len = 0;
    hr = buffer->Lock(&dst, &max_len, nullptr);
    if (FAILED(hr)) return hr;
    std::memcpy(dst, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));

    Com<IMFSample> sample;
    hr = MFCreateSample(sample.put());
    if (FAILED(hr)) return hr;
    sample->AddBuffer(buffer.get());
    if (pts >= 0) sample->SetSampleTime(pts);
    if (duration > 0) sample->SetSampleDuration(duration);
    *out = sample.get();
    (*out)->AddRef();
    return S_OK;
}

// Fallback sizes for MFTs that report cbSize 0: one comfortable audio
// chunk, and a worst-case 4K video keyframe.
constexpr DWORD kAudioAllocFallback = 4096;
constexpr DWORD kVideoAllocFallback = 4u << 20;

// Allocates the output sample when the MFT does not provide its own.
HRESULT alloc_output_sample(const MFT_OUTPUT_STREAM_INFO& info,
                            DWORD fallback, Com<IMFSample>& out) {
    Com<IMFMediaBuffer> buffer;
    HRESULT hr =
        MFCreateMemoryBuffer(info.cbSize ? info.cbSize : fallback,
                             buffer.put());
    if (FAILED(hr)) return hr;
    hr = MFCreateSample(out.put());
    if (FAILED(hr)) return hr;
    return out->AddBuffer(buffer.get());
}

// Common sync-MFT output pump: allocates the output sample when the MFT
// does not provide one, handles stream changes. Returns S_OK with a sample,
// MF_E_TRANSFORM_NEED_MORE_INPUT, or a hard error.
HRESULT pump_output(IMFTransform* mft, DWORD alloc_fallback,
                    Com<IMFSample>& out, bool* stream_changed) {
    *stream_changed = false;
    MFT_OUTPUT_STREAM_INFO info{};
    HRESULT hr = mft->GetOutputStreamInfo(0, &info);
    if (FAILED(hr)) return hr;

    const bool provides =
        (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                         MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    MFT_OUTPUT_DATA_BUFFER output{};
    Com<IMFSample> allocated;
    if (!provides) {
        hr = alloc_output_sample(info, alloc_fallback, allocated);
        if (FAILED(hr)) return hr;
        output.pSample = allocated.get();
    }

    DWORD status = 0;
    hr = mft->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) output.pEvents->Release();

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        *stream_changed = true;
        return hr;
    }
    if (FAILED(hr)) return hr;

    if (output.pSample) {
        out.reset();
        *out.put() = output.pSample;
        if (provides) {
            // MFT-owned sample: we now hold the only reference we manage.
        } else {
            output.pSample->AddRef();   // balance: `allocated` also releases
        }
    }
    return S_OK;
}

// Copies one sample's contiguous payload and timing into a packet.
// `key_default` seeds the keyframe flag for streams that do not stamp
// MFSampleExtension_CleanPoint (every AAC frame is a sync point).
bool sample_to_packet(IMFSample* sample, EncodedPacket& out,
                      bool key_default) {
    LONGLONG pts = 0, duration = 0;
    sample->GetSampleTime(&pts);
    sample->GetSampleDuration(&duration);
    UINT32 clean = key_default ? 1u : 0u;
    sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
    Com<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) return false;
    BYTE* src = nullptr;
    DWORD len = 0;
    if (FAILED(buffer->Lock(&src, nullptr, &len))) return false;
    out.data.assign(src, src + len);
    buffer->Unlock();
    out.pts_100ns = pts;
    out.duration_100ns = duration;
    out.keyframe = clean != 0;
    return true;
}

// Pitch-aware NV12 copy via IMF2DBuffer2 — the layout D3D-backed decoder
// samples come in (GPU pitch, padded plane height). Returns false when the
// sample has no 2D buffer; the caller falls back to the contiguous path.
bool copy_nv12_2d(IMFSample* sample, uint32_t w, uint32_t h,
                  VideoFrameNV12& out) {
    Com<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, buffer.put()))) return false;
    Com<IMF2DBuffer2> buf2d;
    if (FAILED(buffer->QueryInterface(IID_PPV_ARGS(buf2d.put()))))
        return false;
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    BYTE* start = nullptr;
    DWORD len = 0;
    if (FAILED(buf2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch,
                                 &start, &len)))
        return false;
    bool ok = false;
    if (pitch > 0 && static_cast<uint32_t>(pitch) >= w) {
        // NV12: Y rows at scanline 0, UV rows after the texture's padded
        // plane height — recover that height from the buffer size.
        const size_t p = static_cast<size_t>(pitch);
        const size_t padded_h = 2 * static_cast<size_t>(len) / (3 * p);
        if (padded_h >= h) {
            out.data.resize(static_cast<size_t>(w) * h * 3 / 2);
            uint8_t* dst_y = out.data.data();
            for (uint32_t r = 0; r < h; ++r)
                std::memcpy(dst_y + static_cast<size_t>(r) * w,
                            scan0 + r * p, w);
            const BYTE* uv = start + p * padded_h;
            uint8_t* dst_uv = dst_y + static_cast<size_t>(w) * h;
            for (uint32_t r = 0; r < h / 2; ++r)
                std::memcpy(dst_uv + static_cast<size_t>(r) * w,
                            uv + r * p, w);
            ok = true;
        }
    }
    buf2d->Unlock2D();
    return ok;
}

}  // namespace

// ------------------------------------------------------------- MfSession

MfSession::MfSession() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_ = SUCCEEDED(com) || com == RPC_E_CHANGED_MODE;
    const HRESULT mf = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    mf_ = SUCCEEDED(mf);
    ok_ = com_ && mf_;
    if (!ok_) log_error("mf: startup failed (com=%d mf=%d)", com_, mf_);
}

MfSession::~MfSession() {
    if (mf_) MFShutdown();
    if (com_) CoUninitialize();
}

// ----------------------------------------------------------- H264Decoder

struct H264Decoder::Impl {
    Com<IMFTransform> mft;
    std::vector<uint8_t> sps_pps;      // Annex B parameter sets from avcC
    std::vector<uint8_t> annexb;       // conversion scratch
    int nal_length_size = 4;
    uint32_t width = 0;
    uint32_t height = 0;
    bool sent_params = false;
    // PIPELINED READBACK (DXVA): decoded samples held unmapped so the GPU
    // decodes ahead while the CPU maps the OLDEST one - mapping a surface
    // whose decode already finished is a copy, not a stall, and the stall
    // was most of the hardware path's per-frame cost. Depth 1 (software
    // samples are system memory) keeps the old emit-immediately behavior.
    std::deque<Com<IMFSample>> inflight;

    // D3D11/DXVA acceleration: kept alive for the MFT's lifetime.
    Com<IMFDXGIDeviceManager> dxgi_mgr;
    Com<ID3D11Device> d3d;

    // Attaches a DXGI device manager when the decoder is D3D11-aware, so
    // the pixel work runs on the GPU (DXVA). Failure is non-fatal — decode
    // just stays on the pure software path.
    bool try_d3d() {
        Com<IMFAttributes> attrs;
        UINT32 aware = 0;
        if (FAILED(mft->GetAttributes(attrs.put())) ||
            FAILED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware)) || !aware)
            return false;
        Com<ID3D11DeviceContext> ctx;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr,
                                       D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                       nullptr, 0, D3D11_SDK_VERSION,
                                       d3d.put(), nullptr, ctx.put());
        if (FAILED(hr)) return false;
        Com<ID3D11Multithread> mt;
        if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(mt.put()))))
            mt->SetMultithreadProtected(TRUE);
        UINT reset_token = 0;
        hr = MFCreateDXGIDeviceManager(&reset_token, dxgi_mgr.put());
        if (SUCCEEDED(hr))
            hr = dxgi_mgr->ResetDevice(d3d.get(), reset_token);
        if (SUCCEEDED(hr))
            hr = mft->ProcessMessage(
                MFT_MESSAGE_SET_D3D_MANAGER,
                reinterpret_cast<ULONG_PTR>(dxgi_mgr.get()));
        if (FAILED(hr)) {
            dxgi_mgr.reset();
            d3d.reset();
            return false;
        }
        return true;
    }

    bool negotiate_output(std::string* error) {
        // Pick NV12 among the advertised output types.
        for (DWORD i = 0;; ++i) {
            Com<IMFMediaType> type;
            HRESULT hr = mft->GetOutputAvailableType(0, i, type.put());
            if (FAILED(hr))
                return set_error(error, "no NV12 output type", hr);
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12) {
                hr = mft->SetOutputType(0, type.get(), 0);
                if (FAILED(hr))
                    return set_error(error, "SetOutputType(NV12)", hr);
                UINT64 size = 0;
                if (SUCCEEDED(type->GetUINT64(MF_MT_FRAME_SIZE, &size))) {
                    width = static_cast<uint32_t>(size >> 32);
                    height = static_cast<uint32_t>(size);
                }
                return true;
            }
        }
    }
};

H264Decoder::H264Decoder() : impl_(new Impl) {}
H264Decoder::~H264Decoder() = default;

bool H264Decoder::create(const std::vector<uint8_t>& avcc, uint32_t width,
                         uint32_t height, std::string* error, bool allow_d3d,
                         bool low_latency) {
    Impl& d = *impl_;
    d.width = width;
    d.height = height;

    // The record layout lives in media/h264_util.h beside its builder.
    media::AvccInfo info;
    if (!media::parse_avcc(avcc, &info)) {
        if (error) *error = "bad avcC";
        return false;
    }
    d.nal_length_size = info.nal_length_size;
    d.sps_pps = std::move(info.sps_pps_annexb);

    const MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video,
                                         MFVideoFormat_H264};
    HRESULT hr = create_sync_mft(MFT_CATEGORY_VIDEO_DECODER, &in_info,
                                 nullptr, d.mft.put());
    if (FAILED(hr)) return set_error(error, "H.264 decoder MFT not found", hr);

    // Wire the D3D11 manager for speed — before type negotiation
    // so the decoder can plan its DXVA surface pool.
    if (allow_d3d && d.try_d3d())
        log_info("mf: H.264 decode D3D11-accelerated (DXVA)");
    else
        log_info("mf: H.264 decode on the software path%s",
                 low_latency ? " (low latency)" : "");

    // Latency cap for the SOFTWARE decoder only: its output lag equals
    // its thread count (~40 frames on a big CPU), which no ring survives.
    // The DXVA path lags only its DPB (a handful) and MF_LOW_LATENCY
    // there just forces per-frame completion, defeating the GPU's own
    // pipelining.
    if (low_latency && !d.dxgi_mgr) {
        Com<IMFAttributes> attrs;
        if (SUCCEEDED(d.mft->GetAttributes(attrs.put())))
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    Com<IMFMediaType> input;
    MFCreateMediaType(input.put());
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    input->SetUINT64(MF_MT_FRAME_SIZE,
                     (static_cast<UINT64>(width) << 32) | height);
    hr = d.mft->SetInputType(0, input.get(), 0);
    if (FAILED(hr)) return set_error(error, "SetInputType(H264)", hr);

    if (!d.negotiate_output(error)) return false;

    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Decoder::feed(const uint8_t* data, size_t size, int64_t pts_100ns,
                       int64_t duration_100ns, bool keyframe) {
    Impl& d = *impl_;
    if (!d.mft) return false;

    // AVCC -> Annex B; SPS/PPS injected before the first/key frames.
    d.annexb.clear();
    if (keyframe || !d.sent_params) {
        d.annexb.insert(d.annexb.end(), d.sps_pps.begin(), d.sps_pps.end());
        d.sent_params = true;
    }
    size_t pos = 0;
    while (pos + d.nal_length_size <= size) {
        size_t nal_len = 0;
        for (int i = 0; i < d.nal_length_size; ++i)
            nal_len = (nal_len << 8) | data[pos + i];
        pos += d.nal_length_size;
        if (nal_len == 0 || pos + nal_len > size) break;
        media::append_annexb_nal(d.annexb, data + pos, nal_len);
        pos += nal_len;
    }
    if (d.annexb.empty()) return true;   // nothing usable; skip

    Com<IMFSample> sample;
    HRESULT hr = make_sample(d.annexb.data(), d.annexb.size(), pts_100ns,
                             duration_100ns, sample.put());
    if (FAILED(hr)) return false;
    hr = d.mft->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // Caller must drain via receive() first; treat as hard error to
        // keep the feed/receive contract simple.
        log_warn("mf: H264 ProcessInput not accepting — receive() first");
        return false;
    }
    return SUCCEEDED(hr);
}

bool H264Decoder::receive(VideoFrameNV12& out) {
    Impl& d = *impl_;
    if (!d.mft) return false;

    // Fill the in-flight queue without mapping anything, then emit the
    // OLDEST - pulled earliest, so its decode has finished while newer
    // frames were still on the GPU and the map below is a copy, not a
    // stall.
    const size_t depth = d.dxgi_mgr ? 3 : 1;
    while (d.inflight.size() < depth) {
        Com<IMFSample> pulled;
        bool stream_changed = false;
        const HRESULT hr = pump_output(d.mft.get(), kVideoAllocFallback,
                                       pulled, &stream_changed);
        if (stream_changed) {
            std::string err;
            if (!d.negotiate_output(&err)) return false;
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) break;
        if (FAILED(hr) || !pulled) {
            if (d.inflight.empty()) return false;
            break;
        }
        d.inflight.push_back(std::move(pulled));
    }
    if (d.inflight.empty()) return false;

    {
        Com<IMFSample> sample = std::move(d.inflight.front());
        d.inflight.pop_front();

        LONGLONG pts = 0;
        sample->GetSampleTime(&pts);
        const uint32_t w = d.width;
        const uint32_t h = d.height;
        out.width = w;
        out.height = h;
        out.stride = w;
        out.pts_100ns = pts;

        // D3D-backed samples (DXVA path) carry a GPU pitch — read them
        // through the 2D buffer; system-memory samples fall through to the
        // contiguous copy below.
        if (copy_nv12_2d(sample.get(), w, h, out)) return true;

        Com<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())))
            return false;
        BYTE* src = nullptr;
        DWORD len = 0;
        if (FAILED(buffer->Lock(&src, nullptr, &len))) return false;

        // Contiguous NV12: stride == width (MF contiguous layout), height
        // may include alignment padding — copy exactly what we need.
        const size_t needed = static_cast<size_t>(w) * h * 3 / 2;
        out.data.resize(needed);
        if (len >= needed) {
            // Y then UV; MF may pad height to 16 — planes are consecutive in
            // the contiguous buffer at aligned offsets only when padded, so
            // detect: if len == exactly needed, direct copy; else copy Y and
            // UV with the padded plane height.
            if (len == needed) {
                std::memcpy(out.data.data(), src, needed);
            } else {
                const uint32_t padded_h = (h + 15) & ~15u;
                std::memcpy(out.data.data(), src, static_cast<size_t>(w) * h);
                std::memcpy(out.data.data() + static_cast<size_t>(w) * h,
                            src + static_cast<size_t>(w) * padded_h,
                            static_cast<size_t>(w) * h / 2);
            }
        } else {
            buffer->Unlock();
            return false;
        }
        buffer->Unlock();
        return true;
    }
}

void H264Decoder::drain() {
    if (impl_->mft) {
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
}

void H264Decoder::flush() {
    Impl& d = *impl_;
    if (!d.mft) return;
    d.inflight.clear();
    d.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    d.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    d.sent_params = false;
}

// ------------------------------------------- shared PCM decode core
// AAC and MP3 decode through the same sync-MFT shape: negotiate a
// 16-bit PCM output, feed compressed samples, drain PCM chunks. Only
// create() differs (the input type), so the engine lives here once.

struct PcmMftCore {
    Com<IMFTransform> mft;
    uint32_t out_channels = 0;
    uint32_t out_rate = 0;
    const char* label = "audio";

    bool negotiate_output(std::string* error) {
        for (DWORD i = 0;; ++i) {
            Com<IMFMediaType> type;
            HRESULT hr = mft->GetOutputAvailableType(0, i, type.put());
            if (FAILED(hr)) return set_error(error, "no PCM output type", hr);
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            UINT32 bits = 0;
            type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            if (subtype == MFAudioFormat_PCM && bits == 16) {
                hr = mft->SetOutputType(0, type.get(), 0);
                if (FAILED(hr)) return set_error(error, "SetOutputType(PCM)", hr);
                type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &out_channels);
                type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &out_rate);
                return true;
            }
        }
    }

    // Negotiates the output and opens the stream (the tail of create()).
    bool start(std::string* error) {
        if (!negotiate_output(error)) return false;
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    bool feed(const uint8_t* data, size_t size, int64_t pts_100ns) {
        if (!mft) return false;
        Com<IMFSample> sample;
        if (FAILED(make_sample(data, size, pts_100ns, 0, sample.put())))
            return false;
        const HRESULT hr = mft->ProcessInput(0, sample.get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            log_warn("mf: %s ProcessInput not accepting — receive() first",
                     label);
            return false;
        }
        return SUCCEEDED(hr);
    }

    bool receive(AudioChunk& out) {
        if (!mft) return false;
        for (;;) {
            Com<IMFSample> sample;
            bool stream_changed = false;
            const HRESULT hr = pump_output(mft.get(), kAudioAllocFallback,
                                           sample, &stream_changed);
            if (stream_changed) {
                std::string err;
                if (!negotiate_output(&err)) return false;
                continue;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
            if (FAILED(hr) || !sample) return false;

            LONGLONG pts = 0;
            sample->GetSampleTime(&pts);
            Com<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())))
                return false;
            BYTE* src = nullptr;
            DWORD len = 0;
            if (FAILED(buffer->Lock(&src, nullptr, &len))) return false;
            out.channels = out_channels;
            out.sample_rate = out_rate;
            out.pts_100ns = pts;
            out.samples.resize(len / 2);
            std::memcpy(out.samples.data(), src, out.samples.size() * 2);
            buffer->Unlock();
            return true;
        }
    }

    void drain() {
        if (!mft) return;
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
};

// ------------------------------------------------------------ AacDecoder

struct AacDecoder::Impl : PcmMftCore {};

AacDecoder::AacDecoder() : impl_(new Impl) {}
AacDecoder::~AacDecoder() = default;

bool AacDecoder::create(const std::vector<uint8_t>& asc, uint32_t channels,
                        uint32_t sample_rate, std::string* error) {
    Impl& d = *impl_;
    d.label = "AAC";
    const MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Audio, MFAudioFormat_AAC};
    HRESULT hr = create_sync_mft(MFT_CATEGORY_AUDIO_DECODER, &in_info,
                                 nullptr, d.mft.put());
    if (FAILED(hr)) return set_error(error, "AAC decoder MFT not found", hr);

    Com<IMFMediaType> input;
    MFCreateMediaType(input.put());
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    input->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);   // raw AAC frames
    // MF_MT_USER_DATA = HEAACWAVEINFO tail (12 bytes) + AudioSpecificConfig.
    std::vector<uint8_t> user(12, 0);
    user.insert(user.end(), asc.begin(), asc.end());
    input->SetBlob(MF_MT_USER_DATA, user.data(),
                   static_cast<UINT32>(user.size()));
    hr = d.mft->SetInputType(0, input.get(), 0);
    if (FAILED(hr)) return set_error(error, "SetInputType(AAC)", hr);

    return d.start(error);
}

bool AacDecoder::feed(const uint8_t* data, size_t size, int64_t pts_100ns) {
    return impl_->feed(data, size, pts_100ns);
}

bool AacDecoder::receive(AudioChunk& out) { return impl_->receive(out); }

void AacDecoder::drain() { impl_->drain(); }

// ------------------------------------------------------------ Mp3Decoder

struct Mp3Decoder::Impl : PcmMftCore {};

Mp3Decoder::Mp3Decoder() : impl_(new Impl) {}
Mp3Decoder::~Mp3Decoder() = default;

bool Mp3Decoder::create(uint32_t channels, uint32_t sample_rate,
                        std::string* error) {
    Impl& d = *impl_;
    d.label = "MP3";
    const MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Audio, MFAudioFormat_MP3};
    HRESULT hr = create_sync_mft(MFT_CATEGORY_AUDIO_DECODER, &in_info,
                                 nullptr, d.mft.put());
    if (FAILED(hr)) return set_error(error, "MP3 decoder MFT not found", hr);

    Com<IMFMediaType> input;
    MFCreateMediaType(input.put());
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
    input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    hr = d.mft->SetInputType(0, input.get(), 0);
    if (FAILED(hr)) return set_error(error, "SetInputType(MP3)", hr);

    return d.start(error);
}

bool Mp3Decoder::feed(const uint8_t* data, size_t size, int64_t pts_100ns) {
    return impl_->feed(data, size, pts_100ns);
}

bool Mp3Decoder::receive(AudioChunk& out) { return impl_->receive(out); }

void Mp3Decoder::drain() { impl_->drain(); }

// ----------------------------------------------------------- H264Encoder

struct H264Encoder::Impl {
    Com<IMFTransform> mft;
    uint32_t width = 0;
    uint32_t height = 0;

    // Async hardware path: hardware MFTs only run in hardware
    // with an IMFDXGIDeviceManager attached, and they speak the async
    // event-pump protocol (METransformNeedInput / HaveOutput) instead of
    // the sync ProcessInput/Output model. Software fallback keeps the sync
    // members above.
    bool async_mode = false;
    DWORD in_id = 0, out_id = 0;
    Com<IMFMediaEventGenerator> events;
    Com<IMFDXGIDeviceManager> dxgi_mgr;
    Com<ID3D11Device> d3d;
    int needs_input = 0;             // granted-but-unused input slots
    bool drain_complete = false;
    std::deque<EncodedPacket> ready; // outputs collected while pumping

    bool async_read_output();
    bool async_pump(bool wait);
    bool try_hardware(uint32_t width, uint32_t height, uint32_t fps_num,
                      uint32_t fps_den, uint32_t bitrate_bps,
                      uint32_t gop_frames);
};

// B-frames off (pts == dts, the muxer then emits no ctts); an explicit
// GOP size pins the keyframe cadence when the caller asked for one.
static void apply_encoder_codec_api(IMFTransform* mft, uint32_t gop_frames) {
    Com<ICodecAPI> codec_api;
    if (FAILED(mft->QueryInterface(IID_PPV_ARGS(codec_api.put())))) return;
    VARIANT v{};
    v.vt = VT_UI4;
    v.ulVal = 0;
    codec_api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
    if (gop_frames > 0) {
        v.ulVal = gop_frames;
        codec_api->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
    }
}

// Reads one encoded packet from an async encoder's output stream.
bool H264Encoder::Impl::async_read_output() {
    Impl& e = *this;
    MFT_OUTPUT_STREAM_INFO info{};
    e.mft->GetOutputStreamInfo(e.out_id, &info);
    const bool provides =
        (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                         MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    Com<IMFSample> ours;
    MFT_OUTPUT_DATA_BUFFER odb{};
    odb.dwStreamID = e.out_id;
    if (!provides) {
        if (FAILED(alloc_output_sample(info, kVideoAllocFallback, ours)))
            return false;
        odb.pSample = ours.get();
    }
    DWORD status = 0;
    const HRESULT hr = e.mft->ProcessOutput(0, 1, &odb, &status);
    if (odb.pEvents) odb.pEvents->Release();
    IMFSample* got = odb.pSample;
    if (FAILED(hr) || !got) {
        if (provides && got) got->Release();
        return false;
    }

    EncodedPacket pkt;
    const bool ok = sample_to_packet(got, pkt, /*key_default=*/false);
    if (provides && got) got->Release();
    if (ok) e.ready.push_back(std::move(pkt));
    return ok;
}

// Drains the encoder's event queue. `wait` blocks until at least one
// event arrives (used when an input slot or drain-complete is owed).
bool H264Encoder::Impl::async_pump(bool wait) {
    Impl& e = *this;
    for (;;) {
        Com<IMFMediaEvent> ev;
        const HRESULT hr =
            e.events->GetEvent(wait ? 0 : MF_EVENT_FLAG_NO_WAIT, ev.put());
        if (hr == MF_E_NO_EVENTS_AVAILABLE) return true;
        if (FAILED(hr)) return false;
        wait = false;   // one blocking wait per call is enough
        MediaEventType type = MEUnknown;
        ev->GetType(&type);
        if (type == METransformNeedInput) {
            ++e.needs_input;
        } else if (type == METransformHaveOutput) {
            if (!e.async_read_output()) return false;
        } else if (type == METransformDrainComplete) {
            e.drain_complete = true;
        } else if (type == MEError) {
            HRESULT status = S_OK;
            ev->GetStatus(&status);
            log_error("mf: async encoder MEError (hr=0x%08lX)",
                      static_cast<unsigned long>(status));
            return false;
        }
    }
}

// Attempts the hardware path; on any failure the caller falls back to the
// software (sync) encoder with the impl reset to a clean slate.
bool H264Encoder::Impl::try_hardware(uint32_t width, uint32_t height,
                                     uint32_t fps_num, uint32_t fps_den,
                                     uint32_t bitrate_bps,
                                     uint32_t gop_frames) {
    Impl& e = *this;
    auto fail = [](const char* stage, HRESULT hr) {
        log_warn("mf: hardware H.264 encoder unavailable at %s "
                 "(hr=0x%08lX) — using software",
                 stage, static_cast<unsigned long>(hr));
        return false;
    };
    MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                               MFT_ENUM_FLAG_SORTANDFILTER,
                           &in_info, &out_info, &activates, &count);
    if (FAILED(hr) || count == 0) {
        if (activates) CoTaskMemFree(activates);
        return fail("enum", FAILED(hr) ? hr : MF_E_TOPO_CODEC_NOT_FOUND);
    }
    // Try each candidate until one accepts our types (SORTANDFILTER puts
    // the preferred transform first, but e.g. a virtual adapter's encoder
    // can outrank the real GPU's).
    HRESULT last_hr = MF_E_TOPO_CODEC_NOT_FOUND;
    const char* last_stage = "activate";
    for (UINT32 i = 0; i < count && !e.mft; ++i) {
        Com<IMFTransform> mft;
        if (FAILED(last_hr = activates[i]->ActivateObject(
                       IID_PPV_ARGS(mft.put())))) {
            last_stage = "activate";
            continue;
        }
        // Async MFTs must be explicitly unlocked before use.
        Com<IMFAttributes> attrs;
        last_hr = mft->GetAttributes(attrs.put());
        if (SUCCEEDED(last_hr))
            last_hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (FAILED(last_hr)) {
            last_stage = "async unlock";
            continue;
        }
        // Stream ids (E_NOTIMPL means 0..n-1).
        DWORD ins[1] = {0}, outs[1] = {0};
        DWORD in_id = 0, out_id = 0;
        if (mft->GetStreamIDs(1, ins, 1, outs) == S_OK) {
            in_id = ins[0];
            out_id = outs[0];
        }

        Com<IMFMediaType> output;
        MFCreateMediaType(output.put());
        output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        output->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps);
        output->SetUINT64(MF_MT_FRAME_SIZE,
                          (static_cast<UINT64>(width) << 32) | height);
        output->SetUINT64(MF_MT_FRAME_RATE,
                          (static_cast<UINT64>(fps_num) << 32) | fps_den);
        output->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO,
                          (static_cast<UINT64>(1) << 32) | 1);
        output->SetUINT32(MF_MT_INTERLACE_MODE,
                          MFVideoInterlace_Progressive);
        output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        if (FAILED(last_hr = mft->SetOutputType(out_id, output.get(), 0))) {
            last_stage = "SetOutputType";
            continue;
        }

        Com<IMFMediaType> input;
        MFCreateMediaType(input.put());
        input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        input->SetUINT64(MF_MT_FRAME_SIZE,
                         (static_cast<UINT64>(width) << 32) | height);
        input->SetUINT64(MF_MT_FRAME_RATE,
                         (static_cast<UINT64>(fps_num) << 32) | fps_den);
        input->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO,
                         (static_cast<UINT64>(1) << 32) | 1);
        input->SetUINT32(MF_MT_INTERLACE_MODE,
                         MFVideoInterlace_Progressive);
        input->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
        if (FAILED(last_hr = mft->SetInputType(in_id, input.get(), 0))) {
            last_stage = "SetInputType";
            continue;
        }

        WCHAR name[128] = L"";
        UINT32 name_len = 0;
        activates[i]->GetString(MFT_FRIENDLY_NAME_Attribute, name, 127,
                                &name_len);
        log_info("mf: hardware encoder: %ls", name);
        e.mft = std::move(mft);
        e.in_id = in_id;
        e.out_id = out_id;
    }
    for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
    CoTaskMemFree(activates);
    if (!e.mft) return fail(last_stage, last_hr);

    // D3D11 device + DXGI manager: hardware MFTs only run in
    // hardware with a device manager attached.
    UINT reset_token = 0;
    Com<ID3D11DeviceContext> ctx;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, e.d3d.put(), nullptr,
                           ctx.put());
    if (FAILED(hr)) return fail("D3D11 device", hr);
    Com<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(mt.put()))))
        mt->SetMultithreadProtected(TRUE);
    hr = MFCreateDXGIDeviceManager(&reset_token, e.dxgi_mgr.put());
    if (SUCCEEDED(hr)) hr = e.dxgi_mgr->ResetDevice(e.d3d.get(), reset_token);
    if (FAILED(hr)) return fail("DXGI manager", hr);

    // Attach the DXGI manager. Some encoder MFTs accept it only
    // after type negotiation; a refusal is non-fatal — the async hardware
    // MFT still encodes on the GPU from CPU samples, the manager merely
    // enables zero-copy D3D surface input.
    hr = e.mft->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER,
        reinterpret_cast<ULONG_PTR>(e.dxgi_mgr.get()));
    if (FAILED(hr))
        log_warn("mf: encoder refused DXGI manager (hr=0x%08lX) — "
                 "hardware path continues with CPU sample input",
                 static_cast<unsigned long>(hr));

    apply_encoder_codec_api(e.mft.get(), gop_frames);

    hr = e.mft->QueryInterface(IID_PPV_ARGS(e.events.put()));
    if (FAILED(hr)) return fail("event generator", hr);
    hr = e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(hr))
        hr = e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) return fail("start of stream", hr);
    e.async_mode = true;
    return true;
}

H264Encoder::H264Encoder() : impl_(new Impl) {}
H264Encoder::~H264Encoder() = default;

bool H264Encoder::create(uint32_t width, uint32_t height, uint32_t fps_num,
                         uint32_t fps_den, uint32_t bitrate_bps,
                         std::string* error, uint32_t gop_frames) {
    Impl& e = *impl_;
    e.width = width;
    e.height = height;

    // Hardware first: async MFT + IMFDXGIDeviceManager. Any
    // failure resets to a clean slate and falls through to software —
    // import/export are offline, so the fallback is only a speed loss.
    if (e.try_hardware(width, height, fps_num, fps_den, bitrate_bps,
                       gop_frames)) {
        log_info("mf: hardware H.264 encoder active (async MFT + D3D11)");
        return true;
    }
    e.mft.reset();
    e.events.reset();
    e.dxgi_mgr.reset();
    e.d3d.reset();
    e.async_mode = false;
    e.needs_input = 0;
    e.in_id = e.out_id = 0;

    // Software encoder: enumerate by OUTPUT type for encoders.
    const MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Video,
                                          MFVideoFormat_H264};
    HRESULT hr = create_sync_mft(MFT_CATEGORY_VIDEO_ENCODER, nullptr,
                                 &out_info, e.mft.put());
    if (FAILED(hr))
        return set_error(error, "H.264 encoder MFT not found", hr);

    // Encoders: set OUTPUT type first, then input.
    Com<IMFMediaType> output;
    MFCreateMediaType(output.put());
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, bitrate_bps);
    output->SetUINT64(MF_MT_FRAME_SIZE,
                      (static_cast<UINT64>(width) << 32) | height);
    output->SetUINT64(MF_MT_FRAME_RATE,
                      (static_cast<UINT64>(fps_num) << 32) | fps_den);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    hr = e.mft->SetOutputType(0, output.get(), 0);
    if (FAILED(hr)) return set_error(error, "SetOutputType(H264)", hr);

    Com<IMFMediaType> input;
    MFCreateMediaType(input.put());
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    input->SetUINT64(MF_MT_FRAME_SIZE,
                     (static_cast<UINT64>(width) << 32) | height);
    input->SetUINT64(MF_MT_FRAME_RATE,
                     (static_cast<UINT64>(fps_num) << 32) | fps_den);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    hr = e.mft->SetInputType(0, input.get(), 0);
    if (FAILED(hr)) return set_error(error, "SetInputType(NV12)", hr);

    apply_encoder_codec_api(e.mft.get(), gop_frames);

    e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Encoder::feed_nv12(const uint8_t* data, int64_t pts_100ns,
                            int64_t duration_100ns) {
    Impl& e = *impl_;
    if (!e.mft) return false;
    const size_t size = static_cast<size_t>(e.width) * e.height * 3 / 2;
    Com<IMFSample> sample;
    if (FAILED(make_sample(data, size, pts_100ns, duration_100ns, sample.put())))
        return false;
    if (e.async_mode) {
        // Wait until the encoder grants an input slot, collecting any
        // outputs that arrive meanwhile.
        while (e.needs_input == 0)
            if (!e.async_pump(/*wait=*/true)) return false;
        if (FAILED(e.mft->ProcessInput(e.in_id, sample.get(), 0)))
            return false;
        --e.needs_input;
        return e.async_pump(/*wait=*/false);
    }
    const HRESULT hr = e.mft->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        log_warn("mf: H264 encoder not accepting — receive() first");
        return false;
    }
    return SUCCEEDED(hr);
}

bool H264Encoder::receive(EncodedPacket& out) {
    Impl& e = *impl_;
    if (!e.mft) return false;
    if (e.async_mode) {
        e.async_pump(/*wait=*/false);
        if (e.ready.empty()) return false;
        out = std::move(e.ready.front());
        e.ready.pop_front();
        return true;
    }
    Com<IMFSample> sample;
    bool stream_changed = false;
    const HRESULT hr = pump_output(e.mft.get(), kVideoAllocFallback, sample,
                                   &stream_changed);
    if (stream_changed || hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
    if (FAILED(hr) || !sample) return false;
    return sample_to_packet(sample.get(), out, /*key_default=*/false);
}

void H264Encoder::drain() {
    Impl& e = *impl_;
    if (!e.mft) return;
    e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    e.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (e.async_mode) {
        // Pump until METransformDrainComplete; every remaining output
        // lands in the ready queue for receive().
        while (!e.drain_complete)
            if (!e.async_pump(/*wait=*/true)) break;
    }
}

// ------------------------------------------------------------ AacEncoder

struct AacEncoder::Impl {
    Com<IMFTransform> mft;
    std::vector<uint8_t> asc;
    uint32_t channels = 0;
};

AacEncoder::AacEncoder() : impl_(new Impl) {}
AacEncoder::~AacEncoder() = default;

const std::vector<uint8_t>& AacEncoder::audio_specific_config() const {
    return impl_->asc;
}

bool AacEncoder::create(uint32_t channels, uint32_t sample_rate,
                        uint32_t bitrate_bps, std::string* error) {
    Impl& e = *impl_;
    e.channels = channels;

    const MFT_REGISTER_TYPE_INFO out_info{MFMediaType_Audio,
                                          MFAudioFormat_AAC};
    HRESULT hr = create_sync_mft(MFT_CATEGORY_AUDIO_ENCODER, nullptr,
                                 &out_info, e.mft.put());
    if (FAILED(hr)) return set_error(error, "AAC encoder MFT not found", hr);

    Com<IMFMediaType> input;
    MFCreateMediaType(input.put());
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    input->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    hr = e.mft->SetInputType(0, input.get(), 0);
    if (FAILED(hr)) return set_error(error, "SetInputType(PCM)", hr);

    // The AAC encoder wants bytes-per-second in one of its supported tiers.
    const uint32_t bytes_per_sec = bitrate_bps / 8;
    Com<IMFMediaType> output;
    MFCreateMediaType(output.put());
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate);
    output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    output->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes_per_sec);
    output->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    hr = e.mft->SetOutputType(0, output.get(), 0);
    if (FAILED(hr)) return set_error(error, "SetOutputType(AAC)", hr);

    // Extract AudioSpecificConfig from the negotiated output type:
    // MF_MT_USER_DATA = HEAACWAVEINFO tail (12 bytes) + ASC.
    Com<IMFMediaType> negotiated;
    if (SUCCEEDED(e.mft->GetOutputCurrentType(0, negotiated.put()))) {
        UINT32 blob_size = 0;
        if (SUCCEEDED(negotiated->GetBlobSize(MF_MT_USER_DATA, &blob_size)) &&
            blob_size > 12) {
            std::vector<uint8_t> blob(blob_size);
            negotiated->GetBlob(MF_MT_USER_DATA, blob.data(), blob_size, nullptr);
            e.asc.assign(blob.begin() + 12, blob.end());
        }
    }
    if (e.asc.empty())
        return set_error(error, "AAC encoder gave no AudioSpecificConfig",
                         E_UNEXPECTED);

    e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    e.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool AacEncoder::feed(const int16_t* samples, size_t count, int64_t pts_100ns) {
    Impl& e = *impl_;
    if (!e.mft) return false;
    Com<IMFSample> sample;
    if (FAILED(make_sample(reinterpret_cast<const uint8_t*>(samples), count * 2,
                           pts_100ns, 0, sample.put())))
        return false;
    const HRESULT hr = e.mft->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        log_warn("mf: AAC encoder not accepting — receive() first");
        return false;
    }
    return SUCCEEDED(hr);
}

bool AacEncoder::receive(EncodedPacket& out) {
    Impl& e = *impl_;
    if (!e.mft) return false;
    Com<IMFSample> sample;
    bool stream_changed = false;
    const HRESULT hr = pump_output(e.mft.get(), kAudioAllocFallback, sample,
                                   &stream_changed);
    if (stream_changed || hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
    if (FAILED(hr) || !sample) return false;
    return sample_to_packet(sample.get(), out, /*key_default=*/true);
}

void AacEncoder::drain() {
    if (impl_->mft) {
        impl_->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        impl_->mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    }
}

}  // namespace looks::platform
