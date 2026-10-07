#pragma once
#include "Plots.h"
#include "app/AnalysisController.h"
#include <QMainWindow>
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QLineEdit;
class QSlider;
class QLabel;
class QPushButton;
class QProgressBar;
class QDockWidget;
class QListWidget;
class QTimer;

namespace rf
{
class MainWindow : public QMainWindow
{
    Q_OBJECT
  public:
    explicit MainWindow(QWidget *parent = nullptr, QString preferencesPath = {});
    // Public opening entry point is also used by integration tests and the
    // explicit --accept-defaults CLI option; normal File/Open reviews import.
    void openRecording(RecordingDescriptor descriptor, FrameRange range = {});
    void openPath(const QString &path);
    [[nodiscard]] std::shared_ptr<const PreviewResult> previewResult() const { return preview_; }
    [[nodiscard]] std::shared_ptr<const PreviewResult> spectrumResult() const
    {
        return spectrumPreview_;
    }
    [[nodiscard]] FrameRange selectedRange() const { return range_; }
    [[nodiscard]] DspSettings dspSettings() const { return preferences_.dsp; }
    [[nodiscard]] bool isBusy() const { return busy_; }
  signals:
    void analysisDisplayed();
    void analysisError(QString message);

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    void buildMenus();
    void showAbout();
    void buildControls();
    void connectWorker();
    void applyPreferences();
    void schedulePreview();
    void requestPreview();
    void hoverFrame(quint64 frame);
    void selectFrame(quint64 frame);
    void toggleFrameFreeze(quint64 frame);
    void clearFrameCursor();
    void updateFrameCursors();
    void displaySpectrum(std::shared_ptr<const PreviewResult> result, bool resetZoom);
    void updateSpectrumView();
    void updateDsp();
    void updateView();
    void updateRanges();
    void setRange(FrameRange range);
    void seekFrame(std::uint64_t frame);
    void zoomTime(bool inward);
    void panTime(bool forward);
    void startAverage(bool entireRecording);
    void startWaveform();
    void exportCsv(bool average);
    void exportPng();
    void exportSampleRange();
    void openSessionPath(const QString &path);
    void saveCurrentSession(bool choosePath);
    void addBookmark();
    void persistPreferences();
    void setBusy(bool busy, const QString &text = {});
    void reportError(const QString &text);
    void clearMeasurements();
    [[nodiscard]] double displayCenter() const;

    Preferences preferences_;
    QString preferencesPath_;
    QString sessionPath_;
    std::optional<Session> restoringSession_;
    std::vector<Annotation> bookmarks_;
    std::shared_ptr<Recording> recording_;
    std::shared_ptr<const PreviewResult> preview_;
    std::shared_ptr<const PreviewResult> spectrumPreview_;
    std::shared_ptr<const AverageResult> average_;
    std::shared_ptr<const WaveformResult> waveformResult_;
    FrameRange range_;
    quint64 generation_ = 0;
    quint64 spectrumGeneration_ = 0;
    std::optional<std::uint64_t> spectrumFrame_;
    bool frameFrozen_ = false;
    bool frameCursorVisible_ = false;
    bool busy_ = false;
    bool applying_ = false;
    AnalysisController controller_;
    AnalysisController spectrumController_;
    SpectrumPlot *spectrum_, *averagePlot_;
    WaterfallPlot *waterfall_;
    WaveformPlot *waveform_;
    QDockWidget *controlsDock_, *averageDock_, *waveformDock_, *bookmarksDock_;
    QComboBox *fft_, *window_, *scale_, *palette_, *waveformMode_;
    QSpinBox *overlap_;
    QCheckBox *removeDc_, *conjugate_, *autoRange_, *absoluteFrequency_, *maxHold_;
    QDoubleSpinBox *colorMin_, *colorMax_;
    QLineEdit *start_, *end_, *seekTime_;
    QSlider *timeline_;
    QLabel *recordingLabel_, *resolutionLabel_, *coverageLabel_, *cursorLabel_, *averageLabel_;
    QPushButton *cancel_;
    QProgressBar *progress_;
    QListWidget *bookmarksList_;
    QTimer *previewTimer_, *saveTimer_;
};
} // namespace rf
