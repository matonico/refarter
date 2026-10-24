#include <windows.h>
#include <objbase.h>
#include <climits>
#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

int main() {
    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialised)) return 1;
    const auto* codec = avcodec_find_encoder_by_name("h264_mf");
    auto* encoder = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!encoder) { CoUninitialize(); return 1; }
    encoder->width = encoder->height = 64;
    encoder->pix_fmt = AV_PIX_FMT_NV12;
    encoder->time_base = {1, 60};
    encoder->framerate = {60, 1};
    encoder->bit_rate = 128'000;
    // An out-of-range start exhausts even an empty list, without activating any
    // hardware encoder. This exercises FFmpeg's real failed-init cleanup.
    const bool options = av_opt_set_int(encoder->priv_data, "hw_encoding", 1, 0) >= 0 &&
        av_opt_set_int(encoder->priv_data, "mft_index", INT_MAX, 0) >= 0;
    const int result = options ? avcodec_open2(encoder, codec, nullptr) : AVERROR_OPTION_NOT_FOUND;
    avcodec_free_context(&encoder);
    APTTYPE apartment{};
    APTTYPEQUALIFIER qualifier{};
    const HRESULT remaining = CoGetApartmentType(&apartment, &qualifier);
    if (SUCCEEDED(remaining)) CoUninitialize();
    if (!options) { std::cerr << "patched h264_mf options are missing\n"; return 1; }
    if (result != AVERROR_ENCODER_NOT_FOUND) {
        // Windows N/Server installations can lack Media Foundation. That is not
        // the exhausted-list path this regression is intended to validate.
        std::cout << "SKIP: encoder enumeration unavailable, FFmpeg result " << result << '\n';
        return 77;
    }
    if (FAILED(remaining) || apartment != APTTYPE_MTA) {
        std::cerr << "exhausted encoder list consumed the caller's COM initialisation\n";
        return 1;
    }
    return 0;
}
