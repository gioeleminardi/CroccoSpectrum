#pragma once
#include "app/AnalysisController.h"
#include <QDialog>
#include <thread>
class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QDialogButtonBox;
class QTimer;
class QSpinBox;
class QFormLayout;
namespace rf
{
class ImportDialog : public QDialog
{
    Q_OBJECT
  public:
    ImportDialog(QString path, RecordingDescriptor defaults, QWidget *parent = nullptr);
    ~ImportDialog() override;
    [[nodiscard]] RecordingDescriptor descriptor() const;
    void setDescriptor(const RecordingDescriptor &descriptor);
  signals:
    void metadataLoaded(rf::RecordingDescriptor descriptor);
    void metadataFailed(QString error);

  private:
    void requestPreview();
    void updateInputControls();
    RecordingDescriptor base_;
    QComboBox *kind_, *encoding_, *byteOrder_, *componentOrder_;
    QComboBox *input_, *delimiter_;
    QSpinBox *firstColumn_, *secondColumn_, *skipRows_;
    QFormLayout *form_;
    QLabel *conversion_;
    QCheckBox *partial_;
    QLineEdit *sampleRate_, *centerFrequency_, *startUtc_, *offset_, *length_, *fullScale_;
    QLabel *summary_;
    QPlainTextEdit *preview_;
    QDialogButtonBox *buttons_;
    QTimer *timer_;
    AnalysisController controller_;
    quint64 generation_ = 0;
    std::jthread metadataLoader_;
};
} // namespace rf
Q_DECLARE_METATYPE(rf::RecordingDescriptor)
