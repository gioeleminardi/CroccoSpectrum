#include "ui/MainWindow.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <iostream>
#include <sys/resource.h>
#include <vector>

// Sustained-use test run separately from CTest. RF_SOAK_SECONDS controls
// duration in seconds (default 60). Temporary fixtures/preferences isolate
// each run from existing recordings and application settings.
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    bool validDuration = false;
    const auto seconds = qEnvironmentVariable("RF_SOAK_SECONDS", "60").toInt(&validDuration);
    if (!validDuration || seconds < 1 || seconds > 86400)
        return 2;
    QTemporaryDir directory;
    QFile file(directory.filePath("soak.iq"));
    if (!file.open(QIODevice::WriteOnly))
        return 2;
    const auto block = QByteArray::fromHex("00400000").repeated(1'000'000);
    for (int index = 0; index < 100; ++index)
        if (file.write(block) != block.size())
            return 2; // Dense 400 MB fixture.
    file.close();
    rf::MainWindow window(nullptr, directory.filePath("preferences.json"));
    window.show();
    QString error;
    QObject::connect(&window, &rf::MainWindow::analysisError, &window,
                     [&](QString text) { error = text; });
    const auto waitForResult = [&] {
        QElapsedTimer deadline;
        deadline.start();
        while (window.isBusy() && error.isEmpty() && deadline.elapsed() < 10000)
            QTest::qWait(1);
        return error.isEmpty() && !window.isBusy() && window.previewResult();
    };
    rf::RecordingDescriptor descriptor;
    descriptor.path = file.fileName();
    QElapsedTimer first;
    first.start();
    window.openRecording(descriptor);
    if (!waitForResult()) {
        std::cerr << error.toStdString();
        return 1;
    }
    const auto firstMilliseconds = first.elapsed();
    auto *minimap = window.findChild<rf::WaterfallMinimap *>("waterfallMinimap");
    auto *waterfall = window.findChild<rf::WaterfallPlot *>("waterfallPlot");
    if (!minimap || !waterfall)
        return 2;
    QElapsedTimer overviewDeadline;
    overviewDeadline.start();
    while ((!minimap->snapshot() || !minimap->snapshot()->complete) && error.isEmpty() &&
           overviewDeadline.elapsed() < 30000)
        QTest::qWait(1);
    if (!error.isEmpty() || !minimap->snapshot() || !minimap->snapshot()->complete)
        return 1;
    const auto minimapMilliseconds = first.elapsed();
    auto *fft = window.findChild<QComboBox *>("fftSize");
    auto *start = window.findChild<QLineEdit *>("selectionStart");
    auto *end = window.findChild<QLineEdit *>("selectionEnd");
    auto *select = window.findChild<QPushButton *>("applySelection");
    QAction *average = nullptr;
    for (auto *action : window.findChildren<QAction *>())
        if (action->text() == "Average entire recording")
            average = action;
    if (!average)
        return 2;
    auto *color = window.findChild<QDoubleSpinBox *>("colorMinimum");
    std::vector<qint64> latencies, viewportLatencies;
    QElapsedTimer elapsed;
    elapsed.start();
    std::uint64_t cycles = 0;
    int maximumFft = 0;
    const int lengths[] = {4096, 16384, 65536, 1024, 1048576};
    while (elapsed.elapsed() < static_cast<qint64>(seconds) * 1000) {
        const auto length = lengths[cycles % 5];
        maximumFft = std::max(maximumFft, length);
        fft->setCurrentIndex(fft->findData(length));
        const auto begin = (cycles % 12) * 1'000'000;
        start->setText(QString::number(begin));
        end->setText(QString::number(begin + static_cast<std::uint64_t>(std::max(262144, length))));
        QElapsedTimer latency;
        latency.start();
        select->click();
        if (!waitForResult()) {
            std::cerr << error.toStdString();
            return 1;
        }
        latencies.push_back(latency.elapsed());
        if (window.previewResult()->range.begin != begin || window.dspSettings().fftSize != length)
            return 1;
        const auto before = window.previewResult();
        color->setValue(cycles % 2 ? -90 : -100);
        if (window.previewResult() != before)
            return 1; // View edits cannot mutate DSP results.
        // Navigate independently while the current DSP's whole-recording scan
        // is running; require detail for the final viewport and an unchanged
        // analysis interval. Include the maximum FFT in the mixed workload.
        const auto selectedRange = window.selectedRange();
        const auto map = minimap->mapRect();
        QElapsedTimer viewportLatency;
        viewportLatency.start();
        QTest::mouseClick(minimap, Qt::LeftButton, Qt::NoModifier,
                          QPoint(48, qRound(map.top() + map.height() * 0.75)));
        while (error.isEmpty() && viewportLatency.elapsed() < 10000 &&
               (!waterfall->snapshot() || waterfall->snapshot()->range != waterfall->viewport()))
            QTest::qWait(1);
        if (!error.isEmpty() || !waterfall->snapshot() ||
            waterfall->snapshot()->range != waterfall->viewport() ||
            window.selectedRange() != selectedRange || minimap->viewport() != waterfall->viewport())
            return 1;
        viewportLatencies.push_back(viewportLatency.elapsed());
        if (cycles % 8 == 0) {
            average->trigger();
            QTest::qWait(2);
            // Replace a running full-pass request, then require a correct
            // latest preview. This also exercises the populated cache path.
            select->click();
            if (!waitForResult())
                return 1;
        }
        ++cycles;
    }
    window.close();
    std::sort(latencies.begin(), latencies.end());
    std::sort(viewportLatencies.begin(), viewportLatencies.end());
    struct rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const QJsonObject result{{"seconds", elapsed.elapsed() / 1000.0},
                             {"cycles", QString::number(cycles)},
                             {"first_preview_ms", firstMilliseconds},
                             {"seek_p95_ms", latencies[(latencies.size() - 1) * 95 / 100]},
                             {"minimap_complete_ms", minimapMilliseconds},
                             {"viewport_p95_ms", viewportLatencies[(viewportLatencies.size() - 1) * 95 / 100]},
                             {"maximum_fft", maximumFft},
                             {"peak_rss_kib", static_cast<qint64>(usage.ru_maxrss)},
                             {"fixture_bytes", "400000000"}};
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    return 0;
}
