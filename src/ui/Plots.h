#pragma once
#include "app/Session.h"
#include <QImage>
#include <QWidget>

namespace rf
{
// Plot widgets only consume immutable snapshots. They never read a recording,
// run an FFT, or change the source arrays when reducing data for screen pixels.
class SpectrumPlot : public QWidget
{
    Q_OBJECT
  public:
    explicit SpectrumPlot(QWidget *parent = nullptr);
    void setPreview(std::shared_ptr<const PreviewResult> result);
    void setAverage(std::shared_ptr<const AverageResult> result, bool maxHold);
    void setView(ViewSettings view, PowerScale scale, double centerFrequency);
    void clear();
    void resetFrequency();
    void setFrequencyRange(double left, double right);
  signals:
    void cursorChanged(QString text);
    void frequencyRangeChanged(double left, double right);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

  private:
    std::span<const double> frequencies() const;
    std::span<const double> powers() const;
    std::shared_ptr<const PreviewResult> preview_;
    std::shared_ptr<const AverageResult> average_;
    ViewSettings view_;
    PowerScale scale_ = PowerScale::Spectrum;
    bool maxHold_ = false;
    double center_ = 0;
    double left_ = 0, right_ = 1;
    double dragLeft_ = 0, dragRight_ = 1;
    int dragX_ = -1;
};

class WaterfallPlot : public QWidget
{
    Q_OBJECT
  public:
    explicit WaterfallPlot(QWidget *parent = nullptr);
    void setPreview(std::shared_ptr<const PreviewResult> result, double sampleRate);
    void setView(ViewSettings view, PowerScale scale, double centerFrequency);
    void setFrequencyRange(double left, double right);
    void clear();
  signals:
    void cursorChanged(QString text);
    void frameHovered(quint64 frame);
    void frameSelected(quint64 frame);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

  private:
    void rebuildImage();
    std::shared_ptr<const PreviewResult> result_;
    QImage image_;
    ViewSettings view_;
    PowerScale scale_ = PowerScale::Spectrum;
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
    void clear();
  signals:
    void cursorChanged(QString text);
    void frameHovered(quint64 frame);
    void frameSelected(quint64 frame);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;

  private:
    std::shared_ptr<const WaveformResult> result_;
    double sampleRate_ = 1;
    SampleKind kind_ = SampleKind::Complex;
    int mode_ = 0;
};
} // namespace rf
