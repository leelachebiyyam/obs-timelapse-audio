#include "audio-recorder.hpp"

#include <obs-module.h>
#include <obs.h>
#include <plugin-support.h>

#include <media-io/audio-io.h>

#include <cstring>
#include <utility>

namespace timelapse {
namespace {

// Audio track 1 of the OBS mixer. Sources are assigned to every track by
// default, so this carries the full program mix once.
constexpr std::size_t MixerIndex = 0;

// Enough for several seconds of 48 kHz stereo, so a short disk stall does not
// cost samples, but bounded so a stalled disk cannot grow memory forever.
constexpr std::size_t MaximumPendingBytes = 4 * 1024 * 1024;
constexpr std::size_t MaximumSpareChunks = 64;

QString describeOpenFailure(const QString &path)
{
	return QStringLiteral("Could not open the audio scratch file: %1").arg(path);
}

} // namespace

AudioRecorder::AudioRecorder(std::uint32_t sampleRate, std::uint32_t channels)
	: sampleRate_(sampleRate), channels_(channels == 0 ? 2 : channels)
{
}

AudioRecorder::~AudioRecorder()
{
	stop();
}

std::size_t AudioRecorder::bytesPerFrame() const noexcept
{
	return static_cast<std::size_t>(channels_) * sizeof(std::int16_t);
}

bool AudioRecorder::start(const QString &path, QString &error)
{
	if (file_ || worker_.joinable()) {
		error = QStringLiteral("The audio recorder has already been started.");
		return false;
	}

	const QByteArray encodedPath = path.toUtf8();
	file_ = std::fopen(encodedPath.constData(), "wb");
	if (!file_) {
		error = describeOpenFailure(path);
		return false;
	}
	path_ = path;

	audio_convert_info conversion{};
	conversion.samples_per_sec = sampleRate_;
	conversion.format = AUDIO_FORMAT_16BIT;
	conversion.speakers = channels_ == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
	conversion.allow_clipping = true;

	stopRequested_ = false;
	accepting_.store(true);
	{
		std::lock_guard<std::mutex> lock(callbackMutex_);
		callbacksEnabled_ = true;
	}

	try {
		worker_ = std::thread(&AudioRecorder::workerLoop, this);
	} catch (const std::exception &exception) {
		error = QStringLiteral("Could not start the audio writer: %1").arg(QString::fromUtf8(exception.what()));
		accepting_.store(false);
		std::fclose(file_);
		file_ = nullptr;
		path_.clear();
		return false;
	}

	obs_add_raw_audio_callback(MixerIndex, &conversion, &AudioRecorder::rawAudioCallback, this);
	subscribed_.store(true);
	return true;
}

void AudioRecorder::setPaused(bool paused) noexcept
{
	if (paused == paused_.load())
		return;
	paused_.store(paused);
}

void AudioRecorder::rawAudioCallback(void *parameter, size_t mixIndex, audio_data *data) noexcept
{
	if (!parameter || !data || mixIndex != MixerIndex)
		return;

	auto *recorder = static_cast<AudioRecorder *>(parameter);
	bool entered = false;
	try {
		{
			std::lock_guard<std::mutex> lock(recorder->callbackMutex_);
			if (!recorder->callbacksEnabled_)
				return;
			++recorder->callbacksInFlight_;
			entered = true;
		}
		recorder->receive(*data);
	} catch (...) {
		recorder->framesDropped_.fetch_add(data->frames, std::memory_order_relaxed);
	}

	if (entered) {
		try {
			std::lock_guard<std::mutex> lock(recorder->callbackMutex_);
			if (--recorder->callbacksInFlight_ == 0)
				recorder->callbackReady_.notify_all();
		} catch (...) {
		}
	}
}

void AudioRecorder::receive(const audio_data &data) noexcept
{
	if (data.frames == 0 || !data.data[0])
		return;

	const std::size_t bytes = static_cast<std::size_t>(data.frames) * bytesPerFrame();
	std::vector<std::uint8_t> chunk;
	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		if (paused_.load(std::memory_order_relaxed))
			return;
		if (!accepting_.load(std::memory_order_relaxed) || pendingBytes_ + bytes > MaximumPendingBytes) {
			framesDropped_.fetch_add(data.frames, std::memory_order_relaxed);
			return;
		}
		if (!spare_.empty()) {
			chunk = std::move(spare_.back());
			spare_.pop_back();
		}
	}

	try {
		chunk.resize(bytes);
		std::memcpy(chunk.data(), data.data[0], bytes);
	} catch (...) {
		std::lock_guard<std::mutex> lock(queueMutex_);
		spare_.push_back(std::move(chunk));
		framesDropped_.fetch_add(data.frames, std::memory_order_relaxed);
		return;
	}

	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		if (paused_.load() || !accepting_.load() || stopRequested_) {
			if (spare_.size() < MaximumSpareChunks)
				spare_.push_back(std::move(chunk));
			framesDropped_.fetch_add(data.frames, std::memory_order_relaxed);
			return;
		}
		pendingBytes_ += bytes;
		pending_.push_back(std::move(chunk));
	}
	framesCaptured_.fetch_add(data.frames, std::memory_order_relaxed);
	queueReady_.notify_one();
}

void AudioRecorder::workerLoop() noexcept
{
	try {
		for (;;) {
			std::vector<std::uint8_t> chunk;
			{
				std::unique_lock<std::mutex> lock(queueMutex_);
				queueReady_.wait(lock, [this] { return stopRequested_ || !pending_.empty(); });
				if (pending_.empty()) {
					if (stopRequested_)
						break;
					continue;
				}
				chunk = std::move(pending_.front());
				pending_.pop_front();
				pendingBytes_ -= chunk.size();
			}

			if (!chunk.empty() && file_ &&
			    std::fwrite(chunk.data(), 1, chunk.size(), file_) != chunk.size()) {
				setFailure(QStringLiteral("Could not write captured audio to %1.").arg(path_));
				std::lock_guard<std::mutex> lock(queueMutex_);
				framesDropped_.fetch_add(pendingBytes_ / bytesPerFrame(), std::memory_order_relaxed);
				pending_.clear();
				pendingBytes_ = 0;
				break;
			}

			{
				std::lock_guard<std::mutex> lock(queueMutex_);
				if (spare_.size() < MaximumSpareChunks)
					spare_.push_back(std::move(chunk));
			}
		}
	} catch (...) {
		setFailure(QStringLiteral("The audio writer failed unexpectedly."));
	}
}

void AudioRecorder::stop() noexcept
{
	if (!file_ && !worker_.joinable())
		return;

	accepting_.store(false);
	{
		std::lock_guard<std::mutex> lock(callbackMutex_);
		callbacksEnabled_ = false;
	}
	if (subscribed_.exchange(false))
		obs_remove_raw_audio_callback(MixerIndex, &AudioRecorder::rawAudioCallback, this);
	{
		std::unique_lock<std::mutex> lock(callbackMutex_);
		callbackReady_.wait(lock, [this] { return callbacksInFlight_ == 0; });
	}

	{
		std::lock_guard<std::mutex> lock(queueMutex_);
		stopRequested_ = true;
	}
	queueReady_.notify_all();
	if (worker_.joinable()) {
		if (worker_.get_id() == std::this_thread::get_id())
			std::terminate();
		worker_.join();
	}

	if (file_) {
		std::fflush(file_);
		std::fclose(file_);
		file_ = nullptr;
	}
}

std::uint64_t AudioRecorder::framesCaptured() const noexcept
{
	return framesCaptured_.load();
}

std::uint64_t AudioRecorder::framesDropped() const noexcept
{
	return framesDropped_.load();
}

QString AudioRecorder::path() const
{
	return path_;
}

QString AudioRecorder::error() const
{
	std::lock_guard<std::mutex> lock(statusMutex_);
	return error_;
}

bool AudioRecorder::hasFailed() const noexcept
{
	return failed_.load();
}

void AudioRecorder::setFailure(const QString &error) noexcept
{
	try {
		{
			std::lock_guard<std::mutex> lock(statusMutex_);
			if (error_.isEmpty())
				error_ = error;
		}
		failed_.store(true);
		accepting_.store(false);
		obs_log(LOG_ERROR, "audio capture failed: %s", error.toUtf8().constData());
	} catch (...) {
		failed_.store(true);
	}
}

} // namespace timelapse
