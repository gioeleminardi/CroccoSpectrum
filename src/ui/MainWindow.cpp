#include "MainWindow.h"
#include "ImportDialog.h"
#include "app/UpdateChecker.h"
#include "recording/Metadata.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPalette>
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
class DockWidget : public QDockWidget
{
  public:
    using QDockWidget::QDockWidget;

  protected:
    bool event(QEvent *event) override
    {
        const bool handled = QDockWidget::event(event);
        // Qt can reacquire the dock's mouse grab after a native drag has ended.
        // Maximized docks skip Qt's resize handler that normally releases it.
        if (isFloating() && isMaximized() && mouseGrabber() == this &&
            (event->type() == QEvent::WindowStateChange || event->type() == QEvent::MouseMove ||
             event->type() == QEvent::MouseButtonRelease))
            releaseMouse();
        return handled;
    }
};

QDockWidget *dock(QMainWindow *window, const QString &name, const QString &id, QWidget *content,
                  Qt::DockWidgetArea side)
{
    auto *panel = new DockWidget(name, window);
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
    applyTheme();
    previewTimer_ = new QTimer(this);
    previewTimer_->setSingleShot(true);
    previewTimer_->setInterval(100);
    waterfallTimer_ = new QTimer(this);
    waterfallTimer_->setSingleShot(true);
    waterfallTimer_->setInterval(100);
    connect(waterfallTimer_, &QTimer::timeout, this, &MainWindow::requestWaterfall);
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
    minimap_ = new WaterfallMinimap(this);
    minimap_->setObjectName("waterfallMinimap");
    for (auto *plot : {static_cast<QWidget *>(spectrum_), static_cast<QWidget *>(waterfall_)}) {
        auto *container = new QWidget(this);
        auto *layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(plot, 1);
        if (plot == waterfall_)
            layout->addWidget(minimap_);
        else {
            auto *gutter = new QWidget(container);
            gutter->setFixedWidth(minimap_->width());
            gutter->setStyleSheet("background: #101923");
            layout->addWidget(gutter);
        }
        central->addWidget(container);
    }
    connect(minimap_, &WaterfallMinimap::viewportChanged, waterfall_, &WaterfallPlot::setViewport);
    connect(waterfall_, &WaterfallPlot::viewportChanged, this, [this](FrameRange range) {
        minimap_->setViewport(range);
        scheduleWaterfall();
        updateView();
    });
    connect(waterfall_, &WaterfallPlot::detailRequested, this, &MainWindow::scheduleWaterfall);
    central->setStretchFactor(0, 2);
    central->setStretchFactor(1, 3);
    setCentralWidget(central);
    averagePlot_ = new SpectrumPlot(this);
    waveform_ = new WaveformPlot(this);
    waveform_->setObjectName("waveformPlot");
    auto *averageContainer = new QWidget(this);
    auto *averageLayout = new QVBoxLayout(averageContainer);
    averageLayout->setContentsMargins(0, 0, 0, 0);
    averageLabel_ = new QLabel("Run an interval or whole-recording average", this);
    averageLabel_->setWordWrap(true);
    maxHold_ = new QCheckBox("Show max hold instead of average", this);
    auto *averageTextLayout = new QVBoxLayout;
    averageTextLayout->setContentsMargins(10, 0, 0, 0);
    averageTextLayout->addWidget(averageLabel_);
    averageTextLayout->addWidget(maxHold_);
    averageLayout->addLayout(averageTextLayout);
    averageLayout->addWidget(averagePlot_, 1);
    averageDock_ =
        dock(this, "Average spectrum", "averageDock", averageContainer, Qt::RightDockWidgetArea);
    averageDock_->hide();
    waveformDock_ =
        dock(this, "Baseband waveform", "waveformDock", waveform_, Qt::BottomDockWidgetArea);
    waveformDock_->hide();
    bookmarksList_ = new QListWidget(this);
    bookmarksDock_ = dock(this, "Bookmarks / SigMF annotations", "bookmarksDock", bookmarksList_,
                          Qt::RightDockWidgetArea);
    bookmarksDock_->hide();
    buildControls();
    updateChecker_ = new UpdateChecker(preferences_.updates, this);
    connect(updateChecker_, &UpdateChecker::settingsChanged, this, &MainWindow::persistPreferences);
    connect(updateChecker_, &UpdateChecker::finished, this,
            [this](const QString &version, const QUrl &url, bool manual) {
                if (version.isEmpty()) {
                    if (manual)
                        showUpdateMessage(
                            "No newer stable release with complete Linux packages is available.");
                    return;
                }
                if (!manual && notifiedUpdateVersion_ == version)
                    return;
                showUpdateMessage(
                    QString("CroccoSpectrum %1 is available. You're running %2.\n\n"
                            "Open the release page to download and install the update.")
                        .arg(version, RF_VERSION),
                    url);
                notifiedUpdateVersion_ = version;
                preferences_.updates.lastNotifiedVersion = version;
                persistPreferences();
            });
    connect(updateChecker_, &UpdateChecker::failed, this,
            [this](const QString &message, bool manual) {
                if (manual)
                    showUpdateMessage("Couldn't check for updates.\n\n" + message);
            });
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
        connect(waterfall_, &WaterfallPlot::frequencyRangeChanged, plot,
                &SpectrumPlot::setFrequencyRange);
        connect(plot, &SpectrumPlot::measurementActiveChanged, this, [this, plot](bool active) {
            if (active)
                beginMeasurement(plot);
        });
    }
    connect(spectrum_, &SpectrumPlot::frequencyHovered, this,
            [this](double frequency) { waterfall_->setFrequencyCursor(frequency); });
    connect(spectrum_, &SpectrumPlot::cursorLeft, this,
            [this] { waterfall_->setFrequencyCursor(std::nullopt); });
    connect(waterfall_, &WaterfallPlot::measurementActiveChanged, this, [this](bool active) {
        if (active)
            beginMeasurement(waterfall_);
        else
            scheduleWaterfall();
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

void MainWindow::startUpdateChecks()
{
    updateChecker_->startAutomaticChecks();
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
    recentFilesMenu_ = file->addMenu("Open recent");
    recentFilesMenu_->setObjectName("recentFilesMenu");
    updateRecentFilesMenu();
    auto *reinterpret = file->addAction("Review / change interpretation…");
    connect(reinterpret, &QAction::triggered, this, [this] {
        if (!recording_)
            return;
        ImportDialog dialog(recording_->descriptor().path, recording_->descriptor(), this);
        if (dialog.exec() == QDialog::Accepted)
            openRecording(dialog.descriptor(), {}, openingPath_);
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
                spectrum_->clearMeasurement();
                averagePlot_->clearMeasurement();
                waterfall_->clearMeasurement();
                const auto pixels = waterfall_->pixelSize();
                generation_ = controller_.overview(recording_, range_, preferences_.dsp,
                                                   pixels.width(), requestedWaterfallRows());
                setBusy(true, "Scanning every window for exact overview…");
            });
    connect(analysis->addAction("Exact waveform over selected interval"), &QAction::triggered, this,
            &MainWindow::startWaveform);
    auto *mark = analysis->addAction("Add bookmark / note…");
    mark->setShortcut(QKeySequence("Ctrl+B"));
    connect(mark, &QAction::triggered, this, &MainWindow::addBookmark);
    auto *view = menuBar()->addMenu("&View");
    auto *darkTheme = view->addAction("Dark theme");
    darkTheme->setObjectName("darkTheme");
    darkTheme->setCheckable(true);
    darkTheme->setChecked(preferences_.darkTheme);
    connect(darkTheme, &QAction::toggled, this, [this](bool enabled) {
        preferences_.darkTheme = enabled;
        applyTheme();
        persistPreferences();
    });
    view->addSeparator();
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
    auto *settings = menuBar()->addMenu("&Settings");
    auto *rowsMenu = settings->addMenu("Waterfall rows");
    rowsMenu->setObjectName("waterfallRowsMenu");
    rowsMenu->setToolTipsVisible(true);
    waterfallRowsGroup_ = new QActionGroup(this);
    for (const int count : {0, 128, 256, 512, 1024, 2048, 4096}) {
        auto *action = rowsMenu->addAction(count == 0 ? "Automatic (display resolution)"
                                                     : QString::number(count));
        action->setCheckable(true);
        action->setData(count);
        action->setToolTip("Requested rows; the actual count is limited by available FFT windows. "
                           "More rows provide finer time detail and take longer to compute.");
        waterfallRowsGroup_->addAction(action);
    }
    connect(waterfallRowsGroup_, &QActionGroup::triggered, this, [this](QAction *action) {
        const auto rows = action->data().toInt();
        if (preferences_.view.waterfallRows == rows)
            return;
        preferences_.view.waterfallRows = rows;
        scheduleWaterfall();
        saveTimer_->start();
    });
    auto *help = menuBar()->addMenu("&Help");
    auto *updates = help->addAction("Check for updates…");
    updates->setObjectName("checkForUpdates");
    connect(updates, &QAction::triggered, this, [this] { updateChecker_->check(); });
    connect(updateChecker_, &UpdateChecker::checkingChanged, updates, [updates](bool checking) {
        updates->setEnabled(!checking);
        updates->setText(checking ? "Checking for updates…" : "Check for updates…");
    });
    auto *automaticUpdates = help->addAction("Automatically check for updates");
    automaticUpdates->setObjectName("automaticUpdateChecks");
    automaticUpdates->setCheckable(true);
    automaticUpdates->setChecked(preferences_.updates.automatic);
    connect(automaticUpdates, &QAction::toggled, updateChecker_,
            &UpdateChecker::setAutomaticChecking);
    help->addSeparator();
    auto *shortcuts = help->addAction("Keyboard shortcuts…");
    shortcuts->setObjectName("keyboardShortcuts");
    shortcuts->setShortcut(QKeySequence(Qt::Key_F1));
    connect(shortcuts, &QAction::triggered, this, &MainWindow::showKeyboardShortcuts);
    auto *plotControls = help->addAction("Plot controls…");
    plotControls->setObjectName("plotControls");
    connect(plotControls, &QAction::triggered, this, &MainWindow::showPlotControls);
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
            "Wheel: frequency zoom in spectra; frequency/time zoom in waterfall. "
            "Drag: frequency pan in spectra; frequency/time pan in waterfall. "
            "Ctrl + drag waterfall: pan only the visible time range. Drag the minimap box "
            "to navigate; drag its top/bottom edges to resize the viewport. Hover waterfall/waveform: "
            "inspect the shared frame. Click either plot to freeze/unfreeze it. Double-click: "
            "seek. CSV records analysis settings; sessions preserve interpretation and bookmarks.");
    });
    auto *about = help->addAction("About CroccoSpectrum");
    about->setObjectName("aboutCroccoSpectrum");
    connect(about, &QAction::triggered, this, &MainWindow::showAbout);
}

void MainWindow::showUpdateMessage(const QString &message, const QUrl &releaseUrl)
{
    if (auto *existing = findChild<QDialog *>("updateDialog"))
        delete existing;
    auto *dialog = new QDialog(this);
    dialog->setObjectName("updateDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("CroccoSpectrum updates");
    auto *layout = new QVBoxLayout(dialog);
    auto *label = new QLabel(message, dialog);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(label);
    auto *buttons = new QDialogButtonBox(dialog);
    auto *dismiss = buttons->addButton("Dismiss", QDialogButtonBox::RejectRole);
    connect(dismiss, &QPushButton::clicked, dialog, &QDialog::reject);
    if (!releaseUrl.isEmpty()) {
        auto *open = buttons->addButton("Open release page", QDialogButtonBox::AcceptRole);
        open->setObjectName("openUpdateRelease");
        connect(open, &QPushButton::clicked, dialog, [this, dialog, releaseUrl] {
            // Wayland's URL opener waits for an activation token from the focus window.
            // Destroy the dialog first so its deletion cannot cancel that pending launch.
            connect(dialog, &QObject::destroyed, this,
                    [this, releaseUrl] {
                        if (!QDesktopServices::openUrl(releaseUrl))
                            showUpdateMessage("Couldn't open the browser. Open this release URL "
                                              "manually, or try again:\n\n" +
                                                  releaseUrl.toDisplayString(),
                                              releaseUrl);
                    },
                    Qt::QueuedConnection);
            dialog->accept();
        });
    }
    layout->addWidget(buttons);
    dialog->resize(420, 160);
    dialog->show();
}

void MainWindow::showKeyboardShortcuts()
{
    if (auto *existing = findChild<QDialog *>("keyboardShortcutsDialog")) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName("keyboardShortcutsDialog");
    dialog->setWindowTitle("Keyboard shortcuts");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dialog);
    auto *scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *columns = new QHBoxLayout(content);
    auto *commands = new QVBoxLayout;
    auto *controls = new QVBoxLayout;
    columns->addLayout(commands, 1);
    columns->addLayout(controls, 1);
    const auto addRow = [](QFormLayout *form, const QString &keys, const QString &command) {
        auto *keyLabel = new QLabel(keys);
        auto font = keyLabel->font();
        font.setBold(true);
        keyLabel->setFont(font);
        keyLabel->setTextFormat(Qt::PlainText);
        auto *description = new QLabel(command);
        description->setTextFormat(Qt::PlainText);
        description->setWordWrap(true);
        form->addRow(keyLabel, description);
    };
    // Read the live menu bindings so the cheatsheet follows platform defaults
    // and future changes to application shortcuts.
    for (auto *menuAction : menuBar()->actions()) {
        QFormLayout *form = nullptr;
        for (auto *action : menuAction->menu()->actions()) {
            if (action->shortcuts().isEmpty())
                continue;
            if (!form) {
                auto *group = new QGroupBox(QString(menuAction->text()).remove('&'), content);
                form = new QFormLayout(group);
                commands->addWidget(group);
            }
            QStringList keys;
            for (const auto &sequence : action->shortcuts())
                keys.append(sequence.toString(QKeySequence::NativeText));
            addRow(form, keys.join(" / "), QString(action->text()).remove('&'));
        }
    }
    commands->addStretch();
    auto *plots = new QGroupBox("Plot controls", content);
    auto *plotForm = new QFormLayout(plots);
    addRow(plotForm, "Shift + left-click, then left-click",
           "Measure width / mean power in a spectrum, or duration in the waterfall");
    addRow(plotForm, "Shift + drag band", "Move a spectrum / waterfall measurement");
    addRow(plotForm, "Shift + drag marker", "Resize a spectrum / waterfall measurement");
    addRow(plotForm, QKeySequence(Qt::Key_Escape).toString(QKeySequence::NativeText),
           "Clear a measurement in the focused plot");
    addRow(plotForm, "Mouse wheel", "Zoom frequency in a spectrum; frequency and time in waterfall");
    addRow(plotForm, "Left-button drag", "Pan frequency in a spectrum; frequency and time in waterfall");
    addRow(plotForm, "Ctrl + left-button drag",
           "Pan only the waterfall time viewport");
    addRow(plotForm, "Minimap box / top and bottom edges", "Move / resize the waterfall viewport");
    addRow(plotForm, "Hover", "Inspect coordinates; follow the frame in waterfall / waveform");
    addRow(plotForm, "Left-click", "Freeze / unfreeze the frame in waterfall / waveform");
    addRow(plotForm, "Double-click", "Seek to a frame in waterfall / waveform");
    controls->addWidget(plots);
    auto *navigation = new QGroupBox("Keyboard navigation", content);
    auto *navigationForm = new QFormLayout(navigation);
    addRow(navigationForm, "Tab / Shift+Tab", "Move between controls");
    addRow(navigationForm, "Space", "Activate a focused button or checkbox");
    addRow(navigationForm, "Arrow keys",
           "Navigate menus; adjust a focused slider or numeric value");
    addRow(navigationForm, "Enter", "Run the selected menu command");
    addRow(navigationForm, "Alt + menu letter",
           "F: File · A: Analysis · V: View · H: Help (Linux / Windows)");
    addRow(navigationForm, "Esc", "Close this cheatsheet or dismiss a menu / dialog");
    controls->addWidget(navigation);
    controls->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const auto available = dialog->screen()->availableGeometry();
    dialog->resize(std::min(960, available.width() - 60), std::min(620, available.height() - 80));
    dialog->show();
}

void MainWindow::showPlotControls()
{
    if (auto *existing = findChild<QDialog *>("plotControlsDialog")) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setObjectName("plotControlsDialog");
    dialog->setWindowTitle("Plot controls");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dialog);
    auto *scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *sections = new QVBoxLayout(content);
    const auto addSection = [content, sections](const QString &title, const QString &text) {
        auto *group = new QGroupBox(title, content);
        auto *groupLayout = new QVBoxLayout(group);
        auto *label = new QLabel(text, group);
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        groupLayout->addWidget(label);
        sections->addWidget(group);
    };
    addSection("Spectrum / average spectrum",
               "Mouse wheel: zoom frequency around the pointer. Left-button drag: pan frequency. "
               "Hover: inspect frequency and power coordinates.\n\n"
               "Shift + left-click: start measuring frequency width and mean spectral power. "
               "Left-click again to finish. Shift-drag the band to move it or a marker to resize "
               "it. Escape clears the measurement in the focused plot.");
    addSection("Waterfall",
               "Mouse wheel: zoom frequency and time around the pointer. Left-button drag: pan "
               "both axes. Hold Ctrl when starting a drag to pan only time. These gestures change "
               "the waterfall viewport independently of the analysis Start/End fields. "
               "The minimap shows the whole recording: drag its box to move through time, "
               "or drag the top/bottom handles to change the visible time span.\n\n"
               "Hover: inspect the shared frame. Click to freeze/unfreeze the frame; double-click "
               "to seek.\n\n"
               "Shift + left-click: start measuring duration between waterfall rows. Left-click "
               "again to finish. Shift-drag the band to move it or a marker to resize it. Escape "
               "clears the measurement in the focused plot.");
    addSection("Baseband waveform",
               "Hover: inspect sample values and the shared frame. Click to freeze/unfreeze the "
               "frame; double-click to seek.");
    sections->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const auto available = dialog->screen()->availableGeometry();
    dialog->resize(std::min(660, available.width() - 60), std::min(620, available.height() - 80));
    dialog->show();
}

void MainWindow::updateRecentFilesMenu()
{
    recentFilesMenu_->clear();
    recentFilesMenu_->setEnabled(!preferences_.recentFiles.isEmpty());
    for (const auto &path : preferences_.recentFiles) {
        auto *action = recentFilesMenu_->addAction(QDir::toNativeSeparators(path).replace("&", "&&"));
        action->setData(path);
        connect(action, &QAction::triggered, this, [this, path] {
            if (path.endsWith(".rfsession.json"))
                openSessionPath(path);
            else
                openPath(path);
        });
    }
    recentFilesMenu_->addSeparator();
    auto *clear = recentFilesMenu_->addAction("Clear recent files");
    connect(clear, &QAction::triggered, this, [this] {
        preferences_.recentFiles.clear();
        updateRecentFilesMenu();
        saveTimer_->start();
    });
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
    autoRange_->setObjectName("automaticRange");
    waterfallAutoRangeOnZoom_ = new QCheckBox("Adapt waterfall colors on zoom", this);
    waterfallAutoRangeOnZoom_->setObjectName("waterfallAutoRangeOnZoom");
    waterfallAutoRangeOnZoom_->setToolTip(
        "With Automatic range enabled, recalculate the waterfall color limits as you zoom or pan. "
        "Turn off to keep the current limits until the recording, analysis range, or DSP settings change.");
    absoluteFrequency_ = new QCheckBox("Absolute frequency", this);
    absoluteFrequency_->setObjectName("absoluteFrequency");
    waveformMode_ = new QComboBox(this);
    waveformMode_->addItems({"I/Q", "Magnitude", "I only"});
    colorForm->addRow("Minimum", colorMin_);
    colorForm->addRow("Maximum", colorMax_);
    colorForm->addRow("Palette", palette_);
    colorForm->addRow(autoRange_);
    colorForm->addRow(waterfallAutoRangeOnZoom_);
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
    for (auto *check : {autoRange_, waterfallAutoRangeOnZoom_, absoluteFrequency_})
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
                if (descriptor.allowPartial)
                    recordingLabel_->setText(recordingLabel_->text() +
                                             "\nPartial snapshot · reopen to load more samples");
                recordingLabel_->setToolTip(QString::fromUtf8(
                    QJsonDocument(recordingToJson(descriptor)).toJson(QJsonDocument::Indented)));
                setWindowTitle(QFileInfo(descriptor.path).fileName() + " — CroccoSpectrum");
                preferences_.importDefaults = descriptor;
                preferences_.recentFiles.removeAll(openingPath_);
                preferences_.recentFiles.prepend(openingPath_);
                while (preferences_.recentFiles.size() > 10)
                    preferences_.recentFiles.removeLast();
                updateRecentFilesMenu();
                if (restoringSession_) {
                    if (!restoringSession_->sourceIdentity.isEmpty() &&
                        restoringSession_->sourceIdentity != recording_->identity())
                        reportError("Session source identity changed; results are recomputed from "
                                    "the current file");
                    bookmarks_ = restoringSession_->bookmarks;
                    restoringSession_.reset();
                } else {
                    bookmarks_ = descriptor.annotations;
                    if (descriptor.hasFrequencyAt(range_.begin))
                        absoluteFrequency_->setChecked(true);
                }
                bookmarksList_->clear();
                for (const auto &mark : bookmarks_)
                    bookmarksList_->addItem(
                        QString("%1 · frame %2").arg(mark.label).arg(mark.start));
                updateDsp();
                startMinimap();
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
                const bool preserveFrequency = preview_->aggregated;
                waterfall_->setRecordingExtent({0, recording_->frameCount()},
                                                static_cast<std::uint64_t>(preferences_.dsp.fftSize));
                // An explicit analysis overview belongs to the analysis range.
                // Keep a separately navigated waterfall viewport and its data.
                if (!preview_->aggregated || !waterfall_->snapshot() ||
                    waterfall_->viewport() == preview_->range) {
                    if (preview_->aggregated) {
                        waterfallTimer_->stop();
                        waterfallController_.cancel();
                        ++waterfallGeneration_;
                    }
                    waterfall_->setPreview(preview_, recording_->descriptor().sampleRate,
                                           !preserveFrequency);
                }
                if (waterfall_->viewport().size() == 0)
                    waterfall_->setViewport(range_);
                if (!preview_->aggregated)
                    scheduleWaterfall();
                waveformResult_ = std::make_shared<WaveformResult>(preview_->waveform);
                waveform_->setWaveform(waveformResult_, recording_->descriptor().sampleRate,
                                       recording_->descriptor().format.kind);
                displaySpectrum(preview_, !preserveFrequency);
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
    connect(&waterfallController_, &AnalysisController::waterfallReady, this,
            [this](quint64 generation, std::shared_ptr<const PreviewResult> result) {
                if (generation != waterfallGeneration_ || !recording_)
                    return;
                waterfall_->setDetail(std::move(result));
                updateFrameCursors();
                updateWaterfallCoverage();
                updateView();
            });
    connect(&minimapController_, &AnalysisController::waterfallReady, this,
            [this](quint64 generation, std::shared_ptr<const PreviewResult> result) {
                if (generation == minimapGeneration_ && recording_)
                    minimap_->setSnapshot(std::move(result));
            });
    for (auto *worker : {&waterfallController_, &minimapController_})
        connect(worker, &AnalysisController::failed, this,
                [this, worker](quint64 generation, const QString &message) {
                    if (generation == (worker == &waterfallController_ ? waterfallGeneration_ : minimapGeneration_))
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

void MainWindow::applyTheme()
{
    const bool dark = preferences_.darkTheme;
    // Build both palettes explicitly so the desktop theme cannot prevent an override.
    QPalette palette(QColor(dark ? "#202b36" : "#efefef"));
    const QColor text(dark ? "#e5edf3" : "#202020");
    const QColor muted(dark ? "#87949f" : "#767676");
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText,
                      QPalette::ToolTipText})
        palette.setColor(role, text);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, muted);
    palette.setColor(QPalette::Base, QColor(dark ? "#101923" : "#ffffff"));
    palette.setColor(QPalette::AlternateBase, QColor(dark ? "#2a3643" : "#f5f5f5"));
    palette.setColor(QPalette::ToolTipBase, palette.color(QPalette::Window));
    palette.setColor(QPalette::PlaceholderText, muted);
    palette.setColor(QPalette::Highlight, QColor(dark ? "#51d3be" : "#16836e"));
    palette.setColor(QPalette::HighlightedText, QColor(dark ? "#101923" : "#ffffff"));
    palette.setColor(QPalette::Disabled, QPalette::Highlight,
                     QColor(dark ? "#394957" : "#d5d5d5"));
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, muted);
    palette.setColor(QPalette::Link, QColor(dark ? "#70dccc" : "#006b58"));
    palette.setColor(QPalette::LinkVisited, QColor(dark ? "#c2a3e8" : "#6b3f99"));
    QApplication::setPalette(palette);
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
    waterfallAutoRangeOnZoom_->setChecked(preferences_.view.waterfallAutoRangeOnZoom);
    for (auto *action : waterfallRowsGroup_->actions())
        action->setChecked(action->data().toInt() == preferences_.view.waterfallRows);
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
    defaults.allowPartial = false;
    defaults.captures.clear();
    defaults.annotations.clear();
    defaults.centerFrequency.reset();
    defaults.startUtc.clear();
    defaults.metadataSource = "User import";
    ImportDialog dialog(path, defaults, this);
    if (dialog.exec() == QDialog::Accepted)
        openRecording(dialog.descriptor(), {}, path);
}

void MainWindow::openRecording(RecordingDescriptor descriptor, FrameRange range)
{
    const auto sourcePath = descriptor.path;
    openRecording(std::move(descriptor), range, sourcePath);
}

void MainWindow::openRecording(RecordingDescriptor descriptor, FrameRange range,
                               const QString &sourcePath)
{
    try {
        descriptor.validate();
        preferences_.dsp.validate(descriptor.sampleRate);
        previewTimer_->stop();
        minimapController_.cancel();
        ++minimapGeneration_;
        minimap_->clear();
        clearMeasurements();
        recording_.reset();
        openingPath_ = QFileInfo(sourcePath).absoluteFilePath();
        generation_ = controller_.open(std::move(descriptor), preferences_.dsp, range);
        setBusy(true, "Opening recording…");
    } catch (const std::exception &error) {
        reportError(QString::fromUtf8(error.what()));
    }
}

void MainWindow::clearMeasurements()
{
    waterfallTimer_->stop();
    waterfallController_.cancel();
    ++waterfallGeneration_;
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

void MainWindow::scheduleWaterfall()
{
    if (!recording_ || waterfall_->viewport().size() == 0 || !waterfall_->snapshot())
        return;
    // Invalidate queued work immediately. Throttle without restarting the timer
    // so a continuous gesture still obtains fresh data every bounded interval.
    waterfallController_.cancel();
    ++waterfallGeneration_;
    if (!waterfallTimer_->isActive())
        waterfallTimer_->start();
}
void MainWindow::requestWaterfall()
{
    if (!recording_ || !waterfall_->snapshot() || waterfall_->isMeasuring())
        return;
    const auto [left, right] = waterfall_->frequencyRange();
    const auto pixels = waterfall_->pixelSize();
    WaterfallRequest request{waterfall_->viewport(), left, right, pixels.width(),
                              requestedWaterfallRows(), false, preferences_.view.waterfallRows != 0};
    waterfallGeneration_ = waterfallController_.waterfall(recording_, preferences_.dsp, request);
}
int MainWindow::requestedWaterfallRows() const
{
    return preferences_.view.waterfallRows ? preferences_.view.waterfallRows
                                          : std::max(1, waterfall_->pixelSize().height());
}
void MainWindow::startMinimap()
{
    minimapController_.cancel();
    ++minimapGeneration_;
    if (!recording_ || recording_->frameCount() < static_cast<std::uint64_t>(preferences_.dsp.fftSize))
        return;
    const FrameRange extent{0, recording_->frameCount()};
    minimap_->setRecording(extent, static_cast<std::uint64_t>(preferences_.dsp.fftSize));
    const auto viewport = waterfall_->viewport();
    minimap_->setViewport(viewport.size() ? viewport : range_);
    // A small fixed grid bounds progressive copying. Resizing the UI stretches
    // this overview without restarting a full recording scan.
    WaterfallRequest request{extent, 0, 0, 192, 1024, true};
    minimapGeneration_ = minimapController_.waterfall(recording_, preferences_.dsp, request);
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
    const auto displayed = waterfall_->snapshot();
    if (displayed && frame >= displayed->range.begin && frame < displayed->range.end) {
        const auto row = std::upper_bound(displayed->rowStarts.begin(), displayed->rowStarts.end(), frame);
        frame = row == displayed->rowStarts.begin() ? displayed->rowStarts.front() : *(row - 1);
    }
    frame = std::min<std::uint64_t>(frame, recording_->frameCount() -
                                           static_cast<std::uint64_t>(preferences_.dsp.fftSize));
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
    updateWaterfallCoverage();
    updateSpectrumView();
}

void MainWindow::updateWaterfallCoverage()
{
    const auto displayed = waterfall_->snapshot();
    if (!displayed || !spectrumPreview_)
        return;
    coverageLabel_->setText(
        QString("%1 waterfall · %2 rows\n%3 invalid windows · %4 capture-boundary "
                "windows\nSpectrum begins at frame %5")
            .arg(displayed->aggregated ? "Exact maximum-aggregated"
                 : displayed->sampled  ? "Sampled preview"
                                      : "Every window shown")
            .arg(displayed->rowStarts.size())
            .arg(displayed->invalidWindows)
            .arg(displayed->boundaryWindows)
            .arg(spectrumPreview_->spectrumStart));
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
        startMinimap();
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
    view.waterfallAutoRangeOnZoom = waterfallAutoRangeOnZoom_->isChecked();
    waterfallAutoRangeOnZoom_->setEnabled(view.autoRange);
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
    const auto viewport = waterfall_->viewport();
    const auto center = uniformCenter(viewport.size() ? viewport : range_);
    effective.absoluteFrequency = view.absoluteFrequency && center.has_value();
    waterfall_->setView(effective, preferences_.dsp.scale, center.value_or(0));
    minimap_->setView(view);
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
    if (choice == "Waterfall")
        range = waterfall_->viewport();
    if (choice == "Spectrum")
        range = {spectrumPreview_->spectrumStart,
                 spectrumPreview_->spectrumStart +
                     static_cast<std::uint64_t>(preferences_.dsp.fftSize)};
    PngRequest request;
    request.image = plot->grab().toImage();
    request.output = path;
    const auto displayed = choice == "Waterfall" ? waterfall_->snapshot() : preview_;
    request.metadata = {{"application_version", RF_VERSION},
                        {"plot", choice},
                        {"recording", recordingToJson(recording_->descriptor())},
                        {"source_identity", recording_->identity()},
                        {"start", QString::number(range.begin)},
                        {"end", QString::number(range.end)},
                        {"dsp", dspToJson(preferences_.dsp)},
                        {"view", viewToJson(preferences_.view)},
                        {"unit", powerUnit(preferences_.dsp.scale)},
                        {"sampled_preview", choice != "Spectrum" && displayed->sampled},
                        {"exact_overview", choice != "Spectrum" && displayed->aggregated},
                        {"waveform_complete", waveformResult_ && waveformResult_->complete},
                        {"coverage_label", coverageLabel_->text()},
                        {"average_coverage", average_ ? averageLabel_->text() : QString()}};
    if (choice == "Waterfall") {
        const auto [left, right] = waterfall_->frequencyRange();
        request.metadata.insert("frequency_left_hz", left);
        request.metadata.insert("frequency_right_hz", right);
        request.metadata.insert("waterfall_data_start", QString::number(displayed->range.begin));
        request.metadata.insert("waterfall_data_end", QString::number(displayed->range.end));
        request.metadata.insert("waterfall_columns", displayed->columns);
        request.metadata.insert("waterfall_rows", static_cast<int>(displayed->rowStarts.size()));
    }
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
        openRecording(session.recording, session.range, path);
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
    updateChecker_->stop();
    previewTimer_->stop();
    controller_.cancel();
    spectrumController_.cancel();
    waterfallTimer_->stop();
    waterfallController_.cancel();
    minimapController_.cancel();
    ++waterfallGeneration_;
    ++minimapGeneration_;
    ++spectrumGeneration_;
    persistPreferences();
    event->accept();
}
} // namespace rf
