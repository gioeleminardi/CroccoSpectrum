#include "AnalysisController.h"
#include "ExportFile.h"
#include "Session.h"
#include "recording/Metadata.h"
#include <QImageWriter>
#include <QJsonDocument>

namespace rf
{
AnalysisController::AnalysisController(QObject *parent) : QObject(parent)
{
    qRegisterMetaType<FrameRange>();
    qRegisterMetaType<std::shared_ptr<Recording>>();
    qRegisterMetaType<std::shared_ptr<const PreviewResult>>();
    qRegisterMetaType<std::shared_ptr<const AverageResult>>();
    qRegisterMetaType<std::shared_ptr<const WaveformResult>>();
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

AnalysisController::~AnalysisController()
{
    // Join before QObject or cache destruction: the worker can emit signals,
    // but must never access an already-destroyed sender or its members.
    cancel();
    worker_.request_stop();
    wake_.notify_all();
    worker_.join();
}

quint64 AnalysisController::submit(Job job)
{
    const std::lock_guard lock(mutex_);
    if (activeCancellation_)
        activeCancellation_->store(true);
    if (pending_)
        pending_->cancelled->store(true);
    job.generation = ++generation_;
    job.cancelled = std::make_shared<std::atomic_bool>(false);
    activeCancellation_ = job.cancelled;
    pending_ = std::move(job);
    wake_.notify_one();
    return generation_;
}

quint64 AnalysisController::open(RecordingDescriptor descriptor, DspSettings settings,
                                 FrameRange range)
{
    Job job;
    job.kind = Kind::Open;
    job.descriptor = std::move(descriptor);
    job.settings = settings;
    job.range = range;
    return submit(std::move(job));
}
quint64 AnalysisController::inspect(RecordingDescriptor descriptor)
{
    Job job;
    job.kind = Kind::Inspect;
    job.descriptor = std::move(descriptor);
    return submit(std::move(job));
}
quint64 AnalysisController::preview(std::shared_ptr<Recording> recording, FrameRange range,
                                    DspSettings settings)
{
    Job job;
    job.kind = Kind::Preview;
    job.recording = std::move(recording);
    job.range = range;
    job.settings = settings;
    return submit(std::move(job));
}
quint64 AnalysisController::average(std::shared_ptr<Recording> recording, FrameRange range,
                                    DspSettings settings)
{
    Job job;
    job.kind = Kind::Average;
    job.recording = std::move(recording);
    job.range = range;
    job.settings = settings;
    return submit(std::move(job));
}
quint64 AnalysisController::waveform(std::shared_ptr<Recording> recording, FrameRange range)
{
    Job job;
    job.kind = Kind::Waveform;
    job.recording = std::move(recording);
    job.range = range;
    return submit(std::move(job));
}
quint64 AnalysisController::waterfall(std::shared_ptr<Recording> recording, DspSettings settings,
                                      WaterfallRequest request)
{
    Job job;
    job.kind = Kind::Waterfall;
    job.recording = std::move(recording);
    job.settings = settings;
    job.range = request.range;
    job.waterfall = request;
    return submit(std::move(job));
}
quint64 AnalysisController::overview(std::shared_ptr<Recording> recording, FrameRange range,
                                     DspSettings settings, int columns, int rows)
{
    Job job;
    job.kind = Kind::Overview;
    job.waterfall.columns = columns;
    job.waterfall.rows = rows;
    job.recording = std::move(recording);
    job.range = range;
    job.settings = settings;
    return submit(std::move(job));
}
quint64 AnalysisController::samples(std::shared_ptr<Recording> recording, FrameRange range,
                                    QString output)
{
    Job job;
    job.kind = Kind::Samples;
    job.recording = std::move(recording);
    job.range = range;
    job.output = std::move(output);
    return submit(std::move(job));
}
quint64 AnalysisController::csv(CsvRequest request)
{
    Job job;
    job.kind = Kind::Csv;
    job.csv = std::move(request);
    return submit(std::move(job));
}
quint64 AnalysisController::png(PngRequest request)
{
    Job job;
    job.kind = Kind::Png;
    job.png = std::move(request);
    return submit(std::move(job));
}
void AnalysisController::cancel()
{
    const std::lock_guard lock(mutex_);
    if (activeCancellation_)
        activeCancellation_->store(true);
    if (pending_) {
        pending_->cancelled->store(true);
        pending_.reset();
    }
}

void AnalysisController::run(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stop, [this] { return pending_.has_value(); }))
                break;
            job = std::move(*pending_);
            pending_.reset();
        }
        execute(std::move(job));
    }
}

QString AnalysisController::previewKey(const Job &job) const
{
    return job.recording->identity() +
           QString::fromUtf8(QJsonDocument(recordingToJson(job.recording->descriptor()))
                                 .toJson(QJsonDocument::Compact)) +
           QString::fromUtf8(
               QJsonDocument(dspToJson(job.settings)).toJson(QJsonDocument::Compact)) +
           QString("/%1/%2").arg(job.range.begin).arg(job.range.end);
}

void AnalysisController::execute(Job job)
{
    const auto cancelled = [token = job.cancelled] { return token->load(); };
    const auto progress = [this, generation = job.generation](double fraction) {
        emit progressChanged(generation, fraction);
    };
    try {
        if (cancelled())
            throw Cancelled();
        if (job.kind == Kind::Inspect) {
            // Import inspection is independent of FFT length. Even a recording
            // shorter than 256 samples has a valid decoded-sample preview.
            job.recording = std::make_shared<Recording>(std::move(job.descriptor));
            const FrameRange range{0, std::min<std::uint64_t>(256, job.recording->frameCount())};
            const auto result = analyzeWaveform(*job.recording, range, cancelled);
            if (cancelled())
                throw Cancelled();
            emit recordingOpened(job.generation, job.recording, range);
            emit waveformReady(job.generation, result);
        } else if (job.kind == Kind::Open) {
            job.recording = std::make_shared<Recording>(std::move(job.descriptor));
            if (job.range.end == 0)
                job.range = {0, job.recording->frameCount()};
            job.recording->validateRange(job.range);
            if (cancelled())
                throw Cancelled();
            emit recordingOpened(job.generation, job.recording, job.range);
            job.kind = Kind::Preview;
        }
        if (job.kind == Kind::Preview) {
            job.recording->verifyUnchanged();
            const auto key = previewKey(job);
            for (auto entry = cache_.begin(); entry != cache_.end(); ++entry) {
                if (entry->key == key) {
                    const auto result = entry->result;
                    cache_.splice(cache_.begin(), cache_, entry);
                    if (cancelled())
                        throw Cancelled();
                    emit previewReady(job.generation, result);
                    return;
                }
            }
            const auto result = analyzePreview(*job.recording, job.range, job.settings, cancelled);
            constexpr std::size_t cacheBudget = 64U * 1024U * 1024U;
            while (!cache_.empty() && cacheBytes_ + result->memoryBytes() > cacheBudget) {
                cacheBytes_ -= cache_.back().result->memoryBytes();
                cache_.pop_back();
            }
            if (result->memoryBytes() <= cacheBudget) {
                cache_.push_front({key, result});
                cacheBytes_ += result->memoryBytes();
            }
            if (cancelled())
                throw Cancelled();
            emit previewReady(job.generation, result);
        } else if (job.kind == Kind::Average || job.kind == Kind::Overview) {
            auto overview =
                job.kind == Kind::Overview ? std::make_shared<PreviewResult>() : nullptr;
            const auto result = analyzeAverage(*job.recording, job.range, job.settings, cancelled,
                                               progress, overview.get(), job.waterfall.columns,
                                               job.waterfall.rows);
            if (cancelled())
                throw Cancelled();
            emit averageReady(job.generation, result);
            if (overview)
                emit previewReady(job.generation, overview);
        } else if (job.kind == Kind::Waterfall) {
            const auto key = previewKey(job) +
                QString("/waterfall/%1/%2/%3/%4/%5/%6")
                    .arg(job.waterfall.left, 0, 'g', 17).arg(job.waterfall.right, 0, 'g', 17)
                    .arg(job.waterfall.columns).arg(job.waterfall.rows).arg(job.waterfall.exact)
                    .arg(job.waterfall.fixedRows);
            job.recording->verifyUnchanged();
            for (auto entry = cache_.begin(); entry != cache_.end(); ++entry) {
                if (entry->key == key) {
                    const auto result = entry->result;
                    cache_.splice(cache_.begin(), cache_, entry);
                    if (cancelled())
                        throw Cancelled();
                    publishWaterfall(job.generation, result);
                    return;
                }
            }
            const auto updates = job.waterfall.exact
                ? WaterfallUpdate([this, generation = job.generation](auto result) {
                      publishWaterfall(generation, std::move(result));
                  })
                : WaterfallUpdate{};
            const auto result = analyzeWaterfall(*job.recording, job.settings, job.waterfall,
                                                cancelled, updates);
            const std::size_t cacheBudget = (job.waterfall.exact ? 4U : 64U) * 1024U * 1024U;
            while (!cache_.empty() && cacheBytes_ + result->memoryBytes() > cacheBudget) {
                cacheBytes_ -= cache_.back().result->memoryBytes();
                cache_.pop_back();
            }
            if (result->memoryBytes() <= cacheBudget) {
                cache_.push_front({key, result});
                cacheBytes_ += result->memoryBytes();
            }
            if (cancelled())
                throw Cancelled();
            publishWaterfall(job.generation, result);
        } else if (job.kind == Kind::Waveform) {
            const auto result = analyzeWaveform(*job.recording, job.range, cancelled, progress);
            if (cancelled())
                throw Cancelled();
            emit waveformReady(job.generation, result);
        } else if (job.kind == Kind::Samples) {
            exportSamples(*job.recording, job.range, job.output, cancelled, progress);
            emit finished(job.generation, "Sample range and metadata exported");
        } else if (job.kind == Kind::Csv) {
            const auto &csv = *job.csv;
            exportSpectrumCsv(csv.recording, csv.identity, csv.range, csv.settings, csv.frequencies,
                              csv.power, csv.method, csv.coverage, csv.output, cancelled, progress);
            emit finished(job.generation, "Spectrum CSV exported");
        } else if (job.kind == Kind::Png) {
            const auto &png = *job.png;
            ExportFile file(png.output);
            QImageWriter writer(&file.file(), "png");
            writer.setText(
                "CroccoSpectrum",
                QString::fromUtf8(QJsonDocument(png.metadata).toJson(QJsonDocument::Compact)));
            if (!writer.write(png.image))
                throw std::runtime_error(writer.errorString().toStdString());
            if (cancelled())
                throw Cancelled();
            file.commit();
            emit finished(job.generation, "PNG and embedded analysis metadata exported");
        }
    } catch (const Cancelled &) {
        emit finished(job.generation, "Cancelled; incomplete results discarded");
    } catch (const std::exception &error) {
        emit failed(job.generation, QString::fromUtf8(error.what()));
    }
}

void AnalysisController::publishWaterfall(quint64 generation,
                                          std::shared_ptr<const PreviewResult> result)
{
    const std::lock_guard lock(mutex_);
    latestWaterfall_ = std::move(result);
    waterfallGeneration_ = generation;
    if (waterfallNotificationPending_)
        return;
    waterfallNotificationPending_ = true;
    QMetaObject::invokeMethod(this, [this] {
        std::shared_ptr<const PreviewResult> snapshot;
        quint64 snapshotGeneration;
        {
            const std::lock_guard snapshotLock(mutex_);
            snapshot = std::move(latestWaterfall_);
            snapshotGeneration = waterfallGeneration_;
            waterfallNotificationPending_ = false;
        }
        emit waterfallReady(snapshotGeneration, std::move(snapshot));
    }, Qt::QueuedConnection);
}
} // namespace rf
