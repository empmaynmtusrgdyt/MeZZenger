#ifndef FILE_STORE_H
#define FILE_STORE_H

#include <QHash>
#include <QString>
#include <QDateTime>

class DatabaseServer;

// Файлы на диске и одноразовые разрешения (токены) на их передачу отдельным соединением.
// Токен выдаётся вошедшему пользователю по основному каналу и действует один раз.
class FileStore {
public:
    struct Ticket {
        int userId = 0;
        QString fileId;
        bool upload = false;
        QDateTime expires;
    };

    FileStore(DatabaseServer *database, const QString &directory = "files");

    // Создаёт каталог, удаляет недогруженные и забытые файлы
    bool initialize();

    QString path(const QString &fileId) const;
    QString partPath(const QString &fileId) const;

    // Пустой токен и error с причиной, если выдать нельзя
    QString issueUpload(int userId, const QString &name, qint64 size, QString *fileId, QString *error);
    QString issueDownload(int userId, const QString &fileId, QString *error);
    bool take(const QString &token, Ticket *ticket);

    // Имя файла без пути и управляющих символов, не длиннее лимита
    static QString sanitizeName(const QString &name);

    void remove(const QString &fileId);

private:
    DatabaseServer *m_database;
    QString m_directory;
    QHash<QString, Ticket> m_tickets;

    void dropExpired();
    int pendingFor(int userId) const;
    QString newToken();
};

#endif
