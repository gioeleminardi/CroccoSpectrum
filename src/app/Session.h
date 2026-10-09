#pragma once
#include "Analysis.h"
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QStringList>

namespace rf
{
struct ViewSettings {
    double colorMin = -100;
    double colorMax = 0;
    QString palette = "Baudline";
    bool autoRange = false;
    bool waterfallAutoRangeOnZoom = true;
    int waterfallRows = 0; // 0: automatic physical display height.
    bool absoluteFrequency = false;
    int waveformMode = 0; // 0: I/Q, 1: magnitude, 2: I only.
    void validate() const;
};

struct UpdateSettings {
    bool automatic = true;
    QDateTime lastAttempt;
    QDateTime retryAfter;
    QString lastNotifiedVersion;
};

struct Preferences {
    DspSettings dsp;
    ViewSettings view;
    RecordingDescriptor importDefaults;
    QByteArray geometry;
    QByteArray workspace;
    QString lastSession;
    QStringList recentFiles;
    bool darkTheme = false;
    UpdateSettings updates;
};

struct Session {
    RecordingDescriptor recording;
    DspSettings dsp;
    ViewSettings view;
    FrameRange range;
    QString sourceIdentity;
    std::vector<Annotation> bookmarks;
};

QJsonObject dspToJson(const DspSettings &settings);
DspSettings dspFromJson(const QJsonObject &object);
QJsonObject viewToJson(const ViewSettings &settings);
ViewSettings viewFromJson(const QJsonObject &object);
Preferences readPreferences(const QString &path);
void savePreferences(const QString &path, const Preferences &preferences);
Session readSession(const QString &path);
void saveSession(const QString &path, const Session &session);
} // namespace rf
