#include "app/Session.h"
#include "recording/Import.h"
#include "recording/Metadata.h"
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

namespace
{
void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        throw std::runtime_error("Fixture write failed");
}
} // namespace

class ImportTests : public QObject
{
    Q_OBJECT
  private slots:
    void textMappingAndPersistence()
    {
        QTemporaryDir directory;
        rf::RecordingDescriptor descriptor;
        descriptor.path = directory.filePath("samples.csv");
        writeFile(descriptor.path, "time,Q,I\n0,-0.25,0.5\n1,0.75,-0.125\n");
        descriptor.import = {rf::InputFormat::Text, 3, 2, ",", 1};
        descriptor.sampleRate = 1234.5;
        descriptor.centerFrequency = 915000000;
        descriptor.startUtc = "2026-10-20T12:00:00Z";
        rf::Recording recording(descriptor);
        const auto values = recording.read({0, 2});
        QCOMPARE(values[0], std::complex<double>(0.5, -0.25));
        QCOMPARE(values[1], std::complex<double>(-0.125, 0.75));
        const auto &actual = recording.descriptor();
        QCOMPARE(actual.path, descriptor.path);
        QCOMPARE(actual.sampleRate, descriptor.sampleRate);
        QCOMPARE(actual.centerFrequency, descriptor.centerFrequency);
        QCOMPARE(actual.startUtc, descriptor.startUtc);
        QCOMPARE(actual.format.encoding, rf::Encoding::FloatingPoint);
        QCOMPARE(actual.format.bits, 64);
        const auto saved = rf::recordingFromJson(rf::recordingToJson(actual));
        QCOMPARE(saved.import, descriptor.import);
        rf::Recording reopened(saved);
        QCOMPARE(reopened.read({0, 2}), values);
        QCOMPARE(reopened.identity(), recording.identity());
        rf::Session session;
        session.recording = actual;
        session.range = {0, 2};
        const auto sessionPath = directory.filePath("session.json");
        rf::saveSession(sessionPath, session);
        QCOMPARE(rf::readJsonObject(sessionPath)["schema"].toInt(), 2);
        rf::Recording restored(rf::readSession(sessionPath).recording);
        QCOMPARE(restored.read({0, 2}), values);
        const auto exportPath = directory.filePath("export.bin");
        rf::exportSamples(recording, {0, 2}, exportPath, {});
        const auto exported = rf::recordingFromJson(
            rf::readJsonObject(exportPath + ".rfmeta.json")["recording"].toObject());
        QCOMPARE(exported.import.input, rf::InputFormat::Raw);
        rf::Recording exportedRecording(exported);
        QCOMPARE(exportedRecording.read({0, 2}), values);
        // Source mutations remain errors even though analysis uses a temporary copy.
        writeFile(descriptor.path, "changed\n");
        QVERIFY_EXCEPTION_THROWN(recording.read({0, 1}), std::runtime_error);
    }

    void boundedConversionCancellationAndCleanup()
    {
        QTemporaryDir directory;
        const auto previous = qgetenv("TMPDIR");
        qputenv("TMPDIR", directory.path().toUtf8());
        const auto restore = qScopeGuard([&] {
            if (previous.isNull())
                qunsetenv("TMPDIR");
            else
                qputenv("TMPDIR", previous);
        });
        const auto temporaryFiles = [&] {
            QStringList files;
            // Qt uses unnamed O_TMPFILE storage on Linux when available.
            for (const auto &entry : QDir("/proc/self/fd").entryList(
                     QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System)) {
                const auto target = QFile::symLinkTarget("/proc/self/fd/" + entry);
                if (target.startsWith(directory.path() + "/") &&
                    (target.contains("croccospectrum-import-") || target.endsWith(" (deleted)")))
                    files.append(target);
            }
            return files;
        };
        rf::RecordingDescriptor descriptor;
        descriptor.path = directory.filePath("samples.txt");
        descriptor.format.kind = rf::SampleKind::Real;
        descriptor.import.input = rf::InputFormat::Text;
        writeFile(descriptor.path, QByteArray("0.5\n").repeated(100000));
        int checkpoints = 0;
        QVERIFY_EXCEPTION_THROWN(rf::Recording(descriptor, [&] { return ++checkpoints > 40000; }),
                                 rf::Cancelled);
        QVERIFY(temporaryFiles().isEmpty());
        {
            rf::Recording recording(descriptor);
            QCOMPARE(recording.frameCount(), std::uint64_t{100000});
            QCOMPARE(temporaryFiles().size(), qsizetype{1});
            QVERIFY_EXCEPTION_THROWN(recording.readBytes({0, 10000000}), std::runtime_error);
        }
        QVERIFY(temporaryFiles().isEmpty());
        // An overlong row is bounded even when it would have been skipped.
        descriptor.import.skipRows = 1;
        writeFile(descriptor.path, QByteArray(1024 * 1024 + 1, '0') + "\n0.5\n");
        QVERIFY_EXCEPTION_THROWN(rf::Recording{descriptor}, std::runtime_error);
        QVERIFY(temporaryFiles().isEmpty());
        descriptor.import.skipRows = 0;
        writeFile(descriptor.path, "0.5\nbad\n");
        QVERIFY_EXCEPTION_THROWN(rf::Recording{descriptor}, std::runtime_error);
        QVERIFY(temporaryFiles().isEmpty());
        writeFile(descriptor.path, "0.5\n");
        descriptor.import.firstColumn = 2;
        QVERIFY_EXCEPTION_THROWN(rf::Recording{descriptor}, std::runtime_error);
        descriptor.import.firstColumn = 1;
        descriptor.allowPartial = true;
        QVERIFY_EXCEPTION_THROWN(rf::Recording{descriptor}, std::runtime_error);
        QVERIFY(temporaryFiles().isEmpty());
    }

    void invalidImportSettings()
    {
        rf::RecordingDescriptor descriptor;
        descriptor.path = "test";
        descriptor.import.input = rf::InputFormat::Text;
        auto json = rf::recordingToJson(descriptor);
        auto settings = json["import"].toObject();
        settings["schema"] = 2;
        json["import"] = settings;
        QVERIFY_EXCEPTION_THROWN(rf::recordingFromJson(json), std::runtime_error);
        settings["schema"] = 1;
        settings["first_column"] = 1.5;
        json["import"] = settings;
        QVERIFY_EXCEPTION_THROWN(rf::recordingFromJson(json), std::runtime_error);
        descriptor.import.secondColumn = 1;
        QVERIFY_EXCEPTION_THROWN(descriptor.validate(), std::runtime_error);
        QCOMPARE(rf::inputFormatForPath("test.WAV"), rf::InputFormat::Audio);
        QCOMPARE(rf::inputFormatForPath("test.aifc"), rf::InputFormat::Audio);
        QCOMPARE(rf::inputFormatForPath("test.csv"), rf::InputFormat::Text);
        QCOMPARE(rf::inputFormatForPath("test.iq.gz"), rf::InputFormat::Gzip);
        QCOMPARE(rf::inputFormatForPath("test.iq.bz2"), rf::InputFormat::Bzip2);
        QCOMPARE(rf::inputFormatForPath("test.iq"), rf::InputFormat::Raw);
    }
};

QTEST_GUILESS_MAIN(ImportTests)
#include "ImportTests.moc"
