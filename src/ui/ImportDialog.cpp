#include "ImportDialog.h"
#include "recording/Metadata.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
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
    resize(640, 710);
    qRegisterMetaType<RecordingDescriptor>();
    base_.path = path;
    auto *layout = new QVBoxLayout(this);
    auto *explanation = new QLabel(
        "Raw files have no reliable self-description. Verify this interpretation before "
        "analysis.\nDefault raw format: signed int16 complex I/Q, 100 MS/s, little-endian IQ.",
        this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto *form = new QFormLayout;
    auto *fileLabel = new QLabel(path, this);
    fileLabel->setWordWrap(true);
    fileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow("File", fileLabel);
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
    length_->setPlaceholderText("To EOF; incomplete frames are rejected");
    fullScale_ = new QLineEdit(this);
    form->addRow("Samples", kind_);
    form->addRow("Scalar encoding", encoding_);
    form->addRow("Byte order", byteOrder_);
    form->addRow("Component order", componentOrder_);
    form->addRow("Sample rate (samples/s)", sampleRate_);
    form->addRow("Center frequency (Hz)", centerFrequency_);
    form->addRow("Capture start (UTC)", startUtc_);
    form->addRow("Data start (bytes)", offset_);
    form->addRow("Data length (bytes)", length_);
    form->addRow("Float full-scale reference", fullScale_);
    layout->addLayout(form);
    summary_ = new QLabel(this);
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
    for (auto *combo : {kind_, encoding_, byteOrder_, componentOrder_})
        connect(combo, &QComboBox::currentIndexChanged, this, changed);
    connect(&controller_, &AnalysisController::recordingOpened, this,
            [this](quint64 generation, const std::shared_ptr<Recording> &recording, FrameRange) {
                if (generation != generation_)
                    return;
                summary_->setText(QString("%1 frames · %2 seconds · %3 bytes/frame\nSource: %4")
                                      .arg(recording->frameCount())
                                      .arg(static_cast<double>(recording->frameCount()) /
                                               recording->descriptor().sampleRate,
                                           0, 'g', 10)
                                      .arg(recording->descriptor().format.frameBytes())
                                      .arg(base_.metadataSource));
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
    fullScale_->setText(QString::number(descriptor.format.floatFullScale, 'g', 17));
}
RecordingDescriptor ImportDialog::descriptor() const
{
    auto descriptor = base_;
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
    descriptor.validate();
    return descriptor;
}
void ImportDialog::requestPreview()
{
    try {
        generation_ = controller_.inspect(descriptor());
    } catch (const std::exception &error) {
        summary_->setText(QString::fromUtf8(error.what()));
        buttons_->button(QDialogButtonBox::Ok)->setEnabled(false);
    }
}
} // namespace rf
