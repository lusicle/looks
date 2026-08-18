// Dev CLI: exercises ingest end-to-end on a real file. Video sources
// write sidecars only (playback decodes the source natively); stills and
// audio cover art still produce a mezzanine.
//   looks_import <input.mp4> <output_dir> [quality]
// Prints the resulting bundle info; exit 0 on success.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "media/import.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: looks_import <input.mp4> <output_dir> [quality]\n");
        return 2;
    }
    looks::media::ImportOptions options;
    if (argc > 3) options.quality = _wtoi(argv[3]);

    looks::media::ImportProgress progress;
    const looks::media::ImportResult result = looks::media::import_media(
        argv[1], argv[2], options, &progress);

    if (!result.ok) {
        std::fprintf(stderr, "import failed: %s\n", result.error.c_str());
        return 1;
    }
    if (!result.mez_path.empty())
        std::printf("mez: %s\n", result.mez_path.string().c_str());
    if (!result.analysis_path.empty())
        std::printf("analysis: %s\n",
                    result.analysis_path.string().c_str());
    if (!result.thumbs_path.empty())
        std::printf("thumbs: %s\n", result.thumbs_path.string().c_str());
    std::printf("  %ux%u, %u frames, %.3f fps\n", result.width, result.height,
                result.frame_count, result.fps);
    if (!result.pcm_path.empty()) {
        std::printf("pcm: %s\n", result.pcm_path.string().c_str());
        std::printf("  %u ch, %u Hz, %llu frames\n", result.audio_channels,
                    result.audio_sample_rate,
                    static_cast<unsigned long long>(result.audio_frames));
    } else {
        std::printf("no audio track\n");
    }
    return 0;
}
