#pragma once

#include "Recording.h"

namespace rf
{
InputFormat inputFormatForPath(const QString &path);

// Streams into an automatically removed file. The descriptor continues to name
// the original source; its format/rate describe the converted samples.
std::unique_ptr<QTemporaryFile> convertRecording(int sourceFd, RecordingDescriptor &descriptor,
                                                const CancelCheck &cancelled);
} // namespace rf
