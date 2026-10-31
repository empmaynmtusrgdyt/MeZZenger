#include "FileTransfer.h"
#include "Protocol.h"
#include "TlsTrust.h"
#include <QDebug>

namespace {
constexpr qint64 ChunkSize = 64 * 1024;
constexpr qint64 MaxBuffered = 512 * 1024;
}

FileTransfer::FileTransfer(Mode mode, const QString &host, quint16 port, const QString &token,
                           const QString &localPath, qint64 expectedSize,
                           std::function<QWidget *()> dialogParent, QObject *parent)
    : QObject(parent)
    , m_mode(mode)
    , m_host(host)
    , m_port(port)
    , m_token(token)
    , m_localPath(localPath)
    , m_expectedSize(expectedSize)
    , m_dialogParent(std::move(dialogParent))
    , m_socket(new QSslSocket(this))
    , m_connectTimer(new QTimer(this))
{
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, [this]() { finish(false, "Превышено время подключения"); });

    connect(m_socket, &QSslSocket::sslErrors, m_connectTimer, &QTimer::stop);
    TlsTrust::attach(m_socket, m_host, m_port, m_dialogParent, [this]() {
        finish(false, "Сертификат сервера не принят");
    });

    connect(m_socket, &QSslSocket::encrypted, this, &FileTransfer::onEncrypted);
    connect(m_socket, &QSslSocket::readyRead, this, &FileTransfer::onReadyRead);
    connect(m_socket, &QSslSocket::bytesWritten, this, [this]() {
        if (m_mode == Mode::Upload) {
            pumpUpload();
        }
    });
    connect(m_socket, &QSslSocket::errorOccurred, this, [this]() {
        finish(false, "Ошибка соединения: " + m_socket->errorString());
    });
    connect(m_socket, &QSslSocket::disconnected, this, [this]() {
        // Скачивание заканчивается закрытием соединения сервером; проверка полноты в handleDownloadData
        finish(false, "Соединение закрыто до окончания передачи");
    });
}

void FileTransfer::start() {
    if (m_mode == Mode::Upload) {
        m_file.setFileName(m_localPath);
        if (!m_file.open(QIODevice::ReadOnly)) {
            finish(false, "Не удалось открыть файл: " + m_file.errorString());
            return;
        }
        m_total = m_file.size();
        if (m_total != m_expectedSize) {
            finish(false, "Размер файла изменился во время подготовки");
            return;
        }
    } else {
        m_file.setFileName(m_localPath + ".part");
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            finish(false, "Не удалось создать файл: " + m_file.errorString());
            return;
        }
        m_total = m_expectedSize;
    }
    m_connectTimer->start(10000);
    m_socket->connectToHostEncrypted(m_host, m_port);
}

void FileTransfer::cancel() {
    finish(false, "Отменено");
}

void FileTransfer::onEncrypted() {
    m_connectTimer->stop();
    const QJsonObject header = m_mode == Mode::Upload ? ProtocolHelper::createFilePut(m_token)
                                                      : ProtocolHelper::createFileGet(m_token);
    m_socket->write(serializeMessage(header));
    if (m_mode == Mode::Upload) {
        pumpUpload();
    }
}

void FileTransfer::pumpUpload() {
    if (m_finished) {
        return;
    }
    while (!m_allSent && m_socket->bytesToWrite() < MaxBuffered) {
        const QByteArray chunk = m_file.read(ChunkSize);
        if (chunk.isEmpty()) {
            m_allSent = true;
            break;
        }
        m_socket->write(chunk);
        m_done += chunk.size();
        if (m_done >= m_total) {
            m_allSent = true;
        }
    }
    emit progress(qMax<qint64>(0, m_done - m_socket->bytesToWrite()), m_total);
}

void FileTransfer::onReadyRead() {
    m_buffer += m_socket->readAll();
    if (m_mode == Mode::Download) {
        handleDownloadData();
    } else {
        handleUploadReply();
    }
}

void FileTransfer::handleUploadReply() {
    if (m_buffer.size() < static_cast<int>(HeaderSize)) {
        return;
    }
    const uint32_t length = readHeader(m_buffer.left(HeaderSize));
    if (length == 0 || length > MaxMessageSize) {
        finish(false, "Неверный ответ сервера");
        return;
    }
    if (m_buffer.size() < static_cast<int>(HeaderSize + length)) {
        return;
    }
    const QJsonObject reply = deserializeMessage(m_buffer.mid(HeaderSize, length));
    if (ProtocolHelper::getBool(reply, Protocol::SUCCESS)) {
        finish(true);
    } else {
        finish(false, ProtocolHelper::getString(reply, Protocol::MESSAGE, "Сервер отклонил загрузку"));
    }
}

void FileTransfer::handleDownloadData() {
    if (!m_gotInfo) {
        if (m_buffer.size() < static_cast<int>(HeaderSize)) {
            return;
        }
        const uint32_t length = readHeader(m_buffer.left(HeaderSize));
        if (length == 0 || length > MaxMessageSize) {
            finish(false, "Неверный ответ сервера");
            return;
        }
        if (m_buffer.size() < static_cast<int>(HeaderSize + length)) {
            return;
        }
        const QJsonObject info = deserializeMessage(m_buffer.mid(HeaderSize, length));
        m_buffer = m_buffer.mid(HeaderSize + length);
        if (ProtocolHelper::parseMessageType(info) != Protocol::MessageType::FileInfo) {
            finish(false, ProtocolHelper::getString(info, Protocol::MESSAGE, "Сервер не отдал файл"));
            return;
        }
        m_total = static_cast<qint64>(info[Protocol::SIZE].toDouble());
        if (m_total != m_expectedSize) {
            finish(false, "Размер файла не совпадает с ожидаемым");
            return;
        }
        m_gotInfo = true;
    }

    if (m_done + m_buffer.size() > m_total) {
        finish(false, "Получено больше байт, чем заявлено");
        return;
    }
    if (m_file.write(m_buffer) != m_buffer.size()) {
        finish(false, "Не удалось записать файл: " + m_file.errorString());
        return;
    }
    m_done += m_buffer.size();
    m_buffer.clear();
    emit progress(m_done, m_total);

    if (m_done == m_total) {
        m_file.close();
        QFile::remove(m_localPath);
        if (!QFile::rename(m_localPath + ".part", m_localPath)) {
            finish(false, "Не удалось сохранить файл");
            return;
        }
        finish(true);
    }
}

void FileTransfer::finish(bool ok, const QString &error) {
    if (m_finished) {
        return;
    }
    m_finished = true;
    m_connectTimer->stop();
    m_file.close();
    if (!ok && m_mode == Mode::Download) {
        QFile::remove(m_localPath + ".part");
    }
    m_socket->disconnect(this);
    m_socket->abort();
    qDebug() << "Передача файла завершена:" << ok << error;
    emit finished(ok, error);
}
