#pragma once
#include "Recording.h"
#include <QJsonObject>

namespace rf
{
inline constexpr qint64 maxMetadataBytes = 4 * 1024 * 1024;
// Application JSON stores 64-bit indices as decimal strings. SigMF metadata is
// read according to its own schema, not silently rewritten into this schema.
std::uint64_t jsonUnsigned(const QJsonValue &value, const QString &field);
QJsonObject recordingToJson(const RecordingDescriptor &descriptor);
RecordingDescriptor recordingFromJson(const QJsonObject &object);
RecordingDescriptor readSigMf(const QString &metadataPath);
QJsonObject readJsonObject(const QString &path);
void writeJsonAtomic(const QString &path, const QJsonObject &object);
} // namespace rf
