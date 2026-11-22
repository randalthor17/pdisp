// pdisp-gui: status, paired devices, settings and the pairing dialog of pdispd, in a window and a tray icon.
//   pdisp-gui [--background] [--socket PATH] [--screenshot FILE [--page N] [--delay MS]]
// --background starts hidden (tray only); --screenshot writes the window to a PNG after a delay and quits (used by the tests, works offscreen).
#include <QCommandLineParser>
#include <QDir>
#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QStyleHints>
#include <QPainter>
#include <QUrl>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtQml>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>

#include "backend.h"

// The tray glyph is one colour: tint it with the theme's text colour (a tray shows it on the shell's own bar, so text colour is the safest guess)
// and hand the tray a PNG file (the tray protocol wants pixels, not a vector).
static QUrl tintedTrayIcon()
{
    QImage img = QIcon(":/pdisp-tray.svg").pixmap(64, 64).toImage().convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QPainter pt(&img);
    pt.setCompositionMode(QPainter::CompositionMode_SourceIn);
    pt.fillRect(img.rect(), QApplication::palette().color(QPalette::WindowText));
    pt.end();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString file = dir + "/tray-" + QString::number(QApplication::palette().color(QPalette::WindowText).rgb(), 16) + ".png";
    return img.save(file) ? QUrl::fromLocalFile(file) : QUrl("qrc:/pdisp-tray.svg");
}

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
    QCommandLineOption testPalette("test-palette", "Testing: pretend this Qt theme: window,text,highlight (colours like #130d09,#f8e1d6,#f6b998).", "colors");
    QCommandLineOption size("size", "Window size, for example 420x760.", "WxH");
    QCommandLineOption answer("test-answer", "Testing: answer every pairing request with this (always, once, deny) after a moment.", "answer");
    QCommandLineOption delay("delay", "With --screenshot: milliseconds to wait first.", "ms", "1500");
    p.addOptions({background, socket, shot, page, delay, answer, size, testPalette});
    p.process(app);

    QString path = p.value(socket);
    if (path.isEmpty()) path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/pdisp/control.sock";

    if (p.isSet(testPalette)) {
        const QStringList c = p.value(testPalette).split(',');
        if (c.size() >= 3) {
            QPalette pal;
            pal.setColor(QPalette::Window, c[0]);
            pal.setColor(QPalette::Base, c[0]);
            pal.setColor(QPalette::WindowText, c[1]);
            pal.setColor(QPalette::Text, c[1]);
            pal.setColor(QPalette::Highlight, c[2]);
            QApplication::setPalette(pal);
        }
    }
    QQuickStyle::setStyle("Basic"); // the app draws its own Material 3 components; Basic only supplies behaviour
    Backend backend(path);
    QQmlApplicationEngine engine;
    qmlRegisterSingletonInstance("Pdisp", 1, 0, "Backend", &backend); // for the Theme singleton, which cannot see context properties
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("startHidden", p.isSet(background) && !p.isSet(shot));
    engine.rootContext()->setContextProperty("trayIcon", tintedTrayIcon());
    QObject::connect(app.styleHints(), &QStyleHints::colorSchemeChanged, &engine, [&engine](Qt::ColorScheme) { engine.rootContext()->setContextProperty("trayIcon", tintedTrayIcon()); });
    engine.rootContext()->setContextProperty("initialPage", p.value(page).toInt());
    engine.load(QUrl("qrc:/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    if (p.isSet(size)) {
        const QStringList wh = p.value(size).split('x');
        if (wh.size() == 2) if (auto *w = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) w->resize(wh[0].toInt(), wh[1].toInt());
    }

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
