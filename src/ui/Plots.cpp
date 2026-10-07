#include "Plots.h"
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace rf
{
namespace
{
const QColor background("#101923"), grid("#2a3b4b"), foreground("#c6d5df"), trace("#51d3be");
QRectF area(const QWidget &widget)
{
    return QRectF(76, 32, std::max(1, widget.width() - 100), std::max(1, widget.height() - 70));
}
QString number(double value)
{
    return QString::number(value, 'g', 10);
}

void base(QPainter &painter, QWidget &widget, const QString &title)
{
    painter.fillRect(widget.rect(), background);
    painter.setPen(foreground);
    painter.drawText(QRectF(12, 4, widget.width() - 24, 24), Qt::AlignLeft | Qt::AlignVCenter,
                     title);
    painter.setPen(grid);
    painter.drawRect(area(widget));
}

std::pair<double, double> colorLimits(const ViewSettings &view, std::span<const double> power)
{
    if (!view.autoRange)
        return {view.colorMin, view.colorMax};
    double maximum = -std::numeric_limits<double>::infinity();
    for (const auto value : power)
        if (std::isfinite(value) && value > 0)
            maximum = std::max(maximum, powerToDb(value));
    if (!std::isfinite(maximum))
        return {view.colorMin, view.colorMax};
    return {maximum - 80, maximum + 3};
}

QRgb paletteColor(const QString &palette, double position)
{
    // Small sampled control tables keep rendering independent of matplotlib
    // or external assets. Linear interpolation gives a smooth bundled palette.
    static const std::array<QColor, 6> viridis{QColor("#440154"), QColor("#414487"),
                                               QColor("#2a788e"), QColor("#22a884"),
                                               QColor("#7ad151"), QColor("#fde725")};
    static const std::array<QColor, 6> inferno{QColor("#000004"), QColor("#420a68"),
                                               QColor("#932667"), QColor("#dd513a"),
                                               QColor("#fca50a"), QColor("#fcffa4")};
    static const std::array<QColor, 6> turbo{QColor("#30123b"), QColor("#4675ed"),
                                             QColor("#1ae4b6"), QColor("#a4fc3c"),
                                             QColor("#f9ba38"), QColor("#7a0403")};
    const auto &colors = palette == "Inferno" ? inferno : palette == "Turbo" ? turbo : viridis;
    position = std::clamp(position, 0.0, 1.0);
    if (palette == "Grayscale") {
        const int gray = qRound(position * 255);
        return qRgb(gray, gray, gray);
    }
    const double scaled = position * static_cast<double>(colors.size() - 1);
    const auto index = std::min(static_cast<std::size_t>(scaled), colors.size() - 2);
    const double fraction = scaled - static_cast<double>(index);
    const auto blend = [fraction](int first, int second) {
        return qRound(first + (second - first) * fraction);
    };
    return qRgb(blend(colors[index].red(), colors[index + 1].red()),
                blend(colors[index].green(), colors[index + 1].green()),
                blend(colors[index].blue(), colors[index + 1].blue()));
}
} // namespace

SpectrumPlot::SpectrumPlot(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(360, 180);
    setMouseTracking(true);
}
void SpectrumPlot::setPreview(std::shared_ptr<const PreviewResult> result, bool resetZoom)
{
    preview_ = std::move(result);
    average_.reset();
    if (resetZoom)
        resetFrequency();
    update();
}
void SpectrumPlot::setAverage(std::shared_ptr<const AverageResult> result, bool maxHold)
{
    average_ = std::move(result);
    preview_.reset();
    maxHold_ = maxHold;
    resetFrequency();
    update();
}
void SpectrumPlot::setView(ViewSettings view, PowerScale scale, double centerFrequency)
{
    view_ = std::move(view);
    scale_ = scale;
    center_ = centerFrequency;
    update();
}
void SpectrumPlot::clear()
{
    preview_.reset();
    average_.reset();
    update();
}
std::span<const double> SpectrumPlot::frequencies() const
{
    if (preview_)
        return preview_->frequencies;
    if (average_)
        return average_->frequencies;
    return {};
}
std::span<const double> SpectrumPlot::powers() const
{
    if (preview_)
        return preview_->spectrum.power;
    if (average_)
        return maxHold_ ? std::span(average_->maxPower) : std::span(average_->averagePower);
    return {};
}
void SpectrumPlot::resetFrequency()
{
    const auto frequency = frequencies();
    if (frequency.size() > 1) {
        left_ = frequency.front();
        // Complex FFT bins omit +Nyquist, but the display covers the full
        // [-Fs/2, Fs/2] band. Real spectra already include their Nyquist bin.
        right_ = frequency.back() + (left_ < 0 ? frequency[1] - frequency[0] : 0);
        emit frequencyRangeChanged(left_, right_);
    }
    update();
}
void SpectrumPlot::setFrequencyRange(double left, double right)
{
    if (std::isfinite(left) && std::isfinite(right) && left < right) {
        left_ = left;
        right_ = right;
        update();
    }
}

void SpectrumPlot::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const auto power = powers();
    const auto frequency = frequencies();
    base(painter, *this,
         average_ ? (maxHold_ ? "MAX HOLD · complete interval pass"
                              : "AVERAGE SPECTRUM · linear-power mean")
                  : preview_ ? QString("FREQUENCY SPECTRUM · exact window at frame %1")
                                   .arg(preview_->spectrumStart)
                             : "FREQUENCY SPECTRUM");
    if (power.empty() || frequency.empty()) {
        painter.setPen(foreground);
        painter.drawText(area(*this), Qt::AlignCenter,
                         preview_ ? "Invalid/discontinuous window" : "Open an RF recording");
        return;
    }
    const auto plot = area(*this);
    const auto [minimum, maximum] = colorLimits(view_, power);
    const double extent = std::max(std::abs(left_ + (view_.absoluteFrequency ? center_ : 0)),
                                   std::abs(right_ + (view_.absoluteFrequency ? center_ : 0)));
    const double divisor = extent >= 1e9 ? 1e9 : extent >= 1e6 ? 1e6 : extent >= 1e3 ? 1e3 : 1;
    const QString unit = divisor == 1e9   ? "GHz"
                         : divisor == 1e6 ? "MHz"
                         : divisor == 1e3 ? "kHz"
                                          : "Hz";
    for (int tick = 0; tick <= 4; ++tick) {
        const double fraction = tick / 4.0;
        const double x = plot.left() + fraction * plot.width();
        const double y = plot.bottom() - fraction * plot.height();
        painter.setPen(grid);
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(foreground);
        painter.drawText(QRectF(0, y - 9, 68, 18), Qt::AlignRight | Qt::AlignVCenter,
                         number(minimum + fraction * (maximum - minimum)));
        const double labelX = std::clamp(x - 65, 0.0, static_cast<double>(width() - 130));
        painter.drawText(
            QRectF(labelX, plot.bottom() + 5, 130, 18), Qt::AlignCenter,
            number((left_ + fraction * (right_ - left_) + (view_.absoluteFrequency ? center_ : 0)) /
                   divisor));
    }
    painter.drawText(
        QRectF(plot.left(), height() - 20, plot.width(), 18), Qt::AlignCenter,
        QString("%1 frequency (%2) · %3 · bin maxima")
            .arg(view_.absoluteFrequency ? "Absolute" : "Baseband", unit, powerUnit(scale_)));
    QPainterPath path;
    const int columns = std::max(1, static_cast<int>(plot.width()));
    bool started = false;
    for (int column = 0; column < columns; ++column) {
        const double startFrequency = left_ + (right_ - left_) * column / columns;
        const double endFrequency = left_ + (right_ - left_) * (column + 1) / columns;
        auto begin = std::lower_bound(frequency.begin(), frequency.end(), startFrequency);
        auto end = std::upper_bound(begin, frequency.end(), endFrequency);
        if (begin == end)
            continue;
        const auto offset = static_cast<std::size_t>(begin - frequency.begin());
        const auto count = static_cast<std::size_t>(end - begin);
        const double peak =
            *std::max_element(power.begin() + static_cast<std::ptrdiff_t>(offset),
                              power.begin() + static_cast<std::ptrdiff_t>(offset + count));
        const double db = std::clamp(powerToDb(peak), minimum, maximum);
        const QPointF point(plot.left() + column,
                            plot.bottom() - (db - minimum) / (maximum - minimum) * plot.height());
        if (!started) {
            path.moveTo(point);
            started = true;
        } else
            path.lineTo(point);
    }
    painter.save();
    painter.setClipRect(plot);
    painter.setPen(QPen(trace, 1.3));
    painter.drawPath(path);
    painter.restore();
}

void SpectrumPlot::mouseMoveEvent(QMouseEvent *event)
{
    const auto plot = area(*this);
    if (dragX_ >= 0) {
        const double delta =
            (event->position().x() - dragX_) / plot.width() * (dragRight_ - dragLeft_);
        left_ = dragLeft_ - delta;
        right_ = dragRight_ - delta;
        emit frequencyRangeChanged(left_, right_);
        update();
    }
    const auto frequency = frequencies();
    const auto power = powers();
    if (frequency.empty() || power.empty() || !plot.contains(event->position()))
        return;
    const double target =
        left_ + (event->position().x() - plot.left()) / plot.width() * (right_ - left_);
    auto found = std::lower_bound(frequency.begin(), frequency.end(), target);
    const auto index =
        std::min(static_cast<std::size_t>(found - frequency.begin()), power.size() - 1);
    emit cursorChanged(QString("Bin %1 · %2 Hz · %3 %4")
                           .arg(index)
                           // A GHz center plus a narrow FFT bin needs more
                           // precision than the compact axis tick labels.
                           .arg(QString::number(
                               frequency[index] + (view_.absoluteFrequency ? center_ : 0), 'g', 17))
                           .arg(number(powerToDb(power[index])))
                           .arg(powerUnit(scale_)));
}

void SpectrumPlot::wheelEvent(QWheelEvent *event)
{
    const auto frequency = frequencies();
    if (frequency.size() < 2)
        return;
    const auto plot = area(*this);
    const double fraction =
        std::clamp((event->position().x() - plot.left()) / plot.width(), 0.0, 1.0);
    const double anchor = left_ + fraction * (right_ - left_);
    const double span =
        std::clamp((right_ - left_) * (event->angleDelta().y() > 0 ? 0.8 : 1.25),
                   frequency[1] - frequency[0], frequency.back() - frequency.front());
    left_ = std::clamp(anchor - fraction * span, frequency.front(), frequency.back() - span);
    right_ = left_ + span;
    emit frequencyRangeChanged(left_, right_);
    update();
    event->accept();
}
void SpectrumPlot::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        dragX_ = static_cast<int>(event->position().x());
        dragLeft_ = left_;
        dragRight_ = right_;
    }
}
void SpectrumPlot::mouseReleaseEvent(QMouseEvent *)
{
    dragX_ = -1;
}

WaterfallPlot::WaterfallPlot(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(360, 210);
    setMouseTracking(true);
}
void WaterfallPlot::setPreview(std::shared_ptr<const PreviewResult> result, double sampleRate)
{
    result_ = std::move(result);
    hoveredRow_ = -1;
    frameFrozen_ = false;
    sampleRate_ = sampleRate;
    if (result_ && result_->frequencies.size() > 1) {
        left_ = result_->frequencies.front();
        right_ = result_->frequencies.back();
    }
    rebuildImage();
    update();
}
void WaterfallPlot::setView(ViewSettings view, PowerScale scale, double centerFrequency)
{
    view_ = std::move(view);
    scale_ = scale;
    center_ = centerFrequency;
    rebuildImage();
    update();
}
void WaterfallPlot::setFrequencyRange(double left, double right)
{
    left_ = left;
    right_ = right;
    update();
}
void WaterfallPlot::setFrameCursor(std::optional<std::uint64_t> frame, bool frozen)
{
    frameFrozen_ = frozen;
    hoveredRow_ = -1;
    if (result_ && frame && *frame >= result_->range.begin && *frame < result_->range.end) {
        const auto row = std::upper_bound(result_->rowStarts.begin(), result_->rowStarts.end(),
                                          *frame);
        if (row != result_->rowStarts.begin())
            hoveredRow_ = static_cast<int>(row - result_->rowStarts.begin() - 1);
    }
    update();
}
void WaterfallPlot::clear()
{
    result_.reset();
    hoveredRow_ = -1;
    frameFrozen_ = false;
    image_ = {};
    update();
}
void WaterfallPlot::rebuildImage()
{
    if (!result_)
        return;
    const auto [minimum, maximum] = colorLimits(view_, result_->waterfall);
    image_ =
        QImage(result_->columns, static_cast<int>(result_->rowStarts.size()), QImage::Format_RGB32);
    for (int row = 0; row < image_.height(); ++row) {
        auto *pixels = reinterpret_cast<QRgb *>(image_.scanLine(row));
        for (int column = 0; column < image_.width(); ++column) {
            const double power =
                result_->waterfall[static_cast<std::size_t>(row * image_.width() + column)];
            pixels[column] = std::isnan(power)
                                 ? qRgb(160, 30, 130)
                                 : paletteColor(view_.palette,
                                                (powerToDb(power) - minimum) / (maximum - minimum));
        }
    }
}
void WaterfallPlot::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    base(painter, *this,
         result_ && result_->aggregated ? "WATERFALL · EXACT PASS · temporal/frequency maxima"
         : result_ && result_->sampled
             ? "WATERFALL · SAMPLED PREVIEW · skipped windows may contain bursts"
             : "WATERFALL · all window starts shown · frequency cells use maxima");
    if (!result_ || image_.isNull())
        return;
    const auto plot = area(*this);
    const double fullLeft = result_->frequencies.front();
    const double fullRight =
        result_->frequencies.back() + (fullLeft < 0 ? result_->frequencies[1] - fullLeft : 0);
    const QRectF source((left_ - fullLeft) / (fullRight - fullLeft) * image_.width(), 0,
                        (right_ - left_) / (fullRight - fullLeft) * image_.width(),
                        image_.height());
    painter.drawImage(plot, image_, source);
    if (hoveredRow_ >= 0) {
        const double rowHeight = plot.height() / static_cast<double>(result_->rowStarts.size());
        painter.fillRect(QRectF(plot.left(), plot.top() + hoveredRow_ * rowHeight, plot.width(),
                                rowHeight),
                         QColor(255, 255, 255, frameFrozen_ ? 180 : 90));
    }
    painter.setPen(foreground);
    const double timeScale = static_cast<double>(result_->range.end) / sampleRate_ < 0.1 ? 1000 : 1;
    for (int tick = 0; tick <= 4; ++tick) {
        const double fraction = tick / 4.0;
        const auto row = std::min(
            static_cast<std::size_t>(fraction * static_cast<double>(result_->rowStarts.size())),
            result_->rowStarts.size() - 1);
        const double time = static_cast<double>(result_->rowStarts[row]) / sampleRate_;
        painter.drawText(QRectF(0, plot.top() + fraction * plot.height() - 9, 68, 18),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(time * timeScale, 'g', 5));
    }
    const auto [minimum, maximum] = colorLimits(view_, result_->waterfall);
    painter.drawText(QRectF(plot.left(), height() - 24, plot.width(), 20), Qt::AlignCenter,
                     QString("Time (%6) · %1 rows · %2 … %3 %4 · %5")
                         .arg(result_->rowStarts.size())
                         .arg(number(minimum))
                         .arg(number(maximum))
                         .arg(powerUnit(scale_))
                         .arg(view_.palette)
                         .arg(timeScale == 1000 ? "ms" : "s"));
    const QRectF legend(plot.right() + 6, plot.top(), 10, plot.height());
    for (int y = 0; y < static_cast<int>(legend.height()); ++y) {
        painter.setPen(QColor::fromRgb(paletteColor(view_.palette, 1.0 - y / legend.height())));
        painter.drawLine(QPointF(legend.left(), legend.top() + y),
                         QPointF(legend.right(), legend.top() + y));
    }
}
std::optional<std::size_t> WaterfallPlot::rowAt(const QPointF &position) const
{
    if (!result_ || !area(*this).contains(position))
        return {};
    const auto plot = area(*this);
    return std::min(static_cast<std::size_t>((position.y() - plot.top()) / plot.height() *
                                            static_cast<double>(result_->rowStarts.size())),
                    result_->rowStarts.size() - 1);
}
void WaterfallPlot::mouseMoveEvent(QMouseEvent *event)
{
    const auto row = rowAt(event->position());
    if (!row) {
        leaveEvent(nullptr);
        return;
    }
    const auto plot = area(*this);
    if (!frameFrozen_ && hoveredRow_ != static_cast<int>(*row)) {
        hoveredRow_ = static_cast<int>(*row);
        update();
    }
    const double frequency =
        left_ + (event->position().x() - plot.left()) / plot.width() * (right_ - left_) +
        (view_.absoluteFrequency ? center_ : 0);
    emit cursorChanged(QString("%1 row %2 · frame %3 · %4 s · %5 Hz · click to %6 · "
                               "double-click to seek")
                           .arg(result_->aggregated ? "Overview" : "Preview")
                           .arg(*row)
                           .arg(result_->rowStarts[*row])
                           .arg(number(static_cast<double>(result_->rowStarts[*row]) / sampleRate_))
                           .arg(QString::number(frequency, 'g', 17))
                           .arg(frameFrozen_ ? "unfreeze" : "freeze"));
    emit frameHovered(result_->rowStarts[*row]);
}
void WaterfallPlot::leaveEvent(QEvent *)
{
    if (!frameFrozen_) {
        hoveredRow_ = -1;
        update();
    }
    emit cursorLeft();
}
void WaterfallPlot::mousePressEvent(QMouseEvent *event)
{
    const auto row = rowAt(event->position());
    if (event->button() == Qt::LeftButton && row)
        emit frameClicked(result_->rowStarts[*row]);
}
void WaterfallPlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    const auto row = rowAt(event->position());
    if (row)
        emit frameSelected(result_->rowStarts[*row]);
}

WaveformPlot::WaveformPlot(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(360, 180);
    setMouseTracking(true);
}
void WaveformPlot::setWaveform(std::shared_ptr<const WaveformResult> result, double sampleRate,
                               SampleKind kind)
{
    result_ = std::move(result);
    sampleRate_ = sampleRate;
    kind_ = kind;
    update();
}
void WaveformPlot::setMode(int mode)
{
    mode_ = mode;
    update();
}
void WaveformPlot::setFrameCursor(std::optional<std::uint64_t> frame, bool frozen)
{
    cursorFrame_ = frame;
    frameFrozen_ = frozen;
    update();
}
void WaveformPlot::clear()
{
    result_.reset();
    cursorFrame_.reset();
    frameFrozen_ = false;
    update();
}
void WaveformPlot::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    base(painter, *this,
         result_ && !result_->complete
             ? "WAVEFORM · bounded region preview · use Exact waveform for the full selection"
             : "WAVEFORM · exact min/max envelopes");
    if (!result_ || result_->points.empty())
        return;
    const auto plot = area(*this);
    double minimum = mode_ == 1 ? 0 : -1, maximum = 1;
    for (const auto &point : result_->points)
        if (point.valid) {
            const bool both = mode_ == 0 && kind_ == SampleKind::Complex;
            minimum = std::min(minimum, mode_ == 1 ? point.minMagnitude
                                        : both     ? std::min(point.minI, point.minQ)
                                                   : point.minI);
            maximum = std::max(maximum, mode_ == 1 ? point.maxMagnitude
                                        : both     ? std::max(point.maxI, point.maxQ)
                                                   : point.maxI);
        }
    // Normalize coordinates before subtracting extrema; +/- DBL_MAX would
    // overflow a direct maximum-minimum even when individual values are finite.
    const double coordinateScale = std::max(std::abs(minimum), std::abs(maximum));
    const double lowScaled = minimum / coordinateScale;
    const double span = maximum / coordinateScale - lowScaled;
    const auto y = [&](double value) {
        return plot.bottom() - (value / coordinateScale - lowScaled) / span * plot.height();
    };
    painter.setPen(grid);
    painter.drawLine(QPointF(plot.left(), y(0)), QPointF(plot.right(), y(0)));
    painter.save();
    painter.setClipRect(plot);
    for (const auto &point : result_->points) {
        if (!point.valid)
            continue;
        const double x = plot.left() + static_cast<double>(point.first - result_->range.begin) /
                                           static_cast<double>(result_->range.size()) *
                                           plot.width();
        const double low = mode_ == 1 ? point.minMagnitude : point.minI;
        const double high = mode_ == 1 ? point.maxMagnitude : point.maxI;
        painter.setPen(QPen(trace, 1.2));
        painter.drawLine(QPointF(x, y(low)), QPointF(x, y(high)));
        painter.drawPoint(QPointF(x, y(high)));
        if (mode_ == 0 && kind_ == SampleKind::Complex) {
            painter.setPen(QPen(QColor("#f7b75b"), 1.2));
            painter.drawLine(QPointF(x, y(point.minQ)), QPointF(x, y(point.maxQ)));
            painter.drawPoint(QPointF(x, y(point.maxQ)));
        }
    }
    if (cursorFrame_ && *cursorFrame_ >= result_->range.begin &&
        *cursorFrame_ < result_->range.end) {
        const double x = plot.left() +
                         static_cast<double>(*cursorFrame_ - result_->range.begin) /
                             static_cast<double>(result_->range.size()) * plot.width();
        painter.fillRect(QRectF(x - 1, plot.top(), 2, plot.height()),
                         QColor(255, 255, 255, frameFrozen_ ? 180 : 90));
    }
    painter.restore();
    painter.setPen(foreground);
    painter.drawText(QRectF(0, plot.top() - 8, 68, 18), Qt::AlignRight, number(maximum));
    painter.drawText(QRectF(0, plot.bottom() - 10, 68, 18), Qt::AlignRight, number(minimum));
    painter.drawText(QRectF(plot.left(), height() - 24, plot.width(), 20), Qt::AlignCenter,
                     QString("%1 … %2 s · frames [%3, %4) · %5 invalid · %6 clipped components")
                         .arg(number(static_cast<double>(result_->range.begin) / sampleRate_))
                         .arg(number(static_cast<double>(result_->range.end) / sampleRate_))
                         .arg(result_->range.begin)
                         .arg(result_->range.end)
                         .arg(result_->invalidSamples)
                         .arg(result_->clippedComponents));
}
const WavePoint *WaveformPlot::pointAt(const QPointF &position) const
{
    if (!result_ || result_->points.empty() || !area(*this).contains(position))
        return nullptr;
    const auto plot = area(*this);
    const auto index =
        std::min(static_cast<std::size_t>((position.x() - plot.left()) / plot.width() *
                                         static_cast<double>(result_->points.size())),
                 result_->points.size() - 1);
    return &result_->points[index];
}
void WaveformPlot::mouseMoveEvent(QMouseEvent *event)
{
    const auto *point = pointAt(event->position());
    if (!point) {
        leaveEvent(nullptr);
        return;
    }
    if (!frameFrozen_) {
        cursorFrame_ = point->first;
        update();
    }
    emit cursorChanged(
        QString("Frames [%1, %2) · I [%3, %4] · Q [%5, %6] · click to %7 · double-click to seek")
            .arg(point->first)
            .arg(point->last)
            .arg(number(point->minI))
            .arg(number(point->maxI))
            .arg(number(point->minQ))
            .arg(number(point->maxQ))
            .arg(frameFrozen_ ? "unfreeze" : "freeze"));
    emit frameHovered(point->first);
}
void WaveformPlot::leaveEvent(QEvent *)
{
    if (!frameFrozen_) {
        cursorFrame_.reset();
        update();
    }
    emit cursorLeft();
}
void WaveformPlot::mousePressEvent(QMouseEvent *event)
{
    const auto *point = pointAt(event->position());
    if (event->button() == Qt::LeftButton && point)
        emit frameClicked(point->first);
}
void WaveformPlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    const auto *point = pointAt(event->position());
    if (point)
        emit frameSelected(point->first);
}
} // namespace rf
