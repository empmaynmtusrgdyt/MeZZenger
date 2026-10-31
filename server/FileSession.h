#ifndef FILE_SESSION_H
#define FILE_SESSION_H

#include <QObject>
#include <QTcpSocket>
#include <QFile>
#include <QTimer>
#include "FileStore.h"

class DatabaseServer;

// Передача одного файла по отдельному соединению: после кадра file_put идут сырые байты
// файла, после file_get сервер отвечает кадром file_info и присылает байты.
class FileSession : public QObject {
    Q_OBJECT

public:
    FileSession(QTcpSocket *socket, FileStore *store, DatabaseServer *database,
                const FileStore::Ticket &ticket, QObject *parent = nullptr);
    ~FileSession() override;

    void start();
    // Байты, пришедшие по сокету (для загрузки)
    void feed(const QByteArray &data);

private:
    QTcpSocket *m_socket;
    FileStore *m_store;
    DatabaseServer *m_database;
    FileStore::Ticket m_ticket;
    QFile m_file;
    QTimer *m_idle;
    qint64 m_size = 0;
    qint64 m_transferred = 0;
    bool m_done = false;

    void startUpload();
    void startDownload();
    void pump();
    void finishUpload();
    void fail(const QString &reason);
    void sendResult(bool success, const QString &message = QString());
};

#endif
