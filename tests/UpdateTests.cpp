#include "app/UpdateChecker.h"
#include "recording/Metadata.h"
#include "ui/MainWindow.h"
#include <QAction>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSslSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QVersionNumber>
#include <QtTest>
#include <cstring>

namespace
{
QJsonObject release(const QString &tag)
{
    auto version = tag;
    if (version.startsWith('v'))
        version.remove(0, 1);
    QJsonArray assets;
    for (const auto &suffix : {".AppImage", ".tar.gz", ".AppImage.sha256", ".tar.gz.sha256"})
        assets.append(QJsonObject{{"name", "croccospectrum-" + version + "-x86_64" + suffix},
                                  {"state", "uploaded"},
                                  {"size", 100}});
    return {{"tag_name", tag}, {"draft", false}, {"prerelease", false}, {"assets", assets}};
}

QByteArray response(const QJsonArray &releases)
{
    return QJsonDocument(releases).toJson(QJsonDocument::Compact);
}

class FakeReply : public QNetworkReply
{
  public:
    FakeReply(const QNetworkRequest &request, const QByteArray &body, QObject *parent)
        : QNetworkReply(parent), body_(body)
    {
        setRequest(request);
        setUrl(request.url());
        open(QIODevice::ReadOnly);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
    }
    void status(int code) { setAttribute(QNetworkRequest::HttpStatusCodeAttribute, code); }
    void header(const QByteArray &name, const QByteArray &value) { setRawHeader(name, value); }
    void networkError(NetworkError error) { setError(error, "Simulated network failure"); }
    void complete()
    {
        if (isFinished())
            return;
        setFinished(true);
        emit readyRead();
        emit finished();
    }
    void abort() override
    {
        aborted = true;
        setError(OperationCanceledError, "Cancelled");
        complete();
    }
    qint64 bytesAvailable() const override
    {
        return body_.size() - offset_ + QNetworkReply::bytesAvailable();
    }
    bool aborted = false;

  protected:
    qint64 readData(char *target, qint64 maximum) override
    {
        const auto count = std::min(maximum, static_cast<qint64>(body_.size()) - offset_);
        if (count <= 0)
            return -1;
        std::memcpy(target, body_.constData() + offset_, static_cast<std::size_t>(count));
        offset_ += count;
        return count;
    }

  private:
    QByteArray body_;
    qint64 offset_ = 0;
};

class FakeNetwork : public QNetworkAccessManager
{
  public:
    FakeNetwork()
    {
        // Keep the simulated release newer than the configured build version.
        const auto current = QVersionNumber::fromString(RF_VERSION);
        version = QVersionNumber(current.majorVersion(), current.minorVersion(),
                                 current.microVersion() + 1)
                      .toString();
        body = QJsonDocument(release("v" + version)).toJson(QJsonDocument::Compact);
    }
    QString version;
    QByteArray body;
    int requests = 0;
    QPointer<FakeReply> reply;

  protected:
    QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *) override
    {
        ++requests;
        reply = new FakeReply(request, body, this);
        return reply;
    }
};
} // namespace

class UpdateTests : public QObject
{
    Q_OBJECT
  public slots:
    void captureUrl(const QUrl &url)
    {
        openedUrl_ = url;
        dialogDestroyedBeforeLaunch_ = !openingDialog_;
    }

  private slots:
    void developmentBuildsRecognizeTheirFinalRelease()
    {
        const auto json = QJsonDocument(release("v0.2.0")).toJson(QJsonDocument::Compact);
        for (const auto &version : {"0.2.0-dev.184.g233be51", "0.2.0-dev.local.g233be51.dirty",
                                    "0.2.0-dev.local.gunknown"}) {
            const auto candidate = rf::newerRelease(json, version);
            QVERIFY(candidate);
            QCOMPARE(candidate->version, QString("0.2.0"));
        }
        QVERIFY(!rf::newerRelease(json, "0.2.0"));
        QVERIFY(!rf::newerRelease(json, "0.3.0-dev.185.g233be51"));
        QVERIFY(rf::newerRelease(json, "0.1.7-dev.183.g233be51"));
        QVERIFY_EXCEPTION_THROWN(rf::newerRelease(json, "0.2.0-invalid"), std::runtime_error);
    }
    void selectsHighestNumericVersion()
    {
        const auto candidate = rf::newerRelease(
            response({release("v0.1.9"), release("0.1.10"), release("v0.1.8")}), "0.1.6");
        QVERIFY(candidate);
        QCOMPARE(candidate->version, QString("0.1.10"));
        QCOMPARE(candidate->url,
                 QUrl("https://gioeleminardi.github.io/CroccoSpectrum"));
        QVERIFY(!rf::newerRelease(response({release("v0.1.6"), release("0.1.5")}), "0.1.6"));
    }
    void rejectsUnavailableReleases()
    {
        auto draft = release("v0.2.0");
        draft["draft"] = true;
        auto prerelease = release("v0.3.0");
        prerelease["prerelease"] = true;
        auto incomplete = release("v0.4.0");
        auto assets = incomplete["assets"].toArray();
        assets.removeLast();
        incomplete["assets"] = assets;
        auto uploading = release("v0.5.0");
        assets = uploading["assets"].toArray();
        auto asset = assets[0].toObject();
        asset["state"] = "new";
        assets[0] = asset;
        uploading["assets"] = assets;
        const auto candidate = rf::newerRelease(
            response({draft, prerelease, incomplete, uploading, release("v0.6.0-rc.1"),
                      release("v0.7.0junk"), release("v01.8.0"), release("v99999999999999.0.0"),
                      release("v0.1.7")}),
            "0.1.6");
        QVERIFY(candidate);
        QCOMPARE(candidate->version, QString("0.1.7"));
        QVERIFY(!rf::newerRelease(response({incomplete}), "0.1.6"));
        assets = release("v0.1.7")["assets"].toArray();
        asset = assets[0].toObject();
        asset["size"] = 0;
        assets[0] = asset;
        incomplete = release("v0.1.7");
        incomplete["assets"] = assets;
        QVERIFY(!rf::newerRelease(response({incomplete}), "0.1.6"));
    }
    void malformedResponsesFail()
    {
        QVERIFY_EXCEPTION_THROWN(rf::newerRelease("not JSON", "0.1.6"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(rf::newerRelease("{}", "0.1.6"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(rf::newerRelease("[{}]", "0.1.6"), std::runtime_error);
        auto invalid = release("v0.1.7");
        invalid.remove("assets");
        QVERIFY_EXCEPTION_THROWN(rf::newerRelease(response({invalid}), "0.1.6"),
                                 std::runtime_error);
    }
    void preferencesRemainCompatible()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.json");
        rf::Preferences preferences;
        rf::savePreferences(path, preferences);
        auto legacy = rf::readJsonObject(path);
        legacy.remove("updates");
        rf::writeJsonAtomic(path, legacy);
        const auto loaded = rf::readPreferences(path);
        QVERIFY(loaded.updates.automatic);
        QVERIFY(!loaded.updates.lastAttempt.isValid());
        preferences.updates = {false, QDateTime::currentDateTimeUtc().addSecs(-100),
                               QDateTime::currentDateTimeUtc().addSecs(100), "0.1.7"};
        rf::savePreferences(path, preferences);
        const auto restored = rf::readPreferences(path);
        QVERIFY(!restored.updates.automatic);
        QCOMPARE(restored.updates.lastAttempt.toSecsSinceEpoch(),
                 preferences.updates.lastAttempt.toSecsSinceEpoch());
        QCOMPARE(restored.updates.retryAfter.toSecsSinceEpoch(),
                 preferences.updates.retryAfter.toSecsSinceEpoch());
        QCOMPARE(restored.updates.lastNotifiedVersion, QString("0.1.7"));
    }
    void checksAreAsynchronousAndSerialized()
    {
        rf::UpdateSettings settings;
        FakeNetwork network;
        rf::UpdateChecker checker(settings, nullptr, &network);
        QSignalSpy completed(&checker, &rf::UpdateChecker::finished);
        QSignalSpy changed(&checker, &rf::UpdateChecker::checkingChanged);
        checker.check(false);
        QCOMPARE(completed.count(), 0);
        QCOMPARE(changed.count(), 1);
        checker.check(); // A manual request can join an automatic request.
        QCOMPARE(network.requests, 1);
        QCOMPARE(network.reply->request().url().scheme(), QString("https"));
        QCOMPARE(network.reply->request().url().path(),
                 QString("/repos/gioeleminardi/CroccoSpectrum/releases/latest"));
        QCOMPARE(
            network.reply->request().attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
            static_cast<int>(QNetworkRequest::SameOriginRedirectPolicy));
        QVERIFY(settings.lastAttempt.isValid());
        network.reply->complete();
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed[0][0].toString(), network.version);
        QVERIFY(completed[0][2].toBool());
        QCOMPARE(changed.count(), 2);
    }
    void automaticChecksAreThrottledAndOptional()
    {
        rf::UpdateSettings settings;
        FakeNetwork network;
        rf::UpdateChecker checker(settings, nullptr, &network);
        auto *timer = checker.findChild<QTimer *>("automaticUpdateTimer");
        QVERIFY(!timer->isActive());
        checker.startAutomaticChecks();
        QVERIFY(timer->isActive());
        QCOMPARE(timer->interval(), 5000);
        checker.check(false);
        network.reply->complete();
        checker.check(false);
        QCOMPARE(network.requests, 1);
        QVERIFY(timer->interval() > 23 * 60 * 60 * 1000);
        checker.setAutomaticChecking(false);
        QVERIFY(!timer->isActive());
        checker.check(false);
        QCOMPARE(network.requests, 1);
        checker.check();
        QCOMPARE(network.requests, 2);
        network.reply->complete();
        settings.lastAttempt = QDateTime::currentDateTimeUtc().addSecs(-25 * 60 * 60);
        checker.setAutomaticChecking(true);
        checker.check(false);
        QCOMPARE(network.requests, 3);
        checker.setAutomaticChecking(false);
        QVERIFY(network.reply->aborted);
    }
    void startupChecksIgnoreRecentAttempts()
    {
        rf::UpdateSettings settings;
        settings.lastAttempt = QDateTime::currentDateTimeUtc();
        FakeNetwork network;
        rf::UpdateChecker checker(settings, nullptr, &network);
        QSignalSpy completed(&checker, &rf::UpdateChecker::finished);
        checker.startAutomaticChecks();
        auto *timer = checker.findChild<QTimer *>("automaticUpdateTimer");
        QCOMPARE(timer->interval(), 5000);
        timer->start(0);
        QTRY_COMPARE(network.requests, 1);
        network.reply->complete();
        QCOMPARE(completed.count(), 1);
        QCOMPARE(completed[0][0].toString(), network.version);
        QCOMPARE(completed[0][1].toUrl(), QUrl("https://gioeleminardi.github.io/CroccoSpectrum"));
        QVERIFY(!completed[0][2].toBool());
        QVERIFY(timer->interval() > 23 * 60 * 60 * 1000);
        checker.check(false);
        QCOMPARE(network.requests, 1);

        rf::UpdateChecker restarted(settings, nullptr, &network);
        restarted.startAutomaticChecks();
        auto *startupTimer = restarted.findChild<QTimer *>("automaticUpdateTimer");
        QCOMPARE(startupTimer->interval(), 5000);
        startupTimer->start(0);
        QTRY_COMPARE(network.requests, 2);
        network.reply->complete();
    }
    void startupChecksRespectOptOutAndRetryLimits()
    {
        rf::UpdateSettings settings;
        settings.automatic = false;
        FakeNetwork network;
        rf::UpdateChecker checker(settings, nullptr, &network);
        checker.startAutomaticChecks();
        auto *timer = checker.findChild<QTimer *>("automaticUpdateTimer");
        QVERIFY(!timer->isActive());
        checker.check(false);
        QCOMPARE(network.requests, 0);
        settings.retryAfter = QDateTime::currentDateTimeUtc().addSecs(300);
        checker.setAutomaticChecking(true);
        QVERIFY(timer->isActive());
        QVERIFY(timer->interval() > 290000);
        checker.check(false);
        QCOMPARE(network.requests, 0);
        settings.retryAfter = {};
        checker.check(false);
        QCOMPARE(network.requests, 1);
        network.reply->complete();
    }
    void noStableReleaseIsNotAnError()
    {
        rf::UpdateSettings settings;
        FakeNetwork network;
        rf::UpdateChecker checker(settings, nullptr, &network);
        QSignalSpy completed(&checker, &rf::UpdateChecker::finished);
        QSignalSpy failed(&checker, &rf::UpdateChecker::failed);
        checker.check();
        network.reply->status(404);
        network.reply->networkError(QNetworkReply::ContentNotFoundError);
        network.reply->complete();
        QCOMPARE(completed.count(), 1);
        QVERIFY(completed[0][0].toString().isEmpty());
        QVERIFY(failed.isEmpty());
    }
    void failures_data()
    {
        QTest::addColumn<int>("status");
        QTest::addColumn<int>("error");
        QTest::addColumn<QByteArray>("body");
        QTest::newRow("server") << 500 << int(QNetworkReply::NoError) << QByteArray("[]");
        QTest::newRow("offline") << 0 << int(QNetworkReply::HostNotFoundError) << QByteArray();
        QTest::newRow("TLS") << 0 << int(QNetworkReply::SslHandshakeFailedError) << QByteArray();
        QTest::newRow("invalid JSON")
            << 200 << int(QNetworkReply::NoError) << QByteArray("invalid");
        QTest::newRow("oversized")
            << 200 << int(QNetworkReply::NoError) << QByteArray(8 * 1024 * 1024 + 1, ' ');
    }
    void failures()
    {
        QFETCH(int, status);
        QFETCH(int, error);
        QFETCH(QByteArray, body);
        rf::UpdateSettings settings;
        FakeNetwork network;
        network.body = body;
        rf::UpdateChecker checker(settings, nullptr, &network);
        QSignalSpy completed(&checker, &rf::UpdateChecker::finished);
        QSignalSpy failed(&checker, &rf::UpdateChecker::failed);
        checker.check(false);
        network.reply->status(status);
        network.reply->networkError(static_cast<QNetworkReply::NetworkError>(error));
        network.reply->complete();
        QCOMPARE(completed.count(), 0);
        QCOMPARE(failed.count(), 1);
        QVERIFY(!failed[0][0].toString().isEmpty());
        QVERIFY(!failed[0][1].toBool());
    }
    void rateLimitsSurviveRestarts()
    {
        rf::UpdateSettings settings;
        FakeNetwork network;
        rf::UpdateChecker checker(settings, nullptr, &network);
        checker.check();
        network.reply->status(429);
        network.reply->header("Retry-After", "120");
        network.reply->complete();
        QVERIFY(settings.retryAfter > QDateTime::currentDateTimeUtc().addSecs(115));
        checker.check();
        QCOMPARE(network.requests, 1);
        rf::UpdateChecker restarted(settings, nullptr, &network);
        restarted.check();
        QCOMPARE(network.requests, 1);
        settings.retryAfter = {};
        checker.check();
        network.reply->status(403);
        network.reply->header("X-RateLimit-Remaining", "0");
        const auto reset = QDateTime::currentDateTimeUtc().addSecs(300);
        network.reply->header("X-RateLimit-Reset", QByteArray::number(reset.toSecsSinceEpoch()));
        network.reply->complete();
        QCOMPARE(settings.retryAfter.toSecsSinceEpoch(), reset.toSecsSinceEpoch());
        settings.retryAfter = {};
        checker.check();
        network.reply->status(429);
        const auto retry = QDateTime::currentDateTimeUtc().addSecs(600);
        network.reply->header("Retry-After", retry.toString(Qt::RFC2822Date).toLatin1());
        network.reply->complete();
        QCOMPARE(settings.retryAfter.toSecsSinceEpoch(), retry.toSecsSinceEpoch());
    }
    void timeoutAndCancellation()
    {
        rf::UpdateSettings settings;
        FakeNetwork network;
        rf::UpdateChecker checker(settings, nullptr, &network);
        QSignalSpy completed(&checker, &rf::UpdateChecker::finished);
        QSignalSpy failed(&checker, &rf::UpdateChecker::failed);
        checker.findChild<QTimer *>("updateRequestTimeout")->setInterval(10);
        checker.check();
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed[0][0].toString().contains("timed out"));
        checker.check();
        checker.stop();
        QVERIFY(network.reply->aborted);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(completed.count(), 0);
    }
    void notificationsAndMenu()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.json");
        rf::MainWindow window(nullptr, path);
        window.show();
        auto *automatic = window.findChild<QAction *>("automaticUpdateChecks");
        auto *checker = window.findChild<rf::UpdateChecker *>();
        QVERIFY(automatic);
        QVERIFY(window.findChild<QAction *>("checkForUpdates"));
        QVERIFY(!checker->findChild<QTimer *>("automaticUpdateTimer")->isActive());
        automatic->setChecked(false);
        QVERIFY(!rf::readPreferences(path).updates.automatic);
        QVERIFY(window.findChild<QAction *>("checkForUpdates")->isEnabled());
        checker->failed("offline", false);
        QVERIFY(!window.findChild<QDialog *>("updateDialog"));
        const QUrl url("https://gioeleminardi.github.io/CroccoSpectrum");
        checker->finished("0.1.7", url, false);
        auto *dialog = window.findChild<QDialog *>("updateDialog");
        QVERIFY(dialog);
        QVERIFY(!dialog->isModal());
        QCOMPARE(rf::readPreferences(path).updates.lastNotifiedVersion, QString("0.1.7"));
        const auto screenshots = qEnvironmentVariable("RF_UPDATE_SCREENSHOTS");
        if (!screenshots.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(dialog->grab().save(screenshots + "/update.png"));
        }
        QDesktopServices::setUrlHandler("https", this, "captureUrl");
        const auto resetUrlHandler =
            qScopeGuard([] { QDesktopServices::unsetUrlHandler("https"); });
        openingDialog_ = dialog;
        dialog->findChild<QPushButton *>("openUpdateRelease")->click();
        QTRY_COMPARE(openedUrl_, url);
        QVERIFY(dialogDestroyedBeforeLaunch_);
        QVERIFY(!window.findChild<QDialog *>("updateDialog"));
        checker->finished("0.1.7", url, false);
        QVERIFY(!window.findChild<QDialog *>("updateDialog"));
        checker->finished("0.1.7", url, true);
        QVERIFY(window.findChild<QDialog *>("updateDialog"));
        checker->failed("offline", true);
        QVERIFY(window.findChild<QDialog *>("updateDialog")
                    ->findChild<QLabel *>()
                    ->text()
                    .contains("offline"));
        window.close();
        QVERIFY(!checker->findChild<QTimer *>("automaticUpdateTimer")->isActive());
        rf::MainWindow restored(nullptr, path);
        QVERIFY(!restored.findChild<QAction *>("automaticUpdateChecks")->isChecked());
        restored.findChild<rf::UpdateChecker *>()->finished("0.1.7", url, false);
        QVERIFY(restored.findChild<QDialog *>("updateDialog"));
    }
    void releasePageLaunchFailure()
    {
        if (QGuiApplication::platformName() != "offscreen")
            QSKIP("The offscreen platform provides a browser launch failure without external apps");
        QTemporaryDir directory;
        rf::MainWindow window(nullptr, directory.filePath("preferences.json"));
        const QUrl url("https://gioeleminardi.github.io/CroccoSpectrum");
        window.findChild<rf::UpdateChecker *>()->finished("0.1.7", url, true);
        QPointer<QDialog> original = window.findChild<QDialog *>("updateDialog");
        QVERIFY(original);
        original->findChild<QPushButton *>("openUpdateRelease")->click();
        QTRY_VERIFY(!original);
        QTRY_VERIFY(window.findChild<QDialog *>("updateDialog"));
        auto *dialog = window.findChild<QDialog *>("updateDialog");
        auto *label = dialog->findChild<QLabel *>();
        QVERIFY(label->text().contains("Couldn't open the browser"));
        QVERIFY(label->text().contains(url.toDisplayString()));
        QVERIFY(label->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
        QVERIFY(dialog->findChild<QPushButton *>("openUpdateRelease"));
        dialog->reject();
        QTRY_VERIFY(!window.findChild<QDialog *>("updateDialog"));
    }
    void waylandReleasePageHandoff()
    {
        if (!QGuiApplication::platformName().startsWith("wayland"))
            QSKIP("Requires a Wayland session to exercise Qt's asynchronous URL launch");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile opener(directory.filePath("xdg-open"));
        QVERIFY(opener.open(QIODevice::WriteOnly));
        opener.write("#!/bin/sh\nprintf '%s\\n' \"$1\" > \"$0.url\"\n");
        opener.close();
        QVERIFY(opener.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto originalPath = qgetenv("PATH");
        const auto restorePath = qScopeGuard([originalPath] { qputenv("PATH", originalPath); });
        qputenv("PATH", directory.path().toUtf8());

        rf::MainWindow window(nullptr, directory.filePath("preferences.json"));
        window.show();
        const QUrl url("https://gioeleminardi.github.io/CroccoSpectrum");
        window.findChild<rf::UpdateChecker *>()->finished("0.1.7", url, true);
        auto *dialog = window.findChild<QDialog *>("updateDialog");
        QVERIFY(dialog);
        dialog->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(dialog));
        dialog->findChild<QPushButton *>("openUpdateRelease")->click();
        QTRY_VERIFY(!window.findChild<QDialog *>("updateDialog"));
        QFile openedUrl(directory.filePath("xdg-open.url"));
        QTRY_VERIFY_WITH_TIMEOUT(openedUrl.exists(), 5000);
        QVERIFY(openedUrl.open(QIODevice::ReadOnly));
        QCOMPARE(openedUrl.readAll().trimmed(), url.toEncoded());
    }
    void liveHttpsCheck()
    {
        if (!qEnvironmentVariableIsSet("RF_TEST_LIVE_UPDATE_CHECK"))
            QSKIP("Live HTTPS verification is opt-in; normal tests stay offline");
        QVERIFY(QSslSocket::supportsSsl());
        rf::UpdateSettings settings;
        rf::UpdateChecker checker(settings);
        QSignalSpy completed(&checker, &rf::UpdateChecker::finished);
        QSignalSpy failed(&checker, &rf::UpdateChecker::failed);
        checker.check();
        QTRY_VERIFY_WITH_TIMEOUT(completed.count() + failed.count() == 1, 15000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
        QCOMPARE(completed.count(), 1);
    }

  private:
    QUrl openedUrl_;
    QPointer<QDialog> openingDialog_;
    bool dialogDestroyedBeforeLaunch_ = false;
};

QTEST_MAIN(UpdateTests)
#include "UpdateTests.moc"
