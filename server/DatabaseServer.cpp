#include "DatabaseServer.h"
#include <QUuid>
#include <QDebug>
#include <QFile>
#include <QRandomGenerator>
#include <QPasswordDigestor>
#include "Protocol.h"

namespace {

// Формат хранения пароля: "pbkdf2-sha256$<итерации>$<соль base64>$<хеш base64>".
// Параметры лежат рядом с хешем, поэтому их можно усиливать, не ломая старые записи.
constexpr int PasswordIterations = 600000;   // рекомендация OWASP для PBKDF2-HMAC-SHA256
constexpr int PasswordSaltBytes = 16;
constexpr int PasswordHashBytes = 32;
const QString PasswordScheme = "pbkdf2-sha256";

bool constantTimeEquals(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
}

QByteArray derivePassword(const QString &password, const QByteArray &salt, int iterations) {
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256, password.toUtf8(),
                                              salt, iterations, PasswordHashBytes);
}

// Пароли до перехода на PBKDF2 хранились как SHA-256 без соли (64 hex-символа)
QString legacyPasswordHash(const QString &password) {
    return QString::fromLatin1(QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool isLegacyHash(const QString &stored) {
    return stored.size() == 64 && !stored.contains('$');
}

// Код сравнивается без регистра и разделителей: "abcd-efgh" == "ABCDEFGH"
QString inviteHash(const QString &code) {
    QString normalized;
    for (QChar c : code) {
        if (c.isLetterOrNumber()) {
            normalized += c.toUpper();
        }
    }
    return normalized.isEmpty() ? QString()
        : QString::fromLatin1(QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256).toHex());
}

}

DatabaseServer::DatabaseServer(QObject *parent)
    : QObject(parent)
{
}

DatabaseServer::~DatabaseServer() {
    if (m_initialized && m_db.isOpen()) {
        m_db.close();
    }
}

bool DatabaseServer::initialize(const QString &dbPath) {
    m_db = QSqlDatabase::addDatabase("QSQLITE");
    m_db.setDatabaseName(dbPath);

    if (!m_db.open()) {
        qCritical() << "Не удалось открыть базу данных:" << m_db.lastError().text();
        return false;
    }

    QSqlQuery query = createQuery();

    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "nickname TEXT UNIQUE NOT NULL, "
        "password_hash TEXT NOT NULL)"
    )) {
        qCritical() << "Ошибка создания таблицы users:" << query.lastError().text();
        return false;
    }

    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "from_user_id INTEGER, "
        "to_user_id INTEGER, "
        "message TEXT, "
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
        "delivered INTEGER DEFAULT 0, "
        "FOREIGN KEY(from_user_id) REFERENCES users(id), "
        "FOREIGN KEY(to_user_id) REFERENCES users(id))"
    )) {
        qCritical() << "Ошибка создания таблицы messages:" << query.lastError().text();
        return false;
    }

    query.exec("PRAGMA table_info(messages)");
    bool hasDelivered = false;
    while (query.next()) {
        if (query.value(1).toString() == "delivered") {
            hasDelivered = true;
            break;
        }
    }
    if (!hasDelivered) {
        qDebug() << "Добавляем колонку delivered в таблицу messages";
        if (!query.exec("ALTER TABLE messages ADD COLUMN delivered INTEGER DEFAULT 0")) {
            qWarning() << "Не удалось добавить колонку delivered:" << query.lastError().text();
        }
    }

    query.exec("PRAGMA table_info(messages)");
    bool hasRead = false;
    while (query.next()) {
        if (query.value(1).toString() == "read") {
            hasRead = true;
            break;
        }
    }
    if (!hasRead) {
        qDebug() << "Добавляем колонку read в таблицу messages";
        if (!query.exec("ALTER TABLE messages ADD COLUMN read INTEGER DEFAULT 0")) {
            qWarning() << "Не удалось добавить колонку read:" << query.lastError().text();
        }
    }

    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS invites ("
        "code_hash TEXT PRIMARY KEY, "
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP, "
        "used_by INTEGER, "
        "used_at DATETIME)"
    )) {
        qCritical() << "Ошибка создания таблицы invites:" << query.lastError().text();
        return false;
    }

    query.exec("PRAGMA table_info(messages)");
    bool hasClientMsgId = false;
    while (query.next()) {
        if (query.value(1).toString() == "client_msg_id") {
            hasClientMsgId = true;
            break;
        }
    }
    if (!hasClientMsgId) {
        qDebug() << "Добавляем колонку client_msg_id в таблицу messages";
        if (!query.exec("ALTER TABLE messages ADD COLUMN client_msg_id TEXT")) {
            qCritical() << "Не удалось добавить колонку client_msg_id:" << query.lastError().text();
            return false;
        }
    }

    for (const char *column : {"file_id TEXT", "file_name TEXT", "file_size INTEGER", "group_id INTEGER"}) {
        const QString name = QString::fromLatin1(column).section(' ', 0, 0);
        query.exec("PRAGMA table_info(messages)");
        bool present = false;
        while (query.next()) {
            if (query.value(1).toString() == name) {
                present = true;
                break;
            }
        }
        if (!present && !query.exec(QString("ALTER TABLE messages ADD COLUMN %1").arg(QString::fromLatin1(column)))) {
            qCritical() << "Не удалось добавить колонку" << name << ":" << query.lastError().text();
            return false;
        }
    }

    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS files ("
        "id TEXT PRIMARY KEY, "
        "owner_id INTEGER NOT NULL, "
        "name TEXT NOT NULL, "
        "size INTEGER NOT NULL, "
        "complete INTEGER NOT NULL DEFAULT 0, "
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP)"
    )) {
        qCritical() << "Ошибка создания таблицы files:" << query.lastError().text();
        return false;
    }

    query.exec("CREATE INDEX IF NOT EXISTS idx_messages_file ON messages(file_id)");

    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS chat_groups ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "name TEXT NOT NULL, "
        "owner_id INTEGER NOT NULL, "
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP)"
    ) || !query.exec(
        "CREATE TABLE IF NOT EXISTS group_members ("
        "group_id INTEGER NOT NULL, "
        "user_id INTEGER NOT NULL, "
        "PRIMARY KEY (group_id, user_id))"
    )) {
        qCritical() << "Ошибка создания таблиц групп:" << query.lastError().text();
        return false;
    }
    query.exec("CREATE INDEX IF NOT EXISTS idx_group_members_user ON group_members(user_id)");

    // Сообщение в группу даёт несколько строк с одним client_msg_id (по строке на получателя)
    query.exec("DROP INDEX IF EXISTS idx_messages_client_msg");
    if (!query.exec(
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_messages_client_msg_to "
        "ON messages(from_user_id, client_msg_id, to_user_id) WHERE client_msg_id IS NOT NULL"
    )) {
        qCritical() << "Не удалось создать индекс client_msg_id:" << query.lastError().text();
        return false;
    }

    if (!query.exec(
        "CREATE INDEX IF NOT EXISTS idx_messages_to_user ON messages(to_user_id)"
    )) {
        qWarning() << "Не удалось создать индекс:" << query.lastError().text();
    }

    m_initialized = true;
    qDebug() << "База данных инициализирована:" << dbPath;
    return true;
}

QSqlQuery DatabaseServer::createQuery() {
    return QSqlQuery(m_db);
}

QString DatabaseServer::hashPassword(const QString &password) {
    QByteArray salt(PasswordSaltBytes, 0);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(salt.data()), PasswordSaltBytes / 4);
    const QByteArray hash = derivePassword(password, salt, PasswordIterations);
    return QString("%1$%2$%3$%4").arg(PasswordScheme).arg(PasswordIterations)
        .arg(QString::fromLatin1(salt.toBase64()), QString::fromLatin1(hash.toBase64()));
}

int DatabaseServer::createUser(const QString &nick, const QString &password) {
    if (userExists(nick)) {
        return -1;
    }

    QString passwordHash = hashPassword(password);
    QSqlQuery query = createQuery();
    query.prepare("INSERT INTO users (nickname, password_hash) VALUES (:nick, :hash)");
    query.bindValue(":nick", nick);
    query.bindValue(":hash", passwordHash);

    if (!query.exec()) {
        qWarning() << "Ошибка создания пользователя:" << query.lastError().text();
        return -1;
    }

    return query.lastInsertId().toInt();
}

QString DatabaseServer::createInvite() {
    // Без похожих символов (0/O, 1/I/L, U): код диктуют и переписывают вручную
    static const char alphabet[] = "ABCDEFGHJKMNPQRSTVWXYZ23456789";
    const int alphabetSize = sizeof(alphabet) - 1;

    QString code;
    for (int i = 0; i < 16; ++i) {
        if (i > 0 && i % 4 == 0) {
            code += '-';
        }
        code += QLatin1Char(alphabet[QRandomGenerator::system()->bounded(alphabetSize)]);
    }

    QSqlQuery query = createQuery();
    query.prepare("INSERT INTO invites (code_hash) VALUES (:hash)");
    query.bindValue(":hash", inviteHash(code));
    if (!query.exec()) {
        qWarning() << "Ошибка создания приглашения:" << query.lastError().text();
        return QString();
    }
    return code;
}

bool DatabaseServer::inviteAvailable(const QString &code) {
    const QString hash = inviteHash(code);
    if (hash.isEmpty()) {
        return false;
    }

    QSqlQuery query = createQuery();
    query.prepare("SELECT 1 FROM invites WHERE code_hash = :hash AND used_by IS NULL");
    query.bindValue(":hash", hash);
    return query.exec() && query.next();
}

int DatabaseServer::createUserWithInvite(const QString &nick, const QString &password, const QString &code) {
    if (!m_db.transaction()) {
        return -1;
    }

    int userId = createUser(nick, password);
    bool ok = userId > 0;
    if (ok) {
        QSqlQuery query = createQuery();
        query.prepare(
            "UPDATE invites SET used_by = :uid, used_at = CURRENT_TIMESTAMP "
            "WHERE code_hash = :hash AND used_by IS NULL"
        );
        query.bindValue(":uid", userId);
        query.bindValue(":hash", inviteHash(code));
        ok = query.exec() && query.numRowsAffected() == 1;
    }

    if (ok && m_db.commit()) {
        return userId;
    }
    m_db.rollback();
    return -1;
}

bool DatabaseServer::userExists(const QString &nick) {
    QSqlQuery query = createQuery();
    query.prepare("SELECT id FROM users WHERE nickname = :nick");
    query.bindValue(":nick", nick);

    if (!query.exec() || !query.next()) {
        return false;
    }
    return true;
}

int DatabaseServer::getUserIdByNick(const QString &nick) {
    QSqlQuery query = createQuery();
    query.prepare("SELECT id FROM users WHERE nickname = :nick");
    query.bindValue(":nick", nick);

    if (!query.exec() || !query.next()) {
        return -1;
    }
    return query.value(0).toInt();
}

QString DatabaseServer::getNickById(int userId) {
    if (userId <= 0) {
        return QString();
    }

    QSqlQuery query = createQuery();
    query.prepare("SELECT nickname FROM users WHERE id = :id");
    query.bindValue(":id", userId);

    if (!query.exec() || !query.next()) {
        return QString();
    }
    return query.value(0).toString();
}

bool DatabaseServer::setPassword(int userId, const QString &newPassword) {
    if (userId <= 0 || newPassword.size() < Protocol::MinPasswordLength
        || newPassword.size() > Protocol::MaxPasswordLength) {
        return false;
    }
    QSqlQuery update = createQuery();
    update.prepare("UPDATE users SET password_hash = :hash WHERE id = :id");
    update.bindValue(":hash", hashPassword(newPassword));
    update.bindValue(":id", userId);
    if (!update.exec() || update.numRowsAffected() != 1) {
        qWarning() << "Ошибка смены пароля:" << update.lastError().text();
        return false;
    }
    return true;
}

bool DatabaseServer::verifyPassword(int userId, const QString &password) {
    if (userId <= 0) {
        return false;
    }

    if (password.size() > Protocol::MaxPasswordLength) {
        return false;
    }

    QSqlQuery query = createQuery();
    query.prepare("SELECT password_hash FROM users WHERE id = :id");
    query.bindValue(":id", userId);

    if (!query.exec() || !query.next()) {
        return false;
    }

    const QString storedHash = query.value(0).toString();
    bool matches = false;
    bool needsUpgrade = false;

    if (isLegacyHash(storedHash)) {
        matches = constantTimeEquals(legacyPasswordHash(password).toLatin1(), storedHash.toLatin1());
        needsUpgrade = matches;
    } else {
        const QStringList parts = storedHash.split('$');
        bool iterationsOk = false;
        const int iterations = parts.size() == 4 ? parts[1].toInt(&iterationsOk) : 0;
        if (parts.size() != 4 || parts[0] != PasswordScheme || !iterationsOk || iterations <= 0) {
            qWarning() << "Неизвестный формат хеша пароля у пользователя" << userId;
            return false;
        }
        const QByteArray salt = QByteArray::fromBase64(parts[2].toLatin1());
        const QByteArray expected = QByteArray::fromBase64(parts[3].toLatin1());
        matches = constantTimeEquals(derivePassword(password, salt, iterations), expected);
        needsUpgrade = matches && iterations < PasswordIterations;
    }

    if (matches && needsUpgrade) {
        QSqlQuery update = createQuery();
        update.prepare("UPDATE users SET password_hash = :hash WHERE id = :id");
        update.bindValue(":hash", hashPassword(password));
        update.bindValue(":id", userId);
        if (update.exec()) {
            qDebug() << "Хеш пароля пользователя" << userId << "обновлён до" << PasswordScheme;
        } else {
            qWarning() << "Не удалось обновить хеш пароля:" << update.lastError().text();
        }
    }
    return matches;
}

QList<QJsonObject> DatabaseServer::getAllUsers(int excludeUserId) {
    QList<QJsonObject> users;

    QSqlQuery query = createQuery();
    if (excludeUserId > 0) {
        query.prepare("SELECT id, nickname FROM users WHERE id != :id ORDER BY nickname");
        query.bindValue(":id", excludeUserId);
    } else {
        query.prepare("SELECT id, nickname FROM users ORDER BY nickname");
    }

    if (!query.exec()) {
        qWarning() << "Ошибка получения списка пользователей:" << query.lastError().text();
        return users;
    }

    while (query.next()) {
        QJsonObject user;
        user["id"] = query.value(0).toInt();
        user["nick"] = query.value(1).toString();
        users.append(user);
    }

    return users;
}

QList<QJsonObject> DatabaseServer::getUsersWithConversations(int userId) {
    QList<QJsonObject> users;

    if (userId <= 0) {
        return users;
    }

    QSqlQuery query = createQuery();
    query.prepare(
        "SELECT DISTINCT u.id, u.nickname FROM users u "
        "INNER JOIN messages m ON (m.from_user_id = u.id OR m.to_user_id = u.id) "
        "WHERE (m.from_user_id = :userId OR m.to_user_id = :userId) AND u.id != :userId2 "
        "ORDER BY u.nickname"
    );
    query.bindValue(":userId", userId);
    query.bindValue(":userId2", userId);

    if (!query.exec()) {
        qWarning() << "Ошибка получения собеседников:" << query.lastError().text();
        return users;
    }

    while (query.next()) {
        QJsonObject user;
        user["id"] = query.value(0).toInt();
        user["nick"] = query.value(1).toString();
        users.append(user);
    }

    return users;
}

QJsonObject DatabaseServer::searchUser(const QString &nick) {
    QJsonObject result;

    QSqlQuery query = createQuery();
    query.prepare("SELECT id, nickname FROM users WHERE nickname = :nick");
    query.bindValue(":nick", nick);

    if (!query.exec() || !query.next()) {
        return result;
    }

    result["id"] = query.value(0).toInt();
    result["nick"] = query.value(1).toString();
    return result;
}

int DatabaseServer::saveMessage(int fromUserId, int toUserId, const QString &message,
                                const QString &clientMsgId, bool *duplicate, const QString &fileId) {
    if (duplicate) {
        *duplicate = false;
    }
    if (fromUserId <= 0 || toUserId <= 0) {
        return -1;
    }

    QSqlQuery query = createQuery();
    if (!clientMsgId.isEmpty()) {
        query.prepare("SELECT id FROM messages WHERE from_user_id = :from AND client_msg_id = :cid");
        query.bindValue(":from", fromUserId);
        query.bindValue(":cid", clientMsgId);
        if (query.exec() && query.next()) {
            if (duplicate) {
                *duplicate = true;
            }
            return query.value(0).toInt();
        }
    }

    QString fileName;
    qint64 fileSize = 0;
    if (!fileId.isEmpty() && !resolveAttachment(fromUserId, fileId, &fileName, &fileSize)) {
        return -1;
    }

    query.prepare(
        "INSERT INTO messages (from_user_id, to_user_id, message, delivered, client_msg_id, file_id, file_name, file_size) "
        "VALUES (:from, :to, :msg, 0, :cid, :fid, :fname, :fsize)"
    );
    query.bindValue(":from", fromUserId);
    query.bindValue(":to", toUserId);
    query.bindValue(":msg", message);
    query.bindValue(":fid", fileId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(fileId));
    query.bindValue(":fname", fileId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(fileName));
    query.bindValue(":fsize", fileId.isEmpty() ? QVariant(QMetaType(QMetaType::LongLong)) : QVariant(fileSize));
    query.bindValue(":cid", clientMsgId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(clientMsgId));

    if (!query.exec()) {
        qWarning() << "Ошибка сохранения сообщения:" << query.lastError().text();
        return -1;
    }

    return query.lastInsertId().toInt();
}

// Имя и размер берутся из записи о файле, а не из сообщения клиента; прикрепить можно только свой готовый файл
bool DatabaseServer::resolveAttachment(int fromUserId, const QString &fileId, QString *name, qint64 *size) {
    const QJsonObject info = fileInfo(fileId);
    if (info.isEmpty() || info["owner_id"].toInt() != fromUserId || !info["complete"].toBool()) {
        return false;
    }
    *name = info["name"].toString();
    *size = static_cast<qint64>(info["size"].toDouble());
    return true;
}

QList<QJsonObject> DatabaseServer::getUndeliveredMessages(int toUserId) {
    QList<QJsonObject> messages;

    if (toUserId <= 0) {
        return messages;
    }

    QSqlQuery query = createQuery();
    query.prepare(
        "SELECT id, from_user_id, message, timestamp, file_id, file_name, file_size, group_id FROM messages "
        "WHERE to_user_id = :to AND delivered = 0 "
        "ORDER BY id ASC "
        "LIMIT 100"
    );
    query.bindValue(":to", toUserId);

    if (!query.exec()) {
        qWarning() << "Ошибка получения сообщений:" << query.lastError().text();
        return messages;
    }

    while (query.next()) {
        QJsonObject msg;
        msg["id"] = query.value(0).toInt();
        msg["from_user_id"] = query.value(1).toInt();
        msg["message"] = query.value(2).toString();
        msg["timestamp"] = query.value(3).toString();
        if (!query.value(4).isNull()) {
            msg[Protocol::FILE_ID] = query.value(4).toString();
            msg[Protocol::FILE_NAME] = query.value(5).toString();
            msg[Protocol::FILE_SIZE] = query.value(6).toLongLong();
        }
        if (!query.value(7).isNull()) {
            msg[Protocol::GROUP_ID] = query.value(7).toInt();
        }
        messages.append(msg);
    }

    return messages;
}

bool DatabaseServer::markMessagesAsDelivered(int toUserId, const QList<int> &messageIds) {
    if (toUserId <= 0 || messageIds.isEmpty()) {
        return false;
    }

    QStringList placeholders;
    for (int i = 0; i < messageIds.size(); ++i) {
        placeholders << "?";
    }
    QString sql = QString("UPDATE messages SET delivered = 1 WHERE to_user_id = ? AND id IN (%1)")
                      .arg(placeholders.join(","));

    QSqlQuery query = createQuery();
    query.prepare(sql);
    query.addBindValue(toUserId);
    for (int id : messageIds) {
        query.addBindValue(id);
    }

    if (!query.exec()) {
        qWarning() << "Ошибка отметки сообщений как доставленных:" << query.lastError().text();
        return false;
    }

    return true;
}

bool DatabaseServer::markMessagesAsRead(int fromUserId, int toUserId) {
    if (fromUserId <= 0 || toUserId <= 0) {
        return false;
    }

    QSqlQuery query = createQuery();
    query.prepare(
        "UPDATE messages SET read = 1 WHERE from_user_id = :from AND to_user_id = :to AND read = 0"
    );
    query.bindValue(":from", fromUserId);
    query.bindValue(":to", toUserId);

    if (!query.exec()) {
        qWarning() << "Ошибка отметки сообщений как прочитанных:" << query.lastError().text();
        return false;
    }

    qDebug() << "Сообщения от" << fromUserId << "к" << toUserId << "отмечены как прочитанные, затронуто:" << query.numRowsAffected();
    return true;
}

QString DatabaseServer::createFile(int ownerId, const QString &name, qint64 size) {
    const QString id = QUuid::createUuid().toString(QUuid::Id128);
    QSqlQuery query = createQuery();
    query.prepare("INSERT INTO files (id, owner_id, name, size) VALUES (:id, :owner, :name, :size)");
    query.bindValue(":id", id);
    query.bindValue(":owner", ownerId);
    query.bindValue(":name", name);
    query.bindValue(":size", size);
    if (!query.exec()) {
        qWarning() << "Ошибка создания записи о файле:" << query.lastError().text();
        return QString();
    }
    return id;
}

bool DatabaseServer::completeFile(const QString &fileId) {
    QSqlQuery query = createQuery();
    query.prepare("UPDATE files SET complete = 1 WHERE id = :id");
    query.bindValue(":id", fileId);
    return query.exec() && query.numRowsAffected() == 1;
}

QJsonObject DatabaseServer::fileInfo(const QString &fileId) {
    QJsonObject info;
    QSqlQuery query = createQuery();
    query.prepare("SELECT owner_id, name, size, complete FROM files WHERE id = :id");
    query.bindValue(":id", fileId);
    if (query.exec() && query.next()) {
        info["owner_id"] = query.value(0).toInt();
        info["name"] = query.value(1).toString();
        info["size"] = static_cast<double>(query.value(2).toLongLong());
        info["complete"] = query.value(3).toInt() == 1;
    }
    return info;
}

qint64 DatabaseServer::storedBytes(int ownerId) {
    QSqlQuery query = createQuery();
    query.prepare("SELECT COALESCE(SUM(size), 0) FROM files WHERE owner_id = :owner");
    query.bindValue(":owner", ownerId);
    return query.exec() && query.next() ? query.value(0).toLongLong() : 0;
}

bool DatabaseServer::canDownloadFile(int userId, const QString &fileId) {
    QSqlQuery query = createQuery();
    query.prepare(
        "SELECT 1 FROM messages m JOIN files f ON f.id = m.file_id "
        "WHERE m.file_id = :id AND f.complete = 1 AND (m.from_user_id = :user OR m.to_user_id = :user) LIMIT 1"
    );
    query.bindValue(":id", fileId);
    query.bindValue(":user", userId);
    return query.exec() && query.next();
}

bool DatabaseServer::deleteFile(const QString &fileId) {
    QSqlQuery query = createQuery();
    query.prepare("DELETE FROM files WHERE id = :id");
    query.bindValue(":id", fileId);
    return query.exec();
}

QStringList DatabaseServer::staleFileIds() {
    QStringList ids;
    QSqlQuery query = createQuery();
    if (query.exec(
            "SELECT id FROM files WHERE "
            "(complete = 0 AND created_at < datetime('now', '-1 hour')) OR "
            "(created_at < datetime('now', '-1 day') AND id NOT IN (SELECT file_id FROM messages WHERE file_id IS NOT NULL))")) {
        while (query.next()) {
            ids << query.value(0).toString();
        }
    }
    return ids;
}

int DatabaseServer::createGroup(const QString &name, int ownerId, const QList<int> &memberIds) {
    if (!m_db.transaction()) {
        return -1;
    }
    QSqlQuery query = createQuery();
    query.prepare("INSERT INTO chat_groups (name, owner_id) VALUES (:name, :owner)");
    query.bindValue(":name", name);
    query.bindValue(":owner", ownerId);
    if (!query.exec()) {
        qWarning() << "Ошибка создания группы:" << query.lastError().text();
        m_db.rollback();
        return -1;
    }
    const int groupId = query.lastInsertId().toInt();

    // Владелец вступает первым
    QList<int> ordered{ownerId};
    for (int id : memberIds) {
        if (!ordered.contains(id)) {
            ordered.append(id);
        }
    }
    for (int id : ordered) {
        query.prepare("INSERT INTO group_members (group_id, user_id) VALUES (:g, :u)");
        query.bindValue(":g", groupId);
        query.bindValue(":u", id);
        if (!query.exec()) {
            qWarning() << "Ошибка добавления участника:" << query.lastError().text();
            m_db.rollback();
            return -1;
        }
    }
    return m_db.commit() ? groupId : -1;
}

QJsonObject DatabaseServer::groupInfo(int groupId) {
    QJsonObject group;
    QSqlQuery query = createQuery();
    query.prepare("SELECT name, owner_id FROM chat_groups WHERE id = :id");
    query.bindValue(":id", groupId);
    if (!query.exec() || !query.next()) {
        return group;
    }
    group[Protocol::ID] = groupId;
    group[Protocol::NAME] = query.value(0).toString();
    group[Protocol::OWNER_ID] = query.value(1).toInt();

    QJsonArray members;
    query.prepare(
        "SELECT u.id, u.nickname FROM group_members gm JOIN users u ON u.id = gm.user_id "
        "WHERE gm.group_id = :id ORDER BY gm.rowid"
    );
    query.bindValue(":id", groupId);
    if (query.exec()) {
        while (query.next()) {
            QJsonObject member;
            member[Protocol::ID] = query.value(0).toInt();
            member[Protocol::NICK] = query.value(1).toString();
            members.append(member);
        }
    }
    group[Protocol::MEMBERS] = members;
    return group;
}

QList<QJsonObject> DatabaseServer::groupsOf(int userId) {
    QList<QJsonObject> groups;
    QSqlQuery query = createQuery();
    query.prepare("SELECT group_id FROM group_members WHERE user_id = :u ORDER BY group_id");
    query.bindValue(":u", userId);
    QList<int> ids;
    if (query.exec()) {
        while (query.next()) {
            ids.append(query.value(0).toInt());
        }
    }
    for (int id : ids) {
        const QJsonObject group = groupInfo(id);
        if (!group.isEmpty()) {
            groups.append(group);
        }
    }
    return groups;
}

QList<int> DatabaseServer::groupMemberIds(int groupId) {
    QList<int> ids;
    QSqlQuery query = createQuery();
    query.prepare("SELECT user_id FROM group_members WHERE group_id = :g ORDER BY rowid");
    query.bindValue(":g", groupId);
    if (query.exec()) {
        while (query.next()) {
            ids.append(query.value(0).toInt());
        }
    }
    return ids;
}

int DatabaseServer::groupCountOf(int userId) {
    QSqlQuery query = createQuery();
    query.prepare("SELECT COUNT(*) FROM group_members WHERE user_id = :u");
    query.bindValue(":u", userId);
    return query.exec() && query.next() ? query.value(0).toInt() : 0;
}

bool DatabaseServer::addGroupMembers(int groupId, const QList<int> &userIds) {
    QSqlQuery query = createQuery();
    for (int id : userIds) {
        query.prepare("INSERT OR IGNORE INTO group_members (group_id, user_id) VALUES (:g, :u)");
        query.bindValue(":g", groupId);
        query.bindValue(":u", id);
        if (!query.exec()) {
            qWarning() << "Ошибка добавления участника:" << query.lastError().text();
            return false;
        }
    }
    return true;
}

bool DatabaseServer::removeGroupMember(int groupId, int userId) {
    QSqlQuery query = createQuery();
    query.prepare("DELETE FROM group_members WHERE group_id = :g AND user_id = :u");
    query.bindValue(":g", groupId);
    query.bindValue(":u", userId);
    if (!query.exec() || query.numRowsAffected() != 1) {
        return false;
    }
    query.prepare("DELETE FROM messages WHERE group_id = :g AND to_user_id = :u AND delivered = 0");
    query.bindValue(":g", groupId);
    query.bindValue(":u", userId);
    query.exec();
    return true;
}

bool DatabaseServer::renameGroup(int groupId, const QString &name) {
    QSqlQuery query = createQuery();
    query.prepare("UPDATE chat_groups SET name = :name WHERE id = :id");
    query.bindValue(":name", name);
    query.bindValue(":id", groupId);
    return query.exec() && query.numRowsAffected() == 1;
}

bool DatabaseServer::setGroupOwner(int groupId, int ownerId) {
    QSqlQuery query = createQuery();
    query.prepare("UPDATE chat_groups SET owner_id = :owner WHERE id = :id");
    query.bindValue(":owner", ownerId);
    query.bindValue(":id", groupId);
    return query.exec() && query.numRowsAffected() == 1;
}

bool DatabaseServer::deleteGroup(int groupId) {
    QSqlQuery query = createQuery();
    query.prepare("DELETE FROM messages WHERE group_id = :g AND delivered = 0");
    query.bindValue(":g", groupId);
    query.exec();
    query.prepare("DELETE FROM group_members WHERE group_id = :g");
    query.bindValue(":g", groupId);
    query.exec();
    query.prepare("DELETE FROM chat_groups WHERE id = :g");
    query.bindValue(":g", groupId);
    return query.exec();
}

int DatabaseServer::saveGroupMessage(int fromUserId, int groupId, const QString &message, const QString &clientMsgId,
                                     bool *duplicate, const QString &fileId, QList<QPair<int, int>> *created) {
    if (duplicate) {
        *duplicate = false;
    }
    if (fromUserId <= 0 || groupId <= 0) {
        return -1;
    }

    QSqlQuery query = createQuery();
    if (!clientMsgId.isEmpty()) {
        query.prepare("SELECT MIN(id) FROM messages WHERE from_user_id = :from AND client_msg_id = :cid AND group_id = :g");
        query.bindValue(":from", fromUserId);
        query.bindValue(":cid", clientMsgId);
        query.bindValue(":g", groupId);
        if (query.exec() && query.next() && !query.value(0).isNull()) {
            if (duplicate) {
                *duplicate = true;
            }
            return query.value(0).toInt();
        }
    }

    QString fileName;
    qint64 fileSize = 0;
    if (!fileId.isEmpty() && !resolveAttachment(fromUserId, fileId, &fileName, &fileSize)) {
        return -1;
    }

    QList<int> recipients = groupMemberIds(groupId);
    recipients.removeAll(fromUserId);
    if (recipients.isEmpty() || !m_db.transaction()) {
        return -1;
    }

    int firstId = -1;
    QList<QPair<int, int>> rows;
    for (int recipient : recipients) {
        query.prepare(
            "INSERT INTO messages (from_user_id, to_user_id, message, delivered, client_msg_id, file_id, file_name, file_size, group_id) "
            "VALUES (:from, :to, :msg, 0, :cid, :fid, :fname, :fsize, :g)"
        );
        query.bindValue(":from", fromUserId);
        query.bindValue(":to", recipient);
        query.bindValue(":msg", message);
        query.bindValue(":cid", clientMsgId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(clientMsgId));
        query.bindValue(":fid", fileId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(fileId));
        query.bindValue(":fname", fileId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(fileName));
        query.bindValue(":fsize", fileId.isEmpty() ? QVariant(QMetaType(QMetaType::LongLong)) : QVariant(fileSize));
        query.bindValue(":g", groupId);
        if (!query.exec()) {
            qWarning() << "Ошибка сохранения сообщения группы:" << query.lastError().text();
            m_db.rollback();
            return -1;
        }
        const int id = query.lastInsertId().toInt();
        if (firstId < 0) {
            firstId = id;
        }
        rows.append({id, recipient});
    }
    if (!m_db.commit()) {
        return -1;
    }
    if (created) {
        *created = rows;
    }
    return firstId;
}

QList<QPair<int, int>> DatabaseServer::saveGroupNotice(int groupId, const QString &text, const QList<int> &recipients) {
    QList<QPair<int, int>> created;
    QSqlQuery query = createQuery();
    for (int recipient : recipients) {
        query.prepare(
            "INSERT INTO messages (from_user_id, to_user_id, message, delivered, group_id) "
            "VALUES (0, :to, :msg, 0, :g)"
        );
        query.bindValue(":to", recipient);
        query.bindValue(":msg", text);
        query.bindValue(":g", groupId);
        if (query.exec()) {
            created.append({query.lastInsertId().toInt(), recipient});
        } else {
            qWarning() << "Ошибка сохранения служебного сообщения:" << query.lastError().text();
        }
    }
    return created;
}
