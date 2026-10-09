#pragma once
#include "Analysis.h"
#include <QImage>
#include <QObject>
#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <optional>
#include <thread>

namespace rf
{
struct CsvRequest {
    RecordingDescriptor recording;
    QString identity;
    FrameRange range;
    DspSettings settings;
    std::vector<double> frequencies;
    std::vector<double> power;
    QString method;
    QJsonObject coverage;
    QString output;
};
struct PngRequest {
    QImage image; // Captured on the GUI thread; encoding happens on the worker.
    QJsonObject metadata;
    QString output;
};

// One worker and one replaceable pending request are deliberately sufficient
// initially. This bounds memory/queues and makes latest-request behavior easy
// to reason about. No QRunnable is submitted for each drag/mouse event.
class AnalysisController : public QObject
{
    Q_OBJECT
  public:
    explicit AnalysisController(QObject *parent = nullptr);
    ~AnalysisController() override;
    quint64 open(RecordingDescriptor descriptor, DspSettings settings, FrameRange range = {});
    quint64 inspect(RecordingDescriptor descriptor);
    quint64 preview(std::shared_ptr<Recording> recording, FrameRange range, DspSettings settings);
    quint64 average(std::shared_ptr<Recording> recording, FrameRange range, DspSettings settings);
    quint64 overview(std::shared_ptr<Recording> recording, FrameRange range, DspSettings settings,
                     int columns = 1024, int rows = 128);
    quint64 waterfall(std::shared_ptr<Recording> recording, DspSettings settings,
                      WaterfallRequest request);
    quint64 waveform(std::shared_ptr<Recording> recording, FrameRange range);
    quint64 samples(std::shared_ptr<Recording> recording, FrameRange range, QString output);
    quint64 csv(CsvRequest request);
    quint64 png(PngRequest request);
    void cancel();
  signals:
    void recordingOpened(quint64 generation, std::shared_ptr<rf::Recording> recording,
                         rf::FrameRange range);
    void previewReady(quint64 generation, std::shared_ptr<const rf::PreviewResult> result);
    void averageReady(quint64 generation, std::shared_ptr<const rf::AverageResult> result);
    void waveformReady(quint64 generation, std::shared_ptr<const rf::WaveformResult> result);
    void waterfallReady(quint64 generation, std::shared_ptr<const rf::PreviewResult> result);
    void progressChanged(quint64 generation, double fraction);
    void finished(quint64 generation, QString message);
    void failed(quint64 generation, QString message);
    void processingChanged(bool active); // Includes queued and executing work.

  private:
    enum class Kind { Open, Inspect, Preview, Average, Overview, Waveform, Waterfall, Samples, Csv, Png };
    struct Job {
        Kind kind = Kind::Preview;
        quint64 generation = 0;
        RecordingDescriptor descriptor;
        std::shared_ptr<Recording> recording;
        FrameRange range;
        DspSettings settings;
        WaterfallRequest waterfall;
        QString output;
        std::optional<CsvRequest> csv;
        std::optional<PngRequest> png;
        std::shared_ptr<std::atomic_bool> cancelled;
    };
    quint64 submit(Job job);
    void run(std::stop_token stop);
    void execute(Job job);
    QString previewKey(const Job &job) const;
    void publishWaterfall(quint64 generation, std::shared_ptr<const PreviewResult> result);
    void publishProcessing(bool active);
    std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<Job> pending_;
    bool executing_ = false;
    std::shared_ptr<std::atomic_bool> activeCancellation_;
    quint64 generation_ = 0;
    // Worker-only LRU: a total-byte quota, rather than entry count, prevents
    // million-bin spectra from multiplying memory usage without a bound.
    struct CacheEntry {
        QString key;
        std::shared_ptr<const PreviewResult> result;
    };
    std::list<CacheEntry> cache_;
    std::size_t cacheBytes_ = 0;
    // A single queued GUI notification coalesces progressive snapshots. A slow
    // paint/event loop cannot accumulate file-sized result queues.
    std::shared_ptr<const PreviewResult> latestWaterfall_;
    quint64 waterfallGeneration_ = 0;
    bool waterfallNotificationPending_ = false;
    std::jthread worker_;
};
} // namespace rf
Q_DECLARE_METATYPE(rf::FrameRange)
Q_DECLARE_METATYPE(std::shared_ptr<rf::Recording>)
Q_DECLARE_METATYPE(std::shared_ptr<const rf::PreviewResult>)
Q_DECLARE_METATYPE(std::shared_ptr<const rf::AverageResult>)
Q_DECLARE_METATYPE(std::shared_ptr<const rf::WaveformResult>)
