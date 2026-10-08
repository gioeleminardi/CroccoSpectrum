#include "ui/MainWindow.h"
#include <QAction>
#include <QComboBox>
#include <QDataStream>
#include <QDialog>
#include <QDockWidget>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QWheelEvent>
#include <QtTest>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace
{
rf::RecordingDescriptor toneRecording(const QString &path, const std::vector<int> &bins)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Fixture write failed");
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    for (const auto bin : bins)
        for (int sample = 0; sample < 256; ++sample) {
            const double phase = 2 * std::numbers::pi * bin * sample / 256;
            stream << static_cast<qint16>(std::round(16000 * std::cos(phase)))
                   << static_cast<qint16>(std::round(16000 * std::sin(phase)));
        }
    if (stream.status() != QDataStream::Ok)
        throw std::runtime_error("Fixture write failed");
    rf::RecordingDescriptor descriptor;
    descriptor.path = path;
    descriptor.sampleRate = 256;
    return descriptor;
}

QPoint rowPosition(const rf::WaterfallPlot &plot, std::size_t row, std::size_t rows)
{
    return {100, 32 + qRound((row + 0.5) * (plot.height() - 70) / rows)};
}

QPoint waveformRowPosition(const rf::WaveformPlot &plot, std::size_t row, std::size_t rows)
{
    return {76 + qRound((row + 0.5) * (plot.width() - 100) / rows), 60};
}

void moveMouse(QWidget *plot, const QPoint &position, Qt::MouseButtons buttons = Qt::NoButton,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(QEvent::MouseMove, position, plot->mapToGlobal(position), Qt::NoButton,
                      buttons, modifiers);
    QApplication::sendEvent(plot, &event);
}

void shiftDrag(QWidget *plot, const QPoint &from, const QPoint &to)
{
    QTest::mousePress(plot, Qt::LeftButton, Qt::ShiftModifier, from);
    moveMouse(plot, to, Qt::LeftButton, Qt::ShiftModifier);
    QTest::mouseRelease(plot, Qt::LeftButton, Qt::ShiftModifier, to);
}

QAction *findAction(const QWidget &widget, const QString &text)
{
    for (auto *action : widget.findChildren<QAction *>())
        if (action->text() == text)
            return action;
    return nullptr;
}
} // namespace

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
    void spectrumMeasurement()
    {
        rf::SpectrumPlot plot;
        plot.resize(600, 310);
        auto result = std::make_shared<rf::PreviewResult>();
        result->frequencies = {-2000, -1000, 0, 1000, 2000};
        result->spectrum.power = {0, 0.25, 1, 0.25, 0};
        result->spectrumStart = 256;
        plot.setPreview(result);
        plot.show();
        QSignalSpy ranges(&plot, &rf::SpectrumPlot::frequencyRangeChanged);
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(176, 180));
        QVERIFY(plot.isMeasuring());
        moveMouse(&plot, QPoint(276, 180));
        QVERIFY(plot.measurementText().contains("Δf: 1 kHz"));
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(276, 180));
        QVERIFY(!plot.isMeasuring());
        // Mean(0.25, 1) = 0.625; averaging dB would incorrectly give -3.0103.
        QVERIFY(plot.measurementText().contains("-2.041199827 dBFS"));
        QVERIFY(plot.measurementText().contains("frame 256"));
        QCOMPARE(ranges.count(), 0);
        const auto text = plot.measurementText();
        plot.resize(420, 230);
        plot.setFrequencyRange(-1500, 1500);
        rf::ViewSettings view;
        view.palette = "Inferno";
        view.colorMin = -120;
        plot.setView(view, rf::PowerScale::Spectrum, 0);
        QCOMPARE(plot.measurementText(), text);
        plot.resize(600, 310);
        plot.resetFrequency();
        auto next = std::make_shared<rf::PreviewResult>(*result);
        next->spectrumStart = 512;
        next->spectrum.power = {0, 0.5, 0.5, 0.25, 0};
        plot.setPreview(next, false);
        QVERIFY(plot.measurementText().contains("-3.010299957 dBFS"));
        QVERIFY(plot.measurementText().contains("frame 512"));
        view.absoluteFrequency = true;
        plot.setView(view, rf::PowerScale::Spectrum, 1'000'000'000);
        QVERIFY(plot.measurementText().contains("Δf: 1 kHz"));
        const auto screenshots = qEnvironmentVariable("RF_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(plot.grab().save(screenshots + "/spectrum-measurement.png"));
            plot.resize(360, 180);
            QVERIFY(plot.grab().save(screenshots + "/spectrum-measurement-small.png"));
        }
        QTest::keyClick(&plot, Qt::Key_Escape);
        QVERIFY(plot.measurementText().isEmpty());
        plot.resize(600, 310);
        plot.setView({}, rf::PowerScale::Spectrum, 0);
        // Reverse direction, then replace with a same-bin zero-power selection.
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(276, 180));
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(176, 180));
        QVERIFY(plot.measurementText().contains("Δf: 1 kHz"));
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(76, 180));
        QVERIFY(plot.isMeasuring());
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(76, 180));
        QVERIFY(plot.measurementText().contains("Δf: 0 Hz"));
        QVERIFY(plot.measurementText().contains("−∞ dBFS"));
        plot.setPreview(result);
        QVERIFY(plot.measurementText().isEmpty());
    }
    void spectrumMeasurementEditing_data()
    {
        QTest::addColumn<bool>("reversed");
        QTest::newRow("forward") << false;
        QTest::newRow("reverse") << true;
    }
    void spectrumMeasurementEditing()
    {
        QFETCH(bool, reversed);
        rf::SpectrumPlot plot;
        plot.resize(600, 310);
        auto result = std::make_shared<rf::PreviewResult>();
        result->frequencies = {0, 1000, 2000, 3000, 4000, 5000, 6000};
        result->spectrum.power = {1, 1, 1, 1, 4, 4, 4};
        plot.setPreview(result);
        plot.show();
        const QPoint first(159, 200), last(326, 200);
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, reversed ? last : first);
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, reversed ? first : last);
        QVERIFY(plot.measurementText().contains("0 dBFS"));
        QSignalSpy ranges(&plot, &rf::SpectrumPlot::frequencyRangeChanged);
        QTest::mousePress(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(243, 200));
        QVERIFY(plot.isMeasuring());
        moveMouse(&plot, QPoint(326, 200), Qt::LeftButton, Qt::ShiftModifier);
        // Move one bin: mean(1, 1, 4) = 2, while width stays 2 kHz.
        QVERIFY(plot.measurementText().contains("3.010299957 dBFS"));
        QVERIFY(plot.measurementText().contains("Δf: 2 kHz"));
        QVERIFY(plot.measurementText().contains(reversed ? "Start: 4 kHz · End: 2 kHz"
                                                        : "Start: 2 kHz · End: 4 kHz"));
        QTest::mouseRelease(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(326, 200));
        QVERIFY(!plot.isMeasuring());
        shiftDrag(&plot, QPoint(243, 200), QPoint(159, 200));
        QVERIFY(plot.measurementText().contains("Δf: 3 kHz"));
        shiftDrag(&plot, QPoint(409, 200), QPoint(493, 200));
        QVERIFY(plot.measurementText().contains("Δf: 4 kHz"));
        // Cross the other marker, then move the band beyond both plot edges.
        shiftDrag(&plot, QPoint(159, 200), QPoint(576, 200));
        QVERIFY(plot.measurementText().contains("Δf: 1 kHz"));
        QVERIFY(plot.measurementText().contains("6.020599913 dBFS"));
        shiftDrag(&plot, QPoint(534, 200), QPoint(-100, 200));
        QVERIFY(plot.measurementText().contains("Δf: 1 kHz"));
        QVERIFY(plot.measurementText().contains("0 dBFS"));
        shiftDrag(&plot, QPoint(118, 200), QPoint(800, 200));
        QVERIFY(plot.measurementText().contains("Δf: 1 kHz"));
        QVERIFY(plot.measurementText().contains("6.020599913 dBFS"));
        QCOMPARE(ranges.count(), 0);
        // A click without a drag still begins a replacement measurement.
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(534, 200));
        QVERIFY(plot.isMeasuring());
        QTest::keyClick(&plot, Qt::Key_Escape);
        QVERIFY(!plot.isMeasuring());
        QVERIFY(plot.measurementText().isEmpty());
    }
    void spectrumMeasurementDensityAndInvalid()
    {
        rf::SpectrumPlot plot;
        plot.resize(600, 310);
        auto result = std::make_shared<rf::PreviewResult>();
        // Real-input axis includes Nyquist. Select all bins, including zero.
        result->frequencies = {0, 1000, 2000};
        result->spectrum.power = {0, 1, 2};
        plot.setPreview(result);
        plot.setView({}, rf::PowerScale::Density, 0);
        plot.show();
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(76, 180));
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(576, 180));
        QVERIFY(plot.measurementText().contains("Δf: 2 kHz"));
        QVERIFY(plot.measurementText().contains("0 dBFS/Hz"));
        auto invalid = std::make_shared<rf::PreviewResult>(*result);
        invalid->spectrum.valid = false;
        invalid->spectrum.power.clear();
        plot.setPreview(invalid, false);
        QVERIFY(plot.measurementText().contains("unavailable"));
        QVERIFY(plot.measurementText().contains("Δf: 2 kHz"));
        QVERIFY(!plot.grab().isNull());
        plot.clear();
        QVERIFY(plot.measurementText().isEmpty());
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(76, 180));
        QVERIFY(!plot.isMeasuring());
        auto average = std::make_shared<rf::AverageResult>();
        average->frequencies = result->frequencies;
        average->averagePower = {0, 1, 2};
        average->maxPower = {0, 4, 8};
        average->validWindows = 1;
        plot.setAverage(average, false);
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(76, 180));
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(576, 180));
        QVERIFY(plot.measurementText().contains("Average trace"));
        plot.setAverage(average, true);
        QVERIFY(plot.measurementText().contains("6.020599913 dBFS/Hz"));
        QVERIFY(plot.measurementText().contains("Max-hold trace"));
    }
    void spectrumMeasurementUsesEveryBin()
    {
        rf::SpectrumPlot plot;
        plot.resize(600, 310);
        auto result = std::make_shared<rf::PreviewResult>();
        constexpr std::size_t bins = 1'048'576;
        result->frequencies.resize(bins);
        result->spectrum.power.resize(bins);
        for (std::size_t bin = 0; bin < bins; ++bin)
            result->frequencies[bin] = static_cast<double>(bin) - 524288;
        result->spectrum.power[500000] = 1;
        plot.setPreview(result);
        plot.show();
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(76, 180));
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(576, 180));
        // A screen-pixel mean would give a much higher value for this single tone.
        QVERIFY(plot.measurementText().contains("-60.20599913 dBFS"));
        QVERIFY(plot.measurementText().contains("Δf: 1.048575 MHz"));
    }
    void waterfallMeasurement()
    {
        rf::WaterfallPlot plot;
        plot.resize(600, 310);
        constexpr std::uint64_t begin = 9'007'199'254'740'993;
        auto result = std::make_shared<rf::PreviewResult>();
        result->range = {begin, begin + 2000};
        result->frequencies = {-1, 0, 1};
        result->columns = 3;
        result->rowStarts = {begin, begin + 1, begin + 1000};
        result->waterfall.assign(9, 0.01);
        result->sampled = true;
        plot.setPreview(result, 2.5);
        plot.show();
        QSignalSpy clicked(&plot, &rf::WaterfallPlot::frameClicked);
        QSignalSpy selected(&plot, &rf::WaterfallPlot::frameSelected);
        QSignalSpy hovered(&plot, &rf::WaterfallPlot::frameHovered);
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, rowPosition(plot, 1, 3));
        QVERIFY(plot.isMeasuring());
        hovered.clear();
        moveMouse(&plot, rowPosition(plot, 0, 3));
        QCOMPARE(hovered.count(), 0);
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, rowPosition(plot, 0, 3));
        QVERIFY(!plot.isMeasuring());
        QVERIFY(plot.measurementText().contains("Duration: 400 ms"));
        QVERIFY(plot.measurementText().contains("1 frames"));
        QVERIFY(plot.measurementText().contains(QString::number(begin + 1)));
        QCOMPARE(clicked.count(), 0);
        QCOMPARE(selected.count(), 0);
        const auto text = plot.measurementText();
        rf::ViewSettings view;
        view.palette = "Inferno";
        plot.setView(view, rf::PowerScale::Spectrum, 0);
        plot.setFrequencyRange(-0.5, 0.5);
        plot.resize(420, 230);
        QCOMPARE(plot.measurementText(), text);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&plot, &leave);
        QCOMPARE(plot.measurementText(), text);
        QTest::keyClick(&plot, Qt::Key_Escape);
        QVERIFY(plot.measurementText().isEmpty());
        plot.resize(600, 310);
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, rowPosition(plot, 0, 3));
        QTest::mouseDClick(&plot, Qt::LeftButton, Qt::ShiftModifier, rowPosition(plot, 2, 3));
        QVERIFY(!plot.isMeasuring());
        QVERIFY(plot.measurementText().contains("Duration: 400 s"));
        QCOMPARE(selected.count(), 0);
        const auto screenshots = qEnvironmentVariable("RF_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(plot.grab().save(screenshots + "/waterfall-measurement.png"));
            plot.resize(360, 210);
            QVERIFY(plot.grab().save(screenshots + "/waterfall-measurement-small.png"));
        }
        plot.setPreview(result, 2.5);
        QVERIFY(plot.measurementText().isEmpty());
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, QPoint(10, 10));
        QVERIFY(!plot.isMeasuring());
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, rowPosition(plot, 1, 3));
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, rowPosition(plot, 1, 3));
        QVERIFY(plot.measurementText().contains("Duration: 0 s"));
        plot.clear();
        QVERIFY(plot.measurementText().isEmpty());
    }
    void waterfallMeasurementEditing_data()
    {
        spectrumMeasurementEditing_data();
    }
    void waterfallMeasurementEditing()
    {
        QFETCH(bool, reversed);
        rf::WaterfallPlot plot;
        plot.resize(600, 310);
        constexpr std::uint64_t begin = 9'007'199'254'740'993;
        auto result = std::make_shared<rf::PreviewResult>();
        result->range = {begin, begin + 200};
        result->frequencies = {-1, 0, 1};
        result->columns = 3;
        result->rowStarts = {begin, begin + 1, begin + 10, begin + 20, begin + 40, begin + 100};
        result->waterfall.assign(18, 0.01);
        result->sampled = true;
        plot.setPreview(result, 2.5);
        plot.show();
        const auto first = rowPosition(plot, reversed ? 3 : 1, 6);
        const auto last = rowPosition(plot, reversed ? 1 : 3, 6);
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, first);
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::ShiftModifier, last);
        QVERIFY(plot.measurementText().contains("Duration: 7.6 s"));
        QSignalSpy clicked(&plot, &rf::WaterfallPlot::frameClicked);
        QSignalSpy selected(&plot, &rf::WaterfallPlot::frameSelected);
        QSignalSpy hovered(&plot, &rf::WaterfallPlot::frameHovered);
        QTest::mousePress(&plot, Qt::LeftButton, Qt::ShiftModifier, rowPosition(plot, 2, 6));
        QVERIFY(plot.isMeasuring());
        hovered.clear();
        moveMouse(&plot, rowPosition(plot, 3, 6), Qt::LeftButton, Qt::ShiftModifier);
        // Equal row span, different duration because timestamps are irregular.
        QVERIFY(plot.measurementText().contains("Duration: 12 s"));
        QCOMPARE(hovered.count(), 0);
        QTest::mouseRelease(&plot, Qt::LeftButton, Qt::ShiftModifier, rowPosition(plot, 3, 6));
        QVERIFY(!plot.isMeasuring());
        shiftDrag(&plot, rowPosition(plot, 2, 6), rowPosition(plot, 0, 6));
        QVERIFY(plot.measurementText().contains("Duration: 16 s"));
        shiftDrag(&plot, rowPosition(plot, 4, 6), rowPosition(plot, 5, 6));
        QVERIFY(plot.measurementText().contains("Duration: 40 s"));
        shiftDrag(&plot, rowPosition(plot, 0, 6), rowPosition(plot, 5, 6));
        QVERIFY(plot.measurementText().contains("Duration: 0 s"));
        shiftDrag(&plot, rowPosition(plot, 5, 6), rowPosition(plot, 3, 6));
        QVERIFY(plot.measurementText().contains("Duration: 32 s"));
        shiftDrag(&plot, rowPosition(plot, 4, 6), QPoint(100, -100));
        QVERIFY(plot.measurementText().contains("Duration: 4 s"));
        shiftDrag(&plot, rowPosition(plot, 1, 6), QPoint(100, 500));
        QVERIFY(plot.measurementText().contains("Duration: 32 s"));
        QCOMPARE(clicked.count(), 0);
        QCOMPARE(selected.count(), 0);
        // Clearing during a marker drag must also release the interaction lock.
        QTest::mousePress(&plot, Qt::LeftButton, Qt::ShiftModifier, rowPosition(plot, 5, 6));
        moveMouse(&plot, rowPosition(plot, 4, 6), Qt::LeftButton, Qt::ShiftModifier);
        QTest::keyClick(&plot, Qt::Key_Escape);
        QTest::mouseRelease(&plot, Qt::LeftButton, Qt::ShiftModifier, rowPosition(plot, 4, 6));
        QVERIFY(!plot.isMeasuring());
        QVERIFY(plot.measurementText().isEmpty());
    }
    void measurementsPreserveFrameFreeze_data()
    {
        QTest::addColumn<bool>("frozen");
        QTest::newRow("following") << false;
        QTest::newRow("frozen") << true;
    }
    void measurementsPreserveFrameFreeze()
    {
        QFETCH(bool, frozen);
        QTemporaryDir directory;
        rf::Preferences saved;
        saved.dsp.fftSize = 256;
        saved.dsp.overlapPercent = 0;
        const auto preferences = directory.filePath("preferences.json");
        rf::savePreferences(preferences, saved);
        rf::MainWindow window(nullptr, preferences);
        window.show();
        QTest::mouseMove(&window, QPoint(5, 5));
        window.openRecording(toneRecording(directory.filePath("tones.iq"), {32, 64, 96}));
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        auto *spectrum = window.findChild<rf::SpectrumPlot *>("spectrumPlot");
        auto *waterfall = window.findChild<rf::WaterfallPlot *>("waterfallPlot");
        auto *waveform = window.findChild<rf::WaveformPlot *>("waveformPlot");
        const auto range = window.selectedRange();
        if (frozen)
            QTest::mouseClick(waterfall, Qt::LeftButton, Qt::NoModifier,
                              rowPosition(*waterfall, 0, 3));
        // Submit a hover before starting: its queued result must be invalidated.
        moveMouse(waterfall, rowPosition(*waterfall, 1, 3));
        QTest::mouseClick(spectrum, Qt::LeftButton, Qt::ShiftModifier, QPoint(100, 100));
        const auto held = window.spectrumResult();
        moveMouse(waterfall, rowPosition(*waterfall, 2, 3));
        moveMouse(waveform, waveformRowPosition(*waveform, 2, 3));
        QTest::qWait(100);
        QCOMPARE(window.spectrumResult(), held);
        QTest::mouseClick(spectrum, Qt::LeftButton, Qt::ShiftModifier, QPoint(200, 100));
        QVERIFY(!spectrum->measurementText().isEmpty());
        moveMouse(waterfall, rowPosition(*waterfall, 2, 3));
        if (frozen) {
            QTest::qWait(100);
            QCOMPARE(window.spectrumResult(), held);
        } else
            QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart,
                                      std::uint64_t{512}, 10000);
        QVERIFY(!spectrum->measurementText().isEmpty());
        // Moving the completed band holds the source frame throughout the drag.
        QTest::mousePress(spectrum, Qt::LeftButton, Qt::ShiftModifier, QPoint(150, 100));
        QVERIFY(spectrum->isMeasuring());
        const auto editingHeld = window.spectrumResult();
        moveMouse(waveform, waveformRowPosition(*waveform, 1, 3));
        moveMouse(spectrum, QPoint(175, 100), Qt::LeftButton, Qt::ShiftModifier);
        QTest::qWait(100);
        QCOMPARE(window.spectrumResult(), editingHeld);
        QTest::mouseRelease(spectrum, Qt::LeftButton, Qt::ShiftModifier, QPoint(175, 100));
        QVERIFY(!spectrum->isMeasuring());
        // Duration selection also suspends waveform-driven updates and never seeks.
        QTest::mouseClick(waterfall, Qt::LeftButton, Qt::ShiftModifier,
                          rowPosition(*waterfall, 0, 3));
        const auto durationHeld = window.spectrumResult();
        moveMouse(waveform, waveformRowPosition(*waveform, 1, 3));
        QTest::qWait(100);
        QCOMPARE(window.spectrumResult(), durationHeld);
        QTest::mouseDClick(waterfall, Qt::LeftButton, Qt::ShiftModifier,
                           rowPosition(*waterfall, 2, 3));
        QVERIFY(waterfall->measurementText().contains("Duration: 2 s"));
        QCOMPARE(window.selectedRange(), range);
        QTest::mousePress(waterfall, Qt::LeftButton, Qt::ShiftModifier,
                          rowPosition(*waterfall, 2, 3));
        QVERIFY(waterfall->isMeasuring());
        moveMouse(waveform, waveformRowPosition(*waveform, 0, 3));
        moveMouse(waterfall, rowPosition(*waterfall, 1, 3), Qt::LeftButton, Qt::ShiftModifier);
        QTest::qWait(100);
        QCOMPARE(window.spectrumResult(), durationHeld);
        QVERIFY(waterfall->measurementText().contains("Duration: 1 s"));
        QTest::mouseRelease(waterfall, Qt::LeftButton, Qt::ShiftModifier,
                            rowPosition(*waterfall, 1, 3));
        QVERIFY(!waterfall->isMeasuring());
        moveMouse(waterfall, rowPosition(*waterfall, 1, 3));
        if (frozen) {
            QTest::qWait(100);
            QCOMPARE(window.spectrumResult(), durationHeld);
        } else
            QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart,
                                      std::uint64_t{256}, 10000);
        // A new gesture in the other plot cancels an unfinished selection.
        QTest::mouseClick(waterfall, Qt::LeftButton, Qt::ShiftModifier,
                          rowPosition(*waterfall, 0, 3));
        QVERIFY(waterfall->isMeasuring());
        QTest::mouseClick(spectrum, Qt::LeftButton, Qt::ShiftModifier, QPoint(100, 100));
        QVERIFY(!waterfall->isMeasuring());
        QVERIFY(waterfall->measurementText().isEmpty());
        QTest::keyClick(spectrum, Qt::Key_Escape);
        moveMouse(waterfall, rowPosition(*waterfall, 2, 3));
        if (!frozen)
            QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart,
                                      std::uint64_t{512}, 10000);
        window.findChild<QLineEdit *>("selectionEnd")->setText("512");
        QTest::mouseClick(window.findChild<QPushButton *>("applySelection"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        QVERIFY(spectrum->measurementText().isEmpty());
        QVERIFY(waterfall->measurementText().isEmpty());
    }
    void waveformFrameCursor()
    {
        rf::WaveformPlot plot;
        plot.resize(600, 310);
        plot.show();
        auto result = std::make_shared<rf::WaveformResult>();
        constexpr std::uint64_t begin = 9'007'199'254'740'993;
        result->range = {begin, begin + 1000};
        result->points = {{begin, begin + 333}, {begin + 333, begin + 666},
                          {begin + 666, begin + 1000}};
        result->complete = true;
        plot.setWaveform(result, 256, rf::SampleKind::Complex);
        QTest::mouseMove(&plot, QPoint(10, 10));
        const auto original = plot.grab().toImage();
        plot.setFrameCursor(begin + 500, false);
        const auto highlighted = plot.grab().toImage();
        for (int x = 76; x < 576; ++x)
            QCOMPARE(highlighted.pixelColor(x, 100) != original.pixelColor(x, 100),
                     x == 325 || x == 326);
        for (int y = 32; y < 272; ++y)
            QVERIFY(highlighted.pixelColor(326, y) != original.pixelColor(326, y));
        plot.setFrameCursor(begin + 500, true);
        const auto frozen = plot.grab().toImage();
        QTest::mouseMove(&plot, QPoint(500, 60));
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&plot, &leave);
        QCOMPARE(plot.grab().toImage(), frozen);
        plot.setFrameCursor(begin + 500, false);
        QApplication::sendEvent(&plot, &leave);
        QCOMPARE(plot.grab().toImage(), original);
        QSignalSpy clicked(&plot, &rf::WaveformPlot::frameClicked);
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::NoModifier, QPoint(326, 60));
        QCOMPARE(clicked.count(), 1);
        QCOMPARE(clicked.last().at(0).toULongLong(), begin + 333);
        QTest::mouseClick(&plot, Qt::RightButton, Qt::NoModifier, QPoint(326, 60));
        QTest::mouseClick(&plot, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QCOMPARE(clicked.count(), 1);
    }
    void sharedFrameFreeze_data()
    {
        QTest::addColumn<bool>("floating");
        QTest::newRow("docked") << false;
        QTest::newRow("floating") << true;
    }
    void sharedFrameFreeze()
    {
        QFETCH(bool, floating);
        QTemporaryDir directory;
        rf::Preferences saved;
        saved.dsp.fftSize = 256;
        saved.dsp.overlapPercent = 0;
        const auto preferences = directory.filePath("preferences.json");
        rf::savePreferences(preferences, saved);
        rf::MainWindow window(nullptr, preferences);
        window.show();
        QTest::mouseMove(&window, QPoint(5, 5));
        window.openRecording(toneRecording(directory.filePath("tones.iq"), {8, 32, 64}));
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        auto *waveform = window.findChild<rf::WaveformPlot *>("waveformPlot");
        auto *waterfall = window.findChild<rf::WaterfallPlot *>("waterfallPlot");
        if (floating) {
            window.findChild<QDockWidget *>("waveformDock")->setFloating(true);
            QTest::qWait(20);
        }
        const auto waveformOriginal = waveform->grab().toImage();
        const auto waterfallOriginal = waterfall->grab().toImage();
        moveMouse(waveform, waveformRowPosition(*waveform, 1, 3));
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, std::uint64_t{256},
                                  10000);
        const int markerX = 76 + qRound((waveform->width() - 100) / 3.0);
        const auto waterfallPosition = rowPosition(*waterfall, 1, 3);
        QVERIFY(waveform->grab().toImage().pixelColor(markerX, 40) !=
                waveformOriginal.pixelColor(markerX, 40));
        QVERIFY(waterfall->grab().toImage().pixelColor(200, waterfallPosition.y()) !=
                waterfallOriginal.pixelColor(200, waterfallPosition.y()));
        QTest::mouseClick(waveform, Qt::LeftButton, Qt::NoModifier,
                          waveformRowPosition(*waveform, 2, 3));
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, std::uint64_t{512},
                                  10000);
        const auto spectrumFrozen = window.spectrumResult();
        const auto waveformFrozen = waveform->grab().toImage();
        const auto waterfallFrozen = waterfall->grab().toImage();
        moveMouse(waterfall, rowPosition(*waterfall, 0, 3));
        moveMouse(waveform, waveformRowPosition(*waveform, 0, 3));
        QTest::mouseClick(waterfall, Qt::RightButton, Qt::NoModifier,
                          rowPosition(*waterfall, 0, 3));
        QTest::mouseClick(waveform, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(waterfall, &leave);
        QApplication::sendEvent(waveform, &leave);
        QTest::qWait(50);
        QCOMPARE(window.spectrumResult(), spectrumFrozen);
        QCOMPARE(waveform->grab().toImage(), waveformFrozen);
        QCOMPARE(waterfall->grab().toImage(), waterfallFrozen);
        const auto screenshots = qEnvironmentVariable("RF_TEST_SCREENSHOT_DIR");
        if (!screenshots.isEmpty() && !floating) {
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(window.grab().save(screenshots + "/frame-freeze.png"));
        }
        // Either plot can release a freeze made in the other plot.
        QTest::mouseClick(waterfall, Qt::LeftButton, Qt::NoModifier,
                          rowPosition(*waterfall, 1, 3));
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, std::uint64_t{256},
                                  10000);
        moveMouse(waveform, waveformRowPosition(*waveform, 0, 3));
        QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{0});
        QTest::mouseClick(waterfall, Qt::LeftButton, Qt::NoModifier,
                          rowPosition(*waterfall, 0, 3));
        moveMouse(waveform, waveformRowPosition(*waveform, 2, 3));
        QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{0});
        QTest::mouseClick(waveform, Qt::LeftButton, Qt::NoModifier,
                          waveformRowPosition(*waveform, 2, 3));
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, std::uint64_t{512},
                                  10000);
        moveMouse(waterfall, rowPosition(*waterfall, 1, 3));
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, std::uint64_t{256},
                                  10000);
        QApplication::sendEvent(waterfall, &leave);
        QCOMPARE(waveform->grab().toImage(), waveformOriginal);
        QCOMPARE(waterfall->grab().toImage(), waterfallOriginal);
        // Freezing a new row also protects a still-pending FFT from later moves.
        QTest::mouseClick(waterfall, Qt::LeftButton, Qt::NoModifier,
                          rowPosition(*waterfall, 2, 3));
        moveMouse(waveform, waveformRowPosition(*waveform, 0, 3));
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, std::uint64_t{512},
                                  10000);
        window.findChild<QLineEdit *>("selectionStart")->setText("0");
        window.findChild<QLineEdit *>("selectionEnd")->setText("512");
        QTest::mouseClick(window.findChild<QPushButton *>("applySelection"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        moveMouse(waveform, waveformRowPosition(*waveform, 1, 2));
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, std::uint64_t{256},
                                  10000);
        QSignalSpy seeks(waveform, &rf::WaveformPlot::frameSelected);
        QTest::mouseDClick(waveform, Qt::LeftButton, Qt::NoModifier,
                           waveformRowPosition(*waveform, 0, 2));
        QCOMPARE(seeks.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
    }
    void sharedFrameBeyondWaveformPreview()
    {
        QTemporaryDir directory;
        QFile file(directory.filePath("long.iq"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(200000 * 4));
        file.close();
        rf::Preferences saved;
        saved.dsp.fftSize = 256;
        saved.dsp.overlapPercent = 0;
        const auto preferences = directory.filePath("preferences.json");
        rf::savePreferences(preferences, saved);
        rf::RecordingDescriptor descriptor;
        descriptor.path = file.fileName();
        rf::MainWindow window(nullptr, preferences);
        window.show();
        QTest::mouseMove(&window, QPoint(5, 5));
        window.openRecording(descriptor);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        auto *waterfall = window.findChild<rf::WaterfallPlot *>("waterfallPlot");
        auto *waveform = window.findChild<rf::WaveformPlot *>("waveformPlot");
        const auto preview = window.previewResult();
        const auto lastFrame = preview->rowStarts.back();
        QVERIFY(lastFrame >= preview->waveform.range.end);
        QTest::mouseClick(waterfall, Qt::LeftButton, Qt::NoModifier,
                          rowPosition(*waterfall, preview->rowStarts.size() - 1,
                                      preview->rowStarts.size()));
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, lastFrame, 10000);
        QSignalSpy hovered(waveform, &rf::WaveformPlot::frameHovered);
        QSignalSpy cursor(waveform, &rf::WaveformPlot::cursorChanged);
        QTest::mouseMove(waveform, QPoint(76, 60));
        QCOMPARE(hovered.last().at(0).toULongLong(), lastFrame);
        QVERIFY(cursor.last().at(0).toString().contains("click to unfreeze"));
        auto *exact = findAction(window, "Exact waveform over selected interval");
        QVERIFY(exact);
        exact->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy(), 10000);
        QTest::mouseMove(waveform, QPoint(77, 60));
        QTest::mouseMove(waveform, QPoint(76, 60));
        QCOMPARE(hovered.last().at(0).toULongLong(), std::uint64_t{0});
        QCOMPARE(window.spectrumResult()->spectrumStart, lastFrame); // Remains frozen.
        QTest::mouseClick(waterfall, Qt::LeftButton, Qt::NoModifier,
                          rowPosition(*waterfall, 0, preview->rowStarts.size()));
        QTest::mouseMove(waterfall, rowPosition(*waterfall, preview->rowStarts.size() - 1,
                                              preview->rowStarts.size()));
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, lastFrame, 10000);
        QTest::mouseMove(waveform, QPoint(77, 60));
        QTest::mouseMove(waveform, QPoint(76, 60));
        QCOMPARE(hovered.last().at(0).toULongLong(), std::uint64_t{0});
        QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{0});
    }
    void waterfallHoverBand_data()
    {
        QTest::addColumn<int>("row");
        QTest::newRow("first") << 0;
        QTest::newRow("middle") << 1;
        QTest::newRow("last") << 2;
    }
    void waterfallHoverBand()
    {
        QFETCH(int, row);
        rf::WaterfallPlot plot;
        plot.resize(600, 310);
        plot.show();
        auto result = std::make_shared<rf::PreviewResult>();
        result->range = {0, 1280};
        result->frequencies = {-1, 0, 1};
        result->columns = 3;
        result->rowStarts = {0, 256, 1024};
        result->waterfall.assign(9, 0.01);
        plot.setPreview(result, 256);
        QTest::mouseMove(&plot, QPoint(10, 10));
        const auto original = plot.grab().toImage();
        QSignalSpy hovered(&plot, &rf::WaterfallPlot::frameHovered);
        QTest::mouseMove(&plot, rowPosition(plot, row, 3));
        QCOMPARE(hovered.last().at(0).toULongLong(), result->rowStarts[row]);
        const auto highlighted = plot.grab().toImage();
        for (int y = 32; y < 272; ++y)
            QCOMPARE(highlighted.pixelColor(200, y) != original.pixelColor(200, y),
                     y >= 32 + row * 80 && y < 32 + (row + 1) * 80);
        QTest::mouseMove(&plot, QPoint(10, 10));
        QCOMPARE(plot.grab().toImage(), original);
        QTest::mouseMove(&plot, rowPosition(plot, row, 3));
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&plot, &leave);
        QCOMPARE(plot.grab().toImage(), original);
        QTest::mouseMove(&plot, QPoint(101, 32 + row * 80));
        QCOMPARE(hovered.last().at(0).toULongLong(), result->rowStarts[row]);
        plot.setPreview(result, 256);
        QCOMPARE(plot.grab().toImage(), original);
    }
    void waterfallSpectrumSynchronization()
    {
        QTemporaryDir directory;
        rf::Preferences saved;
        saved.dsp.fftSize = 256;
        saved.dsp.overlapPercent = 0;
        saved.view.absoluteFrequency = true;
        const auto preferences = directory.filePath("preferences.json");
        rf::savePreferences(preferences, saved);
        auto descriptor = toneRecording(directory.filePath("tones.iq"), {8, 32, 64});
        descriptor.captures = {{0, 1000, {}}, {256, {}, {}}, {512, 2000, {}}};
        rf::MainWindow window(nullptr, preferences);
        window.show();
        QTest::mouseMove(&window, QPoint(5, 5));
        QSignalSpy errors(&window, &rf::MainWindow::analysisError);
        window.openRecording(descriptor);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        const auto waterfall = window.previewResult();
        QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{0});
        const auto peakFrequency = [&window] {
            const auto result = window.spectrumResult();
            const auto peak = std::max_element(result->spectrum.power.begin(),
                                               result->spectrum.power.end());
            return result->frequencies[peak - result->spectrum.power.begin()];
        };
        QCOMPARE(peakFrequency(), 8.0);
        auto *plot = window.findChild<rf::WaterfallPlot *>("waterfallPlot");
        auto *spectrum = window.findChild<rf::SpectrumPlot *>("spectrumPlot");
        QSignalSpy ranges(spectrum, &rf::SpectrumPlot::frequencyRangeChanged);
        const QPointF zoomPosition(200, 60);
        QWheelEvent zoom(zoomPosition, spectrum->mapToGlobal(zoomPosition.toPoint()), {},
                         QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(spectrum, &zoom);
        QCOMPARE(ranges.count(), 1);
        QSignalSpy cursor(spectrum, &rf::SpectrumPlot::cursorChanged);
        const auto checkCursor = [&](double center) {
            QTest::mouseMove(spectrum, QPoint(76, 60));
            const auto &frequencies = window.spectrumResult()->frequencies;
            const double left = ranges.first().at(0).toDouble();
            const double expected = *std::lower_bound(frequencies.begin(), frequencies.end(), left) +
                                    center;
            return !cursor.isEmpty() && cursor.last().at(0).toString().contains(
                                           QString::number(expected, 'g', 17) + " Hz");
        };
        for (const auto row : {1, 2, 0}) {
            QTest::mouseMove(plot, rowPosition(*plot, row, 3));
            QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart,
                                      static_cast<std::uint64_t>(row * 256), 10000);
            QCOMPARE(peakFrequency(), row == 0 ? 8.0 : row == 1 ? 32.0 : 64.0);
            QVERIFY(checkCursor(row == 0 ? 1000 : row == 1 ? 0 : 2000));
            QCOMPARE(window.previewResult(), waterfall);
            QCOMPARE(window.selectedRange(), waterfall->range);
            QCOMPARE(ranges.count(), 1);
        }
        // Deliver rapid moves without servicing worker results between them.
        const auto move = [plot](int row) {
            const QPointF position = rowPosition(*plot, row, 3);
            QMouseEvent event(QEvent::MouseMove, position,
                              plot->mapToGlobal(position.toPoint()), Qt::NoButton, Qt::NoButton,
                              Qt::NoModifier);
            QApplication::sendEvent(plot, &event);
        };
        for (const auto row : {2, 1, 0, 1, 2})
            move(row);
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, std::uint64_t{512},
                                  10000);
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(plot, &leave);
        QTest::qWait(50);
        QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{512});
        QCOMPARE(peakFrequency(), 64.0);
        move(0);
        auto *average = findAction(window, "Average selected interval");
        QVERIFY(average);
        average->trigger();
        QVERIFY(window.isBusy());
        move(2);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() &&
                                    window.spectrumResult()->spectrumStart == 512,
                                10000);
        auto *averageDock = window.findChild<QDockWidget *>("averageDock");
        QVERIFY(averageDock->findChild<QLabel *>()->text().contains("Complete pass"));
        move(1);
        window.findChild<QLineEdit *>("selectionStart")->setText("256");
        window.findChild<QLineEdit *>("selectionEnd")->setText("768");
        QTest::mouseClick(window.findChild<QPushButton *>("applySelection"), Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{256});
        QTest::qWait(50);
        QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{256});
        auto *fft = window.findChild<QComboBox *>("fftSize");
        fft->setCurrentIndex(fft->findData(512));
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        QCOMPARE(window.spectrumResult()->spectrum.power.size(), std::size_t{0});
        QVERIFY(!window.spectrumResult()->spectrum.valid); // This window crosses the retune.
        window.openRecording(toneRecording(directory.filePath("new.iq"), {64, 32, 8}));
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{0});
        QCOMPARE(errors.count(), 0);
    }
    void waterfallOverviewAndInvalidFrames()
    {
        QTemporaryDir directory;
        rf::Preferences saved;
        saved.dsp.fftSize = 256;
        saved.dsp.overlapPercent = 0;
        const auto preferences = directory.filePath("preferences.json");
        rf::savePreferences(preferences, saved);
        std::vector<int> bins(160, 8);
        bins[158] = bins[159] = 64;
        auto descriptor = toneRecording(directory.filePath("overview.iq"), bins);
        descriptor.captures = {{0, {}, {}}, {128, {}, {}}, {256, {}, {}}};
        rf::MainWindow window(nullptr, preferences);
        window.show();
        QTest::mouseMove(&window, QPoint(5, 5));
        window.openRecording(descriptor);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        QVERIFY(window.previewResult()->sampled);
        QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{0});
        QVERIFY(!window.spectrumResult()->spectrum.valid);
        auto *plot = window.findChild<rf::WaterfallPlot *>("waterfallPlot");
        auto *overview = findAction(window, "Exact waterfall overview + average");
        QVERIFY(overview);
        for (int pass = 0; pass < 2; ++pass) {
            if (pass == 1) {
                overview->trigger();
                QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.previewResult(), 10000);
                QVERIFY(window.previewResult()->aggregated);
                QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{0});
            }
            const auto preview = window.previewResult();
            QTest::mouseMove(plot, rowPosition(*plot, preview->rowStarts.size() - 1,
                                              preview->rowStarts.size()));
            QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart,
                                      preview->rowStarts.back(), 10000);
            const auto result = window.spectrumResult();
            QVERIFY(result->spectrum.valid);
            const auto peak = std::max_element(result->spectrum.power.begin(),
                                               result->spectrum.power.end());
            QCOMPARE(result->frequencies[peak - result->spectrum.power.begin()], 64.0);
            QCOMPARE(window.previewResult(), preview);
            const auto screenshots = qEnvironmentVariable("RF_TEST_SCREENSHOT_DIR");
            if (!screenshots.isEmpty() && pass == 0) {
                QVERIFY(QDir().mkpath(screenshots));
                QVERIFY(window.grab().save(screenshots + "/waterfall-hover.png"));
            }
            QTest::mouseMove(plot, rowPosition(*plot, 0, preview->rowStarts.size()));
            QCOMPARE(window.spectrumResult()->spectrumStart, std::uint64_t{0});
            QVERIFY(!window.spectrumResult()->spectrum.valid);
            QVERIFY(window.spectrumResult()->spectrum.power.empty());
        }
    }
    void waterfallSpectrumExports()
    {
        QTemporaryDir directory;
        rf::Preferences saved;
        saved.dsp.fftSize = 256;
        saved.dsp.overlapPercent = 0;
        const auto preferences = directory.filePath("preferences.json");
        rf::savePreferences(preferences, saved);
        rf::MainWindow window(nullptr, preferences);
        window.show();
        QTest::mouseMove(&window, QPoint(5, 5));
        window.openRecording(toneRecording(directory.filePath("tones.iq"), {8, 32, 64}));
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && window.spectrumResult(), 10000);
        auto *plot = window.findChild<rf::WaterfallPlot *>("waterfallPlot");
        QTest::mouseMove(plot, rowPosition(*plot, 2, 3));
        QTRY_COMPARE_WITH_TIMEOUT(window.spectrumResult()->spectrumStart, std::uint64_t{512},
                                  10000);
        const auto selected = window.spectrumResult();
        const auto nativeDialogs = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        const auto restoreDialogs = qScopeGuard([nativeDialogs] {
            QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, nativeDialogs);
        });
        const auto csvPath = directory.filePath("spectrum.csv");
        bool csvDialogHandled = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QFileDialog *>();
            if (!dialog)
                return;
            dialog->selectFile(csvPath);
            csvDialogHandled = true;
            static_cast<QDialog *>(dialog)->accept();
        });
        auto *csv = findAction(window, "Export current spectrum CSV…");
        QVERIFY(csv);
        csv->trigger();
        QVERIFY(csvDialogHandled);
        // A new hover must not cancel the export or change its captured bins.
        const QPointF position = rowPosition(*plot, 1, 3);
        QMouseEvent move(QEvent::MouseMove, position, plot->mapToGlobal(position.toPoint()),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(plot, &move);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && QFileInfo::exists(csvPath) &&
                                    window.spectrumResult()->spectrumStart == 256,
                                10000);
        QFile file(csvPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto metadata = QJsonDocument::fromJson(file.readLine().mid(2)).object();
        QCOMPARE(metadata["start"].toString(), QString("512"));
        QCOMPARE(metadata["end"].toString(), QString("768"));
        file.readLine(); // Column headings.
        for (std::size_t bin = 0; bin < selected->spectrum.power.size(); ++bin) {
            const auto values = file.readLine().trimmed().split(',');
            QCOMPARE(values.size(), 3);
            QCOMPARE(values[0].toDouble(), selected->frequencies[bin]);
            QCOMPARE(values[1].toDouble(), selected->spectrum.power[bin]);
        }
        QVERIFY(file.atEnd());
        const auto pngPath = directory.filePath("spectrum.png");
        bool pngDialogHandled = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = window.findChild<QInputDialog *>();
            if (!dialog)
                return;
            dialog->setTextValue("Spectrum");
            QTimer::singleShot(0, &window, [&] {
                auto *fileDialog = window.findChild<QFileDialog *>();
                if (!fileDialog)
                    return;
                fileDialog->selectFile(pngPath);
                pngDialogHandled = true;
                static_cast<QDialog *>(fileDialog)->accept();
            });
            dialog->accept();
        });
        auto *png = findAction(window, "Export plot PNG…");
        QVERIFY(png);
        png->trigger();
        QVERIFY(pngDialogHandled);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isBusy() && QFileInfo::exists(pngPath), 10000);
        const QImage image(pngPath);
        QVERIFY(!image.isNull());
        const auto pngMetadata =
            QJsonDocument::fromJson(image.text("CroccoSpectrum").toUtf8()).object();
        QCOMPARE(pngMetadata["start"].toString(), QString("256"));
        QCOMPARE(pngMetadata["end"].toString(), QString("512"));
        QVERIFY(!pngMetadata["sampled_preview"].toBool());
        QVERIFY(!pngMetadata["exact_overview"].toBool());
    }
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
