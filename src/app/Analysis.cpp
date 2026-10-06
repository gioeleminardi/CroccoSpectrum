#include "Analysis.h"
#include "ExportFile.h"
#include "recording/Metadata.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTextStream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace rf
{
namespace
{
void checkpoint(const CancelCheck &cancelled)
{
    if (cancelled && cancelled())
        throw Cancelled();
}

class ProgressReporter
{
  public:
    explicit ProgressReporter(Progress progress) : progress_(std::move(progress)) {}
    void report(double fraction)
    {
        const auto now = std::chrono::steady_clock::now();
        if (progress_ && (fraction >= 1 || now - last_ >= std::chrono::milliseconds(100))) {
            progress_(fraction);
            last_ = now;
        }
    }

  private:
    Progress progress_;
    std::chrono::steady_clock::time_point last_{};
};

// A reusable decoded block amortizes fstat/pread/decoding over many overlapping
// FFT windows. Reload starting at the next window when its tail leaves the
// block; this preserves window alignment without materializing the interval.
class WindowReader
{
  public:
    WindowReader(const Recording &recording, FrameRange range, std::uint64_t fftSize)
        : recording_(recording), range_(range),
          blockFrames_(std::max<std::uint64_t>(262144, fftSize))
    {
    }
    std::span<const std::complex<double>> window(std::uint64_t start, std::uint64_t count)
    {
        if (samples_.empty() || start < blockStart_ ||
            start - blockStart_ + count > samples_.size()) {
            blockStart_ = start;
            samples_ = recording_.read({start, start + std::min(blockFrames_, range_.end - start)});
        }
        return std::span(samples_).subspan(static_cast<std::size_t>(start - blockStart_),
                                           static_cast<std::size_t>(count));
    }

  private:
    const Recording &recording_;
    FrameRange range_;
    std::uint64_t blockFrames_;
    std::uint64_t blockStart_ = 0;
    std::vector<std::complex<double>> samples_;
};

std::uint64_t interpolate(std::uint64_t span, std::uint64_t index, std::uint64_t denominator)
{
    // Exact integer interpolation without span*index overflowing at huge file
    // sizes. index <= denominator and the remainder product stays tiny here.
    return (span / denominator) * index + (span % denominator) * index / denominator;
}
} // namespace

std::size_t PreviewResult::memoryBytes() const
{
    return sizeof(*this) +
           (frequencies.size() + spectrum.power.size() + waterfall.size()) * sizeof(double) +
           rowStarts.size() * sizeof(std::uint64_t) + waveform.points.size() * sizeof(WavePoint);
}

std::shared_ptr<WaveformResult> analyzeWaveform(const Recording &recording, FrameRange range,
                                                const CancelCheck &cancelled,
                                                const Progress &progress)
{
    recording.validateRange(range);
    if (range.size() == 0)
        throw std::runtime_error("Waveform selection is empty");
    auto result = std::make_shared<WaveformResult>();
    result->range = range;
    constexpr std::uint64_t maxPoints = 2048;
    const auto points = std::min(range.size(), maxPoints);
    result->points.resize(static_cast<std::size_t>(points));
    for (std::uint64_t index = 0; index < points; ++index) {
        auto &point = result->points[static_cast<std::size_t>(index)];
        point.first = range.begin + interpolate(range.size(), index, points);
        point.last = range.begin + interpolate(range.size(), index + 1, points);
    }
    ProgressReporter reporter(progress);
    std::size_t bucket = 0;
    constexpr std::uint64_t blockFrames = 131072;
    for (auto start = range.begin; start < range.end;) {
        checkpoint(cancelled);
        const auto end = start + std::min(blockFrames, range.end - start);
        const auto samples = recording.read({start, end});
        for (std::size_t index = 0; index < samples.size(); ++index) {
            if ((index & 4095U) == 0)
                checkpoint(cancelled);
            const auto frame = start + index;
            while (bucket + 1 < result->points.size() && frame >= result->points[bucket].last)
                ++bucket;
            auto &point = result->points[bucket];
            const auto sample = samples[index];
            if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) {
                ++result->invalidSamples;
                continue;
            }
            const double magnitude = std::abs(sample);
            if (!std::isfinite(magnitude)) {
                ++result->invalidSamples;
                continue;
            }
            const auto &format = recording.descriptor().format;
            const double positiveLimit = format.encoding == Encoding::FloatingPoint
                                             ? 1.0
                                             : 1.0 - std::ldexp(1.0, 1 - format.bits);
            if (sample.real() <= -1 || sample.real() >= positiveLimit)
                ++result->clippedComponents;
            if (format.kind == SampleKind::Complex &&
                (sample.imag() <= -1 || sample.imag() >= positiveLimit))
                ++result->clippedComponents;
            if (!point.valid) {
                point.minI = point.maxI = sample.real();
                point.minQ = point.maxQ = sample.imag();
                point.minMagnitude = point.maxMagnitude = magnitude;
                point.valid = true;
            } else {
                point.minI = std::min(point.minI, sample.real());
                point.maxI = std::max(point.maxI, sample.real());
                point.minQ = std::min(point.minQ, sample.imag());
                point.maxQ = std::max(point.maxQ, sample.imag());
                point.minMagnitude = std::min(point.minMagnitude, magnitude);
                point.maxMagnitude = std::max(point.maxMagnitude, magnitude);
            }
        }
        start = end;
        reporter.report(static_cast<double>(start - range.begin) /
                        static_cast<double>(range.size()));
    }
    checkpoint(cancelled);
    recording.verifyUnchanged();
    result->complete = true;
    return result;
}

std::shared_ptr<PreviewResult> analyzePreview(const Recording &recording, FrameRange range,
                                              const DspSettings &settings,
                                              const CancelCheck &cancelled)
{
    recording.validateRange(range);
    settings.validate(recording.descriptor().sampleRate);
    const auto length = static_cast<std::uint64_t>(settings.fftSize);
    if (range.size() < length)
        throw std::runtime_error(
            "Selection is shorter than the FFT window; reduce FFT length or widen selection");
    SpectrumEngine engine(recording.descriptor().format.kind, recording.descriptor().sampleRate,
                          settings);
    auto result = std::make_shared<PreviewResult>();
    result->range = range;
    result->spectrumStart = range.begin;
    result->frequencies = engine.frequencies();
    const auto windows = 1 + (range.size() - length) / settings.hop();
    // Even the largest FFT stays responsive: preview computes at most eight
    // million input sample-frames, irrespective of total recording length.
    const auto maxRows =
        std::min<std::uint64_t>(128, std::max<std::uint64_t>(1, 8'388'608 / length));
    const auto rows = std::min(windows, maxRows);
    result->sampled = rows < windows;
    result->columns = static_cast<int>(std::min<std::size_t>(1024, engine.binCount()));
    result->waterfall.reserve(static_cast<std::size_t>(rows) *
                              static_cast<std::size_t>(result->columns));
    for (std::uint64_t row = 0; row < rows; ++row) {
        checkpoint(cancelled);
        const auto windowIndex = rows == 1 ? 0 : interpolate(windows - 1, row, rows - 1);
        const auto start = range.begin + windowIndex * settings.hop();
        result->rowStarts.push_back(start);
        SpectrumResult spectrum;
        spectrum.enbwHz = engine.enbwHz();
        if (recording.descriptor().crossesCapture(start, length)) {
            ++result->boundaryWindows;
            spectrum.valid = false;
        } else {
            spectrum = engine.calculate(recording.read({start, start + length}));
            if (!spectrum.valid)
                ++result->invalidWindows;
        }
        for (int column = 0; column < result->columns; ++column) {
            double peak = std::numeric_limits<double>::quiet_NaN();
            if (spectrum.valid) {
                const auto begin = static_cast<std::size_t>(column) * spectrum.power.size() /
                                   static_cast<std::size_t>(result->columns);
                const auto end = static_cast<std::size_t>(column + 1) * spectrum.power.size() /
                                 static_cast<std::size_t>(result->columns);
                peak =
                    *std::max_element(spectrum.power.begin() + static_cast<std::ptrdiff_t>(begin),
                                      spectrum.power.begin() + static_cast<std::ptrdiff_t>(end));
            }
            result->waterfall.push_back(peak);
        }
        if (row == 0)
            result->spectrum = std::move(spectrum);
    }
    const FrameRange waveRange{range.begin,
                               range.begin + std::min<std::uint64_t>(range.size(), 131072)};
    result->waveform = *analyzeWaveform(recording, waveRange, cancelled);
    result->waveform.complete = waveRange == range;
    checkpoint(cancelled);
    recording.verifyUnchanged();
    return result;
}

std::shared_ptr<AverageResult> analyzeAverage(const Recording &recording, FrameRange range,
                                              const DspSettings &settings,
                                              const CancelCheck &cancelled,
                                              const Progress &progress, PreviewResult *overview)
{
    recording.validateRange(range);
    settings.validate(recording.descriptor().sampleRate);
    const auto length = static_cast<std::uint64_t>(settings.fftSize);
    if (range.size() < length)
        throw std::runtime_error("Selection is shorter than one complete FFT window");
    SpectrumEngine engine(recording.descriptor().format.kind, recording.descriptor().sampleRate,
                          settings);
    WindowReader reader(recording, range, length);
    auto result = std::make_shared<AverageResult>();
    result->range = range;
    result->frequencies = engine.frequencies();
    result->enbwHz = engine.enbwHz();
    result->averagePower.resize(engine.binCount());
    result->maxPower.resize(engine.binCount());
    const auto windows = 1 + (range.size() - length) / settings.hop();
    result->trailingFrames = range.size() - ((windows - 1) * settings.hop() + length);
    std::uint64_t overviewRow = 0;
    std::uint64_t overviewRows = 0;
    if (overview) {
        // Produce exact overview and exact average in the same full pass. Only
        // fixed-size pixel aggregates are kept; a short event cannot vanish
        // because a window was skipped, although pixel aggregation loses detail.
        overview->range = range;
        overview->spectrumStart = range.begin;
        overview->frequencies = result->frequencies;
        overview->aggregated = true;
        overview->columns = static_cast<int>(std::min<std::size_t>(1024, engine.binCount()));
        overviewRows = std::min<std::uint64_t>(128, windows);
        overview->waterfall.assign(static_cast<std::size_t>(overviewRows) *
                                       static_cast<std::size_t>(overview->columns),
                                   std::numeric_limits<double>::quiet_NaN());
        for (std::uint64_t row = 0; row < overviewRows; ++row)
            overview->rowStarts.push_back(range.begin +
                                          interpolate(windows, row, overviewRows) * settings.hop());
        overview->spectrum.valid = false;
    }
    ProgressReporter reporter(progress);
    for (std::uint64_t index = 0; index < windows; ++index) {
        checkpoint(cancelled);
        const auto start = range.begin + index * settings.hop();
        if (overview && overviewRow + 1 < overviewRows &&
            index >= interpolate(windows, overviewRow + 1, overviewRows))
            ++overviewRow;
        if (recording.descriptor().crossesCapture(start, length))
            ++result->boundaryWindows;
        else {
            auto spectrum = engine.calculate(reader.window(start, length));
            if (!spectrum.valid)
                ++result->invalidWindows;
            else {
                ++result->validWindows;
                for (std::size_t bin = 0; bin < spectrum.power.size(); ++bin) {
                    // Online arithmetic mean in linear power, not dB. This
                    // avoids both file-sized storage and a sum that overflows.
                    result->averagePower[bin] += (spectrum.power[bin] - result->averagePower[bin]) /
                                                 static_cast<double>(result->validWindows);
                    result->maxPower[bin] = std::max(result->maxPower[bin], spectrum.power[bin]);
                    if (overview) {
                        const auto column =
                            ((bin + 1) * static_cast<std::size_t>(overview->columns) - 1) /
                            spectrum.power.size();
                        auto &cell =
                            overview->waterfall[static_cast<std::size_t>(overviewRow) *
                                                    static_cast<std::size_t>(overview->columns) +
                                                column];
                        cell = std::isnan(cell) ? spectrum.power[bin]
                                                : std::max(cell, spectrum.power[bin]);
                    }
                }
            }
            if (overview && index == 0)
                overview->spectrum = std::move(spectrum);
        }
        reporter.report(static_cast<double>(index + 1) / static_cast<double>(windows));
    }
    checkpoint(cancelled);
    recording.verifyUnchanged();
    if (result->validWindows == 0)
        throw std::runtime_error("No valid complete windows in the selected interval");
    if (overview) {
        overview->invalidWindows = result->invalidWindows;
        overview->boundaryWindows = result->boundaryWindows;
        const FrameRange waveRange{range.begin,
                                   range.begin + std::min<std::uint64_t>(range.size(), 131072)};
        overview->waveform = *analyzeWaveform(recording, waveRange, cancelled);
        overview->waveform.complete = waveRange == range;
    }
    return result;
}

void exportSamples(const Recording &recording, FrameRange range, const QString &output,
                   const CancelCheck &cancelled, const Progress &progress)
{
    recording.validateRange(range);
    if (range.size() == 0)
        throw std::runtime_error("Cannot export an empty range");
    const QString metadataPath = output + ".rfmeta.json";
    ExportFile data(output);
    ExportFile metadata(metadataPath);
    auto &file = data.file();
    bool metadataPublished = false;
    try {
        ProgressReporter reporter(progress);
        constexpr std::uint64_t blockFrames = 131072;
        for (auto start = range.begin; start < range.end;) {
            checkpoint(cancelled);
            const auto end = start + std::min(blockFrames, range.end - start);
            const auto bytes = recording.readBytes({start, end});
            if (file.write(reinterpret_cast<const char *>(bytes.data()),
                           static_cast<qint64>(bytes.size())) != static_cast<qint64>(bytes.size()))
                throw std::runtime_error(file.errorString().toStdString());
            start = end;
            reporter.report(static_cast<double>(start - range.begin) /
                            static_cast<double>(range.size()));
        }
        checkpoint(cancelled);
        recording.verifyUnchanged();
        if (!file.flush())
            throw std::runtime_error(file.errorString().toStdString());
        auto descriptor = recording.descriptor();
        descriptor.path = QFileInfo(output).absoluteFilePath();
        descriptor.dataOffset = 0;
        descriptor.dataBytes = range.size() * descriptor.format.frameBytes();
        descriptor.startUtc = recording.descriptor().timestampAt(range.begin);
        descriptor.captures.clear();
        descriptor.annotations.clear();
        // Preserve the original global fallback. Making the initial segment's
        // frequency the new global value would invent a frequency for later
        // captures that explicitly have no known frequency. Instead, describe
        // the cut's initial segment with a capture at frame zero.
        descriptor.captures.push_back(
            {0,
             recording.descriptor().hasFrequencyAt(range.begin)
                 ? std::optional<double>(recording.descriptor().frequencyAt(range.begin))
                 : std::nullopt,
             descriptor.startUtc});
        for (const auto &capture : recording.descriptor().captures)
            if (capture.start > range.begin && capture.start < range.end)
                descriptor.captures.push_back(
                    {capture.start - range.begin, capture.frequency, capture.datetime});
        for (const auto &note : recording.descriptor().annotations) {
            const auto begin = std::max(note.start, range.begin);
            const auto end = std::min(note.start + note.count, range.end);
            if (begin < end ||
                (note.count == 0 && note.start >= range.begin && note.start < range.end))
                descriptor.annotations.push_back({begin - range.begin, end - begin, note.label});
        }
        const QJsonObject root{{"schema", 1},
                               {"complete", true},
                               {"application_version", RF_VERSION},
                               {"recording", recordingToJson(descriptor)},
                               {"source_identity", recording.identity()},
                               {"source_start", QString::number(range.begin)},
                               {"source_end", QString::number(range.end)}};
        const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
        if (bytes.size() > maxMetadataBytes)
            throw std::runtime_error("Export metadata exceed the 4 MiB import limit");
        if (metadata.file().write(bytes) != bytes.size())
            throw std::runtime_error("Metadata export failed");
        checkpoint(cancelled);
        // A file pair cannot be committed as one filesystem transaction. Publish
        // the small sidecar first and data last. A crash between these renames
        // leaves a sidecar pointing to absent data, never apparently valid
        // partial sample bytes. Roll back our sidecar on ordinary failure.
        metadata.commit();
        metadataPublished = true;
        data.commit();
    } catch (...) {
        if (metadataPublished)
            QFile::remove(metadataPath);
        throw;
    }
}

void exportSpectrumCsv(const RecordingDescriptor &recording, const QString &identity,
                       FrameRange range, const DspSettings &settings,
                       std::span<const double> frequencies, std::span<const double> powers,
                       const QString &method, const QJsonObject &coverage, const QString &output,
                       const CancelCheck &cancelled, const Progress &progress)
{
    if (frequencies.size() != powers.size())
        throw std::runtime_error("Frequency/power size mismatch");
    checkpoint(cancelled);
    QJsonObject metadata{{"application_version", RF_VERSION},
                         {"recording", recordingToJson(recording)},
                         {"source_identity", identity},
                         {"start", QString::number(range.begin)},
                         {"end", QString::number(range.end)},
                         {"fft_size", settings.fftSize},
                         {"window", windowName(settings.window)},
                         {"hop_frames", QString::number(settings.hop())},
                         {"remove_dc", settings.removeDc},
                         {"conjugate", settings.conjugate},
                         {"unit", powerUnit(settings.scale)},
                         {"power_reference", "1 normalized mean-square unit"},
                         {"method", method},
                         {"coverage", coverage}};
    // CSV is atomic, so a crash cannot leave a truncated file labeled complete.
    ExportFile file(output);
    QTextStream stream(&file.file());
    ProgressReporter reporter(progress);
    stream.setRealNumberPrecision(17);
    stream << "# " << QJsonDocument(metadata).toJson(QJsonDocument::Compact) << '\n';
    stream << "baseband_frequency_hz,linear_power,power_" << powerUnit(settings.scale) << '\n';
    for (std::size_t index = 0; index < powers.size(); ++index) {
        if ((index & 4095U) == 0) {
            checkpoint(cancelled);
            reporter.report(static_cast<double>(index) / static_cast<double>(powers.size()));
        }
        stream << frequencies[index] << ',' << powers[index] << ',' << powerToDb(powers[index])
               << '\n';
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok)
        throw std::runtime_error("CSV export failed");
    checkpoint(cancelled);
    file.commit();
    reporter.report(1);
}
} // namespace rf
