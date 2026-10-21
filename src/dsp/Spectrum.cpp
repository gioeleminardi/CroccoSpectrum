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

std::vector<SpectralPeak> findSpectralPeaks(std::span<const double> frequencies,
                                         std::span<const double> powers, double lowerHz,
                                         double upperHz, Window window)
{
    std::vector<SpectralPeak> peaks;
    if (frequencies.size() != powers.size() || powers.size() < 2 ||
        !std::isfinite(lowerHz) || !std::isfinite(upperHz) || lowerHz > upperHz)
        return peaks;
    const double spacing = frequencies[1] - frequencies[0];
    if (!std::isfinite(spacing) || spacing <= 0)
        return peaks;
    const auto first = static_cast<std::size_t>(
        std::lower_bound(frequencies.begin(), frequencies.end(), lowerHz) - frequencies.begin());
    const auto end = static_cast<std::size_t>(
        std::upper_bound(frequencies.begin(), frequencies.end(), upperHz) - frequencies.begin());
    std::vector<double> background;
    background.reserve(end - first);
    const auto localMaximum = [powers](std::size_t bin) {
        const double value = powers[bin];
        // Keep one representative of an equal two-bin peak. Wider flat tops
        // have no unique peak and are omitted. Range edges use full-trace neighbors.
        return std::isfinite(value) && value > 0 &&
               (bin == 0 || value > powers[bin - 1]) &&
               (bin + 1 == powers.size() || value > powers[bin + 1] ||
                (value == powers[bin + 1] && bin + 2 < powers.size() &&
                 value > powers[bin + 2]));
    };
    for (auto bin = first; bin < end; ++bin) {
        if (std::isfinite(powers[bin]) && powers[bin] >= 0)
            background.push_back(powers[bin]);
        if (!localMaximum(bin))
            continue;
        SpectralPeak peak{bin, frequencies[bin], powers[bin], {}, {}};
        const auto position = std::find_if(peaks.begin(), peaks.end(), [&peak](const auto &other) {
            return peak.power > other.power;
        });
        peaks.insert(position, peak);
        if (peaks.size() > 12)
            peaks.pop_back();
    }
    if (background.empty())
        return peaks;
    const auto middle = background.begin() + static_cast<std::ptrdiff_t>(background.size() / 2);
    std::nth_element(background.begin(), middle, background.end());
    const double floor = *middle;
    // Conservative main-lobe separation guard, in bins. It cannot detect two
    // carriers that have already merged into one local maximum.
    const std::size_t separation = window == Window::Rectangular ? 2
                                 : window == Window::BlackmanHarris ? 8 : 4;
    for (auto &peak : peaks) {
        const auto bin = peak.bin;
        if (bin == 0 || bin + 1 == powers.size()) {
            peak.limitation = "FFT boundary";
            continue;
        }
        if (background.size() < 5 || (floor > 0 && powerToDb(peak.power) - powerToDb(floor) < 15)) {
            peak.limitation = "Weak / insufficient background";
            continue;
        }
        bool nearby = false;
        for (auto other = bin > separation ? bin - separation + 1 : 0;
             other < std::min(powers.size(), bin + separation); ++other)
            if (other != bin && localMaximum(other) &&
                powerToDb(powers[other]) >= powerToDb(peak.power) - 20)
                nearby = true;
        if (nearby) {
            peak.limitation = "Nearby peak / possibly unresolved";
            continue;
        }
        if (!std::isfinite(powers[bin - 1]) || !std::isfinite(powers[bin + 1]) ||
            powers[bin - 1] <= 0 || powers[bin + 1] <= 0) {
            peak.limitation = "Missing positive neighbors";
            continue;
        }
        // Julius O. Smith, Quadratic Interpolation of Spectral Peaks:
        // p = (alpha - gamma) / (2 * (alpha - 2*beta + gamma)).
        const double left = powerToDb(powers[bin - 1]) - powerToDb(peak.power);
        const double right = powerToDb(powers[bin + 1]) - powerToDb(peak.power);
        const double curvature = left + right;
        const double offset = 0.5 * (left - right) / curvature;
        if (curvature < 0 && std::isfinite(offset) && std::abs(offset) <= 0.5) {
            peak.estimatedHz = peak.frequencyHz + offset * spacing;
            peak.limitation = "Approximate";
        } else
            peak.limitation = "Flat / invalid fit";
    }
    return peaks;
}

std::optional<BandPower> integrateBand(std::span<const double> frequencies,
                                     std::span<const double> powers, PowerScale scale,
                                     double enbwHz, std::size_t first, std::size_t last)
{
    if (first > last)
        std::swap(first, last);
    if (frequencies.size() < 2 || frequencies.size() != powers.size() || last >= powers.size())
        return {};
    const double spacing = frequencies[1] - frequencies[0];
    if (!std::isfinite(spacing) || spacing <= 0 ||
        (scale == PowerScale::Spectrum && (!std::isfinite(enbwHz) || enbwHz <= 0)))
        return {};
    // Spectrum bins have coherent-gain normalization. Divide by ENBW to get
    // PSD, then integrate using the FFT spacing, including DC/Nyquist once.
    const double factor = scale == PowerScale::Density ? spacing : spacing / enbwHz;
    double total = 0;
    for (auto bin = first; bin <= last; ++bin) {
        if (!std::isfinite(powers[bin]) || powers[bin] < 0)
            return {};
        total += powers[bin] * factor;
    }
    const double bandwidth = static_cast<double>(last - first + 1) * spacing;
    if (!std::isfinite(total) || !std::isfinite(bandwidth) ||
        !std::isfinite(frequencies[first]) || !std::isfinite(frequencies[last]))
        return {};
    return BandPower{first, last, frequencies[first], frequencies[last], bandwidth, total};
}

std::optional<OccupiedBandwidth> occupiedBandwidth99(std::span<const double> frequencies,
                                                    std::span<const double> powers,
                                                    PowerScale scale, double enbwHz,
                                                    std::size_t first, std::size_t last)
{
    const auto band = integrateBand(frequencies, powers, scale, enbwHz, first, last);
    if (!band || band->power <= 0)
        return {};
    const double spacing = frequencies[1] - frequencies[0];
    const double factor = scale == PowerScale::Density ? spacing : spacing / enbwHz;
    const double tail = band->power * 0.005;
    const auto edge = [&](bool upper) {
        double excluded = 0;
        for (std::size_t offset = 0; offset <= band->lastBin - band->firstBin; ++offset) {
            const auto bin = upper ? band->lastBin - offset : band->firstBin + offset;
            const double mass = powers[bin] * factor;
            if (mass > 0 && excluded + mass >= tail) {
                const double fraction = std::clamp((tail - excluded) / mass, 0.0, 1.0);
                return frequencies[bin] + (upper ? 0.5 - fraction : fraction - 0.5) * spacing;
            }
            excluded += mass;
        }
        return std::numeric_limits<double>::quiet_NaN();
    };
    // Accumulate each small tail from its own edge to avoid subtracting nearly
    // equal total powers when locating the upper crossing.
    const double lower = edge(false), upper = edge(true);
    if (!std::isfinite(lower) || !std::isfinite(upper) || !std::isfinite(upper - lower))
        return {};
    return OccupiedBandwidth{lower, upper, upper - lower};
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
