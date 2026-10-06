#pragma once

#include <QString>
#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rf
{

enum class SampleKind { Real, Complex };
enum class Encoding { SignedInteger, UnsignedInteger, FloatingPoint };
enum class ByteOrder { LittleEndian, BigEndian };
enum class ComponentOrder { IQ, QI };

// A frame is one instant: one scalar for real data or an I/Q pair for complex
// data. All offsets/ranges in the DSP API use frames, never component indices.
struct SampleFormat {
    SampleKind kind = SampleKind::Complex;
    Encoding encoding = Encoding::SignedInteger;
    int bits = 16;
    ByteOrder byteOrder = ByteOrder::LittleEndian;
    ComponentOrder componentOrder = ComponentOrder::IQ;
    double floatFullScale = 1.0;
    [[nodiscard]] std::uint64_t frameBytes() const;
    void validate() const;
    bool operator==(const SampleFormat &) const = default;
};

struct Capture {
    std::uint64_t start = 0;
    std::optional<double> frequency;
    QString datetime;
};

struct Annotation {
    std::uint64_t start = 0;
    std::uint64_t count = 0;
    QString label;
};

struct RecordingDescriptor {
    QString path;
    SampleFormat format;
    double sampleRate = 100'000'000.0;
    std::optional<double> centerFrequency;
    QString startUtc; // Optional ISO-8601 timestamp with an explicit UTC offset.
    std::uint64_t dataOffset = 0;
    std::optional<std::uint64_t> dataBytes;
    QString metadataSource = "User import";
    std::vector<Capture> captures;
    std::vector<Annotation> annotations;
    void validate() const;
    [[nodiscard]] double frequencyAt(std::uint64_t frame) const;
    [[nodiscard]] bool hasFrequencyAt(std::uint64_t frame) const;
    [[nodiscard]] bool crossesCapture(std::uint64_t start, std::uint64_t count) const;
    [[nodiscard]] QString timestampAt(std::uint64_t frame) const;
};

// Closed-open ranges make adjacency, complete-window counting, and exports
// unambiguous. They also avoid end+1 overflowing near the integer limit.
struct FrameRange {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    [[nodiscard]] std::uint64_t size() const;
    bool operator==(const FrameRange &) const = default;
};

// Decoder is pure: fixtures can test it without a filesystem or GUI. Nonfinite
// floating values remain nonfinite so callers can mark invalid measurements.
std::vector<std::complex<double>> decodeSamples(std::span<const std::byte> bytes,
                                                const SampleFormat &format);

// Linux positional reads avoid shared seek state. No mmap: replacing/truncating
// a recording should produce a controlled exception rather than SIGBUS.
class Recording
{
  public:
    explicit Recording(RecordingDescriptor descriptor);
    ~Recording();
    Recording(const Recording &) = delete;
    Recording &operator=(const Recording &) = delete;
    [[nodiscard]] const RecordingDescriptor &descriptor() const { return descriptor_; }
    [[nodiscard]] std::uint64_t frameCount() const { return frames_; }
    [[nodiscard]] std::uint64_t fileSize() const { return fileSize_; }
    [[nodiscard]] QString identity() const;
    void verifyUnchanged() const;
    [[nodiscard]] std::vector<std::byte> readBytes(FrameRange range) const;
    [[nodiscard]] std::vector<std::complex<double>> read(FrameRange range) const;
    void validateRange(FrameRange range) const;

  private:
    RecordingDescriptor descriptor_;
    int fd_ = -1;
    std::uint64_t frames_ = 0;
    std::uint64_t fileSize_ = 0;
    std::uint64_t device_ = 0;
    std::uint64_t inode_ = 0;
    std::int64_t modifiedSeconds_ = 0;
    std::int64_t modifiedNanos_ = 0;
    std::int64_t changedSeconds_ = 0;
    std::int64_t changedNanos_ = 0;
};

} // namespace rf
