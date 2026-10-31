#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

#include <QObject>
#include <QTcpSocket>
#include <QByteArray>
#include <QJsonObject>
#include <QTimer>
#include <functional>
#include "Protocol.h"
#include "RateLimiter.h"

class DatabaseServer;
class FileStore;
class FileSession;

class ClientHandler : public QObject {
    Q_OBJECT

public:
    explicit ClientHandler(QTcpSocket *socket, DatabaseServer *db, RateLimiter *authLimiter,
                           FileStore *files, QObject *parent = nullptr);
    ~ClientHandler();

    int getUserId() const { return m_userId; }
    QString getNick() const { return m_nick; }
    bool isAuthenticated() const { return m_authenticated; }

    void sendMessage(const QJsonObject &message);

    // ServerCore сообщает, какие пользователи сейчас в сети (для user_list)
    void setOnlineCheck(std::function<bool(int)> check) { m_isOnline = std::move(check); }

signals:
    void messageReady(int toUserId, int messageId, const QJsonObject &message);
    void clientDisconnected(int userId);
    void authenticated(int userId, ClientHandler* handler);
    // Сообщение другому пользователю (участнику группы), если он сейчас в сети
    void pushToUser(int userId, const QJsonObject &message);

public slots:
    void readMessages();
    void onDisconnected();
    void onError();

private:
    QTcpSocket *m_socket;
    DatabaseServer *m_database;
    RateLimiter *m_authLimiter;
    FileStore *m_files;
    FileSession *m_fileSession = nullptr;
    QString m_ipKey;
    TokenBucket m_bucket;
    QTimer *m_authTimer;
    bool m_throttled = false;
    int m_userId = -1;
    QString m_nick;
    bool m_authenticated = false;
    QByteArray m_buffer;
    std::function<bool(int)> m_isOnline;

    void processMessage(const QJsonObject &message);
    void handleLogin(const QJsonObject &message);
    void handleRegister(const QJsonObject &message);
    void handleMessage(const QJsonObject &message);
    void handleSearchUser(const QJsonObject &message);
    void handleMarkMessagesRead(const QJsonObject &message);
    void handleDeliveryAck(const QJsonObject &message);
    void handleChangePassword(const QJsonObject &message);
    void handleGroupCreate(const QJsonObject &message);
    void handleGroupAdd(const QJsonObject &message);
    void handleGroupRemove(const QJsonObject &message);
    void handleGroupRename(const QJsonObject &message);
    void handleGroupMessage(int groupId, const QString &text, const QString &fileId, const QJsonObject &message);
    void pushGroupInfo(int groupId);
    void announceToGroup(int groupId, const QList<int> &recipients, const QString &text);
    void sendGroupList();
    void groupResult(bool success, const QString &message, int groupId = 0);
    void handleFileUploadRequest(const QJsonObject &message);
    void handleFileDownloadRequest(const QJsonObject &message);
    void startFileSession(const QJsonObject &message, bool upload);

    void sendAuthResponse(bool success, const QString &message);
    bool checkProtocolVersion(const QJsonObject &message);
    bool authBlocked();
    void recordAuthFailure();
    void onAuthenticated();
    void sendUserList();
    void handleGetOfflineMessages();
    void forwardMessage(int fromUserId, int toUserId, int messageId, const QString &text, const QString &fileId = QString(),
                        int groupId = 0);
};

#endif
