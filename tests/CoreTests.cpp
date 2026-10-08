#include "app/Session.h"
#include "recording/Metadata.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <bit>
#include <cmath>
#include <numbers>
#include <random>
#include <tuple>

namespace
{
QString writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Fixture write failed");
    return path;
}
std::vector<std::complex<double>> decode(const QByteArray &bytes, rf::SampleFormat format)
{
    return rf::decodeSamples({reinterpret_cast<const std::byte *>(bytes.constData()),
                              static_cast<std::size_t>(bytes.size())},
                             format);
}
} // namespace

class CoreTests : public QObject
{
    Q_OBJECT
  private slots:
    void trimmedExportPreservesCaptureSemantics()
    {
        QTemporaryDir directory;
        rf::RecordingDescriptor descriptor;
        descriptor.path =
            writeFile(directory.filePath("captures.iq"), QByteArray(400 * 4, char{0}));
        descriptor.captures = {{0, 915000000, {}}, {100, {}, {}}, {200, 930000000, {}}};
        descriptor.annotations = {{20, 80, "overlapping note"}};
        rf::Recording original(descriptor);
        const auto output = directory.filePath("trimmed.iq");
        rf::exportSamples(original, {50, 250}, output, {});
        const auto trimmed = rf::recordingFromJson(
            rf::readJsonObject(output + ".rfmeta.json")["recording"].toObject());
        QVERIFY(!trimmed.centerFrequency.has_value());
        QCOMPARE(trimmed.frequencyAt(0), 915000000.0);
        QVERIFY(!trimmed.hasFrequencyAt(50));
        QCOMPARE(trimmed.frequencyAt(150), 930000000.0);
        QCOMPARE(trimmed.annotations.size(), std::size_t{1});
        QCOMPARE(trimmed.annotations[0].start, std::uint64_t{0});
        QCOMPARE(trimmed.annotations[0].count, std::uint64_t{50});
        rf::Recording reopened(trimmed);
        QCOMPARE(reopened.frameCount(), std::uint64_t{200});
    }
    void extremeFloatingPsd()
    {
        rf::DspSettings settings;
        settings.fftSize = 256;
        settings.window = rf::Window::Rectangular;
        settings.scale = rf::PowerScale::Density;
        for (const auto [amplitude, rate, expected] :
             {std::tuple{1e-160, 1e-200, 2.56e-118}, std::tuple{1e154, 1e200, 2.56e110}}) {
            rf::SpectrumEngine engine(rf::SampleKind::Complex, rate, settings);
            const auto result =
                engine.calculate(std::vector<std::complex<double>>(256, {amplitude, 0}));
            QVERIFY(result.valid);
            QVERIFY(std::abs(result.power[128] / expected - 1) < 1e-12);
        }
    }
    void integerFixtures_data()
    {
        QTest::addColumn<int>("bits");
        QTest::addColumn<QByteArray>("hex");
        QTest::newRow("int8") << 8 << QByteArray("80007f40");
        QTest::newRow("int16") << 16 << QByteArray("00800000ff7f0040");
        QTest::newRow("int24") << 24 << QByteArray("000080000000ffff7f000040");
        QTest::newRow("int32") << 32 << QByteArray("0000008000000000ffffff7f00000040");
        QTest::newRow("int64")
            << 64 << QByteArray("00000000000000800000000000000000ffffffffffffff7f0000000000000040");
    }
    void integerFixtures()
    {
        QFETCH(int, bits);
        QFETCH(QByteArray, hex);
        rf::SampleFormat format;
        format.kind = rf::SampleKind::Real;
        format.bits = bits;
        auto bytes = QByteArray::fromHex(hex);
        const auto result = decode(bytes, format);
        QCOMPARE(result.size(), std::size_t{4});
        QCOMPARE(result[0].real(), -1.0);
        QCOMPARE(result[1].real(), 0.0);
        QCOMPARE(result[2].real(), 1.0 - std::ldexp(1.0, 1 - bits));
        QCOMPARE(result[3].real(), 0.5);
        for (int index = 0; index < bytes.size(); index += bits / 8)
            std::reverse(bytes.begin() + index, bytes.begin() + index + bits / 8);
        format.byteOrder = rf::ByteOrder::BigEndian;
        QCOMPARE(decode(bytes, format), result);
        format.encoding = rf::Encoding::UnsignedInteger;
        const auto unsignedResult = decode(bytes, format);
        QCOMPARE(unsignedResult[0].real(), 0.0);
        QCOMPARE(unsignedResult[1].real(), -1.0);
        QCOMPARE(unsignedResult[3].real(), -0.5);
    }
    void componentOrderAndFloat()
    {
        rf::SampleFormat format;
        format.encoding = rf::Encoding::FloatingPoint;
        format.bits = 16;
        const auto samples = decode(QByteArray::fromHex("003c00b800bc0038"), format);
        QCOMPARE(samples[0], (std::complex<double>{1, -0.5}));
        QCOMPARE(samples[1], (std::complex<double>{-1, 0.5}));
        format.componentOrder = rf::ComponentOrder::QI;
        QCOMPARE(decode(QByteArray::fromHex("003c00b8"), format)[0],
                 (std::complex<double>{-0.5, 1}));
        format.kind = rf::SampleKind::Real;
        const auto special = decode(QByteArray::fromHex("01000080007c007e"), format);
        QCOMPARE(special[0].real(), std::ldexp(1.0, -24));
        QVERIFY(std::signbit(special[1].real()));
        QVERIFY(std::isinf(special[2].real()));
        QVERIFY(std::isnan(special[3].real()));
        format.bits = 32;
        QCOMPARE(decode(QByteArray::fromHex("0000803f000000bf"), format)[1].real(), -0.5);
        format.bits = 64;
        QCOMPARE(decode(QByteArray::fromHex("000000000000f03f"), format)[0].real(), 1.0);
        format.bits = 64;
        format.encoding = rf::Encoding::SignedInteger;
        QCOMPARE(decode(QByteArray::fromHex("ffffffffffffffff"), format)[0].real(),
                 -std::ldexp(1.0, -63));
        format.encoding = rf::Encoding::UnsignedInteger;
        const auto centered =
            decode(QByteArray::fromHex("ffffffffffffff7f0100000000000080"), format);
        QCOMPARE(centered[0].real(), -std::ldexp(1.0, -63));
        QCOMPARE(centered[1].real(), std::ldexp(1.0, -63));
    }
    void largeOffsetsAndChanges()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QFile file(directory.filePath("large.iq"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const qint64 size = 100LL * 1024 * 1024 * 1024;
        QVERIFY(file.resize(size));
        QVERIFY(file.seek(size - 4));
        QCOMPARE(file.write(QByteArray::fromHex("004000e0")), qint64{4});
        file.close();
        rf::RecordingDescriptor descriptor;
        descriptor.path = file.fileName();
        rf::Recording recording(descriptor);
        QCOMPARE(recording.frameCount(), static_cast<std::uint64_t>(size / 4));
        QCOMPARE(recording.read({recording.frameCount() - 1, recording.frameCount()})[0],
                 (std::complex<double>{0.5, -0.25}));
        QVERIFY_EXCEPTION_THROWN((void)recording.read({0, 10'000'000}), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(
            (void)recording.read({recording.frameCount(), recording.frameCount() + 1}),
            std::runtime_error);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.resize(4));
        file.close();
        QVERIFY_EXCEPTION_THROWN((void)recording.read({0, 1}), std::runtime_error);
    }
    void partialFramesAndJsonPrecision()
    {
        QTemporaryDir directory;
        rf::RecordingDescriptor descriptor;
        descriptor.path =
            writeFile(directory.filePath("partial.iq"), QByteArray::fromHex("004000e000"));
        QVERIFY_EXCEPTION_THROWN(rf::Recording recording(descriptor), std::runtime_error);
        descriptor.dataBytes = 4;
        rf::Recording recording(descriptor);
        QCOMPARE(recording.frameCount(), std::uint64_t{1});
        descriptor.dataOffset = 9'007'199'254'740'993ULL;
        const auto restored = rf::recordingFromJson(rf::recordingToJson(descriptor));
        QCOMPARE(restored.dataOffset, descriptor.dataOffset);
        const auto root = QJsonDocument::fromJson("{\"n\":9007199254740993}").object();
        QCOMPARE(rf::jsonUnsigned(root["n"], "n"), std::uint64_t{9'007'199'254'740'993ULL});
        QVERIFY_EXCEPTION_THROWN(rf::jsonUnsigned("18446744073709551616", "n"), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(rf::jsonUnsigned(1.5, "n"), std::runtime_error);
    }
    void partialRecordingSnapshot()
    {
        QTemporaryDir directory;
        const auto frame = QByteArray::fromHex("004000e0");
        rf::RecordingDescriptor descriptor;
        descriptor.path = writeFile(directory.filePath("downloading.iq"),
                                    QByteArray("hdr") + frame.repeated(3) + frame.left(3));
        descriptor.dataOffset = 3;
        descriptor.dataBytes = 100;
        descriptor.captures = {{0, 915000000, {}}, {2, 930000000, {}}, {3, 940000000, {}}};
        descriptor.annotations = {{1, 8, "crosses EOF"}, {3, 2, "not downloaded"}};
        QVERIFY_EXCEPTION_THROWN(rf::Recording strict(descriptor), std::runtime_error);
        descriptor.allowPartial = true;
        rf::Recording recording(descriptor);
        QCOMPARE(recording.frameCount(), std::uint64_t{3});
        QCOMPARE(recording.ignoredTrailingBytes(), std::uint64_t{3});
        QCOMPARE(recording.descriptor().dataBytes.value(), std::uint64_t{12});
        QCOMPARE(recording.read({0, 3}), (std::vector<std::complex<double>>(3, {0.5, -0.25})));
        QCOMPARE(recording.descriptor().captures.size(), std::size_t{2});
        QCOMPARE(recording.descriptor().annotations.size(), std::size_t{1});
        QCOMPARE(recording.descriptor().annotations[0].count, std::uint64_t{2});
        const auto identity = recording.identity();
        QFile download(descriptor.path);
        QVERIFY(download.open(QIODevice::Append));
        QCOMPARE(download.write(frame.right(1) + frame), qint64{5});
        download.close();
        recording.verifyUnchanged();
        QCOMPARE(recording.identity(), identity);
        QCOMPARE(recording.frameCount(), std::uint64_t{3});
        QCOMPARE(recording.read({2, 3})[0], (std::complex<double>{0.5, -0.25}));
        QVERIFY_EXCEPTION_THROWN((void)recording.read({0, 4}), std::runtime_error);
        rf::Recording reopened(descriptor);
        QCOMPARE(reopened.frameCount(), std::uint64_t{5});
        rf::Session session;
        session.recording = recording.descriptor();
        session.range = {0, recording.frameCount()};
        const auto sessionPath = directory.filePath("partial.rfsession.json");
        rf::saveSession(sessionPath, session);
        const auto restored = rf::readSession(sessionPath);
        QVERIFY(restored.recording.allowPartial);
        rf::Recording restoredSnapshot(restored.recording);
        QCOMPARE(restoredSnapshot.frameCount(), std::uint64_t{3});
        QCOMPARE(restoredSnapshot.descriptor().captures.size(), std::size_t{2});
        const auto output = directory.filePath("excerpt.iq");
        rf::exportSamples(recording, {0, 3}, output, {});
        QFile excerpt(output);
        QVERIFY(excerpt.open(QIODevice::ReadOnly));
        QCOMPARE(excerpt.readAll(), frame.repeated(3));
        const auto exported = rf::recordingFromJson(
            rf::readJsonObject(output + ".rfmeta.json")["recording"].toObject());
        QVERIFY(!exported.allowPartial);
        rf::Recording completed(exported);
        QCOMPARE(completed.frameCount(), std::uint64_t{3});
        QVERIFY(download.open(QIODevice::ReadWrite));
        QVERIFY(download.resize(7));
        download.close();
        QVERIFY_EXCEPTION_THROWN((void)recording.read({0, 1}), std::runtime_error);
    }
    void partialFramesAcrossFormats_data()
    {
        QTest::addColumn<bool>("complex");
        QTest::addColumn<int>("bits");
        QTest::newRow("real-int16") << false << 16;
        QTest::newRow("complex-int16") << true << 16;
        QTest::newRow("real-int24") << false << 24;
        QTest::newRow("complex-int24") << true << 24;
        QTest::newRow("complex-int64") << true << 64;
    }
    void partialFramesAcrossFormats()
    {
        QFETCH(bool, complex);
        QFETCH(int, bits);
        QTemporaryDir directory;
        rf::RecordingDescriptor descriptor;
        descriptor.format.kind = complex ? rf::SampleKind::Complex : rf::SampleKind::Real;
        descriptor.format.bits = bits;
        const auto frameBytes = descriptor.format.frameBytes();
        descriptor.path = writeFile(directory.filePath("partial.raw"),
                                    QByteArray(static_cast<qsizetype>(3 * frameBytes - 1), char{0}));
        QVERIFY_EXCEPTION_THROWN(rf::Recording strict(descriptor), std::runtime_error);
        descriptor.allowPartial = true;
        rf::Recording recording(descriptor);
        QCOMPARE(recording.frameCount(), std::uint64_t{2});
        QCOMPARE(recording.read({0, 2}), (std::vector<std::complex<double>>(2, {0, 0})));
        descriptor.dataBytes = frameBytes + 1;
        rf::Recording limited(descriptor);
        QCOMPARE(limited.frameCount(), std::uint64_t{1});
        descriptor.dataBytes = frameBytes - 1;
        QVERIFY_EXCEPTION_THROWN(rf::Recording empty(descriptor), std::runtime_error);
        descriptor.dataBytes.reset();
        descriptor.dataOffset = 3 * frameBytes;
        QVERIFY_EXCEPTION_THROWN(rf::Recording offsetPastEof(descriptor), std::runtime_error);
    }
    void partialReplacementAndStrictChanges()
    {
        QTemporaryDir directory;
        rf::RecordingDescriptor descriptor;
        descriptor.path = writeFile(directory.filePath("source.iq"), QByteArray(16, char{0}));
        rf::Recording strict(descriptor);
        descriptor.allowPartial = true;
        rf::Recording partial(descriptor);
        QFile download(descriptor.path);
        QVERIFY(download.open(QIODevice::Append));
        QCOMPARE(download.write(QByteArray(4, char{0})), qint64{4});
        download.close();
        QVERIFY_EXCEPTION_THROWN(strict.verifyUnchanged(), std::runtime_error);
        partial.verifyUnchanged();
        QVERIFY(QFile::rename(descriptor.path, descriptor.path + ".old"));
        writeFile(descriptor.path, QByteArray(20, char{0}));
        QVERIFY_EXCEPTION_THROWN(partial.verifyUnchanged(), std::runtime_error);
        descriptor.allowPartial = false;
        rf::Recording completed(descriptor);
        QVERIFY(download.open(QIODevice::ReadWrite));
        const auto modified = download.fileTime(QFileDevice::FileModificationTime);
        QCOMPARE(download.write(QByteArray(4, char{1})), qint64{4});
        QVERIFY(download.flush());
        // Same-size writes can share timestamps on CI filesystems. Advance the
        // modification time explicitly so this checks detection without a delay.
        QVERIFY(download.setFileTime(modified.addSecs(2), QFileDevice::FileModificationTime));
        download.close();
        QVERIFY_EXCEPTION_THROWN(completed.verifyUnchanged(), std::runtime_error);
    }
    void partialSigMf()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath("partial.sigmf-data"), QByteArray(1025, char{0}));
        const auto path = directory.filePath("partial.sigmf-meta");
        rf::writeJsonAtomic(path,
                            {{"global", QJsonObject{{"core:datatype", "ci16_le"},
                                                    {"core:sample_rate", 100000000}}},
                             {"captures", QJsonArray{QJsonObject{{"core:sample_start", 0}},
                                                     QJsonObject{{"core:sample_start", 512}}}},
                             {"annotations", QJsonArray{QJsonObject{{"core:sample_start", 200},
                                                                    {"core:sample_count", 100}}}}});
        auto descriptor = rf::readSigMf(path);
        QVERIFY_EXCEPTION_THROWN(rf::Recording strict(descriptor), std::runtime_error);
        descriptor.allowPartial = true;
        rf::Recording recording(descriptor);
        QCOMPARE(recording.frameCount(), std::uint64_t{256});
        QCOMPARE(recording.descriptor().captures.size(), std::size_t{1});
        QCOMPARE(recording.descriptor().annotations[0].count, std::uint64_t{56});
    }
    void spectralScalingAndSign()
    {
        constexpr int length = 1024;
        constexpr double rate = 8192;
        std::vector<std::complex<double>> samples(length);
        for (int index = 0; index < length; ++index)
            samples[index] = std::polar(0.5, 2 * std::numbers::pi * 128 * index / length);
        rf::DspSettings settings;
        settings.fftSize = length;
        settings.window = rf::Window::Rectangular;
        rf::SpectrumEngine complex(rf::SampleKind::Complex, rate, settings);
        const auto result = complex.calculate(samples);
        QVERIFY(result.valid);
        const auto peak = std::max_element(result.power.begin(), result.power.end());
        QCOMPARE(static_cast<int>(peak - result.power.begin()), length / 2 + 128);
        QVERIFY(std::abs(*peak - 0.25) < 1e-12);
        settings.conjugate = true;
        rf::SpectrumEngine inverted(rf::SampleKind::Complex, rate, settings);
        const auto inverse = inverted.calculate(samples);
        QCOMPARE(static_cast<int>(std::max_element(inverse.power.begin(), inverse.power.end()) -
                                  inverse.power.begin()),
                 length / 2 - 128);
        for (auto &sample : samples)
            sample = {sample.real() * 2, 0};
        settings.conjugate = false;
        rf::SpectrumEngine real(rf::SampleKind::Real, rate, settings);
        const auto realResult = real.calculate(samples);
        QVERIFY(std::abs(realResult.power[128] - 0.5) < 1e-12);
        std::fill(samples.begin(), samples.end(), std::complex<double>{1, 0});
        QCOMPARE(real.calculate(samples).power[0], 1.0);
        for (int index = 0; index < length; ++index)
            samples[index] = {index % 2 ? -1.0 : 1.0, 0};
        QCOMPARE(real.calculate(samples).power.back(), 1.0);
        samples[0] = {std::numeric_limits<double>::quiet_NaN(), 0};
        QVERIFY(!real.calculate(samples).valid);
        settings.fftSize = 1000;
        QVERIFY_EXCEPTION_THROWN(rf::SpectrumEngine invalid(rf::SampleKind::Real, rate, settings),
                                 std::runtime_error);
    }
    void averagingChunksAndCancellation()
    {
        QTemporaryDir directory;
        const auto path = writeFile(directory.filePath("tone.iq"),
                                    QByteArray::fromHex("00400000").repeated(600003));
        rf::RecordingDescriptor descriptor;
        descriptor.path = path;
        rf::Recording recording(descriptor);
        rf::DspSettings settings;
        settings.fftSize = 256;
        settings.window = rf::Window::Rectangular;
        const auto average = rf::analyzeAverage(recording, {0, recording.frameCount()}, settings,
                                                [] { return false; });
        QCOMPARE(average->validWindows, 1 + (recording.frameCount() - 256) / 128);
        QCOMPARE(average->averagePower[128], 0.25);
        QCOMPARE(average->maxPower[128], 0.25);
        QCOMPARE(average->trailingFrames, (recording.frameCount() - 256) % 128);
        int checks = 0;
        QVERIFY_EXCEPTION_THROWN(rf::analyzeAverage(recording, {0, recording.frameCount()},
                                                    settings, [&checks] { return ++checks > 3; }),
                                 rf::Cancelled);
        descriptor.captures = {{0, {}, {}}, {1000, {}, {}}};
        rf::Recording segmented(descriptor);
        const auto segmentAverage =
            rf::analyzeAverage(segmented, {0, 4096}, settings, [] { return false; });
        QVERIFY(segmentAverage->boundaryWindows > 0);
    }
    void waveformAndByteExport()
    {
        QTemporaryDir directory;
        const QByteArray bytes =
            QByteArray::fromHex("00400000").repeated(10000) + QByteArray::fromHex("ff7f0080");
        rf::RecordingDescriptor descriptor;
        descriptor.path = writeFile(directory.filePath("input.iq"), bytes);
        rf::Recording recording(descriptor);
        const auto waveform =
            rf::analyzeWaveform(recording, {0, recording.frameCount()}, [] { return false; });
        QVERIFY(waveform->complete);
        QVERIFY(waveform->points.back().maxI > 0.99);
        QCOMPARE(waveform->points.back().minQ, -1.0);
        const auto output = directory.filePath("output.iq");
        rf::exportSamples(recording, {9999, 10001}, output, [] { return false; });
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), bytes.right(8));
        QVERIFY_EXCEPTION_THROWN(rf::exportSamples(recording, {0, 1}, output, [] { return false; }),
                                 std::runtime_error);
        const auto cancelledPath = directory.filePath("cancelled.iq");
        QVERIFY_EXCEPTION_THROWN(
            rf::exportSamples(recording, {0, 10000}, cancelledPath, [] { return true; }),
            rf::Cancelled);
        QVERIFY(!QFileInfo::exists(cancelledPath));
        recording.verifyUnchanged();
    }
    void sessionAndSigMf()
    {
        QTemporaryDir directory;
        rf::Session session;
        session.recording.path = "source.iq";
        session.view.palette = "Baudline";
        session.range = {9007199254740993ULL, 9007199254745093ULL};
        session.bookmarks = {{session.range.begin, 4096, "Burst"}};
        const auto path = directory.filePath("session.json");
        rf::saveSession(path, session);
        const auto restored = rf::readSession(path);
        QCOMPARE(restored.range.begin, session.range.begin);
        QCOMPARE(restored.view.palette, QString("Baudline"));
        QCOMPARE(restored.bookmarks[0].start, session.range.begin);
        const auto data = writeFile(directory.filePath("signal.sigmf-data"),
                                    QByteArray::fromHex("00400000").repeated(2048));
        Q_UNUSED(data);
        rf::writeJsonAtomic(directory.filePath("signal.sigmf-meta"),
                            {{"global", QJsonObject{{"core:datatype", "ci16_le"},
                                                    {"core:sample_rate", 100000000},
                                                    {"core:version", "1.2.6"}}},
                             {"captures", QJsonArray{QJsonObject{{"core:sample_start", 0},
                                                                 {"core:frequency", 915000000}}}},
                             {"annotations", QJsonArray{}}});
        const auto descriptor = rf::readSigMf(directory.filePath("signal.sigmf-meta"));
        QCOMPARE(descriptor.sampleRate, 100000000.0);
        QCOMPARE(descriptor.frequencyAt(0), 915000000.0);
        rf::Recording recording(descriptor);
        QCOMPARE(recording.frameCount(), std::uint64_t{2048});
    }
    void exactOverviewPreservesSkippedBursts()
    {
        QTemporaryDir directory;
        // More windows than the preview row budget. A one-window burst must
        // survive the exact overview's temporal maximum aggregation.
        QByteArray data(1024 * 256 * 4, char{0});
        const auto burst = QByteArray::fromHex("00600000").repeated(256);
        data.replace(255 * 256 * 4, burst.size(), burst);
        rf::RecordingDescriptor descriptor;
        descriptor.path = writeFile(directory.filePath("burst.iq"), data);
        rf::Recording recording(descriptor);
        rf::DspSettings settings;
        settings.fftSize = 256;
        settings.overlapPercent = 0;
        settings.window = rf::Window::Rectangular;
        rf::PreviewResult overview;
        const auto average =
            rf::analyzeAverage(recording, {0, recording.frameCount()}, settings, {}, {}, &overview);
        QCOMPARE(average->validWindows, std::uint64_t{1024});
        QCOMPARE(average->maxPower[128], 0.5625);
        QCOMPARE(average->averagePower[128], 0.5625 / 1024);
        QVERIFY(overview.aggregated);
        QVERIFY(!overview.sampled);
        QCOMPARE(overview.rowStarts.size(), std::size_t{128});
        QCOMPARE(*std::max_element(overview.waterfall.begin(), overview.waterfall.end()), 0.5625);
        const auto preview =
            rf::analyzePreview(recording, {0, recording.frameCount()}, settings, {});
        QVERIFY(preview->sampled);
        QVERIFY(*std::max_element(preview->waterfall.begin(), preview->waterfall.end()) < 0.5625);
    }
    void exportTransactionsAndCancellation()
    {
        QTemporaryDir directory;
        std::vector<double> frequency(20000, 0), power(20000, 0.25);
        rf::RecordingDescriptor descriptor;
        descriptor.path = "fixture";
        const auto output = directory.filePath("measurement.csv");
        int checks = 0;
        QVERIFY_EXCEPTION_THROWN(rf::exportSpectrumCsv(descriptor, "identity", {0, 4096}, {},
                                                       frequency, power, "test", {}, output,
                                                       [&checks] { return ++checks > 2; }),
                                 rf::Cancelled);
        QVERIFY(!QFile::exists(output));
        rf::exportSpectrumCsv(descriptor, "identity", {0, 4096}, {}, frequency, power, "test", {},
                              output);
        QFile saved(output);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const auto contents = saved.readAll();
        QVERIFY(contents.startsWith("# {"));
        QVERIFY(contents.contains("baseband_frequency_hz"));
        QVERIFY_EXCEPTION_THROWN(rf::exportSpectrumCsv(descriptor, "identity", {0, 4096}, {},
                                                       frequency, power, "test", {}, output),
                                 std::runtime_error);
        saved.seek(0);
        QCOMPARE(saved.readAll(), contents);
        QCOMPARE(QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
    }
    void strictSettingsAndMetadata()
    {
        QTemporaryDir settingsDirectory;
        const auto settingsPath = settingsDirectory.filePath("settings.json");
        rf::writeJsonAtomic(settingsPath, {{"value", "preserved"}});
        QVERIFY_EXCEPTION_THROWN(
            rf::writeJsonAtomic(settingsPath, {{"value", QString(4 * 1024 * 1024, QChar('x'))}}),
            std::runtime_error);
        QCOMPARE(rf::readJsonObject(settingsPath)["value"].toString(), QString("preserved"));
        auto dsp = rf::dspToJson({});
        dsp["fft_size"] = 4096.5;
        QVERIFY_EXCEPTION_THROWN(rf::dspFromJson(dsp), std::runtime_error);
        dsp = rf::dspToJson({});
        dsp["remove_dc"] = "false";
        QVERIFY_EXCEPTION_THROWN(rf::dspFromJson(dsp), std::runtime_error);
        auto view = rf::viewToJson({});
        view["color_max"] = "0";
        QVERIFY_EXCEPTION_THROWN(rf::viewFromJson(view), std::runtime_error);
        rf::RecordingDescriptor descriptor;
        descriptor.path = "fixture";
        auto metadata = rf::recordingToJson(descriptor);
        metadata["float_full_scale"] = "1";
        QVERIFY_EXCEPTION_THROWN(rf::recordingFromJson(metadata), std::runtime_error);
        metadata = rf::recordingToJson(descriptor);
        metadata["captures"] = QJsonObject{};
        QVERIFY_EXCEPTION_THROWN(rf::recordingFromJson(metadata), std::runtime_error);
        metadata = rf::recordingToJson(descriptor);
        metadata["allow_partial"] = "false";
        QVERIFY_EXCEPTION_THROWN(rf::recordingFromJson(metadata), std::runtime_error);
        metadata.remove("allow_partial");
        QVERIFY(!rf::recordingFromJson(metadata).allowPartial);
        rf::Preferences preferences;
        preferences.importDefaults = descriptor;
        preferences.importDefaults.allowPartial = true;
        rf::savePreferences(settingsPath, preferences);
        QVERIFY(!rf::readPreferences(settingsPath).importDefaults.allowPartial);
        QTemporaryDir directory;
        descriptor.path = writeFile(directory.filePath("short.iq"), QByteArray(16, char{0}));
        descriptor.annotations = {{3, 2, "past EOF"}};
        QVERIFY_EXCEPTION_THROWN(rf::Recording invalid(descriptor), std::runtime_error);
        descriptor.annotations.clear();
        descriptor.startUtc = "2026-10-06T12:00:00.000Z";
        descriptor.sampleRate = 1000;
        descriptor.validate();
        QCOMPARE(descriptor.timestampAt(5), QString("2026-10-06T12:00:00.005Z"));
        descriptor.captures = {{10, {}, ""}};
        QVERIFY(descriptor.timestampAt(15).isEmpty());
        descriptor.startUtc = "2026-10-06T12:00:00";
        QVERIFY_EXCEPTION_THROWN(descriptor.validate(), std::runtime_error);
        // Adversarial short buffers cover every scalar decoder without asking
        // the filesystem to allocate memory based on a malformed descriptor.
        std::mt19937 random(42);
        for (const auto encoding : {rf::Encoding::SignedInteger, rf::Encoding::UnsignedInteger,
                                    rf::Encoding::FloatingPoint})
            for (const auto bits : {8, 16, 24, 32, 64}) {
                if (encoding == rf::Encoding::FloatingPoint && (bits == 8 || bits == 24))
                    continue;
                rf::SampleFormat format;
                format.encoding = encoding;
                format.bits = bits;
                for (int iteration = 0; iteration < 100; ++iteration) {
                    QByteArray bytes(static_cast<qsizetype>(format.frameBytes() * 17), char{0});
                    for (auto &byte : bytes)
                        byte = static_cast<char>(random() & 255);
                    QCOMPARE(decode(bytes, format).size(), std::size_t{17});
                    bytes.chop(1);
                    QVERIFY_EXCEPTION_THROWN(decode(bytes, format), std::runtime_error);
                }
            }
    }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "CoreTests.moc"
