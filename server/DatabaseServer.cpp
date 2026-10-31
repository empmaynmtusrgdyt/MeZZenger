#include "DatabaseServer.h"
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

    if (!query.exec(
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_messages_client_msg "
        "ON messages(from_user_id, client_msg_id) WHERE client_msg_id IS NOT NULL"
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
                                const QString &clientMsgId, bool *duplicate) {
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

    query.prepare(
        "INSERT INTO messages (from_user_id, to_user_id, message, delivered, client_msg_id) "
        "VALUES (:from, :to, :msg, 0, :cid)"
    );
    query.bindValue(":from", fromUserId);
    query.bindValue(":to", toUserId);
    query.bindValue(":msg", message);
    query.bindValue(":cid", clientMsgId.isEmpty() ? QVariant(QMetaType(QMetaType::QString)) : QVariant(clientMsgId));

    if (!query.exec()) {
        qWarning() << "Ошибка сохранения сообщения:" << query.lastError().text();
        return -1;
    }

    return query.lastInsertId().toInt();
}

QList<QJsonObject> DatabaseServer::getUndeliveredMessages(int toUserId) {
    QList<QJsonObject> messages;

    if (toUserId <= 0) {
        return messages;
    }

    QSqlQuery query = createQuery();
    query.prepare(
        "SELECT id, from_user_id, message, timestamp FROM messages "
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
