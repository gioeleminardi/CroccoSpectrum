#pragma once

#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <stdexcept>
#include <utility>

namespace rf
{
// Write alongside the destination, then publish using Qt's no-overwrite
// rename. An exception/cancellation removes the temporary file through RAII.
// Preferences use QSaveFile instead: replacing an old preference is intended,
// whereas a measurement export must never replace somebody else's data.
class ExportFile
{
  public:
    explicit ExportFile(QString destination)
        : destination_(std::move(destination)),
          file_(QFileInfo(destination_).dir().filePath(".croccospectrum-export-XXXXXX"))
    {
        if (QFileInfo::exists(destination_) || QFileInfo(destination_).isSymLink())
            throw std::runtime_error("Output already exists; choose a new filename");
        if (!file_.open())
            throw std::runtime_error(file_.errorString().toStdString());
    }
    QTemporaryFile &file() { return file_; }
    void commit()
    {
        if (!file_.flush())
            throw std::runtime_error(file_.errorString().toStdString());
        file_.close();
        // Both files are on the same filesystem. QFile::rename refuses a
        // destination created after the initial existence check, too.
        if (!file_.rename(destination_))
            throw std::runtime_error(file_.errorString().toStdString());
        file_.setAutoRemove(false);
    }

  private:
    QString destination_;
    QTemporaryFile file_;
};
} // namespace rf
