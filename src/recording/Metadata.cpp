#include "Metadata.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rf
{
namespace
{
[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}
QString decimal(std::uint64_t value)
{
    return QString::number(value);
}

int integerField(const QJsonObject &object, const QString &name, int fallback)
{
    const auto value = object.value(name);
    if (value.isUndefined())
        return fallback;
    if (!value.isDouble() || value.toDouble() != value.toInt(-1))
        fail("Invalid integer field: " + name);
    return value.toInt();
}
} // namespace

std::uint64_t jsonUnsigned(const QJsonValue &value, const QString &field)
{
    if (value.isString()) {
        const auto string = value.toString();
        if (!QRegularExpression("^[0-9]+$").match(string).hasMatch())
            fail("Invalid unsigned field: " + field);
        bool ok = false;
        const auto number = string.toULongLong(&ok);
        if (ok)
            return number;
    } else if (value.isDouble()) {
        // Qt 6 retains integral JSON numbers as qint64. Do not convert them
        // through double, which would round sample indices above 2^53.
        const auto number = value.toInteger(-1);
        if (number >= 0 && static_cast<double>(number) == value.toDouble())
            return static_cast<std::uint64_t>(number);
    }
    fail("Invalid or overflowing unsigned field: " + field);
}

QJsonObject readJsonObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        fail("Cannot read metadata/settings: " + file.errorString());
    if (file.size() > maxMetadataBytes)
        fail("Metadata/settings exceed the 4 MiB limit");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.read(maxMetadataBytes + 1), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        fail("Invalid JSON: " + error.errorString());
    return document.object();
}

void writeJsonAtomic(const QString &path, const QJsonObject &object)
{
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    // Saving must obey the same bound as reading. Otherwise a large notes/
    // annotations session could successfully replace a file we cannot reopen.
    if (bytes.size() > maxMetadataBytes)
        fail("Settings/metadata exceed the 4 MiB limit; original file preserved");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        fail("Cannot save: " + file.errorString());
    if (file.write(bytes) != bytes.size() || !file.commit())
        fail("Atomic save failed: " + file.errorString());
}

QJsonObject recordingToJson(const RecordingDescriptor &descriptor)
{
    const auto &format = descriptor.format;
    QJsonObject object{
        {"path", descriptor.path},
        {"kind", format.kind == SampleKind::Complex ? "complex" : "real"},
        {"encoding", format.encoding == Encoding::FloatingPoint   ? "float"
                     : format.encoding == Encoding::SignedInteger ? "signed"
                                                                  : "unsigned"},
        {"bits", format.bits},
        {"byte_order", format.byteOrder == ByteOrder::LittleEndian ? "little" : "big"},
        {"component_order", format.componentOrder == ComponentOrder::IQ ? "IQ" : "QI"},
        {"float_full_scale", format.floatFullScale},
        {"sample_rate", descriptor.sampleRate},
        {"start_utc", descriptor.startUtc},
        {"data_offset", decimal(descriptor.dataOffset)},
        {"allow_partial", descriptor.allowPartial},
        {"metadata_source", descriptor.metadataSource}};
    if (descriptor.dataBytes)
        object["data_bytes"] = decimal(*descriptor.dataBytes);
    if (descriptor.centerFrequency)
        object["center_frequency"] = *descriptor.centerFrequency;
    QJsonArray captures;
    for (const auto &capture : descriptor.captures) {
        QJsonObject entry{{"start", decimal(capture.start)}, {"datetime", capture.datetime}};
        if (capture.frequency)
            entry["frequency"] = *capture.frequency;
        captures.append(entry);
    }
    object["captures"] = captures;
    QJsonArray annotations;
    for (const auto &note : descriptor.annotations)
        annotations.append(QJsonObject{
            {"start", decimal(note.start)}, {"count", decimal(note.count)}, {"label", note.label}});
    object["annotations"] = annotations;
    return object;
}

RecordingDescriptor recordingFromJson(const QJsonObject &object)
{
    if (object.contains("allow_partial") && !object["allow_partial"].isBool())
        fail("Invalid boolean field: allow_partial");
    for (const auto *field : {"float_full_scale", "sample_rate", "center_frequency"})
        if (object.contains(field) && !object[field].isDouble())
            fail(QString("Invalid numeric field: ") + field);
    for (const auto *field : {"captures", "annotations"})
        if (object.contains(field) && !object[field].isArray())
            fail(QString("Invalid array field: ") + field);
    RecordingDescriptor descriptor;
    descriptor.path = object.value("path").toString();
    const QString kind = object.value("kind").toString();
    if (kind != "real" && kind != "complex")
        fail("Unknown sample kind");
    descriptor.format.kind = kind == "real" ? SampleKind::Real : SampleKind::Complex;
    const QString encoding = object.value("encoding").toString();
    if (encoding != "float" && encoding != "signed" && encoding != "unsigned")
        fail("Unknown scalar encoding");
    descriptor.format.encoding = encoding == "float"    ? Encoding::FloatingPoint
                                 : encoding == "signed" ? Encoding::SignedInteger
                                                        : Encoding::UnsignedInteger;
    descriptor.format.bits = integerField(object, "bits", 16);
    const QString byteOrder = object.value("byte_order").toString();
    const QString components = object.value("component_order").toString();
    if (byteOrder != "little" && byteOrder != "big")
        fail("Unknown byte order");
    if (components != "IQ" && components != "QI")
        fail("Unknown component order");
    descriptor.format.byteOrder =
        byteOrder == "little" ? ByteOrder::LittleEndian : ByteOrder::BigEndian;
    descriptor.format.componentOrder = components == "IQ" ? ComponentOrder::IQ : ComponentOrder::QI;
    descriptor.format.floatFullScale = object.value("float_full_scale").toDouble(1);
    descriptor.sampleRate = object.value("sample_rate").toDouble(0);
    descriptor.startUtc = object.value("start_utc").toString();
    descriptor.dataOffset = jsonUnsigned(object.value("data_offset"), "data_offset");
    descriptor.allowPartial = object.value("allow_partial").toBool(false);
    if (object.contains("data_bytes"))
        descriptor.dataBytes = jsonUnsigned(object.value("data_bytes"), "data_bytes");
    if (object.contains("center_frequency"))
        descriptor.centerFrequency =
            object.value("center_frequency").toDouble(std::numeric_limits<double>::quiet_NaN());
    descriptor.metadataSource = object.value("metadata_source").toString("Saved import");
    for (const auto entry : object.value("captures").toArray()) {
        const auto item = entry.toObject();
        Capture capture{
            jsonUnsigned(item["start"], "capture start"), {}, item["datetime"].toString()};
        if (item.contains("frequency"))
            capture.frequency =
                item["frequency"].toDouble(std::numeric_limits<double>::quiet_NaN());
        descriptor.captures.push_back(capture);
    }
    for (const auto entry : object.value("annotations").toArray()) {
        const auto item = entry.toObject();
        descriptor.annotations.push_back({jsonUnsigned(item["start"], "annotation start"),
                                          jsonUnsigned(item["count"], "annotation count"),
                                          item["label"].toString()});
    }
    descriptor.validate();
    return descriptor;
}

RecordingDescriptor readSigMf(const QString &metadataPath)
{
    const auto root = readJsonObject(metadataPath);
    if (!root["global"].isObject() || !root["captures"].isArray() || !root["annotations"].isArray())
        fail("SigMF requires global, captures, and annotations");
    const auto global = root["global"].toObject();
    if (global.contains("core:extensions") && !global["core:extensions"].isArray())
        fail("Invalid SigMF extension list");
    if (integerField(global, "core:num_channels", 1) != 1)
        fail("Multiple independent SigMF channels are not supported");
    for (const auto extension : global["core:extensions"].toArray())
        if (!extension.toObject()["optional"].toBool(false))
            fail("Required SigMF extension is unsupported");

    const auto datatype = global["core:datatype"].toString();
    const auto match =
        QRegularExpression("^([rc])([fiu])(8|16|32|64)(?:_(le|be))?$").match(datatype);
    if (!match.hasMatch())
        fail("Unsupported SigMF core datatype: " + datatype);
    RecordingDescriptor descriptor;
    descriptor.format.kind = match.captured(1) == "c" ? SampleKind::Complex : SampleKind::Real;
    descriptor.format.encoding = match.captured(2) == "f"   ? Encoding::FloatingPoint
                                 : match.captured(2) == "i" ? Encoding::SignedInteger
                                                            : Encoding::UnsignedInteger;
    descriptor.format.bits = match.captured(3).toInt();
    const bool floatType = descriptor.format.encoding == Encoding::FloatingPoint;
    if ((floatType && descriptor.format.bits != 32 && descriptor.format.bits != 64) ||
        (!floatType && descriptor.format.bits == 64) ||
        (descriptor.format.bits != 8 && match.captured(4).isEmpty()) ||
        (descriptor.format.bits == 8 && !match.captured(4).isEmpty()))
        fail("Datatype is not a SigMF core encoding");
    descriptor.format.byteOrder =
        match.captured(4) == "be" ? ByteOrder::BigEndian : ByteOrder::LittleEndian;
    descriptor.sampleRate = global["core:sample_rate"].toDouble(0);
    if (descriptor.sampleRate > 1e12)
        fail("SigMF sample rate exceeds the core schema limit");
    const QFileInfo metadata(metadataPath);
    const auto dataset = global["core:dataset"].toString();
    descriptor.path =
        dataset.isEmpty()
            ? metadata.absoluteFilePath().chopped(QString(".sigmf-meta").size()) + ".sigmf-data"
            : metadata.dir().absoluteFilePath(dataset);
    descriptor.metadataSource = metadata.absoluteFilePath();
    const auto offset =
        global.contains("core:offset") ? jsonUnsigned(global["core:offset"], "core:offset") : 0;
    for (const auto entry : root["captures"].toArray()) {
        const auto item = entry.toObject();
        const auto absolute = jsonUnsigned(item["core:sample_start"], "core:sample_start");
        if (absolute < offset)
            fail("Capture start precedes the dataset offset");
        const auto header = item.contains("core:header_bytes")
                                ? jsonUnsigned(item["core:header_bytes"], "core:header_bytes")
                                : 0;
        if (header != 0 && !descriptor.captures.empty())
            fail("Per-capture embedded headers are unsupported");
        if (header != 0)
            descriptor.dataOffset = header;
        Capture capture{absolute - offset, {}, item["core:datetime"].toString()};
        if (item.contains("core:frequency"))
            capture.frequency =
                item["core:frequency"].toDouble(std::numeric_limits<double>::quiet_NaN());
        descriptor.captures.push_back(capture);
    }
    if (global.contains("core:trailing_bytes")) {
        const auto trailing = jsonUnsigned(global["core:trailing_bytes"], "core:trailing_bytes");
        const auto size = QFileInfo(descriptor.path).size();
        if (size < 0 || descriptor.dataOffset > static_cast<std::uint64_t>(size) ||
            trailing > static_cast<std::uint64_t>(size) - descriptor.dataOffset)
            fail("Invalid SigMF trailing bytes");
        descriptor.dataBytes = static_cast<std::uint64_t>(size) - descriptor.dataOffset - trailing;
    }
    for (const auto entry : root["annotations"].toArray()) {
        const auto item = entry.toObject();
        const auto start = jsonUnsigned(item["core:sample_start"], "annotation start");
        if (start < offset)
            fail("Annotation start precedes the dataset offset");
        const auto count = item.contains("core:sample_count")
                               ? jsonUnsigned(item["core:sample_count"], "annotation count")
                               : 0;
        descriptor.annotations.push_back({start - offset, count, item["core:label"].toString()});
    }
    descriptor.validate();
    return descriptor;
}
} // namespace rf
