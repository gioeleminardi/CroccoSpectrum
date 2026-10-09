#pragma once

#include "dsp/Spectrum.h"
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace rf
{
using CancelCheck = std::function<bool()>;
using Progress = std::function<void(double)>;
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("Cancelled") {}
};

struct WavePoint {
    std::uint64_t first = 0;
    std::uint64_t last = 0; // Exclusive; min/max cover this entire bucket.
    double minI = 0, maxI = 0, minQ = 0, maxQ = 0, minMagnitude = 0, maxMagnitude = 0;
    bool valid = false;
};

struct WaveformResult {
    FrameRange range;
    std::vector<WavePoint> points;
    std::uint64_t invalidSamples = 0;
    std::uint64_t clippedComponents = 0;
    bool complete = false;
};

struct PreviewResult {
    FrameRange range;
    std::uint64_t spectrumStart = 0;
    std::vector<double> frequencies;
    SpectrumResult spectrum;
    // Only screen-scale linear maxima are retained for waterfall coloring.
    // A cell is NOT an exact numeric bin measurement; CSV uses spectrum.power.
    std::vector<double> waterfall;
    std::vector<std::uint64_t> rowStarts;
    int columns = 0;
    bool sampled = false;
    bool aggregated = false; // Exact temporal/frequency maxima over every window.
    // Waterfall-only requests may cover a cropped frequency band and publish
    // completed rows progressively. Empty optional bounds mean the full band.
    std::optional<std::pair<double, double>> waterfallBand;
    std::size_t completedRows = 0;
    bool complete = true;
    std::uint64_t invalidWindows = 0;
    std::uint64_t boundaryWindows = 0;
    WaveformResult waveform;
    [[nodiscard]] std::size_t memoryBytes() const;
};

struct AverageResult {
    FrameRange range;
    std::vector<double> frequencies;
    std::vector<double> averagePower;
    std::vector<double> maxPower;
    std::uint64_t validWindows = 0;
    std::uint64_t invalidWindows = 0;
    std::uint64_t boundaryWindows = 0;
    std::uint64_t trailingFrames = 0;
    double enbwHz = 0;
};

struct WaterfallRequest {
    FrameRange range;
    double left = 0, right = 0; // Equal bounds request the full frequency band.
    int columns = 1024, rows = 512; // Physical display pixels; bounded by analysis.
    bool exact = false; // Full scan for the minimap; viewport work stays bounded.
};
using WaterfallUpdate = std::function<void(std::shared_ptr<const PreviewResult>)>;
std::shared_ptr<PreviewResult> analyzeWaterfall(const Recording &recording,
                                               const DspSettings &settings,
                                               WaterfallRequest request,
                                               const CancelCheck &cancelled,
                                               const WaterfallUpdate &update = {});

std::shared_ptr<PreviewResult> analyzePreview(const Recording &recording, FrameRange range,
                                              const DspSettings &settings,
                                              const CancelCheck &cancelled);
std::shared_ptr<AverageResult> analyzeAverage(const Recording &recording, FrameRange range,
                                              const DspSettings &settings,
                                              const CancelCheck &cancelled,
                                              const Progress &progress = {},
                                              PreviewResult *overview = nullptr,
                                              int overviewColumns = 1024, int overviewRows = 128);
std::shared_ptr<WaveformResult> analyzeWaveform(const Recording &recording, FrameRange range,
                                                const CancelCheck &cancelled,
                                                const Progress &progress = {});
// Numerical and byte exports are transactional: never overwrite an existing
// path and remove incomplete new outputs on failure/cancellation.
void exportSamples(const Recording &recording, FrameRange range, const QString &output,
                   const CancelCheck &cancelled, const Progress &progress = {});
void exportSpectrumCsv(const RecordingDescriptor &recording, const QString &identity,
                       FrameRange range, const DspSettings &settings,
                       std::span<const double> frequencies, std::span<const double> powers,
                       const QString &method, const QJsonObject &coverage, const QString &output,
                       const CancelCheck &cancelled = {}, const Progress &progress = {});
} // namespace rf
