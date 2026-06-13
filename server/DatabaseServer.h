#ifndef DATABASE_SERVER_H
#define DATABASE_SERVER_H

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QString>
#include <QList>
#include <QJsonObject>
#include <QCryptographicHash>

class DatabaseServer : public QObject {
    Q_OBJECT

public:
    explicit DatabaseServer(QObject *parent = nullptr);
    ~DatabaseServer();

    bool initialize(const QString &dbPath = "server.db");

    int createUser(const QString &nick, const QString &password);
    bool userExists(const QString &nick);
    int getUserIdByNick(const QString &nick);
    QString getNickById(int userId);
    bool verifyPassword(int userId, const QString &password);
    QList<QJsonObject> getAllUsers(int excludeUserId = -1);
    QList<QJsonObject> getUsersWithConversations(int userId);
    QJsonObject searchUser(const QString &nick);

    int saveMessage(int fromUserId, int toUserId, const QString &message);
    QList<QJsonObject> getUndeliveredMessages(int toUserId);
    bool markMessagesAsDelivered(int toUserId, const QList<int> &messageIds);
    bool markMessagesAsRead(int fromUserId, int toUserId);

private:
    QSqlDatabase m_db;
    bool m_initialized = false;

    QString hashPassword(const QString &password);
    QSqlQuery createQuery();
};

#endif
