#pragma once

#include <QString>

#include <cstdint>

namespace timelapse {

struct AudioMergeResult {
	double videoSeconds = 0.0;
	double audioSeconds = 0.0;
	double speedup = 0.0;
	std::uint64_t audioFrames = 0;
	std::uint64_t encodedFrames = 0;
};

// Copies the video stream from `videoPath` and muxes it with the PCM in
// `pcmPath`, time compressed by audioSeconds / videoSeconds so both streams end
// together. The result is written to `outputPath` and the source files are left
// untouched. `pcmPath` holds interleaved signed 16-bit samples.
bool muxTimelapseAudio(const QString &videoPath, const QString &pcmPath, const QString &outputPath,
		       std::uint32_t sampleRate, std::uint32_t channels, int bitrateKbps, AudioMergeResult &result,
		       QString &error);

} // namespace timelapse
