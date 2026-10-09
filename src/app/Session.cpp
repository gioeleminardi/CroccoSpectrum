#include "Session.h"
#include "recording/Metadata.h"
#include <QJsonArray>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace rf
{
namespace
{
void requireIntegers(const QJsonObject &object, std::initializer_list<const char *> fields)
{
    for (const auto *field : fields) {
        const auto value = object[field];
        if (!value.isUndefined() && (!value.isDouble() || value.toDouble() != value.toInt()))
            throw std::runtime_error(std::string("Invalid integer setting: ") + field);
    }
}
void requireBooleans(const QJsonObject &object, std::initializer_list<const char *> fields)
{
    for (const auto *field : fields)
        if (object.contains(field) && !object[field].isBool())
            throw std::runtime_error(std::string("Invalid boolean setting: ") + field);
}
void requireSchema(const QJsonObject &root, const QString &type)
{
    if (root["schema"].toInt() != 1 || root["type"].toString() != type)
        throw std::runtime_error("Unsupported settings/session schema; original file is preserved");
}
} // namespace

void ViewSettings::validate() const
{
    if (!std::isfinite(colorMin) || !std::isfinite(colorMax) ||
        !std::isfinite(colorMax - colorMin) || colorMin >= colorMax)
        throw std::runtime_error("Color minimum must be finite and lower than maximum");
    if (palette != "Viridis" && palette != "Inferno" && palette != "Grayscale" &&
        palette != "Turbo" && palette != "Baudline")
        throw std::runtime_error("Unknown palette");
    if (waveformMode < 0 || waveformMode > 2)
        throw std::runtime_error("Invalid waveform mode");
}

QJsonObject dspToJson(const DspSettings &settings)
{
    return {{"fft_size", settings.fftSize},
            {"window", static_cast<int>(settings.window)},
            {"overlap_percent", settings.overlapPercent},
            {"scale", static_cast<int>(settings.scale)},
            {"remove_dc", settings.removeDc},
            {"conjugate", settings.conjugate}};
}

DspSettings dspFromJson(const QJsonObject &object)
{
    requireIntegers(object, {"fft_size", "window", "overlap_percent", "scale"});
    requireBooleans(object, {"remove_dc", "conjugate"});
    DspSettings settings;
    settings.fftSize = object["fft_size"].toInt(4096);
    settings.window = static_cast<Window>(object["window"].toInt(0));
    settings.overlapPercent = object["overlap_percent"].toInt(50);
    settings.scale = static_cast<PowerScale>(object["scale"].toInt(0));
    settings.removeDc = object["remove_dc"].toBool();
    settings.conjugate = object["conjugate"].toBool();
    settings.validate(1);
    return settings;
}

QJsonObject viewToJson(const ViewSettings &settings)
{
    return {{"color_min", settings.colorMin},
            {"color_max", settings.colorMax},
            {"palette", settings.palette},
            {"auto_range", settings.autoRange},
            {"absolute_frequency", settings.absoluteFrequency},
            {"waveform_mode", settings.waveformMode}};
}

ViewSettings viewFromJson(const QJsonObject &object)
{
    requireIntegers(object, {"waveform_mode"});
    requireBooleans(object, {"auto_range", "absolute_frequency"});
    for (const auto *field : {"color_min", "color_max"})
        if (object.contains(field) && !object[field].isDouble())
            throw std::runtime_error("Invalid color setting type");
    if (object.contains("palette") && !object["palette"].isString())
        throw std::runtime_error("Invalid palette setting type");
    ViewSettings settings;
    settings.colorMin = object["color_min"].toDouble(-100);
    settings.colorMax = object["color_max"].toDouble(0);
    settings.palette = object["palette"].toString(settings.palette);
    settings.autoRange = object["auto_range"].toBool();
    settings.absoluteFrequency = object["absolute_frequency"].toBool();
    settings.waveformMode = object["waveform_mode"].toInt(0);
    settings.validate();
    return settings;
}

Preferences readPreferences(const QString &path)
{
    const auto root = readJsonObject(path);
    requireSchema(root, "preferences");
    if (!root["dsp"].isObject() || !root["view"].isObject())
        throw std::runtime_error("Missing preference settings");
    Preferences preferences;
    preferences.dsp = dspFromJson(root["dsp"].toObject());
    preferences.view = viewFromJson(root["view"].toObject());
    if (root["import_defaults"].isObject())
        preferences.importDefaults = recordingFromJson(root["import_defaults"].toObject());
    preferences.geometry = QByteArray::fromBase64(root["geometry"].toString().toLatin1());
    preferences.workspace = QByteArray::fromBase64(root["workspace"].toString().toLatin1());
    preferences.lastSession = root["last_session"].toString();
    const auto updates = root["updates"].toObject();
    requireBooleans(updates, {"automatic"});
    preferences.updates.automatic = updates["automatic"].toBool(true);
    preferences.updates.lastAttempt =
        QDateTime::fromString(updates["last_attempt"].toString(), Qt::ISODate).toUTC();
    preferences.updates.retryAfter =
        QDateTime::fromString(updates["retry_after"].toString(), Qt::ISODate).toUTC();
    preferences.updates.lastNotifiedVersion = updates["last_notified_version"].toString();
    for (const auto value : root["recent_files"].toArray())
        if (value.isString() && !value.toString().isEmpty())
            preferences.recentFiles.append(value.toString());
    return preferences;
}

void savePreferences(const QString &path, const Preferences &preferences)
{
    auto defaults = preferences.importDefaults;
    // Defaults are an import preset, not evidence about a previously opened
    // recording. Strip recording-specific metadata and retain a valid path.
    defaults.path = "import-preset";
    defaults.captures.clear();
    defaults.annotations.clear();
    defaults.dataOffset = 0;
    defaults.dataBytes.reset();
    defaults.allowPartial = false;
    defaults.centerFrequency.reset();
    defaults.startUtc.clear();
    const QJsonObject updates{
        {"automatic", preferences.updates.automatic},
        {"last_attempt", preferences.updates.lastAttempt.toUTC().toString(Qt::ISODate)},
        {"retry_after", preferences.updates.retryAfter.toUTC().toString(Qt::ISODate)},
        {"last_notified_version", preferences.updates.lastNotifiedVersion}};
    writeJsonAtomic(path, {{"schema", 1},
                           {"type", "preferences"},
                           {"dsp", dspToJson(preferences.dsp)},
                           {"view", viewToJson(preferences.view)},
                           {"import_defaults", recordingToJson(defaults)},
                           {"geometry", QString::fromLatin1(preferences.geometry.toBase64())},
                           {"workspace", QString::fromLatin1(preferences.workspace.toBase64())},
                           {"last_session", preferences.lastSession},
                           {"recent_files", QJsonArray::fromStringList(preferences.recentFiles)},
                           {"updates", updates}});
}

Session readSession(const QString &path)
{
    const auto root = readJsonObject(path);
    requireSchema(root, "session");
    if (!root["dsp"].isObject() || !root["view"].isObject() || !root["bookmarks"].isArray())
        throw std::runtime_error("Invalid session settings/bookmarks");
    Session session;
    session.recording = recordingFromJson(root["recording"].toObject());
    session.dsp = dspFromJson(root["dsp"].toObject());
    session.dsp.validate(session.recording.sampleRate);
    session.view = viewFromJson(root["view"].toObject());
    session.range = {jsonUnsigned(root["start"], "start"), jsonUnsigned(root["end"], "end")};
    if (session.range.size() == 0)
        throw std::runtime_error("Session range is empty");
    session.sourceIdentity = root["source_identity"].toString();
    for (const auto value : root["bookmarks"].toArray()) {
        const auto item = value.toObject();
        session.bookmarks.push_back({jsonUnsigned(item["start"], "bookmark start"),
                                     jsonUnsigned(item["count"], "bookmark count"),
                                     item["label"].toString()});
    }
    return session;
}

void saveSession(const QString &path, const Session &session)
{
    session.recording.validate();
    session.dsp.validate(session.recording.sampleRate);
    session.view.validate();
    (void)session.range.size();
    QJsonArray bookmarks;
    for (const auto &mark : session.bookmarks)
        bookmarks.append(QJsonObject{{"start", QString::number(mark.start)},
                                     {"count", QString::number(mark.count)},
                                     {"label", mark.label}});
    writeJsonAtomic(path, {{"schema", 1},
                           {"type", "session"},
                           {"application_version", RF_VERSION},
                           {"recording", recordingToJson(session.recording)},
                           {"dsp", dspToJson(session.dsp)},
                           {"view", viewToJson(session.view)},
                           {"start", QString::number(session.range.begin)},
                           {"end", QString::number(session.range.end)},
                           {"source_identity", session.sourceIdentity},
                           {"bookmarks", bookmarks}});
}
} // namespace rf
