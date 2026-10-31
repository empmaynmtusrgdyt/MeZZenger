#include "FileSession.h"
#include "DatabaseServer.h"
#include "Protocol.h"
#include "ServerLimits.h"
#include <QDebug>

namespace {
constexpr qint64 ChunkSize = 64 * 1024;
constexpr qint64 MaxBuffered = 512 * 1024;
}

FileSession::FileSession(QTcpSocket *socket, FileStore *store, DatabaseServer *database,
                         const FileStore::Ticket &ticket, QObject *parent)
    : QObject(parent)
    , m_socket(socket)
    , m_store(store)
    , m_database(database)
    , m_ticket(ticket)
    , m_idle(new QTimer(this))
{
    m_idle->setSingleShot(true);
    connect(m_idle, &QTimer::timeout, this, [this]() { fail("Передача файла прервана по таймауту"); });
    connect(m_socket, &QTcpSocket::disconnected, this, [this]() {
        if (!m_done) {
            fail("Соединение закрыто до окончания передачи");
        }
    });
    connect(m_socket, &QTcpSocket::bytesWritten, this, [this]() {
        if (!m_ticket.upload) {
            pump();
        }
    });
}

FileSession::~FileSession() {
    if (!m_done && m_ticket.upload) {
        m_file.close();
        m_store->remove(m_ticket.fileId);
    }
}

void FileSession::start() {
    m_idle->start(Limits::FileIdleTimeoutMs);
    if (m_ticket.upload) {
        startUpload();
    } else {
        startDownload();
    }
}

void FileSession::startUpload() {
    const QJsonObject info = m_database->fileInfo(m_ticket.fileId);
    if (info.isEmpty()) {
        fail("Файл не найден");
        return;
    }
    m_size = static_cast<qint64>(info["size"].toDouble());
    m_file.setFileName(m_store->partPath(m_ticket.fileId));
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Не удалось создать файл" << m_file.fileName() << m_file.errorString();
        fail("Не удалось сохранить файл на сервере");
    }
}

void FileSession::feed(const QByteArray &data) {
    if (m_done || !m_ticket.upload || data.isEmpty()) {
        return;
    }
    m_idle->start(Limits::FileIdleTimeoutMs);
    if (m_transferred + data.size() > m_size) {
        fail("Получено больше байт, чем заявлено");
        return;
    }
    if (m_file.write(data) != data.size()) {
        fail("Не удалось записать файл на сервере");
        return;
    }
    m_transferred += data.size();
    if (m_transferred == m_size) {
        finishUpload();
    }
}

void FileSession::finishUpload() {
    m_file.close();
    if (!QFile::rename(m_store->partPath(m_ticket.fileId), m_store->path(m_ticket.fileId))
        || !m_database->completeFile(m_ticket.fileId)) {
        fail("Не удалось сохранить файл на сервере");
        return;
    }
    qDebug() << "Файл получен:" << m_ticket.fileId << m_size << "байт";
    m_done = true;
    m_idle->stop();
    sendResult(true);
    m_socket->disconnectFromHost();
}

void FileSession::startDownload() {
    m_file.setFileName(m_store->path(m_ticket.fileId));
    if (!m_file.open(QIODevice::ReadOnly)) {
        fail("Файл не найден");
        return;
    }
    m_size = m_file.size();
    m_socket->write(serializeMessage(ProtocolHelper::createFileInfo(m_size)));
    pump();
}

void FileSession::pump() {
    if (m_done) {
        return;
    }
    while (m_socket->bytesToWrite() < MaxBuffered && m_transferred < m_size) {
        const QByteArray chunk = m_file.read(qMin(ChunkSize, m_size - m_transferred));
        if (chunk.isEmpty()) {
            fail("Не удалось прочитать файл на сервере");
            return;
        }
        m_socket->write(chunk);
        m_transferred += chunk.size();
        m_idle->start(Limits::FileIdleTimeoutMs);
    }
    if (m_transferred >= m_size) {
        m_done = true;
        m_idle->stop();
        qDebug() << "Файл отправлен:" << m_ticket.fileId << m_size << "байт";
        m_socket->disconnectFromHost();
    }
}

void FileSession::sendResult(bool success, const QString &message) {
    m_socket->write(serializeMessage(ProtocolHelper::createFileResult(success, message)));
}

void FileSession::fail(const QString &reason) {
    if (m_done) {
        return;
    }
    m_done = true;
    m_idle->stop();
    qWarning() << "Передача файла не удалась:" << reason << m_ticket.fileId;
    m_file.close();
    if (m_ticket.upload) {
        m_store->remove(m_ticket.fileId);
    }
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        sendResult(false, reason);
        m_socket->disconnectFromHost();
    }
}
