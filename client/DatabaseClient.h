#ifndef DATABASE_CLIENT_H
#define DATABASE_CLIENT_H

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QString>
#include <QList>
#include <QDateTime>

struct MessageEntry {
    int id;
    int userId;
    QString fromNick;
    QString message;
    QString timestamp;
    bool isOutgoing;
    bool isRead;
};

class DatabaseClient : public QObject {
    Q_OBJECT

public:
    explicit DatabaseClient(QObject *parent = nullptr);
    ~DatabaseClient();

    bool initialize(const QString &dbPath = "messenger_client.db");
    
    bool saveMessage(int userId, const QString &yourNick, const QString &message, bool isOutgoing);
    bool saveMessage(int userId, const QString &yourNick, const QString &message, 
                     bool isOutgoing, const QString &fromNick);
    
    QList<MessageEntry> getMessageHistory(int userId, const QString &yourNick, int limit = 200);
    QList<int> getConversationPartners(const QString &myNick);
    bool markMessagesAsRead(int userId, const QString &myNick);
    int getUnreadCount(int userId, const QString &myNick);
    bool clearHistory(int userId);
    bool clearAllHistory();

private:
    QSqlDatabase m_db;
    bool m_initialized = false;

    QSqlQuery createQuery();
};

#endif
