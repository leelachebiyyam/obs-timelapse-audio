#include "audio-muxer.hpp"
#include "core-utils.hpp"
#include "mkv-writer.hpp"
#include "png-writer.hpp"
#include "settings.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTimeZone>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char *message)
{
	if (!condition) {
		qCritical().noquote() << message;
		++failures;
	}
}

} // namespace

int main(int argc, char **argv)
{
	QCoreApplication application(argc, argv);
	using namespace timelapse;

	expect(calculateFrameDivisor(10.0, 60, 1) == 600, "10 seconds at 60 FPS should use divisor 600");
	expect(calculateFrameDivisor(0.1, 30000, 1001) == 3, "0.1 seconds at 29.97 FPS should use divisor 3");
	expect(calculateFrameDivisor(0.0, 60, 1) == 1, "invalid interval should safely clamp to divisor 1");
	expect(calculateFrameDivisor(1.0, 0, 1) == 1, "invalid FPS should safely clamp to divisor 1");
	expect(std::abs(calculateEffectiveInterval(3, 30000, 1001) - 0.1001) < 0.000001,
	       "effective NTSC interval should be exact");

	expect(sanitizeNamePrefix(QStringLiteral("  Studio / build: 42  ")) == QStringLiteral("Studio_build_42"),
	       "unsafe filename characters should collapse to underscores");
	expect(sanitizeNamePrefix(QStringLiteral("...")) == QString(), "punctuation-only prefix should become empty");
	expect(sanitizeNamePrefix(QString(100, QLatin1Char('a'))).size() == 64, "prefix should be length bounded");

	expect(formatDuration(0.0) == QStringLiteral("0.0 s"), "zero duration should format with one decimal");
	expect(formatDuration(9.54) == QStringLiteral("9.5 s"), "short durations should keep one decimal");
	expect(formatDuration(42.4) == QStringLiteral("42 s"), "sub-minute durations should round to seconds");
	expect(formatDuration(125.0) == QStringLiteral("2 min 5 s"), "minute durations should split into min/s");
	expect(formatDuration(3720.0) == QStringLiteral("1 h 2 min"), "hour durations should split into h/min");
	expect(formatDuration(-5.0) == QStringLiteral("0.0 s"), "negative durations should clamp to zero");

	const QDateTime timestamp(QDate(2026, 7, 17), QTime(12, 34, 56), QTimeZone::UTC);
	expect(makeSessionName(QStringLiteral("Demo"), timestamp) == QStringLiteral("2026-07-17_12-34-56-Demo"),
	       "session name should combine UTC timestamp and prefix");
	expect(makeSessionName(QString(), timestamp) == QStringLiteral("2026-07-17_12-34-56"),
	       "empty prefix should produce timestamp-only session name");

	CaptureSettings settings;
	settings.outputDirectory.clear();
	expect(!validateSettings(settings).isEmpty(), "empty output directory should be rejected");
	settings.outputDirectory = QStringLiteral("/tmp/example");
	settings.intervalSeconds = 0.01;
	expect(!validateSettings(settings).isEmpty(), "too-small interval should be rejected");
	settings.intervalSeconds = 1.0;
	expect(validateSettings(settings).isEmpty(), "valid settings should pass validation");

	QTemporaryDir output;
	expect(output.isValid(), "temporary MKV test directory should be available");
	const QString partialPath = output.filePath(QStringLiteral("test.mkv.partial"));
	const QString finalPath = output.filePath(QStringLiteral("test.mkv"));
	QString error;
	MkvWriter writer;
	expect(writer.open(partialPath, finalPath, 64, 64, 30, error), qPrintable(error));
	std::vector<uint8_t> pixels(64 * 64 * 4, 0xff);
	for (int frame = 0; frame < 3 && error.isEmpty(); ++frame) {
		pixels[static_cast<std::size_t>(frame) * 4] = static_cast<uint8_t>(frame * 80);
		expect(writer.writeFrame(pixels.data(), 64 * 4, error), qPrintable(error));
	}
	expect(writer.finish(error), qPrintable(error));
	expect(QFileInfo::exists(finalPath), "completed MKV should be renamed to its final path");
	expect(!QFileInfo::exists(partialPath), "completed MKV should not leave a partial file");
	expect(QFileInfo(finalPath).size() > 0, "completed MKV should not be empty");

	{
		const QString oddPartial = output.filePath(QStringLiteral("odd.mkv.partial"));
		const QString oddFinal = output.filePath(QStringLiteral("odd.mkv"));
		QString oddError;
		MkvWriter oddWriter;
		expect(oddWriter.open(oddPartial, oddFinal, 63, 63, 30, oddError), qPrintable(oddError));
		std::vector<uint8_t> oddPixels(63 * 63 * 4, 0x40);
		expect(oddWriter.writeFrame(oddPixels.data(), 63 * 4, oddError), qPrintable(oddError));
		expect(oddWriter.finish(oddError), qPrintable(oddError));
		expect(QFileInfo::exists(oddFinal), "odd-dimension capture should still produce an MKV");

		QString tinyError;
		MkvWriter tinyWriter;
		expect(!tinyWriter.open(output.filePath(QStringLiteral("tiny.mkv.partial")),
					output.filePath(QStringLiteral("tiny.mkv")), 1, 1, 30, tinyError),
		       "a 1x1 capture cannot produce 4:2:0 video and should fail to open");
		expect(!tinyError.isEmpty(), "rejected MKV open should return an operator-facing error");
		expect(!QFileInfo::exists(output.filePath(QStringLiteral("tiny.mkv.partial"))),
		       "rejected MKV open should not leave a partial file behind");
		tinyError.clear();
		expect(tinyWriter.open(output.filePath(QStringLiteral("retry.mkv.partial")),
				       output.filePath(QStringLiteral("retry.mkv")), 64, 64, 30, tinyError),
		       "an MKV writer should allow retry after a failed open");
		expect(tinyWriter.finish(tinyError), qPrintable(tinyError));
	}

	const QString pngPath = output.filePath(QStringLiteral("frame_00000001.png"));
	std::vector<uint8_t> pngPixels(16 * 16 * 4, 0);
	for (std::size_t index = 0; index < pngPixels.size(); index += 4) {
		pngPixels[index] = 0x20;
		pngPixels[index + 1] = 0x40;
		pngPixels[index + 2] = 0x80;
		pngPixels[index + 3] = 0xff;
	}
	error.clear();
	expect(writePngAtomically(pngPath, pngPixels.data(), 16, 16, 16 * 4, 6, error), qPrintable(error));
	expect(QFileInfo::exists(pngPath), "atomic PNG writer should commit the final path");
	const QImage pngImage(pngPath);
	expect(!pngImage.isNull() && pngImage.size() == QSize(16, 16),
	       "committed PNG should decode at its source size");
	expect(pngImage.pixelColor(0, 0) == QColor(0x80, 0x40, 0x20),
	       "committed PNG should preserve BGRA source pixels");

	error.clear();
	expect(!writePngAtomically(output.path(), pngPixels.data(), 16, 16, 16 * 4, 6, error),
	       "atomic PNG writer should reject a directory as its destination");
	expect(!error.isEmpty(), "failed PNG write should return an operator-facing error");

	const std::vector<double> tempoFactors = calculateTempoFactors(300.0);
	double tempoProduct = 1.0;
	for (const double factor : tempoFactors) {
		tempoProduct *= factor;
		expect(factor >= 0.5 && factor <= 100.0, "every tempo factor should stay inside the atempo range");
	}
	expect(std::abs(tempoProduct - 300.0) < 0.01, "chained tempo factors should multiply back to the speedup");
	expect(calculateTempoFactors(1.0).empty(), "unity speed should not add tempo filters");
	expect(calculateTempoFactors(0.0).empty(), "invalid speedup should not add tempo filters");
	expect(std::abs(calculateTempoFactors(4.0).front() - 4.0) < 1.0e-9, "moderate speedups should use one filter");

	{
		QTemporaryDir mergeOutput;
		expect(mergeOutput.isValid(), "temporary audio merge directory should be available");

		const QString videoPartial = mergeOutput.filePath(QStringLiteral("video.mkv.partial"));
		const QString videoFinal = mergeOutput.filePath(QStringLiteral("video.mkv"));
		QString mergeError;
		MkvWriter mergeWriter;
		expect(mergeWriter.open(videoPartial, videoFinal, 64, 64, 30, mergeError), qPrintable(mergeError));
		std::vector<uint8_t> mergePixels(64 * 64 * 4, 0x40);
		for (int frame = 0; frame < 30; ++frame)
			expect(mergeWriter.writeFrame(mergePixels.data(), 64 * 4, mergeError), qPrintable(mergeError));
		expect(mergeWriter.finish(mergeError), qPrintable(mergeError));

		constexpr int SampleRate = 48000;
		constexpr int Channels = 2;
		const QString pcmPath = mergeOutput.filePath(QStringLiteral("audio.pcm"));
		QFile pcm(pcmPath);
		expect(pcm.open(QIODevice::WriteOnly), "audio scratch file should open for writing");
		std::vector<std::int16_t> samples(static_cast<std::size_t>(SampleRate) * Channels * 3);
		double phase = 0.0;
		for (std::size_t frame = 0; frame < samples.size() / Channels; ++frame) {
			phase += 0.05;
			const auto value = static_cast<std::int16_t>(std::sin(phase) * 12000.0);
			for (int channel = 0; channel < Channels; ++channel)
				samples[frame * Channels + channel] = value;
		}
		pcm.write(reinterpret_cast<const char *>(samples.data()),
			  static_cast<qint64>(samples.size() * sizeof(std::int16_t)));
		pcm.close();

		const QString mergedPath = mergeOutput.filePath(QStringLiteral("merged.mkv"));
		AudioMergeResult merge;
		expect(muxTimelapseAudio(videoFinal, pcmPath, mergedPath, SampleRate, Channels, 160, merge, mergeError),
		       qPrintable(mergeError));
		expect(std::abs(merge.audioSeconds - 3.0) < 0.01, "merge should measure the recorded audio duration");
		expect(std::abs(merge.videoSeconds - 1.0) < 0.05, "merge should measure the video duration");
		expect(std::abs(merge.speedup - 3.0) < 0.05, "merge should compress 3 s of audio into 1 s of video");
		expect(merge.encodedFrames > 0, "merge should encode compressed audio frames");

		AVFormatContext *inspect = nullptr;
		expect(avformat_open_input(&inspect, mergedPath.toUtf8().constData(), nullptr, nullptr) >= 0,
		       "merged timelapse should open");
		if (inspect) {
			avformat_find_stream_info(inspect, nullptr);
			int videoStreams = 0;
			int audioStreams = 0;
			for (unsigned index = 0; index < inspect->nb_streams; ++index) {
				const AVMediaType type = inspect->streams[index]->codecpar->codec_type;
				if (type == AVMEDIA_TYPE_VIDEO)
					++videoStreams;
				else if (type == AVMEDIA_TYPE_AUDIO)
					++audioStreams;
			}
			expect(videoStreams == 1, "merged timelapse should keep exactly one video stream");
			expect(audioStreams == 1, "merged timelapse should add exactly one audio stream");
			const double duration =
				static_cast<double>(inspect->duration) / AV_TIME_BASE;
			expect(std::abs(duration - 1.0) < 0.1, "merged timelapse should stay as long as the video");
			avformat_close_input(&inspect);
		}
	}

	return failures == 0 ? 0 : 1;
}
