#include "Import.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <bzlib.h>
#include <cmath>
#include <cstring>
#include <sndfile.h>
#include <unistd.h>
#include <zlib.h>

namespace rf
{
namespace
{
constexpr int blockBytes = 256 * 1024;

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

void checkpoint(const CancelCheck &cancelled)
{
    if (cancelled && cancelled())
        throw Cancelled();
}

void write(QTemporaryFile &output, const char *bytes, qint64 count)
{
    if (output.write(bytes, count) != count)
        fail("Import temporary-file write failed: " + output.errorString());
}

void writeDoubles(QTemporaryFile &output, std::vector<double> &samples)
{
    // The temporary representation is portable little-endian IEEE float64.
    for (auto &sample : samples) {
        auto bits = qToLittleEndian(std::bit_cast<quint64>(sample));
        sample = std::bit_cast<double>(bits);
    }
    write(output, reinterpret_cast<const char *>(samples.data()),
          static_cast<qint64>(samples.size() * sizeof(double)));
}

double audioSampleRate(int sourceFd, const SF_INFO &info, const CancelCheck &cancelled)
{
    // libsndfile can silently shorten truncated containers to complete frames.
    // Check declared chunk extents before accepting that repaired interpretation.
    QFile source;
    if (!source.open(sourceFd, QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        fail("Cannot read audio metadata");
    const auto size = source.size();
    unsigned char containerHeader[12];
    if (::pread(sourceFd, containerHeader, sizeof(containerHeader), 0) != sizeof(containerHeader))
        fail("Truncated audio header");
    const bool aiff = (info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_AIFF;
    const bool big = aiff || std::memcmp(containerHeader, "RIFX", 4) == 0;
    const auto word = [big](const unsigned char *bytes) {
        return big ? qFromBigEndian<quint32>(bytes) : qFromLittleEndian<quint32>(bytes);
    };
    const auto extent = static_cast<qint64>(word(containerHeader + 4)) + 8;
    if (extent > size || extent < 12)
        fail("Truncated or invalid audio container");
    const auto encoding = info.format & SF_FORMAT_SUBMASK;
    const int width = encoding == SF_FORMAT_PCM_S8 || encoding == SF_FORMAT_PCM_U8 ? 1
                      : encoding == SF_FORMAT_PCM_16 ? 2
                      : encoding == SF_FORMAT_PCM_24 ? 3
                      : encoding == SF_FORMAT_DOUBLE ? 8 : 4;
    const auto frameBytes = static_cast<qint64>(width) * info.channels;
    double sampleRate = info.samplerate;
    bool commFound = !aiff;
    int dataChunks = 0;
    for (qint64 offset = 12; offset < extent;) {
        checkpoint(cancelled);
        unsigned char header[8];
        if (extent - offset < 8 ||
            ::pread(sourceFd, header, sizeof(header), offset) != sizeof(header))
            fail("Truncated audio chunk");
        const auto length = static_cast<qint64>(word(header + 4));
        if (length > extent - offset - 8)
            fail("Truncated audio chunk");
        if (aiff && std::memcmp(header, "COMM", 4) == 0) {
            // SF_INFO rounds AIFF's extended-precision rate to an integer.
            unsigned char rate[10];
            unsigned char frames[4];
            if (length < 18 || ::pread(sourceFd, rate, sizeof(rate), offset + 16) != sizeof(rate))
                fail("Invalid AIFF sample rate");
            if (commFound ||
                ::pread(sourceFd, frames, sizeof(frames), offset + 10) != sizeof(frames) ||
                qFromBigEndian<quint32>(frames) != info.frames)
                fail("Invalid or truncated AIFF frame count");
            commFound = true;
            const auto exponent = qFromBigEndian<quint16>(rate);
            const auto significand = qFromBigEndian<quint64>(rate + 2);
            if ((exponent & 0x8000U) != 0 || exponent == 0x7fffU || significand == 0)
                fail("Invalid AIFF sample rate");
            sampleRate = std::ldexp(static_cast<double>(significand),
                                   static_cast<int>(exponent == 0 ? 1 : exponent) - 16383 - 63);
        }
        if (!aiff && std::memcmp(header, "data", 4) == 0) {
            if (++dataChunks != 1 || length % frameBytes != 0 || length / frameBytes != info.frames)
                fail("Incomplete WAV frames or unsupported multiple data chunks");
        }
        if (aiff && std::memcmp(header, "SSND", 4) == 0) {
            unsigned char skip[4];
            if (++dataChunks != 1 || length < 8 ||
                ::pread(sourceFd, skip, sizeof(skip), offset + 8) != sizeof(skip))
                fail("Invalid AIFF sample data");
            const auto available = length - 8 - qFromBigEndian<quint32>(skip);
            if (available < 0 || info.frames > available / frameBytes)
                fail("Truncated AIFF sample data");
        }
        offset += 8 + length;
        if (length % 2 && offset < extent)
            ++offset;
    }
    if (!commFound || dataChunks != 1)
        fail("Missing audio format/sample chunks");
    return sampleRate;
}

void audio(int sourceFd, QTemporaryFile &output, RecordingDescriptor &descriptor,
           const CancelCheck &cancelled)
{
    SF_INFO info{};
    SNDFILE *file = sf_open_fd(sourceFd, SFM_READ, &info, SF_FALSE);
    if (!file)
        fail("Cannot read WAV/AIFF: " + QString::fromUtf8(sf_strerror(nullptr)));
    const auto close = qScopeGuard([&] { sf_close(file); });
    const auto container = info.format & SF_FORMAT_TYPEMASK;
    const auto encoding = info.format & SF_FORMAT_SUBMASK;
    if ((container != SF_FORMAT_WAV && container != SF_FORMAT_WAVEX &&
         container != SF_FORMAT_AIFF) ||
        (encoding != SF_FORMAT_PCM_S8 && encoding != SF_FORMAT_PCM_U8 &&
         encoding != SF_FORMAT_PCM_16 && encoding != SF_FORMAT_PCM_24 &&
         encoding != SF_FORMAT_PCM_32 && encoding != SF_FORMAT_FLOAT &&
         encoding != SF_FORMAT_DOUBLE))
        fail("Supported audio: WAV/AIFF PCM 8/16/24/32-bit or IEEE float32/64");
    if (info.channels < 1 || info.channels > 1024 || info.frames <= 0 || info.samplerate <= 0)
        fail("Invalid or empty audio recording");
    descriptor.sampleRate = audioSampleRate(sourceFd, info, cancelled);
    descriptor.validate();
    const bool complex = descriptor.format.kind == SampleKind::Complex;
    const auto &mapping = descriptor.import;
    if (mapping.firstColumn > info.channels || (complex && mapping.secondColumn > info.channels))
        fail("Selected audio channel is outside the recording");
    const auto framesPerBlock = static_cast<sf_count_t>(32768 / info.channels);
    std::vector<double> source(static_cast<std::size_t>(framesPerBlock) * info.channels);
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(framesPerBlock) * (complex ? 2U : 1U));
    sf_count_t total = 0;
    for (;;) {
        checkpoint(cancelled);
        const auto frames = sf_readf_double(file, source.data(), framesPerBlock);
        if (sf_error(file) != SF_ERR_NO_ERROR)
            fail("Audio decoding failed: " + QString::fromUtf8(sf_strerror(file)));
        if (frames == 0)
            break;
        samples.clear();
        for (sf_count_t frame = 0; frame < frames; ++frame) {
            const auto start = static_cast<std::size_t>(frame) * info.channels;
            samples.push_back(source[start + static_cast<std::size_t>(mapping.firstColumn - 1)]);
            if (complex)
                samples.push_back(
                    source[start + static_cast<std::size_t>(mapping.secondColumn - 1)]);
        }
        writeDoubles(output, samples);
        total += frames;
    }
    if (total != info.frames)
        fail("Audio recording ended before its declared frame count");
    SF_FORMAT_INFO subtype{};
    subtype.format = encoding;
    sf_command(file, SFC_GET_FORMAT_INFO, &subtype, sizeof(subtype));
    descriptor.metadataSource = QString("%1 · %2 · %3 channel(s); selected channels → float64")
                                    .arg(container == SF_FORMAT_AIFF ? "AIFF/AIFC" : "WAV")
                                    .arg(QString::fromUtf8(subtype.name ? subtype.name : "PCM"))
                                    .arg(info.channels);
}

void text(QFile &source, QTemporaryFile &output, const RecordingDescriptor &descriptor,
          const CancelCheck &cancelled)
{
    const auto &mapping = descriptor.import;
    const bool complex = descriptor.format.kind == SampleKind::Complex;
    const auto column = std::max(mapping.firstColumn, complex ? mapping.secondColumn : 1);
    std::vector<double> samples;
    samples.reserve(32768);
    const QRegularExpression whitespace("\\s+");
    std::uint64_t row = 0;
    while (!source.atEnd()) {
        checkpoint(cancelled);
        // Bound even malformed, unterminated lines and explicitly skipped headers.
        auto line = source.readLine(1024 * 1024 + 2);
        if (source.error() != QFile::NoError)
            fail("Text recording read failed: " + source.errorString());
        if (line.size() > 1024 * 1024)
            fail("Text recording line exceeds 1 MiB");
        ++row;
        if (row <= static_cast<std::uint64_t>(mapping.skipRows))
            continue;
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        if (line.contains('"'))
            fail("Quoted CSV fields are unsupported; supply plain numeric columns");
        const auto fields = mapping.delimiter == "whitespace"
                                ? QString::fromLatin1(line).split(whitespace, Qt::SkipEmptyParts)
                                : QString::fromLatin1(line).split(mapping.delimiter,
                                                                  Qt::KeepEmptyParts);
        if (fields.size() < column)
            fail(QString("Missing sample column at text row %1").arg(row));
        for (const auto selected : {mapping.firstColumn, complex ? mapping.secondColumn : 0}) {
            if (selected == 0)
                continue;
            bool ok = false;
            const auto value = fields[selected - 1].trimmed().toDouble(&ok);
            if (!ok)
                fail(QString("Invalid number at text row %1, column %2").arg(row).arg(selected));
            samples.push_back(value);
        }
        if (samples.size() >= 32768) {
            writeDoubles(output, samples);
            samples.clear();
        }
    }
    writeDoubles(output, samples);
}

void decompress(QFile &source, QTemporaryFile &output, bool gzip, const CancelCheck &cancelled)
{
    z_stream z{};
    bz_stream bz{};
    const auto initialize = [&] {
        const auto result = gzip ? inflateInit2(&z, 15 + 16) : BZ2_bzDecompressInit(&bz, 0, 0);
        if (result != 0)
            fail("Cannot initialize decompressor");
    };
    initialize();
    const auto release = [&] {
        if (gzip)
            inflateEnd(&z);
        else
            BZ2_bzDecompressEnd(&bz);
    };
    const auto cleanup = qScopeGuard(release);
    QByteArray input;
    QByteArray buffer(blockBytes, Qt::Uninitialized);
    qsizetype cursor = 0;
    bool ended = false;
    for (;;) {
        checkpoint(cancelled);
        if (cursor == input.size()) {
            input = source.read(blockBytes);
            cursor = 0;
            if (source.error() != QFile::NoError)
                fail("Compressed recording read failed: " + source.errorString());
            if (input.isEmpty()) {
                if (ended)
                    return;
                // A decoder can still hold output after consuming its last
                // input byte. Drain it before deciding that EOF is truncated.
            }
        }
        if (ended) {
            // Concatenated gzip/bzip2 members are one decoded byte stream.
            if (gzip) {
                if (inflateReset2(&z, 15 + 16) != Z_OK)
                    fail("Cannot reset gzip decoder");
            } else {
                BZ2_bzDecompressEnd(&bz);
                bz = {};
                if (BZ2_bzDecompressInit(&bz, 0, 0) != BZ_OK)
                    fail("Cannot reset bzip2 decoder");
            }
            ended = false;
        }
        const auto available = static_cast<unsigned int>(input.size() - cursor);
        unsigned int remaining = 0, produced = 0;
        int result = 0;
        if (gzip) {
            z.next_in = reinterpret_cast<Bytef *>(input.data() + cursor);
            z.avail_in = available;
            z.next_out = reinterpret_cast<Bytef *>(buffer.data());
            z.avail_out = blockBytes;
            result = inflate(&z, Z_NO_FLUSH);
            remaining = z.avail_in;
            produced = blockBytes - z.avail_out;
            ended = result == Z_STREAM_END;
            if (result != Z_OK && !ended)
                fail("Invalid gzip recording (header, data or checksum)");
        } else {
            bz.next_in = input.data() + cursor;
            bz.avail_in = available;
            bz.next_out = buffer.data();
            bz.avail_out = blockBytes;
            result = BZ2_bzDecompress(&bz);
            remaining = bz.avail_in;
            produced = blockBytes - bz.avail_out;
            ended = result == BZ_STREAM_END;
            if (result != BZ_OK && !ended)
                fail("Invalid bzip2 recording (header, data or checksum)");
        }
        if (remaining == available && produced == 0 && !ended)
            fail("Truncated compressed recording");
        cursor += available - remaining;
        write(output, buffer.constData(), produced);
    }
}
} // namespace

InputFormat inputFormatForPath(const QString &path)
{
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == "wav" || suffix == "wave" || suffix == "aif" || suffix == "aiff" ||
        suffix == "aifc")
        return InputFormat::Audio;
    if (suffix == "csv" || suffix == "txt" || suffix == "asc" || suffix == "tsv")
        return InputFormat::Text;
    if (suffix == "gz")
        return InputFormat::Gzip;
    if (suffix == "bz2")
        return InputFormat::Bzip2;
    return InputFormat::Raw;
}

std::unique_ptr<QTemporaryFile> convertRecording(int sourceFd, RecordingDescriptor &descriptor,
                                                const CancelCheck &cancelled)
{
    checkpoint(cancelled);
    auto output =
        std::make_unique<QTemporaryFile>(QDir::tempPath() + "/croccospectrum-import-XXXXXX");
    if (!output->open())
        fail("Cannot create import temporary file: " + output->errorString());
    QFile source;
    if (!source.open(sourceFd, QIODevice::ReadOnly, QFileDevice::DontCloseHandle))
        fail("Cannot read import source");
    const auto input = descriptor.import.input;
    if (input == InputFormat::Audio)
        audio(sourceFd, *output, descriptor, cancelled);
    else if (input == InputFormat::Text) {
        text(source, *output, descriptor, cancelled);
        descriptor.metadataSource =
            "Numeric text; selected columns → float64 (no amplitude rescaling)";
    } else {
        decompress(source, *output, input == InputFormat::Gzip, cancelled);
        descriptor.metadataSource = input == InputFormat::Gzip
                                        ? "gzip → temporary raw bytes; user interpretation"
                                        : "bzip2 → temporary raw bytes; user interpretation";
    }
    if (input == InputFormat::Audio || input == InputFormat::Text) {
        descriptor.format.encoding = Encoding::FloatingPoint;
        descriptor.format.bits = 64;
        descriptor.format.byteOrder = ByteOrder::LittleEndian;
        descriptor.format.componentOrder = ComponentOrder::IQ;
    }
    if (!output->flush())
        fail("Cannot flush import temporary file: " + output->errorString());
    checkpoint(cancelled);
    return output;
}
} // namespace rf
