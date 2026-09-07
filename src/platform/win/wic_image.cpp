#include "platform/win/wic_image.h"

#include <objbase.h>
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdio>

#include "platform/win/com_ptr.h"

#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "ole32")

namespace looks::platform {

struct GifEncoder::Impl {
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Com<IWICImagingFactory> factory;
    Com<IWICStream> stream;
    Com<IWICBitmapEncoder> encoder;
    std::filesystem::path path;
    uint32_t width = 0, height = 0, loops = 1;
    ~Impl() {
        encoder.reset();
        stream.reset();
        factory.reset();
        if (SUCCEEDED(apartment)) CoUninitialize();
    }
};

GifEncoder::GifEncoder() = default;
GifEncoder::~GifEncoder() = default;

bool GifEncoder::open(const std::filesystem::path& path, uint32_t width,
                       uint32_t height, uint32_t loops, std::string* error) {
    impl_ = std::make_unique<Impl>();
    auto& s = *impl_;
    if (!width || !height || width > 65535 || height > 65535 ||
        uint64_t(width) * height * 4 > MAXDWORD || loops > 65535) {
        if (error) *error = "GIF dimensions or loop count are out of range";
        return false;
    }
    s.path = path;
    s.width = width;
    s.height = height;
    s.loops = loops;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(s.factory.put()))) ||
        FAILED(s.factory->CreateStream(s.stream.put())) ||
        FAILED(s.stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(s.factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, s.encoder.put())) ||
        FAILED(s.encoder->Initialize(s.stream.get(), WICBitmapEncoderNoCache))) {
        if (error) *error = "cannot create GIF encoder";
        return false;
    }
    return true;
}

bool GifEncoder::add(const uint8_t* rgba, uint32_t delay_cs, uint32_t colors,
                      bool dither, bool alpha, float alpha_threshold, std::string* error) {
    auto fail = [&](const char* text) { if (error) *error = text; return false; };
    if (!impl_ || !impl_->encoder || !rgba || !delay_cs || delay_cs > 65535 ||
        colors < 2 || colors > 256) return fail("invalid GIF frame settings");
    auto& s = *impl_;
    Com<IWICBitmap> bitmap;
    Com<IWICPalette> palette;
    Com<IWICFormatConverter> converter;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> properties;
    if (FAILED(s.factory->CreateBitmapFromMemory(s.width, s.height, GUID_WICPixelFormat32bppRGBA,
        s.width * 4, s.width * s.height * 4, const_cast<BYTE*>(rgba), bitmap.put())) ||
        FAILED(s.factory->CreatePalette(palette.put())) ||
        FAILED(palette->InitializeFromBitmap(bitmap.get(), colors, alpha)) ||
        FAILED(s.factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(bitmap.get(), GUID_WICPixelFormat8bppIndexed,
            dither ? WICBitmapDitherTypeErrorDiffusion : WICBitmapDitherTypeNone,
            palette.get(), std::clamp(double(alpha_threshold), 0.0, 100.0), WICBitmapPaletteTypeCustom)) ||
        FAILED(s.encoder->CreateNewFrame(frame.put(), properties.put())) ||
        FAILED(frame->Initialize(properties.get())) ||
        FAILED(frame->SetSize(s.width, s.height))) return fail("cannot prepare GIF frame");
    WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat8bppIndexed ||
        FAILED(frame->SetPalette(palette.get()))) return fail("GIF palette setup failed");
    WICColor entries[256]{};
    UINT count = 0;
    if (FAILED(palette->GetColors(256, entries, &count))) return fail("cannot read GIF palette");
    uint8_t transparent = 0;
    bool has_transparent = false;
    for (UINT i = 0; i < count; ++i)
        if ((entries[i] >> 24) == 0) { transparent = uint8_t(i); has_transparent = true; break; }
    if (alpha && !has_transparent) return fail("GIF palette has no transparent entry");
    Com<IWICMetadataQueryWriter> metadata;
    if (FAILED(frame->GetMetadataQueryWriter(metadata.put()))) return fail("GIF metadata unavailable");
    auto set = [&](const wchar_t* key, VARTYPE type, uint32_t value) {
        PROPVARIANT v{};
        v.vt = type;
        if (type == VT_UI2) v.uiVal = USHORT(value);
        else if (type == VT_UI1) v.bVal = BYTE(value);
        else v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        return SUCCEEDED(metadata->SetMetadataByName(key, &v));
    };
    if (!set(L"/grctlext/Delay", VT_UI2, delay_cs) ||
        !set(L"/grctlext/Disposal", VT_UI1, 2) ||
        !set(L"/grctlext/TransparencyFlag", VT_BOOL, alpha) ||
        (alpha && !set(L"/grctlext/TransparentColorIndex", VT_UI1, transparent)))
        return fail("cannot write GIF timing or transparency");
    std::vector<uint8_t> indices(size_t(s.width) * s.height);
    if (FAILED(converter->CopyPixels(nullptr, s.width, UINT(indices.size()), indices.data())) ||
        FAILED(frame->WritePixels(s.height, s.width, UINT(indices.size()), indices.data())) ||
        FAILED(frame->Commit())) return fail("GIF frame write failed");
    return true;
}

bool GifEncoder::finish(std::string* error) {
    if (!impl_ || !impl_->encoder || FAILED(impl_->encoder->Commit())) {
        if (error) *error = "GIF finalization failed";
        return false;
    }
    auto& s = *impl_;
    s.encoder.reset();
    s.stream.reset();
    if (s.loops != 1) {
        FILE* f = _wfopen(s.path.c_str(), L"r+b");
        if (!f) { if (error) *error = "cannot write GIF loop count"; return false; }
        const uint8_t loop[] = {0x21,0xff,11,'N','E','T','S','C','A','P','E','2','.','0',
            3,1,uint8_t(s.loops),uint8_t(s.loops >> 8),0,0x3b};
        const bool ok = _fseeki64(f, -1, SEEK_END) == 0 && std::fgetc(f) == 0x3b &&
            _fseeki64(f, -1, SEEK_END) == 0 && std::fwrite(loop, 1, sizeof(loop), f) == sizeof(loop);
        std::fclose(f);
        if (!ok) { if (error) *error = "cannot write GIF loop extension"; return false; }
    }
    return true;
}

bool encode_png(const std::filesystem::path& path, const uint8_t* rgba,
                 uint32_t width, uint32_t height, std::string* error) {
    auto fail = [&](const char* text) { if (error) *error = text; return false; };
    if (!rgba || !width || !height || uint64_t(width) * height * 4 > MAXDWORD)
        return fail("invalid PNG dimensions");
    struct Apartment {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
    } apartment;
    Com<IWICImagingFactory> factory;
    Com<IWICStream> stream;
    Com<IWICBitmapEncoder> encoder;
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> properties;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(factory.put()))) ||
        FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) ||
        FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.put(), properties.put())) ||
        FAILED(frame->Initialize(properties.get())) || FAILED(frame->SetSize(width, height)))
        return fail("cannot create PNG encoder");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppRGBA;
    Com<IWICBitmap> bitmap;
    Com<IWICFormatConverter> converter;
    if (FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppRGBA,
                                                width * 4, width * height * 4,
                                                const_cast<BYTE*>(rgba), bitmap.put())) ||
        FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(bitmap.get(), format, WICBitmapDitherTypeNone,
                                      nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->WriteSource(converter.get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) return fail("PNG write failed");
    return true;
}

bool decode_gif(const uint8_t* bytes, size_t size,
                const std::function<bool(const AnimationFrame&)>& accept,
                std::string* error) {
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (!bytes || !size || size > MAXDWORD) return fail("invalid GIF size");
    struct Apartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    Com<IWICImagingFactory> factory;
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> decoder;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(factory.put()))) ||
        FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes), DWORD(size))) ||
        FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr,
                                                WICDecodeMetadataCacheOnLoad, decoder.put())))
        return fail("cannot decode GIF");
    GUID format{};
    if (FAILED(decoder->GetContainerFormat(&format)) || format != GUID_ContainerFormatGif)
        return fail("not a GIF");
    auto value = [](IWICMetadataQueryReader* reader, const wchar_t* key, uint32_t fallback) {
        PROPVARIANT v{};
        uint32_t n = fallback;
        if (reader && SUCCEEDED(reader->GetMetadataByName(key, &v))) {
            if (v.vt == VT_UI1) n = v.bVal;
            else if (v.vt == VT_UI2) n = v.uiVal;
            else if (v.vt == VT_UI4) n = v.ulVal;
            else if (v.vt == VT_BOOL) n = v.boolVal != VARIANT_FALSE;
        }
        PropVariantClear(&v);
        return n;
    };
    Com<IWICMetadataQueryReader> global;
    decoder->GetMetadataQueryReader(global.put());
    const uint32_t w = value(global.get(), L"/logscrdesc/Width", 0);
    const uint32_t h = value(global.get(), L"/logscrdesc/Height", 0);
    UINT count = 0;
    if (!w || !h || uint64_t(w) * h * 4 > MAXDWORD ||
        FAILED(decoder->GetFrameCount(&count)) || !count) return fail("invalid GIF canvas");
    std::array<uint8_t, 4> background{};
    Com<IWICPalette> palette;
    if (SUCCEEDED(factory->CreatePalette(palette.put())) &&
        SUCCEEDED(decoder->CopyPalette(palette.get()))) {
        WICColor colors[256]{};
        UINT used = 0;
        const uint32_t bg = value(global.get(), L"/logscrdesc/BackgroundColorIndex", 0);
        if (SUCCEEDED(palette->GetColors(256, colors, &used)) && bg < used) {
            const uint32_t c = colors[bg];
            background = {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c), 255};
        }
    }
    std::vector<uint8_t> canvas(size_t(w) * h * 4), previous, patch;
    for (UINT i = 0; i < count; ++i) {
        Com<IWICBitmapFrameDecode> frame;
        Com<IWICMetadataQueryReader> metadata;
        Com<IWICFormatConverter> converter;
        if (FAILED(decoder->GetFrame(i, frame.put()))) return fail("missing GIF frame");
        frame->GetMetadataQueryReader(metadata.put());
        const uint32_t left = value(metadata.get(), L"/imgdesc/Left", 0);
        const uint32_t top = value(metadata.get(), L"/imgdesc/Top", 0);
        const uint32_t disposal = value(metadata.get(), L"/grctlext/Disposal", 0);
        const bool transparent = value(metadata.get(), L"/grctlext/TransparencyFlag", 0) != 0;
        uint32_t delay = value(metadata.get(), L"/grctlext/Delay", 0) * 10;
        if (!delay) delay = 100;
        if (i == 0 && !transparent)
            for (size_t p = 0; p < canvas.size(); p += 4)
                std::copy(background.begin(), background.end(), canvas.begin() + p);
        UINT fw = 0, fh = 0;
        if (FAILED(frame->GetSize(&fw, &fh)) || !fw || !fh ||
            uint64_t(fw) * fh * 4 > MAXDWORD || left >= w || top >= h)
            return fail("invalid GIF frame bounds");
        if (FAILED(factory->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            return fail("cannot convert GIF frame");
        patch.resize(size_t(fw) * fh * 4);
        if (FAILED(converter->CopyPixels(nullptr, fw * 4, UINT(patch.size()), patch.data())))
            return fail("cannot read GIF pixels");
        if (disposal == 3) previous = canvas;
        const uint32_t rw = std::min(fw, w - left), rh = std::min(fh, h - top);
        for (uint32_t y = 0; y < rh; ++y)
            for (uint32_t x = 0; x < rw; ++x) {
                const uint8_t* p = patch.data() + (size_t(y) * fw + x) * 4;
                if (p[3]) std::copy_n(p, 4, canvas.data() + (size_t(y + top) * w + x + left) * 4);
            }
        if (!accept({w, h, i, count, delay, canvas.data()})) return fail("GIF import stopped");
        if (disposal == 2) {
            const std::array<uint8_t, 4> clear = transparent ? std::array<uint8_t, 4>{} : background;
            for (uint32_t y = 0; y < rh; ++y)
                for (uint32_t x = 0; x < rw; ++x)
                    std::copy(clear.begin(), clear.end(), canvas.begin() + (size_t(y + top) * w + x + left) * 4);
        } else if (disposal == 3) canvas.swap(previous);
    }
    return true;
}

bool decode_image_rgba(const uint8_t* bytes, size_t size, uint32_t* width,
                       uint32_t* height, std::vector<uint8_t>* rgba,
                       std::string* error) {
    auto fail = [&](const char* what) {
        if (error) *error = what;
        return false;
    };
    if (!bytes || !size) return fail("empty image");

    // Call CoUninitialize only when CoInitializeEx returns S_OK.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool balance = co == S_OK;
    bool ok = false;
    {
        Com<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.put())))) {
            if (balance) CoUninitialize();
            return fail("WIC unavailable");
        }
        Com<IWICStream> stream;
        if (FAILED(factory->CreateStream(stream.put())) ||
            FAILED(stream->InitializeFromMemory(
                const_cast<BYTE*>(bytes), static_cast<DWORD>(size)))) {
            if (balance) CoUninitialize();
            return fail("WIC stream");
        }
        Com<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromStream(
                stream.get(), nullptr, WICDecodeMetadataCacheOnDemand,
                decoder.put()))) {
            if (balance) CoUninitialize();
            return fail("undecodable image");
        }
        Com<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.put()))) {
            if (balance) CoUninitialize();
            return fail("no image frame");
        }
        Com<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(
                frame.get(), GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom))) {
            if (balance) CoUninitialize();
            return fail("pixel convert");
        }
        UINT w = 0, h = 0;
        if (FAILED(converter->GetSize(&w, &h)) || !w || !h) {
            if (balance) CoUninitialize();
            return fail("bad image size");
        }
        rgba->resize(static_cast<size_t>(w) * h * 4);
        const HRESULT hr = converter->CopyPixels(
            nullptr, w * 4, static_cast<UINT>(rgba->size()), rgba->data());
        if (SUCCEEDED(hr)) {
            *width = w;
            *height = h;
            ok = true;
        }
    }
    if (balance) CoUninitialize();
    return ok ? true : fail("pixel copy");
}

}  // namespace looks::platform
