#include "SpectrumBank.h"
#include "ExportFile.h"
#include "Session.h"
#include "recording/Metadata.h"
#include <QDataStream>
#include <QJsonDocument>
#include <cmath>
#include <limits>

namespace rf
{
namespace
{
std::vector<double> frequencyGrid(const RecordingDescriptor &recording, const DspSettings &dsp)
{
    const bool real = recording.format.kind == SampleKind::Real;
    const auto bins = static_cast<std::size_t>(real ? dsp.fftSize / 2 + 1 : dsp.fftSize);
    const double spacing = recording.sampleRate / dsp.fftSize;
    std::vector<double> frequencies(bins);
    for (std::size_t bin = 0; bin < bins; ++bin)
        frequencies[bin] = (static_cast<double>(bin) - (real ? 0 : dsp.fftSize / 2)) * spacing;
    return frequencies;
}

QJsonObject interpretation(const RecordingDescriptor &recording)
{
    auto object = recordingToJson(recording);
    for (const auto *field : {"path", "metadata_source", "annotations", "start_utc"})
        object.remove(field);
    return object;
}
} // namespace

bool spectrumCompatible(const SavedSpectrum &trace, const RecordingDescriptor &recording,
                        const QString &identity, const DspSettings &dsp)
{
    return !identity.isEmpty() && trace.sourceIdentity == identity && trace.dsp == dsp &&
           interpretation(trace.recording) == interpretation(recording);
}

bool spectraCompatible(const SavedSpectrum &first, const SavedSpectrum &second)
{
    return spectrumCompatible(first, second.recording, second.sourceIdentity, second.dsp) &&
           first.average && second.average &&
           !first.average->frequencies.empty() &&
           first.average->averagePower.size() == first.average->frequencies.size() &&
           second.average->averagePower.size() == second.average->frequencies.size() &&
           first.average->frequencies == second.average->frequencies;
}

std::vector<double> spectrumDifference(const SavedSpectrum &first, const SavedSpectrum &second)
{
    if (!spectraCompatible(first, second))
        throw std::runtime_error(
            "Spectrum difference requires the same recording and DSP settings");
    std::vector<double> difference(first.average->averagePower.size());
    for (std::size_t bin = 0; bin < difference.size(); ++bin) {
        const double a = first.average->averagePower[bin];
        const double b = second.average->averagePower[bin];
        difference[bin] = a == 0 && b == 0 ? std::numeric_limits<double>::quiet_NaN()
                                         : powerToDb(a) - powerToDb(b);
    }
    return difference;
}

void validateSpectrumBank(const std::vector<SavedSpectrum> &bank)
{
    std::size_t bytes = 0;
    for (const auto &trace : bank) {
        trace.recording.validate();
        trace.dsp.validate(trace.recording.sampleRate);
        if (trace.name.trimmed().isEmpty() || trace.sourceIdentity.isEmpty() || !trace.average)
            throw std::runtime_error("Saved spectrum requires a name, source identity and average");
        const auto &average = *trace.average;
        const auto bins = static_cast<std::size_t>(trace.recording.format.kind == SampleKind::Real
                                                      ? trace.dsp.fftSize / 2 + 1
                                                      : trace.dsp.fftSize);
        if (bins * 2 * sizeof(double) > spectrumBankBytes - bytes)
            throw std::runtime_error("Spectrum bank exceeds the 64 MiB bin-storage limit");
        bytes += bins * 2 * sizeof(double);
        if (average.averagePower.size() != bins ||
            average.frequencies != frequencyGrid(trace.recording, trace.dsp) ||
            average.range.size() < static_cast<std::uint64_t>(trace.dsp.fftSize) ||
            average.validWindows == 0 || !std::isfinite(average.enbwHz) || average.enbwHz <= 0)
            throw std::runtime_error("Invalid saved average bins, interval or ENBW");
        const auto remaining = average.range.size() - static_cast<std::uint64_t>(trace.dsp.fftSize);
        const auto windows = remaining / trace.dsp.hop() + 1;
        if (average.validWindows > windows ||
            average.invalidWindows > windows - average.validWindows ||
            average.boundaryWindows != windows - average.validWindows - average.invalidWindows ||
            average.trailingFrames != remaining % trace.dsp.hop())
            throw std::runtime_error("Invalid saved average coverage");
        for (const auto power : average.averagePower)
            if (!std::isfinite(power) || power < 0)
                throw std::runtime_error("Saved average powers must be finite and nonnegative");
    }
}

QJsonArray spectrumBankToJson(const std::vector<SavedSpectrum> &bank, const CancelCheck &cancelled)
{
    validateSpectrumBank(bank);
    QJsonArray array;
    for (const auto &trace : bank) {
        if (cancelled && cancelled())
            throw Cancelled();
        const auto &average = *trace.average;
        QByteArray powers;
        QDataStream stream(&powers, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::LittleEndian);
        for (std::size_t bin = 0; bin < average.averagePower.size(); ++bin) {
            if (bin % 4096 == 0 && cancelled && cancelled())
                throw Cancelled();
            stream << average.averagePower[bin];
        }
        array.append(QJsonObject{{"name", trace.name}, {"visible", trace.visible},
                                 {"recording", recordingToJson(trace.recording)},
                                 {"source_identity", trace.sourceIdentity},
                                 {"dsp", dspToJson(trace.dsp)},
                                 {"start", QString::number(average.range.begin)},
                                 {"end", QString::number(average.range.end)},
                                 {"valid_windows", QString::number(average.validWindows)},
                                 {"invalid_windows", QString::number(average.invalidWindows)},
                                 {"boundary_windows", QString::number(average.boundaryWindows)},
                                 {"trailing_frames", QString::number(average.trailingFrames)},
                                 {"enbw_hz", average.enbwHz},
                                 {"power_f64_le", QString::fromLatin1(powers.toBase64())}});
    }
    return array;
}

std::vector<SavedSpectrum> spectrumBankFromJson(const QJsonArray &array,
                                               const CancelCheck &cancelled)
{
    std::vector<SavedSpectrum> bank;
    std::size_t bytes = 0;
    for (const auto value : array) {
        if (cancelled && cancelled())
            throw Cancelled();
        const auto object = value.toObject();
        if (!object["name"].isString() || !object["visible"].isBool() ||
            !object["dsp"].isObject() || !object["power_f64_le"].isString() ||
            !object["enbw_hz"].isDouble())
            throw std::runtime_error("Invalid saved spectrum metadata");
        const auto settings = object["dsp"].toObject();
        for (const auto *field : {"fft_size", "window", "overlap_percent", "scale",
                                  "remove_dc", "conjugate"})
            if (!settings.contains(field))
                throw std::runtime_error("Missing saved spectrum DSP setting");
        SavedSpectrum trace;
        trace.name = object["name"].toString();
        trace.visible = object["visible"].toBool();
        trace.recording = recordingFromJson(object["recording"].toObject());
        trace.sourceIdentity = object["source_identity"].toString();
        trace.dsp = dspFromJson(object["dsp"].toObject());
        const auto bins = static_cast<std::size_t>(trace.recording.format.kind == SampleKind::Real
                                                      ? trace.dsp.fftSize / 2 + 1
                                                      : trace.dsp.fftSize);
        if (bins * 2 * sizeof(double) > spectrumBankBytes - bytes)
            throw std::runtime_error("Spectrum bank exceeds the 64 MiB bin-storage limit");
        bytes += bins * 2 * sizeof(double);
        const auto encoded = object["power_f64_le"].toString().toLatin1();
        const auto binaryBytes = static_cast<qsizetype>(bins * sizeof(double));
        if (encoded.size() != (binaryBytes + 2) / 3 * 4)
            throw std::runtime_error("Saved spectrum power array has the wrong length");
        const auto decoded =
            QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.size() != binaryBytes)
            throw std::runtime_error("Invalid saved spectrum power encoding");
        auto average = std::make_shared<AverageResult>();
        average->range = {jsonUnsigned(object["start"], "trace start"),
                          jsonUnsigned(object["end"], "trace end")};
        average->validWindows = jsonUnsigned(object["valid_windows"], "valid windows");
        average->invalidWindows = jsonUnsigned(object["invalid_windows"], "invalid windows");
        average->boundaryWindows = jsonUnsigned(object["boundary_windows"], "boundary windows");
        average->trailingFrames = jsonUnsigned(object["trailing_frames"], "trailing frames");
        average->enbwHz = object["enbw_hz"].toDouble();
        average->frequencies = frequencyGrid(trace.recording, trace.dsp);
        average->averagePower.resize(bins);
        QDataStream stream(decoded.decoded);
        stream.setByteOrder(QDataStream::LittleEndian);
        for (std::size_t bin = 0; bin < bins; ++bin) {
            if (bin % 4096 == 0 && cancelled && cancelled())
                throw Cancelled();
            stream >> average->averagePower[bin];
        }
        trace.average = std::move(average);
        bank.push_back(std::move(trace));
    }
    validateSpectrumBank(bank);
    return bank;
}

SavedSpectrum readSpectrumTrace(const QString &path, const CancelCheck &cancelled)
{
    if (cancelled && cancelled())
        throw Cancelled();
    const auto root = readJsonObject(path, spectrumBankJsonBytes);
    if (root["schema"] != 1 || root["type"] != "spectrum_trace" || !root["traces"].isArray() ||
        root["traces"].toArray().size() != 1)
        throw std::runtime_error("Unsupported spectrum trace format");
    return spectrumBankFromJson(root["traces"].toArray(), cancelled).front();
}

void exportSpectrumTrace(const QString &path, const SavedSpectrum &trace,
                         const CancelCheck &cancelled)
{
    const QJsonObject root{{"schema", 1}, {"type", "spectrum_trace"},
                           {"traces", spectrumBankToJson({trace}, cancelled)}};
    const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (bytes.size() > spectrumBankJsonBytes)
        throw std::runtime_error("Spectrum trace exceeds the JSON storage limit");
    ExportFile output(path);
    if (output.file().write(bytes) != bytes.size())
        throw std::runtime_error("Could not write spectrum trace");
    if (cancelled && cancelled())
        throw Cancelled();
    output.commit();
}
} // namespace rf
