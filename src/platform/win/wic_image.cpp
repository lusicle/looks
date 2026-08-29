#include "platform/win/wic_image.h"

#include <objbase.h>
#include <wincodec.h>

#include "platform/win/com_ptr.h"

#pragma comment(lib, "windowscodecs")
#pragma comment(lib, "ole32")

namespace looks::platform {

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
