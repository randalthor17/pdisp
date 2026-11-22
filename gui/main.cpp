// pdisp-gui: status, paired devices, settings and the pairing dialog of pdispd, in a window and a tray icon.
//   pdisp-gui [--background] [--socket PATH] [--screenshot FILE [--page N] [--delay MS]]
// --background starts hidden (tray only); --screenshot writes the window to a PNG after a delay and quits (used by the tests, works offscreen).
#include <QCommandLineParser>
#include <QDir>
#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>

#include "backend.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv); // Widgets, because Qt Labs Platform (the tray icon) falls back on it
    app.setApplicationName("pdisp-gui");
    app.setOrganizationName("pdisp");
    app.setApplicationDisplayName("pdisp");
    app.setDesktopFileName("pdisp-gui");
    app.setWindowIcon(QIcon(":/pdisp.svg"));
    app.setQuitOnLastWindowClosed(false); // the tray keeps it running

    QCommandLineParser p;
    p.addHelpOption();
    QCommandLineOption background("background", "Start hidden, in the tray only.");
    QCommandLineOption socket("socket", "The pdispd control socket.", "path");
    QCommandLineOption shot("screenshot", "Write the window to this PNG and quit.", "file");
    QCommandLineOption page("page", "With --screenshot: the page to show (0 status, 1 devices, 2 settings).", "n", "0");
    QCommandLineOption answer("test-answer", "Testing: answer every pairing request with this (always, once, deny) after a moment.", "answer");
    QCommandLineOption delay("delay", "With --screenshot: milliseconds to wait first.", "ms", "1500");
    p.addOptions({background, socket, shot, page, delay, answer});
    p.process(app);

    QString path = p.value(socket);
    if (path.isEmpty()) path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/pdisp/control.sock";

    QQuickStyle::setStyle("Fusion"); // follows the desktop's palette (qtengine, KDE colour schemes)
    Backend backend(path);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("startHidden", p.isSet(background) && !p.isSet(shot));
    engine.rootContext()->setContextProperty("initialPage", p.value(page).toInt());
    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;

    if (p.isSet(answer)) {
        QObject::connect(&backend, &Backend::pairingRequested, &app, [&backend, a = p.value(answer)](const QVariantMap &r) {
            QTimer::singleShot(700, [&backend, id = r.value("id").toInt(), a] { backend.answerPairing(id, a); });
        });
    }

    if (p.isSet(shot)) {
        auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QTimer::singleShot(p.value(delay).toInt(), &app, [win, file = p.value(shot), &app] {
            const QImage img = win->grabWindow();
            app.exit(img.save(file) ? 0 : 3);
        });
    }
    return app.exec();
}
