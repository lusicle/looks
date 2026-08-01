// Single TU compiling miniaudio. Playback only: no decoding,
// no encoding — miniaudio is strictly the audio OUTPUT device; all decode
// is ours.

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4189 4245 4310 4324 4505)
#endif

#include <miniaudio.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
