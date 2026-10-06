#include "ui/MainWindow.h"
#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

// QSignalSpy observes signals synchronously. Relay worker emissions onto the
// GUI thread first, so reading a spy cannot race its worker-thread append.
class WorkerSignals : public QObject
{
    Q_OBJECT
  public:
    explicit WorkerSignals(rf::AnalysisController &controller)
    {
        connect(&controller, &rf::AnalysisController::recordingOpened, this,
                &WorkerSignals::recordingOpened, Qt::QueuedConnection);
        connect(&controller, &rf::AnalysisController::previewReady, this,
                &WorkerSignals::previewReady, Qt::QueuedConnection);
        connect(&controller, &rf::AnalysisController::averageReady, this,
                &WorkerSignals::averageReady, Qt::QueuedConnection);
        connect(&controller, &rf::AnalysisController::waveformReady, this,
                &WorkerSignals::waveformReady, Qt::QueuedConnection);
        connect(&controller, &rf::AnalysisController::finished, this, &WorkerSignals::finished,
                Qt::QueuedConnection);
        connect(&controller, &rf::AnalysisController::failed, this, &WorkerSignals::failed,
                Qt::QueuedConnection);
    }
  signals:
    void recordingOpened(quint64, std::shared_ptr<rf::Recording>, rf::FrameRange);
    void previewReady(quint64, std::shared_ptr<const rf::PreviewResult>);
    void averageReady(quint64, std::shared_ptr<const rf::AverageResult>);
    void waveformReady(quint64, std::shared_ptr<const rf::WaveformResult>);
    void finished(quint64, QString);
    void failed(quint64, QString);
};

class UiTests : public QObject
{
    Q_OBJECT
  private slots:
    void brandingAndAbout()
    {
        QTemporaryDir directory;
        rf::MainWindow window(nullptr, directory.filePath("preferences.json"));
        window.show();
        QCOMPARE(window.windowTitle(), QString("CroccoSpectrum"));
        QVERIFY(!window.windowIcon().pixmap(64, 64).isNull());
        auto *about = window.findChild<QAction *>("aboutCroccoSpectrum");
        QVERIFY(about);
        bool visibleLogo = false;
        bool correctTitle = false;
        bool screenshotSaved = true;
        // The modal event loop services this timer after the dialog is painted.
        QTimer::singleShot(50, &window, [&] {
            auto *dialog = window.findChild<QDialog *>("aboutDialog");
            if (!dialog)
                return;
            correctTitle = dialog->windowTitle() == "About CroccoSpectrum";
            auto *logo = dialog->findChild<QLabel *>("aboutLogo");
            visibleLogo = logo && logo->isVisible() && !logo->pixmap().isNull();
            const auto screenshots = qEnvironmentVariable("RF_TEST_SCREENSHOT_DIR");
            if (!screenshots.isEmpty())
                screenshotSaved =
                    QDir().mkpath(screenshots) && dialog->grab().save(screenshots + "/about.png");
            dialog->reject();
        });
        about->trigger();
        QVERIFY(correctTitle);
        QVERIFY(visibleLogo);
        QVERIFY(screenshotSaved);
    }
    void legacyPreferencesSurviveRename()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto oldConfig = qgetenv("XDG_CONFIG_HOME");
        const auto oldOrganization = QCoreApplication::organizationName();
        const auto oldApplication = QCoreApplication::applicationName();
        const auto restoreEnvironment = qScopeGuard([&] {
            if (oldConfig.isNull())
                qunsetenv("XDG_CONFIG_HOME");
            else
                qputenv("XDG_CONFIG_HOME", oldConfig);
            QCoreApplication::setOrganizationName(oldOrganization);
            QCoreApplication::setApplicationName(oldApplication);
        });
        qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
        QCoreApplication::setOrganizationName("CroccoSpectrum");
        QCoreApplication::setApplicationName("CroccoSpectrum");
        const auto legacyPath = directory.filePath("RFAnalyzer/rf-analyzer/preferences.json");
        QVERIFY(QDir().mkpath(directory.filePath("RFAnalyzer/rf-analyzer")));
        rf::Preferences saved;
        saved.dsp.fftSize = 8192;
        saved.view.colorMin = -110;
        rf::savePreferences(legacyPath, saved);
        rf::MainWindow window;
        QCOMPARE(window.dspSettings().fftSize, 8192);
        window.close();
        const auto renamedPath =
            QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
            "/preferences.json";
        QCOMPARE(rf::readPreferences(renamedPath).view.colorMin, -110.0);
        QCOMPARE(rf::readPreferences(legacyPath).dsp.fftSize, 8192);
    }
    void frequencyCursorPrecision()
    {
        rf::SpectrumPlot plot;
        plot.resize(600, 250);
        plot.show();
        auto result = std::make_shared<rf::PreviewResult>();
        result->frequencies = {-95.367431640625, 0, 95.367431640625};
        result->spectrum.power = {0.25, 0.5, 0.25};
        plot.setPreview(result);
        rf::ViewSettings view;
        view.absoluteFrequency = true;
        plot.setView(view, rf::PowerScale::Spectrum, 1'000'000'000);
        QSignalSpy cursor(&plot, &rf::SpectrumPlot::cursorChanged);
        QTest::mouseMove(&plot, QPoint(76, 60));
        QTRY_VERIFY(!cursor.isEmpty());
        QVERIFY(cursor.last().at(0).toString().contains("999999904.63256836 Hz"));
    }
    void latestRequestAndPalette()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("signal.iq");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromHex("00400000").repeated(50000));
        file.close();
        rf::MainWindow window(nullptr, directory.filePath("preferences.json"));
        window.show();
        QSignalSpy ready(&window, &rf::MainWindow::analysisDisplayed);
        QSignalSpy errors(&window, &rf::MainWindow::analysisError);
        rf::RecordingDescriptor descriptor;
        descriptor.path = path;
        window.openRecording(descriptor);
        QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
        QCOMPARE(errors.count(), 0);
        QVERIFY(window.previewResult());
        const auto before = window.previewResult();
        window.findChild<QDoubleSpinBox *>("colorMinimum")->setValue(-80);
        QTest::qWait(250);
        QCOMPARE(window.previewResult(), before); // Palette-only editing must not run DSP.
        auto *fft = window.findChild<QComboBox *>("fftSize");
        fft->setCurrentIndex(fft->findData(512));
        fft->setCurrentIndex(fft->findData(1024));
        fft->setCurrentIndex(fft->findData(2048));
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.previewResult(), 10000);
        QCOMPARE(window.dspSettings().fftSize, 2048);
        QCOMPARE(window.previewResult()->spectrum.power.size(), std::size_t{2048});
        window.findChild<QLineEdit *>("selectionStart")->setText("4096");
        window.findChild<QLineEdit *>("selectionEnd")->setText("12288");
        QTest::mouseClick(window.findChild<QPushButton *>("applySelection"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.previewResult(), 10000);
        QCOMPARE(window.selectedRange().begin, std::uint64_t{4096});
        QCOMPARE(window.previewResult()->spectrumStart, std::uint64_t{4096});
        window.close();
        const auto preferences = rf::readPreferences(directory.filePath("preferences.json"));
        QCOMPARE(preferences.dsp.fftSize, 2048);
        QCOMPARE(preferences.view.colorMin, -80.0);
    }
    void controllerCancellation()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath("large.iq"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(4LL * 1024 * 1024 * 1024));
        file.close();
        auto descriptor = rf::RecordingDescriptor{};
        descriptor.path = file.fileName();
        auto recording = std::make_shared<rf::Recording>(descriptor);
        rf::AnalysisController controller;
        WorkerSignals observed(controller);
        QSignalSpy averages(&observed, &WorkerSignals::averageReady);
        QSignalSpy previews(&observed, &WorkerSignals::previewReady);
        rf::DspSettings settings;
        settings.fftSize = 4096;
        controller.average(recording, {0, recording->frameCount()}, settings);
        QTest::qWait(30);
        const auto generation = controller.preview(recording, {8192, 16384}, settings);
        QTRY_COMPARE_WITH_TIMEOUT(previews.count(), 1, 10000);
        QCOMPARE(previews.at(0).at(0).toULongLong(), generation);
        QCOMPARE(averages.count(), 0);
    }
    void tinyImportAndPngMetadata()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath("tiny.iq"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray::fromHex("00400000").repeated(16));
        file.close();
        rf::RecordingDescriptor descriptor;
        descriptor.path = file.fileName();
        rf::AnalysisController controller;
        WorkerSignals observed(controller);
        QSignalSpy opened(&observed, &WorkerSignals::recordingOpened);
        QSignalSpy waveform(&observed, &WorkerSignals::waveformReady);
        QSignalSpy finished(&observed, &WorkerSignals::finished);
        QSignalSpy failed(&observed, &WorkerSignals::failed);
        controller.inspect(descriptor);
        QTRY_COMPARE_WITH_TIMEOUT(waveform.count(), 1, 5000);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(failed.count(), 0);
        const auto result =
            qvariant_cast<std::shared_ptr<const rf::WaveformResult>>(waveform.at(0).at(1));
        QCOMPARE(result->points.size(), std::size_t{16});
        QCOMPARE(result->points[0].minI, 0.5);
        rf::PngRequest request;
        request.image = QImage(100, 80, QImage::Format_RGB32);
        request.image.fill(Qt::red);
        request.metadata = {{"unit", "dBFS"}, {"start", "9007199254740993"}};
        request.output = directory.filePath("plot.png");
        controller.png(request);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        const QImage restored(request.output);
        QVERIFY(!restored.isNull());
        QVERIFY(restored.text("CroccoSpectrum").contains("9007199254740993"));
        controller.png(request);
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
        QCOMPARE(QImage(request.output), restored);
    }
};
QTEST_MAIN(UiTests)
#include "UiTests.moc"
