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
    std::vector<qint64> latencies;
    QElapsedTimer elapsed;
    elapsed.start();
    std::uint64_t cycles = 0;
    const int lengths[] = {4096, 16384, 65536, 1024};
    while (elapsed.elapsed() < static_cast<qint64>(seconds) * 1000) {
        const auto length = lengths[cycles % 4];
        fft->setCurrentIndex(fft->findData(length));
        const auto begin = (cycles % 12) * 1'000'000;
        start->setText(QString::number(begin));
        end->setText(QString::number(begin + 262144));
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
    struct rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    const QJsonObject result{{"seconds", elapsed.elapsed() / 1000.0},
                             {"cycles", QString::number(cycles)},
                             {"first_preview_ms", firstMilliseconds},
                             {"seek_p95_ms", latencies[(latencies.size() - 1) * 95 / 100]},
                             {"peak_rss_kib", static_cast<qint64>(usage.ru_maxrss)},
                             {"fixture_bytes", "400000000"}};
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    return 0;
}
