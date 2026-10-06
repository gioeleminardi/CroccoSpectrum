#include "ui/MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QFont>
#include <QTimer>
#include <iostream>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("CroccoSpectrum");
    QCoreApplication::setApplicationName("CroccoSpectrum");
    // Match the installed desktop entry, including Wayland's application ID.
    QGuiApplication::setDesktopFileName("croccospectrum");
    QCoreApplication::setApplicationVersion(RF_VERSION);
    QApplication::setStyle("Fusion");
    // The release ships DejaVu Sans. Keep the family consistent across Linux
    // desktops while retaining Qt's DPI-aware point size and scaling behavior.
    auto font = app.font();
    font.setFamily("DejaVu Sans");
    app.setFont(font);
    QCommandLineParser parser;
    parser.setApplicationDescription("CroccoSpectrum — offline RF recording analyzer");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("recording", "Recording or SigMF metadata to open");
    parser.addOption(
        {"accept-defaults",
         "Open raw int16 IQ at 100 MS/s without the import dialog (explicit automation option)"});
    parser.addOption({"screenshot", "Save a rendered window PNG after preview and exit", "path"});
    parser.addOption(
        {"smoke-test", "Exit after successful initialization/preview; failures return nonzero"});
    parser.process(app);
    rf::MainWindow window;
    app.setWindowIcon(window.windowIcon());
    window.show();
    const bool smoke = parser.isSet("smoke-test");
    const QString screenshot = parser.value("screenshot");
    const auto complete = [&app, &window, screenshot] {
        // Let docking/layout and queued paint events settle before capture.
        QTimer::singleShot(100, &window, [&app, &window, screenshot] {
            if (!screenshot.isEmpty() && !window.grab().save(screenshot, "PNG"))
                app.exit(2);
            else
                app.quit();
        });
    };
    if (smoke || !screenshot.isEmpty()) {
        QObject::connect(&window, &rf::MainWindow::analysisDisplayed, &window, complete);
        QObject::connect(&window, &rf::MainWindow::analysisError, &window,
                         [&app](const QString &message) {
                             std::cerr << message.toStdString() << '\n';
                             app.exit(1);
                         });
        QTimer::singleShot(30000, &app, [&app] {
            std::cerr << "GUI initialization timed out\n";
            app.exit(3);
        });
    }
    const auto arguments = parser.positionalArguments();
    if (!arguments.isEmpty()) {
        const auto path = QFileInfo(arguments.front()).absoluteFilePath();
        QTimer::singleShot(0, &window, [&window, &parser, path] {
            if (parser.isSet("accept-defaults")) {
                rf::RecordingDescriptor descriptor;
                descriptor.path = path;
                window.openRecording(descriptor);
            } else
                window.openPath(path);
        });
    } else if (smoke || !screenshot.isEmpty())
        QTimer::singleShot(100, &window, complete);
    return app.exec();
}
