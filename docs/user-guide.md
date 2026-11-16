# User guide

## What gets captured

OBS Timelapse records OBS's composed **program output** — what your viewers would see. Cropping, transforms, filters, overlays, transitions, and browser sources are all included. In Studio Mode it follows Program, not Preview.

Video only in PNG mode. MKV mode can also include the OBS audio mix, time compressed to match the video.

## Starting a session

1. Set up your scene and make sure OBS's video output is running.
2. Open **Tools → OBS Timelapse…**.
3. Pick the output directory, interval, and output mode.
4. Click **Start Timelapse**.
5. Keep an eye on the State and frame counters while it runs.
6. Click **Stop timelapse** before you move, disconnect, or unmount the output storage.

## Settings

| Setting | Meaning |
| --- | --- |
| Output directory | Parent folder for the timestamped session folders. Created on Start if possible. Defaults to the folder OBS records to, so timelapses sit next to normal recordings. |
| Session name | Optional label appended to the UTC timestamp. Unsafe punctuation is replaced and the length is capped. |
| Output mode | `PNG frame folder` or `MKV timelapse movie`. |
| Include sped-up audio | MKV only. Records audio track 1 of the OBS mixer and compresses it by the same factor as the video. |
| Capture interval | Requested time between samples, from 0.1 seconds to 24 hours. |
| Movie playback rate | Frames per second of the finished MKV. Controls playback speed only, not how often frames are captured. |
| PNG compression | `0` is fastest/largest, `9` is slowest/smallest. Lossless at every level. |
| Frame queue | How many full BGRA frames may wait for the background writer, from 2 to 32. |

Sampling is aligned to OBS video frames. For example, 0.1 seconds on a 29.97 FPS output becomes three OBS frames, about 0.1001 seconds. The session manifest records both the requested and the effective interval.

## Choosing an output mode

### PNG frame folder

PNG is heavy on storage and CPU. At high resolutions or short intervals, start with compression 3–6 and a queue of 4. A larger queue rides out short storage stalls but costs memory:

```text
width × height × 4 bytes × queue size
```

At 3840×2160, each queued frame is about 31.6 MiB.

To turn a completed PNG session into a 30 FPS H.264 movie with FFmpeg:

```sh
cd 2026-07-18_01-12-24-studio-build
ffmpeg -framerate 30 -i frame_%08d.png -c:v libx264 -crf 18 -pix_fmt yuv420p timelapse.mkv
```

Change `-framerate` to taste. Frame numbers are just the sample order; they don't encode wall-clock timestamps.

### MKV timelapse movie

Every sample becomes one video frame.

The plugin encodes H.264/YUV420P through the FFmpeg libraries OBS already uses.

The movie is written directly into the output directory as `<session name>.mkv`, with no folder of its own. While running it is named `<session name>.mkv.partial`; on Stop the encoder is drained, the Matroska trailer is written, file handles are closed, and the file is renamed to `<session name>.mkv`.

With audio enabled, Stop takes one extra step: the captured audio is time compressed by the measured ratio between the audio and the written video, encoded as AAC, and muxed under the video. The intermediates (`<session name>.video.mkv` and `<session name>.audio.pcm.partial`) are deleted once `<session name>.mkv` exists.

### Audio

- The tap is mixer channel 1, i.e. OBS's **Audio Track 1**. Sources are assigned to every track by default, so a normally configured scene needs nothing extra. If you unchecked Track 1 for a source, its audio is not in the timelapse.
- Audio is captured as 48 kHz stereo signed 16-bit PCM in `audio.pcm.partial` and encoded to AAC at the configured bitrate (default 160 kbps) during finalization.
- Pausing the timelapse pauses audio capture too.
- The speed-up is measured, not assumed: `audio seconds / video seconds`. Dropped video frames or a pause therefore cannot desynchronize the two streams. At the default 0.1 s interval and 30 FPS, that factor is about 3x; a 4 s interval is about 120x and chains two `atempo` filters.
- Pitch is preserved (`atempo`), so a 3x capture sounds like a fast-forwarded voice rather than a chipmunk. At extreme factors the result is still speech-shaped noise — that is the nature of compressing hours into seconds.
- Audio capture failure never destroys the video: the session reports the error and keeps `<session name>.mkv` (video only).

## Reading the status counters

- **Accepted**: samples copied from OBS into the bounded worker queue.
- **Written**: accepted samples committed to PNG or handed to the MKV writer.
- **Dropped**: samples that didn't make it — the queue was full, capture was stopping, or a writer failure forced pending work to be discarded.

The capture callback never waits for the writer. Keeping OBS rendering, streaming, and recording smooth always wins over saving every last sample.

## Session manifest

The manifest is written to OBS's plugin configuration folder, never next to the
captured media:

```text
<OBS config>/plugin_config/obs-timelapse/sessions/<session name>.json
```

On macOS that is `~/Library/Application Support/obs-studio/plugin_config/obs-timelapse/sessions/`.
It is written at startup and atomically replaced after shutdown, so the output
directory contains only frames or the finished movie. The useful fields:

- `complete`: true only when the writer and the final manifest both succeeded
- `mode`: `png` or `mkv`
- `started_at_utc`, `stop_reason`, and the `output_path` the session wrote to
- `width`, `height`, requested/effective interval, and frame divisor
- accepted, written, and dropped frame counts
- `audio_enabled`, `audio_frames_captured`, `audio_frames_dropped`, `audio_seconds`, and the measured `audio_speedup`
- `error`: the first failure the session hit, if any

Because the manifest exists from the start, you can identify an interrupted run even if OBS, the OS, or the storage disappeared before a clean stop.

## When things go wrong

Failures show up in the dialog and the OBS log. The session flips to Failed, the callback is unregistered, and everything is cleaned up in order — the plugin won't retry forever or hang OBS.

- A leftover `.mkv.partial` means finalization never completed. Keep a copy before experimenting; FFmpeg may be able to inspect or remux it.
- A failed PNG session keeps every frame committed before the failure.
- If `complete` is false, read `error` and `stop_reason` before deciding whether the output is usable.

OBS logs live under **Help → Log Files → View Current Log**. Search for `[obs-timelapse]`.

