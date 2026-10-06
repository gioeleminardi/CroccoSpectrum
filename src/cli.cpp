#include "app/Analysis.h"
#include "recording/Metadata.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <sys/resource.h>

namespace
{
QJsonArray array(std::span<const double> values)
{
    QJsonArray result;
    for (const auto value : values)
        result.append(std::isfinite(value) ? QJsonValue(value) : QJsonValue(QJsonValue::Null));
    return result;
}
QString checkedChoice(const QCommandLineParser &parser, const QString &name,
                      const QStringList &choices)
{
    const auto value = parser.value(name);
    if (!choices.contains(value))
        throw std::runtime_error(("Invalid --" + name).toStdString());
    return value;
}
double numeric(const QCommandLineParser &parser, const QString &name)
{
    bool ok = false;
    const auto value = parser.value(name).toDouble(&ok);
    if (!ok || !std::isfinite(value))
        throw std::runtime_error(("Invalid --" + name).toStdString());
    return value;
}
int integer(const QCommandLineParser &parser, const QString &name)
{
    const double value = numeric(parser, name);
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max() ||
        std::floor(value) != value)
        throw std::runtime_error(("Invalid integer --" + name).toStdString());
    return static_cast<int>(value);
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("croccospectrum-cli");
    app.setApplicationVersion(RF_VERSION);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command",
                                 "inspect, spectrum, average, waveform, or export-samples");
    parser.addOptions(
        {{{"f", "file"}, "Input recording / SigMF metadata / exported metadata", "path"},
         {"kind", "real or complex", "kind", "complex"},
         {"encoding", "signed, unsigned, or float", "encoding", "signed"},
         {"bits", "Scalar width", "bits", "16"},
         {"endian", "little or big", "endian", "little"},
         {"order", "IQ or QI", "order", "IQ"},
         {"sample-rate", "Samples/second", "rate", "100000000"},
         {"center-frequency", "Optional center frequency (Hz)", "frequency"},
         {"full-scale", "Floating-point full scale", "value", "1"},
         {"offset", "Data start byte offset", "bytes", "0"},
         {"length", "Data length in bytes", "bytes"},
         {"start", "First frame (inclusive)", "frame", "0"},
         {"end", "Last frame (exclusive); default EOF", "frame"},
         {"fft", "Power-of-two FFT length", "length", "4096"},
         {"window", "hann, rectangular, hamming, blackman-harris", "window", "hann"},
         {"overlap", "Overlap percentage", "percent", "50"},
         {"scale", "spectrum or density", "scale", "spectrum"},
         {"remove-dc", "Remove each window's mean"},
         {"conjugate", "Invert complex spectral sign"},
         {"output", "Output path for CSV or sample export (must not exist)", "path"},
         {"summary", "Omit numeric bin arrays; useful for benchmarks"}});
    parser.process(app);
    try {
        if (parser.positionalArguments().size() != 1 || parser.value("file").isEmpty())
            throw std::runtime_error("Supply a command and --file; see --help");
        const auto command = parser.positionalArguments().front();
        if (!QStringList{"inspect", "spectrum", "average", "waveform", "export-samples"}.contains(
                command))
            throw std::runtime_error("Unknown command");
        const auto path = QFileInfo(parser.value("file")).absoluteFilePath();
        rf::RecordingDescriptor descriptor;
        if (path.endsWith(".sigmf-meta"))
            descriptor = rf::readSigMf(path);
        else if (path.endsWith(".rfmeta.json"))
            descriptor = rf::recordingFromJson(rf::readJsonObject(path)["recording"].toObject());
        else {
            descriptor.path = path;
            descriptor.format.kind = checkedChoice(parser, "kind", {"real", "complex"}) == "real"
                                         ? rf::SampleKind::Real
                                         : rf::SampleKind::Complex;
            const auto encoding =
                checkedChoice(parser, "encoding", {"signed", "unsigned", "float"});
            descriptor.format.encoding = encoding == "float"    ? rf::Encoding::FloatingPoint
                                         : encoding == "signed" ? rf::Encoding::SignedInteger
                                                                : rf::Encoding::UnsignedInteger;
            descriptor.format.bits = integer(parser, "bits");
            descriptor.format.byteOrder =
                checkedChoice(parser, "endian", {"little", "big"}) == "little"
                    ? rf::ByteOrder::LittleEndian
                    : rf::ByteOrder::BigEndian;
            descriptor.format.componentOrder = checkedChoice(parser, "order", {"IQ", "QI"}) == "IQ"
                                                   ? rf::ComponentOrder::IQ
                                                   : rf::ComponentOrder::QI;
            descriptor.format.floatFullScale = numeric(parser, "full-scale");
            descriptor.sampleRate = numeric(parser, "sample-rate");
            descriptor.dataOffset = rf::jsonUnsigned(parser.value("offset"), "offset");
            if (parser.isSet("length"))
                descriptor.dataBytes = rf::jsonUnsigned(parser.value("length"), "length");
            if (parser.isSet("center-frequency"))
                descriptor.centerFrequency = numeric(parser, "center-frequency");
        }
        rf::Recording recording(descriptor);
        rf::FrameRange range{rf::jsonUnsigned(parser.value("start"), "start"),
                             parser.isSet("end") ? rf::jsonUnsigned(parser.value("end"), "end")
                                                 : recording.frameCount()};
        recording.validateRange(range);
        rf::DspSettings settings;
        settings.fftSize = integer(parser, "fft");
        settings.overlapPercent = integer(parser, "overlap");
        const auto window =
            checkedChoice(parser, "window", {"hann", "rectangular", "hamming", "blackman-harris"});
        settings.window = window == "hann"          ? rf::Window::Hann
                          : window == "rectangular" ? rf::Window::Rectangular
                          : window == "hamming"     ? rf::Window::Hamming
                                                    : rf::Window::BlackmanHarris;
        settings.scale = checkedChoice(parser, "scale", {"spectrum", "density"}) == "spectrum"
                             ? rf::PowerScale::Spectrum
                             : rf::PowerScale::Density;
        settings.removeDc = parser.isSet("remove-dc");
        settings.conjugate = parser.isSet("conjugate");
        settings.validate(descriptor.sampleRate);
        QJsonObject output{{"version", RF_VERSION},
                           {"recording", rf::recordingToJson(descriptor)},
                           {"frame_count", QString::number(recording.frameCount())},
                           {"source_identity", recording.identity()},
                           {"range_start", QString::number(range.begin)},
                           {"range_end", QString::number(range.end)}};
        const auto started = std::chrono::steady_clock::now();
        if (command == "inspect") {
            const auto samples = recording.read(
                {range.begin, range.begin + std::min<std::uint64_t>(16, range.size())});
            QJsonArray decoded;
            for (const auto sample : samples)
                decoded.append(QJsonArray{
                    std::isfinite(sample.real()) ? QJsonValue(sample.real()) : QJsonValue(),
                    std::isfinite(sample.imag()) ? QJsonValue(sample.imag()) : QJsonValue()});
            output["samples"] = decoded;
        } else if (command == "spectrum" || command == "average") {
            std::vector<double> frequencies, power;
            if (command == "spectrum") {
                if (range.size() < static_cast<std::uint64_t>(settings.fftSize))
                    throw std::runtime_error("Selection is shorter than FFT length");
                if (descriptor.crossesCapture(range.begin, settings.fftSize))
                    throw std::runtime_error("Spectrum window crosses a capture boundary");
                rf::SpectrumEngine engine(descriptor.format.kind, descriptor.sampleRate, settings);
                const auto spectrum = engine.calculate(recording.read(
                    {range.begin, range.begin + static_cast<std::uint64_t>(settings.fftSize)}));
                if (!spectrum.valid)
                    throw std::runtime_error("Spectrum contains invalid/nonfinite samples");
                frequencies = engine.frequencies();
                power = spectrum.power;
                output["enbw_hz"] = spectrum.enbwHz;
                range.end = range.begin + static_cast<std::uint64_t>(settings.fftSize);
                output["range_end"] = QString::number(range.end);
            } else {
                const auto average =
                    rf::analyzeAverage(recording, range, settings, [] { return false; });
                frequencies = average->frequencies;
                power = average->averagePower;
                output["valid_windows"] = QString::number(average->validWindows);
                output["invalid_windows"] = QString::number(average->invalidWindows);
                output["boundary_windows"] = QString::number(average->boundaryWindows);
                output["trailing_frames"] = QString::number(average->trailingFrames);
                output["enbw_hz"] = average->enbwHz;
                if (!parser.isSet("summary"))
                    output["max_power"] = array(average->maxPower);
            }
            if (!parser.isSet("summary")) {
                output["frequencies_hz"] = array(frequencies);
                output["power"] = array(power);
            }
            output["power_unit"] = rf::powerUnit(settings.scale);
            output["complete_pass"] = true;
            if (parser.isSet("output"))
                rf::exportSpectrumCsv(descriptor, recording.identity(), range, settings,
                                      frequencies, power, command, output, parser.value("output"));
        } else if (command == "waveform") {
            const auto waveform = rf::analyzeWaveform(recording, range, [] { return false; });
            output["invalid_samples"] = QString::number(waveform->invalidSamples);
            output["clipped_components"] = QString::number(waveform->clippedComponents);
            QJsonArray points;
            for (const auto &point : waveform->points)
                points.append(QJsonObject{{"start", QString::number(point.first)},
                                          {"end", QString::number(point.last)},
                                          {"valid", point.valid},
                                          {"min_i", point.minI},
                                          {"max_i", point.maxI},
                                          {"min_q", point.minQ},
                                          {"max_q", point.maxQ}});
            if (!parser.isSet("summary"))
                output["points"] = points;
        } else {
            if (!parser.isSet("output"))
                throw std::runtime_error("Sample export requires --output");
            rf::exportSamples(recording, range, parser.value("output"), [] { return false; });
            output["complete"] = true;
        }
        recording.verifyUnchanged();
        output["elapsed_seconds"] =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        output["peak_rss_kib"] = static_cast<double>(usage.ru_maxrss);
        std::cout << QJsonDocument(output).toJson(QJsonDocument::Compact).constData() << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
