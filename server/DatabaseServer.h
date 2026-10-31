#ifndef DATABASE_SERVER_H
#define DATABASE_SERVER_H

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QString>
#include <QList>
#include <QStringList>
#include <QPair>
#include <QJsonObject>
#include <QCryptographicHash>

class DatabaseServer : public QObject {
    Q_OBJECT

public:
    explicit DatabaseServer(QObject *parent = nullptr);
    ~DatabaseServer();

    bool initialize(const QString &dbPath = "server.db");

    int createUser(const QString &nick, const QString &password);

    // Регистрация только по одноразовому коду приглашения. В базе хранится лишь хеш кода.
    QString createInvite();
    bool inviteAvailable(const QString &code);
    int createUserWithInvite(const QString &nick, const QString &password, const QString &code);
    bool userExists(const QString &nick);
    int getUserIdByNick(const QString &nick);
    QString getNickById(int userId);
    bool verifyPassword(int userId, const QString &password);
    bool setPassword(int userId, const QString &newPassword);
    QList<QJsonObject> getAllUsers(int excludeUserId = -1);
    QList<QJsonObject> getUsersWithConversations(int userId);
    QJsonObject searchUser(const QString &nick);

    // Если clientMsgId уже есть у этого отправителя, возвращает id прежнего сообщения
    // и выставляет *duplicate = true (новая запись не создаётся).
    int saveMessage(int fromUserId, int toUserId, const QString &message,
                    const QString &clientMsgId = QString(), bool *duplicate = nullptr,
                    const QString &fileId = QString());
    QList<QJsonObject> getUndeliveredMessages(int toUserId);
    bool markMessagesAsDelivered(int toUserId, const QList<int> &messageIds);
    bool markMessagesAsRead(int fromUserId, int toUserId);

    // Файлы: запись создаётся при выдаче разрешения на загрузку и становится complete после приёма всех байт
    QString createFile(int ownerId, const QString &name, qint64 size);
    bool completeFile(const QString &fileId);
    // Пустой объект, если файла нет; иначе owner_id, name, size, complete
    QJsonObject fileInfo(const QString &fileId);
    qint64 storedBytes(int ownerId);
    // Скачать можно файл из сообщения, где пользователь отправитель или получатель
    bool canDownloadFile(int userId, const QString &fileId);
    bool deleteFile(const QString &fileId);
    // Недогруженные (старше часа) и ни к одному сообщению не прикреплённые (старше суток)
    QStringList staleFileIds();

    // Группы. Сообщение в группу хранится отдельной строкой для каждого получателя (у каждого свой
    // delivered), поэтому подтверждения доставки работают так же, как в личной переписке.
    int createGroup(const QString &name, int ownerId, const QList<int> &memberIds);
    // Пустой объект, если группы нет: id, name, owner_id, members [{id, nick}] в порядке вступления
    QJsonObject groupInfo(int groupId);
    QList<QJsonObject> groupsOf(int userId);
    QList<int> groupMemberIds(int groupId);
    int groupCountOf(int userId);
    bool addGroupMembers(int groupId, const QList<int> &userIds);
    // Убирает участника и его недоставленные сообщения группы
    bool removeGroupMember(int groupId, int userId);
    bool renameGroup(int groupId, const QString &name);
    bool setGroupOwner(int groupId, int ownerId);
    bool deleteGroup(int groupId);
    // Возвращает id сообщения для подтверждения отправителю (при повторе - прежний); в created
    // кладёт (id сообщения, получатель) для только что созданных строк.
    // Служебное сообщение группы ("X покинул группу"): строки от несуществующего отправителя (from_user_id = 0)
    QList<QPair<int, int>> saveGroupNotice(int groupId, const QString &text, const QList<int> &recipients);
    int saveGroupMessage(int fromUserId, int groupId, const QString &message, const QString &clientMsgId,
                         bool *duplicate, const QString &fileId, QList<QPair<int, int>> *created);

private:
    QSqlDatabase m_db;
    bool m_initialized = false;

    QString hashPassword(const QString &password);
    bool resolveAttachment(int fromUserId, const QString &fileId, QString *name, qint64 *size);
    QSqlQuery createQuery();
};

#endif
