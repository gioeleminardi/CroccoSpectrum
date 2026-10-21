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
#include <thread>
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
    void spectralPeakTones_data()
    {
        QTest::addColumn<int>("window");
        QTest::addColumn<bool>("real");
        QTest::addColumn<bool>("density");
        QTest::addColumn<double>("offset");
        for (int window = 0; window < 4; ++window)
            for (bool real : {false, true})
                for (bool density : {false, true})
                    for (double offset : {0.0, 0.23, 0.5})
                        QTest::newRow(qPrintable(QString("%1-%2-%3-%4")
                            .arg(window).arg(real).arg(density).arg(offset)))
                            << window << real << density << offset;
    }
    void spectralPeakTones()
    {
        QFETCH(int, window);
        QFETCH(bool, real);
        QFETCH(bool, density);
        QFETCH(double, offset);
        rf::DspSettings settings;
        settings.fftSize = 1024;
        settings.window = static_cast<rf::Window>(window);
        settings.scale = density ? rf::PowerScale::Density : rf::PowerScale::Spectrum;
        // Sub-Hz bins exercise estimates that whole-Hz UI formatting would hide.
        const double rate = 128;
        const double spacing = rate / settings.fftSize;
        const double tone = (real ? 100 + offset : -100 + offset) * spacing;
        std::vector<std::complex<double>> samples(1024);
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const double phase = 2 * std::numbers::pi * tone * static_cast<double>(index) / rate;
            samples[index] = {0.5 * std::cos(phase), real ? 0 : 0.5 * std::sin(phase)};
        }
        rf::SpectrumEngine engine(real ? rf::SampleKind::Real : rf::SampleKind::Complex,
                                  rate, settings);
        const auto spectrum = engine.calculate(samples);
        const auto frequencies = engine.frequencies();
        const auto peaks = rf::findSpectralPeaks(frequencies, spectrum.power,
                                                frequencies.front(), frequencies.back(),
                                                settings.window);
        QVERIFY(!peaks.empty());
        const auto &peak = peaks.front();
        QVERIFY(std::abs(peak.frequencyHz - tone) <= 0.501 * spacing);
        QCOMPARE(peak.power, spectrum.power[peak.bin]);
        QVERIFY2(peak.estimatedHz.has_value(), qPrintable(peak.limitation));
        const double tolerance = settings.window == rf::Window::Rectangular ? 0.18 : 0.025;
        QVERIFY2(std::abs(*peak.estimatedHz - tone) < tolerance * spacing,
                 qPrintable(QString("estimate %1, tone %2").arg(*peak.estimatedHz, 0, 'g', 15)
                                                               .arg(tone, 0, 'g', 15)));
        if (offset == 0.23)
            QVERIFY(std::abs(*peak.estimatedHz - tone) < std::abs(peak.frequencyHz - tone));
    }
    void spectralPeakLimits()
    {
        std::vector<double> frequencies(1024), powers(1024, 1);
        for (std::size_t bin = 0; bin < frequencies.size(); ++bin)
            frequencies[bin] = static_cast<double>(bin);
        powers[0] = 100;
        powers[100] = 1000;
        powers[103] = 500;
        powers[200] = 4;
        powers[300] = powers[301] = 200;
        powers[400] = powers[401] = powers[402] = 100;
        powers[500] = std::numeric_limits<double>::quiet_NaN();
        powers[600] = std::numeric_limits<double>::infinity();
        powers[1023] = 100;
        const auto peaks = rf::findSpectralPeaks(frequencies, powers, 0, 1023, rf::Window::Hann);
        QCOMPARE(peaks.size(), std::size_t{6});
        QCOMPARE(peaks[0].bin, std::size_t{100});
        QVERIFY(!peaks[0].estimatedHz);
        QVERIFY(peaks[0].limitation.contains("Nearby"));
        QVERIFY(!peaks[1].estimatedHz);
        QCOMPARE(peaks[2].bin, std::size_t{300});
        QVERIFY(peaks[2].estimatedHz);
        QCOMPARE(*peaks[2].estimatedHz, 300.5);
        QVERIFY(!peaks.back().estimatedHz);
        QVERIFY(peaks.back().limitation.contains("Weak"));
        QCOMPARE(peaks[3].limitation, QString("FFT boundary"));
        QCOMPARE(peaks[4].limitation, QString("FFT boundary"));
        const auto selected = rf::findSpectralPeaks(frequencies, powers, 101, 110, rf::Window::Hann);
        QCOMPARE(selected.size(), std::size_t{1});
        QCOMPARE(selected[0].bin, std::size_t{103});
        QVERIFY(selected[0].limitation.contains("Nearby")); // Peak 100 is outside the range.
        QVERIFY(rf::findSpectralPeaks(frequencies, powers, 101, 102, rf::Window::Hann).empty());
        QVERIFY(rf::findSpectralPeaks(frequencies, powers, 800, 900, rf::Window::Hann).empty());
        QVERIFY(rf::findSpectralPeaks(frequencies, {}, 0, 1000, rf::Window::Hann).empty());
        QVERIFY(rf::findSpectralPeaks(frequencies, powers, 20, 10, rf::Window::Hann).empty());
        std::fill(powers.begin(), powers.end(), 0);
        QVERIFY(rf::findSpectralPeaks(frequencies, powers, 0, 1023, rf::Window::Hann).empty());
        powers[500] = 1;
        const auto isolated = rf::findSpectralPeaks(frequencies, powers, 490, 510, rf::Window::Hann);
        QCOMPARE(isolated.size(), std::size_t{1});
        QVERIFY(!isolated[0].estimatedHz);
        QVERIFY(isolated[0].limitation.contains("neighbors"));
        powers[500] = 0;
        for (std::size_t bin = 10; bin < 1000; bin += 10)
            powers[bin] = static_cast<double>(bin);
        const auto bounded = rf::findSpectralPeaks(frequencies, powers, 0, 1023, rf::Window::Hann);
        QCOMPARE(bounded.size(), std::size_t{12});
        QCOMPARE(bounded.front().bin, std::size_t{990});
    }
    void spectralPeakNoiseAndCarriers()
    {
        rf::DspSettings settings;
        settings.fftSize = 4096;
        rf::SpectrumEngine engine(rf::SampleKind::Complex, 4096, settings);
        std::mt19937 random(25);
        std::normal_distribution<double> noise(0, 0.01);
        std::vector<std::complex<double>> samples(4096);
        for (auto &sample : samples)
            sample = {noise(random), noise(random)};
        const auto frequencies = engine.frequencies();
        auto spectrum = engine.calculate(samples);
        const auto noisePeaks = rf::findSpectralPeaks(frequencies, spectrum.power, -2048, 2047,
                                                     settings.window);
        QCOMPARE(noisePeaks.size(), std::size_t{12});
        for (const auto &peak : noisePeaks)
            QVERIFY(!peak.estimatedHz);
        for (double separation : {3.0, 10.0, 0.25}) {
            for (std::size_t index = 0; index < samples.size(); ++index) {
                const double phase = 2 * std::numbers::pi * static_cast<double>(index) / 4096;
                samples[index] = std::polar(0.5, phase * 100.23) +
                                 std::polar(0.4, phase * (100.23 + separation));
            }
            spectrum = engine.calculate(samples);
            const auto peaks = rf::findSpectralPeaks(frequencies, spectrum.power, 80, 130,
                                                     settings.window);
            QVERIFY(!peaks.empty());
            if (separation == 3) {
                QVERIFY(peaks.size() >= 2);
                QVERIFY(peaks[0].limitation.contains("Nearby"));
                QVERIFY(peaks[1].limitation.contains("Nearby"));
            } else if (separation == 10) {
                QVERIFY(peaks.size() >= 2);
                QVERIFY(peaks[0].estimatedHz && peaks[1].estimatedHz);
                QVERIFY(std::abs(*peaks[0].estimatedHz - 100.23) < 0.03);
                QVERIFY(std::abs(*peaks[1].estimatedHz - 110.23) < 0.03);
            } else {
                // A merged maximum cannot establish the number of carriers.
                QCOMPARE(peaks.front().bin, std::size_t{2148});
            }
        }
    }
    void occupiedBandwidth()
    {
        const std::vector<double> frequency{-30, -20, -10, 0, 10, 20, 30};
        const auto check = [&](std::vector<double> masses, std::size_t first, std::size_t last,
                               double lower, double upper) {
            for (const auto scale : {rf::PowerScale::Spectrum, rf::PowerScale::Density}) {
                auto powers = masses;
                for (auto &power : powers)
                    power *= scale == rf::PowerScale::Spectrum ? 2.0 : 0.1;
                for (bool reverse : {false, true}) {
                    const auto result = rf::occupiedBandwidth99(frequency, powers, scale, 20,
                        reverse ? last : first, reverse ? first : last);
                    QVERIFY(result);
                    QVERIFY(std::abs(result->lowerHz - lower) < 1e-12);
                    QVERIFY(std::abs(result->upperHz - upper) < 1e-12);
                    QVERIFY(std::abs(result->bandwidthHz - (upper - lower)) < 1e-12);
                }
            }
        };
        check({1, 1, 1, 1, 1, 1, 1}, 0, 6, -34.65, 34.65); // Flat density: 99% of 70 Hz.
        check({0, 0, 0, 1, 0, 0, 0}, 0, 6, -4.95, 4.95); // One bin retains finite width.
        check({1000, 1, 0, 0, 0, 3, 1000}, 1, 5, -24.8, 25 - 0.2 / 3); // Selected only.
        check({1, 0, 198, 0, 1, 0, 0}, 0, 6, -25, 5); // Exact tail ties keep outer edges.
        check({1, 0, 0, 0, 0, 0, 1}, 0, 0, -34.95, -25.05); // Endpoint cell, no clipping.
        check({1, 0, 0, 0, 0, 0, 1}, 6, 6, 25.05, 34.95);
        for (auto invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity()}) {
            std::vector<double> powers(7, 0);
            powers[3] = invalid;
            QVERIFY(!rf::occupiedBandwidth99(frequency, powers, rf::PowerScale::Density, 20, 0, 6));
        }
        const std::vector<double> powers(7, 1);
        QVERIFY(!rf::occupiedBandwidth99(frequency, powers, rf::PowerScale::Spectrum, 0, 0, 6));
        QVERIFY(!rf::occupiedBandwidth99(frequency, powers, rf::PowerScale::Density, 20, 0, 7));
        QVERIFY(!rf::occupiedBandwidth99(frequency, {}, rf::PowerScale::Density, 20, 0, 6));
    }
    void integratedBandAndSnr()
    {
        const std::vector<double> frequency{0, 10, 20, 30, 40, 50};
        const std::vector<double> power{1, 1, 3, 4, 5, 0};
        const auto band = rf::integrateBand(frequency, power, rf::PowerScale::Spectrum, 20, 4, 2);
        QVERIFY(band);
        QCOMPARE(band->lowerHz, 20.0);
        QCOMPARE(band->upperHz, 40.0);
        QCOMPARE(band->bandwidthHz, 30.0);
        QCOMPARE(band->power, 6.0); // Sum 12 * spacing/ENBW = 6.
        const auto single = rf::integrateBand(frequency, power, rf::PowerScale::Density, 0, 2, 2);
        QVERIFY(single);
        QCOMPARE(single->bandwidthHz, 10.0);
        QCOMPARE(single->power, 30.0);
        const auto measured = rf::measureBands(frequency, power, rf::PowerScale::Spectrum, 20,
                                                std::pair{2U, 4U}, std::pair{2U, 4U},
                                                std::pair{0U, 1U});
        const auto snr = measured["snr"].toObject();
        QVERIFY(snr["available"].toBool());
        QCOMPARE(snr["noise_in_signal_band_linear"].toDouble(), 1.5);
        QCOMPARE(snr["net_signal_linear"].toDouble(), 4.5);
        QVERIFY(std::abs(snr["snr_db"].toDouble() - 10 * std::log10(3.0)) < 1e-12);
        QVERIFY(!rf::integrateBand(frequency, power, rf::PowerScale::Spectrum, 0, 0, 1));
        QVERIFY(!rf::integrateBand(frequency, power, rf::PowerScale::Density, 20, 0, 9));
        auto invalid = power;
        invalid[2] = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(!rf::integrateBand(frequency, invalid, rf::PowerScale::Density, 20, 2, 3));
        invalid[2] = -1;
        QVERIFY(!rf::integrateBand(frequency, invalid, rf::PowerScale::Density, 20, 2, 3));
        const auto overlap = rf::measureBands(frequency, power, rf::PowerScale::Density, 20,
                                               {}, std::pair{1U, 2U}, std::pair{0U, 1U});
        QVERIFY(!overlap["snr"].toObject()["available"].toBool());
        const auto weak = rf::measureBands(frequency, power, rf::PowerScale::Density, 20,
                                            {}, std::pair{5U, 5U}, std::pair{0U, 1U});
        QCOMPARE(weak["snr"].toObject()["snr_db"].toString(), QString("-inf"));
        const auto noNoise = rf::measureBands(frequency, power, rf::PowerScale::Density, 20,
                                               {}, std::pair{2U, 4U}, std::pair{5U, 5U});
        QCOMPARE(noNoise["snr"].toObject()["snr_db"].toString(), QString("inf"));
        const std::vector<double> zero(power.size(), 0);
        const auto silence = rf::measureBands(frequency, zero, rf::PowerScale::Density, 20,
                                               std::pair{0U, 5U}, std::pair{2U, 4U},
                                               std::pair{0U, 1U});
        QCOMPARE(silence["selected"].toObject()["power_dbfs"].toString(), QString("-inf"));
        QVERIFY(!silence["snr"].toObject()["available"].toBool());
    }
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
    void fftScansMatchOrderedReference_data()
    {
        QTest::addColumn<bool>("real");
        QTest::newRow("complex") << false;
        QTest::newRow("real") << true;
    }
    void fftScansMatchOrderedReference()
    {
        QFETCH(bool, real);
        QTemporaryDir directory;
        constexpr std::uint64_t frames = 1'100'123;
        std::vector<double> values(static_cast<std::size_t>(frames) * (real ? 1U : 2U));
        for (std::size_t index = 0; index < values.size(); ++index)
            values[index] = static_cast<double>((index * 17U) % 101U) / 101.0 - 0.5;
        values[530'001] = std::numeric_limits<double>::quiet_NaN();
        rf::RecordingDescriptor descriptor;
        descriptor.path = writeFile(directory.filePath("scan.dat"),
            QByteArray(reinterpret_cast<const char *>(values.data()),
                       static_cast<qsizetype>(values.size() * sizeof(double))));
        descriptor.format.kind = real ? rf::SampleKind::Real : rf::SampleKind::Complex;
        descriptor.format.encoding = rf::Encoding::FloatingPoint;
        descriptor.format.bits = 64;
        descriptor.captures = {{0, {}, {}}, {37'777, {}, {}}, {700'003, {}, {}}};
        rf::Recording recording(descriptor);
        rf::DspSettings settings;
        settings.fftSize = 1024;
        settings.overlapPercent = 67;
        settings.removeDc = true;
        settings.conjugate = true;
        settings.scale = rf::PowerScale::Density;
        const rf::FrameRange range{17, frames - 83};
        const auto windows = 1 + (range.size() - 1024) / settings.hop();
        constexpr std::size_t columns = 37, rows = 23;
        rf::SpectrumEngine engine(descriptor.format.kind, descriptor.sampleRate, settings);
        std::vector<double> mean(engine.binCount()), maximum(engine.binCount());
        std::vector<double> cells(rows * columns, std::numeric_limits<double>::quiet_NaN());
        std::uint64_t valid = 0, invalid = 0, boundaries = 0, row = 0;
        for (std::uint64_t index = 0; index < windows; ++index) {
            const auto start = range.begin + index * settings.hop();
            while (row + 1 < rows && index >= windows * (row + 1) / rows)
                ++row;
            if (descriptor.crossesCapture(start, 1024)) {
                ++boundaries;
                continue;
            }
            const auto spectrum = engine.calculate(recording.read({start, start + 1024}));
            if (!spectrum.valid) {
                ++invalid;
                continue;
            }
            ++valid;
            for (std::size_t bin = 0; bin < mean.size(); ++bin) {
                mean[bin] += (spectrum.power[bin] - mean[bin]) / static_cast<double>(valid);
                maximum[bin] = std::max(maximum[bin], spectrum.power[bin]);
                auto &cell = cells[row * columns + ((bin + 1) * columns - 1) / mean.size()];
                cell = std::isnan(cell) ? spectrum.power[bin] : std::max(cell, spectrum.power[bin]);
            }
        }
        const auto caller = std::this_thread::get_id();
        bool callbacksOnCaller = true;
        double lastProgress = 0;
        rf::PreviewResult overview;
        const auto average = rf::analyzeAverage(recording, range, settings,
            [&] { callbacksOnCaller &= std::this_thread::get_id() == caller; return false; },
            [&](double progress) {
                callbacksOnCaller &= std::this_thread::get_id() == caller;
                QVERIFY(progress >= lastProgress);
                lastProgress = progress;
            }, &overview, columns, rows);
        QCOMPARE(average->averagePower, mean);
        QCOMPARE(average->maxPower, maximum);
        QCOMPARE(average->validWindows, valid);
        QCOMPARE(average->invalidWindows, invalid);
        QCOMPARE(average->boundaryWindows, boundaries);
        QCOMPARE(average->trailingFrames, (range.size() - 1024) % settings.hop());
        QCOMPARE(overview.waterfall, cells);
        QCOMPARE(lastProgress, 1.0);
        QVERIFY(invalid > 0 && boundaries > 0);
        std::vector<std::shared_ptr<const rf::PreviewResult>> updates;
        const auto waterfall = rf::analyzeWaterfall(recording, settings,
            {range, 0, 0, columns, rows, true}, {}, [&](auto snapshot) {
                callbacksOnCaller &= std::this_thread::get_id() == caller;
                if (!updates.empty())
                    QVERIFY(snapshot->completedRows >= updates.back()->completedRows);
                updates.push_back(std::move(snapshot));
            });
        QCOMPARE(waterfall->waterfall, cells);
        QCOMPARE(waterfall->rowStarts, overview.rowStarts);
        QCOMPARE(waterfall->invalidWindows, invalid);
        QCOMPARE(waterfall->boundaryWindows, boundaries);
        QVERIFY(waterfall->complete && callbacksOnCaller);
        QCOMPARE(updates.front()->completedRows, std::size_t{0});
        QVERIFY(std::all_of(updates.front()->waterfall.begin(), updates.front()->waterfall.end(),
                            [](double cell) { return std::isnan(cell); }));
        int checks = 0;
        QVERIFY_EXCEPTION_THROWN(rf::analyzeWaterfall(recording, settings,
            {range, 0, 0, columns, rows, true}, [&] { return ++checks > 20; }), rf::Cancelled);
        // A read failure must reach the caller and join every FFT worker.
        QFile source(descriptor.path);
        QVERIFY(source.open(QIODevice::ReadWrite));
        QVERIFY(source.resize(0));
        QVERIFY_EXCEPTION_THROWN(rf::analyzeAverage(recording, range, settings, {}),
                                 std::runtime_error);
    }
    void waveformPreviewCoversWholeInterval()
    {
        QTemporaryDir directory;
        QByteArray bytes = QByteArray::fromHex("002000f0").repeated(262144) +
                           QByteArray::fromHex("006000d0").repeated(262144) +
                           QByteArray::fromHex("ff7f0080");
        bytes.replace(200 * 4, 4, QByteArray::fromHex("ff7f0080"));
        rf::RecordingDescriptor descriptor;
        descriptor.path = writeFile(directory.filePath("levels.iq"), bytes);
        rf::Recording recording(descriptor);
        const rf::FrameRange range{17, recording.frameCount()};
        const auto preview = rf::analyzeWaveform(recording, range, {}, {}, true);
        QCOMPARE(preview->range, range);
        QVERIFY(!preview->complete);
        QCOMPARE(preview->points.size(), std::size_t{2048});
        QCOMPARE(preview->points.front().first, range.begin);
        QCOMPARE(preview->points.back().last, range.end);
        QCOMPARE(preview->points.front().maxI, 0.25);
        QVERIFY(preview->points.back().maxI > 0.99);
        QCOMPARE(preview->points.back().minQ, -1.0);
        QCOMPARE(preview->invalidSamples, std::uint64_t{0});
        QCOMPARE(preview->clippedComponents, std::uint64_t{2});
        for (std::size_t index = 1; index < preview->points.size(); ++index) {
            QVERIFY(preview->points[index].valid);
            QCOMPARE(preview->points[index - 1].last, preview->points[index].first);
        }
        // Exact scans still retain a peak skipped by the bounded preview.
        const auto exact = rf::analyzeWaveform(recording, range, {});
        QVERIFY(exact->complete);
        QVERIFY(exact->points.front().maxI > 0.99);
        QCOMPARE(exact->clippedComponents, std::uint64_t{4});
        const auto chunk = rf::analyzeWaveform(recording, {17, 2049}, {}, {}, true);
        QVERIFY(chunk->complete);
        QVERIFY(chunk->points[183].maxI > 0.99);
        QVERIFY_EXCEPTION_THROWN(
            rf::analyzeWaveform(recording, range, [] { return true; }, {}, true), rf::Cancelled);
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
        session.view.waterfallAutoRangeOnZoom = false;
        session.view.waterfallRows = 2048;
        session.range = {9007199254740993ULL, 9007199254745093ULL};
        session.bookmarks = {{session.range.begin, 4096, "Burst"}};
        const auto path = directory.filePath("session.json");
        rf::saveSession(path, session);
        const auto restored = rf::readSession(path);
        QCOMPARE(restored.range.begin, session.range.begin);
        QCOMPARE(restored.view.palette, QString("Baudline"));
        QVERIFY(!restored.view.waterfallAutoRangeOnZoom);
        QCOMPARE(restored.view.waterfallRows, 2048);
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
    void denseWaterfallAndProgressiveOverview()
    {
        QTemporaryDir directory;
        QByteArray data(1024 * 256 * 4, char{0});
        data.replace(255 * 256 * 4, 256 * 4, QByteArray::fromHex("00600000").repeated(256));
        rf::RecordingDescriptor descriptor;
        descriptor.path = writeFile(directory.filePath("burst.iq"), data);
        rf::Recording recording(descriptor);
        rf::DspSettings settings;
        settings.fftSize = 256;
        settings.overlapPercent = 0;
        settings.window = rf::Window::Rectangular;
        rf::WaterfallRequest request{{0, recording.frameCount()}, 0, 0, 256, 512, false};
        const auto detail = rf::analyzeWaterfall(recording, settings, request, {});
        QCOMPARE(detail->rowStarts.size(), std::size_t{512});
        QCOMPARE(detail->columns, 256);
        QVERIFY(detail->aggregated && !detail->sampled && detail->complete);
        QCOMPARE(*std::max_element(detail->waterfall.begin(), detail->waterfall.end()), 0.5625);
        request.rows = 64;
        request.exact = true;
        std::vector<std::shared_ptr<const rf::PreviewResult>> updates;
        const auto overview = rf::analyzeWaterfall(recording, settings, request, {},
            [&updates](auto result) { updates.push_back(std::move(result)); });
        QVERIFY(!updates.empty());
        QVERIFY(!updates.front()->complete);
        QCOMPARE(updates.front()->completedRows, std::size_t{0});
        QVERIFY(std::all_of(updates.front()->waterfall.begin(), updates.front()->waterfall.end(),
                             [](double power) { return std::isnan(power); }));
        QCOMPARE(overview->completedRows, std::size_t{64});
        QVERIFY(overview->complete);
        QCOMPARE(*std::max_element(overview->waterfall.begin(), overview->waterfall.end()), 0.5625);
        QVERIFY_EXCEPTION_THROWN(rf::analyzeWaterfall(recording, settings, request,
                                                       [] { return true; }), rf::Cancelled);
    }
    void waterfallVisibleFrequencyBins()
    {
        QTemporaryDir directory;
        rf::RecordingDescriptor descriptor;
        descriptor.path = writeFile(directory.filePath("dc.iq"),
                                    QByteArray::fromHex("00400000").repeated(8192));
        descriptor.sampleRate = 8192;
        rf::Recording recording(descriptor);
        rf::DspSettings settings;
        settings.fftSize = 8192;
        settings.window = rf::Window::Rectangular;
        rf::WaterfallRequest request{{0, 8192}, 0, 0, 4096, 1, false};
        const auto full = rf::analyzeWaterfall(recording, settings, request, {});
        QCOMPARE(full->columns, 4096);
        QCOMPARE(full->waterfall[2048], 0.25);
        request.left = -2;
        request.right = 2;
        const auto detail = rf::analyzeWaterfall(recording, settings, request, {});
        QVERIFY(detail->columns <= 5);
        QCOMPARE(detail->waterfall[2], 0.25);
        QCOMPARE(detail->waterfall[1], 0.0);
        QCOMPARE(detail->waterfall[3], 0.0);
        QCOMPARE(detail->waterfallBand->first, -2.0);
        QCOMPARE(detail->waterfallBand->second, 2.0);
        descriptor.format.kind = rf::SampleKind::Real;
        rf::Recording real(descriptor);
        request.range = {0, 8192};
        request.left = 0;
        request.right = 4096;
        const auto oneSided = rf::analyzeWaterfall(real, settings, request, {});
        QCOMPARE(oneSided->frequencies.front(), 0.0);
        QCOMPARE(oneSided->waterfallBand->second, 4096.0);
    }
    void waterfallSamplingAndCaptureBounds()
    {
        QTemporaryDir directory;
        rf::RecordingDescriptor descriptor;
        descriptor.path = writeFile(directory.filePath("zeros.iq"), QByteArray(256 * 8 * 4, char{0}));
        descriptor.captures = {{0, {}, {}}, {128, {}, {}}};
        rf::Recording recording(descriptor);
        rf::DspSettings settings;
        settings.fftSize = 256;
        settings.overlapPercent = 0;
        rf::WaterfallRequest request{{0, recording.frameCount()}, 0, 0, 256, 8, false};
        const auto detail = rf::analyzeWaterfall(recording, settings, request, {});
        QCOMPARE(detail->boundaryWindows, std::uint64_t{1});
        QVERIFY(std::isnan(detail->waterfall[0]));
        QCOMPARE(detail->waterfall[256], 0.0);
        QFile sparse(directory.filePath("large.iq"));
        QVERIFY(sparse.open(QIODevice::WriteOnly));
        QVERIFY(sparse.resize(64LL * 1024 * 1024));
        QCOMPARE(sparse.write(QByteArray::fromHex("00400000").repeated(65536)), qint64{65536 * 4});
        QVERIFY(sparse.seek(sparse.size() - 65536 * 4));
        QCOMPARE(sparse.write(QByteArray::fromHex("00600000").repeated(65536)), qint64{65536 * 4});
        sparse.close();
        descriptor.path = sparse.fileName();
        descriptor.captures.clear();
        rf::Recording large(descriptor);
        request.range = {0, large.frameCount()};
        request.rows = 512;
        const auto sampled = rf::analyzeWaterfall(large, settings, request, {});
        QVERIFY(sampled->sampled && !sampled->aggregated);
        QCOMPARE(sampled->rowStarts.size(), std::size_t{512});
        QCOMPARE(sampled->rowStarts.front(), std::uint64_t{0});
        QCOMPARE(sampled->rowStarts.back(), large.frameCount() - 256);
        settings.fftSize = 65536;
        request.rows = 128;
        request.columns = 37;
        request.fixedRows = true;
        request.left = -1'000'000;
        request.right = 1'000'000;
        const auto parallelSampled = rf::analyzeWaterfall(large, settings, request, {});
        QVERIFY(parallelSampled->sampled);
        const auto preview = rf::analyzePreview(large, request.range, settings, {});
        QCOMPARE(preview->rowStarts, parallelSampled->rowStarts);
        QCOMPARE(*std::max_element(preview->waterfall.begin(),
                                   preview->waterfall.begin() + preview->columns), 0.25);
        QCOMPARE(*std::max_element(preview->waterfall.end() - preview->columns,
                                   preview->waterfall.end()), 0.5625);
        rf::SpectrumEngine reference(rf::SampleKind::Complex, descriptor.sampleRate, settings);
        const auto frequencies = reference.frequencies();
        const auto first = static_cast<std::size_t>(std::upper_bound(
            frequencies.begin(), frequencies.end(), request.left) - frequencies.begin() - 1);
        const auto end = static_cast<std::size_t>(std::lower_bound(
            frequencies.begin(), frequencies.end(), request.right) - frequencies.begin());
        for (std::size_t row = 0; row < parallelSampled->rowStarts.size(); ++row) {
            const auto start = parallelSampled->rowStarts[row];
            const auto spectrum = reference.calculate(large.read({start, start + 65536}));
            for (std::size_t column = 0; column < 37; ++column)
                QCOMPARE(parallelSampled->waterfall[row * 37 + column],
                    *std::max_element(spectrum.power.begin() +
                                          static_cast<std::ptrdiff_t>(first + column * (end - first) / 37),
                                      spectrum.power.begin() +
                                          static_cast<std::ptrdiff_t>(first + (column + 1) * (end - first) / 37)));
        }
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
    void darkThemePreferences()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.json");
        rf::Preferences preferences;
        rf::savePreferences(path, preferences);
        QVERIFY(!rf::readPreferences(path).darkTheme);
        preferences.darkTheme = true;
        rf::savePreferences(path, preferences);
        QVERIFY(rf::readPreferences(path).darkTheme);
        auto legacy = rf::readJsonObject(path);
        legacy.remove("dark_theme");
        rf::writeJsonAtomic(path, legacy);
        QVERIFY(!rf::readPreferences(path).darkTheme);
        legacy["dark_theme"] = "false";
        rf::writeJsonAtomic(path, legacy);
        QVERIFY_EXCEPTION_THROWN(rf::readPreferences(path), std::runtime_error);
    }
    void recentFilesPreferences()
    {
        QTemporaryDir directory;
        const auto path = directory.filePath("preferences.json");
        rf::Preferences preferences;
        preferences.recentFiles = {directory.filePath("signal.iq"),
                                   directory.filePath("session.rfsession.json")};
        rf::savePreferences(path, preferences);
        QCOMPARE(rf::readPreferences(path).recentFiles, preferences.recentFiles);
        auto legacy = rf::readJsonObject(path);
        legacy.remove("recent_files");
        rf::writeJsonAtomic(path, legacy);
        QVERIFY(rf::readPreferences(path).recentFiles.isEmpty());
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
        view = rf::viewToJson({});
        view.remove("waterfall_auto_range_on_zoom");
        QVERIFY(rf::viewFromJson(view).waterfallAutoRangeOnZoom);
        view["waterfall_auto_range_on_zoom"] = "false";
        QVERIFY_EXCEPTION_THROWN(rf::viewFromJson(view), std::runtime_error);
        view = rf::viewToJson({});
        view.remove("waterfall_rows");
        QCOMPARE(rf::viewFromJson(view).waterfallRows, 0);
        for (const auto invalidRows : {-1, 127, 8192}) {
            view["waterfall_rows"] = invalidRows;
            QVERIFY_EXCEPTION_THROWN(rf::viewFromJson(view), std::runtime_error);
        }
        view["waterfall_rows"] = 1024.5;
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
