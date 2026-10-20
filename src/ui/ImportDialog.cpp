#include "ImportDialog.h"
#include "DisplayUnits.h"
#include "recording/Import.h"
#include "recording/Metadata.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <stdexcept>
namespace rf
{
namespace
{
double number(const QLineEdit *field, const QString &name)
{
    bool ok = false;
    const double value = field->text().trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(value))
        throw std::runtime_error(("Invalid " + name).toStdString());
    return value;
}
} // namespace
ImportDialog::ImportDialog(QString path, RecordingDescriptor defaults, QWidget *parent)
    : QDialog(parent), base_(std::move(defaults))
{
    setWindowTitle("Interpret RF recording");
    resize(QSize(680, 760).boundedTo(screen()->availableGeometry().size() - QSize(32, 32)));
    qRegisterMetaType<RecordingDescriptor>();
    if (base_.path != path) {
        base_.import = {};
        base_.import.input = inputFormatForPath(path);
        const auto suffix = QFileInfo(path).suffix().toLower();
        base_.import.delimiter = suffix == "csv" ? "," : suffix == "tsv" ? "\t" : "whitespace";
        if (base_.import.input == InputFormat::Audio || base_.import.input == InputFormat::Text) {
            base_.format.kind = SampleKind::Real;
            base_.format.floatFullScale = 1;
        }
    }
    base_.path = path;
    auto *layout = new QVBoxLayout(this);
    auto *explanation = new QLabel(
        "Verify the input format and decoded preview before analysis. Raw files have no "
        "reliable self-description. Text columns and audio channels are numbered from 1.",
        this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *fields = new QWidget(scroll);
    auto *form = new QFormLayout(fields);
    form_ = form;
    scroll->setWidget(fields);
    layout->addWidget(scroll, 1);
    auto *fileLabel = new QLabel(path, this);
    fileLabel->setWordWrap(true);
    fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("File", fileLabel);
    input_ = new QComboBox(this);
    input_->setObjectName("inputFormat");
    input_->addItems({"Raw binary", "WAV / AIFF audio", "Numeric ASCII / CSV", "gzip raw",
                      "bzip2 raw"});
    form->addRow("Input format", input_);
    conversion_ = new QLabel(this);
    conversion_->setObjectName("importConversion");
    conversion_->setWordWrap(true);
    conversion_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
    form->addRow(conversion_);
    kind_ = new QComboBox(this);
    kind_->setObjectName("sampleKind");
    kind_->addItems({"Real (one scalar)", "Complex (I/Q pair)"});
    encoding_ = new QComboBox(this);
    encoding_->setObjectName("encoding");
    for (const auto type :
         {Encoding::SignedInteger, Encoding::UnsignedInteger, Encoding::FloatingPoint})
        for (const int bits : {8, 16, 24, 32, 64}) {
            if (type == Encoding::FloatingPoint && (bits == 8 || bits == 24))
                continue;
            encoding_->addItem(QString("%1 %2-bit")
                                   .arg(type == Encoding::FloatingPoint   ? "IEEE float"
                                        : type == Encoding::SignedInteger ? "Signed integer"
                                                                          : "Unsigned integer")
                                   .arg(bits),
                               static_cast<int>(type) * 100 + bits);
        }
    byteOrder_ = new QComboBox(this);
    byteOrder_->addItems({"Little-endian", "Big-endian"});
    componentOrder_ = new QComboBox(this);
    componentOrder_->addItems({"IQIQIQ", "QIQIQI"});
    sampleRate_ = new QLineEdit(this);
    sampleRate_->setObjectName("sampleRate");
    sampleRate_->setToolTip(
        "Samples/second; scientific notation and fractional rates are accepted");
    centerFrequency_ = new QLineEdit(this);
    centerFrequency_->setPlaceholderText("Unknown (baseband only)");
    startUtc_ = new QLineEdit(this);
    startUtc_->setPlaceholderText("Optional: 2026-10-06T12:00:00.000Z");
    offset_ = new QLineEdit(this);
    length_ = new QLineEdit(this);
    length_->setPlaceholderText("To EOF");
    partial_ = new QCheckBox("Open partial / growing file", this);
    partial_->setObjectName("allowPartial");
    partial_->setToolTip(
        "Use complete frames currently available and ignore an incomplete final frame. "
        "The downloaded prefix must stay unchanged; only appends are supported. "
        "Reopen the file to load more samples.");
    fullScale_ = new QLineEdit(this);
    form->addRow("Samples", kind_);
    firstColumn_ = new QSpinBox(this);
    firstColumn_->setObjectName("firstColumn");
    secondColumn_ = new QSpinBox(this);
    secondColumn_->setObjectName("secondColumn");
    for (auto *column : {firstColumn_, secondColumn_})
        column->setRange(1, 1048576);
    delimiter_ = new QComboBox(this);
    delimiter_->setObjectName("textDelimiter");
    delimiter_->addItem("Whitespace", "whitespace");
    delimiter_->addItem("Comma", ",");
    delimiter_->addItem("Semicolon", ";");
    delimiter_->addItem("Tab", "\t");
    skipRows_ = new QSpinBox(this);
    skipRows_->setObjectName("skipRows");
    skipRows_->setRange(0, 1000000000);
    form->addRow("Real / I column or channel", firstColumn_);
    form->addRow("Q column or channel", secondColumn_);
    form->addRow("Text separator", delimiter_);
    form->addRow("Skip initial text lines", skipRows_);
    form->addRow("Scalar encoding", encoding_);
    form->addRow("Byte order", byteOrder_);
    form->addRow("Component order", componentOrder_);
    form->addRow("Sample rate (samples/s)", sampleRate_);
    form->addRow("Center frequency (Hz)", centerFrequency_);
    form->addRow("Capture start (UTC)", startUtc_);
    form->addRow("Data start (bytes)", offset_);
    form->addRow("Data length (bytes)", length_);
    form->addRow(partial_);
    form->addRow("Float full-scale reference", fullScale_);
    summary_ = new QLabel(this);
    summary_->setObjectName("importSummary");
    summary_->setWordWrap(true);
    layout->addWidget(summary_);
    preview_ = new QPlainTextEdit(this);
    preview_->setReadOnly(true);
    preview_->setMaximumHeight(155);
    layout->addWidget(preview_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Ok)->setText("Open with this interpretation");
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    timer_->setInterval(150);
    connect(timer_, &QTimer::timeout, this, &ImportDialog::requestPreview);
    const auto changed = [this] {
        ++generation_;
        controller_.cancel();
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
        timer_->start();
    };
    for (auto *field : {sampleRate_, centerFrequency_, startUtc_, offset_, length_, fullScale_})
        connect(field, &QLineEdit::textChanged, this, changed);
    for (auto *combo : {kind_, encoding_, byteOrder_, componentOrder_, delimiter_})
        connect(combo, &QComboBox::currentIndexChanged, this, changed);
    for (auto *spin : {firstColumn_, secondColumn_, skipRows_})
        connect(spin, &QSpinBox::valueChanged, this, changed);
    connect(input_, &QComboBox::currentIndexChanged, this, [this, changed] {
        updateInputControls();
        changed();
    });
    connect(kind_, &QComboBox::currentIndexChanged, this, &ImportDialog::updateInputControls);
    connect(partial_, &QCheckBox::toggled, this, changed);
    connect(&controller_, &AnalysisController::recordingOpened, this,
            [this](quint64 generation, const std::shared_ptr<Recording> &recording, FrameRange) {
                if (generation != generation_)
                    return;
                if (recording->descriptor().import.input == InputFormat::Audio) {
                    const QSignalBlocker blocker(sampleRate_);
                    sampleRate_->setText(
                        QString::number(recording->descriptor().sampleRate, 'g', 17));
                }
                QString summary = QString("%1 frames · %2 · %3 bytes/frame\nSource: %4")
                                      .arg(recording->frameCount())
                                      .arg(durationText(
                                          static_cast<double>(recording->frameCount()) /
                                          recording->descriptor().sampleRate))
                                      .arg(recording->descriptor().format.frameBytes())
                                      .arg(recording->descriptor().metadataSource);
                if (recording->descriptor().allowPartial) {
                    summary += QString("\nPartial snapshot · %1 trailing byte(s) ignored.\n"
                                       "Reopen to load newly downloaded samples.")
                                   .arg(recording->ignoredTrailingBytes());
                }
                summary_->setText(summary);
                buttons_->button(QDialogButtonBox::Ok)->setEnabled(true);
            });
    connect(&controller_, &AnalysisController::waveformReady, this,
            [this](quint64 generation, const std::shared_ptr<const WaveformResult> &result) {
                if (generation != generation_)
                    return;
                QString text = "First decoded samples (normalized):\n";
                for (std::size_t index = 0; index < std::min<std::size_t>(8, result->points.size());
                     ++index) {
                    const auto &point = result->points[index];
                    text += QString("%1: I=%2  Q=%3\n")
                                .arg(point.first)
                                .arg(point.minI, 0, 'g', 9)
                                .arg(point.minQ, 0, 'g', 9);
                }
                preview_->setPlainText(text);
            });
    connect(&controller_, &AnalysisController::failed, this,
            [this](quint64 generation, const QString &message) {
                if (generation != generation_)
                    return;
                preview_->setPlainText(message);
                buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
            });
    connect(this, &ImportDialog::metadataLoaded, this,
            [this](const RecordingDescriptor &descriptor) {
                setDescriptor(descriptor);
                timer_->start();
            });
    connect(this, &ImportDialog::metadataFailed, this, [this](const QString &error) {
        summary_->setText(error);
        timer_->stop();
    });
    setDescriptor(base_);
    if (path.endsWith(".sigmf-meta") || path.endsWith(".rfmeta.json")) {
        timer_->stop();
        summary_->setText("Reading metadata…");
        // Metadata parsing also stays off the GUI thread. Join before QObject
        // destruction; Qt discards queued callbacks when the dialog closes.
        metadataLoader_ = std::jthread([this, path] {
            try {
                emit metadataLoaded(
                    path.endsWith(".sigmf-meta")
                        ? readSigMf(path)
                        : recordingFromJson(readJsonObject(path)["recording"].toObject()));
            } catch (const std::exception &error) {
                emit metadataFailed(QString::fromUtf8(error.what()));
            }
        });
    } else
        timer_->start();
}
ImportDialog::~ImportDialog()
{
    if (metadataLoader_.joinable())
        metadataLoader_.join();
}
void ImportDialog::setDescriptor(const RecordingDescriptor &descriptor)
{
    base_ = descriptor;
    input_->setCurrentIndex(static_cast<int>(descriptor.import.input));
    firstColumn_->setValue(descriptor.import.firstColumn);
    secondColumn_->setValue(descriptor.import.secondColumn);
    delimiter_->setCurrentIndex(delimiter_->findData(descriptor.import.delimiter));
    skipRows_->setValue(descriptor.import.skipRows);
    kind_->setCurrentIndex(descriptor.format.kind == SampleKind::Real ? 0 : 1);
    encoding_->setCurrentIndex(encoding_->findData(
        static_cast<int>(descriptor.format.encoding) * 100 + descriptor.format.bits));
    byteOrder_->setCurrentIndex(descriptor.format.byteOrder == ByteOrder::LittleEndian ? 0 : 1);
    componentOrder_->setCurrentIndex(descriptor.format.componentOrder == ComponentOrder::IQ ? 0
                                                                                            : 1);
    sampleRate_->setText(QString::number(descriptor.sampleRate, 'g', 17));
    centerFrequency_->setText(descriptor.centerFrequency
                                  ? QString::number(*descriptor.centerFrequency, 'g', 17)
                                  : QString());
    startUtc_->setText(descriptor.startUtc);
    offset_->setText(QString::number(descriptor.dataOffset));
    length_->setText(descriptor.dataBytes ? QString::number(*descriptor.dataBytes) : QString());
    partial_->setChecked(descriptor.allowPartial);
    fullScale_->setText(QString::number(descriptor.format.floatFullScale, 'g', 17));
    updateInputControls();
}
void ImportDialog::updateInputControls()
{
    const auto input = static_cast<InputFormat>(input_->currentIndex());
    const bool audio = input == InputFormat::Audio;
    const bool text = input == InputFormat::Text;
    const bool mapped = audio || text;
    form_->setRowVisible(firstColumn_, mapped);
    form_->setRowVisible(secondColumn_, mapped && kind_->currentIndex() == 1);
    form_->setRowVisible(delimiter_, text);
    form_->setRowVisible(skipRows_, text);
    for (auto *widget : {encoding_, byteOrder_, componentOrder_})
        form_->setRowVisible(widget, !mapped);
    for (auto *field : {offset_, length_})
        form_->setRowVisible(field, !mapped);
    form_->setRowVisible(partial_, input == InputFormat::Raw);
    sampleRate_->setEnabled(!audio);
    conversion_->setText(
        input == InputFormat::Raw ? "Raw bytes are read directly; no conversion."
        : audio ? "Selected channels → temporary float64. PCM integers are normalized; sample rate "
                  "comes from the header. Allow disk space for all decoded samples. "
                  "Cancel stops conversion and removes temporary files."
        : text ? "Selected columns → temporary float64; values are kept as supplied. "
                 "Allow disk space for all samples. "
                 "Cancel stops conversion and removes temporary files."
                 : "The entire file is decompressed to temporary disk storage before analysis. "
                   "Encoding, offset and length below apply to decompressed raw bytes. "
                   "Allow disk space for the expanded file; Cancel removes temporary files.");
}
RecordingDescriptor ImportDialog::descriptor() const
{
    auto descriptor = base_;
    descriptor.import.input = static_cast<InputFormat>(input_->currentIndex());
    descriptor.import.firstColumn = firstColumn_->value();
    descriptor.import.secondColumn = secondColumn_->value();
    descriptor.import.delimiter = delimiter_->currentData().toString();
    descriptor.import.skipRows = skipRows_->value();
    descriptor.allowPartial = partial_->isChecked();
    descriptor.format.kind = kind_->currentIndex() == 0 ? SampleKind::Real : SampleKind::Complex;
    const int format = encoding_->currentData().toInt();
    descriptor.format.encoding = static_cast<Encoding>(format / 100);
    descriptor.format.bits = format % 100;
    descriptor.format.byteOrder =
        byteOrder_->currentIndex() == 0 ? ByteOrder::LittleEndian : ByteOrder::BigEndian;
    descriptor.format.componentOrder =
        componentOrder_->currentIndex() == 0 ? ComponentOrder::IQ : ComponentOrder::QI;
    descriptor.sampleRate = number(sampleRate_, "sample rate");
    descriptor.startUtc = startUtc_->text().trimmed();
    descriptor.format.floatFullScale = number(fullScale_, "full scale");
    descriptor.centerFrequency =
        centerFrequency_->text().trimmed().isEmpty()
            ? std::nullopt
            : std::optional<double>(number(centerFrequency_, "center frequency"));
    descriptor.dataOffset = jsonUnsigned(offset_->text().trimmed(), "data offset");
    descriptor.dataBytes =
        length_->text().trimmed().isEmpty()
            ? std::nullopt
            : std::optional<std::uint64_t>(jsonUnsigned(length_->text().trimmed(), "data length"));
    if (descriptor.import.input != InputFormat::Raw)
        descriptor.allowPartial = false;
    if (descriptor.import.input == InputFormat::Audio ||
        descriptor.import.input == InputFormat::Text) {
        descriptor.format.encoding = Encoding::FloatingPoint;
        descriptor.format.bits = 64;
        descriptor.format.byteOrder = ByteOrder::LittleEndian;
        descriptor.format.componentOrder = ComponentOrder::IQ;
        descriptor.dataOffset = 0;
        descriptor.dataBytes.reset();
    }
    descriptor.validate();
    return descriptor;
}
void ImportDialog::requestPreview()
{
    try {
        if (input_->currentIndex() != static_cast<int>(InputFormat::Raw))
            summary_->setText("Preparing temporary sample file… Cancel stops the import.");
        generation_ = controller_.inspect(descriptor());
    } catch (const std::exception &error) {
        summary_->setText(QString::fromUtf8(error.what()));
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
    }
}
} // namespace rf
