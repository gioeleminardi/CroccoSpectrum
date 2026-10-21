#pragma once
#include "Analysis.h"
#include <QJsonArray>

namespace rf
{
// Average bins only: max hold and sampled previews are not bank measurements.
struct SavedSpectrum {
    QString name;
    bool visible = true;
    RecordingDescriptor recording;
    QString sourceIdentity;
    DspSettings dsp;
    std::shared_ptr<const AverageResult> average;
};

constexpr std::size_t spectrumBankBytes = 64 * 1024 * 1024;
constexpr qint64 spectrumBankJsonBytes = 96 * 1024 * 1024;
bool spectrumCompatible(const SavedSpectrum &trace, const RecordingDescriptor &recording,
                        const QString &identity, const DspSettings &dsp);
bool spectraCompatible(const SavedSpectrum &first, const SavedSpectrum &second);
// Undefined zero/zero bins are NaN; a zero denominator/numerator gives +/- infinity.
std::vector<double> spectrumDifference(const SavedSpectrum &first, const SavedSpectrum &second);
void validateSpectrumBank(const std::vector<SavedSpectrum> &bank);
QJsonArray spectrumBankToJson(const std::vector<SavedSpectrum> &bank,
                              const CancelCheck &cancelled = {});
std::vector<SavedSpectrum> spectrumBankFromJson(const QJsonArray &array,
                                               const CancelCheck &cancelled = {});
SavedSpectrum readSpectrumTrace(const QString &path, const CancelCheck &cancelled = {});
void exportSpectrumTrace(const QString &path, const SavedSpectrum &trace,
                         const CancelCheck &cancelled = {});
} // namespace rf
