#include "backend.h"

#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QStandardPaths>

Backend::Backend(const QString &socketPath, QObject *parent) : QObject(parent), m_path(socketPath)
{
    // Material Symbols for the icons: the system's font, else the one this build was given
    const QStringList families = QFontDatabase::families();
    for (const QString &f : {QString("Material Symbols Rounded"), QString("Material Symbols Outlined")})
        if (m_iconFont.isEmpty() && families.contains(f)) m_iconFont = f;
#ifdef PDISP_SYMBOLS_FONT
    if (m_iconFont.isEmpty()) {
        const int id = QFontDatabase::addApplicationFont(QStringLiteral(PDISP_SYMBOLS_FONT));
        if (id >= 0 && !QFontDatabase::applicationFontFamilies(id).isEmpty()) m_iconFont = QFontDatabase::applicationFontFamilies(id).first();
    }
#endif

    connect(&m_sock, &QLocalSocket::connected, this, &Backend::onConnected);
    connect(&m_sock, &QLocalSocket::disconnected, this, &Backend::onDisconnected);
    connect(&m_sock, &QLocalSocket::readyRead, this, &Backend::onReadyRead);
    connect(&m_sock, &QLocalSocket::errorOccurred, this, [this] { if (!m_connected) m_sock.abort(); });
    m_retry.setInterval(1000);
    connect(&m_retry, &QTimer::timeout, this, &Backend::tryConnect);
    m_retry.start();
    tryConnect();
}

void Backend::tryConnect()
{
    if (m_sock.state() == QLocalSocket::UnconnectedState) m_sock.connectToServer(m_path);
}

void Backend::onConnected()
{
    m_connected = true;
    emit connectedChanged();
    send({{"cmd", "subscribe"}, {"pairing", true}});
    refreshDevices();
}

void Backend::onDisconnected()
{
    m_buf.clear();
    if (m_connected) {
        m_connected = false;
        m_state.clear();
        m_pairing.clear();
        emit connectedChanged();
        emit stateChanged();
        emit pairingChanged();
    }
}

void Backend::onReadyRead()
{
    m_buf += m_sock.readAll();
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        const QByteArray line = m_buf.left(nl);
        m_buf.remove(0, nl + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) handle(doc.object());
    }
}

void Backend::handle(const QJsonObject &o)
{
    const QString event = o.value("event").toString();
    if (event == "state") {
        m_state = o.toVariantMap();
        emit stateChanged();
    } else if (event == "pairing") {
        m_pairing = o.toVariantMap();
        emit pairingChanged();
        emit pairingRequested(m_pairing);
    } else if (event == "pairing_done") {
        if (m_pairing.value("id") == o.value("id").toVariant()) {
            m_pairing.clear();
            emit pairingChanged();
        }
        refreshDevices(); // an "always" adds one
    } else if (o.contains("reply")) {
        const QString reply = o.value("reply").toString();
        if (reply == "devices" && o.value("ok").toBool()) {
            m_devices = o.value("devices").toArray().toVariantList();
            emit devicesChanged();
        } else if (reply == "remove_device" && o.value("ok").toBool()) {
            refreshDevices();
        }
        if (!o.value("ok").toBool()) emit error(o.value("error").toString());
    }
}

void Backend::send(const QJsonObject &obj)
{
    if (m_sock.state() != QLocalSocket::ConnectedState) return;
    m_sock.write(QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n');
}

void Backend::disconnectDevice() { send({{"cmd", "disconnect"}}); }
void Backend::answerPairing(int id, const QString &answer) { send({{"cmd", "pair_answer"}, {"id", id}, {"answer", answer}}); }
void Backend::refreshDevices() { send({{"cmd", "devices"}}); }
void Backend::removeDevice(const QString &fp) { send({{"cmd", "remove_device"}, {"fingerprint", fp}}); }
// the daemon's commands take every value as text
void Backend::setConfig(const QString &key, int value) { send({{"cmd", "set_config"}, {"key", key}, {"value", QString::number(value)}}); }
void Backend::setConfigText(const QString &key, const QString &value) { send({{"cmd", "set_config"}, {"key", key}, {"value", value}}); }
void Backend::startDaemon() { QProcess::startDetached("systemctl", {"--user", "start", "pdispd"}); }
