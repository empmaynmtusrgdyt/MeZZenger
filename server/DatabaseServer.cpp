#include "DatabaseServer.h"
#include <QDebug>
#include <QFile>

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
    QByteArray hash = QCryptographicHash::hash(
        password.toUtf8(),
        QCryptographicHash::Sha256
    );
    return hash.toHex();
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

bool DatabaseServer::verifyPassword(int userId, const QString &password) {
    if (userId <= 0) {
        return false;
    }

    QString passwordHash = hashPassword(password);
    QSqlQuery query = createQuery();
    query.prepare("SELECT password_hash FROM users WHERE id = :id");
    query.bindValue(":id", userId);

    if (!query.exec() || !query.next()) {
        return false;
    }

    QString storedHash = query.value(0).toString();
    return storedHash == passwordHash;
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

int DatabaseServer::saveMessage(int fromUserId, int toUserId, const QString &message) {
    if (fromUserId <= 0 || toUserId <= 0) {
        return -1;
    }

    QSqlQuery query = createQuery();
    query.prepare(
        "INSERT INTO messages (from_user_id, to_user_id, message, delivered) "
        "VALUES (:from, :to, :msg, 0)"
    );
    query.bindValue(":from", fromUserId);
    query.bindValue(":to", toUserId);
    query.bindValue(":msg", message);

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
        "ORDER BY timestamp ASC "
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
