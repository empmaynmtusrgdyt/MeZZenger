#include "FileStore.h"
#include "DatabaseServer.h"
#include "Protocol.h"
#include "ServerLimits.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>
#include <QDebug>

FileStore::FileStore(DatabaseServer *database, const QString &directory)
    : m_database(database)
    , m_directory(directory)
{
}

bool FileStore::initialize() {
    if (!QDir().mkpath(m_directory)) {
        qCritical() << "Не удалось создать каталог файлов" << m_directory;
        return false;
    }
    // Остатки оборванных загрузок
    for (const QFileInfo &part : QDir(m_directory).entryInfoList({"*.part"}, QDir::Files)) {
        QFile::remove(part.absoluteFilePath());
    }
    for (const QString &id : m_database->staleFileIds()) {
        qDebug() << "Удаляем забытый файл" << id;
        remove(id);
    }
    return true;
}

QString FileStore::path(const QString &fileId) const {
    return QDir(m_directory).filePath(fileId);
}

QString FileStore::partPath(const QString &fileId) const {
    return path(fileId) + ".part";
}

void FileStore::remove(const QString &fileId) {
    QFile::remove(path(fileId));
    QFile::remove(partPath(fileId));
    m_database->deleteFile(fileId);
}

QString FileStore::sanitizeName(const QString &name) {
    QString clean = name;
    clean.replace('\\', '/');
    clean = clean.section('/', -1);
    QString result;
    for (const QChar &c : clean) {
        if (c.isPrint()) {
            result += c;
        }
    }
    result = result.trimmed();
    if (result.size() > Protocol::MaxFileNameLength) {
        result = result.left(Protocol::MaxFileNameLength);
    }
    if (result.isEmpty() || result == "." || result == "..") {
        result = "file";
    }
    return result;
}

void FileStore::dropExpired() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = m_tickets.begin(); it != m_tickets.end();) {
        if (it->expires < now) {
            // Разрешение на загрузку не использовано: запись о файле больше не нужна
            if (it->upload) {
                remove(it->fileId);
            }
            it = m_tickets.erase(it);
        } else {
            ++it;
        }
    }
}

int FileStore::pendingFor(int userId) const {
    int count = 0;
    for (const Ticket &ticket : m_tickets) {
        count += ticket.userId == userId ? 1 : 0;
    }
    return count;
}

QString FileStore::newToken() {
    return QUuid::createUuid().toString(QUuid::Id128) + QUuid::createUuid().toString(QUuid::Id128);
}

QString FileStore::issueUpload(int userId, const QString &name, qint64 size, QString *fileId, QString *error) {
    dropExpired();
    if (size <= 0 || size > Protocol::MaxFileSize) {
        *error = QString("Размер файла должен быть от 1 байта до %1 МБ").arg(Protocol::MaxFileSize / (1024 * 1024));
        return QString();
    }
    if (m_database->storedBytes(userId) + size > Limits::MaxStoragePerUser) {
        *error = "Превышен лимит места для ваших файлов на сервере";
        return QString();
    }
    if (pendingFor(userId) >= Limits::MaxPendingTickets) {
        *error = "Слишком много незавершённых передач файлов";
        return QString();
    }

    const QString id = m_database->createFile(userId, sanitizeName(name), size);
    if (id.isEmpty()) {
        *error = "Не удалось подготовить загрузку";
        return QString();
    }
    const QString token = newToken();
    m_tickets[token] = {userId, id, true, QDateTime::currentDateTimeUtc().addSecs(Limits::TicketLifetimeSec)};
    *fileId = id;
    return token;
}

QString FileStore::issueDownload(int userId, const QString &fileId, QString *error) {
    dropExpired();
    if (!m_database->canDownloadFile(userId, fileId) || !QFile::exists(path(fileId))) {
        *error = "Файл не найден";
        return QString();
    }
    if (pendingFor(userId) >= Limits::MaxPendingTickets) {
        *error = "Слишком много незавершённых передач файлов";
        return QString();
    }
    const QString token = newToken();
    m_tickets[token] = {userId, fileId, false, QDateTime::currentDateTimeUtc().addSecs(Limits::TicketLifetimeSec)};
    return token;
}

bool FileStore::take(const QString &token, Ticket *ticket) {
    dropExpired();
    auto it = m_tickets.find(token);
    if (it == m_tickets.end()) {
        return false;
    }
    *ticket = it.value();
    m_tickets.erase(it);
    return true;
}
