#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QJsonObject>
#include "Protocol.h"

class DatabaseServer;

class ClientHandler : public QObject {
    Q_OBJECT

public:
    explicit ClientHandler(QTcpSocket *socket, DatabaseServer *db, QObject *parent = nullptr);
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

    void sendAuthResponse(bool success, const QString &message);
    void sendUserList();
    void deliverOfflineMessages();
    void handleGetOfflineMessages();
    void forwardMessage(int fromUserId, int toUserId, int messageId, const QString &text);
};

#endif
