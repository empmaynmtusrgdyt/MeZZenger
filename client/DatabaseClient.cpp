#include "DatabaseClient.h"
#include <QDebug>
#include <QFile>

DatabaseClient::DatabaseClient(QObject *parent)
    : QObject(parent)
{
}

DatabaseClient::~DatabaseClient() {
    if (m_initialized && m_db.isOpen()) {
        m_db.close();
    }
}

bool DatabaseClient::initialize(const QString &dbPath) {
    // Миграция после переименования проекта: старая база messenger_client.db -> новая
    const QString legacyPath = "messenger_client.db";
    if (!QFile::exists(dbPath) && QFile::exists(legacyPath)) {
        if (QFile::rename(legacyPath, dbPath)) {
            qDebug() << "База данных перенесена:" << legacyPath << "->" << dbPath;
        } else {
            qWarning() << "Не удалось перенести старую базу данных" << legacyPath;
        }
    }

    m_db = QSqlDatabase::addDatabase("QSQLITE");
    m_db.setDatabaseName(dbPath);

    if (!m_db.open()) {
        qCritical() << "Не удалось открыть базу данных:" << m_db.lastError().text();
        return false;
    }

    QSqlQuery query = createQuery();
    
    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "user_id INTEGER NOT NULL, "
        "user_nick TEXT NOT NULL, "
        "my_nick TEXT NOT NULL, "
        "message TEXT NOT NULL, "
        "timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, "
        "is_outgoing INTEGER NOT NULL DEFAULT 0, "
        "is_read INTEGER NOT NULL DEFAULT 0)"
    )) {
        qCritical() << "Ошибка создания таблицы messages:" << query.lastError().text();
        return false;
    }

    if (!query.exec(
        "CREATE INDEX IF NOT EXISTS idx_messages_user ON messages(user_id)"
    )) {
        qWarning() << "Не удалось создать индекс:" << query.lastError().text();
    }

    query.exec("PRAGMA table_info(messages)");
    bool hasIsRead = false;
    while (query.next()) {
        if (query.value(1).toString() == "is_read") {
            hasIsRead = true;
            break;
        }
    }
    if (!hasIsRead) {
        qDebug() << "Добавляем колонку is_read в таблицу messages";
        if (!query.exec("ALTER TABLE messages ADD COLUMN is_read INTEGER NOT NULL DEFAULT 0")) {
            qWarning() << "Не удалось добавить колонку is_read:" << query.lastError().text();
        }
    }

    m_initialized = true;
    qDebug() << "Единая база данных клиента инициализирована:" << dbPath;
    return true;
}

QSqlQuery DatabaseClient::createQuery() {
    return QSqlQuery(m_db);
}

bool DatabaseClient::saveMessage(int userId, const QString &yourNick, 
                                  const QString &message, bool isOutgoing) {
    return saveMessage(userId, yourNick, message, isOutgoing, QString());
}

bool DatabaseClient::saveMessage(int userId, const QString &yourNick, 
                                  const QString &message, bool isOutgoing,
                                  const QString &fromNick) {
    if (!m_initialized) {
        return false;
    }

    QString userNick = isOutgoing ? yourNick : fromNick;
    if (userNick.isEmpty()) {
        userNick = QString("user_%1").arg(userId);
    }
    
    QSqlQuery query = createQuery();
    query.prepare(
        "INSERT INTO messages (user_id, user_nick, my_nick, message, is_outgoing) "
        "VALUES (:user_id, :user_nick, :my_nick, :message, :is_outgoing)"
    );
    query.bindValue(":user_id", userId);
    query.bindValue(":user_nick", userNick);
    query.bindValue(":my_nick", yourNick);
    query.bindValue(":message", message);
    query.bindValue(":is_outgoing", isOutgoing ? 1 : 0);

    if (!query.exec()) {
        qWarning() << "Ошибка сохранения сообщения:" << query.lastError().text();
        return false;
    }
    
    qDebug() << "Сохранено сообщение в БД: user_id" << userId 
             << "my_nick" << yourNick << "text" << message;
    
    return true;
}

QList<MessageEntry> DatabaseClient::getMessageHistory(int userId, const QString &yourNick, int limit) {
    QList<MessageEntry> history;
    
    if (!m_initialized || userId <= 0) {
        return history;
    }

    QSqlQuery query = createQuery();
    query.prepare(
        "SELECT id, user_id, user_nick, message, timestamp, is_outgoing, is_read "
        "FROM messages "
        "WHERE user_id = :user_id AND my_nick = :my_nick "
        "ORDER BY timestamp ASC "
        "LIMIT :limit"
    );
    query.bindValue(":user_id", userId);
    query.bindValue(":my_nick", yourNick);
    query.bindValue(":limit", limit);
    
    if (!query.exec()) {
        qWarning() << "Ошибка получения истории:" << query.lastError().text();
        return history;
    }
    
    while (query.next()) {
        MessageEntry entry;
        entry.id = query.value(0).toInt();
        entry.userId = query.value(1).toInt();
        entry.fromNick = query.value(2).toString();
        entry.message = query.value(3).toString();
        entry.timestamp = query.value(4).toString();
        entry.isOutgoing = query.value(5).toInt() == 1;
        entry.isRead = query.value(6).toInt() == 1;
        
        history.append(entry);
    }
    
    qDebug() << "Загружено" << history.size() << "сообщений для user_id" << userId 
             << "my_nick" << yourNick;
    
    return history;
}

QList<int> DatabaseClient::getConversationPartners(const QString &myNick) {
    QList<int> partners;

    if (!m_initialized) {
        return partners;
    }

    QSqlQuery query = createQuery();
    query.prepare(
        "SELECT DISTINCT user_id FROM messages WHERE my_nick = :my_nick"
    );
    query.bindValue(":my_nick", myNick);

    if (!query.exec()) {
        qWarning() << "Ошибка получения собеседников:" << query.lastError().text();
        return partners;
    }

    while (query.next()) {
        partners.append(query.value(0).toInt());
    }

    return partners;
}

bool DatabaseClient::markMessagesAsRead(int userId, const QString &myNick) {
    if (!m_initialized || userId <= 0) {
        return false;
    }

    QSqlQuery query = createQuery();
    query.prepare(
        "UPDATE messages SET is_read = 1 WHERE user_id = :user_id AND my_nick = :my_nick AND is_outgoing = 0 AND is_read = 0"
    );
    query.bindValue(":user_id", userId);
    query.bindValue(":my_nick", myNick);

    if (!query.exec()) {
        qWarning() << "Ошибка отметки сообщений как прочитанных:" << query.lastError().text();
        return false;
    }

    qDebug() << "Сообщения от userId" << userId << "для" << myNick << "отмечены как прочитанные";
    return true;
}

int DatabaseClient::getUnreadCount(int userId, const QString &myNick) {
    if (!m_initialized || userId <= 0) {
        return 0;
    }

    QSqlQuery query = createQuery();
    query.prepare(
        "SELECT COUNT(*) FROM messages WHERE user_id = :user_id AND my_nick = :my_nick AND is_outgoing = 0 AND is_read = 0"
    );
    query.bindValue(":user_id", userId);
    query.bindValue(":my_nick", myNick);

    if (!query.exec() || !query.next()) {
        return 0;
    }

    return query.value(0).toInt();
}

bool DatabaseClient::clearHistory(int userId) {
    if (!m_initialized || userId <= 0) {
        return false;
    }

    QSqlQuery query = createQuery();
    query.prepare("DELETE FROM messages WHERE user_id = :user_id");
    query.bindValue(":user_id", userId);
    
    if (!query.exec()) {
        qWarning() << "Ошибка очистки истории:" << query.lastError().text();
        return false;
    }
    
    return true;
}

bool DatabaseClient::clearAllHistory() {
    if (!m_initialized) {
        return false;
    }

    QSqlQuery query = createQuery();
    if (!query.exec("DELETE FROM messages")) {
        qWarning() << "Ошибка очистки всей истории:" << query.lastError().text();
        return false;
    }
    
    return true;
}
