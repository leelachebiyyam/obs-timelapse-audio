#pragma once

#include <QString>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct audio_data;

namespace timelapse {

// Taps one OBS audio mixer channel and appends the converted PCM to a file.
// The OBS audio thread only copies into a bounded queue; a worker thread writes.
class AudioRecorder final {
public:
	AudioRecorder(std::uint32_t sampleRate, std::uint32_t channels);
	~AudioRecorder();

	AudioRecorder(const AudioRecorder &) = delete;
	AudioRecorder &operator=(const AudioRecorder &) = delete;

	bool start(const QString &path, QString &error);
	void setPaused(bool paused) noexcept;
	void stop() noexcept;

	std::uint64_t framesCaptured() const noexcept;
	std::uint64_t framesDropped() const noexcept;
	QString path() const;
	QString error() const;
	bool hasFailed() const noexcept;

private:
	static void rawAudioCallback(void *parameter, size_t mixIndex, audio_data *data) noexcept;
	void receive(const audio_data &data) noexcept;
	void workerLoop() noexcept;
	void setFailure(const QString &error) noexcept;

	std::size_t bytesPerFrame() const noexcept;

	const std::uint32_t sampleRate_;
	const std::uint32_t channels_;
	QString path_;

	std::atomic<bool> accepting_{false};
	std::atomic<bool> paused_{false};
	std::atomic<bool> failed_{false};
	std::atomic<std::uint64_t> framesCaptured_{0};
	std::atomic<std::uint64_t> framesDropped_{0};
	std::atomic<bool> subscribed_{false};

	std::mutex callbackMutex_;
	std::condition_variable callbackReady_;
	bool callbacksEnabled_ = false;
	std::size_t callbacksInFlight_ = 0;

	mutable std::mutex statusMutex_;
	QString error_;

	std::mutex queueMutex_;
	std::condition_variable queueReady_;
	std::deque<std::vector<std::uint8_t>> pending_;
	std::vector<std::vector<std::uint8_t>> spare_;
	std::size_t pendingBytes_ = 0;
	bool stopRequested_ = false;
	std::thread worker_;
	std::FILE *file_ = nullptr;
};

} // namespace timelapse
