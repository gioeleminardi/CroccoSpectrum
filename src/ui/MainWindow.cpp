#include "MainWindow.h"
#include "ImportDialog.h"
#include "recording/Metadata.h"
#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>

// Q_INIT_RESOURCE must run outside a namespace. This explicit reference also
// keeps the resource object linked when rf_ui is built as a static library.
static void initializeBrandingResources()
{
    Q_INIT_RESOURCE(branding);
}

namespace rf
{
namespace
{
QDockWidget *dock(QMainWindow *window, const QString &name, const QString &id, QWidget *content,
                  Qt::DockWidgetArea side)
{
    auto *panel = new QDockWidget(name, window);
    panel->setObjectName(id);
    panel->setWidget(content);
    window->addDockWidget(side, panel);
    return panel;
}
QString number(double value)
{
    return QString::number(value, 'g', 9);
}
} // namespace

MainWindow::MainWindow(QWidget *parent, QString preferencesPath) : QMainWindow(parent)
{
    initializeBrandingResources();
    setWindowTitle("CroccoSpectrum");
    setWindowIcon(QIcon(":/branding/CroccoSpectrumIcon.png"));
    resize(1440, 980);
    const bool defaultPreferences = preferencesPath.isEmpty();
    preferencesPath_ = preferencesPath.isEmpty()
                           ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) +
                                 "/preferences.json"
                           : std::move(preferencesPath);
    QString restorePath = preferencesPath_;
    // Read the previous application's preferences only on the first renamed
    // launch. Normal saves then use the new location; the old file is retained.
    if (defaultPreferences && !QFileInfo::exists(restorePath)) {
        const auto legacyPath =
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
            "/RFAnalyzer/rf-analyzer/preferences.json";
        if (QFileInfo::exists(legacyPath))
            restorePath = legacyPath;
    }
    QString settingsError;
    if (QFileInfo::exists(restorePath)) {
        try {
            preferences_ = readPreferences(restorePath);
        } catch (const std::exception &error) {
            settingsError =
                "Saved settings could not be restored: " + QString::fromUtf8(error.what());
            // Preserve the rejected file before a later normal preference save.
            const QString backup = restorePath + ".rejected-" +
                                   QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmsszzz");
            if (!QFile::copy(restorePath, backup))
                settingsError += " (backup failed; automatic preference writes disabled)";
            if (!QFileInfo::exists(backup))
                preferencesPath_.clear();
        }
    }
    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(100);
    saveTimer_ = new QTimer(this);
    saveTimer_->setSingleShot(true);
    saveTimer_->setInterval(400);
    connect(previewTimer_, &QTimer::timeout, this, &MainWindow::requestPreview);
    connect(saveTimer_, &QTimer::timeout, this, &MainWindow::persistPreferences);

    spectrum_ = new SpectrumPlot(this);
    spectrum_->setObjectName("spectrumPlot");
    waterfall_ = new WaterfallPlot(this);
    waterfall_->setObjectName("waterfallPlot");
    auto *central = new QSplitter(Qt::Vertical, this);
    central->addWidget(spectrum_);
    central->addWidget(waterfall_);
    central->setStretchFactor(0, 2);
    central->setStretchFactor(1, 3);
    setCentralWidget(central);
    averagePlot_ = new SpectrumPlot(this);
    waveform_ = new WaveformPlot(this);
    waveform_->setObjectName("waveformPlot");
    auto *averageContainer = new QWidget(this);
    auto *averageLayout = new QVBoxLayout(averageContainer);
    averageLabel_ = new QLabel("Run an interval or whole-recording average", this);
    averageLabel_->setWordWrap(true);
    maxHold_ = new QCheckBox("Show max hold instead of average", this);
    averageLayout->addWidget(averageLabel_);
    averageLayout->addWidget(maxHold_);
    averageLayout->addWidget(averagePlot_);
    averageDock_ =
        dock(this, "Average spectrum", "averageDock", averageContainer, Qt::RightDockWidgetArea);
    averageDock_->hide();
    waveformDock_ =
        dock(this, "Baseband waveform", "waveformDock", waveform_, Qt::BottomDockWidgetArea);
    bookmarksList_ = new QListWidget(this);
    bookmarksDock_ = dock(this, "Bookmarks / SigMF annotations", "bookmarksDock", bookmarksList_,
                          Qt::RightDockWidgetArea);
    bookmarksDock_->hide();
    buildControls();
    buildMenus();
    connectWorker();
    cursorLabel_ = new QLabel("Cursor: move over a plot", this);
    cursorLabel_->setMinimumWidth(400);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 1000);
    progress_->setFixedWidth(140);
    progress_->hide();
    cancel_ = new QPushButton("Cancel", this);
    cancel_->setObjectName("cancelAnalysis");
    cancel_->setEnabled(false);
    statusBar()->addWidget(cursorLabel_, 1);
    statusBar()->addPermanentWidget(progress_);
    statusBar()->addPermanentWidget(cancel_);
    connect(cancel_, &QPushButton::clicked, this, [this] {
        controller_.cancel();
        ++generation_;
        setBusy(false, "Cancelled; incomplete results discarded");
    });
    for (auto *plot : {spectrum_, averagePlot_}) {
        connect(plot, &SpectrumPlot::cursorChanged, cursorLabel_, &QLabel::setText);
        connect(plot, &SpectrumPlot::frequencyRangeChanged, waterfall_,
                &WaterfallPlot::setFrequencyRange);
        connect(plot, &SpectrumPlot::measurementActiveChanged, this, [this, plot](bool active) {
            if (active)
                beginMeasurement(plot);
        });
    }
    connect(waterfall_, &WaterfallPlot::measurementActiveChanged, this, [this](bool active) {
        if (active)
            beginMeasurement(waterfall_);
    });
    connect(waterfall_, &WaterfallPlot::measurementChanged, this,
            &MainWindow::updateWaterfallSelection);
    connect(spectrum_, &SpectrumPlot::frequencyRangeChanged, averagePlot_,
            &SpectrumPlot::setFrequencyRange);
    connect(averagePlot_, &SpectrumPlot::frequencyRangeChanged, spectrum_,
            &SpectrumPlot::setFrequencyRange);
    connect(waterfall_, &WaterfallPlot::cursorChanged, cursorLabel_, &QLabel::setText);
    connect(waveform_, &WaveformPlot::cursorChanged, cursorLabel_, &QLabel::setText);
    const auto showTime = [this](quint64 frame) {
        if (!recording_)
            return;
        const auto utc = recording_->descriptor().timestampAt(frame);
        if (!utc.isEmpty())
            cursorLabel_->setText(cursorLabel_->text() + " · UTC (ms): " + utc);
    };
    connect(waterfall_, &WaterfallPlot::frameHovered, this, showTime);
    connect(waterfall_, &WaterfallPlot::frameHovered, this, &MainWindow::hoverFrame);
    connect(waveform_, &WaveformPlot::frameHovered, this, showTime);
    connect(waveform_, &WaveformPlot::frameHovered, this, &MainWindow::hoverFrame);
    connect(waterfall_, &WaterfallPlot::frameClicked, this, &MainWindow::toggleFrameFreeze);
    connect(waveform_, &WaveformPlot::frameClicked, this, &MainWindow::toggleFrameFreeze);
    connect(waterfall_, &WaterfallPlot::cursorLeft, this, &MainWindow::clearFrameCursor);
    connect(waveform_, &WaveformPlot::cursorLeft, this, &MainWindow::clearFrameCursor);
    connect(waterfall_, &WaterfallPlot::frameSelected, this, &MainWindow::seekFrame);
    connect(waveform_, &WaveformPlot::frameSelected, this, &MainWindow::seekFrame);
    connect(maxHold_, &QCheckBox::toggled, this, [this](bool checked) {
        if (average_)
            averagePlot_->setAverage(average_, checked);
    });
    connect(bookmarksList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const auto index = static_cast<std::size_t>(bookmarksList_->row(item));
        if (index < bookmarks_.size()) {
            const auto &mark = bookmarks_[index];
            if (recording_ && mark.count && mark.start < recording_->frameCount())
                setRange({mark.start, mark.start + std::min(mark.count, recording_->frameCount() -
                                                                            mark.start)});
            else
                seekFrame(mark.start);
        }
    });
    applyPreferences();
    if (!preferences_.geometry.isEmpty())
        restoreGeometry(preferences_.geometry);
    if (!preferences_.workspace.isEmpty())
        restoreState(preferences_.workspace, 1);
    if (!settingsError.isEmpty())
        QTimer::singleShot(0, this, [this, settingsError] { reportError(settingsError); });
}

void MainWindow::buildMenus()
{
    auto *file = menuBar()->addMenu("&File");
    auto *open = file->addAction("&Open recording…");
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, "Open RF recording", {},
            "RF recordings (*.iq *.bin *.raw *.dat *.sigmf-meta *.rfmeta.json);;All files (*)");
        if (!path.isEmpty())
            openPath(path);
    });
    auto *reinterpret = file->addAction("Review / change interpretation…");
    connect(reinterpret, &QAction::triggered, this, [this] {
        if (!recording_)
            return;
        ImportDialog dialog(recording_->descriptor().path, recording_->descriptor(), this);
        if (dialog.exec() == QDialog::Accepted)
            openRecording(dialog.descriptor());
    });
    file->addSeparator();
    auto *sessionOpen = file->addAction("Open session…");
    connect(sessionOpen, &QAction::triggered, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, "Open session", {},
                                                       "RF sessions (*.rfsession.json)");
        if (!path.isEmpty())
            openSessionPath(path);
    });
    auto *reopen = file->addAction("Reopen last saved session");
    connect(reopen, &QAction::triggered, this, [this] {
        if (!preferences_.lastSession.isEmpty())
            openSessionPath(preferences_.lastSession);
    });
    auto *save = file->addAction("Save session");
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, [this] { saveCurrentSession(false); });
    auto *saveAs = file->addAction("Save session as…");
    connect(saveAs, &QAction::triggered, this, [this] { saveCurrentSession(true); });
    file->addSeparator();
    auto *csv = file->addAction("Export current spectrum CSV…");
    connect(csv, &QAction::triggered, this, [this] { exportCsv(false); });
    auto *averageCsv = file->addAction("Export average / max-hold CSV…");
    connect(averageCsv, &QAction::triggered, this, [this] { exportCsv(true); });
    auto *png = file->addAction("Export plot PNG…");
    connect(png, &QAction::triggered, this, &MainWindow::exportPng);
    auto *samples = file->addAction("Export selected original samples…");
    connect(samples, &QAction::triggered, this, &MainWindow::exportSampleRange);
    file->addSeparator();
    auto *quit = file->addAction("Quit");
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);
    auto *analysis = menuBar()->addMenu("&Analysis");
    connect(analysis->addAction("Average time selection"), &QAction::triggered, this,
            [this] { startAverage(false); });
    connect(analysis->addAction("Average entire recording"), &QAction::triggered, this,
            [this] { startAverage(true); });
    connect(analysis->addAction("Exact waterfall overview + average"), &QAction::triggered, this,
            [this] {
                if (!recording_)
                    return;
                previewTimer_->stop();
                clearMeasurements();
                generation_ = controller_.overview(recording_, range_, preferences_.dsp);
                setBusy(true, "Scanning every window for exact overview…");
            });
    connect(analysis->addAction("Exact waveform over selected interval"), &QAction::triggered, this,
            &MainWindow::startWaveform);
    auto *mark = analysis->addAction("Add bookmark / note…");
    mark->setShortcut(QKeySequence("Ctrl+B"));
    connect(mark, &QAction::triggered, this, &MainWindow::addBookmark);
    auto *view = menuBar()->addMenu("&View");
    for (auto *panel : {controlsDock_, averageDock_, waveformDock_, bookmarksDock_})
        view->addAction(panel->toggleViewAction());
    auto *reset = view->addAction("Reset frequency zoom");
    reset->setShortcut(QKeySequence("Ctrl+0"));
    connect(reset, &QAction::triggered, spectrum_, &SpectrumPlot::resetFrequency);
    auto *all = view->addAction("Entire recording");
    all->setShortcut(QKeySequence("Home"));
    connect(all, &QAction::triggered, this, [this] {
        if (recording_)
            setRange({0, recording_->frameCount()});
    });
    auto *inward = view->addAction("Zoom time in");
    inward->setShortcut(QKeySequence("Ctrl++"));
    connect(inward, &QAction::triggered, this, [this] { zoomTime(true); });
    auto *outward = view->addAction("Zoom time out");
    outward->setShortcut(QKeySequence("Ctrl+-"));
    connect(outward, &QAction::triggered, this, [this] { zoomTime(false); });
    auto *previous = view->addAction("Previous interval");
    previous->setShortcut(QKeySequence("Alt+Left"));
    connect(previous, &QAction::triggered, this, [this] { panTime(false); });
    auto *next = view->addAction("Next interval");
    next->setShortcut(QKeySequence("Alt+Right"));
    connect(next, &QAction::triggered, this, [this] { panTime(true); });
    auto *help = menuBar()->addMenu("&Help");
    connect(help->addAction("Measurement conventions"), &QAction::triggered, this, [this] {
        QMessageBox::information(
            this, "CroccoSpectrum conventions",
            "One complex frame = I + jQ. IQ order can be changed on import; conjugation reverses "
            "spectral sign.\n\n"
            "Power reference = 1 normalized mean-square unit. A unit complex tone is 0 dBFS; a "
            "peak-1 real sine is -3.0103 dBFS. PSD is dBFS/Hz. Values are not calibrated dBm.\n\n"
            "Averages use linear power and complete windows. Invalid and capture-boundary windows "
            "are excluded and counted.\n\n"
            "A sampled waterfall can miss short events. Full averages and Exact waveform process "
            "every required sample/window.\n\n"
            "Wheel: frequency zoom. Drag spectrum: frequency pan. Hover waterfall/waveform: "
            "inspect the shared frame. Click either plot to freeze/unfreeze it. Double-click: "
            "seek. CSV records analysis settings; sessions preserve interpretation and bookmarks.");
    });
    auto *about = help->addAction("About CroccoSpectrum");
    about->setObjectName("aboutCroccoSpectrum");
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::showAbout()
{
    QDialog dialog(this);
    dialog.setObjectName("aboutDialog");
    dialog.setWindowTitle("About CroccoSpectrum");
    auto *layout = new QVBoxLayout(&dialog);
    auto *logo = new QLabel(&dialog);
    logo->setObjectName("aboutLogo");
    logo->setAlignment(Qt::AlignCenter);
    // Scale in physical pixels, then declare the device ratio, to keep the
    // original square artwork sharp at normal and high-DPI display settings.
    // Leave room for text, buttons and window decorations on smaller screens.
    const auto available = dialog.screen()->availableGeometry();
    const auto extent =
        std::clamp(std::min(available.width() - 60, available.height() - 180), 160, 400);
    const auto ratio = dialog.devicePixelRatioF();
    auto pixmap =
        QPixmap(":/branding/CroccoSpectrumLogo.png")
            .scaled(QSize(extent, extent) * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    pixmap.setDevicePixelRatio(ratio);
    logo->setPixmap(pixmap);
    layout->addWidget(logo);
    auto *description = new QLabel("CroccoSpectrum " RF_VERSION
                                   "\nC++20 · Qt 6 Widgets · FFTW\nOffline RF recording analysis."
                                   "\nGPL-3.0-or-later.",
                                   &dialog);
    description->setAlignment(Qt::AlignCenter);
    description->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(description);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::buildControls()
{
    auto *content = new QWidget(this);
    auto *layout = new QVBoxLayout(content);
    recordingLabel_ = new QLabel("Open an IQ recording to begin", this);
    recordingLabel_->setWordWrap(true);
    recordingLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(recordingLabel_);
    auto *dspGroup = new QGroupBox("Spectral analysis", this);
    auto *dspForm = new QFormLayout(dspGroup);
    fft_ = new QComboBox(this);
    fft_->setObjectName("fftSize");
    for (int exponent = 8; exponent <= 20; ++exponent)
        fft_->addItem(QString::number(1 << exponent), 1 << exponent);
    window_ = new QComboBox(this);
    window_->addItems({"Hann", "Rectangular", "Hamming", "Blackman-Harris"});
    overlap_ = new QSpinBox(this);
    overlap_->setRange(0, 95);
    overlap_->setSuffix(" %");
    scale_ = new QComboBox(this);
    scale_->addItems({"Spectrum · dBFS", "PSD · dBFS/Hz"});
    removeDc_ = new QCheckBox("Remove window mean (DC)", this);
    conjugate_ = new QCheckBox("Conjugate / invert spectrum", this);
    dspForm->addRow("FFT length", fft_);
    dspForm->addRow("Window", window_);
    dspForm->addRow("Overlap", overlap_);
    dspForm->addRow("Power scale", scale_);
    dspForm->addRow(removeDc_);
    dspForm->addRow(conjugate_);
    resolutionLabel_ = new QLabel(this);
    resolutionLabel_->setWordWrap(true);
    dspForm->addRow(resolutionLabel_);
    layout->addWidget(dspGroup);
    auto *colors = new QGroupBox("Display", this);
    auto *colorForm = new QFormLayout(colors);
    colorMin_ = new QDoubleSpinBox(this);
    colorMax_ = new QDoubleSpinBox(this);
    for (auto *spin : {colorMin_, colorMax_}) {
        spin->setRange(-3000, 3000);
        spin->setDecimals(1);
        spin->setSingleStep(5);
    }
    colorMin_->setObjectName("colorMinimum");
    colorMax_->setObjectName("colorMaximum");
    palette_ = new QComboBox(this);
    palette_->setObjectName("waterfallPalette");
    palette_->addItems({"Viridis", "Inferno", "Grayscale", "Turbo", "Baudline"});
    autoRange_ = new QCheckBox("Automatic range (80 dB span)", this);
    absoluteFrequency_ = new QCheckBox("Absolute frequency, when known", this);
    waveformMode_ = new QComboBox(this);
    waveformMode_->addItems({"I/Q", "Magnitude", "I only"});
    colorForm->addRow("Minimum", colorMin_);
    colorForm->addRow("Maximum", colorMax_);
    colorForm->addRow("Palette", palette_);
    colorForm->addRow(autoRange_);
    colorForm->addRow(absoluteFrequency_);
    colorForm->addRow("Waveform", waveformMode_);
    layout->addWidget(colors);
    auto *selection = new QGroupBox("Time selection · [start, end)", this);
    auto *selectionForm = new QFormLayout(selection);
    start_ = new QLineEdit("0", this);
    start_->setObjectName("selectionStart");
    end_ = new QLineEdit("0", this);
    end_->setObjectName("selectionEnd");
    auto *apply = new QPushButton("Apply sample range", this);
    apply->setObjectName("applySelection");
    seekTime_ = new QLineEdit("0", this);
    auto *seek = new QPushButton("Seek to seconds", this);
    timeline_ = new QSlider(Qt::Horizontal, this);
    timeline_->setRange(0, 100000);
    timeline_->setTracking(false);
    selectionForm->addRow("Start frame", start_);
    selectionForm->addRow("End frame", end_);
    selectionForm->addRow(apply);
    selectionForm->addRow("Time (s)", seekTime_);
    selectionForm->addRow(seek);
    selectionForm->addRow(timeline_);
    auto *zoomButtons = new QWidget(this);
    auto *zoomLayout = new QHBoxLayout(zoomButtons);
    zoomLayout->setContentsMargins(0, 0, 0, 0);
    for (const auto &label : {"−", "+", "All"}) {
        auto *button = new QPushButton(QString::fromUtf8(label), this);
        zoomLayout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, label = QString::fromUtf8(label)] {
            if (label == "All") {
                if (recording_)
                    setRange({0, recording_->frameCount()});
            } else
                zoomTime(label == "+");
        });
    }
    selectionForm->addRow(zoomButtons);
    layout->addWidget(selection);
    auto *average = new QPushButton("Average time selection", this);
    average->setObjectName("averageSelection");
    averageWaterfallSelection_ = new QPushButton("Average waterfall selection", this);
    averageWaterfallSelection_->setObjectName("averageWaterfallSelection");
    averageWaterfallSelection_->setEnabled(false);
    averageWaterfallSelection_->setToolTip(
        "Average between the waterfall markers without applying the time range. "
        "Complete a selection spanning at least one FFT window to enable this button.");
    auto *exactWave = new QPushButton("Exact waveform for time selection", this);
    layout->addWidget(average);
    layout->addWidget(averageWaterfallSelection_);
    layout->addWidget(exactWave);
    coverageLabel_ = new QLabel("No analysis yet", this);
    coverageLabel_->setWordWrap(true);
    layout->addWidget(coverageLabel_);
    layout->addStretch();
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    scroll->setMinimumWidth(290);
    controlsDock_ =
        dock(this, "Recording and analysis", "controlsDock", scroll, Qt::LeftDockWidgetArea);
    connect(apply, &QPushButton::clicked, this, [this] {
        try {
            setRange({jsonUnsigned(start_->text().trimmed(), "start frame"),
                      jsonUnsigned(end_->text().trimmed(), "end frame")});
        } catch (const std::exception &error) {
            reportError(QString::fromUtf8(error.what()));
        }
    });
    connect(seek, &QPushButton::clicked, this, [this] {
        if (!recording_)
            return;
        bool ok = false;
        const double seconds = seekTime_->text().toDouble(&ok);
        const long double frame =
            static_cast<long double>(seconds) * recording_->descriptor().sampleRate;
        if (!ok || !std::isfinite(seconds) || seconds < 0 ||
            frame >= static_cast<long double>(recording_->frameCount())) {
            reportError("Seek time is outside the recording");
            return;
        }
        seekFrame(static_cast<std::uint64_t>(frame));
    });
    connect(timeline_, &QSlider::valueChanged, this, [this](int value) {
        if (!recording_ || applying_)
            return;
        const auto count = recording_->frameCount() - 1;
        const auto position = count / 100000U * static_cast<unsigned int>(value) +
                              count % 100000U * static_cast<unsigned int>(value) / 100000U;
        seekFrame(position);
    });
    connect(average, &QPushButton::clicked, this, [this] { startAverage(false); });
    connect(averageWaterfallSelection_, &QPushButton::clicked, this, [this] {
        if (const auto range = waterfall_->measurementRange(); range && !waterfall_->isMeasuring())
            startAverage(*range, "Averaging waterfall selection…");
    });
    connect(exactWave, &QPushButton::clicked, this, &MainWindow::startWaveform);
    for (auto *combo : {fft_, window_, scale_})
        connect(combo, &QComboBox::currentIndexChanged, this, &MainWindow::updateDsp);
    connect(overlap_, &QSpinBox::valueChanged, this, &MainWindow::updateDsp);
    for (auto *check : {removeDc_, conjugate_})
        connect(check, &QCheckBox::toggled, this, &MainWindow::updateDsp);
    for (auto *combo : {palette_, waveformMode_})
        connect(combo, &QComboBox::currentIndexChanged, this, &MainWindow::updateView);
    for (auto *check : {autoRange_, absoluteFrequency_})
        connect(check, &QCheckBox::toggled, this, &MainWindow::updateView);
    for (auto *spin : {colorMin_, colorMax_})
        connect(spin, &QDoubleSpinBox::valueChanged, this, &MainWindow::updateView);
}

void MainWindow::connectWorker()
{
    connect(&controller_, &AnalysisController::recordingOpened, this,
            [this](quint64 generation, std::shared_ptr<Recording> recording, FrameRange range) {
                if (generation != generation_)
                    return;
                recording_ = std::move(recording);
                range_ = range;
                updateRanges();
                const auto &descriptor = recording_->descriptor();
                recordingLabel_->setText(
                    QString("%1\n%2 frames · %3 s\n%4 MS/s · %5-bit %6\n%7")
                        .arg(QFileInfo(descriptor.path).fileName())
                        .arg(recording_->frameCount())
                        .arg(number(static_cast<double>(recording_->frameCount()) /
                                    descriptor.sampleRate))
                        .arg(number(descriptor.sampleRate / 1e6))
                        .arg(descriptor.format.bits)
                        .arg(descriptor.format.kind == SampleKind::Complex ? "I/Q" : "real")
                        .arg(descriptor.metadataSource));
                recordingLabel_->setToolTip(QString::fromUtf8(
                    QJsonDocument(recordingToJson(descriptor)).toJson(QJsonDocument::Indented)));
                setWindowTitle(QFileInfo(descriptor.path).fileName() + " — CroccoSpectrum");
                preferences_.importDefaults = descriptor;
                if (restoringSession_) {
                    if (!restoringSession_->sourceIdentity.isEmpty() &&
                        restoringSession_->sourceIdentity != recording_->identity())
                        reportError("Session source identity changed; results are recomputed from "
                                    "the current file");
                    bookmarks_ = restoringSession_->bookmarks;
                    restoringSession_.reset();
                } else
                    bookmarks_ = descriptor.annotations;
                bookmarksList_->clear();
                for (const auto &mark : bookmarks_)
                    bookmarksList_->addItem(
                        QString("%1 · frame %2").arg(mark.label).arg(mark.start));
                updateDsp();
                saveTimer_->start();
            });
    connect(&controller_, &AnalysisController::previewReady, this,
            [this](quint64 generation, std::shared_ptr<const PreviewResult> result) {
                if (generation != generation_)
                    return;
                spectrumController_.cancel();
                ++spectrumGeneration_;
                preview_ = std::move(result);
                spectrumFrame_ = preview_->spectrumStart;
                waterfall_->setPreview(preview_, recording_->descriptor().sampleRate);
                waveformResult_ = std::make_shared<WaveformResult>(preview_->waveform);
                waveform_->setWaveform(waveformResult_, recording_->descriptor().sampleRate,
                                       recording_->descriptor().format.kind);
                displaySpectrum(preview_, true);
                updateView();
                resolutionLabel_->setText(resolutionLabel_->text().replace(
                    "ENBW shown in CSV/numeric results",
                    QString("ENBW: %1 Hz").arg(number(preview_->spectrum.enbwHz))));
                setBusy(false, "Preview ready");
                emit analysisDisplayed();
            });
    connect(&spectrumController_, &AnalysisController::previewReady, this,
            [this](quint64 generation, std::shared_ptr<const PreviewResult> result) {
                if (generation == spectrumGeneration_)
                    displaySpectrum(std::move(result), false);
            });
    connect(&spectrumController_, &AnalysisController::failed, this,
            [this](quint64 generation, const QString &message) {
                if (generation == spectrumGeneration_)
                    reportError(message);
            });
    connect(&controller_, &AnalysisController::averageReady, this,
            [this](quint64 generation, std::shared_ptr<const AverageResult> result) {
                if (generation != generation_)
                    return;
                average_ = std::move(result);
                averagePlot_->setAverage(average_, maxHold_->isChecked());
                averageDock_->show();
                averageLabel_->setText(
                    QString("Complete pass over frames [%1, %2)\n%3 valid · %4 invalid · %5 "
                            "boundary windows · %6 trailing frames excluded")
                        .arg(average_->range.begin)
                        .arg(average_->range.end)
                        .arg(average_->validWindows)
                        .arg(average_->invalidWindows)
                        .arg(average_->boundaryWindows)
                        .arg(average_->trailingFrames));
                updateView();
                setBusy(false, "Interval analysis complete");
            });
    connect(&controller_, &AnalysisController::waveformReady, this,
            [this](quint64 generation, std::shared_ptr<const WaveformResult> result) {
                if (generation != generation_)
                    return;
                waveformResult_ = std::move(result);
                waveform_->setWaveform(waveformResult_, recording_->descriptor().sampleRate,
                                       recording_->descriptor().format.kind);
                updateFrameCursors();
                waveformDock_->show();
                setBusy(false, "Exact waveform complete");
            });
    connect(&controller_, &AnalysisController::progressChanged, this,
            [this](quint64 generation, double fraction) {
                if (generation == generation_) {
                    progress_->setRange(0, 1000);
                    progress_->setValue(qRound(fraction * 1000));
                }
            });
    connect(&controller_, &AnalysisController::finished, this,
            [this](quint64 generation, const QString &message) {
                if (generation == generation_)
                    setBusy(false, message);
            });
    connect(&controller_, &AnalysisController::failed, this,
            [this](quint64 generation, const QString &message) {
                if (generation == generation_) {
                    setBusy(false);
                    reportError(message);
                }
            });
}

void MainWindow::applyPreferences()
{
    applying_ = true;
    fft_->setCurrentIndex(fft_->findData(preferences_.dsp.fftSize));
    window_->setCurrentIndex(static_cast<int>(preferences_.dsp.window));
    overlap_->setValue(preferences_.dsp.overlapPercent);
    scale_->setCurrentIndex(static_cast<int>(preferences_.dsp.scale));
    removeDc_->setChecked(preferences_.dsp.removeDc);
    conjugate_->setChecked(preferences_.dsp.conjugate);
    colorMin_->setValue(preferences_.view.colorMin);
    colorMax_->setValue(preferences_.view.colorMax);
    palette_->setCurrentText(preferences_.view.palette);
    autoRange_->setChecked(preferences_.view.autoRange);
    absoluteFrequency_->setChecked(preferences_.view.absoluteFrequency);
    waveformMode_->setCurrentIndex(preferences_.view.waveformMode);
    applying_ = false;
    updateView();
    updateDsp();
}

void MainWindow::openPath(const QString &path)
{
    auto defaults = preferences_.importDefaults;
    defaults.dataOffset = 0;
    defaults.dataBytes.reset();
    defaults.captures.clear();
    defaults.annotations.clear();
    defaults.centerFrequency.reset();
    defaults.startUtc.clear();
    defaults.metadataSource = "User import";
    ImportDialog dialog(path, defaults, this);
    if (dialog.exec() == QDialog::Accepted)
        openRecording(dialog.descriptor());
}

void MainWindow::openRecording(RecordingDescriptor descriptor, FrameRange range)
{
    try {
        descriptor.validate();
        preferences_.dsp.validate(descriptor.sampleRate);
        previewTimer_->stop();
        clearMeasurements();
        recording_.reset();
        generation_ = controller_.open(std::move(descriptor), preferences_.dsp, range);
        setBusy(true, "Opening recording…");
    } catch (const std::exception &error) {
        reportError(QString::fromUtf8(error.what()));
    }
}

void MainWindow::clearMeasurements()
{
    spectrumController_.cancel();
    ++spectrumGeneration_;
    spectrumFrame_.reset();
    frameFrozen_ = false;
    frameCursorVisible_ = false;
    spectrumPreview_.reset();
    preview_.reset();
    average_.reset();
    waveformResult_.reset();
    spectrum_->clear();
    waterfall_->clear();
    averagePlot_->clear();
    waveform_->clear();
    averageLabel_->setText("No current interval result");
    coverageLabel_->setText("Analysis pending");
}

void MainWindow::schedulePreview()
{
    if (!recording_)
        return;
    // Invalidate queued results immediately, not only after the debounce timer;
    // otherwise a previous job can repaint during a newly edited selection.
    controller_.cancel();
    ++generation_;
    clearMeasurements();
    previewTimer_->start();
    setBusy(true, "Updating selected range…");
}
void MainWindow::requestPreview()
{
    if (!recording_)
        return;
    generation_ = controller_.preview(recording_, range_, preferences_.dsp);
    setBusy(true, "Computing preview…");
}

bool MainWindow::measurementActive() const
{
    return spectrum_->isMeasuring() || averagePlot_->isMeasuring() || waterfall_->isMeasuring();
}

void MainWindow::updateWaterfallSelection()
{
    const auto range = waterfall_->measurementRange();
    if (range) {
        start_->setText(QString::number(range->begin));
        end_->setText(QString::number(range->end));
    }
    averageWaterfallSelection_->setEnabled(
        recording_ && range && !waterfall_->isMeasuring() &&
        range->size() >= static_cast<std::uint64_t>(preferences_.dsp.fftSize));
}

void MainWindow::beginMeasurement(QWidget *plot)
{
    if (plot != spectrum_ && spectrum_->isMeasuring())
        spectrum_->clearMeasurement();
    if (plot != averagePlot_ && averagePlot_->isMeasuring())
        averagePlot_->clearMeasurement();
    if (plot != waterfall_ && waterfall_->isMeasuring())
        waterfall_->clearMeasurement();
    // A queued hover result must not replace the trace while measuring or editing.
    spectrumController_.cancel();
    ++spectrumGeneration_;
    if (spectrumPreview_)
        spectrumFrame_ = spectrumPreview_->spectrumStart;
    updateFrameCursors();
}

void MainWindow::hoverFrame(quint64 frame)
{
    if (measurementActive())
        updateFrameCursors();
    else if (!frameFrozen_)
        selectFrame(frame);
}

void MainWindow::toggleFrameFreeze(quint64 frame)
{
    if (!recording_ || !preview_ || measurementActive())
        return;
    frameFrozen_ = !frameFrozen_;
    selectFrame(frame);
    statusBar()->showMessage(frameFrozen_ ? "Frame frozen; click either plot to follow the pointer"
                                         : "Following the pointer");
}

void MainWindow::clearFrameCursor()
{
    if (measurementActive())
        updateFrameCursors();
    else if (!frameFrozen_) {
        frameCursorVisible_ = false;
        updateFrameCursors();
    }
}

void MainWindow::updateFrameCursors()
{
    const auto frame = frameCursorVisible_ ? spectrumFrame_ : std::nullopt;
    waterfall_->setFrameCursor(frame, frameFrozen_);
    waveform_->setFrameCursor(frame, frameFrozen_);
}

void MainWindow::selectFrame(quint64 frame)
{
    if (!recording_ || !preview_)
        return;
    // Waveform sample buckets snap to the waterfall row containing their time.
    const auto row = std::upper_bound(preview_->rowStarts.begin(), preview_->rowStarts.end(), frame);
    frame = row == preview_->rowStarts.begin() ? preview_->rowStarts.front() : *(row - 1);
    frameCursorVisible_ = true;
    if (spectrumFrame_ == frame) {
        updateFrameCursors();
        return;
    }
    spectrumFrame_ = frame;
    updateFrameCursors();
    spectrumController_.cancel();
    ++spectrumGeneration_;
    if (frame == preview_->spectrumStart) {
        displaySpectrum(preview_, false);
        return;
    }
    // One-window previews reuse the bounded cache and latest-request worker.
    // A separate controller keeps mouse movement from cancelling full passes or exports.
    spectrumGeneration_ = spectrumController_.preview(
        recording_, {frame, frame + static_cast<std::uint64_t>(preferences_.dsp.fftSize)},
        preferences_.dsp);
}

void MainWindow::displaySpectrum(std::shared_ptr<const PreviewResult> result, bool resetZoom)
{
    spectrumPreview_ = std::move(result);
    spectrum_->setPreview(spectrumPreview_, resetZoom);
    const auto frame = spectrumPreview_->spectrumStart;
    if (waveformResult_ &&
        (frame < waveformResult_->range.begin || frame >= waveformResult_->range.end)) {
        // Keep long recordings bounded: the hover job already supplies local
        // waveform data. Exact waveforms retain their full selected interval.
        auto waveform = std::make_shared<WaveformResult>(spectrumPreview_->waveform);
        waveform->complete = waveform->range == range_;
        waveformResult_ = std::move(waveform);
        waveform_->setWaveform(waveformResult_, recording_->descriptor().sampleRate,
                               recording_->descriptor().format.kind);
    }
    updateFrameCursors();
    coverageLabel_->setText(
        QString("%1 waterfall · %2 rows\n%3 invalid windows · %4 capture-boundary "
                "windows\nSpectrum begins at frame %5")
            .arg(preview_->aggregated ? "Exact maximum-aggregated"
                 : preview_->sampled  ? "Sampled preview"
                                      : "Every window shown")
            .arg(preview_->rowStarts.size())
            .arg(preview_->invalidWindows)
            .arg(preview_->boundaryWindows)
            .arg(spectrumPreview_->spectrumStart));
    updateSpectrumView();
}

void MainWindow::updateDsp()
{
    if (applying_)
        return;
    auto settings = preferences_.dsp;
    settings.fftSize = fft_->currentData().toInt();
    settings.window = static_cast<Window>(window_->currentIndex());
    settings.overlapPercent = overlap_->value();
    settings.scale = static_cast<PowerScale>(scale_->currentIndex());
    settings.removeDc = removeDc_->isChecked();
    settings.conjugate = conjugate_->isChecked();
    const double rate =
        recording_ ? recording_->descriptor().sampleRate : preferences_.importDefaults.sampleRate;
    try {
        settings.validate(rate);
    } catch (const std::exception &error) {
        reportError(QString::fromUtf8(error.what()));
        return;
    }
    const bool changed = settings != preferences_.dsp;
    preferences_.dsp = settings;
    resolutionLabel_->setText(
        QString("Bin spacing: %1 Hz\nWindow: %2 s · hop: %3 s\nENBW shown in CSV/numeric results")
            .arg(number(rate / settings.fftSize))
            .arg(number(settings.fftSize / rate))
            .arg(number(static_cast<double>(settings.hop()) / rate)));
    if (changed) {
        schedulePreview();
        updateView();
        saveTimer_->start();
    }
}

double MainWindow::displayCenter() const
{
    const auto frame = spectrumPreview_ ? spectrumPreview_->spectrumStart : range_.begin;
    return recording_ && recording_->descriptor().hasFrequencyAt(frame)
               ? recording_->descriptor().frequencyAt(frame)
               : 0;
}

void MainWindow::updateSpectrumView()
{
    auto effective = preferences_.view;
    const auto frame = spectrumPreview_ ? spectrumPreview_->spectrumStart : range_.begin;
    effective.absoluteFrequency = effective.absoluteFrequency && recording_ &&
                                  recording_->descriptor().hasFrequencyAt(frame);
    spectrum_->setView(effective, preferences_.dsp.scale, displayCenter());
}

void MainWindow::updateView()
{
    if (applying_)
        return;
    auto view = preferences_.view;
    view.colorMin = colorMin_->value();
    view.colorMax = colorMax_->value();
    view.palette = palette_->currentText();
    view.autoRange = autoRange_->isChecked();
    view.absoluteFrequency = absoluteFrequency_->isChecked();
    view.waveformMode = waveformMode_->currentIndex();
    try {
        view.validate();
    } catch (const std::exception &error) {
        statusBar()->showMessage(QString::fromUtf8(error.what()));
        return;
    }
    preferences_.view = view;
    // Unknown absolute frequency stays explicitly disabled instead of quietly
    // labeling a relative baseband axis as an absolute RF frequency.
    auto effective = view;
    const auto uniformCenter = [this](FrameRange range) -> std::optional<double> {
        if (!recording_ || !recording_->descriptor().hasFrequencyAt(range.begin))
            return {};
        const auto &descriptor = recording_->descriptor();
        const double center = descriptor.frequencyAt(range.begin);
        for (const auto &capture : descriptor.captures)
            if (capture.start > range.begin && capture.start < range.end &&
                (!descriptor.hasFrequencyAt(capture.start) ||
                 descriptor.frequencyAt(capture.start) != center))
                return {};
        return center;
    };
    // A selected waterfall/average may span retunes. Those aggregates are
    // baseband; applying the first capture's frequency to every row would
    // manufacture incorrect absolute RF coordinates.
    const auto center = uniformCenter(range_);
    effective.absoluteFrequency = view.absoluteFrequency && center.has_value();
    waterfall_->setView(effective, preferences_.dsp.scale, center.value_or(0));
    const auto averageCenter = uniformCenter(average_ ? average_->range : range_);
    effective.absoluteFrequency = view.absoluteFrequency && averageCenter.has_value();
    averagePlot_->setView(effective, preferences_.dsp.scale, averageCenter.value_or(0));
    updateSpectrumView();
    waveform_->setMode(view.waveformMode);
    saveTimer_->start();
}

void MainWindow::updateRanges()
{
    start_->setText(QString::number(range_.begin));
    end_->setText(QString::number(range_.end));
    if (recording_) {
        const QSignalBlocker blocker(timeline_);
        timeline_->setValue(static_cast<int>(static_cast<long double>(range_.begin) /
                                             recording_->frameCount() * 100000));
        seekTime_->setText(
            number(static_cast<double>(range_.begin) / recording_->descriptor().sampleRate));
    }
}

void MainWindow::setRange(FrameRange range)
{
    if (!recording_)
        return;
    try {
        recording_->validateRange(range);
        if (range.size() < static_cast<std::uint64_t>(preferences_.dsp.fftSize))
            throw std::runtime_error("Selection must contain at least one complete FFT window");
        range_ = range;
        updateRanges();
        schedulePreview();
    } catch (const std::exception &error) {
        reportError(QString::fromUtf8(error.what()));
    }
}

void MainWindow::seekFrame(std::uint64_t frame)
{
    if (!recording_)
        return;
    // Seeking from an entire-recording view selects a useful local interval;
    // otherwise preserve the current selection width while panning.
    const auto count = recording_->frameCount();
    const auto length = std::min(
        count, range_.size() == count ? static_cast<std::uint64_t>(preferences_.dsp.fftSize) * 128
                                      : range_.size());
    const auto begin = std::min(frame, count - length);
    setRange({begin, begin + length});
}
void MainWindow::zoomTime(bool inward)
{
    if (!recording_)
        return;
    const auto count = recording_->frameCount();
    const auto current = range_.size();
    const auto length = inward ? std::max<std::uint64_t>(preferences_.dsp.fftSize, current / 2)
                        : current > count / 2 ? count
                                              : current * 2;
    const auto center = range_.begin + current / 2;
    const auto begin =
        std::min(center > length / 2 ? center - length / 2 : 0, count - std::min(count, length));
    setRange({begin, begin + std::min(count, length)});
}
void MainWindow::panTime(bool forward)
{
    if (!recording_)
        return;
    const auto step = std::max<std::uint64_t>(1, range_.size() / 2);
    const auto begin = forward
                           ? range_.begin + std::min(step, recording_->frameCount() - range_.end)
                           : range_.begin - std::min(step, range_.begin);
    setRange({begin, begin + range_.size()});
}

void MainWindow::startAverage(bool entireRecording)
{
    if (!recording_)
        return;
    startAverage(entireRecording ? FrameRange{0, recording_->frameCount()} : range_,
                 entireRecording ? "Averaging entire recording…" : "Averaging selected interval…");
}
void MainWindow::startAverage(FrameRange range, const QString &message)
{
    if (!recording_)
        return;
    previewTimer_->stop();
    average_.reset();
    averagePlot_->clear();
    generation_ = controller_.average(recording_, range, preferences_.dsp);
    averageDock_->show();
    setBusy(true, message);
}
void MainWindow::startWaveform()
{
    if (!recording_)
        return;
    previewTimer_->stop();
    generation_ = controller_.waveform(recording_, range_);
    setBusy(true, "Reading exact waveform envelope…");
}

void MainWindow::exportCsv(bool average)
{
    if (!recording_ ||
        (average ? !average_ : !spectrumPreview_ || !spectrumPreview_->spectrum.valid)) {
        reportError("No valid completed spectrum to export");
        return;
    }
    const auto path =
        QFileDialog::getSaveFileName(this, "Export spectrum CSV (new file)", {}, "CSV (*.csv)");
    if (path.isEmpty())
        return;
    CsvRequest request;
    request.recording = recording_->descriptor();
    request.identity = recording_->identity();
    request.settings = preferences_.dsp;
    request.output = path;
    if (average) {
        request.range = average_->range;
        request.frequencies = average_->frequencies;
        request.power = maxHold_->isChecked() ? average_->maxPower : average_->averagePower;
        request.method = maxHold_->isChecked() ? "max_hold" : "linear_power_mean";
        request.coverage = {
            {"complete_pass", true},
            {"valid_windows", QString::number(average_->validWindows)},
            {"invalid_windows", QString::number(average_->invalidWindows)},
            {"capture_boundary_windows", QString::number(average_->boundaryWindows)},
            {"trailing_frames", QString::number(average_->trailingFrames)},
            {"enbw_hz", average_->enbwHz}};
    } else {
        request.range = {spectrumPreview_->spectrumStart,
                         spectrumPreview_->spectrumStart +
                             static_cast<std::uint64_t>(preferences_.dsp.fftSize)};
        request.frequencies = spectrumPreview_->frequencies;
        request.power = spectrumPreview_->spectrum.power;
        request.method = "single_complete_window";
        request.coverage = {{"complete_pass", true},
                            {"valid_windows", "1"},
                            {"enbw_hz", spectrumPreview_->spectrum.enbwHz}};
    }
    generation_ = controller_.csv(std::move(request));
    setBusy(true, "Exporting CSV…");
}

void MainWindow::exportPng()
{
    if (!recording_ || !preview_ || busy_) {
        reportError("Wait for a completed plot before exporting");
        return;
    }
    bool ok = false;
    const auto choice = QInputDialog::getItem(
        this, "Export plot", "Plot", {"Spectrum", "Waterfall", "Waveform", "Average spectrum"}, 0,
        false, &ok);
    if (!ok)
        return;
    const auto path =
        QFileDialog::getSaveFileName(this, "Export PNG (new file)", {}, "PNG (*.png)");
    if (path.isEmpty())
        return;
    if (QFileInfo::exists(path)) {
        reportError("Choose a new filename; export does not overwrite files");
        return;
    }
    QWidget *plot = choice == "Waterfall"          ? static_cast<QWidget *>(waterfall_)
                    : choice == "Waveform"         ? static_cast<QWidget *>(waveform_)
                    : choice == "Average spectrum" ? static_cast<QWidget *>(averagePlot_)
                                                   : static_cast<QWidget *>(spectrum_);
    if (choice == "Average spectrum" && !average_) {
        reportError("Calculate an average before exporting it");
        return;
    }
    auto range = choice == "Average spectrum"              ? average_->range
                 : choice == "Waveform" && waveformResult_ ? waveformResult_->range
                                                           : range_;
    if (choice == "Spectrum")
        range = {spectrumPreview_->spectrumStart,
                 spectrumPreview_->spectrumStart +
                     static_cast<std::uint64_t>(preferences_.dsp.fftSize)};
    PngRequest request;
    request.image = plot->grab().toImage();
    request.output = path;
    request.metadata = {{"application_version", RF_VERSION},
                        {"plot", choice},
                        {"recording", recordingToJson(recording_->descriptor())},
                        {"source_identity", recording_->identity()},
                        {"start", QString::number(range.begin)},
                        {"end", QString::number(range.end)},
                        {"dsp", dspToJson(preferences_.dsp)},
                        {"view", viewToJson(preferences_.view)},
                        {"unit", powerUnit(preferences_.dsp.scale)},
                        {"sampled_preview", choice != "Spectrum" && preview_->sampled},
                        {"exact_overview", choice != "Spectrum" && preview_->aggregated},
                        {"waveform_complete", waveformResult_ && waveformResult_->complete},
                        {"coverage_label", coverageLabel_->text()},
                        {"average_coverage", average_ ? averageLabel_->text() : QString()}};
    generation_ = controller_.png(std::move(request));
    setBusy(true, "Encoding PNG…");
}
void MainWindow::exportSampleRange()
{
    if (!recording_)
        return;
    const auto path = QFileDialog::getSaveFileName(this, "Export original sample bytes (new file)",
                                                   {}, "Raw recording (*.iq)");
    if (path.isEmpty())
        return;
    generation_ = controller_.samples(recording_, range_, path);
    setBusy(true, "Exporting source samples…");
}

void MainWindow::openSessionPath(const QString &path)
{
    try {
        auto session = readSession(path);
        preferences_.dsp = session.dsp;
        preferences_.view = session.view;
        applying_ = true;
        previewTimer_->stop();
        applying_ = false;
        // Suppress requests while controls are restored; opening creates a
        // fresh generation and recomputes data instead of trusting old results.
        recording_.reset();
        applyPreferences();
        restoringSession_ = session;
        sessionPath_ = path;
        preferences_.lastSession = path;
        openRecording(session.recording, session.range);
    } catch (const std::exception &error) {
        reportError(QString::fromUtf8(error.what()));
    }
}
void MainWindow::saveCurrentSession(bool choosePath)
{
    if (!recording_)
        return;
    QString path = sessionPath_;
    if (choosePath || path.isEmpty())
        path = QFileDialog::getSaveFileName(this, "Save session",
                                            QFileInfo(recording_->descriptor().path).fileName() +
                                                ".rfsession.json",
                                            "RF session (*.rfsession.json)");
    if (path.isEmpty())
        return;
    if (QFileInfo(path).canonicalFilePath() ==
        QFileInfo(recording_->descriptor().path).canonicalFilePath()) {
        reportError("A session cannot replace the source recording");
        return;
    }
    try {
        saveSession(path, {recording_->descriptor(), preferences_.dsp, preferences_.view, range_,
                           recording_->identity(), bookmarks_});
        sessionPath_ = path;
        preferences_.lastSession = path;
        persistPreferences();
        statusBar()->showMessage("Session saved", 4000);
    } catch (const std::exception &error) {
        reportError(QString::fromUtf8(error.what()));
    }
}

void MainWindow::addBookmark()
{
    if (!recording_)
        return;
    bool ok = false;
    const auto note = QInputDialog::getMultiLineText(this, "Bookmark selected interval",
                                                     "Label / notes", {}, &ok);
    if (!ok || note.trimmed().isEmpty())
        return;
    bookmarks_.push_back({range_.begin, range_.size(), note});
    bookmarksList_->addItem(QString("%1 · frame %2").arg(note).arg(range_.begin));
    bookmarksDock_->show();
}

void MainWindow::persistPreferences()
{
    if (preferencesPath_.isEmpty())
        return;
    try {
        preferences_.geometry = saveGeometry();
        preferences_.workspace = saveState(1);
        if (!QDir().mkpath(QFileInfo(preferencesPath_).absolutePath()))
            throw std::runtime_error("Cannot create configuration directory");
        savePreferences(preferencesPath_, preferences_);
    } catch (const std::exception &error) {
        reportError(QString::fromUtf8(error.what()));
    }
}
void MainWindow::setBusy(bool busy, const QString &text)
{
    busy_ = busy;
    cancel_->setEnabled(busy);
    progress_->setVisible(busy);
    if (busy)
        progress_->setRange(0, 0);
    if (!text.isEmpty())
        statusBar()->showMessage(text);
}
void MainWindow::reportError(const QString &text)
{
    statusBar()->showMessage(text);
    coverageLabel_->setText(text);
    emit analysisError(text);
}
void MainWindow::closeEvent(QCloseEvent *event)
{
    previewTimer_->stop();
    controller_.cancel();
    spectrumController_.cancel();
    ++spectrumGeneration_;
    persistPreferences();
    event->accept();
}
} // namespace rf
