# OBS Timelapse with audio

An OBS Studio plugin that records a timelapse of your scene — and **speeds the voice up with it**, so a progress video sounds like a streamer hyperlapse instead of being silent.

Fork of [Udo/obs-timelapse](https://github.com/Udo/obs-timelapse) (GPL-2.0), which records video only. This fork adds the audio half.

## What it does

- Captures OBS's composed program output (camera window, screen, overlays — whatever your scene shows) every *N* seconds.
- Writes one H.264 movie: `2026-11-16_02-10-25.mkv` straight into your output folder.
- Optionally records OBS's audio mix (mic + desktop audio) and time-compresses it by the **measured** video speed, so the two stay in sync even if frames get dropped or you pause.
- Pitch is preserved, so a 3× timelapse sounds like fast-forwarded speech, not a chipmunk.

Speed = `capture interval × playback FPS`. At 30 FPS: `0.1 s → 3×`, `0.2 s → 6×`, `0.33 s → 10×`.

## Install (macOS)

1. Close OBS.
2. Download `obs-timelapse.zip` from [Releases](../../releases) and unzip it.
3. Copy `obs-timelapse.plugin` into:

```text
~/Library/Application Support/obs-studio/plugins/
```

4. Start OBS. Builds are ad-hoc signed; if macOS complains, right-click the plugin → Open once.

The release build is Apple Silicon (arm64) and matches OBS 33. Building from source for another platform takes a few minutes — see below.

## Use

1. Set up your scene the way you want it to look (e.g. small camera in a corner, screen behind it).
2. **Tools → OBS Timelapse…**
3. Pick the output directory — it defaults to the folder OBS records to.
4. Output mode **MKV timelapse movie**, set the capture interval, keep **Include sped-up audio** checked.
5. **Start Timelapse**, work, then **Stop timelapse**.

Audio comes from OBS **Audio Track 1**. Sources are assigned to every track by default, so a normal scene needs no changes; if you unchecked Track 1 on a source, that source is not in the timelapse.

Output folder after a run contains only the movie:

```text
2026-11-16_02-10-25.mkv
```

While recording you'll see `...mkv.partial` (and short-lived scratch files when audio is on) — they disappear on Stop. Session metadata goes to `~/Library/Application Support/obs-studio/plugin_config/obs-timelapse/sessions/`, never into the output folder.

PNG mode instead writes a folder of lossless frames for manual stitching.

## Build

macOS, against the OBS you already have installed:

```sh
OBS_SOURCE=/path/to/obs-studio-33.0.1 \
OBS_DEPS=/path/to/obs-deps-2026-10-23 \
OBS_QT6=/path/to/obs-deps-qt6-2026-10-23 \
INSTALL=1 zsh build-aux/build-macos-local.sh
```

`OBS_SOURCE` is the obs-studio source tree for the same release as the app (headers); the two obs-deps prefixes are the archives that release was built with (FFmpeg and Qt headers). The script links against the app's own `libobs`/Qt/FFmpeg, so the plugin can only load the libraries OBS already loaded — no SDK download, no duplicate FFmpeg.

Tests:

```sh
cmake --preset macos
cmake --build --preset macos --config RelWithDebInfo
ctest --test-dir build_macos -C RelWithDebInfo --output-on-failure
```

Linux and Windows builds follow the upstream instructions in [docs/building.md](docs/building.md); the audio path uses FFmpeg's `atempo` and the native AAC encoder that ship with OBS.

## Credits

- Upstream plugin, video pipeline, and UI: [Udo/obs-timelapse](https://github.com/Udo/obs-timelapse).
- License: GPL-2.0-or-later (see [LICENSE](LICENSE)). Forks and redistributions must stay under the same license.
