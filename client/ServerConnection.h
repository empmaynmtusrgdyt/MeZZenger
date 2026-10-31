#ifndef SERVERCONNECTION_H
#define SERVERCONNECTION_H

#include <QObject>
#include <QSslSocket>
#include <QTimer>
#include <QPointer>
#include <QWidget>
#include <QJsonObject>
#include <QList>

// Единственное соединение клиента с сервером: TLS, разбор кадров, вход и переподключение.
// Сначала им пользуется окно входа (попытка входа/регистрации), после успеха соединение
// передаётся главному окну без повторного подключения.
class ServerConnection : public QObject {
    Q_OBJECT

public:
    explicit ServerConnection(QObject *parent = nullptr);
    ~ServerConnection() override;

    // Окно-родитель для вопросов о сертификате
    void setDialogParent(QWidget *widget) { m_dialogParent = widget; }

    // Подключается и входит; непустой invite означает регистрацию, при переподключении всегда вход
    void open(const QString &host, quint16 port, const QString &nick, const QString &password,
              const QString &invite = QString());
    void close();

    // После передачи главному окну: разрыв связи приводит к переподключению
    void setAutoReconnect(bool enabled);

    // Сообщения, пришедшие после ответа на вход, удерживаются, пока потребитель не готов
    void startDispatch();

    bool isAuthenticated() const { return m_authenticated; }
    void send(const QJsonObject &message);

signals:
    void authenticated();
    void authRejected(const QString &message);
    // Не удалось подключиться (только до передачи главному окну)
    void failed(const QString &error);
    void tlsRejected();
    void disconnected();
    void messageReceived(const QJsonObject &message);

private:
    QSslSocket *m_socket;
    QTimer *m_reconnectTimer;
    QTimer *m_attemptTimer;
    QPointer<QWidget> m_dialogParent;

    QString m_host;
    quint16 m_port = 0;
    QString m_nick;
    QString m_password;
    QString m_invite;

    QByteArray m_buffer;
    QList<QJsonObject> m_held;
    bool m_authenticated = false;
    bool m_attemptActive = false;
    bool m_autoReconnect = false;
    bool m_rejected = false;
    bool m_dispatching = false;

    void onReadyRead();
    void onEncrypted();
    void onDisconnected();
    void onError();
    void handleAuthResponse(const QJsonObject &message);
    void finishAttempt(const QString &error);
    void connectNow();
};

#endif
