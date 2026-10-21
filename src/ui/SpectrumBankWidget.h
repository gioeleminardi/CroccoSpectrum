#pragma once
#include "app/AnalysisController.h"
#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;

namespace rf
{
class SpectrumBankPlot;
class SpectrumBankWidget : public QWidget
{
    Q_OBJECT
  public:
    explicit SpectrumBankWidget(QWidget *parent = nullptr);
    void setContext(const RecordingDescriptor &recording, const QString &identity,
                    const DspSettings &dsp);
    void clear();
    void setCaptureAvailable(bool available);
    void addTrace(SavedSpectrum trace);
    void setTraces(std::vector<SavedSpectrum> traces);
    [[nodiscard]] const std::vector<SavedSpectrum> &traces() const { return traces_; }
  signals:
    void captureRequested();
    void error(QString message);

  private:
    void rebuildList();
    void refresh();
    std::vector<SavedSpectrum> traces_;
    RecordingDescriptor recording_;
    QString identity_;
    DspSettings dsp_;
    QListWidget *list_;
    QLabel *details_;
    QCheckBox *difference_;
    QPushButton *capture_, *import_, *export_, *remove_;
    SpectrumBankPlot *plot_;
    quint64 ioGeneration_ = 0;
    bool ioBusy_ = false;
    AnalysisController ioController_;
};
} // namespace rf
