#include "Spectrum.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <numbers>
#include <stdexcept>

namespace rf
{
namespace
{
std::mutex plannerMutex;
}

void DspSettings::validate(double sampleRate) const
{
    if (fftSize < 256 || fftSize > 1'048'576 || (fftSize & (fftSize - 1)) != 0)
        throw std::runtime_error("FFT length must be a power of two from 256 to 1048576");
    if (overlapPercent < 0 || overlapPercent > 95)
        throw std::runtime_error("Overlap must be from 0 to 95 percent");
    if (!std::isfinite(sampleRate) || sampleRate <= 0 ||
        !std::isfinite(static_cast<double>(fftSize) / sampleRate) ||
        sampleRate / static_cast<double>(fftSize) == 0)
        throw std::runtime_error(
            "FFT time/frequency resolution is not representable at this sample rate");
    if (window < Window::Hann || window > Window::BlackmanHarris || scale < PowerScale::Spectrum ||
        scale > PowerScale::Density)
        throw std::runtime_error("Unknown DSP setting");
}

std::uint64_t DspSettings::hop() const
{
    return static_cast<std::uint64_t>(fftSize) * static_cast<unsigned int>(100 - overlapPercent) /
           100U;
}

QString windowName(Window window)
{
    switch (window) {
    case Window::Hann:
        return "Hann";
    case Window::Rectangular:
        return "Rectangular";
    case Window::Hamming:
        return "Hamming";
    case Window::BlackmanHarris:
        return "Blackman-Harris";
    }
    return "Unknown";
}

QString powerUnit(PowerScale scale)
{
    return scale == PowerScale::Density ? "dBFS/Hz" : "dBFS";
}

double powerToDb(double power)
{
    // Zero is negative infinity mathematically. A rendering floor is applied
    // in the painter, not here, so CSV data does not fabricate a noise floor.
    return power > 0 ? 10.0 * std::log10(power) : -std::numeric_limits<double>::infinity();
}

SpectrumEngine::SpectrumEngine(SampleKind kind, double sampleRate, DspSettings settings)
    : kind_(kind), sampleRate_(sampleRate), settings_(settings)
{
    settings_.validate(sampleRate_);
    const auto length = static_cast<std::size_t>(settings_.fftSize);
    window_.resize(length);
    double sum = 0;
    double sumSquares = 0;
    for (std::size_t index = 0; index < length; ++index) {
        // Periodic windows (denominator N) match spectral-analysis references;
        // symmetric N-1 windows serve a different purpose in filter design.
        const double angle =
            2.0 * std::numbers::pi * static_cast<double>(index) / static_cast<double>(length);
        double value = 1;
        switch (settings_.window) {
        case Window::Hann:
            value = 0.5 - 0.5 * std::cos(angle);
            break;
        case Window::Rectangular:
            break;
        case Window::Hamming:
            value = 0.54 - 0.46 * std::cos(angle);
            break;
        case Window::BlackmanHarris:
            value = 0.35875 - 0.48829 * std::cos(angle) + 0.14128 * std::cos(2 * angle) -
                    0.01168 * std::cos(3 * angle);
            break;
        }
        window_[index] = value;
        sum += value;
        sumSquares += value * value;
    }
    enbwHz_ = sampleRate_ * (sumSquares / (sum * sum));
    normalization_ = settings_.scale == PowerScale::Spectrum ? sum * sum : sampleRate_ * sumSquares;
    if (!std::isfinite(normalization_) || normalization_ <= 0 || !std::isfinite(enbwHz_))
        throw std::runtime_error("Spectral normalization overflows at this sample rate");

    // ESTIMATE does not benchmark or overwrite samples and keeps first-open
    // planning latency predictable. The mutex also covers fftw_free below.
    const std::lock_guard lock(plannerMutex);
    output_ = fftw_alloc_complex(binCount());
    if (kind_ == SampleKind::Complex)
        complexInput_ = fftw_alloc_complex(length);
    else
        realInput_ = fftw_alloc_real(length);
    if (output_ && (complexInput_ || realInput_)) {
        plan_ = kind_ == SampleKind::Complex
                    ? fftw_plan_dft_1d(settings_.fftSize, complexInput_, output_, FFTW_FORWARD,
                                       FFTW_ESTIMATE)
                    : fftw_plan_dft_r2c_1d(settings_.fftSize, realInput_, output_, FFTW_ESTIMATE);
    }
    if (!plan_) {
        fftw_free(complexInput_);
        fftw_free(realInput_);
        fftw_free(output_);
        throw std::runtime_error("Unable to allocate FFT buffers/plan");
    }
}

SpectrumEngine::~SpectrumEngine()
{
    const std::lock_guard lock(plannerMutex);
    fftw_destroy_plan(plan_);
    fftw_free(complexInput_);
    fftw_free(realInput_);
    fftw_free(output_);
}

std::size_t SpectrumEngine::binCount() const
{
    return kind_ == SampleKind::Complex ? static_cast<std::size_t>(settings_.fftSize)
                                        : static_cast<std::size_t>(settings_.fftSize / 2 + 1);
}

SpectrumResult SpectrumEngine::calculate(std::span<const std::complex<double>> samples)
{
    if (samples.size() != window_.size())
        throw std::runtime_error("FFT requires exactly N samples");
    SpectrumResult result;
    result.enbwHz = enbwHz_;
    std::complex<double> mean = 0;
    for (const auto &sample : samples) {
        if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) {
            result.valid = false;
            return result;
        }
        if (settings_.removeDc)
            mean += sample / static_cast<double>(samples.size());
    }
    for (std::size_t index = 0; index < samples.size(); ++index) {
        auto sample = (samples[index] - mean) * window_[index];
        if (settings_.conjugate)
            sample = std::conj(sample);
        if (kind_ == SampleKind::Complex) {
            complexInput_[index][0] = sample.real();
            complexInput_[index][1] = sample.imag();
        } else
            realInput_[index] = sample.real();
    }
    fftw_execute(plan_);
    result.power.resize(binCount());
    const double divisor = std::sqrt(normalization_);
    for (std::size_t index = 0; index < result.power.size(); ++index) {
        const auto source =
            kind_ == SampleKind::Complex ? (index + samples.size() / 2) % samples.size() : index;
        const double re = output_[source][0];
        const double im = output_[source][1];
        // Ordinary normalized ADC values need only two squares and a divide.
        // Profiling found per-bin hypot was a major exhaustive-scan cost.
        // Fall back to scaled hypot for extreme floating inputs so premature
        // square overflow/underflow cannot corrupt otherwise representable PSD.
        const double component = std::max(std::abs(re), std::abs(im));
        double power = 0;
        if (component >= 1e-150 && component <= 1e150)
            power = (re * re + im * im) / normalization_;
        else {
            const double magnitude = std::hypot(re, im) / divisor;
            power = magnitude * magnitude;
        }
        if (kind_ == SampleKind::Real && index != 0 && index + 1 != result.power.size())
            power *= 2;
        if (!std::isfinite(power)) {
            result.valid = false;
            result.power.clear();
            return result;
        }
        result.power[index] = power;
    }
    return result;
}

std::vector<double> SpectrumEngine::frequencies(double center) const
{
    std::vector<double> frequencies(binCount());
    const double spacing = sampleRate_ / settings_.fftSize;
    for (std::size_t index = 0; index < frequencies.size(); ++index) {
        const auto bin = static_cast<std::int64_t>(index) -
                         (kind_ == SampleKind::Complex ? settings_.fftSize / 2 : 0);
        frequencies[index] = center + static_cast<double>(bin) * spacing;
    }
    return frequencies;
}
} // namespace rf
