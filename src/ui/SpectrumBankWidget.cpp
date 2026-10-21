#include "SpectrumBankWidget.h"
#include "DisplayUnits.h"
#include <QCheckBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

namespace rf
{
namespace
{
QColor traceColor(std::size_t index)
{
    const auto hue = static_cast<float>(
        std::fmod(0.42 + static_cast<double>(index) * 0.61803398875, 1.0));
    return QColor::fromHsvF(hue, 0.65F, 0.95F);
}

QString traceDetails(const SavedSpectrum &trace)
{
    const auto &average = *trace.average;
    return QString("%1\nFrames [%2, %3) · FFT %4 · %5 · %6\n"
                   "%7 S/s · overlap %8% · DC removal %9 · conjugate %10\n"
                   "Complete linear-power mean · %11 valid / %12 invalid / %13 boundary windows"
                   " · %14 trailing frames · ENBW %15 Hz\nSource identity: %16")
        .arg(trace.recording.path).arg(average.range.begin).arg(average.range.end)
        .arg(trace.dsp.fftSize).arg(windowName(trace.dsp.window), powerUnit(trace.dsp.scale))
        .arg(trace.recording.sampleRate, 0, 'g', 12).arg(trace.dsp.overlapPercent)
        .arg(trace.dsp.removeDc ? "on" : "off", trace.dsp.conjugate ? "on" : "off")
        .arg(average.validWindows).arg(average.invalidWindows).arg(average.boundaryWindows)
        .arg(average.trailingFrames).arg(average.enbwHz, 0, 'g', 12).arg(trace.sourceIdentity);
}
} // namespace

// Dedicated baseband comparison plot: all data are already computed snapshots.
class SpectrumBankPlot : public QWidget
{
  public:
    explicit SpectrumBankPlot(QWidget *parent) : QWidget(parent)
    {
        setObjectName("spectrumBankPlot");
        setMinimumSize(320, 220);
        setMouseTracking(true);
    }
    void setTraces(std::vector<SavedSpectrum> traces, std::vector<QColor> colors,
                   const SavedSpectrum *first, const SavedSpectrum *second)
    {
        traces_ = std::move(traces);
        colors_ = std::move(colors);
        difference_.clear();
        label_.clear();
        if (first && second) {
            difference_ = spectrumDifference(*first, *second);
            frequencies_ = first->average->frequencies;
            label_ = first->name + " − " + second->name + " (dB)";
        } else if (!traces_.empty())
            frequencies_ = traces_.front().average->frequencies;
        else
            frequencies_.clear();
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#101923"));
        painter.setPen(QColor("#dce7f2"));
        const auto title = label_.isEmpty() ? QString("SAVED AVERAGES") : label_;
        painter.drawText(QRect(12, 6, width() - 24, 24), Qt::AlignLeft,
                         painter.fontMetrics().elidedText(title,
                                                         Qt::ElideRight, width() - 24));
        if (frequencies_.empty()) {
            painter.drawText(rect().adjusted(20, 35, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                             "Capture a completed average, then check compatible traces "
                             "to overlay.\n"
                             "Select two checked traces to compare their difference in dB.");
            return;
        }
        const bool difference = !difference_.empty();
        double minimum = difference ? 0 : std::numeric_limits<double>::infinity();
        double maximum = difference ? 0 : -std::numeric_limits<double>::infinity();
        const auto include = [&](double value) {
            if (std::isfinite(value)) {
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        };
        if (difference)
            for (const auto value : difference_)
                include(value);
        else
            for (const auto &trace : traces_)
                for (const auto power : trace.average->averagePower)
                    include(powerToDb(power));
        if (!std::isfinite(minimum)) {
            minimum = -100;
            maximum = 0;
        }
        // Keep ordinary spectra readable despite tiny numerical sidelobes.
        if (!difference)
            minimum = std::max(minimum, maximum - 100);
        const double padding = std::max(1.0, (maximum - minimum) * 0.05);
        minimum -= padding;
        maximum += padding;
        const QRectF plot(66, 36, width() - 86, height() - 88);
        const double left = frequencies_.front(), right = frequencies_.back();
        for (int tick = 0; tick <= 4; ++tick) {
            const double fraction = tick / 4.0;
            const double y = plot.bottom() - fraction * plot.height();
            painter.setPen(QColor("#293746"));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(QColor("#dce7f2"));
            painter.drawText(QRectF(0, y - 10, 58, 20), Qt::AlignRight | Qt::AlignVCenter,
                             decibelText(minimum + fraction * (maximum - minimum)));
        }
        const int columns = std::max(1, static_cast<int>(plot.width()));
        const auto draw = [&](std::span<const double> values, QColor color, bool decibels) {
            // Pixel maxima retain narrow events. A difference also retains each pixel minimum.
            painter.save();
            painter.setClipRect(plot);
            painter.setPen(QPen(color, 1.4));
            QPainterPath path;
            bool started = false;
            for (int column = 0; column < columns; ++column) {
                const double low = left + (right - left) * column / columns;
                const double high = left + (right - left) * (column + 1) / columns;
                const auto begin = std::lower_bound(frequencies_.begin(), frequencies_.end(), low);
                const auto end = std::upper_bound(begin, frequencies_.end(), high);
                double peak = -std::numeric_limits<double>::infinity();
                double trough = std::numeric_limits<double>::infinity();
                for (auto bin = begin; bin != end; ++bin) {
                    const auto index = static_cast<std::size_t>(bin - frequencies_.begin());
                    const double value = decibels ? values[index] : powerToDb(values[index]);
                    if (!std::isnan(value)) {
                        peak = std::max(peak, value);
                        trough = std::min(trough, value);
                    }
                }
                if (begin == end)
                    continue;
                if (trough > peak) {
                    started = false; // Undefined zero/zero differences leave a gap.
                    continue;
                }
                const auto y = [&](double value) {
                    return plot.bottom() - (std::clamp(value, minimum, maximum) - minimum) /
                                               (maximum - minimum) * plot.height();
                };
                const QPointF point(plot.left() + column, y(peak));
                if (started)
                    path.lineTo(point);
                else
                    path.moveTo(point);
                started = true;
                if (decibels)
                    painter.drawLine(point, QPointF(point.x(), y(trough)));
            }
            painter.drawPath(path);
            painter.restore();
        };
        if (difference)
            draw(difference_, QColor("#f9c74f"), true);
        else
            for (std::size_t index = 0; index < traces_.size(); ++index)
                draw(traces_[index].average->averagePower, colors_[index], false);
        painter.setPen(QColor("#dce7f2"));
        painter.drawText(QRectF(plot.left(), plot.bottom() + 4, plot.width(), 20), Qt::AlignLeft,
                         frequencyNumber(left, 1) + " Hz");
        painter.drawText(QRectF(plot.left(), plot.bottom() + 4, plot.width(), 20), Qt::AlignRight,
                         frequencyNumber(right, 1) + " Hz");
        painter.drawText(QRectF(plot.left(), height() - 24, plot.width(), 20), Qt::AlignCenter,
                         "Baseband frequency · " + (difference ? QString("difference (dB)")
                             : powerUnit(traces_.front().dsp.scale)));
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (frequencies_.empty())
            return;
        const double fraction = std::clamp((event->position().x() - 66) / (width() - 86), 0.0, 1.0);
        const double frequency =
            frequencies_.front() + fraction * (frequencies_.back() - frequencies_.front());
        auto bin = std::lower_bound(frequencies_.begin(), frequencies_.end(), frequency);
        if (bin == frequencies_.end())
            --bin;
        const auto index = static_cast<std::size_t>(bin - frequencies_.begin());
        QString text = frequencyNumber(*bin, 1) + " Hz";
        if (!difference_.empty()) {
            const auto value = std::isnan(difference_[index]) ? "undefined (zero / zero)"
                                                             : decibelText(difference_[index]);
            text += "\n" + label_ + ": " + value;
        } else
            for (const auto &trace : traces_)
                text += "\n" + trace.name + ": " +
                        decibelText(powerToDb(trace.average->averagePower[index])) +
                        " " + powerUnit(trace.dsp.scale);
        setToolTip(text);
    }

  private:
    std::vector<SavedSpectrum> traces_;
    std::vector<QColor> colors_;
    std::vector<double> frequencies_, difference_;
    QString label_;
};

SpectrumBankWidget::SpectrumBankWidget(QWidget *parent) : QWidget(parent)
{
    setObjectName("spectrumBank");
    auto *layout = new QVBoxLayout(this);
    auto *buttons = new QHBoxLayout;
    capture_ = new QPushButton("Capture average", this);
    capture_->setObjectName("captureSpectrumTrace");
    capture_->setEnabled(false);
    import_ = new QPushButton("Import trace…", this);
    import_->setObjectName("importSpectrumTrace");
    export_ = new QPushButton("Export trace…", this);
    export_->setObjectName("exportSpectrumTrace");
    remove_ = new QPushButton("Delete", this);
    remove_->setObjectName("deleteSpectrumTrace");
    for (auto *button : {capture_, import_, export_, remove_})
        buttons->addWidget(button);
    buttons->addStretch();
    layout->addLayout(buttons);
    auto *splitter = new QSplitter(this);
    list_ = new QListWidget(splitter);
    list_->setObjectName("spectrumBankLegend");
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setMinimumWidth(160);
    list_->setToolTip("Check to show/hide; double-click to rename. "
                      "Select two checked traces for A − B.");
    plot_ = new SpectrumBankPlot(splitter);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({220, 700});
    layout->addWidget(splitter, 1);
    difference_ = new QCheckBox("Show selected A − B (dB), in bank order", this);
    difference_->setObjectName("spectrumBankDifference");
    layout->addWidget(difference_);
    details_ = new QLabel(this);
    details_->setObjectName("spectrumBankDetails");
    details_->setWordWrap(true);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(details_);
    connect(capture_, &QPushButton::clicked, this, &SpectrumBankWidget::captureRequested);
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        auto &trace = traces_[static_cast<std::size_t>(list_->row(item))];
        if (!item->text().trimmed().isEmpty())
            trace.name = item->text().trimmed();
        const QSignalBlocker blocker(list_);
        item->setText(trace.name);
        trace.visible = item->checkState() == Qt::Checked;
        refresh();
    });
    connect(list_, &QListWidget::itemSelectionChanged, this, &SpectrumBankWidget::refresh);
    connect(difference_, &QCheckBox::toggled, this, &SpectrumBankWidget::refresh);
    connect(remove_, &QPushButton::clicked, this, [this] {
        for (int row = list_->count() - 1; row >= 0; --row)
            if (list_->item(row)->isSelected())
                traces_.erase(traces_.begin() + row);
        rebuildList();
    });
    connect(import_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Import spectrum trace", {},
                                                      "Spectrum trace (*.rftrace.json)");
        if (path.isEmpty())
            return;
        ioBusy_ = true;
        ioGeneration_ = ioController_.readTrace(path);
        refresh();
    });
    connect(export_, &QPushButton::clicked, this, [this] {
        const auto selected = list_->selectedItems();
        if (selected.size() != 1)
            return;
        const auto trace = traces_[static_cast<std::size_t>(list_->row(selected.front()))];
        const auto path = QFileDialog::getSaveFileName(
            this, "Export spectrum trace", "average.rftrace.json",
            "Spectrum trace (*.rftrace.json)");
        if (path.isEmpty())
            return;
        ioBusy_ = true;
        ioGeneration_ = ioController_.exportTrace(path, trace);
        refresh();
    });
    connect(&ioController_, &AnalysisController::traceReady, this,
            [this](quint64 generation, std::shared_ptr<const SavedSpectrum> trace) {
                if (generation != ioGeneration_)
                    return;
                try {
                    addTrace(*trace);
                } catch (const std::exception &exception) {
                    emit error(QString::fromUtf8(exception.what()));
                }
            });
    connect(&ioController_, &AnalysisController::finished, this,
            [this](quint64 generation, const QString &) {
                if (generation == ioGeneration_) {
                    ioBusy_ = false;
                    refresh();
                }
            });
    connect(&ioController_, &AnalysisController::failed, this,
            [this](quint64 generation, const QString &message) {
                if (generation != ioGeneration_)
                    return;
                ioBusy_ = false;
                refresh();
                emit error(message);
            });
    refresh();
}

void SpectrumBankWidget::setContext(const RecordingDescriptor &recording, const QString &identity,
                                    const DspSettings &dsp)
{
    recording_ = recording;
    identity_ = identity;
    dsp_ = dsp;
    refresh();
}

void SpectrumBankWidget::clear()
{
    ioController_.cancel();
    ++ioGeneration_;
    ioBusy_ = false;
    traces_.clear();
    identity_.clear();
    setCaptureAvailable(false);
    rebuildList();
}

void SpectrumBankWidget::setCaptureAvailable(bool available)
{
    capture_->setEnabled(available);
}

void SpectrumBankWidget::addTrace(SavedSpectrum trace)
{
    if (!spectrumCompatible(trace, recording_, identity_, dsp_))
        throw std::runtime_error(
            "Trace requires the same recording, sample interpretation and DSP settings");
    auto bank = traces_;
    bank.push_back(std::move(trace));
    validateSpectrumBank(bank);
    traces_ = std::move(bank);
    rebuildList();
    list_->setCurrentRow(list_->count() - 1);
}

void SpectrumBankWidget::setTraces(std::vector<SavedSpectrum> traces)
{
    validateSpectrumBank(traces);
    traces_ = std::move(traces);
    rebuildList();
}

void SpectrumBankWidget::rebuildList()
{
    const QSignalBlocker blocker(list_);
    list_->clear();
    for (std::size_t index = 0; index < traces_.size(); ++index) {
        const auto &trace = traces_[index];
        auto *item = new QListWidgetItem(trace.name, list_);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
        item->setCheckState(trace.visible ? Qt::Checked : Qt::Unchecked);
        QPixmap swatch(16, 16);
        swatch.fill(traceColor(index));
        item->setIcon(QIcon(swatch));
        item->setToolTip(traceDetails(trace));
    }
    refresh();
}

void SpectrumBankWidget::refresh()
{
    const QSignalBlocker blocker(list_);
    std::vector<SavedSpectrum> shown;
    std::vector<QColor> colors;
    std::vector<std::size_t> selected;
    for (std::size_t index = 0; index < traces_.size(); ++index) {
        const auto &trace = traces_[index];
        auto *item = list_->item(static_cast<int>(index));
        const bool compatible = spectrumCompatible(trace, recording_, identity_, dsp_);
        item->setForeground(compatible ? palette().color(QPalette::Text) : QColor(Qt::gray));
        const auto status = compatible ? QString()
            : QString("Incompatible with current source/settings; excluded.\n");
        item->setToolTip(status + traceDetails(trace));
        if (compatible && trace.visible) {
            shown.push_back(trace);
            colors.push_back(traceColor(index));
            if (item->isSelected())
                selected.push_back(index);
        }
    }
    const bool canCompare = selected.size() == 2 && list_->selectedItems().size() == 2 &&
                           spectraCompatible(traces_[selected[0]], traces_[selected[1]]);
    difference_->setEnabled(canCompare);
    if (!canCompare) {
        const QSignalBlocker differenceBlocker(difference_);
        difference_->setChecked(false);
    }
    const bool compare = canCompare && difference_->isChecked();
    plot_->setTraces(shown, colors, compare ? &traces_[selected[0]] : nullptr,
                                   compare ? &traces_[selected[1]] : nullptr);
    import_->setEnabled(!ioBusy_);
    export_->setEnabled(!ioBusy_ && list_->selectedItems().size() == 1);
    remove_->setEnabled(!list_->selectedItems().isEmpty());
    QString text = QString("%1 / %2 traces shown · baseband comparison · "
                           "double-click a name to rename")
                       .arg(shown.size()).arg(traces_.size());
    if (compare)
        text += "\n" + traces_[selected[0]].name + " − " + traces_[selected[1]].name +
                ": 10 log10(PA) − 10 log10(PB); zero/zero undefined, other zero powers ±∞.";
    if (const auto *item = list_->currentItem())
        text += "\n" + traceDetails(traces_[static_cast<std::size_t>(list_->row(item))]);
    if (ioBusy_)
        text.prepend("Trace file operation running…\n");
    details_->setText(text);
}
} // namespace rf
