#pragma once

#include "recording/Recording.h"
#include <fftw3.h>
#include <span>
#include <vector>

namespace rf
{
enum class Window { Hann, Rectangular, Hamming, BlackmanHarris };
enum class PowerScale { Spectrum, Density };

struct DspSettings {
    int fftSize = 4096;
    Window window = Window::Hann;
    int overlapPercent = 50;
    PowerScale scale = PowerScale::Spectrum;
    bool removeDc = false;
    bool conjugate = false;
    void validate(double sampleRate) const;
    [[nodiscard]] std::uint64_t hop() const;
    bool operator==(const DspSettings &) const = default;
};

QString windowName(Window window);
QString powerUnit(PowerScale scale);
double powerToDb(double power);

struct SpectrumResult {
    std::vector<double> power; // Linear power; average before taking logarithms.
    bool valid = true;
    double enbwHz = 0;
};

// Each job owns an engine and its buffers. FFTW execution is safe concurrently,
// but all planning/destruction is protected by the process-wide planner mutex.
class SpectrumEngine
{
  public:
    SpectrumEngine(SampleKind kind, double sampleRate, DspSettings settings);
    ~SpectrumEngine();
    SpectrumEngine(const SpectrumEngine &) = delete;
    SpectrumEngine &operator=(const SpectrumEngine &) = delete;
    [[nodiscard]] SpectrumResult calculate(std::span<const std::complex<double>> samples);
    [[nodiscard]] std::vector<double> frequencies(double center = 0) const;
    [[nodiscard]] double enbwHz() const { return enbwHz_; }
    [[nodiscard]] std::size_t binCount() const;

  private:
    SampleKind kind_;
    double sampleRate_;
    DspSettings settings_;
    std::vector<double> window_;
    double normalization_ = 0;
    double enbwHz_ = 0;
    fftw_complex *complexInput_ = nullptr;
    double *realInput_ = nullptr;
    fftw_complex *output_ = nullptr;
    fftw_plan plan_ = nullptr;
};
} // namespace rf
