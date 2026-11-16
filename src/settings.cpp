#include "settings.hpp"

#include <obs-frontend-api.h>
#include <util/bmem.h>
#include <util/config-file.h>
#include <util/platform.h>

#include <QDir>
#include <QStandardPaths>

#include <cstring>

namespace timelapse {
namespace {

constexpr auto Section = "OBS-Timelapse";

// Where OBS itself writes recordings for the active profile. Following it means a
// timelapse lands next to the normal recordings instead of somewhere unrelated.
QString recordingDirectory()
{
	config_t *profile = obs_frontend_get_profile_config();
	if (!profile)
		return {};

	const char *mode = config_get_string(profile, "Output", "Mode");
	const bool advanced = mode && std::strcmp(mode, "Advanced") == 0;
	const char *configured = advanced ? config_get_string(profile, "AdvOut", "RecFilePath")
					  : config_get_string(profile, "SimpleOutput", "FilePath");
	if (!configured || !*configured)
		return {};

	// OBS allows %-specifiers in the recording path and expands them per recording.
	QString path;
	char *expanded = os_generate_formatted_filename("", false, configured);
	if (expanded) {
		path = QString::fromUtf8(expanded);
		bfree(expanded);
	} else {
		path = QString::fromUtf8(configured);
	}
	path = path.trimmed();
	if (path.isEmpty())
		return {};
	if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/")))
		path = QDir::homePath() + path.mid(1);
	else if (QDir::isRelativePath(path))
		path = QDir::home().filePath(path);
	return QDir::cleanPath(path);
}

QString defaultOutputDirectory()
{
	const QString recordings = recordingDirectory();
	if (!recordings.isEmpty())
		return recordings;

	QString path = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
	if (path.isEmpty())
		path = QDir::homePath();
	return QDir(path).filePath(QStringLiteral("OBS Timelapse"));
}

config_t *userConfig()
{
	return obs_frontend_get_user_config();
}

} // namespace

const char *outputModeName(OutputMode mode)
{
	return mode == OutputMode::Mkv ? "mkv" : "png";
}

CaptureSettings loadSettings()
{
	CaptureSettings settings;
	settings.outputDirectory = defaultOutputDirectory();

	config_t *config = userConfig();
	if (!config)
		return settings;

	const QByteArray defaultPath = settings.outputDirectory.toUtf8();
	config_set_default_string(config, Section, "OutputDirectory", defaultPath.constData());
	config_set_default_string(config, Section, "NamePrefix", "");
	config_set_default_string(config, Section, "Mode", "png");
	config_set_default_double(config, Section, "IntervalSeconds", 10.0);
	config_set_default_int(config, Section, "PlaybackFps", 30);
	config_set_default_int(config, Section, "PngCompression", 6);
	config_set_default_int(config, Section, "QueueCapacity", 4);
	config_set_default_bool(config, Section, "RecordAudio", true);
	config_set_default_int(config, Section, "AudioBitrateKbps", 160);

	settings.outputDirectory = QString::fromUtf8(config_get_string(config, Section, "OutputDirectory"));
	settings.namePrefix = QString::fromUtf8(config_get_string(config, Section, "NamePrefix"));
	settings.mode = QString::fromUtf8(config_get_string(config, Section, "Mode")) == QStringLiteral("mkv")
				? OutputMode::Mkv
				: OutputMode::Png;
	settings.intervalSeconds = config_get_double(config, Section, "IntervalSeconds");
	settings.playbackFps = static_cast<int>(config_get_int(config, Section, "PlaybackFps"));
	settings.pngCompression = static_cast<int>(config_get_int(config, Section, "PngCompression"));
	settings.queueCapacity = static_cast<std::size_t>(config_get_int(config, Section, "QueueCapacity"));
	settings.recordAudio = config_get_bool(config, Section, "RecordAudio");
	settings.audioBitrateKbps = static_cast<int>(config_get_int(config, Section, "AudioBitrateKbps"));
	return settings;
}

bool saveSettings(const CaptureSettings &settings, QString &error)
{
	config_t *config = userConfig();
	if (!config) {
		error = QStringLiteral("OBS user configuration is unavailable.");
		return false;
	}

	const QByteArray outputDirectory = settings.outputDirectory.toUtf8();
	const QByteArray namePrefix = settings.namePrefix.toUtf8();
	config_set_string(config, Section, "OutputDirectory", outputDirectory.constData());
	config_set_string(config, Section, "NamePrefix", namePrefix.constData());
	config_set_string(config, Section, "Mode", outputModeName(settings.mode));
	config_set_double(config, Section, "IntervalSeconds", settings.intervalSeconds);
	config_set_int(config, Section, "PlaybackFps", settings.playbackFps);
	config_set_int(config, Section, "PngCompression", settings.pngCompression);
	config_set_int(config, Section, "QueueCapacity", static_cast<int64_t>(settings.queueCapacity));
	config_set_bool(config, Section, "RecordAudio", settings.recordAudio);
	config_set_int(config, Section, "AudioBitrateKbps", settings.audioBitrateKbps);

	if (config_save_safe(config, "tmp", nullptr) != CONFIG_SUCCESS) {
		error = QStringLiteral("OBS could not save the plugin configuration.");
		return false;
	}

	return true;
}

QString validateSettings(const CaptureSettings &settings)
{
	if (settings.outputDirectory.trimmed().isEmpty())
		return QStringLiteral("Choose an output directory.");
	if (settings.intervalSeconds < MinimumIntervalSeconds || settings.intervalSeconds > MaximumIntervalSeconds)
		return QStringLiteral("The capture interval must be between 0.1 seconds and 24 hours.");
	if (settings.playbackFps < MinimumPlaybackFps || settings.playbackFps > MaximumPlaybackFps)
		return QStringLiteral("The playback frame rate must be between 1 and 60 FPS.");
	if (settings.pngCompression < MinimumPngCompression || settings.pngCompression > MaximumPngCompression)
		return QStringLiteral("PNG compression must be between 0 and 9.");
	if (settings.queueCapacity < MinimumQueueCapacity || settings.queueCapacity > MaximumQueueCapacity)
		return QStringLiteral("The frame queue must contain between 2 and 32 frames.");
	if (settings.audioBitrateKbps < MinimumAudioBitrateKbps ||
	    settings.audioBitrateKbps > MaximumAudioBitrateKbps)
		return QStringLiteral("The audio bitrate must be between 64 and 320 kbps.");
	return {};
}

} // namespace timelapse
