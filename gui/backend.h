// The GUI's connection to pdispd: a QLocalSocket on the control socket (docs/control.md), the daemon's state as properties for QML.
#pragma once

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QVariantMap state READ state NOTIFY stateChanged)      // the daemon's "state" event as it is: session, monitor, config, usb_state, ...
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantMap pairing READ pairing NOTIFY pairingChanged) // the open pairing request (id, name, fingerprint, code), empty if none
    Q_PROPERTY(QString socketPath READ socketPath CONSTANT)
    Q_PROPERTY(QString iconFont READ iconFont CONSTANT)                 // the Material Symbols font family, empty if none could be found

public:
    explicit Backend(const QString &socketPath, QObject *parent = nullptr);
    // m_sock is declared before m_state and m_pairing, so it is destroyed after them, and closing it makes it emit disconnected(), which clears them:
    // cut the socket off from this object first (AddressSanitizer found the use after free)
    ~Backend() override { m_retry.stop(); QObject::disconnect(&m_sock, nullptr, this, nullptr); }

    bool connected() const { return m_connected; }
    QVariantMap state() const { return m_state; }
    QVariantList devices() const { return m_devices; }
    QVariantMap pairing() const { return m_pairing; }
    QString socketPath() const { return m_path; }
    QString iconFont() const { return m_iconFont; }

    Q_INVOKABLE void disconnectDevice();
    Q_INVOKABLE void answerPairing(int id, const QString &answer); // "always", "once" or "deny"
    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void removeDevice(const QString &fingerprint);
    Q_INVOKABLE void setConfig(const QString &key, int value);
    Q_INVOKABLE void setConfigText(const QString &key, const QString &value);
    Q_INVOKABLE void startDaemon();                                 // systemctl --user start pdispd

signals:
    void connectedChanged();
    void stateChanged();
    void devicesChanged();
    void pairingChanged();
    void pairingRequested(const QVariantMap &request);
    void error(const QString &message);                              // a command the daemon refused

private slots:
    void tryConnect();
    void onConnected();
    void onDisconnected();
    void onReadyRead();

private:
    void send(const QJsonObject &obj);
    void handle(const QJsonObject &obj);

    QString m_path;
    QString m_iconFont;
    QLocalSocket m_sock;
    QTimer m_retry;
    QByteArray m_buf;
    bool m_connected = false;
    QVariantMap m_state, m_pairing;
    QVariantList m_devices;
};
