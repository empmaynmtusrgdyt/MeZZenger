#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QJsonObject>
#include <QTimer>
#include "Protocol.h"
#include "RateLimiter.h"

class DatabaseServer;

class ClientHandler : public QObject {
    Q_OBJECT

public:
    explicit ClientHandler(QTcpSocket *socket, DatabaseServer *db, RateLimiter *authLimiter,
                           QObject *parent = nullptr);
    ~ClientHandler();

    int getUserId() const { return m_userId; }
    QString getNick() const { return m_nick; }
    bool isAuthenticated() const { return m_authenticated; }

    void sendMessage(const QJsonObject &message);

signals:
    void messageReady(int toUserId, int messageId, const QJsonObject &message);
    void clientDisconnected(int userId);
    void authenticated(int userId, ClientHandler* handler);

public slots:
    void readMessages();
    void onDisconnected();
    void onError();

private:
    QTcpSocket *m_socket;
    DatabaseServer *m_database;
    RateLimiter *m_authLimiter;
    QString m_ipKey;
    TokenBucket m_bucket;
    QTimer *m_authTimer;
    bool m_throttled = false;
    int m_userId = -1;
    QString m_nick;
    bool m_authenticated = false;
    QByteArray m_buffer;

    void processMessage(const QJsonObject &message);
    void handleLogin(const QJsonObject &message);
    void handleRegister(const QJsonObject &message);
    void handleMessage(const QJsonObject &message);
    void handleSearchUser(const QJsonObject &message);
    void handleMarkMessagesRead(const QJsonObject &message);
    void handleDeliveryAck(const QJsonObject &message);

    void sendAuthResponse(bool success, const QString &message);
    bool checkProtocolVersion(const QJsonObject &message);
    bool authBlocked();
    void recordAuthFailure();
    void onAuthenticated();
    void sendUserList();
    void handleGetOfflineMessages();
    void forwardMessage(int fromUserId, int toUserId, int messageId, const QString &text);
};

#endif
