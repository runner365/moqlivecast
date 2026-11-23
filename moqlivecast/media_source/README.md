# media_source C API — integration notes

These two files add a **C ABI for injecting pre-encoded media** into moqlivecast.
A camera SDK, a hardware encoder or a DVR driver can push frames directly into the
server — no files, no FIFOs, no polling.

They complement the existing `media_source` implementation: both the C API and
`FlvFileSource` feed the same `MediaSourceSink`, and neither depends on the other.

## 1. Copy the files

```
moq_source_c_api.h    -> moqlivecast/media_source/moq_source_c_api.h
moq_source_c_api.cpp  -> moqlivecast/media_source/moq_source_c_api.cpp
```

## 2. Add to the build

In the `moqlivecast` target's source list (where the other `media_source/*.cpp`
entries are), append:

```cmake
    media_source/moq_source_c_api.cpp
```

## 3. Inject the uv loop from the server

`moqlivecast/moq.cpp`:

```cpp
#include "media_source/moq_source_c_api.h"      // add to the include block

/* ... after g_media_sources is created, before uv_run ... */
g_media_sources = std::make_unique<cpp_streamer::MediaSourceManager>(loop);
g_media_sources->Start(cfg.MediaSources());

moq_source_set_loop(loop);        // <-- add this line
```

## 4. Caller side (camera SDK)

```c
#include "moq_source_c_api.h"

/* after the encoder is initialized */
moq_source_t* src = moq_source_create("anon", "cam0", "cam0");
moq_source_start(src);
moq_source_push_video_config(src, MOQ_CODEC_H264, sps_pps, sps_pps_len);

/* per frame */
moq_video_frame_t f = {0};
f.data        = nalu;
f.len         = nalu_len;
f.dts_ms      = dts;
f.pts_ms      = pts;
f.is_keyframe = (nalu_type == 5);
f.format      = MOQ_BITSTREAM_ANNEXB;
int ret = moq_source_push_video(src, &f);
if (ret == -EAGAIN) {
    /* queue full: for real-time sources, drop the frame instead of retrying */
}

/* optional audio: ADTS in, the API strips the header and derives the
 * AudioSpecificConfig from the first frame */
moq_source_push_audio(src, aac_frame, aac_len, dts, pts, MOQ_AUDIO_AAC_ADTS);
```

## 5. Constraints (first version)

| Constraint | Notes |
|---|---|
| `create` / `start` / `stop` / `destroy` must run on the uv loop thread | `push_*` may be called from any thread |
| Video accepts Annex-B or AVCC | Annex-B is converted to AVCC inside the API |
| Audio accepts raw AAC or ADTS | ADTS headers are stripped; the first ADTS frame produces the AudioSpecificConfig |
| The caller must stop all `push_*` before `stop()` | otherwise the sink may be released while a frame is in flight |
| No runtime add/remove of sources | process-lifetime sources are enough for now |

## 6. Why this belongs upstream

- **Zero-copy ingest path**: camera frames go straight into `MediaStreamManager`
  without touching the filesystem.
- **C ABI**: vendor SDKs do not need to link against the C++ runtime, and the API
  is versioned (`MOQ_SOURCE_API_VERSION`).
- **Coexists with `IMediaSource`**: `FlvFileSource` keeps working; both paths share
  the same queueing, catalog generation and backpressure logic.
- **Observable**: `moq_source_get_stats()` exposes counters that a status
  endpoint (`/dev/moq`, `moqctl status`) can report.
