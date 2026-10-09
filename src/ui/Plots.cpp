#include "Plots.h"
#include <QApplication>
#include <QKeyEvent>
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
const QColor measurementColor("#ffd166");
const QColor crosshairColor("#8ecae6");
MeasurementDrag::Target measurementTarget(double position, double start, double end)
{
    const double startDistance = std::abs(position - start);
    const double endDistance = std::abs(position - end);
    if (std::min(startDistance, endDistance) <= 6)
        return startDistance < endDistance ? MeasurementDrag::Target::Start
                                          : MeasurementDrag::Target::End;
    if (position > std::min(start, end) && position < std::max(start, end))
        return MeasurementDrag::Target::Band;
    return MeasurementDrag::Target::None;
}

std::pair<std::size_t, std::size_t> draggedMeasurement(const MeasurementDrag &drag,
                                                     std::size_t index, std::size_t count)
{
    auto selection = drag.original;
    if (drag.target == MeasurementDrag::Target::Start)
        selection.first = index;
    else if (drag.target == MeasurementDrag::Target::End)
        selection.second = index;
    else {
        const auto [first, last] = std::minmax(selection.first, selection.second);
        const auto delta = std::clamp(static_cast<std::ptrdiff_t>(index) -
                                         static_cast<std::ptrdiff_t>(drag.pressIndex),
                                     -static_cast<std::ptrdiff_t>(first),
                                     static_cast<std::ptrdiff_t>(count - 1 - last));
        selection.first =
            static_cast<std::size_t>(static_cast<std::ptrdiff_t>(selection.first) + delta);
        selection.second =
            static_cast<std::size_t>(static_cast<std::ptrdiff_t>(selection.second) + delta);
    }
    return selection;
}
QRectF area(const QWidget &widget)
{
    return QRectF(76, 32, std::max(1, widget.width() - 100), std::max(1, widget.height() - 70));
}
QString number(double value)
{
    return QString::number(value, 'g', 10);
}

QString frequencyText(double value)
{
    const double magnitude = std::abs(value);
    const double divisor = magnitude >= 1e9 ? 1e9 : magnitude >= 1e6 ? 1e6
                                                 : magnitude >= 1e3 ? 1e3 : 1;
    const QString unit = divisor == 1e9   ? "GHz"
                         : divisor == 1e6 ? "MHz"
                         : divisor == 1e3 ? "kHz" : "Hz";
    return QString::number(value / divisor, 'g', 15) + " " + unit;
}

QString durationText(double value)
{
    if (value > 0 && value < 1e-3)
        return number(value * 1e6) + " µs";
    if (value > 0 && value < 1)
        return number(value * 1e3) + " ms";
    return number(value) + " s";
}

void measurementReadout(QPainter &painter, const QRectF &plot, const QString &text)
{
    painter.save();
    painter.setClipRect(plot);
    const auto body = plot.adjusted(10, 8, -10, -8);
    const auto bounds = painter.boundingRect(body, Qt::AlignLeft | Qt::TextWordWrap, text);
    painter.fillRect(bounds.adjusted(-4, -3, 4, 3), QColor(16, 25, 35, 235));
    painter.setPen(measurementColor);
    painter.drawText(body, Qt::AlignLeft | Qt::TextWordWrap, text);
    painter.restore();
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
    if (palette == "Baudline")
        return qRgb(0, qRound(position * 249), qRound(position * 171));
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
double frameDifference(std::uint64_t frame, std::uint64_t origin)
{
    return frame >= origin ? static_cast<double>(frame - origin)
                           : -static_cast<double>(origin - frame);
}

FrameRange shiftedViewport(FrameRange range, long double delta, FrameRange extent)
{
    if (delta < 0) {
        const auto shift = static_cast<std::uint64_t>(
            std::min(std::round(-delta), static_cast<long double>(range.begin - extent.begin)));
        return {range.begin - shift, range.end - shift};
    }
    const auto shift = static_cast<std::uint64_t>(
        std::min(std::round(delta), static_cast<long double>(extent.end - range.end)));
    return {range.begin + shift, range.end + shift};
}

QImage waterfallImage(const PreviewResult &result, const ViewSettings &view,
                      std::pair<double, double> limits)
{
    const auto [minimum, maximum] = limits;
    std::array<QRgb, 4096> colors;
    for (std::size_t index = 0; index < colors.size(); ++index)
        colors[index] = paletteColor(view.palette, static_cast<double>(index) / (colors.size() - 1));
    QImage image(result.columns, static_cast<int>(result.rowStarts.size()), QImage::Format_RGB32);
    for (int row = 0; row < image.height(); ++row) {
        auto *pixels = reinterpret_cast<QRgb *>(image.scanLine(row));
        for (int column = 0; column < image.width(); ++column) {
            const double power = result.waterfall[static_cast<std::size_t>(row * image.width() + column)];
            if (!result.complete && static_cast<std::size_t>(row) >= result.completedRows)
                pixels[column] = qRgb(30, 43, 58);
            else if (std::isnan(power))
                pixels[column] = qRgb(160, 30, 130);
            else {
                const double position = std::clamp((powerToDb(power) - minimum) / (maximum - minimum),
                                                   0.0, 1.0);
                // Use rounding to retain the existing palettes' cell colors.
                pixels[column] = colors[static_cast<std::size_t>(std::round(position * (colors.size() - 1)))];
            }
        }
    }
    return image;
}
} // namespace

SpectrumPlot::SpectrumPlot(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(360, 180);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}
void SpectrumPlot::setPreview(std::shared_ptr<const PreviewResult> result, bool resetZoom)
{
    if (resetZoom || !preview_ || !result || preview_->frequencies != result->frequencies)
        clearMeasurement();
    preview_ = std::move(result);
    average_.reset();
    updateMeasurementPower();
    if (resetZoom)
        resetFrequency();
    else if (hoverPosition_)
        updateHover(*hoverPosition_);
    update();
}
void SpectrumPlot::setAverage(std::shared_ptr<const AverageResult> result, bool maxHold)
{
    if (average_ != result)
        clearMeasurement();
    average_ = std::move(result);
    preview_.reset();
    maxHold_ = maxHold;
    updateMeasurementPower();
    resetFrequency();
    update();
}
void SpectrumPlot::setView(ViewSettings view, PowerScale scale, double centerFrequency)
{
    if (scale_ != scale)
        clearMeasurement();
    view_ = std::move(view);
    scale_ = scale;
    center_ = centerFrequency;
    update();
}
void SpectrumPlot::clear()
{
    clearMeasurement();
    leaveEvent(nullptr);
    dragX_ = -1;
    preview_.reset();
    average_.reset();
    update();
}
void SpectrumPlot::clearMeasurement()
{
    measurementBins_.reset();
    measurementMean_.reset();
    const bool active = isMeasuring();
    measuring_ = false;
    measurementDrag_ = {};
    unsetCursor();
    if (active)
        emit measurementActiveChanged(false);
    update();
}
void SpectrumPlot::updateMeasurementPower()
{
    measurementMean_.reset();
    if (!measurementBins_ || measuring_ || (preview_ && !preview_->spectrum.valid) ||
        (average_ && average_->validWindows == 0))
        return;
    const auto power = powers();
    const auto [first, last] = std::minmax(measurementBins_->first, measurementBins_->second);
    if (last >= power.size())
        return;
    double mean = 0;
    for (auto bin = first; bin <= last; ++bin) {
        if (!std::isfinite(power[bin]) || power[bin] < 0)
            return;
        // Include zero-valued bins. An online linear mean avoids sum overflow.
        mean += (power[bin] - mean) / static_cast<double>(bin - first + 1);
    }
    measurementMean_ = mean;
}
QString SpectrumPlot::measurementText() const
{
    if (!measurementBins_)
        return {};
    const auto frequency = frequencies();
    const double first = frequency[measurementBins_->first];
    const double last = frequency[measurementBins_->second];
    const double center = view_.absoluteFrequency ? center_ : 0;
    const QString mean = !measurementMean_ ? "unavailable"
                         : *measurementMean_ == 0 ? "−∞ " + powerUnit(scale_)
                         : number(powerToDb(*measurementMean_)) + " " + powerUnit(scale_);
    const QString source = average_ ? (maxHold_ ? "Max-hold trace" : "Average trace")
                                   : QString("FFT frame %1").arg(preview_->spectrumStart);
    return QString("Start: %1 · End: %2\nΔf: %3\n%4\n%5")
        .arg(frequencyText(first + center), frequencyText(last + center),
             frequencyText(std::abs(last - first)),
             measuring_ ? "Click to set end · Esc to clear"
                        : "Mean spectral power: " + mean,
             source);
}
void SpectrumPlot::paintMeasurement(QPainter &painter)
{
    if (!measurementBins_)
        return;
    const auto plot = area(*this);
    const auto frequency = frequencies();
    const double startX = plot.left() + (frequency[measurementBins_->first] - left_) /
                                           (right_ - left_) * plot.width();
    const double endX = plot.left() + (frequency[measurementBins_->second] - left_) /
                                         (right_ - left_) * plot.width();
    painter.save();
    painter.setClipRect(plot);
    painter.fillRect(
        QRectF(std::min(startX, endX), plot.top(), std::abs(endX - startX), plot.height()),
        QColor(255, 209, 102, 35));
    painter.setPen(QPen(measurementColor, 1.5, isMeasuring() ? Qt::DashLine : Qt::SolidLine));
    painter.drawLine(QPointF(startX, plot.top()), QPointF(startX, plot.bottom()));
    painter.drawLine(QPointF(endX, plot.top()), QPointF(endX, plot.bottom()));
    painter.restore();
    measurementReadout(painter, plot, measurementText());
}
void SpectrumPlot::paintCrosshair(QPainter &painter, double minimum, double maximum)
{
    const auto plot = area(*this);
    if (!hoverPosition_ || !plot.contains(*hoverPosition_))
        return;
    const auto position = *hoverPosition_;
    const double frequency = left_ + (position.x() - plot.left()) / plot.width() * (right_ - left_) +
                             (view_.absoluteFrequency ? center_ : 0);
    const double level =
        minimum + (plot.bottom() - position.y()) / plot.height() * (maximum - minimum);
    painter.save();
    painter.setClipRect(plot);
    painter.setPen(QPen(crosshairColor, 1, Qt::DashLine));
    painter.drawLine(QPointF(position.x(), plot.top()), QPointF(position.x(), plot.bottom()));
    painter.drawLine(QPointF(plot.left(), position.y()), QPointF(plot.right(), position.y()));
    painter.restore();
    painter.save();
    const auto badge = [&painter](const QRectF &rect, const QString &text) {
        painter.fillRect(rect, background);
        painter.setPen(crosshairColor);
        painter.drawRect(rect);
        painter.drawText(rect, Qt::AlignCenter, text);
    };
    const double labelHeight = painter.fontMetrics().height() + 2;
    const auto frequencyLabel = frequencyText(std::round(frequency));
    const double labelWidth =
        std::min(plot.width(), painter.fontMetrics().horizontalAdvance(frequencyLabel) + 12.0);
    badge(QRectF(std::clamp(position.x() - labelWidth / 2, plot.left(), plot.right() - labelWidth),
                 plot.bottom() + 1, labelWidth, labelHeight), frequencyLabel);
    badge(QRectF(4, std::clamp(position.y() - labelHeight / 2, plot.top(), plot.bottom() - labelHeight),
                 68, labelHeight), QString::number(level, 'f', 2));
    painter.restore();
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
    if (hoverPosition_)
        updateHover(*hoverPosition_);
    update();
}
void SpectrumPlot::setFrequencyRange(double left, double right)
{
    if (std::isfinite(left) && std::isfinite(right) && left < right) {
        left_ = left;
        right_ = right;
        if (hoverPosition_)
            updateHover(*hoverPosition_);
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
        paintMeasurement(painter);
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
                         QString::number(minimum + fraction * (maximum - minimum), 'f', 1));
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
    paintCrosshair(painter, minimum, maximum);
    paintMeasurement(painter);
}

std::optional<std::size_t> SpectrumPlot::binAt(const QPointF &position) const
{
    const auto frequency = frequencies();
    const auto plot = area(*this);
    if (frequency.empty() || !plot.contains(position))
        return {};
    const double target = left_ + (position.x() - plot.left()) / plot.width() * (right_ - left_);
    const auto found = std::lower_bound(frequency.begin(), frequency.end(), target);
    if (found == frequency.end())
        return frequency.size() - 1;
    auto bin = static_cast<std::size_t>(found - frequency.begin());
    if (bin > 0 && target - frequency[bin - 1] <= frequency[bin] - target)
        --bin;
    return bin;
}

void SpectrumPlot::mouseMoveEvent(QMouseEvent *event)
{
    const auto plot = area(*this);
    if (measurementDrag_.target != MeasurementDrag::Target::None) {
        measurementDrag_.moved |= std::abs(event->position().x() -
                                          measurementDrag_.pressPosition.x()) >=
                                  QApplication::startDragDistance();
        if (measurementDrag_.moved) {
            const QPointF position(std::clamp(event->position().x(), plot.left(), plot.right()),
                                   std::clamp(event->position().y(), plot.top(), plot.bottom()));
            const auto selection = draggedMeasurement(measurementDrag_, *binAt(position),
                                                       frequencies().size());
            if (selection != *measurementBins_) {
                measurementBins_ = selection;
                updateMeasurementPower();
                update();
            }
        }
        updateHover(event->position());
        return;
    }
    if (measuring_) {
        if (const auto bin = binAt(event->position())) {
            measurementBins_->second = *bin;
            update();
        }
        updateHover(event->position());
        return;
    }
    if (dragX_ >= 0) {
        const double delta =
            (event->position().x() - dragX_) / plot.width() * (dragRight_ - dragLeft_);
        left_ = dragLeft_ - delta;
        right_ = dragRight_ - delta;
        emit frequencyRangeChanged(left_, right_);
        update();
    }
    updateHover(event->position());
}

void SpectrumPlot::updateHover(const QPointF &position)
{
    const auto plot = area(*this);
    const auto frequency = frequencies();
    const auto power = powers();
    if (frequency.empty() || power.empty() || !plot.contains(position)) {
        leaveEvent(nullptr);
        return;
    }
    hoverPosition_ = position;
    const double target =
        left_ + (position.x() - plot.left()) / plot.width() * (right_ - left_);
    emit frequencyHovered(target);
    auto found = std::lower_bound(frequency.begin(), frequency.end(), target);
    const auto index =
        std::min(static_cast<std::size_t>(found - frequency.begin()), power.size() - 1);
    const auto [minimum, maximum] = colorLimits(view_, power);
    const double level =
        minimum + (plot.bottom() - position.y()) / plot.height() * (maximum - minimum);
    emit cursorChanged(QString("Bin %1 · %2 Hz · %3 %4 · Cursor: %5 Hz · %6 %4")
                           .arg(index)
                           // A GHz center plus a narrow FFT bin needs more
                           // precision than the compact axis tick labels.
                           .arg(QString::number(
                               frequency[index] + (view_.absoluteFrequency ? center_ : 0), 'g', 17))
                           .arg(number(powerToDb(power[index])))
                           .arg(powerUnit(scale_))
                           .arg(QString::number(
                               std::round(target + (view_.absoluteFrequency ? center_ : 0)), 'f', 0))
                           .arg(number(level)));
    update();
}

void SpectrumPlot::leaveEvent(QEvent *)
{
    if (hoverPosition_) {
        hoverPosition_.reset();
        emit cursorLeft();
    }
    update();
}

void SpectrumPlot::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (hoverPosition_)
        updateHover(*hoverPosition_);
}

void SpectrumPlot::wheelEvent(QWheelEvent *event)
{
    if (measurementDrag_.target != MeasurementDrag::Target::None) {
        event->accept();
        return;
    }
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
    updateHover(event->position());
    update();
    event->accept();
}
void SpectrumPlot::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton &&
        (measuring_ || (event->modifiers() & Qt::ShiftModifier))) {
        if (const auto bin = binAt(event->position())) {
            setFocus(Qt::MouseFocusReason);
            dragX_ = -1;
            if (measurementBins_ && !measuring_) {
                const auto plot = area(*this);
                const auto frequency = frequencies();
                const double start = plot.left() + (frequency[measurementBins_->first] - left_) /
                                                       (right_ - left_) * plot.width();
                const double end = plot.left() + (frequency[measurementBins_->second] - left_) /
                                                     (right_ - left_) * plot.width();
                const auto target = measurementTarget(event->position().x(), start, end);
                if (target != MeasurementDrag::Target::None) {
                    measurementDrag_ = {target, *measurementBins_, event->position(), *bin, false};
                    setCursor(target == MeasurementDrag::Target::Band ? Qt::SizeAllCursor
                                                                      : Qt::SizeHorCursor);
                    emit measurementActiveChanged(true);
                    event->accept();
                    return;
                }
            }
            if (measuring_)
                measurementBins_->second = *bin;
            else
                measurementBins_ = std::pair{*bin, *bin};
            measuring_ = !measuring_;
            updateMeasurementPower();
            emit measurementActiveChanged(measuring_);
            update();
        }
        event->accept();
        return;
    }
    if (isMeasuring())
        return;
    if (event->button() == Qt::LeftButton) {
        dragX_ = static_cast<int>(event->position().x());
        dragLeft_ = left_;
        dragRight_ = right_;
    }
}
void SpectrumPlot::mouseReleaseEvent(QMouseEvent *event)
{
    dragX_ = -1;
    if (event->button() != Qt::LeftButton ||
        measurementDrag_.target == MeasurementDrag::Target::None)
        return;
    mouseMoveEvent(event);
    const auto drag = measurementDrag_;
    measurementDrag_ = {};
    unsetCursor();
    if (!drag.moved) {
        measurementBins_ = std::pair{drag.pressIndex, drag.pressIndex};
        measuring_ = true;
        measurementMean_.reset();
    } else
        emit measurementActiveChanged(false);
    update();
    event->accept();
}
void SpectrumPlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    mousePressEvent(event);
}
void SpectrumPlot::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && measurementBins_) {
        clearMeasurement();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

WaterfallPlot::WaterfallPlot(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(360, 210);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
}
void WaterfallPlot::setPreview(std::shared_ptr<const PreviewResult> result, double sampleRate,
                               bool resetFrequency)
{
    clearMeasurement();
    if (!dragTimeSelection_ || resetFrequency) {
        dragPosition_.reset();
        dragTimeSelection_ = false;
    }
    const double rows = result_ ? static_cast<double>(result_->rowStarts.size()) : 0;
    const double topFraction = !resetFrequency && rows > 0 ? top_ / rows : 0;
    const double bottomFraction = !resetFrequency && rows > 0 ? bottom_ / rows : 1;
    result_ = std::move(result);
    hoveredRow_ = -1;
    if (resetFrequency)
        colorRange_.reset();
    frameFrozen_ = false;
    sampleRate_ = sampleRate;
    const double newRows = result_ ? static_cast<double>(result_->rowStarts.size()) : 1;
    top_ = topFraction * newRows;
    bottom_ = bottomFraction * newRows;
    if (resetFrequency || (!dragTimeSelection_ && result_ && timeSelectionRange_ &&
                            *timeSelectionRange_ == result_->range))
        timeSelectionRange_.reset();
    if (resetFrequency && result_ && result_->frequencies.size() > 1) {
        left_ = result_->frequencies.front();
        right_ = fullFrequencyRight();
    }
    if (dragTimeSelection_ && dragMoved_)
        setCursor(Qt::ClosedHandCursor);
    rebuildImage();
    update();
}
void WaterfallPlot::setRecordingExtent(FrameRange extent, std::uint64_t minimumSpan)
{
    extent_ = extent;
    minimumSpan_ = std::min(extent.size(), minimumSpan);
}
void WaterfallPlot::setViewport(FrameRange range)
{
    const auto span = std::clamp(range.size(), minimumSpan_, extent_.size());
    const auto begin = std::clamp(range.begin, extent_.begin, extent_.end - span);
    const FrameRange next{begin, begin + span};
    if (viewport_ == next)
        return;
    viewport_ = next;
    update();
    emit viewportChanged(next);
}
QSize WaterfallPlot::pixelSize() const
{
    const auto plot = area(*this);
    return {qRound(plot.width() * devicePixelRatioF()), qRound(plot.height() * devicePixelRatioF())};
}
void WaterfallPlot::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    emit detailRequested();
}
bool WaterfallPlot::event(QEvent *event)
{
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::ScreenChangeInternal
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        || event->type() == QEvent::DevicePixelRatioChange
#endif
    )
        emit detailRequested();
    return handled;
}
void WaterfallPlot::setDetail(std::shared_ptr<const PreviewResult> result)
{
    // Keep gesture indices stable until completion; the next request hits cache.
    if (isMeasuring())
        return;
    result_ = std::move(result);
    top_ = 0;
    bottom_ = static_cast<double>(result_->rowStarts.size());
    if (measurementFrames_) {
        const auto index = [this](std::uint64_t frame) {
            const auto row = std::upper_bound(result_->rowStarts.begin(), result_->rowStarts.end(), frame);
            return row == result_->rowStarts.begin() ? std::size_t{0}
                       : static_cast<std::size_t>(row - result_->rowStarts.begin() - 1);
        };
        measurementRows_ = std::pair{index(measurementFrames_->first), index(measurementFrames_->second)};
    }
    rebuildImage();
    setFrameCursor(cursorFrame_, frameFrozen_);
    update();
}
void WaterfallPlot::rememberMeasurement()
{
    if (measurementRows_)
        measurementFrames_ = std::pair{result_->rowStarts[measurementRows_->first],
                                      result_->rowStarts[measurementRows_->second]};
}
void WaterfallPlot::setTimeSelectionRange(FrameRange range)
{
    timeSelectionRange_ = range;
    update();
}
void WaterfallPlot::setView(ViewSettings view, PowerScale scale, double centerFrequency)
{
    if (view_.autoRange != view.autoRange || scale_ != scale)
        colorRange_.reset();
    const bool recolor = view_.colorMin != view.colorMin || view_.colorMax != view.colorMax ||
                         view_.autoRange != view.autoRange || view_.palette != view.palette ||
                         view_.waterfallAutoRangeOnZoom != view.waterfallAutoRangeOnZoom || scale_ != scale;
    view_ = std::move(view);
    scale_ = scale;
    center_ = centerFrequency;
    if (recolor)
        rebuildImage();
    update();
}
void WaterfallPlot::setFrequencyRange(double left, double right)
{
    if (std::isfinite(left) && std::isfinite(right) && left < right) {
        const bool changed = left_ != left || right_ != right;
        left_ = left;
        right_ = right;
        if (changed)
            emit detailRequested();
        update();
    }
}
void WaterfallPlot::setFrequencyCursor(std::optional<double> frequency)
{
    frequencyCursor_ = frequency;
    update();
}
void WaterfallPlot::setFrameCursor(std::optional<std::uint64_t> frame, bool frozen)
{
    cursorFrame_ = frame;
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
    clearMeasurement();
    dragPosition_.reset();
    dragTimeSelection_ = false;
    timeSelectionRange_.reset();
    result_.reset();
    viewport_.reset();
    cursorFrame_.reset();
    hoveredRow_ = -1;
    frameFrozen_ = false;
    image_ = {};
    colorRange_.reset();
    frequencyCursor_.reset();
    update();
}
void WaterfallPlot::clearMeasurement()
{
    measurementRows_.reset();
    measurementFrames_.reset();
    const bool active = isMeasuring();
    measuring_ = false;
    measurementDrag_ = {};
    unsetCursor();
    if (active)
        emit measurementActiveChanged(false);
    emit measurementChanged();
    update();
}
std::optional<FrameRange> WaterfallPlot::measurementRange() const
{
    if (!measurementRows_)
        return {};
    const auto first = measurementFrames_->first;
    const auto last = measurementFrames_->second;
    return FrameRange{std::min(first, last), std::max(first, last)};
}
QString WaterfallPlot::measurementText() const
{
    if (!measurementRows_)
        return {};
    const auto first = measurementFrames_->first;
    const auto last = measurementFrames_->second;
    // Subtract exact frame indices before conversion, even above 2^53.
    const auto frames = first < last ? last - first : first - last;
    const double duration = static_cast<double>(frames) / sampleRate_;
    return QString("Start: %1 s · frame %2\nEnd: %3 s · frame %4\nDuration: %5 · %6 frames%7")
        .arg(number(static_cast<double>(first) / sampleRate_)).arg(first)
        .arg(number(static_cast<double>(last) / sampleRate_)).arg(last)
        .arg(durationText(duration)).arg(frames)
        .arg(measuring_ ? "\nClick to set end · Esc to clear" : "");
}
void WaterfallPlot::rebuildImage()
{
    if (result_) {
        if (!colorRange_ || !view_.autoRange || view_.waterfallAutoRangeOnZoom)
            colorRange_ = colorLimits(view_, result_->waterfall);
        image_ = waterfallImage(*result_, view_, *colorRange_);
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
    const double fullLeft = result_->waterfallBand ? result_->waterfallBand->first
                                                  : result_->frequencies.front();
    const double fullRight = result_->waterfallBand ? result_->waterfallBand->second
                                                   : fullFrequencyRight();
    const QRectF source((left_ - fullLeft) / (fullRight - fullLeft) * image_.width(), visibleTop(),
                        (right_ - left_) / (fullRight - fullLeft) * image_.width(),
                        viewport_ ? static_cast<double>(viewport_->size()) /
                                        static_cast<double>(result_->range.size()) * image_.height()
                                  : bottom_ - top_);
    painter.drawImage(plot, image_, source);
    if (hoveredRow_ >= 0) {
        const double rowHeight = viewport_
            ? static_cast<double>(result_->range.size()) / static_cast<double>(viewport_->size()) *
                  plot.height() / static_cast<double>(result_->rowStarts.size())
            : plot.height() / (bottom_ - top_);
        painter.save();
        painter.setClipRect(plot);
        painter.fillRect(QRectF(plot.left(), rowY(static_cast<std::size_t>(hoveredRow_)) -
                                               rowHeight / 2, plot.width(), rowHeight),
                         QColor(255, 255, 255, frameFrozen_ ? 180 : 90));
        painter.restore();
    }
    if (frequencyCursor_ && *frequencyCursor_ >= left_ && *frequencyCursor_ <= right_) {
        const double x = plot.left() + (*frequencyCursor_ - left_) / (right_ - left_) * plot.width();
        painter.save();
        painter.setClipRect(plot);
        painter.setPen(QPen(crosshairColor, 1, Qt::DashLine));
        painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        painter.restore();
    }
    painter.setPen(foreground);
    const auto timeRange = viewport_.value_or(timeSelectionRange_.value_or(result_->range));
    const double timeScale = static_cast<double>(timeRange.end) / sampleRate_ < 0.1 ? 1000 : 1;
    for (int tick = 0; tick <= 4; ++tick) {
        const double fraction = tick / 4.0;
        const auto row = std::min(
            static_cast<std::size_t>(top_ + fraction * (bottom_ - top_)),
            result_->rowStarts.size() - 1);
        const double time = viewport_
            ? (static_cast<double>(viewport_->begin) + fraction * static_cast<double>(viewport_->size())) / sampleRate_
            : static_cast<double>(timeRange.begin) / sampleRate_ +
                            static_cast<double>(result_->rowStarts[row] - result_->range.begin) /
                                sampleRate_;
        painter.drawText(QRectF(0, plot.top() + fraction * plot.height() - 9, 68, 18),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(time * timeScale, 'g', 5));
    }
    const auto [minimum, maximum] = *colorRange_;
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
    if (measurementRows_) {
        const double startY = viewport_ ? measurementY(measurementFrames_->first) : rowY(measurementRows_->first);
        const double endY = viewport_ ? measurementY(measurementFrames_->second) : rowY(measurementRows_->second);
        painter.save();
        painter.setClipRect(plot);
        painter.fillRect(
            QRectF(plot.left(), std::min(startY, endY), plot.width(), std::abs(endY - startY)),
            QColor(255, 209, 102, 35));
        painter.setPen(QPen(measurementColor, 1.5, isMeasuring() ? Qt::DashLine : Qt::SolidLine));
        painter.drawLine(QPointF(plot.left(), startY), QPointF(plot.right(), startY));
        painter.drawLine(QPointF(plot.left(), endY), QPointF(plot.right(), endY));
        painter.restore();
        measurementReadout(painter, plot, measurementText());
    }
}
double WaterfallPlot::fullFrequencyRight() const
{
    const auto &frequency = result_->frequencies;
    return frequency.back() + (frequency.front() < 0 ? frequency[1] - frequency[0] : 0);
}
double WaterfallPlot::rowY(std::size_t row) const
{
    const auto plot = area(*this);
    if (viewport_) {
        const double offset = (static_cast<double>(row) + 0.5) /
                                  static_cast<double>(result_->rowStarts.size()) *
                                  static_cast<double>(result_->range.size());
        return plot.top() + (frameDifference(result_->range.begin, viewport_->begin) + offset) /
                               static_cast<double>(viewport_->size()) * plot.height();
    }
    return plot.top() + (static_cast<double>(row) + 0.5 - visibleTop()) /
                           (bottom_ - top_) * plot.height();
}
double WaterfallPlot::visibleTop() const
{
    if (result_ && viewport_)
        return frameDifference(viewport_->begin, result_->range.begin) /
               static_cast<double>(result_->range.size()) * image_.height();
    if (!result_ || !timeSelectionRange_)
        return top_;
    const auto begin = timeSelectionRange_->begin;
    const auto original = result_->range.begin;
    // Use integer differences so narrow slices remain precise beyond 2^53.
    const double delta = begin >= original ? static_cast<double>(begin - original)
                                          : -static_cast<double>(original - begin);
    return top_ + delta / static_cast<double>(result_->range.size()) *
                      static_cast<double>(result_->rowStarts.size());
}
std::optional<std::size_t> WaterfallPlot::rowAt(const QPointF &position) const
{
    if (!result_ || result_->rowStarts.empty() || !area(*this).contains(position))
        return {};
    const auto plot = area(*this);
    if (viewport_) {
        const double fraction = (position.y() - plot.top()) / plot.height();
        const double offset = frameDifference(viewport_->begin, result_->range.begin) +
                              fraction * static_cast<double>(viewport_->size());
        const double row = offset / static_cast<double>(result_->range.size()) *
                           static_cast<double>(result_->rowStarts.size());
        if (row < 0 || row > static_cast<double>(result_->rowStarts.size()))
            return {};
        return std::min(static_cast<std::size_t>(row), result_->rowStarts.size() - 1);
    }
    const double row = visibleTop() + (position.y() - plot.top()) / plot.height() * (bottom_ - top_);
    if (row < 0 || row > static_cast<double>(result_->rowStarts.size()))
        return {};
    return std::min(static_cast<std::size_t>(row),
                    result_->rowStarts.size() - 1);
}
double WaterfallPlot::frameY(std::uint64_t frame) const
{
    const auto plot = area(*this);
    return plot.top() + frameDifference(frame, viewport_->begin) /
                           static_cast<double>(viewport_->size()) * plot.height();
}
double WaterfallPlot::measurementY(std::uint64_t frame) const
{
    const auto row = std::lower_bound(result_->rowStarts.begin(), result_->rowStarts.end(), frame);
    if (row != result_->rowStarts.end() && *row == frame)
        return rowY(static_cast<std::size_t>(row - result_->rowStarts.begin()));
    return frameY(frame);
}
void WaterfallPlot::mouseMoveEvent(QMouseEvent *event)
{
    if (measurementDrag_.target != MeasurementDrag::Target::None) {
        const auto plot = area(*this);
        measurementDrag_.moved |= std::abs(event->position().y() -
                                          measurementDrag_.pressPosition.y()) >=
                                  QApplication::startDragDistance();
        if (measurementDrag_.moved) {
            const QPointF position(std::clamp(event->position().x(), plot.left(), plot.right()),
                                   std::clamp(event->position().y(), plot.top(), plot.bottom()));
            const auto selection = draggedMeasurement(measurementDrag_, *rowAt(position),
                                                       result_->rowStarts.size());
            if (viewport_) {
                auto frames = measurementDragFrames_;
                const auto index = *rowAt(position);
                const auto frame = result_->rowStarts[index];
                if (measurementDrag_.target == MeasurementDrag::Target::Start)
                    frames.first = frame;
                else if (measurementDrag_.target == MeasurementDrag::Target::End)
                    frames.second = frame;
                else {
                    const auto [first, last] = std::minmax(frames.first, frames.second);
                    const auto pressFrame = result_->rowStarts[measurementDrag_.pressIndex];
                    const long double delta = frame >= pressFrame
                        ? static_cast<long double>(frame - pressFrame)
                        : -static_cast<long double>(pressFrame - frame);
                    const auto moved = shiftedViewport({first, last}, delta,
                                                       {extent_.begin, extent_.end - minimumSpan_});
                    frames = frames.first <= frames.second ? std::pair{moved.begin, moved.end}
                                                          : std::pair{moved.end, moved.begin};
                }
                if (frames != *measurementFrames_) {
                    measurementFrames_ = frames;
                    measurementRows_ = selection;
                    emit measurementChanged();
                    update();
                }
                return;
            }
            if (selection != *measurementRows_) {
                measurementRows_ = selection;
                rememberMeasurement();
                emit measurementChanged();
                update();
            }
        }
        return;
    }
    const auto row = rowAt(event->position());
    if (measuring_) {
        if (row) {
            measurementRows_->second = *row;
            rememberMeasurement();
            emit measurementChanged();
            update();
        }
        return;
    }
    if (dragPosition_) {
        dragMoved_ |= (event->position() - *dragPosition_).manhattanLength() >=
                      QApplication::startDragDistance();
        if (dragMoved_) {
            const auto plot = area(*this);
            if (dragTimeSelection_) {
                const double fraction = -(event->position().y() - dragPosition_->y()) /
                                        plot.height() * (dragBottom_ - dragTop_) /
                                        dragRowCount_;
                emit timeSelectionPanChanged(fraction);
                setCursor(Qt::ClosedHandCursor);
                event->accept();
                return;
            }
            if (!dragTimeOnly_) {
                const double fullLeft = result_->frequencies.front();
                const double fullRight = fullFrequencyRight();
                const double span = std::min(dragRight_ - dragLeft_, fullRight - fullLeft);
                const double delta =
                    (event->position().x() - dragPosition_->x()) / plot.width() * span;
                left_ = std::clamp(dragLeft_ - delta, fullLeft, fullRight - span);
                right_ = left_ + span;
            }
            if (viewport_) {
                const long double delta = -(event->position().y() - dragPosition_->y()) /
                                          plot.height() * static_cast<long double>(dragViewport_.size());
                setViewport(shiftedViewport(dragViewport_, delta, extent_));
                if (!dragTimeOnly_)
                    emit frequencyRangeChanged(left_, right_);
                emit detailRequested();
                setCursor(Qt::ClosedHandCursor);
                event->accept();
                return;
            }
            const double rows = dragBottom_ - dragTop_;
            const double rowDelta =
                (event->position().y() - dragPosition_->y()) / plot.height() * rows;
            top_ = std::clamp(dragTop_ - rowDelta, 0.0,
                              static_cast<double>(result_->rowStarts.size()) - rows);
            bottom_ = top_ + rows;
            setCursor(Qt::ClosedHandCursor);
            if (!dragTimeOnly_)
                emit frequencyRangeChanged(left_, right_);
            update();
        }
        event->accept();
        return;
    }
    updateHover(event->position());
}
void WaterfallPlot::updateHover(const QPointF &position)
{
    const auto row = rowAt(position);
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
        left_ + (position.x() - plot.left()) / plot.width() * (right_ - left_) +
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
void WaterfallPlot::wheelEvent(QWheelEvent *event)
{
    if (dragPosition_ || measurementDrag_.target != MeasurementDrag::Target::None) {
        event->accept();
        return;
    }
    if (!result_ || image_.isNull() || result_->frequencies.size() < 2 ||
        !area(*this).contains(event->position()) || event->angleDelta().y() == 0) {
        event->ignore();
        return;
    }
    const auto plot = area(*this);
    const double factor = event->angleDelta().y() > 0 ? 0.8 : 1.25;
    const auto &frequency = result_->frequencies;
    const double fullRight = fullFrequencyRight();
    const double fraction = (event->position().x() - plot.left()) / plot.width();
    const double anchor = left_ + fraction * (right_ - left_);
    const double span = std::clamp((right_ - left_) * factor, frequency[1] - frequency[0],
                                  fullRight - frequency.front());
    left_ = std::clamp(anchor - fraction * span, frequency.front(), fullRight - span);
    right_ = left_ + span;
    const double rowFraction = (event->position().y() - plot.top()) / plot.height();
    if (viewport_) {
        const auto spanFrames = static_cast<std::uint64_t>(std::clamp(
            std::round(static_cast<long double>(viewport_->size()) * factor),
            static_cast<long double>(minimumSpan_), static_cast<long double>(extent_.size())));
        const long double delta = rowFraction *
            (static_cast<long double>(viewport_->size()) - spanFrames);
        // Grow around the pointer, then clamp without converting absolute frames to double.
        const auto baseBegin = std::min(viewport_->begin, extent_.end - spanFrames);
        const auto next = shiftedViewport({baseBegin, baseBegin + spanFrames},
            delta + static_cast<long double>(viewport_->begin - baseBegin), extent_);
        setViewport(next);
        emit frequencyRangeChanged(left_, right_);
        emit detailRequested();
        update();
        event->accept();
        return;
    }
    const double rowAnchor = top_ + rowFraction * (bottom_ - top_);
    const double rowCount = static_cast<double>(result_->rowStarts.size());
    const double rows = std::clamp((bottom_ - top_) * factor, 1.0, rowCount);
    top_ = std::clamp(rowAnchor - rowFraction * rows, 0.0, rowCount - rows);
    bottom_ = top_ + rows;
    emit frequencyRangeChanged(left_, right_);
    if (!measuring_)
        updateHover(event->position());
    update();
    event->accept();
}
void WaterfallPlot::mousePressEvent(QMouseEvent *event)
{
    const auto row = rowAt(event->position());
    if (event->button() == Qt::LeftButton &&
        (measuring_ || (event->modifiers() & Qt::ShiftModifier))) {
        if (row) {
            setFocus(Qt::MouseFocusReason);
            dragPosition_.reset();
            if (measurementRows_ && !measuring_) {
                const double start = viewport_ ? measurementY(measurementFrames_->first) : rowY(measurementRows_->first);
                const double end = viewport_ ? measurementY(measurementFrames_->second) : rowY(measurementRows_->second);
                const auto target = measurementTarget(event->position().y(), start, end);
                if (target != MeasurementDrag::Target::None) {
                    measurementDrag_ = {target, *measurementRows_, event->position(), *row, false};
                    measurementDragFrames_ = *measurementFrames_;
                    setCursor(target == MeasurementDrag::Target::Band ? Qt::SizeAllCursor
                                                                      : Qt::SizeVerCursor);
                    emit measurementActiveChanged(true);
                    emit measurementChanged();
                    event->accept();
                    return;
                }
            }
            if (measuring_)
                measurementRows_->second = *row;
            else
                measurementRows_ = std::pair{*row, *row};
            rememberMeasurement();
            measuring_ = !measuring_;
            emit measurementActiveChanged(measuring_);
            emit measurementChanged();
            update();
        }
        event->accept();
        return;
    }
    if (isMeasuring())
        return;
    if (event->button() == Qt::LeftButton && row) {
        dragPosition_ = event->position();
        dragMoved_ = false;
        dragTimeOnly_ = event->modifiers().testFlag(Qt::ControlModifier);
        dragTimeSelection_ = !viewport_ && dragTimeOnly_ && timeSelectionPanEnabled_;
        dragViewport_ = viewport_.value_or(FrameRange{});
        dragLeft_ = left_;
        dragRight_ = right_;
        dragTop_ = top_;
        dragBottom_ = bottom_;
        dragRowCount_ = static_cast<double>(result_->rowStarts.size());
        event->accept();
        if (dragTimeSelection_)
            emit timeSelectionPanActiveChanged(true);
    }
}
void WaterfallPlot::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && dragPosition_) {
        mouseMoveEvent(event);
        dragPosition_.reset();
        unsetCursor();
        if (dragTimeSelection_) {
            dragTimeSelection_ = false;
            emit timeSelectionPanActiveChanged(false);
        }
        if (!dragMoved_)
            if (const auto row = rowAt(event->position()))
                emit frameClicked(result_->rowStarts[*row]);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton ||
        measurementDrag_.target == MeasurementDrag::Target::None)
        return;
    mouseMoveEvent(event);
    const auto drag = measurementDrag_;
    measurementDrag_ = {};
    unsetCursor();
    if (!drag.moved) {
        measurementRows_ = std::pair{drag.pressIndex, drag.pressIndex};
        rememberMeasurement();
        measuring_ = true;
    } else
        emit measurementActiveChanged(false);
    emit measurementChanged();
    update();
    event->accept();
}
void WaterfallPlot::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (measuring_ || (event->modifiers() & Qt::ShiftModifier)) {
        mousePressEvent(event);
        return;
    }
    if (isMeasuring())
        return;
    dragPosition_.reset();
    unsetCursor();
    const auto row = rowAt(event->position());
    if (row)
        emit frameSelected(result_->rowStarts[*row]);
}
void WaterfallPlot::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && measurementRows_) {
        clearMeasurement();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

WaterfallMinimap::WaterfallMinimap(QWidget *parent) : QWidget(parent)
{
    setFixedWidth(96);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName("Waterfall minimap");
    setAccessibleDescription("Drag the viewport box to move through time; drag its top or bottom edge to resize it.");
}
QRectF WaterfallMinimap::mapRect() const
{
    return QRectF(6, 32, width() - 12, std::max(1, height() - 70));
}
QRectF WaterfallMinimap::selectorRect() const
{
    if (extent_.size() == 0)
        return {};
    const auto plot = mapRect();
    const double top = plot.top() + static_cast<double>(viewport_.begin - extent_.begin) /
                                       static_cast<double>(extent_.size()) * plot.height();
    const double bottom = plot.top() + static_cast<double>(viewport_.end - extent_.begin) /
                                          static_cast<double>(extent_.size()) * plot.height();
    return QRectF(plot.left(), top, plot.width(), bottom - top);
}
void WaterfallMinimap::setRecording(FrameRange extent, std::uint64_t minimumSpan)
{
    extent_ = extent;
    minimumSpan_ = std::min(extent.size(), minimumSpan);
    viewport_ = extent;
    target_ = MeasurementDrag::Target::None;
    result_.reset();
    image_ = {};
    update();
}
void WaterfallMinimap::setViewport(FrameRange range)
{
    viewport_ = range;
    update();
}
void WaterfallMinimap::setSnapshot(std::shared_ptr<const PreviewResult> result)
{
    result_ = std::move(result);
    rebuildImage();
    update();
}
void WaterfallMinimap::setView(ViewSettings view)
{
    const bool recolor = view_.colorMin != view.colorMin || view_.colorMax != view.colorMax ||
                         view_.autoRange != view.autoRange || view_.palette != view.palette;
    view_ = std::move(view);
    if (recolor)
        rebuildImage();
    update();
}
void WaterfallMinimap::clear()
{
    extent_ = {};
    viewport_ = {};
    target_ = MeasurementDrag::Target::None;
    result_.reset();
    image_ = {};
    unsetCursor();
    update();
}
void WaterfallMinimap::rebuildImage()
{
    if (result_)
        image_ = waterfallImage(*result_, view_, colorLimits(view_, result_->waterfall));
}
std::uint64_t WaterfallMinimap::frameAt(double y) const
{
    const auto plot = mapRect();
    const long double fraction = std::clamp((y - plot.top()) / plot.height(), 0.0, 1.0);
    const auto offset = static_cast<std::uint64_t>(std::min(
        std::round(fraction * extent_.size()), static_cast<long double>(extent_.size())));
    return extent_.begin + offset;
}
void WaterfallMinimap::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), background);
    painter.setPen(foreground);
    painter.drawText(QRectF(0, 4, width(), 24), Qt::AlignCenter, "MINIMAP");
    const auto plot = mapRect();
    painter.fillRect(plot, QColor(30, 43, 58));
    if (!image_.isNull())
        painter.drawImage(plot, image_);
    if (extent_.size() == 0)
        return;
    const auto box = selectorRect();
    painter.fillRect(box, QColor(142, 202, 230, 45));
    painter.setPen(QPen(crosshairColor, 2));
    painter.drawRect(box);
    // Distinct handles remain usable even when the true box is subpixel tall.
    const double middle = plot.center().x();
    painter.drawLine(QPointF(middle - 10, box.top() - 2), QPointF(middle + 10, box.top() - 2));
    painter.drawLine(QPointF(middle - 10, box.bottom() + 2), QPointF(middle + 10, box.bottom() + 2));
    painter.setPen(foreground);
    const auto text = !result_ ? QString("Waiting…")
                     : result_->complete ? QString("Complete")
                     : QString("%1%").arg(result_->completedRows * 100 / result_->rowStarts.size());
    painter.drawText(QRectF(0, height() - 24, width(), 20), Qt::AlignCenter, text);
}
void WaterfallMinimap::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || extent_.size() == 0 ||
        !mapRect().adjusted(0, -6, 0, 6).contains(event->position()))
        return;
    setFocus(Qt::MouseFocusReason);
    const auto box = selectorRect();
    const double y = event->position().y();
    const double topDistance = std::abs(y - (box.top() - 2));
    const double bottomDistance = std::abs(y - (box.bottom() + 2));
    if (std::min(topDistance, bottomDistance) <= 6)
        target_ = topDistance < bottomDistance ? MeasurementDrag::Target::Start
                                              : MeasurementDrag::Target::End;
    else {
        target_ = MeasurementDrag::Target::Band;
        if (!box.contains(event->position())) {
            const auto frame = frameAt(y);
            const auto span = viewport_.size();
            const auto begin = std::clamp(frame >= span / 2 ? frame - span / 2 : extent_.begin,
                                          extent_.begin, extent_.end - span);
            viewport_ = {begin, begin + span};
            emit viewportChanged(viewport_);
        }
    }
    pressFrame_ = frameAt(y);
    dragViewport_ = viewport_;
    setCursor(target_ == MeasurementDrag::Target::Band ? Qt::ClosedHandCursor : Qt::SizeVerCursor);
    update();
    event->accept();
}
void WaterfallMinimap::mouseMoveEvent(QMouseEvent *event)
{
    if (extent_.size() == 0)
        return;
    if (target_ == MeasurementDrag::Target::None) {
        const auto box = selectorRect();
        const double y = event->position().y();
        if (std::min(std::abs(y - box.top() + 2), std::abs(y - box.bottom() - 2)) <= 6)
            setCursor(Qt::SizeVerCursor);
        else if (box.contains(event->position()))
            setCursor(Qt::OpenHandCursor);
        else
            unsetCursor();
        return;
    }
    const auto frame = frameAt(event->position().y());
    FrameRange next = dragViewport_;
    if (target_ == MeasurementDrag::Target::Start)
        next.begin = std::clamp(frame, extent_.begin, dragViewport_.end - minimumSpan_);
    else if (target_ == MeasurementDrag::Target::End)
        next.end = std::clamp(frame, dragViewport_.begin + minimumSpan_, extent_.end);
    else {
        const long double delta = frame >= pressFrame_ ? static_cast<long double>(frame - pressFrame_)
                                                       : -static_cast<long double>(pressFrame_ - frame);
        next = shiftedViewport(dragViewport_, delta, extent_);
    }
    if (next != viewport_) {
        viewport_ = next;
        update();
        emit viewportChanged(next);
    }
    event->accept();
}
void WaterfallMinimap::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && target_ != MeasurementDrag::Target::None) {
        mouseMoveEvent(event);
        target_ = MeasurementDrag::Target::None;
        unsetCursor();
        event->accept();
    }
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
                         .arg(QString::number(static_cast<double>(result_->range.begin) / sampleRate_,
                                              'f', 3))
                         .arg(QString::number(static_cast<double>(result_->range.end) / sampleRate_,
                                              'f', 3))
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
