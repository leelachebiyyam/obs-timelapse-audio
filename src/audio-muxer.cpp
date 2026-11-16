#include "audio-muxer.hpp"

#include "core-utils.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <QFile>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <deque>
#include <memory>
#include <vector>

namespace timelapse {
namespace {

QString ffmpegError(const QString &operation, int code)
{
	char message[AV_ERROR_MAX_STRING_SIZE]{};
	av_strerror(code, message, sizeof(message));
	return QStringLiteral("%1: %2").arg(operation, QString::fromUtf8(message));
}

struct InputFile {
	AVFormatContext *context = nullptr;

	~InputFile()
	{
		if (context)
			avformat_close_input(&context);
	}
};

struct OutputFile {
	AVFormatContext *context = nullptr;
	bool opened = false;

	~OutputFile()
	{
		if (!context)
			return;
		if (opened && !(context->oformat->flags & AVFMT_NOFILE))
			avio_closep(&context->pb);
		avformat_free_context(context);
	}
};

struct FilterGraph {
	AVFilterGraph *graph = nullptr;
	AVFilterContext *source = nullptr;
	AVFilterContext *sink = nullptr;

	~FilterGraph()
	{
		if (graph)
			avfilter_graph_free(&graph);
	}
};

struct CodecHandle {
	AVCodecContext *context = nullptr;

	~CodecHandle()
	{
		if (context)
			avcodec_free_context(&context);
	}
};

struct FrameHandle {
	AVFrame *frame = nullptr;

	~FrameHandle()
	{
		if (frame)
			av_frame_free(&frame);
	}
};

struct PacketHandle {
	AVPacket *packet = nullptr;

	~PacketHandle()
	{
		if (packet)
			av_packet_free(&packet);
	}
};

struct ScopedFile {
	std::FILE *file = nullptr;

	~ScopedFile()
	{
		if (file)
			std::fclose(file);
	}
};

QString sampleFormatName(AVSampleFormat format)
{
	const char *name = av_get_sample_fmt_name(format);
	return name ? QString::fromUtf8(name) : QStringLiteral("fltp");
}

class VideoFeed {
public:
	VideoFeed(AVFormatContext *input, int streamIndex, AVStream *output) : input_(input), streamIndex_(streamIndex),
									     output_(output)
	{
	}

	~VideoFeed()
	{
		for (AVPacket *packet : queue_)
			av_packet_free(&packet);
	}

	// Writes every buffered video packet that starts at or before `seconds`, and
	// refills the one-packet look-ahead from the demuxer as needed.
	bool writeUpTo(double seconds, AVFormatContext *output, QString &error)
	{
		for (;;) {
			if (queue_.empty() && eof_)
				return true;
			if (queue_.empty() && !fill(error))
				return false;
			if (queue_.empty())
				return true;

			AVPacket *packet = queue_.front();
			const double packetSeconds = packetTime(packet);
			if (packetSeconds > seconds)
				return true;

			queue_.pop_front();
			packet->stream_index = output_->index;
			av_packet_rescale_ts(packet, input_->streams[streamIndex_]->time_base, output_->time_base);
			packet->pos = -1;
			const int result = av_interleaved_write_frame(output, packet);
			av_packet_free(&packet);
			if (result < 0) {
				error = ffmpegError(QStringLiteral("Could not copy a video frame into the timelapse"),
						    result);
				return false;
			}
			++written_;
		}
	}

	bool flush(AVFormatContext *output, QString &error)
	{
		return writeUpTo(std::numeric_limits<double>::infinity(), output, error);
	}

	std::uint64_t written() const noexcept { return written_; }

private:
	bool fill(QString &error)
	{
		for (;;) {
			if (queue_.size() > 0)
				return true;
			PacketHandle packet;
			packet.packet = av_packet_alloc();
			if (!packet.packet) {
				error = QStringLiteral("Could not allocate a video packet.");
				return false;
			}
			const int result = av_read_frame(input_, packet.packet);
			if (result == AVERROR_EOF) {
				eof_ = true;
				return true;
			}
			if (result < 0) {
				error = ffmpegError(QStringLiteral("Could not read the timelapse video"), result);
				return false;
			}
			if (packet.packet->stream_index != streamIndex_)
				continue;
			queue_.push_back(packet.packet);
			packet.packet = nullptr;
			return true;
		}
	}

	double packetTime(const AVPacket *packet) const
	{
		const int64_t timestamp = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
		if (timestamp == AV_NOPTS_VALUE)
			return 0.0;
		return static_cast<double>(timestamp) * av_q2d(input_->streams[streamIndex_]->time_base);
	}

	AVFormatContext *input_;
	int streamIndex_;
	AVStream *output_;
	std::deque<AVPacket *> queue_;
	bool eof_ = false;
	std::uint64_t written_ = 0;
};

} // namespace

bool muxTimelapseAudio(const QString &videoPath, const QString &pcmPath, const QString &outputPath,
		       std::uint32_t sampleRate, std::uint32_t channels, int bitrateKbps, AudioMergeResult &result,
		       QString &error)
{
	if (sampleRate == 0 || channels == 0) {
		error = QStringLiteral("The audio format for the timelapse is invalid.");
		return false;
	}

	ScopedFile pcm;
	const QByteArray pcmEncoded = pcmPath.toUtf8();
	pcm.file = std::fopen(pcmEncoded.constData(), "rb");
	if (!pcm.file) {
		error = QStringLiteral("Could not open the captured audio: %1").arg(pcmPath);
		return false;
	}
	if (std::fseek(pcm.file, 0, SEEK_END) != 0) {
		error = QStringLiteral("Could not measure the captured audio: %1").arg(pcmPath);
		return false;
	}
	const long pcmBytes = std::ftell(pcm.file);
	std::rewind(pcm.file);
	if (pcmBytes <= 0) {
		error = QStringLiteral("The captured audio is empty.");
		return false;
	}

	const std::size_t bytesPerFrame = static_cast<std::size_t>(channels) * sizeof(std::int16_t);
	const double audioSeconds = static_cast<double>(pcmBytes) / (static_cast<double>(sampleRate) * bytesPerFrame);
	result.audioSeconds = audioSeconds;
	result.audioFrames = static_cast<std::uint64_t>(pcmBytes) / bytesPerFrame;

	InputFile input;
	const QByteArray videoEncoded = videoPath.toUtf8();
	int status = avformat_open_input(&input.context, videoEncoded.constData(), nullptr, nullptr);
	if (status < 0 || !input.context) {
		error = ffmpegError(QStringLiteral("Could not open the timelapse video"), status);
		return false;
	}
	status = avformat_find_stream_info(input.context, nullptr);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not read the timelapse video"), status);
		return false;
	}

	int videoIndex = av_find_best_stream(input.context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (videoIndex < 0) {
		error = QStringLiteral("The timelapse video contains no video stream.");
		return false;
	}
	AVStream *inputVideo = input.context->streams[videoIndex];
	double videoSeconds = 0.0;
	if (input.context->duration != AV_NOPTS_VALUE && input.context->duration > 0)
		videoSeconds = static_cast<double>(input.context->duration) / AV_TIME_BASE;
	else if (inputVideo->duration != AV_NOPTS_VALUE && inputVideo->duration > 0)
		videoSeconds = static_cast<double>(inputVideo->duration) * av_q2d(inputVideo->time_base);
	if (videoSeconds <= 0.0) {
		error = QStringLiteral("The timelapse video has no measurable duration.");
		return false;
	}
	result.videoSeconds = videoSeconds;
	result.speedup = audioSeconds / videoSeconds;

	OutputFile output;
	const QByteArray outEncoded = outputPath.toUtf8();
	status = avformat_alloc_output_context2(&output.context, nullptr, "matroska", outEncoded.constData());
	if (status < 0 || !output.context) {
		error = ffmpegError(QStringLiteral("Could not create the timelapse container"), status);
		return false;
	}

	AVStream *videoOut = avformat_new_stream(output.context, nullptr);
	if (!videoOut) {
		error = QStringLiteral("Could not allocate the video stream.");
		return false;
	}
	status = avcodec_parameters_copy(videoOut->codecpar, inputVideo->codecpar);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not copy the video stream settings"), status);
		return false;
	}
	videoOut->codecpar->codec_tag = 0;
	videoOut->time_base = inputVideo->time_base;

	const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!encoder) {
		error = QStringLiteral("This OBS installation does not provide an AAC encoder for timelapse audio.");
		return false;
	}
	CodecHandle audio;
	audio.context = avcodec_alloc_context3(encoder);
	if (!audio.context) {
		error = QStringLiteral("Could not allocate the timelapse audio encoder.");
		return false;
	}

	AVChannelLayout layout{};
	av_channel_layout_default(&layout, static_cast<int>(channels));
	// FFmpeg's native AAC encoder takes planar float at any supported rate.
	audio.context->sample_fmt = AV_SAMPLE_FMT_FLTP;
	audio.context->sample_rate = static_cast<int>(sampleRate);
	av_channel_layout_copy(&audio.context->ch_layout, &layout);
	audio.context->bit_rate = static_cast<int64_t>(std::max(bitrateKbps, 32)) * 1000;
	audio.context->time_base = AVRational{1, static_cast<int>(sampleRate)};
	if (output.context->oformat->flags & AVFMT_GLOBALHEADER)
		audio.context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	status = avcodec_open2(audio.context, encoder, nullptr);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not open the timelapse audio encoder"), status);
		return false;
	}

	AVStream *audioOut = avformat_new_stream(output.context, nullptr);
	if (!audioOut) {
		error = QStringLiteral("Could not allocate the audio stream.");
		return false;
	}
	audioOut->time_base = audio.context->time_base;
	status = avcodec_parameters_from_context(audioOut->codecpar, audio.context);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not configure the audio stream"), status);
		return false;
	}
	audioOut->codecpar->codec_tag = 0;

	if (!(output.context->oformat->flags & AVFMT_NOFILE)) {
		status = avio_open(&output.context->pb, outEncoded.constData(), AVIO_FLAG_WRITE);
		if (status < 0) {
			error = ffmpegError(QStringLiteral("Could not open the partial timelapse file"), status);
			return false;
		}
	}
	output.opened = true;
	status = avformat_write_header(output.context, nullptr);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not write the timelapse header"), status);
		return false;
	}

	char layoutText[64]{};
	if (av_channel_layout_describe(&layout, layoutText, sizeof(layoutText)) < 0) {
		error = QStringLiteral("Could not describe the timelapse audio layout.");
		return false;
	}
	const std::vector<double> tempoFactors = calculateTempoFactors(result.speedup);

	FilterGraph filters;
	filters.graph = avfilter_graph_alloc();
	if (!filters.graph) {
		error = QStringLiteral("Could not allocate the audio filter graph.");
		return false;
	}
	const QString sourceArgs = QStringLiteral("sample_rate=%1:sample_fmt=s16:channel_layout=%2:time_base=1/%1")
					   .arg(sampleRate)
					   .arg(QString::fromUtf8(layoutText));
	status = avfilter_graph_create_filter(&filters.source, avfilter_get_by_name("abuffer"), "in",
					      sourceArgs.toUtf8().constData(), nullptr, filters.graph);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not add the audio input filter"), status);
		return false;
	}
	AVFilterContext *tail = filters.source;
	int tempoIndex = 0;
	for (const double factor : tempoFactors) {
		AVFilterContext *tempo = nullptr;
		const QString args = QStringLiteral("tempo=%1").arg(factor, 0, 'f', 6);
		const QString name = QStringLiteral("tempo%1").arg(tempoIndex++);
		status = avfilter_graph_create_filter(&tempo, avfilter_get_by_name("atempo"), name.toUtf8().constData(),
						      args.toUtf8().constData(), nullptr, filters.graph);
		if (status < 0) {
			error = ffmpegError(QStringLiteral("Could not add the audio speed filter"), status);
			return false;
		}
		status = avfilter_link(tail, 0, tempo, 0);
		if (status < 0) {
			error = ffmpegError(QStringLiteral("Could not connect the audio speed filter"), status);
			return false;
		}
		tail = tempo;
	}
	AVFilterContext *format = nullptr;
	const QString formatArgs = QStringLiteral("sample_fmts=%1:sample_rates=%2:channel_layouts=%3")
					   .arg(sampleFormatName(audio.context->sample_fmt))
					   .arg(sampleRate)
					   .arg(QString::fromUtf8(layoutText));
	status = avfilter_graph_create_filter(&format, avfilter_get_by_name("aformat"), "out",
					      formatArgs.toUtf8().constData(), nullptr, filters.graph);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not add the audio output filter"), status);
		return false;
	}
	status = avfilter_link(tail, 0, format, 0);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not connect the audio output filter"), status);
		return false;
	}
	// The AAC encoder only accepts frames of exactly frame_size samples.
	AVFilterContext *chunker = nullptr;
	status = avfilter_graph_create_filter(&chunker, avfilter_get_by_name("asetnsamples"), "chunk",
					      "n=1024:p=0", nullptr, filters.graph);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not add the audio frame sizer"), status);
		return false;
	}
	status = avfilter_link(format, 0, chunker, 0);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not connect the audio frame sizer"), status);
		return false;
	}
	status = avfilter_graph_create_filter(&filters.sink, avfilter_get_by_name("abuffersink"), "sink", nullptr,
					      nullptr, filters.graph);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not add the audio sink filter"), status);
		return false;
	}
	status = avfilter_link(chunker, 0, filters.sink, 0);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not connect the audio sink filter"), status);
		return false;
	}
	status = avfilter_graph_config(filters.graph, nullptr);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not configure the audio filter graph"), status);
		return false;
	}

	FrameHandle inputFrame;
	inputFrame.frame = av_frame_alloc();
	FrameHandle filtered;
	filtered.frame = av_frame_alloc();
	PacketHandle packet;
	packet.packet = av_packet_alloc();
	if (!inputFrame.frame || !filtered.frame || !packet.packet) {
		error = QStringLiteral("Could not allocate the timelapse audio buffers.");
		return false;
	}

	constexpr int ChunkFrames = 4096;
	inputFrame.frame->format = AV_SAMPLE_FMT_S16;
	inputFrame.frame->sample_rate = static_cast<int>(sampleRate);
	av_channel_layout_copy(&inputFrame.frame->ch_layout, &layout);
	inputFrame.frame->nb_samples = ChunkFrames;
	status = av_frame_get_buffer(inputFrame.frame, 0);
	if (status < 0) {
		error = ffmpegError(QStringLiteral("Could not allocate the audio read buffer"), status);
		return false;
	}

	AudioMergeResult &merge = result;
	VideoFeed video(input.context, videoIndex, videoOut);
	std::vector<std::uint8_t> buffer(static_cast<std::size_t>(ChunkFrames) * bytesPerFrame);
	int64_t inputPts = 0;
	bool failed = false;
	QString failure;

	auto encodeFrame = [&](AVFrame *frame) {
		if (failed)
			return;
		if (frame->pts != AV_NOPTS_VALUE)
			frame->pts = av_rescale_q(frame->pts, av_buffersink_get_time_base(filters.sink),
						  audio.context->time_base);
		int result = avcodec_send_frame(audio.context, frame);
		if (result == AVERROR_EOF)
			return;
		if (result < 0) {
			failed = true;
			failure = ffmpegError(QStringLiteral("Could not encode timelapse audio"), result);
			return;
		}
		for (;;) {
			result = avcodec_receive_packet(audio.context, packet.packet);
			if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
				return;
			if (result < 0) {
				failed = true;
				failure = ffmpegError(QStringLiteral("Could not read encoded timelapse audio"), result);
				return;
			}
			const double audioSecondsOut = packet.packet->pts == AV_NOPTS_VALUE
							       ? 0.0
							       : static_cast<double>(packet.packet->pts) *
									 av_q2d(audio.context->time_base);
			if (!video.writeUpTo(audioSecondsOut, output.context, failure)) {
				failed = true;
				av_packet_unref(packet.packet);
				return;
			}
			packet.packet->stream_index = audioOut->index;
			av_packet_rescale_ts(packet.packet, audio.context->time_base, audioOut->time_base);
			packet.packet->pos = -1;
			result = av_interleaved_write_frame(output.context, packet.packet);
			av_packet_unref(packet.packet);
			if (result < 0) {
				failed = true;
				failure = ffmpegError(QStringLiteral("Could not append audio to the timelapse"), result);
				return;
			}
			++merge.encodedFrames;
		}
	};

	auto drainFilter = [&]() {
		for (;;) {
			if (failed)
				return;
			const int result = av_buffersink_get_frame(filters.sink, filtered.frame);
			if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
				return;
			if (result < 0) {
				failed = true;
				failure = ffmpegError(QStringLiteral("Could not process timelapse audio"), result);
				return;
			}
			encodeFrame(filtered.frame);
			av_frame_unref(filtered.frame);
		}
	};

	while (!failed) {
		const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), pcm.file);
		if (read == 0)
			break;
		const std::size_t frames = read / bytesPerFrame;
		if (frames == 0)
			break;

		if (av_frame_make_writable(inputFrame.frame) < 0) {
			failed = true;
			failure = QStringLiteral("Could not prepare the audio read buffer.");
			break;
		}
		std::memcpy(inputFrame.frame->data[0], buffer.data(), frames * bytesPerFrame);
		inputFrame.frame->nb_samples = static_cast<int>(frames);
		inputFrame.frame->pts = inputPts;
		inputPts += static_cast<int64_t>(frames);
		const int result = av_buffersrc_add_frame_flags(filters.source, inputFrame.frame,
								AV_BUFFERSRC_FLAG_KEEP_REF);
		if (result < 0) {
			failed = true;
			failure = ffmpegError(QStringLiteral("Could not feed captured audio into the filters"), result);
			break;
		}
		drainFilter();
	}

	if (!failed && av_buffersrc_add_frame_flags(filters.source, nullptr, 0) < 0) {
		failed = true;
		failure = QStringLiteral("Could not flush the audio speed filters.");
	}
	if (!failed)
		drainFilter();

	if (!failed && avcodec_send_frame(audio.context, nullptr) < 0) {
		failed = true;
		failure = QStringLiteral("Could not flush the timelapse audio encoder.");
	}
	if (!failed) {
		for (;;) {
			const int result = avcodec_receive_packet(audio.context, packet.packet);
			if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
				break;
			if (result < 0) {
				failed = true;
				failure = ffmpegError(QStringLiteral("Could not read encoded timelapse audio"), result);
				break;
			}
			if (!video.writeUpTo(std::numeric_limits<double>::infinity(), output.context, failure)) {
				failed = true;
				av_packet_unref(packet.packet);
				break;
			}
			packet.packet->stream_index = audioOut->index;
			av_packet_rescale_ts(packet.packet, audio.context->time_base, audioOut->time_base);
			packet.packet->pos = -1;
			const int written = av_interleaved_write_frame(output.context, packet.packet);
			av_packet_unref(packet.packet);
			if (written < 0) {
				failed = true;
				failure = ffmpegError(QStringLiteral("Could not append audio to the timelapse"), written);
				break;
			}
		}
	}
	if (!failed && !video.flush(output.context, failure))
		failed = true;

	if (!failed) {
		const int result = av_write_trailer(output.context);
		if (result < 0) {
			failed = true;
			failure = ffmpegError(QStringLiteral("Could not finalize the timelapse"), result);
		}
	}

	if (failed) {
		error = failure;
		return false;
	}
	return true;
}

} // namespace timelapse
