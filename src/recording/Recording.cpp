#include "Recording.h"

#include <QDateTime>
#include <QFile>
#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace rf
{
namespace
{
[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

std::uint64_t loadUnsigned(const std::byte *data, int bytes, ByteOrder order)
{
    std::uint64_t value = 0;
    for (int index = 0; index < bytes; ++index) {
        const int source = order == ByteOrder::LittleEndian ? bytes - index - 1 : index;
        value = (value << 8U) | std::to_integer<unsigned char>(data[source]);
    }
    return value;
}

double halfToDouble(std::uint16_t bits)
{
    // Convert binary16 mathematically; this works on CPUs without FP16 support
    // and preserves signed zero, subnormals, infinity, and NaN semantics.
    const double sign = (bits & 0x8000U) != 0 ? -1.0 : 1.0;
    const int exponent = (bits >> 10U) & 0x1fU;
    const int fraction = bits & 0x3ffU;
    if (exponent == 31) {
        return fraction == 0 ? sign * std::numeric_limits<double>::infinity()
                             : std::numeric_limits<double>::quiet_NaN();
    }
    if (exponent == 0)
        return sign * std::ldexp(static_cast<double>(fraction), -24);
    return sign * std::ldexp(1.0 + static_cast<double>(fraction) / 1024.0, exponent - 15);
}

double scalar(const std::byte *bytes, const SampleFormat &format)
{
    const std::uint64_t raw = loadUnsigned(bytes, format.bits / 8, format.byteOrder);
    if (format.encoding == Encoding::FloatingPoint) {
        double value = 0;
        if (format.bits == 16)
            value = halfToDouble(static_cast<std::uint16_t>(raw));
        else if (format.bits == 32)
            value = std::bit_cast<float>(static_cast<std::uint32_t>(raw));
        else
            value = std::bit_cast<double>(raw);
        return value / format.floatFullScale;
    }

    const double midpoint = std::ldexp(1.0, format.bits - 1);
    if (format.encoding == Encoding::UnsignedInteger) {
        // Center in the integer domain before converting. For unsigned 64-bit
        // offset binary, casting first would round midpoint +/- 1 to zero.
        const auto center = std::uint64_t{1} << (format.bits - 1);
        return raw >= center ? static_cast<double>(raw - center) / midpoint
                             : -static_cast<double>(center - raw) / midpoint;
    }

    // Subtract in floating point instead of sign-extending with signed shifts
    // or negating INT64_MIN, both easy sources of undefined behavior.
    const bool negative = (raw & (std::uint64_t{1} << (format.bits - 1))) != 0;
    if (!negative)
        return static_cast<double>(raw) / midpoint;
    // Magnitude via two's complement also avoids catastrophic cancellation for
    // -1 in a 64-bit word. Double cannot preserve all 64-bit integer steps.
    const std::uint64_t mask =
        format.bits == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << format.bits) - 1;
    const std::uint64_t magnitude = ((~raw) & mask) + 1;
    return -static_cast<double>(magnitude) / midpoint;
}
} // namespace

void SampleFormat::validate() const
{
    if (kind < SampleKind::Real || kind > SampleKind::Complex ||
        encoding < Encoding::SignedInteger || encoding > Encoding::FloatingPoint ||
        byteOrder < ByteOrder::LittleEndian || byteOrder > ByteOrder::BigEndian ||
        componentOrder < ComponentOrder::IQ || componentOrder > ComponentOrder::QI)
        fail("Unknown sample format");
    const bool integerWidth = bits == 8 || bits == 16 || bits == 24 || bits == 32 || bits == 64;
    const bool floatWidth = bits == 16 || bits == 32 || bits == 64;
    if ((encoding == Encoding::FloatingPoint && !floatWidth) ||
        (encoding != Encoding::FloatingPoint && !integerWidth))
        fail("Unsupported scalar width");
    if (!std::isfinite(floatFullScale) || floatFullScale <= 0)
        fail("Full scale must be finite and positive");
}

std::uint64_t SampleFormat::frameBytes() const
{
    validate();
    return static_cast<std::uint64_t>(bits / 8) * (kind == SampleKind::Complex ? 2U : 1U);
}

void RecordingDescriptor::validate() const
{
    format.validate();
    if (path.isEmpty())
        fail("Recording path is empty");
    if (!std::isfinite(sampleRate) || sampleRate <= 0 || !std::isfinite(1.0 / sampleRate))
        fail("Sample rate must be finite, positive, and numerically representable");
    if (centerFrequency && !std::isfinite(*centerFrequency))
        fail("Invalid center frequency");
    const auto validateTime = [](const QString &text) {
        if (text.isEmpty())
            return;
        const auto date = QDateTime::fromString(text, Qt::ISODateWithMs);
        if (!date.isValid() || date.timeSpec() == Qt::LocalTime)
            fail("Capture time must be ISO-8601 with an explicit UTC offset");
    };
    validateTime(startUtc);
    std::uint64_t previous = 0;
    for (std::size_t index = 0; index < captures.size(); ++index) {
        const auto &capture = captures[index];
        if (index != 0 && capture.start <= previous)
            fail("Capture starts must be strictly increasing");
        if (capture.frequency && !std::isfinite(*capture.frequency))
            fail("Invalid capture frequency");
        validateTime(capture.datetime);
        previous = capture.start;
    }
}

double RecordingDescriptor::frequencyAt(std::uint64_t frame) const
{
    double frequency = centerFrequency.value_or(0.0);
    for (const auto &capture : captures) {
        if (capture.start > frame)
            break;
        // SigMF capture values do not implicitly carry into the next capture.
        frequency = capture.frequency.value_or(centerFrequency.value_or(0.0));
    }
    return frequency;
}

bool RecordingDescriptor::hasFrequencyAt(std::uint64_t frame) const
{
    bool known = centerFrequency.has_value();
    for (const auto &capture : captures) {
        if (capture.start > frame)
            break;
        known = capture.frequency.has_value() || centerFrequency.has_value();
    }
    return known;
}

bool RecordingDescriptor::crossesCapture(std::uint64_t start, std::uint64_t count) const
{
    for (const auto &capture : captures)
        if (capture.start > start && capture.start - start < count)
            return true;
    return false;
}

QString RecordingDescriptor::timestampAt(std::uint64_t frame) const
{
    QString origin = startUtc;
    std::uint64_t originFrame = 0;
    for (const auto &capture : captures) {
        if (capture.start > frame)
            break;
        // A boundary without a timestamp cannot establish continuity across
        // a recorder gap. Retain no inferred wall-clock time for that segment.
        origin = capture.datetime;
        originFrame = capture.start;
    }
    if (origin.isEmpty())
        return {};
    const long double milliseconds =
        static_cast<long double>(frame - originFrame) * 1000 / sampleRate;
    const auto date = QDateTime::fromString(origin, Qt::ISODateWithMs).toUTC();
    if (milliseconds > static_cast<long double>(std::numeric_limits<qint64>::max()) -
                           std::max<qint64>(0, date.toMSecsSinceEpoch()))
        return {};
    // Qt's civil-time API has millisecond resolution. The exact sample index
    // and sample-count time remain authoritative; this is a labeled UTC aid.
    return date.addMSecs(static_cast<qint64>(milliseconds)).toString(Qt::ISODateWithMs);
}

std::uint64_t FrameRange::size() const
{
    if (end < begin)
        fail("Range end precedes start");
    return end - begin;
}

std::vector<std::complex<double>> decodeSamples(std::span<const std::byte> bytes,
                                                const SampleFormat &format)
{
    const auto frameBytes = format.frameBytes();
    if (bytes.size() % frameBytes != 0)
        fail("Input contains an incomplete sample frame");
    std::vector<std::complex<double>> samples(bytes.size() / frameBytes);
    const std::size_t width = static_cast<std::size_t>(format.bits / 8);
    if (format.encoding == Encoding::SignedInteger && format.bits == 16) {
        // A measured signed-int16 fast path: constant width/full scale eliminate
        // per-component ldexp and generic 64-bit sign conversion. This stays
        // ordinary portable C++; the independent decoder matrix covers this branch.
        const auto component = [&](const std::byte *data) {
            const auto word = static_cast<std::uint16_t>(loadUnsigned(data, 2, format.byteOrder));
            return static_cast<double>(std::bit_cast<std::int16_t>(word)) / 32768.0;
        };
        for (std::size_t index = 0; index < samples.size(); ++index) {
            const auto *frame = bytes.data() + index * frameBytes;
            const double first = component(frame);
            const double second = format.kind == SampleKind::Complex ? component(frame + 2) : 0;
            samples[index] =
                format.kind == SampleKind::Complex && format.componentOrder == ComponentOrder::QI
                    ? std::complex<double>{second, first}
                    : std::complex<double>{first, second};
        }
        return samples;
    }
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto *frame = bytes.data() + index * frameBytes;
        const double first = scalar(frame, format);
        if (format.kind == SampleKind::Real)
            samples[index] = {first, 0};
        else {
            const double second = scalar(frame + width, format);
            samples[index] = format.componentOrder == ComponentOrder::IQ
                                 ? std::complex<double>{first, second}
                                 : std::complex<double>{second, first};
        }
    }
    return samples;
}

Recording::Recording(RecordingDescriptor descriptor) : descriptor_(std::move(descriptor))
{
    descriptor_.validate();
    fd_ = ::open(QFile::encodeName(descriptor_.path).constData(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0)
        fail("Cannot open recording: " + QString::fromLocal8Bit(std::strerror(errno)));
    try {
        struct stat information{};
        if (::fstat(fd_, &information) != 0 || !S_ISREG(information.st_mode))
            fail("Input must be a seekable regular file");
        fileSize_ = static_cast<std::uint64_t>(information.st_size);
        device_ = static_cast<std::uint64_t>(information.st_dev);
        inode_ = static_cast<std::uint64_t>(information.st_ino);
        modifiedSeconds_ = information.st_mtim.tv_sec;
        modifiedNanos_ = information.st_mtim.tv_nsec;
        changedSeconds_ = information.st_ctim.tv_sec;
        changedNanos_ = information.st_ctim.tv_nsec;
        if (descriptor_.dataOffset > fileSize_)
            fail("Data offset exceeds file length");
        const auto bytes = descriptor_.dataBytes.value_or(fileSize_ - descriptor_.dataOffset);
        if (bytes > fileSize_ - descriptor_.dataOffset)
            fail("Data length exceeds file length");
        if (bytes % descriptor_.format.frameBytes() != 0)
            fail(
                "Incomplete frame at EOF: correct the format or explicitly exclude trailing bytes");
        frames_ = bytes / descriptor_.format.frameBytes();
        if (frames_ == 0)
            fail("Recording has no complete samples");
        if (!std::isfinite(static_cast<double>(frames_) / descriptor_.sampleRate))
            fail("Recording duration overflows");
        for (const auto &capture : descriptor_.captures)
            if (capture.start >= frames_)
                fail("Capture start is outside recording");
        for (const auto &annotation : descriptor_.annotations)
            if (annotation.start >= frames_ || annotation.count > frames_ - annotation.start)
                fail("Annotation range is outside recording");
    } catch (...) {
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}

Recording::~Recording()
{
    if (fd_ >= 0)
        ::close(fd_);
}

QString Recording::identity() const
{
    return QString("%1:%2:%3:%4:%5:%6:%7")
        .arg(device_)
        .arg(inode_)
        .arg(fileSize_)
        .arg(modifiedSeconds_)
        .arg(modifiedNanos_)
        .arg(changedSeconds_)
        .arg(changedNanos_);
}

void Recording::verifyUnchanged() const
{
    struct stat current{};
    struct stat pathInformation{};
    if (::fstat(fd_, &current) != 0 ||
        ::stat(QFile::encodeName(descriptor_.path).constData(), &pathInformation) != 0)
        fail("Recording is no longer accessible");
    if (static_cast<std::uint64_t>(current.st_size) != fileSize_ ||
        current.st_mtim.tv_sec != modifiedSeconds_ || current.st_mtim.tv_nsec != modifiedNanos_ ||
        current.st_ctim.tv_sec != changedSeconds_ || current.st_ctim.tv_nsec != changedNanos_ ||
        static_cast<std::uint64_t>(pathInformation.st_dev) != device_ ||
        static_cast<std::uint64_t>(pathInformation.st_ino) != inode_)
        fail("Recording changed or was replaced; reopen it before analysis");
}

void Recording::validateRange(FrameRange range) const
{
    (void)range.size();
    if (range.end > frames_)
        fail("Sample range exceeds recording");
}

std::vector<std::byte> Recording::readBytes(FrameRange range) const
{
    validateRange(range);
    // The API itself enforces a block limit, so an accidental whole-file call
    // cannot allocate gigabytes before the scheduler has a chance to cancel.
    constexpr std::uint64_t maxBlockBytes = 32U * 1024U * 1024U;
    const auto frameBytes = descriptor_.format.frameBytes();
    if (range.size() > maxBlockBytes / frameBytes)
        fail("Read exceeds the 32 MiB block budget");
    verifyUnchanged();
    const auto count = static_cast<std::size_t>(range.size() * frameBytes);
    const auto offset = descriptor_.dataOffset + range.begin * frameBytes;
    std::vector<std::byte> bytes(count);
    std::size_t done = 0;
    while (done < count) {
        const auto readCount =
            ::pread(fd_, bytes.data() + done, count - done, static_cast<off_t>(offset + done));
        if (readCount < 0 && errno == EINTR)
            continue;
        if (readCount <= 0)
            fail("Recording read failed or ended unexpectedly");
        done += static_cast<std::size_t>(readCount);
    }
    verifyUnchanged();
    return bytes;
}

std::vector<std::complex<double>> Recording::read(FrameRange range) const
{
    return decodeSamples(readBytes(range), descriptor_.format);
}
} // namespace rf
