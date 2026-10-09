#include "UpdateChecker.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimeZone>
#include <QTimer>
#include <QVersionNumber>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace rf
{
namespace
{
constexpr qint64 day = 24 * 60 * 60;
constexpr qsizetype maximumResponse = 8 * 1024 * 1024;

QVersionNumber releaseVersion(QString tag)
{
    static const QRegularExpression pattern(
        "^v?(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$");
    if (!pattern.match(tag).hasMatch())
        return {};
    if (tag.startsWith('v'))
        tag.remove(0, 1);
    const auto version = QVersionNumber::fromString(tag);
    return version.segmentCount() == 3 && version.toString() == tag ? version : QVersionNumber{};
}

QDateTime retryTime(QNetworkReply *reply)
{
    const auto now = QDateTime::currentDateTimeUtc();
    QDateTime retry;
    bool ok = false;
    const auto seconds = reply->rawHeader("Retry-After").toLongLong(&ok);
    if (ok && seconds >= 0)
        retry = now.addSecs(seconds);
    else
        retry = QDateTime::fromString(QString::fromLatin1(reply->rawHeader("Retry-After")),
                                      Qt::RFC2822Date)
                    .toUTC();
    if (reply->rawHeader("X-RateLimit-Remaining") == "0") {
        const auto reset = reply->rawHeader("X-RateLimit-Reset").toLongLong(&ok);
        if (ok)
            retry = std::max(retry, QDateTime::fromSecsSinceEpoch(reset, QTimeZone("UTC")));
    }
    // GitHub asks clients to wait at least a minute for secondary rate limits.
    return std::max(retry, now.addSecs(60));
}
} // namespace

std::optional<UpdateRelease> newerRelease(const QByteArray &json, const QString &currentVersion)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError ||
        (!document.isArray() && !document.isObject()))
        throw std::runtime_error("GitHub returned invalid release metadata");
    auto newest = releaseVersion(currentVersion);
    bool development = false;
    if (newest.isNull()) {
        static const QRegularExpression pattern(
            "^(\\d+\\.\\d+\\.\\d+)-dev\\.(?:local|[1-9][0-9]*)\\.g(?:[0-9a-f]{7}|unknown)"
            "(?:\\.dirty)?$");
        const auto match = pattern.match(currentVersion);
        if (match.hasMatch()) {
            newest = releaseVersion(match.captured(1));
            development = true;
        }
    }
    if (newest.isNull())
        throw std::runtime_error("The installed application version cannot be compared");
    std::optional<UpdateRelease> result;
    const auto releases = document.isObject() ? QJsonArray{document.object()} : document.array();
    for (const auto value : releases) {
        const auto release = value.toObject();
        if (!release["draft"].isBool() || !release["prerelease"].isBool())
            throw std::runtime_error("GitHub returned invalid release metadata");
        if (release["draft"].toBool() || release["prerelease"].toBool())
            continue;
        const auto tag = release["tag_name"].toString();
        const auto version = releaseVersion(tag);
        if (version.isNull() || version < newest || (version == newest && !development))
            continue;
        if (!release["assets"].isArray())
            throw std::runtime_error("GitHub returned an invalid release asset list");
        QSet<QString> assets;
        for (const auto assetValue : release["assets"].toArray()) {
            const auto asset = assetValue.toObject();
            if (asset["state"].toString() == "uploaded" && asset["size"].toDouble() > 0)
                assets.insert(asset["name"].toString());
        }
        const auto prefix = "croccospectrum-" + version.toString() + "-x86_64";
        const auto appimage = prefix + ".AppImage";
        const auto portable = prefix + ".tar.gz";
        if (!assets.contains(appimage) || !assets.contains(portable) ||
            !assets.contains(appimage + ".sha256") || !assets.contains(portable + ".sha256"))
            continue;
        newest = version;
        development = false;
        result = UpdateRelease{version.toString(),
                               QUrl("https://gioeleminardi.github.io/CroccoSpectrum")};
    }
    return result;
}

UpdateChecker::UpdateChecker(UpdateSettings &settings, QObject *parent,
                             QNetworkAccessManager *network)
    : QObject(parent), settings_(settings),
      network_(network ? network : new QNetworkAccessManager(this)),
      automaticTimer_(new QTimer(this)), timeout_(new QTimer(this))
{
    automaticTimer_->setObjectName("automaticUpdateTimer");
    automaticTimer_->setSingleShot(true);
    timeout_->setObjectName("updateRequestTimeout");
    timeout_->setSingleShot(true);
    timeout_->setInterval(10000);
    connect(automaticTimer_, &QTimer::timeout, this, [this] { check(false); });
    connect(timeout_, &QTimer::timeout, this, [this] {
        requestError_ = "The update check timed out";
        if (reply_)
            reply_->abort();
    });
}

UpdateChecker::~UpdateChecker()
{
    stop();
}

void UpdateChecker::startAutomaticChecks()
{
    if (!automaticStarted_)
        startupCheckPending_ = true;
    automaticStarted_ = true;
    scheduleAutomaticCheck();
}

void UpdateChecker::setAutomaticChecking(bool enabled)
{
    settings_.automatic = enabled;
    if (!enabled && !manual_)
        cancelRequest();
    scheduleAutomaticCheck();
    emit settingsChanged();
}

void UpdateChecker::scheduleAutomaticCheck()
{
    automaticTimer_->stop();
    if (!automaticStarted_ || !settings_.automatic)
        return;
    const auto now = QDateTime::currentDateTimeUtc();
    auto next = now.addSecs(5);
    if (!startupCheckPending_ && settings_.lastAttempt.isValid() && settings_.lastAttempt <= now)
        next = std::max(next, settings_.lastAttempt.addSecs(day));
    if (settings_.retryAfter.isValid())
        next = std::max(next, settings_.retryAfter);
    automaticTimer_->start(static_cast<int>(
        std::clamp<qint64>(now.msecsTo(next), 5000, std::numeric_limits<int>::max())));
}

void UpdateChecker::check(bool manual)
{
    if (reply_) {
        manual_ = manual_ || manual;
        return;
    }
    const auto now = QDateTime::currentDateTimeUtc();
    if (settings_.retryAfter > now) {
        if (manual)
            emit failed(
                "GitHub limited update checks. Try again after " +
                    QLocale().toString(settings_.retryAfter.toLocalTime(), QLocale::ShortFormat),
                true);
        scheduleAutomaticCheck();
        return;
    }
    if (!manual &&
        (!settings_.automatic || (!startupCheckPending_ && settings_.lastAttempt.isValid() &&
                                  settings_.lastAttempt <= now &&
                                  settings_.lastAttempt.secsTo(now) < day))) {
        scheduleAutomaticCheck();
        return;
    }
    manual_ = manual;
    startupCheckPending_ = false;
    settings_.lastAttempt = now;
    settings_.retryAfter = {};
    emit settingsChanged();
    response_.clear();
    requestError_.clear();
    QNetworkRequest request(
        QUrl("https://api.github.com/repos/gioeleminardi/CroccoSpectrum/releases/latest"));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "CroccoSpectrum/" RF_VERSION);
    request.setRawHeader("X-GitHub-Api-Version", "2026-03-10");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::SameOriginRedirectPolicy);
    request.setMaximumRedirectsAllowed(3);
    request.setTransferTimeout(10000);
    reply_ = network_->get(request);
    reply_->setReadBufferSize(maximumResponse + 1);
    connect(reply_, &QNetworkReply::readyRead, this, [this] {
        response_ += reply_->read(maximumResponse - response_.size() + 1);
        if (response_.size() > maximumResponse) {
            requestError_ = "GitHub returned an oversized release list";
            reply_->abort();
        }
    });
    connect(reply_, &QNetworkReply::finished, this, &UpdateChecker::finishRequest);
    timeout_->start();
    emit checkingChanged(true);
    scheduleAutomaticCheck();
}

void UpdateChecker::finishRequest()
{
    timeout_->stop();
    auto *reply = reply_;
    reply_ = nullptr;
    reply->deleteLater();
    response_ += reply->read(std::max<qsizetype>(0, maximumResponse - response_.size() + 1));
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString error = requestError_;
    if (status == 403 || status == 429) {
        settings_.retryAfter = retryTime(reply);
        emit settingsChanged();
        error = "GitHub refused the update check; try again later";
    } else if (error.isEmpty() && status != 200 && status != 404)
        error = status ? QString("GitHub returned HTTP %1").arg(status) : reply->errorString();
    else if (error.isEmpty() && status != 404 && reply->error() != QNetworkReply::NoError)
        error = reply->errorString();
    else if (error.isEmpty() && response_.size() > maximumResponse)
        error = "GitHub returned an oversized release list";
    std::optional<UpdateRelease> release;
    if (error.isEmpty()) {
        try {
            // GitHub returns 404 when no stable release has been published yet.
            release = newerRelease(status == 404 ? QByteArray("[]") : response_, RF_VERSION);
        } catch (const std::exception &exception) {
            error = QString::fromUtf8(exception.what());
        }
    }
    emit checkingChanged(false);
    scheduleAutomaticCheck();
    if (!error.isEmpty())
        emit failed(error, manual_);
    else
        emit finished(release ? release->version : QString{}, release ? release->url : QUrl{},
                      manual_);
}

void UpdateChecker::cancelRequest()
{
    timeout_->stop();
    if (!reply_)
        return;
    auto *reply = reply_;
    reply_ = nullptr;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    emit checkingChanged(false);
}

void UpdateChecker::stop()
{
    automaticStarted_ = false;
    startupCheckPending_ = false;
    automaticTimer_->stop();
    cancelRequest();
}
} // namespace rf
