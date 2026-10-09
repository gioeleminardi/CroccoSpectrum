#pragma once
#include "Session.h"
#include <QObject>
#include <QUrl>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace rf
{
struct UpdateRelease {
    QString version;
    QUrl url;
};

// Only published stable releases with complete Linux packages are candidates.
std::optional<UpdateRelease> newerRelease(const QByteArray &json, const QString &currentVersion);

class UpdateChecker : public QObject
{
    Q_OBJECT
  public:
    explicit UpdateChecker(UpdateSettings &settings, QObject *parent = nullptr,
                           QNetworkAccessManager *network = nullptr);
    ~UpdateChecker() override;
    void startAutomaticChecks();
    void setAutomaticChecking(bool enabled);
    void check(bool manual = true);
    void stop();

  signals:
    void settingsChanged();
    void checkingChanged(bool checking);
    // An empty version means there is no newer release with complete packages.
    void finished(QString version, QUrl url, bool manual);
    void failed(QString message, bool manual);

  private:
    void scheduleAutomaticCheck();
    void cancelRequest();
    void finishRequest();
    UpdateSettings &settings_;
    QNetworkAccessManager *network_;
    QNetworkReply *reply_ = nullptr;
    QTimer *automaticTimer_, *timeout_;
    QByteArray response_;
    QString requestError_;
    bool automaticStarted_ = false;
    bool manual_ = false;
};
} // namespace rf
