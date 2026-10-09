#pragma once
#include "app/Session.h"
#include <QImage>
#include <QWidget>
#include <utility>

namespace rf
{
struct MeasurementDrag {
    enum class Target { None, Band, Start, End };
    Target target = Target::None;
    std::pair<std::size_t, std::size_t> original;
    QPointF pressPosition;
    std::size_t pressIndex = 0;
    bool moved = false;
};

// Plot widgets only consume immutable snapshots. They never read a recording,
// run an FFT, or change the source arrays when reducing data for screen pixels.
class SpectrumPlot : public QWidget
{
    Q_OBJECT
  public:
    explicit SpectrumPlot(QWidget *parent = nullptr);
    void setPreview(std::shared_ptr<const PreviewResult> result, bool resetZoom = true);
    void setAverage(std::shared_ptr<const AverageResult> result, bool maxHold);
    void setView(ViewSettings view, PowerScale scale, double centerFrequency);
    void clear();
    void resetFrequency();
    void setFrequencyRange(double left, double right);
    void clearMeasurement();
    [[nodiscard]] bool isMeasuring() const
    {
        return measuring_ || measurementDrag_.target != MeasurementDrag::Target::None;
    }
    [[nodiscard]] QString measurementText() const;
  signals:
    void cursorChanged(QString text);
    void frequencyHovered(double frequency); // Baseband Hz, matching the linked plot ranges.
    void cursorLeft();
    void frequencyRangeChanged(double left, double right);
    void measurementActiveChanged(bool active);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

  private:
    std::span<const double> frequencies() const;
    std::span<const double> powers() const;
    std::optional<std::size_t> binAt(const QPointF &position) const;
    void updateMeasurementPower();
    void paintMeasurement(QPainter &painter);
    void updateHover(const QPointF &position);
    void paintCrosshair(QPainter &painter, double minimum, double maximum);
    std::shared_ptr<const PreviewResult> preview_;
    std::shared_ptr<const AverageResult> average_;
    ViewSettings view_;
    PowerScale scale_ = PowerScale::Spectrum;
    bool maxHold_ = false;
    double center_ = 0;
    double left_ = 0, right_ = 1;
    double dragLeft_ = 0, dragRight_ = 1;
    int dragX_ = -1;
    std::optional<std::pair<std::size_t, std::size_t>> measurementBins_;
    std::optional<double> measurementMean_;
    bool measuring_ = false;
    MeasurementDrag measurementDrag_;
    std::optional<QPointF> hoverPosition_;
};

class WaterfallPlot : public QWidget
{
    Q_OBJECT
  public:
    explicit WaterfallPlot(QWidget *parent = nullptr);
    void setPreview(std::shared_ptr<const PreviewResult> result, double sampleRate);
    void setView(ViewSettings view, PowerScale scale, double centerFrequency);
    void setFrequencyRange(double left, double right);
    void setFrequencyCursor(std::optional<double> frequency);
    void setFrameCursor(std::optional<std::uint64_t> frame, bool frozen);
    void clear();
    void clearMeasurement();
    [[nodiscard]] bool isMeasuring() const
    {
        return measuring_ || measurementDrag_.target != MeasurementDrag::Target::None;
    }
    [[nodiscard]] QString measurementText() const;
    [[nodiscard]] std::optional<FrameRange> measurementRange() const;
  signals:
    void cursorChanged(QString text);
    void frameHovered(quint64 frame);
    void frameClicked(quint64 frame);
    void cursorLeft();
    void frameSelected(quint64 frame);
    void measurementActiveChanged(bool active);
    void measurementChanged();

  protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

  private:
    void rebuildImage();
    std::optional<std::size_t> rowAt(const QPointF &position) const;
    std::shared_ptr<const PreviewResult> result_;
    QImage image_;
    ViewSettings view_;
    PowerScale scale_ = PowerScale::Spectrum;
    int hoveredRow_ = -1;
    bool frameFrozen_ = false;
    std::optional<double> frequencyCursor_;
    std::optional<std::pair<std::size_t, std::size_t>> measurementRows_;
    bool measuring_ = false;
    MeasurementDrag measurementDrag_;
    double sampleRate_ = 1, center_ = 0, left_ = 0, right_ = 1;
};

class WaveformPlot : public QWidget
{
    Q_OBJECT
  public:
    explicit WaveformPlot(QWidget *parent = nullptr);
    void setWaveform(std::shared_ptr<const WaveformResult> result, double sampleRate,
                     SampleKind kind);
    void setMode(int mode);
    void setFrameCursor(std::optional<std::uint64_t> frame, bool frozen);
    void clear();
  signals:
    void cursorChanged(QString text);
    void frameHovered(quint64 frame);
    void frameClicked(quint64 frame);
    void cursorLeft();
    void frameSelected(quint64 frame);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

  private:
    const WavePoint *pointAt(const QPointF &position) const;
    std::shared_ptr<const WaveformResult> result_;
    std::optional<std::uint64_t> cursorFrame_;
    bool frameFrozen_ = false;
    double sampleRate_ = 1;
    SampleKind kind_ = SampleKind::Complex;
    int mode_ = 0;
};
} // namespace rf
