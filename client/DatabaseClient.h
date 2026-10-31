#ifndef DATABASE_CLIENT_H
#define DATABASE_CLIENT_H

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QString>
#include <QList>
#include <QDateTime>

// Вложение: файл лежит на сервере, у получателя он скачивается по запросу
struct FileAttachment {
    QString id;
    QString name;
    qint64 size = 0;
    bool isEmpty() const { return id.isEmpty(); }
};

struct MessageEntry {
    int id;
    int userId;
    QString fromNick;
    QString message;
    QString timestamp;
    bool isOutgoing;
    bool isRead;
    bool pending;
    FileAttachment file;
};

// Исходящее сообщение, которое сервер ещё не подтвердил
struct PendingMessage {
    int userId;
    QString peerNick;
    QString message;
    QString clientMsgId;
    QString fileId;
};

class DatabaseClient : public QObject {
    Q_OBJECT

public:
    explicit DatabaseClient(QObject *parent = nullptr);
    ~DatabaseClient();

    bool initialize(const QString &dbPath = "mezzenger_client.db");
    
    bool saveMessage(int userId, const QString &yourNick, const QString &message, bool isOutgoing);
    bool saveMessage(int userId, const QString &yourNick, const QString &message, 
                     bool isOutgoing, const QString &fromNick);
    
    enum class SaveResult { Saved, Duplicate, Error };

    // Исходящее сообщение сохраняется как неподтверждённое до message_ack от сервера
    bool saveOutgoing(int userId, const QString &myNick, const QString &peerNick,
                      const QString &message, const QString &clientMsgId,
                      const FileAttachment &file = FileAttachment());
    bool markOutgoingSent(const QString &myNick, const QString &clientMsgId, int serverId);
    QList<PendingMessage> getPendingOutgoing(const QString &myNick);

    // serverId > 0 защищает от дублей при повторной доставке сервером
    SaveResult saveIncoming(int userId, const QString &myNick, const QString &fromNick,
                            const QString &message, int serverId,
                            const FileAttachment &file = FileAttachment());

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
